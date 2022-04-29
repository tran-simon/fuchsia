// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_upper_case_globals)]

use crate::device::DeviceOps;
use crate::fs::devtmpfs::dev_tmp_fs;
use crate::fs::{
    FdEvents, FileObject, FileOps, FileSystem, FileSystemHandle, FileSystemOps, FsNode, FsStr,
    NamespaceNode, ROMemoryDirectory, SeekOrigin, SpecialNode,
};
use crate::lock::{Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard};
use crate::logging::not_implemented;
use crate::mm::vmo::round_up_to_increment;
use crate::mm::{DesiredAddress, MappedVmo, MappingOptions, MemoryManager, UserMemoryCursor};
use crate::syscalls::{SyscallResult, SUCCESS};
use crate::task::{CurrentTask, EventHandler, Kernel, WaitCallback, WaitKey, WaitQueue, Waiter};
use crate::types::*;
use bitflags::bitflags;
use fuchsia_zircon as zx;
use slab::Slab;
use std::collections::{btree_map::Entry as BTreeMapEntry, BTreeMap, VecDeque};
use std::sync::{Arc, Weak};
use zerocopy::{AsBytes, FromBytes};

/// The largest mapping of shared memory allowed by the binder driver.
const MAX_MMAP_SIZE: usize = 4 * 1024 * 1024;

/// Android's binder kernel driver implementation.
pub struct BinderDev {
    driver: Arc<BinderDriver>,
}

impl BinderDev {
    pub fn new() -> Self {
        Self { driver: Arc::new(BinderDriver::new()) }
    }
}

impl DeviceOps for BinderDev {
    fn open(
        &self,
        current_task: &CurrentTask,
        _id: DeviceType,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        let binder_proc = Arc::new(BinderProcess::new(current_task.get_pid()));
        match self.driver.procs.write().entry(binder_proc.pid) {
            BTreeMapEntry::Vacant(entry) => {
                // The process has not previously opened the binder device.
                entry.insert(Arc::downgrade(&binder_proc));
            }
            BTreeMapEntry::Occupied(mut entry) if entry.get().upgrade().is_none() => {
                // The process previously closed the binder device, so it can re-open it.
                entry.insert(Arc::downgrade(&binder_proc));
            }
            _ => {
                // A process cannot open the same binder device more than once.
                return error!(EINVAL);
            }
        }
        Ok(Box::new(BinderDevInstance { proc: binder_proc, driver: self.driver.clone() }))
    }
}

/// An instance of the binder driver, associated with the process that opened the binder device.
struct BinderDevInstance {
    /// The process that opened the binder device.
    proc: Arc<BinderProcess>,
    /// The implementation of the binder driver.
    driver: Arc<BinderDriver>,
}

impl FileOps for BinderDevInstance {
    fn query_events(&self, _current_task: &CurrentTask) -> FdEvents {
        FdEvents::POLLIN | FdEvents::POLLOUT
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        waiter: &Arc<Waiter>,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitKey {
        let binder_thread = self
            .proc
            .thread_pool
            .write()
            .find_or_register_thread(&self.proc, current_task.get_tid());
        self.driver.wait_async(&self.proc, &binder_thread, waiter, events, handler)
    }

    fn cancel_wait(&self, _current_task: &CurrentTask, _waiter: &Arc<Waiter>, key: WaitKey) {
        self.driver.cancel_wait(&self.proc, key)
    }

    fn ioctl(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        request: u32,
        user_addr: UserAddress,
    ) -> Result<SyscallResult, Errno> {
        let binder_thread = self
            .proc
            .thread_pool
            .write()
            .find_or_register_thread(&self.proc, current_task.get_tid());
        self.driver.ioctl(current_task, &self.proc, &binder_thread, request, user_addr)
    }

    fn get_vmo(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        length: Option<usize>,
        _prot: zx::VmarFlags,
    ) -> Result<zx::Vmo, Errno> {
        self.driver.get_vmo(length.unwrap_or(MAX_MMAP_SIZE))
    }

    fn mmap(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        addr: DesiredAddress,
        _vmo_offset: u64,
        length: usize,
        flags: zx::VmarFlags,
        mapping_options: MappingOptions,
        filename: NamespaceNode,
    ) -> Result<MappedVmo, Errno> {
        self.driver.mmap(current_task, &self.proc, addr, length, flags, mapping_options, filename)
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EOPNOTSUPP)
    }
    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EOPNOTSUPP)
    }

    fn seek(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: off_t,
        _whence: SeekOrigin,
    ) -> Result<off_t, Errno> {
        error!(EOPNOTSUPP)
    }

    fn read_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EOPNOTSUPP)
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EOPNOTSUPP)
    }
}

#[derive(Debug)]
struct BinderProcess {
    pid: pid_t,
    /// The [`SharedMemory`] region mapped in both the driver and the binder process. Allows for
    /// transactions to copy data once from the sender process into the receiver process.
    shared_memory: Mutex<Option<SharedMemory>>,
    /// The set of threads that are interacting with the binder driver.
    thread_pool: RwLock<ThreadPool>,
    /// Binder objects hosted by the process shared with other processes.
    objects: Mutex<BTreeMap<UserAddress, Weak<BinderObject>>>,
    /// Handle table of remote binder objects.
    handles: Mutex<HandleTable>,
    /// A queue for commands that could not be scheduled on any existing binder threads. Binder
    /// threads that exhaust their own queue will read from this one.
    command_queue: Mutex<VecDeque<Command>>,
    /// When there are no commands in a thread's and the process' command queue, a binder thread can
    /// register with this [`WaitQueue`] to be notified when commands are available.
    waiters: Mutex<WaitQueue>,
    /// Outstanding references to handles within transaction buffers.
    /// When the process frees them with the BC_FREE_BUFFER, they will be released.
    transaction_refs: Mutex<BTreeMap<UserAddress, TransactionRefs>>,
}

/// References to binder objects that should be dropped when the buffer they're attached to is
/// released.
#[derive(Debug)]
struct TransactionRefs {
    handles: Vec<Handle>,
}

impl BinderProcess {
    fn new(pid: pid_t) -> Self {
        Self {
            pid,
            shared_memory: Mutex::new(None),
            thread_pool: RwLock::new(ThreadPool::default()),
            objects: Mutex::new(BTreeMap::new()),
            handles: Mutex::new(HandleTable::default()),
            command_queue: Mutex::new(VecDeque::new()),
            waiters: Mutex::new(WaitQueue::default()),
            transaction_refs: Mutex::new(BTreeMap::new()),
        }
    }

    /// Enqueues `command` for the process and wakes up any thread that is waiting for commands.
    pub fn enqueue_command(&self, command: Command) {
        self.command_queue.lock().push_back(command);
        self.waiters.lock().notify_events(FdEvents::POLLIN);
    }

    /// Finds the binder object that corresponds to the process-local addresses `local`, or creates
    /// a new [`BinderObject`] to represent the one in the process.
    pub fn find_or_register_object(
        self: &Arc<BinderProcess>,
        local: LocalBinderObject,
    ) -> Arc<BinderObject> {
        let mut objects = self.objects.lock();
        match objects.entry(local.weak_ref_addr) {
            BTreeMapEntry::Occupied(mut entry) => {
                if let Some(binder_object) = entry.get().upgrade() {
                    binder_object
                } else {
                    let binder_object =
                        Arc::new(BinderObject { owner: Arc::downgrade(self), local });
                    entry.insert(Arc::downgrade(&binder_object));
                    binder_object
                }
            }
            BTreeMapEntry::Vacant(entry) => {
                let binder_object = Arc::new(BinderObject { owner: Arc::downgrade(self), local });
                entry.insert(Arc::downgrade(&binder_object));
                binder_object
            }
        }
    }
}

/// The mapped VMO shared between userspace and the binder driver.
///
/// The binder driver copies messages from one process to another, which essentially amounts to
/// a copy between VMOs. It is not possible to copy directly between VMOs without an intermediate
/// copy, and the binder driver must only perform one copy for performance reasons.
///
/// The memory allocated to a binder process is shared with the binder driver, and mapped into
/// the kernel's address space so that a VMO read operation can copy directly into the mapped VMO.
#[derive(Debug)]
struct SharedMemory {
    /// The address in kernel address space where the VMO is mapped.
    kernel_address: *mut u8,
    /// The address in user address space where the VMO is mapped.
    user_address: UserAddress,
    /// The length of the shared memory mapping.
    length: usize,
    /// The next free address in our bump allocator.
    next_free_offset: usize,
}

impl Drop for SharedMemory {
    fn drop(&mut self) {
        let kernel_root_vmar = fuchsia_runtime::vmar_root_self();

        // SAFETY: This object hands out references to the mapped memory, but the borrow checker
        // ensures correct lifetimes.
        let res = unsafe { kernel_root_vmar.unmap(self.kernel_address as usize, self.length) };
        match res {
            Ok(()) => {}
            Err(status) => {
                tracing::error!("failed to unmap shared binder region from kernel: {:?}", status);
            }
        }
    }
}

// SAFETY: SharedMemory has exclusive ownership of the `kernel_address` pointer, so it is safe to
// send across threads.
unsafe impl Send for SharedMemory {}

impl SharedMemory {
    fn map(vmo: &zx::Vmo, user_address: UserAddress, length: usize) -> Result<Self, Errno> {
        // Map the VMO into the kernel's address space.
        let kernel_root_vmar = fuchsia_runtime::vmar_root_self();
        let kernel_address = kernel_root_vmar
            .map(0, vmo, 0, length, zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE)
            .map_err(|status| {
                tracing::error!("failed to map shared binder region in kernel: {:?}", status);
                errno!(ENOMEM)
            })?;
        Ok(Self {
            kernel_address: kernel_address as *mut u8,
            user_address,
            length,
            next_free_offset: 0,
        })
    }

    // This is a temporary implementation of an allocator and should be replaced by something
    // more sophisticated. It currently implements a bump allocator strategy.
    fn allocate_buffer<'a>(
        &'a mut self,
        data_length: usize,
        offsets_length: usize,
    ) -> Result<SharedBuffer<'a>, Errno> {
        // Round `data_length` up to the nearest multiple of 8, so that the offsets buffer is
        // aligned when we pack it next to the data buffer.
        let data_cap = round_up_to_increment(data_length, std::mem::size_of::<binder_uintptr_t>())?;
        // Ensure that the offsets length is valid.
        if offsets_length % std::mem::size_of::<binder_uintptr_t>() != 0 {
            return error!(EINVAL);
        }
        let total_length = data_cap.checked_add(offsets_length).ok_or_else(|| errno!(EINVAL))?;
        if let Some(offset) = self.next_free_offset.checked_add(total_length) {
            if offset <= self.length {
                let this_offset = self.next_free_offset;
                self.next_free_offset = offset;

                return Ok(SharedBuffer {
                    memory: self,
                    data_offset: this_offset,
                    data_length,
                    offsets_offset: this_offset + data_cap,
                    offsets_length,
                });
            }
        }
        error!(ENOMEM)
    }

    // This temporary allocator implementation does not reclaim free buffers.
    fn free_buffer(&mut self, buffer: UserAddress) -> Result<(), Errno> {
        // Sanity check that the buffer being freed came from this memory region.
        if buffer < self.user_address || buffer >= self.user_address + self.length {
            return error!(EINVAL);
        }
        // Bump allocators don't reclaim memory.
        Ok(())
    }
}

