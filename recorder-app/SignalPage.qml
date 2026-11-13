import QtQuick 2.9

Item {
    id: panel
    property var host
    property var noise: host.audio.noise || ({enabled:false,ready:false,learning:false,progress:0,reductionDb:0,auxiliaryDb:[-96,-96,-96,-96,-96,-96]})
    property var monitor: host.audio.monitor || ({active:false,requested:false,volume:25,xruns:0})
    function command(path) { host.touchFocus(); host.request(path,true) }
    function hardware(key) {
        if (key === Qt.Key_2) { host.page = 0; return }
        if (key === Qt.Key_3) { if (!host.busy && !noise.learning) host.startRecording(); return }
        if (key === Qt.Key_1) { if (!host.busy && noise.ready) command("noise/" + (noise.enabled ? "off" : "on")); return }
        var up = key === Qt.Key_4 || key === Qt.Key_VolumeUp
        command("monitor/volume?value=" + Math.max(0,Math.min(100,monitor.volume + (up ? 5 : -5))))
    }
    Text { x: 20; y: 49; text: "HEADPHONE MONITOR"; color: "#8f9db2"; font.pixelSize: 10; font.bold: true }
    Rectangle {
        x: 20; y: 67; width: 280; height: 37; radius: 6; color: monitor.active ? "#26483d" : "#273344"
        Text { anchors.centerIn: parent; text: monitor.active ? "MONITOR ON  ·  tap to mute" : "MONITOR OFF  ·  tap to listen"; color: "#d6eee6"; font.pixelSize: 12; font.bold: true }
        MouseArea { anchors.fill: parent; enabled: !host.networking && !host.playing && host.audio.state !== "assistant"; onClicked: panel.command("monitor/" + (monitor.requested ? "stop" : "start")) }
    }
    Text { x: 20; y: 111; width: 280; text: "Volume " + monitor.volume + "%   ·   ± keys   ·   use headphones"; color: "#9cacbe"; font.pixelSize: 10 }
    Row {
        x: 20; y: 132; spacing: 8
        Repeater {
            model: ["ORIGINAL", "REDUCED"]
            Rectangle {
                width: 136; height: 36; radius: 6
                color: (index === 1) === noise.enabled ? "#39715c" : "#273344"
                opacity: index === 1 && !noise.ready ? .4 : 1
                Text { anchors.centerIn: parent; text: modelData; color: "#eef5fa"; font.pixelSize: 12; font.bold: true }
                MouseArea { anchors.fill: parent; enabled: !host.busy && !noise.learning && (index === 0 || noise.ready); onClicked: panel.command("noise/" + (index === 1 ? "on" : "off")) }
            }
        }
    }
    Rectangle {
        x: 20; y: 178; width: 280; height: 33; radius: 6; color: "#273344"
        Text { anchors.centerIn: parent; text: noise.learning ? "Stay quiet… " + Math.round(noise.progress * 100) + "%" : "LEARN ROOM NOISE  ·  2 seconds"; color: "#83dcc0"; font.pixelSize: 11 }
        MouseArea { anchors.fill: parent; enabled: !host.busy && !noise.learning; onClicked: panel.command("noise/learn") }
    }
    Text { x: 20; y: 220; text: "AUX 0–5"; color: "#8f9db2"; font.pixelSize: 9 }
    Row {
        x: 83; y: 217; spacing: 5
        Repeater {
            model: 6
            Rectangle {
                width: 31; height: 24; color: "#202b39"; radius: 2
                Rectangle { x: 0; y: 24-height; width: 31; height: Math.max(1,Math.min(24,(noise.auxiliaryDb[index]+90)/60*24)); color: "#478c74"; radius: 2 }
                Text { anchors.centerIn: parent; text: index; color: "#e6f1f7"; font.pixelSize: 10 }
            }
        }
    }
    Text {
        x: 20; y: 250; width: 280; wrapMode: Text.WordWrap
        text: noise.learning ? "Learning all 8 mics. Keep room noise steady." : !noise.ready ? "Learn the room noise, then compare both modes." : noise.enabled ? "Reduced stereo · also applied to new recordings" : "Original stereo · new recordings stay unprocessed"
        color: "#9cacbe"; font.pixelSize: 10
    }
}
