// Unit tests for the live playback tweaks: speed ladder, audio-sync nudge,
// A-B loop transitions, the latency-corrected audio clock, the pitch-preserving
// time stretcher, and the 4:3 default for square-flagged 720x480 video.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aspect.h"
#include "av_sync.h"
#include "playback_tuning.h"
#include "time_stretch.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("[-] FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

using umdplay::AudioClock;

// --- speed -----------------------------------------------------------------

void test_speed_ladder() {
    using umdplay::clamp_speed;
    using umdplay::kMaxSpeed;
    using umdplay::kMinSpeed;
    using umdplay::speed_down;
    using umdplay::speed_up;

    CHECK(near(clamp_speed(1.0), 1.0));
    CHECK(near(clamp_speed(0.1), kMinSpeed));
    CHECK(near(clamp_speed(9.0), kMaxSpeed));
    CHECK(near(clamp_speed(std::nan("")), 1.0));

    // Walking up crosses every advertised step and saturates at 2x.
    double s = kMinSpeed;
    const double expected[] = {0.75, 1.0, 1.25, 1.5, 2.0, 2.0};
    for (double want : expected) {
        s = speed_up(s);
        CHECK(near(s, want));
    }
    // ...and back down to 0.5x.
    const double back[] = {1.5, 1.25, 1.0, 0.75, 0.5, 0.5};
    for (double want : back) {
        s = speed_down(s);
        CHECK(near(s, want));
    }

    // An off-ladder rate steps to the next listed one in each direction.
    CHECK(near(speed_up(1.1), 1.25));
    CHECK(near(speed_down(1.1), 1.0));

    char buf[16];
    CHECK(std::string(umdplay::speed_label(1.0, buf, sizeof(buf))) == "1.0x");
    CHECK(std::string(umdplay::speed_label(0.75, buf, sizeof(buf))) == "0.75x");
    CHECK(std::string(umdplay::speed_label(1.5, buf, sizeof(buf))) == "1.5x");
    CHECK(std::string(umdplay::speed_label(2.0, buf, sizeof(buf))) == "2.0x");
}

// --- audio delay -----------------------------------------------------------

void test_audio_delay() {
    using umdplay::clamp_audio_delay_ms;
    using umdplay::step_audio_delay_ms;

    CHECK(clamp_audio_delay_ms(0) == 0);
    CHECK(clamp_audio_delay_ms(50) == 50);
    CHECK(clamp_audio_delay_ms(9999) == 2000);
    CHECK(clamp_audio_delay_ms(-9999) == -2000);
    // Snapped to the 50 ms grid, half a step rounds away from zero.
    CHECK(clamp_audio_delay_ms(74) == 50);
    CHECK(clamp_audio_delay_ms(76) == 100);
    CHECK(clamp_audio_delay_ms(-76) == -100);

    CHECK(step_audio_delay_ms(0, 1) == 50);
    CHECK(step_audio_delay_ms(0, -1) == -50);
    CHECK(step_audio_delay_ms(1950, 1) == 2000);
    CHECK(step_audio_delay_ms(2000, 1) == 2000);      // saturates
    CHECK(step_audio_delay_ms(-2000, -1) == -2000);
    CHECK(step_audio_delay_ms(33, 0) == 50);          // direction 0 just clamps

    char buf[48];
    CHECK(std::string(umdplay::audio_delay_label(150, buf, sizeof(buf),
                                                 "Audio Delay: ")) ==
          "Audio Delay: +150 ms");
    CHECK(std::string(umdplay::audio_delay_label(0, buf, sizeof(buf))) == "+0 ms");
    CHECK(std::string(umdplay::audio_delay_label(-50, buf, sizeof(buf))) == "-50 ms");
}

// --- A-B loop --------------------------------------------------------------

