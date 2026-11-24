#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// One decoded picture, YUV420P, in a single allocation.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace umdplay {

struct VideoFrame {
    std::vector<uint8_t> data;          ///< Y, then Cb, then Cr
    int    width  = 0;                  ///< luma dimensions
    int    height = 0;
    size_t offset[3] = {0, 0, 0};       ///< plane starts within `data`
    int    stride[3] = {0, 0, 0};       ///< bytes per row (tight: plane width)
    bool   interlaced      = false;     ///< coded as two fields
    bool   top_field_first = true;
    double pts             = -1.0;      ///< seconds, < 0 when unknown

    int chroma_width()  const { return width / 2; }
    int chroma_height() const { return height / 2; }
    bool valid() const { return width > 0 && height > 0 && !data.empty(); }

    const uint8_t* plane(int i) const { return data.data() + offset[i]; }
    uint8_t*       plane(int i)       { return data.data() + offset[i]; }

    /// Lay out tight YUV420P planes for a w x h picture (w and h even).
    void allocate(int w, int h) {
        width  = w & ~1;
        height = h & ~1;
        const size_t luma = static_cast<size_t>(width) * height;
        const size_t chroma = static_cast<size_t>(width / 2) * (height / 2);
        data.assign(luma + 2 * chroma, 0);
        offset[0] = 0;
        offset[1] = luma;
        offset[2] = luma + chroma;
        stride[0] = width;
        stride[1] = width / 2;
        stride[2] = width / 2;
    }

    void clear() {
        data.clear();
        width = height = 0;
        offset[0] = offset[1] = offset[2] = 0;
        stride[0] = stride[1] = stride[2] = 0;
        interlaced = false;
        top_field_first = true;
        pts = -1.0;
    }
};

}  // namespace umdplay
