// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/ffmpeg/ffmpeg_decoder_base.h"

#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include "src/media/playback/mediaplayer/ffmpeg/av_codec_context.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"

namespace media_player {

FfmpegDecoderBase::FfmpegDecoderBase(AvCodecContextPtr av_codec_context)
    : av_codec_context_(std::move(av_codec_context)), av_frame_ptr_(ffmpeg::AvFrame::Create()) {
  FX_DCHECK(av_codec_context_);

  av_codec_context_->opaque = this;
  av_codec_context_->get_buffer2 = AllocateBufferForAvFrame;
}

FfmpegDecoderBase::~FfmpegDecoderBase() {}

std::unique_ptr<StreamType> FfmpegDecoderBase::output_stream_type() const {
  return AvCodecContext::GetStreamType(*av_codec_context_);
}

void FfmpegDecoderBase::Flush() {
  FX_DCHECK(is_worker_thread());
  avcodec_flush_buffers(av_codec_context_.get());
  next_pts_ = Packet::kNoPts;
}

bool FfmpegDecoderBase::TransformPacket(const PacketPtr& input, bool new_input, PacketPtr* output) {
  FX_DCHECK(is_worker_thread());
  FX_DCHECK(input);
  FX_DCHECK(output);

  TRACE_DURATION("mediaplayer:decode", "DecodePacket", "type",
                 (av_codec_context_->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));

  *output = nullptr;

  if (new_input) {
    if (input->size() == 0 && !input->end_of_stream()) {
      // This packet isn't end-of-stream, but it has size zero. The underlying
      // decoder interprets an empty input packet as end-of-stream, so we
      // we refrain from decoding this packet and return true to indicate we're
      // done with it.
      //
      // The underlying decoder gets its end-of-stream indication in one of
      // two ways:
      // 1) If the end-of-stream packet is empty, it will get past this check
      //    and be submitted to the decoder, indicating end-of-stream.
      // 2) If the end-of-stream packet is not empty, we let it through and
      //    follow it with an empty end-of-stream packet that we create for
      //    that purpose.
      return true;
    }

    OnNewInputPacket(input);

    // Send the packet to the ffmpeg decoder. If it fails, return true to
    // indicate we're done with the packet.
    if (SendPacket(input) != 0) {
      if (input->end_of_stream()) {
        // The input packet was end-of-stream. We won't get called again before
        // a flush, so make sure the output gets an end-of-stream packet.
        *output = CreateEndOfStreamPacket();
      }

      return true;
    }
  }

  int result = avcodec_receive_frame(av_codec_context_.get(), av_frame_ptr_.get());

  switch (result) {
    case 0: {
      // Succeeded, frame produced. We're not done with the input packet.
      auto* payload_buffer_raw_ptr =
          reinterpret_cast<PayloadBuffer*>(av_buffer_get_opaque(av_frame_ptr_->buf[0]));

      // Take a fresh reference to the payload_buffer since av_frame_unref will
      // drop the av_frame's reference via ReleaseBufferForAvFrame.
      *output = CreateOutputPacket(*av_frame_ptr_, fbl::RefPtr(payload_buffer_raw_ptr));

      // Release the frame returned by |avcodec_receive_frame|.
      av_frame_unref(av_frame_ptr_.get());
      return false;
    }

    case AVERROR(EAGAIN):
      // Succeeded, no frame produced.
      if (input->end_of_stream() && input->size() != 0) {
        // The input packet is an end-of-stream packet, and it has payload. The
        // underlying decoder interprets an empty packet as end-of-stream, so
        // we need to send it an empty packet.
        if (SendPacket(CreateEndOfStreamPacket()) == 0) {
          // |SendPacket| succeeded. We return false to indicate we're not done
          // with the original end-of-stream packet. We'll get called again with
          // the same end-of-stream packet and |new_input| set to false. That
          // will continue until we've extracted all the output packets the
          // decoder has to give us. Note that we won't end up here again,
          // because |avcodec_receive_frame| will return either 0 or
          // |AVERROR_EOF|, not |AVERROR(EAGAIN)|.
          return false;
        }

        // |SendPacket| failed. We return true to indicate we're done with the
        // input packet. We also output an end-of-stream packet to terminate
        // the output stream.
        *output = CreateEndOfStreamPacket();
      }

      // Indicate we're done with the input packet.
      return true;

    case AVERROR_EOF:
      // Succeeded, no frame produced, end-of-stream sequence complete.
      // Produce an end-of-stream packet.
      FX_DCHECK(input->end_of_stream());
      *output = CreateEndOfStreamPacket();
      return true;

    default:
      FX_LOGS(ERROR) << "avcodec_receive_frame failed " << result;
      if (input->end_of_stream()) {
        // The input packet was end-of-stream. We won't get called again before
        // a flush, so make sure the output gets an end-of-stream packet.
        *output = CreateEndOfStreamPacket();
      }

      return true;
  }
}

int FfmpegDecoderBase::SendPacket(const PacketPtr& input) {
  FX_DCHECK(input);

  AVPacket* av_packet = av_packet_alloc();
  FX_DCHECK(av_packet);

  av_packet->data = reinterpret_cast<uint8_t*>(input->payload());
  av_packet->size = input->size();
  av_packet->pts = input->pts();
  if (input->keyframe()) {
    av_packet->flags |= AV_PKT_FLAG_KEY;
  }

  int result = avcodec_send_packet(av_codec_context_.get(), av_packet);

  av_packet_free(&av_packet);

  if (result != 0) {
    FX_LOGS(WARNING) << "avcodec_send_packet failed " << result;
  }

  return result;
}

void FfmpegDecoderBase::OnNewInputPacket(const PacketPtr& packet) {}

AVBufferRef* FfmpegDecoderBase::CreateAVBuffer(fbl::RefPtr<PayloadBuffer> payload_buffer) {
  FX_DCHECK(payload_buffer);
  FX_DCHECK(payload_buffer->size() <= static_cast<uint64_t>(std::numeric_limits<int>::max()));
  uint8_t* data = reinterpret_cast<uint8_t*>(payload_buffer->data());
  int size = static_cast<int>(payload_buffer->size());
  void* opaque = fbl::ExportToRawPtr(&payload_buffer);

  return av_buffer_create(data, size, ReleaseBufferForAvFrame, opaque, /* flags */ 0);
}

// static
int FfmpegDecoderBase::AllocateBufferForAvFrame(AVCodecContext* av_codec_context, AVFrame* av_frame,
                                                int flags) {
  // It's important to use av_codec_context here rather than context(),
  // because av_codec_context is different for different threads when we're
  // decoding on multiple threads. Be sure to avoid using self->context() or
  // self->av_codec_context_.

  // AV_CODEC_CAP_DR1 is required in order to do allocation this way.
  FX_DCHECK(av_codec_context->codec->capabilities & AV_CODEC_CAP_DR1);

  FfmpegDecoderBase* self = reinterpret_cast<FfmpegDecoderBase*>(av_codec_context->opaque);
  FX_DCHECK(self);

  return self->BuildAVFrame(*av_codec_context, av_frame);
}

// static
void FfmpegDecoderBase::ReleaseBufferForAvFrame(void* opaque, uint8_t* buffer) {
  FX_DCHECK(opaque);
  FX_DCHECK(buffer);

  auto ref = fbl::ImportFromRawPtr(reinterpret_cast<PayloadBuffer*>(opaque));

  FX_DCHECK(buffer == ref->data());
}

PacketPtr FfmpegDecoderBase::CreateEndOfStreamPacket() {
  return Packet::CreateEndOfStream(next_pts_, pts_rate_);
}

void FfmpegDecoderBase::Dump(std::ostream& os) const {
  SoftwareProcessor::Dump(os);

  os << fostr::Indent;
  os << fostr::NewLine << "next pts:          " << AsNs(next_pts_) << "@" << pts_rate_;
  os << fostr::Outdent;
}

}  // namespace media_player
