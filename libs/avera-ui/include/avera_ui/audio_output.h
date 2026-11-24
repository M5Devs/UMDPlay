#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QMutex>
#include <cstdint>
#include <vector>

#include "time_stretch.h"

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
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)

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
    /// Frames of real audio the device has consumed since start(): the audio
    /// clock. Underrun silence is excluded, so a starved device freezes the
    /// clock instead of racing ahead of the sound.
    long long playedFrames() const;
    /// Frames decoded but not audible yet (the sink's internal buffer).
    int latencyFrames() const;
    /// Device playback position in frames, from QAudioSink::processedUSecs().
    /// This is the hardware's own clock: monotonic, smooth, and free of the
    /// read-block quantisation that counting pulled frames has.
    long long processedFrames() const;
    /// Total frames of underrun silence inserted into the stream. The content
    /// at the speaker is processedFrames() - silenceFrames().
    long long silenceFrames() const;

    /// Playback rate. Anything but 1.0 time-stretches the PCM (pitch kept), so
    /// the audio stays intelligible at 2x and does not turn into a drone at
    /// 0.5x.
    qreal speed() const { return m_speed; }
    void setSpeed(qreal speed);

    int  volume() const { return m_volume; }
    void setVolume(int percent);
    bool muted() const { return m_muted; }
    void setMuted(bool muted);
    bool active() const;

signals:
    void speedChanged();
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
    qreal        m_speed = 1.0;
    QMutex       m_stretchMutex;      ///< guards m_stretcher (decode thread)
    umdplay::TimeStretcher m_stretcher;
    std::vector<int16_t> m_scratch;   ///< stretched PCM, reused per push
};

}  // namespace avera
