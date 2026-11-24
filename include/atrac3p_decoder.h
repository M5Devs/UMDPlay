#pragma once

// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Native, self-contained Sony ATRAC3+ decoder — no FFmpeg.
//
// Decodes one PSMF ATRAC3+ access unit (the frame payload with Sony's 8-byte
// `0F D0 48 <xx>` sync header already stripped) into 44.1 kHz S16 PCM.  It is a
// C++17 port of FFmpeg's atrac3plus decoder by Maxim Poliakovski; see
// NOTICE and src/atrac3p/ for attribution and the individual ported units.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace umdplay {

class Atrac3PlusDecoder {
public:
    static constexpr int    kSampleRate      = 44100;
    static constexpr int    kChannels        = 2;
    static constexpr int    kSamplesPerFrame = 2048;  ///< per channel
    static constexpr size_t kBytesPerFrame   = static_cast<size_t>(kSamplesPerFrame) *
                                               kChannels * sizeof(int16_t);

    Atrac3PlusDecoder();
    ~Atrac3PlusDecoder();

    Atrac3PlusDecoder(const Atrac3PlusDecoder&)            = delete;
    Atrac3PlusDecoder& operator=(const Atrac3PlusDecoder&) = delete;

    /** Configure the channel layout (1 = mono, 2 = stereo). */
    bool init(int channels = kChannels);

    bool initialized() const;
    int  channels() const;

    /**
     * Decode one access unit.
     *
     * @param data  frame payload, PSMF sync header already stripped
     * @param size  payload length in bytes (688 / 1712 for UMD, any size works)
     * @param pcm   receives interleaved S16 samples (appended)
     * @return samples per channel decoded (kSamplesPerFrame on success),
     *         0 if the frame was rejected, negative on a fatal parse error.
     */
    int decode(const uint8_t* data, size_t size, std::vector<int16_t>& pcm);

    /** Same, but appends little-endian S16 bytes (what SDL wants). */
    int decode(const uint8_t* data, size_t size, std::vector<uint8_t>& pcm_bytes);

    /** Drop all inter-frame state (call after a seek). */
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace umdplay
