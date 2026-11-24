// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/player_controller.h"

#include <QTimer>
#include <QVariantMap>
#include <QtMath>

namespace avera {

namespace {
constexpr int kPollMs = 50;         // 20 Hz state mirror
constexpr int kOsdHideMs = 3500;    // auto-hide delay for the transport bar
constexpr int kToastMs = 1700;      // how long a floating toast stays up
constexpr int kSaveEveryTicks = 100;  // ~5 s of playback between position saves
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

    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    m_toastTimer->setInterval(kToastMs);
    connect(m_toastTimer, &QTimer::timeout, this, [this] {
        if (!m_toastVisible) return;
        m_toastVisible = false;
        emit toastChanged();
    });
}

AveraPlayerController::~AveraPlayerController() = default;

void AveraPlayerController::setBackend(AveraPlayerBackend* backend) {
    m_backend = backend;
    if (m_backend) {
        m_backend->setDeinterlaceMode(m_deinterlaceMode);
        applySpeedToBackend();
        m_backend->setAudioDelayMs(m_audioDelayMs);
        m_timer->start();
    } else {
        m_timer->stop();
    }
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

    // A-B repeat: when the playhead reaches B, jump back to A. The seek is a
    // discontinuity, so the backend re-anchors its audio clock itself.
    if (m_abLoop.active() && pos >= m_abLoop.b) {
        m_backend->seekTo(m_abLoop.a);
        m_position = m_abLoop.a;
        posDirty = true;
    }

    // Persist "where I stopped" a few times a minute, and never mid-seek.
    if (m_playing && m_position > 5.0) {
        if (++m_saveCountdown >= kSaveEveryTicks) {
            m_saveCountdown = 0;
            emit positionSaveRequested(m_position);
        }
    } else {
        m_saveCountdown = 0;
    }

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

void AveraPlayerController::setDeinterlaceMode(const QString& mode) {
    const QString normalized = (mode == QLatin1String("off") || mode == QLatin1String("blend"))
                                   ? mode
                                   : QStringLiteral("smart");
    if (normalized == m_deinterlaceMode) return;
    m_deinterlaceMode = normalized;
    if (m_backend) m_backend->setDeinterlaceMode(m_deinterlaceMode);
    emit deinterlaceModeChanged();
}

void AveraPlayerController::cycleDeinterlaceMode() {
    static const char* const kOrder[] = {"smart", "blend", "off"};
    int at = 0;
    for (int i = 0; i < 3; ++i)
        if (m_deinterlaceMode == QLatin1String(kOrder[i])) at = i;
    setDeinterlaceMode(QString::fromLatin1(kOrder[(at + 1) % 3]));
}

// --- aspect ----------------------------------------------------------------

QString AveraPlayerController::aspectMode() const {
    return QString::fromLatin1(aspect::name(m_aspect));
}

void AveraPlayerController::setAspectMode(const QString& mode) {
    const QByteArray utf8 = mode.toUtf8();
    const aspect::Mode parsed = aspect::mode_from_name(utf8.constData());
    if (parsed == m_aspect) return;
    m_aspect = parsed;
    emit aspectModeChanged();
    toastAspect();
}

QString AveraPlayerController::aspectText() const {
    return QString::fromLatin1(aspect::label(m_aspect));
}

void AveraPlayerController::cycleAspect() {
    m_aspect = aspect::cycle_player(m_aspect);
    emit aspectModeChanged();
    toastAspect();
}

QVariantMap AveraPlayerController::videoRect(const QString& mode, int vw, int vh, qreal par,
                                             qreal ww, qreal wh) const {
    const QByteArray utf8 = mode.toUtf8();
    const aspect::Rect r = aspect::fit(aspect::mode_from_name(utf8.constData()),
                                       float(vw), float(vh), float(par),
                                       float(ww), float(wh));
    QVariantMap out;
    out.insert(QStringLiteral("x"), r.x);
    out.insert(QStringLiteral("y"), r.y);
    out.insert(QStringLiteral("w"), r.w);
    out.insert(QStringLiteral("h"), r.h);
    return out;
}

void AveraPlayerController::toastAspect() {
    showToast(QStringLiteral("Aspect: %1").arg(aspectText()));
}

// --- speed -----------------------------------------------------------------

void AveraPlayerController::setSpeed(double speed) {
    const double clamped = umdplay::clamp_speed(speed);
    if (qFuzzyCompare(clamped, m_speed)) return;
    m_speed = clamped;
    applySpeedToBackend();
    emit speedChanged();
    toastSpeed();
}

void AveraPlayerController::applySpeedToBackend() {
    if (m_backend) m_backend->setSpeed(m_speed);
}

QString AveraPlayerController::speedText() const {
    char buf[16];
    return QString::fromLatin1(umdplay::speed_label(m_speed, buf, sizeof(buf)));
}

void AveraPlayerController::speedUp() { setSpeed(umdplay::speed_up(m_speed)); }
void AveraPlayerController::speedDown() { setSpeed(umdplay::speed_down(m_speed)); }
void AveraPlayerController::resetSpeed() { setSpeed(1.0); }

void AveraPlayerController::toastSpeed() {
    showToast(QStringLiteral("Speed: %1").arg(speedText()));
}

// --- audio sync nudge ------------------------------------------------------

void AveraPlayerController::setAudioDelayMs(int ms) {
    const int clamped = umdplay::clamp_audio_delay_ms(ms);
    if (clamped == m_audioDelayMs) return;
    m_audioDelayMs = clamped;
    if (m_backend) m_backend->setAudioDelayMs(m_audioDelayMs);
    emit audioDelayChanged();
    toastDelay();
}

QString AveraPlayerController::audioDelayText() const {
    char buf[32];
    return QString::fromLatin1(umdplay::audio_delay_label(m_audioDelayMs, buf, sizeof(buf)));
}

void AveraPlayerController::nudgeAudioDelay(int dir) {
    if (dir == 0) return;
    setAudioDelayMs(umdplay::step_audio_delay_ms(m_audioDelayMs, dir));
}

void AveraPlayerController::toastDelay() {
    char buf[48];
    showToast(QString::fromLatin1(
        umdplay::audio_delay_label(m_audioDelayMs, buf, sizeof(buf), "Audio delay: ")));
}

// --- picture effects -------------------------------------------------------

void AveraPlayerController::setBrightness(qreal v) {
    v = qBound<qreal>(-1.0, v, 1.0);
    if (qFuzzyCompare(v + 1.0, m_brightness + 1.0)) return;
    m_brightness = v;
    emit videoEffectsChanged(m_brightness, m_contrast, m_saturation);
}

void AveraPlayerController::setContrast(qreal v) {
    v = qBound<qreal>(0.0, v, 2.0);
    if (qFuzzyCompare(v + 1.0, m_contrast + 1.0)) return;
    m_contrast = v;
    emit videoEffectsChanged(m_brightness, m_contrast, m_saturation);
}

void AveraPlayerController::setSaturation(qreal v) {
    v = qBound<qreal>(0.0, v, 2.0);
    if (qFuzzyCompare(v + 1.0, m_saturation + 1.0)) return;
    m_saturation = v;
    emit videoEffectsChanged(m_brightness, m_contrast, m_saturation);
}

bool AveraPlayerController::effectsActive() const {
    return m_brightness != 0.0 || m_contrast != 1.0 || m_saturation != 1.0;
}

void AveraPlayerController::resetVideoEffects() {
    const bool changed = effectsActive();
    m_brightness = 0.0;
    m_contrast = 1.0;
    m_saturation = 1.0;
    if (changed) {
        emit videoEffectsChanged(m_brightness, m_contrast, m_saturation);
        showToast(QStringLiteral("Picture reset"));
    }
}

// --- A-B loop --------------------------------------------------------------

QString AveraPlayerController::abLoopState() const {
    switch (m_abLoop.state) {
        case umdplay::AbLoopState::Off:     return QStringLiteral("off");
        case umdplay::AbLoopState::SetA:    return QStringLiteral("a");
        case umdplay::AbLoopState::Looping: return QStringLiteral("looping");
    }
    return QStringLiteral("off");
}

QString AveraPlayerController::abLoopText() const {
    char buf[64];
    return QString::fromLatin1(m_abLoop.label(buf, sizeof(buf)));
}

void AveraPlayerController::cycleAbLoop() {
    m_abLoop.press(m_position);
    emit abLoopChanged();

    switch (m_abLoop.state) {
        case umdplay::AbLoopState::Off:
            showToast(QStringLiteral("A-B repeat off"));
            break;
        case umdplay::AbLoopState::SetA:
            showToast(QStringLiteral("A at %1 \u00b7 press L for B").arg(formatTime(m_abLoop.a)));
            break;
        case umdplay::AbLoopState::Looping:
            showToast(QStringLiteral("Looping %1 \u2192 %2")
                          .arg(formatTime(m_abLoop.a), formatTime(m_abLoop.b)));
            break;
    }
}

// --- snapshots -------------------------------------------------------------

void AveraPlayerController::takeSnapshot() {
    if (!m_backend) return;
    const QString path = m_backend->saveSnapshot(QString());
    if (path.isEmpty()) {
        showToast(QStringLiteral("Snapshot failed"));
        return;
    }
    emit snapshotSaved(path);
    showToast(QStringLiteral("Snapshot saved"));
}

// --- resume ----------------------------------------------------------------

void AveraPlayerController::setResumePoint(double seconds) {
    const double clamped = (seconds > 0.0 && qIsFinite(seconds)) ? seconds : 0.0;
    if (qFuzzyCompare(clamped + 1.0, m_resumePoint + 1.0)) return;
    m_resumePoint = clamped;
    emit resumePointChanged();
}

void AveraPlayerController::clearResumePoint() { setResumePoint(0.0); }

// --- toast -----------------------------------------------------------------

void AveraPlayerController::showToast(const QString& text) {
    m_toastText = text;
    m_toastVisible = !text.isEmpty();
    emit toastChanged();
    if (m_toastVisible) m_toastTimer->start();
    else m_toastTimer->stop();
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
    if (m_backend->playing()) pause();
    else play();
}

void AveraPlayerController::play() {
    if (!m_backend) return;
    // Resume is armed at most once: the first play after opening a disc picks
    // up where the last session stopped.
    if (m_resumePoint > 0.0) {
        const double at = m_resumePoint;
        m_resumePoint = 0.0;
        emit resumePointChanged();
        m_backend->seekTo(at);
        showToast(QStringLiteral("Resumed at %1").arg(formatTime(at)));
    }
    m_backend->play();
    pokeOsd();
    tick();
}

void AveraPlayerController::pause() {
    if (!m_backend) return;
    m_backend->pause();
    if (m_position > 5.0) emit positionSaveRequested(m_position);
    pokeOsd();
    tick();
}

void AveraPlayerController::stop() {
    if (!m_backend) return;
    if (m_position > 5.0) emit positionSaveRequested(m_position);
    m_backend->stop();
    hideOsd();
    tick();
}

void AveraPlayerController::seekTo(double seconds) {
    if (!m_backend) return;
    m_backend->seekTo(seconds);
    // A seek inside the loop keeps the loop; a seek outside it drops it, so a
    // chapter jump is not silently dragged back to A.
    if (m_abLoop.active() && (seconds < m_abLoop.a - 0.5 || seconds > m_abLoop.b + 0.5)) {
        m_abLoop.clear();
        emit abLoopChanged();
    }
    pokeOsd();
    tick();
}

void AveraPlayerController::skip(double seconds) {
    if (!m_backend) return;
    double target = m_position + seconds;
    if (target < 0) target = 0;
    if (m_duration > 0 && target > m_duration) target = m_duration;
    seekTo(target);
}

void AveraPlayerController::seekFraction(double fraction) {
    if (!m_backend || m_duration <= 0) return;
    seekTo(qBound(0.0, fraction, 1.0) * m_duration);
}

void AveraPlayerController::seekChapter(int index) {
    if (!m_backend) return;
    const QVariantList ch = m_backend->chapters();
    if (index < 0 || index >= ch.size()) return;
    const double t = ch.at(index).toMap().value(QStringLiteral("seconds")).toDouble();
    seekTo(t);
}

}  // namespace avera
