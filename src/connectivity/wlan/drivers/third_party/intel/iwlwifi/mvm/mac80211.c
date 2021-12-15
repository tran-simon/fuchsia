/******************************************************************************
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

#include <fuchsia/hardware/wlan/associnfo/c/banjo.h>
#include <fuchsia/wlan/ieee80211/c/fidl.h>
#include <string.h>
#include <zircon/status.h>

#include <ddk/hw/wlan/ieee80211/c/banjo.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/nan.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/error-dump.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-eeprom-parse.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-io.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-nvm-parse.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-op-mode.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-phy-db.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-prph.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-vendor-cmd.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/mvm.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/sta.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/time-event.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/mvm/tof.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/ieee80211.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/rcu.h"
#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dnt-cfg.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dnt-dispatch.h"
#endif
#ifdef CPTCFG_NL80211_TESTMODE
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/testmode.h"
#endif

#if 0  // NEEDS_PORTING
static const struct ieee80211_iface_limit iwl_mvm_limits[] = {
    {
        .max = CPTCFG_IWLWIFI_NUM_STA_INTERFACES,
        .types = BIT(NL80211_IFTYPE_STATION),
    },
    {
        .max = 1,
        .types =
            BIT(NL80211_IFTYPE_AP) | BIT(NL80211_IFTYPE_P2P_CLIENT) | BIT(NL80211_IFTYPE_P2P_GO),
    },
    {
        .max = 1,
        .types = BIT(NL80211_IFTYPE_P2P_DEVICE),
    },
};

static const struct ieee80211_iface_combination iwl_mvm_iface_combinations[] = {
    {
        .num_different_channels = CPTCFG_IWLWIFI_NUM_CHANNELS,
        .max_interfaces = CPTCFG_IWLWIFI_NUM_STA_INTERFACES + 2,
        .limits = iwl_mvm_limits,
        .n_limits = ARRAY_SIZE(iwl_mvm_limits),
    },
};

static const struct ieee80211_iface_limit iwl_mvm_limits_nan[] = {
    {
        .max = CPTCFG_IWLWIFI_NUM_STA_INTERFACES,
        .types = BIT(NL80211_IFTYPE_STATION),
    },
    {
        .max = 1,
        .types =
            BIT(NL80211_IFTYPE_AP) | BIT(NL80211_IFTYPE_P2P_CLIENT) | BIT(NL80211_IFTYPE_P2P_GO),
    },
    {
        .max = 1,
        .types = BIT(NL80211_IFTYPE_P2P_DEVICE),
    },
    {
        .max = 1,
        .types = BIT(NL80211_IFTYPE_NAN),
    },
};

static const struct ieee80211_iface_combination iwl_mvm_iface_combinations_nan[] = {
    {
        .num_different_channels = CPTCFG_IWLWIFI_NUM_CHANNELS,
        .max_interfaces = CPTCFG_IWLWIFI_NUM_STA_INTERFACES + 3,
        .limits = iwl_mvm_limits_nan,
        .n_limits = ARRAY_SIZE(iwl_mvm_limits_nan),
    },
};

#ifdef CPTCFG_IWLWIFI_BCAST_FILTERING
/*
 * Use the reserved field to indicate magic values.
 * these values will only be used internally by the driver,
 * and won't make it to the fw (reserved will be 0).
 * BC_FILTER_MAGIC_IP - configure the val of this attribute to
 *  be the vif's ip address. in case there is not a single
 *  ip address (0, or more than 1), this attribute will
 *  be skipped.
 * BC_FILTER_MAGIC_MAC - set the val of this attribute to
 *  the LSB bytes of the vif's mac address
 */
enum {
    BC_FILTER_MAGIC_NONE = 0,
    BC_FILTER_MAGIC_IP,
    BC_FILTER_MAGIC_MAC,
};

static const struct iwl_fw_bcast_filter iwl_mvm_default_bcast_filters[] = {
    {
        /* arp */
        .discard = 0,
        .frame_type = BCAST_FILTER_FRAME_TYPE_ALL,
        .attrs =
            {
                {
                    /* frame type - arp, hw type - ethernet */
                    .offset_type = BCAST_FILTER_OFFSET_PAYLOAD_START,
                    .offset = sizeof(rfc1042_header),
                    .val = cpu_to_be32(0x08060001),
                    .mask = cpu_to_be32(0xffffffff),
                },
                {
                    /* arp dest ip */
                    .offset_type = BCAST_FILTER_OFFSET_PAYLOAD_START,
                    .offset = sizeof(rfc1042_header) + 2 + sizeof(struct arphdr) + ETH_ALEN +
                              sizeof(__be32) + ETH_ALEN,
                    .mask = cpu_to_be32(0xffffffff),
                    /* mark it as special field */
                    .reserved1 = cpu_to_le16(BC_FILTER_MAGIC_IP),
                },
            },
    },
    {
        /* dhcp offer bcast */
        .discard = 0,
        .frame_type = BCAST_FILTER_FRAME_TYPE_IPV4,
        .attrs =
            {
                {
                    /* udp dest port - 68 (bootp client)*/
                    .offset_type = BCAST_FILTER_OFFSET_IP_END,
                    .offset = offsetof(struct udphdr, dest),
                    .val = cpu_to_be32(0x00440000),
                    .mask = cpu_to_be32(0xffff0000),
                },
                {
                    /* dhcp - lsb bytes of client hw address */
                    .offset_type = BCAST_FILTER_OFFSET_IP_END,
                    .offset = 38,
                    .mask = cpu_to_be32(0xffffffff),
                    /* mark it as special field */
                    .reserved1 = cpu_to_le16(BC_FILTER_MAGIC_MAC),
                },
            },
    },
    /* last filter must be empty */
    {},
};
#endif
#endif  // NEEDS_PORTING

void iwl_mvm_ref(struct iwl_mvm* mvm, enum iwl_mvm_ref_type ref_type) {
  if (!iwl_mvm_is_d0i3_supported(mvm)) {
    return;
  }

  IWL_DEBUG_RPM(mvm, "Take mvm reference - type %d\n", ref_type);
  mtx_lock(&mvm->refs_lock);
  mvm->refs[ref_type]++;
  mtx_unlock(&mvm->refs_lock);
  iwl_trans_ref(mvm->trans);
}

void iwl_mvm_unref(struct iwl_mvm* mvm, enum iwl_mvm_ref_type ref_type) {
  if (!iwl_mvm_is_d0i3_supported(mvm)) {
    return;
  }

  IWL_DEBUG_RPM(mvm, "Leave mvm reference - type %d\n", ref_type);
  mtx_lock(&mvm->refs_lock);
  if (WARN_ON(!mvm->refs[ref_type])) {
    mtx_unlock(&mvm->refs_lock);
    return;
  }
  mvm->refs[ref_type]--;
  mtx_unlock(&mvm->refs_lock);
  iwl_trans_unref(mvm->trans);
}

static void iwl_mvm_unref_all_except(struct iwl_mvm* mvm, enum iwl_mvm_ref_type except_ref) {
  enum iwl_mvm_ref_type i, j;

  if (!iwl_mvm_is_d0i3_supported(mvm)) {
    return;
  }

  mtx_lock(&mvm->refs_lock);
  for (i = 0; i < IWL_MVM_REF_COUNT; i++) {
    if (except_ref == i || !mvm->refs[i]) {
      continue;
    }

    IWL_DEBUG_RPM(mvm, "Cleanup: remove mvm ref type %d (%d)\n", i, mvm->refs[i]);
    for (j = 0; j < mvm->refs[i]; j++) {
      iwl_trans_unref(mvm->trans);
    }
    mvm->refs[i] = 0;
  }
  mtx_unlock(&mvm->refs_lock);
}

bool iwl_mvm_ref_taken(struct iwl_mvm* mvm) {
  int i;
  bool taken = false;

  if (!iwl_mvm_is_d0i3_supported(mvm)) {
    return true;
  }

  mtx_lock(&mvm->refs_lock);
  for (i = 0; i < IWL_MVM_REF_COUNT; i++) {
    if (mvm->refs[i]) {
      taken = true;
      break;
    }
  }
  mtx_unlock(&mvm->refs_lock);

  return taken;
}

zx_status_t iwl_mvm_ref_sync(struct iwl_mvm* mvm, enum iwl_mvm_ref_type ref_type) {
  iwl_mvm_ref(mvm, ref_type);

#if 0   // NEEDS_PORTING
    if (!wait_event_timeout(mvm->d0i3_exit_waitq, !test_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status),
                            HZ)) {
        WARN_ON_ONCE(1);
        iwl_mvm_unref(mvm, ref_type);
        return -EIO;
    }
#endif  // NEEDS_PORTING

  return ZX_OK;
}

static void iwl_mvm_reset_phy_ctxts(struct iwl_mvm* mvm) {
  int i;

  memset(mvm->phy_ctxts, 0, sizeof(mvm->phy_ctxts));
  for (i = 0; i < NUM_PHY_CTX; i++) {
    mvm->phy_ctxts[i].id = i;
    mvm->phy_ctxts[i].ref = 0;
  }
}

#if 0  // NEEDS_PORTING
struct ieee80211_regdomain* iwl_mvm_get_regdomain(struct wiphy* wiphy, const char* alpha2,
                                                  enum iwl_mcc_source src_id, bool* changed) {
    struct ieee80211_regdomain* regd = NULL;
    struct ieee80211_hw* hw = wiphy_to_ieee80211_hw(wiphy);
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mcc_update_resp* resp;

    IWL_DEBUG_LAR(mvm, "Getting regdomain data for %s from FW\n", alpha2);

    iwl_assert_lock_held(&mvm->mutex);

    resp = iwl_mvm_update_mcc(mvm, alpha2, src_id);
    if (IS_ERR_OR_NULL(resp)) {
        IWL_DEBUG_LAR(mvm, "Could not get update from FW %d\n", PTR_ERR_OR_ZERO(resp));
        goto out;
    }

    if (changed) {
        uint32_t status = le32_to_cpu(resp->status);

        *changed = (status == MCC_RESP_NEW_CHAN_PROFILE || status == MCC_RESP_ILLEGAL);
    }

    regd = iwl_parse_nvm_mcc_info(mvm->trans->dev, mvm->cfg, __le32_to_cpu(resp->n_channels),
                                  resp->channels, __le16_to_cpu(resp->mcc),
                                  __le16_to_cpu(resp->geo_info));
    /* Store the return source id */
    src_id = resp->source_id;
    kfree(resp);
    if (IS_ERR_OR_NULL(regd)) {
        IWL_DEBUG_LAR(mvm, "Could not get parse update from FW %d\n", PTR_ERR_OR_ZERO(regd));
        goto out;
    }

    IWL_DEBUG_LAR(mvm, "setting alpha2 from FW to %s (0x%x, 0x%x) src=%d\n", regd->alpha2,
                  regd->alpha2[0], regd->alpha2[1], src_id);
    mvm->lar_regdom_set = true;
    mvm->mcc_src = src_id;

out:
    return regd;
}

void iwl_mvm_update_changed_regdom(struct iwl_mvm* mvm) {
    bool changed;
    struct ieee80211_regdomain* regd;

    if (!iwl_mvm_is_lar_supported(mvm)) { return; }

    regd = iwl_mvm_get_current_regdomain(mvm, &changed);
    if (!IS_ERR_OR_NULL(regd)) {
        /* only update the regulatory core if changed */
        if (changed) { regulatory_set_wiphy_regd(mvm->hw->wiphy, regd); }

        kfree(regd);
    }
}

struct ieee80211_regdomain* iwl_mvm_get_current_regdomain(struct iwl_mvm* mvm, bool* changed) {
    return iwl_mvm_get_regdomain(
        mvm->hw->wiphy, "ZZ",
        iwl_mvm_is_wifi_mcc_supported(mvm) ? MCC_SOURCE_GET_CURRENT : MCC_SOURCE_OLD_FW, changed);
}

int iwl_mvm_init_fw_regd(struct iwl_mvm* mvm) {
    enum iwl_mcc_source used_src;
    struct ieee80211_regdomain* regd;
    int ret;
    bool changed;
    const struct ieee80211_regdomain* r = rtnl_dereference(mvm->hw->wiphy->regd);

    if (!r) { return -ENOENT; }

    /* save the last source in case we overwrite it below */
    used_src = mvm->mcc_src;
    if (iwl_mvm_is_wifi_mcc_supported(mvm)) {
        /* Notify the firmware we support wifi location updates */
        regd = iwl_mvm_get_current_regdomain(mvm, NULL);
        if (!IS_ERR_OR_NULL(regd)) { kfree(regd); }
    }

    /* Now set our last stored MCC and source */
    regd = iwl_mvm_get_regdomain(mvm->hw->wiphy, r->alpha2, used_src, &changed);
    if (IS_ERR_OR_NULL(regd)) { return -EIO; }

    /* update cfg80211 if the regdomain was changed */
    if (changed) {
        ret = regulatory_set_wiphy_regd_sync_rtnl(mvm->hw->wiphy, regd);
    } else {
        ret = 0;
    }

    kfree(regd);
    return ret;
}

const static uint8_t he_if_types_ext_capa_sta[] = {
    [0] = WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING,
    [7] = WLAN_EXT_CAPA8_OPMODE_NOTIF,
    [9] = WLAN_EXT_CAPA10_TWT_REQUESTER_SUPPORT,
};

#ifdef CPTCFG_IWLMVM_AX_SOFTAP_TESTMODE
const static uint8_t he_if_types_ext_capa_ap[] = {
    [0] = WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING,
    [7] = WLAN_EXT_CAPA8_OPMODE_NOTIF,
    [9] = WLAN_EXT_CAPA10_TWT_RESPONDER_SUPPORT,
};
#endif /* CPTCFG_IWLMVM_AX_SOFTAP_TESTMODE */

const static struct wiphy_iftype_ext_capab he_iftypes_ext_capa[] = {
    {
        .iftype = NL80211_IFTYPE_STATION,
        .extended_capabilities = he_if_types_ext_capa_sta,
        .extended_capabilities_mask = he_if_types_ext_capa_sta,
        .extended_capabilities_len = sizeof(he_if_types_ext_capa_sta),
    },
#ifdef CPTCFG_IWLMVM_AX_SOFTAP_TESTMODE
    {
        .iftype = NL80211_IFTYPE_AP,
        .extended_capabilities = he_if_types_ext_capa_ap,
        .extended_capabilities_mask = he_if_types_ext_capa_ap,
        .extended_capabilities_len = sizeof(he_if_types_ext_capa_ap),
    },
#endif  /* CPTCFG_IWLMVM_AX_SOFTAP_TESTMODE */
};
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_mac_setup_register(struct iwl_mvm* mvm) {
#if 0   // NEEDS_PORTING: for cipher
    BUILD_BUG_ON(ARRAY_SIZE(mvm->ciphers) < ARRAY_SIZE(mvm_ciphers) + 6);
    memcpy(mvm->ciphers, mvm_ciphers, sizeof(mvm_ciphers));
    hw->wiphy->n_cipher_suites = ARRAY_SIZE(mvm_ciphers);
    hw->wiphy->cipher_suites = mvm->ciphers;

    if (iwl_mvm_has_new_rx_api(mvm)) {
        mvm->ciphers[hw->wiphy->n_cipher_suites] = WLAN_CIPHER_SUITE_GCMP;
        hw->wiphy->n_cipher_suites++;
        mvm->ciphers[hw->wiphy->n_cipher_suites] = WLAN_CIPHER_SUITE_GCMP_256;
        hw->wiphy->n_cipher_suites++;
    }

    /* Enable 11w if software crypto is not enabled (as the
     * firmware will interpret some mgmt packets, so enabling it
     * with software crypto isn't safe).
     */
    if (!iwlwifi_mod_params.swcrypto) {
        ieee80211_hw_set(hw, MFP_CAPABLE);
        mvm->ciphers[hw->wiphy->n_cipher_suites] = WLAN_CIPHER_SUITE_AES_CMAC;
        hw->wiphy->n_cipher_suites++;
        if (iwl_mvm_has_new_rx_api(mvm)) {
            mvm->ciphers[hw->wiphy->n_cipher_suites] = WLAN_CIPHER_SUITE_BIP_GMAC_128;
            hw->wiphy->n_cipher_suites++;
            mvm->ciphers[hw->wiphy->n_cipher_suites] = WLAN_CIPHER_SUITE_BIP_GMAC_256;
            hw->wiphy->n_cipher_suites++;
        }
    }

    /* currently FW API supports only one optional cipher scheme */
    if (mvm->fw->cs[0].cipher) {
        const struct iwl_fw_cipher_scheme* fwcs = &mvm->fw->cs[0];
        struct ieee80211_cipher_scheme* cs = &mvm->cs[0];

        mvm->hw->n_cipher_schemes = 1;

        cs->cipher = le32_to_cpu(fwcs->cipher);
        cs->iftype = BIT(NL80211_IFTYPE_STATION);
        cs->hdr_len = fwcs->hdr_len;
        cs->pn_len = fwcs->pn_len;
        cs->pn_off = fwcs->pn_off;
        cs->key_idx_off = fwcs->key_idx_off;
        cs->key_idx_mask = fwcs->key_idx_mask;
        cs->key_idx_shift = fwcs->key_idx_shift;
        cs->mic_len = fwcs->mic_len;

        mvm->hw->cipher_schemes = mvm->cs;
        mvm->ciphers[hw->wiphy->n_cipher_suites] = cs->cipher;
        hw->wiphy->n_cipher_suites++;
    }
#endif  // NEEDS_PORTING

#if 0  // TODO(fxbug.dev/36682): We need nvm.c porting iwl_nvm_init()
    /* Extract MAC address */
    memcpy(mvm->addresses[0].addr, mvm->nvm_data->hw_addr, ETH_ALEN);

    /* Extract additional MAC addresses if available */
    size_t num_mac =
        (mvm->nvm_data->n_hw_addrs > 1) ? MIN(IWL_MVM_MAX_ADDRESSES, mvm->nvm_data->n_hw_addrs) : 1;

    for (size_t i = 1; i < num_mac; i++) {
        memcpy(mvm->addresses[i].addr, mvm->addresses[i - 1].addr, ETH_ALEN);
        mvm->addresses[i].addr[5]++;
    }
#endif

  iwl_mvm_reset_phy_ctxts(mvm);

  BUILD_BUG_ON(IWL_MVM_SCAN_STOPPING_MASK & IWL_MVM_SCAN_MASK);
  BUILD_BUG_ON(IWL_MVM_MAX_UMAC_SCANS > IWL_MVM_SCAN_MASK_HWEIGHT32 ||
               IWL_MVM_MAX_LMAC_SCANS > IWL_MVM_SCAN_MASK_HWEIGHT32);

  if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_UMAC_SCAN)) {
    mvm->max_scans = IWL_MVM_MAX_UMAC_SCANS;
  } else {
    mvm->max_scans = IWL_MVM_MAX_LMAC_SCANS;
  }

  mvm->rts_threshold = IEEE80211_MAX_RTS_THRESHOLD;

#ifdef CONFIG_PM_SLEEP
  if (iwl_mvm_is_d0i3_supported(mvm) && device_can_wakeup(mvm->trans->dev)) {
    mvm->wowlan.flags = WIPHY_WOWLAN_ANY;
    hw->wiphy->wowlan = &mvm->wowlan;
  }

  if (mvm->fw->img[IWL_UCODE_WOWLAN].num_sec && mvm->trans->ops->d3_suspend &&
      mvm->trans->ops->d3_resume && device_can_wakeup(mvm->trans->dev)) {
    mvm->wowlan.flags |= WIPHY_WOWLAN_MAGIC_PKT | WIPHY_WOWLAN_DISCONNECT |
                         WIPHY_WOWLAN_EAP_IDENTITY_REQ | WIPHY_WOWLAN_RFKILL_RELEASE |
                         WIPHY_WOWLAN_NET_DETECT;
    if (!iwlwifi_mod_params.swcrypto)
      mvm->wowlan.flags |= WIPHY_WOWLAN_SUPPORTS_GTK_REKEY | WIPHY_WOWLAN_GTK_REKEY_FAILURE |
                           WIPHY_WOWLAN_4WAY_HANDSHAKE;

    mvm->wowlan.n_patterns = IWL_WOWLAN_MAX_PATTERNS;
    mvm->wowlan.pattern_min_len = IWL_WOWLAN_MIN_PATTERN_LEN;
    mvm->wowlan.pattern_max_len = IWL_WOWLAN_MAX_PATTERN_LEN;
    mvm->wowlan.max_nd_match_sets = IWL_SCAN_MAX_PROFILES;
    hw->wiphy->wowlan = &mvm->wowlan;
  }
#endif

#ifdef CPTCFG_IWLWIFI_BCAST_FILTERING
  /* assign default bcast filtering configuration */
  mvm->bcast_filters = iwl_mvm_default_bcast_filters;
#endif

#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
  iwl_mvm_set_wiphy_vendor_commands(hw->wiphy);
#endif

  zx_status_t ret = iwl_mvm_leds_init(mvm);
  if (ret) {
    return ret;
  }

  mvm->init_status |= IWL_MVM_INIT_STATUS_REG_HW_INIT_COMPLETE;

  return ZX_OK;
}

#if 0   // NEEDS_PORTING
static bool iwl_mvm_defer_tx(struct iwl_mvm* mvm, struct ieee80211_sta* sta, struct sk_buff* skb) {
    struct iwl_mvm_sta* mvmsta;
    bool defer = false;

    /*
     * double check the IN_D0I3 flag both before and after
     * taking the spinlock, in order to prevent taking
     * the spinlock when not needed.
     */
    if (likely(!test_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status))) { return false; }

    spin_lock(&mvm->d0i3_tx_lock);
    /*
     * testing the flag again ensures the skb dequeue
     * loop (on d0i3 exit) hasn't run yet.
     */
    if (!test_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status)) { goto out; }

    mvmsta = iwl_mvm_sta_from_mac80211(sta);
    if (mvmsta->sta_id == IWL_MVM_INVALID_STA || mvmsta->sta_id != mvm->d0i3_ap_sta_id) {
        goto out;
    }

    __skb_queue_tail(&mvm->d0i3_tx, skb);

    /* trigger wakeup */
    iwl_mvm_ref(mvm, IWL_MVM_REF_TX);
    iwl_mvm_unref(mvm, IWL_MVM_REF_TX);

    defer = true;
