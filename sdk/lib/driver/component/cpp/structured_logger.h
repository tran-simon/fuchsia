// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_DRIVER_COMPONENT_CPP_STRUCTURED_LOGGER_H_
#define LIB_DRIVER_COMPONENT_CPP_STRUCTURED_LOGGER_H_

#include <lib/driver/component/cpp/logger_internal.h>

// Used to denote a key-value pair for use in structured logging API calls.
// This macro exists solely to improve readability of calls to FX_SLOG
#define KV(a, b) a, b

// Structured logging internal macro
#define FDF_SLOG_ETC(flag, args...)                                     \
  do {                                                                  \
    driver_internal::fx_slog(*logger_, flag, __FILE__, __LINE__, args); \
  } while (0)

// Structured logging macro
#define FDF_SLOG(flag, msg...) FDF_SLOG_ETC(FUCHSIA_LOG_##flag, msg)

#endif  // LIB_DRIVER_COMPONENT_CPP_STRUCTURED_LOGGER_H_
