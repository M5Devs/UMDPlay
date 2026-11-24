// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/audio_output.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>
#include <vector>

namespace avera {

/// Pull-mode source for QAudioSink. The decoder thread writes, the audio thread
/// reads; underruns return silence so a slow picture never stalls the device.
class AveraAudioOutput::Ring : public QIODevice {
public:
    Ring(int channels) : m_channels(channels) { open(QIODevice::ReadOnly); }

    void push(const int16_t* src, int frames) {
        QMutexLocker lock(&m_mutex);
        const size_t samples = static_cast<size_t>(frames) * m_channels;
        // Cap at ~2 s so a stalled consumer cannot grow without bound.
        const size_t cap = static_cast<size_t>(44100) * 2 * m_channels;
        if (m_buf.size() + samples > cap) {
            const size_t drop = m_buf.size() + samples - cap;
            m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(std::min(drop, m_buf.size())));
        }
        m_buf.insert(m_buf.end(), src, src + samples);
    }

    void flush() {
        QMutexLocker lock(&m_mutex);
        m_buf.clear();
    }

    int queuedFrames() const {
        QMutexLocker lock(&m_mutex);
        return static_cast<int>(m_buf.size() / m_channels);
    }

    qint64 readData(char* data, qint64 maxlen) override {
        QMutexLocker lock(&m_mutex);
        const size_t want = static_cast<size_t>(maxlen);
        const size_t have = std::min(want, m_buf.size());
        if (have) std::memcpy(data, m_buf.data(), have * sizeof(int16_t));
        if (have < want) {
            std::memset(data + have * sizeof(int16_t), 0, (want - have) * sizeof(int16_t));
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(have));
        return maxlen;  // always "succeed": silence keeps the device clock honest
    }

    qint64 writeData(const char*, qint64) override { return 0; }
    qint64 bytesAvailable() const override {
        QMutexLocker lock(&m_mutex);
        return static_cast<qint64>(m_buf.size() * sizeof(int16_t)) + QIODevice::bytesAvailable();
    }
    bool isSequential() const override { return true; }

private:
    mutable QMutex       m_mutex;
    std::vector<int16_t> m_buf;
    int                  m_channels;
};

AveraAudioOutput::AveraAudioOutput(QObject* parent) : QObject(parent) {}

AveraAudioOutput::~AveraAudioOutput() { stop(); }

bool AveraAudioOutput::start(int sampleRate, int channels) {
    if (m_sink) return true;
    m_channels = channels;

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull() || !dev.isFormatSupported(fmt)) {
        // No device (headless CI): keep a ring so the host's pacing logic still
        // works and playback runs without sound.
        m_ring = new Ring(channels);
        emit activeChanged();
        return false;
    }

    m_ring = new Ring(channels);
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->start(m_ring);
    emit activeChanged();
    return true;
}

void AveraAudioOutput::stop() {
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_ring;
    m_ring = nullptr;
    emit activeChanged();
}

void AveraAudioOutput::flush() {
    if (m_ring) m_ring->flush();
}

void AveraAudioOutput::pushPcm(const int16_t* interleaved, int frames) {
    if (!m_ring || !interleaved || frames <= 0 || m_muted) return;
    m_ring->push(interleaved, frames);
}

int AveraAudioOutput::queuedFrames() const { return m_ring ? m_ring->queuedFrames() : 0; }

void AveraAudioOutput::setVolume(int percent) {
    m_volume = std::clamp(percent, 0, 100);
    if (m_sink) m_sink->setVolume(m_muted ? 0.0 : m_volume / 100.0);
    emit volumeChanged();
}

void AveraAudioOutput::setMuted(bool muted) {
    if (m_muted == muted) return;
    m_muted = muted;
    if (m_sink) m_sink->setVolume(muted ? 0.0 : m_volume / 100.0);
    emit mutedChanged();
}

bool AveraAudioOutput::active() const { return m_sink != nullptr; }

}  // namespace avera
