import QtQuick

// Bottom-left "now playing" glass card: disc thumb, state badge, title, time
// code and the little aero level bars.
Item {
    id: root

    property string artSource: ""
    property string stateText: "PLAYING"
    property string detail: ""
    property string title: ""
    property string positionText: "0:00"
    property string durationText: "0:00"
    property bool active: false

    implicitHeight: 62
    implicitWidth: 320

    GlassPanel {
        anchors.fill: parent
        radius: Theme.radiusLg
        fill: "#BF101B32"
    }

    Rectangle {
        id: thumb
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 46
        height: 54
        radius: Theme.radius
        color: Theme.containerHigh
        clip: true

        Image {
            anchors.fill: parent
            source: root.artSource
            fillMode: Image.PreserveAspectCrop
            smooth: true
            asynchronous: true
            visible: status === Image.Ready
        }
        Rectangle {
            anchors.fill: parent
            visible: !root.artSource.length
            color: "transparent"
            Icon {
                anchors.centerIn: parent
                name: "disc"
                size: 22
                color: "#6600D2FF"
            }
        }
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#33FFFFFF" }
                GradientStop { position: 1.0; color: "#00FFFFFF" }
            }
        }
    }

    Column {
        anchors.left: thumb.right
        anchors.leftMargin: 10
        anchors.right: bars.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1

        Row {
            spacing: 6
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: stateLabel.implicitWidth + 10
                height: 13
                radius: 3
                color: root.active ? "#4400D2FF" : "#33FFFFFF"
                Text {
                    id: stateLabel
                    anchors.centerIn: parent
                    text: root.stateText
                    color: root.active ? Theme.cyanPale : Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: 7
                    font.weight: Font.Bold
                    font.letterSpacing: 1.1
                    font.capitalization: Font.AllUppercase
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.detail
                color: Theme.onSurfaceVariant
                font.family: Theme.monoFamily
                font.pixelSize: 7
                elide: Text.ElideRight
            }
        }

        Text {
            width: parent.width
            text: root.title
            color: "#FFFFFF"
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }

        Row {
            spacing: 5
            Text {
                text: root.positionText
                color: Theme.cyanPale
                font.family: Theme.monoFamily
                font.pixelSize: 8
            }
            Text {
                text: "/"
                color: Theme.outline
                font.family: Theme.monoFamily
                font.pixelSize: 8
            }
            Text {
                text: root.durationText
                color: Theme.onSurfaceVariant
                font.family: Theme.monoFamily
                font.pixelSize: 8
            }
        }
    }

    // Aero level bars.
    Row {
        id: bars
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Repeater {
            model: [0.45, 0.9, 0.6, 1.0, 0.3]
            delegate: Rectangle {
                required property int index
                required property var modelData
                width: 3
                height: 6 + 18 * modelData
                radius: 1.5
                color: index % 2 === 0 ? Theme.cyan : Theme.cyanPale
                opacity: root.active ? 1 : 0.35
                SequentialAnimation on height {
                    running: root.active
                    loops: Animation.Infinite
                    NumberAnimation { to: 8; duration: 420 + index * 90; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 6 + 18 * modelData; duration: 420 + index * 90; easing.type: Easing.InOutQuad }
                }
            }
        }
    }
}
