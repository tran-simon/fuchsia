// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    assert_matches::assert_matches,
    fidl_fuchsia_io as fio,
    fs_management::{
        filesystem::{Filesystem, ServingMultiVolumeFilesystem},
        FSConfig, Fxfs as FxfsConfig,
    },
    fuchsia_zircon::Status,
    ramdevice_client::{RamdiskClient, RamdiskClientBuilder},
    rand::{thread_rng, Rng},
    storage_manager::{
        fxfs::{Args as FxfsArgs, Fxfs},
        StorageManager,
    },
};

const RAMCTL_PATH: &str = "/dev/sys/platform/00:00:2d/ramctl";
const BLOCK_SIZE: u64 = 4096;
const BLOCK_COUNT: u64 = 1024; // 4MB RAM ought to be good enough
const ACCOUNT_LABEL: &str = "account";

fn ramdisk() -> RamdiskClient {
    ramdevice_client::wait_for_device(RAMCTL_PATH, std::time::Duration::from_secs(60))
        .expect("Could not wait for ramctl from isolated-devmgr");

    RamdiskClientBuilder::new(BLOCK_SIZE, BLOCK_COUNT).build().expect("Could not create ramdisk")
}

fn new_fs<FSC: FSConfig>(ramdisk: &RamdiskClient, config: FSC) -> Filesystem<FSC> {
    Filesystem::from_channel(ramdisk.open().unwrap().into_channel(), config).unwrap()
}

async fn make_ramdisk_and_filesystem(
) -> Result<(RamdiskClient, Filesystem<FxfsConfig>, ServingMultiVolumeFilesystem), Error> {
    let ramdisk = ramdisk();

    let mut fxfs: Filesystem<_> = new_fs(&ramdisk, FxfsConfig::default());

    fxfs.format().await.expect("failed to format fxfs");

    let fs: ServingMultiVolumeFilesystem =
        fxfs.serve_multi_volume().await.expect("failed to serve fxfs");

    Ok::<(RamdiskClient, Filesystem<FxfsConfig>, ServingMultiVolumeFilesystem), Error>((
        ramdisk, fxfs, fs,
    ))
}

fn new_key() -> [u8; 32] {
    let mut bits = [0_u8; 32];
    thread_rng().fill(&mut bits[..]);
    bits
}

fn new_storage_manager(fs: &mut ServingMultiVolumeFilesystem) -> Fxfs {
    Fxfs::new(
        FxfsArgs::builder()
            .volume_label(ACCOUNT_LABEL.to_string())
            .filesystem_dir(
                fuchsia_fs::clone_directory(fs.exposed_dir(), fio::OpenFlags::CLONE_SAME_RIGHTS)
                    .unwrap(),
            )
            .use_unique_crypt_name_for_test(true)
            .build(),
    )
}

async fn write_file(root_dir: &fio::DirectoryProxy) -> Result<(), Error> {
    let expected_contents = b"some data";

    let file = fuchsia_fs::directory::open_file(
        root_dir,
        "test",
        fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
    )
    .await?;

    let bytes_written = file.write(expected_contents).await?.map_err(Status::from_raw)?;

    assert_eq!(bytes_written, expected_contents.len() as u64);
    Ok(())
}

async fn read_file(root_dir: &fio::DirectoryProxy) -> Result<(), Error> {
    let expected_contents = b"some data";

    let file =
        fuchsia_fs::directory::open_file(root_dir, "test", fio::OpenFlags::RIGHT_READABLE).await?;
    let actual_contents = fuchsia_fs::file::read(&file).await?;
    assert_eq!(&actual_contents, expected_contents);
    Ok(())
}