out:
    spin_unlock(&mvm->d0i3_tx_lock);
    return defer;
}
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_mac_tx(struct iwl_mvm_vif* mvmvif, struct ieee80211_mac_packet* pkt) {
  iwl_assert_lock_held(&mvmvif->mvm->mutex);

  if (mvmvif->mac_role != WLAN_INFO_MAC_ROLE_CLIENT) {
    IWL_ERR(mvmvif, "%s(): not supported MAC role %d yet\n", __func__, mvmvif->mac_role);
    return ZX_ERR_INVALID_ARGS;
  }

  struct iwl_mvm_sta* mvmsta = mvmvif->mvm->fw_id_to_mac_id[mvmvif->ap_sta_id];
  if (!mvmsta) {
    IWL_ERR(mvmvif, "%s(): mvmsta is NULL. mvmvif->ap_sta_id=%d\n", __func__, mvmvif->ap_sta_id);
    return ZX_ERR_INTERNAL;
  }

  return iwl_mvm_tx_skb(mvmvif->mvm, pkt, mvmsta);

#if 0   // NEEDS_PORTING
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct ieee80211_sta* sta = control->sta;
    struct ieee80211_tx_info* info = IEEE80211_SKB_CB(skb);
    struct ieee80211_hdr* hdr = (void*)skb->data;
    bool offchannel = IEEE80211_SKB_CB(skb)->flags & IEEE80211_TX_CTL_TX_OFFCHAN;

    if (iwl_mvm_is_radio_killed(mvm)) {
        IWL_DEBUG_DROP(mvm, "Dropping - RF/CT KILL\n");
        goto drop;
    }

    if (offchannel && !test_bit(IWL_MVM_STATUS_ROC_RUNNING, &mvm->status) &&
        !test_bit(IWL_MVM_STATUS_ROC_AUX_RUNNING, &mvm->status)) {
        goto drop;
    }

    /* treat non-bufferable MMPDUs on AP interfaces as broadcast */
    if ((info->control.vif->type == NL80211_IFTYPE_AP ||
         info->control.vif->type == NL80211_IFTYPE_ADHOC) &&
        ieee80211_is_mgmt(hdr->frame_control) &&
        !ieee80211_is_bufferable_mmpdu(hdr->frame_control)) {
        sta = NULL;
    }

    /* If there is no sta, and it's not offchannel - send through AP */
    if (!sta && info->control.vif->type == NL80211_IFTYPE_STATION && !offchannel) {
        struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(info->control.vif);
        uint8_t ap_sta_id = READ_ONCE(mvmvif->ap_sta_id);

        if (ap_sta_id < IWL_MVM_STATION_COUNT) {
            /* mac80211 holds rcu read lock */
            sta = rcu_dereference(mvm->fw_id_to_mac_id[ap_sta_id]);
            if (IS_ERR_OR_NULL(sta)) { goto drop; }
        }
    }

    if (sta) {
        if (iwl_mvm_defer_tx(mvm, sta, skb)) { return; }
        if (iwl_mvm_tx_skb(mvm, skb, sta)) { goto drop; }
        return;
    }

    if (iwl_mvm_tx_skb_non_sta(mvm, skb)) { goto drop; }
    return;
drop:
    ieee80211_free_txskb(hw, skb);
#endif  // NEEDS_PORTING
}

void iwl_mvm_mac_itxq_xmit(struct ieee80211_hw* hw, struct ieee80211_txq* txq) {
#if 0   // NEEDS_PORTING
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_txq* mvmtxq = iwl_mvm_txq_from_mac80211(txq);
    struct sk_buff* skb = NULL;

    spin_lock(&mvmtxq->tx_path_lock);

    rcu_read_lock();
    while (likely(!mvmtxq->stopped && (mvm->trans->system_pm_mode == IWL_PLAT_PM_MODE_DISABLED))) {
        skb = ieee80211_tx_dequeue(hw, txq);

        if (!skb) { break; }

        if (!txq->sta) {
            iwl_mvm_tx_skb_non_sta(mvm, skb);
        } else {
            iwl_mvm_tx_skb(mvm, skb, txq->sta);
        }
    }
    rcu_read_unlock();

    spin_unlock(&mvmtxq->tx_path_lock);
#endif  // NEEDS_PORTING
}

#if 0  // NEEDS_PORTING
static void iwl_mvm_mac_wake_tx_queue(struct ieee80211_hw* hw, struct ieee80211_txq* txq) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_txq* mvmtxq = iwl_mvm_txq_from_mac80211(txq);

    /*
     * Please note that racing is handled very carefully here:
     * mvmtxq->txq_id is updated during allocation, and mvmtxq->list is
     * deleted afterwards.
     * This means that if:
     * mvmtxq->txq_id != INVALID_QUEUE && list_empty(&mvmtxq->list):
     *  queue is allocated and we can TX.
     * mvmtxq->txq_id != INVALID_QUEUE && !list_empty(&mvmtxq->list):
     *  a race, should defer the frame.
     * mvmtxq->txq_id == INVALID_QUEUE && list_empty(&mvmtxq->list):
     *  need to allocate the queue and defer the frame.
     * mvmtxq->txq_id == INVALID_QUEUE && !list_empty(&mvmtxq->list):
     *  queue is already scheduled for allocation, no need to allocate,
     *  should defer the frame.
     */

    /* If the queue is allocated TX and return. */
    if (!txq->sta || mvmtxq->txq_id != IWL_MVM_INVALID_QUEUE) {
        /*
         * Check that list is empty to avoid a race where txq_id is
         * already updated, but the queue allocation work wasn't
         * finished
         */
        if (unlikely(txq->sta && !list_empty(&mvmtxq->list))) { return; }

        iwl_mvm_mac_itxq_xmit(hw, txq);
        return;
    }

    /* The list is being deleted only after the queue is fully allocated. */
    if (!list_empty(&mvmtxq->list)) { return; }

    list_add_tail(&mvmtxq->list, &mvm->add_stream_txqs);
    schedule_work(&mvm->add_stream_wk);
}

static inline bool iwl_enable_rx_ampdu(const struct iwl_cfg* cfg) {
    if (iwlwifi_mod_params.disable_11n & IWL_DISABLE_HT_RXAGG) { return false; }
    return true;
}

static inline bool iwl_enable_tx_ampdu(const struct iwl_cfg* cfg) {
    if (iwlwifi_mod_params.disable_11n & IWL_DISABLE_HT_TXAGG) { return false; }
    if (iwlwifi_mod_params.disable_11n & IWL_ENABLE_HT_TXAGG) { return true; }

    /* enabled by default */
    return true;
}

#define CHECK_BA_TRIGGER(_mvm, _trig, _tid_bm, _tid, _fmt...) \
  do {                                                        \
    if (!(le16_to_cpu(_tid_bm) & BIT(_tid)))                  \
      break;                                                  \
    iwl_fw_dbg_collect_trig(&(_mvm)->fwrt, _trig, _fmt);      \
  } while (0)

static void iwl_mvm_ampdu_check_trigger(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                        struct ieee80211_sta* sta, uint16_t tid, uint16_t rx_ba_ssn,
                                        enum ieee80211_ampdu_mlme_action action) {
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct iwl_fw_dbg_trigger_ba* ba_trig;

    trig = iwl_fw_dbg_trigger_on(&mvm->fwrt, ieee80211_vif_to_wdev(vif), FW_DBG_TRIGGER_BA);
    if (!trig) { return; }

    ba_trig = (void*)trig->data;

    switch (action) {
    case IEEE80211_AMPDU_TX_OPERATIONAL: {
        struct iwl_mvm_sta* mvmsta = iwl_mvm_sta_from_mac80211(sta);
        struct iwl_mvm_tid_data* tid_data = &mvmsta->tid_data[tid];

        CHECK_BA_TRIGGER(mvm, trig, ba_trig->tx_ba_start, tid,
                         "TX AGG START: MAC %pM tid %d ssn %d\n", sta->addr, tid, tid_data->ssn);
        break;
    }
    case IEEE80211_AMPDU_TX_STOP_CONT:
        CHECK_BA_TRIGGER(mvm, trig, ba_trig->tx_ba_stop, tid, "TX AGG STOP: MAC %pM tid %d\n",
                         sta->addr, tid);
        break;
    case IEEE80211_AMPDU_RX_START:
        CHECK_BA_TRIGGER(mvm, trig, ba_trig->rx_ba_start, tid,
                         "RX AGG START: MAC %pM tid %d ssn %d\n", sta->addr, tid, rx_ba_ssn);
        break;
    case IEEE80211_AMPDU_RX_STOP:
        CHECK_BA_TRIGGER(mvm, trig, ba_trig->rx_ba_stop, tid, "RX AGG STOP: MAC %pM tid %d\n",
                         sta->addr, tid);
        break;
    default:
        break;
    }
}

static int iwl_mvm_mac_ampdu_action(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                    struct ieee80211_ampdu_params* params) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int ret;
    bool tx_agg_ref = false;
    struct ieee80211_sta* sta = params->sta;
    enum ieee80211_ampdu_mlme_action action = params->action;
    uint16_t tid = params->tid;
    uint16_t* ssn = &params->ssn;
    uint16_t buf_size = params->buf_size;
    bool amsdu = params->amsdu;
    uint16_t timeout = params->timeout;

    IWL_DEBUG_HT(mvm, "A-MPDU action on addr %pM tid %d: action %d\n", sta->addr, tid, action);

    if (!(mvm->nvm_data->sku_cap_11n_enable)) { return -EACCES; }

    /* return from D0i3 before starting a new Tx aggregation */
    switch (action) {
    case IEEE80211_AMPDU_TX_START:
    case IEEE80211_AMPDU_TX_STOP_CONT:
    case IEEE80211_AMPDU_TX_STOP_FLUSH:
    case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
    case IEEE80211_AMPDU_TX_OPERATIONAL:
        /*
         * for tx start, wait synchronously until D0i3 exit to
         * get the correct sequence number for the tid.
         * additionally, some other ampdu actions use direct
         * target access, which is not handled automatically
         * by the trans layer (unlike commands), so wait for
         * d0i3 exit in these cases as well.
         */
        ret = iwl_mvm_ref_sync(mvm, IWL_MVM_REF_TX_AGG);
        if (ret) { return ret; }

        tx_agg_ref = true;
        break;
    default:
        break;
    }

    mutex_lock(&mvm->mutex);

    switch (action) {
    case IEEE80211_AMPDU_RX_START:
        if (iwl_mvm_vif_from_mac80211(vif)->ap_sta_id == iwl_mvm_sta_from_mac80211(sta)->sta_id) {
            struct iwl_mvm_vif* mvmvif;
            uint16_t macid = iwl_mvm_vif_from_mac80211(vif)->id;
            struct iwl_mvm_tcm_mac* mdata = &mvm->tcm.data[macid];

            mdata->opened_rx_ba_sessions = true;
            mvmvif = iwl_mvm_vif_from_mac80211(vif);
            cancel_delayed_work(&mvmvif->uapsd_nonagg_detected_wk);
        }
        if (!iwl_enable_rx_ampdu(mvm->cfg)) {
            ret = -EINVAL;
            break;
        }
        ret = iwl_mvm_sta_rx_agg(mvm, sta, tid, *ssn, true, buf_size, timeout);
        break;
    case IEEE80211_AMPDU_RX_STOP:
        ret = iwl_mvm_sta_rx_agg(mvm, sta, tid, 0, false, buf_size, timeout);
        break;
    case IEEE80211_AMPDU_TX_START:
        if (!iwl_enable_tx_ampdu(mvm->cfg)) {
            ret = -EINVAL;
            break;
        }
        ret = iwl_mvm_sta_tx_agg_start(mvm, vif, sta, tid, ssn);
        break;
    case IEEE80211_AMPDU_TX_STOP_CONT:
        ret = iwl_mvm_sta_tx_agg_stop(mvm, vif, sta, tid);
        break;
    case IEEE80211_AMPDU_TX_STOP_FLUSH:
    case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
        ret = iwl_mvm_sta_tx_agg_flush(mvm, vif, sta, tid);
        break;
    case IEEE80211_AMPDU_TX_OPERATIONAL:
        ret = iwl_mvm_sta_tx_agg_oper(mvm, vif, sta, tid, buf_size, amsdu);
        break;
    default:
        WARN_ON_ONCE(1);
        ret = -EINVAL;
        break;
    }

    if (!ret) {
        uint16_t rx_ba_ssn = 0;

        if (action == IEEE80211_AMPDU_RX_START) { rx_ba_ssn = *ssn; }

        iwl_mvm_ampdu_check_trigger(mvm, vif, sta, tid, rx_ba_ssn, action);
    }
    mutex_unlock(&mvm->mutex);

    /*
     * If the tid is marked as started, we won't use it for offloaded
     * traffic on the next D0i3 entry. It's safe to unref.
     */
    if (tx_agg_ref) { iwl_mvm_unref(mvm, IWL_MVM_REF_TX_AGG); }

    return ret;
}

static void iwl_mvm_cleanup_iterator(void* data, uint8_t* mac, struct ieee80211_vif* vif) {
    struct iwl_mvm* mvm = data;
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);

    mvmvif->uploaded = false;
    mvmvif->ap_sta_id = IWL_MVM_INVALID_STA;

    spin_lock_bh(&mvm->time_event_lock);
    iwl_mvm_te_clear_data(mvm, &mvmvif->time_event_data);
    spin_unlock_bh(&mvm->time_event_lock);

    mvmvif->phy_ctxt = NULL;
    memset(&mvmvif->bf_data, 0, sizeof(mvmvif->bf_data));
    memset(&mvmvif->probe_resp_data, 0, sizeof(mvmvif->probe_resp_data));
}
#endif  // NEEDS_PORTING

static void iwl_mvm_restart_cleanup(struct iwl_mvm* mvm) {
#if 0  // NEEDS_PORTING
    /* clear the D3 reconfig, we only need it to avoid dumping a
     * firmware coredump on reconfiguration, we shouldn't do that
     * on D3->D0 transition
     */
    if (!test_and_clear_bit(IWL_MVM_STATUS_D3_RECONFIG, &mvm->status)) {
        mvm->fwrt.dump.desc = &iwl_dump_desc_assert;
        iwl_fw_error_dump(&mvm->fwrt);

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
        iwl_dnt_dispatch_handle_nic_err(mvm->trans);
#endif
    }
#endif  // NEEDS_PORTING

  /* cleanup all stale references (scan, roc), but keep the
   * ucode_down ref until reconfig is complete
   */
  iwl_mvm_unref_all_except(mvm, IWL_MVM_REF_UCODE_DOWN);

  iwl_mvm_stop_device(mvm);

  mvm->scan_status = 0;
  mvm->ps_disabled = false;
  mvm->calibrating = false;

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
  /*
   * In case that 2g coex was enabled - and now the FW is being
   * restarted, we need to disable 2g coex mode in the driver as well
   * so that the fw & driver will be synced on the mode.
   */
  mvm->coex_2g_enabled = false;
#endif

#if 0   // NEEDS_PORTING
    /* just in case one was running */
    iwl_mvm_cleanup_roc_te(mvm);
    ieee80211_remain_on_channel_expired(mvm->hw);
#endif  // NEEDS_PORTING

#if 0   // NEEDS_PORTING
    /*
     * cleanup all interfaces, even inactive ones, as some might have
     * gone down during the HW restart
     */
    ieee80211_iterate_interfaces(mvm->hw, 0, iwl_mvm_cleanup_iterator, mvm);
#endif  // NEEDS_PORTING

  mvm->p2p_device_vif = NULL;
  mvm->d0i3_ap_sta_id = IWL_MVM_INVALID_STA;

  iwl_mvm_reset_phy_ctxts(mvm);
  memset(mvm->fw_key_table, 0, sizeof(mvm->fw_key_table));
  memset(&mvm->last_bt_notif, 0, sizeof(mvm->last_bt_notif));
  memset(&mvm->last_bt_ci_cmd, 0, sizeof(mvm->last_bt_ci_cmd));

#if 0   // NEEDS_PORTING
    ieee80211_wake_queues(mvm->hw);
#endif  // NEEDS_PORTING

  /* clear any stale d0i3 state */
  clear_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status);

  mvm->vif_count = 0;
  mvm->rx_ba_sessions = 0;
  mvm->fwrt.dump.conf = FW_DBG_INVALID;
  mvm->monitor_on = false;

  /* keep statistics ticking */
  iwl_mvm_accu_radio_stats(mvm);
}

zx_status_t __iwl_mvm_mac_start(struct iwl_mvm* mvm) {
  zx_status_t ret;

  iwl_assert_lock_held(&mvm->mutex);

  if (test_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &mvm->status)) {
    /*
     * Now convert the HW_RESTART_REQUESTED flag to IN_HW_RESTART
     * so later code will - from now on - see that we're doing it.
     */
    set_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status);
    clear_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &mvm->status);
    /* Clean up some internal and mac80211 state on restart */
    iwl_mvm_restart_cleanup(mvm);
  } else {
    /* Hold the reference to prevent runtime suspend while
     * the start procedure runs.  It's a bit confusing
     * that the UCODE_DOWN reference is taken, but it just
     * means "UCODE is not UP yet". ( TODO: rename this
     * reference).
     */
    iwl_mvm_ref(mvm, IWL_MVM_REF_UCODE_DOWN);
  }
  ret = iwl_mvm_up(mvm);

#if 0   // NEEDS_PORTING
    iwl_fw_dbg_apply_point(&mvm->fwrt, IWL_FW_INI_APPLY_POST_INIT);
#endif  // NEEDS_PORTING

  if (ret && test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
    /* Something went wrong - we need to finish some cleanup
     * that normally iwl_mvm_mac_restart_complete() below
     * would do.
     */
    clear_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status);
#ifdef CONFIG_PM
    iwl_mvm_d0i3_enable_tx(mvm, NULL);
#endif
  }

  return ret;
}

zx_status_t iwl_mvm_mac_start(struct iwl_mvm* mvm) {
  zx_status_t ret;

#if 0   // NEEDS_PORTING
    /* Some hw restart cleanups must not hold the mutex */
    if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
        /*
         * Make sure we are out of d0i3. This is needed
         * to make sure the reference accounting is correct
         * (and there is no stale d0i3_exit_work).
         */
        wait_event_timeout(mvm->d0i3_exit_waitq, !test_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status),
                           HZ);
    }
#endif  // NEEDS_PORTING

  mtx_lock(&mvm->mutex);
  ret = __iwl_mvm_mac_start(mvm);
  mtx_unlock(&mvm->mutex);

  return ret;
}

#if 0  // NEEDS_PORTING
static void iwl_mvm_restart_complete(struct iwl_mvm* mvm) {
    int ret;

    mutex_lock(&mvm->mutex);

    clear_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status);
#ifdef CONFIG_PM
    iwl_mvm_d0i3_enable_tx(mvm, NULL);
#endif
    ret = iwl_mvm_update_quotas(mvm, true, NULL);
    if (ret) { IWL_ERR(mvm, "Failed to update quotas after restart (%d)\n", ret); }

    /* allow transport/FW low power modes */
    iwl_mvm_unref(mvm, IWL_MVM_REF_UCODE_DOWN);

    /*
     * If we have TDLS peers, remove them. We don't know the last seqno/PN
     * of packets the FW sent out, so we must reconnect.
     */
    iwl_mvm_teardown_tdls_peers(mvm);

    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_resume_complete(struct iwl_mvm* mvm) {
    if (iwl_mvm_is_d0i3_supported(mvm) && iwl_mvm_enter_d0i3_on_suspend(mvm))
        WARN_ONCE(!wait_event_timeout(mvm->d0i3_exit_waitq,
                                      !test_bit(IWL_MVM_STATUS_IN_D0I3, &mvm->status), HZ),
                  "D0i3 exit on resume timed out\n");
}

static void iwl_mvm_mac_reconfig_complete(struct ieee80211_hw* hw,
                                          enum ieee80211_reconfig_type reconfig_type) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    switch (reconfig_type) {
    case IEEE80211_RECONFIG_TYPE_RESTART:
        iwl_mvm_restart_complete(mvm);
        break;
    case IEEE80211_RECONFIG_TYPE_SUSPEND:
        iwl_mvm_resume_complete(mvm);
        break;
    }
}
#endif  // NEEDS_PORTING

void __iwl_mvm_mac_stop(struct iwl_mvm* mvm) {
  iwl_assert_lock_held(&mvm->mutex);

  /* firmware counters are obviously reset now, but we shouldn't
   * partially track so also clear the fw_reset_accu counters.
   */
  memset(&mvm->accu_radio_stats, 0, sizeof(mvm->accu_radio_stats));

  /* async_handlers_wk is now blocked */

#if 0   // NEEDS_PORTING
    /*
     * The work item could be running or queued if the
     * ROC time event stops just as we get here.
     */
    flush_work(&mvm->roc_done_wk);
#endif  // NEEDS_PORTING

  iwl_mvm_stop_device(mvm);

  iwl_mvm_async_handlers_purge(mvm);
  /* async_handlers_list is empty and will stay empty: HW is stopped */

#if 0   // NEEDS_PORTING
    /* the fw is stopped, the aux sta is dead: clean up driver state */
    iwl_mvm_del_aux_sta(mvm);

    /*
     * Clear IN_HW_RESTART and HW_RESTART_REQUESTED flag when stopping the
     * hw (as restart_complete() won't be called in this case) and mac80211
     * won't execute the restart.
     * But make sure to cleanup interfaces that have gone down before/during
     * HW restart was requested.
     */
    if (test_and_clear_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status) ||
        test_and_clear_bit(IWL_MVM_STATUS_HW_RESTART_REQUESTED, &mvm->status)) {
        ieee80211_iterate_interfaces(mvm->hw, 0, iwl_mvm_cleanup_iterator, mvm);
    }
#endif  // NEEDS_PORTING

  /* We shouldn't have any UIDs still set.  Loop over all the UIDs to
   * make sure there's nothing left there and warn if any is found.
   */
  if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_UMAC_SCAN)) {
    for (unsigned int i = 0; i < mvm->max_scans; i++) {
      if (mvm->scan_uid_status[i]) {
        IWL_WARN(mvm, "UMAC scan UID %d status was not cleaned\n", i);
        mvm->scan_uid_status[i] = 0;
      }
    }
  }
}

void iwl_mvm_mac_stop(struct iwl_mvm* mvm) {
#if 0   // NEEDS_PORTING
    flush_work(&mvm->d0i3_exit_work);
    flush_work(&mvm->async_handlers_wk);
    flush_work(&mvm->add_stream_wk);
#endif  // NEEDS_PORTING

  /*
   * Lock and clear the firmware running bit here already, so that
   * new commands coming in elsewhere, e.g. from debugfs, will not
   * be able to proceed. This is important here because one of those
   * debugfs files causes the firmware dump to be triggered, and if we
   * don't stop debugfs accesses before canceling that it could be
   * retriggered after we flush it but before we've cleared the bit.
   */
  clear_bit(IWL_MVM_STATUS_FIRMWARE_RUNNING, &mvm->status);

  iwl_fw_cancel_dump(&mvm->fwrt);

#if 0  // NEEDS_PORTING
#ifdef CPTCFG_MAC80211_LATENCY_MEASUREMENTS
    cancel_delayed_work_sync(&mvm->tx_latency_watchdog_wk);
#endif  /* CPTCFG_MAC80211_LATENCY_MEASUREMENTS */
    cancel_delayed_work_sync(&mvm->cs_tx_unblock_dwork);
#endif  // NEEDS_PORTING

  iwl_task_release_sync(mvm->scan_timeout_task);
  mvm->scan_timeout_task = NULL;
  iwl_fw_free_dump_desc(&mvm->fwrt);

  mtx_lock(&mvm->mutex);
  __iwl_mvm_mac_stop(mvm);
  mtx_unlock(&mvm->mutex);

#if 0   // NEEDS_PORTING
    /*
     * The worker might have been waiting for the mutex, let it run and
     * discover that its list is now empty.
     */
    cancel_work_sync(&mvm->async_handlers_wk);
#endif  // NEEDS_PORTING
}

static struct iwl_mvm_phy_ctxt* iwl_mvm_get_free_phy_ctxt(struct iwl_mvm* mvm) {
  uint16_t i;

  iwl_assert_lock_held(&mvm->mutex);

  for (i = 0; i < NUM_PHY_CTX; i++) {
    if (!mvm->phy_ctxts[i].ref) {
      return &mvm->phy_ctxts[i];
    }
  }

  IWL_ERR(mvm, "No available PHY context\n");
  return NULL;
}

#if 0  // NEEDS_PORTING
static int iwl_mvm_set_tx_power(struct iwl_mvm* mvm, struct ieee80211_vif* vif, int16_t tx_power) {
    int len;
    union {
        struct iwl_dev_tx_power_cmd v5;
        struct iwl_dev_tx_power_cmd_v4 v4;
    } cmd = {
        .v5.v3.set_mode = cpu_to_le32(IWL_TX_POWER_MODE_SET_MAC),
        .v5.v3.mac_context_id = cpu_to_le32(iwl_mvm_vif_from_mac80211(vif)->id),
        .v5.v3.pwr_restriction = cpu_to_le16(8 * tx_power),
    };

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);

    /*
     * if set Tx power request did not come from Frequency Manager(FM)
     * Take minimum between wanted Tx power to FM Tx power limit
     */
    if (mvmvif->phy_ctxt && tx_power > mvmvif->phy_ctxt->fm_tx_power_limit) {
        cmd.v5.v3.pwr_restriction = cpu_to_le16(mvmvif->phy_ctxt->fm_tx_power_limit);
    }
