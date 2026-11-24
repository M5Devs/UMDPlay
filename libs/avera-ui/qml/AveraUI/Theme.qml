pragma Singleton
import QtQuick

// Design tokens transcribed from the Stitch "Aero Oceanic Media Center" export
// (DESIGN.md + code.html). Sizes are the export's values rescaled from its
// 1920x1080 canvas to a 1280x720 10-foot viewport.
QtObject {
    // --- palette -----------------------------------------------------------
    readonly property color canvas: "#07132a"
    readonly property color canvasDeep: "#030d24"
    readonly property color containerLowest: "#030d24"
    readonly property color containerLow: "#101b32"
    readonly property color container: "#141f37"
    readonly property color containerHigh: "#1f2a42"
    readonly property color containerHighest: "#2a344d"

    readonly property color onSurface: "#d8e2ff"
    readonly property color onSurfaceVariant: "#bbc9cf"
    readonly property color outline: "#859399"
    readonly property color outlineVariant: "#3c494e"

    readonly property color cyan: "#00d2ff"        // primary-container
    readonly property color cyanSoft: "#47d6ff"    // primary-fixed-dim
    readonly property color cyanPale: "#a5e7ff"    // primary
    readonly property color azure: "#0053db"       // secondary-container
    readonly property color azureBright: "#2563eb"
    readonly property color cobalt: "#12376e"
    readonly property color ice: "#b4c9e8"

    // --- translucent glass ------------------------------------------------
    readonly property color glass: "#A60C2044"        // #0c2044 @ 65%
    readonly property color glassDeep: "#D908142B"    // #08142b @ 85%
    readonly property color glassChrome: "#CC101B32"
    readonly property color glassEdgeTop: "#66FFFFFF"
    readonly property color glassEdgeBottom: "#1A00D2FF"
    readonly property color hairline: "#2EFFFFFF"

    // --- radii -------------------------------------------------------------
    readonly property int radiusSm: 6
    readonly property int radius: 10
    readonly property int radiusLg: 14
    readonly property int radiusXl: 20

    // --- rhythm ------------------------------------------------------------
    readonly property int margin: 28
    readonly property int gutter: 22
    readonly property int headerHeight: 48

    // --- type --------------------------------------------------------------
    readonly property string fontFamily: "Plus Jakarta Sans"
    readonly property string monoFamily: "DejaVu Sans Mono"

    readonly property int headlineXl: 34
    readonly property int headlineLg: 26
    readonly property int headlineMd: 21
    readonly property int titleLg: 16
    readonly property int titleMd: 14
    readonly property int bodyMd: 12
    readonly property int labelLg: 12
    readonly property int labelMd: 11
    readonly property int labelSm: 9
}