/// A buffer of memory allocated from a binder process' [`SharedMemory`].
#[derive(Debug)]
struct SharedBuffer<'a> {
    memory: &'a SharedMemory,
    // Offset into the shared memory region where the data buffer begins.
    data_offset: usize,
    // The length of the data buffer.
    data_length: usize,
    // Offset into the shared memory region where the offsets buffer begins.
    offsets_offset: usize,
    // The length of the offsets buffer.
    offsets_length: usize,
}

impl<'a> SharedBuffer<'a> {
    fn as_mut_bytes(&mut self) -> (&mut [u8], &mut [binder_uintptr_t]) {
        // SAFETY: `data_offset + data_length` was bounds-checked by `allocate_buffer`, and the
        // memory region pointed to was zero-allocated by mapping a new VMO.
        let data = unsafe {
            std::slice::from_raw_parts_mut(
                self.memory.kernel_address.add(self.data_offset),
                self.data_length,
            )
        };
        // SAFETY: `offsets_offset + offsets_length` was bounds-checked by `allocate_buffer`, the
        // size of `offsets_length` was checked to be a multiple of `binder_uintptr_t`, and the
        // memory region pointed to was zero-allocated by mapping a new VMO.
        let offsets = unsafe {
            std::slice::from_raw_parts_mut(
                self.memory.kernel_address.add(self.offsets_offset) as *mut binder_uintptr_t,
                self.offsets_length / std::mem::size_of::<binder_uintptr_t>(),
            )
        };
        (data, offsets)
    }

    fn data_user_buffer(&self) -> UserBuffer {
        UserBuffer {
            address: self.memory.user_address + self.data_offset,
            length: self.data_length,
        }
    }

    fn offsets_user_buffer(&self) -> UserBuffer {
        UserBuffer {
            address: self.memory.user_address + self.offsets_offset,
            length: self.offsets_length,
        }
    }
}

/// The set of threads that are interacting with the binder driver for a given process.
#[derive(Debug, Default)]
struct ThreadPool(BTreeMap<pid_t, Arc<BinderThread>>);

impl ThreadPool {
    fn find_or_register_thread(
        &mut self,
        binder_proc: &Arc<BinderProcess>,
        tid: pid_t,
    ) -> Arc<BinderThread> {
        self.0.entry(tid).or_insert_with(|| Arc::new(BinderThread::new(binder_proc, tid))).clone()
    }

    fn find_thread(&self, tid: pid_t) -> Result<Arc<BinderThread>, Errno> {
        self.0.get(&tid).cloned().ok_or_else(|| errno!(ENOENT))
    }

    /// Finds the first available binder thread that is registered with the driver, is not in the
    /// middle of a transaction, and has no work to do.
    fn find_available_thread(&self) -> Option<Arc<BinderThread>> {
        self.0
            .values()
            .find(|thread| {
                let thread_state = thread.read();
                thread_state
                    .registration
                    .intersects(RegistrationState::MAIN | RegistrationState::REGISTERED)
                    && thread_state.command_queue.is_empty()
                    && thread_state.waiter.is_some()
                    && thread_state.transactions.is_empty()
            })
            .cloned()
    }
}

/// Table containing handles to remote binder objects.
#[derive(Debug, Default)]
struct HandleTable {
    table: Slab<BinderObjectRef>,
}

/// A reference to a binder object in another process.
#[derive(Debug)]
enum BinderObjectRef {
    /// A strong reference that keeps the remote binder object from being destroyed.
    StrongRef { strong_ref: Arc<BinderObject>, strong_count: usize, weak_count: usize },
    /// A weak reference that does not keep the remote binder object from being destroyed, but still
    /// occupies a place in the handle table.
    WeakRef { weak_ref: Weak<BinderObject>, weak_count: usize },
}

/// Instructs the [`HandleTable`] on what to do with a handle after the
/// [`BinderObjectRef::dec_strong`] and [`BinderObjectRef::dec_weak`] operations.
enum HandleAction {
    Keep,
    Drop,
}

impl BinderObjectRef {
    /// Increments the strong reference count of the binder object reference, promoting the
    /// reference to a strong reference if it was weak, or failing if the object no longer exists.
    fn inc_strong(&mut self) -> Result<(), Errno> {
        match self {
            BinderObjectRef::StrongRef { strong_count, .. } => *strong_count += 1,
            BinderObjectRef::WeakRef { weak_ref, weak_count } => {
                let strong_ref = weak_ref.upgrade().ok_or_else(|| errno!(EINVAL))?;
                *self = BinderObjectRef::StrongRef {
                    strong_ref,
                    strong_count: 1,
                    weak_count: *weak_count,
                };
            }
        }
        Ok(())
    }

    /// Increments the weak reference count of the binder object reference.
    fn inc_weak(&mut self) {
        match self {
            BinderObjectRef::StrongRef { weak_count, .. }
            | BinderObjectRef::WeakRef { weak_count, .. } => *weak_count += 1,
        }
    }

    /// Decrements the strong reference count of the binder object reference, demoting the reference
    /// to a weak reference or returning [`HandleAction::Drop`] if the handle should be dropped.
    fn dec_strong(&mut self) -> Result<HandleAction, Errno> {
        match self {
            BinderObjectRef::WeakRef { .. } => return error!(EINVAL),
            BinderObjectRef::StrongRef { strong_ref, strong_count, weak_count } => {
                *strong_count -= 1;
                if *strong_count == 0 {
                    let weak_count = *weak_count;
                    *self = BinderObjectRef::WeakRef {
                        weak_ref: Arc::downgrade(strong_ref),
                        weak_count,
                    };
                    if weak_count == 0 {
                        return Ok(HandleAction::Drop);
                    }
                }
            }
        }
        Ok(HandleAction::Keep)
    }

    /// Decrements the weak reference count of the binder object reference, returning
    /// [`HandleAction::Drop`] if the strong count and weak count both drop to 0.
    fn dec_weak(&mut self) -> Result<HandleAction, Errno> {
        match self {
            BinderObjectRef::StrongRef { weak_count, .. } => {
                if *weak_count == 0 {
                    error!(EINVAL)
                } else {
                    *weak_count -= 1;
                    Ok(HandleAction::Keep)
                }
            }
            BinderObjectRef::WeakRef { weak_count, .. } => {
                *weak_count -= 1;
                if *weak_count == 0 {
                    Ok(HandleAction::Drop)
                } else {
                    Ok(HandleAction::Keep)
                }
            }
        }
    }
}

impl HandleTable {
    /// Inserts a reference to a binder object, returning a handle that represents it. The handle
    /// may be an existing handle if the object was already present in the table. If the client does
    /// not acquire a strong reference to this handle before the transaction that inserted it is
    /// complete, the handle will be dropped.
    fn insert_for_transaction(&mut self, object: Arc<BinderObject>) -> Handle {
        let existing_idx = self.table.iter().find_map(|(idx, object_ref)| match object_ref {
            BinderObjectRef::WeakRef { weak_ref, .. } => {
                weak_ref.upgrade().and_then(|strong_ref| {
                    (strong_ref.local.weak_ref_addr == object.local.weak_ref_addr).then(|| idx)
                })
            }
            BinderObjectRef::StrongRef { strong_ref, .. } => {
                (strong_ref.local.weak_ref_addr == object.local.weak_ref_addr).then(|| idx)
            }
        });

        if let Some(existing_idx) = existing_idx {
            return Handle::Object { index: existing_idx };
        }

        let new_idx = self.table.insert(BinderObjectRef::StrongRef {
            strong_ref: object,
            strong_count: 1,
            weak_count: 0,
        });
        Handle::Object { index: new_idx }
    }

    /// Retrieves a reference to a binder object at index `idx`.
    fn get(&self, idx: usize) -> Result<Arc<BinderObject>, Errno> {
        let object_ref = self.table.get(idx).ok_or_else(|| errno!(ENOENT))?;
        match object_ref {
            BinderObjectRef::WeakRef { weak_ref, .. } => {
                weak_ref.upgrade().ok_or_else(|| errno!(ENOENT))
            }
            BinderObjectRef::StrongRef { strong_ref, .. } => Ok(strong_ref.clone()),
        }
    }

    /// Increments the strong reference count of the binder object reference at index `idx`,
    /// failing if the object no longer exists.
    fn inc_strong(&mut self, idx: usize) -> Result<(), Errno> {
        self.table.get_mut(idx).ok_or_else(|| errno!(ENOENT))?.inc_strong()
    }

    /// Increments the weak reference count of the binder object reference at index `idx`, failing
    /// if the object does not exist.
    fn inc_weak(&mut self, idx: usize) -> Result<(), Errno> {
        Ok(self.table.get_mut(idx).ok_or_else(|| errno!(ENOENT))?.inc_weak())
    }

    /// Decrements the strong reference count of the binder object reference at index `idx`, failing
    /// if the object no longer exists.
    fn dec_strong(&mut self, idx: usize) -> Result<(), Errno> {
        let object_ref = self.table.get_mut(idx).ok_or_else(|| errno!(ENOENT))?;
        if let HandleAction::Drop = object_ref.dec_strong()? {
            self.table.remove(idx);
        }
        Ok(())
    }

    /// Decrements the weak reference count of the binder object reference at index `idx`, failing
    /// if the object does not exist.
    fn dec_weak(&mut self, idx: usize) -> Result<(), Errno> {
        let object_ref = self.table.get_mut(idx).ok_or_else(|| errno!(ENOENT))?;
        if let HandleAction::Drop = object_ref.dec_weak()? {
            self.table.remove(idx);
        }
        Ok(())
    }
}

#[derive(Debug)]
struct BinderThread {
    tid: pid_t,
    /// The mutable state of the binder thread, protected by a single lock.
    state: RwLock<BinderThreadState>,
}

impl BinderThread {
    fn new(binder_proc: &Arc<BinderProcess>, tid: pid_t) -> Self {
        Self { tid, state: RwLock::new(BinderThreadState::new(binder_proc)) }
    }

