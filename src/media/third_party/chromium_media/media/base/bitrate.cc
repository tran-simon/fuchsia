// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/bitrate.h"
// Fuchsia change.
// #include "base/check_op.h"
// #include "base/strings/stringprintf.h"
#include "chromium_utils.h"

namespace media {

// static
Bitrate Bitrate::VariableBitrate(uint32_t target_bps, uint32_t peak_bps) {
  DCHECK_GE(peak_bps, target_bps);
  return Bitrate(Mode::kVariable, target_bps, peak_bps);
}

bool Bitrate::operator==(const Bitrate& right) const {
  return (this->mode_ == right.mode_) &&
         (this->target_bps_ == right.target_bps_) &&
         (this->peak_bps_ == right.peak_bps_);
}

bool Bitrate::operator!=(const Bitrate& right) const {
  return !(*this == right);
}

uint32_t Bitrate::peak_bps() const {
  DCHECK_EQ(mode_ == Mode::kConstant, peak_bps_ == 0u);
  return peak_bps_;
}

std::string Bitrate::ToString() const {
  switch (mode_) {
    case Mode::kConstant:
      return base::StringPrintf("CBR: %d bps", target_bps_);
    case Mode::kVariable:
      return base::StringPrintf("VBR: target %d bps, peak %d bps", target_bps_,
                                peak_bps_);
  }
}

}  // namespace media
