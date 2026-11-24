// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/audio_output.h"

#include "playback_tuning.h"

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

    /// Frames of *real audio* the device has consumed since the ring was
    /// created — the audio clock the picture pacing runs against. Silence
    /// pulled during an underrun is not counted: the sound is not progressing
    /// then, so the clock must stand still rather than run on without it.
    long long playedFrames() const {
        QMutexLocker lock(&m_mutex);
        return m_played;
    }

    long long silenceFrames() const {
        QMutexLocker lock(&m_mutex);
        return m_silence;
    }

    qint64 readData(char* data, qint64 maxlen) override {
        QMutexLocker lock(&m_mutex);
        // maxlen is bytes while the ring holds int16 samples: convert first, or
        // the copy overruns the caller's buffer by up to 2x.
        const size_t want = static_cast<size_t>(maxlen) / sizeof(int16_t);
        const size_t have = std::min(want, m_buf.size());
        if (have) std::memcpy(data, m_buf.data(), have * sizeof(int16_t));
        if (have < want) {
            std::memset(data + have * sizeof(int16_t), 0, (want - have) * sizeof(int16_t));
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(have));
        // Count only the frames that carried audio; the zero padding above is a
        // hole in the stream, not playback progress.
        m_played += static_cast<long long>(have / static_cast<size_t>(m_channels));
        m_silence += static_cast<long long>((want - have) / static_cast<size_t>(m_channels));
        return maxlen;  // always "succeed": the sink must never stall on us
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
    long long            m_played = 0;
    long long            m_silence = 0;
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
    // ~120 ms of device buffer: enough to ride out a decode hiccup without
    // adding a noticeable delay to the audio clock.
    m_sink->setBufferSize(44100 * 2 * sizeof(int16_t) * 120 / 1000);
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
    QMutexLocker lock(&m_stretchMutex);
    m_stretcher.reset();
}

void AveraAudioOutput::pushPcm(const int16_t* interleaved, int frames) {
    if (!m_ring || !interleaved || frames <= 0 || m_muted) return;

    QMutexLocker lock(&m_stretchMutex);
    if (m_stretcher.speed() == 1.0) {
        m_ring->push(interleaved, frames);
        return;
    }
    // Time-stretch into a scratch buffer, then hand the device whole frames.
    m_scratch.clear();
    m_stretcher.process(interleaved, static_cast<std::size_t>(frames), m_scratch);
    const int out_frames = static_cast<int>(m_scratch.size() / 2);
    if (out_frames > 0) m_ring->push(m_scratch.data(), out_frames);
}

void AveraAudioOutput::setSpeed(qreal speed) {
    const qreal clamped = std::clamp<qreal>(speed, umdplay::kMinSpeed, umdplay::kMaxSpeed);
    if (qFuzzyCompare(clamped, m_speed)) return;
    m_speed = clamped;
    {
        QMutexLocker lock(&m_stretchMutex);
        m_stretcher.set_speed(clamped);
    }
    emit speedChanged();
}

int AveraAudioOutput::queuedFrames() const { return m_ring ? m_ring->queuedFrames() : 0; }

long long AveraAudioOutput::playedFrames() const {
    return m_ring ? m_ring->playedFrames() : 0;
}

long long AveraAudioOutput::processedFrames() const {
    if (!m_sink) return 0;
    return m_sink->processedUSecs() * 44100 / 1000000;
}

long long AveraAudioOutput::silenceFrames() const {
    return m_ring ? m_ring->silenceFrames() : 0;
}

int AveraAudioOutput::latencyFrames() const {
    // Only what the *device* still holds. playedFrames() counts the frames we
    // have already handed to Qt, so our own ring is not part of that cursor and
    // must not be subtracted again — doing so would make the clock lag by the
    // whole prebuffer. What Qt holds in its pull buffer, though, has been
    // handed over but cannot be heard yet, so it does have to be removed.
    if (!m_sink || m_channels <= 0) return 0;
    const int buffered = m_sink->bufferSize() - int(m_sink->bytesFree());
    return buffered > 0 ? buffered / (int(sizeof(int16_t)) * m_channels) : 0;
}

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
