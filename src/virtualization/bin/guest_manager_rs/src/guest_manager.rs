// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/115695): Remove.
#![allow(unused_variables, unused_imports, dead_code)]

use {
    crate::guest_config,
    anyhow::{anyhow, Error},
    fidl::endpoints::{Proxy, ServerEnd},
    fidl_fuchsia_net::MacAddress,
    fidl_fuchsia_net_interfaces as ninterfaces,
    fidl_fuchsia_virtualization::{
        GuestConfig, GuestDescriptor, GuestError, GuestLifecycleMarker, GuestLifecycleProxy,
        GuestManagerConnectResponder, GuestManagerError, GuestManagerForceShutdownResponder,
        GuestManagerGetInfoResponder, GuestManagerLaunchResponder, GuestManagerRequest,
        GuestManagerRequestStream, GuestMarker, GuestStatus,
    },
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_protocol,
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc::{self, UnboundedSender},
        select_biased,
        stream::SelectAll,
        FutureExt, Stream, StreamExt,
    },
    std::{fmt, rc::Rc},
};

// This is a locally administered MAC address (first byte 0x02) mixed with the
// Google Organizationally Unique Identifier (00:1a:11). The host gets ff:ff:ff
// and the guest gets 00:00:00 for the last three octets.
const DEFAULT_GUEST_MAC_ADDRESS: MacAddress =
    MacAddress { octets: [0x02u8, 0x1a, 0x11, 0x00, 0x01, 0x00] };

fn get_default_guest_memory() -> u64 {
    // There are no assumptions made by this unsafe block; it is only unsafe due to FFI.
    let host_memory = unsafe { zx::sys::zx_system_get_physmem() };
    let max_reserved_host_memory = 3 * (1u64 << 30); // 3 GiB.

    // Reserve half the host memory up to 3 GiB, and allow the rest to be used by the guest.
    host_memory - std::cmp::min(host_memory / 2, max_reserved_host_memory)
}

fn get_default_num_cpus() -> u8 {
    // There are no assumptions made by this unsafe block; it is only unsafe due to FFI.
    let num_system_cpus: u32 = unsafe { zx::sys::zx_system_get_num_cpus() };
    std::cmp::min(num_system_cpus, u8::MAX.into())
        .try_into()
        .expect("this value is known to be no larger than u8::MAX")
}

enum GuestNetworkState {
    // There are at least enough virtual device interfaces to match the guest configuration, and
    // if there is a bridged configuration, there's at least one bridged interface. This doesn't
    // guarantee working networking, but means that the system state is likely correct.
    Ok = 0,

    // This guest wasn't started with a network device, so no networking is expected.
    NoNetworkDevice = 1,

    // Failed to query network interfaces. Check component routing if this is unexpected.
    FailedToQuery = 2,

    // Host doesn't have a WLAN or ethernet interface, so there's probably no guest networking.
    NoHostNetworking = 3,

    // There's at least one missing virtual interface that was expected to be present. Check
    // virtio-net device logs for a failure.
    MissingVirtualInterfaces = 4,

    // An interface is bridged, there's an ethernet interface to bridge against, but the
    // bridge hasn't been created yet. This might be a transient issue while the bridge is created.
    NoBridgeCreated = 5,

    // An interface is bridged, and there's no ethernet to bridge against, but the host is
    // connected to WLAN. This is a common user misconfiguration.
    AttemptedToBridgeWithWlan = 6,
}

impl fmt::Display for GuestNetworkState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Provide a user friendly explanation for each network state. See
        // GuestManager::GuestNetworkStateToStringExplanation for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }
}

enum GuestManagerStateUpdate {
    Status(GuestStatus),
    GuestDescriptor(GuestDescriptor),
    Started(zx::Time),
    Stopped(zx::Time),
    Error(GuestError),
    ClearError,
}

pub struct GuestManager {
    // Path to the default guest config.
    config_path: String,

    // The current state of the VMM managed by this guest manager.
    status: GuestStatus,

    // Cached error reported by the VMM upon stopping, if not stopped due to a clean shutdown.
    last_error: Option<GuestError>,

