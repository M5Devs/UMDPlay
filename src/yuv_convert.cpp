// SPDX-License-Identifier: GPL-2.0-only

#include "yuv_convert.h"

namespace umdplay {
namespace {

// 16.16 fixed point BT.601 inverse matrix, 16..235 luma / 16..240 chroma.
constexpr int kYR   = 76309;   // (y - 16) scale
constexpr int kRV   = 104597;  // 1.402   * 65536
constexpr int kGU   = 25675;   // 0.344136* 65536
constexpr int kGV   = 53279;   // 0.714136* 65536
constexpr int kBU   = 132201;  // 1.772   * 65536
constexpr int kHalf = 32768;

inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

}  // namespace

void yuv420p_to_rgb24(const VideoFrame& f, uint8_t* dst, int dst_stride) {
    if (!f.valid() || !dst) return;
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* py = f.plane(0) + static_cast<size_t>(y) * f.stride[0];
        const uint8_t* pu = f.plane(1) + static_cast<size_t>(y / 2) * f.stride[1];
        const uint8_t* pv = f.plane(2) + static_cast<size_t>(y / 2) * f.stride[2];
        uint8_t* o = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < f.width; ++x) {
            const int c = (static_cast<int>(py[x]) - 16) * kYR;
            const int d = static_cast<int>(pu[x / 2]) - 128;
            const int e = static_cast<int>(pv[x / 2]) - 128;
            o[x * 3 + 0] = clamp8((c + kRV * e + kHalf) >> 16);
            o[x * 3 + 1] = clamp8((c - kGU * d - kGV * e + kHalf) >> 16);
            o[x * 3 + 2] = clamp8((c + kBU * d + kHalf) >> 16);
        }
    }
}

void yuv420p_to_rgba32(const VideoFrame& f, uint8_t* dst, int dst_stride, bool bgra) {
    if (!f.valid() || !dst) return;
    const int ri = bgra ? 2 : 0;
    const int bi = bgra ? 0 : 2;
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* py = f.plane(0) + static_cast<size_t>(y) * f.stride[0];
        const uint8_t* pu = f.plane(1) + static_cast<size_t>(y / 2) * f.stride[1];
        const uint8_t* pv = f.plane(2) + static_cast<size_t>(y / 2) * f.stride[2];
        uint8_t* o = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < f.width; ++x) {
            const int c = (static_cast<int>(py[x]) - 16) * kYR;
            const int d = static_cast<int>(pu[x / 2]) - 128;
            const int e = static_cast<int>(pv[x / 2]) - 128;
            uint8_t* p = o + x * 4;
            p[ri] = clamp8((c + kRV * e + kHalf) >> 16);
            p[1]  = clamp8((c - kGU * d - kGV * e + kHalf) >> 16);
            p[bi] = clamp8((c + kBU * d + kHalf) >> 16);
            p[3]  = 255;
        }
    }
}

}  // namespace umdplay
