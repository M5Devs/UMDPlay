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

    // AveraSfx is owned by the host (see main.cpp) and exposed as `uiSfx`.
    function chime(cue) { if (uiSfx) uiSfx.playNamed(cue) }

    FocusScope {
        id: scope
        anchors.fill: parent
        focus: true

        Keys.onPressed: function (ev) {
            if (root.mode === "hub") {
                if (ev.key === Qt.Key_Left) { root.chime("navigate"); hubModel.moveFocus(-1); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { root.chime("navigate"); hubModel.moveFocus(1); ev.accepted = true }
                else if (ev.key === Qt.Key_Up) { root.chime("navigate"); hubModel.moveCategory(-1); ev.accepted = true }
                else if (ev.key === Qt.Key_Down) { root.chime("navigate"); hubModel.moveCategory(1); ev.accepted = true }
                else if (ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                    root.chime("select"); activateFocus(); ev.accepted = true
                } else if (ev.key === Qt.Key_Space) { root.chime("select"); playDisc(); ev.accepted = true }
                else if (ev.key === Qt.Key_Escape) { root.chime("back"); ev.accepted = true }
            } else {
                if (ev.key === Qt.Key_Space) { root.chime("select"); playerCtl.togglePlay(); ev.accepted = true }
                else if (ev.key === Qt.Key_Left) { root.chime("navigate"); playerCtl.skip(-10); ev.accepted = true }
                else if (ev.key === Qt.Key_Right) { root.chime("navigate"); playerCtl.skip(10); ev.accepted = true }
                else if (ev.key === Qt.Key_Escape) { root.chime("back"); root.mode = "hub"; ev.accepted = true }
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
                sfx: uiSfx
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
