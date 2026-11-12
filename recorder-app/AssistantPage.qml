import QtQuick 2.9

Item {
    id: voice
    property var host
    property var info: ({state:"idle",active:false,activationCode:"",message:"Press M to activate",userText:"",assistantText:"",emotion:"",levelDb:-96,volume:65,sentPackets:0,receivedPackets:0})
    property bool polling: false
    function refresh() {
        if (polling) return
        polling = true
        host.request("assistant",false,function(data,ok) { polling = false; if (ok) info = data })
    }
    function action(name) { host.touchFocus(); host.request("assistant/" + name,true,function() { refresh() }) }
    function talk() {
        if (info.state === "listening") action("finish")
        else if (!info.active && !host.busy) action("talk")
    }
    function hardware(key) {
        if (key === Qt.Key_3) talk()
        else if (key === Qt.Key_2) {
            if (info.state === "listening") action("finish")
            else if (info.active) action("cancel")
            else host.page = 0
        } else if (key === Qt.Key_1) action(info.active ? "cancel" : "activate")
        else if (key === Qt.Key_4 || key === Qt.Key_VolumeUp) action("volume-up")
        else if (key === Qt.Key_5 || key === Qt.Key_VolumeDown) action("volume-down")
    }
    onVisibleChanged: if (visible) refresh()
    Component.onCompleted: refresh()
    Timer { interval: 250; repeat: true; running: voice.visible || voice.info.active; onTriggered: voice.refresh() }
    Rectangle {
        x: 20; y: 49; width: 280; height: 32; radius: 7
        color: voice.info.state === "listening" ? "#4c2631" : "#20372f"
        Text { x: 11; anchors.verticalCenter: parent.verticalCenter; text: voice.info.state.toUpperCase(); color: "#83dcc0"; font.pixelSize: 13; font.bold: true }
        Text { x: 180; width: 89; anchors.verticalCenter: parent.verticalCenter; horizontalAlignment: Text.AlignRight; text: "VOL " + voice.info.volume; color: "#aab8cb"; font.pixelSize: 11 }
        MouseArea { anchors.fill: parent; onClicked: voice.talk() }
    }
    Rectangle {
        x: 20; y: 88; width: 280; height: 6; radius: 3; color: "#283442"
        Rectangle { width: parent.width * Math.max(0,Math.min(1,(voice.info.levelDb + 60) / 60)); height: 6; radius: 3; color: "#83dcc0" }
    }
    Item {
        visible: !!voice.info.activationCode
        x: 20; y: 108; width: 280; height: 170
        Text { width: 280; text: "Activate at xiaozhi.me"; horizontalAlignment: Text.AlignHCenter; color: "#aab8cb"; font.pixelSize: 17 }
        Text { y: 38; width: 280; text: voice.info.activationCode; horizontalAlignment: Text.AlignHCenter; color: "#83dcc0"; font.family: "Droid Sans Mono"; font.pixelSize: 36; font.bold: true }
        Text { y: 104; width: 280; text: "Add device using this code.\nThen press M to check activation."; horizontalAlignment: Text.AlignHCenter; color: "#aab8cb"; font.pixelSize: 13; lineHeight: 1.4; wrapMode: Text.Wrap }
        MouseArea { anchors.fill: parent; onClicked: voice.action("activate") }
    }
    Flickable {
        visible: !voice.info.activationCode
        x: 20; y: 106; width: 280; height: 176; clip: true
        contentHeight: transcript.height
        onMovementStarted: host.touchFocus()
        Column {
            id: transcript; width: 280; spacing: 12
            Text { width: 280; text: voice.info.userText ? "YOU\n" + voice.info.userText : ""; visible: !!text; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: "#afc3dd"; font.pixelSize: 16 }
            Text { width: 280; text: voice.info.assistantText || (voice.info.active ? "" : "Xiaozhi on Sogou E1\n\nREC: speak\nSTOP: send your turn\nM: activate / cancel"); textFormat: Text.PlainText; wrapMode: Text.Wrap; color: "#f2f5fa"; font.pixelSize: 17; lineHeight: 1.2 }
            Text { width: 280; text: voice.info.message || ""; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: voice.info.state === "error" ? "#ff9c9e" : "#83dcc0"; font.pixelSize: 13 }
        }
    }
}
