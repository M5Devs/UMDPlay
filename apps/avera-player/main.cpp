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
#include <cstdio>
#include <memory>
#include <vector>

#include "avera_ui/avera_ui.h"
#include "avera_ui/art_provider.h"
#include "avera_ui/audio_output.h"
#include "avera_ui/hub.h"
#include "avera_ui/player_controller.h"
#include "avera_ui/video_item.h"

#include "decoder.h"
#include "iso_reader.h"
#include "param_sfo.h"
#include "playlist_parser.h"
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

    /// Decode at most one video frame. Returns false at end of stream.
    bool pump() {
        if (!m_playing) return false;
        umdplay::VideoFrame frame;
        std::vector<uint8_t> pcm;

        // Drain until a picture shows up; audio is pushed as it comes.
        for (int guard = 0; guard < 64; ++guard) {
            const FrameType t = m_decoder.read_frame(frame, pcm);
            if (!pcm.empty() && m_audio) {
                m_audio->pushPcm(reinterpret_cast<const int16_t*>(pcm.data()),
                                 static_cast<int>(pcm.size() / 4));
                pcm.clear();
            }
            if (t == FrameType::Eof) { m_playing = false; return false; }
            if (t != FrameType::Video || !frame.valid()) continue;

            if (m_surface) {
                m_surface->setFrame(frame.plane(0), frame.stride[0], frame.plane(1),
                                    frame.stride[1], frame.plane(2), frame.stride[2],
                                    frame.width, frame.height, frame.interlaced,
                                    m_decoder.video_par());
            }
            m_position = m_decoder.current_time();
            if (++m_frames % 30 == 0)
                std::fprintf(stderr, "[*] %d frames, t=%.2f\n", m_frames, m_position);
            return true;
        }
        return false;
    }

    // --- AveraPlayerBackend ---
    void play() override { m_playing = true; }
    void pause() override { m_playing = false; }
    void stop() override { m_playing = false; seekTo(0.0); }
    void seekTo(double seconds) override {
        m_decoder.seek_absolute(seconds);
        if (m_audio) m_audio->flush();
        m_position = seconds;
    }
    double position() const override { return m_position; }
    double duration() const override { return m_duration; }
    bool playing() const override { return m_playing; }
    QString title() const override { return m_title; }
    QString subtitle() const override {
        return QStringLiteral("%1x%2  ·  %3 fps  ·  ATRAC3+")
            .arg(m_decoder.video_width())
            .arg(m_decoder.video_height())
            .arg(m_decoder.get_fps(), 0, 'f', 2);
    }

    int frames() const { return m_frames; }
    int videoWidth() const { return m_decoder.video_width(); }
    int videoHeight() const { return m_decoder.video_height(); }
    double fps() const { return m_decoder.get_fps(); }

private:
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

    // ~60 Hz tick, one picture per tick. With an audio device the queue is the
    // clock; without one (headless) fall back to wall-clock pacing so playback
    // still runs at 1x instead of stalling on an unplayed queue.
    QElapsedTimer wall;
    wall.start();
    double wallOrigin = 0.0;
    double lastPos = 0.0;

    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(16);
    QObject::connect(&tick, &QTimer::timeout, [&]() {
        if (!player.playing()) return;
        const double pos = backend->position();
        if (pos + 0.25 < lastPos) { wall.restart(); wallOrigin = pos; }  // seeked back
        lastPos = pos;

        if (audio.active()) {
            if (audio.queuedFrames() >= 44100 / 4) return;
        } else if (pos >= wallOrigin + wall.elapsed() / 1000.0) {
            return;
        }
        backend->pump();
    });
    tick.start();

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        tick.stop();
        backend->close();
    });

    const int rc = app.exec();
    delete backend;
    return rc;
}
