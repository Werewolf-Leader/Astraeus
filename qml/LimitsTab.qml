import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Limits page (G-Helper-style restyle).
//
// Section 1: Battery charge-limit + live capacity/status. Section 2: Advanced
// power tuning via RyzenAdj (STAPM/fast/slow/APU-slow limits + Tctl thermal
// limit). RyzenAdj owns both the power limits and the hardware temperature
// limit, so the old sysfs PL1/PL2 "Power Limiting" card and the soft-threshold
// "Temperatures" card were removed to avoid two backends fighting over the same
// knobs. Live CPU/GPU temperatures still show in the app header. All data flows
// through the `hardwareManager` context object.
Item {
    id: limitsTab

    property bool batteryApplyOk: true
    property string batteryApplyMessage: ""
    property bool ryzenAdjApplyOk: true
    property string ryzenAdjApplyMessage: ""
    // Which card initiated the last write, so the shared applyResult signal is
    // routed to the right card ("battery" | "ryzenadj" | "").
    property string _pendingApply: ""

    Connections {
        target: (typeof hardwareManager !== "undefined") ? hardwareManager : null
        function onApplyResult(success, message) {
            // Route the shared applyResult to the control that initiated the
            // last write so each card shows only its own outcome.
            if (limitsTab._pendingApply === "ryzenadj") {
                limitsTab.ryzenAdjApplyOk = success
                limitsTab.ryzenAdjApplyMessage = message
            } else {
                limitsTab.batteryApplyOk = success
                limitsTab.batteryApplyMessage = message
            }
            limitsTab._pendingApply = ""
        }
    }

    onVisibleChanged: {
        // Keep temperature polling alive for the header readout, and refresh
        // battery + RyzenAdj values when this tab is shown.
        if (typeof hardwareManager !== "undefined") {
            hardwareManager.setTemperaturePollingActive(visible)
            if (visible) {
                hardwareManager.refreshBattery()
                hardwareManager.refreshRyzenAdj()
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.gap

            // ================= Battery =================
            Card {
                id: batteryCard
                title: qsTr("Battery")

                readonly property var _caps: (typeof hardwareManager !== "undefined")
                                             ? hardwareManager.capabilities : ({})
                readonly property var _batCap: _caps && _caps["BatteryChargeLimit"]
                                               ? _caps["BatteryChargeLimit"] : ({})
                readonly property bool chargeLimitSupported: _batCap.supported === true
                readonly property string chargeLimitReason:
                    _batCap.reason !== undefined ? _batCap.reason : ""

                // Live capacity + status readout.
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap
                    rowSpacing: Theme.gapSmall

                    Text {
                        text: qsTr("Charge")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        text: (typeof hardwareManager === "undefined"
                               || hardwareManager.batteryCapacity < 0)
                              ? qsTr("\u2014")
                              : hardwareManager.batteryCapacity + qsTr("%")
                        font.bold: true
                        font.pixelSize: Theme.fontBody
                        color: Theme.text
                    }

                    Text {
                        text: qsTr("Status")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        text: (typeof hardwareManager === "undefined"
                               || hardwareManager.batteryStatus.length === 0)
                              ? qsTr("\u2014")
                              : hardwareManager.batteryStatus
                        font.pixelSize: Theme.fontBody
                        color: Theme.text
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.border
                }

                // Charge-limit unavailable reason.
                Text {
                    Layout.fillWidth: true
                    visible: !batteryCard.chargeLimitSupported
                    wrapMode: Text.WordWrap
                    color: Theme.warning
                    font.pixelSize: Theme.fontSmall
                    text: batteryCard.chargeLimitReason.length > 0
                          ? batteryCard.chargeLimitReason
                          : qsTr("Battery charge limit not available")
                }

                Text {
                    Layout.fillWidth: true
                    visible: batteryCard.chargeLimitSupported
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("Stop charging at this level to reduce long-term "
                               + "battery wear (20\u2013100%).")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gap
                    enabled: batteryCard.chargeLimitSupported

                    Text {
                        text: qsTr("Charge limit")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }

                    SpinBox {
                        id: chargeLimitSpinBox
                        from: 20
                        to: 100
                        stepSize: 5
                        editable: true
                        value: (typeof hardwareManager !== "undefined"
                                && hardwareManager.chargeLimit >= 20)
                               ? hardwareManager.chargeLimit : 100
                        textFromValue: function (value, locale) { return value + qsTr("%") }
                        valueFromText: function (text, locale) { return parseInt(text) }

                        // Keep the spinbox in sync when the underlying limit
                        // changes (e.g. after a read-back or external change).
                        readonly property int _liveLimit:
                            (typeof hardwareManager !== "undefined")
                            ? hardwareManager.chargeLimit : -1
                        on_LiveLimitChanged: {
                            if (_liveLimit >= 20)
                                value = _liveLimit
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: qsTr("Apply Limit")
                        enabled: batteryCard.chargeLimitSupported
                                 && (typeof hardwareManager === "undefined"
                                     || chargeLimitSpinBox.value !== hardwareManager.chargeLimit)
                        onClicked: {
                            if (typeof hardwareManager === "undefined")
                                return;
                            limitsTab.batteryApplyOk = true
                            limitsTab.batteryApplyMessage = ""
                            limitsTab._pendingApply = "battery"
                            hardwareManager.applyChargeLimit(chargeLimitSpinBox.value)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !limitsTab.batteryApplyOk
                             && limitsTab.batteryApplyMessage.length > 0
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                    text: limitsTab.batteryApplyMessage
                }
            }

            // ================= Advanced Power (RyzenAdj) =================
            // Optional/experimental SMU tuning: STAPM/fast/slow/APU-slow limits
            // and the Tctl thermal limit, applied via the ryzenadj CLI. These
            // are volatile (the SMU resets them), so an auto re-apply toggle is
            // offered. Hidden behind a clear "experimental" label and only
            // enabled when ryzenadj is installed.
            Card {
                id: ryzenAdjCard
                title: qsTr("Advanced Power \u00B7 RyzenAdj (experimental)")

                readonly property var _caps: (typeof hardwareManager !== "undefined")
                                             ? hardwareManager.capabilities : ({})
                readonly property var _raCap: _caps && _caps["RyzenAdj"]
                                              ? _caps["RyzenAdj"] : ({})
                readonly property bool ryzenAdjSupported: _raCap.supported === true
                readonly property string ryzenAdjReason:
                    _raCap.reason !== undefined ? _raCap.reason : ""

                readonly property var _vals: (typeof hardwareManager !== "undefined")
                                             ? hardwareManager.ryzenAdjValues : ({})

                Text {
                    Layout.fillWidth: true
                    visible: !ryzenAdjCard.ryzenAdjSupported
                    wrapMode: Text.WordWrap
                    color: Theme.warning
                    font.pixelSize: Theme.fontSmall
                    text: ryzenAdjCard.ryzenAdjReason.length > 0
                          ? ryzenAdjCard.ryzenAdjReason
                          : qsTr("RyzenAdj not available")
                }

                Text {
                    Layout.fillWidth: true
                    visible: ryzenAdjCard.ryzenAdjSupported
                    wrapMode: Text.WordWrap
                    color: Theme.warning
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("Advanced AMD SMU power + thermal tuning (replaces "
                               + "the old PL1/PL2 and temperature cards). "
                               + "Requires running Boreas as root. Values are "
                               + "volatile \u2014 enable auto re-apply to keep "
                               + "them in effect. Set a field to 0 to leave it "
                               + "unchanged.")
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap
                    rowSpacing: Theme.gapSmall
                    enabled: ryzenAdjCard.ryzenAdjSupported

                    Text {
                        text: qsTr("STAPM (sustained)")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    SpinBox {
                        id: raStapm
                        editable: true
                        from: 0; to: 200
                        value: ryzenAdjCard._vals.stapmW > 0 ? ryzenAdjCard._vals.stapmW : 0
                        textFromValue: function (v, l) { return v + qsTr(" W") }
                        valueFromText: function (t, l) { return parseInt(t) }
                    }

                    Text {
                        text: qsTr("Fast (boost)")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    SpinBox {
                        id: raFast
                        editable: true
                        from: 0; to: 200
                        value: ryzenAdjCard._vals.fastW > 0 ? ryzenAdjCard._vals.fastW : 0
                        textFromValue: function (v, l) { return v + qsTr(" W") }
                        valueFromText: function (t, l) { return parseInt(t) }
                    }

                    Text {
                        text: qsTr("Slow")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    SpinBox {
                        id: raSlow
                        editable: true
                        from: 0; to: 200
                        value: ryzenAdjCard._vals.slowW > 0 ? ryzenAdjCard._vals.slowW : 0
                        textFromValue: function (v, l) { return v + qsTr(" W") }
                        valueFromText: function (t, l) { return parseInt(t) }
                    }

                    Text {
                        text: qsTr("APU slow")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    SpinBox {
                        id: raApuSlow
                        editable: true
                        from: 0; to: 200
                        value: ryzenAdjCard._vals.apuSlowW > 0 ? ryzenAdjCard._vals.apuSlowW : 0
                        textFromValue: function (v, l) { return v + qsTr(" W") }
                        valueFromText: function (t, l) { return parseInt(t) }
                    }

                    Text {
                        text: qsTr("Tctl temp limit")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    SpinBox {
                        id: raTctl
                        editable: true
                        from: 0; to: 105
                        value: ryzenAdjCard._vals.tctlTempC > 0 ? ryzenAdjCard._vals.tctlTempC : 0
                        textFromValue: function (v, l) { return v + qsTr(" \u00B0C") }
                        valueFromText: function (t, l) { return parseInt(t) }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gap
                    enabled: ryzenAdjCard.ryzenAdjSupported

                    CheckBox {
                        id: raAutoReapply
                        text: qsTr("Auto re-apply")
                        checked: (typeof hardwareManager !== "undefined")
                                 && hardwareManager.ryzenAdjAutoReapply
                        onToggled: {
                            if (typeof hardwareManager !== "undefined")
                                hardwareManager.ryzenAdjAutoReapply = checked
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: qsTr("Apply Advanced")
                        enabled: ryzenAdjCard.ryzenAdjSupported
                        onClicked: {
                            if (typeof hardwareManager === "undefined")
                                return;
                            limitsTab.ryzenAdjApplyOk = true
                            limitsTab.ryzenAdjApplyMessage = ""
                            limitsTab._pendingApply = "ryzenadj"
                            // A field of 0 means "leave unchanged": map 0 -> -1.
                            hardwareManager.applyRyzenAdj(
                                raStapm.value > 0 ? raStapm.value : -1,
                                raFast.value > 0 ? raFast.value : -1,
                                raSlow.value > 0 ? raSlow.value : -1,
                                raApuSlow.value > 0 ? raApuSlow.value : -1,
                                raTctl.value > 0 ? raTctl.value : -1)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !limitsTab.ryzenAdjApplyOk
                             && limitsTab.ryzenAdjApplyMessage.length > 0
                    color: Theme.danger
                    font.pixelSize: Theme.fontSmall
                    text: limitsTab.ryzenAdjApplyMessage
                }
            }
        }
    }
}
