// Avera Player — the UMDPlay shell built on AveraUI.
import QtQuick
import QtQuick.Window
import "qrc:/avera/qml/AveraUI"

Window {
    id: root
    width: 1280
    height: 720
    color: Theme.bg
    title: "Avera Player"
    visible: true

    // Mode: hub | player
    property string mode: "hub"

    FocusScope {
        id: scope
        anchors.fill: parent
        focus: true

        Keys.onPressed: function(ev) {
            if (root.mode === "hub") {
                if (ev.key === Qt.Key_Left)  { hubModel.move(-1, 0); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { hubModel.move(1, 0); ev.accepted = true }
                else if (ev.key === Qt.Key_Up)    { hubModel.move(0, -1); ev.accepted = true }
                else if (ev.key === Qt.Key_Down)  { hubModel.move(0, 1); ev.accepted = true }
                else if (ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                    hubModel.activateCurrent(); ev.accepted = true
                }
            } else {
                if (ev.key === Qt.Key_Space) { playerCtl.togglePlay(); ev.accepted = true }
                else if (ev.key === Qt.Key_Left)  { playerCtl.skip(-10); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { playerCtl.skip(10); ev.accepted = true }
                else if (ev.key === Qt.Key_Escape) { root.mode = "hub"; ev.accepted = true }
            }
        }

        // Backdrop
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.bgTop }
                GradientStop { position: 1.0; color: Theme.bgBottom }
            }
        }

        // --- Hub ---------------------------------------------------------
        Item {
            id: hubPage
            anchors.fill: parent
            visible: root.mode === "hub"
            opacity: visible ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 180 } }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 56
                anchors.leftMargin: Theme.gutter
                spacing: 6

                Text {
                    text: "AVERA"
                    color: Theme.accent
                    font: Qt.font({ pixelSize: 17, weight: Font.DemiBold,
                                    letterSpacing: 8, capitalization: Font.AllUppercase })
                }
                Text {
                    text: hubModel.currentTitle.length > 0 ? hubModel.currentTitle : "UMD Video"
                    color: Theme.text
                    font: Theme.display
                }
                Text {
                    text: hubModel.currentSubtitle
                    color: Theme.textDim
                    font: Theme.cardTitle
                    visible: text.length > 0
                }
            }

            HubView {
                id: hubView
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 180
                hub: hubModel
            }

            Text {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 22
                text: "← → move   ↑ ↓ row   Enter select"
                color: Theme.textFaint
                font: Theme.mono
            }
        }

        // --- Player ------------------------------------------------------
        PlayerView {
            id: playerPage
            anchors.fill: parent
            visible: root.mode === "player"
            player: playerCtl
        }
    }

    Connections {
        target: playerCtl
        function onStateChanged() {
            if (playerCtl.playing) root.mode = "player"
        }
    }
}