#endif

    if (tx_power == IWL_DEFAULT_MAX_TX_POWER) {
        cmd.v5.v3.pwr_restriction = cpu_to_le16(IWL_DEV_MAX_TX_POWER);
    }

    if (fw_has_api(&mvm->fw->ucode_capa, IWL_UCODE_TLV_API_REDUCE_TX_POWER)) {
        len = sizeof(cmd.v5);
    } else if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_TX_POWER_ACK)) {
        len = sizeof(cmd.v4);
    } else {
        len = sizeof(cmd.v4.v3);
    }

    return iwl_mvm_send_cmd_pdu(mvm, REDUCE_TX_POWER_CMD, 0, len, &cmd);
}

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
int iwl_mvm_fm_set_tx_power(struct iwl_mvm* mvm, struct ieee80211_vif* vif, int8_t txpower) {
    int ret;

    mutex_lock(&mvm->mutex);
    /* set Tx power to min between drivers limit and FM limit */
    ret = iwl_mvm_set_tx_power(mvm, vif, min_t(int8_t, txpower, vif->bss_conf.txpower));
    mutex_unlock(&mvm->mutex);
    return ret;
}

/*
 * Updates Tx power limitation for the mac if FM has already limited
 * the Tx power on the channel that this mac is using.
 */
static void iwl_mvm_update_ctx_tx_power_limit(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                              struct iwl_mvm_phy_ctxt* phy_ctxt) {
    /* Tx power has not been limited by FM */
    if (phy_ctxt->fm_tx_power_limit == IWL_DEFAULT_MAX_TX_POWER) { return; }
    iwl_mvm_set_tx_power(mvm, vif, phy_ctxt->fm_tx_power_limit);
}
#endif
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_find_free_mvmvif_slot(struct iwl_mvm* mvm, int* ret_idx) {
  int idx;

  ZX_ASSERT(ret_idx);

  iwl_assert_lock_held(&mvm->mutex);

  for (idx = 0; idx < MAX_NUM_MVMVIF; idx++) {
    if (!mvm->mvmvif[idx]) {
      *ret_idx = idx;
      return ZX_OK;
    }
  }
  return ZX_ERR_NO_RESOURCES;
}

// This function doesn't take the ownership of the mvmvif after binding. It just adds a reference
// from mvm to the mvmvif instance.
zx_status_t iwl_mvm_bind_mvmvif(struct iwl_mvm* mvm, int idx, struct iwl_mvm_vif* mvmvif) {
  iwl_assert_lock_held(&mvm->mutex);

  ZX_ASSERT(mvmvif);

  if (mvm->mvmvif[idx]) {
    IWL_ERR(mvm, "mvm->mvmvif[%d] has been binded.\n", idx);
    return ZX_ERR_ALREADY_EXISTS;
  }

  mvm->mvmvif[idx] = mvmvif;
  return ZX_OK;
}

void iwl_mvm_unbind_mvmvif(struct iwl_mvm* mvm, int idx) {
  iwl_assert_lock_held(&mvm->mutex);
  mvm->mvmvif[idx] = NULL;
}

zx_status_t iwl_mvm_mac_add_interface(struct iwl_mvm_vif* mvmvif) {
  struct iwl_mvm* mvm = mvmvif->mvm;
  mvmvif->probe_resp_data = NULL;

  /*
   * make sure D0i3 exit is completed, otherwise a target access
   * during tx queue configuration could be done when still in
   * D0i3 state.
   */
  zx_status_t ret = iwl_mvm_ref_sync(mvm, IWL_MVM_REF_ADD_IF);
  if (ret != ZX_OK) {
    return ret;
  }

  /*
   * Not much to do here. The stack will not allow interface
   * types or combinations that we didn't advertise, so we
   * don't really have to check the types.
   */

  mtx_lock(&mvm->mutex);

  /* make sure that beacon statistics don't go backwards with FW reset */
  if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
    mvmvif->beacon_stats.accu_num_beacons += mvmvif->beacon_stats.num_beacons;
  }

  /* Allocate resources for the MAC context, and add it to the fw  */
  ret = iwl_mvm_mac_ctxt_init(mvmvif);
  if (ret != ZX_OK) {
    goto out_unlock;
  }

#if 0   // NEEDS_PORTING
    /* Currently not much to do for NAN */
    if (vif->type == NL80211_IFTYPE_NAN) { goto out_unlock; }

    /* Counting number of interfaces is needed for legacy PM */
    if (vif->type != NL80211_IFTYPE_P2P_DEVICE) { mvm->vif_count++; }

    /*
     * The AP binding flow can be done only after the beacon
     * template is configured (which happens only in the mac80211
     * start_ap() flow), and adding the broadcast station can happen
     * only after the binding.
     * In addition, since modifying the MAC before adding a bcast
     * station is not allowed by the FW, delay the adding of MAC context to
     * the point where we can also add the bcast station.
     * In short: there's not much we can do at this point, other than
     * allocating resources :)
     */
    if (vif->type == NL80211_IFTYPE_AP || vif->type == NL80211_IFTYPE_ADHOC) {
        ret = iwl_mvm_alloc_bcast_sta(mvm, vif);
        if (ret) {
            IWL_ERR(mvm, "Failed to allocate bcast sta\n");
            goto out_release;
        }

        /*
         * Only queue for this station is the mcast queue,
         * which shouldn't be in TFD mask anyway
         */
        ret = iwl_mvm_allocate_int_sta(mvm, &mvmvif->mcast_sta, 0, vif->type, IWL_STA_MULTICAST);
        if (ret) { goto out_release; }

        iwl_mvm_vif_dbgfs_register(mvm, vif);
        goto out_unlock;
    }

    mvmvif->features |= hw->netdev_features;
#endif  // NEEDS_PORTING
  mvm->vif_count++;

  ret = iwl_mvm_mac_ctxt_add(mvmvif);
  if (ret != ZX_OK) {
    goto out_release;
  }

#if 0  // NEEDS_PORTING
    ret = iwl_mvm_power_update_mac(mvm);
    if (ret) { goto out_remove_mac; }

    /* beacon filtering */
    ret = iwl_mvm_disable_beacon_filter(mvm, vif, 0);
    if (ret != ZX_OK) { goto out_remove_mac; }

    if (!mvm->bf_allowed_vif && vif->type == NL80211_IFTYPE_STATION && !vif->p2p) {
        mvm->bf_allowed_vif = mvmvif;
        vif->driver_flags |= IEEE80211_VIF_BEACON_FILTER | IEEE80211_VIF_SUPPORTS_CQM_RSSI;
    }

    /*
     * P2P_DEVICE interface does not have a channel context assigned to it,
     * so a dedicated PHY context is allocated to it and the corresponding
     * MAC context is bound to it at this stage.
     */
    if (vif->type == NL80211_IFTYPE_P2P_DEVICE) {
        mvmvif->phy_ctxt = iwl_mvm_get_free_phy_ctxt(mvm);
        if (!mvmvif->phy_ctxt) {
            ret = -ENOSPC;
            goto out_free_bf;
        }

        iwl_mvm_phy_ctxt_ref(mvm, mvmvif->phy_ctxt);
        ret = iwl_mvm_binding_add_vif(mvm, vif);
        if (ret) { goto out_unref_phy; }

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
        iwl_mvm_update_ctx_tx_power_limit(mvm, vif, mvmvif->phy_ctxt);
#endif

        ret = iwl_mvm_add_p2p_bcast_sta(mvm, vif);
        if (ret) { goto out_unbind; }

        /* Save a pointer to p2p device vif, so it can later be used to
         * update the p2p device MAC when a GO is started/stopped */
        mvm->p2p_device_vif = vif;
    }

    iwl_mvm_tcm_add_vif(mvm, vif);

    if (vif->type == NL80211_IFTYPE_MONITOR) { mvm->monitor_on = true; }

    iwl_mvm_vif_dbgfs_register(mvm, vif);
#endif  // NEEDS_PORTING
  goto out_unlock;

#if 0   // NEEDS_PORTING
out_unbind:
    iwl_mvm_binding_remove_vif(mvm, vif);
out_unref_phy:
    iwl_mvm_phy_ctxt_unref(mvm, mvmvif->phy_ctxt);
out_free_bf:
    if (mvm->bf_allowed_vif == mvmvif) {
        mvm->bf_allowed_vif = NULL;
        vif->driver_flags &= ~(IEEE80211_VIF_BEACON_FILTER | IEEE80211_VIF_SUPPORTS_CQM_RSSI);
    }
out_remove_mac:
#endif  // NEEDS_PORTING
  mvmvif->phy_ctxt = NULL;
  iwl_mvm_mac_ctxt_remove(mvmvif);
out_release:
#if 0   // NEEDS_PORTING
    if (vif->type != NL80211_IFTYPE_P2P_DEVICE) { mvm->vif_count--; }
#endif  // NEEDS_PORTING
  mvm->vif_count--;
out_unlock:
  mtx_unlock(&mvm->mutex);

  iwl_mvm_unref(mvm, IWL_MVM_REF_ADD_IF);

  return ret;
}

#if 0   // NEEDS_PORTING
static void iwl_mvm_prepare_mac_removal(struct iwl_mvm* mvm, struct ieee80211_vif* vif) {
    if (vif->type == NL80211_IFTYPE_P2P_DEVICE) {
        /*
         * Flush the ROC worker which will flush the OFFCHANNEL queue.
         * We assume here that all the packets sent to the OFFCHANNEL
         * queue are sent in ROC session.
         */
        flush_work(&mvm->roc_done_wk);
    }
}
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_mac_remove_interface(struct iwl_mvm_vif* mvmvif) {
  struct iwl_mvm* mvm = mvmvif->mvm;
  struct iwl_probe_resp_data* probe_data;

#if 0   // NEEDS_PORTING
    iwl_mvm_prepare_mac_removal(mvm, vif);

    if (vif->type == NL80211_IFTYPE_NAN) {
        struct wireless_dev* wdev = ieee80211_vif_to_wdev(vif);
        /* cfg80211 should stop NAN before interface removal */
        if (wdev && WARN_ON(wdev_running(wdev))) { iwl_mvm_stop_nan(hw, vif); }

        return;
    }

    if (!(vif->type == NL80211_IFTYPE_AP || vif->type == NL80211_IFTYPE_ADHOC)) {
        iwl_mvm_tcm_rm_vif(mvm, vif);
    }
#endif  // NEEDS_PORTING

  mtx_lock(&mvm->mutex);

  probe_data = iwl_rcu_exchange(mvmvif->probe_resp_data, NULL);
  if (probe_data) {
    iwl_rcu_free_sync(mvm->dev, probe_data);
  }

#if 0  // NEEDS_PORTING
    if (mvm->bf_allowed_vif == mvmvif) {
        mvm->bf_allowed_vif = NULL;
        vif->driver_flags &= ~(IEEE80211_VIF_BEACON_FILTER | IEEE80211_VIF_SUPPORTS_CQM_RSSI);
    }

    iwl_mvm_vif_dbgfs_clean(mvm, vif);

    /*
     * For AP/GO interface, the tear down of the resources allocated to the
     * interface is be handled as part of the stop_ap flow.
     */
    if (vif->type == NL80211_IFTYPE_AP || vif->type == NL80211_IFTYPE_ADHOC) {
#ifdef CPTCFG_NL80211_TESTMODE
        if (vif == mvm->noa_vif) {
            mvm->noa_vif = NULL;
            mvm->noa_duration = 0;
        }
#endif
        iwl_mvm_dealloc_int_sta(mvm, &mvmvif->mcast_sta);
        iwl_mvm_dealloc_bcast_sta(mvm, vif);
        goto out_release;
    }
#endif  // NEEDS_PORTING

#ifdef CPTCFG_IWLMVM_P2P_OPPPS_TEST_WA
  if (mvmvif == mvm->p2p_opps_test_wa_vif) {
    mvm->p2p_opps_test_wa_vif = NULL;
  }
#endif

#if 0   // NEEDS_PORTING
    if (vif->type == NL80211_IFTYPE_P2P_DEVICE) {
        mvm->p2p_device_vif = NULL;
        iwl_mvm_rm_p2p_bcast_sta(mvm, vif);
        iwl_mvm_binding_remove_vif(mvm, vif);
        iwl_mvm_phy_ctxt_unref(mvm, mvmvif->phy_ctxt);
        mvmvif->phy_ctxt = NULL;
    }

    if (mvm->vif_count && vif->type != NL80211_IFTYPE_P2P_DEVICE) { mvm->vif_count--; }
#endif  // NEEDS_PORTING
  if (mvm->vif_count) {
    mvm->vif_count--;
  }

#if 0   // NEEDS_PORTING
    iwl_mvm_power_update_mac(mvm);
#endif  // NEEDS_PORTING
  zx_status_t ret = iwl_mvm_mac_ctxt_remove(mvmvif);

#if 0   // NEEDS_PORTING
    if (vif->type == NL80211_IFTYPE_MONITOR) { mvm->monitor_on = false; }
#endif  // NEEDS_PORTING

#ifdef CPTCFG_IWLMVM_TDLS_PEER_CACHE
  iwl_mvm_tdls_peer_cache_clear(mvm, vif);
#endif /* CPTCFG_IWLMVM_TDLS_PEER_CACHE */

#if 0   // NEEDS_PORTING
out_release:
#endif  // NEEDS_PORTING
  mtx_unlock(&mvm->mutex);

  return ret;
}

#if 0   // NEEDS_PORTING
static int iwl_mvm_mac_config(struct ieee80211_hw* hw, uint32_t changed) {
    return 0;
}
#endif  // NEEDS_PORTING

struct iwl_mvm_mc_iter_data {
  struct iwl_mvm* mvm;
  int port_id;
};

// Once the interface becomes an associated client interface, the driver uses the pre-configured
// MCAST_FILTER_CMD to tell firmware the multicast packets it is interested so that the firmware
// can forward them to driver when the firmware receives them.
//
static void iwl_mvm_mc_iface_iterator(void* _data, struct iwl_mvm_vif* mvmvif) {
  struct iwl_mvm_mc_iter_data* data = _data;
  struct iwl_mvm* mvm = data->mvm;
  struct iwl_mcast_filter_cmd* cmd = mvm->mcast_filter_cmd;
  struct iwl_host_cmd hcmd = {
      .id = MCAST_FILTER_CMD,
      .flags = CMD_ASYNC,
      .dataflags[0] = IWL_HCMD_DFL_NOCOPY,
  };
  int ret, len;

#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
  if (!(mvm->rx_filters & IWL_MVM_VENDOR_RXFILTER_EINVAL) && mvm->mcast_active_filter_cmd) {
    cmd = mvm->mcast_active_filter_cmd;
  }
#endif

  /* if we don't have free ports, mcast frames will be dropped */
  if (data->port_id >= MAX_PORT_ID_NUM) {
    IWL_WARN(mvmvif, "%s(): port id (%d) is larger than max port number (%d)\n", __func__,
             data->port_id, MAX_PORT_ID_NUM);
    return;
  }

  // Only associated client interface can continue. Other interfaces will be ignored.
  if (mvmvif->mac_role != WLAN_INFO_MAC_ROLE_CLIENT ||
      mvmvif->mvm->fw_id_to_mac_id[0]->sta_state != IWL_STA_AUTHORIZED) {
    IWL_ERR(mvmvif, "unexpected state while setting mcast filter. role: %d!=%d or state: %d!=%d\n",
            mvmvif->mac_role, WLAN_INFO_MAC_ROLE_CLIENT, mvmvif->mvm->fw_id_to_mac_id[0]->sta_state,
            IWL_STA_AUTHORIZED);
    return;
  }

  cmd->port_id = data->port_id++;
  memcpy(cmd->bssid, mvmvif->bss_conf.bssid, ETH_ALEN);
  len = ROUND_UP(sizeof(*cmd) + cmd->count * ETH_ALEN, 4);

  hcmd.len[0] = len;
  hcmd.data[0] = cmd;

  ret = iwl_mvm_send_cmd(mvm, &hcmd);
  if (ret != ZX_OK) {
    IWL_ERR(mvmvif, "mcast filter cmd error. ret=%s\n", zx_status_get_string(ret));
  }
}

// Traverse all interfaces and set the multicast filter for associated client interface.
static void iwl_mvm_recalc_multicast(struct iwl_mvm* mvm) {
  struct iwl_mvm_mc_iter_data iter_data = {
      .mvm = mvm,
  };

  iwl_assert_lock_held(&mvm->mutex);

  if (!mvm->mcast_filter_cmd) {
    IWL_WARN(mvm, "%s(): mcast_filter_cmd is NULL\n", __func__);
    return;
  }

  ieee80211_iterate_active_interfaces_atomic(mvm, iwl_mvm_mc_iface_iterator, &iter_data);
}

#if 0   // NEEDS_PORTING
// TODO(51238): implement iwl_mvm_prepare_multicast()
static uint64_t iwl_mvm_prepare_multicast(struct ieee80211_hw* hw,
                                          struct netdev_hw_addr_list* mc_list) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mcast_filter_cmd* cmd;
    struct netdev_hw_addr* addr;
    int addr_count;
    bool pass_all;
    int len;

    addr_count = netdev_hw_addr_list_count(mc_list);
    pass_all = addr_count > MAX_MCAST_FILTERING_ADDRESSES || IWL_MVM_FW_MCAST_FILTER_PASS_ALL;
    if (pass_all) { addr_count = 0; }

    len = roundup(sizeof(*cmd) + addr_count * ETH_ALEN, 4);
    cmd = kzalloc(len, GFP_ATOMIC);
    if (!cmd) { return 0; }

    if (pass_all) {
        cmd->pass_all = 1;
        return (uint64_t)(unsigned long)cmd;
    }

    netdev_hw_addr_list_for_each(addr, mc_list) {
        IWL_DEBUG_MAC80211(mvm, "mcast addr (%d): %pM\n", cmd->count, addr->addr);
        memcpy(&cmd->addr_list[cmd->count * ETH_ALEN], addr->addr, ETH_ALEN);
        cmd->count++;
    }

    return (uint64_t)(unsigned long)cmd;
}
#endif  // NEEDS_PORTING

void iwl_mvm_configure_filter(struct iwl_mvm* mvm) {
  // There are 3 multicast addresses we want firmware to pass it to driver.
  // TODO(51238): remove the hardcoded mcast_addrs after iwl_mvm_prepare_multicast() is implemented.
  uint8_t mcast_addrs[][ETH_ALEN] = {
      {
          // IPv6 mutlicast address
          0x33,
          0x33,
          0x00,
          0x00,
          0x00,
          0x01,
      },
      {
          // IPv6 mutlicast address
          0x33,
          0x33,
          0x00,
          0x00,
          0x00,
          0x02,
      },
      {
          // IPv4 mutlicast address
          0x01,
          0x00,
          0x5e,
          0x00,
          0x00,
          0x01,
      },
  };

  struct iwl_mcast_filter_cmd* cmd =
      calloc(1, sizeof(struct iwl_mcast_filter_cmd) + sizeof(mcast_addrs));

  mtx_lock(&mvm->mutex);

  /* replace previous configuration */
  free(mvm->mcast_filter_cmd);
  mvm->mcast_filter_cmd = cmd;
  if (!cmd) {
    goto out;
  }

  if (cmd->pass_all) {
    cmd->count = 0;
  }

  for (size_t i = 0; i < ARRAY_SIZE(mcast_addrs); i++) {
    size_t offset = i * ETH_ALEN;
    memcpy(&cmd->addr_list[offset], mcast_addrs[i], ETH_ALEN);
  }
  cmd->count = ARRAY_SIZE(mcast_addrs);

#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
  iwl_mvm_active_rx_filters(mvm);
#endif
  iwl_mvm_recalc_multicast(mvm);
out:
  mtx_unlock(&mvm->mutex);
}

#if 0  // NEEDS_PORTING
static void iwl_mvm_config_iface_filter(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                        unsigned int filter_flags, unsigned int changed_flags) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    /* We support only filter for probe requests */
    if (!(changed_flags & FIF_PROBE_REQ)) { return; }

    /* Supported only for p2p client interfaces */
    if (vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.assoc || !vif->p2p) { return; }

    mutex_lock(&mvm->mutex);
    iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL);
    mutex_unlock(&mvm->mutex);
}

#ifdef CPTCFG_IWLWIFI_BCAST_FILTERING
struct iwl_bcast_iter_data {
    struct iwl_mvm* mvm;
    struct iwl_bcast_filter_cmd* cmd;
    uint8_t current_filter;
};

static void iwl_mvm_set_bcast_filter(struct ieee80211_vif* vif,
                                     const struct iwl_fw_bcast_filter* in_filter,
                                     struct iwl_fw_bcast_filter* out_filter) {
    struct iwl_fw_bcast_filter_attr* attr;
    int i;

    memcpy(out_filter, in_filter, sizeof(*out_filter));

    for (i = 0; i < ARRAY_SIZE(out_filter->attrs); i++) {
        attr = &out_filter->attrs[i];

        if (!attr->mask) { break; }

        switch (attr->reserved1) {
        case cpu_to_le16(BC_FILTER_MAGIC_IP):
            if (vif->bss_conf.arp_addr_cnt != 1) {
                attr->mask = 0;
                continue;
            }

            attr->val = vif->bss_conf.arp_addr_list[0];
            break;
        case cpu_to_le16(BC_FILTER_MAGIC_MAC):
            attr->val = *(__be32*)&vif->addr[2];
            break;
        default:
            break;
        }
        attr->reserved1 = 0;
        out_filter->num_attrs++;
    }
}

static void iwl_mvm_bcast_filter_iterator(void* _data, uint8_t* mac, struct ieee80211_vif* vif) {
    struct iwl_bcast_iter_data* data = _data;
    struct iwl_mvm* mvm = data->mvm;
    struct iwl_bcast_filter_cmd* cmd = data->cmd;
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct iwl_fw_bcast_mac* bcast_mac;
    int i;

    if (WARN_ON(mvmvif->id >= ARRAY_SIZE(cmd->macs))) { return; }

    bcast_mac = &cmd->macs[mvmvif->id];

    /*
     * enable filtering only for associated stations, but not for P2P
     * Clients
     */
    if (vif->type != NL80211_IFTYPE_STATION || vif->p2p || !vif->bss_conf.assoc) { return; }

    bcast_mac->default_discard = 1;

    /* copy all configured filters */
    for (i = 0; mvm->bcast_filters[i].attrs[0].mask; i++) {
        /*
         * Make sure we don't exceed our filters limit.
         * if there is still a valid filter to be configured,
         * be on the safe side and just allow bcast for this mac.
         */
        if (WARN_ON_ONCE(data->current_filter >= ARRAY_SIZE(cmd->filters))) {
            bcast_mac->default_discard = 0;
            bcast_mac->attached_filters = 0;
            break;
        }

        iwl_mvm_set_bcast_filter(vif, &mvm->bcast_filters[i], &cmd->filters[data->current_filter]);

        /* skip current filter if it contains no attributes */
        if (!cmd->filters[data->current_filter].num_attrs) { continue; }

        /* attach the filter to current mac */
        bcast_mac->attached_filters |= cpu_to_le16(BIT(data->current_filter));

        data->current_filter++;
    }
}

