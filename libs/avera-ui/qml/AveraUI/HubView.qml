import QtQuick

// The Avera media center: fixed header, disc telemetry strip, vertical category
// rail, hero disc with its action pills, a dimmed peek card, and the single WMC
// bottom row (mini now-playing + transport rig).
Item {
    id: root

    property var hub: null          // AveraHub
    property var player: null       // AveraPlayerController
    property var sfx: null          // AveraSfx (optional; null = silent)
    property string logoSource: ""

    function chime(cue) { if (sfx) sfx.playNamed(cue) }

    signal playRequested()
    signal discMenuRequested()
    signal chaptersRequested()
    signal optionsRequested()

    // --- live clock --------------------------------------------------------
    property string clockTime: Qt.formatTime(new Date(), "h:mm AP")
    property string clockDate: Qt.formatDate(new Date(), "dddd, MMM d")

    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            const now = new Date();
            root.clockTime = Qt.formatTime(now, "h:mm AP");
            root.clockDate = Qt.formatDate(now, "dddd, MMM d");
        }
    }

    AuroraBackground { anchors.fill: parent }

    TopBar {
        id: topBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.headerHeight
        logoSource: root.logoSource
        clockTime: root.clockTime
        clockDate: root.clockDate
        navModel: root.hub ? root.hub.navItems : []
        activeNav: root.hub ? root.hub.activeNav : 0
        onNavActivated: function (index) {
            if (root.hub) root.hub.activeNav = index;
        }
    }

    StatusStrip {
        id: status
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        anchors.topMargin: 14
        height: 30
        driveLabel: root.hub ? root.hub.driveLabel : ""
        medium: root.hub ? root.hub.mediumText : ""
        sizeText: root.hub ? root.hub.sizeText : ""
        stateText: root.hub ? root.hub.stateText : ""
        codecText: root.hub ? root.hub.codecText : ""
        engineText: root.hub ? root.hub.engineText : ""
    }

    // --- stage: rail + hero + peek ----------------------------------------
    Item {
        id: stage
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: status.bottom
        anchors.topMargin: 18
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        height: heroBlock.height + 8

        CategoryRail {
            id: rail
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 264
            model: root.hub ? root.hub.categories : []
            current: root.hub ? root.hub.currentCategory : 0
            onSelected: function (index) {
                if (root.hub && root.hub.currentCategory === index) return;
                if (root.hub) root.hub.currentCategory = index;
                root.chime("navigate");
            }
        }

        Item {
            id: heroBlock
            anchors.left: rail.right
            anchors.leftMargin: 26
            anchors.right: parent.right
            anchors.top: parent.top
            height: hero.height + 12 + pills.height

            HeroCard {
                id: hero
                width: 560
                height: width * 9 / 16
                artSource: root.hub ? root.hub.heroArt : ""
                title: root.hub ? root.hub.heroTitle : ""
                meta: root.hub ? root.hub.heroMeta : ""
                kind: root.hub
                      ? root.hub.mediumText.replace("-ROM", "").replace("-", " ")
                      : ""
                badges: root.hub ? root.hub.heroBadges : []
                onActivated: { root.chime("select"); root.playRequested() }
            }

            PeekCard {
                id: peek
                anchors.left: hero.right
                anchors.leftMargin: 26
                anchors.top: parent.top
                width: 322
                height: width * 9 / 16
                artSource: root.hub ? root.hub.peekArt : ""
                title: root.hub ? root.hub.peekTitle : ""
                meta: root.hub ? root.hub.peekMeta : ""
                kind: root.hub ? root.hub.peekKind : ""
                slot: root.hub ? root.hub.peekSlot : ""
                cta: root.hub ? root.hub.peekCta : ""
                onActivated: { root.chime("select"); root.chaptersRequested() }
            }

            Row {
                id: pills
                anchors.left: hero.left
                anchors.top: hero.bottom
                anchors.topMargin: 12
                spacing: 8

                ActionPill {
                    text: "Play Disc"
                    icon: "play"
                    primary: true
                    focused: root.hub ? root.hub.focusIndex === 0 : true
                    onClicked: { root.chime("select"); root.playRequested() }
                }
                ActionPill {
                    text: "Disc Menu"
                    icon: "menu-book"
                    focused: root.hub ? root.hub.focusIndex === 1 : false
                    onClicked: { root.chime("select"); root.discMenuRequested() }
                }
                ActionPill {
                    text: root.hub ? root.hub.chapterPillText : "Chapters"
                    icon: "chapters"
                    focused: root.hub ? root.hub.focusIndex === 2 : false
                    onClicked: { root.chime("select"); root.chaptersRequested() }
                }
                ActionPill {
                    text: "Options"
                    icon: "options"
                    focused: root.hub ? root.hub.focusIndex === 3 : false
                    onClicked: { root.chime("select"); root.optionsRequested() }
                }
            }
        }
    }

    // --- single WMC bottom row --------------------------------------------
    Item {
        id: bottomRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        anchors.bottomMargin: 30
        height: 62

        MiniPlayer {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 348
            height: 72
            artSource: root.hub ? root.hub.heroArt : ""
            stateText: root.hub ? root.hub.nowStateText : "READY"
            detail: root.hub ? root.hub.nowDetail : ""
            title: root.hub ? root.hub.heroTitle : ""
            positionText: root.player ? root.player.positionText : "0:00"
            durationText: root.player ? root.player.durationText : "0:00"
            active: root.player ? root.player.playing : false
        }

        TransportPill {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            playing: root.player ? root.player.playing : false
            volume: root.player ? root.player.volume : 80
            muted: root.player ? root.player.muted : false
            onTogglePlay: { root.chime("select"); root.playRequested() }
            onStop: { root.chime("back"); if (root.player) root.player.stop() }
            onSkipPrev: { root.chime("navigate"); if (root.player) root.player.seekChapter(0) }
            onSkipNext: { root.chime("navigate"); if (root.player) root.player.skip(30) }
            onRewind: { root.chime("navigate"); if (root.player) root.player.skip(-15) }
            onForward: { root.chime("navigate"); if (root.player) root.player.skip(15) }
        }
    }
}
