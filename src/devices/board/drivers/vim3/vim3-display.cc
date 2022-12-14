// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>

#include <soc/aml-a311d/a311d-gpio.h>
#include <soc/aml-a311d/a311d-hw.h>

#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"
#include "vim3-gpios.h"
#include "vim3.h"

namespace vim3 {
namespace fpbus = fuchsia_hardware_platform_bus;

static const std::vector<fpbus::Mmio> display_mmios{
    {{
        // VBUS/VPU
        .base = A311D_VPU_BASE,
        .length = A311D_VPU_LENGTH,
    }},
    {},
    {},
    {{
        // HHI
        .base = A311D_HIU_BASE,
        .length = A311D_HIU_LENGTH,
    }},
    {{
        // AOBUS
        .base = A311D_AOBUS_BASE,
        .length = A311D_AOBUS_LENGTH,
    }},
    {{
        // CBUS
        .base = A311D_CBUS_BASE,
        .length = A311D_CBUS_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> display_irqs{
    {{
        .irq = A311D_VIU1_VSYNC_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
    {{
        .irq = A311D_RDMA_DONE_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
};

static const std::vector<fpbus::Bti> display_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_DISPLAY,
    }},
};

static const fpbus::Node display_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "display";
  dev.vid() = PDEV_VID_AMLOGIC;
  dev.pid() = PDEV_PID_AMLOGIC_A311D;
  dev.did() = PDEV_DID_AMLOGIC_DISPLAY;
  dev.mmio() = display_mmios;
  dev.irq() = display_irqs;
  dev.bti() = display_btis;
  return dev;
}();

// Composite binding rules for display driver.

static const zx_bind_inst_t hpd_gpio_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
    BI_MATCH_IF(EQ, BIND_GPIO_PIN, VIM3_HPD_IN),
};

static const zx_bind_inst_t sysmem_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SYSMEM),
};

static const zx_bind_inst_t canvas_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_AMLOGIC_CANVAS),
};

static const zx_bind_inst_t hdmi_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_HDMI),
};

static const device_fragment_part_t hpd_gpio_fragment[] = {
    {std::size(hpd_gpio_match), hpd_gpio_match},
};

static const device_fragment_part_t sysmem_fragment[] = {
    {std::size(sysmem_match), sysmem_match},
};

static const device_fragment_part_t canvas_fragment[] = {
    {std::size(canvas_match), canvas_match},
};

static const device_fragment_part_t hdmi_fragment[] = {
    {std::size(hdmi_match), hdmi_match},
};

static const device_fragment_t fragments[] = {
    {"gpio", std::size(hpd_gpio_fragment), hpd_gpio_fragment},
    {"sysmem", std::size(sysmem_fragment), sysmem_fragment},
    {"canvas", std::size(canvas_fragment), canvas_fragment},
    {"hdmi", std::size(hdmi_fragment), hdmi_fragment},
};

zx_status_t Vim3::DisplayInit() {
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('DISP');
  auto result = pbus_.buffer(arena)->AddCompositeImplicitPbusFragment(
      fidl::ToWire(fidl_arena, display_dev),
      platform_bus_composite::MakeFidlFragment(fidl_arena, fragments, std::size(fragments)), {});
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddCompositeImplicitPbusFragment Display(display_dev) request failed: %s",
           __func__, result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddCompositeImplicitPbusFragment Display(display_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace vim3
