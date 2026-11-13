import QtQuick 2.9

Rectangle {
    id: app
    width: 320; height: 320; color: "#10141c"
    property int page: 4
    property var audio: ({state:"idle", leftDb:-96, rightDb:-96, leftRms:-96, rightRms:-96, elapsed:0, position:0, duration:0, marks:[], left:[], right:[], error:"", captureReady:false})
    property var files: []
    property int fileIndex: 0
    property var preview: ({name:"", duration:0, marks:[], left:[], right:[]})
    property real previewPosition: 0
    property real freeBytes: 0
    property string message: "Connecting to audio…"
    property bool polling: false
    property bool mHomePress: false
    property bool touchSeeking: false
    property bool powerHeld: false
    property bool shutdownPending: false
    property int powerSeconds: 3
    property bool recording: audio.state === "recording"
    property bool playing: audio.state === "playing"
    property bool networking: audio.state === "network"
    property bool busy: recording || playing || networking || audio.state === "assistant"
    property string api: "http://127.0.0.1:18770/api/"
    focus: true
    Keys.enabled: true
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: {
        var key = event.key
        if (isPowerKey(key)) {
            event.accepted = true
            if (!event.isAutoRepeat && !powerHeld && !shutdownPending) {
                console.log("E1_POWER_PRESS " + key)
                mHomePress = false; mHoldTimer.stop(); exitPrompt.visible = false
                powerHeld = true; powerSeconds = 3; powerHoldTimer.restart()
            }
            return
        }
        if (powerHeld || shutdownPending) { event.accepted = true; return }
        if (key !== Qt.Key_1 && key !== Qt.Key_2 && key !== Qt.Key_3 && key !== Qt.Key_4 && key !== Qt.Key_5
                && key !== Qt.Key_VolumeUp && key !== Qt.Key_VolumeDown) { event.accepted = false; return }
        event.accepted = true
        if (event.isAutoRepeat) return
        console.log("E1_HARDWARE_KEY " + key)
        hardwareKey(key)
    }
    Keys.onReleased: {
        if (isPowerKey(event.key)) {
            event.accepted = true
            if (!event.isAutoRepeat) { console.log("E1_POWER_RELEASE"); powerHeld = false; powerHoldTimer.stop() }
            return
        }
        if (event.key !== Qt.Key_1) { event.accepted = false; return }
        event.accepted = true
        if (event.isAutoRepeat) return
        if (mHomePress) {
            mHomePress = false
            if (mHoldTimer.running) { mHoldTimer.stop(); showFiles() }
        }
    }
    onPageChanged: captureTimer.restart()
    onRecordingChanged: captureTimer.restart()
    onPlayingChanged: captureTimer.restart()
    function scheduleCapture() { captureTimer.restart() }
    function isPowerKey(key) { return key === Qt.Key_6 || key === Qt.Key_PowerOff }

    function timeText(t, precise) {
        t = Math.max(0, t || 0)
        var m = Math.floor(t / 60), s = Math.floor(t % 60), h = Math.floor(t * 100) % 100
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s + (precise ? "." + (h < 10 ? "0" : "") + h : "")
    }
    function title(name) {
        var m = /^REC-(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/.exec(name)
        return m ? m[2] + "/" + m[3] + "  " + m[4] + ":" + m[5] + ":" + m[6] : name
    }
    function request(path, post, callback, form) {
        var xhr = new XMLHttpRequest()
        xhr.open(post ? "POST" : "GET", api + path)
        if (post) xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var data
            try { data = JSON.parse(xhr.responseText) } catch(e) { data = {error:"Audio service unavailable"} }
            if (xhr.status !== 200 || data.error) { app.message = data.error || "Audio request failed"; notice.restart() }
            if (callback) callback(data, xhr.status === 200 && !data.error)
        }
        xhr.send(form || "")
    }
    function refreshFiles() {
        request("files", false, function(data, ok) {
            if (!ok) return
            var selected = files.length && fileIndex < files.length ? files[fileIndex].name : ""
            files = data.files; freeBytes = data.freeBytes
            fileIndex = Math.max(0, Math.min(fileIndex, files.length - 1))
            for (var i = 0; i < files.length; i++) if (files[i].name === selected) fileIndex = i
            if (!preview.name && files.length) loadWave(files[0].name, false)
        })
    }
    function loadWave(name, show) {
        request("wave?file=" + encodeURIComponent(name), false, function(data, ok) {
            if (!ok) return
            preview = data; previewPosition = 0
            if (show) page = 2
        })
    }
    function poll() {
        if (polling) return
        polling = true
        request("status", false, function(data, ok) {
            polling = false
            if (!ok) return
            var old = audio.state
            audio = data
            if (data.state === "network" && old !== "network") page = 5
            if (data.shutdownFailed && shutdownPending) { shutdownPending = false; powerHeld = false; powerHoldTimer.stop() }
            if (data.error) message = data.error
            else if (message === "Connecting to audio…") message = ""
            if (data.state === "playing" && data.file === preview.name && !touchSeeking) previewPosition = data.position
            if (old === "recording" && data.state === "idle") {
                refreshFiles(); loadWave(data.file, true)
                message = data.error || "Recording saved"; notice.restart()
            }
            if (old === "playing" && data.state === "idle") previewPosition = data.position
        })
    }
    function seek(seconds) {
        previewPosition = Math.min(preview.duration,Math.max(0,seconds))
        if (playing) request("seek?position=" + previewPosition.toFixed(6), true)
    }
    function seekStep(forward) {
        var position = previewPosition
        if (forward) {
            for (var i = 0; i < preview.marks.length; i++)
                if (preview.marks[i].seconds > position + 0.08) { seek(preview.marks[i].seconds); return }
        } else {
            for (var j = preview.marks.length - 1; j >= 0; j--)
                if (preview.marks[j].seconds < position - 0.08) { seek(preview.marks[j].seconds); return }
        }
        seek(Math.min(preview.duration,Math.max(0,position + (forward ? 5 : -5))))
    }
    function startRecording() { page = 0; message = ""; request("record", true) }
    function selectPreviewFile() {
        for (var i = 0; i < files.length; i++) if (files[i].name === preview.name) fileIndex = i
    }
    function showFiles() { refreshFiles(); selectPreviewFile(); page = 1 }
    function touchFocus() { mHomePress = false; mHoldTimer.stop(); forceActiveFocus() }
    function goBack() {
        touchFocus()
        if (page === 5) { networkPage.stop(); page = 0; return }
        if (page === 3) { wifiPage.back(); return }
        if (page === 4) { if (assistantPage.info.active) assistantPage.action("cancel"); page = 0; return }
        if (playing) request("stop",true)
        selectPreviewFile()
        page = page === 2 ? 1 : 0
    }
    function playPreview() {
        if (playing) { request("stop",true); return }
        if (!recording && preview.name && preview.duration > 0)
            request("play?file=" + encodeURIComponent(preview.name) + "&position=" + previewPosition.toFixed(6),true)
    }
    function hardwareKey(key) {
        if (key !== Qt.Key_1) { mHomePress = false; mHoldTimer.stop() }
        if (exitPrompt.visible) {
            if (key === Qt.Key_2) exitPrompt.visible = false
            else if (key === Qt.Key_1) Qt.quit()
            return
        }
        if (page === 5 || networking) { page = 5; networkPage.hardware(key); return }
        if (page === 4) { assistantPage.hardware(key); return }
        if (audio.state === "assistant") { if (key === Qt.Key_2) request("stop",true); return }
        if (key === Qt.Key_3) {
            if (!busy && audio.captureReady) startRecording()
            return
        }
        if (page === 3) { wifiPage.hardware(key); return }
        if (key === Qt.Key_2) {
            request("stop",true)
            if (!busy) { selectPreviewFile(); page = page === 2 ? 1 : 0 }
            return
        }
        if (key === Qt.Key_1) {
            if (recording) { request("mark",true); message = "Mark added"; notice.restart() }
            else if (page === 0) { mHomePress = true; mHoldTimer.restart() }
            else if (page === 1 && files.length) loadWave(files[fileIndex].name,true)
            else if (page === 2) playPreview()
            return
        }
        if (recording) return
        var up = key === Qt.Key_4 || key === Qt.Key_VolumeUp
        if (page === 0 && up && !busy) { page = 3; return }
        if (page === 2) seekStep(up)
        else {
            if (page === 0) showFiles()
            fileIndex = Math.max(0,Math.min(files.length - 1,fileIndex + (up ? -1 : 1)))
            recordingsList.positionViewAtIndex(fileIndex,ListView.Contain)
        }
    }

    Component.onCompleted: { forceActiveFocus(); poll(); refreshFiles(); console.log("E1_RECORDER_UI_READY 1.0.0") }
    Timer { interval: 100; running: true; repeat: true; onTriggered: app.poll() }
    Timer { interval: 15000; running: true; repeat: true; onTriggered: if (!app.busy) app.refreshFiles() }
    Timer { id: notice; interval: 5000; onTriggered: if (!app.audio.error) app.message = "" }
    Timer { id: mHoldTimer; interval: 1200; onTriggered: if (!app.busy && app.page === 0) exitPrompt.visible = true }
    Timer {
        id: powerHoldTimer; interval: 3000
        onTriggered: {
            if (!app.powerHeld || app.shutdownPending) return
            app.shutdownPending = true
            console.log("E1_POWER_LONG_PRESS 3000")
            app.request("poweroff",true,function(data,ok) {
                if (!ok) { app.shutdownPending = false; app.powerHeld = false }
            })
        }
    }
    Timer { interval: 1000; running: app.powerHeld && !app.shutdownPending; repeat: true; onTriggered: app.powerSeconds = Math.max(1,app.powerSeconds - 1) }
    Timer {
        id: captureTimer
        interval: 1800; running: true
        onTriggered: if (page !== 4 && (page !== 3 || !wifiPage.editing || !wifiPage.password.length)) app.grabToImage(function(result) { result.saveToFile("/userdata/e1-recorder/screenshot.png") })
    }

    Item {
        x: 18; y: 10; width: 285; height: 32
        Text {
            x: page ? 28 : 0; y: 5
            text: page === 0 ? "RECORDER" : page === 1 ? "Recordings" : page === 3 ? "Wi-Fi" : page === 4 ? "XIAOZHI" : page === 5 ? "Wi-Fi Audio" : "Playback"
            color: "#f2f5fa"; font.family: "Roboto"; font.bold: true; font.pixelSize: 17
        }
        Text { visible: page !== 0; x: 0; y: -2; text: "‹"; color: "#aab8cb"; font.pixelSize: 31 }
        MouseArea {
            visible: page !== 0; x: -8; y: -5; width: 45; height: 42
            onClicked: app.goBack()
        }
        Text { visible: page === 0; x: 195; y: 7; text: "WI-FI"; color: "#83dcc0"; font.pixelSize: 10; font.bold: true; opacity: busy ? 0.35 : 1 }
        MouseArea { visible: page === 0; enabled: !busy; x: 190; y: -3; width: 45; height: 40; onClicked: { app.touchFocus(); app.page = 3 } }
        Text { visible: page === 0; x: 107; y: 7; text: "VOICE"; color: "#83dcc0"; font.pixelSize: 10; font.bold: true; opacity: busy ? 0.35 : 1 }
        MouseArea { visible: page === 0; enabled: !busy; x: 100; y: -3; width: 45; height: 40; onClicked: { app.touchFocus(); app.page = 4 } }
        Text { visible: page === 0; x: 151; y: 7; text: "AUDIO"; color: "#83dcc0"; font.pixelSize: 10; font.bold: true; opacity: busy ? 0.35 : 1 }
        MouseArea { visible: page === 0; enabled: !busy; x: 145; y: -3; width: 45; height: 40; onClicked: { app.touchFocus(); app.page = 5 } }
        Text { x: page === 0 ? 235 : 207; y: 7; width: page === 0 ? 50 : 78; horizontalAlignment: Text.AlignRight; text: page === 0 ? "FILES" : page === 1 ? "± SELECT" : page === 3 ? "NETWORK" : page === 4 ? "WI-FI" : "HARD KEYS"; color: "#83dcc0"; font.pixelSize: 10; font.bold: true; opacity: recording ? 0.35 : 1 }
        MouseArea {
            visible: page === 0 || page === 4; enabled: !busy; x: page === 0 ? 235 : 205; y: -3; width: page === 0 ? 50 : 80; height: 40
            onClicked: { app.touchFocus(); if (page === 4) page = 3; else app.showFiles() }
        }
    }

    Item {
        visible: page === 0; anchors.fill: parent
        Rectangle {
            x: 20; y: 49; width: recording ? 83 : 62; height: 20; radius: 4
            color: recording ? "#4c2631" : "#20372f"
            Text { anchors.centerIn: parent; text: recording ? "●  REC" : audio.captureReady ? "READY" : "WAIT"; color: recording ? "#ff8795" : "#83dcc0"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1 }
        }
        Text { x: 111; y: 53; width: 189; horizontalAlignment: Text.AlignRight; text: "48 kHz · 24 bit · STEREO"; color: "#8f9db2"; font.pixelSize: 10 }
        Text {
            x: 20; y: 77; width: 280; horizontalAlignment: Text.AlignHCenter
            text: timeText(recording ? audio.elapsed : 0, true)
            color: recording ? "#ff8894" : "#f2f5fa"; font.family: "Droid Sans Mono"; font.pixelSize: 33
        }
        LevelMeter { x: 20; y: 123; channel: "L"; peak: audio.leftDb; rms: audio.leftRms }
        LevelMeter { x: 20; y: 147; channel: "R"; peak: audio.rightDb; rms: audio.rightRms }
        Waveform {
            x: 20; y: 177; width: 280; height: 76
            samplesLeft: recording ? audio.left : preview.left
            samplesRight: recording ? audio.right : preview.right
            marks: recording ? audio.marks : preview.marks
            duration: recording ? audio.elapsed : preview.duration
            position: recording ? audio.elapsed : 0
        }
        Text {
            x: 20; y: 263; width: 280; elide: Text.ElideMiddle
            text: recording ? audio.marks.length + " marks  ·  " + title(audio.file) : "WAV PCM  ·  /Recordings"
            color: "#8e9cb1"; font.pixelSize: 10
        }
    }

    Item {
        visible: page === 1; anchors.fill: parent
        Text { x: 20; y: 49; text: files.length + " WAV files  ·  " + (freeBytes / (1024*1024*1024)).toFixed(1) + " GB free"; color: "#8f9db2"; font.pixelSize: 11 }
        ListView {
            id: recordingsList
            x: 16; y: 75; width: 288; height: 207; clip: true; spacing: 6; interactive: true
            onMovementStarted: app.touchFocus()
            onMovementEnded: app.touchFocus()
            model: files
            currentIndex: app.fileIndex
            delegate: Rectangle {
                width: 288; height: 54; radius: 8; color: index === app.fileIndex ? "#283b41" : "#1d2735"
                border.width: index === app.fileIndex ? 1 : 0
                border.color: "#83dcc0"
                Text { x: 13; y: 9; width: 245; text: title(modelData.name); color: "#eaf0fa"; font.pixelSize: 14; elide: Text.ElideRight }
                Text { x: 13; y: 32; text: timeText(modelData.duration,false) + "   ·   " + (modelData.bytes/(1024*1024)).toFixed(1) + " MB   ·   24-bit stereo"; color: "#8fa0b8"; font.pixelSize: 10 }
                Text { x: 265; y: 13; text: "›"; color: "#81dabb"; font.pixelSize: 24 }
                MouseArea {
                    anchors.fill: parent
                    onPressed: app.touchFocus()
                    onClicked: { app.fileIndex = index; app.loadWave(modelData.name,true); app.forceActiveFocus() }
                    onCanceled: app.forceActiveFocus()
                }
            }
        }
        Text { visible: !files.length; x: 20; y: 134; width: 280; horizontalAlignment: Text.AlignHCenter; text: "No recordings yet.\nPress REC to begin."; color: "#8f9db2"; font.pixelSize: 17; lineHeight: 1.7 }
    }

    Item {
        visible: page === 2; anchors.fill: parent
        Text { x: 20; y: 48; width: 280; elide: Text.ElideMiddle; text: title(preview.name); color: "#9aaac0"; font.pixelSize: 12 }
        Text { x: 20; y: 74; text: timeText(previewPosition,true); color: "#f0f5fc"; font.family: "Droid Sans Mono"; font.pixelSize: 23 }
        Text { x: 183; y: 81; width: 117; horizontalAlignment: Text.AlignRight; text: "/ " + timeText(preview.duration,false); color: "#8f9db2"; font.pixelSize: 14 }
        Waveform {
            id: previewWave
            x: 20; y: 112; width: 280; height: 121
            samplesLeft: preview.left; samplesRight: preview.right; marks: preview.marks; duration: preview.duration; position: previewPosition
            interactive: true
            onInteractionStarted: { app.touchFocus(); app.touchSeeking = true }
            onSeekPreview: app.previewPosition = seconds
            onSeekRequested: app.seek(seconds)
            onInteractionFinished: { app.touchSeeking = false; app.forceActiveFocus() }
        }
        Flickable {
            x: 20; y: 243; width: 280; height: 25; clip: true; interactive: true
            onMovementStarted: app.touchFocus()
            onMovementEnded: app.touchFocus()
            contentWidth: markRow.width
            Row {
                id: markRow; spacing: 6
                Repeater {
                    model: preview.marks
                    Rectangle {
                        width: 78; height: 24; radius: 5; color: "#3a3025"
                        Text { anchors.centerIn: parent; text: "◆ " + modelData.id + "  " + timeText(modelData.seconds,false); color: "#edc989"; font.pixelSize: 10 }
                        MouseArea {
                            anchors.fill: parent
                            onPressed: app.touchFocus()
                            onClicked: { app.seek(modelData.seconds); app.forceActiveFocus() }
                            onCanceled: app.forceActiveFocus()
                        }
                    }
                }
            }
            Text { visible: !preview.marks.length; text: "No marks  ·  touch waveform to seek"; color: "#7f8fa6"; font.pixelSize: 10 }
        }
        Text { x: 20; y: 274; text: playing ? "▶ PLAYING" : "■ STOPPED"; color: playing ? "#83dcc0" : "#8f9db2"; font.pixelSize: 10; font.bold: true }
    }
    NetworkPage { id: networkPage; visible: page === 5; anchors.fill: parent; host: app }
    AssistantPage { id: assistantPage; visible: page === 4; anchors.fill: parent; host: app }
    WifiPage { id: wifiPage; visible: page === 3; anchors.fill: parent; host: app }
    Rectangle { x: 20; y: 291; width: 280; height: 1; color: "#2a3442" }
    Text {
        x: 20; y: 300; width: 280; elide: Text.ElideRight
        text: page === 5 ? (networkPage.info.armed ? "STOP disable · touch status to disable" : "REC enable · M Wi-Fi · STOP back") : page === 4 ? (assistantPage.info.active ? "STOP send/cancel · M cancel · ± volume" : "REC talk · M activate · ‹ recorder · Wi-Fi") : page === 3 ? wifiPage.localMessage || wifiPage.info.message || (wifiPage.editing ? "Tap to cycle · hold for digit · abc › modes" : "STOP back · REC record") : message || (page === 1 ? "± choose · M open · STOP back" : page === 2 ? "M play/stop · ± marks/seek · STOP back" : recording ? "M mark · STOP save" : "REC record · M files · + Wi-Fi · hold M exit")
        color: audio.error ? "#ff9c9e" : "#8193ac"; font.pixelSize: 10
    }
    Rectangle {
        id: exitPrompt; visible: false; anchors.fill: parent; color: "#ed10141c"; z: 20
        Text { x: 24; y: 81; width: 272; text: "Return to the\nfactory recorder?"; horizontalAlignment: Text.AlignHCenter; color: "white"; font.pixelSize: 23 }
        Text { x: 24; y: 182; width: 272; text: "M: return to factory app"; horizontalAlignment: Text.AlignHCenter; color: "#83dcc0"; font.pixelSize: 17 }
        Text { x: 24; y: 225; width: 272; text: "STOP: cancel"; horizontalAlignment: Text.AlignHCenter; color: "#a9b6ca"; font.pixelSize: 15 }
    }
    Rectangle {
        visible: powerHeld || shutdownPending; anchors.fill: parent; color: "#f510141c"; z: 30
        Text {
            x: 24; y: 93; width: 272; horizontalAlignment: Text.AlignHCenter
            text: shutdownPending ? "Shutting down…" : "Hold POWER"
            color: "#f2f5fa"; font.pixelSize: 25; font.bold: true
        }
        Text {
            x: 24; y: 150; width: 272; horizontalAlignment: Text.AlignHCenter
            text: shutdownPending ? "Saving audio and syncing storage" : powerSeconds + " s"
            color: "#83dcc0"; font.pixelSize: shutdownPending ? 13 : 32
        }
        Text { visible: !shutdownPending; x: 24; y: 220; width: 272; text: "Release to cancel"; horizontalAlignment: Text.AlignHCenter; color: "#8f9db2"; font.pixelSize: 15 }
        MouseArea { anchors.fill: parent }
    }
}