void test_ab_loop() {
    using umdplay::AbLoop;
    using umdplay::AbLoopState;

    AbLoop loop;
    CHECK(loop.state == AbLoopState::Off);
    CHECK(!loop.active());

    // Off -> Set A.
    loop.press(12.0);
    CHECK(loop.state == AbLoopState::SetA);
    CHECK(near(loop.a, 12.0));
    CHECK(!loop.active());

    // B too close to A is rejected: the machine stays on Set A.
    loop.press(12.2);
    CHECK(loop.state == AbLoopState::SetA);
    // ...as is a B before A.
    loop.press(5.0);
    CHECK(loop.state == AbLoopState::SetA);

    // A real B completes the loop.
    loop.press(41.0);
    CHECK(loop.state == AbLoopState::Looping);
    CHECK(loop.active());
    CHECK(near(loop.span(), 29.0));
    CHECK(!loop.expired(40.9));
    CHECK(loop.expired(41.0));
    CHECK(loop.expired(41.5));       // the caller seeks back to A

    // A press while looping clears both points.
    loop.press(20.0);
    CHECK(loop.state == AbLoopState::Off);
    CHECK(near(loop.a, 0.0) && near(loop.b, 0.0));
    CHECK(!loop.active());

    // Nonsense positions are ignored rather than poisoning the state.
    AbLoop odd;
    odd.press(-1.0);
    odd.press(std::nan(""));
    CHECK(odd.state == AbLoopState::Off);

    char buf[64];
    CHECK(loop.label(buf, sizeof(buf)) == std::string(""));
    loop.press(65.0);
    CHECK(loop.label(buf, sizeof(buf)) == std::string("A-B: A set"));
    loop.press(95.0);
    CHECK(loop.label(buf, sizeof(buf)) == std::string("A-B 1:05 \xe2\x86\x92 1:35"));
}

// --- audio clock -----------------------------------------------------------

void test_audio_clock() {
    // Plain: 1 s of played audio at 44.1 kHz starts 10 s in.
    CHECK(near(umdplay::av_audio_clock(10.0, 44100, 44100), 11.0));
    // Half a second.
    CHECK(near(umdplay::av_audio_clock(10.0, 22050, 44100), 10.5));
    // Unanchored / bogus rate -> unknown clock.
    CHECK(umdplay::av_audio_clock(-1.0, 44100, 44100) < 0.0);
    CHECK(umdplay::av_audio_clock(10.0, 44100, 0) < 0.0);
    // Negative played frames cannot rewind the clock.
    CHECK(near(umdplay::av_audio_clock(10.0, -44100, 44100), 10.0));

    // Latency: 441 frames (10 ms) still buffered are not audible yet.
    AudioClock c;
    c.anchor_pts = 10.0;
    c.played_frames = 44100;
    c.latency_frames = 441;               // 10 ms
    CHECK(near(umdplay::av_audio_clock(c), 11.0 - 0.010));

    // A bigger device buffer pushes the clock further behind.
    c.latency_frames = 4410;              // 100 ms
    CHECK(near(umdplay::av_audio_clock(c), 10.9));

    // Latency larger than what has played (just after a seek) is allowed and
    // still describes the speaker: the picture waits instead of jumping.
    c.played_frames = 100;
    c.latency_frames = 5000;
    CHECK(near(umdplay::av_audio_clock(c), 10.0 + (100.0 - 5000.0) / 44100.0));

    // Speed: output frames carry `speed` times the content.
    c.played_frames = 44100;
    c.latency_frames = 0;
    c.speed = 2.0;
    CHECK(near(umdplay::av_audio_clock(c), 12.0));   // 1 s out = 2 s content
    c.speed = 0.5;
    CHECK(near(umdplay::av_audio_clock(c), 10.5));

    // Sync nudge: positive = the sound is late, so the clock reads ahead.
    c.speed = 1.0;
    c.delay_ms = 150;
    CHECK(near(umdplay::av_audio_clock(c), 11.15));
    c.delay_ms = -150;
    CHECK(near(umdplay::av_audio_clock(c), 10.85));

    // All of it at once: speed, nudge and latency compose.
    c.speed = 1.5;
    c.delay_ms = 100;
    c.latency_frames = 882;              // 20 ms behind
    const double want = 10.0 + (44100.0 - 882.0) / 44100.0 * 1.5 + 0.1;
    CHECK(near(umdplay::av_audio_clock(c), want, 1e-9));

    // av_decide still lands on the right side of the windows.
    CHECK(umdplay::av_decide(10.05, 10.0) == umdplay::AvAction::Hold);
    CHECK(umdplay::av_decide(9.99, 10.0) == umdplay::AvAction::Present);
    CHECK(umdplay::av_decide(9.5, 10.0) == umdplay::AvAction::Drop);
}