bool iwl_mvm_bcast_filter_build_cmd(struct iwl_mvm* mvm, struct iwl_bcast_filter_cmd* cmd) {
    struct iwl_bcast_iter_data iter_data = {
        .mvm = mvm,
        .cmd = cmd,
    };

    if (IWL_MVM_FW_BCAST_FILTER_PASS_ALL) { return false; }

    memset(cmd, 0, sizeof(*cmd));
    cmd->max_bcast_filters = ARRAY_SIZE(cmd->filters);
    cmd->max_macs = ARRAY_SIZE(cmd->macs);

#ifdef CPTCFG_IWLWIFI_DEBUGFS
    /* use debugfs filters/macs if override is configured */
    if (mvm->dbgfs_bcast_filtering.override) {
        memcpy(cmd->filters, &mvm->dbgfs_bcast_filtering.cmd.filters, sizeof(cmd->filters));
        memcpy(cmd->macs, &mvm->dbgfs_bcast_filtering.cmd.macs, sizeof(cmd->macs));
        return true;
    }
#endif

    /* if no filters are configured, do nothing */
    if (!mvm->bcast_filters) { return false; }

#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
    if (!(mvm->rx_filters & IWL_MVM_VENDOR_RXFILTER_EINVAL) &&
        mvm->rx_filters & IWL_MVM_VENDOR_RXFILTER_BCAST) {
        cmd->disable = 1;
        return true;
    }
#endif
    /* configure and attach these filters for each associated sta vif */
    ieee80211_iterate_active_interfaces(mvm->hw, IEEE80211_IFACE_ITER_NORMAL,
                                        iwl_mvm_bcast_filter_iterator, &iter_data);

    return true;
}

int iwl_mvm_configure_bcast_filter(struct iwl_mvm* mvm) {
    struct iwl_bcast_filter_cmd cmd;

    if (!(mvm->fw->ucode_capa.flags & IWL_UCODE_TLV_FLAGS_BCAST_FILTERING)) { return 0; }

    if (!iwl_mvm_bcast_filter_build_cmd(mvm, &cmd)) { return 0; }

    return iwl_mvm_send_cmd_pdu(mvm, BCAST_FILTER_CMD, 0, sizeof(cmd), &cmd);
}
#else
inline int iwl_mvm_configure_bcast_filter(struct iwl_mvm* mvm) {
    return 0;
}
#endif

static int iwl_mvm_update_mu_groups(struct iwl_mvm* mvm, struct ieee80211_vif* vif) {
    struct iwl_mu_group_mgmt_cmd cmd = {};

    memcpy(cmd.membership_status, vif->bss_conf.mu_group.membership, WLAN_MEMBERSHIP_LEN);
    memcpy(cmd.user_position, vif->bss_conf.mu_group.position, WLAN_USER_POSITION_LEN);

    return iwl_mvm_send_cmd_pdu(mvm, WIDE_ID(DATA_PATH_GROUP, UPDATE_MU_GROUPS_CMD), 0, sizeof(cmd),
                                &cmd);
}

static void iwl_mvm_mu_mimo_iface_iterator(void* _data, uint8_t* mac, struct ieee80211_vif* vif) {
    if (vif->mu_mimo_owner) {
        struct iwl_mu_group_mgmt_notif* notif = _data;

        /*
         * MU-MIMO Group Id action frame is little endian. We treat
         * the data received from firmware as if it came from the
         * action frame, so no conversion is needed.
         */
        ieee80211_update_mu_groups(vif, (uint8_t*)&notif->membership_status,
                                   (uint8_t*)&notif->user_position);
    }
}

void iwl_mvm_mu_mimo_grp_notif(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb) {
    struct iwl_rx_packet* pkt = rxb_addr(rxb);
    struct iwl_mu_group_mgmt_notif* notif = (void*)pkt->data;

    ieee80211_iterate_active_interfaces_atomic(mvm->hw, IEEE80211_IFACE_ITER_NORMAL,
                                               iwl_mvm_mu_mimo_iface_iterator, notif);
}

static uint8_t iwl_mvm_he_get_ppe_val(uint8_t* ppe, uint8_t ppe_pos_bit) {
    uint8_t byte_num = ppe_pos_bit / 8;
    uint8_t bit_num = ppe_pos_bit % 8;
    uint8_t residue_bits;
    uint8_t res;

    if (bit_num <= 5) {
        return (ppe[byte_num] >> bit_num) & (BIT(IEEE80211_PPE_THRES_INFO_PPET_SIZE) - 1);
    }

    /*
     * If bit_num > 5, we have to combine bits with next byte.
     * Calculate how many bits we need to take from current byte (called
     * here "residue_bits"), and add them to bits from next byte.
     */

    residue_bits = 8 - bit_num;

    res = (ppe[byte_num + 1] & (BIT(IEEE80211_PPE_THRES_INFO_PPET_SIZE - residue_bits) - 1))
          << residue_bits;
    res += (ppe[byte_num] >> bit_num) & (BIT(residue_bits) - 1);

    return res;
}

static const uint8_t mac80211_ac_to_ucode_ac[] = {AC_VO, AC_VI, AC_BE, AC_BK};

static void iwl_mvm_cfg_he_sta(struct iwl_mvm* mvm, struct ieee80211_vif* vif, uint8_t sta_id) {
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct iwl_he_sta_context_cmd sta_ctxt_cmd = {
        .sta_id = sta_id,
        .tid_limit = IWL_MAX_TID_COUNT,
        .bss_color = vif->bss_conf.bss_color,
        .htc_trig_based_pkt_ext = vif->bss_conf.htc_trig_based_pkt_ext,
        .frame_time_rts_th = cpu_to_le16(vif->bss_conf.frame_time_rts_th),
    };
    struct ieee80211_sta* sta;
    uint32_t flags;
    int i;

    rcu_read_lock();

    sta = rcu_dereference(mvm->fw_id_to_mac_id[sta_ctxt_cmd.sta_id]);
    if (IS_ERR(sta)) {
        rcu_read_unlock();
        WARN(1, "Can't find STA to configure HE\n");
        return;
    }

    if (!sta->he_cap.has_he) {
        rcu_read_unlock();
        return;
    }

    flags = 0;

    /* HTC flags */
    if (sta->he_cap.he_cap_elem.mac_cap_info[0] & IEEE80211_HE_MAC_CAP0_HTC_HE) {
        sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_SUPPORT);
    }
    if ((sta->he_cap.he_cap_elem.mac_cap_info[1] & IEEE80211_HE_MAC_CAP1_LINK_ADAPTATION) ||
        (sta->he_cap.he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_LINK_ADAPTATION)) {
        uint8_t link_adap =
            ((sta->he_cap.he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_LINK_ADAPTATION)
             << 1) +
            (sta->he_cap.he_cap_elem.mac_cap_info[1] & IEEE80211_HE_MAC_CAP1_LINK_ADAPTATION);

        if (link_adap == 2) {
            sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_LINK_ADAP_UNSOLICITED);
        } else if (link_adap == 3) {
            sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_LINK_ADAP_BOTH);
        }
    }
    if (sta->he_cap.he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_BSR) {
        sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_BSR_SUPP);
    }
    if (sta->he_cap.he_cap_elem.mac_cap_info[3] & IEEE80211_HE_MAC_CAP3_OMI_CONTROL) {
        sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_OMI_SUPP);
    }
    if (sta->he_cap.he_cap_elem.mac_cap_info[4] & IEEE80211_HE_MAC_CAP4_BQR) {
        sta_ctxt_cmd.htc_flags |= cpu_to_le32(IWL_HE_HTC_BQR_SUPP);
    }

    /*
     * Initialize the PPE thresholds to "None" (7), as described in Table
     * 9-262ac of 80211.ax/D3.0.
     */
    memset(&sta_ctxt_cmd.pkt_ext, 7, sizeof(sta_ctxt_cmd.pkt_ext));

    /* If PPE Thresholds exist, parse them into a FW-familiar format. */
    if (sta->he_cap.he_cap_elem.phy_cap_info[6] & IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT) {
        uint8_t nss = (sta->he_cap.ppe_thres[0] & IEEE80211_PPE_THRES_NSS_MASK) + 1;
        uint8_t ru_index_bitmap =
            (sta->he_cap.ppe_thres[0] & IEEE80211_PPE_THRES_RU_INDEX_BITMASK_MASK) >>
            IEEE80211_PPE_THRES_RU_INDEX_BITMASK_POS;
        uint8_t* ppe = &sta->he_cap.ppe_thres[0];
        uint8_t ppe_pos_bit = 7; /* Starting after PPE header */

        /*
         * FW currently supports only nss == MAX_HE_SUPP_NSS
         *
         * If nss > MAX: we can ignore values we don't support
         * If nss < MAX: we can set zeros in other streams
         */
        if (nss > MAX_HE_SUPP_NSS) {
            IWL_INFO(mvm, "Got NSS = %d - trimming to %d\n", nss, MAX_HE_SUPP_NSS);
            nss = MAX_HE_SUPP_NSS;
        }

        for (i = 0; i < nss; i++) {
            uint8_t ru_index_tmp = ru_index_bitmap << 1;
            uint8_t bw;

            for (bw = 0; bw < MAX_HE_CHANNEL_BW_INDX; bw++) {
                ru_index_tmp >>= 1;
                if (!(ru_index_tmp & 1)) { continue; }

                sta_ctxt_cmd.pkt_ext.pkt_ext_qam_th[i][bw][1] =
                    iwl_mvm_he_get_ppe_val(ppe, ppe_pos_bit);
                ppe_pos_bit += IEEE80211_PPE_THRES_INFO_PPET_SIZE;
                sta_ctxt_cmd.pkt_ext.pkt_ext_qam_th[i][bw][0] =
                    iwl_mvm_he_get_ppe_val(ppe, ppe_pos_bit);
                ppe_pos_bit += IEEE80211_PPE_THRES_INFO_PPET_SIZE;
            }
        }

        flags |= STA_CTXT_HE_PACKET_EXT;
    }
    rcu_read_unlock();

    /* Mark MU EDCA as enabled, unless none detected on some AC */
    flags |= STA_CTXT_HE_MU_EDCA_CW;
    for (i = 0; i < IEEE80211_AC_MAX; i++) {
        struct ieee80211_he_mu_edca_param_ac_rec* mu_edca =
            &mvmvif->queue_params[i].mu_edca_param_rec;
        uint8_t ac = mac80211_ac_to_ucode_ac[i];

        if (!mvmvif->queue_params[i].mu_edca) {
            flags &= ~STA_CTXT_HE_MU_EDCA_CW;
            break;
        }

        sta_ctxt_cmd.trig_based_txf[ac].cwmin = cpu_to_le16(mu_edca->ecw_min_max & 0xf);
        sta_ctxt_cmd.trig_based_txf[ac].cwmax = cpu_to_le16((mu_edca->ecw_min_max & 0xf0) >> 4);
        sta_ctxt_cmd.trig_based_txf[ac].aifsn = cpu_to_le16(mu_edca->aifsn & 0xf);
        sta_ctxt_cmd.trig_based_txf[ac].mu_time = cpu_to_le16(mu_edca->mu_edca_timer);
    }

    if (vif->bss_conf.multi_sta_back_32bit) { flags |= STA_CTXT_HE_32BIT_BA_BITMAP; }

    if (vif->bss_conf.ack_enabled) { flags |= STA_CTXT_HE_ACK_ENABLED; }

    if (vif->bss_conf.uora_exists) {
        flags |= STA_CTXT_HE_TRIG_RND_ALLOC;

        sta_ctxt_cmd.rand_alloc_ecwmin = vif->bss_conf.uora_ocw_range & 0x7;
        sta_ctxt_cmd.rand_alloc_ecwmax = (vif->bss_conf.uora_ocw_range >> 3) & 0x7;
    }

    if (!(sta->he_cap.he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_ACK_EN)) {
        flags |= STA_CTXT_HE_NIC_NOT_ACK_ENABLED;
    }

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
    if (mvm->trans->dbg_cfg.no_ack_en & 0x2) { flags &= ~STA_CTXT_HE_ACK_ENABLED; }
#endif

    /* TODO: support Multi BSSID IE */

    sta_ctxt_cmd.flags = cpu_to_le32(flags);

    if (iwl_mvm_send_cmd_pdu(mvm, iwl_cmd_id(STA_HE_CTXT_CMD, DATA_PATH_GROUP, 0), 0,
                             sizeof(sta_ctxt_cmd), &sta_ctxt_cmd)) {
        IWL_ERR(mvm, "Failed to config FW to work HE!\n");
    }
}

static void iwl_mvm_bss_info_changed_station(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                             struct ieee80211_bss_conf* bss_conf,
                                             uint32_t changes) {
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    int ret;

    /*
     * Re-calculate the tsf id, as the master-slave relations depend on the
     * beacon interval, which was not known when the station interface was
     * added.
     */
    if (changes & BSS_CHANGED_ASSOC && bss_conf->assoc) {
        if (vif->bss_conf.he_support && !iwlwifi_mod_params.disable_11ax) {
            iwl_mvm_cfg_he_sta(mvm, vif, mvmvif->ap_sta_id);
        }

        iwl_mvm_mac_ctxt_recalc_tsf_id(mvm, vif);
    }

    /*
     * If we're not associated yet, take the (new) BSSID before associating
     * so the firmware knows. If we're already associated, then use the old
     * BSSID here, and we'll send a cleared one later in the CHANGED_ASSOC
     * branch for disassociation below.
     */
    if (changes & BSS_CHANGED_BSSID && !mvmvif->associated) {
        memcpy(mvmvif->bssid, bss_conf->bssid, ETH_ALEN);
    }

    ret = iwl_mvm_mac_ctxt_changed(mvm, vif, false, mvmvif->bssid);
    if (ret) { IWL_ERR(mvm, "failed to update MAC %pM\n", vif->addr); }

    /* after sending it once, adopt mac80211 data */
    memcpy(mvmvif->bssid, bss_conf->bssid, ETH_ALEN);
    mvmvif->associated = bss_conf->assoc;

    if (changes & BSS_CHANGED_ASSOC) {
        if (bss_conf->assoc) {
            /* clear statistics to get clean beacon counter */
            iwl_mvm_request_statistics(mvm, true);
            memset(&mvmvif->beacon_stats, 0, sizeof(mvmvif->beacon_stats));

            /* add quota for this interface */
            ret = iwl_mvm_update_quotas(mvm, true, NULL);
            if (ret) {
                IWL_ERR(mvm, "failed to update quotas\n");
                return;
            }

            if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
                /*
                 * If we're restarting then the firmware will
                 * obviously have lost synchronisation with
                 * the AP. It will attempt to synchronise by
                 * itself, but we can make it more reliable by
                 * scheduling a session protection time event.
                 *
                 * The firmware needs to receive a beacon to
                 * catch up with synchronisation, use 110% of
                 * the beacon interval.
                 *
                 * Set a large maximum delay to allow for more
                 * than a single interface.
                 */
                uint32_t dur = (11 * vif->bss_conf.beacon_int) / 10;
                iwl_mvm_protect_session(mvm, vif, dur, dur, 5 * dur, false);
            }

            iwl_mvm_sf_update(mvm, vif, false);
            iwl_mvm_power_vif_assoc(mvm, vif);
            if (vif->p2p) {
                iwl_mvm_ref(mvm, IWL_MVM_REF_P2P_CLIENT);
                iwl_mvm_update_smps(mvm, vif, IWL_MVM_SMPS_REQ_PROT, IEEE80211_SMPS_DYNAMIC);
            }
        } else if (mvmvif->ap_sta_id != IWL_MVM_INVALID_STA) {
            /*
             * If update fails - SF might be running in associated
             * mode while disassociated - which is forbidden.
             */
            WARN_ONCE(iwl_mvm_sf_update(mvm, vif, false),
                      "Failed to update SF upon disassociation\n");

            /*
             * If we get an assert during the connection (after the
             * station has been added, but before the vif is set
             * to associated), mac80211 will re-add the station and
             * then configure the vif. Since the vif is not
             * associated, we would remove the station here and
             * this would fail the recovery.
             */
            if (!test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
                /*
                 * Remove AP station now that
                 * the MAC is unassoc
                 */
                ret = iwl_mvm_rm_sta_id(mvm, vif, mvmvif->ap_sta_id);
                if (ret) { IWL_ERR(mvm, "failed to remove AP station\n"); }

                if (mvm->d0i3_ap_sta_id == mvmvif->ap_sta_id) {
                    mvm->d0i3_ap_sta_id = IWL_MVM_INVALID_STA;
                }
                mvmvif->ap_sta_id = IWL_MVM_INVALID_STA;
            }

            /* remove quota for this interface */
            ret = iwl_mvm_update_quotas(mvm, false, NULL);
            if (ret) { IWL_ERR(mvm, "failed to update quotas\n"); }

            if (vif->p2p) { iwl_mvm_unref(mvm, IWL_MVM_REF_P2P_CLIENT); }

            /* this will take the cleared BSSID from bss_conf */
            ret = iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL);
            if (ret) {
                IWL_ERR(mvm, "failed to update MAC %pM (clear after unassoc)\n", vif->addr);
            }
        }

        /*
         * The firmware tracks the MU-MIMO group on its own.
         * However, on HW restart we should restore this data.
         */
        if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status) &&
            (changes & BSS_CHANGED_MU_GROUPS) && vif->mu_mimo_owner) {
            ret = iwl_mvm_update_mu_groups(mvm, vif);
            if (ret) { IWL_ERR(mvm, "failed to update VHT MU_MIMO groups\n"); }
        }

        iwl_mvm_recalc_multicast(mvm);
        iwl_mvm_configure_bcast_filter(mvm);

        /* reset rssi values */
        mvmvif->bf_data.ave_beacon_signal = 0;

        iwl_mvm_bt_coex_vif_change(mvm);
        iwl_mvm_update_smps(mvm, vif, IWL_MVM_SMPS_REQ_TT, IEEE80211_SMPS_AUTOMATIC);
        if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_UMAC_SCAN)) {
            iwl_mvm_config_scan(mvm);
        }
    }

    if (changes & BSS_CHANGED_BEACON_INFO) {
        /*
         * We received a beacon from the associated AP so
         * remove the session protection.
         */
        iwl_mvm_stop_session_protection(mvm, vif);

        iwl_mvm_sf_update(mvm, vif, false);
        WARN_ON(iwl_mvm_enable_beacon_filter(mvm, vif, 0));
    }

    if (changes & (BSS_CHANGED_PS | BSS_CHANGED_P2P_PS | BSS_CHANGED_QOS |
                   /*
                    * Send power command on every beacon change,
                    * because we may have not enabled beacon abort yet.
                    */
                   BSS_CHANGED_BEACON_INFO)) {
        ret = iwl_mvm_power_update_mac(mvm);
        if (ret) { IWL_ERR(mvm, "failed to update power mode\n"); }
    }

    if (changes & BSS_CHANGED_TXPOWER) {
        IWL_DEBUG_CALIB(mvm, "Changing TX Power to %d\n", bss_conf->txpower);
        iwl_mvm_set_tx_power(mvm, vif, bss_conf->txpower);
    }

    if (changes & BSS_CHANGED_CQM) {
        IWL_DEBUG_MAC80211(mvm, "cqm info_changed\n");
        /* reset cqm events tracking */
        mvmvif->bf_data.last_cqm_event = 0;
        if (mvmvif->bf_data.bf_enabled) {
            ret = iwl_mvm_enable_beacon_filter(mvm, vif, 0);
            if (ret) { IWL_ERR(mvm, "failed to update CQM thresholds\n"); }
        }
    }

    if (changes & BSS_CHANGED_ARP_FILTER) {
        IWL_DEBUG_MAC80211(mvm, "arp filter changed\n");
        iwl_mvm_configure_bcast_filter(mvm);
    }
}

static int iwl_mvm_start_ap_ibss(struct ieee80211_hw* hw, struct ieee80211_vif* vif) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    int ret;

    /*
     * iwl_mvm_mac_ctxt_add() might read directly from the device
     * (the system time), so make sure it is available.
     */
    ret = iwl_mvm_ref_sync(mvm, IWL_MVM_REF_START_AP);
    if (ret) { return ret; }

    mutex_lock(&mvm->mutex);

    /* Send the beacon template */
    ret = iwl_mvm_mac_ctxt_beacon_changed(mvm, vif);
    if (ret) { goto out_unlock; }

    /*
     * Re-calculate the tsf id, as the master-slave relations depend on the
     * beacon interval, which was not known when the AP interface was added.
     */
    if (vif->type == NL80211_IFTYPE_AP) { iwl_mvm_mac_ctxt_recalc_tsf_id(mvm, vif); }

    mvmvif->ap_assoc_sta_count = 0;

    /* Add the mac context */
    ret = iwl_mvm_mac_ctxt_add(mvm, vif);
    if (ret) { goto out_unlock; }

    /* Perform the binding */
    ret = iwl_mvm_binding_add_vif(mvm, vif);
    if (ret) { goto out_remove; }

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
    iwl_mvm_update_ctx_tx_power_limit(mvm, vif, mvmvif->phy_ctxt);
#endif

    /*
     * This is not very nice, but the simplest:
     * For older FWs adding the mcast sta before the bcast station may
     * cause assert 0x2b00.
     * This is fixed in later FW so make the order of removal depend on
     * the TLV
     */
    if (fw_has_api(&mvm->fw->ucode_capa, IWL_UCODE_TLV_API_STA_TYPE)) {
        ret = iwl_mvm_add_mcast_sta(mvm, vif);
        if (ret) { goto out_unbind; }
        /*
         * Send the bcast station. At this stage the TBTT and DTIM time
         * events are added and applied to the scheduler
         */
        ret = iwl_mvm_send_add_bcast_sta(mvm, vif);
        if (ret) {
            iwl_mvm_rm_mcast_sta(mvm, vif);
            goto out_unbind;
        }
    } else {
        /*
         * Send the bcast station. At this stage the TBTT and DTIM time
         * events are added and applied to the scheduler
         */
        ret = iwl_mvm_send_add_bcast_sta(mvm, vif);
        if (ret) { goto out_unbind; }
        ret = iwl_mvm_add_mcast_sta(mvm, vif);
        if (ret) {
            iwl_mvm_send_rm_bcast_sta(mvm, vif);
            goto out_unbind;
        }
    }

    /* must be set before quota calculations */
    mvmvif->ap_ibss_active = true;

    if (vif->type == NL80211_IFTYPE_AP && !vif->p2p) {
        iwl_mvm_vif_set_low_latency(mvmvif, true, LOW_LATENCY_VIF_TYPE);
        iwl_mvm_send_low_latency_cmd(mvm, true, mvmvif->id);
    }

    /* power updated needs to be done before quotas */
    iwl_mvm_power_update_mac(mvm);

    ret = iwl_mvm_update_quotas(mvm, false, NULL);
    if (ret) { goto out_quota_failed; }

    /* Need to update the P2P Device MAC (only GO, IBSS is single vif) */
    if (vif->p2p && mvm->p2p_device_vif) {
        iwl_mvm_mac_ctxt_changed(mvm, mvm->p2p_device_vif, false, NULL);
    }

    iwl_mvm_ref(mvm, IWL_MVM_REF_AP_IBSS);

    iwl_mvm_bt_coex_vif_change(mvm);

    /* we don't support TDLS during DCM */
    if (iwl_mvm_phy_ctx_count(mvm) > 1) { iwl_mvm_teardown_tdls_peers(mvm); }

    goto out_unlock;

out_quota_failed:
    iwl_mvm_power_update_mac(mvm);
    mvmvif->ap_ibss_active = false;
    iwl_mvm_send_rm_bcast_sta(mvm, vif);
    iwl_mvm_rm_mcast_sta(mvm, vif);
