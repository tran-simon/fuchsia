// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/basemgr/session_context_impl.h"

#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <lib/sys/cpp/testing/fake_launcher.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <optional>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/modular/bin/basemgr/sessions.h"
#include "src/modular/lib/modular_config/modular_config.h"
#include "src/modular/lib/modular_config/modular_config_accessor.h"
#include "src/modular/lib/modular_config/modular_config_constants.h"

namespace modular_testing {
namespace {

class SessionContextImplTest : public gtest::RealLoopFixture {};

TEST_F(SessionContextImplTest, StartSessionmgr) {
  sys::testing::FakeLauncher launcher;

  std::string url = "test_url_string";
  fuchsia::modular::session::AppConfig sessionmgr_app_config;
  sessionmgr_app_config.set_url(url);

  bool callback_called = false;
  launcher.RegisterComponent(
      url,
      [&callback_called](fuchsia::sys::LaunchInfo /* unused */,
                         fidl::InterfaceRequest<fuchsia::sys::ComponentController> /* unused */) {
        callback_called = true;
      });

  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  scenic::ViewRefPair view_ref_pair = scenic::ViewRefPair::New();
  auto modular_config_accessor = modular::ModularConfigAccessor(modular::DefaultConfig());

  modular::SessionContextImpl impl(
      &launcher, std::move(sessionmgr_app_config), &modular_config_accessor,
      std::make_optional(modular::GfxViewParams{.view_token = std::move(view_token),
                                                .view_ref_pair = std::move(view_ref_pair)}),
      /*v2_services_for_sessionmgr=*/fuchsia::sys::ServiceList(),
      /*svc_from_v1_sessionmgr=*/nullptr,
      /*on_session_shutdown=*/[](modular::SessionContextImpl::ShutDownReason /* unused */) {});

  EXPECT_TRUE(callback_called);
}

TEST_F(SessionContextImplTest, SessionmgrCrashInvokesOnSessionShutdown) {
  // Program the fake launcher to drop the CreateComponent request such that
  // the error handler of the sessionmgr_app is invoked. This should invoke the
  // on_session_shutdown callback.
  sys::testing::FakeLauncher launcher;

  std::string url = "test_url_string";
  fuchsia::modular::session::AppConfig sessionmgr_app_config;
  sessionmgr_app_config.set_url(url);

  launcher.RegisterComponent(
      url, [](fuchsia::sys::LaunchInfo /* unused */,
              fidl::InterfaceRequest<fuchsia::sys::ComponentController> /* unused */) {});

  auto modular_config_accessor = modular::ModularConfigAccessor(modular::DefaultConfig());
  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  scenic::ViewRefPair view_ref_pair = scenic::ViewRefPair::New();

  bool on_session_shutdown_called = false;
  modular::SessionContextImpl impl(
      &launcher, std::move(sessionmgr_app_config), &modular_config_accessor,
      std::make_optional(modular::GfxViewParams{.view_token = std::move(view_token),
                                                .view_ref_pair = std::move(view_ref_pair)}),
      /*v2_services_for_sessionmgr=*/fuchsia::sys::ServiceList(),
      /*svc_from_v1_sessionmgr=*/nullptr,
      /*on_session_shutdown=*/
      [&on_session_shutdown_called](modular::SessionContextImpl::ShutDownReason /* unused */) {
        on_session_shutdown_called = true;
      });

  RunLoopUntil([&on_session_shutdown_called]() { return on_session_shutdown_called; });
  EXPECT_TRUE(on_session_shutdown_called);
}

}  // namespace
}  // namespace modular_testing
