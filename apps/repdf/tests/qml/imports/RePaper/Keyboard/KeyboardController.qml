import QtQuick 2.15

QtObject {
    property var window: null
    property bool fallbackVisible: false
    property real platformHeight: 0
    property rect platformRectangle: Qt.rect(0, 0, 0, 0)
    property int dismissCalls: 0
    function dismiss() { ++dismissCalls }
}
