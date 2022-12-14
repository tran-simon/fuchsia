// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_EXAMPLES_BT_LE_HEART_RATE_PERIPHERAL_APP_H_
#define SRC_CONNECTIVITY_BLUETOOTH_EXAMPLES_BT_LE_HEART_RATE_PERIPHERAL_APP_H_

#include <fuchsia/bluetooth/gatt2/cpp/fidl.h>
#include <fuchsia/bluetooth/le/cpp/fidl.h>
#include <lib/fidl/cpp/string.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>

#include "service.h"

namespace bt_le_heart_rate {

class App final {
 public:
  explicit App(std::unique_ptr<HeartModel> heart_model);
  ~App() = default;

  void StartAdvertising();

  Service* service() { return &service_; }

 private:
  static constexpr char kDeviceName[] = "FX BLE Heart Rate";

  // Server for an active advertisement.
  class AdvertisedPeripheral final : fuchsia::bluetooth::le::AdvertisedPeripheral {
   public:
    AdvertisedPeripheral(
        App* app, fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisedPeripheral> request);

    void OnConnected(fuchsia::bluetooth::le::Peer peer,
                     fidl::InterfaceHandle<fuchsia::bluetooth::le::Connection> connection,
                     OnConnectedCallback callback) override;

   private:
    App* app_;
    fidl::Binding<fuchsia::bluetooth::le::AdvertisedPeripheral> binding_;
  };

  // Application
  std::unique_ptr<sys::ComponentContext> context_;

  // Our local Heart Rate GATT service implementation.
  Service service_;

  // Proxy to the gatt.Server service.
  fuchsia::bluetooth::gatt2::ServerPtr gatt_server_;

  // Proxy to the le.Peripheral service which we use for advertising to solicit connections.
  fuchsia::bluetooth::le::PeripheralPtr peripheral_;

  // Server for active advertisement.
  std::optional<AdvertisedPeripheral> advertised_peripheral_;

  // Connection from a peer that connected to our advertisement.
  fidl::InterfacePtr<fuchsia::bluetooth::le::Connection> connection_;
};

}  // namespace bt_le_heart_rate

#endif  // SRC_CONNECTIVITY_BLUETOOTH_EXAMPLES_BT_LE_HEART_RATE_PERIPHERAL_APP_H_
