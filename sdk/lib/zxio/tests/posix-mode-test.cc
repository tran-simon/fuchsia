// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zxio/posix_mode.h>
#include <lib/zxio/types.h>
#include <sys/stat.h>

#include <zxtest/zxtest.h>

#include "sdk/lib/zxio/private.h"

// Tests the conversion between |zxio_node_attributes_t| and POSIX |mode_t|.

TEST(NodeProtocolsToPosixType, Basic) {
  EXPECT_EQ(S_IFREG, zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_CONNECTOR));
  EXPECT_EQ(S_IFDIR, zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_DIRECTORY));
  EXPECT_EQ(S_IFREG, zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_FILE));
}

TEST(NodeProtocolsToPosixType, MultiProtocol) {
  EXPECT_EQ(S_IFREG, zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_FILE));
  // If the node supports both directory and file protocol, we only assert that
  // the conversion result is either |S_IFDIR| (directory) or |S_IFREG| (file).
  EXPECT_TRUE(zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_DIRECTORY |
                                                ZXIO_NODE_PROTOCOL_FILE) == S_IFDIR ||
              zxio_node_protocols_to_posix_type(ZXIO_NODE_PROTOCOL_DIRECTORY |
                                                ZXIO_NODE_PROTOCOL_FILE) == S_IFREG);
}

TEST(AbilitiesToPosixPermissions, File) {
  EXPECT_EQ(S_IRUSR | S_IFREG,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE, ZXIO_OPERATION_READ_BYTES));
  EXPECT_EQ(S_IRUSR | S_IWUSR | S_IFREG,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE,
                                ZXIO_OPERATION_READ_BYTES | ZXIO_OPERATION_WRITE_BYTES));
  EXPECT_EQ(S_IRUSR | S_IWUSR | S_IXUSR | S_IFREG,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE, ZXIO_OPERATION_READ_BYTES |
                                                             ZXIO_OPERATION_WRITE_BYTES |
                                                             ZXIO_OPERATION_EXECUTE));
  EXPECT_EQ(S_IFREG, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE, ZXIO_OPERATION_ENUMERATE));
  EXPECT_EQ(S_IFREG, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE, ZXIO_OPERATION_MODIFY_DIRECTORY));
  EXPECT_EQ(S_IFREG, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_FILE, ZXIO_OPERATION_TRAVERSE));
}

TEST(AbilitiesToPosixPermissions, Directory) {
  EXPECT_EQ(S_IRUSR | S_IFDIR,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY, ZXIO_OPERATION_ENUMERATE));
  EXPECT_EQ(S_IRUSR | S_IWUSR | S_IFDIR,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY,
                                ZXIO_OPERATION_ENUMERATE | ZXIO_OPERATION_MODIFY_DIRECTORY));
  EXPECT_EQ(S_IRUSR | S_IWUSR | S_IXUSR | S_IFDIR,
            zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY, ZXIO_OPERATION_ENUMERATE |
                                                                  ZXIO_OPERATION_MODIFY_DIRECTORY |
                                                                  ZXIO_OPERATION_TRAVERSE));
  // These are ignored when converting in directory mode.
  EXPECT_EQ(S_IFDIR, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY, ZXIO_OPERATION_READ_BYTES));
  EXPECT_EQ(S_IFDIR, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY, ZXIO_OPERATION_WRITE_BYTES));
  EXPECT_EQ(S_IFDIR, zxio_get_posix_mode(ZXIO_NODE_PROTOCOL_DIRECTORY, ZXIO_OPERATION_EXECUTE));
}
