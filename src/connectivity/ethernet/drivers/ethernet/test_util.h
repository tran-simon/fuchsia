// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_ETHERNET_DRIVERS_ETHERNET_TEST_UTIL_H_
#define SRC_CONNECTIVITY_ETHERNET_DRIVERS_ETHERNET_TEST_UTIL_H_

#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/zx/process.h>

#include <memory>
#include <thread>

#include "ethernet.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace ethernet_testing {

class FakeEthernetImplProtocol
    : public ddk::Device<FakeEthernetImplProtocol, ddk::GetProtocolable>,
      public ddk::EthernetImplProtocol<FakeEthernetImplProtocol, ddk::base_protocol> {
 public:
  explicit FakeEthernetImplProtocol(MockDevice* parent)
      : ddk::Device<FakeEthernetImplProtocol, ddk::GetProtocolable>(parent),
        proto_({&ethernet_impl_protocol_ops_, this}) {}

  const ethernet_impl_protocol_t* proto() const { return &proto_; }

  void DdkRelease() {}

  zx_status_t EthernetImplQuery(uint32_t options, ethernet_info_t* info) {
    info->netbuf_size = sizeof(ethernet_netbuf_t);
    info->mtu = 1500;
    info->features = features_;
    memcpy(info->mac, mac_, sizeof(info->mac));
    return ZX_OK;
  }

  void EthernetImplStop() {}

  zx_status_t EthernetImplStart(const ethernet_ifc_protocol_t* ifc) {
    client_ = std::make_unique<ddk::EthernetIfcProtocolClient>(ifc);
    return ZX_OK;
  }

  void EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                           ethernet_impl_queue_tx_callback completion_cb, void* cookie) {
    queue_tx_called_ = true;
    completion_cb(cookie, ZX_OK, netbuf);
  }

  zx_status_t EthernetImplSetParam(uint32_t param, int32_t value, const uint8_t* data,
                                   size_t data_size) {
    if (param == ETHERNET_SETPARAM_DUMP_REGS) {
      dump_called_ = true;
    }
    if (param == ETHERNET_SETPARAM_PROMISC) {
      promiscuous_ = value;
    }
    if (set_param_callback_) {
      set_param_callback_();
    }
    return ZX_OK;
  }

  void EthernetImplGetBti(zx::bti* bti) { bti->reset(); }

  bool TestInfo(fuchsia_hardware_ethernet::wire::Info* info) {
    return !(memcmp(mac_, info->mac.octets.data(), ETH_MAC_SIZE) || (info->mtu != 1500));
  }

  bool TestDump() const { return dump_called_; }

  int32_t TestPromiscuous() const { return promiscuous_; }

  bool TestIfc() {
    if (!client_) {
      return false;
    }
    // Use the provided client to test the ifc client.
    client_->Status(0);
    client_->Recv(nullptr, 0, 0);
    return true;
  }

  bool SetStatus(uint32_t status) {
    if (!client_)
      return false;
    client_->Status(status);
    return true;
  }

  void SetFeatures(uint32_t features) { features_ = features; }

  bool TestQueueTx() const { return queue_tx_called_; }

  bool TestRecv() {
    if (!client_) {
      return false;
    }
    uint8_t data = 0xAA;
    client_->Recv(&data, 1, 0);
    return true;
  }

  void SetOnSetParamCallback(fit::function<void()> callback) {
    set_param_callback_ = std::move(callback);
  }

 private:
  ethernet_impl_protocol_t proto_;
  const uint8_t mac_[ETH_MAC_SIZE] = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
  std::unique_ptr<ddk::EthernetIfcProtocolClient> client_;

  bool dump_called_ = false;
  int32_t promiscuous_ = -1;
  bool queue_tx_called_ = false;
  fit::function<void()> set_param_callback_;
  uint32_t features_ = 0;
};

class EthernetTester {
 public:
  EthernetTester() {
    parent_->AddProtocol(ZX_PROTOCOL_ETHERNET_IMPL, ethernet_.proto()->ops, ethernet_.proto()->ctx);
  }

  FakeEthernetImplProtocol& ethmac() { return ethernet_; }
  eth::EthDev0* eth0() { return parent_->GetLatestChild()->GetDeviceContext<eth::EthDev0>(); }
  const std::shared_ptr<MockDevice>& parent() { return parent_; }

 private:
  const std::shared_ptr<MockDevice> parent_ = MockDevice::FakeRootParent();
  FakeEthernetImplProtocol ethernet_ = FakeEthernetImplProtocol(parent_.get());
};

}  // namespace ethernet_testing

#endif  // SRC_CONNECTIVITY_ETHERNET_DRIVERS_ETHERNET_TEST_UTIL_H_
