// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>

#include <cstdlib>

#include "app.h"
#include "heart_model.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/strings/string_number_conversions.h"

int main(int argc, char* argv[]) {
  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  async::Loop message_loop(&kAsyncLoopConfigAttachToCurrentThread);

  auto heart_model = std::make_unique<bt_le_heart_rate::HeartModel>();
  bt_le_heart_rate::App app(std::move(heart_model));

  std::string interval_option;
  if (command_line.GetOptionValue("interval", &interval_option)) {
    int measurement_interval;

    if (fxl::StringToNumberWithError(interval_option, &measurement_interval)) {
      app.service()->set_measurement_interval(measurement_interval);
    }
  }

  async::PostTask(message_loop.dispatcher(), [&app] { app.StartAdvertising(); });
  message_loop.Run();

  return EXIT_SUCCESS;
}
