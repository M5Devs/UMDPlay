import QtQuick

// Aero glass container: a two-stop specular gradient rim (white at the top edge
// fading to cyan at the bottom) wrapped around a translucent navy fill, with a
// top hairline that reads as the light-catching cut of the glass.
Item {
    id: root

    property real radius: Theme.radiusLg
    property color fill: Theme.glass
    property color edgeTop: Theme.glassEdgeTop
    property color edgeBottom: Theme.glassEdgeBottom
    property bool hairline: true
    property bool glow: false

    default property alias content: inner.data

    // Outer glow, drawn as a soft rounded halo behind the rim.
    Rectangle {
        visible: root.glow
        anchors.fill: rim
        anchors.margins: -3
        radius: root.radius + 3
        color: "transparent"
        border.width: 3
        border.color: "#3300D2FF"
    }

    Rectangle {
        id: rim
        anchors.fill: parent
        radius: root.radius
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.edgeTop }
            GradientStop { position: 1.0; color: root.edgeBottom }
        }
    }

    Rectangle {
        id: inner
        anchors.fill: parent
        anchors.margins: 1
        radius: Math.max(0, root.radius - 1)
        color: root.fill
    }

    Rectangle {
        visible: root.hairline
        x: root.radius
        y: 1
        width: Math.max(0, parent.width - root.radius * 2)
        height: 1
        color: Theme.hairline
    }
}
