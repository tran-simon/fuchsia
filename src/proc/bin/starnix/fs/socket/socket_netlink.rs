// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use zerocopy::{AsBytes, FromBytes};

use crate::fs::buffers::*;
use crate::fs::*;
use crate::lock::Mutex;
use crate::logging::not_implemented;
use crate::mm::MemoryAccessor;
use crate::mm::MemoryAccessorExt;
use crate::task::*;
use crate::types::*;

// From netlink/socket.go in gVisor.
pub const SOCKET_MIN_SIZE: usize = 4 << 10;
pub const SOCKET_DEFAULT_SIZE: usize = 16 * 1024;
pub const SOCKET_MAX_SIZE: usize = 4 << 20;

pub struct NetlinkSocket {
    family: NetlinkFamily,
    inner: Mutex<NetlinkSocketInner>,
}

#[derive(Default, Debug, Clone, PartialEq, Eq)]
#[repr(C)]
pub struct NetlinkAddress {
    pid: u32,
    groups: u32,
}

impl NetlinkAddress {
    pub fn new(pid: u32, groups: u32) -> Self {
        NetlinkAddress { pid, groups }
    }

    pub fn set_pid_if_zero(&mut self, pid: i32) {
        if self.pid == 0 {
            self.pid = pid as u32;
        }
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        sockaddr_nl { nl_family: AF_NETLINK, nl_pid: self.pid, nl_pad: 0, nl_groups: self.groups }
            .as_bytes()
            .to_vec()
    }
}

#[derive(Debug, Hash, Eq, PartialEq, Clone)]
pub enum NetlinkFamily {
    Unsupported,
    Route,
    Usersock,
    Firewall,
    SockDiag,
    Nflog,
    Xfrm,
    Selinux,
    Iscsi,
    Audit,
    FibLookup,
    Connector,
    Netfilter,
    Ip6Fw,
    Dnrtmsg,
    KobjectUevent,
    Generic,
    Scsitransport,
    Ecryptfs,
    Rdma,
    Crypto,
    Smc,
}

impl NetlinkFamily {
    pub fn from_raw(family: u32) -> Self {
        match family {
            NETLINK_ROUTE => NetlinkFamily::Route,
            NETLINK_USERSOCK => NetlinkFamily::Usersock,
            NETLINK_FIREWALL => NetlinkFamily::Firewall,
            NETLINK_SOCK_DIAG => NetlinkFamily::SockDiag,
            NETLINK_NFLOG => NetlinkFamily::Nflog,
            NETLINK_XFRM => NetlinkFamily::Xfrm,
            NETLINK_SELINUX => NetlinkFamily::Selinux,
            NETLINK_ISCSI => NetlinkFamily::Iscsi,
            NETLINK_AUDIT => NetlinkFamily::Audit,
            NETLINK_FIB_LOOKUP => NetlinkFamily::FibLookup,
            NETLINK_CONNECTOR => NetlinkFamily::Connector,
            NETLINK_NETFILTER => NetlinkFamily::Netfilter,
            NETLINK_IP6_FW => NetlinkFamily::Ip6Fw,
            NETLINK_DNRTMSG => NetlinkFamily::Dnrtmsg,
            NETLINK_KOBJECT_UEVENT => NetlinkFamily::KobjectUevent,
            NETLINK_GENERIC => NetlinkFamily::Generic,
            NETLINK_SCSITRANSPORT => NetlinkFamily::Scsitransport,
            NETLINK_ECRYPTFS => NetlinkFamily::Ecryptfs,
            NETLINK_RDMA => NetlinkFamily::Rdma,
            NETLINK_CRYPTO => NetlinkFamily::Crypto,
            NETLINK_SMC => NetlinkFamily::Smc,
            _ => NetlinkFamily::Unsupported,
        }
    }

    pub fn as_raw(&self) -> u32 {
        match self {
            NetlinkFamily::Route => NETLINK_ROUTE,
            NetlinkFamily::KobjectUevent => NETLINK_KOBJECT_UEVENT,
            _ => 0,
        }
    }
}

#[allow(dead_code)]
pub struct NetlinkSocketInner {
    /// The `MessageQueue` that contains messages sent to this socket.
    messages: MessageQueue,

    /// This queue will be notified on reads, writes, disconnects etc.
    waiters: WaitQueue,

    /// The address of this socket.
    address: Option<NetlinkAddress>,

    /// See SO_PASSCRED.
    pub passcred: bool,

    /// See SO_TIMESTAMP.
    pub timestamp: bool,
}

impl NetlinkSocketInner {
    pub fn bind(&mut self, netlink_address: NetlinkAddress) -> Result<(), Errno> {
        if self.address.is_some() {
            return error!(EINVAL);
        }

        self.address = Some(netlink_address);
        Ok(())
    }

    fn set_capacity(&mut self, requested_capacity: usize) {
        let capacity = requested_capacity.clamp(SOCKET_MIN_SIZE, SOCKET_MAX_SIZE);
        let capacity = std::cmp::max(capacity, self.messages.len());
        // We have validated capacity sufficiently that set_capacity should always succeed.
        self.messages.set_capacity(capacity).unwrap();
    }

