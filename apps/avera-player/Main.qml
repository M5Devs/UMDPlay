// Avera Player — the UMDPlay shell built on AveraUI.
import QtQuick
import QtQuick.Window
import "qrc:/avera/qml/AveraUI"

Window {
    id: root
    width: 1280
    height: 720
    color: Theme.canvas
    title: "Avera Player"
    visible: true

    // Mode: hub | player
    property string mode: "hub"

    FocusScope {
        id: scope
        anchors.fill: parent
        focus: true

        Keys.onPressed: function (ev) {
            if (root.mode === "hub") {
                if (ev.key === Qt.Key_Left) { hubModel.moveFocus(-1); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { hubModel.moveFocus(1); ev.accepted = true }
                else if (ev.key === Qt.Key_Up) { hubModel.moveCategory(-1); ev.accepted = true }
                else if (ev.key === Qt.Key_Down) { hubModel.moveCategory(1); ev.accepted = true }
                else if (ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                    activateFocus(); ev.accepted = true
                } else if (ev.key === Qt.Key_Space) { playDisc(); ev.accepted = true }
            } else {
                if (ev.key === Qt.Key_Space) { playerCtl.togglePlay(); ev.accepted = true }
                else if (ev.key === Qt.Key_Left) { playerCtl.skip(-10); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { playerCtl.skip(10); ev.accepted = true }
                else if (ev.key === Qt.Key_Escape) { root.mode = "hub"; ev.accepted = true }
            }
        }

        function playDisc() {
            playerCtl.play();
            root.mode = "player";
        }

        function activateFocus() {
            switch (hubModel.focusIndex) {
            case 0: playDisc(); break;
            case 1: playerCtl.pokeOsd(); root.mode = "player"; break;
            case 2: hubModel.currentCategory = 2; break;
            default: hubModel.currentCategory = 4; break;
            }
        }

        // --- Hub ---------------------------------------------------------
        Item {
            id: hubPage
            anchors.fill: parent
            visible: root.mode === "hub"
            opacity: visible ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 220 } }

            HubView {
                anchors.fill: parent
                hub: hubModel
                player: playerCtl
                logoSource: "image://discart/logo"
                onPlayRequested: scope.playDisc()
                onDiscMenuRequested: { playerCtl.pokeOsd(); root.mode = "player" }
                onChaptersRequested: hubModel.currentCategory = 2
                onOptionsRequested: hubModel.currentCategory = 4
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

    // Playback starting from anywhere (transport pill, remote, --play-after)
    // moves the shell to the player page.
    Connections {
        target: playerCtl
        function onStateChanged() {
            if (playerCtl.playing) root.mode = "player"
        }
    }
}
