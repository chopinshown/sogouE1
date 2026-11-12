import QtQuick 2.9
import "Build.js" as Build

Rectangle {
    id: screen
    width: 320
    height: 320
    color: "#10141c"
    property int taps: 0

    Text {
        x: 24; y: 21
        text: "E1 / APP LAB"
        color: "#8792a5"
        font.family: "Roboto"
        font.pixelSize: 12
        font.letterSpacing: 2
    }
    Rectangle {
        x: 231; y: 18; width: 65; height: 25; radius: 12
        color: "#23313c"
        Text {
            anchors.centerIn: parent
            text: "v" + Build.version
            color: Build.accent
            font.family: "Roboto"
            font.pixelSize: 12
        }
    }
    Text {
        x: 24; y: 60
        text: "Hello,\nworld."
        lineHeight: 0.95
        color: "#f6f8fc"
        font.family: "Roboto"
        font.bold: true
        font.pixelSize: 48
    }
    Row {
        x: 26; y: 182; spacing: 9
        Rectangle {
            width: 7; height: 7; radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: Build.accent
        }
        Text {
            text: Build.status
            color: Build.accent
            font.family: "Roboto"
            font.pixelSize: 12
            font.letterSpacing: 1
        }
    }
    Rectangle {
        id: tapButton
        x: 24; y: 216; width: 272; height: 39; radius: 10
        color: tapArea.pressed ? "#354353" : "#25303e"
        Text {
            anchors.centerIn: parent
            text: screen.taps ? "Touch works  /  " + screen.taps : "Tap to say hello"
            color: "#e2e9f4"
            font.family: "Roboto"
            font.pixelSize: 15
        }
        MouseArea {
            id: tapArea
            anchors.fill: parent
            onClicked: screen.taps++
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 278
        text: "Back to recorder"
        color: "#a2aec1"
        font.family: "Roboto"
        font.pixelSize: 13
    }
    MouseArea {
        x: 24; y: 264; width: 272; height: 48
        onClicked: Qt.quit()
    }
    Timer {
        interval: 1800
        running: true
        repeat: false
        onTriggered: {
            console.log("E1_HELLO_READY " + Build.version)
            var accepted = screen.grabToImage(function(result) {
                var saved = result.saveToFile("/userdata/e1-hello/screenshots/" + Build.version + ".png")
                console.log("E1_HELLO_SCREENSHOT " + Build.version + " " + saved)
            })
            if (!accepted)
                console.log("E1_HELLO_SCREENSHOT_UNAVAILABLE " + Build.version)
        }
    }
}