out_unbind:
    iwl_mvm_binding_remove_vif(mvm, vif);
out_remove:
    iwl_mvm_mac_ctxt_remove(mvm, vif);
out_unlock:
    mutex_unlock(&mvm->mutex);
    iwl_mvm_unref(mvm, IWL_MVM_REF_START_AP);
    return ret;
}

static void iwl_mvm_stop_ap_ibss(struct ieee80211_hw* hw, struct ieee80211_vif* vif) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);

    iwl_mvm_prepare_mac_removal(mvm, vif);

    mutex_lock(&mvm->mutex);

    /* Handle AP stop while in CSA */
    if (rcu_access_pointer(mvm->csa_vif) == vif) {
        iwl_mvm_remove_time_event(mvm, mvmvif, &mvmvif->time_event_data);
        RCU_INIT_POINTER(mvm->csa_vif, NULL);
        mvmvif->csa_countdown = false;
    }

    if (rcu_access_pointer(mvm->csa_tx_blocked_vif) == vif) {
        RCU_INIT_POINTER(mvm->csa_tx_blocked_vif, NULL);
        mvm->csa_tx_block_bcn_timeout = 0;
    }

    mvmvif->ap_ibss_active = false;
    mvm->ap_last_beacon_gp2 = 0;

    if (vif->type == NL80211_IFTYPE_AP && !vif->p2p) {
        iwl_mvm_vif_set_low_latency(mvmvif, false, LOW_LATENCY_VIF_TYPE);
        iwl_mvm_send_low_latency_cmd(mvm, false, mvmvif->id);
    }

    iwl_mvm_bt_coex_vif_change(mvm);

    iwl_mvm_unref(mvm, IWL_MVM_REF_AP_IBSS);

    /* Need to update the P2P Device MAC (only GO, IBSS is single vif) */
    if (vif->p2p && mvm->p2p_device_vif) {
        iwl_mvm_mac_ctxt_changed(mvm, mvm->p2p_device_vif, false, NULL);
    }

    iwl_mvm_update_quotas(mvm, false, NULL);

    /*
     * This is not very nice, but the simplest:
     * For older FWs removing the mcast sta before the bcast station may
     * cause assert 0x2b00.
     * This is fixed in later FW (which will stop beaconing when removing
     * bcast station).
     * So make the order of removal depend on the TLV
     */
    if (!fw_has_api(&mvm->fw->ucode_capa, IWL_UCODE_TLV_API_STA_TYPE)) {
        iwl_mvm_rm_mcast_sta(mvm, vif);
    }
    iwl_mvm_send_rm_bcast_sta(mvm, vif);
    if (fw_has_api(&mvm->fw->ucode_capa, IWL_UCODE_TLV_API_STA_TYPE)) {
        iwl_mvm_rm_mcast_sta(mvm, vif);
    }
    iwl_mvm_binding_remove_vif(mvm, vif);

    iwl_mvm_power_update_mac(mvm);

    iwl_mvm_mac_ctxt_remove(mvm, vif);

    kfree(mvmvif->ap_wep_key);
    mvmvif->ap_wep_key = NULL;

    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_bss_info_changed_ap_ibss(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                             struct ieee80211_bss_conf* bss_conf,
                                             uint32_t changes) {
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);

    /* Changes will be applied when the AP/IBSS is started */
    if (!mvmvif->ap_ibss_active) { return; }

    if (changes &
            (BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_HT | BSS_CHANGED_BANDWIDTH | BSS_CHANGED_QOS) &&
        iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL)) {
        IWL_ERR(mvm, "failed to update MAC %pM\n", vif->addr);
    }

    /* Need to send a new beacon template to the FW */
    if (changes & BSS_CHANGED_BEACON && iwl_mvm_mac_ctxt_beacon_changed(mvm, vif)) {
        IWL_WARN(mvm, "Failed updating beacon data\n");
    }

    if (changes & BSS_CHANGED_TXPOWER) {
        IWL_DEBUG_CALIB(mvm, "Changing TX Power to %d\n", bss_conf->txpower);
        iwl_mvm_set_tx_power(mvm, vif, bss_conf->txpower);
    }
}

static void iwl_mvm_bss_info_changed(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                     struct ieee80211_bss_conf* bss_conf, uint32_t changes) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    /*
     * iwl_mvm_bss_info_changed_station() might call
     * iwl_mvm_protect_session(), which reads directly from
     * the device (the system time), so make sure it is available.
     */
    if (iwl_mvm_ref_sync(mvm, IWL_MVM_REF_BSS_CHANGED)) { return; }

    mutex_lock(&mvm->mutex);

    if (changes & BSS_CHANGED_IDLE && !bss_conf->idle) {
        iwl_mvm_scan_stop(mvm, IWL_MVM_SCAN_SCHED, true);
    }

    switch (vif->type) {
    case NL80211_IFTYPE_STATION:
        iwl_mvm_bss_info_changed_station(mvm, vif, bss_conf, changes);
        break;
    case NL80211_IFTYPE_AP:
    case NL80211_IFTYPE_ADHOC:
        iwl_mvm_bss_info_changed_ap_ibss(mvm, vif, bss_conf, changes);
        break;
    case NL80211_IFTYPE_MONITOR:
        if (changes & BSS_CHANGED_MU_GROUPS) { iwl_mvm_update_mu_groups(mvm, vif); }
        break;
    default:
        /* shouldn't happen */
        WARN_ON_ONCE(1);
    }

    mutex_unlock(&mvm->mutex);
    iwl_mvm_unref(mvm, IWL_MVM_REF_BSS_CHANGED);
}
#endif  // NEEDS_PORTING

// Modified from original iwl_mvm_mac_hw_scan() to split call path for active and passive.
zx_status_t iwl_mvm_mac_hw_scan_passive(struct iwl_mvm_vif* mvmvif,
                                        const wlan_softmac_passive_scan_args_t* passive_scan_args,
                                        uint64_t* out_scan_id) {
  struct iwl_mvm* mvm = mvmvif->mvm;
  zx_status_t ret;

  if (passive_scan_args->channels_count == 0 ||
      passive_scan_args->channels_count > mvm->fw->ucode_capa.n_scan_channels) {
    IWL_WARN(mvmvif, "Cannot scan: invalid #channel (%zu). FW's cap (%d)\n",
             passive_scan_args->channels_count, mvm->fw->ucode_capa.n_scan_channels);
    return ZX_ERR_INVALID_ARGS;
  }

  mtx_lock(&mvm->mutex);
  ret = iwl_mvm_reg_scan_start_passive(mvmvif, passive_scan_args);
  mtx_unlock(&mvm->mutex);

  if (ret != ZX_OK) {
    return ret;
  }

  // TODO(fxbug.dev/88934): scan_id is always 0
  *out_scan_id = 0;
  return ret;
}

#if 0   // NEEDS_PORTING
static void iwl_mvm_mac_cancel_hw_scan(struct ieee80211_hw* hw, struct ieee80211_vif* vif) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    mutex_lock(&mvm->mutex);

    /* Due to a race condition, it's possible that mac80211 asks
     * us to stop a hw_scan when it's already stopped.  This can
     * happen, for instance, if we stopped the scan ourselves,
     * called ieee80211_scan_completed() and the userspace called
     * cancel scan scan before ieee80211_scan_work() could run.
     * To handle that, simply return if the scan is not running.
     */
    if (mvm->scan_status & IWL_MVM_SCAN_REGULAR) {
        iwl_mvm_scan_stop(mvm, IWL_MVM_SCAN_REGULAR, true);
    }

    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_mac_allow_buffered_frames(struct ieee80211_hw* hw, struct ieee80211_sta* sta,
                                              uint16_t tids, int num_frames,
                                              enum ieee80211_frame_release_type reason,
                                              bool more_data) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    /* Called when we need to transmit (a) frame(s) from mac80211 */

    iwl_mvm_sta_modify_sleep_tx_count(mvm, sta, reason, num_frames, tids, more_data, false);
}

static void iwl_mvm_mac_release_buffered_frames(struct ieee80211_hw* hw, struct ieee80211_sta* sta,
                                                uint16_t tids, int num_frames,
                                                enum ieee80211_frame_release_type reason,
                                                bool more_data) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    /* Called when we need to transmit (a) frame(s) from agg or dqa queue */

    iwl_mvm_sta_modify_sleep_tx_count(mvm, sta, reason, num_frames, tids, more_data, true);
}

static void __iwl_mvm_mac_sta_notify(struct ieee80211_hw* hw, enum sta_notify_cmd cmd,
                                     struct ieee80211_sta* sta) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_sta* mvmsta = iwl_mvm_sta_from_mac80211(sta);
    unsigned long txqs = 0, tids = 0;
    int tid;

    /*
     * If we have TVQM then we get too high queue numbers - luckily
     * we really shouldn't get here with that because such hardware
     * should have firmware supporting buffer station offload.
     */
    if (WARN_ON(iwl_mvm_has_new_tx_api(mvm))) { return; }

    spin_lock_bh(&mvmsta->lock);
    for (tid = 0; tid < IWL_MAX_TID_COUNT; tid++) {
        struct iwl_mvm_tid_data* tid_data = &mvmsta->tid_data[tid];

        if (tid_data->txq_id == IWL_MVM_INVALID_QUEUE) { continue; }

        __set_bit(tid_data->txq_id, &txqs);

        if (iwl_mvm_tid_queued(mvm, tid_data) == 0) { continue; }

        __set_bit(tid, &tids);
    }

    switch (cmd) {
    case STA_NOTIFY_SLEEP:
        for_each_set_bit(tid, &tids, IWL_MAX_TID_COUNT) ieee80211_sta_set_buffered(sta, tid, true);

        if (txqs) { iwl_trans_freeze_txq_timer(mvm->trans, txqs, true); }
        /*
         * The fw updates the STA to be asleep. Tx packets on the Tx
         * queues to this station will not be transmitted. The fw will
         * send a Tx response with TX_STATUS_FAIL_DEST_PS.
         */
        break;
    case STA_NOTIFY_AWAKE:
        if (WARN_ON(mvmsta->sta_id == IWL_MVM_INVALID_STA)) { break; }

        if (txqs) { iwl_trans_freeze_txq_timer(mvm->trans, txqs, false); }
        iwl_mvm_sta_modify_ps_wake(mvm, sta);
        break;
    default:
        break;
    }
    spin_unlock_bh(&mvmsta->lock);
}

static void iwl_mvm_mac_sta_notify(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                   enum sta_notify_cmd cmd, struct ieee80211_sta* sta) {
    __iwl_mvm_mac_sta_notify(hw, cmd, sta);
}

void iwl_mvm_sta_pm_notif(struct iwl_mvm* mvm, struct iwl_rx_cmd_buffer* rxb) {
    struct iwl_rx_packet* pkt = rxb_addr(rxb);
    struct iwl_mvm_pm_state_notification* notif = (void*)pkt->data;
    struct ieee80211_sta* sta;
    struct iwl_mvm_sta* mvmsta;
    bool sleeping = (notif->type != IWL_MVM_PM_EVENT_AWAKE);

    if (WARN_ON(notif->sta_id >= ARRAY_SIZE(mvm->fw_id_to_mac_id))) { return; }

    rcu_read_lock();
    sta = rcu_dereference(mvm->fw_id_to_mac_id[notif->sta_id]);
    if (WARN_ON(IS_ERR_OR_NULL(sta))) {
        rcu_read_unlock();
        return;
    }

    mvmsta = iwl_mvm_sta_from_mac80211(sta);

    if (!mvmsta->vif || mvmsta->vif->type != NL80211_IFTYPE_AP) {
        rcu_read_unlock();
        return;
    }

    if (mvmsta->sleeping != sleeping) {
        mvmsta->sleeping = sleeping;
        __iwl_mvm_mac_sta_notify(mvm->hw, sleeping ? STA_NOTIFY_SLEEP : STA_NOTIFY_AWAKE, sta);
        ieee80211_sta_ps_transition(sta, sleeping);
    }

    if (sleeping) {
        switch (notif->type) {
        case IWL_MVM_PM_EVENT_AWAKE:
        case IWL_MVM_PM_EVENT_ASLEEP:
            break;
        case IWL_MVM_PM_EVENT_UAPSD:
            ieee80211_sta_uapsd_trigger(sta, IEEE80211_TIDS_MAX);
            break;
        case IWL_MVM_PM_EVENT_PS_POLL:
            ieee80211_sta_pspoll(sta);
            break;
        default:
            break;
        }
    }

    rcu_read_unlock();
}

static void iwl_mvm_sta_pre_rcu_remove(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                       struct ieee80211_sta* sta) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_sta* mvm_sta = iwl_mvm_sta_from_mac80211(sta);

    /*
     * This is called before mac80211 does RCU synchronisation,
     * so here we already invalidate our internal RCU-protected
     * station pointer. The rest of the code will thus no longer
     * be able to find the station this way, and we don't rely
     * on further RCU synchronisation after the sta_state()
     * callback deleted the station.
     */
    mutex_lock(&mvm->mutex);
    if (sta == rcu_access_pointer(mvm->fw_id_to_mac_id[mvm_sta->sta_id])) {
        rcu_assign_pointer(mvm->fw_id_to_mac_id[mvm_sta->sta_id], ERR_PTR(-ENOENT));
    }

    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_check_uapsd(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                const uint8_t* bssid) {
    int i;

    if (!test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
        struct iwl_mvm_tcm_mac* mdata;

        mdata = &mvm->tcm.data[iwl_mvm_vif_from_mac80211(vif)->id];
        ewma_rate_init(&mdata->uapsd_nonagg_detect.rate);
        mdata->opened_rx_ba_sessions = false;
    }

    if (!(mvm->fw->ucode_capa.flags & IWL_UCODE_TLV_FLAGS_UAPSD_SUPPORT)) { return; }

    if (vif->p2p && !iwl_mvm_is_p2p_scm_uapsd_supported(mvm)) {
        vif->driver_flags &= ~IEEE80211_VIF_SUPPORTS_UAPSD;
        return;
    }

    if (!vif->p2p && (iwlwifi_mod_params.uapsd_disable & IWL_DISABLE_UAPSD_BSS)) {
        vif->driver_flags &= ~IEEE80211_VIF_SUPPORTS_UAPSD;
        return;
    }

    for (i = 0; i < IWL_MVM_UAPSD_NOAGG_LIST_LEN; i++) {
        if (ether_addr_equal(mvm->uapsd_noagg_bssids[i].addr, bssid)) {
            vif->driver_flags &= ~IEEE80211_VIF_SUPPORTS_UAPSD;
            return;
        }
    }

    vif->driver_flags |= IEEE80211_VIF_SUPPORTS_UAPSD;
}

static void iwl_mvm_tdls_check_trigger(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                       uint8_t* peer_addr, enum nl80211_tdls_operation action) {
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct iwl_fw_dbg_trigger_tdls* tdls_trig;

    trig = iwl_fw_dbg_trigger_on(&mvm->fwrt, ieee80211_vif_to_wdev(vif), FW_DBG_TRIGGER_TDLS);
    if (!trig) { return; }

    tdls_trig = (void*)trig->data;

    if (!(tdls_trig->action_bitmap & BIT(action))) { return; }

    if (tdls_trig->peer_mode && memcmp(tdls_trig->peer, peer_addr, ETH_ALEN) != 0) { return; }

    iwl_fw_dbg_collect_trig(&mvm->fwrt, trig, "TDLS event occurred, peer %pM, action %d", peer_addr,
                            action);
}
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_mac_sta_state(struct iwl_mvm_vif* mvmvif, struct iwl_mvm_sta* mvm_sta,
                                  enum iwl_sta_state old_state, enum iwl_sta_state new_state) {
  struct iwl_mvm* mvm = mvmvif->mvm;
  zx_status_t ret;

  IWL_DEBUG_MAC80211(mvm, "station state change %d->%d\n", old_state, new_state);

  /* this would be a mac80211 bug ... but don't crash */
  if (!mvmvif->phy_ctxt) {
    return ZX_ERR_BAD_STATE;
  }

#if 0   // NEEDS_PORTING
    /*
     * If we are in a STA removal flow and in DQA mode:
     *
     * This is after the sync_rcu part, so the queues have already been
     * flushed. No more TXs on their way in mac80211's path, and no more in
     * the queues.
     * Also, we won't be getting any new TX frames for this station.
     * What we might have are deferred TX frames that need to be taken care
     * of.
     *
     * Drop any still-queued deferred-frame before removing the STA, and
     * make sure the worker is no longer handling frames for this STA.
     */
    if (old_state == IEEE80211_STA_NONE && new_state == IEEE80211_STA_NOTEXIST) {
        flush_work(&mvm->add_stream_wk);

        /*
         * No need to make sure deferred TX indication is off since the
         * worker will already remove it if it was on
         */
    }
#endif  // NEEDS_PORTING

  mtx_lock(&mvm->mutex);
  /* track whether or not the station is associated */
  mvm_sta->sta_state = new_state;

  if (old_state == IWL_STA_NOTEXIST && new_state == IWL_STA_NONE) {
    /*
     * Firmware bug - it'll crash if the beacon interval is less
     * than 16. We can't avoid connecting at all, so refuse the
     * station state change, this will cause mac80211 to abandon
     * attempts to connect to this AP, and eventually wpa_s will
     * blacklist the AP...
     */
    if (mvmvif->mac_role == WLAN_INFO_MAC_ROLE_CLIENT && mvmvif->bss_conf.beacon_int < 16) {
      IWL_ERR(mvm, "AP %pM beacon interval is %d, refusing due to firmware bug!\n", mvm_sta->addr,
              mvmvif->bss_conf.beacon_int);
      ret = ZX_ERR_INVALID_ARGS;
      goto out_unlock;
    }

#if 0   // NEEDS_PORTING
        if (sta->tdls && (vif->p2p || iwl_mvm_tdls_sta_count(mvm, NULL) == IWL_MVM_TDLS_STA_COUNT ||
                          iwl_mvm_phy_ctx_count(mvm) > 1)) {
            IWL_DEBUG_MAC80211(mvm, "refusing TDLS sta\n");
            ret = -EBUSY;
            goto out_unlock;
        }
#endif  // NEEDS_PORTING

    ret = iwl_mvm_add_sta(mvmvif, mvm_sta);
#if 0   // NEEDS_PORTING
        if (sta->tdls && ret == 0) {
            iwl_mvm_recalc_tdls_state(mvm, vif, true);
            iwl_mvm_tdls_check_trigger(mvm, vif, sta->addr, NL80211_TDLS_SETUP);
        }

        sta->max_rc_amsdu_len = 1;
#endif  // NEEDS_PORTING
  } else if (old_state == IWL_STA_NONE && new_state == IWL_STA_AUTH) {
#if 0   // NEEDS_PORTING
        /*
         * EBS may be disabled due to previous failures reported by FW.
         * Reset EBS status here assuming environment has been changed.
         */
        mvm->last_ebs_successful = true;
        iwl_mvm_check_uapsd(mvm, vif, sta->addr);
#endif  // NEEDS_PORTING

    ret = ZX_OK;

  } else if (old_state == IWL_STA_AUTH && new_state == IWL_STA_ASSOC) {
#if 0   // NEEDS_PORTING
        // TODO(36677): Supports AP role
        if (mvmvif->mac_role == WLAN_INFO_MAC_ROLE_AP) {
            mvmvif->ap_assoc_sta_count++;
            iwl_mvm_mac_ctxt_changed(mvmvif, false, NULL);
            if (vif->bss_conf.he_support && !iwlwifi_mod_params.disable_11ax) {
                iwl_mvm_cfg_he_sta(mvmvif, mvm_sta->sta_id);
            }
        }

        iwl_mvm_rs_rate_init(mvm, sta, mvmvif->phy_ctxt->channel->band, false);
#endif  // NEEDS_PORTING

    ret = iwl_mvm_update_sta(mvm, mvm_sta);

  } else if (old_state == IWL_STA_ASSOC && new_state == IWL_STA_AUTHORIZED) {
#if 0   // NEEDS_PORTING
        /* we don't support TDLS during DCM */
        if (iwl_mvm_phy_ctx_count(mvm) > 1) { iwl_mvm_teardown_tdls_peers(mvm); }

        if (sta->tdls) {
            iwl_mvm_tdls_check_trigger(mvm, vif, sta->addr, NL80211_TDLS_ENABLE_LINK);
        }
#endif  // NEEDS_PORTING

    /* enable beacon filtering */
    if (ZX_OK != iwl_mvm_enable_beacon_filter(mvmvif, 0)) {
      IWL_WARN(mvm, "cannot enable beacon filter\n");
    }
    ret = ZX_OK;

#if 0   // NEEDS_PORTING
        iwl_mvm_rs_rate_init(mvm, sta, mvmvif->phy_ctxt->channel->band, true);

        // TODO(36677): Supports AP role
        /* if wep is used, need to set the key for the station now */
        if (mvmvif->mac_role == WLAN_INFO_MAC_ROLE_AP && mvmvif->ap_wep_key) {
            ret = iwl_mvm_set_sta_key(mvm, vif, sta, mvmvif->ap_wep_key, STA_KEY_IDX_INVALID);
        } else {
            ret = ZX_OK;
        }
#endif  // NEEDS_PORTING
  } else if (old_state == IWL_STA_AUTHORIZED && new_state == IWL_STA_ASSOC) {
    /* disable beacon filtering */
    if (ZX_OK != iwl_mvm_disable_beacon_filter(mvmvif, 0)) {
      IWL_WARN(mvm, "cannot enable beacon filter\n");
    }
    ret = ZX_OK;
  } else if (old_state == IWL_STA_ASSOC && new_state == IWL_STA_AUTH) {
#if 0   // NEEDS_PORTING
        // TODO(36677): Supports AP role
        if (mvmvif->mac_role == WLAN_INFO_MAC_ROLE_AP) {
            mvmvif->ap_assoc_sta_count--;
            iwl_mvm_mac_ctxt_changed(mvmvif, false, NULL);
        }
#endif  // NEEDS_PORTING
    ret = ZX_OK;
  } else if (old_state == IWL_STA_AUTH && new_state == IWL_STA_NONE) {
    ret = ZX_OK;
  } else if (old_state == IWL_STA_NONE && new_state == IWL_STA_NOTEXIST) {
    // Delete all set keys
    // TODO(fxbug.dev/86728): remove the WPA2 key workaround
    for (int i = 0; i < STA_KEY_MAX_NUM; i++) {
      if (mvm->active_key_list[i].keylen) {
        // delete the key if present
        if (iwl_mvm_remove_sta_key(mvmvif, mvm_sta, &mvm->active_key_list[i]) != ZX_OK) {
          IWL_ERR(mvm, "Unable to delete key at offset %d", i);
        }
        memset(&mvm->active_key_list[i], 0, sizeof(struct iwl_mvm_sta_key_conf));
      }
    }
    ret = iwl_mvm_rm_sta(mvmvif, mvm_sta);
#if 0   // NEEDS_PORTING
        if (sta->tdls) {
            iwl_mvm_recalc_tdls_state(mvm, vif, false);
            iwl_mvm_tdls_check_trigger(mvm, vif, sta->addr, NL80211_TDLS_DISABLE_LINK);
        }
#endif  // NEEDS_PORTING
  } else {
    IWL_ERR(mvmvif, "set_state(): state transition is invalid (%d -> %d).\n", old_state, new_state);
    ret = ZX_ERR_IO;
  }
out_unlock:
  mtx_unlock(&mvm->mutex);

#if 0   // NEEDS_PORTING
    if (sta->tdls && ret == 0) {
        if (old_state == IWL_STA_NOTEXIST && new_state == IWL_STA_NONE) {
            ieee80211_reserve_tid(sta, IWL_MVM_TDLS_FW_TID);
        } else if (old_state == IWL_STA_NONE && new_state == IWL_STA_NOTEXIST) {
            ieee80211_unreserve_tid(sta, IWL_MVM_TDLS_FW_TID);
        }
    }
#endif  // NEEDS_PORTING

  return ret;
}

