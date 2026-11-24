// SPDX-License-Identifier: GPL-2.0-only

#include "av_sync.h"

namespace umdplay {

AvAction av_decide(double video_pts, double audio_clock, const AvSyncParams& params) {
    if (video_pts < 0.0 || audio_clock < 0.0) return AvAction::Present;
    if (video_pts > audio_clock + params.early) return AvAction::Hold;
    if (video_pts < audio_clock - params.late) return AvAction::Drop;
    return AvAction::Present;
}

double av_audio_clock(double anchor_pts, long long played_frames, int sample_rate) {
    if (anchor_pts < 0.0 || sample_rate <= 0) return -1.0;
    if (played_frames < 0) played_frames = 0;
    return anchor_pts + static_cast<double>(played_frames) / static_cast<double>(sample_rate);
}

}  // namespace umdplay
