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

/// Audio clock for a stream anchored at `anchor_pts` that has since played
/// `played_frames` frames of `sample_rate` Hz audio.
double av_audio_clock(double anchor_pts, long long played_frames,
                      int sample_rate = 44100);

}  // namespace umdplay
