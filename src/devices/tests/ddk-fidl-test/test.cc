// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.hardware.test/cpp/wire.h>
#include <fuchsia/hardware/test/c/fidl.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <zircon/fidl.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <vector>

#include <zxtest/zxtest.h>

#include "fidl/fuchsia.hardware.test/cpp/wire_messaging.h"

using driver_integration_test::IsolatedDevmgr;

namespace {

void CheckTransaction(const board_test::DeviceEntry& entry, const char* device_fs) {
  IsolatedDevmgr devmgr;

  // Set the driver arguments.
  IsolatedDevmgr::Args args;
  args.device_list.push_back(entry);

  // Create the isolated Devmgr.
  zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr);
  ASSERT_OK(status);

  // Wait for the driver to be created
  zx::result channel = device_watcher::RecursiveWaitForFile(devmgr.devfs_root().get(), device_fs);
  ASSERT_OK(channel.status_value());

  // Get a FIDL channel to the device
  zx::channel driver_channel = std::move(channel.value());

  // The method does not define a request payload, so the message should be a header only.
  fidl_message_header_t hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  zx_txid_t first_txid = 1;
  fidl::InitTxnHeader(&hdr, first_txid, fuchsia_hardware_test_DeviceGetChannelOrdinal,
                      fidl::MessageDynamicFlags::kStrictMethod);
  ASSERT_OK(driver_channel.write(0, &hdr, sizeof(hdr), nullptr, 0));
  ASSERT_OK(driver_channel.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), nullptr));

  std::memset(&hdr, 0, sizeof(hdr));
  zx_txid_t second_txid = 2;
  fidl::InitTxnHeader(&hdr, second_txid, fuchsia_hardware_test_DeviceGetChannelOrdinal,
                      fidl::MessageDynamicFlags::kStrictMethod);
  ASSERT_OK(driver_channel.write(0, &hdr, sizeof(hdr), nullptr, 0));

  // If the transaction incorrectly closes the sent handles, it will cause a policy violation.
  // Waiting for the channel to be readable once isn't enough, there is still a very small amount
  // of time before the transaction destructor runs. A second read ensures that the first
  // succeeded. If a policy violation occurs, the second read below will fail as the driver
  // channel will have been closed.
  auto msg_bytes = std::make_unique<uint8_t[]>(ZX_CHANNEL_MAX_MSG_BYTES);
  auto msg_handles = std::make_unique<zx_handle_t[]>(ZX_CHANNEL_MAX_MSG_HANDLES);
  uint32_t actual_bytes = 0;
  uint32_t actual_handles = 0;

  status = driver_channel.read(0, msg_bytes.get(), msg_handles.get(), ZX_CHANNEL_MAX_MSG_BYTES,
                               ZX_CHANNEL_MAX_MSG_HANDLES, &actual_bytes, &actual_handles);
  if (status == ZX_ERR_SHOULD_WAIT) {
    ASSERT_OK(driver_channel.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), nullptr));
  } else {
    ASSERT_OK(status);
  }

  status = driver_channel.read(0, msg_bytes.get(), msg_handles.get(), ZX_CHANNEL_MAX_MSG_BYTES,
                               ZX_CHANNEL_MAX_MSG_HANDLES, &actual_bytes, &actual_handles);
  if (status == ZX_ERR_SHOULD_WAIT) {
    ASSERT_OK(driver_channel.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), nullptr));
  } else {
    ASSERT_OK(status);
  }
}

// Test that the transaction does not incorrectly close handles during Reply.
TEST(FidlDDKDispatcherTest, SyncTransactionHandleTest) {
  board_test::DeviceEntry entry = {};
  strcpy(entry.name, "ddk-fidl");
  entry.vid = PDEV_VID_TEST;
  entry.pid = PDEV_PID_DDKFIDL_TEST;
  entry.did = PDEV_DID_TEST_DDKFIDL;
  CheckTransaction(entry, "sys/platform/11:09:d/ddk-fidl");
}

TEST(FidlDDKDispatcherTest, AsyncTransactionHandleTest) {
  board_test::DeviceEntry entry = {};
  strcpy(entry.name, "ddk-async-fidl");
  entry.vid = PDEV_VID_TEST;
  entry.pid = PDEV_PID_DDKFIDL_TEST;
  entry.did = PDEV_DID_TEST_DDKASYNCFIDL;
  CheckTransaction(entry, "sys/platform/11:09:15/ddk-async-fidl");
}

}  // namespace
