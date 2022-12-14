// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_VIRTUAL_LOOPBACK_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_VIRTUAL_LOOPBACK_H_

#include <fidl/fuchsia.hardware.bluetooth/cpp/wire.h>
#include <fuchsia/hardware/bt/hci/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/thread_checker.h>
#include <lib/zx/event.h>
#include <threads.h>
#include <zircon/compiler.h>
#include <zircon/device/bt-hci.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <ddktl/fidl.h>

namespace bt_hci_virtual {

// A driver that implements a ZX_PROTOCOL_BT_HCI device.
// This driver is greatly taken from the bt_transport_uart driver. The key differences are that this
// doesn't bind to service device but to a zx::channel as a virtual loopback UART device.
class LoopbackDevice;
using LoopbackDeviceType = ddk::Device<LoopbackDevice, ddk::GetProtocolable, ddk::Unbindable,
                                       ddk::Messageable<fuchsia_hardware_bluetooth::Hci>::Mixin>;

class LoopbackDevice : public LoopbackDeviceType, public ddk::BtHciProtocol<LoopbackDevice> {
 public:
  explicit LoopbackDevice(zx_device_t* parent, async_dispatcher_t* dispatcher);

  // ddk::BtHciProtocol mixins:
  zx_status_t BtHciOpenCommandChannel(zx::channel channel);
  zx_status_t BtHciOpenAclDataChannel(zx::channel channel);
  zx_status_t BtHciOpenScoChannel(zx::channel channel);
  void BtHciConfigureSco(sco_coding_format_t coding_format, sco_encoding_t encoding,
                         sco_sample_rate_t sample_rate, bt_hci_configure_sco_callback callback,
                         void* cookie);
  void BtHciResetSco(bt_hci_reset_sco_callback callback, void* cookie);
  zx_status_t BtHciOpenSnoopChannel(zx::channel channel);

  // ddk::Messageable mixins:
  void OpenCommandChannel(OpenCommandChannelRequestView request,
                          OpenCommandChannelCompleter::Sync& completer) override;
  void OpenAclDataChannel(OpenAclDataChannelRequestView request,
                          OpenAclDataChannelCompleter::Sync& completer) override;
  void OpenSnoopChannel(OpenSnoopChannelRequestView request,
                        OpenSnoopChannelCompleter::Sync& completer) override;

  // DDK Mixins
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out_proto);

  // Bind this device to this underlying virtual UART channel.
  zx_status_t Bind(zx_handle_t channel, std::string_view name) __TA_EXCLUDES(mutex_);

 private:
  // HCI UART packet indicators
  enum BtHciPacketIndicator : uint8_t {
    kHciNone = 0,
    kHciCommand = 1,
    kHciAclData = 2,
    kHciSco = 3,
    kHciEvent = 4,
  };

  struct HciWriteCtx {
    LoopbackDevice* ctx;
    // Owned.
    uint8_t* buffer;
  };

  // This wrapper around async_wait enables us to get a LoopbackDevice* in the handler.
  // We use this instead of async::WaitMethod because async::WaitBase isn't thread safe.
  struct Wait : public async_wait {
    explicit Wait(LoopbackDevice* uart, zx::channel* channel);
    static void Handler(async_dispatcher_t* dispatcher, async_wait_t* async_wait,
                        zx_status_t status, const zx_packet_signal_t* signal);
    LoopbackDevice* uart;
    // Indicates whether a wait has begun and not ended.
    bool pending = false;
    // The channel that this wait waits on.
    zx::channel* channel;
  };

  // Returns length of current event packet being received
  // Must only be called in the read callback (HciHandleUartReadEvents).
  size_t EventPacketLength();

  // Returns length of current ACL data packet being received
  // Must only be called in the read callback (HciHandleUartReadEvents).
  size_t AclPacketLength();

  // Returns length of current SCO data packet being received
  // Must only be called in the read callback (HciHandleUartReadEvents).
  size_t ScoPacketLength();

  void ChannelCleanupLocked(zx::channel* channel) __TA_REQUIRES(mutex_);

  void SnoopChannelWriteLocked(uint8_t flags, uint8_t* bytes, size_t length) __TA_REQUIRES(mutex_);

  void HciBeginShutdown() __TA_EXCLUDES(mutex_);

  void HciHandleIncomingChannel(zx::channel* chan, zx_signals_t pending) __TA_EXCLUDES(mutex_);

