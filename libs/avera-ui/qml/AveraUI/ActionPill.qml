import QtQuick

// WMC action pill: either the primary cyan-gradient trigger or a frosted
// secondary with a cyan glyph.
Item {
    id: root

    property string text: ""
    property string icon: ""
    property bool primary: false
    property bool focused: false

    signal clicked()

    implicitWidth: label.implicitWidth + (root.primary ? 62 : 44)
    implicitHeight: root.primary ? 38 : 36

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        visible: root.primary
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00D2FF" }
            GradientStop { position: 0.55; color: "#0A7BE8" }
            GradientStop { position: 1.0; color: "#0053DB" }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: parent.radius - 1
            color: "transparent"
            border.width: 1
            border.color: "#66FFFFFF"
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        visible: !root.primary
        color: hover.containsMouse || root.focused ? "#B32A344D" : "#991F2A42"
        border.width: 1
        border.color: root.focused ? "#9900D2FF" : "#2EFFFFFF"
    }

    // Cyan bloom for the focused / hovered state.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -2
        radius: height / 2
        color: "transparent"
        visible: root.primary || root.focused
        border.width: 2
        border.color: root.primary ? "#5500D2FF" : "#4400D2FF"
    }

    Row {
        anchors.centerIn: parent
        spacing: 7

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.icon !== ""
            name: root.icon
            size: root.primary ? 16 : 14
            color: root.primary ? "#FFFFFF" : Theme.cyanPale
            weight: 2
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.primary ? "#FFFFFF" : Theme.onSurface
            font.family: Theme.fontFamily
            font.pixelSize: root.primary ? 13 : 12
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
        }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
