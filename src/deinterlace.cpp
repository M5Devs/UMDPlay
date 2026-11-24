// SPDX-License-Identifier: GPL-2.0-only

#include "deinterlace.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace umdplay {
namespace {

/// Width of the blend ramp above the threshold, in comb units. A hard switch
/// leaves visible seams along a gradient; this fades in over a few levels.
constexpr int kRamp = 24;

/// 1-2-1 vertical blend uniform across the picture.
void blend_plane(uint8_t* plane, int width, int height, int stride) {
    if (width <= 0 || height <= 0 || height == 1) return;

    const size_t n = static_cast<size_t>(width);
    std::vector<uint8_t> prev(n), cur(n);
    std::memcpy(prev.data(), plane, n);  // row -1 clamps to row 0

    for (int y = 0; y < height; ++y) {
        uint8_t* dst = plane + static_cast<size_t>(y) * stride;
        std::memcpy(cur.data(), dst, n);
        const uint8_t* next = (y + 1 < height)
                                  ? plane + static_cast<size_t>(y + 1) * stride
                                  : cur.data();
        const uint8_t* a = prev.data();
        const uint8_t* b = cur.data();
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<uint8_t>((a[x] + 2 * b[x] + next[x] + 2) >> 2);
        }
        prev.swap(cur);
    }
}

/// Motion-adaptive blend. A pixel is only repaired when it disagrees with the
/// *other* field far more than with its own: compare it against the mean of
/// rows y+-1 (the opposite field) and of rows y+-2 (its own field). A woven
/// moving edge fails the first and passes the second; a real horizontal edge
/// -- a cartoon line, a title -- fails both equally, so it stays sharp.
void blend_plane_adaptive(uint8_t* plane, int width, int height, int stride,
                          const DeinterlaceParams& params) {
    if (width <= 0 || height <= 0 || height < 3) return;

    const size_t n = static_cast<size_t>(width);
    const int thr = std::max(0, params.threshold);
    std::vector<uint8_t> prev2(n), prev1(n), cur(n), next1(n), next2(n), comb(n), out(n);

    auto load = [&](std::vector<uint8_t>& dst, int y) {
        const int clamped = std::min(std::max(y, 0), height - 1);
        std::memcpy(dst.data(), plane + static_cast<size_t>(clamped) * stride, n);
    };
    load(prev2, -2);
    load(prev1, -1);

    for (int y = 0; y < height; ++y) {
        load(cur, y);
        load(next1, y + 1);
        load(next2, y + 2);

        for (int x = 0; x < width; ++x) {
            const int opposite = std::abs(int(cur[x]) - ((int(prev1[x]) + int(next1[x]) + 1) >> 1));
            const int own = std::abs(int(cur[x]) - ((int(prev2[x]) + int(next2[x]) + 1) >> 1));
            comb[x] = static_cast<uint8_t>(std::min(255, std::max(0, opposite - own)));
        }

        uint8_t* dst = plane + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            int c = comb[x];
            if (params.neighbourhood) {
                const int lo = std::max(0, x - 1);
                const int hi = std::min(width - 1, x + 1);
                c = std::max({comb[x], comb[lo], comb[hi]});
            }
            const int blended =
                (prev1[x] + 2 * cur[x] + next1[x] + 2) >> 2;
            int w = 0;
            if (c > thr) w = (c >= thr + kRamp) ? 256 : ((c - thr) << 8) / kRamp;
            out[x] = static_cast<uint8_t>((cur[x] * (256 - w) + blended * w + 128) >> 8);
        }
        std::memcpy(dst, out.data(), n);

        std::swap(prev2, prev1);
        std::swap(prev1, cur);
        std::swap(cur, next1);
        std::swap(next1, next2);
    }
}

void apply(uint8_t* plane, int width, int height, int stride, DeinterlaceMode mode,
           const DeinterlaceParams& params) {
    if (mode == DeinterlaceMode::Blend) blend_plane(plane, width, height, stride);
    else if (mode == DeinterlaceMode::Smart)
        blend_plane_adaptive(plane, width, height, stride, params);
}

}  // namespace

void deinterlace_yuv420p(VideoFrame& frame, DeinterlaceMode mode,
                         const DeinterlaceParams& params) {
    if (!frame.valid() || mode == DeinterlaceMode::Off) return;
    apply(frame.plane(0), frame.width, frame.height, frame.stride[0], mode, params);
    apply(frame.plane(1), frame.chroma_width(), frame.chroma_height(), frame.stride[1],
          mode, params);
    apply(frame.plane(2), frame.chroma_width(), frame.chroma_height(), frame.stride[2],
          mode, params);
}

}  // namespace umdplay
