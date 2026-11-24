#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>

// SPDX-License-Identifier: GPL-2.0-only
//
// VLC-style playback tweaks, as pure state machines and arithmetic so the
// front ends only have to forward key presses: playback speed steps, the
// audio-sync nudge, and the A-B loop.

namespace umdplay {

// --- playback speed --------------------------------------------------------

// The steps the UI offers, slowest first. 1.0 is always present.
inline constexpr int    kSpeedStepCount = 6;
inline constexpr double kMinSpeed = 0.5;
inline constexpr double kMaxSpeed = 2.0;
inline constexpr double kSpeedSteps[kSpeedStepCount] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};

/// Clamp an arbitrary rate into [kMinSpeed, kMaxSpeed]; non-finite -> 1.0.
double clamp_speed(double speed);   // defined below (inline)

/// Index of the step nearest `speed` (ties go to the slower step).
int speed_index(double speed);

/// Step up / down the ladder, saturating at either end. An unlisted speed
/// (say 1.1) snaps to the next step in the requested direction.
double speed_up(double speed);
double speed_down(double speed);

/// "1.0x" / "0.75x" into `buf`; returns buf. Buffer need only be 16 bytes.
const char* speed_label(double speed, char* buf, unsigned long size);

// --- audio sync nudge ------------------------------------------------------

inline constexpr int kMaxAudioDelayMs = 2000;
inline constexpr int kAudioDelayStepMs = 50;

/// Clamp to +/-kMaxAudioDelayMs and snap to a multiple of kAudioDelayStepMs.
int clamp_audio_delay_ms(int ms);
/// One step (+1 / -1) from `ms`, clamped.
int step_audio_delay_ms(int ms, int direction);

/// "Audio delay: +100 ms" into `buf`, where `prefix` may be nullptr.
/// Positive means the sound is late relative to the picture.
const char* audio_delay_label(int ms, char* buf, unsigned long size,
                              const char* prefix = nullptr);

// --- A-B loop --------------------------------------------------------------

enum class AbLoopState {
    Off,      ///< no loop points
    SetA,     ///< A marked, waiting for B
    Looping,  ///< A and B set, playback confined to [a, b]
};

inline constexpr double kAbMinSpan = 0.5;  ///< seconds; shorter is rejected

/// The A-B repeat state machine. Each press walks
/// Off -> SetA -> Looping -> Off (the player's L key / OSD button).
struct AbLoop {
    AbLoopState state = AbLoopState::Off;
    double      a = 0.0;   ///< loop start, seconds
    double      b = 0.0;   ///< loop end, seconds

    /// Advance one step from `position` (the current playhead). Marking B at
    /// or before A, or closer than kAbMinSpan, is ignored and stays on SetA so
    /// the user can pick a later point.
    void press(double position);

    /// Forget both points.
    void clear();

    bool active() const { return state == AbLoopState::Looping; }
    /// True when `t` is at or past B: the caller should seek back to A.
    bool expired(double t) const { return active() && t >= b; }
    /// Span in seconds (0 unless looping).
    double span() const { return active() ? b - a : 0.0; }
    /// "A-B 0:12 → 0:41" for the OSD; empty when off.
    const char* label(char* buf, unsigned long size) const;
};

// --- speed ----------------------------------------------------------------

inline double clamp_speed(double speed) {
    if (!std::isfinite(speed)) return 1.0;
    if (speed < kMinSpeed) return kMinSpeed;
    if (speed > kMaxSpeed) return kMaxSpeed;
    return speed;
}

inline int speed_index(double speed) {
    speed = clamp_speed(speed);
    int best = 0;
    double best_dist = std::abs(kSpeedSteps[0] - speed);
    for (int i = 1; i < kSpeedStepCount; ++i) {
        const double d = std::abs(kSpeedSteps[i] - speed);
        // Strictly closer only: ties resolve to the slower (earlier) step.
        if (d < best_dist) { best = i; best_dist = d; }
    }
    return best;
}

