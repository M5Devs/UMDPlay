#pragma once

#include <cstdint>

// SDL-free gesture recognizer so the recognition math can be unit-tested
// without a window. Fed normalised pointer samples (a finger or a mouse drag);
// it emits discrete gestures (tap / double-tap / swipe) plus continuous drag
// info. Coordinates are window pixels, time is seconds.
namespace gesture {

enum class Type {
    None,
    Tap,         // short press with little movement
    DoubleTap,   // two taps inside the double-tap window
    Swipe,       // fast, long drag released
};

enum class Axis { None, Horizontal, Vertical };

struct Event {
    Type  type = Type::None;
    float x = 0.0f, y = 0.0f;   // position (start for swipes, point for taps)
    float dx = 0.0f, dy = 0.0f; // total movement over the gesture
    float vx = 0.0f, vy = 0.0f; // release velocity (px/s) for momentum
    Axis  axis = Axis::None;    // dominant axis (swipes only)
};

struct Config {
    float tap_max_move     = 14.0f;   // px a tap may drift before it is a drag
    float tap_max_time     = 0.30f;   // s a tap may last
    float double_tap_time  = 0.34f;   // s between two taps to count as double
    float double_tap_dist  = 48.0f;   // px between two taps
    float swipe_min_dist   = 44.0f;   // px a swipe must travel
    float swipe_min_vel    = 260.0f;  // px/s a swipe must reach
    float velocity_smooth  = 0.35f;   // 0..1 exponential velocity smoothing
};

class Recognizer {
public:
    explicit Recognizer(Config cfg = {}) : m_cfg(cfg) {}

    // Begin a press at (x, y) at time t (seconds, monotonic).
    void begin(float x, float y, double t);
    // Move the active press. No-op when no press is active.
    void move(float x, float y, double t);
    // Release the press. Emits Tap (possibly deferred) or Swipe.
    // Returns the number of events written to `out` (0 or 1).
    int end(float x, float y, double t, Event* out);

    // Advance the clock. Flushes a deferred single tap once the double-tap
    // window has elapsed. Returns 1 when a Tap event was written to `out`.
    int tick(double t, Event* out);

    bool  active() const { return m_active; }
    bool  dragging() const { return m_active && m_dragging; }
    float drag_dx() const { return m_cur_x - m_start_x; }
    float drag_dy() const { return m_cur_y - m_start_y; }
    Axis  drag_axis() const { return m_axis; }

    // Abandon any in-flight press and pending tap (e.g. on focus loss).
    void reset();

private:
    void clear_press();

    Config m_cfg;

    bool  m_active   = false;
    bool  m_dragging = false;
    float m_start_x = 0.0f, m_start_y = 0.0f;
    float m_cur_x   = 0.0f, m_cur_y   = 0.0f;
    float m_last_x  = 0.0f, m_last_y  = 0.0f;
    double m_start_t = 0.0, m_last_t = 0.0;
    float m_vx = 0.0f, m_vy = 0.0f;
    Axis  m_axis = Axis::None;

    // Deferred single tap, waiting out the double-tap window.
    bool   m_tap_pending = false;
    float  m_tap_x = 0.0f, m_tap_y = 0.0f;
    double m_tap_t = 0.0;
};

}  // namespace gesture