#[fuchsia::test]
async fn test_lifecycle() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);

    let key = new_key();

    // "/volumes/account" doesn't exist yet.
    assert!(!fs.has_volume(ACCOUNT_LABEL).await.expect("has_volume failed"));

    // Provisioning the volume creates and mounts it. The volume is now unlocked.
    assert_matches!(fxfs_storage_manager.provision(&key).await, Ok(()));

    // "/volumes/account" has been created by the call to .provision().
    assert!(fs.has_volume(ACCOUNT_LABEL).await.expect("has_volume failed"));

    {
        let root_dir = fxfs_storage_manager.get_root_dir().await.unwrap();

        write_file(&root_dir).await?;

        // Drop |root_dir| when it falls out-of-scope here, which allows us to
        // close channels to the directory and eventually unmount the volume.

        // TODO(jbuckland): Once it is possible to unmount the volume without
        // closing channels, write tests which (a) lock before closing, and (b)
        // try and fail to read |file|.
    }

    // And then lock the volume.
    assert_matches!(fxfs_storage_manager.lock_storage().await, Ok(()));

    // We should be able to unlock it with the same key,
    assert_matches!(fxfs_storage_manager.unlock_storage(&key).await, Ok(()));

    // And view that same file.
    {
        let root_dir = fxfs_storage_manager.get_root_dir().await.unwrap();
        read_file(&root_dir).await?;
    }

    // Finally, destroy the storage manager,...
    assert_matches!(fxfs_storage_manager.destroy().await, Ok(()));

    // which means that we cannot access the root directory.
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();

    Ok(())
}

#[fuchsia::test]
async fn test_get_root_dir_before_provision() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);

    // Calling .get_root_dir() before .provision() fails, since the root
    // directory has not yet been set up.
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();
    Ok(())
}

#[fuchsia::test]
async fn test_get_root_dir_while_locked() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);
    let key = new_key();

    // Calling .get_root_dir() while locked fails, since the root
    // directory is inaccessible.
    assert_matches!(fxfs_storage_manager.provision(&key).await, Ok(()));
    assert_matches!(fxfs_storage_manager.lock_storage().await, Ok(()));
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();
    Ok(())
}

#[fuchsia::test]
async fn test_file_should_not_persist_across_destruction_new_sm() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let key = new_key();

    {
        let sm = new_storage_manager(&mut fs);

        // Make a new volume and write a file to it. Then destroy the volume.
        assert_matches!(sm.provision(&key).await, Ok(()));
        {
            let root_dir = sm.get_root_dir().await.unwrap();
            write_file(&root_dir).await?;
            read_file(&root_dir).await?;
        }
        assert_matches!(sm.destroy().await, Ok(()));
    }

    // Reading that same file back should fail -- just because this is a new
    // volume with the same account name does not mean that files should persist
    // across destruction.
    {
        // Make a new storagemanager on the same filesystem with the same key.
        let sm = new_storage_manager(&mut fs);
        assert_matches!(sm.provision(&key).await, Ok(()));

        {
            let root_dir = sm.get_root_dir().await.unwrap();
            assert_matches!(read_file(&root_dir).await, Err(_));
        }
        assert_matches!(sm.destroy().await, Ok(()));
    }

    let () = fs.shutdown().await.unwrap();

    Ok(())
}

#[fuchsia::test]
async fn test_file_should_not_persist_across_destruction_reuse_sm() -> Result<(), Error> {
    // This test is same as the one above, except that we reuse the same storage
    // manager instead of creating a new one. We want to demonstrate that the
    // file is destroyed no matter what.

    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let key = new_key();

    let sm = new_storage_manager(&mut fs);

    assert_matches!(sm.provision(&key).await, Ok(()));
    {
        let root_dir = sm.get_root_dir().await.unwrap();
        write_file(&root_dir).await?;
        read_file(&root_dir).await?;
    }
    assert_matches!(sm.destroy().await, Ok(()));

    assert_matches!(sm.provision(&key).await, Ok(()));
    {
        let root_dir = sm.get_root_dir().await.unwrap();
        assert_matches!(read_file(&root_dir).await, Err(_));
    }
    assert_matches!(sm.destroy().await, Ok(()));

    let () = fs.shutdown().await.unwrap();

    Ok(())
}
