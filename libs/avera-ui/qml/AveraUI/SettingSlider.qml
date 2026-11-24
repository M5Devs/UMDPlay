// Compact horizontal slider for the settings drawer (no Controls dependency:
// the design has its own look and the library ships no widget style).
import QtQuick

Item {
    id: root

    property string label: ""
    property real from: 0.0
    property real to: 1.0
    property real value: 0.0
    property real defaultValue: 0.0
    property string valueText: ""
    property bool active: Math.abs(value - defaultValue) > 0.0001

    signal moved(real value)

    implicitHeight: 30
    implicitWidth: 320

    readonly property real t: Math.max(0, Math.min(1, (value - from) / (to - from)))

    Text {
        id: caption
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 78
        text: root.label
        color: Theme.onSurfaceVariant
        font.family: Theme.fontFamily
        font.pixelSize: Theme.labelMd
    }

    Item {
        id: track
        anchors.left: caption.right
        anchors.right: readout.left
        anchors.verticalCenter: parent.verticalCenter
        height: 20

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: "#26FFFFFF"
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(2, parent.width * root.t)
            height: 4
            radius: 2
            color: Theme.cyan
        }

        // Neutral marker so "reset" has an obvious home position.
        Rectangle {
            x: parent.width * Math.max(0, Math.min(1, (root.defaultValue - root.from) / (root.to - root.from))) - 1
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 8
            color: "#66FFFFFF"
        }

        Rectangle {
            x: Math.max(0, Math.min(parent.width - 10, parent.width * root.t - 5))
            anchors.verticalCenter: parent.verticalCenter
            width: 10
            height: 10
            radius: 5
            color: "#FFFFFF"
            border.width: 2
            border.color: Theme.cyan
        }

        MouseArea {
            anchors.fill: parent
            anchors.topMargin: -8
            anchors.bottomMargin: -8
            cursorShape: Qt.PointingHandCursor
            function apply(mx) {
                const f = Math.max(0, Math.min(1, mx / width))
                root.moved(root.from + f * (root.to - root.from))
            }
            onPressed: function (m) { apply(m.x) }
            onPositionChanged: function (m) { if (pressed) apply(m.x) }
            onWheel: function (w) {
                const step = (root.to - root.from) / 20
                root.moved(root.value + (w.angleDelta.y > 0 ? step : -step))
            }
        }
    }

    Text {
        id: readout
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 58
        horizontalAlignment: Text.AlignRight
        text: root.valueText
        color: root.active ? Theme.cyanPale : Theme.onSurfaceVariant
        font.family: Theme.monoFamily
        font.pixelSize: 10
    }
}
