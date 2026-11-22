import QtQuick
import QtQuick.Window
import VeyAmux 1.0
import "Strings.js" as S

Window {
    id: win
    visible: false          // shown from C++ once the graphics device is configured
    width: 1280
    height: 720
    minimumWidth: 720
    minimumHeight: 420
    color: Theme.bg
    title: player.hasMedia ? player.title + " — VeyAmux" : "VeyAmux"

    // the theme (Theme.qml) follows the saved choice
    Binding { target: Theme; property: "name"; value: player.theme }

    readonly property bool fs: visibility === Window.FullScreen
    // small screens (Steam Deck and other handhelds, under ~10" wide) and small windows: the menu fills the whole window
    readonly property real screenMm: Screen.pixelDensity > 0 ? Screen.width * Screen.devicePixelRatio / Screen.pixelDensity : 1000
    readonly property bool compactMenu: handheld || Qt.application.arguments.indexOf("--compact-menu") >= 0 || screenMm < 260 || width < 1000
    property bool panelOpen: startPanel
    onPanelOpenChanged: if (panelOpen) player.demoRelease()   // the demo stops swimming while the menu is open
    Binding { target: player; property: "meterWanted"; value: win.panelOpen }   // the spectrum is only updated while it shows
    property bool controlsShown: true
    property bool scrubbing: false
    property real scrubValue: 0

    // texts: Strings.js; player.language is the chosen language (follows the system unless one is picked)
    function t(k) { var s = S.strings[player.language] || S.strings.en; if (s[k] !== undefined) return s[k]; return S.strings.en[k] !== undefined ? S.strings.en[k] : k }
    function fmt(sec) {
        sec = Math.max(0, Math.floor(sec))
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function wake() { controlsShown = true; hideTimer.restart() }
    readonly property var scales: [1.0, 1.5, 2.0, 3.0]

    Timer { id: hideTimer; interval: 2600; onTriggered: if (player.playing && !panelOpen && !scrubbing && !keepControls) controlsShown = false }

    // ------------------------------------------------------------- video surface
    // HDR output: the whole window is drawn into an FP16 layer and converted to scRGB light for the HDR swapchain
    Item {
    id: hdrRoot
    anchors.fill: parent
    layer.enabled: player.hdrOutputActive
    layer.format: ShaderEffectSource.RGBA16F
    layer.effect: ShaderEffect {
        property real white: 203.0
        property real mode: player.hdrModeActive
        fragmentShader: "qrc:/shaders/hdr_window.frag.qsb"
    }

    VideoItem {
        id: video; anchors.fill: parent; bridge: player
        // HDR output: the picture is PQ-coded; this layer turns it into extended sRGB for the window layer below
        layer.enabled: player.hdrOutputActive
        layer.effect: ShaderEffect { fragmentShader: "qrc:/shaders/pq_video.frag.qsb" }
    }


    // ------------------------------------------------------------- start screen (nothing playing)
    // The sources as three large cards, "continue" for the last capture card, the demo, and the look below.
    // Scaled with the window height so it reads the same on a TV from the couch.
    Item {
        id: startCol
        visible: !player.hasMedia
        anchors.fill: parent
        readonly property real k: Math.max(0.85, Math.min(1.7, win.height / 900))
        readonly property string lastDev: player.lastCaptureDevice()
        readonly property string lastName: player.lastCaptureName()
        // a soft wash of the theme's colours from below
        Rectangle {
            anchors.fill: parent; opacity: 0.16
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.55; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.accent }
            }
        }
        // top: name and version, an update when there is one, the menu
        Row {
            x: 28 * startCol.k; y: 22 * startCol.k; spacing: 12 * startCol.k
            Image { source: Theme.logo; sourceSize: Qt.size(64, 64); width: 40 * startCol.k; height: width; smooth: true; anchors.verticalCenter: parent.verticalCenter }
            Text { font.family: Theme.headFont; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; text: "VeyAmux"; color: Theme.heading
                font.pixelSize: 26 * Theme.headScale * startCol.k; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
            Text { font.family: Theme.font; text: player.appVersion; color: Theme.dim; font.pixelSize: 13 * startCol.k; anchors.verticalCenter: parent.verticalCenter }
        }
        Row {
            anchors { right: parent.right; top: parent.top; rightMargin: 24 * startCol.k; topMargin: 22 * startCol.k }
            spacing: 10 * startCol.k
            Rectangle {
                visible: player.updateState === "available" || player.updateState === "downloading" || player.updateState === "ready"
                height: 40 * startCol.k; width: updRow.implicitWidth + 28 * startCol.k; radius: height / 2 * Theme.round
                gradient: updMa.containsMouse ? Theme.sunsetHover : Theme.sunset
                Row { id: updRow; anchors.centerIn: parent; spacing: 8
                    Image { source: "icons/download.svg"; width: 18 * startCol.k; height: width; sourceSize: Qt.size(36, 36); anchors.verticalCenter: parent.verticalCenter }
                    Text { font.family: Theme.font; anchors.verticalCenter: parent.verticalCenter; color: Theme.accentInk; font.pixelSize: 14 * startCol.k; font.weight: Font.DemiBold
                        text: player.updateState === "ready" ? t("updRestart") : player.updateState === "downloading" ? player.updateText
                              : t("updBanner").replace("%1", player.updateVersion) + "  ·  " + t("updBannerBtn") } }
                MouseArea { id: updMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: player.updateState === "ready" ? player.restartApp() : player.updateState === "available" ? player.installUpdate() : null }
            }
            IconButton { size: 44 * startCol.k; source: "icons/system.svg"; onClicked: { win.panelOpen = true } }
        }

        Column {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -20 * startCol.k
            spacing: 22 * startCol.k
            Text { font.family: Theme.headFont; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; anchors.horizontalCenter: parent.horizontalCenter
                text: t("startHeadline"); color: Theme.heading; font.pixelSize: 40 * Theme.headScale * startCol.k; font.weight: Font.DemiBold }
            // continue with the last capture card
            Rectangle {
                visible: startCol.lastDev !== "" && startCol.lastDev !== "screencast"
                anchors.horizontalCenter: parent.horizontalCenter
                width: resumeRow.implicitWidth + 48 * startCol.k; height: 52 * startCol.k; radius: height / 2 * Theme.round
                gradient: resMa.containsMouse ? Theme.sunsetHover : Theme.sunset
                border.width: Theme.edge; border.color: "#000000"
                Row { id: resumeRow; anchors.centerIn: parent; spacing: 10 * startCol.k
                    Image { source: "icons/play.svg"; width: 20 * startCol.k; height: width; sourceSize: Qt.size(40, 40); anchors.verticalCenter: parent.verticalCenter }
                    Text { font.family: Theme.headCaps ? Theme.headFont : Theme.font; anchors.verticalCenter: parent.verticalCenter; color: Theme.accentInk
                        font.pixelSize: (Theme.headCaps ? 12 : 17) * startCol.k; font.weight: Font.DemiBold; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase
                        text: t("startResume").replace("%1", startCol.lastName !== "" ? startCol.lastName : t("startCapture")) } }
                MouseArea { id: resMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.openCaptureAuto(startCol.lastDev) }
            }
            // the three sources
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 18 * startCol.k
                Repeater {
                    model: [
                        { icon: "icons/capture.svg", title: "startCapture", sub: "startCaptureSub", act: 0 },
                        { icon: "icons/gamepad.svg", title: "startPs", sub: "startPsSub", act: 1 },
                        { icon: "icons/folder.svg", title: "startFile", sub: "startFileSub", act: 2 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        width: 250 * startCol.k; height: 190 * startCol.k; radius: 18 * Theme.round
                        color: srcMa.containsMouse ? Theme.surfaceHover : Theme.surface
                        border.width: Math.max(1, Theme.edge); border.color: srcMa.containsMouse ? Theme.accentLine : Theme.border
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Rectangle { visible: Theme.edge > 0; x: 2; y: parent.height - 7; width: parent.width - 4; height: 5; color: "#59000000" }
                        Column {
                            anchors { top: parent.top; topMargin: 22 * startCol.k; horizontalCenter: parent.horizontalCenter }
                            spacing: 10 * startCol.k; width: parent.width - 32 * startCol.k
                            Rectangle { width: 64 * startCol.k; height: width; radius: width / 2 * Theme.round; anchors.horizontalCenter: parent.horizontalCenter
                                gradient: Theme.sunset
                                Image { source: modelData.icon; anchors.centerIn: parent; width: 30 * startCol.k; height: width; sourceSize: Qt.size(64, 64) } }
                            Text { font.family: Theme.headCaps ? Theme.headFont : Theme.font; text: t(modelData.title); color: Theme.heading; width: parent.width; horizontalAlignment: Text.AlignHCenter
                                font.pixelSize: (Theme.headCaps ? 12 : 19) * startCol.k; font.weight: Font.DemiBold; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; elide: Text.ElideRight }
                            Text { font.family: Theme.font; text: t(modelData.sub); color: Theme.muted; width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                                font.pixelSize: 13 * startCol.k; maximumLineCount: 2 }
                        }
                        MouseArea { id: srcMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: modelData.act === 0 ? capDlg.openDialog() : modelData.act === 1 ? psDlg.openDialog() : player.openDialog() }
                    }
                }
            }
            // the demo: try every feature without a capture card
            Column {
                visible: player.demoAvailable
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10 * startCol.k
                Item { width: 1; height: 4 * startCol.k }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter; spacing: 8
                    Image { source: "icons/waves.svg"; width: 20 * startCol.k; height: width; sourceSize: Qt.size(40, 40); anchors.verticalCenter: parent.verticalCenter; opacity: 0.8 }
                    Text { font.family: Theme.font; text: t("demoTitle"); color: Theme.label; font.pixelSize: 15 * startCol.k; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
                }
                Text { font.family: Theme.font; anchors.horizontalCenter: parent.horizontalCenter; width: Math.min(620 * startCol.k, win.width - 60); horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap; text: t("demoHint"); color: Theme.muted; font.pixelSize: 13 * startCol.k }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 10 * startCol.k
                    Repeater {
                        model: [[720, 30], [720, 60], [1080, 30], [1080, 60]]
                        delegate: Rectangle {
                            required property var modelData
                            width: 130 * startCol.k; height: 38 * startCol.k; radius: height / 2 * Theme.round
                            color: dmMa.containsMouse ? Theme.surfaceHover : Theme.surfaceDeep; border.color: Theme.accentLine
                            border.width: Math.max(1, Theme.edge)
                            Text { font.family: Theme.headCaps ? Theme.headFont : Theme.font; anchors.centerIn: parent; text: modelData[0] + "p · " + modelData[1] + " fps"; color: Theme.text
                                font.pixelSize: (Theme.headCaps ? 10 : 14) * startCol.k; font.weight: Font.DemiBold; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase }
                            MouseArea { id: dmMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.openDemo(modelData[1], modelData[0]) }
                        }
                    }
                }
            }
        }

        // bottom: theme and language, and the keys
        Column {
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 20 * startCol.k }
            spacing: 10 * startCol.k
            Grid {
                anchors.horizontalCenter: parent.horizontalCenter
                columns: themeChoices.implicitWidth + languageChoices.implicitWidth + columnSpacing <= startCol.width - 24 ? 2 : 1
                horizontalItemAlignment: Grid.AlignHCenter
                columnSpacing: 26 * startCol.k
                rowSpacing: 6 * startCol.k
                Row {
                    id: themeChoices
                    spacing: 8 * startCol.k
                    Repeater {
                        model: Theme.names
                        delegate: Rectangle {
                            required property string modelData
                            readonly property var pal: Theme.palettes[modelData]
                            readonly property bool sel: Theme.name === modelData
                            width: 112 * startCol.k; height: 36 * startCol.k; radius: 10 * Theme.round
                            color: pal.surface
                            border.width: sel ? 2 : 1
                            border.color: sel ? pal.accent : (thMa.containsMouse ? pal.accentLine : Theme.border)
                            Row { anchors.centerIn: parent; spacing: 6
                                Image { source: "icons/logo-" + modelData + ".svg"; sourceSize: Qt.size(40, 40); width: 20 * startCol.k; height: width; anchors.verticalCenter: parent.verticalCenter }
                                Text { font.family: Theme.font; text: pal.title; color: pal.text; font.pixelSize: 13 * startCol.k; font.weight: sel ? Font.DemiBold : Font.Medium; anchors.verticalCenter: parent.verticalCenter } }
                            MouseArea { id: thMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.theme = modelData }
                        }
                    }
                }
                Row {
                    id: languageChoices
                    spacing: 8 * startCol.k
                    Repeater {
                        model: ["auto"].concat(S.languages)
                        delegate: Rectangle {
                            required property string modelData
                            readonly property bool sel: player.languageChoice === modelData
                            width: lgText.implicitWidth + 18 * startCol.k; height: 36 * startCol.k; radius: height / 2 * Theme.round
                            color: sel ? Theme.accent : (lgMa.containsMouse ? Theme.surfaceHover : Theme.surface)
                            gradient: sel ? Theme.sunset : null
                            border.width: Theme.edge; border.color: "#000000"
                            Text { id: lgText; font.family: Theme.font; anchors.centerIn: parent; font.pixelSize: 13 * startCol.k
                                text: modelData === "auto" ? t("langAuto") : modelData.toUpperCase()
                                color: parent.sel ? Theme.accentInk : Theme.text; font.weight: parent.sel ? Font.DemiBold : Font.Normal }
                            MouseArea { id: lgMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: player.languageChoice = modelData }
                        }
                    }
                }
            }
            Text { font.family: Theme.font; anchors.horizontalCenter: parent.horizontalCenter; text: t("hint"); color: Theme.dim; font.pixelSize: 12 * startCol.k }
        }
    }

    // Click / hover / double-click on the video; in the demo scene, dragging looks around
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        enabled: player.hasMedia
        property real lx: 0
        property real ly: 0
        cursorShape: player.demoActive && pressed ? Qt.ClosedHandCursor : (win.controlsShown || !win.fs) ? Qt.ArrowCursor : Qt.BlankCursor
        onPressed: (m) => { lx = m.x; ly = m.y }
        onPositionChanged: (m) => {
            wake()
            if (pressed && player.demoActive) { player.demoLook(m.x - lx, m.y - ly); lx = m.x; ly = m.y }
        }
        // a tap on a touchscreen (Steam Deck) only shows the controls with the menu button; a mouse click pauses
        onClicked: (m) => { if (!player.demoActive && m.source === Qt.MouseEventNotSynthesized) player.togglePlay(); wake() }
        onDoubleClicked: (m) => { if (!player.demoActive && m.source === Qt.MouseEventNotSynthesized) player.togglePlay(); win.visibility = fs ? Window.Windowed : Window.FullScreen }
    }

    // demo scene: how to swim (while the controls are shown)
    Rectangle {
        visible: player.demoActive && player.demoProgress >= 1 && win.controlsShown && !win.panelOpen
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 96 }
        width: Math.min(parent.width - 40, demoKeysText.implicitWidth + 32); height: demoKeysText.implicitHeight + 16
        radius: 10 * Theme.round; color: Theme.overlay
        Text { id: demoKeysText; anchors.centerIn: parent; width: parent.width - 32; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
            font.family: Theme.font; font.pixelSize: 13; color: Theme.text; text: t("demoKeys") }
    }

    DropArea {
        anchors.fill: parent
        onDropped: (drop) => { if (drop.hasUrls) player.openUrl(drop.urls[0]) }
    }

    // ------------------------------------------------------------- status toast
    Rectangle {
        visible: player.status !== ""
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 18 }
        width: Math.min(parent.width - 40, toastText.implicitWidth + 32); height: 36; radius: 18 * Theme.round; color: player.hdrOutputActive ? Qt.rgba(Theme.toast.r, Theme.toast.g, Theme.toast.b, 1) : Theme.toast
        border.color: Theme.accentLine
        Text { font.family: Theme.font; id: toastText; anchors.centerIn: parent; text: player.status; color: Theme.warn; font.pixelSize: 13; elide: Text.ElideRight; width: parent.width - 28; horizontalAlignment: Text.AlignHCenter }
    }

    // short confirmations (profile loaded, clip or screenshot saved)
    Rectangle {
        visible: player.notice !== "" && player.status === ""
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 18 }
        width: Math.min(parent.width - 40, noticeText.implicitWidth + 32); height: 36; radius: 18 * Theme.round; color: player.hdrOutputActive ? Qt.rgba(Theme.toast.r, Theme.toast.g, Theme.toast.b, 1) : Theme.toast
        border.color: Theme.okLine
        Text { font.family: Theme.font; id: noticeText; anchors.centerIn: parent; text: player.notice; color: Theme.ok; font.pixelSize: 13; elide: Text.ElideRight; width: parent.width - 28; horizontalAlignment: Text.AlignHCenter }
    }

    // frame-time overlay (F3)
    Rectangle {
        id: ftOverlay
        visible: player.overlay
        z: 6
        anchors { left: parent.left; bottom: parent.bottom; leftMargin: 16; bottomMargin: 90 }
        width: 440; height: ftCol.implicitHeight + 16; radius: 10 * Theme.round
        color: player.hdrOutputActive ? Theme.bg : Theme.overlay; border.color: Theme.border
        Column {
            id: ftCol
            x: 8; y: 8; width: parent.width - 16; spacing: 6
            Text { width: parent.width; wrapMode: Text.Wrap; text: player.overlayStats; color: Theme.text; font.pixelSize: 12; font.family: "monospace" }
            Canvas {
                id: ftCanvas
                width: parent.width; height: 70
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    const g = player.overlayGraph, n = g.length;
                    const target = player.overlayTarget;
                    const top = Math.max(target * 2.5, 20);
                    ctx.fillStyle = "rgba(255,255,255,0.05)";
                    ctx.fillRect(0, 0, width, height);
                    if (target > 0) {   // average frame time
                        const y = height - target / top * height;
                        ctx.strokeStyle = "rgba(63,224,208,0.6)"; ctx.lineWidth = 1;
                        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();
                    }
                    if (n < 2) return;
                    ctx.strokeStyle = Theme.accent; ctx.lineWidth = 1.5;
                    ctx.beginPath();
                    for (let i = 0; i < n; ++i) {
                        const x = i * width / (n - 1);
                        const y = height - Math.min(g[i], top) / top * height;
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
                    }
                    ctx.stroke();
                }
                Connections { target: player; function onOverlayTick() { ftCanvas.requestPaint() } }
            }
            Text { font.family: Theme.font; text: t("overlayHint"); color: Theme.dim; font.pixelSize: 11 }
        }
    }

    // recording indicator (stays small and out of the way; the picture itself is recorded without it)
    Row {
        visible: player.recording || player.streaming
        anchors { top: parent.top; right: parent.right; topMargin: 14; rightMargin: 16 }
        spacing: 6; z: 5
        Rectangle { width: 10; height: 10; radius: 5 * Theme.round; color: "#ff4040"; anchors.verticalCenter: parent.verticalCenter
            SequentialAnimation on opacity { running: player.recording || player.streaming; loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 700 } NumberAnimation { to: 1; duration: 700 } } }
        Text { font.family: Theme.font; text: player.streaming ? (player.recording ? t("liveRecordingBadge") : t("live")) : t("recordingBadge"); color: "#ffdddd"; font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 1 }
    }

    // ------------------------------------------------------------- title (top)
    Rectangle {
        id: topBar
        visible: player.hasMedia
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 70
        opacity: controlsShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        gradient: Gradient { GradientStop { position: 0; color: "#b0000000" } GradientStop { position: 1; color: "#00000000" } }
        Text { font.family: Theme.font; x: 22; y: 18; width: parent.width - 44; text: player.title; color: Theme.heading; font.pixelSize: 16; font.weight: Font.Medium; elide: Text.ElideRight }
        Text { font.family: Theme.font; x: 22; y: 40; text: player.mediaInfo; color: Theme.label; font.pixelSize: 12 }
    }

    // ------------------------------------------------------------- bottom controls
    Rectangle {
        id: bottomBar
        visible: player.hasMedia
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: 110
        opacity: controlsShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        gradient: Gradient { GradientStop { position: 0; color: "#00000000" } GradientStop { position: 1; color: "#d0000000" } }

        // seek bar
        Item {
            id: seek
            visible: player.duration > 0
            anchors { left: parent.left; right: parent.right; bottom: controlsRow.top; leftMargin: 22; rightMargin: 22; bottomMargin: 4 }
            height: 22
            Text { font.family: Theme.font; id: tCur; anchors.verticalCenter: parent.verticalCenter; text: fmt(scrubbing ? scrubValue : player.position); color: Theme.text; font.pixelSize: 12; width: 52 }
            VSlider {
                anchors { left: tCur.right; right: tDur.left; verticalCenter: parent.verticalCenter; leftMargin: 6; rightMargin: 6 }
                from: 0; to: Math.max(player.duration, 0.001)
                value: scrubbing ? scrubValue : player.position
                onDraggingChanged: { if (dragging) { win.scrubbing = true } else { win.scrubbing = false; player.seek(win.scrubValue) } }
                onMoved: (v) => { win.scrubValue = v; wake() }
            }
            Text { font.family: Theme.font; id: tDur; anchors { right: parent.right; verticalCenter: parent.verticalCenter } text: fmt(player.duration); color: Theme.label; font.pixelSize: 12; width: 52; horizontalAlignment: Text.AlignRight }
        }

        Rectangle {
            visible: player.live
            anchors { left: parent.left; bottom: controlsRow.top; leftMargin: 22; bottomMargin: 10 }
            width: 58; height: 22; radius: 11 * Theme.round; color: Theme.live
            Text { font.family: Theme.font; anchors.centerIn: parent; text: t("live"); color: Theme.heading; font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 1 }
        }
        Row {
            id: controlsRow
            anchors { left: parent.left; bottom: parent.bottom; leftMargin: 14; bottomMargin: 10 }
            spacing: 2
            IconButton { size: 40; source: "icons/folder.svg"; onClicked: player.openDialog() }
            IconButton { size: 40; source: "icons/capture.svg"; active: player.live; onClicked: capDlg.openDialog() }
            IconButton { size: 40; source: "icons/back10.svg"; onClicked: { player.seekRelative(-10); wake() } }
            IconButton { size: 46; source: player.playing ? "icons/pause.svg" : "icons/play.svg"; onClicked: { player.togglePlay(); wake() } }
            IconButton { size: 40; source: "icons/fwd10.svg"; onClicked: { player.seekRelative(10); wake() } }
            IconButton { size: 40; source: player.muted || player.volume < 0.01 ? "icons/mute.svg" : "icons/volume.svg"; onClicked: player.muted = !player.muted }
            VSlider {
                width: 90; anchors.verticalCenter: parent.verticalCenter
                from: 0; to: 1.0; value: player.muted ? 0 : player.volume
                onMoved: (v) => { player.muted = false; player.volume = v }
            }
            Item { width: 14; height: 1 }
            IconButton { size: 40; source: "icons/sparkles.svg"; active: win.panelOpen; onClicked: { win.panelOpen = !win.panelOpen; wake() } }
            IconButton { size: 40; source: win.fs ? "icons/exitfs.svg" : "icons/fullscreen.svg"; onClicked: win.visibility = win.fs ? Window.Windowed : Window.FullScreen }
        }
    }

    // ------------------------------------------------------------- controller (Gamepad.h)
    // Select + Start (or Guide held) opens / closes the menu; while it is open the controllers are kept from the game.
    // The D-pad / left stick moves between the menu's controls (the nearest one in that direction), A presses,
    // left/right change a slider, LB/RB jump between the columns, B closes the menu.
    // The controller also steers the start screen and the capture dialog (no game is running there).
    Binding { target: gamepad; property: "grabbed"; value: win.panelOpen || capDlg.visible || psDlg.visible || !player.hasMedia }
    Connections {
        target: gamepad
        function onMenuCombo() {
            if (capDlg.visible || psDlg.visible) return
            win.panelOpen = !win.panelOpen
            padNav.active = true; padNav.current = null
            if (win.panelOpen) padOpenTimer.restart(); else Qt.callLater(padNav.reset)
        }
        function onButton(name) { if (padNav.scope()) { padNav.active = true; padNav.press(name) } }
    }
    // a newly shown dialog starts on its first control
    Connections { target: capDlg; function onVisibleChanged() { if (padNav.active) { padNav.current = null; Qt.callLater(padNav.reset) } } }
    Connections { target: psDlg; function onVisibleChanged() { if (!psDlg.visible) keyRoot.forceActiveFocus(); if (padNav.active) { padNav.current = null; Qt.callLater(padNav.reset) } } }
    // the panel fades in (160 ms) and is invisible until then, so the first control is picked once it shows
    Timer { id: padOpenTimer; interval: 200; onTriggered: if (win.panelOpen && !padNav.current) padNav.reset() }
    QtObject {
        id: padNav
        property bool active: false        // shows the highlight; mouse use hides it again
        property Item current: null
        // every control in the menu: clickable areas (buttons, segments, switches, list rows) and sliders
        function collect(item, out) {
            if (!item || !item.visible || item.opacity <= 0.01) return
            if (item.from !== undefined && item.to !== undefined && item.neutral !== undefined && item.moved !== undefined) {
                out.push(item); return                       // a slider (VSlider): its own mouse area is not a target
            }
            if (item.acceptedButtons !== undefined && item.enabled && item.objectName !== "padSkip" && item.width > 4 && item.height > 4
                    && !(item.width >= win.width * 0.9 && item.height >= win.height * 0.5)) out.push(item)
            for (var i = 0; i < item.children.length; ++i) collect(item.children[i], out)
        }
        // what the controller steers: the capture dialog, else the menu, else the start screen
        function scope() { return psDlg.visible ? psDlg : capDlg.visible ? capDlg : win.panelOpen ? panel : !player.hasMedia ? startCol : null }
        function targets() { var o = []; collect(scope(), o); return o }
        function centre(t) { var p = t.mapToItem(win.contentItem, t.width / 2, t.height / 2); return p }
        function isSlider(t) { return t && t.neutral !== undefined && t.moved !== undefined }
        function reset() {
            var ts = targets(); if (ts.length === 0) { current = null; return }
            var best = ts[0], bp = centre(best)
            for (var i = 1; i < ts.length; ++i) { var p = centre(ts[i]); if (p.x + p.y * 3 < bp.x + bp.y * 3) { best = ts[i]; bp = p } }
            current = best
        }
        function move(dx, dy) {
            var ts = targets(); if (!current || ts.indexOf(current) < 0) { reset(); return }
            var c = centre(current), best = null, bestScore = 1e9
            for (var i = 0; i < ts.length; ++i) {
                if (ts[i] === current) continue
                var p = centre(ts[i]), vx = p.x - c.x, vy = p.y - c.y
                var along = vx * dx + vy * dy, across = Math.abs(vx * dy - vy * dx)
                if (along <= 2) continue                     // not in that direction
                var score = along + across * 2.5
                if (score < bestScore) { bestScore = score; best = ts[i] }
            }
            if (best) current = best
        }
        // LB / RB: the menu's tabs
        function column(step) {
            if (scope() !== panel) return
            panel.switchTab(step)
            current = null
            Qt.callLater(reset)
        }
        function press(name) {
            if (!current || targets().indexOf(current) < 0) reset()
            if (name === "up") move(0, -1)
            else if (name === "down") move(0, 1)
            else if (name === "left") { if (isSlider(current)) current.moved(Math.max(current.from, current.value - (current.to - current.from) / 40)); else move(-1, 0) }
            else if (name === "right") { if (isSlider(current)) current.moved(Math.min(current.to, current.value + (current.to - current.from) / 40)); else move(1, 0) }
            else if (name === "lb") column(-1)
            else if (name === "rb") column(1)
            else if (name === "b") { if (psDlg.visible) psDlg.close(); else if (capDlg.visible) { capDlg.visible = false; capDlg.closed() } else win.panelOpen = false }
            else if (name === "a" && current && !isSlider(current)) current.clicked(null)
            Qt.callLater(function () { padRing.place() })
        }
    }
    // the highlight around the selected control
    Rectangle {
        id: padRing
        z: 1000
        visible: padNav.active && padNav.scope() !== null && padNav.current !== null && padNav.targets().indexOf(padNav.current) >= 0
        color: "transparent"; border.width: 3; border.color: Theme.accent; radius: 8 * Theme.round
        function place() {
            var t = padNav.current; if (!t) return
            var p = t.mapToItem(win.contentItem, 0, 0), q = t.mapToItem(win.contentItem, t.width, t.height)
            x = p.x - 4; y = p.y - 4; width = q.x - p.x + 8; height = q.y - p.y + 8
        }
        Connections { target: padNav; function onCurrentChanged() {
            // the menu scrolls: bring the selected control into view
            if (padNav.current && panelOpen) {
                const f = panel.flick
                const p = padNav.current.mapToItem(f.contentItem, 0, 0)
                const h = padNav.current.height * panel.fit
                if (p.y < f.contentY + 20) f.contentY = Math.max(0, p.y - 40)
                else if (p.y + h > f.contentY + f.height - 20)
                    f.contentY = Math.max(0, Math.min(f.contentHeight - f.height, p.y + h - f.height + 40))
            }
            padRing.place()
        } }
    }

    // ------------------------------------------------------------- menu (Menu.qml: six tabs)
    Menu {
        id: panel
        tr: win.t
        open: win.panelOpen
        compact: win.compactMenu
        onCloseRequested: win.panelOpen = false
        onAwake: { wake(); padNav.active = false }
    }

    CaptureDialog { id: capDlg; anchors.fill: parent; z: 100; tr: win.t; onRemotePlay: psDlg.openDialog() }
    RemotePlayDialog { id: psDlg; anchors.fill: parent; z: 101; tr: win.t }

    // ------------------------------------------------------------- keyboard
    Item {
        id: keyRoot
        anchors.fill: parent
        focus: true
        // demo scene: W A S D Q E Shift swim, 1-6 viewpoints (while the menu is closed)
        function demoKeyName(k) {
            switch (k) {
            case Qt.Key_W: return "w"; case Qt.Key_A: return "a"; case Qt.Key_S: return "s"; case Qt.Key_D: return "d"
            case Qt.Key_Q: return "q"; case Qt.Key_E: return "e"; case Qt.Key_Shift: return "shift"
            }
            return ""
        }
        Keys.onReleased: (e) => { const k = demoKeyName(e.key); if (k !== "" && !e.isAutoRepeat && player.demoActive) { player.demoKey(k, false); e.accepted = true } }
        Keys.onPressed: (e) => {
            if (psDlg.visible) { if (e.key === Qt.Key_Escape) { psDlg.close(); e.accepted = true } return }
            if (player.demoActive && !win.panelOpen && !capDlg.visible && !(e.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                const k = demoKeyName(e.key)
                if (k !== "") { if (!e.isAutoRepeat) player.demoKey(k, true); e.accepted = true; return }
                if (e.key >= Qt.Key_1 && e.key <= Qt.Key_6) { player.demoPose(e.key - Qt.Key_1); e.accepted = true; return }
            }
            if (e.key >= Qt.Key_1 && e.key <= Qt.Key_9 && !(e.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                player.loadProfileIndex(e.key - Qt.Key_1); e.accepted = true; return
            }
            wake()
            if (capDlg.visible) { if (e.key === Qt.Key_Escape) { capDlg.visible = false; e.accepted = true } return }
            switch (e.key) {
            case Qt.Key_Space: player.togglePlay(); break
            case Qt.Key_Left: player.seekRelative(e.modifiers & Qt.ShiftModifier ? -30 : -5); break
            case Qt.Key_Right: player.seekRelative(e.modifiers & Qt.ShiftModifier ? 30 : 5); break
            case Qt.Key_Up: player.volume = Math.min(1.5, player.volume + 0.05); break
            case Qt.Key_Down: player.volume = Math.max(0, player.volume - 0.05); break
            case Qt.Key_M: player.muted = !player.muted; break
            case Qt.Key_F: case Qt.Key_F11: win.visibility = win.fs ? Window.Windowed : Window.FullScreen; break
            case Qt.Key_Escape: if (win.fs) win.visibility = Window.Windowed; break
            case Qt.Key_E: player.enhance = !player.enhance; break
            case Qt.Key_P: win.panelOpen = !win.panelOpen; break
            case Qt.Key_O: player.openDialog(); break
            case Qt.Key_C: capDlg.openDialog(); break
            case Qt.Key_F3: player.overlay = !player.overlay; break
            case Qt.Key_F9: player.saveReplay(); break
            case Qt.Key_F10: player.toggleRecording(); break
            case Qt.Key_F6: player.addMarker(); break
            case Qt.Key_F12: case Qt.Key_Print: player.screenshot(); break
            default: return
            }
            e.accepted = true
        }
    }
    }   // hdrRoot
    Component.onCompleted: { hideTimer.start(); if (startDialog) capDlg.openDialog() }
}
