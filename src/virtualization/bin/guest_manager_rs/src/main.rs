// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod guest_config;
mod guest_manager;

use {
    anyhow::{anyhow, Error},
    fidl_fuchsia_virtualization::{GuestLifecycleMarker, GuestManagerRequestStream},
    fuchsia_component::{client::connect_to_protocol, server},
    guest_manager::GuestManager,
    std::rc::Rc,
};

#[fuchsia::main(logging = true, threads = 1)]
async fn main() -> Result<(), Error> {
    let mut fs = server::ServiceFs::new();
    fs.dir("svc").add_fidl_service(|stream: GuestManagerRequestStream| stream);
    fs.take_and_serve_directory_handle()
        .map_err(|err| anyhow!("Error starting server: {}", err))?;

    let mut manager = GuestManager::new_with_defaults();
    let lifecycle = Rc::new(connect_to_protocol::<GuestLifecycleMarker>()?);
    if let Err(err) = manager.run(lifecycle, fs).await {
        tracing::error!(%err, "failed to run guest manager");
        Err(err)
    } else {
        Ok(())
    }
}
