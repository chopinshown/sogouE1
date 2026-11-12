import QtQuick 2.9

Item {
    property string channel: "L"
    property real peak: -96
    property real rms: -96
    property real held: -96
    width: 280; height: 18
    onPeakChanged: { if (peak > held) held = peak }
    Timer { interval: 80; running: true; repeat: true; onTriggered: parent.held = Math.max(parent.peak, parent.held - 1.4) }
    Text { x: 0; y: 1; text: channel; color: "#94a1b6"; font.pixelSize: 11; font.bold: true }
    Rectangle {
        x: 18; y: 2; width: 209; height: 13; radius: 3; color: "#26303d"; clip: true
        Rectangle {
            width: Math.max(0, Math.min(1, (peak + 72) / 72)) * 209
            height: parent.height
            color: peak > -1 ? "#ff6474" : peak > -12 ? "#f3cf75" : "#77dab2"
            Behavior on width { NumberAnimation { duration: 70 } }
        }
        Rectangle { x: Math.max(0, Math.min(1, (held + 72) / 72)) * 207; width: 2; height: parent.height; color: "white" }
        Repeater {
            model: 20
            Rectangle { x: (index + 1) * 10.45; width: 1; height: 13; color: "#10141c"; opacity: 0.65 }
        }
    }
    Text {
        x: 233; y: 0; width: 47; horizontalAlignment: Text.AlignRight
        text: peak <= -95 ? "−∞" : peak.toFixed(0) + " dB"
        color: peak > -1 ? "#ff6474" : "#a8b4c8"
        font.family: "Roboto"; font.pixelSize: 10
    }
}
