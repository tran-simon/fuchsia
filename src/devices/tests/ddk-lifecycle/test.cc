// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.device.lifecycle.test/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.serial/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/ddk/platform-defs.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <vector>

#include <zxtest/zxtest.h>

using driver_integration_test::IsolatedDevmgr;
using fuchsia_device::Controller;
using fuchsia_device_lifecycle_test::Lifecycle;
using fuchsia_device_lifecycle_test::TestDevice;

class LifecycleTest : public zxtest::Test {
 public:
  ~LifecycleTest() override = default;
  void SetUp() override {
    IsolatedDevmgr::Args args;

#ifdef DFV2
    args.use_driver_framework_v2 = true;
#endif

    board_test::DeviceEntry dev = {};
    dev.vid = PDEV_VID_TEST;
    dev.pid = PDEV_PID_LIFECYCLE_TEST;
    dev.did = 0;
    args.device_list.push_back(dev);

    zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr_);
    ASSERT_OK(status);
    zx::result channel = device_watcher::RecursiveWaitForFile(
        devmgr_.devfs_root().get(), "sys/platform/11:10:0/ddk-lifecycle-test");
    ASSERT_OK(channel.status_value());
    chan_ = fidl::ClientEnd<TestDevice>(std::move(channel.value()));

    // Subscribe to the device lifecycle events.
    auto endpoints = fidl::CreateEndpoints<Lifecycle>();
    ASSERT_OK(endpoints.status_value());
    auto [local, remote] = *std::move(endpoints);

    auto result = fidl::WireCall(chan_)->SubscribeToLifecycle(std::move(remote));
    ASSERT_OK(result.status());
    ASSERT_FALSE(result->is_error());
    lifecycle_chan_ = std::move(local);
  }

 protected:
  void WaitPreRelease(uint64_t child_id);

  fidl::ClientEnd<TestDevice> chan_;
  IsolatedDevmgr devmgr_;

  fidl::ClientEnd<Lifecycle> lifecycle_chan_;
};

void LifecycleTest::WaitPreRelease(uint64_t child_id) {
  class EventHandler : public fidl::WireSyncEventHandler<Lifecycle> {
   public:
    EventHandler() = default;

    bool removed() const { return removed_; }
    uint64_t device_id() const { return device_id_; }

    void OnChildPreRelease(fidl::WireEvent<Lifecycle::OnChildPreRelease>* event) override {
      device_id_ = event->child_id;
      removed_ = true;
    }

   private:
    bool removed_ = false;
    uint64_t device_id_ = 0;
  };

  EventHandler event_handler;
  while (!event_handler.removed()) {
    ASSERT_OK(event_handler.HandleOneEvent(lifecycle_chan_).status());
  }
  ASSERT_EQ(event_handler.device_id(), child_id);
}

TEST_F(LifecycleTest, ChildPreRelease) {
  // Add some child devices and store the returned ids.
  std::vector<uint64_t> child_ids;
  const uint32_t num_children = 10;
  for (unsigned int i = 0; i < num_children; i++) {
    auto result =
        fidl::WireCall(chan_)->AddChild(true /* complete_init */, ZX_OK /* init_status */);
    ASSERT_OK(result.status());
    ASSERT_FALSE(result->is_error());
    child_ids.push_back(result->value()->child_id);
  }

  // Remove the child devices and check the test device received the pre-release notifications.
  for (auto child_id : child_ids) {
    auto result = fidl::WireCall(chan_)->RemoveChild(child_id);
    ASSERT_OK(result.status());
    ASSERT_FALSE(result->is_error());

    // Wait for the child pre-release notification.
    ASSERT_NO_FATAL_FAILURE(WaitPreRelease(child_id));
  }
}

TEST_F(LifecycleTest, Init) {
  // Add a child device that does not complete its init hook yet.
  uint64_t child_id;
  auto result = fidl::WireCall(chan_)->AddChild(false /* complete_init */, ZX_OK /* init_status */);
  ASSERT_OK(result.status());
  ASSERT_FALSE(result->is_error());
  child_id = result->value()->child_id;

  auto remove_result = fidl::WireCall(chan_)->RemoveChild(child_id);
  ASSERT_OK(remove_result.status());
  ASSERT_FALSE(remove_result->is_error());

  auto init_result = fidl::WireCall(chan_)->CompleteChildInit(child_id);
  ASSERT_OK(init_result.status());
  ASSERT_FALSE(init_result->is_error());

  // Wait for the child pre-release notification.
  ASSERT_NO_FATAL_FAILURE(WaitPreRelease(child_id));
}

