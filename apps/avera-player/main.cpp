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
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "avera_ui/avera_ui.h"
#include "avera_ui/audio_output.h"
#include "avera_ui/hub.h"
#include "avera_ui/player_controller.h"
#include "avera_ui/video_item.h"

#include "decoder.h"
#include "iso_reader.h"
#include "param_sfo.h"

namespace {

QString secondsText(double s) {
    if (s <= 0.0) return QStringLiteral("--:--");
    const int total = static_cast<int>(s + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QChar('0'))
                                  .arg(total % 60, 2, 10, QChar('0'));
}

/// Bridges UMDPlay's Decoder to AveraUI. Decoding happens on the GUI thread,
/// a few frames per tick: a 480i picture costs a couple of milliseconds and
/// keeps the QML scene free of cross-thread plumbing.
class UmdPlayerBackend : public avera::AveraPlayerBackend {
public:
    UmdPlayerBackend(std::string iso, MpsEntry entry, avera::AveraVideoItem* surface,
                     avera::AveraAudioOutput* audio)
        : m_iso(std::move(iso)), m_entry(std::move(entry)), m_surface(surface), m_audio(audio) {}

    ~UmdPlayerBackend() override { close(); }

    bool open() {
        if (!m_decoder.open_stream(m_iso, m_entry)) return false;
        m_audio->start(44100, 2);
        m_title = QString::fromStdString(m_entry.name);
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

private:
    std::string m_iso;
    MpsEntry m_entry;
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
    std::vector<MpsEntry> streams;
    bool ok = false;
};

DiscInfo probeDisc(const std::string& path) {
    DiscInfo d;
    d.iso = path;
    IsoReader iso(path);
    if (!iso.open()) return d;
    d.streams = iso.get_mps_entries();
    d.title = iso.param().title;
    d.disc_id = iso.param().disc_id;
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

    // Hub: the disc, its streams, and the transport actions.
    QVector<avera::AveraRow> rows;

    avera::AveraRow discRow;
    discRow.key = QStringLiteral("disc");
    discRow.title = QStringLiteral("Disc");
    avera::AveraCard feature;
    feature.title = QString::fromStdString(disc.title.empty() ? disc.iso : disc.title);
    feature.subtitle = QString::fromStdString(disc.disc_id);
    feature.badge = QStringLiteral("UMD");
    feature.data.insert(QStringLiteral("kind"), QStringLiteral("play"));
    discRow.cards.push_back(feature);
    rows.push_back(discRow);

    avera::AveraRow streamRow;
    streamRow.key = QStringLiteral("streams");
    streamRow.title = QStringLiteral("Streams");
    for (const auto& s : disc.streams) {
        avera::AveraCard c;
        c.title = QString::fromStdString(s.name);
        c.subtitle = QStringLiteral("%1 MB").arg(s.size_bytes / (1024.0 * 1024.0), 0, 'f', 1);
        c.badge = QStringLiteral("MPS");
        c.data.insert(QStringLiteral("kind"), QStringLiteral("stream"));
        c.data.insert(QStringLiteral("name"), QString::fromStdString(s.name));
        streamRow.cards.push_back(c);
    }
    rows.push_back(streamRow);

    avera::AveraRow actionRow;
    actionRow.key = QStringLiteral("actions");
    actionRow.title = QStringLiteral("Playback");
    const struct { const char* title; const char* sub; const char* badge; } actions[] = {
        {"Play feature", "Native H.264 + ATRAC3+", "PLAY"},
        {"Restart", "Back to 00:00", "0:00"},
        {"Skip forward", "+30 seconds", "+30"},
    };
    for (const auto& a : actions) {
        avera::AveraCard c;
        c.title = QString::fromLatin1(a.title);
        c.subtitle = QString::fromLatin1(a.sub);
        c.badge = QString::fromLatin1(a.badge);
        c.data.insert(QStringLiteral("kind"), QStringLiteral("action"));
        c.data.insert(QStringLiteral("action"), QString::fromLatin1(a.title));
        actionRow.cards.push_back(c);
    }
    rows.push_back(actionRow);

    hub.setRows(rows);

    const MpsEntry* featureEntry = featureStream(disc);
    if (!featureEntry) return 3;

    auto* backend = new UmdPlayerBackend(disc.iso, *featureEntry, nullptr, &audio);
    if (!backend->open()) {
        std::fprintf(stderr, "[-] could not open %s\n", featureEntry->name.c_str());
        return 4;
    }
    player.setBackend(backend);

    QObject::connect(&hub, &avera::AveraHub::activated, &player,
                     [&](const QString&, int, const QVariantMap& card) {
                         const QString kind = card.value(QStringLiteral("kind")).toString();
                         if (kind == QLatin1String("play")) player.play();
                         else if (kind == QLatin1String("action")) {
                             const QString a = card.value(QStringLiteral("action")).toString();
                             if (a == QLatin1String("Play feature")) player.play();
                             else if (a == QLatin1String("Restart")) player.seekTo(0.0);
                             else if (a == QLatin1String("Skip forward")) player.skip(30.0);
                         }
                     });

    QQmlApplicationEngine engine;
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
            const QImage shot = window->grabWindow();
            if (!shot.isNull() && shot.save(shotPath))
                std::printf("[+] wrote %s (%dx%d)\n", qPrintable(shotPath),
                            shot.width(), shot.height());
            else
                std::fprintf(stderr, "[-] could not grab the window\n");
            app.quit();
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