#if 0   // NEEDS_PORTING
static int iwl_mvm_mac_set_rts_threshold(struct ieee80211_hw* hw, uint32_t value) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    mvm->rts_threshold = value;

    return 0;
}

static void iwl_mvm_sta_rc_update(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                  struct ieee80211_sta* sta, uint32_t changed) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    if (vif->type == NL80211_IFTYPE_STATION && changed & IEEE80211_RC_NSS_CHANGED) {
        iwl_mvm_sf_update(mvm, vif, false);
    }
}

static int iwl_mvm_mac_conf_tx(struct ieee80211_hw* hw, struct ieee80211_vif* vif, uint16_t ac,
                               const struct ieee80211_tx_queue_params* params) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);

    mvmvif->queue_params[ac] = *params;

    /*
     * No need to update right away, we'll get BSS_CHANGED_QOS
     * The exception is P2P_DEVICE interface which needs immediate update.
     */
    if (vif->type == NL80211_IFTYPE_P2P_DEVICE) {
        int ret;

        mutex_lock(&mvm->mutex);
        ret = iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL);
        mutex_unlock(&mvm->mutex);
        return ret;
    }
    return 0;
}
#endif  // NEEDS_PORTING

// Prepare for transmitting a management frame for association before associated.
//
// This function is used to tell firmware to sync the channel time.
//
void iwl_mvm_mac_mgd_prepare_tx(struct iwl_mvm* mvm, struct iwl_mvm_vif* mvmvif,
                                uint16_t req_duration) {
  uint32_t duration = IWL_MVM_TE_SESSION_PROTECTION_MAX_TIME_MS;
  uint32_t min_duration = IWL_MVM_TE_SESSION_PROTECTION_MIN_TIME_MS;

  /*
   * iwl_mvm_protect_session() reads directly from the device
   * (the system time), so make sure it is available.
   */
  if (iwl_mvm_ref_sync(mvm, IWL_MVM_REF_PREPARE_TX)) {
    return;
  }

  if (req_duration > duration) {
    duration = req_duration;
  }

  mtx_lock(&mvm->mutex);
  /* Try really hard to protect the session and hear a beacon */
  iwl_mvm_protect_session(mvm, mvmvif, duration, min_duration, 500, false);
  mtx_unlock(&mvm->mutex);

  iwl_mvm_unref(mvm, IWL_MVM_REF_PREPARE_TX);
}

#if 0   // NEEDS_PORTING
static int iwl_mvm_mac_sched_scan_start(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                        struct cfg80211_sched_scan_request* req,
                                        struct ieee80211_scan_ies* ies) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    int ret;

    mutex_lock(&mvm->mutex);

    if (!vif->bss_conf.idle) {
        ret = -EBUSY;
        goto out;
    }

    ret = iwl_mvm_sched_scan_start(mvm, vif, req, ies, IWL_MVM_SCAN_SCHED);

out:
    mutex_unlock(&mvm->mutex);
    return ret;
}

static int iwl_mvm_mac_sched_scan_stop(struct ieee80211_hw* hw, struct ieee80211_vif* vif) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int ret;

    mutex_lock(&mvm->mutex);

    /* Due to a race condition, it's possible that mac80211 asks
     * us to stop a sched_scan when it's already stopped.  This
     * can happen, for instance, if we stopped the scan ourselves,
     * called ieee80211_sched_scan_stopped() and the userspace called
     * stop sched scan scan before ieee80211_sched_scan_stopped_work()
     * could run.  To handle this, simply return if the scan is
     * not running.
     */
    if (!(mvm->scan_status & IWL_MVM_SCAN_SCHED)) {
        mutex_unlock(&mvm->mutex);
        return 0;
    }

    ret = iwl_mvm_scan_stop(mvm, IWL_MVM_SCAN_SCHED, false);
    mutex_unlock(&mvm->mutex);
    iwl_mvm_wait_for_async_handlers(mvm);

    return ret;
}
#endif  // NEEDS_PORTING

zx_status_t iwl_mvm_mac_set_key(struct iwl_mvm_vif* mvmvif, struct iwl_mvm_sta* mvmsta,
                                const struct iwl_mvm_sta_key_conf* key) {
  zx_status_t ret = ZX_OK;
  struct iwl_mvm* mvm = mvmvif->mvm;
  struct iwl_mvm_key_pn* ptk_pn;
  uint8_t key_offset = 0;

  if (iwlwifi_mod_params.swcrypto) {
    IWL_DEBUG_MAC80211(mvm, "leave - hwcrypto disabled\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  switch (key->cipher_type) {
    case fuchsia_wlan_ieee80211_CipherSuiteType_CCMP_128:
      if (iwl_mvm_has_new_tx_api(mvm)) {
        return ZX_ERR_NOT_SUPPORTED;
      }
      break;
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }

  mtx_lock(&mvm->mutex);

  // Porting note: the following is the equivalent of just the SET_KEY path, as Fuchsia does not
  // have the equivalent to a DELETE_KEY call.

  if ((mvmvif->mac_role == WLAN_INFO_MAC_ROLE_MESH || mvmvif->mac_role == WLAN_INFO_MAC_ROLE_AP) &&
      !mvmsta) {
    /*
     * GTK on AP interface is a TX-only key, return 0;
     * on IBSS they're per-station and because we're lazy
     * we don't support them for RX, so do the same.
     * CMAC/GMAC in AP/IBSS modes must be done in software.
     */
    if (key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_BIP_CMAC_128 ||
        key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_BIP_GMAC_128 ||
        key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_BIP_GMAC_256) {
      ret = ZX_ERR_NOT_SUPPORTED;
    } else {
      ret = ZX_OK;
    }
  }

  if (!test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status) && mvmsta &&
      iwl_mvm_has_new_rx_api(mvm) && key->key_type == WLAN_KEY_TYPE_PAIRWISE &&
      (key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_CCMP_128 ||

       key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_GCMP_128 ||
       key->cipher_type == fuchsia_wlan_ieee80211_CipherSuiteType_GCMP_256)) {
    int tid, q;

    ptk_pn = calloc(1, sizeof(*ptk_pn) + sizeof(ptk_pn->q->pn) * mvm->trans->num_rx_queues);
    if (!ptk_pn) {
      ret = ZX_ERR_NO_MEMORY;
      goto out;
    }

    for (tid = 0; tid < IWL_MAX_TID_COUNT; tid++) {
      for (q = 0; q < mvm->trans->num_rx_queues; q++) {
        memset(ptk_pn->q[q].pn[tid], 0, IEEE80211_CCMP_PN_LEN);
      }
    }

    struct iwl_mvm_key_pn* old_ptk_pn = iwl_rcu_exchange(mvmsta->ptk_pn[key->keyidx], ptk_pn);
    if (old_ptk_pn) {
      iwl_rcu_free_sync(mvm->dev, old_ptk_pn);
    }
  }

  /* in HW restart reuse the index, otherwise request a new one */
  if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
    key_offset = 0;
  } else {
    key_offset = STA_KEY_IDX_INVALID;
  }

  IWL_DEBUG_MAC80211(mvm, "set hwcrypto key\n");
  ret = iwl_mvm_set_sta_key(mvm, mvmvif, mvmsta, key, key_offset);
  if (ret != ZX_OK) {
    IWL_ERR(mvm, "set key failed: %s\n", zx_status_get_string(ret));
    /*
     * can't add key for RX, but we don't need it
     * in the device for TX so still return 0
     */
    ret = ZX_OK;
    goto out;
  }

out:
  mtx_unlock(&mvm->mutex);
  return ret;
}

#if 0  // NEEDS_PORTING
static void iwl_mvm_mac_update_tkip_key(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                        struct ieee80211_key_conf* keyconf,
                                        struct ieee80211_sta* sta, uint32_t iv32,
                                        uint16_t* phase1key) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    if (keyconf->hw_key_idx == STA_KEY_IDX_INVALID) { return; }

    iwl_mvm_update_tkip_key(mvm, vif, keyconf, sta, iv32, phase1key);
}

static bool iwl_mvm_rx_aux_roc(struct iwl_notif_wait_data* notif_wait, struct iwl_rx_packet* pkt,
                               void* data) {
    struct iwl_mvm* mvm = container_of(notif_wait, struct iwl_mvm, notif_wait);
    struct iwl_hs20_roc_res* resp;
    int resp_len = iwl_rx_packet_payload_len(pkt);
    struct iwl_mvm_time_event_data* te_data = data;

    if (WARN_ON(pkt->hdr.cmd != HOT_SPOT_CMD)) { return true; }

    if (WARN_ON_ONCE(resp_len != sizeof(*resp))) {
        IWL_ERR(mvm, "Invalid HOT_SPOT_CMD response\n");
        return true;
    }

    resp = (void*)pkt->data;

    IWL_DEBUG_TE(mvm, "Aux ROC: Received response from ucode: status=%d uid=%d\n", resp->status,
                 resp->event_unique_id);

    te_data->uid = le32_to_cpu(resp->event_unique_id);
    IWL_DEBUG_TE(mvm, "TIME_EVENT_CMD response - UID = 0x%x\n", te_data->uid);

    spin_lock_bh(&mvm->time_event_lock);
    list_add_tail(&te_data->list, &mvm->aux_roc_te_list);
    spin_unlock_bh(&mvm->time_event_lock);

    return true;
}

#define AUX_ROC_MIN_DURATION MSEC_TO_TU(100)
#define AUX_ROC_MIN_DELAY MSEC_TO_TU(200)
#define AUX_ROC_MAX_DELAY MSEC_TO_TU(600)
#define AUX_ROC_SAFETY_BUFFER MSEC_TO_TU(20)
#define AUX_ROC_MIN_SAFETY_BUFFER MSEC_TO_TU(10)
static int iwl_mvm_send_aux_roc_cmd(struct iwl_mvm* mvm, struct ieee80211_channel* channel,
                                    struct ieee80211_vif* vif, int duration) {
    int res, time_reg = DEVICE_SYSTEM_TIME_REG;
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct iwl_mvm_time_event_data* te_data = &mvmvif->hs_time_event_data;
    static const uint16_t time_event_response[] = {HOT_SPOT_CMD};
    struct iwl_notification_wait wait_time_event;
    uint32_t dtim_interval = vif->bss_conf.dtim_period * vif->bss_conf.beacon_int;
    uint32_t req_dur, delay;
    struct iwl_hs20_roc_req aux_roc_req = {
        .action = cpu_to_le32(FW_CTXT_ACTION_ADD),
        .id_and_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(MAC_INDEX_AUX, 0)),
        .sta_id_and_color = cpu_to_le32(mvm->aux_sta.sta_id),
        /* Set the channel info data */
        .channel_info.band = (channel->band == NL80211_BAND_2GHZ) ? PHY_BAND_24 : PHY_BAND_5,
        .channel_info.channel = channel->hw_value,
        .channel_info.width = PHY_VHT_CHANNEL_MODE20,
        /* Set the time and duration */
        .apply_time = cpu_to_le32(iwl_read_prph(mvm->trans, time_reg)),
    };

    delay = AUX_ROC_MIN_DELAY;
    req_dur = MSEC_TO_TU(duration);

    /*
     * If we are associated we want the delay time to be at least one
     * dtim interval so that the FW can wait until after the DTIM and
     * then start the time event, this will potentially allow us to
     * remain off-channel for the max duration.
     * Since we want to use almost a whole dtim interval we would also
     * like the delay to be for 2-3 dtim intervals, in case there are
     * other time events with higher priority.
     */
    if (vif->bss_conf.assoc) {
        delay = min_t(uint32_t, dtim_interval * 3, AUX_ROC_MAX_DELAY);
        /* We cannot remain off-channel longer than the DTIM interval */
        if (dtim_interval <= req_dur) {
            req_dur = dtim_interval - AUX_ROC_SAFETY_BUFFER;
            if (req_dur <= AUX_ROC_MIN_DURATION) {
                req_dur = dtim_interval - AUX_ROC_MIN_SAFETY_BUFFER;
            }
        }
    }

    aux_roc_req.duration = cpu_to_le32(req_dur);
    aux_roc_req.apply_time_max_delay = cpu_to_le32(delay);

    IWL_DEBUG_TE(mvm,
                 "ROC: Requesting to remain on channel %u for %ums (requested = %ums, max_delay = "
                 "%ums, dtim_interval = %ums)\n",
                 channel->hw_value, req_dur, duration, delay, dtim_interval);
    /* Set the node address */
    memcpy(aux_roc_req.node_addr, vif->addr, ETH_ALEN);

    iwl_assert_lock_held(&mvm->mutex);

    spin_lock_bh(&mvm->time_event_lock);

    if (WARN_ON(te_data->id == HOT_SPOT_CMD)) {
        spin_unlock_bh(&mvm->time_event_lock);
        return -EIO;
    }

    te_data->vif = vif;
    te_data->duration = duration;
    te_data->id = HOT_SPOT_CMD;

    spin_unlock_bh(&mvm->time_event_lock);

    /*
     * Use a notification wait, which really just processes the
     * command response and doesn't wait for anything, in order
     * to be able to process the response and get the UID inside
     * the RX path. Using CMD_WANT_SKB doesn't work because it
     * stores the buffer and then wakes up this thread, by which
     * time another notification (that the time event started)
     * might already be processed unsuccessfully.
     */
    iwl_init_notification_wait(&mvm->notif_wait, &wait_time_event, time_event_response,
                               ARRAY_SIZE(time_event_response), iwl_mvm_rx_aux_roc, te_data);

    res = iwl_mvm_send_cmd_pdu(mvm, HOT_SPOT_CMD, 0, sizeof(aux_roc_req), &aux_roc_req);

    if (res) {
        IWL_ERR(mvm, "Couldn't send HOT_SPOT_CMD: %d\n", res);
        iwl_remove_notification(&mvm->notif_wait, &wait_time_event);
        goto out_clear_te;
    }

    /* No need to wait for anything, so just pass 1 (0 isn't valid) */
    res = iwl_wait_notification(&mvm->notif_wait, &wait_time_event, 1);
    /* should never fail */
    WARN_ON_ONCE(res);

    if (res) {
    out_clear_te:
        spin_lock_bh(&mvm->time_event_lock);
        iwl_mvm_te_clear_data(mvm, te_data);
        spin_unlock_bh(&mvm->time_event_lock);
    }

    return res;
}

static int iwl_mvm_roc(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                       struct ieee80211_channel* channel, int duration,
                       enum ieee80211_roc_type type) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct cfg80211_chan_def chandef;
    struct iwl_mvm_phy_ctxt* phy_ctxt;
    int ret, i;

    IWL_DEBUG_MAC80211(mvm, "enter (%d, %d, %d)\n", channel->hw_value, duration, type);

    /*
     * Flush the done work, just in case it's still pending, so that
     * the work it does can complete and we can accept new frames.
     */
    flush_work(&mvm->roc_done_wk);

    mutex_lock(&mvm->mutex);

    switch (vif->type) {
    case NL80211_IFTYPE_STATION:
        if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_HOTSPOT_SUPPORT)) {
            /* Use aux roc framework (HS20) */
            ret = iwl_mvm_send_aux_roc_cmd(mvm, channel, vif, duration);
            goto out_unlock;
        }
        IWL_ERR(mvm, "hotspot not supported\n");
        ret = -EINVAL;
        goto out_unlock;
    case NL80211_IFTYPE_P2P_DEVICE:
        /* handle below */
        break;
    default:
        IWL_ERR(mvm, "vif isn't P2P_DEVICE: %d\n", vif->type);
        ret = -EINVAL;
        goto out_unlock;
    }

    for (i = 0; i < NUM_PHY_CTX; i++) {
        phy_ctxt = &mvm->phy_ctxts[i];
        if (phy_ctxt->ref == 0 || mvmvif->phy_ctxt == phy_ctxt) { continue; }

        if (phy_ctxt->ref && channel == phy_ctxt->channel) {
            /*
             * Unbind the P2P_DEVICE from the current PHY context,
             * and if the PHY context is not used remove it.
             */
            ret = iwl_mvm_binding_remove_vif(mvm, vif);
            if (WARN(ret, "Failed unbinding P2P_DEVICE\n")) { goto out_unlock; }

            iwl_mvm_phy_ctxt_unref(mvm, mvmvif->phy_ctxt);

            /* Bind the P2P_DEVICE to the current PHY Context */
            mvmvif->phy_ctxt = phy_ctxt;

            ret = iwl_mvm_binding_add_vif(mvm, vif);
            if (WARN(ret, "Failed binding P2P_DEVICE\n")) { goto out_unlock; }

            iwl_mvm_phy_ctxt_ref(mvm, mvmvif->phy_ctxt);
            goto schedule_time_event;
        }
    }

    /* Need to update the PHY context only if the ROC channel changed */
    if (channel == mvmvif->phy_ctxt->channel) { goto schedule_time_event; }

    cfg80211_chandef_create(&chandef, channel, NL80211_CHAN_NO_HT);

    /*
     * Change the PHY context configuration as it is currently referenced
     * only by the P2P Device MAC
     */
    if (mvmvif->phy_ctxt->ref == 1) {
        ret = iwl_mvm_phy_ctxt_changed(mvm, mvmvif->phy_ctxt, &chandef, 1, 1);
        if (ret) { goto out_unlock; }
    } else {
        /*
         * The PHY context is shared with other MACs. Need to remove the
         * P2P Device from the binding, allocate an new PHY context and
         * create a new binding
         */
        phy_ctxt = iwl_mvm_get_free_phy_ctxt(mvm);
        if (!phy_ctxt) {
            ret = -ENOSPC;
            goto out_unlock;
        }

        ret = iwl_mvm_phy_ctxt_changed(mvm, phy_ctxt, &chandef, 1, 1);
        if (ret) {
            IWL_ERR(mvm, "Failed to change PHY context\n");
            goto out_unlock;
        }

        /* Unbind the P2P_DEVICE from the current PHY context */
        ret = iwl_mvm_binding_remove_vif(mvm, vif);
        if (WARN(ret, "Failed unbinding P2P_DEVICE\n")) { goto out_unlock; }

        iwl_mvm_phy_ctxt_unref(mvm, mvmvif->phy_ctxt);

        /* Bind the P2P_DEVICE to the new allocated PHY context */
        mvmvif->phy_ctxt = phy_ctxt;

        ret = iwl_mvm_binding_add_vif(mvm, vif);
        if (WARN(ret, "Failed binding P2P_DEVICE\n")) { goto out_unlock; }

        iwl_mvm_phy_ctxt_ref(mvm, mvmvif->phy_ctxt);
    }

schedule_time_event:
    /* Schedule the time events */
    ret = iwl_mvm_start_p2p_roc(mvm, vif, duration, type);

out_unlock:
    mutex_unlock(&mvm->mutex);
    IWL_DEBUG_MAC80211(mvm, "leave\n");
    return ret;
}

static int iwl_mvm_cancel_roc(struct ieee80211_hw* hw) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    IWL_DEBUG_MAC80211(mvm, "enter\n");

    mutex_lock(&mvm->mutex);
    iwl_mvm_stop_roc(mvm);
    mutex_unlock(&mvm->mutex);

    IWL_DEBUG_MAC80211(mvm, "leave\n");
    return 0;
}
#endif  // NEEDS_PORTING

static zx_status_t __iwl_mvm_add_chanctx(struct iwl_mvm* mvm, const wlan_channel_t* chandef,
                                         uint16_t* phy_ctxt_id) {
  struct iwl_mvm_phy_ctxt* phy_ctxt;
  zx_status_t ret;

  iwl_assert_lock_held(&mvm->mutex);

  IWL_DEBUG_MAC80211(mvm, "Add channel context\n");

  phy_ctxt = iwl_mvm_get_free_phy_ctxt(mvm);
  if (!phy_ctxt) {
    ret = ZX_ERR_NO_RESOURCES;
    goto out;
  }

  // TODO(45353): support MIMO Rx.
  ret = iwl_mvm_phy_ctxt_changed(mvm, phy_ctxt, chandef, 1, 1);
  if (ret != ZX_OK) {
    IWL_ERR(mvm, "Failed to add PHY context\n");
    goto out;
  }

  iwl_mvm_phy_ctxt_ref(mvm, phy_ctxt);
  *phy_ctxt_id = phy_ctxt->id;

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
  iwl_mvm_fm_notify_channel_change(ctx, IWL_FM_ADD_CHANCTX);
#endif

out:
  return ret;
}

zx_status_t iwl_mvm_add_chanctx(struct iwl_mvm* mvm, const wlan_channel_t* chandef,
                                uint16_t* phy_ctxt_id) {
  zx_status_t ret;

  mtx_lock(&mvm->mutex);
  ret = __iwl_mvm_add_chanctx(mvm, chandef, phy_ctxt_id);
  mtx_unlock(&mvm->mutex);

  return ret;
}

static zx_status_t __iwl_mvm_remove_chanctx(struct iwl_mvm* mvm, uint16_t phy_ctxt_id) {
  struct iwl_mvm_phy_ctxt* phy_ctxt = &mvm->phy_ctxts[phy_ctxt_id];

  iwl_assert_lock_held(&mvm->mutex);

  return iwl_mvm_phy_ctxt_unref(mvm, phy_ctxt);
#ifdef CPTCFG_IWLWIFI_FRQ_MGR
  iwl_mvm_fm_notify_channel_change(ctx, IWL_FM_REMOVE_CHANCTX);
#endif
}

zx_status_t iwl_mvm_remove_chanctx(struct iwl_mvm* mvm, uint16_t phy_ctxt_id) {
  zx_status_t ret;
  mtx_lock(&mvm->mutex);
  ret = __iwl_mvm_remove_chanctx(mvm, phy_ctxt_id);
  mtx_unlock(&mvm->mutex);
  return ret;
}

zx_status_t iwl_mvm_change_chanctx(struct iwl_mvm* mvm, uint16_t phy_ctxt_id,
                                   const wlan_channel_t* chandef) {
  struct iwl_mvm_phy_ctxt* phy_ctxt = &mvm->phy_ctxts[phy_ctxt_id];

  if (phy_ctxt->ref > 1) {
    IWL_WARN(mvm, "Cannot change PHY. Ref=%d\n", phy_ctxt->ref);
    return ZX_ERR_BAD_STATE;
  }

  mtx_lock(&mvm->mutex);

#if 0   // NEEDS_PORTING
    /* we are only changing the min_width, may be a noop */
    if (changed == IEEE80211_CHANCTX_CHANGE_MIN_WIDTH) {
        if (phy_ctxt->width == def->width) { goto out_unlock; }

        /* we are just toggling between 20_NOHT and 20 */
        if (phy_ctxt->width <= NL80211_CHAN_WIDTH_20 && def->width <= NL80211_CHAN_WIDTH_20) {
            goto out_unlock;
        }
    }

    iwl_mvm_bt_coex_vif_change(mvm);
#endif  // NEEDS_PORTING

  // TODO(45353): support MIMO Rx.
  zx_status_t ret = iwl_mvm_phy_ctxt_changed(mvm, phy_ctxt, chandef, 1, 1);
#ifdef CPTCFG_IWLWIFI_FRQ_MGR
  iwl_mvm_fm_notify_channel_change(ctx, IWL_FM_CHANGE_CHANCTX);
#endif

  mtx_unlock(&mvm->mutex);

  return ret;
}

