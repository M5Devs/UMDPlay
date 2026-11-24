// Transport overlay: glass now-playing bar, scrubber and circular controls.
// Binds to an AveraPlayerController; all interaction goes through its
// invokables.
import QtQuick

Item {
    id: osd

    property var player: null
    property bool shown: player ? player.osdVisible : false

    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

    function fraction() {
        if (!player || player.duration <= 0) return 0
        return Math.max(0, Math.min(1, player.position / player.duration))
    }

    // Bottom scrim so the chrome stays readable over any picture.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: parent.height * 0.55
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#CC030D24" }
        }
    }

    GlassPanel {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 26
        height: 96
        radius: Theme.radiusXl
        fill: "#D90A1730"

        Column {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.topMargin: 12
            anchors.bottomMargin: 10
            spacing: 6

            Row {
                width: parent.width
                spacing: 10

                Badge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: osd.player && osd.player.playing ? "Playing" : "Paused"
                    strong: true
                    fontSize: 8
                }

                Text {
                    id: osdTitle
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - 300)
                    text: osd.player ? osd.player.title : ""
                    color: "#FFFFFF"
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.titleLg
                    font.weight: Font.Bold
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 200
                    horizontalAlignment: Text.AlignRight
                    text: osd.player ? osd.player.subtitle : ""
                    color: Theme.onSurfaceVariant
                    elide: Text.ElideRight
                    font.family: Theme.monoFamily
                    font.pixelSize: 9
                }
            }

            Item {
                width: parent.width
                height: 16

                Rectangle {
                    id: track
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 5
                    radius: 3
                    color: "#26FFFFFF"
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(6, track.width * osd.fraction())
                    height: 5
                    radius: 3
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.azureBright }
                        GradientStop { position: 1.0; color: Theme.cyan }
                    }
                }

                Rectangle {
                    x: Math.max(0, Math.min(track.width - 12, track.width * osd.fraction() - 6))
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    radius: 6
                    color: "#FFFFFF"
                    border.width: 2
                    border.color: Theme.cyan
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: function (mouse) {
                        if (osd.player) osd.player.seekFraction(mouse.x / width)
                    }
                }
            }

            Item {
                width: parent.width
                height: 26

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: osd.player ? osd.player.positionText : "0:00"
                        color: Theme.cyanPale
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "/"
                        color: Theme.outline
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: osd.player ? osd.player.durationText : "0:00"
                        color: Theme.onSurfaceVariant
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                    }
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Repeater {
                        model: [
                            { icon: "camera", act: "snapshot", size: 30 },
                            { icon: "ab", act: "abloop", size: 30 },
                            { icon: "skip-prev", act: "chapter", size: 30 },
                            { icon: "rewind", act: "back", size: 30 },
                            { icon: "play", act: "toggle", size: 38 },
                            { icon: "forward", act: "fwd", size: 30 },
                            { icon: "fullscreen", act: "full", size: 30 }
                        ]
                        delegate: Item {
                            required property var modelData
                            width: modelData.size
                            height: modelData.size

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: modelData.act === "toggle" ? "transparent"
                                    : modelData.act === "abloop" && osd.player
                                        && osd.player.abLoopState === "looping"
                                        ? Theme.cyan
                                        : (ma.containsMouse ? Theme.containerHigh : "#8C141F37")
                                border.width: modelData.act === "toggle" ? 0 : 1
                                border.color: "#2EFFFFFF"
                            }

                            Rectangle {
                                visible: modelData.act === "toggle"
                                anchors.fill: parent
                                radius: width / 2
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: Theme.cyan }
                                    GradientStop { position: 0.55; color: "#0A7BE8" }
                                    GradientStop { position: 1.0; color: Theme.azure }
                                }
                            }

                            Icon {
                                anchors.centerIn: parent
                                name: modelData.act === "toggle"
                                    ? (osd.player && osd.player.playing ? "pause" : "play")
                                    : modelData.icon
                                size: modelData.act === "toggle" ? 18 : 14
                                color: modelData.act === "toggle" ? "#FFFFFF"
                                    : modelData.act === "abloop" && osd.player
                                        && osd.player.abLoopState === "looping"
                                        ? "#003543" : Theme.onSurface
                                weight: 2
                            }

                            MouseArea {
                                id: ma
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (!osd.player) return
                                    osd.player.pokeOsd()
                                    switch (modelData.act) {
                                    case "toggle": osd.player.togglePlay(); break
                                    case "chapter": osd.player.seekChapter(0); break
                                    case "back": osd.player.skip(-15); break
                                    case "fwd": osd.player.skip(15); break
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
