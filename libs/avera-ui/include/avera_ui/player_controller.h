#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "aspect.h"
#include "playback_tuning.h"

class QTimer;

namespace avera {

/// What AveraUI needs from a media engine. Implement this in the host
/// application; every method is called on the GUI thread.
class AveraPlayerBackend {
public:
    virtual ~AveraPlayerBackend() = default;

    virtual void   play() = 0;
    virtual void   pause() = 0;
    virtual void   stop() = 0;
    virtual void   seekTo(double seconds) = 0;

    virtual double position() const = 0;
    virtual double duration() const = 0;
    virtual bool   playing() const = 0;

    /// Deinterlacing policy for field-coded pictures: "smart" (motion
    /// adaptive), "blend" (1-2-1 everywhere) or "off".
    virtual void setDeinterlaceMode(const QString& /*mode*/) {}

    /// Playback rate. The engine must stretch its audio so the pitch holds.
    virtual void setSpeed(double /*speed*/) {}
    /// Audio sync nudge in ms, positive = sound later (the engine moves the
    /// picture clock, it never cuts PCM mid-sample).
    virtual void setAudioDelayMs(int /*ms*/) {}

    /// Write the current picture to `directory` (empty = the platform's
    /// Pictures/snapshots) and return the file written, or an empty string.
    virtual QString saveSnapshot(const QString& /*directory*/) { return {}; }

    virtual QString title() const { return {}; }
    virtual QString subtitle() const { return {}; }
    /// Chapter marks as {title, seconds} maps; empty when the stream has none.
    virtual QVariantList chapters() const { return {}; }
    virtual bool   hasDiscMenu() const { return false; }
};

/// QML-facing playback state: mirrors the backend on a 20 Hz poll and owns the
/// transport UI state (OSD auto-hide, settings drawer, volume/mute) plus the
/// VLC-style tweaks: speed, audio-sync nudge, aspect, picture effects,
/// snapshots, A-B loop and resume-where-you-stopped.
class AveraPlayerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY positionChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY stateChanged)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool osdVisible READ osdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(bool settingsOpen READ settingsOpen WRITE setSettingsOpen NOTIFY settingsOpenChanged)
    Q_PROPERTY(QString deinterlaceMode READ deinterlaceMode WRITE setDeinterlaceMode
               NOTIFY deinterlaceModeChanged)
    Q_PROPERTY(QString aspectMode READ aspectMode WRITE setAspectMode NOTIFY aspectModeChanged)
    Q_PROPERTY(QString aspectText READ aspectText NOTIFY aspectModeChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QString speedText READ speedText NOTIFY speedChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY audioDelayChanged)
    Q_PROPERTY(QString audioDelayText READ audioDelayText NOTIFY audioDelayChanged)
    Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness NOTIFY videoEffectsChanged)
    Q_PROPERTY(qreal contrast READ contrast WRITE setContrast NOTIFY videoEffectsChanged)
    Q_PROPERTY(qreal saturation READ saturation WRITE setSaturation NOTIFY videoEffectsChanged)
    Q_PROPERTY(bool effectsActive READ effectsActive NOTIFY videoEffectsChanged)
    Q_PROPERTY(QString abLoopState READ abLoopState NOTIFY abLoopChanged)
    Q_PROPERTY(QString abLoopText READ abLoopText NOTIFY abLoopChanged)
    Q_PROPERTY(QString toastText READ toastText NOTIFY toastChanged)
    Q_PROPERTY(bool toastVisible READ toastVisible NOTIFY toastChanged)
    Q_PROPERTY(bool hasResumePoint READ hasResumePoint NOTIFY resumePointChanged)
    Q_PROPERTY(double resumePoint READ resumePoint NOTIFY resumePointChanged)

