// SPDX-License-Identifier: GPL-2.0-only
//
// avera-player — UMDPlay's Qt 6 Quick front end.
//
// The window, hub carousel and transport overlay come from AveraUI; this file
// only adapts UMDPlay's Decoder to AveraPlayerBackend and feeds decoded frames
// into the QML scene.

#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QString>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <vector>

#include "avera_ui/avera_ui.h"
#include "avera_ui/art_provider.h"
#include "avera_ui/audio_output.h"
#include "avera_ui/hub.h"
#include "avera_ui/ui_sfx.h"
#include "avera_ui/player_controller.h"
#include "avera_ui/video_item.h"

#include "av_sync.h"
#include "decoder.h"
#include "iso_reader.h"
#include "param_sfo.h"
#include "playlist_parser.h"
#include "recent.h"
#include "umdplay_logo.h"

namespace {

/// "1.73 GB" / "544 MB" from a byte count.
QString sizeText(qint64 bytes) {
    const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
    const double mb = double(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

QString clockText(double s) {
    if (s <= 0.0) return QStringLiteral("--:--");
    const int total = static_cast<int>(s + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QChar('0'))
                                  .arg(total % 60, 2, 10, QChar('0'));
}

/// "UMD_VIDEO" -> "UMD-VIDEO-ROM", the medium name the shell reports.
QString mediumName(const std::string& root) {
    if (root == "UMD_AUDIO") return QStringLiteral("UMD-AUDIO-ROM");
    if (root == "UMD_VIDEO") return QStringLiteral("UMD-VIDEO-ROM");
    return QStringLiteral("UMD-ROM");
}

/// Bridges UMDPlay's Decoder to AveraUI. Decoding happens on the GUI thread,
/// a few frames per tick: a 480i picture costs a couple of milliseconds and
/// keeps the QML scene free of cross-thread plumbing.
class UmdPlayerBackend : public avera::AveraPlayerBackend {
public:
    UmdPlayerBackend(std::string iso, MpsEntry entry, QString discTitle,
                     avera::AveraVideoItem* surface, avera::AveraAudioOutput* audio)
        : m_iso(std::move(iso)), m_entry(std::move(entry)), m_discTitle(std::move(discTitle)),
          m_surface(surface), m_audio(audio) {}

    ~UmdPlayerBackend() override { close(); }

    bool open() {
        if (!m_decoder.open_stream(m_iso, m_entry)) return false;
        m_audio->start(44100, 2);
        m_decoder.set_deinterlace_mode(m_deinterlace);
        // The shell shows the disc's own title, never the raw .MPS file name.
        m_title = m_discTitle.isEmpty() ? QString::fromStdString(m_entry.name) : m_discTitle;
        m_duration = m_decoder.get_duration();
        return true;
    }

    void close() {
        m_decoder.close();
        if (m_audio) m_audio->stop();
        m_playing = false;
    }

    void setSurface(avera::AveraVideoItem* surface) { m_surface = surface; }

    /// Fill a short picture queue (which also pushes the PCM that keeps the
    /// device fed), then present the oldest picture if the audio clock has
    /// reached it. Pictures that are ahead are held for the next tick; pictures
    /// that fell behind are dropped so picture and sound stay locked together.
    void pump() {
        if (!m_playing) return;
        fillQueue();

        while (!m_queue.empty()) {
            const double clock = audioClock();
            if (clock < 0.0) { presentFront(); return; }  // no device: decode-paced

            const double pts = m_queue.front().pts;
            // A desync this large is not a late picture, it is a broken
            // timeline: the device consumed far more audio than we have
            // decoded (a stall, a stalled-then-resumed sink). Dropping the
            // whole backlog would freeze the picture, so re-base the clock on
            // the picture and carry on; the sound resyncs a moment later.
            if (pts >= 0.0 && clock - pts > kResyncSeconds) {
                m_pcm_anchor_pts = pts;
                m_latency_ema = -1.0;
                m_clock_floor = -1.0;
                m_anchor_played = m_audio ? m_audio->playedFrames() : 0;
                std::fprintf(stderr, "[*] A/V resync: audio %.0f ms ahead of the picture\n",
                             (clock - pts) * 1000.0);
                continue;
            }

            switch (umdplay::av_decide(pts, clock)) {
            case umdplay::AvAction::Hold:
                return;
            case umdplay::AvAction::Drop:
                ++m_dropped;
                m_queue.pop_front();
                continue;
            case umdplay::AvAction::Present:
                presentFront();
                return;
            }
        }
    }

    /// Seconds the loudspeaker has reached, or -1 while the stream is not
    /// anchored yet (no audio device, or nothing queued since the last seek).
    /// Anchored on the first decoded *audio* sample's own PTS, and corrected
    /// for what is still buffered, so it describes the speaker rather than the
    /// decoder.
    double audioClock() const {
        if (!m_audio || !m_audio->active() || m_pcm_anchor_pts < 0.0) return -1.0;
        // The clock counts the frames we have handed to the device, minus the
        // ones it is still holding (its pull buffer), so it tracks the audio we
        // produced — the same decode that produced the pictures. Reading the
        // device cursor instead makes the two timelines drift apart by whatever
        // the sound card's clock is off by, and the picture then has to drop to
        // keep up. The buffer term is smoothed: it swings tens of ms as Qt
        // fills and drains it, and that jitter would be subtracted straight
        // into the clock.
        const double instant = static_cast<double>(m_audio->latencyFrames());
        if (m_latency_ema < 0.0) m_latency_ema = instant;
        else m_latency_ema += (instant - m_latency_ema) * 0.02;

        // The device takes frames in ~90 ms blocks, so the raw counter steps
        // rather than flows; presenting against it drops a couple of pictures
        // at every step. Interpolate between blocks with the wall clock (the
        // audio really is playing at that rate), capped just past the truth so
        // a stalled device cannot run the clock away.
        const long long played = m_audio->playedFrames();
        const qint64    now_ms = m_clock_timer.elapsed();
        if (played != m_clock_last_played) {
            m_clock_last_played = played;
            m_clock_last_ms = now_ms;
        }
        const double slope = 44.1 * m_speed;                 // frames per ms
        const double ahead = static_cast<double>(now_ms - m_clock_last_ms) * slope;
        const double limit = static_cast<double>(played) + slope * 60.0;   // 60 ms
        const double cursor = std::min(static_cast<double>(played) + ahead, limit);

        umdplay::AudioClock c;
        c.anchor_pts = m_pcm_anchor_pts;
        c.played_frames = static_cast<long long>(cursor) - m_anchor_played;
        c.sample_rate = 44100;
        c.speed = m_speed;
        c.delay_ms = m_audio_delay_ms;
        c.latency_frames = static_cast<long long>(std::llround(m_latency_ema));
        const double clock = umdplay::av_audio_clock(c);
        // A clock never runs backwards. The device buffer estimate moves by tens
        // of ms as Qt's pull loop fills and drains it, and every such step back
        // would make the pump wait — time the picture never gets back, so the
        // video drifts late and then has to drop to catch up. Hold the high-water
        // mark instead; a real re-anchor (seek, pause, resync) resets it.
        if (clock < m_clock_floor) return m_clock_floor;
        m_clock_floor = clock;
        return clock;
    }

    // --- AveraPlayerBackend ---
    void play() override {
        m_playing = true;
        // The device keeps counting through a pause, so resume from a clean
        // anchor: drop the stale tail and let the next decoded audio sample
        // re-anchor the clock.
        if (m_audio) m_audio->flush();
        m_queue.clear();
        m_pcm_anchor_pts = -1.0;
        m_latency_ema = -1.0;
        m_clock_floor = -1.0;
        m_clock_timer.start();
        m_clock_last_played = -1;
        m_anchor_played = m_audio ? m_audio->playedFrames() : 0;
    }
    void pause() override { m_playing = false; }
    void stop() override {
        m_playing = false;
        seekTo(0.0);
        m_pcm_anchor_pts = -1.0;
    }
    void seekTo(double seconds) override {
        m_decoder.seek_absolute(seconds);
        if (m_audio) m_audio->flush();
        m_queue.clear();
        // Drop the old anchor: the first PCM after the seek re-anchors on its
        // own PTS, so the pictures that follow cannot inherit the old offset.
        m_pcm_anchor_pts = -1.0;
        m_latency_ema = -1.0;
        m_clock_floor = -1.0;
        m_clock_timer.restart();
        m_clock_last_played = -1;
        m_anchor_played = m_audio ? m_audio->playedFrames() : 0;
        m_position = seconds;
    }
    double position() const override { return m_position; }
    double duration() const override { return m_duration; }
    bool playing() const override { return m_playing; }
    QString title() const override { return m_title; }
    QString subtitle() const override {
        QString text = QStringLiteral("%1x%2  ·  %3 fps  ·  ATRAC3+")
                           .arg(m_decoder.video_width())
                           .arg(m_decoder.video_height())
                           .arg(m_decoder.get_fps(), 0, 'f', 2);
        if (m_speed != 1.0) text += QStringLiteral("  ·  %1x").arg(m_speed, 0, 'f', 2);
        if (m_audio_delay_ms != 0) text += QStringLiteral("  ·  %1 %2 ms")
                                              .arg(QChar(0x0394))
                                              .arg(m_audio_delay_ms);
        return text;
    }

    void setSpeed(double speed) override {
        if (!(speed > 0.0)) return;
        m_speed = speed;
        if (m_audio) m_audio->setSpeed(speed);
    }
    void setAudioDelayMs(int ms) override { m_audio_delay_ms = ms; }

    QString saveSnapshot(const QString& directory) override {
        return m_surface ? m_surface->takeSnapshot(directory) : QString();
    }

    void setVideoEffects(qreal brightness, qreal contrast, qreal saturation) {
        if (!m_surface) return;
        m_surface->setBrightness(brightness);
        m_surface->setContrast(contrast);
        m_surface->setSaturation(saturation);
    }

    void setDeinterlaceMode(const QString& mode) override {
        m_deinterlace = (mode == QLatin1String("off"))   ? umdplay::DeinterlaceMode::Off
                        : (mode == QLatin1String("blend")) ? umdplay::DeinterlaceMode::Blend
                                                           : umdplay::DeinterlaceMode::Smart;
        m_decoder.set_deinterlace_mode(m_deinterlace);
    }

    std::size_t queueSize() const { return m_queue.size(); }
    int audioQueued() const { return m_audio ? m_audio->queuedFrames() : -1; }
    int audioLatency() const { return m_audio ? m_audio->latencyFrames() : -1; }
    long long audioProcessed() const { return m_audio ? m_audio->processedFrames() : -1; }
    long long audioSilence() const { return m_audio ? m_audio->silenceFrames() : -1; }
    long long audioPulled() const { return m_audio ? m_audio->playedFrames() : -1; }

    int frames() const { return m_frames; }
    int droppedFrames() const { return m_dropped; }
    int videoWidth() const { return m_decoder.video_width(); }
    int videoHeight() const { return m_decoder.video_height(); }
    double fps() const { return m_decoder.get_fps(); }

private:
    /// Keep both queues fed. Decoding the next picture is the only way to get
    /// PCM, so a picture queue that is "full" while the ring is shallow would
    /// starve the device, freeze the audio cursor and with it the clock.
    void fillQueue() {
        int pictures = static_cast<int>(m_queue.size());
        while (pictures < kLookahead && !audioAhead()) {
            if (!decodeNext()) break;
            ++pictures;
        }
        // Top the ring up by decoding past the lookahead (those pictures are
        // merely ready early); the hard cap keeps a stalled device from growing
        // the backlog without bound.
        while (m_audio && m_audio->queuedFrames() < kAudioLowFrames &&
               pictures < kMaxLookahead && !audioAhead()) {
            if (!decodeNext()) break;
            ++pictures;
        }
    }

    /// True when the ring is already deep enough that more PCM would only be
    /// dead weight (and a seek would throw it away). A single decode call can
    /// deliver a whole backlog, so this is checked before every decode too.
    bool audioAhead() const {
        return m_audio && m_audio->queuedFrames() >= kAudioHighFrames;
    }

    bool decodeNext() {
        umdplay::VideoFrame frame;
        std::vector<uint8_t> pcm;
        for (int guard = 0; guard < 64; ++guard) {
            const FrameType t = m_decoder.read_frame(frame, pcm);
            if (!pcm.empty()) {
                if (m_audio) {
                    // The decoder aligns this chunk to the first post-seek
                    // picture and reports where it starts: anchor the clock on
                    // the audio itself, before any of it has been played.
                    if (m_pcm_anchor_pts < 0.0 && m_decoder.pcm_time() >= 0.0) {
                        m_pcm_anchor_pts = m_decoder.pcm_time();
                        m_latency_ema = -1.0;
                        m_clock_floor = -1.0;
                        m_clock_timer.restart();
                        m_clock_last_played = -1;
                        m_anchor_played = m_audio->playedFrames();
                    }
                    m_audio->pushPcm(reinterpret_cast<const int16_t*>(pcm.data()),
                                     static_cast<int>(pcm.size() / 4));
                }
                pcm.clear();
            }
            if (t == FrameType::Eof) { m_playing = false; return false; }
            if (t != FrameType::Video || !frame.valid()) continue;
            m_queue.push_back(std::move(frame));
            return true;
        }
        return false;
    }

    void presentFront() {
        umdplay::VideoFrame frame = std::move(m_queue.front());
        m_queue.pop_front();
        if (m_surface) {
            m_surface->setFrame(frame.plane(0), frame.stride[0], frame.plane(1),
                                frame.stride[1], frame.plane(2), frame.stride[2],
                                frame.width, frame.height, frame.interlaced,
                                m_decoder.video_par());
        }
        m_position = frame.pts >= 0.0 ? frame.pts : m_decoder.current_time();
        if (++m_frames % 30 == 0) {
            const double clock = audioClock();
            std::fprintf(stderr,
                         "[*] %d frames, t=%.2f  drop=%d  drift=%+.0f ms  q=%d/%d  il=%d"
                         "  dev=%.2f pull=%.2f sil=%.2f\n",
                         m_frames, m_position, m_dropped,
                         clock >= 0.0 ? (m_position - clock) * 1000.0 : 0.0,
                         m_audio ? m_audio->queuedFrames() : 0,
                         m_audio ? m_audio->latencyFrames() : 0,
                         m_audio ? m_audio->processedFrames() / 44100.0 : 0.0,
                         m_audio ? m_audio->playedFrames() / 44100.0 : 0.0,
                         m_audio ? m_audio->silenceFrames() / 44100.0 : 0.0,
                         m_decoder.last_frame_interlaced() ? 1 : 0);
        }
    }

    std::string m_iso;
    MpsEntry m_entry;
    QString m_discTitle;
    avera::AveraVideoItem* m_surface = nullptr;
    avera::AveraAudioOutput* m_audio = nullptr;
    Decoder m_decoder;
    QString m_title;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_playing = false;
    int m_frames = 0;
    int m_dropped = 0;
    umdplay::DeinterlaceMode m_deinterlace = umdplay::DeinterlaceMode::Smart;
    static constexpr int kLookahead = 6;          ///< pictures decoded ahead of the clock
    static constexpr int kMaxLookahead = 20;      ///< hard cap while topping the audio up
    static constexpr int kAudioLowFrames = 8820;  ///< refill the ring below ~200 ms of PCM
    static constexpr int kAudioHighFrames = 13230; ///< and stop at ~300 ms
    static constexpr double kResyncSeconds = 0.5;  ///< audio this far ahead -> re-base
    std::deque<umdplay::VideoFrame> m_queue;
    double m_pcm_anchor_pts = -1.0;     ///< PTS the queued PCM starts at
    long long m_anchor_played = 0;      ///< device cursor when it was anchored
    double m_speed = 1.0;               ///< playback rate (audio time-stretched)
    int    m_audio_delay_ms = 0;        ///< user sync nudge (positive = late sound)
    mutable double m_latency_ema = -1.0;    ///< smoothed pipeline latency, in frames
    mutable double m_clock_floor = -1.0;    ///< monotonic guard for the audio clock
    QElapsedTimer  m_clock_timer;           ///< wall clock between device reads
    mutable long long m_clock_last_played = -1;
    mutable qint64    m_clock_last_ms = 0;
};

struct DiscInfo {
    std::string iso;
    std::string title;
    std::string disc_id;
    std::string category;
    std::string root_name;
    qint64      bytes = 0;
    std::vector<MpsEntry> streams;
    QByteArray  icon;            // ICON0.PNG (square, for small cards)
    QByteArray  pic;             // PIC1.PNG  (16:9 splash, for the hero)
    int         chapters = 0;
    double      feature_seconds = 0.0;
    std::string feature_track;   // playlist track name, e.g. "Feature"
    bool        ok = false;
};

QByteArray toBytes(const std::vector<uint8_t>& v) {
    return QByteArray(reinterpret_cast<const char*>(v.data()), int(v.size()));
}

DiscInfo probeDisc(const std::string& path) {
    DiscInfo d;
    d.iso = path;
    IsoReader iso(path);
    if (!iso.open()) return d;
    d.streams = iso.get_mps_entries();
    d.title = iso.param().title;
    d.disc_id = iso.param().disc_id;
    d.category = iso.param().category;
    d.root_name = iso.root_name();
    d.bytes = QFileInfo(QString::fromStdString(path)).size();
    d.icon = toBytes(iso.read_metadata("ICON0.PNG"));
    d.pic = toBytes(iso.read_metadata("PIC1.PNG"));
    if (d.pic.isEmpty()) d.pic = d.icon;

    // Chapters come from the playlist's main track, which is the disc's own
    // notion of the feature — never a hardcoded stream index.
    if (const UmdAsset* playlist = iso.find_asset("PLAYLIST.UMD")) {
        const auto raw = iso.read_asset(*playlist);
        umdplay::Playlist parsed = umdplay::parse_playlist(raw.data(), raw.size());
        if (parsed.valid) {
            if (const auto* track = parsed.main_track()) {
                const int track_index = int(track - parsed.tracks.data());
                const auto chapters = parsed.chapters(track_index);
                d.chapters = int(chapters.size());
                d.feature_seconds = track->end_seconds - track->start_seconds;
                d.feature_track = track->name;
            }
        }
    }
    d.ok = !d.streams.empty();
    return d;
}

/// Longest stream = the feature; short ones are menus / trailers.
const MpsEntry* featureStream(const DiscInfo& d) {
    const MpsEntry* best = nullptr;
    for (const auto& s : d.streams) {
        if (!best || s.size_bytes > best->size_bytes) best = &s;
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Avera Player"));
    app.setOrganizationName(QStringLiteral("UMDPlay"));

    avera::registerTypes();

    const QStringList args = app.arguments();
    QString isoPath;
    bool windowed = false;
    bool autoplay = false;
    int playAfterMs = -1;
    QString shotPath;
    int shotDelay = 1500;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--windowed")) windowed = true;
        else if (args[i] == QLatin1String("--play")) autoplay = true;
        else if (args[i] == QLatin1String("--play-after") && i + 1 < args.size())
            playAfterMs = args[++i].toInt();
        else if (args[i] == QLatin1String("--screenshot") && i + 1 < args.size())
            shotPath = args[++i];
        else if (args[i] == QLatin1String("--shot-delay") && i + 1 < args.size())
            shotDelay = args[++i].toInt();
        else if (!args[i].startsWith(QLatin1Char('-'))) isoPath = args[i];
    }

    DiscInfo disc;
    if (!isoPath.isEmpty()) disc = probeDisc(isoPath.toStdString());
    if (!disc.ok) {
        std::fprintf(stderr, "usage: avera-player <disc.iso> [--windowed]\n");
        return 2;
    }

    avera::AveraHub hub;
    avera::AveraPlayerController player;
    avera::AveraAudioOutput audio;
    avera::AveraSfx sfx;

    const MpsEntry* featureEntry = featureStream(disc);
    if (!featureEntry) return 3;

    const QString discTitle = QString::fromStdString(
        disc.title.empty() ? std::string("Unknown Disc") : disc.title);
    auto* backend = new UmdPlayerBackend(disc.iso, *featureEntry, discTitle, nullptr, &audio);
    if (!backend->open()) {
        std::fprintf(stderr, "[-] could not open %s\n", featureEntry->name.c_str());
        return 4;
    }
    player.setBackend(backend);

    // --- fill the hub from what the disc actually says ---------------------
    const QString title = discTitle;
    const QString medium = mediumName(disc.root_name);
    const QString discSize = sizeText(disc.bytes);
    const int videoW = backend->videoWidth();
    const int videoH = backend->videoHeight();
    const double fps = backend->fps();

    // The feature's own clip entry, for its size and (fallback) duration.
    const double featureSeconds = disc.feature_seconds > 0.0
        ? disc.feature_seconds : backend->duration();

    hub.setNavItems({QStringLiteral("Media Home"), QStringLiteral("Disc Library"),
                     QStringLiteral("Chapters"), QStringLiteral("Browse")});
    hub.setStatus(QStringLiteral("DRIVE 1 [ISO]"), medium, discSize,
                  QStringLiteral("READY"),
                  QStringLiteral("H.264 + ATRAC3+"),
                  QStringLiteral("NATIVE / ZERO-FFMPEG"));

    QVector<avera::AveraCategory> categories;
    categories.push_back({QStringLiteral("Now Playing"),
                          backend->playing() ? QStringLiteral("On the transport")
                                             : QStringLiteral("Idle · ready to play")});
    categories.push_back({QStringLiteral("Recent Discs"),
                          QStringLiteral("1 disc loaded \u00b7 cached ISO")});
    categories.push_back({QStringLiteral("Chapters"),
                          disc.chapters > 0
                              ? QStringLiteral("%1 markers").arg(disc.chapters)
                              : QStringLiteral("No markers on this disc")});
    categories.push_back({QStringLiteral("Browse"),
                          QStringLiteral("%1 titles on the disc").arg(int(disc.streams.size()))});
    categories.push_back({QStringLiteral("Settings"),
                          QStringLiteral("Native core · no FFmpeg")});
    hub.setCategories(categories);
    hub.setCurrentCategory(1);

    avera::AveraHero hero;
    hero.artUrl = QStringLiteral("image://discart/hero");
    hero.title = title;
    hero.meta = QStringLiteral("%1 chapters \u00b7 %2 \u00b7 %3")
                    .arg(disc.chapters)
                    .arg(clockText(featureSeconds))
                    .arg(sizeText(qint64(featureEntry->size_bytes)));
    QVariantList badges;
    badges.push_back(QVariantMap{{QStringLiteral("text"),
                                  QStringLiteral("%1x%2").arg(videoW).arg(videoH)},
                                 {QStringLiteral("strong"), true}});
    badges.push_back(QVariantMap{{QStringLiteral("text"), QStringLiteral("H.264")},
                                 {QStringLiteral("strong"), false}});
    badges.push_back(QVariantMap{{QStringLiteral("text"), QStringLiteral("ATRAC3+")},
                                 {QStringLiteral("strong"), false}});
    if (fps > 0.0)
        badges.push_back(QVariantMap{{QStringLiteral("text"),
                                      QStringLiteral("%1 fps").arg(fps, 0, 'f', 2)},
                                     {QStringLiteral("strong"), false}});
    hero.badges = badges;
    hub.setHero(hero);

    // Peek card: the disc's next title (the menu clip, when it has one).
    const MpsEntry* menuEntry = nullptr;
    for (const auto& stream : disc.streams) {
        if (&stream != featureEntry && (!menuEntry || stream.size_bytes > menuEntry->size_bytes))
            menuEntry = &stream;
    }
    avera::AveraPeek peek;
    peek.artUrl = QStringLiteral("image://discart/icon");
    peek.title = QStringLiteral("Disc Menu");
    peek.meta = menuEntry
        ? QStringLiteral("MENU \u00b7 %1").arg(sizeText(qint64(menuEntry->size_bytes)))
        : QStringLiteral("NO MENU STREAM");
    peek.kind = QStringLiteral("UMD MENU");
    peek.slot = menuEntry
        ? QStringLiteral("TITLE 02 \u00b7 %1").arg(sizeText(qint64(menuEntry->size_bytes)))
        : QStringLiteral("TITLE 02 \u00b7 MISSING");
    peek.cta = QStringLiteral("Open Menu");
    hub.setPeek(peek);

    hub.setChapterPillText(disc.chapters > 0
                               ? QStringLiteral("%1 Chapters").arg(disc.chapters)
                               : QStringLiteral("Chapters"));
    hub.setNowPlaying(QStringLiteral("Ready"),
                      QStringLiteral("%1 \u00b7 %2").arg(medium, discSize));

    // Cover art is served from memory: the disc's own ICON0/PIC1 plus the
    // embedded UMDPlay mark, with no temporary files on disk.
    auto* art = new avera::ArtProvider;
    art->insert(QStringLiteral("hero"), disc.pic);
    art->insert(QStringLiteral("icon"), disc.icon.isEmpty() ? disc.pic : disc.icon);
    art->insert(QStringLiteral("logo"),
                QByteArray(reinterpret_cast<const char*>(umdplay_logo_png),
                           int(umdplay_logo_png_len)));

    avera::loadFonts();

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("discart"), art);
    engine.rootContext()->setContextProperty(QStringLiteral("hubModel"), &hub);
    engine.rootContext()->setContextProperty(QStringLiteral("playerCtl"), &player);
    engine.rootContext()->setContextProperty(QStringLiteral("uiSfx"), &sfx);
    engine.load(QUrl(QStringLiteral("qrc:/avera/app/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 5;

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) return 6;
    if (!shotPath.isEmpty()) {
        window->resize(1280, 720);
        window->show();
    } else if (!windowed) {
        window->showFullScreen();
    } else {
        window->show();
    }

    // The QML scene owns the video item; hand it to the decoder once the
    // object tree exists.
    auto* surface = qobject_cast<avera::AveraVideoItem*>(
        window->findChild<QQuickItem*>(QStringLiteral("surface")));
    if (surface) backend->setSurface(surface);
    else std::fprintf(stderr, "[-] no video surface in the QML scene\n");

    // Picture tweaks live in the surface, the controller owns their values.
    QObject::connect(&player, &avera::AveraPlayerController::videoEffectsChanged,
                     &player, [backend](qreal b, qreal c, qreal s) {
                         backend->setVideoEffects(b, c, s);
                     });
    QObject::connect(&player, &avera::AveraPlayerController::snapshotSaved,
                     &sfx, [&sfx](const QString& path) {
                         sfx.play(avera::AveraSfx::Select);
                         std::fprintf(stderr, "[+] snapshot: %s\n", qPrintable(path));
                     });

    // Resume: offer the point this disc was last left at, and keep it saved.
    const std::string resumeKey = disc.iso;
    const double resumeAt = recent::load_position(resumeKey);
    if (resumeAt > 5.0 &&
        (backend->duration() <= 0.0 || resumeAt < backend->duration() - 20.0)) {
        player.setResumePoint(resumeAt);
        std::fprintf(stderr, "[*] resume point %.1f s\n", resumeAt);
    }
    auto savePosition = [resumeKey, backend](double seconds) {
        // Near either end a resume point is noise: clear it instead.
        const double dur = backend->duration();
        const bool useful = seconds > 30.0 && (dur <= 0.0 || seconds < dur - 30.0);
        recent::save_position(resumeKey, useful ? seconds : 0.0);
    };
    QObject::connect(&player, &avera::AveraPlayerController::positionSaveRequested,
                     &player, [savePosition](double seconds) { savePosition(seconds); });

    if (autoplay) player.play();
    if (playAfterMs >= 0) QTimer::singleShot(playAfterMs, [&player] { player.play(); });

    if (!shotPath.isEmpty()) {
        QTimer::singleShot(shotDelay, [&]() {
            // Show the transport chrome so the grab captures the full shell.
            player.pokeOsd();
            QTimer::singleShot(350, [&]() {
                const QImage shot = window->grabWindow();
                if (!shot.isNull() && shot.save(shotPath))
                    std::printf("[+] wrote %s (%dx%d)\n", qPrintable(shotPath),
                                shot.width(), shot.height());
                else
                    std::fprintf(stderr, "[-] could not grab the window\n");
                app.quit();
            });
        });
    }

    // ~60 Hz tick. With an audio device the backend paces pictures against the
    // device clock (audio master); without one (headless) the wall clock stands
    // in, so playback still runs at 1x instead of stalling on an unplayed queue.
    QElapsedTimer wall;
    wall.start();
    double wallOrigin = 0.0;
    double lastPos = 0.0;
    bool   wallFallback = false;

    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(16);
    QObject::connect(&tick, &QTimer::timeout, [&]() {
        if (!player.playing()) return;
        const double pos = backend->position();
        const bool seekedBack = pos + 0.25 < lastPos;
        lastPos = pos;

        // Audio is the master clock whenever it has one. Without it — no
        // device yet, a fresh seek, or a ring that starved after a device
        // hiccup — fall back to the wall clock so playback degrades to
        // video-only instead of freezing.
        if (backend->audioClock() < 0.0) {
            if (!wallFallback || seekedBack) {
                wallFallback = true;
                wallOrigin = pos;
                wall.restart();
            }
            if (pos >= wallOrigin + wall.elapsed() / 1000.0) return;
        } else {
            wallFallback = false;
        }
        backend->pump();
    });
    tick.start();

    // Startup chime once the window is on screen and the audio device is live.
    QTimer::singleShot(600, &app, [&sfx]() { sfx.play(avera::AveraSfx::Startup); });

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        tick.stop();
        savePosition(backend->position());
        backend->close();
    });

    const int rc = app.exec();
    delete backend;
    return rc;
}
