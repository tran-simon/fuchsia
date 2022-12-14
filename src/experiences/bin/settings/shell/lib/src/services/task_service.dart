// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Defines a service that can be started and stopped.
abstract class TaskService {
  Future<void> start();
  Future<void> stop();
  void dispose();
}
