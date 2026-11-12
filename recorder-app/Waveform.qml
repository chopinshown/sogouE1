import QtQuick 2.9

Rectangle {
    id: wave
    property var samplesLeft: []
    property var samplesRight: []
    property var marks: []
    property real duration: 0
    property real position: 0
    property bool interactive: false
    signal seekRequested(real seconds)
    radius: 8; color: "#19212d"; clip: true
    onSamplesLeftChanged: drawing.requestPaint()
    onSamplesRightChanged: drawing.requestPaint()
    onMarksChanged: drawing.requestPaint()
    onPositionChanged: drawing.requestPaint()
    onDurationChanged: drawing.requestPaint()
    Canvas {
        id: drawing
        anchors.fill: parent
        onPaint: {
            var c = getContext("2d")
            c.clearRect(0, 0, width, height)
            var mid1 = height * 0.27, mid2 = height * 0.75
            c.strokeStyle = "#344153"; c.lineWidth = 1
            c.beginPath(); c.moveTo(0, mid1); c.lineTo(width, mid1); c.moveTo(0, mid2); c.lineTo(width, mid2); c.stroke()
            var max = 0.00001
            for (var i = 0; i < wave.samplesLeft.length; i++) max = Math.max(max, wave.samplesLeft[i])
            for (i = 0; i < wave.samplesRight.length; i++) max = Math.max(max, wave.samplesRight[i])
            var rows = [wave.samplesLeft, wave.samplesRight], centers = [mid1, mid2], colors = ["#7cdec0", "#79b8ee"]
            for (var r = 0; r < 2; r++) {
                c.strokeStyle = colors[r]; c.lineWidth = Math.max(1, width / Math.max(1, rows[r].length) * 0.65)
                c.beginPath()
                for (i = 0; i < rows[r].length; i++) {
                    var x = (i + 0.5) * width / rows[r].length
                    var h = Math.max(0.6, Math.sqrt(rows[r][i] / max) * height * 0.20)
                    c.moveTo(x, centers[r] - h); c.lineTo(x, centers[r] + h)
                }
                c.stroke()
            }
            if (wave.duration > 0) {
                c.strokeStyle = "#e8bd76"; c.lineWidth = 1
                for (i = 0; i < wave.marks.length; i++) {
                    var mx = wave.marks[i].seconds / wave.duration * width
                    c.beginPath(); c.moveTo(mx, 0); c.lineTo(mx, height); c.stroke()
                    c.fillStyle = "#e8bd76"; c.fillRect(mx - 2, 0, 5, 5)
                }
                var px = Math.min(width - 1, Math.max(0, wave.position / wave.duration * width))
                c.strokeStyle = "#f6f8fc"; c.lineWidth = 1.5
                c.beginPath(); c.moveTo(px, 0); c.lineTo(px, height); c.stroke()
            }
        }
    }
    Text { x: 5; y: 2; text: "L"; color: "#7cdec0"; font.pixelSize: 8; opacity: 0.7 }
    Text { x: 5; y: parent.height * 0.5 + 1; text: "R"; color: "#79b8ee"; font.pixelSize: 8; opacity: 0.7 }
    MouseArea {
        anchors.fill: parent; enabled: wave.interactive
        onReleased: wave.seekRequested(Math.min(1, Math.max(0, mouse.x / width)) * wave.duration)
    }
}
