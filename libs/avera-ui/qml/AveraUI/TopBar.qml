import QtQuick

// Fixed top chrome: brand mark + product lockup, primary nav pills, live clock.
Item {
    id: root

    property string logoSource: ""
    property string clockTime: ""
    property string clockDate: ""
    property int activeNav: 0
    property var navModel: []

    signal navActivated(int index)

    implicitHeight: Theme.headerHeight

    Rectangle {
        anchors.fill: parent
        color: "#CC030D24"
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: "#1AFFFFFF"
    }

    Row {
        id: brand
        anchors.left: parent.left
        anchors.leftMargin: Theme.margin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Image {
            id: mark
            source: root.logoSource
            height: 26
            width: 26
            fillMode: Image.PreserveAspectFit
            smooth: true
            visible: status === Image.Ready
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 7
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "UMDPlay"
                color: "#FFFFFF"
                font.family: Theme.fontFamily
                font.pixelSize: 18
                font.weight: Font.Bold
                font.letterSpacing: -0.3
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: avera.implicitWidth + 12
                height: avera.implicitHeight + 4
                radius: 3
                color: Theme.cyan
                Text {
                    id: avera
                    anchors.centerIn: parent
                    text: "AVERA"
                    color: "#003543"
                    font.family: Theme.fontFamily
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    font.letterSpacing: 1.4
                }
            }
        }
    }

    Row {
        id: nav
        anchors.centerIn: parent
        spacing: 4

        Repeater {
            model: root.navModel
            delegate: Rectangle {
                id: pill
                required property int index
                required property var modelData
                readonly property bool active: index === root.activeNav

                width: navLabel.implicitWidth + 26
                height: 28
                radius: Theme.radiusSm
                color: active ? Theme.azure : (hover.containsMouse ? Theme.containerHigh : "transparent")

                Rectangle {
                    visible: pill.active
                    anchors.fill: parent
                    radius: parent.radius
                    color: "transparent"
                    border.width: 1
                    border.color: "#663B82FF"
                }

                Text {
                    id: navLabel
                    anchors.centerIn: parent
                    text: pill.modelData
                    color: pill.active ? "#FFFFFF" : Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelLg
                    font.weight: pill.active ? Font.DemiBold : Font.Medium
                }

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.navActivated(pill.index)
                }
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.margin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0
            Text {
                anchors.right: parent.right
                text: root.clockTime
                color: Theme.cyanPale
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelLg
                font.weight: Font.DemiBold
                font.letterSpacing: 0.4
            }
            Text {
                anchors.right: parent.right
                text: root.clockDate
                color: Theme.onSurfaceVariant
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelSm
                font.letterSpacing: 0.6
                font.capitalization: Font.AllUppercase
            }
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 26
            height: 26
            radius: 13
            color: Theme.cyanPale
            Icon {
                anchors.centerIn: parent
                name: "person"
                size: 15
                color: "#003543"
                weight: 2
            }
        }
    }
}
