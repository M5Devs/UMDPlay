pragma Singleton

// AveraUI design tokens. One place to retheme the whole shell.
import QtQuick

QtObject {
    // Surfaces
    readonly property color bg:          "#070b10"
    readonly property color bgTop:       "#0e1620"
    readonly property color bgBottom:    "#05080c"
    readonly property color glass:       "#18222e"
    readonly property color glassFocus:  "#243547"
    readonly property color glassEdge:   "#2b3d51"

    // Ink
    readonly property color text:        "#eef4fb"
    readonly property color textDim:     "#93a6ba"
    readonly property color textFaint:   "#5d7086"

    // Accent
    readonly property color accent:      "#4cc2ff"
    readonly property color accentDeep:  "#1f7fb5"
    readonly property color accentGlow:  "#804cc2ff"
    readonly property color warn:        "#ffb454"

    // Metrics
    readonly property int   radius:      14
    readonly property int   cardWidth:   272
    readonly property int   cardHeight:  130
    readonly property int   rowSpacing:  18
    readonly property int   cardSpacing: 18
    readonly property int   gutter:      64

    // Type
    readonly property font display: Qt.font({ pixelSize: 30, weight: Font.DemiBold,
                                              letterSpacing: 0.4 })
    readonly property font rowTitle: Qt.font({ pixelSize: 17, weight: Font.DemiBold,
                                               letterSpacing: 2.2, capitalization: Font.AllUppercase })
    readonly property font cardTitle: Qt.font({ pixelSize: 17, weight: Font.Medium })
    readonly property font cardSub: Qt.font({ pixelSize: 13, weight: Font.Normal })
    readonly property font mono: Qt.font({ pixelSize: 14, family: "monospace" })
}