    // Snapshot of some of the configuration settings used to start this guest. This is
    // informational only, and sent in response to a get_info call.
    guest_descriptor: GuestDescriptor,

    // Start and stop time for a guest, used to calculate the guest's uptime.
    start_time: zx::Time,
    stop_time: zx::Time,
}

impl GuestManager {
    pub fn new_with_defaults() -> Self {
        GuestManager::new("/guest_pkg/data/guest.cfg".to_string())
    }

    fn new(config_path: String) -> Self {
        GuestManager {
            config_path,
            status: GuestStatus::NotStarted,
            last_error: None,
            guest_descriptor: GuestDescriptor::EMPTY,
            start_time: zx::Time::INFINITE_PAST,
            stop_time: zx::Time::INFINITE_PAST,
        }
    }

    pub async fn run<St: Stream<Item = GuestManagerRequestStream> + Unpin>(
        &mut self,
        mut lifecycle: Rc<GuestLifecycleProxy>,
        request_streams: St,
    ) -> Result<(), Error> {
        let mut on_closed = lifecycle.on_closed().extend_lifetime().fuse();
        let (state_tx, mut state_rx) = mpsc::unbounded::<GuestManagerStateUpdate>();

        let mut request_streams = request_streams.fuse();
        let mut connections = SelectAll::new();

        loop {
            select_biased! {
                result = on_closed => {
                    result.map_err(|err| anyhow!(
                        "failed to wait on guest lifecycle proxy closed: {}", err))?;
                    tracing::error!("VMM component has unexpectedly stopped");
                    state_tx
                        .unbounded_send(
                            GuestManagerStateUpdate::Status(GuestStatus::VmmUnexpectedTermination))
                        .expect("unexpected end of state tx stream");

                    // The VMM component has terminated, create a new one by opening a new
                    // lifecycle channel.
                    lifecycle = Rc::new(connect_to_protocol::<GuestLifecycleMarker>()?);
                    on_closed = lifecycle.on_closed().extend_lifetime().fuse();
                }
                stream = request_streams.next() => {
                    connections.push(stream.ok_or(anyhow!(
                        "unexpected end of stream of guest manager request streams"))?);
                }
                state = state_rx.next() => {
                    self.apply_state_update(state.expect("unexpected end of state rx stream"));
                }
                request = connections.next() => {
                    let request = match request {
                        None => {
                            // Clean end of this connection stream.
                            continue;
                        }
                        Some(result) => match result {
                            Ok(request) => request,
                            Err(err) => {
                                tracing::error!(%err, "Connection stream ended with reason");
                                continue;
                            }
                        }
                    };

                    match request {
                        GuestManagerRequest::Launch { guest_config, controller, responder } => {
                            let config = self.get_merged_config(guest_config, state_tx.clone());
                            if let Err(err) = config {
                                // TODO(fxbug.dev/115695): Log and respond via responder.
                                continue;
                            }
                            let config = config.unwrap();

                            let create =
                                GuestManager::send_create_request(lifecycle.clone(), config).await;
                            if let Err(err) = create {
                                // TODO(fxbug.dev/115695): Log and respond via responder.
                                GuestManager::handle_guest_stopped(create, state_tx.clone());
                                continue;
                            }

                            // The guest has been created, so respond a success value via the
                            // responder, connect the controller (see the connect function), and
                            // send a state update giving the guest manager a Running status.
                            // TODO(fxbug.dev/115695): Remove this comment when done.

                            // Run returns when the guest has stopped. Detach this long running
                            // async call to prevent blocking other tasks from running.
                            fasync::Task::local(async move {
                                let run = GuestManager::send_run_request(lifecycle.clone()).await;
                                GuestManager::handle_guest_stopped(run, state_tx.clone());
                            }).detach();

                            unimplemented!();
                        }
                        GuestManagerRequest::ForceShutdown { responder } => {
                            // Check if the guest is running (see the is_guest_started function).
                            // If it's not running, respond immediately and return. Otherwise update
                            // the guest state.
                            // TODO(fxbug.dev/115695): Remove this comment when done.

                            if let Err(err) = lifecycle.stop().await {
                                tracing::error!(%err, "failed to send Stop FIDL call");
                            }

                            // TODO(fxbug.dev/115695): Respond to the caller via the responder.
                            unimplemented!();
                        }
                        GuestManagerRequest::Connect { controller, responder } => {
                            // If the guest is running (see the is_guest_started function), connect
                            // the controller (see the connect function). Respond via the responder
                            // with either success or GuestManagerError::NotRunning.
                            // TODO(fxbug.dev/115695): Remove this comment when done.
                            unimplemented!();
                        }
                        GuestManagerRequest::GetInfo { responder } => {
                            // Get the network interface watcher proxy, copy the descriptor, and
                            // call query_guest_network_state to retrieve the network state. Use
                            // that state to get diagnostic strings via check_for_problems, and
                            // ultimately call get_info with the data.
                            // TODO(fxbug.dev/115695): Remove this comment when done.
                            unimplemented!();
                        }
                    };
                }
            }
        }
    }

