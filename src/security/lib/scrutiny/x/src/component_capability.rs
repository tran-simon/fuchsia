// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/111244): Implement production component capability API.

#[cfg(test)]
pub mod fake {
    use crate::api::CapabilityDestination;
    use crate::api::CapabilityKind;
    use crate::api::CapabilitySource;
    use crate::api::ComponentCapability as ComponentCapabilityApi;
    use crate::api::ComponentCapabilityName as ComponentCapabilityNameApi;
    use crate::api::ComponentCapabilityPath as ComponentCapabilityPathApi;
    use crate::component::fake::Component;

    #[derive(Default)]
    pub(crate) struct ComponentCapability;

    impl ComponentCapabilityApi for ComponentCapability {
        type Component = Component;
        type CapabilityName = ComponentCapabilityName;
        type CapabilityPath = ComponentCapabilityPath;

        fn component(&self) -> Self::Component {
            Component::default()
        }

        fn kind(&self) -> CapabilityKind {
            CapabilityKind::Unknown
        }

        fn source(&self) -> CapabilitySource {
            CapabilitySource::Unknown
        }

        fn destination(&self) -> CapabilityDestination {
            CapabilityDestination::Unknown
        }

        fn source_name(&self) -> Option<Self::CapabilityName> {
            None
        }

        fn destination_name(&self) -> Option<Self::CapabilityName> {
            None
        }

        fn source_path(&self) -> Option<Self::CapabilityPath> {
            None
        }

        fn destination_path(&self) -> Option<Self::CapabilityPath> {
            None
        }
    }

    /// TODO(fxbug.dev/111244): Implement for production component capability API.
    #[derive(Default)]
    pub(crate) struct ComponentCapabilityName;

    impl ComponentCapabilityNameApi for ComponentCapabilityName {
        type ComponentCapability = ComponentCapability;

        fn component(&self) -> Self::ComponentCapability {
            ComponentCapability::default()
        }
    }

    /// TODO(fxbug.dev/111244): Implement for production component capability API.
    #[derive(Default)]
    pub(crate) struct ComponentCapabilityPath;

    impl ComponentCapabilityPathApi for ComponentCapabilityPath {
        type ComponentCapability = ComponentCapability;

        fn component(&self) -> Self::ComponentCapability {
            ComponentCapability::default()
        }
    }
}
