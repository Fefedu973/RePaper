import QtQuick 2.15
Canvas {
    id: icon
    property string name: "document"
    property color ink: "black"
    implicitWidth: Theme.icon; implicitHeight: Theme.icon
    onNameChanged: requestPaint()
    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const c = getContext("2d"); c.reset(); c.scale(width / 24, height / 24)
        c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 1.4; c.lineCap = "round"; c.lineJoin = "round"
        function line(points) { c.beginPath(); c.moveTo(points[0], points[1]); for (let i=2;i<points.length;i+=2)c.lineTo(points[i], points[i+1]); c.stroke() }
        function circle(x,y,r) { c.beginPath();c.arc(x,y,r,0,Math.PI*2);c.stroke() }
        if (name === "back") line([15,5,8,12,15,19])
        else if (name === "next") line([9,5,16,12,9,19])
        else if (name === "close") { line([6,6,18,18]);line([18,6,6,18]) }
        else if (name === "search") { circle(10,10,6);line([14.5,14.5,21,21]) }
        else if (name === "pen") { line([4,20,6,14,17,3,21,7,10,18,4,20]);line([6,14,10,18]);line([15,5,19,9]) }
        else if (name === "wire") { circle(4,18,2);line([6,18,12,18,12,6,18,6]);circle(20,6,2) }
        else if (name === "refresh") {
            c.beginPath(); c.arc(12,12,8.5,0,Math.PI*1.75); c.lineTo(20.5,8); c.stroke()
            line([20.5,3,20.5,8,15.5,8])
        }
        else if (name === "fit-width") { line([3,5,3,19]);line([21,5,21,19]);line([5,12,19,12]);line([8,9,5,12,8,15]);line([16,9,19,12,16,15]) }
        else if (name === "rotate") { c.beginPath();c.arc(12,12,8,-2.6,1.7);c.stroke();line([6,3,6,9,1,9]);line([9,10,15,10,15,16,9,16,9,10]) }
        else if (name === "history") { c.beginPath();c.arc(12,12,8,-2.5,3.5);c.stroke();line([3,4,3,10,8,10]);line([12,7,12,12,16,14]) }
        else if (name === "more") { for(let y=5;y<21;y+=7){c.beginPath();c.arc(12,y,1,0,Math.PI*2);c.fill()} }
        else if (name === "settings") { line([3,6,21,6]);line([3,12,21,12]);line([3,18,21,18]);c.fillRect(7,3,3,6);c.fillRect(15,9,3,6);c.fillRect(6,15,3,6) }
        else if (name === "fullscreen") { line([8,3,3,3,3,8]);line([16,3,21,3,21,8]);line([3,16,3,21,8,21]);line([16,21,21,21,21,16]) }
        else if (name === "folder") line([3,6,10,6,12,9,21,9,21,20,3,20,3,6])
        else { line([5,2,14,2,20,8,20,22,5,22,5,2]);line([14,2,14,8,20,8]);if(name==="note"){line([8,12,17,12]);line([8,16,15,16])} }
    }
}
