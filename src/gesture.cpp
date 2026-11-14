#include "gesture.h"

#include <cmath>

namespace gesture {

namespace {

float dist(float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void Recognizer::clear_press() {
    m_active = false;
    m_dragging = false;
    m_vx = m_vy = 0.0f;
    m_axis = Axis::None;
}

void Recognizer::reset() {
    clear_press();
    m_tap_pending = false;
}

void Recognizer::begin(float x, float y, double t) {
    // A second press that starts soon after a released tap is the second half
    // of a double tap; keep the pending tap until end() decides.
    m_active   = true;
    m_dragging = false;
    m_start_x = m_cur_x = m_last_x = x;
    m_start_y = m_cur_y = m_last_y = y;
    m_start_t = m_last_t = t;
    m_vx = m_vy = 0.0f;
    m_axis = Axis::None;
}

void Recognizer::move(float x, float y, double t) {
    if (!m_active) return;

    const float dt = static_cast<float>(t - m_last_t);
    if (dt > 1e-4f) {
        const float ivx = (x - m_last_x) / dt;
        const float ivy = (y - m_last_y) / dt;
        const float a = m_cfg.velocity_smooth;
        m_vx = m_vx * (1.0f - a) + ivx * a;
        m_vy = m_vy * (1.0f - a) + ivy * a;
    }
    m_last_x = x;
    m_last_y = y;
    m_cur_x  = x;
    m_cur_y  = y;
    m_last_t = t;

    if (!m_dragging && dist(m_start_x, m_start_y, x, y) > m_cfg.tap_max_move) {
        m_dragging = true;
    }
    if (m_dragging) {
        const float dx = m_cur_x - m_start_x, dy = m_cur_y - m_start_y;
        m_axis = (std::fabs(dx) >= std::fabs(dy)) ? Axis::Horizontal : Axis::Vertical;
    }
}

int Recognizer::end(float x, float y, double t, Event* out) {
    if (!m_active) return 0;
    // Fold in the release position so a tap that barely moved still counts.
    if (x != m_last_x || y != m_last_y) move(x, y, t);

    const float dx = m_cur_x - m_start_x;
    const float dy = m_cur_y - m_start_y;
    const float travel = std::sqrt(dx * dx + dy * dy);
    const double held = t - m_start_t;

    // Capture the press state before clearing it (clear_press zeroes velocity).
    const float sx = m_start_x, sy = m_start_y;
    const float vx = m_vx, vy = m_vy;

    const bool is_swipe = travel >= m_cfg.swipe_min_dist ||
                          (std::fabs(vx) + std::fabs(vy)) >= m_cfg.swipe_min_vel;

    clear_press();

    if (is_swipe) {
        // A swipe cancels any pending tap.
        m_tap_pending = false;
        if (out) {
            out->type = Type::Swipe;
            out->x = sx;
            out->y = sy;
            out->dx = dx;
            out->dy = dy;
            out->vx = vx;
            out->vy = vy;
            out->axis = (std::fabs(dx) >= std::fabs(dy)) ? Axis::Horizontal : Axis::Vertical;
        }
        return 1;
    }

    if (held <= m_cfg.tap_max_time && travel <= m_cfg.tap_max_move) {
        // Second tap close in time and space to a pending one -> double tap.
        if (m_tap_pending &&
            (t - m_tap_t) <= m_cfg.double_tap_time &&
            dist(m_tap_x, m_tap_y, m_cur_x, m_cur_y) <= m_cfg.double_tap_dist) {
            m_tap_pending = false;
            if (out) {
                out->type = Type::DoubleTap;
                out->x = m_cur_x;
                out->y = m_cur_y;
            }
            return 1;
        }
        // Otherwise defer the single tap: it may still become a double tap.
        m_tap_pending = true;
        m_tap_x = m_cur_x;
        m_tap_y = m_cur_y;
        m_tap_t = t;
    }
    return 0;
}

int Recognizer::tick(double t, Event* out) {
    if (m_tap_pending && (t - m_tap_t) > m_cfg.double_tap_time) {
        m_tap_pending = false;
        if (out) {
            out->type = Type::Tap;
            out->x = m_tap_x;
            out->y = m_tap_y;
        }
        return 1;
    }
    return 0;
}

}  // namespace gesture
