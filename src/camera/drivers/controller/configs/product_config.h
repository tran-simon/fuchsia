// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_CONTROLLER_CONFIGS_PRODUCT_CONFIG_H_
#define SRC_CAMERA_DRIVERS_CONTROLLER_CONFIGS_PRODUCT_CONFIG_H_

#include <fuchsia/camera2/hal/cpp/fidl.h>

#include <string>
#include <vector>

#include "src/camera/drivers/controller/configs/internal_config.h"

namespace camera {

class ProductConfig {
 public:
  static std::unique_ptr<ProductConfig> Create();

  virtual ~ProductConfig() = default;

  virtual std::vector<fuchsia::camera2::hal::Config> ExternalConfigs() const = 0;

  virtual InternalConfigs InternalConfigs() const = 0;

  virtual const char* GetGdcConfigFile(GdcConfig config_type) const = 0;

  virtual std::string ToString() const = 0;

  static fuchsia::camera2::DeviceInfo DeviceInfo();
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_CONTROLLER_CONFIGS_PRODUCT_CONFIG_H_
