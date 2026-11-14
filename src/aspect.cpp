#include "aspect.h"

#include <algorithm>

namespace aspect {

float target_ratio(Mode mode, float par) {
    const float p = (par > 0.0f) ? par : 1.0f;
    switch (mode) {
        case Mode::R16x9: return 16.0f / 9.0f;
        case Mode::R4x3:  return 4.0f / 3.0f;
        default:          return -1.0f;   // Auto / Fill: caller uses the frame's
    }
    return p;   // unreachable; keeps the switch exhaustive for the compiler
}

Rect fit(Mode mode, float vw, float vh, float par, float ww, float wh) {
    Rect r;
    if (vw <= 0.0f || vh <= 0.0f || ww <= 0.0f || wh <= 0.0f) return r;

    const float p = (par > 0.0f) ? par : 1.0f;
    // Display aspect ratio of the frame as stored.
    const float frame_ar = (vw * p) / vh;
    float ar = frame_ar;
    if (mode == Mode::R16x9) ar = 16.0f / 9.0f;
    else if (mode == Mode::R4x3) ar = 4.0f / 3.0f;

    if (mode == Mode::Fill) {
        // Cover the window; overflow is cropped by the caller.
        float dw = wh * ar, dh = wh;
        if (dw < ww) { dw = ww; dh = ww / ar; }
        r.w = dw;
        r.h = dh;
    } else {
        // Fit inside the window (letterbox / pillarbox).
        float dw = wh * ar, dh = wh;
        if (dw > ww) { dw = ww; dh = ww / ar; }
        r.w = dw;
        r.h = dh;
    }
    r.x = (ww - r.w) * 0.5f;
    r.y = (wh - r.h) * 0.5f;
    return r;
}

const char* label(Mode mode) {
    switch (mode) {
        case Mode::Auto:  return "Auto (Original)";
        case Mode::R16x9: return "16:9";
        case Mode::R4x3:  return "4:3";
        case Mode::Fill:  return "Fill";
    }
    return "Auto (Original)";
}

Mode next(Mode mode) {
    switch (mode) {
        case Mode::Auto:  return Mode::R16x9;
        case Mode::R16x9: return Mode::R4x3;
        case Mode::R4x3:  return Mode::Fill;
        case Mode::Fill:  return Mode::Auto;
    }
    return Mode::Auto;
}

Mode prev(Mode mode) {
    switch (mode) {
        case Mode::Auto:  return Mode::Fill;
        case Mode::R16x9: return Mode::Auto;
        case Mode::R4x3:  return Mode::R16x9;
        case Mode::Fill:  return Mode::R4x3;
    }
    return Mode::Auto;
}

}  // namespace aspect
