#include "ui_sfx.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>

namespace umdplay {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSR = static_cast<double>(UiSfx::sample_rate());

// A decaying, optionally gliding sine partial. `detune` spreads the right
// channel a hair above the left for a glassy shimmer without any sample data.
struct Tone {
    double start  = 0.0;   // seconds
    double dur    = 0.0;   // seconds
    double f0     = 0.0;   // start frequency (Hz)
    double f1     = 0.0;   // end frequency (Hz)
    double amp    = 1.0;   // linear gain
    double tau    = 0.1;   // exponential decay time constant (s)
    double attack = 0.004; // attack ramp (s) to avoid a click
    double detune = 0.0;   // right-channel frequency multiplier offset
};

void add_tone(std::vector<float>& buf, const Tone& t) {
    const size_t frames = buf.size() / 2;
    const double s0 = std::max(0.0, t.start);
    const double s1 = t.start + t.dur;
    const size_t i0 = static_cast<size_t>(s0 * kSR);
    const size_t i1 = std::min(frames, static_cast<size_t>(s1 * kSR));
    if (i1 <= i0) return;

    const double dur = std::max(1e-6, t.dur);
    const double tau = std::max(1e-6, t.tau);
    double phase_l = 0.0;
    double phase_r = 0.0;
    for (size_t i = i0; i < i1; ++i) {
        const double local = static_cast<double>(i - i0) / kSR;
        const double u = local / dur;
        const double f = t.f0 + (t.f1 - t.f0) * u;
        phase_l += 2.0 * kPi * f / kSR;
        phase_r += 2.0 * kPi * (f * (1.0 + t.detune)) / kSR;
        const double atk = t.attack > 0.0 ? std::min(1.0, local / t.attack) : 1.0;
        const double env = atk * std::exp(-local / tau);
        const double a = env * t.amp;
        buf[i * 2 + 0] += static_cast<float>(std::sin(phase_l) * a);
        buf[i * 2 + 1] += static_cast<float>(std::sin(phase_r) * a);
    }
}

std::vector<float> make_buffer(double seconds) {
    const size_t frames = static_cast<size_t>(seconds * kSR);
    return std::vector<float>(frames * 2, 0.0f);
}

// Scale a float cue to a gentle headroom peak, then quantize to S16.
std::vector<int16_t> to_s16(std::vector<float> buf, double peak) {
    float max_abs = 0.0f;
    for (float v : buf) max_abs = std::max(max_abs, std::fabs(v));
    const double gain = max_abs > 1e-6f ? peak / static_cast<double>(max_abs) : 0.0;

    std::vector<int16_t> out(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        double v = static_cast<double>(buf[i]) * gain;
        v = std::clamp(v, -1.0, 1.0);
        out[i] = static_cast<int16_t>(std::lround(v * 32767.0));
    }
    return out;
}

// Navigate: a short soft glassy tick that glides down like a water drop.
std::vector<float> cue_navigate() {
    auto b = make_buffer(0.12);
    add_tone(b, {0.0, 0.12, 1760.0, 1500.0, 0.60, 0.030, 0.003, 0.0020});
    add_tone(b, {0.0, 0.10, 3520.0, 3000.0, 0.16, 0.020, 0.003, 0.0025});
    return b;
}

// Select: a warm two-tone chime rising a fifth (like a glassy confirm).
std::vector<float> cue_select() {
    auto b = make_buffer(0.38);
    add_tone(b, {0.00, 0.30,  880.0,  880.0, 0.55, 0.100, 0.006, 0.0018});
    add_tone(b, {0.09, 0.26, 1320.0, 1320.0, 0.48, 0.090, 0.006, 0.0022});
    add_tone(b, {0.09, 0.22, 2640.0, 2640.0, 0.12, 0.060, 0.006, 0.0030});
    return b;
}

// Back: a soft descending two-tone on Esc / Back.
std::vector<float> cue_back() {
    auto b = make_buffer(0.34);
    add_tone(b, {0.00, 0.30, 660.0, 660.0, 0.50, 0.100, 0.006, 0.0018});
    add_tone(b, {0.09, 0.24, 440.0, 440.0, 0.46, 0.090, 0.006, 0.0022});
    add_tone(b, {0.09, 0.20, 220.0, 220.0, 0.14, 0.070, 0.006, 0.0015});
    return b;
}

// Startup: a welcoming major chord with a high shimmer.
std::vector<float> cue_startup() {
    auto b = make_buffer(1.15);
    add_tone(b, {0.00, 1.05, 523.25, 523.25, 0.34, 0.45, 0.020, 0.0015});  // C5
    add_tone(b, {0.00, 1.05, 659.25, 659.25, 0.29, 0.45, 0.020, 0.0018});  // E5
    add_tone(b, {0.03, 1.05, 783.99, 783.99, 0.26, 0.42, 0.020, 0.0020});  // G5
    add_tone(b, {0.06, 0.90, 1567.98, 1567.98, 0.10, 0.35, 0.050, 0.0030}); // G6 shimmer
    return b;
}

std::vector<float> render_cue(UiCue cue) {
    switch (cue) {
        case UiCue::Navigate: return cue_navigate();
        case UiCue::Select:   return cue_select();
        case UiCue::Back:     return cue_back();
        case UiCue::Startup:  return cue_startup();
        case UiCue::None:
        case UiCue::Count:    break;
    }
    return {};
}

// Gentle per-cue headroom so nothing clips when cues stack.
double cue_peak(UiCue cue) {
    switch (cue) {
        case UiCue::Navigate: return 0.30;
        case UiCue::Select:   return 0.42;
        case UiCue::Back:     return 0.38;
        case UiCue::Startup:  return 0.50;
        case UiCue::None:
        case UiCue::Count:    break;
    }
    return 0.4;
}

}  // namespace

