// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Use this macro to use the new subtool interfaces in a plugin embedded in ffx (or
/// another subtool). It takes the type that implements `FfxTool` and `FfxMain` as an
/// argument and sets up the global functions that the old `#[plugin()]` interface
/// used to do.
#[macro_export]
macro_rules! embedded_plugin {
    ($tool:ty) => {
        pub async fn ffx_plugin_impl(
            injector: &dyn ffx_core::Injector,
            cmd: <$tool as FfxTool>::Command,
        ) -> $crate::macro_deps::anyhow::Result<()> {
            #[allow(unused_imports)]
            use $crate::macro_deps::{anyhow::Context, argh, global_env_context, Ffx};

            let ffx: &Ffx = &argh::from_env();
            let context = &global_env_context().context("Loading global environment context")?;

            let env = $crate::FhoEnvironment { ffx, context, injector };

            let tool = <$tool>::from_env(env, cmd).await?;
            match $crate::FfxMain::main(tool).await {
                Ok(ok) => Ok(ok),
                Err($crate::Error::User(err)) => Err(err),
                Err($crate::Error::Unexpected(err)) => Err(err),
                other => other.context("Running command (unexpected error type)"),
            }
        }

        pub fn ffx_plugin_writer_output() -> String {
            String::from("Not supported")
        }

        pub fn ffx_plugin_is_machine_supported() -> bool {
            false
        }
    };
}

#[cfg(test)]
mod tests {
    use crate::testing::*;
    use crate::{subtool::FhoHandler, FfxTool};

    // The main testing part will happen in the `main()` function of the tool.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_run_fake_tool_with_legacy_shim() {
        let _test_env = ffx_config::test_init().await.expect("Initializing test environment");
        let (_ffx, injector, tool_cmd) = setup_fho_items::<FakeTool>();

        embedded_plugin!(FakeTool);

        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            0,
            "tool pre-check should not have been called yet"
        );

        assert_eq!(
            ffx_plugin_writer_output(),
            "Not supported",
            "Test plugin should not support machine output"
        );
        assert!(
            !ffx_plugin_is_machine_supported(),
            "Test plugin should not support machine output"
        );

        let fake_tool = match tool_cmd.subcommand {
            FhoHandler::Standalone(t) => t,
            FhoHandler::Metadata(_) => panic!("Not testing metadata generation"),
        };

        ffx_plugin_impl(&injector, fake_tool).await.expect("Plugin to run successfully");

        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            1,
            "tool pre-check should have been called once"
        );
    }
}
