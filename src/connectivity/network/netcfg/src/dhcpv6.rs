// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl::endpoints::Responder as _;
use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_dhcpv6 as fnet_dhcpv6;
use fidl_fuchsia_net_name as fnet_name;

use anyhow::Context as _;
use async_utils::stream::WithTag as _;
use dns_server_watcher::{DnsServers, DnsServersUpdateSource};
use futures::future::TryFutureExt as _;
use futures::stream::StreamExt as _;

use crate::errors::{self, ContextExt as _};
use crate::{dns, DnsServerWatchers};

/// Start a DHCPv6 client for the specified host interface.
pub(super) fn start_client(
    dhcpv6_client_provider: &fnet_dhcpv6::ClientProviderProxy,
    interface_id: u64,
    sockaddr: fnet::Ipv6SocketAddress,
    watchers: &mut DnsServerWatchers<'_>,
) -> Result<(), errors::Error> {
    let source = DnsServersUpdateSource::Dhcpv6 { interface_id };
    if watchers.contains_key(&source) {
        return Err(errors::Error::Fatal(anyhow::anyhow!(
            "interface with id={} already has a DHCPv6 client",
            interface_id
        )));
    }

    let params = fnet_dhcpv6::NewClientParams {
        interface_id: Some(interface_id),
        address: Some(sockaddr),
        config: Some(fnet_dhcpv6::ClientConfig {
            information_config: Some(fnet_dhcpv6::InformationConfig {
                dns_servers: Some(true),
                ..fnet_dhcpv6::InformationConfig::EMPTY
            }),
            ..fnet_dhcpv6::ClientConfig::EMPTY
        }),
        ..fnet_dhcpv6::NewClientParams::EMPTY
    };
    let (client, server) = fidl::endpoints::create_proxy::<fnet_dhcpv6::ClientMarker>()
        .context("error creating DHCPv6 client fidl endpoints")
        .map_err(errors::Error::Fatal)?;

    // Not all environments may have a DHCPv6 client service so we consider this a
    // non-fatal error.
    let () = dhcpv6_client_provider
        .new_client(params, server)
        .context("error creating new DHCPv6 client")
        .map_err(errors::Error::NonFatal)?;

    let stream = futures::stream::try_unfold(client, move |proxy| {
        proxy.watch_servers().map_ok(move |s| Some((s, proxy)))
    })
    .map(move |r| {
        r.with_context(|| {
            format!(
                "error getting next event from DHCPv6 DNS server watcher for interface ID = {}",
                interface_id
            )
        })
    })
    .tagged(source);

    if watchers.insert(source, stream.boxed()).is_some() {
        return Err(errors::Error::Fatal(anyhow::anyhow!(
            "interface with id={} should not have a DHCPv6 client",
            interface_id
        )));
    }

    Ok(())
}

/// Stops the DHCPv6 client running on the specified host interface.
///
/// Any DNS servers learned by the client will be cleared.
pub(super) async fn stop_client(
    lookup_admin: &fnet_name::LookupAdminProxy,
    dns_servers: &mut DnsServers,
    interface_id: u64,
    watchers: &mut DnsServerWatchers<'_>,
) -> Result<(), errors::Error> {
    let source = DnsServersUpdateSource::Dhcpv6 { interface_id };

    // Dropping the client end of the Client interface should stop the
    // DHCPv6 client.
    if watchers.remove(&source).is_none() {
        // Should never happen as we only set the DHCPv6 client
        // socket address if we successfully create a client.
        return Err(errors::Error::Fatal(anyhow::anyhow!(
            "expected to remove a DNS watcher for host interface with id={}",
            interface_id
        )));
    }

    dns::update_servers(lookup_admin, dns_servers, source, vec![])
        .await
        .context("error clearing DNS servers")
}

#[derive(Default)]
pub(super) struct PrefixProviderHandler {
    pub(super) prefix_control_request_stream: Option<fnet_dhcpv6::PrefixControlRequestStream>,
    pub(super) watch_prefix_responder: Option<fnet_dhcpv6::PrefixControlWatchPrefixResponder>,
}

impl PrefixProviderHandler {
    pub(super) fn on_prefix_control_client_close(&mut self) {
        let Self { prefix_control_request_stream, watch_prefix_responder: _ } = self;
        let _: fnet_dhcpv6::PrefixControlRequestStream = prefix_control_request_stream
            .take()
            .expect("no fuchsia.net.dhcpv6/PrefixControl request stream to remove");
    }

    pub(super) fn handle_dhcpv6_prefix_control_request(
        &mut self,
        req: fnet_dhcpv6::PrefixControlRequest,
    ) -> Result<(), errors::Error> {
        let Self { prefix_control_request_stream: _, watch_prefix_responder } = self;
        let fnet_dhcpv6::PrefixControlRequest::WatchPrefix { responder } = req;
        if let Some(responder) = watch_prefix_responder.take() {
            self.on_prefix_control_client_close();
            return responder
                .control_handle()
                .send_on_exit(fnet_dhcpv6::PrefixControlExitReason::DoubleWatch)
                .context("send DoubleWatch OnExit reason")
                .map_err(errors::Error::NonFatal);
        }
        *watch_prefix_responder = Some(responder);
        Ok(())
    }
}
