// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/ui_sfx.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

#include <cmath>
#include <cstring>
#include <vector>

namespace avera {

namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 2;

// One additive "glass" chime: a fast-attack / exponential-decay envelope over a
// few partials, so cues sound like a UI and not like a beep.
std::vector<int16_t> renderCue(double baseHz, double ms, double gain) {
    const int frames = static_cast<int>(kRate * ms / 1000.0);
    std::vector<int16_t> out(static_cast<size_t>(frames) * kChannels, 0);
    const double partials[3] = {1.0, 2.0, 3.02};
    const double amps[3] = {1.0, 0.35, 0.12};
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double attack = std::min(1.0, t / 0.006);
        const double decay = std::exp(-t * 9.0);
        double s = 0.0;
        for (int p = 0; p < 3; ++p) {
            s += amps[p] * std::sin(2.0 * M_PI * baseHz * partials[p] * t);
        }
        const double v = s * attack * decay * gain;
        const auto sample = static_cast<int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32000.0));
        out[static_cast<size_t>(i) * 2 + 0] = sample;
        out[static_cast<size_t>(i) * 2 + 1] = sample;
    }
    return out;
}

}  // namespace

/// Mixes cue voices into a short ring that QAudioSink pulls from.
class AveraSfx::Mixer : public QIODevice {
public:
    Mixer() { open(QIODevice::ReadOnly); }

    // Anchor the cue just ahead of the play cursor: the reader keeps advancing
    // whether or not a cue is queued, so a separate write cursor would drift.
    void play(const std::vector<int16_t>& cue) {
        QMutexLocker lock(&m_mutex);
        if (cue.empty()) return;
        const size_t lead = static_cast<size_t>(kRate / 50) * kChannels;  // 20 ms
        const size_t start = (m_read + lead) % m_ring.size();
        for (size_t i = 0; i < cue.size(); ++i) {
            const size_t at = (start + i) % m_ring.size();
            const int mixed = m_ring[at] + cue[i];
            m_ring[at] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
        }
    }

    qint64 readData(char* data, qint64 maxlen) override {
        QMutexLocker lock(&m_mutex);
        auto* out = reinterpret_cast<int16_t*>(data);
        const size_t want = static_cast<size_t>(maxlen / sizeof(int16_t));
        for (size_t i = 0; i < want; ++i) {
            const size_t at = (m_read + i) % m_ring.size();
            out[i] = m_ring[at];
            m_ring[at] = 0;  // consumed once
        }
        m_read = (m_read + want) % m_ring.size();
        return maxlen;
    }
    qint64 writeData(const char*, qint64) override { return 0; }
    bool isSequential() const override { return true; }

private:
    mutable QMutex        m_mutex;
    std::vector<int16_t>  m_ring = std::vector<int16_t>(static_cast<size_t>(kRate) * kChannels, 0);
    size_t                m_read = 0;
};

AveraSfx::AveraSfx(QObject* parent) : QObject(parent) {
    m_mixer = new Mixer;
    startDevice();
}

AveraSfx::~AveraSfx() {
    if (m_sink) { m_sink->stop(); delete m_sink; }
    delete m_mixer;
}

bool AveraSfx::startDevice() {
    QAudioFormat fmt;
    fmt.setSampleRate(kRate);
    fmt.setChannelCount(kChannels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull() || !dev.isFormatSupported(fmt)) return false;
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setVolume(0.6);
    m_sink->start(m_mixer);
    return true;
}

void AveraSfx::play(int cue) {
    if (!m_enabled || !m_mixer || !m_sink) return;
    static std::vector<std::vector<int16_t>> cache;
    if (cache.empty()) {
        cache.push_back(renderCue(880.0, 70.0, 0.30));    // Navigate
        cache.push_back(renderCue(1320.0, 110.0, 0.34));  // Select
        cache.push_back(renderCue(560.0, 90.0, 0.28));    // Back
        cache.push_back(renderCue(660.0, 320.0, 0.30));   // Startup
    }
    if (cue < 0 || cue >= static_cast<int>(cache.size())) return;
    m_mixer->play(cache[static_cast<size_t>(cue)]);
}

void AveraSfx::playNamed(const QString& cue) {
    const QString n = cue.toLower();
    if (n == QLatin1String("select")) play(Select);
    else if (n == QLatin1String("back")) play(Back);
    else if (n == QLatin1String("startup")) play(Startup);
    else play(Navigate);
}

void AveraSfx::setEnabled(bool on) {
    if (m_enabled == on) return;
    m_enabled = on;
    emit enabledChanged();
}

}  // namespace avera
