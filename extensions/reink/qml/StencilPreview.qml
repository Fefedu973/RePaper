import QtQuick 2.15

Canvas {
    property var drawing: ({})
    property bool thumbnail: false
    property real thumbnailPadding: 12
    property real minimumThumbnailWidth: 0.8
    function schedulePaint() { if (visible && width > 0 && height > 0) requestPaint() }
    onDrawingChanged: schedulePaint()
    onWidthChanged: schedulePaint()
    onHeightChanged: schedulePaint()
    onVisibleChanged: schedulePaint()
    onPaint: {
        const context = getContext("2d")
        context.reset()
        const sourceWidth = Number(drawing.width) || 120
        const sourceHeight = Number(drawing.height) || 80
        const padding = thumbnail ? thumbnailPadding : 12
        const scale = Math.min((width - padding) / sourceWidth, (height - padding) / sourceHeight)
        if (scale <= 0) return
        context.translate((width - sourceWidth * scale) / 2, (height - sourceHeight * scale) / 2)
        context.scale(scale, scale)
        context.lineCap = "round"
        context.lineJoin = "round"
        const strokes = drawing.strokes || []
        for (let i = 0; i < strokes.length; ++i) {
            const stroke = strokes[i], points = stroke.points || []
            if (points.length < 2) continue
            context.strokeStyle = stroke.color || "black"
            context.lineWidth = Math.max(thumbnail ? minimumThumbnailWidth / scale : 0.25 / scale, Number(stroke.width) || 2.3)
            context.beginPath()
            context.moveTo(points[0].x, points[0].y)
            for (let j = 1; j < points.length; ++j) context.lineTo(points[j].x, points[j].y)
            context.stroke()
        }
    }
}
