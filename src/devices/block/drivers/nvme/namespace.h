// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_NVME_NAMESPACE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_NVME_NAMESPACE_H_

#include <fuchsia/hardware/block/cpp/banjo.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <threads.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <fbl/mutex.h>

namespace nvme {

struct IoCommand;
class Nvme;

class Namespace;
using NamespaceDeviceType = ddk::Device<Namespace, ddk::Initializable>;
class Namespace : public NamespaceDeviceType,
                  public ddk::BlockImplProtocol<Namespace, ddk::base_protocol> {
 public:
  explicit Namespace(zx_device_t* parent, Nvme* controller, uint32_t namespace_id)
      : NamespaceDeviceType(parent), controller_(controller), namespace_id_(namespace_id) {}

  // Create a namespace on |controller| with |namespace_id|.
  static zx::result<Namespace*> Bind(Nvme* controller, uint32_t namespace_id);
  zx_status_t AddNamespace();

  void DdkInit(ddk::InitTxn txn);
  void DdkRelease();

  // BlockImpl implementations
  void BlockImplQuery(block_info_t* out_info, uint64_t* out_block_op_size);
  void BlockImplQueue(block_op_t* op, block_impl_queue_callback callback, void* cookie);

  // Notifies IoThread() that it has work to do. Called by the IRQ handler.
  void SignalIo() { sync_completion_signal(&io_signal_); }

 private:
  static int IoThread(void* arg) { return static_cast<Namespace*>(arg)->IoLoop(); }
  int IoLoop();

  // Main driver initialization.
  zx_status_t Init();

  // Attempt to submit NVMe transactions for an IoCommand. Returns false if this could not be
  // completed due to temporary lack of resources, or true if either it succeeded or errored out.
  bool SubmitAllTxnsForIoCommand(IoCommand* io_cmd);

  // Process pending IO commands. Called in the IoLoop().
  void ProcessIoSubmissions();
  // Process pending IO completions. Called in the IoLoop().
  void ProcessIoCompletions();

  Nvme* controller_;
  const uint32_t namespace_id_;

  fbl::Mutex commands_lock_;
  // The pending list consists of commands that have been received via BlockImplQueue() and are
  // waiting for IO to start. The exception is the head of the pending list which may be partially
  // started, waiting for more txn slots to become available.
  list_node_t pending_commands_ TA_GUARDED(commands_lock_);  // Inbound commands to process.
  // The active list consists of commands where all txns have been created and we're waiting for
  // them to complete or error out.
  list_node_t active_commands_ TA_GUARDED(commands_lock_);  // Commands in flight.

  // Notifies IoThread() that it has work to do. Signaled from BlockImplQueue() or the IRQ handler.
  sync_completion_t io_signal_;

  thrd_t io_thread_;
  bool io_thread_started_ = false;
  bool driver_shutdown_ = false;

  block_info_t block_info_ = {};
  uint32_t max_transfer_blocks_;
};

}  // namespace nvme

#endif  // SRC_DEVICES_BLOCK_DRIVERS_NVME_NAMESPACE_H_
