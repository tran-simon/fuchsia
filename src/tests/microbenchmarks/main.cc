// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "main.h"

#include <perftest/perftest.h>

#include "round_trips.h"

// The zeroth command line argument, argv[0], used for locating this process's
// executable in order to find dependencies.
const char* argv0;

int main(int argc, char** argv) {
  argv0 = argc >= 1 ? argv[0] : "";

#if defined(__Fuchsia__)
  // Check for the argument used by test cases for launching subprocesses.
  if (argc == 3 && strcmp(argv[1], "--subprocess") == 0) {
    RunSubprocess(argv[2]);
    return 0;
  }
#endif

  return perftest::PerfTestMain(argc, argv, "fuchsia.microbenchmarks");
}