    fn apply_state_update(&mut self, update: GuestManagerStateUpdate) {
        // Check for what enum value the state update is, and do the appropriate action.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
    }

    fn handle_guest_stopped(
        reason: Result<(), GuestError>,
        state_tx: UnboundedSender<GuestManagerStateUpdate>,
    ) {
        // Send a state update setting the status to Stopped, provide a stop time, and set a
        // last error if needed.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    fn get_merged_config(
        &self,
        user_config: GuestConfig,
        state_tx: UnboundedSender<GuestManagerStateUpdate>,
    ) -> Result<GuestConfig, Error> {
        // 1) Get the default guest config, merge it with the user provided config.
        // 2) Set any additional config defaults (memory, cpus, and default net interface).
        // 3) Send a state update, including:
        //      - The start time
        //      - Setting the status to started
        //      - The snapshotted config (see the snapshot_config function)
        //      - A cleared last error
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    pub fn connect(controller: ServerEnd<GuestMarker>) -> Result<(), Error> {
        // Connect the ServerEnd to the VMM component. See ProtocolConnector::connect_with in
        // fuchsia-component.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    pub fn get_info(
        &self,
        detected_problems: Vec<String>,
        responder: GuestManagerGetInfoResponder,
    ) -> Result<(), Error> {
        // Fill a GuestInfo message and send it via the responder. See GuestManager::GetInfo
        // for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    async fn query_guest_network_state(
        descriptor: GuestDescriptor,
        watcher: ninterfaces::WatcherProxy,
    ) -> GuestNetworkState {
        // Check the guest network config settings (stored via snapshot_config) against the
        // host network interfaces to get a network state. See GuestManager::QueryGuestNetworkState
        // for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    fn check_for_problems(network_state: GuestNetworkState) -> Vec<String> {
        // Helper function called by get_info to obtain some diagnostic strings. Ignore the
        // memory pressure handler for now. See GuestManager::CheckForProblems for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    fn get_default_guest_config(&self) -> Result<GuestConfig, GuestManagerError> {
        // Read the default guest config from the file `self.config_path`, and parse it into a
        // GuestConfig. See GuestManager::GetDefaultGuestConfig for an example.
        unimplemented!();
    }

    fn snapshot_config(config: &GuestConfig) -> GuestDescriptor {
        // Store some config fields in a GuestDescriptor struct. See GuestManager::SnapshotConfig
        // for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    fn is_guest_started(&self) -> bool {
        // Check the guest status and return true if it has started. See
        // GuestManager::is_guest_started for an example.
        // TODO(fxbug.dev/115695): Implement this function and remove this comment.
        unimplemented!();
    }

    async fn send_create_request(
        lifecycle: Rc<GuestLifecycleProxy>,
        config: GuestConfig,
    ) -> Result<(), GuestError> {
        let result = lifecycle.create(config).await;
        if let Err(err) = result {
            tracing::error!(%err, "failed to send Create FIDL call");
            Err(GuestError::InternalError)
        } else {
            result.unwrap()
        }
    }

    async fn send_run_request(lifecycle: Rc<GuestLifecycleProxy>) -> Result<(), GuestError> {
        let result = lifecycle.run().await;
        if let Err(err) = result {
            tracing::error!(%err, "failed to send Run FIDL call");
            Err(GuestError::InternalError)
        } else {
            result.unwrap()
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, async_utils::PollExt, fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_virtualization::GuestManagerMarker,
    };

    impl GuestManager {
        fn new_for_test(config_path: String) -> Self {
            GuestManager::new(config_path)
        }
    }

    #[fuchsia::test]
    fn vmm_component_crash() {
        let mut executor = fasync::TestExecutor::new().expect("failed to create test executor");
        let mut manager = GuestManager::new_for_test("foo".to_string());
        let (stream_tx, state_rx) = mpsc::unbounded::<GuestManagerRequestStream>();

        let (proxy, server) = create_proxy_and_stream::<GuestLifecycleMarker>()
            .expect("failed to create proxy/stream");

        let run_fut = manager.run(Rc::new(proxy), state_rx);
        futures::pin_mut!(run_fut);

        // No connections.
        assert!(executor.run_until_stalled(&mut run_fut).is_pending());

        let (manager_proxy, manager_server) =
            create_proxy_and_stream::<GuestManagerMarker>().expect("failed to create proxy/stream");
        stream_tx.unbounded_send(manager_server).expect("stream should never close");

        // There's now a connection sent via mpsc.
        assert!(executor.run_until_stalled(&mut run_fut).is_pending());

        // TODO(fxbug.dev/115695): Call get_info and check for a NotStarted status.

        // Dropping the server closes the connection. The status should change, and a new
        // connection should be established.
        drop(server);
        assert!(executor.run_until_stalled(&mut run_fut).is_pending());

        // TODO(fxbug.dev/115695): Call get_info and check for a VmmUnexpectedTermination status.
    }

    #[fuchsia::test]
    async fn launch_fails_due_to_invalid_config_path() {
        // Pass an invalid config path to GuestManager and ensure launch fails with BAD_CONFIG.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    async fn launch_fails_due_to_bad_config_schema() {
        // Pass a real config file with an invalid schema and ensure launch fails with BAD_CONFIG.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn force_shutdown_non_running_guest() {
        // Call force shutdown on a guest that isn't running, and ensure the state doesn't change.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn force_shutdown_guest() {
        // Launch a guest, check the state, force shutdown, and ensure the state channged to
        // a stop state.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn failed_to_create_and_initialize_vmm_with_restart() {
        // Fail to create the VMM when launch is called, then call launch again and succeed
        // the second time.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_initiated_clean_shutdown() {
        // Launch guest, respond via the run callback, ensure the guest state becomes stopped.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn launch_and_apply_user_guest_config() {
        // Provide a user guest config and check the launch result for whether its merged.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn double_launch_fails() {
        // Call launch twice, ensure the second launch returns ALREADY_RUNNING.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn launch_and_get_info() {
        // Call launch, and then call get_info and check the results.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn connect_to_vmm() {
        // Call connect, and ensure failure. Call launch, call connect, and check for success.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn duplicate_listeners_provided_by_user_guest_config() {
        // Duplicate listeners, call launch, check for a failure.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn user_provided_initial_listeners() {
        // Set listeners, call launch, check config.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_probably_has_networking() {
        // Create a situation where querying guest network state returns an ok.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_no_network_devices() {
        // Create a situation where querying guest network state returns no network device.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_bridging_required_but_host_on_wifi() {
        // Create a situation where querying guest network state returns attempted to bridge with
        // wlan.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_bridging_required_and_host_on_wifi_and_ethernet() {
        // Create a situation where querying guest network state returns no bridge created.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_bridging_required_and_host_on_ethernet() {
        // Create a situation where querying guest network state returns no bridge created.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn not_enough_virtual_interfaces_for_guest() {
        // Create a situation where querying guest network state returns missing virtual interfaces.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    fn guest_requires_networking_but_no_host_networking() {
        // Create a situation where querying guest network state returns no host networking.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }
}
