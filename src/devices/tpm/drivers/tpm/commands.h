// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_TPM_DRIVERS_TPM_COMMANDS_H_
#define SRC_DEVICES_TPM_DRIVERS_TPM_COMMANDS_H_

#include <endian.h>
#include <fidl/fuchsia.tpm/cpp/wire.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

// The definitions here are spread across two parts of the TPM2 spec:
// - part 2, "structures".
// - part 3, "commands".
//
// https://trustedcomputinggroup.org/wp-content/uploads/TCG_TPM2_r1p59_Part2_Structures_pub.pdf
// https://trustedcomputinggroup.org/wp-content/uploads/TCG_TPM2_r1p59_Part3_Commands_pub.pdf

#define TPM_ST_NO_SESSIONS (0x8001)
#define TPM_CC_SHUTDOWN (0x0145)

#define TPM_SU_CLEAR 0x00
#define TPM_SU_STATE 0x01

struct TpmCmdHeader {
  uint16_t tag;
  uint32_t command_size;
  uint32_t command_code;
} __PACKED;

struct TpmShutdownCmd {
  TpmCmdHeader hdr;
  uint16_t shutdown_type;

  explicit TpmShutdownCmd(uint16_t type) : shutdown_type(htobe16(type)) {
    hdr = {.tag = htobe16(TPM_ST_NO_SESSIONS),
           .command_size = htobe32(sizeof(TpmShutdownCmd)),
           .command_code = htobe32(TPM_CC_SHUTDOWN)};
  }
} __PACKED;

/// TpmVendorCmd expects |data_src| to not include a header instead
/// the constructor will generate a no-session based TPM header from
/// the provided |command_code|.
struct TpmVendorCmd {
  TpmCmdHeader hdr;
  uint8_t data[fuchsia_tpm::wire::kMaxVendorCommandLen];

  TpmVendorCmd(uint32_t code, cpp20::span<const uint8_t> data_src) {
    ZX_ASSERT(data_src.size_bytes() < sizeof(data));
    hdr = {.tag = htobe16(TPM_ST_NO_SESSIONS),
           .command_size = htobe32(data_src.size() + sizeof(hdr)),
           .command_code = htobe32(code)};
    memcpy(data, data_src.data(), data_src.size_bytes());
  }
} __PACKED;

/// TpmCmd expects |data_src| to include a valid TPM header and will
/// use this to populate |hdr| and |data|.
struct TpmCmd {
  TpmCmdHeader hdr;
  uint8_t data[fuchsia_tpm::wire::kMaxVendorCommandLen];

  TpmCmd(cpp20::span<const uint8_t> data_src) {
    ZX_ASSERT(data_src.size_bytes() > sizeof(hdr));
    ZX_ASSERT(data_src.size_bytes() < sizeof(hdr) + sizeof(data));
    memcpy(&hdr, data_src.data(), sizeof(hdr));
    if (data_src.size_bytes() > sizeof(hdr)) {
      size_t bytes_to_write = data_src.size_bytes() - sizeof(hdr);
      memcpy(data, data_src.data() + sizeof(hdr), bytes_to_write);
    }
  }
};

struct TpmResponseHeader {
  uint16_t tag;
  uint32_t response_size;
  uint32_t response_code;

 public:
  uint32_t ResponseSize() const { return betoh32(response_size); }
  uint32_t ResponseCode() const { return betoh32(response_code); }
} __PACKED;

struct TpmVendorResponse {
  TpmResponseHeader hdr;
  uint8_t data[0];
} __PACKED;

#endif  // SRC_DEVICES_TPM_DRIVERS_TPM_COMMANDS_H_