  void HciHandleClientChannel(zx::channel* chan, zx_signals_t pending) __TA_EXCLUDES(mutex_);

  void HciHandleUartReadEvents(const uint8_t* buf, size_t length) __TA_EXCLUDES(mutex_);

  // Reads the next packet chunk from |uart_src| into |buffer| and increments |buffer_offset| and
  // |uart_src| by the number of bytes read. If a complete packet is read, it will be written to
  // |channel|.
  using PacketLengthFunction = size_t (LoopbackDevice::*)();
  void ProcessNextUartPacketFromReadBuffer(uint8_t* buffer, size_t buffer_size,
                                           size_t* buffer_offset, const uint8_t** uart_src,
                                           const uint8_t* uart_end,
                                           PacketLengthFunction get_packet_length,
                                           zx::channel* channel, bt_hci_snoop_type_t snoop_type);

  void HciReadComplete(zx_status_t status, const uint8_t* buffer, size_t length)
      __TA_EXCLUDES(mutex_);

  void HciWriteComplete(zx_status_t status) __TA_EXCLUDES(mutex_);

  static int HciThread(void* arg) __TA_EXCLUDES(mutex_);

  void OnChannelSignal(Wait* wait, zx_status_t status, const zx_packet_signal_t* signal);

  zx_status_t HciOpenChannel(zx::channel* in_channel, zx_handle_t in) __TA_EXCLUDES(mutex_);

  // 1 byte packet indicator + 3 byte header + payload
  static constexpr uint32_t kCmdBufSize = 255 + 4;

  // The number of currently supported HCI channel endpoints. We currently have
  // one channel for command/event flow and one for ACL data flow. The sniff channel is managed
  // separately.
  static constexpr uint8_t kNumChannels = 2;

  // add one for the wakeup event
  static constexpr uint8_t kNumWaitItems = kNumChannels + 1;

  // The maximum HCI ACL frame size used for data transactions
  // (1024 + 4 bytes for the ACL header + 1 byte packet indicator)
  static constexpr uint32_t kAclMaxFrameSize = 1029;

  // The maximum HCI SCO frame size used for data transactions.
  // (255 byte payload + 3 bytes for the SCO header + 1 byte packet indicator)
  static constexpr uint32_t kScoMaxFrameSize = 259;

  // 1 byte packet indicator + 2 byte header + payload
  static constexpr uint32_t kEventBufSize = 255 + 3;

  // Backing channel for this device.
  zx::channel in_channel_ __TA_GUARDED(mutex_);
  Wait in_channel_wait_ __TA_GUARDED(mutex_){this, &in_channel_};

  // Upper channels.
  zx::channel cmd_channel_ __TA_GUARDED(mutex_);
  Wait cmd_channel_wait_ __TA_GUARDED(mutex_){this, &cmd_channel_};

  zx::channel acl_channel_ __TA_GUARDED(mutex_);
  Wait acl_channel_wait_ __TA_GUARDED(mutex_){this, &acl_channel_};

  zx::channel sco_channel_ __TA_GUARDED(mutex_);
  Wait sco_channel_wait_ __TA_GUARDED(mutex_){this, &sco_channel_};

  zx::channel snoop_channel_ __TA_GUARDED(mutex_);

  std::atomic_bool shutting_down_ = false;

  // type of current packet being read from the UART
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  BtHciPacketIndicator cur_uart_packet_type_ = kHciNone;

  // for accumulating HCI events
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  uint8_t event_buffer_[kEventBufSize];
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  size_t event_buffer_offset_ = 0;

  // for accumulating ACL data packets
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  uint8_t acl_buffer_[kAclMaxFrameSize];
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  size_t acl_buffer_offset_ = 0;

  // For accumulating SCO packets
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  uint8_t sco_buffer_[kScoMaxFrameSize];
  // Must only be used in the UART read callback (HciHandleUartReadEvents).
  size_t sco_buffer_offset_ = 0;

  // for sending outbound packets to the UART
  // kAclMaxFrameSize is the largest frame size sent.
  uint8_t write_buffer_[kAclMaxFrameSize] __TA_GUARDED(mutex_);

  std::mutex mutex_;

  std::optional<async::Loop> loop_;
  // In production, this is loop_.dispatcher(). In tests, this is the test dispatcher.
  async_dispatcher_t* dispatcher_ = nullptr;
};

}  // namespace bt_hci_virtual

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_VIRTUAL_LOOPBACK_H_
