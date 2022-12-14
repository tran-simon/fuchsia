// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/errors.h>

#include <mutex>

#include <fbl/ref_ptr.h>

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

DirtyPageList::~DirtyPageList() {
  std::lock_guard list_lock(list_lock_);
  ZX_ASSERT(dirty_list_.is_empty());
}

void DirtyPageList::Reset() {
  std::lock_guard list_lock(list_lock_);
  dirty_list_.clear();
}

zx::result<> DirtyPageList::AddDirty(LockedPage &page) {
  ZX_DEBUG_ASSERT(page->InTreeContainer());
  ZX_DEBUG_ASSERT(page->IsActive());
  if (page->GetVnode().GetPageType() == PageType::kData && !page->GetVnode().IsDir()) {
    std::lock_guard lock(list_lock_);
    if (page->InListContainer()) {
      return zx::error(ZX_ERR_ALREADY_EXISTS);
    } else {
      // No need to consider a case where |page| is recycled as passing |page| is always active.
      dirty_list_.push_back(page.CopyRefPtr());
    }
  }
  return zx::ok();
}

zx::result<> DirtyPageList::RemoveDirty(LockedPage &page) {
  ZX_DEBUG_ASSERT(page->IsActive());
  if (page->GetVnode().GetPageType() == PageType::kData && !page->GetVnode().IsDir()) {
    std::lock_guard lock(list_lock_);
    if (!page->InListContainer()) {
      return zx::error(ZX_ERR_NOT_FOUND);
    }
    dirty_list_.erase(*page);
  }
  return zx::ok();
}

std::vector<LockedPage> DirtyPageList::TakePages(size_t count) {
  std::vector<LockedPage> dirty_pages;
  std::lock_guard lock(list_lock_);
  PageList temp_list;
  size_t try_count = std::min(count, dirty_list_.size());
  dirty_pages.reserve(try_count);
  while (!dirty_list_.is_empty() && try_count--) {
    auto page = dirty_list_.pop_front();
    if (page->IsDirty()) {
      if (!page->TryLock()) {
        LockedPage locked_page(std::move(page), false);
        dirty_pages.push_back(std::move(locked_page));
      } else {
        // If someone already holds its lock, skip it.
        temp_list.push_back(std::move(page));
      }
    }
  }

  // Keep the order Pages are inserted in dirty_list_.
  dirty_list_.splice(dirty_list_.begin(), temp_list);
  return dirty_pages;
}

}  // namespace f2fs
