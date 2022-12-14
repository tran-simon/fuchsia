// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_V2_NODE_GROUP_V2_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_V2_NODE_GROUP_V2_H_

#include "src/devices/bin/driver_manager/node_group/node_group.h"
#include "src/devices/bin/driver_manager/v2/parent_set_collector.h"

namespace dfv2 {

class NodeGroupV2 : public NodeGroup {
 public:
  // Must only be called by Create() to ensure the objects are verified.
  NodeGroupV2(NodeGroupCreateInfo create_info, async_dispatcher_t* dispatcher,
              NodeManager* node_manager);

  ~NodeGroupV2() override = default;

 protected:
  zx::result<std::optional<DeviceOrNode>> BindNodeImpl(
      fuchsia_driver_index::wire::MatchedNodeGroupInfo info,
      const DeviceOrNode& device_or_node) override;

 private:
  std::optional<ParentSetCollector> parent_set_collector_;
  async_dispatcher_t* const dispatcher_;
  NodeManager* node_manager_;
};

}  // namespace dfv2

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_V2_NODE_GROUP_V2_H_
