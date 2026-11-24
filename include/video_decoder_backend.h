#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// Video decoder backend seam.
//
// `Decoder` never talks to a codec library directly: it pushes Annex-B access
// units into a `VideoDecoderBackend` and pulls YUV420P `VideoFrame`s out. The
// default backend is UMDPlay's own H.264 decoder (third_party/h264dec), which
// keeps FFmpeg out of the player entirely.

#include "video_frame.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace umdplay {

// Sentinel for "no PES timestamp".
constexpr int64_t kPtsUnknown = INT64_MIN;

class VideoDecoderBackend {
public:
    virtual ~VideoDecoderBackend() = default;

    /// Prime the decoder with Annex-B parameter sets (may be empty).
    virtual bool init(const uint8_t* extradata, size_t size) = 0;

    /**
     * Push one access unit (Annex B, decoding order).
     *
     * @param pts90k PES presentation time on the 90 kHz clock, or kPtsUnknown
     * @return false when the unit could not be decoded
     */
    virtual bool decode(const uint8_t* au, size_t size, int64_t pts90k) = 0;

    /// Pop the next picture in display order; false when none is ready yet.
    virtual bool receive(VideoFrame& out) = 0;

    /// Signal end of stream so reordered pictures are released.
    virtual void flush() = 0;

    /// Forget every picture (used on seek).
    virtual void reset() = 0;

    virtual const char* backend_name() const = 0;  ///< "h264dec", "ffmpeg"
    virtual bool native() const = 0;               ///< built into the player

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual float par() const = 0;          ///< pixel aspect ratio (1.0 if square)
    virtual const char* name() const = 0;   ///< "h264"
};

/// Built-in H.264 decoder (third_party/h264dec, MIT). Always available.
std::unique_ptr<VideoDecoderBackend> make_native_h264_backend();

#if defined(UMDPLAY_FFMPEG_BACKENDS)
/// Optional libavcodec-backed decoder, for A/B builds only.
std::unique_ptr<VideoDecoderBackend> make_ffmpeg_h264_backend();
#endif

}  // namespace umdplay
