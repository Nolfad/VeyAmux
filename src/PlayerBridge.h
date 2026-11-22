// QML-facing wrapper around veyamux::MediaEngine.
#pragma once
#include "AppUpdate.h"
#include <algorithm>
#include <cmath>
#include <QElapsedTimer>
#include <QSettings>
#include <mutex>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>
#include <QStandardPaths>
#include "LiveVulkan.h"
#include "LosslessScaling.h"
#include "MediaEngine.h"
#include "ObsStream.h"
#include "Recorder.h"
#include "AudioMeter.h"
#include "ScreenCast.h"
#include "ScreenCastPortal.h"
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QPointer>
#include <QProcess>
#include <QWindow>
#include <memory>

class PlayerBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY mediaChanged)
    // the demo scene (DemoSource.h): available in this build, running as the source, and how far its world is built
    Q_PROPERTY(bool demoAvailable READ demoAvailable CONSTANT)
    Q_PROPERTY(bool demoActive READ demoActive NOTIFY mediaChanged)
    Q_PROPERTY(double demoProgress READ demoProgress NOTIFY demoChanged)
    Q_PROPERTY(bool live READ live NOTIFY mediaChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY mediaChanged)
    Q_PROPERTY(QString title READ title NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaInfo READ mediaInfo NOTIFY mediaChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY audioChanged)
    // Audio offset in ms (positive = sound later); capture can only delay the sound
    Q_PROPERTY(int audioDelay READ audioDelay WRITE setAudioDelay NOTIFY audioChanged)
    // Output device (sink name, "" = system default) and loudness levelling
    Q_PROPERTY(QString audioOutput READ audioOutput WRITE setAudioOutput NOTIFY audioChanged)
    Q_PROPERTY(bool normalize READ normalize WRITE setNormalize NOTIFY audioChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY audioChanged)
    // spectrum of the source's sound (before volume and mute): 24 bands 0..1, ~30 updates per second
    Q_PROPERTY(QVariantList audioBands READ audioBands NOTIFY audioMeterChanged)
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY audioMeterChanged)
    // the menu (with the spectrum) is open: only then is the meter sent to the UI, 30 times a second
    Q_PROPERTY(bool meterWanted READ meterWanted WRITE setMeterWanted)
    Q_PROPERTY(QString gpuName READ gpuName NOTIFY statsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString perf READ perf NOTIFY statsChanged)
    Q_PROPERTY(double videoAspect READ videoAspect NOTIFY frameReady)
    // enhancement
    Q_PROPERTY(bool enhance READ enhance WRITE setEnhance NOTIFY enhanceChanged)
    Q_PROPERTY(double scale READ scale WRITE setScale NOTIFY enhanceChanged)
    Q_PROPERTY(int upscaler READ upscaler WRITE setUpscaler NOTIFY enhanceChanged)
    Q_PROPERTY(bool nvidiaRuntimeInstalled READ nvidiaRuntimeInstalled NOTIFY nvidiaInstallChanged)
    Q_PROPERTY(bool nvidiaInstalling READ nvidiaInstalling NOTIFY nvidiaInstallChanged)
    Q_PROPERTY(QString nvidiaInstallStatus READ nvidiaInstallStatus NOTIFY nvidiaInstallChanged)
    Q_PROPERTY(bool nvidiaVfgActive READ nvidiaVfgActive NOTIFY statsChanged)
    Q_PROPERTY(int nvidiaQuality READ nvidiaQuality WRITE setNvidiaQuality NOTIFY enhanceChanged)
    Q_PROPERTY(bool nvidiaAutoRegulate READ nvidiaAutoRegulate WRITE setNvidiaAutoRegulate NOTIFY enhanceChanged)
    Q_PROPERTY(QString nvidiaRegulationStatus READ nvidiaRegulationStatus NOTIFY statsChanged)
    Q_PROPERTY(QString nvidiaVfgError READ nvidiaVfgError NOTIFY statsChanged)
    Q_PROPERTY(QString upscalerError READ upscalerError NOTIFY statsChanged)
    // Frame generation: 0 off, 2 Lossless Scaling fixed 2x (default), 3 Lossless Scaling fixed 3x
    Q_PROPERTY(int frameGenMode READ frameGenMode WRITE setFrameGenMode NOTIFY enhanceChanged)
    Q_PROPERTY(bool lsfgPerformance READ lsfgPerformance WRITE setLsfgPerformance NOTIFY lsfgChanged)
    Q_PROPERTY(int lsfgFlowPercent READ lsfgFlowPercent NOTIFY lsfgChanged)
    // OBS browser source (local MJPEG stream + saved HTML file)
    Q_PROPERTY(bool obsEnabled READ obsEnabled WRITE setObsEnabled NOTIFY obsChanged)
    Q_PROPERTY(int obsHeight READ obsHeight WRITE setObsHeight NOTIFY obsChanged)
    Q_PROPERTY(int obsViewers READ obsViewers NOTIFY obsChanged)
    Q_PROPERTY(QString obsUrl READ obsUrl NOTIFY obsChanged)
    Q_PROPERTY(QString obsFile READ obsFile NOTIFY obsChanged)
    Q_PROPERTY(QString obsError READ obsError NOTIFY obsChanged)
    // Lossless Scaling (lsfg-vk): state code for the UI, see lsfgState()
    Q_PROPERTY(QString lsfgState READ lsfgState NOTIFY lsfgChanged)
    Q_PROPERTY(QString lsfgFolder READ lsfgFolder NOTIFY lsfgChanged)
    Q_PROPERTY(QString lsfgLog READ lsfgLog NOTIFY lsfgChanged)
    Q_PROPERTY(bool lsfgActive READ lsfgActive NOTIFY lsfgChanged)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY lsfgChanged)
    Q_PROPERTY(int lsfgMultiplier READ lsfgMultiplier NOTIFY lsfgChanged)
    // lsfg-vk 2 install (when only lsfg-vk 1 is found): "" / "busy" / "done" / "error", and the text to show
    Q_PROPERTY(QString lsfgInstall READ lsfgInstall NOTIFY lsfgChanged)
    Q_PROPERTY(QString lsfgInstallText READ lsfgInstallText NOTIFY lsfgChanged)
    // frame generation paused automatically while the capture runs at ~120 Hz (the game is already fast)
    Q_PROPERTY(bool fgPaused READ fgPaused NOTIFY lsfgChanged)
    Q_PROPERTY(double displayHz READ displayHz NOTIFY lsfgChanged)
    Q_PROPERTY(bool fxaa READ fxaa WRITE setFxaa NOTIFY enhanceChanged)
    Q_PROPERTY(bool deflicker READ deflicker WRITE setDeflicker NOTIFY enhanceChanged)
    Q_PROPERTY(bool retroTv READ retroTv WRITE setRetroTv NOTIFY enhanceChanged)
    // UI theme: vice / code / inferno / retro (Theme.qml), saved as ui/theme
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    // UI language: languageChoice is auto or a code (saved as ui/lang); language is the one in use
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QString languageChoice READ languageChoice WRITE setLanguageChoice NOTIFY languageChanged)
    Q_PROPERTY(int retroLines READ retroLines WRITE setRetroLines NOTIFY enhanceChanged)
    // pixel-perfect scaling (EnhanceSettings::pixelPerfect); pixelDisplay = the display enlarges with sharp pixels now
    Q_PROPERTY(bool pixelPerfect READ pixelPerfect WRITE setPixelPerfect NOTIFY enhanceChanged)
    Q_PROPERTY(int pixelGrid READ pixelGrid WRITE setPixelGrid NOTIFY enhanceChanged)
    Q_PROPERTY(QString pixelGridSize READ pixelGridSize NOTIFY enhanceChanged)
    // Denoise: 0 off, 1 spatial (NLM), 2 temporal, 3 both; strength 0 light, 1 medium, 2 strong
    Q_PROPERTY(int denoise READ denoise WRITE setDenoise NOTIFY enhanceChanged)
    // HDR capture (P010): brightest level of the source in nits (tone-mapped to SDR)
    Q_PROPERTY(int hdrPeak READ hdrPeak WRITE setHdrPeak NOTIFY enhanceChanged)
    // VRR mode: the game's own pictures at an even pace (frame generation off); vsync changes need a restart
    // HDR display output (experimental): requested in the settings (takes effect at the next start) / actually running
    Q_PROPERTY(bool hdrOutput READ hdrOutput WRITE setHdrOutput NOTIFY enhanceChanged)
    Q_PROPERTY(bool hdrOutputActive READ hdrOutputActive NOTIFY enhanceChanged)
    Q_PROPERTY(bool hdrOutputRestart READ hdrOutputRestart NOTIFY enhanceChanged)
    Q_PROPERTY(int displayPeak READ displayPeak WRITE setDisplayPeak NOTIFY enhanceChanged)
    Q_PROPERTY(int displayPeakEdid READ displayPeakEdid NOTIFY enhanceChanged)
    Q_PROPERTY(bool hdrOutputXwayland READ hdrOutputXwayland NOTIFY enhanceChanged)
    Q_PROPERTY(bool hdrOutputNoVulkan READ hdrOutputNoVulkan NOTIFY enhanceChanged)
    // 0 = HDR10 (PQ), 1 = scRGB: the swapchain format chosen in the settings / the one this run uses
    Q_PROPERTY(int hdrMode READ hdrMode WRITE setHdrMode NOTIFY enhanceChanged)
    Q_PROPERTY(int hdrModeActive READ hdrModeActive NOTIFY enhanceChanged)
    // start inside gamescope (HDR + VRR through gamescope's XWayland); takes effect at the next start
    Q_PROPERTY(bool gamescope READ gamescope WRITE setGamescope NOTIFY enhanceChanged)
    Q_PROPERTY(bool gamescopeAvailable READ gamescopeAvailable CONSTANT)
    Q_PROPERTY(bool inGamescope READ inGamescope CONSTANT)
    Q_PROPERTY(bool gamescopeRestart READ gamescopeRestart NOTIFY enhanceChanged)
    Q_PROPERTY(bool vrr READ vrr WRITE setVrr NOTIFY enhanceChanged)
    // frame pacing repair for frame generation (MediaEngine / FramePacingRepair.h)
    Q_PROPERTY(bool pacingRepair READ pacingRepair WRITE setPacingRepair NOTIFY enhanceChanged)
    Q_PROPERTY(bool hudLock READ hudLock WRITE setHudLock NOTIFY enhanceChanged)
    // the desktop has VRR on for a screen while Lossless Scaling is chosen: lsfg-vk 2 cannot pace its frames then
    Q_PROPERTY(bool lsfgVrrWarning READ lsfgVrrWarning NOTIFY lsfgChanged)
    // frame generation asks for more pictures than the screen shows (30 fps x 3 on a 60 Hz screen): its text, or empty
    Q_PROPERTY(QString fgOverRefresh READ fgOverRefresh NOTIFY lsfgChanged)
    Q_PROPERTY(QString vrrScreens READ vrrScreens NOTIFY lsfgChanged)
    Q_PROPERTY(bool vrrRestartNeeded READ vrrRestartNeeded NOTIFY enhanceChanged)
    Q_PROPERTY(int denoiseStrength READ denoiseStrength WRITE setDenoiseStrength NOTIFY enhanceChanged)
    Q_PROPERTY(int fxaaQuality READ fxaaQuality WRITE setFxaaQuality NOTIFY enhanceChanged)
    // Named settings profiles (picture settings + audio delay); a profile can be bound to a capture signal and is then
    // applied automatically when that signal is opened. Keys 1-9 load profile 1-9.
    Q_PROPERTY(QStringList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)   // short confirmation (profile loaded, clip saved)
    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY profilesChanged)
    Q_PROPERTY(QString signalKey READ signalKey NOTIFY mediaChanged)
    // Instant replay / recording / screenshots
    Q_PROPERTY(bool replay READ replay WRITE setReplay NOTIFY recChanged)
    Q_PROPERTY(int replaySeconds READ replaySeconds WRITE setReplaySeconds NOTIFY recChanged)
    Q_PROPERTY(int recHeight READ recHeight WRITE setRecHeight NOTIFY recChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recChanged)
    Q_PROPERTY(QString recInfo READ recInfo NOTIFY recTick)
    // before/after comparison clips: layout 1 split, 2 side by side, 3 vertical (9:16), 4 reveal; length in seconds
    Q_PROPERTY(int compareLayout READ compareLayout WRITE setCompareLayout NOTIFY recChanged)
    Q_PROPERTY(int compareSeconds READ compareSeconds WRITE setCompareSeconds NOTIFY recChanged)
    Q_PROPERTY(bool comparing READ comparing NOTIFY recChanged)
    Q_PROPERTY(int markers READ markers NOTIFY recTick)
    // updates from inside the app (AppUpdate.h): "", checking, uptodate, available, downloading, ready, error
    Q_PROPERTY(QString updateState READ updateState NOTIFY updateChanged)
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
    Q_PROPERTY(QString updateText READ updateText NOTIFY updateChanged)
    Q_PROPERTY(QString updateNotes READ updateNotes NOTIFY updateChanged)
    Q_PROPERTY(bool updateSelf READ updateSelf CONSTANT)   // runs as an AppImage that can be replaced
    Q_PROPERTY(bool autoUpdateCheck READ autoUpdateCheck WRITE setAutoUpdateCheck NOTIFY updateChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)   // clip markers in the recording that runs
    // Live stream (RTMP): 0 = Twitch, 1 = YouTube, 2 = own server URL; the key is kept in the settings file
    Q_PROPERTY(bool streaming READ streaming NOTIFY recChanged)
    // Frame-time overlay (F3): intervals between pictures on screen (ms, newest last) and a summary line
    Q_PROPERTY(bool overlay READ overlay WRITE setOverlay NOTIFY overlayChanged)
    Q_PROPERTY(QVariantList overlayGraph READ overlayGraph NOTIFY overlayTick)
    Q_PROPERTY(QString overlayStats READ overlayStats NOTIFY overlayTick)
    Q_PROPERTY(double overlayTarget READ overlayTarget NOTIFY overlayTick)
    Q_PROPERTY(int streamService READ streamService WRITE setStreamService NOTIFY recChanged)
    Q_PROPERTY(QString streamKey READ streamKey WRITE setStreamKey NOTIFY recChanged)
    Q_PROPERTY(QString streamServer READ streamServer WRITE setStreamServer NOTIFY recChanged)
    Q_PROPERTY(int streamKbps READ streamKbps WRITE setStreamKbps NOTIFY recChanged)
    Q_PROPERTY(double sharpen READ sharpen WRITE setSharpen NOTIFY enhanceChanged)
    Q_PROPERTY(double exposure READ exposure WRITE setExposure NOTIFY enhanceChanged)
    Q_PROPERTY(double contrast READ contrast WRITE setContrast NOTIFY enhanceChanged)
    Q_PROPERTY(double saturation READ saturation WRITE setSaturation NOTIFY enhanceChanged)
    Q_PROPERTY(bool smaa READ smaa WRITE setSmaa NOTIFY enhanceChanged)            // anti-aliasing: SMAA instead of FXAA
    Q_PROPERTY(double texture READ texture WRITE setTexture NOTIFY enhanceChanged)   // colour effects, -1..1
    Q_PROPERTY(double clarity READ clarity WRITE setClarity NOTIFY enhanceChanged)
    Q_PROPERTY(double dehaze READ dehaze WRITE setDehaze NOTIFY enhanceChanged)
    // PlayStation Remote Play (RemotePlay.h): consoles found on the network merged with the registered ones,
    // [{id, name, host, ps5, ready, registered, app, passcode}]
    Q_PROPERTY(QVariantList psConsoles READ psConsoles NOTIFY psChanged)
    Q_PROPERTY(bool psHasAccount READ psHasAccount NOTIFY psChanged)
    Q_PROPERTY(QString psBusy READ psBusy NOTIFY psChanged)          // what is running ("" = nothing)
    Q_PROPERTY(QString psMessage READ psMessage NOTIFY psChanged)    // the last error / result
    Q_PROPERTY(bool psMessageError READ psMessageError NOTIFY psChanged)
    Q_PROPERTY(int psHeight READ psHeight WRITE setPsHeight NOTIFY psChanged)   // 720 / 1080
    Q_PROPERTY(int psFps READ psFps WRITE setPsFps NOTIFY psChanged)            // 30 / 60
    Q_PROPERTY(bool psHdr READ psHdr WRITE setPsHdr NOTIFY psChanged)           // PS5: HDR picture
    Q_PROPERTY(bool psActive READ psActive NOTIFY mediaChanged)
