// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod constants;

use {
    anyhow::{ensure, Context as _, Error},
    fidl_fuchsia_hardware_block::BlockProxy,
    fuchsia_zircon::{self as zx, HandleBased as _},
};

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum DiskFormat {
    Unknown,
    Gpt,
    Mbr,
    Minfs,
    Fat,
    Blobfs,
    Fvm,
    Zxcrypt,
    FactoryFs,
    BlockVerity,
    VbMeta,
    BootPart,
    Fxfs,
    F2fs,
    NandBroker,
}

pub fn round_up(val: u64, divisor: u64) -> u64 {
    ((val + (divisor - 1)) / divisor) * divisor
}

pub async fn detect_disk_format(block_proxy: &BlockProxy) -> DiskFormat {
    match detect_disk_format_res(&block_proxy).await {
        Ok(format) => format,
        Err(e) => {
            tracing::error!("detect_disk_format failed: {}", e);
            return DiskFormat::Unknown;
        }
    }
}

async fn detect_disk_format_res(block_proxy: &BlockProxy) -> Result<DiskFormat, Error> {
    let block_info = block_proxy
        .get_info()
        .await
        .context("transport error on get_info call")?
        .map_err(zx::Status::from_raw)
        .context("get_info call failed")?;
    ensure!(block_info.block_size > 0, "block size expected to be non-zero");

    let header_size = if constants::HEADER_SIZE > 2 * block_info.block_size {
        constants::HEADER_SIZE
    } else {
        2 * block_info.block_size
    };

    ensure!(
        header_size as u64 <= block_info.block_size as u64 * block_info.block_count,
        "header size is larger than the size of the block device"
    );

    let buffer_size = round_up(header_size as u64, block_info.block_size as u64);
    let vmo = zx::Vmo::create(buffer_size).context("failed to create vmo")?;
    let () = block_proxy
        .read_blocks(
            vmo.duplicate_handle(zx::Rights::SAME_RIGHTS)
                .context("vmo duplicate handle call failed")?,
            buffer_size,
            0,
            0,
        )
        .await
        .context("transport error on read_blocks protocol")?
        .map_err(zx::Status::from_raw)
        .context("read_blocks returned an error status")?;

    let mut data = vec![0; buffer_size as usize];
    vmo.read(&mut data, 0).context("vmo read failed")?;

    if data.starts_with(&constants::FVM_MAGIC) {
        return Ok(DiskFormat::Fvm);
    }

    if data.starts_with(&constants::ZXCRYPT_MAGIC) {
        return Ok(DiskFormat::Zxcrypt);
    }

    if data.starts_with(&constants::BLOCK_VERITY_MAGIC) {
        return Ok(DiskFormat::BlockVerity);
    }

    if &data[block_info.block_size as usize..block_info.block_size as usize + 16]
        == &constants::GPT_MAGIC
    {
        return Ok(DiskFormat::Gpt);
    }

    if data.starts_with(&constants::MINFS_MAGIC) {
        return Ok(DiskFormat::Minfs);
    }

    if data.starts_with(&constants::BLOBFS_MAGIC) {
        return Ok(DiskFormat::Blobfs);
    }

    if data.starts_with(&constants::FACTORYFS_MAGIC) {
        return Ok(DiskFormat::FactoryFs);
    }

    if data.starts_with(&constants::VB_META_MAGIC) {
        return Ok(DiskFormat::VbMeta);
    }

    if data[510] == 0x55 && data[511] == 0xAA {
        if data[38] == 0x29 || data[66] == 0x29 {
            // 0x55AA are always placed at offset 510 and 511 for FAT filesystems.
            // 0x29 is the Boot Signature, but it is placed at either offset 38 or
            // 66 (depending on FAT type).
            return Ok(DiskFormat::Fat);
        }
        return Ok(DiskFormat::Mbr);
    }

    if &data[1024..1024 + constants::F2FS_MAGIC.len()] == &constants::F2FS_MAGIC {
        return Ok(DiskFormat::F2fs);
    }

    if data.starts_with(&constants::FXFS_MAGIC) {
        return Ok(DiskFormat::Fxfs);
    }

    return Ok(DiskFormat::Unknown);
}

#[cfg(test)]
mod tests {
    use {
        super::{constants, detect_disk_format, DiskFormat},
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_hardware_block::{BlockInfo, BlockMarker, BlockRequest, Flag},
        futures::{pin_mut, select, FutureExt, TryStreamExt},
    };

