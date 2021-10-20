// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use ffx_emulator_start_args::StartCommand;
use fidl_fuchsia_developer_bridge as bridge;
use fvdl_emulator_common::vdl_files::VDLFiles;

pub async fn start(
    cmd: StartCommand,
    daemon_proxy: bridge::DaemonProxy,
) -> Result<(), anyhow::Error> {
    std::process::exit(
        VDLFiles::new(cmd.sdk, cmd.verbose)?.start_emulator(&cmd, Some(&daemon_proxy)).await?,
    )
}
