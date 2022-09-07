// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    anyhow::{Context, Error},
    assert_matches::assert_matches,
    blobfs_ramdisk::BlobfsRamdisk,
    fidl::endpoints::Proxy,
    fidl::endpoints::{ClientEnd, RequestStream, ServerEnd},
    fidl_fuchsia_io as fio,
    fidl_fuchsia_paver::{Asset, Configuration, PaverRequestStream},
    fidl_fuchsia_pkg_ext::{MirrorConfigBuilder, RepositoryConfigBuilder, RepositoryConfigs},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::{Capability, ChildOptions, RealmBuilder, Ref, Route},
    fuchsia_merkle::Hash,
    fuchsia_pkg_testing::{
        make_epoch_json, serve::ServedRepository, Package, PackageBuilder, RepositoryBuilder,
    },
    fuchsia_zircon as zx,
    futures::prelude::*,
    http::uri::Uri,
    isolated_ota::{
        download_and_apply_update_with_pre_configured_components, OmahaConfig, UpdateError,
    },
    isolated_swd::cache::Cache,
    mock_omaha_server::{
        OmahaResponse, OmahaServer, OmahaServerBuilder, ResponseAndMetadata, ResponseMap,
    },
    mock_paver::{hooks as mphooks, MockPaverService, MockPaverServiceBuilder, PaverEvent},
    parking_lot::Mutex,
    serde_json::json,
    std::{
        collections::{BTreeSet, HashMap},
        io::Write,
        str::FromStr,
        sync::Arc,
    },
    tempfile::TempDir,
    vfs::directory::entry::DirectoryEntry,
};

const EMPTY_REPO_PATH: &str = "/pkg/empty-repo";
const TEST_CERTS_PATH: &str = "/pkg/data/ssl";
const TEST_REPO_URL: &str = "fuchsia-pkg://integration.test.fuchsia.com";

enum OmahaState {
    /// Don't use Omaha for this update.
    Disabled,
    /// Set up an Omaha server automatically.
    Auto(OmahaResponse),
    /// Pass the given OmahaConfig to Omaha.
    Manual(OmahaConfig),
}

struct TestEnvBuilder {
    blobfs: Option<ClientEnd<fio::DirectoryMarker>>,
    board: String,
    channel: String,
    images: HashMap<String, Vec<u8>>,
    omaha: OmahaState,
    packages: Vec<Package>,
    paver: MockPaverServiceBuilder,
    repo_config: Option<RepositoryConfigs>,
    version: String,
}

impl TestEnvBuilder {
    pub fn new() -> Self {
        TestEnvBuilder {
            blobfs: None,
            board: "test-board".to_owned(),
            channel: "test".to_owned(),
            images: HashMap::new(),
            omaha: OmahaState::Disabled,
            packages: vec![],
            paver: MockPaverServiceBuilder::new(),
            repo_config: None,
            version: "0.1.2.3".to_owned(),
        }
    }

    /// Add an image to the update package that will be generated by this TestEnvBuilder
    pub fn add_image(mut self, name: &str, data: &[u8]) -> Self {
        self.images.insert(name.to_owned(), data.to_vec());
        self
    }

    /// Add a package to the repository generated by this TestEnvBuilder.
    /// The package will also be listed in the generated update package
    /// so that it will be downloaded as part of the OTA.
    pub fn add_package(mut self, pkg: Package) -> Self {
        self.packages.push(pkg);
        self
    }

    pub fn blobfs(mut self, client: ClientEnd<fio::DirectoryMarker>) -> Self {
        self.blobfs = Some(client);
        self
    }

    /// Provide a TUF repository configuration to the package resolver.
    /// This will override the repository that the builder would otherwise generate.
    pub fn repo_config(mut self, repo: RepositoryConfigs) -> Self {
        self.repo_config = Some(repo);
        self
    }

    /// Enable/disable Omaha. OmahaState::Auto will automatically set up an Omaha server and tell
    /// the updater to use it.
    pub fn omaha_state(mut self, state: OmahaState) -> Self {
        self.omaha = state;
        self
    }