// The first listed step above `speed` (an off-ladder 1.1 -> 1.25), saturating
// at the ends.
inline double speed_up(double speed) {
    speed = clamp_speed(speed);
    for (int i = 0; i < kSpeedStepCount; ++i)
        if (kSpeedSteps[i] > speed + 1e-9) return kSpeedSteps[i];
    return kSpeedSteps[kSpeedStepCount - 1];
}

// The last listed step below `speed` (1.25 -> 1.0 when asked to slow down).
inline double speed_down(double speed) {
    speed = clamp_speed(speed);
    for (int i = kSpeedStepCount - 1; i >= 0; --i)
        if (kSpeedSteps[i] < speed - 1e-9) return kSpeedSteps[i];
    return kSpeedSteps[0];
}

inline const char* speed_label(double speed, char* buf, unsigned long size) {
    if (!buf || size == 0) return "";
    std::snprintf(buf, size, "%.2fx", clamp_speed(speed));
    // Trim a trailing zero: "1.00x" -> "1.0x", "1.25x" stays.
    char* dot = std::strchr(buf, '.');
    if (dot) {
        char* end = buf + std::strlen(buf);
        if (end > dot + 2 && end[-2] == '0') {
            end[-2] = end[-1];       // slide the 'x' over the zero
            end[-1] = '\0';
        }
    }
    return buf;
}

// --- audio delay ----------------------------------------------------------

inline int clamp_audio_delay_ms(int ms) {
    if (ms > kMaxAudioDelayMs) ms = kMaxAudioDelayMs;
    if (ms < -kMaxAudioDelayMs) ms = -kMaxAudioDelayMs;
    // Snap to the nearest step, away from zero on a tie so a nudge never looks
    // like it did nothing.
    const int sign = ms < 0 ? -1 : 1;
    const int mag = std::abs(ms);
    if (mag <= 0) return 0;
    return sign * ((mag + kAudioDelayStepMs / 2) / kAudioDelayStepMs) * kAudioDelayStepMs;
}

inline int step_audio_delay_ms(int ms, int direction) {
    if (direction == 0) return clamp_audio_delay_ms(ms);
    return clamp_audio_delay_ms(clamp_audio_delay_ms(ms) +
                                (direction < 0 ? -kAudioDelayStepMs : kAudioDelayStepMs));
}

inline const char* audio_delay_label(int ms, char* buf, unsigned long size,
                                     const char* prefix) {
    if (!buf || size == 0) return "";
    ms = clamp_audio_delay_ms(ms);
    std::snprintf(buf, size, "%s%+d ms", prefix ? prefix : "", ms);
    return buf;
}

// --- A-B loop -------------------------------------------------------------

inline void AbLoop::clear() {
    state = AbLoopState::Off;
    a = 0.0;
    b = 0.0;
}

inline void AbLoop::press(double position) {
    if (!std::isfinite(position) || position < 0.0) return;

    switch (state) {
        case AbLoopState::Off:
            a = position;
            b = 0.0;
            state = AbLoopState::SetA;
            return;
        case AbLoopState::SetA:
            // B must come after A with a usable span; otherwise stay on SetA
            // and let the user try again.
            if (position < a + kAbMinSpan) return;
            b = position;
            state = AbLoopState::Looping;
            return;
        case AbLoopState::Looping:
            clear();
            return;
    }
}

inline const char* AbLoop::label(char* buf, unsigned long size) const {
    if (!buf || size == 0) return "";
    switch (state) {
        case AbLoopState::Off:  buf[0] = '\0'; return buf;
        case AbLoopState::SetA: std::snprintf(buf, size, "A-B: A set"); return buf;
        case AbLoopState::Looping: {
            const int as = static_cast<int>(a + 0.5);
            const int bs = static_cast<int>(b + 0.5);
            std::snprintf(buf, size, "A-B %d:%02d \xe2\x86\x92 %d:%02d",
                          as / 60, as % 60, bs / 60, bs % 60);
            return buf;
        }
    }
    buf[0] = '\0';
    return buf;
}

}  // namespace umdplay
