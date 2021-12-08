// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fshost/block-device-manager.h"

#include <fcntl.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <lib/fdio/cpp/caller.h>
#include <sys/statfs.h>

#include <cobalt-client/cpp/collector.h>
#include <cobalt-client/cpp/in_memory_logger.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/storage/fshost/constants.h"
#include "src/storage/fshost/filesystem-mounter.h"
#include "src/storage/fshost/fs-manager.h"
#include "src/storage/fshost/fshost_integration_test.h"
#include "src/storage/fshost/mock-block-device.h"
#include "src/storage/testing/fvm.h"
#include "src/storage/testing/ram_disk.h"

namespace fshost {
namespace {

using ::testing::ContainerEq;

// For tests that want the full integration test suite.
using BlockDeviceManagerIntegration = FshostIntegrationTest;

TEST(BlockDeviceManager, BlobfsLimit) {
  Config::Options options = Config::DefaultOptions();
  options[Config::kBlobfsMaxBytes] = "7654321";
  Config config(options);
  BlockDeviceManager manager(&config);

  // When there's no FVM we expect no match and no max size call.
  MockBlobfsDevice blobfs_device;
  manager.AddDevice(blobfs_device);
  ASSERT_FALSE(blobfs_device.max_size());

  // Add FVM and re-try. This should call the limit set function.
  MockBlockDevice fvm_device(MockBlockDevice::FvmOptions());
  EXPECT_EQ(manager.AddDevice(fvm_device), ZX_OK);
  manager.AddDevice(blobfs_device);
  ASSERT_TRUE(blobfs_device.max_size());
  EXPECT_EQ(7654321u, *blobfs_device.max_size());

  // Make a blobfs that looks like it's in a ramdisk, the limit should not be set.
  MockBlockDevice::Options ramdisk_opts = MockBlobfsDevice::BlobfsOptions();
  ramdisk_opts.topological_path =
      "/dev/sys/platform/00:00:2d/ramctl" + ramdisk_opts.topological_path;
  MockBlockDevice ramdisk_blobfs(ramdisk_opts);
  manager.AddDevice(ramdisk_blobfs);
  ASSERT_FALSE(ramdisk_blobfs.max_size());
}

TEST(BlockDeviceManager, MinfsLimit) {
  Config::Options options = Config::DefaultOptions();
  options[Config::kMinfsMaxBytes] = "7654321";
  Config config(options);
  BlockDeviceManager manager(&config);

  MockBlockDevice fvm_device(MockBlockDevice::FvmOptions());
  EXPECT_EQ(manager.AddDevice(fvm_device), ZX_OK);

  MockBlockDevice::Options device_options = MockZxcryptDevice::ZxcryptOptions();
  device_options.content_format = fs_management::kDiskFormatUnknown;
  MockZxcryptDevice zxcrypt_device(device_options);
  EXPECT_EQ(manager.AddDevice(zxcrypt_device), ZX_OK);

  MockMinfsDevice minfs_device;
  EXPECT_EQ(manager.AddDevice(minfs_device), ZX_OK);
  ASSERT_TRUE(minfs_device.max_size());
  EXPECT_EQ(7654321u, *minfs_device.max_size());
}

TEST(BlockDeviceManager, ReadOptions) {
  std::stringstream stream;
  stream << "# A comment" << std::endl
         << Config::kDefault << std::endl
         << Config::kNoZxcrypt
         << std::endl
         // Duplicate keys should be de-duped.
         << Config::kNoZxcrypt << std::endl
         << Config::kMinfsMaxBytes << "=1"
         << std::endl
         // Duplicates should overwrite the value.
         << Config::kMinfsMaxBytes << "=12345"
         << std::endl
         // Empty value.
         << Config::kBlobfsMaxBytes << "=" << std::endl
         << "-" << Config::kBlobfs << std::endl
         << "-" << Config::kFormatMinfsOnCorruption;

  auto expected_options = Config::DefaultOptions();
  expected_options[Config::kNoZxcrypt] = std::string();
  expected_options[Config::kMinfsMaxBytes] = "12345";
  expected_options[Config::kBlobfsMaxBytes] = std::string();
  expected_options.erase(Config::kBlobfs);
  expected_options.erase(Config::kFormatMinfsOnCorruption);

  EXPECT_THAT(Config::ReadOptions(stream), ContainerEq(expected_options));
}

// The component for the fshost integration test sets the fshost config:
//   minfs_maximum_runtime_bytes = 32768
// which in turn sets the fshost variable kMinfsMaxBytes. This test is checking that this setting
// actually was sent to fshost and applies to FVM.
TEST_F(BlockDeviceManagerIntegration, MaxSize) {
  constexpr uint32_t kBlockCount = 9 * 1024 * 256;
  constexpr uint32_t kBlockSize = 512;
  constexpr uint32_t kSliceSize = 32'768;
  constexpr size_t kDeviceSize = kBlockCount * kBlockSize;

  PauseWatcher();  // Pause whilst we create a ramdisk.

  // Create a ramdisk with an unformatted minfs partitition.
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(kDeviceSize, 0, &vmo), ZX_OK);