UiSfx::UiSfx() {
    for (int i = 0; i < cue_count(); ++i) {
        const UiCue cue = static_cast<UiCue>(i);
        m_pcm[i] = to_s16(render_cue(cue), cue_peak(cue));
    }
}

UiSfx::~UiSfx() { close(); }

const char* UiSfx::cue_name(UiCue cue) {
    switch (cue) {
        case UiCue::Navigate: return "navigate";
        case UiCue::Select:   return "select";
        case UiCue::Back:     return "back";
        case UiCue::Startup:  return "startup";
        case UiCue::None:
        case UiCue::Count:    break;
    }
    return "?";
}

const std::vector<int16_t>& UiSfx::pcm(UiCue cue) const {
    static const std::vector<int16_t> empty;
    const int i = static_cast<int>(cue);
    if (i < 0 || i >= cue_count()) return empty;
    return m_pcm[i];
}

std::vector<float> UiSfx::render_float(UiCue cue) { return render_cue(cue); }

bool UiSfx::init() {
    // UiManager typically already brought up SDL_INIT_AUDIO; only initialize the
    // subsystem ourselves when it is not up yet, and remember that so close()
    // does not tear down state another component still needs.
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        m_own_audio_subsystem = true;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = sample_rate();
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels());
    want.samples = 512;
    want.callback = nullptr;   // SDL_QueueAudio

    SDL_AudioSpec got;
    SDL_zero(got);
    m_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (m_dev != 0) SDL_PauseAudioDevice(m_dev, 0);
    return m_dev != 0;
}

void UiSfx::close() {
    if (m_dev != 0) {
        SDL_CloseAudioDevice(m_dev);
        m_dev = 0;
    }
    if (m_own_audio_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_own_audio_subsystem = false;
    }
}

void UiSfx::queue(const std::vector<int16_t>& buf) {
    if (buf.empty()) return;
    const uint32_t bytes = static_cast<uint32_t>(buf.size() * sizeof(int16_t));
    // Drop the cue if the device is already backed up, so rapid navigation
    // cannot pile cues into an ever-growing latency.
    if (SDL_GetQueuedAudioSize(m_dev) > bytes * 3u) return;
    SDL_QueueAudio(m_dev, buf.data(), bytes);
}

void UiSfx::play(UiCue cue) {
    if (m_dev == 0 || !m_enabled || m_muted) return;
    const int i = static_cast<int>(cue);
    if (i < 0 || i >= cue_count()) return;
    queue(m_pcm[i]);
}

}  // namespace umdplay
