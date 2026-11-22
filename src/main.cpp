#include "LiveVulkan.h"
#include "DlssUpscaler.h"
#include "LosslessScaling.h"
#include "PlayerBridge.h"
#include "Gamepad.h"
#include "VideoItem.h"
#include "VkContext.h"
#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include "Sandbox.h"
#include "DemoSource.h"
#include "Watcher.h"
#include <cstring>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QStandardPaths>
#include <QFile>
#include <QTimer>
#include <QVulkanInstance>
#include <QScreen>
#include <memory>
#include <QObject>
#include <thread>
#include <chrono>
#include <unistd.h>

static QString findShaderDir() {
    const QString env = qEnvironmentVariable("VEYAMUX_SHADER_DIR");
    if (!env.isEmpty()) return env;
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString& c : {app + "/../share/veyamux/shaders", app + "/shaders", app + "/../shaders"})
        if (QFileInfo::exists(c + "/ScaleBlit.spv")) return QDir::cleanPath(c);
    return app + "/shaders";
}

// Danish when the language OR the region is Danish (e.g. LANG=en_DK or a Danish LC_TIME also count); English otherwise.

static QString relaunchTarget() {
    // Inside an AppImage the binary lives in a temporary mount that vanishes on exit; relaunch the AppImage itself.
    return qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
}

// Decides how the UI is drawn. Vulkan lets Qt and the video chain share one GPU device (zero-copy capture, no CPU
// compositing). Because a failing graphics backend aborts the process, "auto" first tries it in a child process.
// On a Wayland session whose Vulkan path fails, XWayland is tried before giving up (relaunch with QT_QPA_PLATFORM=xcb).
enum class UiChoice { Software, Vulkan, RelaunchXcb };

static bool probeVulkan(const QString& platform) {
    QProcess probe;
    probe.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!platform.isEmpty()) env.insert("QT_QPA_PLATFORM", platform);
    env.insert("DISABLE_LSFGVK", "1");      // test the UI backend itself, not an optional frame-generation layer
    probe.setProcessEnvironment(env);
    probe.start(QCoreApplication::applicationFilePath(), {"--probe-ui"});
    const bool ok = probe.waitForFinished(20000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    if (!ok) { probe.kill(); probe.waitForFinished(2000); }
    return ok;
}