  // Create a child VMO so that we can keep hold of the original.
  zx::vmo child_vmo;
  ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, kDeviceSize, &child_vmo), ZX_OK);

  // Now create the ram-disk with a single FVM partition.
  {
    auto ramdisk_or = storage::RamDisk::CreateWithVmo(std::move(child_vmo), kBlockSize);
    ASSERT_EQ(ramdisk_or.status_value(), ZX_OK);
    storage::FvmOptions options{
        .name = kDataPartitionLabel,
        .type = std::array<uint8_t, BLOCK_GUID_LEN>{GUID_DATA_VALUE},
    };
    auto fvm_partition_or = storage::CreateFvmPartition(ramdisk_or->path(), kSliceSize, options);
    ASSERT_EQ(fvm_partition_or.status_value(), ZX_OK);
  }

  ResumeWatcher();

  // Now reattach the ram-disk and fshost should format it.
  auto ramdisk_or = storage::RamDisk::CreateWithVmo(std::move(vmo), kBlockSize);
  ASSERT_EQ(ramdisk_or.status_value(), ZX_OK);
  auto [fd, fs_type] = WaitForMount("minfs");
  ASSERT_TRUE(fd);
  EXPECT_TRUE(fs_type == VFS_TYPE_MINFS || fs_type == VFS_TYPE_FXFS);

  // FVM will be at something like "/dev/sys/platform/00:00:2d/ramctl/ramdisk-1/block/fvm"
  std::string fvm_path = ramdisk_or.value().path() + "/fvm";
  fbl::unique_fd fvm_fd(open(fvm_path.c_str(), O_RDONLY));
  ASSERT_TRUE(fvm_fd);

  // The minfs partition will be the only one inside FVM.
  std::string partition_path = fvm_path + "/";
  partition_path.append(kDataPartitionLabel);
  partition_path.append("-p-1/block");
  fbl::unique_fd partition_fd(open(partition_path.c_str(), O_RDONLY));
  ASSERT_TRUE(partition_fd);

  // Query the minfs partition instance guid. This is needed to query the limit later on.
  fdio_cpp::UnownedFdioCaller partition_caller(partition_fd.get());
  namespace volume = fuchsia_hardware_block_volume;
  auto guid_result = fidl::WireCall<volume::Volume>(partition_caller.channel())->GetInstanceGuid();
  ASSERT_EQ(ZX_OK, guid_result.status());
  ASSERT_EQ(ZX_OK, guid_result->status);

  // Query the partition limit for the minfs partition.
  fdio_cpp::UnownedFdioCaller fvm_caller(fvm_fd.get());
  auto limit_result = fidl::WireCall<volume::VolumeManager>(fvm_caller.channel())
                          ->GetPartitionLimit(*guid_result->guid);
  ASSERT_EQ(ZX_OK, limit_result.status());
  ASSERT_EQ(ZX_OK, limit_result->status);

  // The partition limit should match the value set in the integration test fshost configuration
  // (see the BUILD.gn file).
  EXPECT_EQ(limit_result->byte_count, 117440512u);
}

TEST_F(BlockDeviceManagerIntegration, SetPartitionName) {
  constexpr uint32_t kBlockCount = 9 * 1024 * 256;
  constexpr uint32_t kBlockSize = 512;
  constexpr uint32_t kSliceSize = 32'768;
  constexpr size_t kDeviceSize = kBlockCount * kBlockSize;

  PauseWatcher();  // Pause whilst we create a ramdisk.

  // Create a ramdisk with an unformatted minfs partitition.
  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(kDeviceSize, 0, &vmo), ZX_OK);

  // Create a child VMO so that we can keep hold of the original.
  zx::vmo child_vmo;
  ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, kDeviceSize, &child_vmo), ZX_OK);

  // Now create the ram-disk with a single FVM partition.
  {
    auto ramdisk_or = storage::RamDisk::CreateWithVmo(std::move(child_vmo), kBlockSize);
    ASSERT_EQ(ramdisk_or.status_value(), ZX_OK);
    storage::FvmOptions options{
        .name = "minfs",  // Use a legacy name
        .type = std::array<uint8_t, BLOCK_GUID_LEN>{GUID_DATA_VALUE},
    };
    auto fvm_partition_or = storage::CreateFvmPartition(ramdisk_or->path(), kSliceSize, options);
    ASSERT_EQ(fvm_partition_or.status_value(), ZX_OK);
  }

  ResumeWatcher();

  // Now reattach the ram-disk and fshost should format it.
  auto ramdisk_or = storage::RamDisk::CreateWithVmo(std::move(vmo), kBlockSize);
  ASSERT_EQ(ramdisk_or.status_value(), ZX_OK);
  auto [fd, fs_type] = WaitForMount("minfs");
  ASSERT_TRUE(fd);
  EXPECT_TRUE(fs_type == VFS_TYPE_MINFS || fs_type == VFS_TYPE_FXFS);

  // FVM will be at something like "/dev/sys/platform/00:00:2d/ramctl/ramdisk-1/block/fvm"
  std::string fvm_path = ramdisk_or.value().path() + "/fvm";
  fbl::unique_fd fvm_fd(open(fvm_path.c_str(), O_RDONLY));
  ASSERT_TRUE(fvm_fd);

  // The minfs partition will be the only one inside FVM.
  std::string partition_path = fvm_path + "/minfs-p-1/block";
  fbl::unique_fd partition_fd(open(partition_path.c_str(), O_RDONLY));
  ASSERT_TRUE(partition_fd);

  // Query the partition name.
  fdio_cpp::UnownedFdioCaller partition_caller(partition_fd.get());
  auto result = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::Volume>(
                                   partition_caller.borrow_channel()))
                    ->GetName();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result->status, ZX_OK);

  // It should be the preferred name.
  ASSERT_EQ(result->name.get(), kDataPartitionLabel);
}

}  // namespace
}  // namespace fshost
