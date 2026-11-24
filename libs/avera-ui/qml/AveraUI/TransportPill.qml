import QtQuick

// Bottom-right glossy circular transport rig (WMC remote target).
Item {
    id: root

    property bool playing: false
    property int volume: 80
    property bool muted: false
    property bool focused: false

    signal togglePlay()
    signal stop()
    signal skipPrev()
    signal skipNext()
    signal rewind()
    signal forward()
    signal fullscreen()

    implicitWidth: row.implicitWidth + 30
    implicitHeight: 70

    GlassPanel {
        anchors.fill: parent
        radius: height / 2
        fill: "#BF101B32"
        edgeTop: root.focused ? "#9900D2FF" : Theme.glassEdgeTop
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        component RoundButton: Item {
            id: btn
            property string icon: "play"
            property bool hero: false
            property bool enabled2: true
            signal clicked()

            width: hero ? 50 : 36
            height: width

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: btn.hero ? "transparent"
                    : (ma.containsMouse ? Theme.containerHigh : "#8C141F37")
                border.width: btn.hero ? 0 : 1
                border.color: "#2EFFFFFF"
            }

            Rectangle {
                visible: btn.hero
                anchors.fill: parent
                radius: width / 2
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#00D2FF" }
                    GradientStop { position: 0.5; color: "#1279E0" }
                    GradientStop { position: 1.0; color: "#0B2A55" }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: parent.radius - 1
                    color: "transparent"
                    border.width: 1
                    border.color: "#80FFFFFF"
                }
            }

            Icon {
                anchors.centerIn: parent
                name: btn.icon
                size: btn.hero ? 19 : 14
                color: btn.hero ? "#FFFFFF" : Theme.onSurface
                weight: 2
            }

            MouseArea {
                id: ma
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: btn.clicked()
            }
        }

        RoundButton { icon: "skip-prev"; onClicked: root.skipPrev() }
        RoundButton { icon: "rewind"; onClicked: root.rewind() }
        RoundButton {
            icon: root.playing ? "pause" : "play"
            hero: true
            onClicked: root.togglePlay()
        }
        RoundButton { icon: "stop"; onClicked: root.stop() }
        RoundButton { icon: "forward"; onClicked: root.forward() }
        RoundButton { icon: "skip-next"; onClicked: root.skipNext() }

        Rectangle {
            width: 1
            height: 22
            color: "#3C494E"
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 5

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "volume"
                size: 14
                color: root.muted ? Theme.outline : Theme.onSurface
                weight: 2
            }

            Rectangle {
                width: 44
                height: 4
                radius: 2
                color: Theme.containerHighest
                Rectangle {
                    width: parent.width * (root.muted ? 0 : root.volume / 100)
                    height: parent.height
                    radius: parent.radius
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.azureBright }
                        GradientStop { position: 1.0; color: Theme.cyan }
                    }
                }
            }
        }

        RoundButton { icon: "fullscreen"; onClicked: root.fullscreen() }
    }
}
