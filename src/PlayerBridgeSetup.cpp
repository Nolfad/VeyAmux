// VeyAmux for Linux: "Check my setup" and the performance report (after PushFPS 4.1, GPL-3.0-or-later).
//
// Check my setup looks at what is running and says what costs sharpness, smoothness or sound, each with the one
// change that answers it. Nothing changes until a fix is pressed, and every fix is a setting the menu can put back.
// The performance report is text to paste into a bug report: the graphics card, the settings that matter and what
// VeyAmux measured itself. Nothing personal: no file or folder names, no account, no address.
#include "PlayerBridge.h"
#include "CaptureSource.h"
#include "LosslessScaling.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QVariantMap>

#include <linux/videodev2.h>
#include <cmath>

namespace {
QVariantMap item(const QString& id, const QString& kind, const QString& title, const QString& detail, const QString& fix) {
    QVariantMap m;
    m["id"] = id;          // what applySetupFix() changes
    m["kind"] = kind;      // "warn" costs quality or smoothness, "info" a tip
    m["title"] = title;
    m["detail"] = detail;
    m["fix"] = fix;        // the button's text; empty: no button (a setting outside VeyAmux)
    return m;
}
bool compressed(uint32_t f) { return f == V4L2_PIX_FMT_MJPEG || f == V4L2_PIX_FMT_JPEG || f == V4L2_PIX_FMT_H264; }
}

