// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/fuzzing/framework/engine/process-proxy-test.h"

#include <zircon/status.h>

#include "src/sys/fuzzing/common/async-eventpair.h"
#include "src/sys/fuzzing/framework/target/process.h"

namespace fuzzing {

void ProcessProxyTest::SetUp() {
  AsyncTest::SetUp();
  pool_ = std::make_shared<ModulePool>();
}

OptionsPtr ProcessProxyTest::DefaultOptions() {
  auto options = MakeOptions();
  ProcessProxy::AddDefaults(options.get());
  return options;
}

std::unique_ptr<ProcessProxy> ProcessProxyTest::CreateAndConnectProxy(zx::process process) {
  return CreateAndConnectProxy(std::move(process), ProcessProxyTest::DefaultOptions());
}

std::unique_ptr<ProcessProxy> ProcessProxyTest::CreateAndConnectProxy(zx::process process,
                                                                      const OptionsPtr& options) {
  AsyncEventPair eventpair(executor());
  return CreateAndConnectProxy(std::move(process), options, eventpair.Create());
}

std::unique_ptr<ProcessProxy> ProcessProxyTest::CreateAndConnectProxy(zx::process process,
                                                                      zx::eventpair eventpair) {
  return CreateAndConnectProxy(std::move(process), ProcessProxyTest::DefaultOptions(),
                               std::move(eventpair));
}

std::unique_ptr<ProcessProxy> ProcessProxyTest::CreateAndConnectProxy(zx::process process,
                                                                      const OptionsPtr& options,
                                                                      zx::eventpair eventpair) {
  auto process_proxy = std::make_unique<ProcessProxy>(executor(), pool_);
  process_proxy->Configure(options);
  InstrumentedProcess instrumented;
  instrumented.set_process(std::move(process));
  instrumented.set_eventpair(std::move(eventpair));
  EXPECT_EQ(process_proxy->Connect(instrumented), ZX_OK);
  return process_proxy;
}

}  // namespace fuzzing
