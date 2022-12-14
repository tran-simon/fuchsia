// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_DISPATCHER_H_
#define SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_DISPATCHER_H_

#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/async/task.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <zircon/status.h>

#include <mutex>

namespace wlan {
namespace common {

template <typename I>
class Dispatcher {
 public:
  explicit Dispatcher(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

  // Start serving requests on the given channel.
  // Fails if shutdown has been initiated.
  zx_status_t AddBinding(fidl::ServerEnd<I> server_end, fidl::WireServer<I>* intf) {
    std::lock_guard<std::mutex> shutdown_guard(lock_);
    if (shutdown_initiated_) {
      return ZX_ERR_PEER_CLOSED;
    }

    bindings_.emplace_back(fidl::BindServer(dispatcher_, std::move(server_end), intf));
    return ZX_OK;
  }

  // Stop accepting new requests initiate shutdown.
  // If |ready_callback| is supplied, then it will be called from
  // the event loop thread once shutdown is complete.
  //
  // If |InitiateShutdown| has been already called previously,
  // then it returns immediately, and |ready_callback| is ignored.
  void InitiateShutdown(fit::closure ready_callback) {
    std::vector<fidl::ServerBindingRef<I>> bindings;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (shutdown_initiated_) {
        return;
      }
      shutdown_initiated_ = true;

      bindings_.swap(bindings);
    }

    // Release any FIDL bindings and close their channels. This ensures
    // that no additional requests will be made via this dispatcher.
    for (fidl::ServerBindingRef<I> binding : bindings) {
      binding.Unbind();
    }

    // Submit a sentinel task. Since the event loop in our async_t is single-threaded,
    // the execution of this task will guarantee that all in-flight requests have finished.
    if (ready_callback) {
      zx_status_t status = async::PostTask(dispatcher_, std::move(ready_callback));
      ZX_DEBUG_ASSERT_MSG(status == ZX_OK, "%s", zx_status_get_string(status));
    }
  }

 private:
  std::vector<fidl::ServerBindingRef<I>> bindings_ __TA_GUARDED(lock_);
  async_dispatcher_t* dispatcher_;
  std::mutex lock_;
  bool shutdown_initiated_ __TA_GUARDED(lock_){false};
};

}  // namespace common
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_DISPATCHER_H_
