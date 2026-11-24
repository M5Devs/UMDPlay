#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// Audio-master A/V sync arithmetic. The audio device is the clock: every
// decoded picture is compared against the PTS the loudspeaker has reached and
// is presented, held back, or dropped.

namespace umdplay {

enum class AvAction {
    Present,  ///< due now (within the early window)
    Hold,     ///< ahead of the audio clock: keep it and re-check next tick
    Drop,     ///< behind the audio clock: skip it, decode on
};

struct AvSyncParams {
    /// How early a picture may be shown. One display refresh at 30 Hz is
    /// 33 ms, so a picture that is at most this far ahead is due this tick.
    double early = 0.020;
    /// How far behind the audio clock a picture may fall before it is worth
    /// skipping. Past this the picture is stale and showing it would read as
    /// a stutter.
    double late = 0.080;
};

/// Decide what to do with a picture whose PTS is `video_pts` when the audio
/// clock reads `audio_clock` (both seconds). An unknown PTS (-1) is presented:
/// decode order is the only ordering information available then.
AvAction av_decide(double video_pts, double audio_clock,
                   const AvSyncParams& params = {});

/// Everything the audio clock needs to know. One struct instead of a growing
/// argument list.
struct AudioClock {
    /// Content PTS (seconds) of the audio sample the device cursor started at.
    /// Negative means "not anchored yet": the clock is then unknown (-1).
    double anchor_pts = -1.0;
    /// Frames the device has genuinely played since the anchor (silence pulled
    /// during an underrun must NOT be counted: it is not audio progressing).
    long long played_frames = 0;
    int sample_rate = 44100;
    /// Playback rate; the PCM is time-stretched, so one output frame carries
    /// `speed` times as much content.
    double speed = 1.0;
    /// User sync nudge. Positive = the sound is late relative to the picture.
    int delay_ms = 0;
    /// Frames decoded-but-not-yet-audible: still in our ring plus whatever the
    /// QAudioSink has buffered. Subtracted so the clock describes the speaker,
    /// not the decoder.
    long long latency_frames = 0;
};

/// Content time (seconds) of the audio reaching the speaker right now, or -1
/// while unanchored / for a nonsensical sample rate.
double av_audio_clock(const AudioClock& clock);

/// Legacy shorthands (no speed, delay or latency).
double av_audio_clock(double anchor_pts, long long played_frames,
                      int sample_rate = 44100);
double av_audio_clock(double anchor_pts, long long played_frames, int sample_rate,
                      double speed, int audio_delay_ms);

}  // namespace umdplay
