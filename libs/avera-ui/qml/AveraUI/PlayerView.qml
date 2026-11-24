// Playback surface: the video texture plus the transport overlay. The host
// pushes frames into `surface` (an AveraVideoItem) from its decoder.
import QtQuick
import Avera.UI 1.0

Item {
    id: root

    property var player: null

    readonly property alias surface: video

    /// Picture rectangle from the shared aspect math (include/aspect.h), so
    /// 720x480 renders at its authored 4:3 unless the user forces another
    /// shape. Falls back to a plain letterbox without a controller.
    readonly property var rect: {
        if (!video.hasFrame) return { x: 0, y: 0, w: root.width, h: root.height }
        if (!root.player) {
            const dw = video.sourceWidth * video.pixelAspect
            const dh = video.sourceHeight
            const k = Math.min(root.width / dw, root.height / dh)
            return { x: (root.width - dw * k) / 2, y: (root.height - dh * k) / 2,
                     w: dw * k, h: dh * k }
        }
        return root.player.videoRect(root.player.aspectMode, video.sourceWidth,
                                     video.sourceHeight, video.pixelAspect,
                                     root.width, root.height)
    }

    // Fill/Stretch overflow the window; clip it here.
    clip: true

    Rectangle { anchors.fill: parent; color: "#000000" }

    VideoSurface {
        id: video
        objectName: "surface"
        x: root.rect.x
        y: root.rect.y
        width: Math.round(root.rect.w)
        height: Math.round(root.rect.h)
        visible: hasFrame
    }

    // Shown until the first picture arrives.
    Column {
        anchors.centerIn: parent
        spacing: 12
        visible: !video.hasFrame
        opacity: 0.7

        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: "disc"
            size: 44
            color: Theme.cyan
            weight: 1.4
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "DECODING"
            color: Theme.onSurfaceVariant
            font.family: Theme.fontFamily
            font.pixelSize: Theme.labelMd
            font.weight: Font.DemiBold
            font.letterSpacing: 3
        }
    }

    TransportOsd {
        anchors.fill: parent
        player: root.player
    }

    // Floating status ("Speed: 1.5x", "Audio delay: +150 ms", "Snapshot saved").
    Toast {
        player: root.player
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 150
    }

    // Live settings: reachable with S (or the Options pill on the hub) and
    // applied to the next decoded picture, so the effect is visible at once.
    SettingsPanel {
        player: root.player
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 26
        anchors.bottomMargin: 134
        visible: root.player ? root.player.settingsOpen : false
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: {
            if (!root.player) return
            root.player.pokeOsd()
        }
        onDoubleClicked: if (root.player) root.player.togglePlay()
    }
}