public:
    explicit PlayerBridge(const QString& shaderDir, QObject* parent = nullptr);
    ~PlayerBridge() override;

    bool hasMedia() const { return engine_.isOpen(); }
    bool demoAvailable() const;
    bool demoActive() const { return demoActive_ && engine_.isOpen(); }
    double demoProgress() const { return demoProgress_; }
    bool live() const { return engine_.isLive(); }
    bool playing() const { return engine_.playing(); }
    double position() const { return engine_.isOpen() ? engine_.position() : 0.0; }
    double duration() const { return engine_.info().duration; }
    QString title() const {
        const QString t = QString::fromStdString(engine_.info().title);
        return engine_.info().videoCodec == "PipeWire" ? L("Screen share: window / screen", "Skærmdeling: vindue / skærm") : t;
    }
    QString mediaInfo() const;
    QString outputFpsText(double srcFps) const;
    double frameGenBaseFps() const;
    double volume() const { return volume_; }
    bool muted() const { return muted_; }
    QString gpuName() const { return gpuName_; }
    QString status() const { return status_; }
    QString perf() const { return perf_; }
    double videoAspect() const { return aspect_; }
    bool enhance() const { return s_.enabled; }
    double scale() const { return s_.scale; }
    int upscaler() const { return int(s_.upscaler); }
    bool nvidiaRuntimeInstalled() const;
    bool nvidiaInstalling() const { return nvidiaInstaller_ != nullptr; }
    QString nvidiaInstallStatus() const { return translateMessage(nvidiaInstallStatus_); }
    int nvidiaQuality() const { return s_.nvidiaQuality; }
    bool nvidiaAutoRegulate() const { return s_.nvidiaAutoRegulate; }
    void setNvidiaQuality(int level) { s_.nvidiaQuality = qBound(0, level, 2); QSettings().setValue("enh/nvidiaQuality", s_.nvidiaQuality); applyEnhance(); }
    void setNvidiaAutoRegulate(bool value) { s_.nvidiaAutoRegulate = value; QSettings().setValue("enh/nvidiaAutoRegulate", value); applyEnhance(); }
    QString nvidiaRegulationStatus() const;
    QString nvidiaQualityName(int level) const;
    Q_INVOKABLE void installNvidiaRuntime();
    bool nvidiaVfgActive() const { return s_.frameGen == veyamux::FrameGenMode::NvidiaVfg && liveVulkanActive() && liveVk_->nvidiaVfgActive(); }
    QString nvidiaVfgError() const {
        if (!engine_.isOpen() || s_.frameGen != veyamux::FrameGenMode::NvidiaVfg) return {};
        if (!liveVulkanActive()) return L("NVIDIA VFG requires live capture with the Vulkan renderer.");
        return translateMessage(liveVk_->nvidiaVfgError());
    }
    QString upscalerError() const { return upError_; }
    bool fxaa() const { return s_.fxaa; }
    bool deflicker() const { return s_.deflicker; }
    bool retroTv() const { return s_.retroTv; }
    QString theme() const { return theme_; }
    QString language() const { return lang_; }
    QString languageChoice() const { return langChoice_; }
    void setLanguageChoice(const QString& c);
    static QString systemLanguage();
    void setTheme(const QString& t);
    int retroLines() const { return s_.retroLines; }
    bool pixelPerfect() const { return s_.pixelPerfect; }
    int pixelGrid() const { return s_.pixelGrid; }
    bool pixelDisplay() const { return s_.pixelActive(); }
    QString pixelGridSize() const { return s_.gridW > 0 ? QStringLiteral("%1×%2").arg(s_.gridW).arg(s_.gridH) : QString(); }
    int denoise() const { return int(s_.denoise); }
    int hdrPeak() const { return int(std::lround(s_.hdrPeak)); }
    void setHdrPeak(int v) { s_.hdrPeak = float(std::clamp(v, 203, 10000)); QSettings().setValue("enh/hdrPeak", hdrPeak()); applyEnhance(); }
    bool vrr() const { return s_.vrr; }
    bool pacingRepair() const { return s_.pacingRepair; }
    bool hudLock() const { return s_.hudLock; }
    void setHudLock(bool v) { s_.hudLock = v; QSettings().setValue("enh/hudLock", v); applyEnhance(); }
    void setPacingRepair(bool v) { s_.pacingRepair = v; QSettings().setValue("enh/pacingRepair", v); applyEnhance(); }
    bool lsfgVrrWarning() const { return desktopVrr_ && s_.lsfgFg(); }
    QString vrrScreens() const { return vrrScreens_; }
    Q_INVOKABLE void checkDesktopVrr();
    bool desktopVrr_ = false, vrrNoticed_ = false;
    QString vrrScreens_;
    QElapsedTimer pacingClock_;
    double pacingShown_ = -1;
    void setVrr(bool v) { s_.vrr = v; QSettings().setValue("enh/vrr", v); applyEnhance(); }
    // vsync is chosen at start: VRR mode with frame generation off wants it on
    bool vrrRestartNeeded() const { return (s_.vrr && s_.frameGen == veyamux::FrameGenMode::Off) != vrrAtStart_; }
    void setVrrAtStart(bool v) { vrrAtStart_ = v; }
    bool hdrOutput() const { return hdrOutWanted_; }
    void setHdrOutput(bool v) { hdrOutWanted_ = v; QSettings().setValue("enh/hdrOut", v); s_.hdrOut = v && hdrOutActive_; applyEnhance(); }
    bool hdrOutputActive() const { return s_.hdrOut; }
    bool hdrOutputRestart() const { return hdrOutWanted_ != hdrOutAtStart_ || (hdrOutWanted_ && hdrMode_ != hdrModeAtStart_); }
    int hdrMode() const { return hdrMode_; }
    void setHdrMode(int m) { hdrMode_ = m == 1 ? 1 : 0; QSettings().setValue("enh/hdrMode", hdrMode_); emit enhanceChanged(); }
    int hdrModeActive() const { return hdrModeAtStart_; }
    bool gamescope() const { return QSettings().value("ui/gamescope", false).toBool(); }
    void setGamescope(bool v) { QSettings().setValue("ui/gamescope", v); emit enhanceChanged(); }
    bool gamescopeRestart() const { return gamescope() != gamescopeAtStart_; }
    bool gamescopeAtStart_ = QSettings().value("ui/gamescope", false).toBool();
    bool gamescopeAvailable() const { return qEnvironmentVariable("VEYAMUX_GAMESCOPE") == "1" && !QStandardPaths::findExecutable("gamescope").isEmpty(); }
    bool inGamescope() const { return qEnvironmentVariableIsSet("GAMESCOPE_WAYLAND_DISPLAY") || qEnvironmentVariableIsSet("VEYAMUX_IN_GAMESCOPE"); }
    void setHdrModeAtStart(int m) { hdrModeAtStart_ = m; emit enhanceChanged(); }
    // 0 = automatic (the display's own maximum from its EDID, 1000 nits if it does not say)
    int displayPeak() const { return peakAuto_ ? 0 : int(std::lround(s_.displayPeak)); }
    void setDisplayPeak(int v);
    int displayPeakEdid() const { return edidPeak_; }   // 0 = unknown
    // EDID HDR static metadata: the display's maximum luminance in nits for a DRM connector ("DP-1"), 0 if not given
    static int edidPeakNits(const QString& connector);
    static int edidPeakFromBytes(const QByteArray& edid);
    // main(): what this run asked Qt for, and whether the window really got an HDR (scRGB) swapchain
    void setHdrOutputState(bool requested, bool active) {
        hdrOutAtStart_ = requested; hdrOutActive_ = requested && active; s_.hdrOut = hdrOutWanted_ && hdrOutActive_; applyEnhance();
    }
    // HDR output mode: the finished picture is PQ-coded; this turns a copy of it back into normal SDR (OBS, files)
    QImage sdrCopy(QImage img) const;
    bool hdrOutputXwayland() const { return hdrOutXwayland_; }
    bool hdrOutputNoVulkan() const { return hdrOutNoVulkan_; }
    void setHdrOutputNoVulkan(bool v) { hdrOutNoVulkan_ = v; emit enhanceChanged(); }
    void setHdrOutputXwayland(bool v) { hdrOutXwayland_ = v; emit enhanceChanged(); }
    void setDenoise(int v) { s_.denoise = veyamux::Denoise(std::clamp(v, 0, 3)); applyEnhance(); }
    int denoiseStrength() const { return s_.denoiseStrength; }
    void setDenoiseStrength(int v) { s_.denoiseStrength = std::clamp(v, 0, 2); applyEnhance(); }
    int frameGenMode() const { return int(s_.frameGen); }
    bool lsfgPerformance() const { return lsfgPerformance_; }
    void setLsfgPerformance(bool v);
    int lsfgFlowPercent() const { return int(std::lround(lsfgFlowScale() * 100)); }
    double lsfgFlowScale() const;
    // "active", "ready" (restart to enable), "noInstall", "noDll" (only Lossless.dll: needs the lsfg-vk Steam branch),
    // "noLayer" (lsfg-vk not installed), "oldLayer" (lsfg-vk 1.x), "noVulkanUi", "crashed"
    QString lsfgState() const;
    QString lsfgFolder() const { return lsfg_.installDir; }
    // What lsfg-vk itself reports in this run (from its log file), translated framing; empty when not applicable.
    QString lsfgLog() const { return lsfgLog_; }
    QString lsfgInstall() const { return lsfgInstall_; }
    QString lsfgInstallText() const { return lsfgInstallText_; }
    bool lsfgActive() const { return lsfgActive_; }
    bool restartNeeded() const;
    int lsfgMultiplier() const { return lsfgMultiplier_; }
    bool fgPaused() const { return fgPaused_; }
    QString fgOverRefresh() const { return fgOverRefresh_; }
    double displayHz() const;
    // Set by main() before the window exists: whether Vulkan UI is used and whether lsfg-vk was enabled for this run.
    void setRunContext(bool vulkanUi, bool lsfgActive, bool lsfgCrashed);
    void setWindow(QWindow* w) {
        window_ = w;
        refreshEdidPeak();
        if (w) connect(w, &QWindow::screenChanged, this, [this] { refreshEdidPeak(); applyEnhance(); });
    }
    QString translateMessage(const QString& msg) const;
    int fxaaQuality() const { return s_.fxaaQuality; }
    QStringList profiles() const;
    QString notice() const { return notice_; }
    void showNotice(const QString& s);
    QString activeProfile() const { return activeProfile_; }
    QString signalKey() const;   // "1920x1080@60 NV12"-style key of the open capture signal, "" for files
    Q_INVOKABLE void saveProfile(const QString& name, bool bindToSignal);
    Q_INVOKABLE bool loadProfile(const QString& name);
    Q_INVOKABLE void loadProfileIndex(int i) { const auto p = profiles(); if (i >= 0 && i < p.size()) loadProfile(p[i]); }
    Q_INVOKABLE void deleteProfile(const QString& name);
    Q_INVOKABLE QString profileSignal(const QString& name) const;
    double sharpen() const { return s_.sharpen; }
    double exposure() const { return s_.exposure; }
    double contrast() const { return s_.contrast; }
    double saturation() const { return s_.saturation; }
    bool smaa() const { return s_.smaa; }
    double texture() const { return s_.texture; }
    double clarity() const { return s_.clarity; }
    double dehaze() const { return s_.dehaze; }

    void setVolume(double v);
    int audioDelay() const { return audioDelay_; }
    void setAudioDelay(int ms) { audioDelay_ = std::clamp(ms, -300, 300); engine_.setAudioDelay(audioDelay_); QSettings().setValue("audio/delayMs", audioDelay_); emit audioChanged(); }
    void setMuted(bool m);
    void setEnhance(bool v);
    void setScale(double v);
    void setUpscaler(int v);
    void setFxaa(bool v);
    void setDeflicker(bool v);
    void setRetroTv(bool v) { s_.retroTv = v; applyEnhance(); }
    void setRetroLines(int v) { s_.retroLines = (v == 0 || v == 240 || v == 480) ? v : 240; applyEnhance(); }
    void setPixelPerfect(bool v) { s_.pixelPerfect = v; applyEnhance(); }
    void setPixelGrid(int v) { s_.pixelGrid = std::clamp(v, 0, 1); applyEnhance(); }
    void setFrameGenMode(int v);
    void setFxaaQuality(int v);
    void setSharpen(double v);
    void setExposure(double v);
    void setContrast(double v);
    void setSaturation(double v);
    void setSmaa(bool v) { s_.smaa = v; applyEnhance(); }
    void setTexture(double v) { s_.texture = float(qBound(-1.0, v, 1.0)); applyEnhance(); }
    void setClarity(double v) { s_.clarity = float(qBound(-1.0, v, 1.0)); applyEnhance(); }
    void setDehaze(double v) { s_.dehaze = float(qBound(-1.0, v, 1.0)); applyEnhance(); }

    QImage currentImage();
    void setGpuDevice(const QString& n) { gpuDevice_ = n; }
    void setLiveVulkan(LiveVulkan* l) {
        liveVk_ = l;
        if (l) {
            connect(l, &LiveVulkan::outputChanged, this, &PlayerBridge::frameReady);   // VideoItem re-syncs
            l->setSettings(s_);
            updateObsSource();
            updateRecordSource();
        }
    }
    bool obsEnabled() const { return obsEnabled_; }
    void setObsEnabled(bool v);
    int obsHeight() const { return obs_.maxHeight(); }
    void setObsHeight(int h);
    int obsViewers() const { return obs_.viewers(); }
    QString obsUrl() const;
    QString obsFile() const { return obsFile_; }
    QString obsError() const { return obsError_; }
    Q_INVOKABLE void saveObsFile();
    Q_INVOKABLE void copyObsUrl();
    LiveVulkan* liveVulkan() const { return liveVk_; }
    // Save settings and release the capture card/audio right away (called on quit, before the exit watchdog starts).
    void shutdown() {
        saveSettings(); QSettings().sync();
        if (liveVk_) { liveVk_->setStream(-1, nullptr); liveVk_->setRecord(-1, nullptr); }
        if (rec_ && rec_->recording()) rec_->stopRecording();
        obs_.stop();
        engine_.close();
        QSettings().remove("lsfg/launching");   // a normal quit is not a crash, however early
    }
    bool liveVulkanActive() const { return liveVk_ && liveVk_->active(); }

    bool replay() const { return replay_; }
    void setReplay(bool v);
    int replaySeconds() const { return replaySeconds_; }
    void setReplaySeconds(int s);
    int recHeight() const { return recHeight_; }
    void setRecHeight(int h);
    bool recording() const { return rec_ && rec_->recording(); }
    QString recInfo() const;
    int compareLayout() const { return compareLayout_; }
    void setCompareLayout(int v) { compareLayout_ = std::clamp(v, 1, 4); QSettings().setValue("rec/compareLayout", compareLayout_); emit recChanged(); }
    int compareSeconds() const { return compareSeconds_; }
    void setCompareSeconds(int v) { compareSeconds_ = std::clamp(v, 5, 120); QSettings().setValue("rec/compareSeconds", compareSeconds_); emit recChanged(); }
    bool comparing() const { return comparing_; }
    Q_INVOKABLE void toggleCompare();   // records a comparison clip (stops by itself after compareSeconds)
    Q_INVOKABLE void addMarker();
    Q_INVOKABLE QVariantList setupCheck();          // [{id, kind, title, detail, fix}] (PlayerBridgeSetup.cpp)
    Q_INVOKABLE void applySetupFix(const QString& id);
    Q_INVOKABLE QString perfReport();
    Q_INVOKABLE void copyPerfReport();       // F6: notes this moment of the recording in "<recording>-markers.txt"
    int markers() const { return int(markers_.size()); }
    QString updateState() const { return updState_; }
    QString updateVersion() const { return updRelease_.version; }
    QString updateText() const { return updText_; }
    QString updateNotes() const { return updRelease_.notes; }
    bool updateSelf() const;
    bool autoUpdateCheck() const { return QSettings().value("update/auto", true).toBool(); }
    void setAutoUpdateCheck(bool v) { QSettings().setValue("update/auto", v); emit updateChanged(); }
    QString appVersion() const;
    Q_INVOKABLE void checkForUpdate(bool manual = true);
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void openReleasePage();
    Q_INVOKABLE void saveReplay();
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void screenshot();
    Q_INVOKABLE void openRecordingsFolder();
    bool streaming() const { return rec_ && rec_->streaming(); }
    bool overlay() const { return overlay_; }
    void setOverlay(bool v) { overlay_ = v; QSettings().setValue("ui/overlay", v); emit overlayChanged(); }
    QVariantList overlayGraph() const { return overlayGraph_; }
    QString overlayStats() const { return overlayStats_; }
    double overlayTarget() const { return overlayTarget_; }
    int streamService() const { return QSettings().value("stream/service", 0).toInt(); }
    void setStreamService(int v) { QSettings().setValue("stream/service", std::clamp(v, 0, 2)); emit recChanged(); }
    QString streamKey() const { return QSettings().value("stream/key").toString(); }
    void setStreamKey(const QString& v) { QSettings().setValue("stream/key", v.trimmed()); emit recChanged(); }
    QString streamServer() const { return QSettings().value("stream/server").toString(); }
    void setStreamServer(const QString& v) { QSettings().setValue("stream/server", v.trimmed()); emit recChanged(); }
    int streamKbps() const { return QSettings().value("stream/kbps", 6000).toInt(); }
    void setStreamKbps(int v) { QSettings().setValue("stream/kbps", std::clamp(v, 1000, 50000)); emit recChanged(); }
    Q_INVOKABLE void toggleStream();
    // Adds VeyAmux to Steam as a non-Steam game (fullscreen, last capture card), for Big Picture / Game Mode
    Q_INVOKABLE void addToSteam();
    void noticeGamescopeFailed() {
        showNotice(L("gamescope did not start last time, so VeyAmux starts normally (gamescope start switched off)",
                     "gamescope startede ikke sidst, så VeyAmux starter normalt (start i gamescope er slået fra)"));
        emit enhanceChanged();
    }
    // Start VeyAmux when the capture card is plugged in (autostart watcher)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY recChanged)
    bool autostart() const;
    void setAutostart(bool on);

