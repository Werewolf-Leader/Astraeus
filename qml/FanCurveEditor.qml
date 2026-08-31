pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Fan Curve Editor — G-Helper-style, with a CPU/GPU toggle, a draggable X/Y
// graph, and a compact numeric table for precise edits.
//
// Data flows through the `hardwareManager` context object. This editor NEVER
// writes hardware directly and NEVER auto-applies (Req 6.7): editing only
// mutates a local ListModel; the only path to hardware is the confirmation
// Dialog's accept handler, which calls hardwareManager.applyFanCurve(profile,
// points, fan).
//
// CPU (pwm1) and GPU (pwm2) fans carry INDEPENDENT curves. The "fan" toggle
// selects which one is loaded/edited/applied; switching reloads that fan's
// stored curve. The curve is also reloaded when the selected power profile
// changes (Req 12.3).
Item {
    id: fanCurveEditor

    implicitHeight: content.implicitHeight

    // ---- Point-count bounds (Req 6.2, 6.3) --------------------------------
    readonly property int minPoints: 2
    readonly property int maxPoints: 8

    // Which fan is being edited: "cpu" (pwm1) or "gpu" (pwm2).
    property string activeFan: "cpu"

    // ---- Capability lookup (Req 3.2, 3.3, 6.11) ---------------------------
    readonly property var _caps: (typeof hardwareManager !== "undefined")
                                 ? hardwareManager.capabilities : ({})
    readonly property var _fanCap: _caps && _caps["FanCurve"]
                                   ? _caps["FanCurve"] : ({})
    readonly property bool fanCurveSupported: _fanCap.supported === true

    // ---- Live validation state (Req 6.3, 6.6) -----------------------------
    property bool curveValid: false
    property string invalidReason: ""

    // ---- Live RPM + calibrated range for the selected fan -----------------
    readonly property bool _isGpu: activeFan === "gpu"
    readonly property int liveRpm: (typeof hardwareManager === "undefined")
        ? -1
        : (_isGpu ? hardwareManager.gpuFanRpm : hardwareManager.cpuFanRpm)
    readonly property var _range: (typeof hardwareManager !== "undefined"
                                   && hardwareManager.fanRpmRange)
                                  ? hardwareManager.fanRpmRange : ({})
    readonly property int rangeMin: {
        var r = _range || {};
        var v = _isGpu ? r.gpuMin : r.cpuMin;
        return v !== undefined ? v : -1;
    }
    readonly property int rangeMax: {
        var r = _range || {};
        var v = _isGpu ? r.gpuMax : r.cpuMax;
        return v !== undefined ? v : -1;
    }

    // Build the QVariantList of {tempC, percent} from the current editable model.
    function pointsFromModel() {
        var pts = [];
        for (var i = 0; i < curveModel.count; ++i) {
            var row = curveModel.get(i);
            pts.push({ tempC: row.tempC, percent: row.percent });
        }
        return pts;
    }

    // Re-run validation over the current model and refresh gating (Req 6.3-6.6).
    function revalidate() {
        if (typeof hardwareManager === "undefined") {
            curveValid = false;
            invalidReason = "";
            graphCanvas.requestPaint();
            return;
        }
        var result = hardwareManager.validateFanCurve(pointsFromModel());
        curveValid = result && result.valid === true;
        invalidReason = (result && result.reason !== undefined) ? result.reason : "";
        graphCanvas.requestPaint();
    }

    // (Re)load the editable model from the persisted curve for the selected
    // profile + active fan (Req 6.1, 6.2, 12.3).
    function reloadCurve() {
        curveModel.clear();
        if (typeof hardwareManager === "undefined")
            return;
        var profile = hardwareManager.selectedProfile;
        var stored = hardwareManager.fanCurveFor(profile, fanCurveEditor.activeFan);
        for (var i = 0; i < stored.length; ++i) {
            var p = stored[i];
            curveModel.append({
                tempC: p.tempC !== undefined ? p.tempC : 0,
                percent: p.percent !== undefined ? p.percent : 0
            });
        }
        revalidate();
    }

    // Editable table backing store (Req 6.2). Rows are {tempC, percent},
    // maintained in ascending-temp order.
    ListModel {
        id: curveModel
    }

    // Keep the model sorted by tempC (used after a drag moves a point past a
    // neighbour). Simple insertion sort over the small (<=8) list.
    function sortModel() {
        for (var i = 1; i < curveModel.count; ++i) {
            var j = i;
            while (j > 0 && curveModel.get(j - 1).tempC > curveModel.get(j).tempC) {
                curveModel.move(j, j - 1, 1);
                j--;
            }
        }
    }

    Component.onCompleted: reloadCurve()

    Connections {
        target: (typeof hardwareManager !== "undefined") ? hardwareManager : null
        function onSelectedProfileChanged() {
            fanCurveEditor.reloadCurve();
        }
    }

    GroupBox {
        id: content
        anchors.fill: parent
        title: qsTr("Fan Curve")

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            // ---- Unavailable_State (Req 3.2, 3.3, 6.11) -------------------
            Label {
                id: unavailableLabel
                Layout.fillWidth: true
                visible: !fanCurveEditor.fanCurveSupported
                text: qsTr("Fan curve control not supported by current driver")
                color: Theme.warning
                wrapMode: Text.WordWrap
            }

            // ---- CPU / GPU toggle -----------------------------------------
            RowLayout {
                Layout.fillWidth: true
                visible: fanCurveEditor.fanCurveSupported
                spacing: Theme.gapSmall

                Repeater {
                    model: [
                        { key: "cpu", label: qsTr("CPU Fan") },
                        { key: "gpu", label: qsTr("GPU Fan") }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool current: fanCurveEditor.activeFan === modelData.key
                        Layout.fillWidth: true
                        implicitHeight: 32
                        radius: Theme.radiusSmall
                        color: current ? Theme.accentSoft
                                       : (fanMouse.containsMouse ? Theme.surfaceAlt : Theme.surface)
                        border.color: current ? Theme.accent : Theme.border
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            color: parent.current ? Theme.text : Theme.textMuted
                            font.pixelSize: Theme.fontBody
                            font.bold: parent.current
                        }
                        MouseArea {
                            id: fanMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (fanCurveEditor.activeFan !== parent.modelData.key) {
                                    fanCurveEditor.activeFan = parent.modelData.key;
                                    fanCurveEditor.reloadCurve();
                                }
                            }
                        }
                    }
                }
            }

            // ---- Live RPM readout + Calibrate -----------------------------
            RowLayout {
                Layout.fillWidth: true
                visible: fanCurveEditor.fanCurveSupported
                spacing: Theme.gap

                Text {
                    text: qsTr("Speed")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    text: fanCurveEditor.liveRpm < 0
                          ? qsTr("\u2014")
                          : fanCurveEditor.liveRpm + qsTr(" RPM")
                    color: Theme.text
                    font.bold: true
                    font.pixelSize: Theme.fontBody
                }

                // Calibrated range (only after a calibration run).
                Text {
                    visible: fanCurveEditor.rangeMin >= 0 || fanCurveEditor.rangeMax >= 0
                    text: qsTr("(range %1\u2013%2)")
                            .arg(fanCurveEditor.rangeMin < 0 ? qsTr("?") : fanCurveEditor.rangeMin)
                            .arg(fanCurveEditor.rangeMax < 0 ? qsTr("?") : fanCurveEditor.rangeMax)
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                }

                Item { Layout.fillWidth: true }

                Button {
                    id: calibrateButton
                    text: qsTr("Calibrate")
                    enabled: fanCurveEditor.fanCurveSupported
                    onClicked: confirmCalibrateDialog.open()
                }
            }

            // ---- Draggable X/Y graph --------------------------------------
            // X = temperature 0..105 C, Y = fan 0..100 %. Points are draggable
            // handles; the line connects them. Dragging updates the model and
            // re-validates; it never applies to hardware (Req 6.7).
            Item {
                id: graph
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                visible: fanCurveEditor.fanCurveSupported

                // Plot geometry / axis ranges.
                readonly property int padL: 34
                readonly property int padR: 10
                readonly property int padT: 10
                readonly property int padB: 22
                readonly property real tMin: 0
                readonly property real tMax: 105
                readonly property real pMin: 0
                readonly property real pMax: 100
                readonly property real plotW: width - padL - padR
                readonly property real plotH: height - padT - padB

                function tempToX(t) {
                    return padL + (t - tMin) / (tMax - tMin) * plotW;
                }
                function pctToY(p) {
                    return padT + (1 - (p - pMin) / (pMax - pMin)) * plotH;
                }
                function xToTemp(x) {
                    var t = tMin + (x - padL) / plotW * (tMax - tMin);
                    return Math.round(Math.max(tMin, Math.min(tMax, t)));
                }
                function yToPct(y) {
                    var p = pMin + (1 - (y - padT) / plotH) * (pMax - pMin);
                    return Math.round(Math.max(pMin, Math.min(pMax, p)));
                }

                Rectangle {
                    anchors.fill: parent
                    color: Theme.surface
                    radius: Theme.radiusSmall
                    border.color: Theme.border
                    border.width: 1
                }

                Canvas {
                    id: graphCanvas
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.reset();
                        var g = graph;

                        // Grid lines + axis labels (temp every 20C, pct every 25%).
                        ctx.strokeStyle = Theme.border;
                        ctx.fillStyle = Theme.textFaint;
                        ctx.lineWidth = 1;
                        ctx.font = "9px sans-serif";
                        for (var t = 0; t <= 105; t += 20) {
                            var x = g.tempToX(t);
                            ctx.globalAlpha = 0.4;
                            ctx.beginPath(); ctx.moveTo(x, g.padT); ctx.lineTo(x, g.padT + g.plotH); ctx.stroke();
                            ctx.globalAlpha = 1.0;
                            ctx.fillText(t + "\u00B0", x - 6, g.height - 8);
                        }
                        for (var p = 0; p <= 100; p += 25) {
                            var y = g.pctToY(p);
                            ctx.globalAlpha = 0.4;
                            ctx.beginPath(); ctx.moveTo(g.padL, y); ctx.lineTo(g.padL + g.plotW, y); ctx.stroke();
                            ctx.globalAlpha = 1.0;
                            ctx.fillText(p + "%", 4, y + 3);
                        }

                        if (curveModel.count === 0)
                            return;

                        // Curve line.
                        ctx.strokeStyle = fanCurveEditor.curveValid ? Theme.accent : Theme.danger;
                        ctx.lineWidth = 2;
                        ctx.beginPath();
                        for (var i = 0; i < curveModel.count; ++i) {
                            var row = curveModel.get(i);
                            var px = g.tempToX(row.tempC);
                            var py = g.pctToY(row.percent);
                            if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
                        }
                        ctx.stroke();
                    }
                }

                // Draggable point handles.
                Repeater {
                    model: curveModel
                    delegate: Rectangle {
                        id: handle
                        required property int index
                        required property int tempC
                        required property int percent

                        width: 14; height: 14; radius: 7
                        color: fanCurveEditor.curveValid ? Theme.accent : Theme.danger
                        border.color: Theme.text
                        border.width: 1
                        x: graph.tempToX(tempC) - width / 2
                        y: graph.pctToY(percent) - height / 2

                        MouseArea {
                            anchors.fill: parent
                            drag.target: handle
                            drag.axis: Drag.XAndYAxis
                            cursorShape: Qt.SizeAllCursor
                            drag.minimumX: graph.padL - handle.width / 2
                            drag.maximumX: graph.padL + graph.plotW - handle.width / 2
                            drag.minimumY: graph.padT - handle.height / 2
                            drag.maximumY: graph.padT + graph.plotH - handle.height / 2

                            onPositionChanged: {
                                if (!drag.active)
                                    return;
                                var cx = handle.x + handle.width / 2;
                                var cy = handle.y + handle.height / 2;
                                curveModel.setProperty(handle.index, "tempC", graph.xToTemp(cx));
                                curveModel.setProperty(handle.index, "percent", graph.yToPct(cy));
                                fanCurveEditor.revalidate();
                            }
                            onReleased: {
                                fanCurveEditor.sortModel();
                                fanCurveEditor.revalidate();
                            }
                            // Double-click removes this point, gated so the count
                            // never drops below the minimum (Req 6.2, 6.3).
                            onDoubleClicked: {
                                if (curveModel.count > fanCurveEditor.minPoints) {
                                    curveModel.remove(handle.index);
                                    fanCurveEditor.revalidate();
                                }
                            }
                        }
                    }
                }
            }

            // Hint: how to add/remove points now that the numeric table is gone.
            Label {
                Layout.fillWidth: true
                visible: fanCurveEditor.fanCurveSupported
                text: qsTr("Drag points to shape the curve. Double-click a point "
                           + "to remove it.")
                color: Theme.textFaint
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            // ---- Add / validation reason row ------------------------------
            RowLayout {
                Layout.fillWidth: true
                visible: fanCurveEditor.fanCurveSupported
                spacing: 8

                Button {
                    text: qsTr("Add Point")
                    enabled: curveModel.count < fanCurveEditor.maxPoints
                    onClicked: {
                        var lastTemp = 0;
                        var lastPct = 0;
                        if (curveModel.count > 0) {
                            var last = curveModel.get(curveModel.count - 1);
                            lastTemp = Math.min(last.tempC + 5, 105);
                            lastPct = Math.min(last.percent + 10, 100);
                        }
                        curveModel.append({ tempC: lastTemp, percent: lastPct });
                        fanCurveEditor.revalidate();
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    visible: !fanCurveEditor.curveValid
                             && fanCurveEditor.invalidReason.length > 0
                    text: fanCurveEditor.invalidReason
                    color: Theme.danger
                    wrapMode: Text.WordWrap
                }
            }

            // ---- Apply ----------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                visible: fanCurveEditor.fanCurveSupported
                spacing: 8

                Item { Layout.fillWidth: true }

                Button {
                    id: applyButton
                    text: qsTr("Apply %1 Curve")
                            .arg(fanCurveEditor.activeFan === "gpu" ? qsTr("GPU") : qsTr("CPU"))
                    enabled: fanCurveEditor.fanCurveSupported
                             && fanCurveEditor.curveValid
                    highlighted: enabled
                    onClicked: confirmApplyDialog.open()
                }
            }
        }
    }

    // ---- Apply confirmation dialog (Disruptive_Action) --------------------
    Dialog {
        id: confirmApplyDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Confirm Fan Curve")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.bold: true
                text: {
                    var profile = (typeof hardwareManager !== "undefined")
                                  ? hardwareManager.selectedProfile : "";
                    var fan = fanCurveEditor.activeFan === "gpu" ? qsTr("GPU") : qsTr("CPU");
                    return qsTr("Apply this %1 fan curve to the \"%2\" profile?")
                             .arg(fan).arg(profile);
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                text: qsTr("This changes how the fan responds and may be disruptive.")
            }
        }

        // The SOLE caller of applyFanCurve for the active fan (Req 6.8, 6.10).
        onAccepted: {
            if (typeof hardwareManager !== "undefined") {
                hardwareManager.applyFanCurve(hardwareManager.selectedProfile,
                                              fanCurveEditor.pointsFromModel(),
                                              fanCurveEditor.activeFan);
            }
        }
    }

    // ---- Calibrate confirmation dialog ------------------------------------
    // Calibration briefly forces the fan to 0% then 100% to measure its RPM
    // range, then restores the saved curve. Warn the operator first since the
    // fan will audibly change speed for a few seconds.
    Dialog {
        id: confirmCalibrateDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Calibrate Fan")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape

        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.bold: true
                text: qsTr("Calibrate the %1 fan?")
                        .arg(fanCurveEditor.activeFan === "gpu" ? qsTr("GPU") : qsTr("CPU"))
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                text: qsTr("The fan will briefly spin to minimum then maximum "
                           + "(a few seconds) to measure its RPM range, then the "
                           + "saved curve is restored. Requires root.")
            }
        }

        onAccepted: {
            if (typeof hardwareManager !== "undefined")
                hardwareManager.calibrateFan(fanCurveEditor.activeFan);
        }
    }
}
