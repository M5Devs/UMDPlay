#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// Minimal YUV420P -> packed RGB conversion (replaces libswscale).

#include "video_frame.h"

#include <cstdint>

namespace umdplay {

/// BT.601 limited-range YUV420P to packed RGB24 (`dst_stride` >= width * 3).
void yuv420p_to_rgb24(const VideoFrame& frame, uint8_t* dst, int dst_stride);

/// Same, but 32-bit RGBA (or BGRA) with an opaque alpha byte.
void yuv420p_to_rgba32(const VideoFrame& frame, uint8_t* dst, int dst_stride,
                       bool bgra);

}  // namespace umdplay