    /// Mutate the MockPaverServiecBuilder used by this TestEnvBuilder.
    pub fn paver<F>(mut self, func: F) -> Self
    where
        F: FnOnce(MockPaverServiceBuilder) -> MockPaverServiceBuilder,
    {
        self.paver = func(self.paver);
        self
    }

    fn generate_packages(&self) -> String {
        json!({
            "version": "1",
            "content": self.packages
                .iter()
                .map(|p| format!("{}/{}/0?hash={}",
                                 TEST_REPO_URL,
                                 p.name(),
                                 p.meta_far_merkle_root()))
                .collect::<Vec<String>>(),
        })
        .to_string()
    }

    /// Turn this |TestEnvBuilder| into a |TestEnv|
    pub async fn build(mut self) -> Result<TestEnv, Error> {
        let mut update = PackageBuilder::new("update")
            .add_resource_at("packages.json", self.generate_packages().as_bytes());

        let (repo_config, served_repo, cert_dir, packages, merkle) = if self.repo_config.is_none() {
            // If no repo config was specified, host a repo containing the provided packages,
            // and an update package containing given images + all packages in the repo.
            for (name, data) in self.images.iter() {
                update = update.add_resource_at(name, data.as_slice());
            }

            let update = update.build().await.context("Building update package")?;
            let repo = Arc::new(
                self.packages
                    .iter()
                    .fold(
                        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH).add_package(&update),
                        |repo, package| repo.add_package(package),
                    )
                    .build()
                    .await
                    .context("Building repo")?,
            );

            let served_repo = Arc::clone(&repo).server().start().context("serving repo")?;
            let config = RepositoryConfigs::Version1(vec![
                served_repo.make_repo_config(TEST_REPO_URL.parse()?)
            ]);

            let update_merkle = update.meta_far_merkle_root().clone();
            // Add the update package to the list of packages, so that TestResult::check_packages
            // will expect to see the update package's blobs in blobfs.
            let mut packages = vec![update];
            packages.append(&mut self.packages);
            (
                config,
                Some(served_repo),
                std::fs::File::open(TEST_CERTS_PATH).context("opening test certificates")?,
                packages,
                update_merkle,
            )
        } else {
            // Use the provided repo config. Assume that this means we'll actually want to use
            // real SSL certificates, and that we don't need to host our own repository.
            (
                self.repo_config.unwrap(),
                None,
                std::fs::File::open("/config/ssl").context("opening system ssl certificates")?,
                vec![],
                Hash::from_str("deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
                    .context("Making merkle")?,
            )
        };

        let dir = tempfile::tempdir()?;
        let mut path = dir.path().to_owned();
        path.push("repo_config.json");
        let path = path.as_path();
        let mut file =
            std::io::BufWriter::new(std::fs::File::create(path).context("creating file")?);
        serde_json::to_writer(&mut file, &repo_config).unwrap();
        file.flush().unwrap();

        Ok(TestEnv {
            blobfs: self.blobfs,
            board: self.board,
            channel: self.channel,
            omaha: self.omaha,
            packages,
            paver: Arc::new(self.paver.build()),
            _repo: served_repo,
            repo_config_dir: dir,
            ssl_certs: cert_dir,
            update_merkle: merkle,
            version: self.version,
        })
    }
}

struct TestEnv {
    blobfs: Option<ClientEnd<fio::DirectoryMarker>>,
    board: String,
    channel: String,
    omaha: OmahaState,
    packages: Vec<Package>,
    paver: Arc<MockPaverService>,
    _repo: Option<ServedRepository>,
    repo_config_dir: TempDir,
    ssl_certs: std::fs::File,
    update_merkle: Hash,
    version: String,
}

impl TestEnv {
    fn start_omaha(omaha: OmahaState, merkle: Hash) -> Result<Option<OmahaConfig>, Error> {
        match omaha {
            OmahaState::Disabled => Ok(None),
            OmahaState::Manual(cfg) => Ok(Some(cfg)),
            OmahaState::Auto(response) => {
                let server = OmahaServerBuilder::default()
                    .responses_by_appid(
                        vec![(
                            "integration-test-appid".to_string(),
                            ResponseAndMetadata { response, merkle, ..Default::default() },
                        )]
                        .into_iter()
                        .collect::<ResponseMap>(),
                    )
                    .build()
                    .unwrap();
                let addr = OmahaServer::start(Arc::new(Mutex::new(server)))
                    .context("Starting omaha server")?;
                let config =
                    OmahaConfig { app_id: "integration-test-appid".to_owned(), server_url: addr };

                Ok(Some(config))
            }
        }
    }

