import QtQuick
import QtQuick.Layouts

// A titled rounded surface used to group controls (G-Helper-style card).
// Styling wrapper only; place content via its default `content` alias target.
Rectangle {
    id: card

    // Optional section title shown at the top of the card.
    property string title: ""
    // Children added to `card` are reparented into the content column below.
    default property alias content: contentColumn.data

    color: Theme.surface
    radius: Theme.radius
    border.color: Theme.border
    border.width: 1

    Layout.fillWidth: true
    implicitHeight: outer.implicitHeight + Theme.pad * 2

    ColumnLayout {
        id: outer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.pad
        spacing: Theme.gapSmall

        Text {
            visible: card.title.length > 0
            text: card.title
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.bold: true
            font.capitalization: Font.AllUppercase
            Layout.fillWidth: true
        }

        ColumnLayout {
            id: contentColumn
            Layout.fillWidth: true
            spacing: Theme.gapSmall
        }
    }
}
