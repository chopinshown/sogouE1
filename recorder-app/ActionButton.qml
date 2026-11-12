import QtQuick 2.9

Rectangle {
    id: button
    property string label: ""
    property string tone: "normal"
    signal activated()
    radius: 10
    color: area.pressed ? "#3a4658" : tone === "record" ? "#d94b5c" : tone === "primary" ? "#78dcb5" : "#263140"
    opacity: enabled ? 1 : 0.35
    Text {
        anchors.centerIn: parent
        text: button.label
        color: button.tone === "primary" ? "#10221b" : "#f5f7fb"
        font.family: "Roboto"
        font.pixelSize: 15
        font.bold: true
    }
    MouseArea { id: area; anchors.fill: parent; onClicked: button.activated() }
}