    /// Run the update, consuming this |TestEnv| and returning a |TestResult|.
    pub async fn run(mut self) -> TestResult {
        let (blobfs, blobfs_handle) = if let Some(client) = self.blobfs.take() {
            (None, client)
        } else {
            let blobfs = BlobfsRamdisk::start().expect("launching blobfs");
            let client = blobfs.root_dir_handle().expect("getting blobfs root handle");
            (Some(blobfs), client)
        };

        let omaha_config =
            TestEnv::start_omaha(self.omaha, self.update_merkle).expect("Starting Omaha server");

        let mut service_fs = ServiceFs::new();
        let paver_clone = Arc::clone(&self.paver);
        service_fs.add_fidl_service(move |stream: PaverRequestStream| {
            fasync::Task::spawn(
                Arc::clone(&paver_clone)
                    .run_paver_service(stream)
                    .unwrap_or_else(|e| panic!("Failed to run mock paver: {:?}", e)),
            )
            .detach();
        });
        let (client, server) =
            fidl::endpoints::create_endpoints().expect("creating channel for servicefs");
        service_fs.serve_connection(server).expect("Failed to serve connection");
        fasync::Task::spawn(service_fs.collect()).detach();

        let realm_builder = RealmBuilder::new().await.unwrap();

        let pkg_component =
            realm_builder.add_child("pkg", "#meta/pkg.cm", ChildOptions::new()).await.unwrap();
        realm_builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.boot.Arguments"))
                    .capability(Capability::protocol_by_name("fuchsia.cobalt.LoggerFactory"))
                    .capability(Capability::protocol_by_name("fuchsia.tracing.provider.Registry"))
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .from(Ref::parent())
                    .to(&pkg_component),
            )
            .await
            .unwrap();

        realm_builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.pkg.PackageCache"))
                    .capability(Capability::protocol_by_name("fuchsia.pkg.RetainedPackages"))
                    .capability(Capability::protocol_by_name("fuchsia.space.Manager"))
                    .from(&pkg_component)
                    .to(Ref::parent()),
            )
            .await
            .unwrap();

        let blobfs_proxy = fio::DirectoryProxy::from_channel(
            fasync::Channel::from_channel(blobfs_handle.into_channel()).unwrap(),
        );

        let (blobfs_client_end_clone, remote) =
            fidl::endpoints::create_endpoints::<fio::DirectoryMarker>().unwrap();
        blobfs_proxy
            .clone(fio::OpenFlags::CLONE_SAME_RIGHTS, ServerEnd::from(remote.into_channel()))
            .unwrap();

        let blobfs_proxy_clone = blobfs_client_end_clone.into_proxy().unwrap();
        let blobfs_vfs = vfs::remote::remote_dir(blobfs_proxy_clone);
        let blobfs_reflector = realm_builder
            .add_local_child(
                "pkg_cache_blobfs",
                move |handles| {
                    let blobfs_vfs = blobfs_vfs.clone();
                    let out_dir = vfs::pseudo_directory! {
                        "blob" => blobfs_vfs,
                    };
                    let scope = vfs::execution_scope::ExecutionScope::new();
                    let () = out_dir.open(
                        scope.clone(),
                        fio::OpenFlags::RIGHT_READABLE
                            | fio::OpenFlags::RIGHT_WRITABLE
                            | fio::OpenFlags::RIGHT_EXECUTABLE,
                        0,
                        vfs::path::Path::dot(),
                        handles.outgoing_dir.into_channel().into(),
                    );
                    async move { Ok(scope.wait().await) }.boxed()
                },
                ChildOptions::new(),
            )
            .await
            .unwrap();

        realm_builder
            .add_route(
                Route::new()
                    .capability(
                        Capability::directory("blob-exec")
                            .path("/blob")
                            .rights(fio::RW_STAR_DIR | fio::Operations::EXECUTE),
                    )
                    .from(&blobfs_reflector)
                    .to(&pkg_component),
            )
            .await
            .unwrap();

        let realm_instance = realm_builder.build().await.unwrap();
        let pkg_cache_proxy = realm_instance
            .root
            .connect_to_protocol_at_exposed_dir::<fidl_fuchsia_pkg::PackageCacheMarker>()
            .expect("connect to pkg cache");
        let space_manager_proxy = realm_instance
            .root
            .connect_to_protocol_at_exposed_dir::<fidl_fuchsia_space::ManagerMarker>()
            .expect("connect to space manager");

        let (cache_clone, remote) =
            fidl::endpoints::create_endpoints::<fidl_fuchsia_io::DirectoryMarker>().unwrap();
        realm_instance
            .root
            .get_exposed_dir()
            .clone(
                fidl_fuchsia_io::OpenFlags::CLONE_SAME_RIGHTS,
                ServerEnd::from(remote.into_channel()),
            )
            .unwrap();

        let cache = Cache::new_with_proxies(
            pkg_cache_proxy,
            space_manager_proxy,
            cache_clone.into_proxy().unwrap(),
        )
        .unwrap();

        let result = download_and_apply_update_with_pre_configured_components(
            blobfs_proxy,
            ClientEnd::from(client),
            std::fs::File::open(self.repo_config_dir.into_path()).expect("Opening repo config dir"),
            self.ssl_certs,
            &self.channel,
            &self.board,
            &self.version,
            omaha_config,
            Arc::new(cache),
        )
        .await;

        TestResult {
            blobfs,
            packages: self.packages,
            paver_events: self.paver.take_events(),
            result,
        }
    }
}

