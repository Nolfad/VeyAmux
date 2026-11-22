import QtQuick

// Frame generation: Off (optionally VRR pacing) / Lossless Scaling (Fixed 2x = default, Fixed 3x) / AMD FSR 3.1.6 2x (free).
Column {
    id: root
    property var tr: function (k) { return k }
    spacing: 6

    readonly property string ls: player.lsfgState
    readonly property bool lsUsable: ls === "active" || ls === "ready"

    component Option: Rectangle {
        id: opt
        property int mode
        property string title
        property string detail
        property bool usable: true
        readonly property bool sel: player.frameGenMode === mode
        width: root.width
        height: col.implicitHeight + 14
        radius: 9 * Theme.round
        opacity: usable ? 1 : 0.45
        color: sel ? Theme.accentSoft : (ma.containsMouse && usable ? Theme.border : Theme.surface)
        border.color: sel ? Theme.accent : "transparent"
        Row {
            x: 10; anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Rectangle {   // radio dot
                width: 14; height: 14; radius: 7 * Theme.round; anchors.verticalCenter: parent.verticalCenter
                color: "transparent"; border.color: opt.sel ? Theme.accent : Theme.dim; border.width: 2
                Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3 * Theme.round; color: Theme.accent; visible: opt.sel }
            }
            Column {
                id: col
                width: root.width - 44
                spacing: 2
                Text { font.family: Theme.font; width: parent.width; text: opt.title; color: opt.sel ? Theme.heading : Theme.text; font.pixelSize: 17; font.weight: Font.Medium; wrapMode: Text.Wrap }
                Text { font.family: Theme.font; width: parent.width; text: opt.detail; visible: text !== ""; color: Theme.muted; font.pixelSize: 15; wrapMode: Text.Wrap }
            }
        }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; enabled: opt.usable; cursorShape: Qt.PointingHandCursor
            onClicked: player.frameGenMode = opt.mode }
    }

    // an indented on/off option belonging to the selected mode
    component SubToggle: Item {
        id: st
        property string title
        property string detail
        property bool checked
        signal toggled(bool value)
        width: root.width
        height: stCol.implicitHeight + 10
        Column {
            id: stCol
            x: 34; y: 5; width: parent.width - 34 - 52; spacing: 2
            Text { font.family: Theme.font; width: parent.width; text: st.title; color: Theme.text; font.pixelSize: 16; font.weight: Font.Medium; wrapMode: Text.Wrap }
            Text { font.family: Theme.font; width: parent.width; text: st.detail; visible: text !== ""; color: Theme.muted; font.pixelSize: 15; wrapMode: Text.Wrap }
        }
        ToggleSwitch { anchors.right: parent.right; anchors.rightMargin: 4; y: 6; checked: st.checked; onToggled: (v) => st.toggled(v) }
    }

    component LinkButton: Rectangle {
        id: lb
        property string text
        signal clicked()
        width: lbText.implicitWidth + 22; height: 32; radius: 10 * Theme.round
        color: lbMa.containsMouse ? Theme.accentHover : Theme.accent
        Text { font.family: Theme.font; id: lbText; anchors.centerIn: parent; text: lb.text; color: Theme.accentInk; font.pixelSize: 16; font.weight: Font.DemiBold }
        MouseArea { id: lbMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: lb.clicked() }
    }

    Text { font.family: Theme.headFont; width: Math.min(implicitWidth, parent.width - 62); elide: Text.ElideRight; font.capitalization: Theme.headCaps ? Font.AllUppercase : Font.MixedCase; text: tr("fg"); color: Theme.heading; font.pixelSize: 21 * Theme.headScale; font.weight: Font.Bold; font.letterSpacing: 0.5 }

    Option { mode: 0; title: tr("fgOff") }
    SubToggle {
        visible: player.frameGenMode === 0
        title: tr("vrr"); detail: tr("vrrHint")
        checked: player.vrr; onToggled: (v) => player.vrr = v
    }
    Row {
        visible: player.vrrRestartNeeded
        x: 34; spacing: 10
        Text { font.family: Theme.font; text: tr("vrrRestart"); color: Theme.warn; font.pixelSize: 15; anchors.verticalCenter: parent.verticalCenter; width: root.width - 34 - vrrBtn.width - 12; wrapMode: Text.Wrap }
        LinkButton { id: vrrBtn; text: tr("restartNow"); onClicked: player.restartApp() }
    }

    Text { font.family: Theme.font; text: "Lossless Scaling"; color: Theme.label; font.pixelSize: 16; font.weight: Font.Medium; topPadding: 4 }
    Option { mode: 2; usable: root.lsUsable; title: tr("lsFixed"); detail: tr("lsFixedHint") }
    Option {
        mode: 3; usable: root.lsUsable; title: tr("ls3x")
        detail: tr("ls3xHint")
    }

    SubToggle {
        visible: root.lsUsable && (player.frameGenMode === 2 || player.frameGenMode === 3)
        title: tr("lsPerf"); detail: tr("lsPerfHint").replace("%1", player.lsfgFlowPercent)
        checked: player.lsfgPerformance; onToggled: (v) => player.lsfgPerformance = v
    }
    Text { font.family: Theme.font;   // VRR on in the desktop: lsfg-vk 2 cannot pace its frames
        visible: player.lsfgVrrWarning
        width: root.width; wrapMode: Text.Wrap; lineHeight: 1.15; font.pixelSize: 15; color: Theme.warn
        text: tr("lsVrrWarn").replace("%1", player.vrrScreens)
    }
    Text { font.family: Theme.font;   // more frames than the screen shows (30 fps x 3 on 60 Hz)
        visible: player.fgOverRefresh !== ""
        width: root.width; wrapMode: Text.Wrap; lineHeight: 1.15; font.pixelSize: 15; color: Theme.warn
        text: player.fgOverRefresh
    }

    // AMD FSR 3.1.6 frame generation inside VeyAmux (free, open source); can be chosen next to Lossless Scaling
    Text { font.family: Theme.font; text: "AMD FSR 3.1.6"; color: Theme.label; font.pixelSize: 16; font.weight: Font.Medium; topPadding: 4 }
    Option { mode: 5; title: tr("fsrFg"); detail: tr("fsrFgHint") }
    SubToggle {   // still HUD pixels (text, map, gauges) come from the real frame in generated frames
        visible: player.frameGenMode === 5
        title: tr("hudLock"); detail: tr("hudLockHint")
        checked: player.hudLock; onToggled: (v) => player.hudLock = v
    }

    Option { mode: 6; title: "NVIDIA VFG 2× 👑"; detail: tr("nvidiaVfgHint") }
    Text {
        visible: player.frameGenMode === 6
        x: 34; font.family: Theme.font; color: Theme.label; font.pixelSize: 16
        text: tr("vfgQuality")
    }
    Segmented {
        visible: player.frameGenMode === 6
        x: 34; width: parent.width - 34
        model: tr("vfgLevels"); current: player.nvidiaQuality
        onPicked: (level) => player.nvidiaQuality = level
    }
    SubToggle {
        visible: player.frameGenMode === 6
        title: tr("vfgRegulate"); detail: tr("vfgRegulateHint")
        checked: player.nvidiaAutoRegulate; onToggled: (enabled) => player.nvidiaAutoRegulate = enabled
    }
    Text {
        visible: player.frameGenMode === 6 && text.length > 0
        x: 34; width: parent.width - 34; wrapMode: Text.Wrap; font.family: Theme.font
        font.pixelSize: 15; color: Theme.muted; text: player.nvidiaRegulationStatus
    }
    Text {
        visible: player.frameGenMode === 6 && player.nvidiaRuntimeInstalled
        width: root.width; wrapMode: Text.Wrap; font.family: Theme.font; font.pixelSize: 15
        color: player.nvidiaVfgError.length ? Theme.warn : Theme.muted
        text: player.nvidiaVfgError || (player.nvidiaVfgActive ? tr("nvidiaVfgActive") : tr("nvidiaVfgWaiting"))
    }
    LinkButton {
        visible: player.frameGenMode === 6 && !player.nvidiaRuntimeInstalled && !player.nvidiaInstalling
        text: tr("installNvidiaVfg"); onClicked: player.installNvidiaRuntime()
    }
    Text {
        visible: player.frameGenMode === 6 && player.nvidiaInstallStatus.length > 0
        width: root.width; wrapMode: Text.Wrap; font.family: Theme.font; font.pixelSize: 15
        color: Theme.text; text: player.nvidiaInstallStatus
    }
    LinkButton {
        visible: player.frameGenMode === 6 && player.nvidiaRuntimeInstalled && player.nvidiaInstallStatus.length > 0
        text: tr("restartNow"); onClicked: player.restartApp()
    }

    Text { font.family: Theme.font;
        visible: player.fgPaused && (player.frameGenMode === 5 || player.frameGenMode === 6)   // lsfg-vk cannot pause (a notice says so)
        width: root.width; wrapMode: Text.Wrap; lineHeight: 1.15; font.pixelSize: 15; color: Theme.accent3
        text: tr("fgPaused")
    }

    // frame pacing repair: even out a locked game's uneven pictures before frame generation (capture and Remote Play)
    SubToggle {
        visible: player.frameGenMode !== 0
        title: tr("pacingRepair"); detail: tr("pacingRepairHint")
        checked: player.pacingRepair; onToggled: (v) => player.pacingRepair = v
    }

    // Lossless Scaling status / actions
    Rectangle {
        visible: player.frameGenMode !== 6
        width: root.width
        height: lsCol.implicitHeight + 16
        radius: 9 * Theme.round
        color: Theme.surfaceDeep
        border.color: root.ls === "crashed" || (!root.lsUsable) ? Theme.accentLine : Theme.border
        Column {
            id: lsCol
            x: 10; y: 8; width: parent.width - 20; spacing: 8
            Text { font.family: Theme.font;
                width: parent.width; wrapMode: Text.Wrap; lineHeight: 1.15; font.pixelSize: 15
                color: root.ls === "active" ? Theme.ok : (root.lsUsable ? Theme.text : Theme.warn)
                text: {
                    switch (root.ls) {
                    case "active": return tr("lsActive").replace("%1", player.lsfgFolder)
                    case "ready": return player.restartNeeded ? tr("lsRestart") : tr("lsReady").replace("%1", player.lsfgFolder)
                    case "noInstall": return tr("lsNoInstall")
                    case "noDll": return tr("lsNoDll")
                    case "noLayer": return tr("lsNoLayer")
                    case "oldLayer": return tr("lsOldLayer")
                    case "noVulkanUi": return tr("lsNoVulkan")
                    case "crashed": return tr("lsCrashed")
                    }
                    return ""
                }
            }
            Text { font.family: Theme.font;   // lsfg-vk 2 being installed / installed / failed
                width: parent.width; wrapMode: Text.Wrap; lineHeight: 1.15; font.pixelSize: 15
                visible: player.lsfgInstall !== ""
                color: player.lsfgInstall === "error" ? Theme.warn : (player.lsfgInstall === "done" ? Theme.ok : Theme.label)
                text: player.lsfgInstallText
            }
            Text { font.family: Theme.font;
                width: parent.width; wrapMode: Text.WrapAnywhere; font.pixelSize: 15
                visible: player.lsfgLog !== ""
                color: player.lsfgLog.indexOf("[ERROR]") >= 0 || player.lsfgLog.indexOf("[WARNING]") >= 0
                       || player.lsfgLog.indexOf("lsfg-vk-cli") >= 0 ? Theme.warn : Theme.label
                text: player.lsfgLog
            }
            Row {
                spacing: 8
                LinkButton { visible: player.restartNeeded || player.lsfgInstall === "done"; text: tr("restartNow"); onClicked: player.restartApp() }
                LinkButton { visible: (root.ls === "oldLayer" || root.ls === "noLayer") && player.lsfgInstall !== "busy" && player.lsfgInstall !== "done"
                             text: tr("installLsfg2"); onClicked: player.installLsfg() }
                LinkButton { visible: root.ls === "crashed"; text: tr("tryAgain"); onClicked: player.retryLsfg() }
                LinkButton { visible: root.ls === "noInstall" || root.ls === "noDll"; text: tr("chooseFolder"); onClicked: player.chooseLsfgFolder() }
                LinkButton { visible: root.ls !== "active" && root.ls !== "crashed"; text: tr("checkAgain"); onClicked: player.refreshLsfg() }
            }
        }
    }
}
