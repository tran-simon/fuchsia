// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::*;
use crate::logging::not_implemented;
use crate::task::*;
use crate::types::*;
use zerocopy::AsBytes;

/// The version of selinux_status_t this kernel implements.
const SELINUX_STATUS_VERSION: u32 = 1;

struct SeLinuxFs;
impl FileSystemOps for SeLinuxFs {
    fn statfs(&self, _fs: &FileSystem) -> Result<statfs, Errno> {
        Ok(statfs { f_type: SELINUX_MAGIC as i64, ..Default::default() })
    }
}

impl SeLinuxFs {
    fn new() -> Result<FileSystemHandle, Errno> {
        let fs = FileSystem::new_with_permanent_entries(SeLinuxFs);
        fs.set_root(ROMemoryDirectory);
        let root = fs.root();
        root.add_node_ops(b"load", mode!(IFREG, 0600), SimpleFileNode::new(|| Ok(SeLoad)))?;
        root.add_node_ops(b"enforce", mode!(IFREG, 0644), SimpleFileNode::new(|| Ok(SeEnforce)))?;
        root.add_node_ops(
            b"checkreqprot",
            mode!(IFREG, 0644),
            SimpleFileNode::new(|| Ok(SeCheckReqProt)),
        )?;
        root.add_node_ops(
            b"deny_unknown",
            mode!(IFREG, 0444),
            // Allow all unknown object classes/permissions.
            ByteVecFile::new(b"0:0\n".to_vec()),
        )?;

        // The status file needs to be mmap-able, so use a VMO-backed file.
        // When the selinux state changes in the future, the way to update this data (and
        // communicate updates with userspace) is to use the
        // ["seqlock"](https://en.wikipedia.org/wiki/Seqlock) technique.
        root.add_node_ops(
            b"status",
            mode!(IFREG, 0444),
            VmoFileNode::from_bytes(
                selinux_status_t { version: SELINUX_STATUS_VERSION, ..Default::default() }
                    .as_bytes(),
            )?,
        )?;

        Ok(fs)
    }
}

/// The C-style struct exposed to userspace by the /sys/fs/selinux/status file.
/// Defined here (instead of imported through bindgen) as selinux headers are not exposed through
/// kernel uapi headers.
#[derive(Debug, Copy, Clone, AsBytes, Default)]
#[repr(C, packed)]
struct selinux_status_t {
    /// Version number of this structure (1).
    version: u32,
    /// Sequence number. See [seqlock](https://en.wikipedia.org/wiki/Seqlock).
    sequence: u32,
    /// `0` means permissive mode, `1` means enforcing mode.
    enforcing: u32,
    /// The number of times the selinux policy has been reloaded.
    policyload: u32,
    /// `0` means allow and `1` means deny unknown object classes/permissions.
    deny_unknown: u32,
}

struct SeLoad;
impl FileOps for SeLoad {
    fileops_impl_seekable!();
    fileops_impl_nonblocking!();

    fn write_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        if offset != 0 {
            return error!(EINVAL);
        }
        let size = UserBuffer::get_total_length(data)?;
        let mut buf = vec![0u8; size];
        current_task.mm.read_all(&data, &mut buf)?;
        not_implemented!("got selinux policy, length {}, ignoring", size);
        Ok(size)
    }

    fn read_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

struct SeEnforce;
impl FileOps for SeEnforce {
    fileops_impl_seekable!();
    fileops_impl_nonblocking!();

    fn write_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        if offset != 0 {
            return error!(EINVAL);
        }
        let size = UserBuffer::get_total_length(data)?;
        let mut buf = vec![0u8; size];
        current_task.mm.read_all(&data, &mut buf)?;
        let enforce = parse_int(&buf)?;
        not_implemented!("selinux setenforce: {}", enforce);
        Ok(size)
    }
    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        _offset: usize,
        data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        // Don't pretend that selinux is enforced until it is implemented.
        current_task.mm.write_all(data, b"0\n")
    }
}

struct SeCheckReqProt;
impl FileOps for SeCheckReqProt {
    fileops_impl_seekable!();
    fileops_impl_nonblocking!();

    fn write_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        if offset != 0 {
            return error!(EINVAL);
        }
        let size = UserBuffer::get_total_length(data)?;
        let mut buf = vec![0u8; size];
        current_task.mm.read_all(&data, &mut buf)?;
        let checkreqprot = parse_int(&buf)?;
        not_implemented!("selinux checkreqprot: {}", checkreqprot);
        Ok(size)
    }
    fn read_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

fn parse_int(buf: &[u8]) -> Result<u32, Errno> {
    let i = buf.iter().position(|c| !char::from(*c).is_digit(10)).unwrap_or(buf.len());
    std::str::from_utf8(&buf[..i]).unwrap().parse::<u32>().map_err(|_| errno!(EINVAL))
}

pub fn selinux_fs(kern: &Kernel) -> &FileSystemHandle {
    kern.selinux_fs.get_or_init(|| SeLinuxFs::new().expect("failed to construct selinuxfs"))
}
