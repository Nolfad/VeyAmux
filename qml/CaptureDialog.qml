import QtQuick

// Modal for choosing a V4L2 capture card (or a window / screen through the compositor's share dialog), its video
// mode and the audio source.
Item {
    id: dlg
    property var tr: function (k) { return k }
    property var devices: []
    property var modes: []
    property var sources: []        // [{name, desc}]
    property int devIdx: 0
    property int modeIdx: 0
    property int srcIdx: 0          // 0 = no audio
    property string help: ""        // why no card is listed
    signal closed()
    signal remotePlay()             // "PlayStation Remote Play" was chosen

    visible: false
    function openDialog() { refresh(); visible = true }
    readonly property bool isShare: devices.length > 0 && devIdx < devices.length && devices[devIdx].path === "screencast"
    readonly property int cardCount: devices.length - (player.screenCastAvailable() ? 1 : 0)
    function refresh() {
        var dv = player.captureDevices()
        if (player.screenCastAvailable()) dv.push({ path: "screencast", name: tr("screenShare") })
        devices = dv
        help = player.captureHelp()
        var s = [{ name: "", desc: tr("noAudio") }]
        var list = player.audioSources()
        list.push({ name: "@default", desc: tr("defaultInput") })
        for (var i = 0; i < list.length; ++i) s.push(list[i])
        sources = s
        var last = player.lastCaptureDevice()
        devIdx = 0
        for (var d = 0; d < devices.length; ++d) if (devices[d].path === last) devIdx = d
        pickDevice(devIdx)
    }
    function pickDevice(i) {
        devIdx = i
        var share = devices.length > 0 && devices[i].path === "screencast"
        modes = devices.length > 0 && !share ? player.captureModes(devices[i].path) : []
        modeIdx = 0
        if (share) {
            // the game plays its own sound: no audio unless one was chosen for a window/screen before
            srcIdx = 0
            if (player.lastCaptureDevice() === "screencast")
                for (var a = 0; a < sources.length; ++a) if (sources[a].name !== "" && sources[a].name === player.lastCaptureAudio()) srcIdx = a
            return
        }
        // audio: remembered source, else best name match for this card, else none
        var want = player.lastCaptureAudio()
        var guess = devices.length > 0 ? player.guessAudioSource(devices[i].name) : ""
        srcIdx = 0
        for (var k = 0; k < sources.length; ++k) if (sources[k].name !== "" && (sources[k].name === guess)) srcIdx = k
        if (srcIdx === 0) for (var j = 0; j < sources.length; ++j) if (sources[j].name !== "" && sources[j].name === want) srcIdx = j
    }

    Rectangle { anchors.fill: parent; color: "#cc000000"; MouseArea { anchors.fill: parent; onClicked: { dlg.visible = false; dlg.closed() } } }

    component Row_: Rectangle {
        id: row
        property string text
        property bool sel: false
        signal clicked()
        width: parent ? parent.width : 100; height: 34; radius: 8 * Theme.round
        color: sel ? Theme.accentSoft : (ma.containsMouse ? Theme.border : Theme.surface)
        border.color: sel ? Theme.accent : "transparent"
        Text { font.family: Theme.font; anchors { verticalCenter: parent.verticalCenter; left: parent.left; right: parent.right; leftMargin: 12; rightMargin: 12 }
            text: row.text; color: row.sel ? Theme.heading : Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: row.clicked() }
    }
    component Section: Text { font.family: Theme.font; color: Theme.label; font.pixelSize: 12; font.weight: Font.Medium }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(620, parent.width - 40); height: Math.min(640, parent.height - 40)
        radius: 16 * Theme.round; color: Theme.surfaceDeep; border.color: Theme.border
        MouseArea { anchors.fill: parent }   // swallow clicks

        Column {
            anchors { fill: parent; margins: 22 }
            spacing: 10
            Text { font.family: Theme.headFont; width: Math.min(implicitWidth, parent.width - 62); elide: Text.ElideRight; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; text: tr("captureTitle"); color: Theme.heading; font.pixelSize: 20 * Theme.headScale; font.weight: Font.DemiBold }

            Section { text: tr("device") }
            Column {
                width: parent.width; spacing: 4
                Repeater { model: dlg.devices
                    Row_ { text: modelData.path === "screencast" ? modelData.name : modelData.name + "  (" + modelData.path + ")"
                        sel: index === dlg.devIdx; onClicked: dlg.pickDevice(index) } }
                Text { font.family: Theme.font; visible: dlg.cardCount <= 0; text: tr("noDevices") + (dlg.help !== "" ? "\n" + dlg.help : ""); color: Theme.warn; font.pixelSize: 13; width: parent.width; wrapMode: Text.Wrap; lineHeight: 1.15 }
            }

            Text { font.family: Theme.font; visible: dlg.isShare; text: tr("screenShareHint"); color: Theme.muted; font.pixelSize: 12; width: parent.width; wrapMode: Text.Wrap; lineHeight: 1.15 }
            Section { text: tr("mode"); visible: dlg.modes.length > 0 }
            Flickable {
                visible: dlg.modes.length > 0
                width: parent.width; height: Math.min(170, contentHeight); contentHeight: modeCol.height; clip: true
                Column { id: modeCol; width: parent.width; spacing: 4
                    Repeater { model: dlg.modes
                        Row_ { text: modelData + (index === 0 ? "   ★" : ""); sel: index === dlg.modeIdx; onClicked: dlg.modeIdx = index } } }
            }

            // a compressed mode where the card has an uncompressed one (after PushFPS 4.1): said, never switched for you
            Rectangle {
                readonly property string advice: dlg.modes.length > 0 ? player.modeAdvice(dlg.modeIdx) : ""
                readonly property int better: dlg.modes.length > 0 ? player.betterModeIndex(dlg.modeIdx) : -1
                visible: advice !== ""
                width: parent.width; height: visible ? adviceCol.height + 16 : 0; radius: 8 * Theme.round
                color: Theme.surface; border.color: Theme.border
                Column {
                    id: adviceCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                    spacing: 6
                    Text { font.family: Theme.font; width: parent.width; wrapMode: Text.Wrap; text: parent.parent.advice; color: Theme.text; font.pixelSize: 12; lineHeight: 1.15 }
                    Text { font.family: Theme.font; visible: parent.parent.better >= 0; text: tr("useUncompressed"); color: Theme.accent; font.pixelSize: 12; font.weight: Font.DemiBold
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: dlg.modeIdx = parent.parent.parent.better } }
                }
            }

            Section { text: tr("audio") }
            Flickable {
                width: parent.width; height: Math.min(130, contentHeight); contentHeight: srcCol.height; clip: true
                Column { id: srcCol; width: parent.width; spacing: 4
                    Repeater { model: dlg.sources
                        Row_ { text: modelData.desc; sel: index === dlg.srcIdx; onClicked: dlg.srcIdx = index } } }
            }
        }

        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom; margins: 22 }
            width: psT.implicitWidth + 28; height: 38; radius: 10 * Theme.round; color: m0.containsMouse ? Theme.surfaceHover : Theme.surface
            Text { id: psT; font.family: Theme.font; anchors.centerIn: parent; text: tr("remotePlayTitle"); color: Theme.text; font.pixelSize: 13 }
            MouseArea { id: m0; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { dlg.visible = false; dlg.closed(); dlg.remotePlay() } } }
        Row {
            anchors { right: parent.right; bottom: parent.bottom; margins: 22 }
            spacing: 8
            Rectangle { width: 96; height: 38; radius: 10 * Theme.round; color: m1.containsMouse ? Theme.surfaceHover : Theme.surface
                Text { font.family: Theme.font; anchors.centerIn: parent; text: tr("refresh"); color: Theme.text; font.pixelSize: 13 }
                MouseArea { id: m1; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: dlg.refresh() } }
            Rectangle { width: 96; height: 38; radius: 10 * Theme.round; color: m2.containsMouse ? Theme.surfaceHover : Theme.surface
                Text { font.family: Theme.font; anchors.centerIn: parent; text: tr("cancel"); color: Theme.text; font.pixelSize: 13 }
                MouseArea { id: m2; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { dlg.visible = false; dlg.closed() } } }
            Rectangle { width: 120; height: 38; radius: 10 * Theme.round; opacity: dlg.devices.length > 0 && (dlg.modes.length > 0 || dlg.isShare) ? 1 : 0.4
                color: m3.containsMouse ? Theme.accentHover : Theme.accent
                Text { font.family: Theme.font; anchors.centerIn: parent; text: tr("start"); color: Theme.accentInk; font.pixelSize: 14; font.weight: Font.DemiBold }
                MouseArea { id: m3; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    enabled: dlg.devices.length > 0 && (dlg.modes.length > 0 || dlg.isShare)
                    onClicked: {
                        if (dlg.isShare) { player.openScreenCast(dlg.sources[dlg.srcIdx].name, false); dlg.visible = false; dlg.closed(); return }
                        if (player.openCapture(dlg.devices[dlg.devIdx].path, dlg.modeIdx, dlg.sources[dlg.srcIdx].name)) { dlg.visible = false; dlg.closed() }
                    } } }
        }
    }
}