QVariantList PlayerBridge::setupCheck() {
    QVariantList out;
    const bool live = engine_.isOpen() && engine_.isLive();
    const bool card = live && curMode_.fourcc != 0;
    const auto content = live ? engine_.liveContent() : veyamux::ContentProbe::Result{};

    // the capture card sends a compressed picture where it has an uncompressed one of the same size
    if (card && compressed(curMode_.fourcc)) {
        const auto modes = veyamux::listCaptureModes(curDevice_.toStdString());
        int best = -1;
        for (int i = 0; i < int(modes.size()); ++i) {
            const auto& o = modes[size_t(i)];
            if (compressed(o.fourcc) || o.w != curMode_.w || o.h != curMode_.h || o.fps + 0.5 < curMode_.fps) continue;
            if (best < 0) best = i;
        }
        if (best >= 0)
            out << item("format", "warn", L("The capture card sends a compressed picture", "Capture-kortet sender et komprimeret billede"),
                        L("It also has %1 uncompressed at the same size and rate: sharper, without compression blocks.",
                          "Det har også %1 ukomprimeret i samme størrelse og hastighed: skarpere, uden komprimeringsblokke.")
                            .arg(QString::fromStdString(modes[size_t(best)].label)),
                        L("Use uncompressed", "Brug ukomprimeret"));
    }
    // a capture card without its sound
    if (card && curAudio_.isEmpty()) {
        QString cardName;
        for (const auto& d : veyamux::listCaptureDevices()) if (QString::fromStdString(d.path) == curDevice_) cardName = QString::fromStdString(d.name);
        const QString guess = guessAudioSource(cardName);
        out << item("audio", "warn", L("No sound from the capture card", "Ingen lyd fra capture-kortet"),
                    guess.isEmpty() ? L("Choose the card's sound under Capture (C).", "Vælg kortets lyd under Capture (C).")
                                    : L("The card's sound input was found but is not used.", "Kortets lydindgang er fundet, men bruges ikke."),
                    guess.isEmpty() ? QString() : L("Use the card's sound", "Brug kortets lyd"));
    }
    // Lossless Scaling chosen, but its layer is not running yet
    if (s_.lsfgFg() && !lsfgActive_) {
        const QString st = lsfgState();
        if (st == "ready")
            out << item("lsfgRestart", "warn", L("Lossless Scaling is not running yet", "Lossless Scaling kører ikke endnu"),
                        L("lsfg-vk is loaded when VeyAmux starts.", "lsfg-vk indlæses, når VeyAmux starter."), L("Restart now", "Genstart nu"));
        else
            out << item("lsfgMissing", "warn", L("Lossless Scaling cannot start", "Lossless Scaling kan ikke starte"),
                        L("See Frame generation in the menu for what is missing.", "Se Frame generation i menuen for, hvad der mangler."), QString());
    }
    // VRR on the desktop with Lossless Scaling
    if (lsfgVrrWarning())
        out << item("vrr", "warn", L("VRR (Adaptive Sync) is on", "VRR (adaptiv synkronisering) er slået til"),
                    L("lsfg-vk 2 needs a fixed refresh rate to space its frames. Set Adaptive sync to Never for %1.",
                      "lsfg-vk 2 skal bruge en fast opdateringsfrekvens for at fordele billederne. Sæt Adaptiv synkronisering til Aldrig for %1.")
                        .arg(vrrScreens_.isEmpty() ? L("the screen", "skærmen") : vrrScreens_),
                    QStandardPaths::findExecutable("systemsettings").isEmpty() ? QString() : L("Open display settings", "Åbn skærmindstillinger"));
    // more frames than the screen shows
    if (!fgOverRefresh_.isEmpty())
        out << item("overRefresh", "warn", L("The screen cannot show all frames", "Skærmen kan ikke vise alle billeder"), fgOverRefresh_,
                    s_.frameGen == veyamux::FrameGenMode::LsfgFixed3 ? L("Use Fixed 2×", "Brug Fast 2×") : QString());
    // a locked 30 fps game with frame generation and no pacing repair: a tip, not a fault
    if (live && s_.frameGen != veyamux::FrameGenMode::Off && !s_.pacingRepair && content.contentFps > 24 && content.contentFps < 36)
        out << item("pacing", "info", L("A 30 fps game with frame generation", "Et 30 fps-spil med frame generation"),
                    L("If the motion judders now and then, Even out frame pacing moves every picture to an even rhythm (+1 refresh of delay).",
                      "Hvis bevægelsen hakker indimellem, flytter Jævn billedrytme hvert billede til en jævn rytme (+1 opdatering forsinkelse)."),
                    L("Turn it on", "Slå den til"));
    // upscaling a picture that is already as large as the window: work without a sharper result
    if (live && s_.enabled && s_.scale > 1.001f && !s_.retroTv && !s_.pixelActive() && window_) {
        const qreal dpr = window_->devicePixelRatio();
        const int ww = int(window_->width() * dpr), wh = int(window_->height() * dpr);
        const auto& i = engine_.info();
        if (i.width >= ww && i.height >= wh)
            out << item("scale", "warn", L("Upscaling a picture larger than the window", "Opskalering af et billede, der er større end vinduet"),
                        L("The signal (%1×%2) already covers the window (%3×%4): %5× upscaling only costs graphics card time.",
                          "Signalet (%1×%2) dækker allerede vinduet (%3×%4): %5× opskalering koster kun grafikkort-tid.")
                            .arg(i.width).arg(i.height).arg(ww).arg(wh).arg(double(s_.scale)),
                        L("Use Native", "Brug Native"));
    }
    // the graphics card needs longer per picture than the game gives it
    if (live && liveVulkanActive()) {
        const double gpu = liveVk_->gpuMs();
        const double fps = content.contentFps > 1 ? content.contentFps : engine_.info().fps;
        if (gpu > 0 && fps > 1 && gpu > 0.8 * 1000.0 / fps)
            out << item("gpuTime", "warn", L("The enhancement is too heavy for the graphics card", "Forbedringen er for tung for grafikkortet"),
                        L("%1 ms per picture, but a new one comes every %2 ms. Lighter noise reduction or upscaling helps.",
                          "%1 ms pr. billede, men der kommer et nyt hvert %2 ms. Lettere støjfjernelse eller opskalering hjælper.")
                            .arg(gpu, 0, 'f', 1).arg(1000.0 / fps, 0, 'f', 1),
                        s_.denoise != veyamux::Denoise::Off ? L("Turn noise reduction off", "Slå støjfjernelse fra") : QString());
    }
    if (!live)
        out << item("idle", "info", L("Nothing is playing", "Der afspilles ikke noget"),
                    L("Start the capture card or PlayStation Remote Play, then check again.", "Start capture-kortet eller PlayStation Remote Play, og tjek igen."), QString());
    else if (out.isEmpty())
        out << item("ok", "ok", L("Everything looks right", "Alt ser rigtigt ud"),
                    L("Nothing found that costs sharpness, smoothness or sound.", "Intet fundet, der koster skarphed, flydende billede eller lyd."), QString());
    return out;
}

