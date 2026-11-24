#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <cstdint>

class QAudioSink;
class QAudioFormat;

namespace avera {

/// Thread-safe S16 stereo PCM sink backed by QAudioSink.
///
/// The decode thread pushes frames; a pull-mode ring buffer feeds the audio
/// device. Underruns yield silence rather than a stall, so a slow decoder never
/// blocks the GUI.
class AveraAudioOutput : public QObject {
    Q_OBJECT
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit AveraAudioOutput(QObject* parent = nullptr);
    ~AveraAudioOutput() override;

    /// Open the device. Safe to call when already running.
    bool start(int sampleRate = 44100, int channels = 2);
    void stop();
    /// Discard queued audio (seek / stop).
    void flush();

    /// Queue interleaved S16 frames. Thread-safe.
    void pushPcm(const int16_t* interleaved, int frames);
    /// Queued frames not yet played, for A/V pacing.
    int queuedFrames() const;

    int  volume() const { return m_volume; }
    void setVolume(int percent);
    bool muted() const { return m_muted; }
    void setMuted(bool muted);
    bool active() const;

signals:
    void volumeChanged();
    void mutedChanged();
    void activeChanged();

private:
    class Ring;
    Ring*        m_ring = nullptr;
    QAudioSink*  m_sink = nullptr;
    int          m_volume = 80;
    bool         m_muted = false;
    int          m_channels = 2;
};

}  // namespace avera
