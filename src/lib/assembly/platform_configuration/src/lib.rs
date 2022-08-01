// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::ensure;
use assembly_config_schema::product_config::{BuildType, FeatureControl, ProductAssemblyConfig};
use fidl_fuchsia_logger::MAX_TAGS;
use std::collections::{BTreeMap, BTreeSet};

/// ffx config flag for enabling configuring the assembly+structured config example.
const EXAMPLE_ENABLED_FLAG: &str = "assembly_example_enabled";

const BASE_CONSOLE_ALLOWED_TAGS: &[&str] = &[
    "console-launcher",
    "driver_manager.cm",
    "driver_host2.cm",
    "driver",
    "device",
    "mdns",
    "netcfg",
    "netstack",
    "fshost",
    "blobfs",
    "minfs",
    "wlan",
    "fxfs",
    "sshd-host",
];
static_assertions::const_assert!(BASE_CONSOLE_ALLOWED_TAGS.len() <= MAX_TAGS as usize);

const BASE_CONSOLE_DENIED_TAGS: &[&str] = &["NUD", "klog"];

/// Convert the high-level description of product configuration into a series of configuration
/// value files with concrete component tuples.
///
/// Returns a map from components manifest paths to configuration fields.
pub fn define_bootfs_config(config: &ProductAssemblyConfig) -> anyhow::Result<PackageConfigPatch> {
    let mut bootfs_patches = PackageConfigPatch::default();

    // Configure the serial console.
    let allowed_log_tags = {
        let mut allowed_log_tags: BTreeSet<_> =
            BASE_CONSOLE_ALLOWED_TAGS.iter().map(|s| s.to_string()).collect();

        let num_product_tags = config.platform.additional_serial_log_tags.len();
        let max_product_tags = MAX_TAGS as usize - BASE_CONSOLE_ALLOWED_TAGS.len();
        ensure!(
            num_product_tags <= max_product_tags,
            "Max {max_product_tags} tags can be forwarded to serial, got {num_product_tags}."
        );
        allowed_log_tags.extend(config.platform.additional_serial_log_tags.iter().cloned());
        allowed_log_tags.into_iter().collect::<Vec<_>>()
    };
    let denied_log_tags: Vec<_> = BASE_CONSOLE_DENIED_TAGS.iter().map(|s| s.to_string()).collect();
    bootfs_patches
        .component("meta/console.cm")
        .field("allowed_log_tags", allowed_log_tags)
        .field("denied_log_tags", denied_log_tags);

    Ok(bootfs_patches)
}

/// Convert the high-level description of product configuration into a series of configuration
/// value files with concrete package/component tuples.
///
/// Returns a map from package names to configuration updates.
pub fn define_repackaging(
    config: &ProductAssemblyConfig,
) -> anyhow::Result<StructuredConfigPatches> {
    let mut patches = PatchesBuilder::default();

    // Configure the Product Assembly + Structured Config example, if enabled.
    if should_configure_example() {
        // [START example_patches]
        patches
            .package("configured_by_assembly")
            .component("meta/to_configure.cm")
            .field("enable_foo", matches!(config.platform.build_type, BuildType::Eng));
        // [END example_patches]
    }

    // Configure the session URL.
    ensure!(
        config.product.session_url.is_empty()
            || config.product.session_url.starts_with("fuchsia-pkg://"),
        "valid session URLs must start with `fuchsia-pkg://`, got `{}`",
        config.product.session_url
    );
    patches
        .package("session_manager")
        .component("meta/session_manager.cm")
        .field("session_url", config.product.session_url.to_owned());

    // Configure enabling pinweaver.
    patches
        .package("password_authenticator")
        .component("meta/password-authenticator.cm")
        .field(
            "allow_pinweaver",
            config.platform.identity.password_pinweaver != FeatureControl::Disabled,
        )
        .field(
            "allow_scrypt",
            config.platform.identity.password_pinweaver != FeatureControl::Required,
        );

    Ok(patches.inner)
}

/// Check ffx config for whether we should execute example code.
fn should_configure_example() -> bool {
    futures::executor::block_on(ffx_config::get::<bool, _>(EXAMPLE_ENABLED_FLAG))
        .unwrap_or_default()
}

/// A builder for collecting all of the structure configuration repackaging to perform in a given
/// system.
#[derive(Default)]
struct PatchesBuilder {
    inner: StructuredConfigPatches,
}

impl PatchesBuilder {
    fn package(&mut self, name: &str) -> &mut PackageConfigPatch {
        self.inner.entry(name.to_string()).or_default()
    }
}

/// A map from package names to patches to apply to their structured configuration.
pub type StructuredConfigPatches = BTreeMap<String, PackageConfigPatch>;

#[derive(Clone, Debug, Default)]
pub struct PackageConfigPatch {
    /// A map from manifest paths within the package namespace to the values for the component.
    pub components: BTreeMap<String, ComponentConfig>,
}

impl PackageConfigPatch {
    fn component(&mut self, pkg_path: &str) -> &mut ComponentConfig {
        assert!(
            self.components.insert(pkg_path.to_owned(), Default::default()).is_none(),
            "each component's config can only be defined once"
        );
        self.components.get_mut(pkg_path).expect("just inserted this value")
    }
}

#[derive(Clone, Debug, Default)]
pub struct ComponentConfig {
    pub fields: BTreeMap<String, serde_json::Value>,
}

impl ComponentConfig {
    fn field(&mut self, key: &str, value: impl Into<serde_json::Value>) -> &mut Self {
        assert!(
            self.fields.insert(key.to_owned(), value.into()).is_none(),
            "each configuration key can only be defined once"
        );
        self
    }
}
