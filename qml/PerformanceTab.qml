import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Performance page (G-Helper-style restyle).
//
// The power-profile segmented selector now lives in the Main header, so this
// page focuses on the GPU Mode selector (with disruptive-action confirmation)
// and the embedded Fan Curve Editor. All data flows through the
// `hardwareManager` context object; this page never writes hardware directly
// (writes go through the HardwareManager invokables and are reflected via the
// applyResult signal). Behavior and confirmation gating are unchanged from the
// original implementation — only the presentation is restyled.
Item {
    id: performanceTab

    // ---- Capability lookups (guarded) -------------------------------------
    readonly property var _caps: (typeof hardwareManager !== "undefined")
                                 ? hardwareManager.capabilities : ({})
    readonly property var _gpuCap: _caps && _caps["GpuMode"]
                                   ? _caps["GpuMode"] : ({})
    readonly property bool gpuModeSupported: _gpuCap.supported === true
    readonly property string gpuModeReason:
        _gpuCap.reason !== undefined ? _gpuCap.reason : ""

    readonly property var supportedGpuModes: (typeof hardwareManager !== "undefined")
                                             ? hardwareManager.supportedGpuModes : []
    readonly property bool gpuSelectorAvailable:
        gpuModeSupported && supportedGpuModes.length > 0

    // ---- Inline apply-result feedback -------------------------------------
    property bool lastApplyOk: true
    property string lastApplyMessage: ""

    // Refresh the active GPU mode when this page becomes visible.
    onVisibleChanged: {
        if (visible && typeof hardwareManager !== "undefined")
            hardwareManager.refreshGpuMode();
    }

    Connections {
        target: (typeof hardwareManager !== "undefined") ? hardwareManager : null
        function onApplyResult(success, message) {
            performanceTab.lastApplyOk = success;
            performanceTab.lastApplyMessage = message;
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.gap

            // ================= GPU Mode =================
            Card {
                title: qsTr("GPU Mode")

                // Unavailable reason.
                Text {
                    Layout.fillWidth: true
                    visible: !performanceTab.gpuSelectorAvailable
                    text: {
                        if (performanceTab.gpuModeReason.length > 0)
                            return performanceTab.gpuModeReason;
                        if (!performanceTab.gpuModeSupported)
                            return qsTr("supergfxctl D-Bus service not available");
                        return qsTr("No supported GPU modes reported");
                    }
                    color: Theme.warning
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gapSmall

                    ComboBox {
                        id: gpuModeCombo
                        Layout.fillWidth: true
                        enabled: performanceTab.gpuSelectorAvailable
                        model: performanceTab.supportedGpuModes

                        readonly property string activeMode:
                            (typeof hardwareManager !== "undefined")
                            ? hardwareManager.activeGpuMode : ""
                        currentIndex: model ? model.indexOf(activeMode) : -1
                        onActiveModeChanged: currentIndex = model ? model.indexOf(activeMode) : -1

                        onActivated: function (index) {
                            var chosen = model[index];
                            if (chosen === undefined || chosen === activeMode)
                                return;
                            gpuModeConfirmDialog.pendingMode = chosen;
                            gpuModeConfirmDialog.open();
                        }
                    }

                    Button {
                        text: qsTr("Refresh")
                        enabled: performanceTab.gpuModeSupported
                        onClicked: {
                            if (typeof hardwareManager !== "undefined")
                                hardwareManager.refreshGpuMode();
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: performanceTab.gpuSelectorAvailable
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    text: {
                        var active = (typeof hardwareManager !== "undefined")
                                     ? hardwareManager.activeGpuMode : "";
                        return active.length > 0
                               ? qsTr("Active: %1").arg(active)
                               : qsTr("Active GPU mode unknown");
                    }
                }

                // Inline write-outcome warning (failed apply only).
                Text {
                    Layout.fillWidth: true
                    visible: !performanceTab.lastApplyOk
                             && performanceTab.lastApplyMessage.length > 0
                    text: performanceTab.lastApplyMessage
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }

            // ================= Fan Curve =================
            FanCurveEditor {
                id: fanCurveEditor
                Layout.fillWidth: true
            }

            Item { Layout.fillWidth: true; Layout.fillHeight: true }
        }
    }

    // GPU mode switch confirmation dialog (Disruptive_Action). The accept
    // handler is the ONLY caller of applyGpuMode; cancel/failure reverts the
    // selector via its activeGpuMode binding.
    Dialog {
        id: gpuModeConfirmDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Confirm GPU Mode Switch")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape

        property string pendingMode: ""

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.gapSmall

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Switch GPU mode to \"%1\"?")
                        .arg(gpuModeConfirmDialog.pendingMode)
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                text: qsTr("This is a disruptive action that may require you to "
                           + "log out or reboot to complete.")
            }
        }

        onAccepted: {
            if (pendingMode.length > 0 && typeof hardwareManager !== "undefined")
                hardwareManager.applyGpuMode(pendingMode);
            pendingMode = "";
        }
        onRejected: {
            pendingMode = "";
            gpuModeCombo.currentIndex = gpuModeCombo.model
                ? gpuModeCombo.model.indexOf(gpuModeCombo.activeMode) : -1;
        }
    }
}
