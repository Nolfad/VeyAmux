import QtQuick
import VeyAmux 1.0
import "Strings.js" as S

// The menu (P / the ✨ button / Select + Start on a controller): a panel on the right with six tabs, so each part has
// room and nothing has to be shrunk to fit. On small screens (Steam Deck) it fills the window. The content is laid
// out at a fixed design width and scaled with the window height, so it reads the same from the couch on a TV.
Rectangle {
    id: menu
    property var tr: function (k) { return k }
    property bool open: false
    property bool compact: false
    signal closeRequested()

    // the controller (Main.qml padNav): what it steers, and how LB / RB move between the tabs
    readonly property Item scopeItem: tabContent
    readonly property Flickable flick: flickArea
    readonly property real fit: contentScale
    function switchTab(step) { tab = (tab + step + tabs.length) % tabs.length; flickArea.contentY = 0 }

    property int tab: 0
    // --tab=N opens the menu on tab N (screenshots, tests)
    Component.onCompleted: { for (const a of Qt.application.arguments) if (a.startsWith("--tab=")) tab = parseInt(a.substring(6)) }
    readonly property var tabs: [
        { key: "tabPicture", icon: "icons/picture.svg" },
        { key: "tabFrameGen", icon: "icons/sparkles.svg" },
        { key: "tabDisplay", icon: "icons/display.svg" },
        { key: "tabSound", icon: "icons/volume.svg" },
        { key: "tabRecord", icon: "icons/record.svg" },
        { key: "tabSystem", icon: "icons/system.svg" }
    ]
    readonly property var scales: [1.0, 4 / 3, 1.5, 2.0, 3.0]

    readonly property real designW: 600
    readonly property real contentScale: compact ? Math.max(0.9, Math.min(1.35, (width - 40) / designW))
                                                 : Math.max(1.0, Math.min(1.8, (parent ? parent.height : 1080) / 1000))
    width: compact ? (parent ? parent.width : 800) : Math.min((parent ? parent.width : 1200) - 24, designW * contentScale + 48)
    height: compact ? (parent ? parent.height : 600) : (parent ? parent.height - 24 : 600)
    x: compact ? 0 : (parent ? parent.width - width - 12 : 0)
    y: compact ? 0 : 12
    radius: compact ? 0 : 18 * Theme.round
    opacity: open ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 160 } }
    // HDR output: fully opaque (bright HDR video behind a see-through panel greys it out)
    color: player.hdrOutputActive || compact || !player.hasMedia ? Qt.rgba(Theme.panel.r, Theme.panel.g, Theme.panel.b, 1) : Theme.panel
    border.color: Theme.border
    Rectangle { x: 18; width: parent.width - 36; height: 2; radius: 1 * Theme.round; gradient: Theme.sunset; opacity: 0.85 }
    MouseArea { objectName: "padSkip"; anchors.fill: parent; hoverEnabled: true; onPositionChanged: menu.awake() }
    signal awake()

    // ---------------------------------------------------------------- building blocks
    component Title: Text { font.family: Theme.headFont; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase
        color: Theme.heading; font.pixelSize: 22 * Theme.headScale; font.weight: Font.Bold; font.letterSpacing: 0.5; width: parent ? parent.width : 100; elide: Text.ElideRight }
    component Label_: Text { font.family: Theme.font; color: Theme.label; font.pixelSize: 16; width: parent ? parent.width : 100; wrapMode: Text.Wrap }
    component Hint: Text { font.family: Theme.font; color: Theme.muted; font.pixelSize: 14; width: parent ? parent.width : 100; wrapMode: Text.Wrap; lineHeight: 1.15 }
    component Sep: Rectangle { width: parent ? parent.width : 100; height: 1; color: Theme.line }
    component SwitchRow: Item {
        id: sr
        property string text
        property bool checked: false
        property bool active: true
        signal toggled(bool v)
        width: parent ? parent.width : 100; height: Math.max(30, srText.implicitHeight)
        opacity: active ? 1 : 0.45
        Text { id: srText; font.family: Theme.font; text: sr.text; color: Theme.label; font.pixelSize: 16; width: parent.width - 62; wrapMode: Text.Wrap; anchors.verticalCenter: parent.verticalCenter }
        ToggleSwitch { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; checked: sr.checked; onToggled: (v) => { if (sr.active) sr.toggled(v) } }
    }
    component Btn: Rectangle {
        id: b
        property string text
        property bool primary: false
        property bool danger: false
        signal clicked()
        width: bText.implicitWidth + 26; height: 34; radius: 10 * Theme.round
        color: danger ? (bMa.containsMouse ? "#ff6b6b" : "#e04848") : (bMa.containsMouse ? Theme.surfaceHover : Theme.surface)
        gradient: primary && !danger ? (bMa.containsMouse ? Theme.sunsetHover : Theme.sunset) : null
        border.width: Theme.edge; border.color: "#000000"
        Text { id: bText; font.family: Theme.font; anchors.centerIn: parent; text: b.text; color: b.primary || b.danger ? Theme.accentInk : Theme.text; font.pixelSize: 15; font.weight: Font.DemiBold }
        MouseArea { id: bMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: b.clicked() }
    }
    component Param: Column {
        id: p
        property string label
        property real from; property real to; property real value; property real neutral: NaN
        property int decimals: 2
        signal moved(real v)
        width: parent ? parent.width : 100; spacing: 4
        Row {
            width: parent.width
            Text { font.family: Theme.font; text: p.label; color: Theme.label; font.pixelSize: 16; width: parent.width - 60 }
            Text { font.family: Theme.font; text: player.language === "pt-BR" ? p.value.toLocaleString(Qt.locale("pt_BR"), 'f', p.decimals) : p.value.toFixed(p.decimals); color: Theme.text; font.pixelSize: 16; width: 60; horizontalAlignment: Text.AlignRight }
        }
        VSlider { width: parent.width; from: p.from; to: p.to; value: p.value; neutral: p.neutral; onMoved: (v) => p.moved(v) }
    }

    // ---------------------------------------------------------------- header: logo, name, version, close
    Item {
        id: header
        x: 20; y: 14; width: parent.width - 40; height: 44
        Row {
            spacing: 10; anchors.verticalCenter: parent.verticalCenter
            Image { source: Theme.logo; sourceSize: Qt.size(30, 30); width: 30; height: 30; anchors.verticalCenter: parent.verticalCenter }
            Text { font.family: Theme.headFont; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; text: "VeyAmux"; color: Theme.heading
                font.pixelSize: 22 * Theme.headScale; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
            Text { font.family: Theme.font; text: player.appVersion; color: Theme.dim; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
            Rectangle {   // a new version: one click to the System tab
                visible: player.updateState === "available" || player.updateState === "ready"
                anchors.verticalCenter: parent.verticalCenter
                width: updPill.implicitWidth + 20; height: 24; radius: 12 * Theme.round; gradient: Theme.sunset
                Text { id: updPill; font.family: Theme.font; anchors.centerIn: parent; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.accentInk
                    text: player.updateState === "ready" ? tr("updRestart") : tr("updBanner").replace("%1", player.updateVersion) }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: menu.tab = 5 }
            }
        }
        IconButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; size: 40; source: "icons/close.svg"; onClicked: menu.closeRequested() }
    }

    // ---------------------------------------------------------------- tabs
    Row {
        id: tabBar
        x: 16; anchors.top: header.bottom; anchors.topMargin: 10
        width: parent.width - 32; spacing: 4
        Repeater {
            model: menu.tabs
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool sel: menu.tab === index
                width: (tabBar.width - 5 * tabBar.spacing) / 6; height: 62; radius: 12 * Theme.round
                color: sel ? Theme.accentSoft : (tMa.containsMouse ? Theme.surfaceHover : "transparent")
                border.color: sel ? Theme.accentLine : "transparent"; border.width: 1
                Column {
                    anchors.centerIn: parent; spacing: 4
                    Image { source: modelData.icon; width: 22; height: 22; sourceSize: Qt.size(44, 44); anchors.horizontalCenter: parent.horizontalCenter; opacity: sel ? 1 : 0.75 }
                    Text { font.family: Theme.font; text: tr(modelData.key); color: sel ? Theme.heading : Theme.label; font.pixelSize: 12
                        font.weight: sel ? Font.DemiBold : Font.Normal; anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.min(implicitWidth, tabBar.width / 6 - 4); elide: Text.ElideRight }
                }
                Rectangle { visible: sel; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; width: parent.width * 0.5; height: 3; radius: 1.5; gradient: Theme.sunset }
                MouseArea { id: tMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { menu.tab = index; flickArea.contentY = 0 } }
            }
        }
    }
    Rectangle { anchors.top: tabBar.bottom; anchors.topMargin: 8; x: 16; width: parent.width - 32; height: 1; color: Theme.line }

    // ---------------------------------------------------------------- content (one tab at a time, scrolls)
    Flickable {
        id: flickArea
        anchors { top: tabBar.bottom; topMargin: 16; left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 12 }
        clip: true
        contentWidth: width
        contentHeight: tabContent.height * menu.contentScale + 24
        boundsBehavior: Flickable.StopAtBounds
        Item {
            id: tabContent
            width: menu.designW
            height: pages.children[menu.tab] ? pages.children[menu.tab].height : 0
            transformOrigin: Item.TopLeft
            scale: menu.contentScale
            x: (flickArea.width - width * scale) / 2
            Item {
                id: pages
                width: parent.width

                // ======================================== Picture
                Column {
                    visible: menu.tab === 0
                    width: parent.width; spacing: 16
                    SwitchRow { text: tr("enhance"); checked: player.enhance; onToggled: (v) => player.enhance = v }
                    Column {
                        width: parent.width; spacing: 10; opacity: player.enhance ? 1 : 0.45
                        Label_ { text: tr("upscale") }
                        Segmented {
                            width: parent.width; enabled: player.enhance
                            model: [tr("native"), "1⅓×", tr("scaleOneAndHalf"), "2×", "3×"]
                            current: { var i = menu.scales.findIndex(function (scale) { return Math.abs(scale - player.scale) < 0.001 }); return i < 0 ? 0 : i }
                            onPicked: (i) => player.scale = menu.scales[i]
                        }
                        Label_ { text: tr("method") }
                        Segmented {
                            width: parent.width; enabled: player.enhance
                            readonly property var values: [0, 2, 1, 3, 4]
                            model: ["SGSR", "FSR 1", "FSR 3.1", "NIS", "DLSS SR"]
                            current: Math.max(0, values.indexOf(player.upscaler))
                            onPicked: (i) => player.upscaler = values[i]
                        }
                        Hint {
                            color: player.upscalerError !== "" ? Theme.warn : Theme.muted
                            text: player.upscalerError !== "" ? (player.upscaler === 4 ? tr("dlssFallback") : tr("fsrFallback")) + player.upscalerError
                                  : (player.upscaler === 4 ? tr("dlssHint")
                                     : player.upscaler === 1 ? (player.scale <= 1.001 ? tr("fsrNativeAaHint") : tr("fsr31Hint"))
                                     : player.upscaler === 2 ? tr("fsr1Hint") + (player.scale <= 1.001 ? " " + tr("fsrNeedsScale") : "")
                                     : player.upscaler === 3 ? tr("nisHint") + (player.scale <= 1.001 ? " " + tr("fsrNeedsScale") : "")
                                                             : tr("sgsrHint"))
                        }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 10; opacity: player.enhance ? 1 : 0.45
                        Label_ { text: tr("denoise") }
                        Segmented { width: parent.width; model: tr("denoiseModes"); enabled: player.enhance; current: player.denoise; onPicked: (i) => player.denoise = i }
                        Segmented { width: parent.width; model: tr("denoiseLevels"); enabled: player.enhance && player.denoise > 0
                            current: player.denoiseStrength; onPicked: (i) => player.denoiseStrength = i }
                        Hint { text: tr("denoiseHint" + player.denoise) }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 10; opacity: player.enhance ? 1 : 0.45
                        SwitchRow { text: tr("aa"); checked: player.fxaa; active: player.enhance; onToggled: (v) => player.fxaa = v }
                        Segmented { width: parent.width; model: ["FXAA", "SMAA"]; enabled: player.enhance && player.fxaa
                            current: player.smaa ? 1 : 0; onPicked: (i) => player.smaa = i === 1 }
                        Segmented { width: parent.width; model: tr("aaq"); enabled: player.enhance && player.fxaa
                            current: player.fxaaQuality; onPicked: (i) => player.fxaaQuality = i }
                        Hint { text: player.smaa ? tr("smaaHint") : tr("fxaaHint") }
                        SwitchRow { text: tr("deflicker"); checked: player.deflicker; active: player.enhance; onToggled: (v) => player.deflicker = v }
                        Hint { text: tr("deflickerHint") }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 14; opacity: player.enhance ? 1 : 0.45
                        Param { label: tr("sharpen"); from: 0; to: 1; value: player.sharpen; onMoved: (v) => player.sharpen = v }
                        Param { label: tr("exposure"); from: -2; to: 2; value: player.exposure; neutral: 0; onMoved: (v) => player.exposure = v }
                        Param { label: tr("contrast"); from: 0.6; to: 1.4; value: player.contrast; neutral: 1; onMoved: (v) => player.contrast = v }
                        Param { label: tr("saturation"); from: 0; to: 2; value: player.saturation; neutral: 1; onMoved: (v) => player.saturation = v }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 14; opacity: player.enhance ? 1 : 0.45
                        Title { text: tr("detailTitle") }
                        Hint { text: tr("detailHint") }
                        Param { label: tr("texture"); from: -1; to: 1; value: player.texture; neutral: 0; onMoved: (v) => player.texture = Math.abs(v) < 0.03 ? 0 : v }
                        Param { label: tr("clarity"); from: -1; to: 1; value: player.clarity; neutral: 0; onMoved: (v) => player.clarity = Math.abs(v) < 0.03 ? 0 : v }
                        Param { label: tr("dehaze"); from: -1; to: 1; value: player.dehaze; neutral: 0; onMoved: (v) => player.dehaze = Math.abs(v) < 0.03 ? 0 : v }
                    }
                    Btn { width: parent.width; text: tr("reset"); onClicked: player.resetEnhance() }
                    Sep {}
                    // ---- profiles
                    Column {
                        width: parent.width; spacing: 8
                        Title { text: tr("profiles") }
                        Flow {
                            width: parent.width; spacing: 6
                            visible: player.profiles.length > 0
                            Repeater {
                                model: player.profiles
                                delegate: Rectangle {
                                    required property string modelData
                                    required property int index
                                    readonly property bool active: player.activeProfile === modelData
                                    readonly property string bound: player.profileSignal(modelData)
                                    height: 30; radius: 8 * Theme.round; width: chipRow.implicitWidth + 16
                                    color: active ? Theme.accent : (chipMa.containsMouse ? Theme.surfaceHover : Theme.surface)
                                    Row {
                                        id: chipRow; anchors.centerIn: parent; spacing: 6
                                        Text { font.family: Theme.font; text: (index < 9 ? (index + 1) + "  " : "") + modelData + (bound !== "" ? "  ⚡" : ""); color: active ? Theme.accentInk : Theme.text; font.pixelSize: 15; anchors.verticalCenter: parent.verticalCenter }
                                        Text { font.family: Theme.font; text: "×"; color: active ? Theme.accentInk : Theme.muted; font.pixelSize: 18; anchors.verticalCenter: parent.verticalCenter
                                            MouseArea { anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: player.deleteProfile(modelData) } }
                                    }
                                    MouseArea { id: chipMa; anchors.fill: parent; anchors.rightMargin: 18; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.loadProfile(modelData) }
                                }
                            }
                        }
                        Row {
                            width: parent.width; spacing: 6
                            Rectangle {
                                width: parent.width - saveBtn.width - 6; height: 34; radius: 8 * Theme.round; color: Theme.surface
                                border.color: nameIn.activeFocus ? Theme.accent : Theme.surfaceHover
                                TextInput {
                                    id: nameIn; anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                                    verticalAlignment: TextInput.AlignVCenter; color: Theme.text; font.pixelSize: 15; maximumLength: 24; clip: true; selectByMouse: true
                                    Text { font.family: Theme.font; anchors.verticalCenter: parent.verticalCenter; visible: !parent.text && !parent.activeFocus; text: tr("profileName"); color: Theme.dim; font.pixelSize: 15 }
                                    onAccepted: saveBtn.save()
                                }
                            }
                            Btn {
                                id: saveBtn
                                function save() { if (nameIn.text.trim() !== "") { player.saveProfile(nameIn.text, bindSig.checked); nameIn.text = "" } }
                                text: tr("saveProfile"); primary: nameIn.text.trim() !== ""; onClicked: save()
                            }
                        }
                        Row {
                            id: bindSig
                            property bool checked: false
                            visible: player.signalKey !== ""
                            spacing: 8
                            Rectangle {
                                width: 18; height: 18; radius: 4 * Theme.round; anchors.verticalCenter: parent.verticalCenter
                                color: bindSig.checked ? Theme.accent : "transparent"; border.color: bindSig.checked ? Theme.accent : Theme.dim
                                Text { font.family: Theme.font; anchors.centerIn: parent; text: "✓"; visible: bindSig.checked; color: Theme.accentInk; font.pixelSize: 14 }
                            }
                            Text { font.family: Theme.font; text: tr("bindSignal").replace("%1", player.signalKey); color: Theme.text; font.pixelSize: 15; width: pages.width - 30; wrapMode: Text.Wrap }
                            TapHandler { onTapped: bindSig.checked = !bindSig.checked }
                        }
                        Hint { text: tr("profilesHint") }
                    }
                }

                // ======================================== Frame generation
                Column {
                    visible: menu.tab === 1
                    width: parent.width; spacing: 16
                    FrameGenSection { width: parent.width; tr: menu.tr }
                }

                // ======================================== Display
                Column {
                    visible: menu.tab === 2
                    width: parent.width; spacing: 16
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: "HDR" }
                        Label_ { text: tr("hdr") }
                        Segmented {
                            width: parent.width; model: ["600", "1000", "2000", "4000 nits"]
                            readonly property var peaks: [600, 1000, 2000, 4000]
                            current: Math.max(0, peaks.indexOf(player.hdrPeak)); onPicked: (i) => player.hdrPeak = peaks[i]
                        }
                        Hint { text: tr("hdrHint") }
                        SwitchRow { text: tr("hdrOut"); checked: player.hdrOutput; onToggled: (v) => player.hdrOutput = v }
                        Segmented { visible: player.hdrOutput; width: parent.width; model: ["HDR10", "scRGB"]; current: player.hdrMode; onPicked: (i) => player.hdrMode = i }
                        Segmented {
                            visible: player.hdrOutput; width: parent.width
                            model: [tr("autoBrightness"), "400", "600", "1000", "1500 nits"]
                            readonly property var peaks: [0, 400, 600, 1000, 1500]
                            current: Math.max(0, peaks.indexOf(player.displayPeak)); onPicked: (i) => player.displayPeak = peaks[i]
                        }
                        Hint {
                            visible: player.hdrOutput
                            color: player.hdrOutputActive ? Theme.ok : Theme.warn
                            text: player.hdrOutputRestart ? tr("hdrOutRestart") : player.hdrOutputActive ? tr("hdrOutOn") + (player.displayPeak === 0 ? " " + (player.displayPeakEdid > 0 ? tr("peakEdid").replace("%1", player.displayPeakEdid) : tr("peakNoEdid")) : "")
                                  : player.hdrOutputNoVulkan ? tr("hdrOutSw") : player.hdrOutputXwayland ? tr("hdrOutX11") : tr("hdrOutNo")
                        }
                        SwitchRow { visible: player.gamescopeAvailable || player.inGamescope; text: tr("gamescope"); checked: player.gamescope; onToggled: (v) => player.gamescope = v }
                        Hint { visible: player.gamescopeAvailable || player.inGamescope; color: player.inGamescope ? Theme.accent3 : Theme.muted
                            text: player.inGamescope ? tr("gamescopeOn") : tr("gamescopeHint") }
                        Btn { visible: player.hdrOutputRestart || player.gamescopeRestart; text: tr("restartNow"); primary: true; onClicked: player.restartApp() }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 10
                        SwitchRow { text: tr("retroTvTitle"); checked: player.retroTv; onToggled: (v) => player.retroTv = v }
                        Hint { text: tr("retroHint") }
                        Label_ { visible: player.retroTv; text: tr("retroLines") }
                        Segmented {
                            visible: player.retroTv; width: parent.width
                            readonly property var values: [240, 480, 0]
                            model: ["240", "480", tr("native")]
                            current: Math.max(0, values.indexOf(player.retroLines)); onPicked: (i) => player.retroLines = values[i]
                        }
                    }
                    Sep {}
                    Column {
                        width: parent.width; spacing: 10
                        SwitchRow { text: tr("pixelPerfect"); checked: player.pixelPerfect; onToggled: (v) => player.pixelPerfect = v }
                        Hint { text: tr("pixelHint") }
                        Label_ { visible: player.pixelPerfect; text: tr("pixelGrid") + (player.pixelGrid === 1 && player.pixelGridSize !== "" ? "  ·  " + player.pixelGridSize : "") }
                        Segmented { visible: player.pixelPerfect; width: parent.width; model: tr("pixelGridModes"); current: player.pixelGrid; onPicked: (i) => player.pixelGrid = i }
                        Hint { visible: player.pixelPerfect; text: tr("pixelGridHint") }
                    }
                }

                // ======================================== Sound
                Column {
                    id: soundPage
                    visible: menu.tab === 3
                    width: parent.width; spacing: 14
                    property var sinks: []
                    function refresh() { sinks = player.audioSinks() }
                    Component.onCompleted: refresh()
                    Connections { target: menu; function onOpenChanged() { if (menu.open) soundPage.refresh() } }
                    Title { text: tr("audioOut") }
                    Column {
                        width: parent.width; spacing: 4
                        Repeater {
                            model: [{ name: "", desc: tr("audioOutDefault") }].concat(soundPage.sinks.slice(0, 8))
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool sel: player.audioOutput === modelData.name
                                width: soundPage.width; height: 32; radius: 8 * Theme.round
                                color: sinkMa.containsMouse ? Theme.surfaceHover : Theme.surface
                                gradient: sel ? Theme.sunset : null
                                Text { font.family: Theme.font; anchors { left: parent.left; right: parent.right; leftMargin: 12; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                    text: modelData.desc; elide: Text.ElideRight; font.pixelSize: 15; color: parent.sel ? Theme.accentInk : Theme.text }
                                MouseArea { id: sinkMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.audioOutput = modelData.name }
                            }
                        }
                    }
                    Item {
                        width: parent.width; height: 22
                        Text { font.family: Theme.font; text: tr("audioIn"); color: Theme.label; font.pixelSize: 16; anchors.verticalCenter: parent.verticalCenter }
                        Text { font.family: Theme.font; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; font.pixelSize: 13; font.weight: Font.Medium
                            color: player.audioActive ? Theme.ok : Theme.muted
                            text: player.audioActive ? (player.muted ? tr("audioInMuted") : tr("audioInOn")) : tr("audioInNone") }
                    }
                    Rectangle {   // spectrum: 24 bands from 50 Hz to 16 kHz
                        width: parent.width; height: 56; radius: 8 * Theme.round; color: Theme.surfaceDeep; border.color: Theme.border
                        Row {
                            id: specRow
                            anchors { fill: parent; margins: 6 }
                            spacing: 2
                            readonly property int n: 24
                            Repeater {
                                model: specRow.n
                                delegate: Item {
                                    required property int index
                                    width: (specRow.width - (specRow.n - 1) * specRow.spacing) / specRow.n; height: specRow.height
                                    readonly property real level: player.audioBands.length > index ? player.audioBands[index] : 0
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; radius: 2 * Theme.round
                                        height: Math.max(2, parent.height * parent.level); gradient: Theme.sunset; opacity: player.muted ? 0.45 : 1 }
                                }
                            }
                        }
                    }
                    Hint { text: tr("audioInHint") }
                    Sep {}
                    Param { label: tr("audioDelay"); from: -300; to: 300; decimals: 0; neutral: 0; value: player.audioDelay; onMoved: (v) => player.audioDelay = Math.round(v / 10) * 10 }
                    Hint { text: tr("audioDelayHint") }
                    SwitchRow { text: tr("normalize"); checked: player.normalize; onToggled: (v) => player.normalize = v }
                }

                // ======================================== Record & stream
                Column {
                    visible: menu.tab === 4
                    width: parent.width; spacing: 16
                    RecordSection { width: parent.width; tr: menu.tr }
                    Sep {}
                    // comparison clips and markers
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: tr("compareTitle") }
                        Hint { text: tr("compareHint") }
                        Segmented { width: parent.width; model: tr("compareLayouts"); current: player.compareLayout - 1; onPicked: (i) => player.compareLayout = i + 1 }
                        Label_ { text: tr("compareLength") }
                        Segmented { width: parent.width; model: ["10 s", "15 s", "30 s", "60 s"]
                            readonly property var secs: [10, 15, 30, 60]
                            current: Math.max(0, secs.indexOf(player.compareSeconds)); onPicked: (i) => player.compareSeconds = secs[i] }
                        Btn { text: player.comparing ? tr("compareStop") : tr("compareStart"); primary: !player.comparing; danger: player.comparing; onClicked: player.toggleCompare() }
                        Label_ { text: tr("markerTitle") }
                        Hint { text: tr("markerHint") }
                        Btn { text: tr("markerAdd") + (player.recording && player.markers > 0 ? "  ·  " + player.markers : ""); onClicked: player.addMarker() }
                    }
                    Sep {}
                    ObsSection { width: parent.width; tr: menu.tr }
                }

                // ======================================== System
                Column {
                    id: sysPage
                    visible: menu.tab === 5
                    width: parent.width; spacing: 16
                    property var checks: []
                    // ---- check my setup
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: tr("checkTitle") }
                        Hint { text: tr("checkHint") }
                        Btn { text: tr("checkRun"); primary: true; onClicked: sysPage.checks = player.setupCheck() }
                        Repeater {
                            model: sysPage.checks
                            delegate: Rectangle {
                                required property var modelData
                                width: sysPage.width; height: chkCol.implicitHeight + 20; radius: 10 * Theme.round
                                color: Theme.surfaceDeep
                                border.color: modelData.kind === "warn" ? Theme.warn : modelData.kind === "ok" ? Theme.okLine : Theme.border
                                Column {
                                    id: chkCol
                                    x: 12; y: 10; width: parent.width - 24; spacing: 6
                                    Text { font.family: Theme.font; width: parent.width; wrapMode: Text.Wrap; text: modelData.title; font.pixelSize: 16; font.weight: Font.DemiBold
                                        color: modelData.kind === "warn" ? Theme.warn : modelData.kind === "ok" ? Theme.ok : Theme.text }
                                    Text { font.family: Theme.font; width: parent.width; wrapMode: Text.Wrap; text: modelData.detail; font.pixelSize: 14; color: Theme.label; lineHeight: 1.15 }
                                    Btn { visible: modelData.fix !== ""; text: modelData.fix
                                        onClicked: { player.applySetupFix(modelData.id); Qt.callLater(function () { sysPage.checks = player.setupCheck() }) } }
                                }
                            }
                        }
                    }
                    Sep {}
                    // ---- updates
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: tr("updTitle") }
                        Text { font.family: Theme.font; width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 15
                            color: player.updateState === "error" ? Theme.warn : player.updateState === "available" || player.updateState === "ready" ? Theme.ok : Theme.text
                            text: tr("updVersion").replace("%1", player.appVersion) + (player.updateText !== "" ? "\n" + player.updateText : "") }
                        Flow {
                            width: parent.width; spacing: 8
                            Btn { enabled: player.inAppUpdatesEnabled; visible: player.updateState !== "available" && player.updateState !== "ready" && player.updateState !== "downloading"
                                text: tr("updCheck"); onClicked: player.checkForUpdate(true) }
                            Btn { visible: player.updateState === "available"; primary: true
                                text: player.updateSelf ? tr("updInstall").replace("%1", player.updateVersion) : tr("updPage"); onClicked: player.installUpdate() }
                            Btn { visible: player.updateState === "ready"; primary: true; text: tr("updRestart"); onClicked: player.restartApp() }
                            Btn { visible: player.updateState === "available" || player.updateState === "ready"; text: tr("updNotes"); onClicked: player.openReleasePage() }
                        }
                        Hint { visible: !player.updateSelf; text: tr("updNotSelf") }
                        SwitchRow { text: tr("updAuto"); active: player.inAppUpdatesEnabled; checked: player.inAppUpdatesEnabled && player.autoUpdateCheck; onToggled: (v) => player.autoUpdateCheck = v }
                        Hint { text: !player.inAppUpdatesEnabled ? tr("nvidiaLocalUpdate") : tr("updAutoHint") }
                    }
                    Sep {}
                    // ---- performance report
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: tr("perfTitle") }
                        Hint { text: tr("perfHint") }
                        Btn { text: tr("perfCopy"); onClicked: player.copyPerfReport() }
                        Text { font.family: Theme.font; width: parent.width; text: tr("gpu") + ": " + (player.gpuName !== "" ? player.gpuName : tr("cpuOnly")); color: Theme.muted; font.pixelSize: 14; wrapMode: Text.Wrap }
                        Text { font.family: Theme.font; width: parent.width; text: player.perf; color: Theme.label; font.pixelSize: 15; wrapMode: Text.Wrap }
                    }
                    Sep {}
                    // ---- Steam, start on plug-in, controller
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: "Steam" }
                        Hint { text: tr("steamHint") }
                        Btn { text: tr("steamAdd"); onClicked: player.addToSteam() }
                        SwitchRow { text: tr("autostart"); checked: player.autostart; onToggled: (v) => player.autostart = v }
                        Hint { text: tr("autostartHint") }
                        Item {
                            width: parent.width; height: 26
                            Text { font.family: Theme.font; text: tr("controller"); color: Theme.label; font.pixelSize: 16; anchors.verticalCenter: parent.verticalCenter }
                            Text { font.family: Theme.font; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; width: parent.width * 0.62; horizontalAlignment: Text.AlignRight; elide: Text.ElideRight
                                font.pixelSize: 14; color: gamepad.controllers.length > 0 ? Theme.ok : Theme.muted
                                text: gamepad.controllers.length > 0 ? "● " + tr("padOn").replace("%1", gamepad.controllers.join(", ")) : tr("padNone") }
                        }
                        Hint { text: tr("padHint") }
                    }
                    Sep {}
                    // ---- look and language
                    Column {
                        width: parent.width; spacing: 10
                        Title { text: tr("startLook") }
                        Flow {
                            width: parent.width; spacing: 8
                            Repeater {
                                model: Theme.names
                                delegate: Rectangle {
                                    required property string modelData
                                    readonly property var pal: Theme.palettes[modelData]
                                    readonly property bool sel: Theme.name === modelData
                                    width: 140; height: 44; radius: 12 * Theme.round; color: pal.surface
                                    border.width: sel ? 2 : 1; border.color: sel ? pal.accent : Theme.border
                                    Row { anchors.centerIn: parent; spacing: 8
                                        Image { source: "icons/logo-" + modelData + ".svg"; sourceSize: Qt.size(26, 26); width: 26; height: 26; anchors.verticalCenter: parent.verticalCenter }
                                        Text { font.family: Theme.font; text: pal.title; color: pal.text; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter } }
                                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: player.theme = modelData }
                                }
                            }
                        }
                        Flow {
                            width: parent.width; spacing: 6
                            Repeater {
                                model: ["auto"].concat(S.languages)
                                delegate: Rectangle {
                                    required property string modelData
                                    readonly property bool sel: player.languageChoice === modelData
                                    width: lgText.implicitWidth + 22; height: 30; radius: 15 * Theme.round
                                    color: sel ? Theme.accent : (lgMa.containsMouse ? Theme.surfaceHover : Theme.surface)
                                    gradient: sel ? Theme.sunset : null
                                    Text { id: lgText; font.family: Theme.font; anchors.centerIn: parent; font.pixelSize: 13
                                        text: modelData === "auto" ? tr("langAuto") : S.names[modelData]
                                        color: parent.sel ? Theme.accentInk : Theme.text }
                                    MouseArea { id: lgMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.languageChoice = modelData }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
