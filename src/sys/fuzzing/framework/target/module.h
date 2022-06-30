// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_FRAMEWORK_TARGET_MODULE_H_
#define SRC_SYS_FUZZING_FRAMEWORK_TARGET_MODULE_H_

#include <fuchsia/fuzzer/cpp/fidl.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "src/lib/fxl/macros.h"
#include "src/sys/fuzzing/common/shared-memory.h"

namespace fuzzing {

using Identifier = std::array<uint64_t, 2>;

// Represents an LLVM "module", e.g. a collection of translation units, such as a shared object
// library. The instrumented processes rely on instances of these classes to collect feedback (e.g.
// code coverage) for the fuzzer engine.
class Module final {
 public:
  // For every edge, there should be an 8-bit counter, a PC uintptr_t, and a PCFlags uintptr_t.
  // Thus, |counters| should be an array of length |num_pcs|, and |pcs| of length |num_pcs| * 2.
  // See also: https://clang.llvm.org/docs/SanitizerCoverage.html
  Module(uint8_t* counters, const uintptr_t* pcs, size_t num_pcs);
  Module(Module&& other) noexcept { *this = std::move(other); }
  ~Module() = default;
  Module& operator=(Module&& other) noexcept;

  // Return a unique identifier for this module as described in |fuchsia.fuzzer.Feedback|. This
  // identifier will be the same for the same module across multiple processes and/or invocations.
  const Identifier& legacy_id() const { return legacy_id_; }
  const std::string& id() const { return id_; }

  // Shares the VMO containing the code coverage. This will set a name on the VMO constructed from
  // the given |target_id| and the module's id.
  __WARN_UNUSED_RESULT zx_status_t Share(uint64_t target_id, zx::vmo* out) const;

  // Update the code-coverage counters to produce feedback for this module.
  void Update() { counters_.Update(); }

  // Reset the code-coverage counters for this module.
  void Clear() { counters_.Clear(); }

 private:
  Identifier legacy_id_;
  std::string id_;
  SharedMemory counters_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Module);
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_FRAMEWORK_TARGET_MODULE_H_
