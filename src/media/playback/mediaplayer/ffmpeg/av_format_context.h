// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FORMAT_CONTEXT_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FORMAT_CONTEXT_H_

#include "src/media/playback/mediaplayer/ffmpeg/av_io_context.h"
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

namespace media_player {

struct AVFormatContextDeleter {
  inline void operator()(AVFormatContext* ptr) const { avformat_free_context(ptr); }
};

using AvFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AvFormatContext {
  static AvFormatContextPtr OpenInput(const AvIoContextPtr& io_context) {
    // We use a raw pointer here, because avformat_open_input wants the
    // opportunity to replace the context (which is why we pass it a pointer
    // to the pointer). This can't happen if the context is already managed by
    // an AvFormatContextPtr.
    AVFormatContext* format_context = avformat_alloc_context();
    format_context->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FAST_SEEK;
    format_context->pb = io_context.get();

    // TODO(dalesat): This synchronous operation may take a long time.
    int r = avformat_open_input(&format_context, nullptr, nullptr, nullptr);
    if (r < 0) {
      // avformat_open_input promises to delete the context if it fails.
      FX_DCHECK(format_context == nullptr);
      format_context = nullptr;
    }

    return AvFormatContextPtr(format_context);
  }
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FORMAT_CONTEXT_H_
