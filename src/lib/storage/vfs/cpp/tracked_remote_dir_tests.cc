// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <utility>

#include <zxtest/zxtest.h>

#include "src/lib/storage/vfs/cpp/tracked_remote_dir.h"

namespace {

// A Remote Directory which shuts down a dispatch loop when it is destroyed. This may be utilized to
// synchronize destruction of the remote directory with a test's dispatch loop.
class TestRemoteDir final : public fs::TrackedRemoteDir {
 public:
  TestRemoteDir(fidl::ClientEnd<fuchsia_io::Directory> remote, async::Loop* loop)
      : TrackedRemoteDir(std::move(remote)), loop_(loop) {}
  ~TestRemoteDir() { loop_->Shutdown(); }

 private:
  async::Loop* loop_;
};

TEST(TrackedRemoteDir, AddingTrackedDirectory) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  zx::result root = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(root.status_value());

  fbl::String name = "remote-directory";
  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();

  // Get attributes.
  fs::VnodeAttributes attr;
  EXPECT_EQ(ZX_OK, dir->GetAttributes(&attr));
  EXPECT_EQ(V_TYPE_DIR | V_IRUSR, attr.mode);
  EXPECT_EQ(1, attr.link_count);

  // "name" should not yet exist within the directory.
  fbl::RefPtr<fs::Vnode> node;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, dir->Lookup(name, &node));

  // Add a remote directory, observe that it can be looked up.
  auto remote = fbl::MakeRefCounted<TestRemoteDir>(std::move(root->client), &loop);
  EXPECT_EQ(ZX_OK, remote->AddAsTrackedEntry(loop.dispatcher(), dir.get(), name));
  remote.reset();
  EXPECT_EQ(ZX_OK, dir->Lookup(name, &node));
  node.reset();

  // Forcing the remote connection to become "peer closed" causes the entry to be removed.
  root->server.reset();
  EXPECT_EQ(ZX_ERR_BAD_STATE, loop.Run());
  EXPECT_EQ(ZX_ERR_NOT_FOUND, dir->Lookup(name, &node));
}

TEST(TrackedRemoteDir, AddingTrackedDirectoryMultiple) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  zx::result root = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(root.status_value());

  fbl::String name = "remote-directory";
  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();

  auto remote = fbl::MakeRefCounted<TestRemoteDir>(std::move(root->client), &loop);
  EXPECT_EQ(ZX_OK, remote->AddAsTrackedEntry(loop.dispatcher(), dir.get(), name));

  // Observe that we cannot track the remote object multiple times.
  EXPECT_EQ(ZX_ERR_BAD_STATE, remote->AddAsTrackedEntry(loop.dispatcher(), dir.get(), name));

  // Observe that we cannot track the remote directory in a different
  // container.
  auto dir2 = fbl::MakeRefCounted<fs::PseudoDir>();
  EXPECT_EQ(ZX_ERR_BAD_STATE, remote->AddAsTrackedEntry(loop.dispatcher(), dir2.get(), name));

  remote.reset();

  // Forcing the remote connection to become "peer closed" causes the entry to be removed.
  root->server.reset();
  EXPECT_EQ(ZX_ERR_BAD_STATE, loop.Run());
  fbl::RefPtr<fs::Vnode> node;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, dir->Lookup(name, &node));
}

TEST(TrackedRemoteDir, TrackAddingDifferentVnode) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  zx::result root = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(root.status_value());

  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();

  auto remote = fbl::MakeRefCounted<TestRemoteDir>(std::move(root->client), &loop);
  auto not_remote = fbl::MakeRefCounted<fs::PseudoDir>();

  // Test a subtle behavior:
  // - Add |remote| to |dir|, begin tracking the remote handle.
  // - Remove |remote| from |dir| (while still tracking).
  // - Add a different Vnode to |dir| with the same name.
  // - Close the remote connection for the still-tracked, but already-removed vnode.
  //
  // This tests that when |remote| is closed, we don't accidentally remove the "wrong" Vnode from
  // the containing pseudodirectory.
  fbl::String name = "remote-directory";
  EXPECT_EQ(ZX_OK, remote->AddAsTrackedEntry(loop.dispatcher(), dir.get(), name));
  EXPECT_EQ(ZX_OK, dir->RemoveEntry(name));
  EXPECT_EQ(ZX_OK, dir->AddEntry(name, not_remote));
  remote.reset();
  root->server.reset();

  EXPECT_EQ(ZX_ERR_BAD_STATE, loop.Run());
  fbl::RefPtr<fs::Vnode> node;

  // The underlying entry should NOT have been removed.
  EXPECT_EQ(ZX_OK, dir->Lookup(name, &node));
}

}  // namespace
