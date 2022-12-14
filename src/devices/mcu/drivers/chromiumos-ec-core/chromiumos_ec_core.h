// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_MCU_DRIVERS_CHROMIUMOS_EC_CORE_CHROMIUMOS_EC_CORE_H_
#define SRC_DEVICES_MCU_DRIVERS_CHROMIUMOS_EC_CORE_CHROMIUMOS_EC_CORE_H_

#include <fidl/fuchsia.hardware.google.ec/cpp/wire_messaging.h>
#include <fidl/fuchsia.hardware.google.ec/cpp/wire_types.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/executor.h>
#include <lib/ddk/debug.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/svc/outgoing.h>

#include <chromiumos-platform-ec/ec_commands.h>
#include <ddktl/device.h>

#include "src/devices/lib/acpi/client.h"

namespace chromiumos_ec_core {

// Keys used for inspect nodes/properties.
inline constexpr const char* kNodeCore = "core";
inline constexpr const char* kPropVersionRo = "version-ro";
inline constexpr const char* kPropVersionRw = "version-rw";
inline constexpr const char* kPropCurrentImage = "current-image";
inline constexpr const char* kPropBuildInfo = "build-info";
inline constexpr const char* kPropChipVendor = "chip-vendor";
inline constexpr const char* kPropChipName = "chip-name";
inline constexpr const char* kPropChipRevision = "chip-revision";
inline constexpr const char* kPropBoardVersion = "board-version";
inline constexpr const char* kPropFeatures = "features";

// Board names used to bind board-specific drivers.
inline constexpr const char* kAtlasBoardName = "atlas";

namespace fcrosec = fuchsia_hardware_google_ec::wire;
struct CommandResult {
  // Status returned by the EC.
  fcrosec::EcStatus status;
  // Output struct.
  std::vector<uint8_t> data;

  template <typename Output>
  Output* GetData() {
    static_assert(!std::is_pointer<Output>::value, "Output type should not be a pointer.");
    static_assert(std::is_pod<Output>::value, "Output type should be POD.");
    if (data.size() < sizeof(Output)) {
      return nullptr;
    }
    return reinterpret_cast<Output*>(data.data());
  }
};

class ChromiumosEcCore;
using DeviceType = ddk::Device<ChromiumosEcCore, ddk::Initializable, ddk::Unbindable>;
class ChromiumosEcCore : public DeviceType,
                         public fidl::WireServer<fuchsia_hardware_acpi::NotifyHandler> {
 public:
  explicit ChromiumosEcCore(zx_device_t* parent)
      : DeviceType(parent),
        loop_(&kAsyncLoopConfigNeverAttachToThread),
        executor_(loop_.dispatcher()) {}
  virtual ~ChromiumosEcCore() = default;

  static zx_status_t Bind(void* ctx, zx_device_t* dev);
  zx_status_t Bind();
  void DdkInit(ddk::InitTxn txn);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  // FIDL NotifyHandler implementation
  void Handle(HandleRequestView request, HandleCompleter::Sync& completer);

  // For inspect test.
  inspect::Inspector& inspect() { return inspect_; }

  // Issue an EC command with the given input.
  template <typename Input>
  fpromise::promise<CommandResult, zx_status_t> IssueCommand(uint16_t command, uint8_t version,
                                                             Input& input) {
    static_assert(!std::is_pointer<Input>::value, "Input type should be a reference, not pointer.");
    static_assert(std::is_pod<Input>::value, "Input type should be POD.");
    return IssueRawCommand(command, version,
                           cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(&input), sizeof(Input)));
  }
  // Issue an EC command with no input.
  fpromise::promise<CommandResult, zx_status_t> IssueCommand(uint16_t command, uint8_t version) {
    return IssueRawCommand(command, version, cpp20::span<uint8_t>());
  }

  // Returns true if |feature| is supported by the EC.
  bool HasFeature(size_t feature);

  bool IsBoard(const std::string& board) { return version_string_rw_.rfind(board, 0) == 0; }

  async::Executor& executor() { return executor_; }
  async::Loop& loop() { return loop_; }

