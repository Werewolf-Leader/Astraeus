import QtQuick
import QtQuick.Layouts

// Segmented power-profile selector (G-Helper-style pill row).
//
// Offers exactly Quiet / Balanced / Performance, highlights the entry matching
// hardwareManager.activePowerProfile, and calls setPowerProfile on click. When
// capabilities["PowerProfile"] is unsupported the row is disabled and a reason
// is shown. Reuses the existing hardwareManager API only.
Item {
    id: profileSelector

    implicitHeight: column.implicitHeight

    readonly property var _caps: (typeof hardwareManager !== "undefined")
                                 ? hardwareManager.capabilities : ({})
    readonly property var _cap: _caps && _caps["PowerProfile"]
                                ? _caps["PowerProfile"] : ({})
    readonly property bool supported: _cap.supported === true
    readonly property string reason: _cap.reason !== undefined ? _cap.reason : ""
    readonly property string active: (typeof hardwareManager !== "undefined")
                                     ? hardwareManager.activePowerProfile : ""

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.gapSmall

        // Segmented pill row.
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            enabled: profileSelector.supported
            opacity: profileSelector.supported ? 1.0 : 0.5

            Repeater {
                model: ["Quiet", "Balanced", "Performance"]

                delegate: Rectangle {
                    id: seg
                    required property int index
                    required property string modelData

                    readonly property bool current: profileSelector.active === modelData
                    readonly property color tint: Theme.profileColor(modelData)

                    Layout.fillWidth: true
                    implicitHeight: 40
                    // Join the segments visually: only round the outer corners.
                    radius: Theme.radiusSmall
                    color: current ? tint
                                   : (segMouse.containsMouse ? Theme.surfaceAlt : Theme.surface)
                    border.color: current ? tint : Theme.border
                    border.width: 1

                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: seg.modelData
                        color: seg.current ? "#0d0f14" : Theme.text
                        font.pixelSize: Theme.fontBody
                        font.bold: seg.current
                    }

                    MouseArea {
                        id: segMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hardwareManager !== "undefined") {
                                hardwareManager.setPowerProfile(seg.modelData);
                                // Scope per-profile settings (fan curves, RyzenAdj)
                                // to the selected profile via the property setter.
                                hardwareManager.selectedProfile = seg.modelData;
                            }
                        }
                    }
                }
            }
        }

        // Unavailable reason.
        Text {
            Layout.fillWidth: true
            visible: !profileSelector.supported
            text: profileSelector.reason.length > 0
                  ? profileSelector.reason
                  : qsTr("Power profile control not available")
            color: Theme.warning
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
}
