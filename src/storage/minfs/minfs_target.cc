// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/macros.h>

#include "src/storage/minfs/minfs_private.h"

#ifndef __Fuchsia__
static_assert(false, "This file is not meant to be used on host");
#endif  // __Fuchsia__

namespace minfs {

[[nodiscard]] bool Minfs::IsJournalErrored() {
  return journal_ != nullptr && !journal_->IsWritebackEnabled();
}

std::vector<fbl::RefPtr<VnodeMinfs>> Minfs::GetDirtyVnodes() {
  fbl::RefPtr<VnodeMinfs> vn;
  std::vector<fbl::RefPtr<VnodeMinfs>> vnodes;

  // vnode_hash_ is locked to release vnode reference. If we are the last one to hold vnode
  // reference and the vnode is clean then this block will end up releasing the vnode. To avoid
  // deadlock, we park clean vnodes in an array which will be released outside of the lock.
  std::vector<fbl::RefPtr<VnodeMinfs>> unused_clean_vnodes;

  // Avoid releasing a reference to |vn| while holding |hash_lock_|.
  fbl::AutoLock lock(&hash_lock_);
  for (auto& raw_vnode : vnode_hash_) {
    vn = fbl::MakeRefPtrUpgradeFromRaw(&raw_vnode, hash_lock_);
    if (vn == nullptr) {
      continue;
    }
    if (vn->IsDirty()) {
      vnodes.push_back(std::move(vn));
    } else {
      unused_clean_vnodes.push_back(std::move(vn));
    }
  }
  return vnodes;
}

zx::result<> Minfs::ContinueTransaction(size_t reserve_blocks,
                                        std::unique_ptr<CachedBlockTransaction> cached_transaction,
                                        std::unique_ptr<Transaction>* out) {
  if (journal_ == nullptr) {
    return zx::error(ZX_ERR_BAD_STATE);
  }

  if (!journal_->IsWritebackEnabled()) {
    return zx::error(ZX_ERR_IO_REFUSED);
  }

  // TODO(unknown): Once we are splitting up write
  // transactions, assert this on host as well.
  ZX_DEBUG_ASSERT(reserve_blocks <= limits_.GetMaximumDataBlocks());

  *out = Transaction::FromCachedBlockTransaction(this, std::move(cached_transaction));

  // Reserve blocks from allocators before returning WritebackWork to client.
  auto status = (*out)->ExtendBlockReservation(reserve_blocks);
  if (status.status_value() == ZX_ERR_NO_SPACE && reserve_blocks > 0) {
    // When there's no more space, flush the journal in case a recent transaction has freed blocks
    // but has yet to be flushed from the journal and committed. Then, try again.
    FX_LOGS(INFO)
        << "Unable to reserve blocks. Flushing journal in attempt to reclaim unlinked blocks.";
    auto sync_status = BlockingJournalSync();
    if (sync_status.is_error()) {
      FX_LOGS(ERROR) << "Failed to flush journal (status: " << sync_status.status_string() << ")";
      inspect_tree_.OnOutOfSpace();
      // Return the original status.
      return sync_status.take_error();
    }

    status = (*out)->ExtendBlockReservation(reserve_blocks);
    if (status.is_ok()) {
      inspect_tree_.OnRecoveredSpace();
    }
  }

  if (status.is_error()) {
    FX_LOGS(ERROR) << "Failed to extend block reservation (status: " << status.status_string()
                   << ")";
    if (status.error_value() == ZX_ERR_NO_SPACE) {
      inspect_tree_.OnOutOfSpace();
    }
    return status.take_error();
  }

  return zx::ok();
}

bool Minfs::AllReservationsBacked(const Transaction&) const {
  // Fsck should ensure we reformat the device should this assertion fire.
  ZX_ASSERT(Info().block_count >= Info().alloc_block_count);
  uint32_t blocks_available = Info().block_count - Info().alloc_block_count;
  return BlocksReserved() <= blocks_available;
}

zx::result<> Minfs::BlockingJournalSync() {
  zx_status_t sync_status = ZX_OK;
  sync_completion_t sync_completion = {};
  journal_->schedule_task(journal_->Sync().then(
      [&sync_status, &sync_completion](fpromise::result<void, zx_status_t>& a) {
        sync_status = a.is_ok() ? ZX_OK : a.error();
        sync_completion_signal(&sync_completion);
        return fpromise::ok();
      }));
  zx_status_t status = sync_completion_wait(&sync_completion, ZX_TIME_INFINITE);
  if (status != ZX_OK) {
    return zx::make_result(status);
  }
  return zx::make_result(sync_status);
}

}  // namespace minfs
