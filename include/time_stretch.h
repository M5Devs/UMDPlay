#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// Pitch-preserving time scaling for interleaved stereo S16 PCM, used to play
// at speeds other than 1x without the "chipmunk" pitch shift that resampling
// would cause.
//
// WSOLA: fixed 20 ms grains, 50% overlap-add with triangular weights (which
// sum to exactly 1, so a steady tone keeps its level), and a small waveform-
// similarity search around the nominal input position so overlapping grains
// stay phase-coherent. Speed 1.0 is a bit-exact passthrough.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace umdplay {

class TimeStretcher {
public:
    /// Interleaved stereo frames per grain (20 ms at 44.1 kHz).
    static constexpr int kGrain = 880;
    /// Synthesis hop: half a grain, so two windows always overlap.
    static constexpr int kHop = kGrain / 2;
    /// Waveform-similarity search range, in frames, and its step.
    static constexpr int kSearch = 220;
    static constexpr int kSearchStep = 4;

    /// Reset all internal state (seek, speed change back to 1.0, stop).
    void reset();

    /// Set the rate: 1.0 = untouched, 2.0 = twice as fast, 0.5 = half speed.
    /// Out-of-range values are clamped to [0.5, 2.0].
    void set_speed(double speed);
    double speed() const { return m_speed; }

    /// Append stretched PCM for `frames` input frames to `out`.
    void process(const int16_t* in, std::size_t frames, std::vector<int16_t>& out);

    /// Emit whatever is still buffered when the input ends (fades the tail
    /// out rather than clicking).
    void flush(std::vector<int16_t>& out);

private:
    /// Frames of buffered input that have not been consumed yet.
    std::size_t available() const { return m_buf.size() / 2 - m_pos; }
    /// True once `available() >= kGrain + kSearch`.
    bool ready() const;

    double m_speed = 1.0;
    std::vector<int16_t> m_buf;     ///< interleaved input, m_pos frames consumed
    std::size_t m_pos = 0;
    std::vector<float> m_tail;      ///< last kHop output frames (for the search)
    double m_in_hop = 0.0;          ///< fractional input advance per grain
    double m_credit = 0.0;          ///< sub-frame remainder of m_in_hop
};


namespace {
constexpr int kChannels = 2;

/// Dot product of an integer grain with the float tail buffer (the tail keeps
/// fractional samples, so it cannot be reinterpreted as int16).
inline float dot(const int16_t* a, const float* b, int frames) {
    float acc = 0.0f;
    const int n = frames * kChannels;
    for (int i = 0; i < n; ++i) acc += static_cast<float>(a[i]) * b[i];
    return acc;
}

inline float energy(const int16_t* a, int frames) {
    float acc = 0.0f;
    const int n = frames * kChannels;
    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(a[i]);
        acc += s * s;
    }
    return acc;
}
}  // namespace

inline void TimeStretcher::reset() {
    m_buf.clear();
    m_pos = 0;
    m_tail.clear();
    m_credit = 0.0;
    m_in_hop = 0.0;
}

inline void TimeStretcher::set_speed(double speed) {
    if (!std::isfinite(speed)) speed = 1.0;
    speed = std::min(2.0, std::max(0.5, speed));
    if (speed == m_speed) return;
    m_speed = speed;
    reset();
}

inline bool TimeStretcher::ready() const {
    return available() >= static_cast<std::size_t>(kGrain + kSearch);
}

