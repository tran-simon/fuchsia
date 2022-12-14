// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_EXTERNAL_MEMORY_ALLOCATOR_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_EXTERNAL_MEMORY_ALLOCATOR_H_

#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <lib/zx/event.h>

#include "allocator.h"

namespace sysmem_driver {
class ExternalMemoryAllocator : public MemoryAllocator {
 public:
  ExternalMemoryAllocator(MemoryAllocator::Owner* owner,
                          fidl::WireSharedClient<fuchsia_sysmem2::Heap> heap,
                          fuchsia_sysmem2::HeapProperties properties);

  ~ExternalMemoryAllocator() override;

  zx_status_t Allocate(uint64_t size, std::optional<std::string> name,
                       zx::vmo* parent_vmo) override;
  zx_status_t SetupChildVmo(const zx::vmo& parent_vmo, const zx::vmo& child_vmo,
                            fuchsia_sysmem2::SingleBufferSettings buffer_settings) override;
  void Delete(zx::vmo parent_vmo) override;
  bool is_empty() override { return allocations_.empty(); }

 private:
  MemoryAllocator::Owner* owner_;
  fidl::WireSharedClient<fuchsia_sysmem2::Heap> heap_;

  // From parent vmo handle to ID.
  std::map<zx_handle_t, uint64_t> allocations_;

  inspect::Node node_;
  inspect::ValueList properties_;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_EXTERNAL_MEMORY_ALLOCATOR_H_
