// In-player settings drawer: the VLC/MPC-HC live tweaks, all applied while the
// picture keeps rolling — deinterlace policy, speed, audio sync nudge, aspect,
// brightness/contrast/saturation, snapshot and the A-B loop.
import QtQuick

GlassPanel {
    id: root

    property var player: null

    signal chime(string cue)

    readonly property var deintModes: ["smart", "off", "blend"]
    readonly property var deintLabels: ["Smart", "Off", "Blend"]
    readonly property var aspects: ["auto", "16:9", "3:2", "fill", "stretch"]
    readonly property var aspectLabels: ["Auto (4:3)", "16:9", "3:2", "Fill", "Stretch"]
    readonly property var speeds: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]

    implicitWidth: 700
    implicitHeight: 396
    radius: Theme.radiusXl
    fill: "#EB0A1730"

    // --- shared bits -------------------------------------------------------
    component SectionTitle: Row {
        spacing: 8
        property string text: ""
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "options"
            size: 13
            color: Theme.cyan
            weight: 1.6
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.text
            color: Theme.onSurface
            font.family: Theme.fontFamily
            font.pixelSize: Theme.titleMd
            font.weight: Font.DemiBold
        }
    }

    component Choice: Rectangle {
        id: choice
        property string label: ""
        property bool active: false
        signal picked()
        width: Math.max(40, text.implicitWidth + 16)
        height: 28
        radius: Theme.radius
        color: choice.active ? Theme.cyan : "#1AFFFFFF"
        border.width: 1
        border.color: choice.active ? Theme.cyan : Theme.hairline

        Text {
            id: text
            anchors.centerIn: parent
            text: choice.label
            color: choice.active ? "#003543" : Theme.onSurfaceVariant
            font.family: Theme.fontFamily
            font.pixelSize: Theme.labelMd
            font.weight: choice.active ? Font.Bold : Font.Medium
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.chime("select"); choice.picked() }
        }
    }

    Row {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 26

        // --- left: transport-ish -------------------------------------------
        Column {
            width: 318
            spacing: 8

            SectionTitle { text: "Playback" }

            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Speed \u00d7"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Repeater {
                        model: root.speeds
                        delegate: Choice {
                            required property var modelData
                            label: "" + modelData
                            active: root.player
                                ? Math.abs(root.player.speed - modelData) < 0.001 : false
                            onPicked: root.player.speed = modelData
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Audio delay"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Choice { label: "\u2212"; active: false; onPicked: root.player.nudgeAudioDelay(-1) }
                    Text {
                        width: 78
                        height: 28
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: root.player ? root.player.audioDelayText : "0 ms"
                        color: Theme.cyanPale
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                    }
                    Choice { label: "+"; active: false; onPicked: root.player.nudgeAudioDelay(1) }
                    Choice {
                        label: "Reset"
                        active: false
                        onPicked: root.player.audioDelayMs = 0
                    }
                }
            }

            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Aspect"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Repeater {
                        model: root.aspects
                        delegate: Choice {
                            required property var modelData
                            label: root.aspectLabels[modelData === "auto" ? 0
                                : modelData === "16:9" ? 1
                                : modelData === "3:2" ? 2
                                : modelData === "fill" ? 3 : 4]
                            active: root.player ? root.player.aspectMode === modelData : false
                            onPicked: root.player.aspectMode = modelData
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "A-B loop"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        height: 28
                        verticalAlignment: Text.AlignVCenter
                        text: root.player ? (root.player.abLoopText || "off") : ""
                        color: root.player && root.player.abLoopState === "looping"
                            ? Theme.cyanPale : Theme.onSurfaceVariant
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                    }
                    Choice {
                        label: root.player && root.player.abLoopState === "off" ? "Set A"
                             : root.player && root.player.abLoopState === "a" ? "Set B"
                             : "Clear"
                        active: root.player ? root.player.abLoopState === "looping" : false
                        onPicked: root.player.cycleAbLoop()
                    }
                }
            }

            Item {
                width: parent.width
                height: 30
                Choice {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Snapshot"
                    active: false
                    onPicked: root.player.takeSnapshot()
                }
                Text {
                    x: 104
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Shift+S"
                    color: Theme.outline
                    font.family: Theme.monoFamily
                    font.pixelSize: 9
                }
            }
        }

        // --- right: picture ------------------------------------------------
        Column {
            width: parent.width - 344
            spacing: 8

            SectionTitle { text: "Picture" }

            Row {
                spacing: 4
                Text {
                    width: 78
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: "Deinterlace"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                }
                Repeater {
                    model: root.deintModes
                    delegate: Choice {
                        required property var modelData
                        label: root.deintLabels[modelData === "smart" ? 0
                            : modelData === "off" ? 1 : 2]
                        active: root.player ? root.player.deinterlaceMode === modelData : false
                        onPicked: root.player.deinterlaceMode = modelData
                    }
                }
            }

            SettingSlider {
                width: parent.width
                label: "Brightness"
                from: -1.0; to: 1.0
                value: root.player ? root.player.brightness : 0.0
                defaultValue: 0.0
                valueText: (root.player ? Math.round(root.player.brightness * 100) : 0) + "%"
                onMoved: function (v) { if (root.player) root.player.brightness = v }
            }
            SettingSlider {
                width: parent.width
                label: "Contrast"
                from: 0.0; to: 2.0
                value: root.player ? root.player.contrast : 1.0
                defaultValue: 1.0
                valueText: (root.player ? Math.round(root.player.contrast * 100) : 100) + "%"
                onMoved: function (v) { if (root.player) root.player.contrast = v }
            }
            SettingSlider {
                width: parent.width
                label: "Saturation"
                from: 0.0; to: 2.0
                value: root.player ? root.player.saturation : 1.0
                defaultValue: 1.0
                valueText: (root.player ? Math.round(root.player.saturation * 100) : 100) + "%"
                onMoved: function (v) { if (root.player) root.player.saturation = v }
            }

            Row {
                spacing: 6
                Choice {
                    label: "Reset picture"
                    active: root.player ? root.player.effectsActive : false
                    onPicked: root.player.resetVideoEffects()
                }
                Text {
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: root.player && root.player.effectsActive ? "modified" : "neutral"
                    color: Theme.outline
                    font.family: Theme.monoFamily
                    font.pixelSize: 9
                }
            }
        }
    }

    // Close hint, bottom-right of the panel.
    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 14
        text: "S / Esc to close"
        color: Theme.outline
        font.family: Theme.monoFamily
        font.pixelSize: 9
    }
}