    /// Acquire a reader lock to the binder thread's mutable state.
    pub fn read<'a>(&'a self) -> RwLockReadGuard<'a, BinderThreadState> {
        self.state.read()
    }

    /// Acquire a writer lock to the binder thread's mutable state.
    pub fn write<'a>(&'a self) -> RwLockWriteGuard<'a, BinderThreadState> {
        self.state.write()
    }
}

/// The mutable state of a binder thread.
#[derive(Debug)]
struct BinderThreadState {
    /// The process this thread belongs to.
    process: Weak<BinderProcess>,
    /// The registered state of the thread.
    registration: RegistrationState,
    /// The stack of transactions that are active for this thread.
    transactions: Vec<Transaction>,
    /// The binder driver uses this queue to communicate with a binder thread. When a binder thread
    /// issues a [`BINDER_IOCTL_WRITE_READ`] ioctl, it will read from this command queue.
    command_queue: VecDeque<Command>,
    /// The [`Waiter`] object the binder thread is waiting on when there are no commands in the
    /// command queue. If `None`, the binder thread is not currently waiting.
    waiter: Option<Arc<Waiter>>,
}

impl BinderThreadState {
    fn new(binder_proc: &Arc<BinderProcess>) -> Self {
        Self {
            process: Arc::downgrade(binder_proc),
            registration: RegistrationState::empty(),
            transactions: Vec::new(),
            command_queue: VecDeque::new(),
            waiter: None,
        }
    }

    /// Enqueues `command` for the thread and wakes it up if necessary.
    pub fn enqueue_command(&mut self, command: Command) {
        self.command_queue.push_back(command);
        if let Some(waiter) = self.waiter.take() {
            // Wake up the thread that is waiting.
            waiter.wake_immediately(FdEvents::POLLIN.mask(), WaitCallback::none());
        }
        // Notify any threads that are waiting on events from the binder driver FD.
        if let Some(binder_proc) = self.process.upgrade() {
            binder_proc.waiters.lock().notify_events(FdEvents::POLLIN);
        }
    }

    /// Get the binder thread (pid and tid) to reply to, or fail if there is no ongoing transaction.
    pub fn transaction_caller(&self) -> Result<(pid_t, pid_t), Errno> {
        self.transactions
            .last()
            .map(|transaction| (transaction.peer_pid, transaction.peer_tid))
            .ok_or_else(|| errno!(EINVAL))
    }
}

bitflags! {
    /// The registration state of a thread.
    struct RegistrationState: u32 {
        /// The thread is the main binder thread.
        const MAIN = 1;

        /// The thread is an auxiliary binder thread.
        const REGISTERED = 1 << 2;
    }
}

/// Commands for a binder thread to execute.
#[derive(Debug, PartialEq, Eq)]
enum Command {
    /// Notifies a binder thread that a remote process acquired a strong reference to the specified
    /// binder object. The object should not be destroyed until a [`Command::ReleaseRef`] is
    /// delivered.
    AcquireRef(LocalBinderObject),
    /// Notifies a binder thread that there are no longer any remote processes holding strong
    /// references to the specified binder object. The object may still have references within the
    /// owning process.
    ReleaseRef(LocalBinderObject),
    /// Commands a binder thread to start processing an incoming transaction from another binder
    /// process.
    Transaction(Transaction),
    /// Commands a binder thread to process an incoming reply to its transaction.
    Reply(Transaction),
    /// Notifies a binder thread that a transaction has completed.
    TransactionComplete,
    /// Notifies the initiator of a transaction that the recipient is dead.
    DeadReply,
}

impl Command {
    /// Returns the command's BR_* code for serialization.
    fn driver_return_code(&self) -> binder_driver_return_protocol {
        match self {
            Command::AcquireRef(..) => binder_driver_return_protocol_BR_ACQUIRE,
            Command::ReleaseRef(..) => binder_driver_return_protocol_BR_RELEASE,
            Command::Transaction(..) => binder_driver_return_protocol_BR_TRANSACTION,
            Command::Reply(..) => binder_driver_return_protocol_BR_REPLY,
            Command::TransactionComplete => binder_driver_return_protocol_BR_TRANSACTION_COMPLETE,
            Command::DeadReply => binder_driver_return_protocol_BR_DEAD_REPLY,
        }
    }

    /// Serializes and writes the command into userspace memory at `buffer`.
    fn write_to_memory(&self, mm: &MemoryManager, buffer: &UserBuffer) -> Result<usize, Errno> {
        match self {
            Command::AcquireRef(obj) | Command::ReleaseRef(obj) => {
                #[repr(C, packed)]
                #[derive(AsBytes)]
                struct AcquireRefData {
                    command: binder_driver_return_protocol,
                    weak_ref_addr: u64,
                    strong_ref_addr: u64,
                }
                if buffer.length < std::mem::size_of::<AcquireRefData>() {
                    return error!(ENOMEM);
                }
                mm.write_object(
                    UserRef::new(buffer.address),
                    &AcquireRefData {
                        command: self.driver_return_code(),
                        weak_ref_addr: obj.weak_ref_addr.ptr() as u64,
                        strong_ref_addr: obj.strong_ref_addr.ptr() as u64,
                    },
                )
            }
            Command::Transaction(transaction) | Command::Reply(transaction) => {
                #[repr(C, packed)]
                #[derive(AsBytes)]
                struct TransactionData {
                    command: binder_driver_return_protocol,
                    transaction: [u8; std::mem::size_of::<binder_transaction_data>()],
                }
                if buffer.length < std::mem::size_of::<TransactionData>() {
                    return error!(ENOMEM);
                }
                mm.write_object(
                    UserRef::new(buffer.address),
                    &TransactionData {
                        command: self.driver_return_code(),
                        transaction: transaction.as_bytes(),
                    },
                )
            }
            Command::TransactionComplete => {
                if buffer.length < std::mem::size_of::<binder_driver_return_protocol>() {
                    return error!(ENOMEM);
                }
                mm.write_object(UserRef::new(buffer.address), &self.driver_return_code())
            }
            Command::DeadReply => {
                if buffer.length < std::mem::size_of::<binder_driver_return_protocol>() {
                    return error!(ENOMEM);
                }
                mm.write_object(UserRef::new(buffer.address), &self.driver_return_code())
            }
        }
    }
}

/// A binder object, which is owned by a process. Process-local unique memory addresses identify it
/// to the owner.
#[derive(Debug)]
struct BinderObject {
    /// The owner of the binder object. If the owner cannot be promoted to a strong reference,
    /// the object is dead.
    owner: Weak<BinderProcess>,
    /// The addresses to the binder (weak and strong) in the owner's address space. These are
    /// treated as opaque identifiers in the driver, and only have meaning to the owning process.
    local: LocalBinderObject,
}

impl Drop for BinderObject {
    fn drop(&mut self) {
        if let Some(owner) = self.owner.upgrade() {
            // The owner process is not dead, so tell it that the last remote reference has been
            // released.
            let thread_pool = owner.thread_pool.read();
            if let Some(binder_thread) = thread_pool.find_available_thread() {
                binder_thread.write().enqueue_command(Command::ReleaseRef(self.local.clone()));
            } else {
                owner.enqueue_command(Command::ReleaseRef(self.local.clone()));
            }
        }
    }
}

/// A binder object.
/// All addresses are in the owning process' address space.
#[derive(Debug, Clone, Eq, PartialEq)]
struct LocalBinderObject {
    /// Address to the weak ref-count structure. This uniquely identifies a binder object within
    /// a process. Guaranteed to exist.
    weak_ref_addr: UserAddress,
    /// Address to the strong ref-count structure (actual object). May not exist if the object was
    /// destroyed.
    strong_ref_addr: UserAddress,
}

/// Non-union version of [`binder_transaction_data`].
#[derive(Debug, PartialEq, Eq)]
struct Transaction {
    peer_pid: pid_t,
    peer_tid: pid_t,
    peer_euid: u32,

    object: FlatBinderObject,
    code: u32,
    flags: u32,

    data_buffer: UserBuffer,
    offsets_buffer: UserBuffer,
}

impl Transaction {
    fn as_bytes(&self) -> [u8; std::mem::size_of::<binder_transaction_data>()] {
        match self.object {
            FlatBinderObject::Remote { handle } => {
                struct_with_union_into_bytes!(binder_transaction_data {
                    target.handle: handle.into(),
                    cookie: 0,
                    code: self.code,
                    flags: self.flags,
                    sender_pid: self.peer_pid,
                    sender_euid: self.peer_euid,
                    data_size: self.data_buffer.length as u64,
                    offsets_size: self.offsets_buffer.length as u64,
                    data.ptr: binder_transaction_data__bindgen_ty_2__bindgen_ty_1 {
                        buffer: self.data_buffer.address.ptr() as u64,
                        offsets: self.offsets_buffer.address.ptr() as u64,
                    },
                })
            }
            FlatBinderObject::Local { ref object } => {
                struct_with_union_into_bytes!(binder_transaction_data {
                    target.ptr: object.weak_ref_addr.ptr() as u64,
                    cookie: object.strong_ref_addr.ptr() as u64,
                    code: self.code,
                    flags: self.flags,
                    sender_pid: self.peer_pid,
                    sender_euid: self.peer_euid,
                    data_size: self.data_buffer.length as u64,
                    offsets_size: self.offsets_buffer.length as u64,
                    data.ptr: binder_transaction_data__bindgen_ty_2__bindgen_ty_1 {
                        buffer: self.data_buffer.address.ptr() as u64,
                        offsets: self.offsets_buffer.address.ptr() as u64,
                    },
                })
            }
        }
    }
}

/// Non-union version of [`flat_binder_object`].
#[derive(Debug, PartialEq, Eq)]
enum FlatBinderObject {
    Local { object: LocalBinderObject },
    Remote { handle: Handle },
}

/// A handle to a binder object.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Handle {
    /// Special handle `0` to the binder `IServiceManager` object within the context manager
    /// process.
    SpecialServiceManager,
    /// A handle to a binder object in another process.
    Object {
        /// The index of the binder object in a process' handle table.
        /// This is `handle - 1`, because the special handle 0 is reserved.
        index: usize,
    },
}

impl Handle {
    pub const fn from_raw(handle: u32) -> Handle {
        if handle == 0 {
            Handle::SpecialServiceManager
        } else {
            Handle::Object { index: handle as usize - 1 }
        }
    }

    /// Returns the underlying object index the handle represents, panicking if the handle was the
    /// special `0` handle.
    #[cfg(test)]
    pub fn object_index(&self) -> usize {
        match self {
            Handle::SpecialServiceManager => {
                panic!("handle does not have an object index")
            }
            Handle::Object { index } => *index,
        }
    }
}

impl From<u32> for Handle {
    fn from(handle: u32) -> Self {
        Handle::from_raw(handle)
    }
}