    async fn get_detected_disk_format(
        content: &[u8],
        block_count: u64,
        block_size: u64,
    ) -> DiskFormat {
        let (proxy, mut stream) = create_proxy_and_stream::<BlockMarker>().unwrap();

        let mock_device = async {
            while let Some(request) = stream.try_next().await.unwrap() {
                match request {
                    BlockRequest::GetInfo { responder } => {
                        responder
                            .send(&mut Ok(BlockInfo {
                                block_count: block_count,
                                block_size: block_size as u32,
                                max_transfer_size: 1024 * 1024,
                                flags: Flag::empty(),
                            }))
                            .unwrap();
                    }
                    BlockRequest::ReadBlocks { vmo, length, dev_offset, vmo_offset, responder } => {
                        assert_eq!(dev_offset, 0);
                        assert_eq!(length, content.len() as u64);
                        vmo.write(content, vmo_offset).unwrap();
                        responder.send(&mut Ok(())).unwrap();
                    }
                    _ => unreachable!(),
                }
            }
        }
        .fuse();

        pin_mut!(mock_device);

        select! {
          _ = mock_device => unreachable!(),
          format = detect_disk_format(&proxy).fuse() => format,
        }
    }

    #[fuchsia::test]
    async fn detect_format_fvm() {
        let mut data = vec![0; 4096];
        data[..constants::FVM_MAGIC.len()].copy_from_slice(&constants::FVM_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Fvm);
    }

    #[fuchsia::test]
    async fn detect_format_zxcrypt() {
        let mut data = vec![0; 4096];
        data[0..constants::ZXCRYPT_MAGIC.len()].copy_from_slice(&constants::ZXCRYPT_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Zxcrypt);
    }

    #[fuchsia::test]
    async fn detect_format_block_verity() {
        let mut data = vec![0; 4096];
        data[0..constants::BLOCK_VERITY_MAGIC.len()]
            .copy_from_slice(&constants::BLOCK_VERITY_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::BlockVerity);
    }

    #[fuchsia::test]
    async fn detect_format_gpt() {
        let mut data = vec![0; 4096];
        data[512..512 + 16].copy_from_slice(&constants::GPT_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Gpt);
    }

    #[fuchsia::test]
    async fn detect_format_minfs() {
        let mut data = vec![0; 4096];
        data[0..constants::MINFS_MAGIC.len()].copy_from_slice(&constants::MINFS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Minfs);
    }

    #[fuchsia::test]
    async fn detect_format_minfs_large_block_device() {
        let mut data = vec![0; 32768];
        data[0..constants::MINFS_MAGIC.len()].copy_from_slice(&constants::MINFS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1250000, 16384).await, DiskFormat::Minfs);
    }

    #[fuchsia::test]
    async fn detect_format_blobfs() {
        let mut data = vec![0; 4096];
        data[0..constants::BLOBFS_MAGIC.len()].copy_from_slice(&constants::BLOBFS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Blobfs);
    }

    #[fuchsia::test]
    async fn detect_format_factory_fs() {
        let mut data = vec![0; 4096];
        data[0..constants::FACTORYFS_MAGIC.len()].copy_from_slice(&constants::FACTORYFS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::FactoryFs);
    }

    #[fuchsia::test]
    async fn detect_format_vb_meta() {
        let mut data = vec![0; 4096];
        data[0..constants::VB_META_MAGIC.len()].copy_from_slice(&constants::VB_META_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::VbMeta);
    }

    #[fuchsia::test]
    async fn detect_format_mbr() {
        let mut data = vec![0; 4096];
        data[510] = 0x55 as u8;
        data[511] = 0xAA as u8;
        data[38] = 0x30 as u8;
        data[66] = 0x30 as u8;
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Mbr);
    }

    #[fuchsia::test]
    async fn detect_format_fat_1() {
        let mut data = vec![0; 4096];
        data[510] = 0x55 as u8;
        data[511] = 0xAA as u8;
        data[38] = 0x29 as u8;
        data[66] = 0x30 as u8;
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Fat);
    }

    #[fuchsia::test]
    async fn detect_format_fat_2() {
        let mut data = vec![0; 4096];
        data[510] = 0x55 as u8;
        data[511] = 0xAA as u8;
        data[38] = 0x30 as u8;
        data[66] = 0x29 as u8;
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Fat);
    }

    #[fuchsia::test]
    async fn detect_format_f2fs() {
        let mut data = vec![0; 4096];
        data[1024..1024 + constants::F2FS_MAGIC.len()].copy_from_slice(&constants::F2FS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::F2fs);
    }

    #[fuchsia::test]
    async fn detect_format_fxfs() {
        let mut data = vec![0; 4096];
        data[0..constants::FXFS_MAGIC.len()].copy_from_slice(&constants::FXFS_MAGIC);
        assert_eq!(get_detected_disk_format(&data, 1000, 512).await, DiskFormat::Fxfs);
    }

    #[fuchsia::test]
    async fn detect_format_unknown() {
        assert_eq!(get_detected_disk_format(&vec![0; 4096], 1000, 512).await, DiskFormat::Unknown);
    }
}
