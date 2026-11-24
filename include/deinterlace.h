#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// In-memory deinterlacer for woven 480i frames (replaces libavfilter's yadif).

#include "video_frame.h"

namespace umdplay {

/**
 * Remove the comb pattern of a frame whose two fields were woven together by
 * blending each line with its vertical neighbours (1-2-1, in place, on all
 * three planes). Costs a little vertical detail; keeps the frame clock intact
 * (one frame in, one frame out) which the PSMF pacing depends on.
 */
void deinterlace_yuv420p(VideoFrame& frame);

}  // namespace umdplay
