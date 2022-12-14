// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZX_PROCESS_H_
#define LIB_ZX_PROCESS_H_

#include <lib/zx/object.h>
#include <lib/zx/task.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/availability.h>
#include <zircon/process.h>

namespace zx {
class job;
class thread;

class process final : public task<process> {
 public:
  static constexpr zx_obj_type_t TYPE = ZX_OBJ_TYPE_PROCESS;

  constexpr process() = default;

  explicit process(zx_handle_t value) : task(value) {}

  explicit process(handle&& h) : task(h.release()) {}

  process(process&& other) : task(other.release()) {}

  process& operator=(process&& other) {
    reset(other.release());
    return *this;
  }

  static zx_status_t create(const job& job, const char* name, uint32_t name_len, uint32_t flags,
                            process* proc, vmar* root_vmar) ZX_AVAILABLE_SINCE(7);

  static zx_status_t create_shared(const process& prototype, const char* name, uint32_t name_len,
                                   uint32_t flags, process* proc, vmar* restricted_vmar)
      ZX_AVAILABLE_SINCE(8);

  zx_status_t start(const thread& thread_handle, uintptr_t entry, uintptr_t stack,
                    handle arg_handle, uintptr_t arg2) const ZX_AVAILABLE_SINCE(7);

  zx_status_t read_memory(uintptr_t vaddr, void* buffer, size_t len, size_t* actual) const
      ZX_AVAILABLE_SINCE(7) {
    return zx_process_read_memory(get(), vaddr, buffer, len, actual);
  }

  zx_status_t write_memory(uintptr_t vaddr, const void* buffer, size_t len, size_t* actual) const
      ZX_AVAILABLE_SINCE(7) {
    return zx_process_write_memory(get(), vaddr, buffer, len, actual);
  }

  // Provide strongly-typed overload, in addition to get_child(handle*).
  using task<process>::get_child;
  zx_status_t get_child(uint64_t koid, zx_rights_t rights, thread* result) const
      ZX_AVAILABLE_SINCE(7);

  static inline unowned<process> self() ZX_AVAILABLE_SINCE(7) {
    return unowned<process>(zx_process_self());
  }
} ZX_AVAILABLE_SINCE(7);

using unowned_process = unowned<process> ZX_AVAILABLE_SINCE(7);

}  // namespace zx

#endif  // LIB_ZX_PROCESS_H_