// --- time stretcher --------------------------------------------------------

std::vector<int16_t> sine(double hz, double seconds, double amp = 12000.0) {
    const int frames = int(44100 * seconds);
    std::vector<int16_t> out(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double v = amp * std::sin(2.0 * M_PI * hz * i / 44100.0);
        out[size_t(i) * 2] = int16_t(std::lround(v));
        out[size_t(i) * 2 + 1] = int16_t(std::lround(v));
    }
    return out;
}

double rms_mid(const std::vector<int16_t>& pcm) {
    const size_t frames = pcm.size() / 2;
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = frames / 4; i < frames * 3 / 4; ++i) {
        const double v = pcm[i * 2];
        acc += v * v;
        ++n;
    }
    return n ? std::sqrt(acc / double(n)) : 0.0;
}

double estimate_hz(const std::vector<int16_t>& pcm) {
    const size_t frames = pcm.size() / 2;
    size_t crossings = 0;
    int prev = 0;
    size_t first = frames / 4, last = frames * 3 / 4;
    for (size_t i = first; i < last; ++i) {
        const int v = pcm[i * 2];
        if (prev < 0 && v >= 0) ++crossings;
        prev = v;
    }
    const double seconds = double(last - first) / 44100.0;
    return crossings / seconds;
}

void test_time_stretch() {
    umdplay::TimeStretcher st;
    const auto input = sine(440.0, 1.0);

    // 1.0x is a bit-exact passthrough.
    {
        st.set_speed(1.0);
        std::vector<int16_t> out;
        st.process(input.data(), input.size() / 2, out);
        CHECK(out.size() == input.size());
        CHECK(std::memcmp(out.data(), input.data(), input.size() * sizeof(int16_t)) == 0);
    }

    // 2.0x halves the duration but keeps the pitch (that is the whole point:
    // naive decimation would double it to 880 Hz).
    {
        st.set_speed(2.0);
        std::vector<int16_t> out;
        st.process(input.data(), input.size() / 2, out);
        st.flush(out);
        const double frames = double(out.size() / 2);
        CHECK(std::fabs(frames - 22050.0) < 2.0 * umdplay::TimeStretcher::kGrain);
        const double hz = estimate_hz(out);
        CHECK(std::fabs(hz - 440.0) < 440.0 * 0.06);
        // Triangular windows sum to one: the level is preserved, not halved.
        const double before = rms_mid(input);
        const double after = rms_mid(out);
        CHECK(after > before * 0.75 && after < before * 1.30);
    }

    // 0.5x doubles it, same pitch.
    {
        st.set_speed(0.5);
        std::vector<int16_t> out;
        st.process(input.data(), input.size() / 2, out);
        st.flush(out);
        const double frames = double(out.size() / 2);
        CHECK(std::fabs(frames - 88200.0) < 2.0 * umdplay::TimeStretcher::kGrain);
        const double hz = estimate_hz(out);
        CHECK(std::fabs(hz - 440.0) < 440.0 * 0.06);
    }

    // Small chunks (as the decoder delivers them) produce the same length as a
    // single big one: the state machine carries across calls.
    {
        st.set_speed(1.5);
        std::vector<int16_t> out;
        const int chunk = 1024;
        for (size_t at = 0; at + size_t(chunk) <= input.size() / 2; at += chunk)
            st.process(input.data() + at * 2, chunk, out);
        st.flush(out);
        const double frames = double(out.size() / 2);
        CHECK(std::fabs(frames - input.size() / 2 / 1.5) <
              3.0 * umdplay::TimeStretcher::kGrain);

        // No wild sample-to-sample jumps: an artefact-free crossfade stays at
        // the source's own slope for a 440 Hz tone. The first and last frames
        // are the deliberate fades, so measure the steady middle.
        int max_step = 0;
        for (size_t i = 2000; i + 2000 < out.size() / 2; ++i)
            max_step = std::max(max_step, std::abs(int(out[i * 2]) - int(out[(i - 1) * 2])));
        const int source_slope = int(12000.0 * 2.0 * M_PI * 440.0 / 44100.0) + 64;
        CHECK(max_step < source_slope * 2);
    }

    // Speed out of range is clamped, and silence/reset stay safe.
    {
        st.set_speed(99.0);
        CHECK(st.speed() == 2.0);
        st.set_speed(0.0);
        CHECK(st.speed() == 0.5);
        std::vector<int16_t> out;
        st.process(nullptr, 0, out);
        st.flush(out);
        st.reset();
    }
}

