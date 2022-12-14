// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <storage/buffer/resizeable_vmo_buffer.h>

namespace storage {

zx::result<> ResizeableVmoBuffer::Attach(const char* name, storage::VmoidRegistry* device) {
  ZX_DEBUG_ASSERT(!vmoid_.IsAttached());
  zx_status_t status = vmo_.CreateAndMap(block_size_, name);
  if (status != ZX_OK)
    return zx::error(status);
  return zx::make_result(device->BlockAttachVmo(vmo_.vmo(), &vmoid_));
}

zx::result<> ResizeableVmoBuffer::Detach(storage::VmoidRegistry* device) {
  return zx::make_result(device->BlockDetachVmo(std::move(vmoid_)));
}

zx_status_t ResizeableVmoBuffer::Zero(size_t index, size_t count) {
  return vmo_.vmo().op_range(ZX_VMO_OP_ZERO, index * BlockSize(), count * BlockSize(), nullptr, 0);
}

}  // namespace storage
