// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_NVME_NVME_H_
#define SRC_DEVICES_BLOCK_DRIVERS_NVME_NVME_H_

#include <lib/device-protocol/pci.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/sync/completion.h>
#include <threads.h>
#include <zircon/types.h>

#include <ddktl/device.h>

#include "src/devices/block/drivers/nvme/commands.h"
#include "src/devices/block/drivers/nvme/queue-pair.h"
#include "src/devices/block/drivers/nvme/registers.h"

namespace nvme {

class Namespace;

class Nvme;
using DeviceType = ddk::Device<Nvme, ddk::Initializable>;
class Nvme : public DeviceType {
 public:
  explicit Nvme(zx_device_t* parent) : DeviceType(parent) {}
  ~Nvme() = default;

  static zx_status_t Bind(void* ctx, zx_device_t* dev);
  zx_status_t AddDevice(zx_device_t* dev);

  void DdkInit(ddk::InitTxn txn);
  void DdkRelease();

  // Perform an admin command synchronously (i.e., blocks for the command to complete or timeout).
  zx_status_t DoAdminCommandSync(Submission& submission,
                                 std::optional<zx::unowned_vmo> admin_data = std::nullopt);

  QueuePair* io_queue() const { return io_queue_.get(); }
  uint32_t max_data_transfer_bytes() const { return max_data_transfer_bytes_; }
  uint16_t atomic_write_unit_normal() const { return atomic_write_unit_normal_; }
  uint16_t atomic_write_unit_power_fail() const { return atomic_write_unit_power_fail_; }

 private:
  static int IrqThread(void* arg) { return static_cast<Nvme*>(arg)->IrqLoop(); }
  int IrqLoop();

  // Main driver initialization.
  zx_status_t Init();

  pci_protocol_t pci_;
  std::unique_ptr<fdf::MmioBuffer> mmio_;
  zx_handle_t irqh_;
  zx::bti bti_;
  CapabilityReg caps_;
  VersionReg version_;

  // Admin submission and completion queues.
  std::unique_ptr<QueuePair> admin_queue_;
  fbl::Mutex admin_lock_;  // Used to serialize admin transactions.
  sync_completion_t admin_signal_;
  Completion admin_result_;

  // IO submission and completion queues.
  std::unique_ptr<QueuePair> io_queue_;

  thrd_t irq_thread_;
  bool irq_thread_started_ = false;

  uint32_t max_data_transfer_bytes_;
  bool volatile_write_cache_ = false;
  uint16_t atomic_write_unit_normal_;
  uint16_t atomic_write_unit_power_fail_;

  std::vector<Namespace*> namespaces_;
};

}  // namespace nvme

#endif  // SRC_DEVICES_BLOCK_DRIVERS_NVME_NVME_H_
