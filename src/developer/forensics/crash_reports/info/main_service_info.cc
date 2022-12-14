// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/info/main_service_info.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/forensics/feedback/config.h"

namespace forensics {
namespace crash_reports {

MainServiceInfo::MainServiceInfo(std::shared_ptr<InfoContext> context)
    : context_(std::move(context)) {
  FX_CHECK(context_);
}

void MainServiceInfo::ExposeConfig(const feedback::BuildTypeConfig& config) {
  // TODO(fxbug.dev/111700): expose config  at a high-level in the Feedback component.
  context_->InspectManager().ExposeConfig(config);
}

}  // namespace crash_reports
}  // namespace forensics
