// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains host specific minfs::File code.

#include <lib/zx/result.h>

#include <memory>

#include "src/storage/minfs/file.h"
#include "src/storage/minfs/minfs_private.h"

namespace minfs {

File::~File() = default;

// We don't enable dirty cache on host.
bool File::DirtyCacheEnabled() const { return false; }

bool File::IsDirty() const { return false; }

zx::result<> File::WalkFileBlocks(size_t offset, size_t length,
                                  WalkWriteBlockHandlerType& handler) {
  return zx::ok();
}

zx::result<uint32_t> File::GetRequiredBlockCountForDirtyCache(size_t offset, size_t length,
                                                              uint32_t uncached_block_count) {
  return zx::ok(uncached_block_count);
}

zx::result<> File::MarkRequiredBlocksPending(size_t offset, size_t length,
                                             const Transaction& transaction) {
  return zx::ok();
}

zx::result<> File::FlushCachedWrites() { return zx::ok(); }
void File::DropCachedWrites() {}

zx::result<bool> File::ShouldFlush(bool is_truncate, size_t length, size_t offset) {
  return zx::ok(true);
}

zx::result<> File::ForceFlushTransaction(std::unique_ptr<Transaction> transaction) {
  // Ensure this Vnode remains alive while it has an operation in-flight.
  transaction->PinVnode(fbl::RefPtr(this));
  InodeSync(transaction.get(), kMxFsSyncMtime);  // Successful write/truncate updates mtime
  Vfs()->CommitTransaction(std::move(transaction));
  return zx::ok();
}

zx::result<> File::FlushTransaction(std::unique_ptr<Transaction> transaction, bool force) {
  return ForceFlushTransaction(std::move(transaction));
}

}  // namespace minfs