    fn read(
        &mut self,
        current_task: &CurrentTask,
        user_buffers: &mut UserBufferIterator<'_>,
        _flags: SocketMessageFlags,
    ) -> Result<MessageReadInfo, Errno> {
        let msg = self.messages.read_message();
        match msg {
            Some(message) => {
                // Mark the message as complete and return it.
                let mut nl_msg = nlmsghdr::read_from_prefix(message.data.bytes())
                    .ok_or_else(|| errno!(EINVAL))?;
                nl_msg.nlmsg_type = NLMSG_DONE as u16;
                nl_msg.nlmsg_flags &= NLM_F_MULTI as u16;
                let msg_bytes = nl_msg.as_bytes();
                let mut bytes_read = 0;
                while let Some(user_buffer) = user_buffers.next(msg_bytes.len() - bytes_read) {
                    let bytes_chunk = &msg_bytes[bytes_read..(bytes_read + user_buffer.length)];
                    current_task.mm.write_memory(user_buffer.address, bytes_chunk)?;
                    bytes_read += user_buffer.length;
                }

                let info = MessageReadInfo {
                    bytes_read,
                    message_length: msg_bytes.len(),
                    address: Some(SocketAddress::Netlink(NetlinkAddress::default())),
                    ancillary_data: vec![],
                };
                Ok(info)
            }
            None => Ok(MessageReadInfo::default()),
        }
    }

    fn write(
        &mut self,
        current_task: &CurrentTask,
        user_buffers: &mut UserBufferIterator<'_>,
        address: Option<NetlinkAddress>,
        ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        let socket_address = match address {
            Some(addr) => Some(SocketAddress::Netlink(addr)),
            None => self.address.as_ref().map(|addr| SocketAddress::Netlink(addr.clone())),
        };
        let bytes_written = self.messages.write_datagram(
            current_task,
            user_buffers,
            socket_address,
            ancillary_data,
        )?;
        if bytes_written > 0 {
            self.waiters.notify_events(FdEvents::POLLIN);
        }
        Ok(bytes_written)
    }
}

impl NetlinkSocket {
    pub fn new(socket_type: SocketType, family: NetlinkFamily) -> Result<NetlinkSocket, Errno> {
        if socket_type != SocketType::Datagram && socket_type != SocketType::Raw {
            return error!(ESOCKTNOSUPPORT);
        }

        Ok(NetlinkSocket {
            family,
            inner: Mutex::new(NetlinkSocketInner {
                messages: MessageQueue::new(SOCKET_DEFAULT_SIZE),
                waiters: WaitQueue::default(),
                address: None,
                passcred: false,
                timestamp: false,
            }),
        })
    }

    /// Locks and returns the inner state of the Socket.
    fn lock(&self) -> crate::lock::MutexGuard<'_, NetlinkSocketInner> {
        self.inner.lock()
    }

    pub fn bind_if_not_bound(&self, pid: i32) {
        // Ignore error if this socket is already bound.
        let _ = self.lock().bind(NetlinkAddress { pid: pid as u32, groups: 0 });
    }
}

impl SocketOps for NetlinkSocket {
    fn connect(
        &self,
        _socket: &SocketHandle,
        _peer: SocketPeer,
        _credentials: ucred,
    ) -> Result<(), Errno> {
        not_implemented!("?", "NetlinkSocket::connect is stubbed");
        Ok(())
    }

    fn listen(&self, _socket: &Socket, _backlog: i32, _credentials: ucred) -> Result<(), Errno> {
        error!(EOPNOTSUPP)
    }

    fn accept(&self, _socket: &Socket) -> Result<SocketHandle, Errno> {
        error!(EOPNOTSUPP)
    }

    fn remote_connection(&self, _socket: &Socket, _file: FileHandle) -> Result<(), Errno> {
        not_implemented!("?", "NetlinkSocket::remote_connection is stubbed");
        Ok(())
    }

    fn bind(&self, _socket: &Socket, socket_address: SocketAddress) -> Result<(), Errno> {
        match socket_address {
            SocketAddress::Netlink(netlink_address) => self.lock().bind(netlink_address),
            _ => error!(EINVAL),
        }
    }

    fn read(
        &self,
        _socket: &Socket,
        current_task: &CurrentTask,
        user_buffers: &mut UserBufferIterator<'_>,
        flags: SocketMessageFlags,
    ) -> Result<MessageReadInfo, Errno> {
        self.lock().read(current_task, user_buffers, flags)
    }