void PlayerBridge::applySetupFix(const QString& id) {
    if (id == "format") {
        const auto modes = veyamux::listCaptureModes(curDevice_.toStdString());
        for (const auto& o : modes)
            if (!compressed(o.fourcc) && o.w == curMode_.w && o.h == curMode_.h && o.fps + 0.5 >= curMode_.fps) {
                lastModes_ = modes;
                startCapture(curDevice_, o, curAudio_);
                break;
            }
    } else if (id == "audio") {
        QString cardName;
        for (const auto& d : veyamux::listCaptureDevices()) if (QString::fromStdString(d.path) == curDevice_) cardName = QString::fromStdString(d.name);
        const QString guess = guessAudioSource(cardName);
        if (!guess.isEmpty()) startCapture(curDevice_, curMode_, guess);
    } else if (id == "lsfgRestart") {
        restartWithSource();
    } else if (id == "vrr") {
        QProcess::startDetached("systemsettings", {"kcm_kscreen"});
    } else if (id == "overRefresh") {
        setFrameGenMode(int(veyamux::FrameGenMode::LsfgFixed));
    } else if (id == "pacing") {
        setPacingRepair(true);
    } else if (id == "scale") {
        setScale(1.0);
    } else if (id == "gpuTime") {
        setDenoise(0);
    }
    showNotice(L("Changed. Check again to see the result", "Ændret. Tjek igen for at se resultatet"));
}