// --- aspect ----------------------------------------------------------------

void test_aspect_43_default() {
    using aspect::display_par;

    // Sony UMD Video: 720x480 with a square/unspecified SAR is authored for
    // 4:3, so it must not be stretched to 3:2.
    CHECK(near(display_par(720, 480, 1.0f), 8.0 / 9.0, 1e-6));
    CHECK(near(display_par(720, 480, 0.0f), 8.0 / 9.0, 1e-6));
    CHECK(near(display_par(704, 480, 1.0f), 8.0 / 9.0, 1e-6));
    // Anamorphic 16:9 discs keep the SAR the stream actually flags.
    CHECK(near(display_par(720, 480, 32.0f / 27.0f), 32.0 / 27.0, 1e-6));
    CHECK(near(display_par(720, 480, 40.0f / 33.0f), 40.0 / 33.0, 1e-6));
    // Other geometries are left alone.
    CHECK(near(display_par(1280, 720, 1.0f), 1.0, 1e-6));
    CHECK(near(display_par(1920, 1080, 0.0f), 1.0, 1e-6));

    // And the result really is 4:3 on screen at 1280x720 (pillarboxed 960x720).
    const aspect::Rect r = aspect::fit(aspect::Mode::Auto, 720, 480,
                                       display_par(720, 480, 1.0f), 1280, 720);
    CHECK(near(r.w, 960.0f, 0.5f));
    CHECK(near(r.h, 720.0f, 0.5f));
    CHECK(near(r.x, 160.0f, 0.5f));

    // Forcing 3:2 is what the old behaviour looked like, and it still exists.
    const aspect::Rect three_two = aspect::fit(aspect::Mode::R3x2, 720, 480, 8.0f / 9.0f,
                                               1280, 720);
    CHECK(near(three_two.w, 1080.0f, 0.5f));
    CHECK(near(three_two.x, 100.0f, 0.5f));

    // Stretch matches the window exactly.
    const aspect::Rect stretch = aspect::fit(aspect::Mode::Stretch, 720, 480, 1.0f,
                                             1280, 720);
    CHECK(near(stretch.w, 1280.0f) && near(stretch.h, 720.0f));

    // The player's cycle: Auto (4:3) -> 16:9 -> 3:2 -> Fill -> Stretch -> Auto.
    using aspect::Mode;
    CHECK(aspect::cycle_player(Mode::Auto) == Mode::R16x9);
    CHECK(aspect::cycle_player(Mode::R16x9) == Mode::R3x2);
    CHECK(aspect::cycle_player(Mode::R3x2) == Mode::Fill);
    CHECK(aspect::cycle_player(Mode::Fill) == Mode::Stretch);
    CHECK(aspect::cycle_player(Mode::Stretch) == Mode::Auto);

    // Names round-trip through the QML-facing strings.
    CHECK(aspect::mode_from_name("4:3") == Mode::R4x3);
    CHECK(aspect::mode_from_name("3:2") == Mode::R3x2);
    CHECK(aspect::mode_from_name("16:9") == Mode::R16x9);
    CHECK(aspect::mode_from_name("fill") == Mode::Fill);
    CHECK(aspect::mode_from_name("stretch") == Mode::Stretch);
    CHECK(aspect::mode_from_name("nonsense") == Mode::Auto);
    CHECK(std::string(aspect::label(Mode::Auto)) == "Auto (4:3)");
    CHECK(std::string(aspect::name(Mode::R3x2)) == "3:2");
}

}  // namespace

int main() {
    test_speed_ladder();
    test_audio_delay();
    test_ab_loop();
    test_audio_clock();
    test_time_stretch();
    test_aspect_43_default();

    if (g_failures) {
        std::printf("[-] %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("[+] tuning: speed, delay, A-B loop, audio clock, stretch, 4:3 SAR ok\n");
    return 0;
}
