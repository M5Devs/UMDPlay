// Transport overlay: title, scrub bar and time. Binds to an
// AveraPlayerController; all interaction goes through its invokables.
import QtQuick

Item {
    id: osd

    property var player: null
    property bool shown: player ? player.osdVisible : false

    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

    function fraction() {
        if (!player || player.duration <= 0) return 0
        return Math.max(0, Math.min(1, player.position / player.duration))
    }

    // Bottom scrim so text stays readable over any picture.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: parent.height * 0.55
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#cc05080c" }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 48
        spacing: 14

        Text {
            width: parent.width
            text: osd.player ? osd.player.title : ""
            color: Theme.text
            font: Theme.display
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: osd.player ? osd.player.subtitle : ""
            color: Theme.textDim
            font: Theme.cardTitle
            elide: Text.ElideRight
        }

        Item {
            width: parent.width
            height: 26

            // Track
            Rectangle {
                id: track
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 6
                radius: 3
                color: "#33ffffff"
            }
            // Progress
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(6, track.width * osd.fraction())
                height: 6
                radius: 3
                color: Theme.accent
            }
            // Playhead
            Rectangle {
                x: Math.max(0, Math.min(track.width - 12, track.width * osd.fraction() - 6))
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: 6
                color: Theme.text
            }
            MouseArea {
                anchors.fill: parent
                onClicked: function(mouse) {
                    if (osd.player) osd.player.seekFraction(mouse.x / width)
                }
            }
        }

        Item {
            width: parent.width
            height: 30

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: osd.player ? osd.player.positionText + "  /  " + osd.player.durationText : ""
                color: Theme.textDim
                font: Theme.mono
            }

            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: osd.player && osd.player.playing ? "\u2016  PAUSE" : "\u25B6  PLAY"
                color: Theme.text
                font: Theme.cardTitle

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -10
                    onClicked: if (osd.player) osd.player.togglePlay()
                }
            }
        }
    }
}
