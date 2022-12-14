// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_NAND_DRIVERS_BROKER_TEST_PARENT_H_
#define SRC_DEVICES_NAND_DRIVERS_BROKER_TEST_PARENT_H_

#include <fidl/fuchsia.hardware.nand/cpp/wire.h>
#include <limits.h>

#include <variant>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramnand.h>

// The nand device that will be used as the parent of the broker device. This
// can be a ram-nand device instantiated for the test, or any nand device
// already on the system.
class ParentDevice {
 public:
  struct TestConfig {
    fuchsia_hardware_nand::wire::Info info;                   // Configuration for a new ram-nand.
    fuchsia_hardware_nand::wire::PartitionMap partition_map;  // Configuration for a new ram-nand.
    const char* path;                                         // Path to an existing device.
    bool is_broker;        // True is the device is a broker (not a nand).
    uint32_t num_blocks;   // Number of blocks to use.
    uint32_t first_block;  // First block to use.
  };

  static zx::result<ParentDevice> Create(const TestConfig& config);

  // Not copyable.
  ParentDevice(const ParentDevice&) = delete;
  ParentDevice& operator=(const ParentDevice&) = delete;

  // Movable.
  ParentDevice(ParentDevice&&) = default;
  ParentDevice& operator=(ParentDevice&&) = default;

  ~ParentDevice() = default;

  const char* Path() const;

  bool IsExternal() const {
    return std::holds_alternative<fidl::ClientEnd<fuchsia_device::Controller>>(device_);
  }
  bool IsBroker() const { return config_.is_broker; }

  const fidl::ClientEnd<fuchsia_device::Controller>& controller() const;

  const fuchsia_hardware_nand::wire::Info& Info() const { return config_.info; }
  void SetInfo(const fuchsia_hardware_nand::wire::Info& info);

  const fuchsia_hardware_nand::wire::PartitionMap& PartitionMap() const {
    return config_.partition_map;
  }
  void SetPartitionMap(const fuchsia_hardware_nand::wire::PartitionMap& partition_map);

  uint32_t NumBlocks() const { return config_.num_blocks; }
  uint32_t FirstBlock() const { return config_.first_block; }

 private:
  ParentDevice(fidl::ClientEnd<fuchsia_device::Controller> controller, const TestConfig& config);
  ParentDevice(ramdevice_client::RamNand ram_nand, const TestConfig& config);

  std::variant<fidl::ClientEnd<fuchsia_device::Controller>, ramdevice_client::RamNand> device_;
  TestConfig config_;
};

extern ParentDevice* g_parent_device_;

#endif  // SRC_DEVICES_NAND_DRIVERS_BROKER_TEST_PARENT_H_
