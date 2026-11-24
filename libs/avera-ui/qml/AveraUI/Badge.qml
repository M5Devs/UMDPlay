import QtQuick

// Frosted pill used for codec / resolution / state tags.
Rectangle {
    id: root

    property string text: ""
    property color textColor: Theme.cyanPale
    property color fill: "#3300D2FF"
    property bool strong: false      // solid cyan chip (on-primary text)
    property int fontSize: Theme.labelSm
    property bool mono: false

    implicitHeight: label.implicitHeight + 8
    implicitWidth: label.implicitWidth + 16
    radius: 4
    color: root.strong ? Theme.cyan : root.fill
    border.width: root.strong ? 0 : 1
    border.color: "#3300D2FF"

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: root.strong ? "#003543" : root.textColor
        font.family: root.mono ? Theme.monoFamily : Theme.fontFamily
        font.pixelSize: root.fontSize
        font.weight: Font.Bold
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }
}
