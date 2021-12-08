// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_MOUNT_H_
#define SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_MOUNT_H_

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fidl/llcpp/client_end.h>
#include <zircon/compiler.h>

#include <fbl/unique_fd.h>
#include <fs-management/admin.h>
#include <fs-management/launch.h>

namespace fs_management {

struct MountOptions {
  bool readonly = false;
  bool verbose_mount = false;
  bool collect_metrics = false;

  // Ensures that requests to the mountpoint will be propagated to the underlying FS
  bool wait_until_ready = true;

  // Create the mountpoint directory if it doesn't already exist.
  // Must be false if passed to "fmount".
  bool create_mountpoint = false;

  // An optional compression algorithm specifier for the filesystem to use when storing files (if
  // the filesystem supports it).
  const char* write_compression_algorithm = nullptr;

  // An optional cache eviction policy specifier for the filesystem to use for in-memory data (if
  // the filesystem supports it).
  const char* cache_eviction_policy = nullptr;

  // If set, run fsck after every transaction.
  bool fsck_after_every_transaction = false;

  // If set, attach the filesystem with O_ADMIN, which will allow the use of the DirectoryAdmin
  // protocol.
  bool admin = true;

  // If true, bind to the namespace rather than using a remote-mount.
  bool bind_to_namespace = false;

  // If true, puts decompression in a sandboxed process.
  bool sandbox_decompression = false;

  // If set, handle to the crypt client. The handle is *always* consumed, even on error.
  zx_handle_t crypt_client = ZX_HANDLE_INVALID;
};

// Given the following:
//  - A device containing a filesystem image of a known format
//  - A path on which to mount the filesystem
//  - Some configuration options for launching the filesystem, and
//  - A callback which can be used to 'launch' an fs server,
//
// Prepare the argv arguments to the filesystem process, mount a handle on the
// expected mount_path, and call the 'launch' callback (if the filesystem is
// recognized).
//
// device_fd is always consumed. If the callback is reached, then the 'device_fd'
// is transferred via handles to the callback arguments.
zx::status<fidl::ClientEnd<fuchsia_io_admin::DirectoryAdmin>> Mount(fbl::unique_fd device_fd,
                                                                    const char* mount_path,
                                                                    DiskFormat df,
                                                                    const MountOptions& options,
                                                                    LaunchCallback cb);

// 'mount_fd' is used in lieu of the mount_path. It is not consumed (i.e.,
// it will still be open after this function completes, regardless of
// success or failure).
zx::status<fidl::ClientEnd<fuchsia_io_admin::DirectoryAdmin>> Mount(fbl::unique_fd device_fd,
                                                                    int mount_fd, DiskFormat df,
                                                                    const MountOptions& options,
                                                                    LaunchCallback cb);

// Mounts the filesystem being served via root_handle.
zx_status_t MountRootHandle(fidl::ClientEnd<fuchsia_io_admin::DirectoryAdmin> root,
                            const char* mount_path);

// Umount the filesystem process.
//
// Returns ZX_ERR_BAD_STATE if mount_path could not be opened.
// Returns ZX_ERR_NOT_FOUND if there is no mounted filesystem on mount_path.
// Other errors may also be returned if problems occur while unmounting.
zx_status_t Unmount(const char* mount_path);
// 'mount_fd' is used in lieu of the mount_path. It is not consumed.
zx_status_t Unmount(int mount_fd);

}  // namespace fs_management

#endif  // SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_MOUNT_H_
