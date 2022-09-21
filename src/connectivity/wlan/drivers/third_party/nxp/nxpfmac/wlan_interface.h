// Copyright (c) 2022 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_WLAN_INTERFACE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_WLAN_INTERFACE_H_

#include <fuchsia/hardware/wlan/fullmac/cpp/banjo.h>
#include <zircon/types.h>

#include <mutex>

#include <ddktl/device.h>

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/client_connection.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ioctl_adapter.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/mlan.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/scanner.h"

namespace wlan::nxpfmac {

class WlanInterface;
using WlanInterfaceDeviceType = ::ddk::Device<WlanInterface>;

class WlanInterface : public WlanInterfaceDeviceType,
                      public ::ddk::WlanFullmacImplProtocol<WlanInterface, ::ddk::base_protocol> {
 public:
  // Static factory function.  The returned instance is unowned, since its lifecycle is managed by
  // the devhost.
  static zx_status_t Create(zx_device_t* parent, const char* name, uint32_t iface_index,
                            wlan_mac_role_t role, async_dispatcher_t* dispatcher,
                            EventHandler* event_handler, IoctlAdapter* ioctl_adapter,
                            zx::channel&& mlme_channel, WlanInterface** out_interface);

  // Device operations.
  void DdkRelease();

  // ZX_PROTOCOL_WLAN_FULLMAC_IMPL operations.
  zx_status_t WlanFullmacImplStart(const wlan_fullmac_impl_ifc_protocol_t* ifc,
                                   zx::channel* out_mlme_channel);
  void WlanFullmacImplStop();
  void WlanFullmacImplQuery(wlan_fullmac_query_info_t* info);
  void WlanFullmacImplQueryMacSublayerSupport(mac_sublayer_support_t* resp);
  void WlanFullmacImplQuerySecuritySupport(security_support_t* resp);
  void WlanFullmacImplQuerySpectrumManagementSupport(spectrum_management_support_t* resp);
  void WlanFullmacImplStartScan(const wlan_fullmac_scan_req_t* req);
  void WlanFullmacImplConnectReq(const wlan_fullmac_connect_req_t* req);
  void WlanFullmacImplReconnectReq(const wlan_fullmac_reconnect_req_t* req);
  void WlanFullmacImplAuthResp(const wlan_fullmac_auth_resp_t* resp);
  void WlanFullmacImplDeauthReq(const wlan_fullmac_deauth_req_t* req);
  void WlanFullmacImplAssocResp(const wlan_fullmac_assoc_resp_t* resp);
  void WlanFullmacImplDisassocReq(const wlan_fullmac_disassoc_req_t* req);
  void WlanFullmacImplResetReq(const wlan_fullmac_reset_req_t* req);
  void WlanFullmacImplStartReq(const wlan_fullmac_start_req_t* req);
  void WlanFullmacImplStopReq(const wlan_fullmac_stop_req_t* req);
  void WlanFullmacImplSetKeysReq(const wlan_fullmac_set_keys_req_t* req,
                                 wlan_fullmac_set_keys_resp_t* resp);
  void WlanFullmacImplDelKeysReq(const wlan_fullmac_del_keys_req_t* req);
  void WlanFullmacImplEapolReq(const wlan_fullmac_eapol_req_t* req);
  void WlanFullmacImplStatsQueryReq();
  zx_status_t WlanFullmacImplGetIfaceCounterStats(wlan_fullmac_iface_counter_stats_t* out_stats);
  zx_status_t WlanFullmacImplGetIfaceHistogramStats(
      wlan_fullmac_iface_histogram_stats_t* out_stats);
  void WlanFullmacImplStartCaptureFrames(const wlan_fullmac_start_capture_frames_req_t* req,
                                         wlan_fullmac_start_capture_frames_resp_t* resp);
  void WlanFullmacImplStopCaptureFrames();
  zx_status_t WlanFullmacImplSetMulticastPromisc(bool enable);
  void WlanFullmacImplDataQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                                  ethernet_impl_queue_tx_callback completion_cb, void* cookie);
  void WlanFullmacImplSaeHandshakeResp(const wlan_fullmac_sae_handshake_resp_t* resp);
  void WlanFullmacImplSaeFrameTx(const wlan_fullmac_sae_frame_t* frame);
  void WlanFullmacImplWmmStatusReq();
  void WlanFullmacImplOnLinkStateChanged(bool online);

 private:
  explicit WlanInterface(zx_device_t* parent, uint32_t iface_index, wlan_mac_role_t role,
                         async_dispatcher_t* dispatcher, EventHandler* event_handler,
                         IoctlAdapter* ioctl, zx::channel&& mlme_channel);

  wlan_mac_role_t role_;
  zx::channel mlme_channel_;

  ClientConnection client_connection_;
  Scanner scanner_;

  ::ddk::WlanFullmacImplIfcProtocolClient fullmac_ifc_;

  bool is_up_ __TA_GUARDED(mutex_) = false;
  std::mutex mutex_;
};

}  // namespace wlan::nxpfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_WLAN_INTERFACE_H_