impl From<Handle> for u32 {
    fn from(handle: Handle) -> Self {
        match handle {
            Handle::SpecialServiceManager => 0,
            Handle::Object { index } => (index as u32) + 1,
        }
    }
}

impl std::fmt::Display for Handle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Handle::SpecialServiceManager => f.write_str("0"),
            Handle::Object { index } => f.write_fmt(format_args!("{}", index + 1)),
        }
    }
}

/// The ioctl character for all binder ioctls.
const BINDER_IOCTL_CHAR: u8 = b'b';

/// The ioctl for initiating a read-write transaction.
const BINDER_IOCTL_WRITE_READ: u32 =
    encode_ioctl_write_read::<binder_write_read>(BINDER_IOCTL_CHAR, 1);

/// The ioctl for setting the maximum number of threads the process wants to create for binder work.
const BINDER_IOCTL_SET_MAX_THREADS: u32 = encode_ioctl_write::<u32>(BINDER_IOCTL_CHAR, 5);

/// The ioctl for requests to become context manager.
const BINDER_IOCTL_SET_CONTEXT_MGR: u32 = encode_ioctl_write::<u32>(BINDER_IOCTL_CHAR, 7);

/// The ioctl for retrieving the kernel binder version.
const BINDER_IOCTL_VERSION: u32 = encode_ioctl_write_read::<binder_version>(BINDER_IOCTL_CHAR, 9);

/// The ioctl for requests to become context manager, v2.
const BINDER_IOCTL_SET_CONTEXT_MGR_EXT: u32 =
    encode_ioctl_write::<flat_binder_object>(BINDER_IOCTL_CHAR, 13);

// The ioctl for enabling one-way transaction spam detection.
const BINDER_IOCTL_ENABLE_ONEWAY_SPAM_DETECTION: u32 =
    encode_ioctl_write::<u32>(BINDER_IOCTL_CHAR, 16);

struct BinderDriver {
    /// The "name server" process that is addressed via the special handle 0 and is responsible
    /// for implementing the binder protocol `IServiceManager`.
    context_manager: RwLock<Option<Weak<BinderProcess>>>,

    /// Manages the internal state of each process interacting with the binder driver.
    procs: RwLock<BTreeMap<pid_t, Weak<BinderProcess>>>,
}

impl BinderDriver {
    fn new() -> Self {
        Self { context_manager: RwLock::new(None), procs: RwLock::new(BTreeMap::new()) }
    }

    fn find_process(&self, pid: pid_t) -> Result<Arc<BinderProcess>, Errno> {
        self.procs.read().get(&pid).and_then(Weak::upgrade).ok_or_else(|| errno!(ENOENT))
    }

    /// Creates the binder process state to represent a process with `pid`.
    #[cfg(test)]
    fn create_process(&self, pid: pid_t) -> Arc<BinderProcess> {
        let binder_process = Arc::new(BinderProcess::new(pid));
        assert!(
            self.procs.write().insert(pid, Arc::downgrade(&binder_process)).is_none(),
            "process with same pid created"
        );
        binder_process
    }

    /// Creates the binder process and thread state to represent a process with `pid` and one main
    /// thread.
    #[cfg(test)]
    fn create_process_and_thread(&self, pid: pid_t) -> (Arc<BinderProcess>, Arc<BinderThread>) {
        let binder_process = Arc::new(BinderProcess::new(pid));
        assert!(
            self.procs.write().insert(pid, Arc::downgrade(&binder_process)).is_none(),
            "process with same pid created"
        );
        let binder_thread =
            binder_process.thread_pool.write().find_or_register_thread(&binder_process, pid);
        (binder_process, binder_thread)
    }

    fn get_context_manager(&self) -> Result<Arc<BinderProcess>, Errno> {
        self.context_manager.read().as_ref().and_then(Weak::upgrade).ok_or_else(|| errno!(ENOENT))
    }

    fn ioctl(
        &self,
        current_task: &CurrentTask,
        binder_proc: &Arc<BinderProcess>,
        binder_thread: &Arc<BinderThread>,
        request: u32,
        user_arg: UserAddress,
    ) -> Result<SyscallResult, Errno> {
        match request {
            BINDER_IOCTL_VERSION => {
                // A thread is requesting the version of this binder driver.

                if user_arg.is_null() {
                    return error!(EINVAL);
                }
                let response =
                    binder_version { protocol_version: BINDER_CURRENT_PROTOCOL_VERSION as i32 };
                current_task.mm.write_object(UserRef::new(user_arg), &response)?;
                Ok(SUCCESS)
            }
            BINDER_IOCTL_SET_CONTEXT_MGR | BINDER_IOCTL_SET_CONTEXT_MGR_EXT => {
                // A process is registering itself as the context manager.

                if user_arg.is_null() {
                    return error!(EINVAL);
                }

                // TODO: Read the flat_binder_object when ioctl is BINDER_IOCTL_SET_CONTEXT_MGR_EXT.

                *self.context_manager.write() = Some(Arc::downgrade(&binder_proc));
                Ok(SUCCESS)
            }
            BINDER_IOCTL_WRITE_READ => {
                // A thread is requesting to exchange data with the binder driver.

                if user_arg.is_null() {
                    return error!(EINVAL);
                }

                let user_ref = UserRef::new(user_arg);
                let mut input = binder_write_read::new_zeroed();
                current_task.mm.read_object(user_ref, &mut input)?;

                // We will be writing this back to userspace, don't trust what the client gave us.
                input.write_consumed = 0;
                input.read_consumed = 0;

                if input.write_size > 0 {
                    // The calling thread wants to write some data to the binder driver.
                    let mut cursor = UserMemoryCursor::new(
                        &*current_task.mm,
                        UserAddress::from(input.write_buffer),
                        input.write_size,
                    );

                    // Handle all the data the calling thread sent, which may include multiple
                    // commands.
                    while cursor.bytes_read() < input.write_size as usize {
                        self.handle_thread_write(
                            current_task,
                            &binder_proc,
                            &binder_thread,
                            &mut cursor,
                        )?;
                    }
                    input.write_consumed = cursor.bytes_read() as u64;
                }

                if input.read_size > 0 {
                    // The calling thread wants to read some data from the binder driver, blocking
                    // if there is nothing immediately available.
                    let read_buffer = UserBuffer {
                        address: UserAddress::from(input.read_buffer),
                        length: input.read_size as usize,
                    };
                    input.read_consumed = self.handle_thread_read(
                        current_task,
                        &*binder_proc,
                        &*binder_thread,
                        &read_buffer,
                    )? as u64;
                }

                // Write back to the calling thread how much data was read/written.
                current_task.mm.write_object(user_ref, &input)?;
                Ok(SUCCESS)
            }
            BINDER_IOCTL_SET_MAX_THREADS => {
                not_implemented!("binder ignoring SET_MAX_THREADS ioctl");
                Ok(SUCCESS)
            }
            BINDER_IOCTL_ENABLE_ONEWAY_SPAM_DETECTION => {
                not_implemented!("binder ignoring ENABLE_ONEWAY_SPAM_DETECTION ioctl");
                Ok(SUCCESS)
            }
            _ => {
                tracing::error!("binder received unknown ioctl request 0x{:08x}", request);
                error!(EINVAL)
            }
        }
    }

