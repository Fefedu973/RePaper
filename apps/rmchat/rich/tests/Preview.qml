import QtQuick 2.15
import RePaper.Rich 1.0

Rectangle {
    width: 900
    height: 1350
    color: "white"
    RichMessage {
        id: message
        objectName: "richPreview"
        x: 34
        y: 24
        width: parent.width - 68
        height: contentHeight
        fontPixelSize: 21
        text: previewText
    }
}
