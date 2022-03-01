// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/cpp/internal/message_extensions.h>
#include <lib/fidl/cpp/internal/natural_server_messenger.h>
#include <lib/fidl/cpp/message.h>
#include <lib/fidl/llcpp/transaction.h>

#include "lib/fidl/llcpp/internal/transport.h"

namespace fidl {
namespace internal {

// TODO(fxbug.dev/82189): Switch to new natural domain objects instead of HLCPP.
void NaturalServerMessenger::SendReply(const fidl_type_t* type,
                                       HLCPPOutgoingMessage&& message) const {
  ConvertFromHLCPPOutgoingMessageThen(
      type, std::move(message), [&](fidl::OutgoingMessage outgoing) {
        completer_base_->SendReply(&outgoing, OutgoingTransportContext{});
      });
}

}  // namespace internal
}  // namespace fidl
