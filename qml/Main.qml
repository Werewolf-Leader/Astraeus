import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Main application shell — G-Helper-inspired compact dark restyle.
//
// Layout:
//   - A header card with the app name, always-visible live CPU/GPU temperature
//     readouts, and the segmented power-profile selector (ProfileSelector).
//   - A flat pill tab strip switching between the Performance and Limits pages.
//   - A shared, collapsible StatusLogPanel pinned at the bottom.
//
// All data still flows through the `hardwareManager` context object (Req 1.1)
// and the app still loads via loadFromModule("BoreasApp", "Main"). This is a
// styling/layout change only; the underlying bindings and behavior are reused.
ApplicationWindow {
    id: window

    width: 460
    height: 720
    minimumWidth: 420
    minimumHeight: 560
    visible: true
    title: qsTr("Boreas")
    color: Theme.background

    property alias statusLog: statusLogPanel

    // Poll temperatures for the header readout regardless of which page is
    // shown, so the header stays live. The Limits page also drives polling; the
    // HardwareManager timer is idempotent (start-if-not-running).
    Component.onCompleted: {
        if (typeof hardwareManager !== "undefined")
            hardwareManager.setTemperaturePollingActive(true);
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: Theme.gap

        // ================= Header =================
        Card {
            Layout.fillWidth: true

            // Title row + live temps.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                ColumnLayout {
                    spacing: 0
                    Text {
                        text: qsTr("BOREAS")
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle + 3
                        font.bold: true
                        font.letterSpacing: 2
                    }
                    Text {
                        text: qsTr("Operator Control")
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Item { Layout.fillWidth: true }

                // Live CPU/GPU temperature chips (— when unavailable).
                Repeater {
                    model: [
                        { label: qsTr("CPU"), get: "cpu" },
                        { label: qsTr("GPU"), get: "gpu" }
                    ]
                    delegate: ColumnLayout {
                        required property var modelData
                        spacing: 0
                        Layout.alignment: Qt.AlignVCenter

                        readonly property real _t: {
                            if (typeof hardwareManager === "undefined")
                                return NaN;
                            return modelData.get === "cpu"
                                   ? hardwareManager.cpuTemperature
                                   : hardwareManager.gpuTemperature;
                        }

                        Text {
                            text: modelData.label
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontSmall
                            horizontalAlignment: Text.AlignRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: isNaN(parent._t)
                                  ? qsTr("\u2014")
                                  : parent._t.toFixed(0) + qsTr("\u00B0")
                            color: isNaN(parent._t) ? Theme.textFaint
                                                    : (parent._t >= 85 ? Theme.danger
                                                       : parent._t >= 70 ? Theme.warning
                                                       : Theme.text)
                            font.pixelSize: Theme.fontMetric
                            font.bold: true
                            horizontalAlignment: Text.AlignRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // Segmented profile selector.
            ProfileSelector {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gapSmall
            }
        }

        // ================= Pill tab strip =================
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gapSmall

            Repeater {
                id: tabRepeater
                property int current: 0
                model: [qsTr("Performance"), qsTr("Limits")]

                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    readonly property bool current: tabRepeater.current === index

                    Layout.fillWidth: true
                    implicitHeight: 36
                    radius: Theme.radiusSmall
                    color: current ? Theme.accentSoft
                                   : (tabMouse.containsMouse ? Theme.surfaceAlt : Theme.surface)
                    border.color: current ? Theme.accent : Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: parent.current ? Theme.text : Theme.textMuted
                        font.pixelSize: Theme.fontBody
                        font.bold: parent.current
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: tabRepeater.current = parent.index
                    }
                }
            }
        }

        // ================= Page content =================
        StackLayout {
            id: pageStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabRepeater.current

            PerformanceTab {
                id: performanceTab
                // Kept visible-driven refresh working: StackLayout toggles
                // visibility of the current child.
            }

            LimitsTab {
                id: limitsTab
            }
        }

        // ================= Shared status log =================
        StatusLogPanel {
            id: statusLogPanel
            Layout.fillWidth: true
        }
    }
}
