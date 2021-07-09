// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_

#include <zircon/types.h>

#include <set>
#include <string>

#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/shared/status.h"
#include "src/lib/fxl/macros.h"

namespace debug_agent {

// A single breakpoint may apply to many processes and many addresses (even in
// the same process). These instances are called ProcessBreakpoints.
//
// Multiple Breakpoints can also correspond to the same ProcessBreakpoint if
// they have the same process/address.
class Breakpoint {
 public:
  // Normally an exception applies to a breakpoint only if both types match. But in the case of
  // watchpoints, they can be triggered either by a read or a write exception, so a same address
  // could apply or not depending on the type of the breakpoint (eg. a read exception would apply
  // to both read and read/write breakpoints).
  //
  // All checks to see if an exception matches a breakpoint should be done by this function and not
  // directly checking |type()|.
  static bool DoesExceptionApply(debug_ipc::BreakpointType exception,
                                 debug_ipc::BreakpointType bp_type);
  enum class HitResult {
    // Breakpoint was hit and the hit count was incremented.
    kHit,

    // One-shot breakpoint hit. The caller should delete this breakpoint
    // when it sees this result.
    kOneShotHit,

    // This will need to be expanded to include "kContinue" to indicate that
    // this doesn't count as hitting the breakpoint (for when we implement
    // "break on hit count == 5" or "multiple of 7").
  };

  // The process delegate should outlive the Breakpoint object. It allows
  // Breakpoint dependencies to be mocked for testing.
  class ProcessDelegate {
   public:
    // Called to register a new ProcessBreakpoint with the appropriate
    // location. If this fails, the breakpoint has not been set.
    virtual debug::Status RegisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid,
                                             uint64_t address);

    // Called When the breakpoint no longer applies to this location.
    virtual void UnregisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid, uint64_t address);

    virtual debug::Status RegisterWatchpoint(Breakpoint* bp, zx_koid_t process_koid,
                                             const debug_ipc::AddressRange& range);

    virtual void UnregisterWatchpoint(Breakpoint* bp, zx_koid_t process_koid,
                                      const debug_ipc::AddressRange& range);
  };

  // Some breakpoints are internally generated by the debug_agent. These are different than the
  // internal breakpoints set by the zxdb frontend which we see as nrmal breakpoints. Our internal
  // breakpoints are never sent to the client.
  explicit Breakpoint(ProcessDelegate* process_delegate, bool is_debug_agent_internal = false);
  ~Breakpoint();

  bool is_debug_agent_internal() const { return is_debug_agent_internal_; }
  const debug_ipc::BreakpointStats& stats() const { return stats_; }

  // Calling SetSettings() will update the settings and install/move the breakpoint as required.
  // The one taking an address is for internal process-wide software breakpoints that have no ID.
  debug::Status SetSettings(const debug_ipc::BreakpointSettings& settings);
  debug::Status SetSettings(std::string name, zx_koid_t process_koid, uint64_t address);
  const debug_ipc::BreakpointSettings& settings() const { return settings_; }

  // A breakpoint can be set to apply to a specific set of threads. A thread
  // hitting an exception needs to query whether it should apply to it or not.
  bool AppliesToThread(zx_koid_t process_koid, zx_koid_t thread_koid) const;

  // Notification that this breakpoint was just hit.
  HitResult OnHit();

 private:
  debug::Status SetBreakpointLocations(const debug_ipc::BreakpointSettings&);
  debug::Status SetWatchpointLocations(const debug_ipc::BreakpointSettings&);

  // A process koid + address identifies one unique location.
  using LocationPair = std::pair<zx_koid_t, uint64_t>;

  using WatchpointLocationPair = std::pair<zx_koid_t, debug_ipc::AddressRange>;
  struct WatchpointLocationPairCompare {
    bool operator()(const WatchpointLocationPair&, const WatchpointLocationPair&) const;
  };

  ProcessDelegate* process_delegate_;  // Non-owning.
  bool is_debug_agent_internal_;

  debug_ipc::BreakpointSettings settings_;

  debug_ipc::BreakpointStats stats_;

  std::set<LocationPair> locations_;
  std::set<WatchpointLocationPair, WatchpointLocationPairCompare> watchpoint_locations_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Breakpoint);
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_
