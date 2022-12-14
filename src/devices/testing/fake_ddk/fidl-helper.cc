// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fake_ddk/fidl-helper.h"

#include <lib/fidl/cpp/wire/channel.h>

#include "src/devices/lib/fidl/transaction.h"

namespace fake_ddk {

void FidlMessenger::Dispatch(fidl::IncomingHeaderAndMessage&& msg, ::fidl::Transaction* txn) {
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  fidl_incoming_msg_t c_msg = std::move(msg).ReleaseToEncodedCMessage();
  auto status = message_op_(op_ctx_, &c_msg, ddk_txn.Txn());
  const bool found = status == ZX_OK || status == ZX_ERR_ASYNC;
  if (!found) {
    FidlHandleCloseMany(c_msg.handles, c_msg.num_handles);
    txn->Close(status);
  }
}

zx_status_t FidlMessenger::SetMessageOp(void* op_ctx, MessageOp* op,
                                        std::optional<zx::channel> optional_remote) {
  zx_status_t status;

  if (message_op_) {
    // Message op was already set
    return ZX_ERR_BAD_STATE;
  }
  message_op_ = op;
  op_ctx_ = op_ctx;

  // If the caller provided a remote endpoint, we use it below and assume they kept the local
  // endpoint. Otherwise, create a new channel and store the local endpoint.
  zx::channel remote;
  if (optional_remote) {
    remote = std::move(optional_remote.value());
  } else {
    if ((status = zx::channel::create(0, &local_, &remote)) < 0) {
      return status;
    }
  }

  if ((status = loop_.StartThread("fake_ddk_fidl")) < 0) {
    return status;
  }

  auto binding = fidl::BindServer<FidlProtocol>(loop_.dispatcher(), std::move(remote), this);
  binding_ = std::make_unique<fidl::ServerBindingRef<FidlProtocol>>(std::move(binding));
  return ZX_OK;
}

}  // namespace fake_ddk
