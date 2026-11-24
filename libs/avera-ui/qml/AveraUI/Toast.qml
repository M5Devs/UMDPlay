// Floating status pill: "Speed: 1.5x", "Audio delay: +100 ms", "Snapshot
// saved". Driven entirely by the controller's toastText/toastVisible.
import QtQuick

Item {
    id: root

    property var player: null
    property bool shown: player ? player.toastVisible : false

    readonly property real padH: 16
    implicitWidth: label.implicitWidth + padH * 2
    implicitHeight: 40

    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.01
    scale: shown ? 1.0 : 0.94
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: "#E60A1730"
        border.width: 1
        border.color: Theme.hairline

        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: parent.radius - 1
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#3314E0FF" }
                GradientStop { position: 1.0; color: "#00000000" }
            }
        }
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.player ? root.player.toastText : ""
        color: "#FFFFFF"
        font.family: Theme.fontFamily
        font.pixelSize: Theme.labelLg
        font.weight: Font.DemiBold
        font.letterSpacing: 0.4
    }
}
