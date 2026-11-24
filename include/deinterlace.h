#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// In-memory deinterlacer for woven 480i frames (replaces libavfilter's yadif).

#include "video_frame.h"

namespace umdplay {

enum class DeinterlaceMode {
    Off,    ///< pass the woven frame through untouched
    Blend,  ///< 1-2-1 vertical blend everywhere: soft, never combs
    Smart,  ///< motion-adaptive: blend only where the two fields disagree
};

struct DeinterlaceParams {
    /// How much more a pixel may disagree with the opposite field (rows y+-1)
    /// than with its own (rows y+-2) before it counts as combing, on 8-bit
    /// luma. A real horizontal edge disagrees equally with both and is left
    /// alone, which is what keeps line drawings and titles sharp.
    int threshold = 24;
    /// Judge pixels on a 3x3 neighbourhood, so single-pixel noise or film
    /// grain cannot pull a sharp edge into the blended branch.
    bool neighbourhood = true;
};

/**
 * Make a woven frame display-ready in place. Smart leaves progressive content
 * (and the sharp edges of animation, which a 1-2-1 blend smears) alone and
 * only repairs the lines where the two fields actually disagree -- which is
 * exactly where combing shows. One frame in, one frame out, so the PSMF
 * pacing and the A/V clock are unaffected.
 */
void deinterlace_yuv420p(VideoFrame& frame,
                         DeinterlaceMode mode = DeinterlaceMode::Blend,
                         const DeinterlaceParams& params = {});

}  // namespace umdplay
