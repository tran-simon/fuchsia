// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_THERMAL_H_
#define SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_THERMAL_H_

#include <fidl/fuchsia.hardware.thermal/cpp/wire.h>

#define MAX_VOLTAGE_TABLE 37
#define MAX_DVFS_TABLE 3

#define AMLOGIC_SMC_GET_DVFS_TABLE_INDEX 0x82000088

using aml_voltage_table_t = struct {
  uint32_t microvolt;
  uint32_t duty_cycle;
};

using aml_thermal_info_t = struct {
  aml_voltage_table_t voltage_table[MAX_VOLTAGE_TABLE];
  uint32_t initial_cluster_frequencies[fuchsia_hardware_thermal::wire::kMaxDvfsDomains];
  uint32_t voltage_pwm_period_ns;
  // Multiple DVFS tables are specified for Nelson, and one gets selected by a secure monitor call
  // at boot. The thermal driver will use these tables only if it gets an SMC resource, otherwise it
  // use the tables in ThermalDeviceInfo as usual.
  fuchsia_hardware_thermal::wire::OperatingPoint
      opps[fuchsia_hardware_thermal::wire::kMaxDvfsDomains][MAX_DVFS_TABLE];
  // Maps PowerDomain to cluster numbers used by the secure monitor.
  uint64_t cluster_id_map[fuchsia_hardware_thermal::wire::kMaxDvfsDomains];
};

#endif  // SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_THERMAL_H_
