// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod bound;
mod constants;
mod datatypes;
mod diagnostics;
mod httpsdate;
mod sampler;

use {
    crate::diagnostics::{
        CobaltDiagnostics, CompositeDiagnostics, Diagnostics, InspectDiagnostics,
    },
    crate::httpsdate::{HttpsDateUpdateAlgorithm, RetryStrategy},
    crate::sampler::{HttpsSampler, HttpsSamplerImpl},
    anyhow::{Context, Error},
    fidl_fuchsia_net_interfaces::StateMarker,
    fidl_fuchsia_time_external::{PushSourceRequestStream, Status},
    fuchsia_component::server::{ServiceFs, ServiceObj},
    fuchsia_zircon as zx,
    futures::{
        future::{join, Future},
        FutureExt, StreamExt,
    },
    push_source::PushSource,
    tracing::warn,
};

/// Retry strategy used while polling for time.
const RETRY_STRATEGY: RetryStrategy = RetryStrategy {
    min_between_failures: zx::Duration::from_seconds(1),
    max_exponent: 3,
    tries_per_exponent: 3,
    converge_time_between_samples: zx::Duration::from_minutes(2),
    maintain_time_between_samples: zx::Duration::from_minutes(20),
};

/// URI used to obtain time samples.
// TODO(fxbug.dev/68621): Allow configuration per product.
const REQUEST_URI: &str = "https://clients3.google.com/generate_204";

/// HttpsDate config, populated from build-time generated structured config.
/// TODO(fxb/105777): Remove next line when PullSource support is implemented.
#[allow(dead_code)]
pub struct Config {
    https_timeout: zx::Duration,
    standard_deviation_bound_percentage: u8,
    first_rtt_time_factor: u16,
    use_pull_api: bool,
}

impl From<httpsdate_config::Config> for Config {
    fn from(source: httpsdate_config::Config) -> Self {
        Config {
            https_timeout: zx::Duration::from_seconds(source.https_timeout_sec.into()),
            standard_deviation_bound_percentage: source.standard_deviation_bound_percentage,
            first_rtt_time_factor: source.first_rtt_time_factor,
            use_pull_api: source.use_pull_api,
        }
    }
}

/// Serves `PushSource` FIDL API.
pub struct PushServer<
    S: HttpsSampler + Send + Sync,
    D: Diagnostics,
    N: Future<Output = Result<(), Error>> + Send,
> {
    push_source: PushSource<HttpsDateUpdateAlgorithm<S, D, N>>,
}

impl<S, D, N> PushServer<S, D, N>
where
    S: HttpsSampler + Send + Sync,
    D: Diagnostics,
    N: Future<Output = Result<(), Error>> + Send,
{
    fn new(diagnostics: D, sampler: S, internet_reachable: N) -> Result<Self, Error> {
        let update_algorithm =
            HttpsDateUpdateAlgorithm::new(RETRY_STRATEGY, diagnostics, sampler, internet_reachable);
        let push_source = PushSource::new(update_algorithm, Status::Initializing)?;

        Ok(PushServer { push_source })
    }

    /// Start serving `PushSource` FIDL API.
    fn serve<'a>(
        &'a self,
        fs: &'a mut ServiceFs<ServiceObj<'static, PushSourceRequestStream>>,
    ) -> Result<impl 'a + Future<Output = Result<(), anyhow::Error>>, Error> {
        let update_fut = self.push_source.poll_updates();

        fs.dir("svc").add_fidl_service(|stream: PushSourceRequestStream| stream);
        let service_fut = fs.for_each_concurrent(None, |stream| {
            handle_push_source_request(stream, &self.push_source)
        });
        Ok(join(update_fut, service_fut).map(|(update_result, _serve_result)| update_result))
    }
}

/// Handle next `PushSource` FIDL API request.
async fn handle_push_source_request<T: push_source::UpdateAlgorithm>(
    stream: PushSourceRequestStream,
    push_source: &PushSource<T>,
) {
    push_source
        .handle_requests_for_stream(stream)
        .await
        .unwrap_or_else(|e| warn!("Error handling PushSource stream: {:?}", e));
}

#[fuchsia::main(logging_tags=["time"])]
async fn main() -> Result<(), Error> {
    let config: Config = httpsdate_config::Config::take_from_startup_handle().into();

    let inspect = InspectDiagnostics::new(fuchsia_inspect::component::inspector().root());
    let (cobalt, cobalt_sender_fut) = CobaltDiagnostics::new();
    let diagnostics = CompositeDiagnostics::new(inspect, cobalt);

    let mut fs = ServiceFs::new();
    inspect_runtime::serve(fuchsia_inspect::component::inspector(), &mut fs)?;

    fs.take_and_serve_directory_handle()?;

    let sampler = HttpsSamplerImpl::new(REQUEST_URI.parse()?, config);

    let interface_state_service = fuchsia_component::client::connect_to_protocol::<StateMarker>()
        .context("failed to connect to fuchsia.net.interfaces/State")?;
    let internet_reachable = fidl_fuchsia_net_interfaces_ext::wait_for_reachability(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state_service)
            .context("failed to create network interface event stream")?,
    )
    .map(|r| r.context("reachability status stream error"));

    let server = PushServer::new(diagnostics, sampler, internet_reachable)?;
    let update_fut = server.serve(&mut fs)?;

    let (update_res, _) = join(update_fut, cobalt_sender_fut).await;
    update_res
}