inline void TimeStretcher::process(const int16_t* in, std::size_t frames,
                            std::vector<int16_t>& out) {
    if (!in || frames == 0) return;

    if (m_speed == 1.0) {
        const std::size_t at = out.size();
        out.resize(at + frames * kChannels);
        std::memcpy(out.data() + at, in, frames * kChannels * sizeof(int16_t));
        return;
    }

    m_buf.insert(m_buf.end(), in, in + frames * kChannels);
    m_in_hop = kHop * m_speed;

    while (ready()) {
        const int16_t* base = m_buf.data() + m_pos * kChannels;

        // WSOLA: slide the grain by up to +-kSearch frames to best match the
        // tail of what we just emitted, so the crossfade does not cancel.
        int best_shift = 0;
        if (m_tail.size() == static_cast<std::size_t>(kHop * kChannels)) {
            // Never search before the start of the buffer we still hold.
            const int lo = std::max(-kSearch, -static_cast<int>(m_pos));
            float best = -1e30f;
            for (int d = lo; d <= kSearch; d += kSearchStep) {
                const int16_t* cand = base + d * kChannels;
                const float e = energy(cand, kHop);
                if (e <= 0.0f) continue;
                const float score = dot(cand, m_tail.data(), kHop) / std::sqrt(e);
                // Prefer the smaller shift on a near-tie: it keeps the tempo
                // honest and avoids a slow drift.
                const float bias = 1.0f - 0.0005f * static_cast<float>(d < 0 ? -d : d);
                if (score * bias > best) { best = score * bias; best_shift = d; }
            }
        }
        const int16_t* grain = base + best_shift * kChannels;

        // Overlap-add the grain with triangular weights. The first half
        // crossfades against the previous grain's tail; the second half is
        // emitted as the new tail for the next grain.
        const std::size_t at = out.size();
        out.resize(at + kHop * kChannels);
        int16_t* dst = out.data() + at;

        for (int n = 0; n < kHop; ++n) {
            const float w = static_cast<float>(n) / static_cast<float>(kHop);
            for (int c = 0; c < kChannels; ++c) {
                const int idx = n * kChannels + c;
                float fade_in = static_cast<float>(grain[idx]) * w;
                if (!m_tail.empty()) fade_in += m_tail[static_cast<std::size_t>(idx)] * (1.0f - w);
                const float v = std::min(32767.0f, std::max(-32768.0f, fade_in));
                dst[idx] = static_cast<int16_t>(std::lround(v));
            }
        }

        // The next grain's tail is this grain's *raw* second half; the (1 - w)
        // taper is applied once, when it is crossfaded in. Weighting it here
        // too would square the taper and dip the level at every seam.
        m_tail.assign(static_cast<std::size_t>(kHop * kChannels), 0.0f);
        for (int n = 0; n < kHop; ++n) {
            const int src = (kHop + n) * kChannels;
            for (int c = 0; c < kChannels; ++c)
                m_tail[static_cast<std::size_t>(n * kChannels + c)] =
                    static_cast<float>(grain[src + c]);
        }

        // Advance the input by the (fractional) input hop.
        m_credit += m_in_hop;
        const auto whole = static_cast<std::size_t>(m_credit);
        m_credit -= static_cast<double>(whole);
        m_pos = std::min(m_pos + whole, m_buf.size() / kChannels);

        // Drop consumed input so a long stream does not grow the buffer.
        if (m_pos > static_cast<std::size_t>(8 * kGrain)) {
            m_buf.erase(m_buf.begin(),
                        m_buf.begin() + static_cast<std::ptrdiff_t>(m_pos * kChannels));
            m_pos = 0;
        }
    }
}

inline void TimeStretcher::flush(std::vector<int16_t>& out) {
    const std::size_t left = available();
    if (left == 0) {
        reset();
        return;
    }
    // Window the remainder in and out so neither joining the last grain nor
    // ending the stream clicks. This runs once per stream/seek, so the few
    // attenuated milliseconds cost nothing.
    const std::size_t at = out.size();
    out.resize(at + left * kChannels);
    int16_t* dst = out.data() + at;
    const int16_t* src = m_buf.data() + m_pos * kChannels;
    const std::size_t ramp = std::min<std::size_t>(left / 2, kHop / 2);
    for (std::size_t n = 0; n < left; ++n) {
        float w = 1.0f;
        if (ramp > 0) {
            const float up = static_cast<float>(n) / static_cast<float>(ramp);
            const float down = static_cast<float>(left - 1 - n) / static_cast<float>(ramp);
            w = std::min(1.0f, std::min(up, down));
        }
        for (int c = 0; c < kChannels; ++c) {
            const std::size_t idx = n * kChannels + static_cast<std::size_t>(c);
            dst[idx] = static_cast<int16_t>(std::lround(static_cast<float>(src[idx]) * w));
        }
    }
    reset();
}

}  // namespace umdplay
