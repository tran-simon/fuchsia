// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_FIDL_METADATA_TEE_H_
#define SRC_DEVICES_LIB_FIDL_METADATA_TEE_H_

#include <lib/stdcompat/span.h>
#include <lib/zx/result.h>
#include <stdint.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <string>
#include <vector>

namespace fidl_metadata::tee {

struct raw_uuid_t {
  uint32_t time_low;
  uint16_t time_mid;
  uint16_t time_hi_and_version;
  uint8_t clock_seq_and_node[8];
};

struct CustomThreadConfig {
  std::string role;
  uint32_t count;
  std::vector<raw_uuid_t> trusted_apps;
};

// Convert an Tee Thread Config to fuchsia.hardware.tee.TeeMetadata encoded
// in a FIDL byte stream, using the convention for data persistence:
// https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0120_standalone_use_of_fidl_wire_format?hl=en#convention_for_data_persistence
zx::result<std::vector<uint8_t>> TeeMetadataToFidl(
    uint32_t default_thread_count, cpp20::span<const CustomThreadConfig> thread_config);

}  // namespace fidl_metadata::tee

#endif  // SRC_DEVICES_LIB_FIDL_METADATA_TEE_H_
