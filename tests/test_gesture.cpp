// Unit tests for the SDL-free gesture recognizer and the aspect/zoom math.
// These are pure functions, so the whole suite runs headless with no window.
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).
#include "gesture.h"
#include "aspect.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__,         \
                        __LINE__);                                             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

void test_tap() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(100, 100, 0.0);
    CHECK(r.active());
    r.move(101, 101, 0.05);          // tiny drift stays a tap
    CHECK(!r.dragging());
    int n = r.end(101, 101, 0.08, &e);
    CHECK(n == 0);                   // tap is deferred, not emitted yet
    CHECK(r.tick(0.10, &e) == 0);    // still inside the double-tap window
    n = r.tick(0.60, &e);            // after the window the single tap flushes
    CHECK(n == 1 && e.type == gesture::Type::Tap);
    CHECK(near(e.x, 101) && near(e.y, 101));
    CHECK(r.tick(0.90, &e) == 0);    // no re-emit
}

void test_double_tap() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(200, 300, 0.0);
    CHECK(r.end(200, 300, 0.05, &e) == 0);    // first tap deferred
    r.begin(202, 301, 0.12);                  // second tap within the window
    int n = r.end(202, 301, 0.16, &e);
    CHECK(n == 1 && e.type == gesture::Type::DoubleTap);
    CHECK(near(e.x, 202) && near(e.y, 301));
    CHECK(r.tick(1.0, &e) == 0);              // pending single tap must not fire
}

void test_double_tap_too_slow() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(10, 10, 0.0);
    r.end(10, 10, 0.03, &e);
    r.begin(10, 10, 1.0);            // far apart in time -> two singles
    CHECK(r.end(10, 10, 1.03, &e) == 0);
    CHECK(r.tick(1.6, &e) == 1 && e.type == gesture::Type::Tap);
}

void test_swipe_horizontal() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(600, 360, 0.0);
    for (int i = 1; i <= 10; ++i) r.move(600.0f - i * 40.0f, 360, i * 0.016);
    CHECK(r.dragging());
    CHECK(r.drag_axis() == gesture::Axis::Horizontal);
    int n = r.end(200, 360, 0.16, &e);
    CHECK(n == 1 && e.type == gesture::Type::Swipe);
    CHECK(e.axis == gesture::Axis::Horizontal);
    CHECK(e.dx < 0.0f);              // moved left
    CHECK(e.vx < 0.0f);              // and moving left at release
    CHECK(r.tick(1.0, &e) == 0);     // a swipe leaves no pending tap
}

void test_swipe_vertical() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(900, 200, 0.0);
    for (int i = 1; i <= 8; ++i) r.move(900, 200.0f + i * 30.0f, i * 0.016);
    int n = r.end(900, 440, 0.13, &e);
    CHECK(n == 1 && e.type == gesture::Type::Swipe);
    CHECK(e.axis == gesture::Axis::Vertical);
    CHECK(e.dy > 0.0f);
}

void test_slow_short_nudge_is_neither() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(100, 100, 0.0);
    r.move(120, 100, 0.30);
    r.move(130, 100, 0.60);          // 30px over 0.6s -> slow and short
    CHECK(r.end(130, 100, 0.62, &e) == 0);   // not a swipe
    CHECK(r.tick(2.0, &e) == 0);             // and too long to be a tap
}

void test_reset_cancels() {
    gesture::Recognizer r;
    gesture::Event e;
    r.begin(5, 5, 0.0);
    r.end(5, 5, 0.02, &e);           // pending tap
    r.reset();
    CHECK(!r.active());
    CHECK(r.tick(1.0, &e) == 0);     // pending tap cleared
}

void test_aspect_modes() {
    // 720x480 with PAR 1.0 is a 3:2 frame; Auto fits by the frame's own aspect.
    {
        aspect::Rect r = aspect::fit(aspect::Mode::Auto, 720, 480, 1.0f, 1280, 720);
        CHECK(near(r.h, 720.0f));                 // full height
        CHECK(near(r.w, 1080.0f));                // 720 * 1.5 (3:2)
        CHECK(near(r.x, (1280.0f - r.w) * 0.5f));
        CHECK(near(r.y, 0.0f));
    }
    // Same source but forced to 4:3 -> pillarboxed to 960px wide.
    {
        aspect::Rect r = aspect::fit(aspect::Mode::R4x3, 720, 480, 1.0f, 1280, 720);
        CHECK(near(r.h, 720.0f));
        CHECK(near(r.w, 960.0f));
        CHECK(near(r.x, 160.0f));
    }
    // Forced 16:9 in a 16:9 window fills it exactly.
    {
        aspect::Rect r = aspect::fit(aspect::Mode::R16x9, 720, 480, 1.0f, 1280, 720);
        CHECK(near(r.w, 1280.0f) && near(r.h, 720.0f));
        CHECK(near(r.x, 0.0f) && near(r.y, 0.0f));
    }
    {
        aspect::Rect r = aspect::fit(aspect::Mode::Fill, 720, 480, 1.0f, 1280, 720);
        CHECK(r.w >= 1280.0f - 0.5f && r.h >= 720.0f - 0.5f);   // covers
    }
}

void test_aspect_anamorphic_par() {
    // Anamorphic 720x480 with PAR 32:27 -> ~1.78 display aspect.
    const float par = 32.0f / 27.0f;
    aspect::Rect r = aspect::fit(aspect::Mode::Auto, 720, 480, par, 1280, 720);
    const float dar = r.w / r.h;
    CHECK(near(dar, 16.0f / 9.0f, 0.02f));
}

void test_aspect_cycle_and_labels() {
    using aspect::Mode;
    CHECK(aspect::next(Mode::Auto) == Mode::R16x9);
    CHECK(aspect::next(Mode::R16x9) == Mode::R4x3);
    CHECK(aspect::next(Mode::R4x3) == Mode::Fill);
    CHECK(aspect::next(Mode::Fill) == Mode::Auto);
    CHECK(aspect::prev(Mode::Auto) == Mode::Fill);
    CHECK(aspect::prev(Mode::Fill) == Mode::R4x3);
    CHECK(std::string(aspect::label(Mode::Auto)) == "Auto (Original)");
    CHECK(std::string(aspect::label(Mode::R16x9)) == "16:9");
    CHECK(std::string(aspect::label(Mode::R4x3)) == "4:3");
    CHECK(std::string(aspect::label(Mode::Fill)) == "Fill");
}

void test_aspect_degenerate() {
    aspect::Rect r = aspect::fit(aspect::Mode::Auto, 0, 0, 1.0f, 1280, 720);
    CHECK(near(r.w, 0.0f) && near(r.h, 0.0f));
}

}  // namespace

int main() {
    test_tap();
    test_double_tap();
    test_double_tap_too_slow();
    test_swipe_horizontal();
    test_swipe_vertical();
    test_slow_short_nudge_is_neither();
    test_reset_cancels();
    test_aspect_modes();
    test_aspect_anamorphic_par();
    test_aspect_cycle_and_labels();
    test_aspect_degenerate();

    if (g_failures) {
        std::printf("[test_gesture] %d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("[test_gesture] all gesture + aspect assertions passed\n");
    return 0;
}
