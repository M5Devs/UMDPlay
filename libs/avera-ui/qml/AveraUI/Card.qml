// One poster card in a hub row. The host fills the text; the card only owns
// its look and the focus animation.
import QtQuick

Item {
    id: card

    property string title: ""
    property string subtitle: ""
    property string badge: ""
    property bool   selected: false
    property bool   playable: true

    signal activated()

    width: Theme.cardWidth
    height: Theme.cardHeight

    scale: selected ? 1.0 : 0.94
    opacity: playable ? 1.0 : 0.55

    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    Rectangle {
        id: body
        anchors.fill: parent
        radius: Theme.radius
        color: card.selected ? Theme.glassFocus : Theme.glass
        border.width: card.selected ? 2 : 1
        border.color: card.selected ? Theme.accent : Theme.glassEdge

        Behavior on color { ColorAnimation { duration: 120 } }

        // Soft accent wash that only shows on the focused card.
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            opacity: card.selected ? 0.16 : 0.0
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.accent }
                GradientStop { position: 1.0; color: "transparent" }
            }
            Behavior on opacity { NumberAnimation { duration: 160 } }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 16
            spacing: 4

            Text {
                width: parent.width
                text: card.title
                color: Theme.text
                font: Theme.cardTitle
                elide: Text.ElideRight
                maximumLineCount: 2
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: card.subtitle
                color: Theme.textDim
                font: Theme.cardSub
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }

        // Badge pill, top-right (e.g. "480i", "15:01").
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            visible: card.badge.length > 0
            height: 24
            width: badgeText.implicitWidth + 20
            radius: 12
            color: card.selected ? Theme.accent : "#33ffffff"

            Text {
                id: badgeText
                anchors.centerIn: parent
                text: card.badge
                font: Theme.mono
                color: card.selected ? Theme.bg : Theme.text
            }
        }

        // Play triangle for playable cards.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            width: 30
            height: 30
            radius: 15
            visible: card.playable
            color: card.selected ? Theme.accent : "#22ffffff"

            Text {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 1
                text: "\u25B6"
                font.pixelSize: 12
                color: card.selected ? Theme.bg : Theme.textDim
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: card.activated()
    }
}