// Text for a bug report: what VeyAmux ran on, with which settings, and what it measured.
QString PlayerBridge::perfReport() {
    QStringList r;
    r << L("VeyAmux %1 performance report").arg(QCoreApplication::applicationVersion());
    r << L("System: ") + QSysInfo::prettyProductName() + " · kernel " + QSysInfo::kernelVersion() + " · " +
             qEnvironmentVariable("XDG_CURRENT_DESKTOP", "?") + " (" + qEnvironmentVariable("XDG_SESSION_TYPE", "?") + ")";
    r << L("Graphics card: ") + (gpuDevice_.isEmpty() ? gpuName_ : gpuDevice_);
    const QScreen* sc = window_ && window_->screen() ? window_->screen() : QGuiApplication::primaryScreen();
    if (sc) r << L("Screen: %1x%2 @ %3 Hz, VRR %4%5").arg(int(sc->size().width() * sc->devicePixelRatio()))
                     .arg(int(sc->size().height() * sc->devicePixelRatio())).arg(sc->refreshRate(), 0, 'f', 2)
                     .arg(desktopVrr_ ? L("on") : L("off")).arg(window_ && window_->visibility() == QWindow::FullScreen ? L(", fullscreen") : "");
    const bool live = engine_.isOpen() && engine_.isLive();
    if (engine_.isOpen()) {
        const auto& i = engine_.info();
        QString src = live ? (curMode_.fourcc ? L("capture card") : demoActive_ ? L("demo") : L("live")) : L("video file");
        r << L("Source: %1, %2x%3 %4 @ %5 fps").arg(src).arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec)).arg(i.fps, 0, 'f', 2);
        if (live) {
            const auto c = engine_.liveContent();
            r << L("Measured: game %1 fps%2, estimated %3")
                     .arg(c.contentFps, 0, 'f', 1).arg(c.staticPicture ? L(" (still picture)") : "")
                     .arg(c.estW > 0 ? QString("%1x%2").arg(c.estW).arg(c.estH) : L("unknown"));
        }
    } else {
        r << L("Source: none");
    }
    static const char* ups[] = {"SGSR", "FSR 3.1", "FSR 1", "NIS", "DLSS SR"};
    const QString nr[] = {L("off"), L("spatial"), L("temporal"), L("both")};
    r << L("Picture: enhance %1, %2 %3x, noise reduction %4/%5, AA %6, anti-flicker %7, sharpen %8, texture %9 clarity %10 dehaze %11")
             .arg(s_.enabled ? L("on") : L("off")).arg(ups[std::clamp(int(s_.upscaler), 0, 4)]).arg(double(s_.scale))
             .arg(nr[std::clamp(int(s_.denoise), 0, 3)]).arg(s_.denoiseStrength)
             .arg(s_.fxaa ? (s_.smaa ? "SMAA" : "FXAA") + QString(" q%1").arg(s_.fxaaQuality) : L("off"))
             .arg(s_.deflicker ? L("on") : L("off")).arg(double(s_.sharpen), 0, 'f', 2)
             .arg(double(s_.texture), 0, 'f', 2).arg(double(s_.clarity), 0, 'f', 2).arg(double(s_.dehaze), 0, 'f', 2);
    QString fg = L("off");
    if (s_.lsfgFg()) fg = QString("Lossless Scaling %1x (lsfg-vk %2, %3)").arg(s_.frameGen == veyamux::FrameGenMode::LsfgFixed3 ? 3 : 2)
                              .arg(lsfg_.layerV2 ? "2" : lsfg_.layerManifest.isEmpty() ? "missing" : "1").arg(translateMessage(lsfgState()));
    else if (s_.frameGen == veyamux::FrameGenMode::FsrFree) fg = "AMD FSR 3.1.6 2x";
    else if (s_.frameGen == veyamux::FrameGenMode::NvidiaVfg)
        fg = nvidiaVfgActive() ? L("NVIDIA VFG 2x (active)") : L("NVIDIA VFG 2x (inactive) ") + nvidiaVfgError();
    if (!upError_.isEmpty()) r << L("Upscaler status: ") + upError_;
    if (s_.vrr) fg += L(", VRR mode");
    r << L("Frame generation: ") + fg + L(", pacing repair %1, HUD lock %2").arg(s_.pacingRepair ? L("on") : L("off")).arg(s_.hudLock ? L("on") : L("off"));
    if (s_.frameGen == veyamux::FrameGenMode::NvidiaVfg && liveVulkanActive()) {
        const auto n = liveVk_->nvidiaFrameStats();
        const auto& m = n.models;
        r << L("VFG quality: %1. Automatic regulation: %2.")
                .arg(nvidiaQualityName(nvidiaQuality()), nvidiaAutoRegulate() ? L("On") : L("Off"));
        r << L("Faster model ready: %1. Pairs using primary / faster: %2 / %3.")
                .arg(m.fastReady ? L("Yes") : L("No")).arg(m.primaryPairs).arg(m.fastPairs);
        if (!m.fastStatus.empty()) r << translateMessage(QString::fromStdString(m.fastStatus));
        if (m.primaryMs > 0 || m.fastMs > 0)
            r << L("VFG model timing: primary %1 ms, faster %2 ms.")
                    .arg(m.primaryMs, 0, 'f', 2).arg(m.fastMs, 0, 'f', 2);
        r << L("NVIDIA VFG output (application): %1 real + %2 generated = %3 fps; %4 real / %5 generated frames this session")
                 .arg(n.realFps, 0, 'f', 1).arg(n.generatedFps, 0, 'f', 1)
                 .arg(n.realFps + n.generatedFps, 0, 'f', 1).arg(n.real).arg(n.generated);
        if (n.generated)
            r << L("NVIDIA VFG exchange: device-local Vulkan/CUDA memory. Render-thread processing: %1 ms per input (smoothed)")
                     .arg(n.processingMs, 0, 'f', 2);
    }
    r << L("HDR: capture peak ") + QString::number(int(s_.hdrPeak)) + L(" nits, output ") + (hdrOutputActive() ? L("on") : L("off"));
    if (liveVulkanActive()) {
        r << L("Delay in VeyAmux: %1 ms, GPU %2 ms per picture").arg(liveVk_->pipelineMs(), 0, 'f', 1).arg(liveVk_->gpuMs(), 0, 'f', 2);
        const auto st = liveVk_->gpuStageMs();
        QStringList parts;
        static const char* names[] = {"input", "anti-flicker/denoise", "AA", "upscale", "finish", "output"};
        for (size_t k = 0; k < st.size() && k < 6; ++k) if (st[k] > 0) parts << QString("%1 %2").arg(L(names[k])).arg(st[k], 0, 'f', 2);
        if (!parts.isEmpty()) r << L("GPU stages (ms): ") + parts.join(", ");
    }
    updateOverlay();
    if (!overlayStats_.isEmpty()) r << L("On screen: ") + QString(overlayStats_).replace('\n', " · ");
    r << L("(VeyAmux's own software timing, not controller-to-screen.)");
    return r.join('\n');
}

void PlayerBridge::copyPerfReport() {
    QGuiApplication::clipboard()->setText(perfReport());
    showNotice(L("Performance report copied", "Ydelsesrapport kopieret"));
}
