import QtQuick 2.9

Item {
    id: keyboard
    width: 300; height: 144
    property string value: ""
    property int mode: 0 // lowercase, uppercase, digits, symbols
    property int pendingKey: -1
    property int pendingIndex: 0
    property int pendingPosition: -1
    property var letters: [".,?!1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"]
    property var punctuation: [".,?!", "@#$", "%^&*", "()[]", "{}<>", "-_=+", "/\\|", ";:'\"", "`~"]
    property var modes: ["abc", "ABC", "123", "#+="]
    signal edited(string text)

    function commit() { cycleTimer.stop(); pendingKey = -1; pendingIndex = 0; pendingPosition = -1 }
    function reset() { commit(); mode = 0 }
    function nextMode() { commit(); mode = (mode + 1) % 4 }
    function erase() { commit(); edited(value.slice(0,-1)) }
    function append(text) { commit(); if (value.length < 64) edited(value + text) }
    function group(key) {
        if (mode === 2) return String(key + 1)
        if (mode === 3) return punctuation[key]
        return mode === 1 ? letters[key].toUpperCase() : letters[key]
    }
    function press(key) {
        if (key === 9) { nextMode(); return }
        if (key === 10) { append(mode === 2 ? "0" : " "); return }
        if (key === 11) { erase(); return }
        if (mode === 2) { append(String(key + 1)); return }
        var chars = group(key)
        if (pendingKey === key && cycleTimer.running && pendingPosition === value.length - 1) {
            pendingIndex = (pendingIndex + 1) % chars.length
            edited(value.slice(0,-1) + chars[pendingIndex])
        } else {
            commit()
            if (value.length >= 64) return
            pendingKey = key; pendingIndex = 0; pendingPosition = value.length
            edited(value + chars[0])
        }
        cycleTimer.restart()
    }
    function hold(key) {
        if (key < 9 && mode < 2) append(String(key + 1))
        else press(key)
    }
    function label(key) {
        if (key === 9) return modes[mode] + " ›"
        if (key === 10) return mode === 2 ? "0" : "SPACE"
        if (key === 11) return "⌫"
        var chars = group(key)
        if (key === pendingKey) return chars[pendingIndex]
        return mode < 2 && key > 0 ? chars.slice(0,-1) : chars
    }
    onVisibleChanged: if (!visible) commit()
    Timer { id: cycleTimer; interval: 900; onTriggered: keyboard.commit() }

    Grid {
        anchors.fill: parent; columns: 3; spacing: 4
        Repeater {
            model: 12
            Rectangle {
                width: (keyboard.width - 8)/3; height: (keyboard.height - 12)/4; radius: 5
                color: touch.pressed ? "#46647a" : index === keyboard.pendingKey ? "#36594d" : index >= 9 ? "#304158" : "#253245"
                border.width: index === keyboard.pendingKey ? 1 : 0; border.color: "#83dcc0"
                Text { anchors.centerIn: parent; text: keyboard.label(index); color: "#f2f5fa"; font.pixelSize: index === 10 ? 12 : 18 }
                Text { visible: index < 9 && keyboard.mode < 2; x: 6; y: 3; text: index + 1; color: "#96a9be"; font.pixelSize: 9 }
                MouseArea {
                    id: touch; anchors.fill: parent
                    property bool held: false
                    onPressed: { held = false; keyboard.forceActiveFocus() }
                    onPressAndHold: { held = true; keyboard.hold(index) }
                    onClicked: if (!held) keyboard.press(index)
                    onCanceled: held = false
                }
            }
        }
    }
}