static zx_status_t __iwl_mvm_assign_vif_chanctx(struct iwl_mvm_vif* mvmvif,
                                                const wlan_channel_t* chandef,
                                                bool switching_chanctx) {
  zx_status_t ret;

  iwl_assert_lock_held(&mvmvif->mvm->mutex);

  // Assume mvmvif->phy_ctxt had been assigned in mac_start().
  if (!mvmvif->phy_ctxt) {
    IWL_ERR(mvmvif, "PHY context is not assigned yet.\n");
    return ZX_ERR_BAD_STATE;
  }

  switch (mvmvif->mac_role) {
#if 0   // NEEDS_PORTING
    case NL80211_IFTYPE_AP:
        /* only needed if we're switching chanctx (i.e. during CSA) */
        if (switching_chanctx) {
            mvmvif->ap_ibss_active = true;
            break;
        }
    case NL80211_IFTYPE_ADHOC:
        /*
         * The AP binding flow is handled as part of the start_ap flow
         * (in bss_info_changed), similarly for IBSS.
         */
        ret = 0;
        goto out;
#endif  // NEEDS_PORTING
    case WLAN_INFO_MAC_ROLE_CLIENT:
      mvmvif->csa_bcn_pending = false;
      break;
#if 0   // NEEDS_PORTING
    case NL80211_IFTYPE_MONITOR:
        /* always disable PS when a monitor interface is active */
        mvmvif->ps_disabled = true;
        break;
#endif  // NEEDS_PORTING
    default:
      ret = ZX_ERR_NOT_SUPPORTED;
      IWL_ERR(mvmvif, "%s(): mac_role: %d not supported yet\n", __func__, mvmvif->mac_role);
      goto out;
  }

  ret = iwl_mvm_binding_add_vif(mvmvif);
  if (ret != ZX_OK) {
    IWL_ERR(mvmvif, "Cannot add vif binding: %s\n", zx_status_get_string(ret));
    goto out;
  }

#ifdef CPTCFG_IWLWIFI_FRQ_MGR
  iwl_mvm_update_ctx_tx_power_limit(mvm, vif, mvmvif->phy_ctxt);
#endif

  /*
   * Power state must be updated before quotas,
   * otherwise fw will complain.
   */
  iwl_mvm_power_update_mac(mvmvif->mvm);

#if 0   // NEEDS_PORTING
    /* Setting the quota at this stage is only required for monitor
     * interfaces. For the other types, the bss_info changed flow
     * will handle quota settings.
     */
    if (vif->type == NL80211_IFTYPE_MONITOR) {
        mvmvif->monitor_active = true;
        ret = iwl_mvm_update_quotas(mvm, false, NULL);
        if (ret) { goto out_remove_binding; }

        ret = iwl_mvm_add_snif_sta(mvm, vif);
        if (ret) { goto out_remove_binding; }
    }

    /* Handle binding during CSA */
    if (vif->type == NL80211_IFTYPE_AP) {
        iwl_mvm_update_quotas(mvm, false, NULL);
        iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL);
    }
#endif  // NEEDS_PORTING

  if (switching_chanctx && mvmvif->mac_role == WLAN_INFO_MAC_ROLE_CLIENT) {
    uint32_t duration = 3 * mvmvif->bss_conf.beacon_int;

    /* iwl_mvm_protect_session() reads directly from the
     * device (the system time), so make sure it is
     * available.
     */
    ret = iwl_mvm_ref_sync(mvmvif->mvm, IWL_MVM_REF_PROTECT_CSA);
    if (ret != ZX_OK) {
      goto out_remove_binding;
    }

    /* Protect the session to make sure we hear the first
     * beacon on the new channel.
     */
    mvmvif->csa_bcn_pending = true;
    iwl_mvm_protect_session(mvmvif->mvm, mvmvif, duration, duration,
                            mvmvif->bss_conf.beacon_int / 2, true);

    iwl_mvm_unref(mvmvif->mvm, IWL_MVM_REF_PROTECT_CSA);

#if 0   // NEEDS_PORTING
        iwl_mvm_update_quotas(mvm, false, NULL);
#endif  // NEEDS_PORTING
  }

  goto out;

out_remove_binding:
  iwl_mvm_binding_remove_vif(mvmvif);
  iwl_mvm_power_update_mac(mvmvif->mvm);
out:
  if (ret != ZX_OK) {
    mvmvif->phy_ctxt = NULL;
  }
  return ret;
}

zx_status_t iwl_mvm_assign_vif_chanctx(struct iwl_mvm_vif* mvmvif, const wlan_channel_t* chandef) {
  zx_status_t ret;

  mtx_lock(&mvmvif->mvm->mutex);
  ret = __iwl_mvm_assign_vif_chanctx(mvmvif, chandef, false);
  mtx_unlock(&mvmvif->mvm->mutex);

  return ret;
}

static zx_status_t __iwl_mvm_unassign_vif_chanctx(struct iwl_mvm_vif* mvmvif,
                                                  bool switching_chanctx) {
#if 0   // NEEDS_PORTING
    struct ieee80211_vif* disabled_vif = NULL;
#endif  // NEEDS_PORTING

  iwl_assert_lock_held(&mvmvif->mvm->mutex);

  zx_status_t ret;

  switch (mvmvif->mac_role) {
#if 0   // NEEDS_PORTING
    case NL80211_IFTYPE_ADHOC:
        goto out;
    case NL80211_IFTYPE_MONITOR:
        mvmvif->monitor_active = false;
        mvmvif->ps_disabled = false;
        iwl_mvm_rm_snif_sta(mvm, vif);
        break;
    case NL80211_IFTYPE_AP:
        /* This part is triggered only during CSA */
        if (!switching_chanctx || !mvmvif->ap_ibss_active) { goto out; }

        mvmvif->csa_countdown = false;

        /* Set CS bit on all the stations */
        iwl_mvm_modify_all_sta_disable_tx(mvm, mvmvif, true);

        /* Save blocked iface, the timeout is set on the next beacon */
        rcu_assign_pointer(mvm->csa_tx_blocked_vif, vif);

        mvmvif->ap_ibss_active = false;
        break;
#endif  // NEEDS_PORTING
    case WLAN_INFO_MAC_ROLE_CLIENT:
      if (!switching_chanctx) {
        break;
      }

#if 0   // NEEDS_PORTING
        disabled_vif = vif;
#endif  // NEEDS_PORTING

      ret = iwl_mvm_mac_ctxt_changed(mvmvif, true, NULL);
      if (ret != ZX_OK) {
        IWL_ERR(mvmvif, "Cannot update MAC context while unassigning: %s\n",
                zx_status_get_string(ret));
      }
      break;
    default:
      break;
  }

#if 0   // NEEDS_PORTING
  // TODO(43218): support multiple interfaces. Port iwl_mvm_update_quotas() in mvm/quota.c.
  iwl_mvm_update_quotas(mvm, false, disabled_vif);
#endif  // NEEDS_PORTING

  ret = iwl_mvm_binding_remove_vif(mvmvif);
  if (ret != ZX_OK) {
    IWL_ERR(mvmvif, "cannot remove VIF binding: %s\n", zx_status_get_string(ret));
  }

#if 0   // NEEDS_PORTING
out:
#endif  // NEEDS_PORTING

  mvmvif->phy_ctxt = NULL;
  ret = iwl_mvm_power_update_mac(mvmvif->mvm);
  if (ret != ZX_OK) {
    IWL_ERR(mvmvif, "cannot update the power setting of MAC: %s\n", zx_status_get_string(ret));
  }

  return ZX_OK;
}

zx_status_t iwl_mvm_unassign_vif_chanctx(struct iwl_mvm_vif* mvmvif) {
  mtx_lock(&mvmvif->mvm->mutex);
  zx_status_t ret = __iwl_mvm_unassign_vif_chanctx(mvmvif, false);
  mtx_unlock(&mvmvif->mvm->mutex);

  return ret;
}

#if 0  // NEEDS_PORTING
static int iwl_mvm_switch_vif_chanctx_swap(struct iwl_mvm* mvm,
                                           struct ieee80211_vif_chanctx_switch* vifs) {
    int ret;

    mutex_lock(&mvm->mutex);
    __iwl_mvm_unassign_vif_chanctx(mvm, vifs[0].vif, vifs[0].old_ctx, true);
    __iwl_mvm_remove_chanctx(mvm, vifs[0].old_ctx);

    ret = __iwl_mvm_add_chanctx(mvm, vifs[0].new_ctx);
    if (ret) {
        IWL_ERR(mvm, "failed to add new_ctx during channel switch\n");
        goto out_reassign;
    }

    ret = __iwl_mvm_assign_vif_chanctx(mvm, vifs[0].vif, vifs[0].new_ctx, true);
    if (ret) {
        IWL_ERR(mvm, "failed to assign new_ctx during channel switch\n");
        goto out_remove;
    }

    /* we don't support TDLS during DCM - can be caused by channel switch */
    if (iwl_mvm_phy_ctx_count(mvm) > 1) { iwl_mvm_teardown_tdls_peers(mvm); }

    goto out;

out_remove:
    __iwl_mvm_remove_chanctx(mvm, vifs[0].new_ctx);

out_reassign:
    if (__iwl_mvm_add_chanctx(mvm, vifs[0].old_ctx)) {
        IWL_ERR(mvm, "failed to add old_ctx back after failure.\n");
        goto out_restart;
    }

    if (__iwl_mvm_assign_vif_chanctx(mvm, vifs[0].vif, vifs[0].old_ctx, true)) {
        IWL_ERR(mvm, "failed to reassign old_ctx after failure.\n");
        goto out_restart;
    }

    goto out;

out_restart:
    /* things keep failing, better restart the hw */
    iwl_mvm_nic_restart(mvm, false);

out:
    mutex_unlock(&mvm->mutex);

    return ret;
}

static int iwl_mvm_switch_vif_chanctx_reassign(struct iwl_mvm* mvm,
                                               struct ieee80211_vif_chanctx_switch* vifs) {
    int ret;

    mutex_lock(&mvm->mutex);
    __iwl_mvm_unassign_vif_chanctx(mvm, vifs[0].vif, vifs[0].old_ctx, true);

    ret = __iwl_mvm_assign_vif_chanctx(mvm, vifs[0].vif, vifs[0].new_ctx, true);
    if (ret) {
        IWL_ERR(mvm, "failed to assign new_ctx during channel switch\n");
        goto out_reassign;
    }

    goto out;

out_reassign:
    if (__iwl_mvm_assign_vif_chanctx(mvm, vifs[0].vif, vifs[0].old_ctx, true)) {
        IWL_ERR(mvm, "failed to reassign old_ctx after failure.\n");
        goto out_restart;
    }

    goto out;

out_restart:
    /* things keep failing, better restart the hw */
    iwl_mvm_nic_restart(mvm, false);

out:
    mutex_unlock(&mvm->mutex);

    return ret;
}

static int iwl_mvm_switch_vif_chanctx(struct ieee80211_hw* hw,
                                      struct ieee80211_vif_chanctx_switch* vifs, int n_vifs,
                                      enum ieee80211_chanctx_switch_mode mode) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int ret;

    /* we only support a single-vif right now */
    if (n_vifs > 1) { return -EOPNOTSUPP; }

    switch (mode) {
    case CHANCTX_SWMODE_SWAP_CONTEXTS:
        ret = iwl_mvm_switch_vif_chanctx_swap(mvm, vifs);
        break;
    case CHANCTX_SWMODE_REASSIGN_VIF:
        ret = iwl_mvm_switch_vif_chanctx_reassign(mvm, vifs);
        break;
    default:
        ret = -EOPNOTSUPP;
        break;
    }

    return ret;
}

static int iwl_mvm_tx_last_beacon(struct ieee80211_hw* hw) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    return mvm->ibss_manager;
}

static int iwl_mvm_set_tim(struct ieee80211_hw* hw, struct ieee80211_sta* sta, bool set) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_sta* mvm_sta = iwl_mvm_sta_from_mac80211(sta);

    if (!mvm_sta || !mvm_sta->vif) {
        IWL_ERR(mvm, "Station is not associated to a vif\n");
        return -EINVAL;
    }

    return iwl_mvm_mac_ctxt_beacon_changed(mvm, mvm_sta->vif);
}

#ifdef CPTCFG_NL80211_TESTMODE
static const struct nla_policy iwl_mvm_tm_policy[IWL_TM_ATTR_MAX + 1] = {
    [IWL_TM_ATTR_CMD] = {.type = NLA_U32},
    [IWL_TM_ATTR_NOA_DURATION] = {.type = NLA_U32},
    [IWL_TM_ATTR_BEACON_FILTER_STATE] = {.type = NLA_U32},
};

static int __iwl_mvm_mac_testmode_cmd(struct iwl_mvm* mvm, struct ieee80211_vif* vif, void* data,
                                      int len) {
    struct nlattr* tb[IWL_TM_ATTR_MAX + 1];
    int err;
    uint32_t noa_duration;

    err = nla_parse(tb, IWL_TM_ATTR_MAX, data, len, iwl_mvm_tm_policy, NULL);
    if (err) { return err; }

    if (!tb[IWL_TM_ATTR_CMD]) { return -EINVAL; }

    switch (nla_get_u32(tb[IWL_TM_ATTR_CMD])) {
    case IWL_TM_CMD_SET_NOA:
        if (!vif || vif->type != NL80211_IFTYPE_AP || !vif->p2p || !vif->bss_conf.enable_beacon ||
            !tb[IWL_TM_ATTR_NOA_DURATION]) {
            return -EINVAL;
        }

        noa_duration = nla_get_u32(tb[IWL_TM_ATTR_NOA_DURATION]);
        if (noa_duration >= vif->bss_conf.beacon_int) { return -EINVAL; }

        mvm->noa_duration = noa_duration;
        mvm->noa_vif = vif;

        if (fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
#ifdef CPTCFG_IWLWIFI_DEBUG_HOST_CMD_ENABLED
            int beacon_int = vif->bss_conf.beacon_int;
            int max_quota_percent = (100 * (beacon_int - noa_duration)) / beacon_int;

            return iwl_mvm_dhc_quota_enforce(mvm, iwl_mvm_vif_from_mac80211(vif),
                                             max_quota_percent);
#else
            return -EOPNOTSUPP;
#endif
        }

        return iwl_mvm_update_quotas(mvm, true, NULL);
    case IWL_TM_CMD_SET_BEACON_FILTER:
        /* must be associated client vif - ignore authorized */
        if (!vif || vif->type != NL80211_IFTYPE_STATION || !vif->bss_conf.assoc ||
            !vif->bss_conf.dtim_period || !tb[IWL_TM_ATTR_BEACON_FILTER_STATE]) {
            return -EINVAL;
        }

        if (nla_get_u32(tb[IWL_TM_ATTR_BEACON_FILTER_STATE])) {
            return iwl_mvm_enable_beacon_filter(mvm, vif, 0);
        }
        return iwl_mvm_disable_beacon_filter(mvm, vif, 0);
    }

    return -EOPNOTSUPP;
}

static int iwl_mvm_mac_testmode_cmd(struct ieee80211_hw* hw, struct ieee80211_vif* vif, void* data,
                                    int len) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int err;

    mutex_lock(&mvm->mutex);
    err = __iwl_mvm_mac_testmode_cmd(mvm, vif, data, len);
    mutex_unlock(&mvm->mutex);

    return err;
}
#endif

static void iwl_mvm_channel_switch(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                   struct ieee80211_channel_switch* chsw) {
    /* By implementing this operation, we prevent mac80211 from
     * starting its own channel switch timer, so that we can call
     * ieee80211_chswitch_done() ourselves at the right time
     * (which is when the absence time event starts).
     */

    IWL_DEBUG_MAC80211(IWL_MAC80211_GET_MVM(hw), "dummy channel switch op\n");
}

static int iwl_mvm_pre_channel_switch(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                      struct ieee80211_channel_switch* chsw) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct ieee80211_vif* csa_vif;
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    uint32_t apply_time;
    int ret;

    mutex_lock(&mvm->mutex);

    mvmvif->csa_failed = false;

    IWL_DEBUG_MAC80211(mvm, "pre CSA to freq %d\n", chsw->chandef.center_freq1);

    iwl_fw_dbg_trigger_simple_stop(&mvm->fwrt, ieee80211_vif_to_wdev(vif),
                                   FW_DBG_TRIGGER_CHANNEL_SWITCH);

    switch (vif->type) {
    case NL80211_IFTYPE_AP:
        csa_vif = rcu_dereference_protected(mvm->csa_vif, lockdep_is_held(&mvm->mutex));
        if (WARN_ONCE(csa_vif && csa_vif->csa_active, "Another CSA is already in progress")) {
            ret = -EBUSY;
            goto out_unlock;
        }

        /* we still didn't unblock tx. prevent new CS meanwhile */
        if (rcu_dereference_protected(mvm->csa_tx_blocked_vif, lockdep_is_held(&mvm->mutex))) {
            ret = -EBUSY;
            goto out_unlock;
        }

        rcu_assign_pointer(mvm->csa_vif, vif);

        if (WARN_ONCE(mvmvif->csa_countdown, "Previous CSA countdown didn't complete")) {
            ret = -EBUSY;
            goto out_unlock;
        }

        mvmvif->csa_target_freq = chsw->chandef.chan->center_freq;

        break;
    case NL80211_IFTYPE_STATION:
        /* Schedule the time event to a bit before beacon 1,
         * to make sure we're in the new channel when the
         * GO/AP arrives. In case count <= 1 immediately schedule the
         * TE (this might result with some packet loss or connection
         * loss).
         */
        if (chsw->count <= 1) {
            apply_time = 0;
        } else
            apply_time = chsw->device_timestamp + ((vif->bss_conf.beacon_int * (chsw->count - 1) -
                                                    IWL_MVM_CHANNEL_SWITCH_TIME_CLIENT) *
                                                   1024);

        if (chsw->block_tx) { iwl_mvm_csa_client_absent(mvm, vif); }

        iwl_mvm_schedule_csa_period(mvm, vif, vif->bss_conf.beacon_int, apply_time);
        if (mvmvif->bf_data.bf_enabled) {
            ret = iwl_mvm_disable_beacon_filter(mvm, vif, 0);
            if (ret) { goto out_unlock; }
        }

        break;
    default:
        break;
    }

    mvmvif->ps_disabled = true;

    ret = iwl_mvm_power_update_ps(mvm);
    if (ret) { goto out_unlock; }

    /* we won't be on this channel any longer */
    iwl_mvm_teardown_tdls_peers(mvm);

out_unlock:
    mutex_unlock(&mvm->mutex);

    return ret;
}

static int iwl_mvm_post_channel_switch(struct ieee80211_hw* hw, struct ieee80211_vif* vif) {
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int ret;

    mutex_lock(&mvm->mutex);

    if (mvmvif->csa_failed) {
        mvmvif->csa_failed = false;
        ret = -EIO;
        goto out_unlock;
    }

    if (vif->type == NL80211_IFTYPE_STATION) {
        struct iwl_mvm_sta* mvmsta;

        mvmvif->csa_bcn_pending = false;
        mvmsta = iwl_mvm_sta_from_staid_protected(mvm, mvmvif->ap_sta_id);

        if (WARN_ON(!mvmsta)) {
            ret = -EIO;
            goto out_unlock;
        }

        iwl_mvm_sta_modify_disable_tx(mvm, mvmsta, false);

        iwl_mvm_mac_ctxt_changed(mvm, vif, false, NULL);

        ret = iwl_mvm_enable_beacon_filter(mvm, vif, 0);
        if (ret) { goto out_unlock; }

        iwl_mvm_stop_session_protection(mvm, vif);
    }

    mvmvif->ps_disabled = false;

    ret = iwl_mvm_power_update_ps(mvm);

out_unlock:
    mutex_unlock(&mvm->mutex);

    return ret;
}

static void iwl_mvm_flush_no_vif(struct iwl_mvm* mvm, uint32_t queues, bool drop) {
    int i;

    if (!iwl_mvm_has_new_tx_api(mvm)) {
        if (drop) {
            mutex_lock(&mvm->mutex);
            iwl_mvm_flush_tx_path(mvm, iwl_mvm_flushable_queues(mvm) & queues, 0);
            mutex_unlock(&mvm->mutex);
        } else {
            iwl_trans_wait_tx_queues_empty(mvm->trans, queues);
        }
        return;
    }

    mutex_lock(&mvm->mutex);
    for (i = 0; i < ARRAY_SIZE(mvm->fw_id_to_mac_id); i++) {
        struct ieee80211_sta* sta;

        sta = rcu_dereference_protected(mvm->fw_id_to_mac_id[i], lockdep_is_held(&mvm->mutex));
        if (IS_ERR_OR_NULL(sta)) { continue; }

        if (drop) {
            iwl_mvm_flush_sta_tids(mvm, i, 0xFF, 0);
        } else {
            iwl_mvm_wait_sta_queues_empty(mvm, iwl_mvm_sta_from_mac80211(sta));
        }
    }
    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_mac_flush(struct ieee80211_hw* hw, struct ieee80211_vif* vif, uint32_t queues,
                              bool drop) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif;
    struct iwl_mvm_sta* mvmsta;
    struct ieee80211_sta* sta;
    int i;
    uint32_t msk = 0;

    if (!vif) {
        iwl_mvm_flush_no_vif(mvm, queues, drop);
        return;
    }

    if (vif->type != NL80211_IFTYPE_STATION) { return; }

    /* Make sure we're done with the deferred traffic before flushing */
    flush_work(&mvm->add_stream_wk);

    mutex_lock(&mvm->mutex);
    mvmvif = iwl_mvm_vif_from_mac80211(vif);

    /* flush the AP-station and all TDLS peers */
    for (i = 0; i < ARRAY_SIZE(mvm->fw_id_to_mac_id); i++) {
        sta = rcu_dereference_protected(mvm->fw_id_to_mac_id[i], lockdep_is_held(&mvm->mutex));
        if (IS_ERR_OR_NULL(sta)) { continue; }

        mvmsta = iwl_mvm_sta_from_mac80211(sta);
        if (mvmsta->vif != vif) { continue; }

        /* make sure only TDLS peers or the AP are flushed */
        WARN_ON(i != mvmvif->ap_sta_id && !sta->tdls);

        if (drop) {
            if (iwl_mvm_flush_sta(mvm, mvmsta, false, 0)) { IWL_ERR(mvm, "flush request fail\n"); }
        } else {
            msk |= mvmsta->tfd_queue_msk;
            if (iwl_mvm_has_new_tx_api(mvm)) { iwl_mvm_wait_sta_queues_empty(mvm, mvmsta); }
        }
    }

    mutex_unlock(&mvm->mutex);

    /* this can take a while, and we may need/want other operations
     * to succeed while doing this, so do it without the mutex held
     */
    if (!drop && !iwl_mvm_has_new_tx_api(mvm)) { iwl_trans_wait_tx_queues_empty(mvm->trans, msk); }
}

static int iwl_mvm_mac_get_survey(struct ieee80211_hw* hw, int idx, struct survey_info* survey) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    int ret;

    memset(survey, 0, sizeof(*survey));

    /* only support global statistics right now */
    if (idx != 0) { return -ENOENT; }

    if (!fw_has_capa(&mvm->fw->ucode_capa, IWL_UCODE_TLV_CAPA_RADIO_BEACON_STATS)) {
        return -ENOENT;
    }

    mutex_lock(&mvm->mutex);

    if (iwl_mvm_firmware_running(mvm)) {
        ret = iwl_mvm_request_statistics(mvm, false);
        if (ret) { goto out; }
    }

    survey->filled =
        SURVEY_INFO_TIME | SURVEY_INFO_TIME_RX | SURVEY_INFO_TIME_TX | SURVEY_INFO_TIME_SCAN;
    survey->time = mvm->accu_radio_stats.on_time_rf + mvm->radio_stats.on_time_rf;
    do_div(survey->time, USEC_PER_MSEC);

    survey->time_rx = mvm->accu_radio_stats.rx_time + mvm->radio_stats.rx_time;
    do_div(survey->time_rx, USEC_PER_MSEC);

    survey->time_tx = mvm->accu_radio_stats.tx_time + mvm->radio_stats.tx_time;
    do_div(survey->time_tx, USEC_PER_MSEC);

    survey->time_scan = mvm->accu_radio_stats.on_time_scan + mvm->radio_stats.on_time_scan;
    do_div(survey->time_scan, USEC_PER_MSEC);

    ret = 0;