    /// Consumes one command from the userspace binder_write_read buffer and handles it.
    /// This method will never block.
    fn handle_thread_write<'a>(
        &self,
        current_task: &CurrentTask,
        binder_proc: &Arc<BinderProcess>,
        binder_thread: &Arc<BinderThread>,
        cursor: &mut UserMemoryCursor<'a>,
    ) -> Result<(), Errno> {
        let command = cursor.read_object::<binder_driver_command_protocol>()?;
        match command {
            binder_driver_command_protocol_BC_ENTER_LOOPER => {
                self.handle_looper_registration(binder_thread, RegistrationState::MAIN)
            }
            binder_driver_command_protocol_BC_REGISTER_LOOPER => {
                self.handle_looper_registration(binder_thread, RegistrationState::REGISTERED)
            }
            binder_driver_command_protocol_BC_INCREFS
            | binder_driver_command_protocol_BC_ACQUIRE
            | binder_driver_command_protocol_BC_DECREFS
            | binder_driver_command_protocol_BC_RELEASE => {
                let handle = cursor.read_object::<u32>()?.into();
                self.handle_refcount_operation(command, binder_proc, handle)
            }
            binder_driver_command_protocol_BC_INCREFS_DONE
            | binder_driver_command_protocol_BC_ACQUIRE_DONE => {
                let object = LocalBinderObject {
                    weak_ref_addr: UserAddress::from(cursor.read_object::<binder_uintptr_t>()?),
                    strong_ref_addr: UserAddress::from(cursor.read_object::<binder_uintptr_t>()?),
                };
                self.handle_refcount_operation_done(command, binder_thread, object)
            }
            binder_driver_command_protocol_BC_FREE_BUFFER => {
                let buffer_ptr = UserAddress::from(cursor.read_object::<binder_uintptr_t>()?);
                self.handle_free_buffer(binder_proc, buffer_ptr)
            }
            binder_driver_command_protocol_BC_REQUEST_DEATH_NOTIFICATION => {
                // A binder thread is requesting to be sent a notification when a remote binder
                // object dies.
                let handle = cursor.read_object::<u32>()?;
                let cookie = cursor.read_object::<binder_uintptr_t>()?;
                not_implemented!(
                    "binder thread {} BC_REQUEST_DEATH_NOTIFICATION for handle {} (cookie={:?})",
                    binder_thread.tid,
                    handle,
                    UserAddress::from(cookie)
                );
                Ok(())
            }
            binder_driver_command_protocol_BC_TRANSACTION => {
                let data = cursor.read_object::<binder_transaction_data>()?;
                self.handle_transaction(current_task, binder_proc, binder_thread, data)
            }
            binder_driver_command_protocol_BC_REPLY => {
                let data = cursor.read_object::<binder_transaction_data>()?;
                self.handle_reply(current_task, binder_proc, binder_thread, data)
            }
            _ => {
                tracing::error!("binder received unknown RW command: 0x{:08x}", command);
                error!(EINVAL)
            }
        }
    }

    /// Handle a binder thread's request to register itself with the binder driver.
    /// This makes the binder thread eligible for receiving commands from the driver.
    fn handle_looper_registration(
        &self,
        binder_thread: &Arc<BinderThread>,
        registration: RegistrationState,
    ) -> Result<(), Errno> {
        let mut thread_state = binder_thread.write();
        if thread_state
            .registration
            .intersects(RegistrationState::MAIN | RegistrationState::REGISTERED)
        {
            // This thread is already registered.
            error!(EINVAL)
        } else {
            thread_state.registration |= registration;
            Ok(())
        }
    }

    /// Handle a binder thread's request to increment/decrement a strong/weak reference to a remote
    /// binder object.
    fn handle_refcount_operation(
        &self,
        command: binder_driver_command_protocol,
        binder_proc: &Arc<BinderProcess>,
        handle: Handle,
    ) -> Result<(), Errno> {
        let idx = match handle {
            Handle::SpecialServiceManager => {
                // TODO: Figure out how to acquire/release refs for the context manager
                // object.
                not_implemented!("acquire/release refs for context manager object");
                return Ok(());
            }
            Handle::Object { index } => index,
        };

        let mut handles = binder_proc.handles.lock();
        match command {
            binder_driver_command_protocol_BC_ACQUIRE => handles.inc_strong(idx),
            binder_driver_command_protocol_BC_RELEASE => handles.dec_strong(idx),
            binder_driver_command_protocol_BC_INCREFS => handles.inc_weak(idx),
            binder_driver_command_protocol_BC_DECREFS => handles.dec_weak(idx),
            _ => unreachable!(),
        }
    }

    /// Handle a binder thread's notification that it successfully incremented a strong/weak
    /// reference to a local (in-process) binder object. This is in response to a
    /// `BR_ACQUIRE`/`BR_INCREFS` command.
    fn handle_refcount_operation_done(
        &self,
        command: binder_driver_command_protocol,
        binder_thread: &Arc<BinderThread>,
        object: LocalBinderObject,
    ) -> Result<(), Errno> {
        // TODO: When the binder driver keeps track of references internally, this should
        // reduce the temporary refcount that is held while the binder thread performs the
        // refcount.
        let msg = match command {
            binder_driver_command_protocol_BC_INCREFS_DONE => "BC_INCREFS_DONE",
            binder_driver_command_protocol_BC_ACQUIRE_DONE => "BC_ACQUIRE_DONE",
            _ => unreachable!(),
        };
        not_implemented!("binder thread {} {} {:?}", binder_thread.tid, msg, &object);
        Ok(())
    }

    /// A binder thread is done reading a buffer allocated to a transaction. The binder
    /// driver can reclaim this buffer.
    fn handle_free_buffer(
        &self,
        binder_proc: &Arc<BinderProcess>,
        buffer_ptr: UserAddress,
    ) -> Result<(), Errno> {
        // Drop the temporary references held during the transaction.
        {
            let transaction_refs = binder_proc.transaction_refs.lock().remove(&buffer_ptr);
            let mut handle_table = binder_proc.handles.lock();
            if let Some(transaction_refs) = transaction_refs {
                for handle in transaction_refs.handles {
                    match handle {
                        Handle::SpecialServiceManager => {}
                        Handle::Object { index } => {
                            let _: Result<(), Errno> = handle_table.dec_strong(index);
                        }
                    }
                }
            }
        }

        // Reclaim the memory.
        let mut shared_memory_lock = binder_proc.shared_memory.lock();
        let shared_memory = shared_memory_lock.as_mut().ok_or_else(|| errno!(ENOMEM))?;
        shared_memory.free_buffer(buffer_ptr)
    }

    /// A binder thread is starting a transaction on a remote binder object.
    fn handle_transaction(
        &self,
        current_task: &CurrentTask,
        binder_proc: &Arc<BinderProcess>,
        binder_thread: &Arc<BinderThread>,
        data: binder_transaction_data,
    ) -> Result<(), Errno> {
        // SAFETY: Transactions can only refer to handles.
        let handle = unsafe { data.target.handle }.into();

        let (object, target_proc) = match handle {
            Handle::SpecialServiceManager => {
                // This handle (0) always refers to the context manager, which is always "remote",
                // even for the context manager itself.
                (
                    FlatBinderObject::Remote { handle: Handle::SpecialServiceManager },
                    self.get_context_manager()?,
                )
            }
            Handle::Object { index } => {
                let object = binder_proc.handles.lock().get(index)?;
                if let Some(owner) = object.owner.upgrade() {
                    (FlatBinderObject::Local { object: object.local.clone() }, owner)
                } else {
                    // The binder object is dead, send an error to the caller.
                    binder_thread.write().enqueue_command(Command::DeadReply);
                    return Ok(());
                }
            }
        };

        // Copy the transaction data to the target process.
        let (data_buffer, offsets_buffer) = self.copy_transaction_buffers(
            current_task,
            binder_proc,
            binder_thread,
            &target_proc,
            &data,
        )?;

        if data.flags & transaction_flags_TF_ONE_WAY != 0 {
            // The caller is not expecting a reply.
            binder_thread.write().enqueue_command(Command::TransactionComplete);
        } else {
            // Create a new transaction on the sender so that they can wait on a reply.
            binder_thread.write().transactions.push(Transaction {
                peer_pid: target_proc.pid,
                peer_tid: 0,
                peer_euid: 0,

                object: FlatBinderObject::Remote { handle },
                code: data.code,
                flags: data.flags,

                data_buffer: UserBuffer::default(),
                offsets_buffer: UserBuffer::default(),
            });
        }

        let command = Command::Transaction(Transaction {
            peer_pid: binder_proc.pid,
            peer_tid: binder_thread.tid,
            peer_euid: current_task.read().creds.euid,

            object,
            code: data.code,
            flags: data.flags,

            data_buffer,
            offsets_buffer,
        });

        // Acquire an exclusive lock to prevent a thread from being scheduled twice.
        let target_thread_pool = target_proc.thread_pool.write();

        // Find a thread to handle the transaction, or use the process' command queue.
        if let Some(target_thread) = target_thread_pool.find_available_thread() {
            target_thread.write().enqueue_command(command);
        } else {
            target_proc.enqueue_command(command);
        }
        Ok(())
    }

    /// A binder thread is sending a reply to a transaction.
    fn handle_reply(
        &self,
        current_task: &CurrentTask,
        binder_proc: &Arc<BinderProcess>,
        binder_thread: &Arc<BinderThread>,
        data: binder_transaction_data,
    ) -> Result<(), Errno> {
        // Find the process and thread that initiated the transaction. This reply is for them.
        let (target_proc, target_thread) = {
            let (peer_pid, peer_tid) = binder_thread.read().transaction_caller()?;
            let target_proc = self.find_process(peer_pid)?;
            let target_thread = target_proc.thread_pool.read().find_thread(peer_tid)?;
            (target_proc, target_thread)
        };

        // Copy the transaction data to the target process.
        let (data_buffer, offsets_buffer) = self.copy_transaction_buffers(
            current_task,
            binder_proc,
            binder_thread,
            &target_proc,
            &data,
        )?;

        // Schedule the transaction on the target process' command queue.
        target_thread.write().enqueue_command(Command::Reply(Transaction {
            peer_pid: binder_proc.pid,
            peer_tid: binder_thread.tid,
            peer_euid: current_task.read().creds.euid,

            object: FlatBinderObject::Remote { handle: Handle::SpecialServiceManager },
            code: data.code,
            flags: data.flags,

            data_buffer,
            offsets_buffer,
        }));

        // Schedule the transaction complete command on the caller's command queue.
        binder_thread.write().enqueue_command(Command::TransactionComplete);

        Ok(())
    }

    /// Dequeues a command from the thread's command queue, or blocks until commands are available.
    fn handle_thread_read(
        &self,
        current_task: &CurrentTask,
        binder_proc: &BinderProcess,
        binder_thread: &BinderThread,
        read_buffer: &UserBuffer,
    ) -> Result<usize, Errno> {
        loop {
            // THREADING: Always acquire the [`BinderProcess::command_queue`] lock before the
            // [`BinderThread::state`] lock or else it may lead to deadlock.
            let mut proc_command_queue = binder_proc.command_queue.lock();
            let mut thread_state = binder_thread.write();

            // Select which command queue to read from, preferring the thread-local one.
            let command_queue = if !thread_state.command_queue.is_empty() {
                &mut thread_state.command_queue
            } else {
                &mut *proc_command_queue
            };

            if let Some(command) = command_queue.front() {
                // Attempt to write the command to the thread's buffer.
                let bytes_written = command.write_to_memory(&current_task.mm, read_buffer)?;

                // SAFETY: There is an item in the queue since we're in the `Some` branch.
                match command_queue.pop_front().unwrap() {
                    Command::Transaction(t) => {
                        // A transaction has begun, push it onto the transaction stack.
                        thread_state.transactions.push(t);
                    }
                    Command::Reply(..) | Command::TransactionComplete => {
                        // A transaction is complete, pop it from the transaction stack.
                        thread_state.transactions.pop();
                    }
                    Command::AcquireRef(..) | Command::ReleaseRef(..) | Command::DeadReply => {}
                }

                return Ok(bytes_written);
            }

            // No commands readily available to read. Wait for work.
            let waiter = Waiter::new();
            thread_state.waiter = Some(waiter.clone());
            drop(thread_state);
            drop(proc_command_queue);

            // Put this thread to sleep.
            scopeguard::defer! {
                binder_thread.write().waiter = None
            }
            waiter.wait(current_task)?;
        }
    }

    /// Copies transaction buffers from the source process' address space to a new buffer in the
    /// target process' shared binder VMO.
    /// Returns a pair of addresses, the first the address to the transaction data, the second the
    /// address to the offset buffer.
    fn copy_transaction_buffers(
        &self,
        current_task: &CurrentTask,
        source_proc: &Arc<BinderProcess>,
        source_thread: &Arc<BinderThread>,
        target_proc: &Arc<BinderProcess>,
        data: &binder_transaction_data,
    ) -> Result<(UserBuffer, UserBuffer), Errno> {
        // Get the shared memory of the target process.
        let mut shared_memory_lock = target_proc.shared_memory.lock();
        let shared_memory = shared_memory_lock.as_mut().ok_or_else(|| errno!(ENOMEM))?;

        // Allocate a buffer from the target process' shared memory.
        let mut shared_buffer =
            shared_memory.allocate_buffer(data.data_size as usize, data.offsets_size as usize)?;
        let (data_buffer, offsets_buffer) = shared_buffer.as_mut_bytes();

        // SAFETY: `binder_transaction_data` was read from a userspace VMO, which means that all
        // bytes are defined, making union access safe (even if the value is garbage).
        let userspace_addrs = unsafe { data.data.ptr };

        // Copy the data straight into the target's buffer.
        current_task.mm.read_memory(UserAddress::from(userspace_addrs.buffer), data_buffer)?;
        current_task.mm.read_objects(
            UserRef::new(UserAddress::from(userspace_addrs.offsets)),
            offsets_buffer,
        )?;

        // Fix up binder objects.
        if !offsets_buffer.is_empty() {
            // Translate any handles/fds from the source process' handle table to the target
            // process' handle table.
            let transaction_refs = self.translate_handles(
                source_proc,
                source_thread,
                target_proc,
                &offsets_buffer,
                data_buffer,
            )?;
            target_proc
                .transaction_refs
                .lock()
                .insert(shared_buffer.data_user_buffer().address, transaction_refs);
        }

        Ok((shared_buffer.data_user_buffer(), shared_buffer.offsets_user_buffer()))
    }

    /// Translates binder object handles from the sending process to the receiver process, patching
    /// the transaction data as needed.
    ///
    /// When a binder object is sent from one process to another, it must be added to the receiving
    /// process' handle table. Conversely, a handle being sent to the process that owns the
    /// underlying binder object should receive the actual pointers to the object.
    ///
    /// Returns [`TransactionRefs`], which contains the handles in the target process' handle table
    /// for which temporary strong references were acquired. When the buffer containing the
    /// translated binder handles is freed with the `BC_FREE_BUFFER` command, the strong references
    /// are released.
    fn translate_handles(
        &self,
        source_proc: &Arc<BinderProcess>,
        source_thread: &Arc<BinderThread>,
        target_proc: &Arc<BinderProcess>,
        offsets: &[binder_uintptr_t],
        transaction_data: &mut [u8],
    ) -> Result<TransactionRefs, Errno> {
        let mut handles = Vec::new();
        for offset in offsets {
            // Bounds-check the offset.
            let object_end = offset
                .checked_add(std::mem::size_of::<flat_binder_object>() as u64)
                .ok_or_else(|| errno!(EINVAL))? as usize;
            if object_end > transaction_data.len() {
                return error!(EINVAL);
            }
            // SAFETY: The pointer to `flat_binder_object` is within the bounds of the slice.
            let object_ptr = unsafe {
                transaction_data.as_mut_ptr().add(*offset as usize) as *mut flat_binder_object
            };
            // SAFETY: The object may not be aligned, so read a copy.
            let flat_object = unsafe { object_ptr.read_unaligned() };
            match flat_object.hdr.type_ {
                BINDER_TYPE_HANDLE => {
                    // The `flat_binder_object` is a binder handle.
                    // SAFETY: Union access is safe because backing memory was initialized by a VMO.
                    let handle = unsafe { flat_object.__bindgen_anon_1.handle }.into();
                    match handle {
                        Handle::SpecialServiceManager => {
                            // The special handle 0 does not need to be translated. It is universal.
                        }
                        Handle::Object { index } => {
                            let proxy = source_proc.handles.lock().get(index)?;
                            let patched = if std::ptr::eq(
                                Arc::as_ptr(target_proc),
                                proxy.owner.as_ptr(),
                            ) {
                                // The binder object belongs to the receiving process, so convert it
                                // from a handle to a local object.
                                struct_with_union_into_bytes!(flat_binder_object {
                                    hdr.type_: BINDER_TYPE_BINDER,
                                    __bindgen_anon_1.binder: proxy.local.weak_ref_addr.ptr() as u64,
                                    flags: flat_object.flags,
                                    cookie: proxy.local.strong_ref_addr.ptr() as u64,
                                })
                            } else {
                                // The binder object does not belong to the receiving process, so
                                // dup the handle in the receiving process' handle table.
                                let new_handle =
                                    target_proc.handles.lock().insert_for_transaction(proxy);

                                // Tie this handle's strong reference to be held as long as this
                                // buffer.
                                handles.push(new_handle);

                                struct_with_union_into_bytes!(flat_binder_object {
                                    hdr.type_: BINDER_TYPE_HANDLE,
                                    __bindgen_anon_1.handle: new_handle.into(),
                                    flags: flat_object.flags,
                                    cookie: flat_object.cookie,
                                })
                            };

                            // Write the translated `flat_binder_object` back to the buffer.
                            // SAFETY: `struct_with_union_into_bytes!` is used to ensure there are
                            // no uninitialized fields. The result of this is a byte slice, so we
                            // operate below on a byte slice instead of a pointer to
                            // `flat_binder_object`.
                            unsafe {
                                std::slice::from_raw_parts_mut(
                                    object_ptr as *mut u8,
                                    std::mem::size_of::<flat_binder_object>(),
                                )
                                .copy_from_slice(&patched[..]);
                            };
                        }
                    }
                }
                BINDER_TYPE_BINDER => {
                    // We are passing a binder object across process boundaries. We need
                    // to translate this address to some handle.

                    // SAFETY: Union access is safe because backing memory was initialized by a VMO.
                    let local = LocalBinderObject {
                        weak_ref_addr: UserAddress::from(unsafe {
                            flat_object.__bindgen_anon_1.binder
                        }),
                        strong_ref_addr: UserAddress::from(flat_object.cookie),
                    };

                    // Register this binder object if it hasn't already been registered.
                    let object = source_proc.find_or_register_object(local.clone());

                    // Tell the owning process that a remote process now has a strong reference to
                    // to this object.
                    source_thread.state.write().enqueue_command(Command::AcquireRef(local));

                    // Create a handle in the receiving process that references the binder object
                    // in the sender's process.
                    let handle = target_proc.handles.lock().insert_for_transaction(object);

                    // Tie this handle's strong reference to be held as long as this buffer.
                    handles.push(handle);

                    // Translate the `flat_binder_object` to refer to the handle.
                    let patched = struct_with_union_into_bytes!(flat_binder_object {
                        hdr.type_: BINDER_TYPE_HANDLE,
                        __bindgen_anon_1.handle: handle.into(),
                        flags: flat_object.flags,
                        cookie: 0,
                    });

                    // Write the translated `flat_binder_object` back to the buffer.
                    // SAFETY: `struct_with_union_into_bytes!` is used to ensure there are
                    // no uninitialized fields. The result of this is a byte slice, so we
                    // operate below on a byte slice instead of a pointer to
                    // `flat_binder_object`.
                    unsafe {
                        std::slice::from_raw_parts_mut(
                            object_ptr as *mut u8,
                            std::mem::size_of::<flat_binder_object>(),
                        )
                        .copy_from_slice(&patched[..]);
                    };
                }
                _ => {
                    tracing::error!("unknown object type {}", flat_object.hdr.type_);
                    return error!(EINVAL);
                }
            }
        }
        Ok(TransactionRefs { handles })
    }

    fn get_vmo(&self, length: usize) -> Result<zx::Vmo, Errno> {
        zx::Vmo::create(length as u64).map_err(|_| errno!(ENOMEM))
    }

    fn mmap(
        &self,
        current_task: &CurrentTask,
        binder_proc: &Arc<BinderProcess>,
        addr: DesiredAddress,
        length: usize,
        flags: zx::VmarFlags,
        mapping_options: MappingOptions,
        filename: NamespaceNode,
    ) -> Result<MappedVmo, Errno> {
        // Do not support mapping shared memory more than once.
        let mut shared_memory = binder_proc.shared_memory.lock();
        if shared_memory.is_some() {
            return error!(EINVAL);
        }

        // Create a VMO that will be shared between the driver and the client process.
        let vmo = Arc::new(self.get_vmo(length)?);

        // Map the VMO into the binder process' address space.
        let user_address = current_task.mm.map(
            addr,
            vmo.clone(),
            0,
            length,
            flags,
            mapping_options,
            Some(filename),
        )?;

        // Map the VMO into the driver's address space.
        match SharedMemory::map(&*vmo, user_address, length) {
            Ok(mem) => {
                *shared_memory = Some(mem);
                Ok(MappedVmo::new(vmo, user_address))
            }
            Err(err) => {
                // Try to cleanup by unmapping from userspace, but ignore any errors. We
                // can't really recover from them.
                let _ = current_task.mm.unmap(user_address, length);
                Err(err)
            }
        }
    }

    fn wait_async(
        &self,
        binder_proc: &Arc<BinderProcess>,
        binder_thread: &Arc<BinderThread>,
        waiter: &Arc<Waiter>,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitKey {
        // THREADING: Always acquire the [`BinderProcess::command_queue`] lock before the
        // [`BinderThread::state`] lock or else it may lead to deadlock.
        let proc_command_queue = binder_proc.command_queue.lock();
        let thread_state = binder_thread.write();

        if proc_command_queue.is_empty() && thread_state.command_queue.is_empty() {
            binder_proc.waiters.lock().wait_async_events(waiter, events, handler)
        } else {
            waiter.wake_immediately(FdEvents::POLLIN.mask(), handler)
        }
    }

    fn cancel_wait(&self, binder_proc: &Arc<BinderProcess>, key: WaitKey) {
        binder_proc.waiters.lock().cancel_wait(key);
    }
}

