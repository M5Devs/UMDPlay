#pragma once

// Video scaling / zoom math, kept free of SDL, ImGui and Qt so the destination
// rectangle for each "Zoom" mode can be unit-tested directly, and so both
// front ends (the WMC shell and AveraUI) share one implementation.
//
// Header-only on purpose: AveraUI links no UMDPlay core library, and this is
// pure geometry that both need.

namespace aspect {

enum class Mode {
    Auto,    // fit the whole frame, preserving its pixel aspect (letterbox)
    R16x9,   // force a 16:9 frame (letterbox/crop to the window's height)
    R4x3,    // force a 4:3 frame
    R3x2,    // force a 3:2 frame (what 720x480 looks like with square pixels)
    Fill,    // scale to cover the whole window (crop overflow)
    Stretch, // fill the window exactly, ignoring both aspects (distort)
};

/// Presentation pixel aspect for a decoded picture.
///
/// Sony UMD Video is 720x480 H.264 whose stream usually carries no useful SAR
/// (square/unspecified), yet it is authored for 4:3 television. Treating 1:1
/// as truth stretches everyone 33% wide, so a square-flagged NTSC frame
/// defaults to the 4:3 PAR (8/9). A stream that does signal an anamorphic SAR
/// (16:9 discs) keeps it.
inline float display_par(int width, int height, float stream_par) {
    const bool flagged = stream_par > 0.0f && (stream_par > 1.02f || stream_par < 0.98f);
    if (flagged) return stream_par;
    if (height == 480 && (width == 720 || width == 704)) return 8.0f / 9.0f;
    return 1.0f;
}

struct Rect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };

// Aspect ratio (w/h) the mode asks for, or < 0 for "use the frame's own".
// `par` is the frame's pixel aspect ratio (sample_aspect_ratio; <=0 means 1:1).
inline float target_ratio(Mode mode, float par) {
    const float p = (par > 0.0f) ? par : 1.0f;
    switch (mode) {
        case Mode::R16x9: return 16.0f / 9.0f;
        case Mode::R4x3:  return 4.0f / 3.0f;
        case Mode::R3x2:  return 3.0f / 2.0f;
        default:          return -1.0f;   // Auto / Fill / Stretch: caller decides
    }
    return p;   // unreachable; keeps the switch exhaustive for the compiler
}

// Destination rectangle (window pixels) for a `vw` x `vh` frame in a `ww` x
// `wh` window. Auto/16:9/4:3 fit inside (letterbox/pillarbox); Fill covers and
// overflows so the caller's clip rect crops it; Stretch matches the window
// exactly. `par` is the frame PAR.
inline Rect fit(Mode mode, float vw, float vh, float par, float ww, float wh) {
    Rect r;
    if (vw <= 0.0f || vh <= 0.0f || ww <= 0.0f || wh <= 0.0f) return r;

    if (mode == Mode::Stretch) {
        r.w = ww;
        r.h = wh;
        return r;
    }

    const float p = (par > 0.0f) ? par : 1.0f;
    // Display aspect ratio of the frame as stored.
    const float frame_ar = (vw * p) / vh;
    float ar = frame_ar;
    if (mode == Mode::R16x9) ar = 16.0f / 9.0f;
    else if (mode == Mode::R4x3) ar = 4.0f / 3.0f;
    else if (mode == Mode::R3x2) ar = 3.0f / 2.0f;

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

// Human-readable label for the OSD ("Auto (4:3)", "16:9", ...).
inline const char* label(Mode mode) {
    switch (mode) {
        case Mode::Auto:    return "Auto (4:3)";
        case Mode::R16x9:   return "16:9";
        case Mode::R4x3:    return "4:3";
        case Mode::R3x2:    return "3:2";
        case Mode::Fill:    return "Fill";
        case Mode::Stretch: return "Stretch";
    }
    return "Auto (Original)";
}

// Short stable identifier, for settings files and QML property values.
inline const char* name(Mode mode) {
    switch (mode) {
        case Mode::Auto:    return "auto";
        case Mode::R16x9:   return "16:9";
        case Mode::R4x3:    return "4:3";
        case Mode::R3x2:    return "3:2";
        case Mode::Fill:    return "fill";
        case Mode::Stretch: return "stretch";
    }
    return "auto";
}

// Parse a name() value (or the shell's spellings). Unknown -> Auto.
inline Mode mode_from_name(const char* n) {
    if (!n) return Mode::Auto;
    if (n[0] == '4') return Mode::R4x3;
    if (n[0] == '3') return Mode::R3x2;
    if (n[0] == '1' && n[1] == '6') return Mode::R16x9;
    if (n[0] == 'f' || n[0] == 'F') return Mode::Fill;
    if (n[0] == 's' || n[0] == 'S') return Mode::Stretch;
    return Mode::Auto;
}

// Cycle Auto -> 16:9 -> 4:3 -> Fill -> Stretch -> Auto (the WMC settings strip,
// whose help text documents Auto -> 16:9 -> 4:3 -> Fill).
inline Mode next(Mode mode) {
    switch (mode) {
        case Mode::Auto:    return Mode::R16x9;
        case Mode::R16x9:   return Mode::R4x3;
        case Mode::R4x3:    return Mode::R3x2;
        case Mode::R3x2:    return Mode::Fill;
        case Mode::Fill:    return Mode::Stretch;
        case Mode::Stretch: return Mode::Auto;
    }
    return Mode::Auto;
}

inline Mode prev(Mode mode) {
    switch (mode) {
        case Mode::Auto:    return Mode::Stretch;
        case Mode::R16x9:   return Mode::Auto;
        case Mode::R4x3:    return Mode::R16x9;
        case Mode::R3x2:    return Mode::R4x3;
        case Mode::Fill:    return Mode::R3x2;
        case Mode::Stretch: return Mode::Fill;
    }
    return Mode::Auto;
}

// The AveraUI player's own order, matching its on-screen list (MPC-HC's
// "Pan&Scan / 4:3 / 16:9 / 3:2 / stretch"):
// Auto (4:3) -> 16:9 -> 3:2 -> Fill -> Stretch -> Auto.
inline Mode cycle_player(Mode mode) {
    switch (mode) {
        case Mode::Auto:    return Mode::R16x9;
        case Mode::R16x9:   return Mode::R3x2;
        case Mode::R3x2:    return Mode::Fill;
        case Mode::Fill:    return Mode::Stretch;
        case Mode::Stretch: return Mode::Auto;
        case Mode::R4x3:    return Mode::R16x9;   // 4:3 is already Auto's default
    }
    return Mode::Auto;
}

}  // namespace aspect