  fidl::WireSharedClient<fuchsia_hardware_acpi::Device>& acpi() { return acpi_client_; }

  // RAII notify handler manager. Returned by AddNotifyHandler.
  class NotifyHandlerDeleter {
   public:
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(NotifyHandlerDeleter);
    NotifyHandlerDeleter(NotifyHandlerDeleter&& other) noexcept {
      index_ = other.index_;
      ec_ = other.ec_;
      other.ec_ = nullptr;
    }
    NotifyHandlerDeleter& operator=(NotifyHandlerDeleter&& other) noexcept {
      index_ = other.index_;
      ec_ = other.ec_;
      other.ec_ = nullptr;
      return *this;
    }
    ~NotifyHandlerDeleter() {
      if (ec_) {
        ec_->RemoveNotifyHandler(index_);
      }
    }

   private:
    friend class ChromiumosEcCore;
    NotifyHandlerDeleter(ChromiumosEcCore* ec, size_t index) : ec_(ec), index_(index) {}

    ChromiumosEcCore* ec_;
    size_t index_;
  };
  using NotifyHandlerCallback = std::function<void(uint32_t event)>;
  // Add a NotifyHandler to be called when ACPI notifications occur.
  NotifyHandlerDeleter AddNotifyHandler(NotifyHandlerCallback cb)
      __TA_EXCLUDES(callback_lock_) __WARN_UNUSED_RESULT {
    std::scoped_lock lock(callback_lock_);
    ZX_ASSERT_MSG(callbacks_.size() <= std::numeric_limits<ssize_t>::max(),
                  "index %zu out of bounds", callbacks_.size());
    NotifyHandlerDeleter d(this, callbacks_.size());
    callbacks_.emplace_back(std::move(cb));
    return d;
  }

 private:
  void RemoveNotifyHandler(size_t index) __TA_EXCLUDES(callback_lock_) {
    ZX_ASSERT_MSG(index <= std::numeric_limits<ssize_t>::max(), "index out of bounds");
    std::scoped_lock lock(callback_lock_);
    callbacks_.erase(callbacks_.begin() + static_cast<ssize_t>(index));
  }
  fpromise::promise<CommandResult, zx_status_t> IssueRawCommand(uint16_t command, uint8_t version,
                                                                cpp20::span<uint8_t> input);

  // Bind the FIDL client on the async dispatcher thread, which is necessary to obey the threading
  // constraints of fidl::WireClient.
  fpromise::promise<void, zx_status_t> BindFidlClients(
      fidl::ClientEnd<fuchsia_hardware_google_ec::Device> ec_client,
      fidl::ClientEnd<fuchsia_hardware_acpi::Device> acpi_client);

  // Get available features from the EC.
  fpromise::promise<void, zx_status_t> GetFeatures();
  // Get firmware versions from the EC.
  fpromise::promise<void, zx_status_t> GetVersion();

  // Run commands for populating inspect data.
  void ScheduleInspectCommands();

  inspect::Inspector inspect_;
  inspect::Node core_ = inspect_.GetRoot().CreateChild(kNodeCore);
  async::Loop loop_;
  async::Executor executor_;
  fidl::WireSharedClient<fuchsia_hardware_acpi::Device> acpi_client_;
  fidl::WireSharedClient<fuchsia_hardware_google_ec::Device> ec_client_;
  fpromise::bridge<> acpi_teardown_;
  fpromise::bridge<> ec_teardown_;

  std::optional<fidl::ServerBindingRef<fuchsia_hardware_acpi::NotifyHandler>> notify_ref_;
  fpromise::bridge<> server_teardown_;

  std::optional<ddk::InitTxn> init_txn_;

  std::mutex callback_lock_;
  std::vector<NotifyHandlerCallback> callbacks_ __TA_GUARDED(callback_lock_);

  ec_response_get_features features_;
  std::string version_string_rw_;
};

}  // namespace chromiumos_ec_core

#endif  // SRC_DEVICES_MCU_DRIVERS_CHROMIUMOS_EC_CORE_CHROMIUMOS_EC_CORE_H_