    fn write(
        &self,
        _socket: &Socket,
        current_task: &CurrentTask,
        user_buffers: &mut UserBufferIterator<'_>,
        dest_address: &mut Option<SocketAddress>,
        ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        let inner = self.lock();
        let mut local_address = inner.address.clone();
        drop(inner);

        let destination = match dest_address {
            Some(SocketAddress::Netlink(addr)) => addr,
            _ => match &mut local_address {
                Some(addr) => addr,
                _ => {
                    let bytes = user_buffers.drain_to_vec();
                    return Ok(bytes.len());
                }
            },
        };

        if destination.groups != 0 {
            not_implemented!("?", "NetlinkSockets multicasting is stubbed");
            let bytes = user_buffers.drain_to_vec();
            return Ok(bytes.len());
        }

        self.lock().write(
            current_task,
            user_buffers,
            Some(NetlinkAddress::default()),
            ancillary_data,
        )
    }

    fn wait_async(
        &self,
        socket: &Socket,
        current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
        options: WaitAsyncOptions,
    ) -> WaitKey {
        let present_events = self.query_events(socket, current_task);
        if events & present_events && !options.contains(WaitAsyncOptions::EDGE_TRIGGERED) {
            waiter.wake_immediately(present_events.mask(), handler)
        } else {
            self.lock().waiters.wait_async_mask(waiter, events.mask(), handler)
        }
    }

    fn cancel_wait(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        _waiter: &Waiter,
        _key: WaitKey,
    ) {
    }

    fn query_events(&self, _socket: &Socket, _current_task: &CurrentTask) -> FdEvents {
        let mut present_events = FdEvents::empty();
        let local_events = self.lock().messages.query_events();
        if local_events & FdEvents::POLLIN {
            present_events = FdEvents::POLLIN;
        }

        present_events
    }

    fn shutdown(&self, _socket: &Socket, _how: SocketShutdownFlags) -> Result<(), Errno> {
        not_implemented!("?", "NetlinkSocket::shutdown is stubbed");
        Ok(())
    }

    fn close(&self, _socket: &Socket) {
        not_implemented!("?", "NetlinkSocket::close is stubbed");
    }

    fn getsockname(&self, _socket: &Socket) -> Vec<u8> {
        match &self.lock().address {
            Some(addr) => addr.to_bytes(),
            _ => vec![],
        }
    }

    fn getpeername(&self, _socket: &Socket) -> Result<Vec<u8>, Errno> {
        match &self.lock().address {
            Some(addr) => Ok(addr.to_bytes()),
            None => {
                let addr = sockaddr_nl { nl_family: AF_NETLINK, ..Default::default() };
                Ok(addr.as_bytes().to_vec())
            }
        }
    }

    fn getsockopt(
        &self,
        _socket: &Socket,
        level: u32,
        optname: u32,
        _optlen: u32,
    ) -> Result<Vec<u8>, Errno> {
        let opt_value = match level {
            SOL_SOCKET => match optname {
                SO_PASSCRED => (self.lock().passcred as u32).as_bytes().to_vec(),
                SO_TIMESTAMP => (self.lock().timestamp as u32).as_bytes().to_vec(),
                SO_SNDBUF => (self.lock().messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_RCVBUF => (self.lock().messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_SNDBUFFORCE => {
                    (self.lock().messages.capacity() as socklen_t).to_ne_bytes().to_vec()
                }
                SO_RCVBUFFORCE => {
                    (self.lock().messages.capacity() as socklen_t).to_ne_bytes().to_vec()
                }
                SO_PROTOCOL => self.family.as_raw().as_bytes().to_vec(),
                _ => return error!(ENOSYS),
            },
            _ => vec![],
        };

        Ok(opt_value)
    }

    fn setsockopt(
        &self,
        _socket: &Socket,
        task: &Task,
        level: u32,
        optname: u32,
        user_opt: UserBuffer,
    ) -> Result<(), Errno> {
        fn read<T: Default + AsBytes + FromBytes>(
            task: &Task,
            user_opt: UserBuffer,
        ) -> Result<T, Errno> {
            let user_ref = UserRef::<T>::from_buf(user_opt).ok_or_else(|| errno!(EINVAL))?;
            task.mm.read_object(user_ref)
        }

        match level {
            SOL_SOCKET => match optname {
                SO_SNDBUF => {
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.lock().set_capacity(requested_capacity * 2);
                }
                SO_RCVBUF => {
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.lock().set_capacity(requested_capacity);
                }
                SO_PASSCRED => {
                    let passcred = read::<u32>(task, user_opt)?;
                    self.lock().passcred = passcred != 0;
                }
                SO_TIMESTAMP => {
                    let timestamp = read::<u32>(task, user_opt)?;
                    self.lock().timestamp = timestamp != 0;
                }
                SO_SNDBUFFORCE => {
                    if !task.creds().has_capability(CAP_NET_ADMIN) {
                        return error!(EPERM);
                    }
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.lock().set_capacity(requested_capacity * 2);
                }
                SO_RCVBUFFORCE => {
                    if !task.creds().has_capability(CAP_NET_ADMIN) {
                        return error!(EPERM);
                    }
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.lock().set_capacity(requested_capacity);
                }
                _ => return error!(ENOSYS),
            },
            _ => return error!(ENOSYS),
        }

        Ok(())
    }
}
