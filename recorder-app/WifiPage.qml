import QtQuick 2.9

Item {
    id: wifi
    property var host
    property var info: ({enabled:true, available:false, scanning:false, connecting:false, status:"Starting", ssid:"", ip:"", message:"", networks:[]})
    property var selected: null
    property string password: ""
    property bool reveal: false
    property bool editing: false
    property bool forgetting: false
    property bool waiting: false
    property bool fetching: false
    property int selection: 0
    property string localMessage: ""
    function refresh() {
        if (fetching) return
        fetching = true
        host.request("wifi", false, function(data, ok) {
            fetching = false
            if (ok) { info = data; selection = Math.max(0,Math.min(selection,info.networks.length - 1)) }
        })
    }
    function command(action, form, callback) {
        if (waiting) return
        waiting = true; localMessage = ""
        host.request("wifi/" + action, true, function(data, ok) {
            waiting = false
            if (!ok) localMessage = data.error || "Request failed"
            if (callback) callback(ok)
            refresh()
        }, form || "")
    }
    function open() { host.touchFocus(); selected = null; editing = false; forgetting = false; password = ""; localMessage = ""; refresh() }
    function choose(network) {
        host.touchFocus(); selected = network; password = ""; reveal = false; passwordKeys.reset(); forgetting = false; localMessage = ""
        editing = true
        if (!network.supported) localMessage = "This security type is unsupported."
    }
    function connect(useSaved) {
        passwordKeys.commit()
        if (!selected || !selected.supported) return
        var security = useSaved ? "saved" : selected.security
        if (security === "saved" && !useSaved) { localMessage = "Scan the network to change its password."; return }
        var form = "ssid=" + selected.ssidHex + "&security=" + encodeURIComponent(security) + "&password=" + encodeURIComponent(password)
        command("connect", form, function(ok) { if (ok) { password = ""; editing = false; selected = null } })
    }
    function back() {
        host.touchFocus()
        if (forgetting) forgetting = false
        else if (editing) { editing = false; selected = null; password = ""; localMessage = "" }
        else host.page = 0
    }
    function hardware(key) {
        host.touchFocus()
        if (key === Qt.Key_2) { back(); return }
        if (editing) {
            if (!forgetting && (key === Qt.Key_4 || key === Qt.Key_VolumeUp)) { passwordKeys.nextMode(); return }
            if (!forgetting && (key === Qt.Key_5 || key === Qt.Key_VolumeDown)) { passwordKeys.erase(); return }
            if (key === Qt.Key_1 && !forgetting && selected && selected.supported) connect(selected.saved && !password.length)
            return
        }
        if (key === Qt.Key_1) { if (info.networks.length) choose(info.networks[selection]); return }
        if (key === Qt.Key_4 || key === Qt.Key_VolumeUp || key === Qt.Key_5 || key === Qt.Key_VolumeDown) {
            selection = Math.max(0,Math.min(info.networks.length - 1,selection + (key === Qt.Key_4 || key === Qt.Key_VolumeUp ? -1 : 1)))
            networkList.positionViewAtIndex(selection,ListView.Contain)
        }
    }
    onVisibleChanged: if (visible) open(); else { password = ""; editing = false; selected = null }
    onEditingChanged: { passwordKeys.commit(); if (host) host.scheduleCapture() }
    Timer { interval: 1000; running: wifi.visible; repeat: true; onTriggered: wifi.refresh() }

    Item {
        visible: !wifi.editing; anchors.fill: parent
        Text { x: 20; y: 49; width: 193; text: info.status || "Starting"; color: info.ip ? "#83dcc0" : "#a9b6ca"; font.pixelSize: 14; elide: Text.ElideRight }
        Text { x: 230; y: 49; width: 70; horizontalAlignment: Text.AlignRight; text: info.enabled ? "ON" : "OFF"; color: info.enabled ? "#83dcc0" : "#8f9db2"; font.pixelSize: 14; font.bold: true }
        MouseArea { x: 220; y: 42; width: 83; height: 36; onClicked: { host.touchFocus(); wifi.command("enable", "enabled=" + (info.enabled ? "0" : "1")) } }
        Text { x: 20; y: 73; width: 235; text: info.ip ? info.ssid + " · " + info.ip : info.ssid || (info.enabled ? "Choose a network" : "Tap OFF to enable Wi-Fi"); color: "#8f9db2"; font.pixelSize: 10; elide: Text.ElideRight }
        Text { x: 254; y: 74; width: 46; horizontalAlignment: Text.AlignRight; text: info.scanning ? "…" : "SCAN"; color: "#83dcc0"; font.pixelSize: 10 }
        MouseArea { x: 248; y: 70; width: 58; height: 29; enabled: info.enabled && !info.scanning && !wifi.waiting; onClicked: { host.touchFocus(); wifi.command("scan") } }
        ListView {
            id: networkList
            x: 16; y: 100; width: 288; height: 154; spacing: 4; clip: true
            model: info.enabled ? info.networks : []; currentIndex: wifi.selection
            onMovementStarted: host.touchFocus()
            delegate: Rectangle {
                width: 288; height: 47; radius: 6; color: index === wifi.selection ? "#283b41" : "#1d2735"
                Text { x: 10; y: 6; width: 265; text: modelData.ssid; color: "#f2f5fa"; font.pixelSize: 13; elide: Text.ElideRight }
                Text { x: 10; y: 28; width: 265; text: (modelData.saved ? "Saved · " : "") + modelData.security + (modelData.signal > -100 ? " · " + modelData.signal + " dBm" : " · out of range"); color: modelData.supported ? "#8f9db2" : "#edc989"; font.pixelSize: 10 }
                MouseArea { anchors.fill: parent; onClicked: { wifi.selection = index; wifi.choose(modelData) } }
            }
        }
        Text { visible: info.enabled && !info.networks.length; x: 24; y: 149; width: 272; text: info.scanning ? "Searching for networks…" : "No networks found. Tap SCAN."; color: "#8f9db2"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter }
        Text { x: 20; y: 264; width: info.connecting ? 213 : 280; height: 25; text: wifi.localMessage || info.message || (info.connecting ? "Connecting…" : "Touch a network · ± choose · M open"); color: wifi.localMessage || info.message ? "#edc989" : "#8193ac"; font.pixelSize: 10; wrapMode: Text.Wrap }
        Text { visible: info.connecting; x: 246; y: 265; text: "CANCEL"; color: "#ff9c9e"; font.pixelSize: 10 }
        MouseArea { visible: info.connecting; x: 239; y: 255; width: 66; height: 35; onClicked: { host.touchFocus(); wifi.command("cancel") } }
    }
    Item {
        visible: wifi.editing; anchors.fill: parent
        Text { x: 20; y: 48; width: 280; text: wifi.selected ? wifi.selected.ssid : ""; color: "white"; font.pixelSize: 16; elide: Text.ElideRight }
        Rectangle {
            x: 20; y: 73; width: 280; height: 29; radius: 4; color: "#1d2735"
            Text { x: 8; anchors.verticalCenter: parent.verticalCenter; width: 204; elide: Text.ElideLeft; text: wifi.password.length ? (wifi.reveal ? wifi.password : Array(wifi.password.length + 1).join("•")) : wifi.selected && wifi.selected.saved ? "Saved password · tap CONNECT" : "Password"; color: wifi.password.length ? "#f2f5fa" : "#8193ac"; font.pixelSize: 12 }
            Text { x: 212; anchors.verticalCenter: parent.verticalCenter; text: wifi.password.length; color: "#8193ac"; font.pixelSize: 9 }
            Text { x: 238; anchors.verticalCenter: parent.verticalCenter; text: wifi.reveal ? "HIDE" : "SHOW"; color: "#83dcc0"; font.pixelSize: 10 }
            MouseArea { x: 232; width: 48; height: parent.height; onClicked: { host.touchFocus(); wifi.reveal = !wifi.reveal } }
        }
        Row {
            x: 20; y: 109; spacing: 8
            Rectangle {
                width: wifi.selected && wifi.selected.saved ? 175 : 280; height: 28; radius: 4; color: "#264c43"
                Text { anchors.centerIn: parent; text: wifi.waiting ? "WAIT…" : "CONNECT"; color: "#a3edcd"; font.pixelSize: 11; font.bold: true }
                MouseArea { anchors.fill: parent; enabled: !wifi.waiting; onClicked: { host.touchFocus(); wifi.connect(wifi.selected.saved && !wifi.password.length) } }
            }
            Rectangle {
                visible: wifi.selected ? wifi.selected.saved : false; width: 97; height: 28; radius: 4; color: "#432c32"
                Text { anchors.centerIn: parent; text: "FORGET"; color: "#ff9c9e"; font.pixelSize: 11 }
                MouseArea { anchors.fill: parent; onClicked: { host.touchFocus(); wifi.forgetting = true } }
            }
        }
        CompactKeyboard {
            id: passwordKeys
            x: 10; y: 143; width: 300; height: 144
            value: wifi.password
            onEdited: { host.touchFocus(); wifi.password = text }
        }
    }
    Rectangle {
        visible: wifi.forgetting; x: 10; y: 45; width: 300; height: 244; color: "#f510141c"; z: 5
        Text { x: 15; y: 35; width: 270; text: "Forget this network?\n\n" + (wifi.selected ? wifi.selected.ssid : ""); color: "white"; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap }
        Row {
            x: 20; y: 180; spacing: 10
            Repeater {
                model: ["CANCEL", "FORGET"]
                Rectangle {
                    width: 125; height: 36; radius: 5; color: index ? "#432c32" : "#253245"
                    Text { anchors.centerIn: parent; text: modelData; color: index ? "#ff9c9e" : "white"; font.pixelSize: 12 }
                    MouseArea { anchors.fill: parent; onClicked: { host.touchFocus(); if (index === 0) wifi.forgetting = false; else wifi.command("forget", "id=" + wifi.selected.id, function(ok) { if (ok) { wifi.forgetting = false; wifi.editing = false; wifi.selected = null; wifi.password = "" } }) } }
                }
            }
        }
    }
}
