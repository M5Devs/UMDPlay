import QtQuick

// Vertical category rail in the WMC idiom: oversized light-weight type, the
// selected entry blooming into a glass card with a cyan notch and glow dot.
Item {
    id: root

    property var model: []            // [{ title, sub }]
    property int current: 0
    property int activeIndex: -1      // index rendered as "ACTIVE LIBRARY"

    signal selected(int index)

    implicitWidth: 268

    Column {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        spacing: 2

        Repeater {
            model: root.model

            delegate: Item {
                id: slot
                required property int index
                required property var modelData
                readonly property bool active: index === root.current

                width: root.width
                height: active ? 82 : 35

                Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

                // Inactive: plain oversized label.
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !slot.active
                    text: slot.modelData.title
                    color: Theme.onSurface
                    opacity: hover.containsMouse ? 0.8 : 0.4
                    font.family: Theme.fontFamily
                    font.pixelSize: 22
                    font.weight: Font.Light
                    font.letterSpacing: -0.3
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                }

                // Active: glass card with the notch and the glowing dot.
                Item {
                    anchors.fill: parent
                    visible: slot.active
                    opacity: slot.active ? 1 : 0

                    GlassPanel {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        anchors.topMargin: 3
                        anchors.bottomMargin: 3
                        radius: Theme.radiusLg
                        fill: "#661F2A42"
                        glow: true
                    }

                    Rectangle {
                        x: 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: 4
                        height: 38
                        radius: 2
                        color: Theme.cyan
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -4
                            radius: 6
                            color: "#3300D2FF"
                        }
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 22
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0

                        Text {
                            text: "ACTIVE LIBRARY"
                            color: Theme.cyanPale
                            font.family: Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Font.Bold
                            font.letterSpacing: 1.8
                            font.capitalization: Font.AllUppercase
                        }
                        Text {
                            text: slot.modelData.title
                            color: "#FFFFFF"
                            font.family: Theme.fontFamily
                            font.pixelSize: 28
                            font.weight: Font.Bold
                            font.letterSpacing: -0.6
                        }
                        Text {
                            text: slot.modelData.sub
                            color: Theme.onSurfaceVariant
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.labelMd
                        }
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7
                        height: 7
                        radius: 4
                        color: Theme.cyan
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -4
                            radius: 8
                            color: "#5500D2FF"
                        }
                    }
                }

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selected(slot.index)
                }
            }
        }
    }

    // Remote hint, as in the export.
    Row {
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.bottom: parent.bottom
        spacing: 8
        opacity: 0.55

        Rectangle {
            width: 30
            height: 16
            radius: 4
            color: Theme.container
            Row {
                anchors.centerIn: parent
                spacing: 2
                Icon { name: "chevron-left"; size: 9; color: Theme.cyanPale; weight: 2.2 }
                Icon { name: "chevron-right"; size: 9; color: Theme.cyanPale; weight: 2.2 }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Use D-Pad to scrub titles"
            color: Theme.onSurfaceVariant
            font.family: Theme.fontFamily
            font.pixelSize: Theme.labelSm
        }
    }
}
