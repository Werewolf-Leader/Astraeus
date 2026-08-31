pragma Singleton

import QtQuick

// Shared visual theme for the Boreas UI (G-Helper-inspired dark restyle).
//
// A QML singleton so every component references one source of truth for colors,
// spacing, radii, and fonts. Registered in the BoreasApp module via a
// `singleton` line in the generated qmldir (see CMakeLists.txt set_source_files
// QT_QML_SINGLETON_TYPE). Styling only: no hardware/behavior changes.
QtObject {
    id: theme

    // ---- Surfaces --------------------------------------------------------
    readonly property color background:  "#12141a"   // window backdrop
    readonly property color surface:     "#1a1d26"   // card background
    readonly property color surfaceAlt:  "#222634"   // raised / hover
    readonly property color border:      "#2c3040"   // hairline separators

    // ---- Text ------------------------------------------------------------
    readonly property color text:        "#eef1f7"
    readonly property color textMuted:   "#8b90a0"
    readonly property color textFaint:   "#5b6070"

    // ---- Accent / status -------------------------------------------------
    readonly property color accent:      "#4c8dff"   // primary accent (selection)
    readonly property color accentSoft:  "#2a3960"   // accent-tinted fill
    readonly property color success:     "#3ecf8e"
    readonly property color warning:     "#ffb454"
    readonly property color danger:      "#ff5c5c"

    // Per-profile accent tint (segmented profile buttons).
    function profileColor(name) {
        if (name === "Quiet")       return "#3ecf8e";
        if (name === "Balanced")    return "#4c8dff";
        if (name === "Performance") return "#ff7a59";
        return theme.accent;
    }

    // ---- Metrics ---------------------------------------------------------
    readonly property int radius:      10
    readonly property int radiusSmall: 6
    readonly property int gap:         12
    readonly property int gapSmall:    8
    readonly property int pad:         14

    // ---- Type ------------------------------------------------------------
    readonly property int fontSmall:  11
    readonly property int fontBody:   13
    readonly property int fontTitle:  15
    readonly property int fontMetric: 22
}