static UiChoice chooseUi(const QStringList& args, bool probing) {
    const QString env = qEnvironmentVariable("VEYAMUX_UI").toLower();
    if (env == "software") return UiChoice::Software;
    if (env == "vulkan" || probing) return UiChoice::Vulkan;
    QSettings q;
    const QString platform = QGuiApplication::platformName();
    // On Wayland, XWayland is the tested, stable path (Lossless Scaling pacing, resizing). Native Wayland is used only
    // for HDR output, which XWayland cannot do.
    const bool native = q.value("enh/hdrOut", false).toBool() && qEnvironmentVariable("VEYAMUX_HDR_OUT") != "0";
    const QString key = "ui/backend/" + platform + "/" + QCoreApplication::applicationVersion() + (native ? "/native" : "");
    if (args.contains("--reset-ui")) q.remove(key);
    QString cached = q.value(key).toString();
    if (cached.isEmpty()) {
        const bool canX = platform == "wayland" && qEnvironmentVariableIsSet("DISPLAY");
        if (!native && canX && probeVulkan("xcb")) cached = "xcb";
        else if (probeVulkan({})) cached = "vulkan";
        else if (canX && probeVulkan("xcb")) cached = "xcb";
        else cached = "software";
        if (cached != "software") q.setValue(key, cached);   // a failed probe is retried at the next start
    }
    if (cached == "vulkan") return UiChoice::Vulkan;
    if (cached == "xcb" && platform == "wayland" && !qEnvironmentVariableIsSet("VEYAMUX_RELAUNCHED")) return UiChoice::RelaunchXcb;
    return UiChoice::Software;
}

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--test-vfg") == 0 || std::strcmp(argv[1], "--bench-vfg") == 0)) {
        QCoreApplication cli(argc, argv);
        QProcess probe;
        probe.setProcessChannelMode(QProcess::ForwardedChannels);
        probe.start(QCoreApplication::applicationDirPath() + "/veyamux-nvidia-vfg-probe", cli.arguments().mid(1));
        if (!probe.waitForStarted() || !probe.waitForFinished(-1) || probe.exitStatus() != QProcess::NormalExit) {
            qWarning("Unable to run NVIDIA VFG diagnostic: %s", qPrintable(probe.errorString()));
            return 1;
        }
        return probe.exitCode();
    }
    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", "threaded");
    // Grey (not sub-pixel/LCD) text antialiasing: sub-pixel text gets coloured fringes when the window is scaled by the
    // compositor, drawn through a layer (HDR output) or captured.
    if (!qEnvironmentVariableIsSet("QSG_DISTANCEFIELD_ANTIALIASING")) qputenv("QSG_DISTANCEFIELD_ANTIALIASING", "gray");
    // Qt 6.4's Wayland plugin loads the Vulkan loader as plain "libvulkan.so", which only exists where the -dev package
    // is installed (not on Bazzite/Fedora Atomic/SteamOS). Without this the native Wayland Vulkan UI fails and VeyAmux
    // ends up on XWayland (no HDR). The X11 plugin already asks for libvulkan.so.1.
    if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) qputenv("QT_VULKAN_LIB", "libvulkan.so.1");
    // libva looks for the host Mesa VAAPI drivers; distros keep them in different places.
    if (!qEnvironmentVariableIsSet("LIBVA_DRIVERS_PATH"))
        qputenv("LIBVA_DRIVERS_PATH", "/usr/lib64/dri:/usr/lib/x86_64-linux-gnu/dri:/usr/lib/dri:/usr/lib/va");

    QCoreApplication::setOrganizationName("VeyAmux");
    QCoreApplication::setApplicationName("VeyAmuxNvidiaTest");
    QCoreApplication::setApplicationVersion(QStringLiteral(VEYAMUX_RELEASE_VERSION));
    // `--watch`: no window, only the capture-card watcher (started from ~/.config/autostart)
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--watch") == 0) {
            QCoreApplication wapp(argc, argv);
            return veyamux::watch::run(relaunchTarget());
        }
    // The UI itself is cheap; what matters is latency. VSync is off by default (like Windows' tearing mode);
    // VEYAMUX_VSYNC=1 turns it back on. VRR mode (frame generation off) presents with vsync, so a VRR display shows
    // each evenly paced picture whole, at the moment it is presented, without tearing.
    bool vrrAtStart = false;
    {
        QSettings q;
        vrrAtStart = q.value("enh/vrr", false).toBool() && q.value("enh/frameGenMode", 6).toInt() == 0;
        if (qEnvironmentVariable("VEYAMUX_VSYNC") == "1" || vrrAtStart) qunsetenv("QSG_NO_VSYNC");
        else qputenv("QSG_NO_VSYNC", "1");
    }
    QApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName("VeyAmux NVIDIA Test");
    QGuiApplication::setDesktopFileName(QStringLiteral("veyamux"));

    QString openFile, grabPath, capturePath;
    bool startPanel = false, startDialog = false;
    const QStringList args = app.arguments();
    const bool probing = args.contains("--probe-ui");
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--panel") startPanel = true;
        else if (args[i] == "--dialog") startDialog = true;
        else if (args[i] == "--capture" && i + 1 < args.size()) capturePath = args[++i];
        else if (args[i] == "--grab" && i + 1 < args.size()) grabPath = args[++i];
        else if (!args[i].startsWith("--")) openFile = args[i];
    }

    // ---- gamescope (optional): run inside Valve's compositor for HDR and VRR through XWayland ----
    // Not when already inside gamescope (Steam Game Mode, or this relaunch).
    // Crash guard: if the last gamescope start never reported "running fine", gamescope is switched off again.
    bool gamescopeFailed = false;
    if (!probing && !qEnvironmentVariableIsSet("VEYAMUX_IN_GAMESCOPE")) {
        QSettings q;
        if (q.value("ui/gamescopeLaunching", false).toBool()) {
            q.remove("ui/gamescopeLaunching");
            q.setValue("ui/gamescope", false);
            gamescopeFailed = true;
            qWarning("The last start inside gamescope did not come up; gamescope start switched off");
        }
    }
    // Experimental and opt-in only via VEYAMUX_GAMESCOPE=1: on some systems gamescope's WSI layer rejects the swapchain
    // ("Creating swapchain for non-Gamescope swapchain") and pressing OK can freeze the desktop.
    if (!probing && QSettings().value("ui/gamescope", false).toBool() && qEnvironmentVariable("VEYAMUX_GAMESCOPE") == "1"
        && !qEnvironmentVariableIsSet("GAMESCOPE_WAYLAND_DISPLAY") && !qEnvironmentVariableIsSet("VEYAMUX_IN_GAMESCOPE")) {
        const QString gs = QStandardPaths::findExecutable("gamescope");
        if (gs.isEmpty()) {
            qWarning("gamescope is not installed; starting normally");
        } else {
            QScreen* sc = app.primaryScreen();
            const QSize px = sc ? sc->size() * sc->devicePixelRatio() : QSize(1920, 1080);
            const int hz = sc ? std::max(30, qRound(sc->refreshRate())) : 60;
            QStringList a{"-f", "-W", QString::number(px.width()), "-H", QString::number(px.height()),
                          "-r", QString::number(hz), "--adaptive-sync"};
            if (QSettings().value("enh/hdrOut", false).toBool()) a << "--hdr-enabled";
            a << "--" << relaunchTarget() << args.mid(1);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("VEYAMUX_IN_GAMESCOPE", "1");
            env.insert("QT_QPA_PLATFORM", "xcb");        // gamescope's own XWayland, not the desktop's Wayland
            env.remove("WAYLAND_DISPLAY");
            env.insert("ENABLE_GAMESCOPE_WSI", "1");     // HDR swapchains for X11 Vulkan apps inside gamescope
            // Other swapchain layers break gamescope's WSI layer ("Creating swapchain for non-Gamescope swapchain"):
            // the desktop HDR layer (ENABLE_HDR_WSI) and Lossless Scaling's lsfg-vk are left out inside gamescope.
            env.remove("ENABLE_HDR_WSI");
            env.insert("DISABLE_HDR_WSI", "1");
            env.insert("VEYAMUX_LSFG", "0");
            env.insert("DISABLE_LSFGVK", "1");
            // other common capture/overlay layers that wrap the swapchain (OBS can capture the gamescope window)
            for (const char* v : {"OBS_VKCAPTURE", "MANGOHUD", "ENABLE_VKBASALT", "VK_INSTANCE_LAYERS"}) env.remove(v);
            env.insert("DISABLE_MANGOHUD", "1");
            env.insert("DISABLE_VKBASALT", "1");
            env.insert("VK_LOADER_DEBUG", "layer");   // which layers load: written to the log below
            const QString log = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/veyamux-gamescope.log";
            QDir().mkpath(QFileInfo(log).absolutePath());
            QSettings().setValue("ui/gamescopeLaunching", true);
            QProcess p;
            p.setProgram(gs);
            p.setArguments(a);
            p.setProcessEnvironment(env);
            p.setStandardOutputFile(log);
            p.setStandardErrorFile(log);
            qInfo("Starting inside gamescope: %s %s", qPrintable(gs), qPrintable(a.join(' ')));
            if (p.startDetached()) return 0;
            qWarning("Could not start gamescope; starting normally");
        }
    }

    // ---- graphics backend ----
    std::unique_ptr<QVulkanInstance> vkInst;
    std::unique_ptr<veyamux::VkContext> vkCtx;
    const UiChoice choice = chooseUi(args, probing);
    if (choice == UiChoice::RelaunchXcb) {
        qputenv("QT_QPA_PLATFORM", "xcb");
        qputenv("VEYAMUX_RELAUNCHED", "1");
        qInfo("Vulkan UI works through XWayland on this system; relaunching with the X11 backend");
        if (QProcess::startDetached(relaunchTarget(), args.mid(1))) return 0;
    }
    bool vulkanUi = choice != UiChoice::Software;

    // ---- Lossless Scaling (lsfg-vk layer): must be decided before the Vulkan instance exists ----
    bool lsfgEnabled = false, lsfgCrashed = false;
    if (!probing) {
        QSettings q;
        const int fgMode = q.value("enh/frameGenMode", 6).toInt();
        const bool wantLs = fgMode >= 1 && fgMode <= 4 && qEnvironmentVariable("VEYAMUX_LSFG") != "0";
        // Crash guard: if the previous start with lsfg-vk never reached the "running fine" mark, keep Lossless Scaling
        // off until the user presses "Try again" (otherwise a broken layer would make every start crash).
        if (q.value("lsfg/launching", false).toBool()) {
            q.remove("lsfg/launching");
            q.setValue("lsfg/disabledAfterCrash", true);
        }
        lsfgCrashed = q.value("lsfg/disabledAfterCrash", false).toBool();
        if (wantLs && vulkanUi && !lsfgCrashed) {
            const lsfg::Detection d = lsfg::detect(q.value("lsfg/folder").toString());
            if (d.ready() && d.layerV2) {
                QFile::remove(lsfg::logPath());   // the log the menu shows must be this run's
                const QScreen* scr = QGuiApplication::primaryScreen();
                const int outH = scr ? int(scr->size().height() * scr->devicePixelRatio()) : 1080;
                const int startMult = fgMode == 3 ? 3 : 2;
                lsfg::writeConfig(d, startMult, q.value("lsfg/performance", false).toBool(), lsfg::recommendedFlowScale(outH));
                lsfgEnabled = true;
                qInfo("Lossless Scaling: lsfg-vk enabled for this window (profile %s, config %s)", lsfg::kProfileName,
                      qPrintable(lsfg::configPath()));
                q.setValue("lsfg/launching", true);
                q.sync();
            }
        }
        lsfg::configureEnvironment(lsfgEnabled);
    }
    const bool hdrOutSetting = !probing && QSettings().value("enh/hdrOut", false).toBool() && qEnvironmentVariable("VEYAMUX_HDR_OUT") != "0";
    bool hdrOutRequested = hdrOutSetting;
    // HDR10 (PQ, BT.2020, 10 bit) is the common HDR format (KDE, GNOME, gamescope); scRGB (FP16) is the alternative.
    int hdrMode = QSettings().value("enh/hdrMode", 0).toInt() == 1 ? 1 : 0;
    if (qEnvironmentVariable("VEYAMUX_HDR_MODE") == "scrgb") hdrMode = 1;
    if (qEnvironmentVariable("VEYAMUX_HDR_MODE") == "hdr10") hdrMode = 0;
    if (vulkanUi) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        vkInst = std::make_unique<QVulkanInstance>();
        vkInst->setApiVersion(QVersionNumber(1, 3));
        QByteArrayList exts;
        if (vkInst->supportedExtensions().contains(QByteArrayLiteral("VK_EXT_debug_utils")))
            exts << QByteArrayLiteral("VK_EXT_debug_utils");   // FidelityFX debug labels
        // HDR display output (experimental, opt-in): ask Qt Quick for an scRGB (FP16, extended linear sRGB) swapchain.
        // Needs the colour-space extension; Qt falls back to a normal swapchain if the display/compositor can't do it.
        if (hdrOutRequested && vkInst->supportedExtensions().contains(QByteArrayLiteral("VK_EXT_swapchain_colorspace"))) {
            exts << QByteArrayLiteral("VK_EXT_swapchain_colorspace");
            qputenv("QSG_RHI_HDR", hdrMode == 1 ? "scrgb" : "hdr10");
        } else {
            hdrOutRequested = false;
        }
        for (const auto& name : veyamux::DlssUpscaler::extensions()) {
            const QByteArray extension(name.c_str());
            if (vkInst->supportedExtensions().contains(extension) && !exts.contains(extension))
                exts << extension;
        }
        vkInst->setExtensions(exts);
        std::string err;
        if (!vkInst->create()) {
            qWarning("Vulkan UI unavailable (QVulkanInstance); using the software renderer");
            vulkanUi = false;
        } else {
            vkCtx = std::make_unique<veyamux::VkContext>();
            if (!vkCtx->initShared(vkInst->vkInstance(), err)) {
                qWarning("Vulkan UI unavailable (%s); using the software renderer", err.c_str());
                vkCtx.reset();
                vulkanUi = false;
            }
        }
    }
    if (probing && !vulkanUi) return 4;            // the probe must report failure, not silently fall back
    if (!vulkanUi) {
        vkCtx.reset();
        vkInst.reset();
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    std::unique_ptr<LiveVulkan> live;       // declared before the player/engine so it is destroyed after them
    qmlRegisterType<VideoItem>("VeyAmux", 1, 0, "VideoItem");
    PlayerBridge player(findShaderDir());
    veyamux::GamepadInput gamepad;   // controllers for the menu (Select + Start); outlives the QML engine
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("gamepad", &gamepad);
    // the demo scene swims with the controller while the menu is closed; Y jumps to the next viewpoint
    QObject::connect(&gamepad, &veyamux::GamepadInput::analog, &player, &PlayerBridge::demoPad);
    // PlayStation Remote Play: the controller to the console, the game's rumble back
    QObject::connect(&gamepad, &veyamux::GamepadInput::padState, &player, &PlayerBridge::psPad);
    QObject::connect(&player, &PlayerBridge::psRumble, &gamepad, &veyamux::GamepadInput::rumble);
    QObject::connect(&gamepad, &veyamux::GamepadInput::freeButton, &player, [&player](const QString& b) {
        if (b == "y" && player.demoActive()) player.demoPose(-1);
    });
    engine.rootContext()->setContextProperty("player", &player);
    engine.rootContext()->setContextProperty("startPanel", startPanel);
    engine.rootContext()->setContextProperty("startDialog", startDialog);
    // a handheld PC (Steam Deck, ROG Ally, Legion Go, ...): the menu fills the whole window (Main.qml compactMenu)
    const bool handheld = [] {
        if (qEnvironmentVariable("SteamDeck") == "1") return true;   // set by Steam in the Deck's Game Mode
        QFile f("/sys/class/dmi/id/product_name");
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QString n = QString::fromLatin1(f.readAll()).trimmed();
        static const char* names[] = {"Jupiter", "Galileo", "ROG Ally", "RC71L", "RC72L", "Legion Go", "83E1", "83L3",
                                      "AYANEO", "AYA NEO", "ONEXPLAYER", "ONE XPLAYER", "Claw", "G1618", "G1619", "WIN 4", "WIN Mini"};
        for (const char* h : names) if (n.contains(QLatin1String(h), Qt::CaseInsensitive)) return true;
        return false;
    }();
    engine.rootContext()->setContextProperty("handheld", handheld);
    engine.rootContext()->setContextProperty("keepControls", !grabPath.isEmpty());
    player.setVrrAtStart(vrrAtStart);
    player.setRunContext(vulkanUi, lsfgEnabled, lsfgCrashed);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!win) return 1;

    if (vulkanUi) {
        win->setVulkanInstance(vkInst.get());
        win->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(vkCtx->physical(), vkCtx->device(), int(vkCtx->queueFamily()), 0));
        live = std::make_unique<LiveVulkan>(win, vkCtx.get(), findShaderDir());
        std::string err;
        if (live->init(err)) { player.setLiveVulkan(live.get()); player.setGpuDevice(QString::fromStdString(vkCtx->deviceName())); }
        else { qWarning("Zero-copy live path disabled: %s", err.c_str()); live.reset(); }
    }
    player.setWindow(win);
    player.setHdrOutputState(hdrOutSetting, false);   // until the swapchain test below says otherwise
    player.setHdrModeAtStart(hdrMode);
    player.setHdrOutputXwayland(QGuiApplication::platformName() == "xcb" && qEnvironmentVariableIsSet("WAYLAND_DISPLAY"));
    player.setHdrOutputNoVulkan(!vulkanUi);
    win->setVisible(true);
    // --fullscreen, and always inside gamescope (Steam Game Mode or VeyAmux's own gamescope start)
    if (args.contains("--fullscreen") || qEnvironmentVariableIsSet("GAMESCOPE_WAYLAND_DISPLAY") || qEnvironmentVariableIsSet("VEYAMUX_IN_GAMESCOPE"))
        win->showFullScreen();
    if (vulkanUi && hdrOutRequested) {
        // Same test Qt makes before it creates the swapchain: does the window surface offer FP16 + extended sRGB?
        QObject::connect(win, &QQuickWindow::frameSwapped, &player, [&player, win, hdrMode, inst = vkInst.get(), ctx = vkCtx.get()] {
            static bool done = false;
            if (done) return;
            done = true;
            bool ok = false;
            const VkSurfaceKHR surf = QVulkanInstance::surfaceForWindow(win);
            auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(inst->getInstanceProcAddr("vkGetPhysicalDeviceSurfaceFormatsKHR"));
            if (surf && getFormats) {
                uint32_t n = 0;
                getFormats(ctx->physical(), surf, &n, nullptr);
                std::vector<VkSurfaceFormatKHR> f(n);
                if (n) getFormats(ctx->physical(), surf, &n, f.data());
                for (const auto& sf : f) {
                    if (hdrMode == 1)
                        ok = ok || (sf.format == VK_FORMAT_R16G16B16A16_SFLOAT && sf.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
                    else
                        ok = ok || ((sf.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || sf.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
                                    && sf.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT);
                }
            }
            if (qEnvironmentVariableIsSet("VEYAMUX_FORCE_HDR_OUT")) ok = true;   // test hook (SDR swapchain, clipped)
            qInfo("HDR output: %s", ok ? (hdrMode == 1 ? "scRGB swapchain available" : "HDR10 swapchain available")
                                       : "the display/compositor offers no HDR swapchain; using SDR");
            QMetaObject::invokeMethod(&player, [&player, ok] { player.setHdrOutputState(true, ok); }, Qt::QueuedConnection);
        }, Qt::DirectConnection);
    }
    if (qEnvironmentVariableIsSet("VEYAMUX_IN_GAMESCOPE"))   // came up inside gamescope: clear its crash guard
        QTimer::singleShot(5000, &app, [] { QSettings().remove("ui/gamescopeLaunching"); });
    if (gamescopeFailed)
        QTimer::singleShot(1500, &player, [&player] { player.noticeGamescopeFailed(); });
    if (lsfgEnabled) {
        // Running fine for a few seconds with lsfg-vk loaded: clear the crash guard.
        QTimer::singleShot(8000, &app, [] { QSettings q; q.remove("lsfg/launching"); });
    }

    if (probing) {
        QObject::connect(win, &QQuickWindow::frameSwapped, &app, [] { QTimer::singleShot(150, [] { QCoreApplication::exit(0); }); });
        QTimer::singleShot(12000, &app, [] { QCoreApplication::exit(3); });
        return app.exec();
    }
    // Test hook: feed synthetic raw frames to the zero-copy path (VEYAMUX_TEST_LIVE=yuyv|uyvy|nv12|p010).
    const QString testLive = qEnvironmentVariable("VEYAMUX_TEST_LIVE");
    if (!testLive.isEmpty() && live) {
        auto* tick = new QTimer(&app);
        tick->setInterval(16);
        auto counter = std::make_shared<int>(0);
        live->setActive(true);
        QObject::connect(tick, &QTimer::timeout, [&live, counter, testLive] {
            const int W = 640, H = 360, n = (*counter)++;
            veyamux::RawFrame f;
            f.w = W; f.h = H;
            const int bx = (n * 4) % (W - 80);
            auto yuv = [&](int x, int y, int& Y, int& U, int& V) {
                Y = 16 + x * 219 / W; U = 128; V = 128;
                if (x >= bx && x < bx + 80 && y >= 140 && y < 220) { Y = 81; U = 90; V = 240; }   // red box (BT.601/709 limited)
            };
            if (testLive == "p010") {
                // HDR test picture: top half a PQ grey ramp 0 -> 4000 nits, bottom half four 203-nit (SDR white) patches
                // in grey / BT.2020 red / green / blue, plus a moving 1000-nit box. 10-bit limited range, MSB-aligned.
                auto pq = [](double nits) {
                    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
                    const double p = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
                    return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
                };
                auto ycc = [&](double r, double g, double b, int& Y, int& U, int& V) {   // linear BT.2020 nits -> codes
                    const double R = pq(r), G = pq(g), B = pq(b);
                    const double y = 0.2627 * R + 0.6780 * G + 0.0593 * B;
                    Y = int(std::lround(64 + 876 * y));
                    U = int(std::lround(512 + 896 * (B - y) / 1.8814));
                    V = int(std::lround(512 + 896 * (R - y) / 1.4746));
                };
                f.fmt = veyamux::RawFrame::P010; f.stride = W * 2;
                f.data.assign(size_t(W) * H * 3, 0);
                auto put16 = [&](size_t off, int v) { const uint16_t c = uint16_t(std::clamp(v, 0, 1023) << 6); f.data[off] = uint8_t(c); f.data[off + 1] = uint8_t(c >> 8); };
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    double r, g, b;
                    if (y < H / 2) { const double nits = 4000.0 * std::pow(double(x) / (W - 1), 3.0); r = g = b = nits; }
                    else {
                        const int q = x * 4 / W;
                        r = q == 0 || q == 1 ? 203.0 : 0.0; g = q == 0 || q == 2 ? 203.0 : 0.0; b = q == 0 || q == 3 ? 203.0 : 0.0;
                    }
                    if (x >= bx && x < bx + 80 && y >= 140 && y < 220) r = g = b = 1000.0;
                    int Y, U, V; ycc(r, g, b, Y, U, V);
                    put16(size_t(y) * W * 2 + size_t(x) * 2, Y);
                    if ((x & 1) == 0 && (y & 1) == 0) {
                        const size_t uv = size_t(W) * 2 * H + size_t(y / 2) * W * 2 + size_t(x / 2) * 4;
                        put16(uv, U); put16(uv + 2, V);
                    }
                }
            } else if (testLive == "nv12") {
                f.fmt = veyamux::RawFrame::NV12; f.stride = W;
                f.data.assign(size_t(W) * H * 3 / 2, 0);
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { int Y, U, V; yuv(x, y, Y, U, V); f.data[size_t(y) * W + x] = uint8_t(Y);
                    if ((x & 1) == 0 && (y & 1) == 0) { f.data[size_t(W) * H + size_t(y / 2) * W + x] = uint8_t(U); f.data[size_t(W) * H + size_t(y / 2) * W + x + 1] = uint8_t(V); } }
            } else {
                const bool uyvy = testLive == "uyvy";
                f.fmt = uyvy ? veyamux::RawFrame::UYVY : veyamux::RawFrame::YUYV; f.stride = W * 2;
                f.data.assign(size_t(W) * H * 2, 0);
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; x += 2) {
                    int Y0, U, V, Y1, U1, V1; yuv(x, y, Y0, U, V); yuv(x + 1, y, Y1, U1, V1);
                    uint8_t* p = &f.data[size_t(y) * W * 2 + size_t(x) * 2];
                    if (uyvy) { p[0] = uint8_t(U); p[1] = uint8_t(Y0); p[2] = uint8_t(V); p[3] = uint8_t(Y1); }
                    else { p[0] = uint8_t(Y0); p[1] = uint8_t(U); p[2] = uint8_t(Y1); p[3] = uint8_t(V); }
                }
            }
            live->submit(f);
        });
        tick->start();
    }
    // Test hook: replay + recording + screenshot round trip (VEYAMUX_TEST_REC=1, with VEYAMUX_TEST_LIVE or a file).
    if (qEnvironmentVariableIsSet("VEYAMUX_TEST_REC")) {
        QTimer::singleShot(500, [&] { player.setReplay(true); });
        QTimer::singleShot(1500, [&] { player.toggleRecording(); });
        QTimer::singleShot(4500, [&] { player.toggleRecording(); player.saveReplay(); player.screenshot(); });
    }
    // --capture last: the card used last time (Steam / Game Mode shortcut)
    if (capturePath == "last") capturePath = player.lastCaptureDevice();
    if (!capturePath.isEmpty()) QTimer::singleShot(0, [&] { player.openCaptureAuto(capturePath); });
    if (!openFile.isEmpty()) QTimer::singleShot(0, [&] { player.openPath(openFile); });
    if (!grabPath.isEmpty()) {
        QTimer::singleShot(9000, [&] {
            win->grabWindow().save(grabPath);
            QCoreApplication::quit();
        });
    }
    // Shutdown watchdog: if tearing down (Vulkan device, implicit layers such as lsfg-vk, audio) hangs, the process
    // would keep the capture card open and block the next start. Settings are already saved at this point.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&player] {
        player.shutdown();
        veyamux::demo::shutdown();   // the demo scene's render thread
        std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(4)); _exit(0); }).detach();
    });
    return app.exec();
}
