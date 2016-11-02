// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _MAGMA_SYSTEM_BUFFER_H_
#define _MAGMA_SYSTEM_BUFFER_H_

#include "msd.h"
#include "platform_buffer.h"

#include <functional>
#include <memory>

using msd_buffer_unique_ptr_t = std::unique_ptr<msd_buffer, decltype(&msd_buffer_destroy)>;

static inline msd_buffer_unique_ptr_t MsdBufferUniquePtr(msd_buffer* buffer)
{
    return msd_buffer_unique_ptr_t(buffer, &msd_buffer_destroy);
}

class MagmaSystemBuffer {
public:
    static std::unique_ptr<MagmaSystemBuffer>
    Create(std::unique_ptr<magma::PlatformBuffer> platform_buffer);
    ~MagmaSystemBuffer() {}

    uint64_t size() { return platform_buf_->size(); }
    uint64_t id() { return platform_buf_->id(); }

    // note: this does not relinquish ownership of the PlatformBuffer
    magma::OpaquePlatformBuffer* platform_buffer() { return platform_buf_.get(); }

    msd_buffer* msd_buf() { return msd_buf_.get(); }

private:
    MagmaSystemBuffer(std::unique_ptr<magma::PlatformBuffer> platform_buf,
                      msd_buffer_unique_ptr_t msd_buf);
    std::unique_ptr<magma::OpaquePlatformBuffer> platform_buf_;
    msd_buffer_unique_ptr_t msd_buf_;
};

#endif //_MAGMA_SYSTEM_BUFFER_H_