pub struct BinderFs(());
impl FileSystemOps for BinderFs {}

const BINDERS: &[&'static FsStr] = &[b"binder", b"hwbinder", b"vndbinder"];

impl BinderFs {
    pub fn new(kernel: &Kernel) -> Result<FileSystemHandle, Errno> {
        let fs = FileSystem::new_with_permanent_entries(BinderFs(()));
        fs.set_root(ROMemoryDirectory);
        for binder in BINDERS {
            BinderFs::add_binder(kernel, &fs, &binder)?;
        }
        Ok(fs)
    }

    fn add_binder(kernel: &Kernel, fs: &FileSystemHandle, name: &FsStr) -> Result<(), Errno> {
        let dev = kernel.device_registry.write().register_misc_chrdev(BinderDev::new())?;
        fs.root().add_node_ops_dev(name, mode!(IFCHR, 0o600), dev, SpecialNode)?;
        Ok(())
    }
}

pub fn create_binders(kernel: &Kernel) -> Result<(), Errno> {
    for binder in BINDERS {
        BinderFs::add_binder(kernel, dev_tmp_fs(kernel), binder)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mm::PAGE_SIZE;
    use crate::testing::*;
    use assert_matches::assert_matches;

    const BASE_ADDR: UserAddress = UserAddress::from(0x0000000000000100);
    const VMO_LENGTH: usize = 4096;

    #[fuchsia::test]
    fn handle_tests() {
        assert_matches!(Handle::from(0), Handle::SpecialServiceManager);
        assert_matches!(Handle::from(1), Handle::Object { index: 0 });
        assert_matches!(Handle::from(2), Handle::Object { index: 1 });
        assert_matches!(Handle::from(99), Handle::Object { index: 98 });
    }

    #[fuchsia::test]
    fn handle_0_fails_when_context_manager_is_not_set() {
        let driver = BinderDriver::new();
        assert_eq!(
            driver.get_context_manager().expect_err("unexpectedly succeeded"),
            errno!(ENOENT),
        );
    }

    #[fuchsia::test]
    fn handle_0_succeeds_when_context_manager_is_set() {
        let driver = BinderDriver::new();
        let context_manager = driver.create_process(1);
        *driver.context_manager.write() = Some(Arc::downgrade(&context_manager));
        let object = driver.get_context_manager().expect("failed to find handle 0");
        assert!(Arc::ptr_eq(&context_manager, &object));
    }

    #[fuchsia::test]
    fn fail_to_retrieve_non_existing_handle() {
        let driver = BinderDriver::new();
        let binder_proc = driver.create_process(1);
        assert_eq!(
            binder_proc.handles.lock().get(3).expect_err("unexpectedly succeeded"),
            errno!(ENOENT),
        );
    }

    #[fuchsia::test]
    fn handle_is_dropped_after_transaction_finishes() {
        let driver = BinderDriver::new();
        let proc_1 = driver.create_process(1);
        let proc_2 = driver.create_process(2);

        let transaction_ref = Arc::new(BinderObject {
            owner: Arc::downgrade(&proc_1),
            local: LocalBinderObject {
                weak_ref_addr: UserAddress::from(0xffffffffffffffff),
                strong_ref_addr: UserAddress::from(0x1111111111111111),
            },
        });

        // Transactions always take a strong reference to binder objects.
        let handle = proc_2.handles.lock().insert_for_transaction(transaction_ref.clone());

        // The object should be present in the handle table until a strong decrement.
        assert!(Arc::ptr_eq(
            &proc_2.handles.lock().get(handle.object_index()).expect("valid object"),
            &transaction_ref
        ));

        // Drop the transaction reference.
        proc_2.handles.lock().dec_strong(handle.object_index()).expect("dec_strong");

        // The handle should now have been dropped.
        proc_2
            .handles
            .lock()
            .get(handle.object_index())
            .expect_err("handle should have been dropped");
    }

    #[fuchsia::test]
    fn handle_is_dropped_after_last_weak_ref_released() {
        let driver = BinderDriver::new();
        let proc_1 = driver.create_process(1);
        let proc_2 = driver.create_process(2);

        let transaction_ref = Arc::new(BinderObject {
            owner: Arc::downgrade(&proc_1),
            local: LocalBinderObject {
                weak_ref_addr: UserAddress::from(0xffffffffffffffff),
                strong_ref_addr: UserAddress::from(0x1111111111111111),
            },
        });

        // The handle starts with a strong ref.
        let handle = proc_2.handles.lock().insert_for_transaction(transaction_ref.clone());

        // Acquire a weak reference.
        proc_2.handles.lock().inc_weak(handle.object_index()).expect("inc_weak");

        // The object should be present in the handle table.
        assert!(Arc::ptr_eq(
            &proc_2.handles.lock().get(handle.object_index()).expect("valid object"),
            &transaction_ref
        ));

        // Drop the strong reference. The handle should still be present as there is an outstanding
        // weak reference.
        proc_2.handles.lock().dec_strong(handle.object_index()).expect("dec_strong");
        assert!(Arc::ptr_eq(
            &proc_2.handles.lock().get(handle.object_index()).expect("valid object"),
            &transaction_ref
        ));

        // Drop the weak reference. The handle should now be gone, even though the underlying object
        // is still alive (another process could have references to it).
        proc_2.handles.lock().dec_weak(handle.object_index()).expect("dec_weak");
        proc_2.handles.lock().get(handle.object_index()).expect_err("handle should be dropped");
    }

    #[fuchsia::test]
    fn shared_memory_allocation_fails_with_invalid_offsets_length() {
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        let mut shared_memory =
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory");
        shared_memory.allocate_buffer(3, 1).expect_err("offsets_length should be multiple of 8");
    }

    #[fuchsia::test]
    fn shared_memory_allocation_aligns_offsets_buffer() {
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        let mut shared_memory =
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory");

        const DATA_LEN: usize = 3;
        const OFFSETS_COUNT: usize = 1;
        const OFFSETS_LEN: usize = std::mem::size_of::<binder_uintptr_t>() * OFFSETS_COUNT;
        let mut buf =
            shared_memory.allocate_buffer(DATA_LEN, OFFSETS_LEN).expect("allocate buffer");
        assert_eq!(buf.data_user_buffer(), UserBuffer { address: BASE_ADDR, length: DATA_LEN });
        assert_eq!(
            buf.offsets_user_buffer(),
            UserBuffer { address: BASE_ADDR + 8usize, length: OFFSETS_LEN }
        );
        let (data_buf, offsets_buf) = buf.as_mut_bytes();
        assert_eq!(data_buf.len(), DATA_LEN);
        assert_eq!(offsets_buf.len(), OFFSETS_COUNT);
    }

    #[fuchsia::test]
    fn shared_memory_allocation_buffers_correctly_write_through() {
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        let mut shared_memory =
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory");

        const DATA_LEN: usize = 256;
        const OFFSETS_COUNT: usize = 4;
        const OFFSETS_LEN: usize = std::mem::size_of::<binder_uintptr_t>() * OFFSETS_COUNT;
        let mut buf =
            shared_memory.allocate_buffer(DATA_LEN, OFFSETS_LEN).expect("allocate buffer");
        let (data_buf, offsets_buf) = buf.as_mut_bytes();

        // Write data to the allocated buffers.
        const DATA_FILL: u8 = 0xff;
        data_buf.fill(0xff);

        const OFFSETS_FILL: binder_uintptr_t = 0xDEADBEEFDEADBEEF;
        offsets_buf.fill(OFFSETS_FILL);

        // Check that the correct bit patterns were written through to the underlying VMO.
        let mut data = [0u8; DATA_LEN];
        vmo.read(&mut data, 0).expect("read VMO failed");
        assert!(data.iter().all(|b| *b == DATA_FILL));

        let mut data = [0u64; OFFSETS_COUNT];
        vmo.read(data.as_bytes_mut(), DATA_LEN as u64).expect("read VMO failed");
        assert!(data.iter().all(|b| *b == OFFSETS_FILL));
    }

    #[fuchsia::test]
    fn shared_memory_allocates_multiple_buffers() {
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        let mut shared_memory =
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory");

        // Check that two buffers allocated from the same shared memory region don't overlap.
        const BUF1_DATA_LEN: usize = 64;
        const BUF1_OFFSETS_LEN: usize = 8;
        let buf = shared_memory
            .allocate_buffer(BUF1_DATA_LEN, BUF1_OFFSETS_LEN)
            .expect("allocate buffer 1");
        assert_eq!(
            buf.data_user_buffer(),
            UserBuffer { address: BASE_ADDR, length: BUF1_DATA_LEN }
        );
        assert_eq!(
            buf.offsets_user_buffer(),
            UserBuffer { address: BASE_ADDR + BUF1_DATA_LEN, length: BUF1_OFFSETS_LEN }
        );

        const BUF2_DATA_LEN: usize = 32;
        const BUF2_OFFSETS_LEN: usize = 0;
        let buf = shared_memory
            .allocate_buffer(BUF2_DATA_LEN, BUF2_OFFSETS_LEN)
            .expect("allocate buffer 2");
        assert_eq!(
            buf.data_user_buffer(),
            UserBuffer {
                address: BASE_ADDR + BUF1_DATA_LEN + BUF1_OFFSETS_LEN,
                length: BUF2_DATA_LEN
            }
        );
        assert_eq!(
            buf.offsets_user_buffer(),
            UserBuffer {
                address: BASE_ADDR + BUF1_DATA_LEN + BUF1_OFFSETS_LEN + BUF2_DATA_LEN,
                length: BUF2_OFFSETS_LEN
            }
        );
    }

    #[fuchsia::test]
    fn shared_memory_too_large_allocation_fails() {
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        let mut shared_memory =
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory");

        shared_memory.allocate_buffer(VMO_LENGTH + 1, 0).expect_err("out-of-bounds allocation");
        shared_memory.allocate_buffer(VMO_LENGTH, 1).expect_err("out-of-bounds allocation");

        let _ = shared_memory.allocate_buffer(VMO_LENGTH, 0).expect("allocate buffer");

        shared_memory.allocate_buffer(1, 0).expect_err("out-of-bounds allocation");
    }

    #[fuchsia::test]
    fn binder_object_enqueues_release_command_when_dropped() {
        let driver = BinderDriver::new();
        let proc = driver.create_process(1);

        let object = Arc::new(BinderObject {
            owner: Arc::downgrade(&proc),
            local: LocalBinderObject {
                weak_ref_addr: UserAddress::from(0x0000000000000010),
                strong_ref_addr: UserAddress::from(0x0000000000000100),
            },
        });

        drop(object);

        assert_eq!(
            proc.command_queue.lock().front(),
            Some(&Command::ReleaseRef(LocalBinderObject {
                weak_ref_addr: UserAddress::from(0x0000000000000010),
                strong_ref_addr: UserAddress::from(0x0000000000000100),
            }))
        );
    }

    #[fuchsia::test]
    fn handle_table_refs() {
        let driver = BinderDriver::new();
        let proc = driver.create_process(1);

        let object = Arc::new(BinderObject {
            owner: Arc::downgrade(&proc),
            local: LocalBinderObject {
                weak_ref_addr: UserAddress::from(0x0000000000000010),
                strong_ref_addr: UserAddress::from(0x0000000000000100),
            },
        });

        let mut handle_table = HandleTable::default();

        // Starts with one strong reference.
        let handle = handle_table.insert_for_transaction(object.clone());

        handle_table.inc_strong(handle.object_index()).expect("inc_strong 1");
        handle_table.inc_strong(handle.object_index()).expect("inc_strong 2");
        handle_table.inc_weak(handle.object_index()).expect("inc_weak 0");
        handle_table.dec_strong(handle.object_index()).expect("dec_strong 2");
        handle_table.dec_strong(handle.object_index()).expect("dec_strong 1");

        // Remove the initial strong reference.
        handle_table.dec_strong(handle.object_index()).expect("dec_strong 0");

        // Removing more strong references should fail.
        handle_table.dec_strong(handle.object_index()).expect_err("dec_strong -1");

        // The object should still take up an entry in the handle table, as there is 1 weak
        // reference.
        handle_table.get(handle.object_index()).expect("object still exists");

        drop(object);

        // Our weak reference won't keep the object alive.
        handle_table.get(handle.object_index()).expect_err("object should be dead");

        // Remove from our table.
        handle_table.dec_weak(handle.object_index()).expect("dec_weak 0");

        // Another removal attempt will prove the handle has been removed.
        handle_table.dec_weak(handle.object_index()).expect_err("handle should no longer exist");
    }

    #[fuchsia::test]
    fn copy_transaction_data_between_processes() {
        let (_kernel, task1) = create_kernel_and_task();
        let driver = BinderDriver::new();

        // Register a binder process that represents `task1`. This is the source process: data will
        // be copied out of process ID 1 into process ID 2's shared memory.
        let (proc1, thread1) = driver.create_process_and_thread(1);

        // Initialize process 2 with shared memory in the driver.
        let proc2 = driver.create_process(2);
        let vmo = zx::Vmo::create(VMO_LENGTH as u64).expect("failed to create VMO");
        *proc2.shared_memory.lock() = Some(
            SharedMemory::map(&vmo, BASE_ADDR, VMO_LENGTH).expect("failed to map shared memory"),
        );

        // Map some memory for process 1.
        let data_addr = map_memory(&task1, UserAddress::default(), *PAGE_SIZE);

        // Write transaction data in process 1.
        const BINDER_DATA: &[u8; 8] = b"binder!!";
        let mut transaction_data = Vec::new();
        transaction_data.extend(BINDER_DATA);
        transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr: binder_object_header { type_: BINDER_TYPE_HANDLE },
            flags: 0,
            __bindgen_anon_1.handle: 0,
            cookie: 0,
        }));

        let offsets_addr = data_addr
            + task1
                .mm
                .write_memory(data_addr, &transaction_data)
                .expect("failed to write transaction data");

        // Write the offsets data (where in the data buffer `flat_binder_object`s are).
        let offsets_data: u64 = BINDER_DATA.len() as u64;
        task1
            .mm
            .write_object(UserRef::new(offsets_addr), &offsets_data)
            .expect("failed to write offsets buffer");

        // Construct the `binder_transaction_data` struct that contains pointers to the data and
        // offsets buffers.
        let transaction = binder_transaction_data {
            code: 1,
            flags: 0,
            sender_pid: 1,
            sender_euid: 0,
            target: binder_transaction_data__bindgen_ty_1 { handle: 0 },
            cookie: 0,
            data_size: transaction_data.len() as u64,
            offsets_size: std::mem::size_of::<u64>() as u64,
            data: binder_transaction_data__bindgen_ty_2 {
                ptr: binder_transaction_data__bindgen_ty_2__bindgen_ty_1 {
                    buffer: data_addr.ptr() as u64,
                    offsets: offsets_addr.ptr() as u64,
                },
            },
        };

        // Copy the data from process 1 to process 2
        let (data_buffer, offsets_buffer) = driver
            .copy_transaction_buffers(&task1, &proc1, &thread1, &proc2, &transaction)
            .expect("copy data");

        // Check that the returned buffers are in-bounds of process 2's shared memory.
        assert!(data_buffer.address >= BASE_ADDR);
        assert!(data_buffer.address < BASE_ADDR + VMO_LENGTH);
        assert!(offsets_buffer.address >= BASE_ADDR);
        assert!(offsets_buffer.address < BASE_ADDR + VMO_LENGTH);

        // Verify the contents of the copied data in process 2's shared memory VMO.
        let mut buffer = [0u8; BINDER_DATA.len() + std::mem::size_of::<flat_binder_object>()];
        vmo.read(&mut buffer, (data_buffer.address - BASE_ADDR) as u64)
            .expect("failed to read data");
        assert_eq!(&buffer[..], &transaction_data);

        let mut buffer = [0u8; std::mem::size_of::<u64>()];
        vmo.read(&mut buffer, (offsets_buffer.address - BASE_ADDR) as u64)
            .expect("failed to read offsets");
        assert_eq!(&buffer[..], offsets_data.as_bytes());
    }

    #[fuchsia::test]
    fn transaction_translate_binder_leaving_process() {
        let driver = BinderDriver::new();
        let (sender_proc, sender_thread) = driver.create_process_and_thread(1);
        let receiver = driver.create_process(2);

        let binder_object = LocalBinderObject {
            weak_ref_addr: UserAddress::from(0x0000000000000010),
            strong_ref_addr: UserAddress::from(0x0000000000000100),
        };

        const DATA_PREAMBLE: &[u8; 5] = b"stuff";

        let mut transaction_data = Vec::new();
        transaction_data.extend(DATA_PREAMBLE);
        let offsets = [transaction_data.len() as binder_uintptr_t];
        transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_BINDER,
            flags: 0,
            cookie: binder_object.strong_ref_addr.ptr() as u64,
            __bindgen_anon_1.binder: binder_object.weak_ref_addr.ptr() as u64,
        }));

        const EXPECTED_HANDLE: Handle = Handle::from_raw(1);

        let transaction_refs = driver
            .translate_handles(
                &sender_proc,
                &sender_thread,
                &receiver,
                &offsets,
                &mut transaction_data,
            )
            .expect("failed to translate handles");

        // Verify that the new handle was returned in `transaction_refs` so that it gets dropped
        // at the end of the transaction.
        assert_eq!(transaction_refs.handles[0], EXPECTED_HANDLE);

        // Verify that the transaction data was mutated.
        let mut expected_transaction_data = Vec::new();
        expected_transaction_data.extend(DATA_PREAMBLE);
        expected_transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_HANDLE,
            flags: 0,
            cookie: 0,
            __bindgen_anon_1.handle: EXPECTED_HANDLE.into(),
        }));
        assert_eq!(&expected_transaction_data, &transaction_data);

        // Verify that a handle was created in the receiver.
        let object = receiver
            .handles
            .lock()
            .get(EXPECTED_HANDLE.object_index())
            .expect("expected handle not present");
        assert!(std::ptr::eq(object.owner.as_ptr(), Arc::as_ptr(&sender_proc)));
        assert_eq!(object.local, binder_object);

        // Verify that a strong acquire command is sent to the sender process (on the same thread
        // that sent the transaction).
        assert_eq!(
            sender_thread.read().command_queue.front(),
            Some(&Command::AcquireRef(binder_object))
        );
    }

    #[fuchsia::test]
    fn transaction_translate_binder_handle_entering_owning_process() {
        let driver = BinderDriver::new();
        let (sender_proc, sender_thread) = driver.create_process_and_thread(1);
        let receiver = driver.create_process(2);

        let binder_object = LocalBinderObject {
            weak_ref_addr: UserAddress::from(0x0000000000000010),
            strong_ref_addr: UserAddress::from(0x0000000000000100),
        };

        // Pretend the binder object was given to the sender earlier, so it can be sent back.
        let handle = sender_proc.handles.lock().insert_for_transaction(Arc::new(BinderObject {
            owner: Arc::downgrade(&receiver),
            local: binder_object.clone(),
        }));

        const DATA_PREAMBLE: &[u8; 5] = b"stuff";

        let mut transaction_data = Vec::new();
        transaction_data.extend(DATA_PREAMBLE);
        let offsets = [transaction_data.len() as binder_uintptr_t];
        transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_HANDLE,
            flags: 0,
            cookie: 0,
            __bindgen_anon_1.handle: handle.into(),
        }));

        driver
            .translate_handles(
                &sender_proc,
                &sender_thread,
                &receiver,
                &offsets,
                &mut transaction_data,
            )
            .expect("failed to translate handles");

        // Verify that the transaction data was mutated.
        let mut expected_transaction_data = Vec::new();
        expected_transaction_data.extend(DATA_PREAMBLE);
        expected_transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_BINDER,
            flags: 0,
            cookie: binder_object.strong_ref_addr.ptr() as u64,
            __bindgen_anon_1.binder: binder_object.weak_ref_addr.ptr() as u64,
        }));
        assert_eq!(&expected_transaction_data, &transaction_data);
    }

    #[fuchsia::test]
    fn transaction_translate_binder_handle_passed_between_non_owning_processes() {
        let driver = BinderDriver::new();
        let (sender_proc, sender_thread) = driver.create_process_and_thread(1);
        let receiver = driver.create_process(2);
        let owner = driver.create_process(3);

        let binder_object = LocalBinderObject {
            weak_ref_addr: UserAddress::from(0x0000000000000010),
            strong_ref_addr: UserAddress::from(0x0000000000000100),
        };

        const SENDING_HANDLE: Handle = Handle::from_raw(1);
        const RECEIVING_HANDLE: Handle = Handle::from_raw(2);

        // Pretend the binder object was given to the sender earlier.
        let handle = sender_proc.handles.lock().insert_for_transaction(Arc::new(BinderObject {
            owner: Arc::downgrade(&owner),
            local: binder_object.clone(),
        }));
        assert_eq!(SENDING_HANDLE, handle);

        // Give the receiver another handle so that the input handle number and output handle
        // number aren't the same.
        receiver.handles.lock().insert_for_transaction(Arc::new(BinderObject {
            owner: Arc::downgrade(&owner),
            local: LocalBinderObject {
                strong_ref_addr: UserAddress::default(),
                weak_ref_addr: UserAddress::default(),
            },
        }));

        const DATA_PREAMBLE: &[u8; 5] = b"stuff";

        let mut transaction_data = Vec::new();
        transaction_data.extend(DATA_PREAMBLE);
        let offsets = [transaction_data.len() as binder_uintptr_t];
        transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_HANDLE,
            flags: 0,
            cookie: 0,
            __bindgen_anon_1.handle: SENDING_HANDLE.into(),
        }));

        let transaction_refs = driver
            .translate_handles(
                &sender_proc,
                &sender_thread,
                &receiver,
                &offsets,
                &mut transaction_data,
            )
            .expect("failed to translate handles");

        // Verify that the new handle was returned in `transaction_refs` so that it gets dropped
        // at the end of the transaction.
        assert_eq!(transaction_refs.handles[0], RECEIVING_HANDLE);

        // Verify that the transaction data was mutated.
        let mut expected_transaction_data = Vec::new();
        expected_transaction_data.extend(DATA_PREAMBLE);
        expected_transaction_data.extend(struct_with_union_into_bytes!(flat_binder_object {
            hdr.type_: BINDER_TYPE_HANDLE,
            flags: 0,
            cookie: 0,
            __bindgen_anon_1.handle: RECEIVING_HANDLE.into(),
        }));
        assert_eq!(&expected_transaction_data, &transaction_data);

        // Verify that a handle was created in the receiver.
        let object = receiver
            .handles
            .lock()
            .get(RECEIVING_HANDLE.object_index())
            .expect("expected handle not present");
        assert!(std::ptr::eq(object.owner.as_ptr(), Arc::as_ptr(&owner)));
        assert_eq!(object.local, binder_object);
    }

    #[fuchsia::test]
    fn process_state_cleaned_up_after_binder_fd_closed() {
        let (_kernel, current_task) = create_kernel_and_task();
        let binder_dev = BinderDev::new();
        let node = FsNode::new_root(PlaceholderFsNodeOps);

        // Open the binder device, which creates an instance of the binder device associated with
        // the process.
        let binder_instance = binder_dev
            .open(&current_task, DeviceType::NONE, &node, OpenFlags::RDWR)
            .expect("binder dev open failed");

        // Ensure that the binder driver has created process state.
        binder_dev.driver.find_process(current_task.get_pid()).expect("failed to find process");

        // Simulate closing the FD by dropping the binder instance.
        drop(binder_instance);

        // Verify that the process state no longer exists.
        binder_dev
            .driver
            .find_process(current_task.get_pid())
            .expect_err("process was not cleaned up");
    }
}
