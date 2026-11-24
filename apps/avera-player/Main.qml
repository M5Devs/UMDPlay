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
            } else if (handlePlayerKey(ev)) {
                ev.accepted = true
            }
        }

        // Player hotkeys, VLC/MPC-HC style. Returns true when handled.
        function handlePlayerKey(ev) {
            const shift = (ev.modifiers & Qt.ShiftModifier) !== 0
            const key = ev.key

            if (key === Qt.Key_Space) { root.chime("select"); playerCtl.togglePlay(); return true }
            if (key === Qt.Key_Left) {
                root.chime("navigate")
                if (playerCtl.settingsOpen) playerCtl.cycleAspect()
                else playerCtl.skip(-10)
                return true
            }
            if (key === Qt.Key_Right) {
                root.chime("navigate")
                if (playerCtl.settingsOpen) playerCtl.cycleAspect()
                else playerCtl.skip(10)
                return true
            }
            // Shift+S snapshots, plain S opens the drawer.
            if (key === Qt.Key_S) {
                if (shift) { root.chime("select"); playerCtl.takeSnapshot() }
                else { root.chime("select"); playerCtl.settingsOpen = !playerCtl.settingsOpen }
                return true
            }
            // Speed: [ slower, ] faster (and -/+ for keyboards without them).
            if (key === Qt.Key_BracketLeft || key === Qt.Key_Minus) {
                root.chime("navigate"); playerCtl.speedDown(); return true
            }
            if (key === Qt.Key_BracketRight || key === Qt.Key_Plus || key === Qt.Key_Equal) {
                root.chime("navigate"); playerCtl.speedUp(); return true
            }
            // Audio delay: J later, K earlier (50 ms a step).
            if (key === Qt.Key_J) { root.chime("navigate"); playerCtl.nudgeAudioDelay(1); return true }
            if (key === Qt.Key_K) { root.chime("navigate"); playerCtl.nudgeAudioDelay(-1); return true }
            if (key === Qt.Key_A) { root.chime("select"); playerCtl.cycleAspect(); return true }
            if (key === Qt.Key_L) { root.chime("select"); playerCtl.cycleAbLoop(); return true }
            if (key === Qt.Key_Backspace) { root.chime("navigate"); playerCtl.resetSpeed(); return true }
            if (key === Qt.Key_Escape) {
                if (playerCtl.settingsOpen) playerCtl.settingsOpen = false
                else { root.chime("back"); root.mode = "hub" }
                return true
            }
            return false
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
