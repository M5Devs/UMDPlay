#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// Audio decoder backend seam.
//
// `Decoder` never talks to a codec library directly: it hands each demuxed
// access unit to an `AudioDecoderBackend` and receives interleaved S16LE PCM.
// The default backend is UMDPlay's own ATRAC3+ decoder (see
// `atrac3p_decoder.h`), which keeps FFmpeg out of the audio path entirely.
// A second backend can be added without touching the demuxer or the player.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace umdplay {

class AudioDecoderBackend {
public:
    virtual ~AudioDecoderBackend() = default;

    /** Configure the channel layout (1 = mono, 2 = stereo). */
    virtual bool init(int channels) = 0;

    /**
     * Decode one access unit and append interleaved S16LE samples to `pcm`.
     *
     * @param data      frame payload (container framing already stripped)
     * @param size      payload length in bytes
     * @param pcm       receives little-endian S16 samples (appended)
     * @return samples per channel decoded, 0 if the frame was rejected
     */
    virtual int decode(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& pcm) = 0;

    /** Drop all inter-frame state (call after a seek). */
    virtual void reset() = 0;

    virtual int sample_rate() const = 0;
    virtual bool native() const = 0;    ///< built into the player
    virtual int channels() const = 0;
};

/**
 * The built-in Sony ATRAC3+ backend (44.1 kHz, 2048 samples per channel per
 * access unit). Requires no external libraries.
 */
std::unique_ptr<AudioDecoderBackend> make_native_atrac3p_backend();

#if defined(UMDPLAY_FFMPEG_BACKENDS)
/// Optional libavcodec + libswresample backend, for A/B builds only.
std::unique_ptr<AudioDecoderBackend> make_ffmpeg_atrac3p_backend();
#endif

}  // namespace umdplay