struct TestResult {
    blobfs: Option<BlobfsRamdisk>,
    packages: Vec<Package>,
    pub paver_events: Vec<PaverEvent>,
    pub result: Result<(), UpdateError>,
}

impl TestResult {
    /// Assert that all blobs in all the packages that were part of the Update
    /// have been installed into the blobfs, and that the blobfs contains no extra blobs.
    pub fn check_packages(&self) {
        let written_blobs = self
            .blobfs
            .as_ref()
            .unwrap_or_else(|| panic!("Test had no blobfs"))
            .list_blobs()
            .expect("Listing blobfs blobs");
        let mut all_package_blobs = BTreeSet::new();
        for package in self.packages.iter() {
            all_package_blobs.append(&mut package.list_blobs().expect("Listing package blobs"));
        }

        assert_eq!(written_blobs, all_package_blobs);
    }
}

async fn build_test_package() -> Result<Package, Error> {
    PackageBuilder::new("test-package")
        .add_resource_at("data/test", "hello, world!".as_bytes())
        .build()
        .await
        .context("Building test package")
}

#[fasync::run_singlethreaded(test)]
pub async fn test_no_network() -> Result<(), Error> {
    // Test what happens when we can't reach the remote repo.
    let bad_mirror =
        MirrorConfigBuilder::new("http://does-not-exist.fuchsia.com".parse::<Uri>().unwrap())?
            .build();
    let invalid_repo = RepositoryConfigs::Version1(vec![RepositoryConfigBuilder::new(
        fuchsia_url::RepositoryUrl::parse_host("fuchsia.com".to_owned()).unwrap(),
    )
    .add_mirror(bad_mirror)
    .build()]);

    let env = TestEnvBuilder::new()
        .repo_config(invalid_repo)
        .build()
        .await
        .context("Building TestEnv")?;

    let update_result = env.run().await;
    assert_eq!(
        update_result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
        ]
    );
    update_result.check_packages();

    let err = update_result.result.unwrap_err();
    assert_matches!(err, UpdateError::InstallError(_));
    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_pave_fails() -> Result<(), Error> {
    // Test what happens if the paver fails while paving.
    let test_package = build_test_package().await?;
    let paver_hook = |p: &PaverEvent| {
        if let PaverEvent::WriteAsset { payload, .. } = p {
            if payload.as_slice() == "FAIL".as_bytes() {
                return zx::Status::IO;
            }
        }
        zx::Status::OK
    };

    let env = TestEnvBuilder::new()
        .paver(|p| p.insert_hook(mphooks::return_error(paver_hook)))
        .add_package(test_package)
        .add_image("zbi.signed", "FAIL".as_bytes())
        .add_image("epoch.json", make_epoch_json(1).as_bytes())
        .add_image("fuchsia.vbmeta", "FAIL".as_bytes())
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteAsset {
                asset: Asset::Kernel,
                configuration: Configuration::B,
                payload: "FAIL".as_bytes().to_vec(),
            },
        ]
    );
    println!("Paver events: {:?}", result.paver_events);
    assert_matches!(result.result.unwrap_err(), UpdateError::InstallError(_));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_updater_succeeds() -> Result<(), Error> {
    let mut builder = TestEnvBuilder::new()
        .add_image("zbi.signed", "This is a zbi".as_bytes())
        .add_image("fuchsia.vbmeta", "This is a vbmeta".as_bytes())
        .add_image("recovery", "This is recovery".as_bytes())
        .add_image("recovery.vbmeta", "This is another vbmeta".as_bytes())
        .add_image("bootloader", "This is a bootloader upgrade".as_bytes())
        .add_image("epoch.json", make_epoch_json(1).as_bytes())
        .add_image("firmware_test", "This is the test firmware".as_bytes());
    for i in 0i64..3 {
        let name = format!("test-package{}", i);
        let package = PackageBuilder::new(name)
            .add_resource_at(
                format!("data/my-package-data-{}", i),
                format!("This is some test data for test package {}", i).as_bytes(),
            )
            .add_resource_at("bin/binary", "#!/boot/bin/sh\necho Hello".as_bytes())
            .build()
            .await
            .context("Building test package")?;
        builder = builder.add_package(package);
    }

    let env = builder.build().await.context("Building TestEnv")?;
    let result = env.run().await;

    result.check_packages();
    assert!(result.result.is_ok());
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "".to_owned(),
                payload: "This is a bootloader upgrade".as_bytes().to_vec(),
            },
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "test".to_owned(),
                payload: "This is the test firmware".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::Kernel,
                payload: "This is a zbi".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::VerifiedBootMetadata,
                payload: "This is a vbmeta".as_bytes().to_vec(),
            },
            PaverEvent::DataSinkFlush,
            // Note that recovery isn't written, as isolated-ota skips them.
            PaverEvent::SetConfigurationActive { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            // This is the isolated-ota library checking to see if the paver configured ABR properly.
            PaverEvent::QueryActiveConfiguration,
        ]
    );
    Ok(())
}

