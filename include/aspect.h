#pragma once

// Video scaling / zoom math, kept free of SDL and ImGui so the destination
// rectangle for each WMC "Zoom" mode can be unit-tested directly.

namespace aspect {

enum class Mode {
    Auto,   // fit the whole frame, preserving its pixel aspect (letterbox)
    R16x9,  // force a 16:9 frame (stretch/crop to fill the screen's height)
    R4x3,   // force a 4:3 frame
    Fill,   // scale to cover the whole window (crop overflow)
};

struct Rect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };

// Aspect ratio (w/h) the mode asks for, or <= 0 for "use the frame's own".
// `par` is the frame's pixel aspect ratio (sample_aspect_ratio; <=0 means 1:1).
float target_ratio(Mode mode, float par);

// Destination rectangle (window pixels) for a `vw` x `vh` frame in a `ww` x
// `wh` window. Auto/16:9/4:3 fit inside (letterbox/pillarbox); Fill covers and
// overflows so the caller's clip rect crops it. `par` is the frame PAR.
Rect fit(Mode mode, float vw, float vh, float par, float ww, float wh);

// Human-readable label for the OSD ("Auto (Original)", "16:9", ...).
const char* label(Mode mode);

// Cycle Auto -> 16:9 -> 4:3 -> Fill -> Auto.
Mode next(Mode mode);
Mode prev(Mode mode);

}  // namespace aspect
