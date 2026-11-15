// Unit tests for the procedural WMC UI sound synthesizer. The cue buffers are
// pure math, so their shape can be asserted with no audio hardware; the SDL
// playback path is exercised under the dummy audio driver (set via ctest env).
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).
#include "ui_sfx.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__,         \
                        __LINE__);                                             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// A cue is "well formed" when it is non-empty, even-length (interleaved
// stereo), free of NaN/Inf, strictly inside [-1, 1], and not silent.
void check_well_formed(const std::vector<float>& b, const char* name) {
    CHECK(!b.empty());
    CHECK(b.size() % 2 == 0);
    float peak = 0.0f;
    double energy = 0.0;
    for (float v : b) {
        CHECK(std::isfinite(v));
        CHECK(v >= -1.0f && v <= 1.0f);
        peak = std::max(peak, std::fabs(v));
        energy += static_cast<double>(v) * v;
    }
    CHECK(peak > 0.05f);      // clearly audible, not a whisper
    CHECK(energy > 1e-3);     // actually carries signal
    std::printf("  %-9s %6zu frames  peak=%.3f  rms=%.4f\n", name,
                b.size() / 2, static_cast<double>(peak),
                std::sqrt(energy / static_cast<double>(b.size())));
}

void test_float_cues() {
    for (int i = 0; i < umdplay::UiSfx::cue_count(); ++i) {
        const auto cue = static_cast<umdplay::UiCue>(i);
        check_well_formed(umdplay::UiSfx::render_float(cue),
                          umdplay::UiSfx::cue_name(cue));
    }
}

void test_cues_distinct() {
    // Every cue should have its own duration, so they cannot be the same buffer.
    std::vector<size_t> sizes;
    for (int i = 0; i < umdplay::UiSfx::cue_count(); ++i)
        sizes.push_back(umdplay::UiSfx::render_float(
                            static_cast<umdplay::UiCue>(i)).size());
    for (size_t i = 0; i < sizes.size(); ++i)
        for (size_t j = i + 1; j < sizes.size(); ++j)
            CHECK(sizes[i] != sizes[j]);
}

void test_s16_buffers() {
    umdplay::UiSfx sfx;   // construction generates all buffers, device optional
    CHECK(umdplay::UiSfx::sample_rate() == 44100);
    CHECK(umdplay::UiSfx::channels() == 2);

    for (int i = 0; i < umdplay::UiSfx::cue_count(); ++i) {
        const auto cue = static_cast<umdplay::UiCue>(i);
        const auto& pcm = sfx.pcm(cue);
        CHECK(!pcm.empty());
        CHECK(pcm.size() % 2 == 0);
        int peak = 0;
        for (int16_t s : pcm) peak = std::max(peak, std::abs(static_cast<int>(s)));
        CHECK(peak > 1000);      // ~3% of full scale at minimum
        CHECK(peak <= 32767);
    }
    // UiCue::None is not a playable cue.
    CHECK(sfx.pcm(umdplay::UiCue::None).empty());
}

void test_playback_path() {
    umdplay::UiSfx sfx;
    const bool have_dev = sfx.init();
    std::printf("  SDL audio device: %s\n", have_dev ? "open" : "unavailable");
    // play() must be safe whether or not a device opened (headless CI has none).
    sfx.play(umdplay::UiCue::Startup);
    sfx.set_muted(true);
    sfx.play(umdplay::UiCue::Select);   // muted -> no-op
    sfx.set_muted(false);
    sfx.set_enabled(false);
    sfx.play(umdplay::UiCue::Back);     // disabled -> no-op
    sfx.set_enabled(true);
    sfx.play(umdplay::UiCue::Navigate);
    sfx.play(umdplay::UiCue::None);     // sentinel -> no-op
    sfx.close();
}

}  // namespace

int main() {
    test_float_cues();
    test_cues_distinct();
    test_s16_buffers();
    test_playback_path();

    if (g_failures) {
        std::printf("[test_ui_sfx] %d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("[test_ui_sfx] all UI sound assertions passed\n");
    return 0;
}