fn launch_cloned_blobfs(
    end: ServerEnd<fio::NodeMarker>,
    flags: fio::OpenFlags,
    parent_flags: fio::OpenFlags,
) {
    let flags =
        if flags.contains(fio::OpenFlags::CLONE_SAME_RIGHTS) { parent_flags } else { flags };
    let chan = fidl::AsyncChannel::from_channel(end.into_channel()).expect("cloning blobfs dir");
    let stream = fio::DirectoryRequestStream::from_channel(chan);
    fasync::Task::spawn(async move {
        serve_failing_blobfs(stream, flags)
            .await
            .unwrap_or_else(|e| panic!("Failed to serve cloned blobfs handle: {:?}", e));
    })
    .detach();
}

async fn serve_failing_blobfs(
    mut stream: fio::DirectoryRequestStream,
    open_flags: fio::OpenFlags,
) -> Result<(), Error> {
    if open_flags.contains(fio::OpenFlags::DESCRIBE) {
        stream
            .control_handle()
            .send_on_open_(
                zx::Status::OK.into_raw(),
                Some(&mut fio::NodeInfo::Directory(fio::DirectoryObject)),
            )
            .context("sending on open")?;
    }
    while let Some(req) = stream.try_next().await? {
        match req {
            fio::DirectoryRequest::Clone { flags, object, control_handle: _ } => {
                launch_cloned_blobfs(object, flags, open_flags)
            }
            fio::DirectoryRequest::Reopen { rights_request, object_request, control_handle: _ } => {
                let _ = object_request;
                todo!("https://fxbug.dev/77623: rights_request={:?}", rights_request);
            }
            fio::DirectoryRequest::Close { responder } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing close")?
            }
            fio::DirectoryRequest::Describe { responder } => responder
                .send(&mut fio::NodeInfo::Directory(fio::DirectoryObject))
                .context("describing")?,
            fio::DirectoryRequest::GetConnectionInfo { responder } => {
                let _ = responder;
                todo!("https://fxbug.dev/77623");
            }
            fio::DirectoryRequest::Sync { responder } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing sync")?
            }
            fio::DirectoryRequest::AdvisoryLock { request: _, responder } => {
                responder.send(&mut Err(zx::sys::ZX_ERR_NOT_SUPPORTED))?
            }
            fio::DirectoryRequest::GetAttr { responder } => responder
                .send(
                    zx::Status::IO.into_raw(),
                    &mut fio::NodeAttributes {
                        mode: 0,
                        id: 0,
                        content_size: 0,
                        storage_size: 0,
                        link_count: 0,
                        creation_time: 0,
                        modification_time: 0,
                    },
                )
                .context("failing getattr")?,
            fio::DirectoryRequest::SetAttr { flags: _, attributes: _, responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing setattr")?
            }
            fio::DirectoryRequest::GetAttributes { query, responder } => {
                let _ = responder;
                todo!("https://fxbug.dev/77623: query={:?}", query);
            }
            fio::DirectoryRequest::UpdateAttributes { payload, responder } => {
                let _ = responder;
                todo!("https://fxbug.dev/77623: payload={:?}", payload);
            }
            fio::DirectoryRequest::GetFlags { responder } => responder
                .send(zx::Status::IO.into_raw(), fio::OpenFlags::empty())
                .context("failing getflags")?,
            fio::DirectoryRequest::SetFlags { flags: _, responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing setflags")?
            }
            fio::DirectoryRequest::Open { flags, mode: _, path, object, control_handle: _ } => {
                if &path == "." {
                    launch_cloned_blobfs(object, flags, open_flags);
                } else {
                    object.close_with_epitaph(zx::Status::IO).context("failing open")?;
                }
            }
            fio::DirectoryRequest::Open2 { path, protocols, object_request, control_handle: _ } => {
                let _ = object_request;
                todo!("https://fxbug.dev/77623: path={} protocols={:?}", path, protocols);
            }
            fio::DirectoryRequest::AddInotifyFilter {
                path,
                filter,
                watch_descriptor,
                socket: _,
                responder: _,
            } => {
                todo!(
                    "https://fxbug.dev/77623: path={} filter={:?} watch_descriptor={}",
                    path,
                    filter,
                    watch_descriptor
                );
            }
            fio::DirectoryRequest::Unlink { name: _, options: _, responder } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing unlink")?
            }
            fio::DirectoryRequest::ReadDirents { max_bytes: _, responder } => {
                responder.send(zx::Status::IO.into_raw(), &[]).context("failing readdirents")?
            }
            fio::DirectoryRequest::Enumerate { options, iterator, control_handle: _ } => {
                let _ = iterator;
                todo!("https://fxbug.dev/77623: options={:?}", options);
            }
            fio::DirectoryRequest::Rewind { responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing rewind")?
            }
            fio::DirectoryRequest::GetToken { responder } => {
                responder.send(zx::Status::IO.into_raw(), None).context("failing gettoken")?
            }
            fio::DirectoryRequest::Rename { src: _, dst_parent_token: _, dst: _, responder } => {
                responder.send(&mut Err(zx::Status::IO.into_raw())).context("failing rename")?
            }
            fio::DirectoryRequest::Link { src: _, dst_parent_token: _, dst: _, responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing link")?
            }
            fio::DirectoryRequest::Watch { mask: _, options: _, watcher: _, responder } => {
                responder.send(zx::Status::IO.into_raw()).context("failing watch")?
            }
            fio::DirectoryRequest::Query { responder } => {
                let _ = responder;
                todo!("https://fxbug.dev/77623");
            }
            fio::DirectoryRequest::QueryFilesystem { responder } => responder
                .send(zx::Status::IO.into_raw(), None)
                .context("failing queryfilesystem")?,
        };
    }

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_blobfs_broken() -> Result<(), Error> {
    let (client, server) = zx::Channel::create().context("creating blobfs channel")?;
    let package = build_test_package().await?;
    let paver_hook = |_: &PaverEvent| zx::Status::IO;
    let env = TestEnvBuilder::new()
        .add_package(package)
        .add_image("zbi.signed", "ZBI".as_bytes())
        .blobfs(ClientEnd::from(client))
        .paver(|p| p.insert_hook(mphooks::return_error(paver_hook)))
        .build()
        .await
        .context("Building TestEnv")?;

    let stream =
        fio::DirectoryRequestStream::from_channel(fidl::AsyncChannel::from_channel(server)?);

    fasync::Task::spawn(async move {
        serve_failing_blobfs(stream, fio::OpenFlags::empty())
            .await
            .unwrap_or_else(|e| panic!("Failed to serve blobfs: {:?}", e));
    })
    .detach();

    let result = env.run().await;

    assert_matches!(result.result, Err(UpdateError::InstallError(_)));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_omaha_broken() -> Result<(), Error> {
    let bad_omaha_config = OmahaConfig {
        app_id: "broken-omaha-test".to_owned(),
        server_url: "http://does-not-exist.fuchsia.com".to_owned(),
    };
    let package = build_test_package().await?;
    let env = TestEnvBuilder::new()
        .add_package(package)
        .add_image("zbi.signed", "ZBI".as_bytes())
        .omaha_state(OmahaState::Manual(bad_omaha_config))
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    assert_matches!(result.result, Err(UpdateError::InstallError(_)));

    Ok(())
}

#[fasync::run_singlethreaded(test)]
pub async fn test_omaha_works() -> Result<(), Error> {
    let mut builder = TestEnvBuilder::new()
        .add_image("zbi.signed", "This is a zbi".as_bytes())
        .add_image("fuchsia.vbmeta", "This is a vbmeta".as_bytes())
        .add_image("recovery", "This is recovery".as_bytes())
        .add_image("recovery.vbmeta", "This is another vbmeta".as_bytes())
        .add_image("bootloader", "This is a bootloader upgrade".as_bytes())
        .add_image("epoch.json", make_epoch_json(1).as_bytes())
        .add_image("firmware_test", "This is the test firmware".as_bytes());
    for i in 0i64..3 {
        let name = format!("test-package{}", i);
        let package = PackageBuilder::new(name)
            .add_resource_at(
                format!("data/my-package-data-{}", i),
                format!("This is some test data for test package {}", i).as_bytes(),
            )
            .add_resource_at("bin/binary", "#!/boot/bin/sh\necho Hello".as_bytes())
            .build()
            .await
            .context("Building test package")?;
        builder = builder.add_package(package);
    }

    let env = builder
        .omaha_state(OmahaState::Auto(OmahaResponse::Update))
        .build()
        .await
        .context("Building TestEnv")?;

    let result = env.run().await;
    result.check_packages();
    assert!(result.result.is_ok());
    assert_eq!(
        result.paver_events,
        vec![
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::ReadAsset {
                configuration: Configuration::A,
                asset: Asset::VerifiedBootMetadata
            },
            PaverEvent::ReadAsset { configuration: Configuration::A, asset: Asset::Kernel },
            PaverEvent::QueryCurrentConfiguration,
            PaverEvent::QueryConfigurationStatus { configuration: Configuration::A },
            PaverEvent::SetConfigurationUnbootable { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "".to_owned(),
                payload: "This is a bootloader upgrade".as_bytes().to_vec(),
            },
            PaverEvent::WriteFirmware {
                configuration: Configuration::B,
                firmware_type: "test".to_owned(),
                payload: "This is the test firmware".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::Kernel,
                payload: "This is a zbi".as_bytes().to_vec(),
            },
            PaverEvent::WriteAsset {
                configuration: Configuration::B,
                asset: Asset::VerifiedBootMetadata,
                payload: "This is a vbmeta".as_bytes().to_vec(),
            },
            PaverEvent::DataSinkFlush,
            // Note that recovery isn't written, as isolated-ota skips them.
            PaverEvent::SetConfigurationActive { configuration: Configuration::B },
            PaverEvent::BootManagerFlush,
            // This is the isolated-ota library checking to see if the paver configured ABR properly.
            PaverEvent::QueryActiveConfiguration,
        ]
    );

    Ok(())
}