#ifndef DFV2
TEST_F(LifecycleTest, CloseAllConnectionsOnInstanceUnbind) {
  auto result = fidl::WireCall(chan_)->AddChild(true /* complete_init */, ZX_OK /* init_status */);
  ASSERT_OK(result.status());
  ASSERT_FALSE(result->is_error());
  auto child_id = result->value()->child_id;

  zx::result channel = device_watcher::RecursiveWaitForFile(
      devmgr_.devfs_root().get(),
      "sys/platform/11:10:0/ddk-lifecycle-test/ddk-lifecycle-test-child-0");
  ASSERT_OK(channel.status_value());

  zx::channel chan = std::move(channel.value());
  ASSERT_TRUE(fidl::WireCall(chan_)->RemoveChild(child_id).ok());
  zx_signals_t closed;
  ASSERT_OK(chan.wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &closed));
  ASSERT_TRUE(closed & ZX_CHANNEL_PEER_CLOSED);
  // Wait for the child pre-release notification.
  ASSERT_NO_FATAL_FAILURE(WaitPreRelease(child_id));
}

TEST_F(LifecycleTest, CallsFailDuringUnbind) {
  auto result = fidl::WireCall(chan_)->AddChild(true /* complete_init */, ZX_OK /* init_status */);
  ASSERT_OK(result.status());
  ASSERT_FALSE(result->is_error());
  auto child_id = result->value()->child_id;

  zx::result channel = device_watcher::RecursiveWaitForFile(
      devmgr_.devfs_root().get(),
      "sys/platform/11:10:0/ddk-lifecycle-test/ddk-lifecycle-test-child-0");
  ASSERT_OK(channel.status_value());

  fidl::ClientEnd<fuchsia_io::File> file_client(std::move(channel.value()));

  ASSERT_TRUE(fidl::WireCall(chan_)->AsyncRemoveChild(child_id).ok());
  {
    const fidl::WireResult result = fidl::WireCall(file_client)->GetAttr();
    ASSERT_OK(result.status());
    const fidl::WireResponse response = result.value();
    ASSERT_STATUS(response.s, ZX_ERR_IO_NOT_PRESENT);
  }
  int fd2 = open("sys/platform/11:10:0/ddk-lifecycle-test/ddk-lifecycle-test-child-0", O_RDWR);
  ASSERT_EQ(fd2, -1);
  fidl::ClientEnd<fuchsia_hardware_serial::Device> device_client(file_client.TakeChannel());
  ASSERT_EQ(fidl::WireCall(device_client)->GetClass().status(), ZX_ERR_PEER_CLOSED);
  struct Epitaph {
    zx_txid_t txid;
    uint8_t flags[3];
    uint8_t magic_number;
    uint64_t ordinal;
    zx_status_t error;
  } epitaph;
  constexpr auto kEpitaph = 0xFFFFFFFFFFFFFFFF;
  uint32_t actual_bytes = 0;
  uint32_t actual_handles = 0;
  ASSERT_OK(device_client.channel().read(0, &epitaph, nullptr, sizeof(epitaph), 0, &actual_bytes,
                                         &actual_handles));
  ASSERT_EQ(actual_bytes, sizeof(epitaph));
  ASSERT_EQ(epitaph.ordinal, kEpitaph);
  ASSERT_EQ(epitaph.error, ZX_ERR_IO_NOT_PRESENT);
}

#endif

TEST_F(LifecycleTest, CloseAllConnectionsOnUnbind) {
  const fidl::WireResult result =
      fidl::WireCall(fidl::UnownedClientEnd<Controller>(chan_.channel().borrow()))
          ->ScheduleUnbind();
  ASSERT_OK(result.status());
  const fit::result response = result.value();
  ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));
  zx_signals_t closed;
  ASSERT_OK(chan_.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &closed));
  ASSERT_TRUE(closed & ZX_CHANNEL_PEER_CLOSED);
}

// Tests that the child device is removed if init fails.
TEST_F(LifecycleTest, FailedInit) {
  uint64_t child_id;
  auto result = fidl::WireCall<TestDevice>(chan_)->AddChild(true /* complete_init */,
                                                            ZX_ERR_BAD_STATE /* init_status */);
  ASSERT_OK(result.status());
  ASSERT_FALSE(result->is_error());
  child_id = result->value()->child_id;

  // Wait for the child pre-release notification.
  ASSERT_NO_FATAL_FAILURE(WaitPreRelease(child_id));
}
