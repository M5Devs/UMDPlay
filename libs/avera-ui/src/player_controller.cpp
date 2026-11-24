// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/player_controller.h"

#include <QTimer>
#include <QVariantMap>
#include <QtMath>

namespace avera {

namespace {
constexpr int kPollMs = 50;        // 20 Hz state mirror
constexpr int kOsdHideMs = 3500;   // auto-hide delay for the transport bar
}  // namespace

AveraPlayerController::AveraPlayerController(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &AveraPlayerController::tick);

    m_osdTimer = new QTimer(this);
    m_osdTimer->setSingleShot(true);
    m_osdTimer->setInterval(kOsdHideMs);
    connect(m_osdTimer, &QTimer::timeout, this, [this] {
        if (!m_settingsOpen) hideOsd();
    });
}

AveraPlayerController::~AveraPlayerController() = default;

void AveraPlayerController::setBackend(AveraPlayerBackend* backend) {
    m_backend = backend;
    if (m_backend) m_timer->start();
    else m_timer->stop();
    emit stateChanged();
    emit positionChanged();
}

bool AveraPlayerController::playing() const {
    return m_backend ? m_backend->playing() : false;
}

double AveraPlayerController::position() const { return m_position; }
double AveraPlayerController::duration() const { return m_duration; }

QString AveraPlayerController::positionText() const { return formatTime(m_position); }
QString AveraPlayerController::durationText() const { return formatTime(m_duration); }

QString AveraPlayerController::title() const {
    return m_backend ? m_backend->title() : QString();
}

QString AveraPlayerController::subtitle() const {
    return m_backend ? m_backend->subtitle() : QString();
}

QVariantList AveraPlayerController::chapters() const {
    return m_backend ? m_backend->chapters() : QVariantList();
}

QString AveraPlayerController::formatTime(double seconds) const {
    if (seconds < 0 || !qIsFinite(seconds)) seconds = 0;
    const int total = int(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

void AveraPlayerController::tick() {
    if (!m_backend) return;

    const bool playingNow = m_backend->playing();
    const double pos = m_backend->position();
    const double dur = m_backend->duration();

    bool stateDirty = false;
    bool posDirty = false;
    if (playingNow != m_playing) { m_playing = playingNow; stateDirty = true; }
    if (!qFuzzyCompare(pos + 1.0, m_position + 1.0)) { m_position = pos; posDirty = true; }
    if (!qFuzzyCompare(dur + 1.0, m_duration + 1.0)) { m_duration = dur; posDirty = true; }

    if (stateDirty) emit stateChanged();
    if (posDirty) emit positionChanged();
}

void AveraPlayerController::setVolume(int percent) {
    percent = qBound(0, percent, 100);
    if (percent == m_volume) return;
    m_volume = percent;
    if (m_muted && percent > 0) { m_muted = false; emit mutedChanged(); }
    emit volumeChanged();
}

void AveraPlayerController::setMuted(bool muted) {
    if (muted == m_muted) return;
    m_muted = muted;
    emit mutedChanged();
}

void AveraPlayerController::setSettingsOpen(bool open) {
    if (open == m_settingsOpen) return;
    m_settingsOpen = open;
    emit settingsOpenChanged();
    if (open) {
        m_osdTimer->stop();
        if (!m_osdVisible) { m_osdVisible = true; emit osdVisibleChanged(); }
    } else {
        m_osdTimer->start();
    }
}

void AveraPlayerController::setDeinterlace(bool on) {
    if (on == m_deinterlace) return;
    m_deinterlace = on;
    emit deinterlaceChanged();
}

void AveraPlayerController::setAspectMode(const QString& mode) {
    if (mode == m_aspect) return;
    m_aspect = mode;
    emit aspectModeChanged();
}

void AveraPlayerController::pokeOsd() {
    if (!m_osdVisible) {
        m_osdVisible = true;
        emit osdVisibleChanged();
    }
    if (!m_settingsOpen) m_osdTimer->start();
}

void AveraPlayerController::hideOsd() {
    if (m_settingsOpen) return;
    if (!m_osdVisible) return;
    m_osdVisible = false;
    emit osdVisibleChanged();
}

void AveraPlayerController::togglePlay() {
    if (!m_backend) return;
    if (m_backend->playing()) m_backend->pause();
    else m_backend->play();
    pokeOsd();
    tick();
}

void AveraPlayerController::play() {
    if (!m_backend) return;
    m_backend->play();
    pokeOsd();
    tick();
}

void AveraPlayerController::pause() {
    if (!m_backend) return;
    m_backend->pause();
    pokeOsd();
    tick();
}

void AveraPlayerController::stop() {
    if (!m_backend) return;
    m_backend->stop();
    hideOsd();
    tick();
}

void AveraPlayerController::seekTo(double seconds) {
    if (!m_backend) return;
    m_backend->seekTo(seconds);
    pokeOsd();
    tick();
}

void AveraPlayerController::skip(double seconds) {
    if (!m_backend) return;
    double target = m_position + seconds;
    if (target < 0) target = 0;
    if (m_duration > 0 && target > m_duration) target = m_duration;
    m_backend->seekTo(target);
    pokeOsd();
    tick();
}

void AveraPlayerController::seekFraction(double fraction) {
    if (!m_backend || m_duration <= 0) return;
    m_backend->seekTo(qBound(0.0, fraction, 1.0) * m_duration);
    pokeOsd();
    tick();
}

void AveraPlayerController::seekChapter(int index) {
    if (!m_backend) return;
    const QVariantList ch = m_backend->chapters();
    if (index < 0 || index >= ch.size()) return;
    const double t = ch.at(index).toMap().value(QStringLiteral("seconds")).toDouble();
    m_backend->seekTo(t);
    pokeOsd();
    tick();
}

void AveraPlayerController::cycleAspect() {
    if (m_aspect == QLatin1String("fit")) setAspectMode(QStringLiteral("zoom"));
    else if (m_aspect == QLatin1String("zoom")) setAspectMode(QStringLiteral("stretch"));
    else setAspectMode(QStringLiteral("fit"));
}

}  // namespace avera
