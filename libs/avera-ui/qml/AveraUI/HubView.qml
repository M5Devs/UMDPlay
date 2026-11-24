// Two-dimensional carousel: rows run down, cards run across. The host owns the
// model (an AveraHub) and the selection; this view only mirrors it and forwards
// clicks.
import QtQuick

Item {
    id: hubView

    property var hub: null
    property int focusRow: hub ? hub.currentRow : 0
    property int focusIndex: hub ? hub.currentIndex : 0

    function ensureVisible(row, index) {
        var lv = rows.itemAt(row)
        if (lv && lv.ensureIndex) lv.ensureIndex(index)
    }

    onFocusRowChanged: ensureVisible(focusRow, focusIndex)
    onFocusIndexChanged: ensureVisible(focusRow, focusIndex)

    Component.onCompleted: ensureVisible(focusRow, focusIndex)

    Column {
        id: rows
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.rowSpacing

        function itemAt(i) { return children.length > i ? children[i] : null }
        function ensureIndex(row, index) { var lv = itemAt(row); if (lv && lv.ensureIndex) lv.ensureIndex(index); }

        Repeater {
            model: hubView.hub ? hubView.hub.rows : []

            delegate: Item {
                id: rowItem

                required property var modelData
                required property int index

                width: rows.width
                height: label.height + 10 + Theme.cardHeight

                readonly property bool focused: hubView.focusRow === index

                Text {
                    id: label
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.gutter
                    text: modelData.title
                    color: rowItem.focused ? Theme.accent : Theme.textFaint
                    font: Theme.rowTitle
                    Behavior on color { ColorAnimation { duration: 120 } }
                }

                ListView {
                    id: strip
                    anchors.top: label.bottom
                    anchors.topMargin: 10
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.gutter
                    anchors.rightMargin: Theme.gutter
                    height: Theme.cardHeight
                    orientation: ListView.Horizontal
                    spacing: Theme.cardSpacing
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: rowItem.modelData.cards

                    function ensureIndex(i) {
                        var it = itemAtIndex(i)
                        if (it) positionViewAtIndex(i, ListView.Contain)
                    }

                    delegate: Card {
                        required property var modelData
                        required property int index

                        title: modelData.title
                        subtitle: modelData.subtitle
                        badge: modelData.badge
                        playable: modelData.playable
                        selected: rowItem.focused && hubView.focusIndex === index

                        onActivated: {
                            if (hubView.hub) {
                                hubView.hub.setSelection(rowItem.index, index)
                                hubView.hub.activate(rowItem.index, index)
                            }
                        }
                    }
                }
            }
        }
    }
}
