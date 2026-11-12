import QtQuick 2.9

Rectangle {
    id: app
    width: 320; height: 320; color: "#10141c"
    property int page: 0
    property var audio: ({state:"idle", leftDb:-96, rightDb:-96, leftRms:-96, rightRms:-96, elapsed:0, position:0, duration:0, marks:[], left:[], right:[], error:"", captureReady:false})
    property var files: []
    property var preview: ({name:"", duration:0, marks:[], left:[], right:[]})
    property real previewPosition: 0
    property real freeBytes: 0
    property string message: "Connecting to audio…"
    property bool polling: false
    property bool recording: audio.state === "recording"
    property bool playing: audio.state === "playing"
    property bool busy: recording || playing
    property string api: "http://127.0.0.1:18770/api/"
    onPageChanged: captureTimer.restart()
    onRecordingChanged: captureTimer.restart()
    onPlayingChanged: captureTimer.restart()

    function timeText(t, precise) {
        t = Math.max(0, t || 0)
        var m = Math.floor(t / 60), s = Math.floor(t % 60), h = Math.floor(t * 100) % 100
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s + (precise ? "." + (h < 10 ? "0" : "") + h : "")
    }
    function title(name) {
        var m = /^REC-(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/.exec(name)
        return m ? m[2] + "/" + m[3] + "  " + m[4] + ":" + m[5] + ":" + m[6] : name
    }
    function request(path, post, callback) {
        var xhr = new XMLHttpRequest()
        xhr.open(post ? "POST" : "GET", api + path)
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var data
            try { data = JSON.parse(xhr.responseText) } catch(e) { data = {error:"Audio service unavailable"} }
            if (xhr.status !== 200 || data.error) { app.message = data.error || "Audio request failed"; notice.restart() }
            if (callback) callback(data, xhr.status === 200 && !data.error)
        }
        xhr.send("")
    }
    function refreshFiles() {
        request("files", false, function(data, ok) {
            if (!ok) return
            files = data.files; freeBytes = data.freeBytes
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
            if (data.error) message = data.error
            else if (message === "Connecting to audio…") message = ""
            if (data.state === "playing" && data.file === preview.name) previewPosition = data.position
            if (old === "recording" && data.state === "idle") {
                refreshFiles(); loadWave(data.file, true)
                message = data.error || "Recording saved"; notice.restart()
            }
            if (old === "playing" && data.state === "idle") previewPosition = data.position
        })
    }
    function seek(seconds) {
        previewPosition = seconds
        if (playing) request("seek?position=" + seconds.toFixed(6), true)
    }
    function startRecording() { page = 0; message = ""; request("record", true) }

    Component.onCompleted: { poll(); refreshFiles(); console.log("E1_RECORDER_UI_READY 1.0.0") }
    Timer { interval: 100; running: true; repeat: true; onTriggered: app.poll() }
    Timer { interval: 15000; running: true; repeat: true; onTriggered: if (!app.busy) app.refreshFiles() }
    Timer { id: notice; interval: 5000; onTriggered: if (!app.audio.error) app.message = "" }
    Timer {
        id: captureTimer
        interval: 1800; running: true
        onTriggered: app.grabToImage(function(result) { result.saveToFile("/userdata/e1-recorder/screenshot.png") })
    }

    Item {
        x: 18; y: 10; width: 285; height: 32
        Text {
            x: page ? 28 : 0; y: 5
            text: page === 0 ? "RECORDER" : page === 1 ? "Recordings" : "Playback"
            color: "#f2f5fa"; font.family: "Roboto"; font.bold: true; font.pixelSize: 17
        }
        Text { visible: page !== 0; x: 0; y: -2; text: "‹"; color: "#aab8cb"; font.pixelSize: 31 }
        MouseArea { visible: page !== 0; x: -8; y: -5; width: 40; height: 42; onClicked: { if (playing) request("stop", true); page = page === 2 ? 1 : 0 } }
        Text { visible: page === 0; x: 232; y: 7; text: "FILES"; color: "#83dcc0"; font.pixelSize: 12; font.bold: true; opacity: recording ? 0.3 : 1 }
        MouseArea { visible: page === 0; enabled: !recording; x: 218; y: -3; width: 66; height: 40; onClicked: { refreshFiles(); page = 1 } }
        MouseArea { visible: page === 0; x: 0; y: 0; width: 170; height: 35; onPressAndHold: if (!busy) exitPrompt.visible = true }
    }

    Item {
        visible: page === 0; anchors.fill: parent
        Rectangle {
            x: 20; y: 49; width: recording ? 83 : 62; height: 20; radius: 4
            color: recording ? "#4c2631" : "#20372f"
            Text { anchors.centerIn: parent; text: recording ? "●  REC" : audio.captureReady ? "READY" : "WAIT"; color: recording ? "#ff8795" : "#83dcc0"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1 }
        }
        Text { x: 111; y: 53; width: 189; horizontalAlignment: Text.AlignRight; text: "44.1 kHz · 24 bit · STEREO"; color: "#8f9db2"; font.pixelSize: 10 }
        Text {
            x: 20; y: 77; width: 280; horizontalAlignment: Text.AlignHCenter
            text: timeText(recording ? audio.elapsed : 0, true)
            color: recording ? "#ff8894" : "#f2f5fa"; font.family: "Droid Sans Mono"; font.pixelSize: 33
        }
        LevelMeter { x: 20; y: 123; channel: "L"; peak: audio.leftDb; rms: audio.leftRms }
        LevelMeter { x: 20; y: 147; channel: "R"; peak: audio.rightDb; rms: audio.rightRms }
        Waveform {
            x: 20; y: 177; width: 280; height: 48
            samplesLeft: recording ? audio.left : preview.left
            samplesRight: recording ? audio.right : preview.right
            marks: recording ? audio.marks : preview.marks
            duration: recording ? audio.elapsed : preview.duration
            position: recording ? audio.elapsed : 0
        }
        Text {
            x: 20; y: 232; width: 280; elide: Text.ElideMiddle
            text: recording ? audio.marks.length + " marks  ·  " + title(audio.file) : "WAV PCM  ·  /Recordings"
            color: "#8e9cb1"; font.pixelSize: 10
        }
        ActionButton { x: 20; y: 255; width: 114; height: 44; label: "● Record"; tone: "record"; enabled: !busy && audio.captureReady; onActivated: startRecording() }
        ActionButton { x: 142; y: 255; width: 76; height: 44; label: "◆ Mark"; enabled: recording; onActivated: { request("mark", true); message="Mark added"; notice.restart() } }
        ActionButton { x: 226; y: 255; width: 74; height: 44; label: "■ Stop"; enabled: busy; onActivated: request("stop", true) }
    }

    Item {
        visible: page === 1; anchors.fill: parent
        Text { x: 20; y: 49; text: files.length + " WAV files  ·  " + (freeBytes / (1024*1024*1024)).toFixed(1) + " GB free"; color: "#8f9db2"; font.pixelSize: 11 }
        ListView {
            x: 16; y: 75; width: 288; height: 217; clip: true; spacing: 6
            model: files
            delegate: Rectangle {
                width: 288; height: 54; radius: 8; color: fileArea.pressed ? "#344255" : "#1d2735"
                Text { x: 13; y: 9; width: 245; text: title(modelData.name); color: "#eaf0fa"; font.pixelSize: 14; elide: Text.ElideRight }
                Text { x: 13; y: 32; text: timeText(modelData.duration,false) + "   ·   " + (modelData.bytes/(1024*1024)).toFixed(1) + " MB   ·   24-bit stereo"; color: "#8fa0b8"; font.pixelSize: 10 }
                Text { x: 265; y: 13; text: "›"; color: "#81dabb"; font.pixelSize: 24 }
                MouseArea { id: fileArea; anchors.fill: parent; onClicked: loadWave(modelData.name,true) }
            }
        }
        Text { visible: !files.length; x: 20; y: 134; width: 280; horizontalAlignment: Text.AlignHCenter; text: "No recordings yet.\nTap Record to begin."; color: "#8f9db2"; font.pixelSize: 17; lineHeight: 1.7 }
    }

    Item {
        visible: page === 2; anchors.fill: parent
        Text { x: 20; y: 48; width: 280; elide: Text.ElideMiddle; text: title(preview.name); color: "#9aaac0"; font.pixelSize: 12 }
        Text { x: 20; y: 74; text: timeText(previewPosition,true); color: "#f0f5fc"; font.family: "Droid Sans Mono"; font.pixelSize: 23 }
        Text { x: 183; y: 81; width: 117; horizontalAlignment: Text.AlignRight; text: "/ " + timeText(preview.duration,false); color: "#8f9db2"; font.pixelSize: 14 }
        Waveform {
            x: 20; y: 112; width: 280; height: 100
            samplesLeft: preview.left; samplesRight: preview.right; marks: preview.marks; duration: preview.duration; position: previewPosition
            interactive: true; onSeekRequested: app.seek(seconds)
        }
        Flickable {
            x: 20; y: 220; width: 280; height: 25; clip: true
            contentWidth: markRow.width
            Row {
                id: markRow; spacing: 6
                Repeater {
                    model: preview.marks
                    Rectangle {
                        width: 78; height: 24; radius: 5; color: "#3a3025"
                        Text { anchors.centerIn: parent; text: "◆ " + modelData.id + "  " + timeText(modelData.seconds,false); color: "#edc989"; font.pixelSize: 10 }
                        MouseArea { anchors.fill: parent; onClicked: app.seek(modelData.seconds) }
                    }
                }
            }
            Text { visible: !preview.marks.length; text: "Tap waveform to seek  ·  no marks"; color: "#7f8fa6"; font.pixelSize: 10 }
        }
        ActionButton { x: 20; y: 255; width: 130; height: 44; label: playing ? "Playing…" : "▶ Play"; tone: "primary"; enabled: !busy && preview.duration>0; onActivated: request("play?file="+encodeURIComponent(preview.name)+"&position="+previewPosition.toFixed(6),true) }
        ActionButton { x: 158; y: 255; width: 80; height: 44; label: "■ Stop"; enabled: playing; onActivated: request("stop",true) }
        ActionButton { x: 246; y: 255; width: 54; height: 44; label: "●"; tone: "record"; enabled: !busy; onActivated: startRecording() }
    }
    Text { x: 20; y: 304; width: 280; elide: Text.ElideRight; text: message || ""; color: audio.error ? "#ff9c9e" : "#8193ac"; font.pixelSize: 9 }
    Rectangle {
        id: exitPrompt; visible: false; anchors.fill: parent; color: "#ed10141c"; z: 20
        Text { x: 24; y: 81; width: 272; text: "Return to the\nfactory recorder?"; horizontalAlignment: Text.AlignHCenter; color: "white"; font.pixelSize: 23 }
        ActionButton { x: 24; y: 176; width: 272; height: 45; label: "Back to factory app"; onActivated: Qt.quit() }
        ActionButton { x: 24; y: 231; width: 272; height: 45; label: "Cancel"; onActivated: exitPrompt.visible=false }
    }
}
