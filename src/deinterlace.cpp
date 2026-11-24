// SPDX-License-Identifier: GPL-2.0-only

#include "deinterlace.h"

#include <cstring>
#include <vector>

namespace umdplay {
namespace {

// out[y] = (in[y-1] + 2*in[y] + in[y+1] + 2) >> 2, edges clamped. The two
// scratch rows hold the *original* previous and current line, because the
// destination aliases the current line.
void blend_plane(uint8_t* plane, int width, int height, int stride) {
    if (width <= 0 || height <= 0) return;
    if (height == 1) return;

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

}  // namespace

void deinterlace_yuv420p(VideoFrame& frame) {
    if (!frame.valid()) return;
    blend_plane(frame.plane(0), frame.width, frame.height, frame.stride[0]);
    blend_plane(frame.plane(1), frame.chroma_width(), frame.chroma_height(),
                frame.stride[1]);
    blend_plane(frame.plane(2), frame.chroma_width(), frame.chroma_height(),
                frame.stride[2]);
}

}  // namespace umdplay
