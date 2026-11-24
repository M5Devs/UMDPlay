// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/avera_ui.h"

#include "avera_ui/audio_output.h"
#include "avera_ui/hub.h"
#include "avera_ui/player_controller.h"
#include "avera_ui/ui_sfx.h"
#include "avera_ui/video_item.h"

#include <QQmlEngine>

namespace avera {

QString version() { return QStringLiteral("1.0.0"); }

void registerTypes() {
    static bool done = false;
    if (done) return;
    done = true;

    qmlRegisterType<AveraVideoItem>("Avera.UI", 1, 0, "VideoSurface");
    qmlRegisterUncreatableType<AveraHub>("Avera.UI", 1, 0, "Hub",
                                        QStringLiteral("Hub is owned by the host"));
    qmlRegisterUncreatableType<AveraPlayerController>(
        "Avera.UI", 1, 0, "Player",
        QStringLiteral("Player is owned by the host"));
    qmlRegisterUncreatableType<AveraSfx>("Avera.UI", 1, 0, "Sfx",
                                         QStringLiteral("Sfx is owned by the host"));
    qmlRegisterUncreatableType<AveraAudioOutput>(
        "Avera.UI", 1, 0, "AudioOutput",
        QStringLiteral("AudioOutput is owned by the host"));
}

}  // namespace avera
