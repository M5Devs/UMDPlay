import QtQuick

// Disc-tray telemetry: the drive/medium/size/state pill on the left and the
// decode-engine readout on the right. Every field is real disc data.
Item {
    id: root

    property string driveLabel: "DRIVE 1 [ISO]"
    property string medium: "UMD-VIDEO-ROM"
    property string sizeText: ""
    property string stateText: "READY"
    property string codecText: ""
    property string engineText: ""

    implicitHeight: 30

    GlassPanel {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: leftRow.implicitWidth + 28
        height: 30
        radius: 15
        fill: "#B3101B32"

        Row {
            id: leftRow
            anchors.centerIn: parent
            spacing: 8

            Item {
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                Rectangle {
                    anchors.centerIn: parent
                    width: 14
                    height: 14
                    radius: 7
                    color: "#3300D2FF"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 6
                    height: 6
                    radius: 3
                    color: Theme.cyan
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.driveLabel
                color: Theme.onSurface
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelMd
                font.weight: Font.DemiBold
                font.letterSpacing: 0.4
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "\u2022"
                color: Theme.outline
                font.pixelSize: 9
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.medium
                color: Theme.cyanPale
                font.family: Theme.fontFamily
                font.pixelSize: Theme.labelMd
                font.weight: Font.Medium
                font.letterSpacing: 0.6
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "\u2022"
                color: Theme.outline
                font.pixelSize: 9
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.sizeText
                color: Theme.onSurfaceVariant
                font.family: Theme.monoFamily
                font.pixelSize: Theme.labelMd
            }
            Badge {
                anchors.verticalCenter: parent.verticalCenter
                text: root.stateText
                strong: true
                fontSize: 8
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        GlassPanel {
            width: codecRow.implicitWidth + 22
            height: 30
            radius: 15
            fill: "#99141F37"
            Row {
                id: codecRow
                anchors.centerIn: parent
                spacing: 6
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "CODEC:"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelSm
                    font.letterSpacing: 0.8
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.codecText
                    color: Theme.cyan
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelMd
                    font.weight: Font.DemiBold
                }
            }
        }

        GlassPanel {
            width: engineRow.implicitWidth + 22
            height: 30
            radius: 15
            fill: "#99141F37"
            Row {
                id: engineRow
                anchors.centerIn: parent
                spacing: 6
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "ENGINE:"
                    color: Theme.onSurfaceVariant
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.labelSm
                    font.letterSpacing: 0.8
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.engineText
                    color: Theme.onSurface
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.labelMd
                }
            }
        }
    }
}
