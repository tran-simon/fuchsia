// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "forcewake.h"

#include <thread>

#include "magma_util/macros.h"
#include "msd_intel_register_io.h"

ForceWake::ForceWake(MsdIntelRegisterIo* register_io, uint32_t device_id) {
  status_render_ = registers::ForceWakeStatus::GetRender(register_io);

  if (DeviceId::is_gen12(device_id)) {
    status_gen12_vdbox0_ = registers::ForceWakeStatus::GetGen12Vdbox0(register_io);
  } else {
    status_gen9_media_ = registers::ForceWakeStatus::GetGen9Media(register_io);
  }
}

bool ForceWake::IsActive(MsdIntelRegisterIo* reg_io, ForceWakeDomain domain) {
  get_status_register(domain)->ReadFrom(reg_io);
  return is_active_cached(domain);
}

bool ForceWake::Reset(MsdIntelRegisterIo* reg_io, ForceWakeDomain domain) {
  registers::ForceWakeRequest::reset(reg_io, get_request_offset(domain));

  return Wait(reg_io, domain, false);
}

bool ForceWake::Request(MsdIntelRegisterIo* reg_io, ForceWakeDomain domain) {
  if (IsActive(reg_io, domain))
    return true;

  registers::ForceWakeRequest::write(reg_io, get_request_offset(domain), 1 << kThreadShift,
                                     1 << kThreadShift);

  return Wait(reg_io, domain, true);
}

bool ForceWake::Release(MsdIntelRegisterIo* reg_io, ForceWakeDomain domain) {
  if (!IsActive(reg_io, domain))
    return true;

  registers::ForceWakeRequest::write(reg_io, get_request_offset(domain), 1 << kThreadShift, 0);

  return Wait(reg_io, domain, false);
}

bool ForceWake::Wait(MsdIntelRegisterIo* register_io, ForceWakeDomain domain, bool set) {
  registers::ForceWakeStatus* status_register = get_status_register(domain);
  DASSERT(status_register);

  for (unsigned int ms = 0; ms < kRetryMaxMs; ms++) {
    status_register->ReadFrom(register_io);

    uint32_t status = status_register->status();
    if (((status >> kThreadShift) & 1) == (set ? 1 : 0))
      return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  MAGMA_LOG(WARNING, "Timed out waiting for forcewake domain %d set %d", domain, set);
  return false;
}
