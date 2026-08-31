import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Collapsible, shared status log panel (G-Helper-style restyle).
//
// Bound to hardwareManager.recentLog ({ timestamp, action, success, message },
// oldest first, capped at 10). Header row toggles expand/collapse. Newest entry
// shown at the top. Presentation uses the shared Theme; behavior is unchanged.
Rectangle {
    id: statusLogPanel

    property bool expanded: false

    property var logEntries: (typeof hardwareManager !== "undefined")
                             ? hardwareManager.recentLog
                             : []

    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    radius: Theme.radius

    Layout.fillWidth: true
    implicitHeight: contentColumn.implicitHeight

    ColumnLayout {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        // Header / collapse toggle.
        Item {
            Layout.fillWidth: true
            implicitHeight: 40

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.pad
                anchors.rightMargin: Theme.pad
                spacing: Theme.gapSmall

                Text {
                    text: statusLogPanel.expanded ? "\u25BC" : "\u25B6"
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    text: qsTr("Status Log")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    Layout.fillWidth: true
                }
                Text {
                    text: statusLogPanel.logEntries.length + qsTr(" recent")
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: statusLogPanel.expanded = !statusLogPanel.expanded
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.border
            visible: statusLogPanel.expanded
        }

        ListView {
            id: logList
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            Layout.maximumHeight: 220
            visible: statusLogPanel.expanded
            clip: true
            interactive: true
            boundsBehavior: Flickable.StopAtBounds

            model: {
                var reversed = [];
                var src = statusLogPanel.logEntries;
                for (var i = src.length - 1; i >= 0; --i)
                    reversed.push(src[i]);
                return reversed;
            }

            delegate: Item {
                width: ListView.view ? ListView.view.width : 0
                implicitHeight: rowCol.implicitHeight + Theme.gapSmall

                required property var modelData

                RowLayout {
                    id: rowCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.pad
                    anchors.rightMargin: Theme.pad
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.gapSmall

                    Text {
                        text: modelData.success ? "\u2713" : "\u2717"
                        color: modelData.success ? Theme.success : Theme.danger
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        Layout.alignment: Qt.AlignTop
                    }

                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true

                        RowLayout {
                            spacing: Theme.gapSmall
                            Layout.fillWidth: true
                            Text {
                                text: modelData.action
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.timestamp
                                color: Theme.textFaint
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        Text {
                            text: modelData.message
                            color: modelData.success ? Theme.textMuted : Theme.warning
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                            visible: text.length > 0
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: logList.count === 0
                text: qsTr("No actions recorded yet.")
                color: Theme.textFaint
                font.pixelSize: Theme.fontSmall
                padding: Theme.pad
            }
        }
    }
}
