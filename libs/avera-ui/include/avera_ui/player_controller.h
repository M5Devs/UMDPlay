#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

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

    virtual QString title() const { return {}; }
    virtual QString subtitle() const { return {}; }
    /// Chapter marks as {title, seconds} maps; empty when the stream has none.
    virtual QVariantList chapters() const { return {}; }
    virtual bool   hasDiscMenu() const { return false; }
};

/// QML-facing playback state: mirrors the backend on a 20 Hz poll and owns the
/// transport UI state (OSD auto-hide, settings drawer, volume/mute).
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
    QString aspectMode() const { return m_aspect; }
    void setAspectMode(const QString& mode);

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
    Q_INVOKABLE void cycleAspect();
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

    /// The user asked for the disc's interactive menu.
    void discMenuRequested();
    /// The user asked to leave fullscreen / toggle it (F11).
    void fullscreenToggled();
    /// The user pressed Back / Stop at the hub level.
    void backRequested();

private:
    void tick();

    AveraPlayerBackend* m_backend = nullptr;
    QTimer*    m_timer = nullptr;
    QTimer*    m_osdTimer = nullptr;
    bool       m_playing = false;
    double     m_position = 0.0;
    double     m_duration = 0.0;
    int        m_volume = 80;
    bool       m_muted = false;
    bool       m_osdVisible = true;
    bool       m_settingsOpen = false;
    QString    m_deinterlaceMode = QStringLiteral("smart");
    QString    m_aspect = QStringLiteral("fit");
};

}  // namespace avera