out:
    mutex_unlock(&mvm->mutex);
    return ret;
}

static void iwl_mvm_mac_sta_statistics(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                       struct ieee80211_sta* sta, struct station_info* sinfo) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_vif* mvmvif = iwl_mvm_vif_from_mac80211(vif);
    struct iwl_mvm_sta* mvmsta = iwl_mvm_sta_from_mac80211(sta);

    if (mvmsta->avg_energy) {
        sinfo->signal_avg = mvmsta->avg_energy;
        sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL_AVG);
    }

    /* if beacon filtering isn't on mac80211 does it anyway */
    if (!(vif->driver_flags & IEEE80211_VIF_BEACON_FILTER)) { return; }

    if (!vif->bss_conf.assoc) { return; }

    mutex_lock(&mvm->mutex);

    if (mvmvif->ap_sta_id != mvmsta->sta_id) { goto unlock; }

    if (iwl_mvm_request_statistics(mvm, false)) { goto unlock; }

    sinfo->rx_beacon = mvmvif->beacon_stats.num_beacons + mvmvif->beacon_stats.accu_num_beacons;
    sinfo->filled |= BIT_ULL(NL80211_STA_INFO_BEACON_RX);
    if (mvmvif->beacon_stats.avg_signal) {
        /* firmware only reports a value after RXing a few beacons */
        sinfo->rx_beacon_signal_avg = mvmvif->beacon_stats.avg_signal;
        sinfo->filled |= BIT_ULL(NL80211_STA_INFO_BEACON_SIGNAL_AVG);
    }
unlock:
    mutex_unlock(&mvm->mutex);
}

static void iwl_mvm_event_mlme_callback(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                        const struct ieee80211_event* event) {
#define CHECK_MLME_TRIGGER(_cnt, _fmt...)              \
  do {                                                 \
    if ((trig_mlme->_cnt) && --(trig_mlme->_cnt))      \
      break;                                           \
    iwl_fw_dbg_collect_trig(&(mvm)->fwrt, trig, _fmt); \
  } while (0)

    struct iwl_fw_dbg_trigger_tlv* trig;
    struct iwl_fw_dbg_trigger_mlme* trig_mlme;

    trig = iwl_fw_dbg_trigger_on(&mvm->fwrt, ieee80211_vif_to_wdev(vif), FW_DBG_TRIGGER_MLME);
    if (!trig) { return; }

    trig_mlme = (void*)trig->data;

    if (event->u.mlme.data == ASSOC_EVENT) {
        if (event->u.mlme.status == MLME_DENIED) {
            CHECK_MLME_TRIGGER(stop_assoc_denied, "DENIED ASSOC: reason %d", event->u.mlme.reason);
        } else if (event->u.mlme.status == MLME_TIMEOUT) {
            CHECK_MLME_TRIGGER(stop_assoc_timeout, "ASSOC TIMEOUT");
        }
    } else if (event->u.mlme.data == AUTH_EVENT) {
        if (event->u.mlme.status == MLME_DENIED) {
            CHECK_MLME_TRIGGER(stop_auth_denied, "DENIED AUTH: reason %d", event->u.mlme.reason);
        } else if (event->u.mlme.status == MLME_TIMEOUT) {
            CHECK_MLME_TRIGGER(stop_auth_timeout, "AUTH TIMEOUT");
        }
    } else if (event->u.mlme.data == DEAUTH_RX_EVENT) {
        CHECK_MLME_TRIGGER(stop_rx_deauth, "DEAUTH RX %d", event->u.mlme.reason);
    } else if (event->u.mlme.data == DEAUTH_TX_EVENT) {
        CHECK_MLME_TRIGGER(stop_tx_deauth, "DEAUTH TX %d", event->u.mlme.reason);
    }
#undef CHECK_MLME_TRIGGER
}

static void iwl_mvm_event_bar_rx_callback(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                          const struct ieee80211_event* event) {
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct iwl_fw_dbg_trigger_ba* ba_trig;

    trig = iwl_fw_dbg_trigger_on(&mvm->fwrt, ieee80211_vif_to_wdev(vif), FW_DBG_TRIGGER_BA);
    if (!trig) { return; }

    ba_trig = (void*)trig->data;

    if (!(le16_to_cpu(ba_trig->rx_bar) & BIT(event->u.ba.tid))) { return; }

    iwl_fw_dbg_collect_trig(&mvm->fwrt, trig, "BAR received from %pM, tid %d, ssn %d",
                            event->u.ba.sta->addr, event->u.ba.tid, event->u.ba.ssn);
}

#ifdef CPTCFG_MAC80211_LATENCY_MEASUREMENTS
#define MARKER_CMD_TX_LAT_PAYLOAD_SIZE 5
#define MARKER_CMD_TX_LAT_TID_OFFSET 12
#define MARKER_CMD_TX_LAT_DEFAULT_WIN 1000
#define MARKER_CMD_TX_LAT_UNKNOWN 0xffffffff

static uint32_t iwl_mvm_send_latency_marker_cmd(struct iwl_mvm* mvm, uint32_t msrmnt, uint16_t seq,
                                                uint16_t tid) {
    struct timespec ts;
    int ret;
    struct iwl_mvm_marker_rsp* rsp;
    struct iwl_mvm_marker* marker;
    struct iwl_host_cmd cmd = {
        .id = MARKER_CMD,
        .flags = CMD_WANT_SKB,
    };
    uint32_t cmd_size =
        sizeof(struct iwl_mvm_marker) + MARKER_CMD_TX_LAT_PAYLOAD_SIZE * sizeof(uint32_t);

    getnstimeofday(&ts);

    marker = kzalloc(cmd_size, GFP_KERNEL);
    if (!marker) { return -ENOMEM; }

    cmd.data[0] = marker;
    cmd.len[0] = cmd_size;

    marker->dw_len = 0x8;
    marker->marker_id = MARKER_ID_TX_FRAME_LATENCY;
    marker->timestamp = cpu_to_le64(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    /* metadata[0]-frame latency */
    marker->metadata[0] = cpu_to_le32(msrmnt);
    /* metadata[1]-delta in msec from UTC to frame enter the kernel */
    marker->metadata[1] = cpu_to_le32(MARKER_CMD_TX_LAT_UNKNOWN);
    /* metadata[2]-delta in msec from UTC to frame enter the NIC */
    marker->metadata[2] = cpu_to_le32(MARKER_CMD_TX_LAT_UNKNOWN);
    /* metadata[3]-delta in msec from UTC to frecieved from the NIC */
    marker->metadata[3] = cpu_to_le32(MARKER_CMD_TX_LAT_UNKNOWN);
    /* metadata[4]-bits:16-31-TFD Queue ID, 12-15-TID, 0-11-sequence */
    marker->metadata[4] =
        cpu_to_le32(0xffff0000 | ((0xf & tid) << MARKER_CMD_TX_LAT_TID_OFFSET) | (seq & 0x0fff));

    mutex_lock(&mvm->mutex);
    ret = iwl_mvm_send_cmd(mvm, &cmd);
    mutex_unlock(&mvm->mutex);
    if (ret) {
        IWL_ERR(mvm, "Couldn't send MARKER_CMD: %d\n", ret);
        goto out;
    }

    rsp = (void*)cmd.resp_pkt->data;
    ret = le32_to_cpu(rsp->gp2);
    iwl_free_resp(&cmd);
out:
    kfree(marker);
    return ret;
}

/*
 * Trigger the fw log collection in case of a Tx packet that has crossed a
 * configured threshold.
 *
 * There are 3 differnt modes of triggering:
 *
 * Immediate Internal buffer mode:
 * The driver will retrieve monitor/usniffer data on the first driver
 * notification and wait for 1 second (during this 1 second it will not issue
 * another monitor/usniffer retrieve request). During this 1 second the driver
 * will store all notifications data to the trace including GP2 timestamp for
 * every notification. Also special mark will be sent to the firmware for every
 * notification.
 *
 * Delayed Internal buffer mode:
 * During the window interval the driver will calculate which tx had the
 * largest latency. When the monitor_collect_window expires the driver will
 * retrieve monitor/sniffer and print the notification for the packet with max
 * latency to the trace including GP2 timestamp. Also special mark will be sent
 * to the firmware for every notification.
 *
 * Continuous External buffer mode:
 * The mode is used when we are able to direct the usniffer logs to an external
 * memory device (should be started/stopped Manually).
 * During the window interval the driver will calculate which tx had the
 * largest latency. When the monitor_collect_window expires the driver print
 * the notification for the packet with max latency to the trace including GP2
 * timestamp. Also special mark will be sent to the firmware for every
 * notification.
 */

/*
 * TX latency monitor watchdog is armed upon first latency notification.
 * It fires when monitor window is expired and does the following:
 * - immediate internal buffer mode: just reset start_round_ts flag
 * - delayed internal buffer mode: writes metadata to trace-cmd and collects
 *   firmware dump
 * - continuous external buffer mode: just writes metadata to trace-cmd
 */
void iwl_mvm_tx_latency_watchdog_wk(struct work_struct* wk) {
    struct iwl_mvm* mvm = container_of(wk, struct iwl_mvm, tx_latency_watchdog_wk.work);
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct ieee80211_tx_latency_event* tx_lat = &mvm->last_tx_lat_event;
    struct ieee80211_tx_latency_event* max = &mvm->round_max_tx_lat;
    uint32_t round_dur = tx_lat->monitor_collec_wind;

    mvm->start_round_ts = 0;

    if (!round_dur) { return; }

    if (tx_lat->mode == IEEE80211_TX_LATENCY_EXT_BUF) {
        trace_iwlwifi_dev_tx_latency_thrshld(mvm->dev, max->msrmnt, max->pkt_start, max->pkt_end,
                                             max->tid, max->event_time, max->seq,
                                             mvm->max_tx_latency_gp2, 1);
        return;
    }

    trig = iwl_fw_dbg_get_trigger(mvm->fw, FW_DBG_TRIGGER_TX_LATENCY);
    trace_iwlwifi_dev_tx_latency_thrshld(mvm->dev, max->msrmnt, max->pkt_start, max->pkt_end,
                                         max->tid, max->event_time, max->seq,
                                         mvm->max_tx_latency_gp2, 1);
    iwl_fw_dbg_collect_trig(&mvm->fwrt, trig,
                            "Tx Latency threshold was crossed, seq: 0x%x, msrmnt: %d.", max->seq,
                            max->msrmnt);
}

/*
 * TX latency monitor work is scheduled upon every latency event. Regardless of
 * the monitor mode it as a first step sends firmware marker command containing
 * problematic packet's data - latency measurement, sequence number and TID.
 * It also obtains GP2 timestamp for this marker command.
 * The rest depends on monitor mode as follows:
 * 1. Immediate Internal Buffer mode.
 *    - arms watchdog to fire after monitor period (1 sec)
 *    - write packet's metadata to trace-cmd
 *    - only if the packet is the first one starting the monitor period,
 *      collect firmware dump
 * 2. Delayed Internal Buffer mode.
 *    - arms watchdog to fire after user defined monitor period
 *    - save largest latency packet's metadata
 * 3. Continuous External Buffer mode with delay.
 *    - arms watchdog to fire after user defined monitor period
 *    - save largest latency packet's metadata
 * 4. Continuous External Buffer mode without delay.
 *    - write packet's metadata to trace-cmd
 */
void iwl_mvm_tx_latency_wk(struct work_struct* wk) {
    struct iwl_mvm* mvm = container_of(wk, struct iwl_mvm, tx_latency_wk);
    struct iwl_fw_dbg_trigger_tlv* trig;
    struct ieee80211_tx_latency_event* tx_lat = &mvm->last_tx_lat_event;
    struct ieee80211_tx_latency_event* max = &mvm->round_max_tx_lat;
    s64 ts = ktime_to_ms(ktime_get());
    uint32_t round_dur = tx_lat->monitor_collec_wind;
    uint32_t round_end =
        tx_lat->monitor_collec_wind ? tx_lat->monitor_collec_wind : MARKER_CMD_TX_LAT_DEFAULT_WIN;
    bool first_pkt = false;
    uint32_t gp2 = 0;
    uint32_t delay = 0;

    if (!iwl_fw_dbg_trigger_enabled(mvm->fw, FW_DBG_TRIGGER_TX_LATENCY)) { return; }

    trig = iwl_fw_dbg_get_trigger(mvm->fw, FW_DBG_TRIGGER_TX_LATENCY);

    tx_lat->event_time = ktime_to_ms(ktime_get());

    gp2 = iwl_mvm_send_latency_marker_cmd(mvm, tx_lat->msrmnt, tx_lat->seq, tx_lat->tid);
    /*
     * If this is the first packet that crossed the threshold in the round
     * update start time stamp
     */
    if (!mvm->start_round_ts) {
        mvm->start_round_ts = ts;
        first_pkt = true;
        memcpy(max, tx_lat, sizeof(*tx_lat));
        mvm->max_tx_latency_gp2 = gp2;
        if (round_dur) {
            delay = msecs_to_jiffies(round_dur);
        } else if (tx_lat->mode == IEEE80211_TX_LATENCY_INT_BUF) {
            delay = msecs_to_jiffies(round_end);
        }
        if (delay) { schedule_delayed_work(&mvm->tx_latency_watchdog_wk, delay); }
    }

    /*
     * Updated the packet with the max latency.
     */
    if (round_dur && max->msrmnt < tx_lat->msrmnt) {
        memcpy(max, tx_lat, sizeof(*tx_lat));
        mvm->max_tx_latency_gp2 = gp2;
    }

    if (round_dur) { return; }

    if (tx_lat->mode == IEEE80211_TX_LATENCY_EXT_BUF) {
        trace_iwlwifi_dev_tx_latency_thrshld(mvm->dev, tx_lat->msrmnt, tx_lat->pkt_start,
                                             tx_lat->pkt_end, tx_lat->tid, tx_lat->event_time,
                                             tx_lat->seq, gp2, 0);
        return;
    }

    trace_iwlwifi_dev_tx_latency_thrshld(mvm->dev, tx_lat->msrmnt, tx_lat->pkt_start,
                                         tx_lat->pkt_end, tx_lat->tid, tx_lat->event_time,
                                         tx_lat->seq, gp2, 0);
    if (first_pkt)
        iwl_fw_dbg_collect_trig(&mvm->fwrt, trig,
                                "Tx Latency threshold was crossed, seq: 0x%x, msrmnt: %d.",
                                tx_lat->seq, tx_lat->msrmnt);
}

static void iwl_mvm_event_tx_latency_callback(struct iwl_mvm* mvm, struct ieee80211_vif* vif,
                                              const struct ieee80211_event* event) {
    memcpy(&mvm->last_tx_lat_event, &event->u.tx_lat, sizeof(event->u.tx_lat));
    schedule_work(&mvm->tx_latency_wk);
}
#endif /* CPTCFG_MAC80211_LATENCY_MEASUREMENTS */

static void iwl_mvm_mac_event_callback(struct ieee80211_hw* hw, struct ieee80211_vif* vif,
                                       const struct ieee80211_event* event) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    switch (event->type) {
    case MLME_EVENT:
        iwl_mvm_event_mlme_callback(mvm, vif, event);
        break;
    case BAR_RX_EVENT:
        iwl_mvm_event_bar_rx_callback(mvm, vif, event);
        break;
    case BA_FRAME_TIMEOUT:
        iwl_mvm_event_frame_timeout_callback(mvm, vif, event->u.ba.sta, event->u.ba.tid);
        break;
#ifdef CPTCFG_MAC80211_LATENCY_MEASUREMENTS
    case TX_LATENCY_EVENT:
        iwl_mvm_event_tx_latency_callback(mvm, vif, event);
        break;
#endif /* CPTCFG_MAC80211_LATENCY_MEASUREMENTS */
    default:
        break;
    }
}

void iwl_mvm_sync_rx_queues_internal(struct iwl_mvm* mvm, struct iwl_mvm_internal_rxq_notif* notif,
                                     uint32_t size) {
    uint32_t qmask = BIT(mvm->trans->num_rx_queues) - 1;
    int ret;

    iwl_assert_lock_held(&mvm->mutex);

    if (!iwl_mvm_has_new_rx_api(mvm)) { return; }

    notif->cookie = mvm->queue_sync_cookie;

    if (notif->sync) { atomic_set(&mvm->queue_sync_counter, mvm->trans->num_rx_queues); }

    ret = iwl_mvm_notify_rx_queue(mvm, qmask, (uint8_t*)notif, size);
    if (ret) {
        IWL_ERR(mvm, "Failed to trigger RX queues sync (%d)\n", ret);
        goto out;
    }

    if (notif->sync) {
        ret = wait_event_timeout(
            mvm->rx_sync_waitq,
            atomic_read(&mvm->queue_sync_counter) == 0 || iwl_mvm_is_radio_killed(mvm), HZ);
        WARN_ON_ONCE(!ret && !iwl_mvm_is_radio_killed(mvm));
    }

out:
    atomic_set(&mvm->queue_sync_counter, 0);
    mvm->queue_sync_cookie++;
}

static void iwl_mvm_sync_rx_queues(struct ieee80211_hw* hw) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);
    struct iwl_mvm_internal_rxq_notif data = {
        .type = IWL_MVM_RXQ_EMPTY,
        .sync = 1,
    };

    mutex_lock(&mvm->mutex);
    iwl_mvm_sync_rx_queues_internal(mvm, &data, sizeof(data));
    mutex_unlock(&mvm->mutex);
}

static bool iwl_mvm_can_hw_csum(struct sk_buff* skb) {
#if IS_ENABLED(CONFIG_INET)
    uint8_t protocol = ip_hdr(skb)->protocol;

    if (protocol != IPPROTO_TCP || protocol != IPPROTO_UDP) { return false; }
    return true;
#else
    return false;
#endif
}

static bool iwl_mvm_mac_can_aggregate(struct ieee80211_hw* hw, struct sk_buff* head,
                                      struct sk_buff* skb) {
    struct iwl_mvm* mvm = IWL_MAC80211_GET_MVM(hw);

    /* For now don't aggregate IPv6 in AMSDU */
    if (skb->protocol != htons(ETH_P_IP)) { return false; }

    if (!iwl_mvm_is_csum_supported(mvm)) { return true; }

    return iwl_mvm_can_hw_csum(skb) == iwl_mvm_can_hw_csum(head);
}
#endif  // NEEDS_PORTING

const struct ieee80211_ops iwl_mvm_hw_ops = {
#if 0  // NEEDS_PORTING
    .tx = iwl_mvm_mac_tx,
    .wake_tx_queue = iwl_mvm_mac_wake_tx_queue,
    .ampdu_action = iwl_mvm_mac_ampdu_action,
    .start = iwl_mvm_mac_start,
    .reconfig_complete = iwl_mvm_mac_reconfig_complete,
    .stop = iwl_mvm_mac_stop,
    .add_interface = iwl_mvm_mac_add_interface,
    .remove_interface = iwl_mvm_mac_remove_interface,
    .config = iwl_mvm_mac_config,
    .prepare_multicast = iwl_mvm_prepare_multicast,
    .configure_filter = iwl_mvm_configure_filter,
    .config_iface_filter = iwl_mvm_config_iface_filter,
    .bss_info_changed = iwl_mvm_bss_info_changed,
    .hw_scan = iwl_mvm_mac_hw_scan,
    .cancel_hw_scan = iwl_mvm_mac_cancel_hw_scan,
    .sta_pre_rcu_remove = iwl_mvm_sta_pre_rcu_remove,
    .sta_state = iwl_mvm_mac_sta_state,
    .sta_notify = iwl_mvm_mac_sta_notify,
    .allow_buffered_frames = iwl_mvm_mac_allow_buffered_frames,
    .release_buffered_frames = iwl_mvm_mac_release_buffered_frames,
    .set_rts_threshold = iwl_mvm_mac_set_rts_threshold,
    .sta_rc_update = iwl_mvm_sta_rc_update,
    .conf_tx = iwl_mvm_mac_conf_tx,
    .mgd_prepare_tx = iwl_mvm_mac_mgd_prepare_tx,
    .mgd_protect_tdls_discover = iwl_mvm_mac_mgd_protect_tdls_discover,
    .flush = iwl_mvm_mac_flush,
    .sched_scan_start = iwl_mvm_mac_sched_scan_start,
    .sched_scan_stop = iwl_mvm_mac_sched_scan_stop,
    .set_key = iwl_mvm_mac_set_key,
    .update_tkip_key = iwl_mvm_mac_update_tkip_key,
    .remain_on_channel = iwl_mvm_roc,
    .cancel_remain_on_channel = iwl_mvm_cancel_roc,
    .add_chanctx = iwl_mvm_add_chanctx,
    .remove_chanctx = iwl_mvm_remove_chanctx,
    .change_chanctx = iwl_mvm_change_chanctx,
    .assign_vif_chanctx = iwl_mvm_assign_vif_chanctx,
    .unassign_vif_chanctx = iwl_mvm_unassign_vif_chanctx,
    .switch_vif_chanctx = iwl_mvm_switch_vif_chanctx,

    .start_ap = iwl_mvm_start_ap_ibss,
    .stop_ap = iwl_mvm_stop_ap_ibss,
    .join_ibss = iwl_mvm_start_ap_ibss,
    .leave_ibss = iwl_mvm_stop_ap_ibss,

    .tx_last_beacon = iwl_mvm_tx_last_beacon,

    .set_tim = iwl_mvm_set_tim,

    .channel_switch = iwl_mvm_channel_switch,
    .pre_channel_switch = iwl_mvm_pre_channel_switch,
    .post_channel_switch = iwl_mvm_post_channel_switch,

    .tdls_channel_switch = iwl_mvm_tdls_channel_switch,
    .tdls_cancel_channel_switch = iwl_mvm_tdls_cancel_channel_switch,
    .tdls_recv_channel_switch = iwl_mvm_tdls_recv_channel_switch,

    .event_callback = iwl_mvm_mac_event_callback,

    .sync_rx_queues = iwl_mvm_sync_rx_queues,

    CFG80211_TESTMODE_CMD(iwl_mvm_mac_testmode_cmd)

#ifdef CONFIG_PM_SLEEP
        /* look at d3.c */
        .suspend = iwl_mvm_suspend,
    .resume = iwl_mvm_resume,
    .set_wakeup = iwl_mvm_set_wakeup,
    .set_rekey_data = iwl_mvm_set_rekey_data,
#if IS_ENABLED(CONFIG_IPV6)
    .ipv6_addr_change = iwl_mvm_ipv6_addr_change,
#endif
    .set_default_unicast_key = iwl_mvm_set_default_unicast_key,
#endif
    .get_survey = iwl_mvm_mac_get_survey,
    .sta_statistics = iwl_mvm_mac_sta_statistics,

    .start_nan = iwl_mvm_start_nan,
    .stop_nan = iwl_mvm_stop_nan,
    .add_nan_func = iwl_mvm_add_nan_func,
    .del_nan_func = iwl_mvm_del_nan_func,

    .can_aggregate_in_amsdu = iwl_mvm_mac_can_aggregate,
#ifdef CPTCFG_IWLWIFI_DEBUGFS
    .sta_add_debugfs = iwl_mvm_sta_add_debugfs,
#endif
#endif  // NEEDS_PORTING
};
