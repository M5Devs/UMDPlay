// Playback settings drawer. Currently the picture-repair policy: Smart blends
// only the lines where the two fields disagree (no ghosting on animation),
// Blend smooths every line, Off shows the woven frame as coded.
import QtQuick

GlassPanel {
    id: root

    property var player: null

    readonly property var modes: ["smart", "off", "blend"]
    readonly property var labels: ["Smart", "Off", "Blend"]
    readonly property var hints: ["Motion-adaptive · no ghosting",
                                  "Show the woven frame as coded",
                                  "Smooth every line"]

    signal chime(string cue)

    implicitWidth: 460
    implicitHeight: 168
    radius: Theme.radiusXl

    function select(mode) {
        if (!player || player.deinterlaceMode === mode) return
        root.chime("select")
        player.deinterlaceMode = mode
    }

    Column {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Row {
            spacing: 8
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "options"
                size: 16
                color: Theme.cyan
                weight: 1.6
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Deinterlace · Anti-Ghosting"
                color: Theme.onSurface
                font.family: Theme.fontFamily
                font.pixelSize: Theme.titleMd
                font.weight: Font.DemiBold
            }
        }

        Row {
            spacing: 8

            Repeater {
                model: root.modes.length

                delegate: Rectangle {
                    id: choice
                    required property int index
                    readonly property bool active: root.player
                        && root.player.deinterlaceMode === root.modes[index]

                    width: 132
                    height: 34
                    radius: Theme.radius
                    color: active ? Theme.cyan : "#1AFFFFFF"
                    border.width: 1
                    border.color: active ? Theme.cyan : Theme.hairline

                    Text {
                        anchors.centerIn: parent
                        text: root.labels[choice.index]
                        color: choice.active ? "#003543" : Theme.onSurfaceVariant
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.labelMd
                        font.weight: choice.active ? Font.Bold : Font.Medium
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.select(root.modes[choice.index])
                    }
                }
            }
        }

        Text {
            width: parent.width
            text: root.player ? root.hints[Math.max(0, root.modes.indexOf(root.player.deinterlaceMode))]
                              : ""
            color: Theme.onSurfaceVariant
            font.family: Theme.fontFamily
            font.pixelSize: Theme.labelMd
            opacity: 0.85
        }
    }
}
