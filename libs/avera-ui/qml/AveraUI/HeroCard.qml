import QtQuick

// The focused disc: 16:9 glass frame over the disc's own PIC1.PNG, with the
// specular sheen, the inserted-disc chrome and the metadata scrim.
Item {
    id: root

    property string artSource: ""
    property string title: ""
    property string meta: ""
    property string topBadge: "INSERTED DISC"
    property string kind: "UMD VIDEO"
    property var badges: []          // [{ text, strong }]
    property bool focused: true

    signal activated()

    // Bioluminescent bloom: three offset halos approximate the export's
    // `0 0 36px rgba(0,210,255,0.4)` shadow without a blur pass.
    Repeater {
        model: [ { m: 1, a: 0.45 }, { m: 4, a: 0.22 }, { m: 8, a: 0.12 } ]
        delegate: Rectangle {
            required property var modelData
            visible: root.focused
            anchors.fill: parent
            anchors.margins: -modelData.m
            radius: Theme.radiusXl + modelData.m
            color: "transparent"
            border.width: 2
            border.color: Qt.rgba(0 / 255, 210 / 255, 255 / 255, modelData.a)
        }
    }

    GlassPanel {
        anchors.fill: parent
        radius: Theme.radiusXl
        fill: Theme.containerHigh
        edgeTop: root.focused ? "#B300D2FF" : Theme.glassEdgeTop
        edgeBottom: root.focused ? "#5500D2FF" : Theme.glassEdgeBottom
        glow: root.focused
    }

    Rectangle {
        id: clip
        anchors.fill: parent
        anchors.margins: 2
        radius: Theme.radiusXl - 2
        color: Theme.containerLow
        clip: true

        Image {
            id: art
            anchors.fill: parent
            source: root.artSource
            fillMode: Image.PreserveAspectCrop
            smooth: true
            asynchronous: true
            visible: status === Image.Ready
        }

        // Placeholder when the disc ships no PIC1.PNG.
        Rectangle {
            anchors.fill: parent
            visible: art.status !== Image.Ready
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.containerHigh }
                GradientStop { position: 1.0; color: Theme.canvasDeep }
            }
            Icon {
                anchors.centerIn: parent
                name: "disc"
                size: 54
                color: "#5500D2FF"
                weight: 1.4
            }
        }

        // Specular sheen: diagonal white wash plus the top-left highlight band.
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#4DFFFFFF" }
                GradientStop { position: 0.35; color: "#0DFFFFFF" }
                GradientStop { position: 1.0; color: "#00FFFFFF" }
            }
        }
        Rectangle {
            x: -parent.width * 0.25
            y: -parent.height * 0.35
            width: parent.width * 1.2
            height: parent.height * 0.42
            rotation: -12
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#40FFFFFF" }
                GradientStop { position: 1.0; color: "#00FFFFFF" }
            }
        }

        // Scrim so the metadata stays legible over any artwork.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * 0.62
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00030D24" }
                GradientStop { position: 0.55; color: "#B3030D24" }
                GradientStop { position: 1.0; color: "#F2030D24" }
            }
        }

        // Top chrome.
        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            height: 22

            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: inserted.implicitWidth + 26
                height: 22
                radius: 11
                color: "#CC030D24"
                Row {
                    id: inserted
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        color: Theme.cyan
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.topBadge
                        color: Theme.cyanPale
                        font.family: Theme.fontFamily
                        font.pixelSize: 8
                        font.weight: Font.Bold
                        font.letterSpacing: 1.2
                        font.capitalization: Font.AllUppercase
                    }
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: kindRow.implicitWidth + 22
                height: 22
                radius: 11
                color: "#CC030D24"
                Row {
                    id: kindRow
                    anchors.centerIn: parent
                    spacing: 5
                    Icon { name: "disc"; size: 12; color: Theme.cyanSoft; weight: 2 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.kind
                        color: Theme.onSurface
                        font.family: Theme.fontFamily
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                        font.capitalization: Font.AllUppercase
                    }
                }
            }
        }

        // Bottom metadata block.
        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            spacing: 4

            Row {
                spacing: 6
                Repeater {
                    model: root.badges
                    delegate: Badge {
                        required property var modelData
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.text
                        strong: modelData.strong === true
                        fontSize: 8
                    }
                }
            }

            Text {
                width: parent.width
                text: root.title
                color: "#FFFFFF"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 27
                font.weight: Font.Bold
                font.letterSpacing: -0.5
            }

            Text {
                width: parent.width
                text: root.meta
                color: Theme.onSurfaceVariant
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelMd
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.activated()
    }
}
