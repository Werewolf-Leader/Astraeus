pragma Singleton

import QtQuick

// Shared visual theme for the Boreas UI.
//
// A QML singleton so every component references one source of truth for colors,
// spacing, radii, and fonts. Registered in the BoreasApp module via a
// `singleton` line in the generated qmldir (see CMakeLists.txt set_source_files
// QT_QML_SINGLETON_TYPE). Styling only: no hardware/behavior changes.
QtObject {
    id: theme

    // ---- Surfaces --------------------------------------------------------
    readonly property color background:  "#111111"
    readonly property color surface:     "#191919"
    readonly property color surfaceAlt:  "#242424"
    readonly property color border:      "#3a3a3a"

    // ---- Text ------------------------------------------------------------
    readonly property color text:        "#f2f2f2"
    readonly property color textMuted:   "#aaaaaa"
    readonly property color textFaint:   "#707070"

    // ---- Accent / status -------------------------------------------------
    readonly property color accent:      "#d8d8d8"
    readonly property color accentSoft:  "#333333"
    readonly property color success:     "#d8d8d8"
    readonly property color warning:     "#bdbdbd"
    readonly property color danger:      "#ffffff"

    // Keep profile selection monochrome; the label carries the meaning.
    function profileColor(name) {
        return theme.accent;
    }

    // ---- Metrics ---------------------------------------------------------
    readonly property int radius:      2
    readonly property int radiusSmall: 2
    readonly property int gap:         12
    readonly property int gapSmall:    8
    readonly property int pad:         14

    // ---- Type ------------------------------------------------------------
    readonly property int fontSmall:  11
    readonly property int fontBody:   13
    readonly property int fontTitle:  15
    readonly property int fontMetric: 22
}
