import QtQuick 2.9

Item {
    id: panel
    property var host
    property var info: ({armed:false, active:false, state:"off", message:"REC enables Wi-Fi Audio", inputLeftDb:-96, inputRightDb:-96, outputLeftDb:-96, outputRightDb:-96})
    property string address: "Connect Wi-Fi first"
    property bool polling: false
    function refresh() {
        if (polling) return
        polling = true
        host.request("network", false, function(data, ok) { polling = false; if (ok) info = data })
    }
    function toggle() { host.touchFocus(); host.request(info.armed ? "network/stop" : "network/start", true, function() { refresh() }) }
    function stop() { host.request("network/stop", true, function() { refresh() }) }
    function hardware(key) {
        if (key === Qt.Key_3) { if (!info.armed) toggle(); return }
        if (key === Qt.Key_2) { if (info.armed || host.networking) stop(); else host.page = 0; return }
        if (key === Qt.Key_1 && !info.armed) { host.page = 3; return }
    }
    onVisibleChanged: if (visible) {
        refresh()
        host.request("wifi", false, function(data, ok) { if (ok) address = data.ip || data.ipAddress || "Connect Wi-Fi first" })
    }
    Timer { interval: 250; running: panel.visible; repeat: true; onTriggered: panel.refresh() }
    Rectangle {
        x: 20; y: 50; width: 280; height: 57; radius: 8
        color: info.active ? "#203d36" : "#1d2938"
        Text { x: 12; y: 9; text: info.active ? "● CONNECTED" : info.armed ? "WAITING FOR COMPUTER" : "WI-FI AUDIO OFF"; color: "#83dcc0"; font.pixelSize: 13; font.bold: true }
        Text { x: 12; y: 32; width: 256; text: info.active ? info.peer + " · " + info.bufferMs + " ms" : address; color: "#b5c2d4"; font.pixelSize: 12; elide: Text.ElideRight }
        MouseArea { anchors.fill: parent; onClicked: panel.toggle() }
    }
    Text { x: 20; y: 117; text: "MIC → COMPUTER"; color: "#8f9db2"; font.pixelSize: 10 }
    LevelMeter { x: 20; y: 135; channel: "L"; peak: info.inputLeftDb }
    LevelMeter { x: 20; y: 159; channel: "R"; peak: info.inputRightDb }
    Text { x: 20; y: 189; text: "COMPUTER → OUTPUT"; color: "#8f9db2"; font.pixelSize: 10 }
    LevelMeter { x: 20; y: 207; channel: "L"; peak: info.outputLeftDb }
    LevelMeter { x: 20; y: 231; channel: "R"; peak: info.outputRightDb }
    Text {
        x: 20; y: 263; width: 280; font.pixelSize: 10; color: info.concealedPackets ? "#edc989" : "#8f9db2"
        text: info.state === "error" ? info.message : info.active ? "Recovered " + info.recoveredPackets + " · gaps " + info.concealedPackets + " · " + Number(info.queueMs).toFixed(0) + " ms" : "44.1 kHz · 24 bit · stereo · RTP"
    }
}
