// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>

#include "simple-display.h"
#include "src/graphics/display/drivers/simple/simple-nv-bind.h"

static zx_status_t nv_disp_bind(void* ctx, zx_device_t* dev) {
  // framebuffer bar seems to be 1
  return bind_simple_pci_display_bootloader(dev, "nv", 1u, /*use_fidl=*/false);
}

static zx_driver_ops_t nv_disp_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = nv_disp_bind,
};

ZIRCON_DRIVER(nv_disp, nv_disp_driver_ops, "zircon", "0.1");
