// SPDX-License-Identifier: GPL-2.0-only

#include "av_sync.h"

namespace umdplay {

AvAction av_decide(double video_pts, double audio_clock, const AvSyncParams& params) {
    if (video_pts < 0.0 || audio_clock < 0.0) return AvAction::Present;
    if (video_pts > audio_clock + params.early) return AvAction::Hold;
    if (video_pts < audio_clock - params.late) return AvAction::Drop;
    return AvAction::Present;
}

double av_audio_clock(const AudioClock& c) {
    if (c.anchor_pts < 0.0 || c.sample_rate <= 0) return -1.0;

    const long long played = c.played_frames > 0 ? c.played_frames : 0;
    const long long latency = c.latency_frames > 0 ? c.latency_frames : 0;
    const double rate = static_cast<double>(c.sample_rate);

    // Content seconds the device cursor has covered, minus the audio still
    // sitting in buffers (which has been "played" from the ring's point of
    // view but is not audible yet), times the stretch rate, plus the nudge.
    const double audible = static_cast<double>(played - latency);
    const double speed = (c.speed > 0.0 && c.speed <= 4.0) ? c.speed : 1.0;
    return c.anchor_pts + (audible / rate) * speed +
           static_cast<double>(c.delay_ms) / 1000.0;
}

double av_audio_clock(double anchor_pts, long long played_frames, int sample_rate) {
    AudioClock c;
    c.anchor_pts = anchor_pts;
    c.played_frames = played_frames;
    c.sample_rate = sample_rate;
    return av_audio_clock(c);
}

double av_audio_clock(double anchor_pts, long long played_frames, int sample_rate,
                      double speed, int audio_delay_ms) {
    AudioClock c;
    c.anchor_pts = anchor_pts;
    c.played_frames = played_frames;
    c.sample_rate = sample_rate;
    c.speed = speed;
    c.delay_ms = audio_delay_ms;
    return av_audio_clock(c);
}

}  // namespace umdplay