public slots:
    QVariantList captureDevices();
    QVariantList captureModes(const QString& path);
    // A word on the chosen mode of the last captureModes() list: a compressed (MJPEG) mode where the card also has an
    // uncompressed one, and at which cost (after PushFPS 4.1). Empty when there is nothing to say.
    QString modeAdvice(int modeIndex) const;
    int betterModeIndex(int modeIndex) const;   // the uncompressed mode the advice points to, or -1
    QString captureHelp() const;   // why no capture card is listed (device files without access, another program), or empty
    QVariantList audioSources();
    Q_INVOKABLE QVariantList audioSinks();   // [{name, desc}] from pactl
    QString audioOutput() const { return QSettings().value("audio/sink").toString(); }
    void setAudioOutput(const QString& s) { QSettings().setValue("audio/sink", s); engine_.setAudioSink(s.toStdString()); emit audioChanged(); }
    bool normalize() const { return QSettings().value("audio/normalize", false).toBool(); }
    void setNormalize(bool v) { QSettings().setValue("audio/normalize", v); engine_.setNormalize(v); emit audioChanged(); }
    QString guessAudioSource(const QString& cardName);
    QString lastCaptureDevice() const;
    QString lastCaptureName() const { return QSettings().value("cap/name").toString(); }   // the card's name, for "continue"
    QString lastCaptureAudio() const;
    bool openCapture(const QString& path, int modeIndex, const QString& audioSource);
    bool openCaptureAuto(const QString& path);
    // A window or screen as the source: opens the compositor's share dialog; the capture starts when one is picked.
    // useLast: let the compositor reuse the previous choice without asking (start-up / --capture last).
    void openScreenCast(const QString& audioSource, bool useLast = false);
    bool screenCastAvailable() const { return veyamux::ScreenCastStream::available(); }
    bool openUrl(const QUrl& url);
    bool openPath(const QString& path);
    void openDialog();
    void togglePlay();
    void play();
    void pause();
    void seek(double seconds);
    void seekRelative(double delta);
    void resetEnhance();
    void restartApp();
    void retryLsfg();
    void refreshLsfg();
    void chooseLsfgFolder();
    void installLsfg();   // downloads lsfg-vk 2 from builds.lsfg-vk.dev into ~/.local (LsfgInstall.h)
    // demo scene: start it at 30 or 60 fps; keyboard / mouse / controller input while the menu is closed
    void openDemo(int fps, int height = 1080);   // height 720 or 1080
    void demoKey(const QString& key, bool down);   // "w" "a" "s" "d" "q" "e" "shift"
    void demoLook(double dx, double dy);
    void demoPose(int pose);                       // -1: the next viewpoint
    void demoRelease();
    void demoPad(double lx, double ly, double rx, double ry, double lt, double rt, bool fast);

    // ---- PlayStation Remote Play
    QVariantList psConsoles() const;
    bool psHasAccount() const;
    QString psBusy() const { return psBusy_; }
    QString psMessage() const { return psMessage_; }
    bool psMessageError() const { return psMessageError_; }
    int psHeight() const;
    void setPsHeight(int h);
    int psFps() const;
    void setPsFps(int f);
    bool psHdr() const;
    void setPsHdr(bool v);
    bool psActive() const;
    Q_INVOKABLE void psStartSearch();       // while the Remote Play window is open
    Q_INVOKABLE void psStopSearch();
    Q_INVOKABLE void psOpenLogin();         // the PlayStation Network login page in the browser
    Q_INVOKABLE void psSetLogin(const QString& addressOrAccountId);   // the address the browser ended on, or the ID
    Q_INVOKABLE void psRegister(const QString& id, const QString& pin);
    Q_INVOKABLE void psSetPasscode(const QString& id, const QString& passcode);
    Q_INVOKABLE void psForget(const QString& id);
    Q_INVOKABLE void psConnect(const QString& id);
    // a console by its IP address (when the search does not find it): asked directly; it can be registered even if it
    // does not answer the search. ps5: the console type, used only until it answers.
    Q_INVOKABLE bool psAddHost(const QString& address, bool ps5);
    Q_INVOKABLE void psRemoveHost(const QString& address);   // wakes the console if needed, then starts Remote Play
    // the controller, PlayStation layout (Gamepad -> RemotePlay); idle while the menu is open
    void psPad(quint32 buttons, double lx, double ly, double rx, double ry, double l2, double r2);

