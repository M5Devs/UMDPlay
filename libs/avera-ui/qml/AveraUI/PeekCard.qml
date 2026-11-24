import QtQuick

// The dimmed "next up" card to the right of the hero: the disc's other title,
// with its own ICON0.PNG artwork and slot telemetry.
Item {
    id: root

    property string artSource: ""
    property string title: ""
    property string meta: ""
    property string kind: "UMD MOVIE"
    property string slot: ""
    property string cta: "Switch Disc"
    property bool dimmed: true

    signal activated()

    opacity: hover.containsMouse ? 1.0 : (root.dimmed ? 0.78 : 1.0)
    Behavior on opacity { NumberAnimation { duration: 180 } }

    GlassPanel {
        anchors.fill: parent
        radius: Theme.radiusLg
        fill: Theme.container
    }

    Rectangle {
        id: clip
        anchors.fill: parent
        anchors.margins: 1
        radius: Theme.radiusLg - 1
        color: Theme.containerLow
        clip: true

        Image {
            id: art
            anchors.fill: parent
            source: root.artSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            visible: status === Image.Ready
        }

        Rectangle {
            anchors.fill: parent
            visible: art.status !== Image.Ready
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.containerHigh }
                GradientStop { position: 1.0; color: Theme.canvasDeep }
            }
            Icon {
                anchors.centerIn: parent
                name: "movie"
                size: 38
                color: "#4400D2FF"
            }
        }

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#26FFFFFF" }
                GradientStop { position: 0.6; color: "#00FFFFFF" }
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.72
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00030D24" }
                GradientStop { position: 1.0; color: "#EE030D24" }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            width: kindRow.implicitWidth + 18
            height: 20
            radius: 10
            color: "#B3030D24"
            Row {
                id: kindRow
                anchors.centerIn: parent
                spacing: 5
                Icon { name: "movie"; size: 11; color: Theme.cyanSoft; weight: 2 }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.kind
                    color: Theme.onSurface
                    font.family: Theme.fontFamily
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                    font.capitalization: Font.AllUppercase
                }
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 10
            spacing: 2

            Text {
                width: parent.width
                text: root.meta
                color: Theme.cyanPale
                elide: Text.ElideRight
                font.family: Theme.monoFamily
                font.pixelSize: 8
                font.capitalization: Font.AllUppercase
            }
            Text {
                width: parent.width
                text: root.title
                color: "#FFFFFF"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 18
                font.weight: Font.Bold
            }
        }
    }

    // Slot telemetry under the card.
    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: clip.bottom
        anchors.topMargin: 6
        height: 16

        Text {
            anchors.left: parent.left
            text: root.slot
            color: Theme.onSurfaceVariant
            font.family: Theme.monoFamily
            font.pixelSize: 8
            font.capitalization: Font.AllUppercase
        }
        Row {
            anchors.right: parent.right
            spacing: 3
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.cta
                color: Theme.cyanPale
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelSm
                font.weight: Font.DemiBold
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "arrow-right"
                size: 10
                color: Theme.cyanPale
                weight: 2
            }
        }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.activated()
    }
}
