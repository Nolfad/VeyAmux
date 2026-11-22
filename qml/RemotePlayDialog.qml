import QtQuick

// PlayStation Remote Play (PS5 / PS4 on the same network): link the PlayStation Network account once, register each
// console once with the PIN it shows, then play. The console's picture runs through all of VeyAmux's processing.
Item {
    id: dlg
    property var tr: function (k) { return k }
    property string selId: ""          // the console row that is open
    signal closed()
    visible: false

    function openDialog() { player.psStartSearch(); loginOpen = !player.psHasAccount; visible = true }
    function close() { player.psStopSearch(); visible = false; closed() }
    property bool loginOpen: false
    // the consoles, updated in place (a new JS array would rebuild the rows and lose what is being typed)
    ListModel { id: consoleModel }
    function syncConsoles() {
        var list = player.psConsoles, keep = {}
        for (var i = 0; i < list.length; ++i) {
            var c = list[i], at = -1
            keep[c.id] = true
            for (var j = 0; j < consoleModel.count; ++j) if (consoleModel.get(j).id === c.id) { at = j; break }
            if (at < 0) consoleModel.append(c)
            else for (var k in c) if (consoleModel.get(at)[k] !== c[k]) consoleModel.setProperty(at, k, c[k])
        }
        for (var r = consoleModel.count - 1; r >= 0; --r) if (!keep[consoleModel.get(r).id]) consoleModel.remove(r)
    }
    Connections { target: player; function onPsChanged() { dlg.syncConsoles() } }
    onVisibleChanged: if (visible) syncConsoles()
    readonly property bool busy: player.psBusy !== ""

    Rectangle { anchors.fill: parent; color: "#cc000000"; MouseArea { anchors.fill: parent; onClicked: dlg.close() } }

    component Section: Text { font.family: Theme.font; color: Theme.label; font.pixelSize: 13; font.weight: Font.Medium }
    component Hint: Text { font.family: Theme.font; color: Theme.muted; font.pixelSize: 13; width: parent ? parent.width : 100; wrapMode: Text.Wrap; lineHeight: 1.15 }
    component Btn: Rectangle {
        id: b
        property string text
        property bool primary: false
        property bool danger: false
        signal clicked()
        width: Math.max(96, bt.implicitWidth + 28); height: 36; radius: 10 * Theme.round
        opacity: enabled ? 1 : 0.4
        color: primary ? (bm.containsMouse ? Theme.accentHover : Theme.accent) : (bm.containsMouse ? Theme.surfaceHover : Theme.surface)
        border.color: danger ? Theme.warn : "transparent"
        Text { id: bt; font.family: Theme.font; anchors.centerIn: parent; text: b.text; color: b.primary ? Theme.accentInk : (b.danger ? Theme.warn : Theme.text)
            font.pixelSize: 14; font.weight: b.primary ? Font.DemiBold : Font.Normal }
        MouseArea { id: bm; anchors.fill: parent; hoverEnabled: true; enabled: b.enabled; cursorShape: Qt.PointingHandCursor; onClicked: b.clicked() }
    }
    component Field: Rectangle {
        id: f
        property alias text: inp.text
        property string placeholder
        property bool secret: false
        property int maxLength: 2048
        signal accepted()
        height: 34; radius: 9 * Theme.round; color: Theme.surface; border.color: inp.activeFocus ? Theme.accentLine : Theme.border
        TextInput {
            id: inp; anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
            verticalAlignment: TextInput.AlignVCenter; color: Theme.text; font.pixelSize: 15; clip: true; selectByMouse: true
            maximumLength: f.maxLength; echoMode: f.secret ? TextInput.Password : TextInput.Normal
            onAccepted: f.accepted()
            Text { font.family: Theme.font; anchors.verticalCenter: parent.verticalCenter; visible: !parent.text && !parent.activeFocus
                text: f.placeholder; color: Theme.dim; font.pixelSize: 15; width: parent.width; elide: Text.ElideRight }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.IBeamCursor; onClicked: inp.forceActiveFocus() }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(680, parent.width - 40); height: Math.min(720, parent.height - 40)
        radius: 16 * Theme.round; color: Theme.surfaceDeep; border.color: Theme.border
        MouseArea { anchors.fill: parent }   // swallow clicks

        Flickable {
            anchors { left: parent.left; right: parent.right; top: parent.top; bottom: bottomRow.top; margins: 22; bottomMargin: 12 }
            contentHeight: col.height; clip: true
            Column {
                id: col
                width: parent.width
                spacing: 10
                Text { font.family: Theme.headFont; width: parent.width; elide: Text.ElideRight; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase
                    text: tr("remotePlayTitle"); color: Theme.heading; font.pixelSize: 20 * Theme.headScale; font.weight: Font.DemiBold }
                Hint { text: tr("psIntro") }

                // ---- account
                Section { text: tr("psAccount") }
                Row {
                    visible: player.psHasAccount && !dlg.loginOpen
                    spacing: 10
                    Text { font.family: Theme.font; anchors.verticalCenter: parent.verticalCenter; text: "✓ " + tr("psAccountLinked"); color: Theme.ok; font.pixelSize: 14 }
                    Btn { text: tr("psChange"); onClicked: dlg.loginOpen = true }
                }
                Column {
                    visible: !player.psHasAccount || dlg.loginOpen
                    width: parent.width; spacing: 8
                    Hint { text: tr("psLoginHint") }
                    Btn { text: tr("psLogin"); primary: true; onClicked: player.psOpenLogin() }
                    Field { id: loginIn; width: parent.width; placeholder: tr("psPasteAddress")
                        onAccepted: { player.psSetLogin(text); text = ""; dlg.loginOpen = false } }
                    Row {
                        spacing: 8
                        Btn { text: tr("psSave"); enabled: loginIn.text.length > 0 && !dlg.busy
                            onClicked: { player.psSetLogin(loginIn.text); loginIn.text = ""; dlg.loginOpen = false } }
                        Btn { visible: player.psHasAccount; text: tr("cancel"); onClicked: dlg.loginOpen = false }
                    }
                }

                // ---- consoles
                Section { text: tr("psConsoles") }
                Hint { visible: consoleModel.count === 0; text: tr("psSearching") }
                Repeater {
                    model: consoleModel
                    delegate: Rectangle {
                        id: row
                        required property var model
                        readonly property var modelData: model
                        readonly property bool open: dlg.selId === modelData.id
                        width: col.width; height: rowCol.height + 16; radius: 10 * Theme.round
                        color: open ? Theme.accentSoft : Theme.surface; border.color: open ? Theme.accent : "transparent"
                        Column {
                            id: rowCol
                            x: 12; y: 8; width: parent.width - 24; spacing: 8
                            Item {
                                width: parent.width; height: 30
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - (row.modelData.manual === true ? 230 : 120)
                                    Text { font.family: Theme.font; width: parent.width; elide: Text.ElideRight; color: Theme.heading; font.pixelSize: 15; font.weight: Font.Medium
                                        text: row.modelData.name + "  ·  " + (row.modelData.ps5 ? "PS5" : "PS4") }
                                    Text { font.family: Theme.font; width: parent.width; elide: Text.ElideRight; font.pixelSize: 12
                                        color: row.modelData.ready ? Theme.ok : Theme.muted
                                        text: (row.modelData.manual ? tr("psManualWaiting") : !row.modelData.found ? tr("psNotFound") : row.modelData.ready ? tr("psReady") : tr("psRest"))
                                              + (row.modelData.app ? "  ·  " + row.modelData.app : "") + "  ·  " + row.modelData.host }
                                }
                                Btn {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    visible: row.modelData.registered
                                    text: tr("psPlay"); primary: true; enabled: !dlg.busy
                                    onClicked: { player.psConnect(row.modelData.id) }
                                }
                                Btn {
                                    id: regBtn
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    visible: !row.modelData.registered
                                    text: tr("psRegister"); enabled: player.psHasAccount && !dlg.busy
                                    onClicked: dlg.selId = row.open ? "" : row.modelData.id
                                }
                                Btn {   // an address typed in: take it away again
                                    anchors { right: regBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                                    visible: row.modelData.manual === true
                                    text: tr("psRemoveHost"); danger: true
                                    onClicked: { dlg.selId = ""; player.psRemoveHost(row.modelData.host) }
                                }
                            }
                            // registration: the PIN from the console
                            Column {
                                visible: row.open && !row.modelData.registered
                                width: parent.width; spacing: 8
                                Hint { text: (row.modelData.ps5 ? tr("psPinHintPs5") : tr("psPinHintPs4")) + (row.modelData.manual ? " " + tr("psManualRegister") : "") }
                                Row {
                                    spacing: 8
                                    Field { id: pinIn; width: 160; placeholder: "12345678"; maxLength: 9
                                        onAccepted: player.psRegister(row.modelData.id, text) }
                                    Btn { text: tr("psRegister"); primary: true; enabled: pinIn.text.length >= 8 && !dlg.busy
                                        onClicked: player.psRegister(row.modelData.id, pinIn.text) }
                                }
                            }
                            // a registered console: login passcode, forget
                            Row {
                                visible: row.modelData.registered
                                spacing: 8
                                Field { width: 200; placeholder: tr("psPasscode"); secret: true; maxLength: 4
                                    text: row.modelData.passcode; onAccepted: player.psSetPasscode(row.modelData.id, text)
                                    onTextChanged: if (text.length === 4 || text.length === 0) player.psSetPasscode(row.modelData.id, text) }
                                Btn { text: tr("psForget"); danger: true; onClicked: player.psForget(row.modelData.id) }
                            }
                        }
                    }
                }

                // ---- a console by its IP address (the search does not reach it, or a firewall drops its answer)
                Section { text: tr("psManualTitle") }
                Hint { text: tr("psManualHint") }
                Row {
                    id: manualRow
                    spacing: 8
                    property bool ps5: true
                    Field { id: ipIn; width: Math.min(220, col.width - 300); placeholder: "192.168.1.50"; maxLength: 253
                        onAccepted: if (player.psAddHost(text, manualRow.ps5)) text = "" }
                    Segmented { width: 150; model: ["PS5", "PS4"]; current: manualRow.ps5 ? 0 : 1; onPicked: (i) => manualRow.ps5 = i === 0 }
                    Btn { text: tr("psAddHost"); enabled: ipIn.text.trim() !== ""
                        onClicked: if (player.psAddHost(ipIn.text, manualRow.ps5)) ipIn.text = "" }
                }

                // ---- picture
                Section { text: tr("psPicture") }
                Segmented { width: col.width; model: ["720p", "1080p"]; current: player.psHeight === 720 ? 0 : 1
                    onPicked: (i) => player.psHeight = i === 0 ? 720 : 1080 }
                Segmented { width: col.width; model: ["30 fps", "60 fps"]; current: player.psFps === 30 ? 0 : 1
                    onPicked: (i) => player.psFps = i === 0 ? 30 : 60 }
                Item {
                    width: col.width; height: 36
                    Text { font.family: Theme.font; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 70; wrapMode: Text.Wrap
                        text: tr("psHdr"); color: Theme.text; font.pixelSize: 14 }
                    ToggleSwitch { anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        checked: player.psHdr; onToggled: (v) => player.psHdr = v }
                }
                Hint { text: tr("psControllerHint") }
            }
        }

        // status + close
        Item {
            id: bottomRow
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 22 }
            height: Math.max(38, statusText.implicitHeight)
            Text {
                id: statusText
                anchors { left: parent.left; right: closeBtn.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                font.family: Theme.font; font.pixelSize: 13; wrapMode: Text.Wrap
                color: dlg.busy ? Theme.accent : player.psMessageError ? Theme.warn : Theme.ok
                text: dlg.busy ? player.psBusy : player.psMessage
            }
            Btn { id: closeBtn; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: tr("close"); onClicked: dlg.close() }
        }
    }

    // a stream that started closes the window
    Connections { target: player; function onMediaChanged() { if (dlg.visible && player.psActive) dlg.close() } }
}