public:
    explicit AveraPlayerController(QObject* parent = nullptr);
    ~AveraPlayerController() override;

    /// Borrow a backend; the controller does not take ownership.
    void setBackend(AveraPlayerBackend* backend);

    bool playing() const;
    double position() const;
    double duration() const;
    QString positionText() const;
    QString durationText() const;
    QString title() const;
    QString subtitle() const;
    QVariantList chapters() const;

    int  volume() const { return m_volume; }
    void setVolume(int percent);
    bool muted() const { return m_muted; }
    void setMuted(bool muted);

    bool osdVisible() const { return m_osdVisible; }
    bool settingsOpen() const { return m_settingsOpen; }
    void setSettingsOpen(bool open);
    /// "smart" | "off" | "blend"; applied to the backend immediately so the
    /// choice takes effect on the next decoded picture.
    QString deinterlaceMode() const { return m_deinterlaceMode; }
    void setDeinterlaceMode(const QString& mode);
    Q_INVOKABLE void cycleDeinterlaceMode();

    // --- aspect -------------------------------------------------------------
    /// "auto" | "4:3" | "16:9" | "fill" | "stretch".
    QString aspectMode() const;
    void setAspectMode(const QString& mode);
    QString aspectText() const;
    Q_INVOKABLE void cycleAspect();
    /// Destination rectangle for the picture, as {x, y, w, h}. Pure geometry
    /// from include/aspect.h, so QML never re-derives the letterbox maths.
    Q_INVOKABLE QVariantMap videoRect(const QString& mode, int vw, int vh, qreal par,
                                      qreal ww, qreal wh) const;

    // --- speed --------------------------------------------------------------
    double speed() const { return m_speed; }
    void setSpeed(double speed);
    QString speedText() const;
    Q_INVOKABLE void speedUp();
    Q_INVOKABLE void speedDown();
    Q_INVOKABLE void resetSpeed();

    // --- audio sync nudge ---------------------------------------------------
    int audioDelayMs() const { return m_audioDelayMs; }
    void setAudioDelayMs(int ms);
    QString audioDelayText() const;
    /// dir > 0 delays the sound by one step, dir < 0 advances it.
    Q_INVOKABLE void nudgeAudioDelay(int dir);

    // --- picture effects ----------------------------------------------------
    qreal brightness() const { return m_brightness; }
    void setBrightness(qreal v);
    qreal contrast() const { return m_contrast; }
    void setContrast(qreal v);
    qreal saturation() const { return m_saturation; }
    void setSaturation(qreal v);
    bool effectsActive() const;
    Q_INVOKABLE void resetVideoEffects();

    // --- A-B loop -----------------------------------------------------------
    QString abLoopState() const;
    QString abLoopText() const;
    Q_INVOKABLE void cycleAbLoop();

    // --- snapshots ----------------------------------------------------------
    /// Ask the backend to write the current picture; toasts the result.
    Q_INVOKABLE void takeSnapshot();

    // --- resume -------------------------------------------------------------
    double resumePoint() const { return m_resumePoint; }
    bool hasResumePoint() const { return m_resumePoint > 0.0; }
    /// Arm a resume point; the next play() jumps to it once.
    Q_INVOKABLE void setResumePoint(double seconds);
    Q_INVOKABLE void clearResumePoint();

    // --- toast --------------------------------------------------------------
    QString toastText() const { return m_toastText; }
    bool toastVisible() const { return m_toastVisible; }
    Q_INVOKABLE void showToast(const QString& text);

    /// Show the transport OSD and restart its auto-hide timer.
    Q_INVOKABLE void pokeOsd();
    Q_INVOKABLE void hideOsd();

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void skip(double seconds);
    Q_INVOKABLE void seekFraction(double fraction);
    Q_INVOKABLE void seekChapter(int index);
    Q_INVOKABLE QString formatTime(double seconds) const;

signals:
    void stateChanged();
    void positionChanged();
    void volumeChanged();
    void mutedChanged();
    void osdVisibleChanged();
    void settingsOpenChanged();
    void deinterlaceModeChanged();
    void aspectModeChanged();
    void speedChanged();
    void audioDelayChanged();
    /// Emitted whenever brightness/contrast/saturation change: the host applies
    /// them to its video surface (the controller owns no picture).
    void videoEffectsChanged(qreal brightness, qreal contrast, qreal saturation);
    void abLoopChanged();
    void toastChanged();
    void resumePointChanged();
    void snapshotSaved(const QString& path);
    /// The playhead moved far enough that the host should persist it.
    void positionSaveRequested(double seconds);

    /// The user asked for the disc's interactive menu.
    void discMenuRequested();
    /// The user asked to leave fullscreen / toggle it (F11).
    void fullscreenToggled();
    /// The user pressed Back / Stop at the hub level.
    void backRequested();

private:
    void tick();
    void applySpeedToBackend();
    void toastSpeed();
    void toastDelay();
    void toastAspect();

    AveraPlayerBackend* m_backend = nullptr;
    QTimer*    m_timer = nullptr;
    QTimer*    m_osdTimer = nullptr;
    QTimer*    m_toastTimer = nullptr;
    bool       m_playing = false;
    double     m_position = 0.0;
    double     m_duration = 0.0;
    int        m_volume = 80;
    bool       m_muted = false;
    bool       m_osdVisible = true;
    bool       m_settingsOpen = false;
    QString    m_deinterlaceMode = QStringLiteral("smart");
    aspect::Mode m_aspect = aspect::Mode::Auto;
    double     m_speed = 1.0;
    int        m_audioDelayMs = 0;
    qreal      m_brightness = 0.0;
    qreal      m_contrast = 1.0;
    qreal      m_saturation = 1.0;
    umdplay::AbLoop m_abLoop;
    QString    m_toastText;
    bool       m_toastVisible = false;
    double     m_resumePoint = 0.0;
    int        m_saveCountdown = 0;   ///< ticks until the next position save
};

}  // namespace avera