signals:
    void nvidiaInstallChanged();
    void updateChanged();
    void psChanged();
    void psRumble(int low, int high);
    void demoChanged();
    void languageChanged();
    void themeChanged();
    void audioMeterChanged();
    void noticeChanged();
    void profilesChanged();
    void mediaChanged();
    void playingChanged();
    void positionChanged();
    void audioChanged();
    void statsChanged();
    void statusChanged();
    void enhanceChanged();
    void frameReady();
    void ended();
    void lsfgChanged();
    void obsChanged();
    void recChanged();
    void overlayChanged();
    void overlayTick();
    void recTick();

private:
    QString delayText(double ms, bool live, QSize size);
    double delaySum_ = 0, delayShown_ = -1;
    int delayN_ = 0;
    QElapsedTimer delayClock_;
    QString timingText(float gpu, const std::array<float, veyamux::GpuTimer::kStages>& st, double cpuMs, double pipeMs) const;
    void applyEnhance();
    void setStatus(const QString& s);
    void loadSettings();
    void saveSettings();

    veyamux::MediaEngine engine_;
    veyamux::EnhanceSettings s_;
    QTimer tick_;
    QMutex imgMutex_;
    QImage image_;
    bool framePending_ = false;
    double aspect_ = 16.0 / 9.0;
    double volume_ = 1.0;
    bool muted_ = false;
    QString gpuName_, status_, perf_;
    bool lastPlaying_ = false;
    bool demoActive_ = false;
    double demoProgress_ = 0;
    int demoPose_ = 0;
    std::vector<veyamux::CaptureMode> lastModes_;
    veyamux::AudioMeter meter_;
    QString theme_;
    // measured game resolution for pixel-perfect "Measured": taken over once it has been the same for ~1.5 s
    int gridCandW_ = 0, gridCandH_ = 0, gridStable_ = 0;
    void updatePixelGrid();
    QVariantList bands_;
    bool audioActive_ = false;
    QVariantList audioBands() const { return bands_; }
    bool meterWanted() const { return meterWanted_; }
    void setMeterWanted(bool v) { meterWanted_ = v; }
    bool meterWanted_ = true;
    bool audioActive() const { return audioActive_; }
    veyamux::ScreenCastPortal* portal_ = nullptr;
    QString scAudio_;
    bool startCapture(const QString& path, const veyamux::CaptureMode& mode, const QString& audioSource);
    // Remote Play: one background task at a time (network, registration, wake-up)
    QString psBusy_, psMessage_;
    bool psMessageError_ = false;
    std::thread psWorker_;
    std::shared_ptr<std::atomic<bool>> psQuit_ = std::make_shared<std::atomic<bool>>(false);   // VeyAmux is closing
    QTimer* psTimer_ = nullptr;     // refreshes the console list while searching
    QVariantList psSeen_;           // last discovery result
    void psRun(const QString& busy, std::function<void(QString& message, bool& error)> work, std::function<void()> done = {});
    void psSay(const QString& message, bool error);
    bool psStart(const QString& id, QString& error);
    LiveVulkan* liveVk_ = nullptr;
    QPointer<QWindow> window_;
    QString openFile_;              // the video file that is open (to reopen it after a restart)
    bool lsfgOffPending_ = false;   // a restart without lsfg-vk is on its way
    bool lsfgHighNoticed_ = false;  // the 120 Hz hint for Lossless Scaling was shown
    int overRefreshTicks_ = 0;      // 100 ms ticks the output was above the screen's refresh rate (or back below it)
    QString fgOverRefresh_;
    int compareLayout_ = 1, compareSeconds_ = 15;
    update::Release updRelease_;
    QString updState_, updText_;
    bool comparing_ = false;
    QTimer* compareTimer_ = nullptr;
    QString recPath_;                 // the recording being written
    QList<double> markers_;           // its markers (seconds)
    void stopCompare();
    QImage compareLabel(const QString& text, bool accent) const;
    veyamux::CaptureMode curMode_;   // the capture card mode in use (fourcc 0: none / not a card)
    QString curDevice_, curAudio_;
    QString gpuDevice_;              // the Vulkan device's name (performance report)
    bool overRefreshNoticed_ = false;
    void checkOverRefresh();
    // restarts VeyAmux with the same source (and fullscreen) - used to take lsfg-vk off the window
    void restartWithSource();
    lsfg::Detection lsfg_;
    bool vulkanUi_ = false, lsfgActive_ = false, lsfgCrashed_ = false;
    QString lsfgInstall_, lsfgInstallText_;
    bool lsfgAutoTried_ = false;
    bool steamBusy_ = false;
    void finishAddToSteam(bool restartSteam);
    QString lang_ = "en";            // the language in use (en, da, de, es, fr, sv, nb, ru)
    QString langChoice_ = "auto";    // what the user picked (ui/lang): auto = follow the system
    int lsfgMultiplier_ = 2;
    bool fgPaused_ = false;
    int fgHighTicks_ = 0, fgLowTicks_ = 0;
    void checkHighRate();   // 100 ms tick: pause/resume frame generation for ~120 Hz signals
    bool lsfgPerformance_ = false;
    double lastFlow_ = -1;
    QPointer<QProcess> nvidiaInstaller_;
    QString nvidiaInstallStatus_;
    QString upError_;
    bool vrrAtStart_ = false;
    int hdrMode_ = QSettings().value("enh/hdrMode", 0).toInt() == 1 ? 1 : 0, hdrModeAtStart_ = 0;
    bool peakAuto_ = QSettings().value("enh/displayPeakAuto", true).toBool();
    int edidPeak_ = 0;
    void refreshEdidPeak();
    bool hdrOutWanted_ = false, hdrOutAtStart_ = false, hdrOutActive_ = false, hdrOutXwayland_ = false, hdrOutNoVulkan_ = false;
    int audioDelay_ = 0;
    QString activeProfile_;
    QString notice_;
    int noticeSeq_ = 0;
    void applySignalProfile();
    // displayed frame rate (with frame generation), measured over ~1 s windows
    double outFps_ = 0;
    QString lastMediaInfo_;
    ObsStream obs_;
    bool obsEnabled_ = false;
    quint16 obsPort_ = 47920;
    QString obsFile_, obsError_;
    int obsStreamH_ = -2;             // what the live path was last told (-1 = off)
    void updateObsSource();
    std::unique_ptr<veyamux::Recorder> rec_;
    bool replay_ = false;
    int replaySeconds_ = 30, recHeight_ = 1080;
    int recStreamH_ = -2;
    void updateRecordSource();
    bool overlay_ = QSettings().value("ui/overlay", false).toBool();
    QVariantList overlayGraph_;
    QString overlayStats_;
    double overlayTarget_ = 0;
    std::mutex fileShownM_;
    std::deque<double> fileShown_;
    void updateOverlay();
    QString recFolder(bool pictures) const;
    void onRecMessage(const QString& m, bool error);
    uint64_t fpsCount_ = 0;
    qint64 fpsT0_ = 0;
    QElapsedTimer fpsClock_;
    QString lsfgLog_;
    QElapsedTimer runClock_;
    void updateLsfgLog();
    void updateOutputFps(uint64_t shownTotal);
    void updateLsfgMultiplier();
    // a text in the UI language: Danish inline, English as the key into i18n/messages.json for the others
    QString L(const char* en, const char* da = nullptr) const;
};
