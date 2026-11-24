// Playback surface: the video texture plus the transport overlay. The host
// pushes frames into `surface` (an AveraVideoItem) from its decoder.
import QtQuick
import Avera.UI 1.0

Item {
    id: root

    property var player: null

    readonly property alias surface: video

    Rectangle { anchors.fill: parent; color: "#000000" }

    // Letterbox to the picture's true shape: coded size times pixel aspect, so
    // anamorphic 720x480 renders at 4:3 instead of being stretched.
    readonly property real displayWidth: video.hasFrame
        ? video.sourceWidth * video.pixelAspect : root.width
    readonly property real displayHeight: video.hasFrame ? video.sourceHeight : root.height
    readonly property real fit: Math.min(root.width / displayWidth,
                                         root.height / displayHeight)

    VideoSurface {
        id: video
        objectName: "surface"
        width: Math.round(root.displayWidth * root.fit)
        height: Math.round(root.displayHeight * root.fit)
        anchors.centerIn: parent
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
