#include "PlayerBridge.h"
#include "Sandbox.h"
#include "DemoSource.h"
#include "RemotePlay.h"
#include "PulseDevices.h"
#include <cstring>
#include <QLocale>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrl>
#include <QTimer>
#include <QCoreApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QVariantMap>
#include <QSettings>
#include <QIcon>
#include <QLinearGradient>
#include <QFontMetrics>
#include <QPainter>
#include <linux/videodev2.h>
#include <unistd.h>
#include "DisplayVrr.h"
#include "LsfgInstall.h"
#include "SteamShortcut.h"
#include "Watcher.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QStandardPaths>

using veyamux::MediaEngine;

PlayerBridge::PlayerBridge(const QString& shaderDir, QObject* parent)
    : QObject(parent), engine_(shaderDir.toStdString()), rec_(std::make_unique<veyamux::Recorder>()) {
    connect(this, &PlayerBridge::playingChanged, this, &PlayerBridge::updateDisplayIdle);
    connect(this, &PlayerBridge::mediaChanged, this, &PlayerBridge::updateDisplayIdle);
    QSettings local;
    const QSettings original("VeyAmux", "VeyAmux");
    setTheme(local.value("ui/theme", original.value("ui/theme", "vice")).toString());
    // the game's rumble (Remote Play) goes to the controller (main.cpp: psRumble -> GamepadInput::rumble)
    veyamux::remoteplay::setRumbleCallback([this](int low, int high) {
        QMetaObject::invokeMethod(this, [this, low, high] { emit psRumble(low, high); }, Qt::QueuedConnection);
    });
    setLanguageChoice(local.value("ui/lang", original.value("ui/lang", "auto")).toString());
    rec_->onMessage = [this](const std::string& m, bool error) {
        const QString q = QString::fromStdString(m);
        QMetaObject::invokeMethod(this, [this, q, error] { onRecMessage(q, error); }, Qt::QueuedConnection);
    };
    loadSettings();
    connect(&obs_, &ObsStream::viewersChanged, this, [this] { updateObsSource(); emit obsChanged(); });
    if (QSettings().value("obs/enabled", false).toBool()) setObsEnabled(true);
    MediaEngine::Callbacks cb;
    cb.onFrame = [this](const uint8_t* rgba, int w, int h, double aspect) {
        if (overlay_) {
            std::lock_guard l(fileShownM_);
            fileShown_.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
            while (fileShown_.size() > 600) fileShown_.pop_front();
        }
        QImage img = QImage(rgba, w, h, w * 4, QImage::Format_RGBA8888).copy();   // detach from engine buffer
        if (!liveVulkanActive() && (obs_.wantsFrames() || rec_->active())) {
            const QImage sdr = s_.hdrOut ? sdrCopy(img) : img;
            if (obs_.wantsFrames()) obs_.pushFrame(sdr);        // shared, not copied
            if (rec_->active()) rec_->pushRgba(sdr.constBits(), w, h, int(sdr.bytesPerLine()), recHeight_, std::chrono::steady_clock::now());
        }
        bool post = false;
        {
            QMutexLocker l(&imgMutex_);
            image_ = std::move(img);
            aspect_ = aspect;
            post = !framePending_;
            framePending_ = true;
        }
        if (post) QMetaObject::invokeMethod(this, [this] {
            { QMutexLocker l(&imgMutex_); framePending_ = false; }
            emit frameReady();
        }, Qt::QueuedConnection);
    };
    cb.onEnded = [this] { QMetaObject::invokeMethod(this, [this] { emit ended(); emit playingChanged(); }, Qt::QueuedConnection); };
    cb.onError = [this](const std::string& m) {
        const QString q = QString::fromStdString(m);
        QMetaObject::invokeMethod(this, [this, q] { setStatus(translateMessage(q)); }, Qt::QueuedConnection);
    };
    cb.onGpuMessage = [this](const std::string& m) {
        const QString q = QString::fromStdString(m);
        QMetaObject::invokeMethod(this, [this, q] {
            if (q.startsWith("GPU:")) gpuName_ = q.mid(5); else setStatus(translateMessage(q));
            emit statsChanged();
        }, Qt::QueuedConnection);
    };
    cb.onLiveFrame = [this](veyamux::RawFrame& f) { if (liveVk_) liveVk_->submit(f); };
    cb.onAudio = [this](const int16_t* pcm, int frames, std::chrono::steady_clock::time_point t) {
        meter_.push(pcm, frames);   // the source's sound, before volume and mute (also what recordings/streams get)
        rec_->pushAudio(pcm, frames, t);
    };
    {   // spectrum meter for the menu, ~30 updates per second while something moves
        auto* mt = new QTimer(this);
        connect(mt, &QTimer::timeout, this, [this] {
            if (!meterWanted_) return;   // menu closed: the spectrum is not on screen
            const auto b = meter_.bands();
            const bool act = meter_.active();
            bool any = false;
            for (float v : b) any = any || v > 0.001f;
            if (!any && bands_.size() == int(b.size()) && !audioActive_ && !act) {
                bool allZero = true;
                for (const QVariant& v : bands_) allZero = allZero && v.toDouble() <= 0.001;
                if (allZero) return;   // silent and already drawn as silent
            }
            QVariantList l;
            l.reserve(int(b.size()));
            for (float v : b) l << double(v);
            bands_ = l;
            audioActive_ = act;
            emit audioMeterChanged();
        });
        mt->start(33);
    }
    engine_.setCallbacks(cb);
    engine_.setAudioSink(audioOutput().toStdString());
    engine_.setNormalize(normalize());
    applyEnhance();
    {
        QSettings q;
        replaySeconds_ = std::clamp(q.value("rec/replaySeconds", 30).toInt(), 10, 300);
        recHeight_ = std::clamp(q.value("rec/height", 1080).toInt(), 0, 4320);
        compareLayout_ = std::clamp(q.value("rec/compareLayout", 1).toInt(), 1, 4);
        compareSeconds_ = std::clamp(q.value("rec/compareSeconds", 15).toInt(), 5, 120);
        if (q.value("rec/replay", false).toBool()) setReplay(true);
    }
    tick_.setInterval(100);
    connect(&tick_, &QTimer::timeout, this, [this] {
        if (rec_->recording() || rec_->streaming()) emit recTick();
        if (overlay_) updateOverlay();
        checkHighRate();
        checkOverRefresh();
        updatePixelGrid();
        if (demoActive_) {
            const double p = veyamux::demo::progress();
            if (p != demoProgress_) {
                demoProgress_ = p;
                emit demoChanged();
                if (p >= 1) setStatus({});
                else setStatus(L("Building the underwater world", "Bygger undervandsverdenen") + QStringLiteral("... %1 %").arg(int(p * 100)));
            }
            if (veyamux::demo::failed() && !veyamux::demo::stage().empty()) setStatus(translateMessage(QString::fromStdString(veyamux::demo::stage())));
        }
        if (!engine_.isOpen()) return;
        emit positionChanged();
        if (engine_.playing() != lastPlaying_) { lastPlaying_ = engine_.playing(); emit playingChanged(); }
        const auto st = engine_.stats();
        updateOutputFps(liveVulkanActive() ? liveVk_->framesShown() : st.rendered);
        updateObsSource();
        if (engine_.isLive()) {   // measured content values change continuously: refresh the media line when its text does
            const QString m = mediaInfo();
            if (m != lastMediaInfo_) { lastMediaInfo_ = m; emit mediaChanged(); }
        }
        if (liveVulkanActive()) {
            const QSize sz = liveVk_->imageSize();
            const double pipe = liveVk_->pipelineMs();
            perf_ = delayText(pipe > 0 ? pipe : std::max(0.0, double(liveVk_->gpuMs())) + liveVk_->lastMs(), pipe > 0, sz);
            if (qEnvironmentVariableIsSet("VEYAMUX_DEBUG_FG"))
                qInfo("timing:%s", qPrintable(timingText(liveVk_->gpuMs(), liveVk_->gpuStageMs(), liveVk_->lastMs(), pipe)));
            gpuName_ = L("Vulkan (shared with the UI)", "Vulkan (delt med brugerfladen)");
            upError_ = (s_.upscaler == veyamux::Upscaler::AmdFsr3 || s_.upscaler == veyamux::Upscaler::NvidiaDlss) ? translateMessage(liveVk_->lastUpError()) : QString();
            updateLsfgMultiplier();
            emit statsChanged();
            return;
        }
        upError_ = (s_.upscaler == veyamux::Upscaler::AmdFsr3 || s_.upscaler == veyamux::Upscaler::NvidiaDlss) ? translateMessage(QString::fromStdString(st.upError)) : QString();
        updateLsfgMultiplier();
        perf_ = delayText(st.gpuActive ? std::max(0.0, double(st.gpuMs)) + st.processMs : st.processMs, false, QSize(st.outW, st.outH))
                + (st.dropped ? QString(" · %1 %2").arg(st.dropped).arg(L("dropped", "tabt")) : QString());
        if (!engine_.gpuName().empty()) gpuName_ = QString::fromStdString(engine_.gpuName());
        emit statsChanged();
    });
    tick_.start();
    // updates: asked by itself a few seconds after the start (not while the first picture is being set up)
    if (inAppUpdatesEnabled() && autoUpdateCheck() && qEnvironmentVariable("VEYAMUX_NO_UPDATE_CHECK") != "1")
        QTimer::singleShot(8000, this, [this] { checkForUpdate(false); });
}

void PlayerBridge::updateDisplayIdle() {
    const bool visible = window_ && window_->isVisible() && window_->visibility() != QWindow::Minimized;
    displayIdle_.setActive(visible && engine_.isOpen() && engine_.playing() && engine_.info().hasVideo);
}

PlayerBridge::~PlayerBridge() {
    saveSettings();
    veyamux::remoteplay::setRumbleCallback({});
    // a running Remote Play task (registration, wake-up: up to ~40 s) is not waited for
    psQuit_->store(true);
    if (psWorker_.joinable()) psWorker_.detach();
    veyamux::remoteplay::stopDiscovery();
    if (liveVk_) { liveVk_->setStream(-1, nullptr); liveVk_->setRecord(-1, nullptr); }
    obs_.stop();
    engine_.close();
}

// ---------------------------------------------------------------- OBS browser source

QString PlayerBridge::obsUrl() const { return QString("http://127.0.0.1:%1/").arg(obsPort_); }

void PlayerBridge::setObsEnabled(bool v) {
    obsError_.clear();
    if (v) {
        QString e;
        if (!obs_.start(obsPort_, &e)) {
            obsError_ = L("Could not open port %1: ", "Kunne ikke åbne port %1: ").arg(obsPort_) + e;
            v = false;
        }
    } else {
        obs_.stop();
    }
    obsEnabled_ = v;
    QSettings().setValue("obs/enabled", v);
    updateObsSource();
    emit obsChanged();
}

void PlayerBridge::setObsHeight(int h) {
    h = std::clamp(h, 0, 4320);
    obs_.setMaxHeight(h);
    QSettings().setValue("obs/height", h);
    updateObsSource();
    emit obsChanged();
}

// The live (GPU-direct) path copies frames back from the GPU only while OBS is actually watching.
void PlayerBridge::updateObsSource() {
    if (!liveVk_) return;
    const int want = obs_.wantsFrames() ? obs_.maxHeight() : -1;
    if (want == obsStreamH_) return;
    obsStreamH_ = want;
    liveVk_->setStream(want, want < 0 ? std::function<void(QImage, int)>() : [this](QImage img, int delayMs) { obs_.pushFrame(std::move(img), delayMs); });
}

// ---------------------------------------------------------------- instant replay / recording / screenshots

QString PlayerBridge::recFolder(bool pictures) const {
    QString base = QStandardPaths::writableLocation(pictures ? QStandardPaths::PicturesLocation : QStandardPaths::MoviesLocation);
    if (base.isEmpty()) base = QDir::homePath();
    const QString dir = QSettings().value(pictures ? "rec/shotFolder" : "rec/folder", base + "/VeyAmux").toString();
    QDir().mkpath(dir);
    return dir;
}

static QString stamp() { return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss"); }

void PlayerBridge::updateRecordSource() {
    if (!liveVk_) return;
    const int want = rec_->active() ? recHeight_ : -1;
    if (want == recStreamH_) return;
    recStreamH_ = want;
    liveVk_->setRecord(want, want < 0 ? std::function<void(veyamux::EnhanceChain::RecFrame&&)>()
                                      : [this](veyamux::EnhanceChain::RecFrame&& f) { rec_->pushVideo(std::move(f.nv12), f.w, f.h, f.t); });
}

void PlayerBridge::setReplay(bool v) {
    replay_ = v;
    rec_->setReplay(v, replaySeconds_);
    QSettings().setValue("rec/replay", v);
    updateRecordSource();
    emit recChanged();
}

void PlayerBridge::setReplaySeconds(int s) {
    replaySeconds_ = std::clamp(s, 10, 300);
    QSettings().setValue("rec/replaySeconds", replaySeconds_);
    if (replay_) rec_->setReplay(true, replaySeconds_);
    emit recChanged();
}

void PlayerBridge::setRecHeight(int h) {
    recHeight_ = std::clamp(h, 0, 4320);
    QSettings().setValue("rec/height", recHeight_);
    recStreamH_ = -2;
    updateRecordSource();
    emit recChanged();
}

QString PlayerBridge::recInfo() const {
    const QString enc = QString::fromStdString(rec_->encoderName());
    const QString e = enc.isEmpty() ? QString() : enc.contains("vaapi") ? enc.section('_', 0, 0).toUpper() + L(" (GPU, VA-API)", " (GPU, VA-API)")
                                                                         : enc + L(" (CPU)", " (CPU)");
    QStringList parts;
    if (rec_->streaming()) {
        const int s = int(rec_->streamSeconds());
        parts << L("● LIVE %1:%2", "● LIVE %1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    }
    if (rec_->recording()) {
        const int s = int(rec_->recordingSeconds());
        parts << L("● REC %1:%2", "● OPTAGER %1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    }
    if (!e.isEmpty()) parts << e;
    return parts.join("  ·  ");
}

void PlayerBridge::saveReplay() {
    if (!replay_) { showNotice(L("Instant replay is off", "Instant replay er slået fra")); return; }
    const QString path = recFolder(false) + "/VeyAmux-replay_" + stamp() + ".mp4";
    if (!rec_->saveReplay(path.toStdString())) showNotice(L("Nothing to save yet", "Intet at gemme endnu"));
    else showNotice(L("Saving replay…", "Gemmer replay…"));
}

void PlayerBridge::toggleRecording() {
    if (comparing_) { stopCompare(); return; }
    if (rec_->recording()) {
        rec_->stopRecording();
    } else {
        if (!engine_.isOpen() && !liveVulkanActive()) { showNotice(L("Nothing is playing", "Der afspilles ikke noget")); return; }
        recPath_ = recFolder(false) + "/VeyAmux_" + stamp() + ".mp4";
        markers_.clear();
        rec_->startRecording(recPath_.toStdString());
    }
    updateRecordSource();
    emit recChanged();
    emit recTick();
}

// ------------------------------------------------------------------ comparison clips and clip markers

// A badge for a comparison clip: rounded, drawn at 1080p size (the shader scales it with the clip).
QImage PlayerBridge::compareLabel(const QString& text, bool accent) const {
    QFont f(QStringLiteral("sans-serif"));
    f.setPixelSize(30);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);
    const int w = fm.horizontalAdvance(text) + 44, h = fm.height() + 20;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    if (accent) {
        QLinearGradient g(0, 0, w, 0);
        g.setColorAt(0, QColor(255, 122, 69));
        g.setColorAt(1, QColor(255, 61, 127));
        p.setBrush(g);
    } else {
        p.setBrush(QColor(12, 12, 16, 215));
    }
    p.drawRoundedRect(QRectF(0, 0, w, h), h / 2.0, h / 2.0);
    p.setFont(f);
    p.setPen(accent ? QColor(20, 10, 16) : QColor(240, 240, 245));
    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, text);
    p.end();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

// A comparison clip: a normal recording of a picture that holds the original next to VeyAmux's (EnhanceChain).
// The original is the capture as it arrived, before upscaling, noise reduction, sharpening and colour. Lossless
// Scaling's frames are made after VeyAmux, so they are in no recording; FSR frame generation's are not recorded either.
void PlayerBridge::toggleCompare() {
    if (comparing_) { stopCompare(); return; }
    if (!liveVulkanActive()) { showNotice(L("Comparison clips need a live picture (capture card, PlayStation or the demo)",
                                            "Sammenligningsklip kræver et live-billede (capture-kort, PlayStation eller demoen)")); return; }
    if (rec_->recording()) { showNotice(L("Stop the recording first", "Stop optagelsen først")); return; }
    const auto c = engine_.liveContent();
    const int fps = int(std::lround(c.contentFps > 1 ? c.contentFps : engine_.info().fps));
    QString what;
    if (s_.enabled && s_.scale > 1.001f) {
        static const char* names[] = {"SGSR", "FSR 3.1", "FSR 1", "NIS", "DLSS SR"};
        what = QString(" · %1 %2×").arg(names[std::clamp(int(s_.upscaler), 0, 4)]).arg(double(s_.scale));
    }
    const QString a = L("Original", "Original") + (fps > 0 ? QString(" · %1 fps").arg(fps) : QString());
    const QString b = "VeyAmux" + what;
    liveVk_->setCompare(compareLayout_, compareLabel(a, false), compareLabel(b, true));
    recPath_ = recFolder(false) + "/VeyAmux-compare_" + stamp() + ".mp4";
    markers_.clear();
    rec_->startRecording(recPath_.toStdString());
    comparing_ = true;
    if (!compareTimer_) {
        compareTimer_ = new QTimer(this);
        compareTimer_->setSingleShot(true);
        connect(compareTimer_, &QTimer::timeout, this, [this] { if (comparing_) stopCompare(); });
    }
    compareTimer_->start(compareSeconds_ * 1000);
    updateRecordSource();
    showNotice(L("Recording a comparison clip (%1 s)", "Optager et sammenligningsklip (%1 s)").arg(compareSeconds_));
    emit recChanged();
    emit recTick();
}

void PlayerBridge::stopCompare() {
    comparing_ = false;
    if (compareTimer_) compareTimer_->stop();
    if (rec_->recording()) rec_->stopRecording();
    if (liveVk_) liveVk_->setCompare(0);
    updateRecordSource();
    emit recChanged();
    emit recTick();
}

// Clip markers (after PushFPS 4.1): "<recording>-markers.txt" beside the recording, one line a marker, written again
// at every marker so nothing is lost if the recording ends badly. The "H:MM:SS.t  Marker n" form is what video sites
// take for chapters.
void PlayerBridge::addMarker() {
    if (!rec_->recording() || recPath_.isEmpty()) { showNotice(L("Markers are set while recording (F10)", "Markører sættes under en optagelse (F10)")); return; }
    const double sec = rec_->recordingSeconds();
    markers_ << sec;
    auto fmt = [](double s) {
        const int tenths = int(std::lround(std::max(0.0, s) * 10));
        const int h = tenths / 36000, m = tenths / 600 % 60, sc = tenths / 10 % 60, t = tenths % 10;
        return QString("%1:%2:%3.%4").arg(h).arg(m, 2, 10, QChar('0')).arg(sc, 2, 10, QChar('0')).arg(t);
    };
    QString text = "VeyAmux markers for " + QFileInfo(recPath_).fileName() + "\n";
    for (int i = 0; i < markers_.size(); ++i) text += fmt(markers_[i]) + "  Marker " + QString::number(i + 1) + "\n";
    QFile f(recPath_.left(recPath_.size() - 4) + "-markers.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
    showNotice(L("Marker %1 · %2", "Markør %1 · %2").arg(markers_.size()).arg(fmt(sec)));
    emit recTick();
}

// Frame-time overlay: the last ~2 s of on-screen picture intervals, plus fps, 1 % low, jitter and the delay in VeyAmux.
void PlayerBridge::updateOverlay() {
    std::vector<double> t;
    if (liveVulkanActive()) t = liveVk_->shownTimes();
    else { std::lock_guard l(fileShownM_); t.assign(fileShown_.begin(), fileShown_.end()); }
    const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<double> iv;
    for (size_t i = 1; i < t.size(); ++i)
        if (now - t[i] < 2000.0) iv.push_back(t[i] - t[i - 1]);
    if (iv.size() > 240) iv.erase(iv.begin(), iv.end() - 240);
    overlayGraph_.clear();
    for (double v : iv) overlayGraph_ << v;
    if (iv.size() < 4) {
        overlayStats_ = L("Frame times: waiting for pictures", "Billedtider: venter på billeder");
        overlayTarget_ = 0;
    } else {
        double sum = 0;
        for (double v : iv) sum += v;
        const double mean = sum / iv.size();
        double var = 0;
        for (double v : iv) var += (v - mean) * (v - mean);
        std::vector<double> sorted = iv;
        std::sort(sorted.begin(), sorted.end());
        const double p99 = sorted[std::min(sorted.size() - 1, size_t(sorted.size() * 0.99))];
        overlayTarget_ = mean;
        QString s = L("%1 fps · %2 ms · 1%-low %3 fps · jitter %4 ms")
                        .arg(1000.0 / mean, 0, 'f', 1).arg(mean, 0, 'f', 2).arg(1000.0 / p99, 0, 'f', 0).arg(std::sqrt(var / iv.size()), 0, 'f', 2);
        if (s_.frameGen == veyamux::FrameGenMode::NvidiaVfg && liveVulkanActive()) {
            const auto n = liveVk_->nvidiaFrameStats();
            s += L("\nNVIDIA VFG: %1 real + %2 generated fps")
                     .arg(n.realFps, 0, 'f', 1).arg(n.generatedFps, 0, 'f', 1);
        }
        if (liveVulkanActive() && liveVk_->pipelineMs() > 0) s += L(" · delay %1 ms", " · forsinkelse %1 ms").arg(liveVk_->pipelineMs(), 0, 'f', 1);
        const auto st = engine_.stats();
        if (st.dropped) s += L(" · %1 dropped", " · %1 tabt").arg(st.dropped);
        if (engine_.isLive() && s_.pacingRepair && s_.frameGen != veyamux::FrameGenMode::Off) {
            if (!pacingClock_.isValid() || pacingClock_.elapsed() >= 1000) {
                pacingShown_ = engine_.pacingRepairShare();
                pacingClock_.start();
            }
            if (pacingShown_ >= 0)
                s += L("\nFrame pacing repair: %1 % of the pictures moved to an even rhythm (+1 refresh of delay)",
                       "\nRetter billedrytme: %1 % af billederne flyttet til en jævn rytme (+1 skærmopdatering forsinkelse)")
                         .arg(int(std::lround(pacingShown_ * 100)));
        }
        // Remote Play: the delay from the console, as far as it can be measured (the console's own encoding cannot;
        // a PS5 / PS4 typically needs 5-10 ms for it)
        if (const auto rp = veyamux::remoteplay::stats(); rp.active) {
            const double vey = liveVulkanActive() ? liveVk_->pipelineMs() : 0;
            const double net = rp.rttMs / 2;
            QString r = L("\nRemote Play: network %1 ms (round trip %2) · decoding %3 ms · VeyAmux %4 ms",
                          "\nRemote Play: netværk %1 ms (rundtur %2) · afkodning %3 ms · VeyAmux %4 ms")
                            .arg(net, 0, 'f', 1).arg(rp.rttMs, 0, 'f', 1).arg(rp.decodeMs, 0, 'f', 1).arg(vey, 0, 'f', 1);
            r += L("\ntotal ≈ %1 ms (+ console ~8 ms) · %2 Mbit/s · loss %3 %", "\ni alt ≈ %1 ms (+ konsol ~8 ms) · %2 Mbit/s · tab %3 %")
                     .arg(net + rp.decodeMs + vey, 0, 'f', 0).arg(rp.mbit, 0, 'f', 1).arg(rp.loss * 100.0, 0, 'f', 1);
            if (rp.framesLost) r += L(" · %1 frames lost", " · %1 billeder tabt").arg(rp.framesLost);
            r += rp.hwDecode ? L(" · decoded on the GPU (VA-API)", " · afkodet på grafikkortet (VA-API)")
                             : L(" · decoded on the CPU", " · afkodet på processoren");
            s += r;
        }
        overlayStats_ = s;
    }
    emit overlayTick();
}

int PlayerBridge::edidPeakFromBytes(const QByteArray& e) {
    if (e.size() < 256) return 0;
    const int exts = uint8_t(e[126]);
    for (int x = 1; x <= exts && (x + 1) * 128 <= e.size(); ++x) {
        const int b = x * 128;
        if (uint8_t(e[b]) != 0x02) continue;              // CTA-861 extension
        const int end = b + uint8_t(e[b + 2]);
        for (int p = b + 4; p < end && p < b + 127;) {
            const int tag = uint8_t(e[p]) >> 5, len = uint8_t(e[p]) & 31;
            // extended tag 6 = HDR static metadata: [6, EOTFs, descriptors, max luminance, max frame-average, min]
            if (tag == 7 && len >= 4 && uint8_t(e[p + 1]) == 6) {
                const int cv = uint8_t(e[p + 4]);
                if (cv > 0) return int(std::lround(50.0 * std::pow(2.0, cv / 32.0)));
            }
            p += 1 + len;
        }
    }
    return 0;
}

int PlayerBridge::edidPeakNits(const QString& connector) {
    if (connector.isEmpty()) return 0;
    for (const QString& d : QDir("/sys/class/drm").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!d.endsWith("-" + connector)) continue;
        QFile f("/sys/class/drm/" + d + "/edid");
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (const int n = edidPeakFromBytes(f.readAll())) return n;
    }
    return 0;
}

void PlayerBridge::refreshEdidPeak() {
    QScreen* sc = window_ ? window_->screen() : QGuiApplication::primaryScreen();
    edidPeak_ = sc ? edidPeakNits(sc->name()) : 0;
    if (peakAuto_) s_.displayPeak = float(edidPeak_ > 0 ? std::clamp(edidPeak_, 203, 10000) : 1000);
}

void PlayerBridge::setDisplayPeak(int v) {
    peakAuto_ = v <= 0;
    QSettings().setValue("enh/displayPeakAuto", peakAuto_);
    if (peakAuto_) refreshEdidPeak();
    else { s_.displayPeak = float(std::clamp(v, 203, 10000)); QSettings().setValue("enh/displayPeak", int(std::lround(s_.displayPeak))); }
    applyEnhance();
}

void PlayerBridge::toggleStream() {
    if (rec_->streaming()) {
        rec_->stopStream();
    } else {
        if (!engine_.isOpen() && !liveVulkanActive()) { showNotice(L("Nothing is playing", "Der afspilles ikke noget")); return; }
        static const char* bases[] = {"rtmp://live.twitch.tv/app/", "rtmp://a.rtmp.youtube.com/live2/"};
        const int svc = streamService();
        QString url = svc < 2 ? QString(bases[svc]) + streamKey() : streamServer();
        if (svc == 2 && !streamKey().isEmpty()) url += (url.endsWith('/') ? "" : "/") + streamKey();
        if ((svc < 2 && streamKey().isEmpty()) || url.isEmpty()) { showNotice(L("Enter the stream key first", "Indtast stream-nøglen først")); return; }
        showNotice(L("Connecting…", "Forbinder …"));
        rec_->startStream(url.toStdString(), streamKbps());
    }
    updateRecordSource();
    emit recChanged();
    emit recTick();
}

void PlayerBridge::screenshot() {
    const QString path = recFolder(true) + "/VeyAmux_" + stamp() + ".png";
    auto save = [this, path](QImage img) {
        if (s_.hdrOut) img = sdrCopy(img);
        const bool ok = !img.isNull() && img.convertToFormat(QImage::Format_RGB888).save(path, "PNG");
        QMetaObject::invokeMethod(this, [this, ok, path] {
            showNotice(ok ? L("Screenshot saved: %1", "Skærmbillede gemt: %1").arg(QFileInfo(path).fileName())
                          : L("Could not save the screenshot", "Kunne ikke gemme skærmbilledet"));
        }, Qt::QueuedConnection);
    };
    if (liveVulkanActive()) { liveVk_->requestScreenshot(save); return; }
    QImage img;
    { QMutexLocker l(&imgMutex_); img = image_; }
    save(img);
}

QImage PlayerBridge::sdrCopy(QImage img) const {
    // The same as PqCodedToSdr() in shaders/PqCode.hlsli: PQ BT.2020 -> nits -> BT.709 relative to 203 nits, soft
    // shoulder on the luminance, Veyra's hue-preserving gamut compression (ToneGamut), sRGB.
    static const std::array<float, 256> nits = [] {
        std::array<float, 256> t{};
        const double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
        for (int i = 0; i < 256; ++i) {
            const double e = std::pow(i / 255.0, 1.0 / m2);
            t[size_t(i)] = float(10000.0 * std::pow(std::max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1) / 203.0);
        }
        return t;
    }();
    static const std::array<uint8_t, 4096> enc = [] {   // linear [0,1] -> sRGB byte
        std::array<uint8_t, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            const double y = i / 4095.0;
            t[size_t(i)] = uint8_t(std::lround(255.0 * (y <= 0.0031308 ? y * 12.92 : 1.055 * std::pow(y, 1 / 2.4) - 0.055)));
        }
        return t;
    }();
    img = img.convertToFormat(QImage::Format_RGBA8888);
    for (int yy = 0; yy < img.height(); ++yy) {
        uint8_t* p = img.scanLine(yy);
        for (int x = 0; x < img.width(); ++x, p += 4) {
            const float R = nits[p[0]], G = nits[p[1]], B = nits[p[2]];
            float r = 1.660491f * R - 0.587641f * G - 0.072850f * B;
            float g = -0.124550f * R + 1.132900f * G - 0.008349f * B;
            float b = -0.018151f * R - 0.100579f * G + 1.118730f * B;
            const float y = 0.212639f * r + 0.715169f * g + 0.072192f * b;
            if (y <= 0) { p[0] = p[1] = p[2] = 0; continue; }
            const float m = y <= 0.75f ? y : 0.75f + 0.25f * (1.0f - std::exp(-(y - 0.75f) / 0.25f));
            r *= m / y; g *= m / y; b *= m / y;
            // ToneGamut: move along the chroma ray toward grey until every channel fits
            float cr = r - m, cg = g - m, cb = b - m;
            auto occ = [m](float c) { return c >= 0 ? c / std::max(1.0f - m, 1e-7f) : -c / std::max(m, 1e-7f); };
            const float ext = std::max({occ(cr), occ(cg), occ(cb)});
            if (ext > 0.9f) { const float k = (0.9f + 0.1f * (1.0f - std::exp(-(ext - 0.9f) / 0.1f))) / ext; cr *= k; cg *= k; cb *= k; }
            auto e = [](float v) { return enc[size_t(std::clamp(v, 0.0f, 1.0f) * 4095.0f + 0.5f)]; };
            p[0] = e(m + cr); p[1] = e(m + cg); p[2] = e(m + cb);
        }
    }
    return img;
}

void PlayerBridge::addToSteam() {
    // Steam rewrites shortcuts.vdf when it exits, so it is closed first, the shortcut and the library artwork are
    // written, and Steam is started again. Not possible when Steam started VeyAmux (closing Steam closes VeyAmux).
    if (steamBusy_) return;
    namespace st = veyamux::steam;
    const bool running = st::steamRunning();
    if (running && st::startedBySteam()) {
        showNotice(L("VeyAmux was started from Steam, so it cannot close Steam. Start VeyAmux from the desktop and press the button again",
                     "VeyAmux er startet fra Steam og kan derfor ikke lukke Steam. Start VeyAmux fra skrivebordet, og tryk på knappen igen"));
        return;
    }
    if (!running) { finishAddToSteam(false); return; }
    if (!st::shutdownSteam()) {
        showNotice(L("Close Steam first, then press the button again", "Luk Steam først, og tryk så på knappen igen"));
        return;
    }
    steamBusy_ = true;
    showNotice(L("Closing Steam to add VeyAmux…", "Lukker Steam for at tilføje VeyAmux…"));
    auto* t = new QTimer(this);
    auto* clock = new QElapsedTimer;
    clock->start();
    connect(t, &QTimer::timeout, this, [this, t, clock] {
        const bool still = veyamux::steam::steamRunning();
        if (!still || clock->elapsed() > 45000) {
            t->stop();
            t->deleteLater();
            delete clock;
            if (still) {
                steamBusy_ = false;
                showNotice(L("Steam did not close. Close it yourself, then press the button again",
                             "Steam lukkede ikke. Luk det selv, og tryk så på knappen igen"));
                return;
            }
            // give Steam's last file writes a moment
            QTimer::singleShot(1500, this, [this] { finishAddToSteam(true); });
        }
    });
    t->start(500);
}

void PlayerBridge::finishAddToSteam(bool restartSteam) {
    steamBusy_ = false;
    // the AppImage itself (the binary inside lives in a temporary mount)
    const auto run = veyamux::sandbox::launcher();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString icon = dir + "/veyamux-steam.png";
    QIcon(":/qml/icons/logo.svg").pixmap(512, 512).save(icon, "PNG");
    const QString opts = (run.args + QStringList{"--fullscreen", "--capture", "last"}).join(' ');
    const auto r = veyamux::steam::addShortcut(run.program, icon, opts, veyamux::steam::artwork());
    const bool restarted = restartSteam && veyamux::steam::startSteam();
    if (r.ok && restarted) showNotice(L("Added to Steam with cover art, and Steam is starting again", "Tilføjet til Steam med billeder, og Steam starter igen"));
    else if (r.ok) showNotice(L("Added to Steam: start VeyAmux from your library or Game Mode", "Tilføjet til Steam: start VeyAmux fra biblioteket eller Game Mode"));
    else if (r.message == "Close Steam first") showNotice(L("Close Steam first, then press the button again", "Luk Steam først, og tryk så på knappen igen"));
    else showNotice(L("Could not add to Steam: ", "Kunne ikke tilføje til Steam: ") + r.message);
}

void PlayerBridge::openRecordingsFolder() { QDesktopServices::openUrl(QUrl::fromLocalFile(recFolder(false))); }

void PlayerBridge::onRecMessage(const QString& m, bool error) {
    if (m == "stream-started") showNotice(L("Live: the stream is running", "Live: streamen kører"));
    else if (m == "stream-stopped") showNotice(L("Stream stopped", "Stream stoppet"));
    else if (m == "recording-started") showNotice(L("Recording started", "Optagelse startet"));
    else if (m == "recording-saved") { showNotice(L("Recording saved", "Optagelse gemt")); updateRecordSource(); }
    else if (m.startsWith("replay-saved:")) showNotice(L("Replay saved: %1", "Replay gemt: %1").arg(QFileInfo(m.mid(13)).fileName()));
    else showNotice((error ? L("Recording error: ", "Optagefejl: ") : QString()) + m);
    emit recChanged();
    emit recTick();
}

void PlayerBridge::saveObsFile() {
    const QString start = obsFile_.isEmpty() ? QDir::homePath() + "/VeyAmux-OBS.html" : obsFile_;
    QString path = QFileDialog::getSaveFileName(nullptr, L("Save the OBS browser file", "Gem browserfilen til OBS"), start,
                                                L("Web page (*.html)", "Webside (*.html)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".html", Qt::CaseInsensitive)) path += ".html";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        obsError_ = L("Could not save %1: ", "Kunne ikke gemme %1: ").arg(path) + f.errorString();
        emit obsChanged();
        return;
    }
    f.write(ObsStream::pageHtml(QString("http://127.0.0.1:%1/stream.mjpg").arg(obsPort_), "VeyAmux"));
    f.close();
    obsFile_ = path;
    QSettings().setValue("obs/file", path);
    if (!obsEnabled_) setObsEnabled(true);   // the file is only useful while the stream runs
    emit obsChanged();
}

void PlayerBridge::copyObsUrl() { QGuiApplication::clipboard()->setText(obsUrl()); }

void PlayerBridge::loadSettings() {
    QSettings q;
    obsPort_ = quint16(std::clamp(q.value("obs/port", 47920).toInt(), 1024, 65535));
    obs_.setMaxHeight(std::clamp(q.value("obs/height", 1080).toInt(), 0, 4320));
    obsFile_ = q.value("obs/file").toString();
    s_.enabled = q.value("enh/enabled", true).toBool();
    s_.scale = float(q.value("enh/scale", 4.0 / 3.0).toDouble());
    s_.upscaler = veyamux::Upscaler(std::clamp(q.value("enh/upscaler", 4).toInt(), 0, 4));
    s_.fxaa = q.value("enh/fxaa", false).toBool();
    s_.deflicker = q.value("enh/deflicker", false).toBool();
    s_.retroTv = q.value("enh/retroTv", false).toBool();
    { const int l = q.value("enh/retroLines", 240).toInt(); s_.retroLines = (l == 0 || l == 480) ? l : 240; }
    s_.pixelPerfect = q.value("enh/pixelPerfect", false).toBool();
    s_.pixelGrid = std::clamp(q.value("enh/pixelGrid", 0).toInt(), 0, 1);
    s_.denoise = veyamux::Denoise(std::clamp(q.value("enh/denoise", 0).toInt(), 0, 3));
    s_.denoiseStrength = std::clamp(q.value("enh/denoiseStrength", 1).toInt(), 0, 2);
    s_.hdrPeak = float(std::clamp(q.value("enh/hdrPeak", 1000).toInt(), 203, 10000));
    s_.displayPeak = float(std::clamp(q.value("enh/displayPeak", 1000).toInt(), 203, 10000));
    hdrOutWanted_ = q.value("enh/hdrOut", false).toBool();
    s_.vrr = q.value("enh/vrr", false).toBool();
    s_.pacingRepair = q.value("enh/pacingRepair", false).toBool();
    s_.hudLock = q.value("enh/hudLock", false).toBool();
    s_.nvidiaQuality = qBound(0, q.value("enh/nvidiaQuality", 1).toInt(), 2);
    s_.nvidiaAutoRegulate = q.value("enh/nvidiaAutoRegulate", true).toBool();
    // The experimental profile starts with NVIDIA VFG. The old in-app modes (1 AMD FSR / RIFE, 4 RIFE + Lossless Scaling) become 2x.
    {
        int fg = std::clamp(q.value("enh/frameGenMode", 6).toInt(), 0, 6);
        if (fg == 1 || fg == 4) fg = 2;   // 5 = AMD FSR 3.1 frame generation (free)
        s_.frameGen = veyamux::FrameGenMode(fg);
    }
    lsfgPerformance_ = q.value("lsfg/performance", false).toBool();
    s_.fxaaQuality = q.value("enh/fxaaQuality", 1).toInt();
    s_.sharpen = float(q.value("enh/sharpen", 0.0).toDouble());
    s_.exposure = float(q.value("enh/exposure", 0.0).toDouble());
    s_.contrast = float(q.value("enh/contrast", 1.0).toDouble());
    s_.saturation = float(q.value("enh/saturation", 1.0).toDouble());
    s_.smaa = q.value("enh/smaa", false).toBool();
    s_.texture = float(std::clamp(q.value("enh/texture", 0.0).toDouble(), -1.0, 1.0));
    s_.clarity = float(std::clamp(q.value("enh/clarity", 0.0).toDouble(), -1.0, 1.0));
    s_.dehaze = float(std::clamp(q.value("enh/dehaze", 0.0).toDouble(), -1.0, 1.0));
    volume_ = q.value("audio/volume", 1.0).toDouble();
    audioDelay_ = std::clamp(q.value("audio/delayMs", 0).toInt(), -300, 300);
    muted_ = q.value("audio/muted", false).toBool();
}

void PlayerBridge::saveSettings() {
    QSettings q;
    q.setValue("enh/enabled", s_.enabled); q.setValue("enh/scale", double(s_.scale)); q.setValue("enh/upscaler", int(s_.upscaler));
    q.setValue("enh/retroTv", s_.retroTv); q.setValue("enh/retroLines", s_.retroLines);
    q.setValue("enh/pixelPerfect", s_.pixelPerfect); q.setValue("enh/pixelGrid", s_.pixelGrid);
    q.setValue("enh/fxaa", s_.fxaa); q.setValue("enh/deflicker", s_.deflicker); q.setValue("enh/denoise", int(s_.denoise)); q.setValue("enh/denoiseStrength", s_.denoiseStrength); q.setValue("enh/hdrPeak", hdrPeak()); q.setValue("enh/vrr", s_.vrr); q.setValue("enh/frameGenMode", int(s_.frameGen)); q.setValue("enh/fxaaQuality", s_.fxaaQuality);
    q.setValue("enh/sharpen", double(s_.sharpen)); q.setValue("enh/exposure", double(s_.exposure));
    q.setValue("enh/contrast", double(s_.contrast)); q.setValue("enh/saturation", double(s_.saturation));
    q.setValue("enh/smaa", s_.smaa); q.setValue("enh/texture", double(s_.texture)); q.setValue("enh/clarity", double(s_.clarity)); q.setValue("enh/dehaze", double(s_.dehaze));
    q.setValue("audio/volume", volume_); q.setValue("audio/muted", muted_);
}

// The rate frame generation starts from: for capture with frame generation on, repeated pictures are dropped, so it
// is the game's measured frame rate; otherwise the source frame rate.
double PlayerBridge::frameGenBaseFps() const {
    if (!engine_.isOpen()) return 60.0;
    if (engine_.isLive() && s_.frameGen != veyamux::FrameGenMode::Off) {
        const auto c = engine_.liveContent();
        if (c.contentFps > 1.0) return c.contentFps;
    }
    return engine_.info().fps > 0 ? engine_.info().fps : 60.0;
}

void PlayerBridge::updateOutputFps(uint64_t shownTotal) {
    if (!fpsClock_.isValid()) fpsClock_.start();
    const qint64 now = fpsClock_.elapsed();
    if (fpsT0_ == 0 || shownTotal < fpsCount_) { fpsT0_ = now; fpsCount_ = shownTotal; return; }
    const qint64 dt = now - fpsT0_;
    if (dt < 1000) return;
    const double fps = double(shownTotal - fpsCount_) * 1000.0 / double(dt);
    fpsT0_ = now; fpsCount_ = shownTotal;
    const double old = outFps_;
    outFps_ = engine_.playing() ? fps : 0.0;
    if (qEnvironmentVariableIsSet("VEYAMUX_DEBUG_FPS")) qInfo("output fps %.1f (playing %d)", fps, int(engine_.playing()));
    if (std::lround(old) != std::lround(outFps_)) emit mediaChanged();
}

// Timing lines under the menu: measured GPU time (Vulkan timestamps) per stage, the CPU time to record the work and,
// live, VeyAmux's share of the latency (capture driver timestamp -> GPU work done).
// One number for the menu: live = capture driver timestamp -> picture finished on the GPU (what VeyAmux adds before the
// display), files = processing time per picture. Averaged and refreshed once a second, so it is readable (and frame
// generation does not blend two different numbers into each other).
QString PlayerBridge::delayText(double ms, bool live, QSize size) {
    delaySum_ += ms; ++delayN_;
    if (delayShown_ < 0 || delayClock_.elapsed() >= 1000 || !delayClock_.isValid()) {
        delayShown_ = delaySum_ / std::max(1, delayN_);
        delaySum_ = 0; delayN_ = 0;
        delayClock_.start();
    }
    return (live ? L("Delay in VeyAmux: %1 ms", "Forsinkelse i VeyAmux: %1 ms") : L("Processing: %1 ms per picture", "Behandling: %1 ms pr. billede"))
               .arg(delayShown_, 0, 'f', 1)
           + QString("  (%1×%2)").arg(size.width()).arg(size.height());
}

QString PlayerBridge::timingText(float gpu, const std::array<float, veyamux::GpuTimer::kStages>& st, double cpuMs, double pipeMs) const {
    QString t;
    if (gpu >= 0) t += QString(" · GPU %1 ms").arg(double(gpu), 0, 'f', 2);
    t += QString(" · CPU %1 ms").arg(cpuMs, 0, 'f', 2);
    if (pipeMs > 0) t += L(" · in VeyAmux %1 ms", " · i VeyAmux %1 ms").arg(pipeMs, 0, 'f', 1);
    if (gpu >= 0) {
        static const char* en[] = {"input", "noise red.", "FXAA", "upscale", "finish", "OBS"};
        static const char* da[] = {"ind", "støjred.", "FXAA", "opskal.", "afslut.", "OBS"};
        QStringList parts;
        for (int i = 0; i < veyamux::GpuTimer::kStages; ++i)
            if (st[size_t(i)] >= 0) parts << QString("%1 %2").arg(L(en[i], da[i])).arg(double(st[size_t(i)]), 0, 'f', 2);
        if (!parts.isEmpty()) t += "\n" + parts.join(" · ");
    }
    return t;
}

// A 120 Hz capture (signal mode or a game running at ~120 fps) needs no generated frames: 2x would ask for 240 fps,
// more than the display shows, and only adds latency and GPU load. Frame generation is paused (multiplier 1, the
// setting itself is kept) after 1 s at >= 95 fps and resumes after 2 s below 80 fps.
void PlayerBridge::updatePixelGrid() {
    if (!s_.pixelActive() || s_.pixelGrid != 1 || !engine_.isOpen() || !engine_.isLive()) { gridStable_ = 0; return; }
    const auto c = engine_.liveContent();
    const int w = c.estW, h = c.estH;
    if (w <= 0 || h <= 0) return;
    if (w != gridCandW_ || h != gridCandH_) { gridCandW_ = w; gridCandH_ = h; gridStable_ = 0; return; }
    if (++gridStable_ < 15 || (w == s_.gridW && h == s_.gridH)) return;   // 15 x 100 ms
    s_.gridW = w;
    s_.gridH = h;
    applyEnhance();
}

void PlayerBridge::checkHighRate() {
    const bool live = engine_.isOpen() && engine_.isLive();
    double rate = 0;
    if (live) {
        const auto c = engine_.liveContent();
        // a shared window/screen announces the display's maximum rate (144, 240 ...): only its measured rate counts
        const bool share = engine_.info().videoCodec == "PipeWire";
        rate = std::max(share ? 0.0 : engine_.info().fps, c.staticPicture ? 0.0 : c.contentFps);
    }
    const bool high = live && rate >= 95.0;
    const bool low = !live || rate < 80.0;
    fgHighTicks_ = high ? fgHighTicks_ + 1 : 0;
    fgLowTicks_ = low ? fgLowTicks_ + 1 : 0;
    bool change = false;
    if (!fgPaused_ && fgHighTicks_ >= 10) { fgPaused_ = true; change = true; }
    if (fgPaused_ && fgLowTicks_ >= 20) { fgPaused_ = false; change = true; }
    if (!change) return;
    s_.fgPaused = fgPaused_;
    applyEnhance();
    if (s_.lsfgFg() && lsfgActive_) {
        // lsfg-vk cannot pause: say once that it keeps going and how to stop it
        if (fgPaused_ && !lsfgHighNoticed_) {
            lsfgHighNoticed_ = true;
            showNotice(L("120 Hz signal: Lossless Scaling keeps generating frames. Set frame generation to Off to stop it",
                         "120 Hz-signal: Lossless Scaling laver stadig billeder. Sæt frame generation til Fra for at stoppe den"));
        }
    } else if (s_.frameGen == veyamux::FrameGenMode::FsrFree || s_.frameGen == veyamux::FrameGenMode::NvidiaVfg) {
        showNotice(fgPaused_ ? L("120 Hz signal: frame generation paused", "120 Hz-signal: frame generation sat på pause")
                             : L("Frame generation resumed", "Frame generation kører igen"));
    }
    updateLsfgMultiplier();
    emit lsfgChanged();
    emit mediaChanged();
}

// Frame generation that asks for more pictures than the screen can show (a 40 fps game x 2 = 80 on a 60 Hz screen, or
// 30 x 3 on 60 Hz): lsfg-vk presents in FIFO, so its frames queue up behind the refresh, the delay grows and frames are
// thrown away unevenly; FSR's are dropped. After PushFPS 4.1's display cap. Shown after 3 s, cleared after 3 s below.
void PlayerBridge::checkOverRefresh() {
    int mult = 1;
    if (engine_.isOpen() && engine_.isLive() && !fgPaused_) {
        if (s_.lsfgFg() && lsfgActive_) mult = lsfgMultiplier_;
        else if ((s_.fsrFg() || nvidiaVfgActive()) && liveVulkanActive()) mult = 2;
    }
    const double hz = displayHz();
    const double base = mult > 1 ? frameGenBaseFps() : 0.0;
    const bool over = mult > 1 && hz > 1 && base * mult > hz * 1.05;
    const bool shown = !fgOverRefresh_.isEmpty();
    overRefreshTicks_ = over != shown ? overRefreshTicks_ + 1 : 0;
    if (overRefreshTicks_ < 30) return;
    overRefreshTicks_ = 0;
    if (over) {
        const int out = int(std::lround(base * mult)), screen = int(std::lround(hz));
        // the multiplier that fits, if any
        const int fit = int(std::floor(hz * 1.05 / std::max(1.0, base)));
        QString tip;
        if (s_.lsfgFg() && mult == 3 && fit >= 2) tip = L(" Use Fixed 2×, or a screen with a higher refresh rate.", " Brug Fast 2×, eller en skærm med højere opdateringsfrekvens.");
        else tip = L(" Set the screen to a higher refresh rate, or turn frame generation off for this game.",
                     " Sæt skærmen til en højere opdateringsfrekvens, eller slå frame generation fra til dette spil.");
        fgOverRefresh_ = L("%1 fps × %2 = %3 fps, but the screen shows %4 Hz: frames queue up and are dropped.",
                           "%1 fps × %2 = %3 fps, men skærmen viser %4 Hz: billederne hober sig op og smides væk.")
                             .arg(std::lround(base)).arg(mult).arg(out).arg(screen) + tip;
        if (!overRefreshNoticed_) { overRefreshNoticed_ = true; showNotice(fgOverRefresh_); }
    } else {
        fgOverRefresh_.clear();
    }
    emit lsfgChanged();
}

// "→ 120 fps" after the source rate when frame generation raises it: source rate x multiplier for Lossless Scaling
// (lsfg-vk generates its frames after VeyAmux, in the driver layer).
QString PlayerBridge::outputFpsText(double srcFps) const {
    const bool ls = s_.lsfgFg();
    if (ls && lsfgActive_ && lsfgMultiplier_ > 1 && srcFps > 0)
        return QString(" → %1 fps (Lossless Scaling %2×)").arg(std::lround(frameGenBaseFps() * lsfgMultiplier_)).arg(lsfgMultiplier_);
    if (nvidiaVfgActive() && srcFps > 0) return QStringLiteral(" · NVIDIA VFG 2×");
    if (s_.fsrFg() && liveVulkanActive() && srcFps > 0)
        return QString(" → %1 fps (AMD FSR 3.1 2×)").arg(std::lround(frameGenBaseFps() * 2));
    return {};
}

QString PlayerBridge::mediaInfo() const {
    const auto& i = engine_.info();
    if (!engine_.isOpen()) return {};
    QStringList p;
    if (i.hasVideo && engine_.isLive()) {
        // A capture card always sends its fixed signal (e.g. 3840x2160 @ 60): lead with what the game actually renders
        // (measured frame rate, estimated resolution with "~"), the signal after it.
        const auto c = engine_.liveContent();
        const QString signal = L("signal %1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                                   .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) : QString());
        if (c.contentFps > 0 || c.estW > 0) {
            QString res;
            if (c.estW > 0 && c.estW != i.width) res = QString("≈%1×%2").arg(c.estW).arg(c.estH);
            else res = QString("%1×%2").arg(i.width).arg(i.height);
            QString fps;
            if (c.contentFps > 0) fps = QString(" @ %1 fps").arg(std::lround(c.contentFps));
            if (c.staticPicture) fps += L(" (still picture)", " (stillestående billede)");
            p << res + fps + outputFpsText(i.fps) << signal;
        } else {
            p << QString("%1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                     .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) + outputFpsText(i.fps) : QString());
        }
    } else if (i.hasVideo) {
        p << QString("%1×%2 %3%4").arg(i.width).arg(i.height).arg(QString::fromStdString(i.videoCodec))
                 .arg(i.fps > 0 ? QString(" @ %1 fps").arg(i.fps, 0, 'f', 2) + outputFpsText(i.fps) : QString());
    }
    if (i.hasAudio) p << QString::fromStdString(i.audioCodec);
    if (engine_.hwDecodeActive()) p << "VAAPI";
    return p.join(" · ");
}

QImage PlayerBridge::currentImage() { QMutexLocker l(&imgMutex_); return image_; }

void PlayerBridge::setStatus(const QString& s) { status_ = s; emit statusChanged(); }
void PlayerBridge::showNotice(const QString& s) {
    notice_ = s;
    ++noticeSeq_;
    emit noticeChanged();
    const int seq = noticeSeq_;
    QTimer::singleShot(2500, this, [this, seq] { if (seq == noticeSeq_) { notice_.clear(); emit noticeChanged(); } });
}

bool PlayerBridge::openUrl(const QUrl& url) { return openPath(url.isLocalFile() ? url.toLocalFile() : url.toString()); }

bool PlayerBridge::openPath(const QString& path) {
    std::string err;
    openFile_ = path;
    demoActive_ = false;
    if (liveVk_) liveVk_->setActive(false);
    { QMutexLocker l(&imgMutex_); image_ = QImage(); }
    // the picture goes straight to the window's Vulkan frame when it can (VEYAMUX_FILE_GPU=0: the CPU way)
    const bool zeroCopy = liveVk_ != nullptr && qEnvironmentVariable("VEYAMUX_FILE_GPU") != "0";
    if (!engine_.open(path.toStdString(), err, zeroCopy)) {
        setStatus(translateMessage(QString::fromStdString(err)));
        emit mediaChanged();
        return false;
    }
    if (engine_.isRawLive()) {
        qInfo("Video file: decoded pictures go straight to the GPU (%s decoding)", engine_.hwDecodeActive() ? "VA-API" : "software");
        if (engine_.info().height > 0) aspect_ = double(engine_.info().width) / engine_.info().height;
        liveVk_->setSettings(s_);
        liveVk_->setActive(true);
    }
    updateLsfgMultiplier();
    engine_.setVolume(float(volume_));
    engine_.setAudioDelay(audioDelay_);
    engine_.setMuted(muted_);
    applyEnhance();
    setStatus({});
    emit mediaChanged();
    engine_.play();
    lastPlaying_ = true;
    emit playingChanged();
    return true;
}

void PlayerBridge::openDialog() {
    const QString f = QFileDialog::getOpenFileName(nullptr, L("Open video", "Åbn video"), QString(),
        L("Media (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.m2ts *.wmv *.flv *.mp3 *.flac *.ogg *.opus *.m4a *.wav);;All files (*)",
          "Medier (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.m2ts *.wmv *.flv *.mp3 *.flac *.ogg *.opus *.m4a *.wav);;Alle filer (*)"));
    if (!f.isEmpty()) openPath(f);
}

void PlayerBridge::togglePlay() { engine_.playing() ? pause() : play(); }
void PlayerBridge::play() { engine_.play(); lastPlaying_ = engine_.playing(); emit playingChanged(); }
void PlayerBridge::pause() { engine_.pause(); lastPlaying_ = false; emit playingChanged(); }
void PlayerBridge::seek(double s) { engine_.seek(s); emit positionChanged(); }
void PlayerBridge::seekRelative(double d) { seek(position() + d); }

void PlayerBridge::setVolume(double v) { volume_ = qBound(0.0, v, 1.5); engine_.setVolume(float(volume_)); emit audioChanged(); }
void PlayerBridge::setMuted(bool m) { muted_ = m; engine_.setMuted(m); emit audioChanged(); }

void PlayerBridge::applyEnhance() { engine_.setEnhance(s_); if (liveVk_) liveVk_->setSettings(s_); emit enhanceChanged(); }
void PlayerBridge::setEnhance(bool v) { s_.enabled = v; applyEnhance(); }
void PlayerBridge::setScale(double v) { s_.scale = float(qBound(1.0, v, 4.0)); applyEnhance(); }
void PlayerBridge::setUpscaler(int v) {
    v = std::clamp(v, 0, 4);
    if (int(s_.upscaler) == v) return;
    s_.upscaler = veyamux::Upscaler(v);
    applyEnhance();
}
void PlayerBridge::setFxaa(bool v) { s_.fxaa = v; applyEnhance(); }
void PlayerBridge::setDeflicker(bool v) { s_.deflicker = v; applyEnhance(); }
void PlayerBridge::setFrameGenMode(int v) {
    v = std::clamp(v, 0, 6);
    if (v == 1 || v == 4) v = 2;
    const bool wasLs = s_.lsfgFg();
    s_.frameGen = veyamux::FrameGenMode(v);
    const bool isLs = v == 2 || v == 3;
    { QSettings q; q.setValue("enh/frameGenMode", v); }
    if (lsfgActive_ && isLs) {
        // lsfg-vk hot-reloads its config: 2x <-> 3x changes while it runs
        lsfgMultiplier_ = v == 3 ? 3 : 2;
        lsfg::writeConfig(lsfg_, lsfgMultiplier_, lsfgPerformance_, lsfgFlowScale());
    }
    applyEnhance();
    if (isLs && !wasLs) { vrrNoticed_ = false; checkDesktopVrr(); }   // the desktop's VRR may have changed meanwhile
    else emit lsfgChanged();   // lsfgVrrWarning follows the mode
    // lsfg-vk 2 cannot be switched off while it runs (a multiplier of 1 is refused and the old config stays; without
    // a matching profile the window stops presenting): VeyAmux starts again without it, on the same source.
    if (lsfgActive_ && !isLs && !lsfgOffPending_) {
        lsfgOffPending_ = true;
        saveSettings();
        showNotice(L("Turning Lossless Scaling off: VeyAmux restarts…", "Slår Lossless Scaling fra: VeyAmux genstarter…"));
        QTimer::singleShot(700, this, [this] { restartWithSource(); });
    }
    emit mediaChanged();   // the "→ N fps" part of the media line
    if (wasLs != isLs) emit lsfgChanged();
    emit lsfgChanged();
}
void PlayerBridge::setFxaaQuality(int v) { s_.fxaaQuality = qBound(0, v, 2); applyEnhance(); }
void PlayerBridge::setSharpen(double v) { s_.sharpen = float(qBound(0.0, v, 1.0)); applyEnhance(); }
void PlayerBridge::setExposure(double v) { s_.exposure = float(qBound(-3.0, v, 3.0)); applyEnhance(); }
void PlayerBridge::setContrast(double v) { s_.contrast = float(qBound(0.6, v, 1.4)); applyEnhance(); }
void PlayerBridge::setSaturation(double v) { s_.saturation = float(qBound(0.0, v, 2.0)); applyEnhance(); }
void PlayerBridge::resetEnhance() {
    // picture settings only: frame generation and VRR (which may need a restart) are kept
    const auto keep = s_;
    s_ = veyamux::EnhanceSettings{};
    s_.enabled = keep.enabled; s_.frameGen = keep.frameGen; s_.vrr = keep.vrr; s_.hdrPeak = keep.hdrPeak; s_.hdrOut = keep.hdrOut; s_.displayPeak = keep.displayPeak; s_.fgPaused = keep.fgPaused;
    s_.pacingRepair = keep.pacingRepair; s_.hudLock = keep.hudLock; s_.fitW = keep.fitW; s_.fitH = keep.fitH;
    s_.nvidiaQuality = keep.nvidiaQuality;
    s_.nvidiaAutoRegulate = keep.nvidiaAutoRegulate;
    activeProfile_.clear();
    emit profilesChanged();
    applyEnhance();
}

// ------------------------------------------------------------------ profiles

QStringList PlayerBridge::profiles() const {
    QSettings q;
    QStringList order = q.value("profiles/order").toStringList();
    q.beginGroup("profiles");
    const QStringList groups = q.childGroups();
    q.endGroup();
    QStringList out;
    for (const auto& n : order) if (groups.contains(QString(QUrl::toPercentEncoding(n)))) out << n;
    return out;
}

QString PlayerBridge::signalKey() const {
    if (!engine_.isLive()) return {};
    const auto& i = engine_.info();
    return QString("%1x%2@%3 %4").arg(i.width).arg(i.height).arg(std::lround(i.fps)).arg(QString::fromStdString(i.videoCodec));
}

QString PlayerBridge::profileSignal(const QString& name) const {
    return QSettings().value("profiles/" + QString(QUrl::toPercentEncoding(name)) + "/signal").toString();
}

void PlayerBridge::saveProfile(const QString& rawName, bool bindToSignal) {
    const QString name = rawName.trimmed().left(24);
    if (name.isEmpty()) return;
    QSettings q;
    const QString g = "profiles/" + QString(QUrl::toPercentEncoding(name)) + "/";
    q.setValue(g + "enabled", s_.enabled); q.setValue(g + "scale", double(s_.scale)); q.setValue(g + "upscaler", int(s_.upscaler));
    q.setValue(g + "fxaa", s_.fxaa); q.setValue(g + "fxaaQuality", s_.fxaaQuality); q.setValue(g + "deflicker", s_.deflicker);
    q.setValue(g + "retroTv", s_.retroTv); q.setValue(g + "retroLines", s_.retroLines);
    q.setValue(g + "pixelPerfect", s_.pixelPerfect); q.setValue(g + "pixelGrid", s_.pixelGrid);
    q.setValue(g + "denoise", int(s_.denoise)); q.setValue(g + "denoiseStrength", s_.denoiseStrength);
    q.setValue(g + "hdrPeak", hdrPeak());
    q.setValue(g + "sharpen", double(s_.sharpen)); q.setValue(g + "exposure", double(s_.exposure));
    q.setValue(g + "contrast", double(s_.contrast)); q.setValue(g + "saturation", double(s_.saturation));
    q.setValue(g + "smaa", s_.smaa); q.setValue(g + "texture", double(s_.texture)); q.setValue(g + "clarity", double(s_.clarity)); q.setValue(g + "dehaze", double(s_.dehaze));
    q.setValue(g + "audioDelay", audioDelay_);
    const QString sig = bindToSignal ? signalKey() : QString();
    if (!sig.isEmpty()) {   // one profile per signal: unbind the others
        for (const auto& other : profiles())
            if (other != name && profileSignal(other) == sig) q.remove("profiles/" + QString(QUrl::toPercentEncoding(other)) + "/signal");
        q.setValue(g + "signal", sig);
    } else {
        q.remove(g + "signal");
    }
    QStringList order = q.value("profiles/order").toStringList();
    if (!order.contains(name)) order << name;
    q.setValue("profiles/order", order);
    activeProfile_ = name;
    emit profilesChanged();
}

bool PlayerBridge::loadProfile(const QString& name) {
    QSettings q;
    const QString g = "profiles/" + QString(QUrl::toPercentEncoding(name)) + "/";
    if (!q.contains(g + "scale")) return false;
    s_.enabled = q.value(g + "enabled", true).toBool();
    s_.scale = float(q.value(g + "scale", 1.0).toDouble());
    s_.upscaler = veyamux::Upscaler(std::clamp(q.value(g + "upscaler", 0).toInt(), 0, 4));
    s_.fxaa = q.value(g + "fxaa", false).toBool();
    s_.deflicker = q.value(g + "deflicker", false).toBool();
    s_.fxaaQuality = std::clamp(q.value(g + "fxaaQuality", 1).toInt(), 0, 2);
    s_.retroTv = q.value(g + "retroTv", false).toBool();
    { const int l = q.value(g + "retroLines", 240).toInt(); s_.retroLines = (l == 0 || l == 480) ? l : 240; }
    s_.pixelPerfect = q.value(g + "pixelPerfect", false).toBool();
    s_.pixelGrid = std::clamp(q.value(g + "pixelGrid", 0).toInt(), 0, 1);
    s_.denoise = veyamux::Denoise(std::clamp(q.value(g + "denoise", 0).toInt(), 0, 3));
    s_.denoiseStrength = std::clamp(q.value(g + "denoiseStrength", 1).toInt(), 0, 2);
    s_.hdrPeak = float(std::clamp(q.value(g + "hdrPeak", 1000).toInt(), 203, 10000));
    s_.sharpen = float(q.value(g + "sharpen", 0.0).toDouble());
    s_.smaa = q.value(g + "smaa", false).toBool();
    s_.texture = float(std::clamp(q.value(g + "texture", 0.0).toDouble(), -1.0, 1.0));
    s_.clarity = float(std::clamp(q.value(g + "clarity", 0.0).toDouble(), -1.0, 1.0));
    s_.dehaze = float(std::clamp(q.value(g + "dehaze", 0.0).toDouble(), -1.0, 1.0));
    s_.exposure = float(q.value(g + "exposure", 0.0).toDouble());
    s_.contrast = float(q.value(g + "contrast", 1.0).toDouble());
    s_.saturation = float(q.value(g + "saturation", 1.0).toDouble());
    setAudioDelay(q.value(g + "audioDelay", 0).toInt());
    activeProfile_ = name;
    emit profilesChanged();
    applyEnhance();
    showNotice(L("Profile: %1", "Profil: %1").arg(name));
    return true;
}

void PlayerBridge::deleteProfile(const QString& name) {
    QSettings q;
    q.remove("profiles/" + QString(QUrl::toPercentEncoding(name)));
    QStringList order = q.value("profiles/order").toStringList();
    order.removeAll(name);
    q.setValue("profiles/order", order);
    if (activeProfile_ == name) activeProfile_.clear();
    emit profilesChanged();
}

// A capture signal with a bound profile gets it applied when it is opened.
void PlayerBridge::applySignalProfile() {
    const QString sig = signalKey();
    if (sig.isEmpty()) return;
    for (const auto& n : profiles())
        if (profileSignal(n) == sig) { loadProfile(n); return; }
}

// ------------------------------------------------------------------ capture card support

QVariantList PlayerBridge::captureDevices() {
    QVariantList out;
    for (const auto& d : veyamux::listCaptureDevices()) {
        QVariantMap m;
        m["path"] = QString::fromStdString(d.path);
        m["name"] = QString::fromStdString(d.name);
        out << m;
    }
    return out;
}

namespace {
bool compressedFourcc(uint32_t f) { return f == V4L2_PIX_FMT_MJPEG || f == V4L2_PIX_FMT_JPEG || f == V4L2_PIX_FMT_H264; }
}

int PlayerBridge::betterModeIndex(int modeIndex) const {
    if (modeIndex < 0 || modeIndex >= int(lastModes_.size())) return -1;
    const auto& m = lastModes_[size_t(modeIndex)];
    if (!compressedFourcc(m.fourcc)) return -1;
    int best = -1;
    for (int i = 0; i < int(lastModes_.size()); ++i) {
        const auto& o = lastModes_[size_t(i)];
        if (compressedFourcc(o.fourcc) || o.w != m.w || o.h != m.h) continue;
        if (best < 0 || o.fps > lastModes_[size_t(best)].fps + 0.5) best = i;
    }
    return best;
}

QString PlayerBridge::modeAdvice(int modeIndex) const {
    const int b = betterModeIndex(modeIndex);
    if (b < 0) return {};
    const auto& m = lastModes_[size_t(modeIndex)];
    const auto& o = lastModes_[size_t(b)];
    const QString name = QString::fromStdString(veyamux::fourccName(o.fourcc));
    if (o.fps + 0.5 >= m.fps)
        return L("The card also sends this size uncompressed (%1) at the same rate: a sharper picture, without the "
                 "compression's blocks and no decoding. Choose \"%2\".",
                 "Kortet sender også denne størrelse ukomprimeret (%1) med samme hastighed: et skarpere billede uden "
                 "komprimeringens blokke og uden afkodning. Vælg \"%2\".")
            .arg(name, QString::fromStdString(o.label));
    return L("MJPEG is compressed and softer. Uncompressed (%1) is only offered at %2 fps at this size: sharper, but a "
             "60 fps game then reaches VeyAmux with half its frames.",
             "MJPEG er komprimeret og blødere. Ukomprimeret (%1) findes kun ved %2 fps i denne størrelse: skarpere, men et "
             "spil i 60 fps når så VeyAmux med halvdelen af sine billeder.")
        .arg(name).arg(std::lround(o.fps));
}

QString PlayerBridge::captureHelp() const {
    const QStringList devs = QDir("/dev").entryList({"video*"}, QDir::System);
    if (devs.isEmpty())
        return L("No /dev/video* at all: plug the card into a USB 3 port (blue or marked SS) directly on the PC, not a hub, "
                 "and give it a few seconds.",
                 "Slet ingen /dev/video*: sæt kortet i en USB 3-port (blå eller mærket SS) direkte på pc'en, ikke en hub, "
                 "og giv det et par sekunder.");
    int noAccess = 0;
    for (const QString& d : devs) if (::access(("/dev/" + d).toLocal8Bit().constData(), R_OK | W_OK) != 0) ++noAccess;
    if (noAccess == devs.size())
        return L("%1 exists, but your user may not open it. Add yourself to the \"video\" group "
                 "(sudo usermod -aG video $USER) and log in again.",
                 "%1 findes, men din bruger må ikke åbne den. Føj dig selv til gruppen \"video\" "
                 "(sudo usermod -aG video $USER) og log ind igen.").arg("/dev/" + devs.first());
    return {};
}

QVariantList PlayerBridge::captureModes(const QString& path) {
    lastModes_ = veyamux::listCaptureModes(path.toStdString());
    QVariantList out;
    for (const auto& m : lastModes_) out << QString::fromStdString(m.label);
    return out;
}

namespace {
// i18n/messages.json: { "<language>": { "<English text>": "<translation>" } }
const QJsonObject& messageTable() {
    static const QJsonObject t = [] {
        QFile f(QStringLiteral(":/i18n/messages.json"));
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }();
    return t;
}
const QStringList kLanguages{"en", "da", "de", "es", "fr", "sv", "nb", "ru", "pt-BR"};
}

QString PlayerBridge::L(const char* en, const char* da) const {
    if (lang_ == "en") return QString::fromUtf8(en);
    if (lang_ == "da" && da && std::strcmp(en, da) != 0) return QString::fromUtf8(da);
    const QJsonValue v = messageTable().value(lang_).toObject().value(QString::fromUtf8(en));
    return v.isString() ? v.toString() : QString::fromUtf8(lang_ == "da" && da ? da : en);
}

QString PlayerBridge::systemLanguage() {
    // the system's language if VeyAmux has it, else English (Norwegian Nynorsk / "no" use Bokmål)
    QStringList cands;
    for (const char* var : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty()) for (const QString& part : v.split(':')) cands << part;
    }
    cands << QLocale::system().name();
    for (QString c : cands) {
        c = c.left(c.indexOf(QRegularExpression("[_.@-]")) < 0 ? c.size() : c.indexOf(QRegularExpression("[_.@-]"))).toLower();
        if (c == "no" || c == "nn") c = "nb";
        if (c == "pt") return QStringLiteral("pt-BR");
        if (kLanguages.contains(c)) return c;
        if (!c.isEmpty() && c != "c" && c != "posix") return "en";
    }
    return "en";
}

void PlayerBridge::setLanguageChoice(const QString& c) {
    const QString choice = (c == "auto" || kLanguages.contains(c)) ? c : QStringLiteral("auto");
    const QString use = choice == "auto" ? systemLanguage() : choice;
    QSettings().setValue("ui/lang", choice);
    if (choice == langChoice_ && use == lang_) return;
    langChoice_ = choice;
    lang_ = use;
    emit languageChanged();
}

void PlayerBridge::setTheme(const QString& t) {
    static const QStringList known{"vice", "code", "inferno", "retro"};
    const QString v = known.contains(t) ? t : QStringLiteral("vice");
    // the window icon follows the theme's logo (X11/XWayland task bars; Wayland uses the desktop file's icon)
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qml/icons/logo-%1.svg").arg(v)));
    if (v == theme_) return;
    theme_ = v;
    QSettings().setValue("ui/theme", v);
    emit themeChanged();
}

QVariantList PlayerBridge::audioSources() {
    // PulseAudio/PipeWire input sources (capture cards expose their HDMI audio as one); the "monitors" of outputs are
    // left out (they would record VeyAmux's own sound).
    QVariantList out;
    for (const auto& d : veyamux::pulse::sources())
        if (!d.monitor) { QVariantMap m; m["name"] = QString::fromStdString(d.name); m["desc"] = QString::fromStdString(d.description); out << m; }
    return out;
}

QVariantList PlayerBridge::audioSinks() {
    QVariantList out;
    for (const auto& d : veyamux::pulse::sinks()) {
        QVariantMap m; m["name"] = QString::fromStdString(d.name); m["desc"] = QString::fromStdString(d.description); out << m;
    }
    return out;
}

QString PlayerBridge::guessAudioSource(const QString& cardName) {
    static const QStringList skip = {"usb", "video", "capture", "hdmi", "audio", "device", "card", "the"};
    QStringList toks;
    for (const QString& t : cardName.toLower().split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts))
        if (t.size() >= 3 && !skip.contains(t)) toks << t;
    if (toks.isEmpty()) return {};
    int best = 0; QString bestName;
    for (const QVariant& v : audioSources()) {
        const QVariantMap m = v.toMap();
        const QString hay = (m["name"].toString() + " " + m["desc"].toString()).toLower();
        int score = 0;
        for (const QString& t : toks) if (hay.contains(t)) ++score;
        if (score > best) { best = score; bestName = m["name"].toString(); }
    }
    return bestName;
}

QString PlayerBridge::lastCaptureDevice() const {
    // the same card may come back under another /dev/videoN after being re-plugged
    QSettings q;
    const QString dev = q.value("cap/device").toString(), name = q.value("cap/name").toString();
    if (!name.isEmpty() && veyamux::watch::cardName(dev) != name)
        if (const QString d = veyamux::watch::findDeviceByName(name); !d.isEmpty()) return d;
    return dev;
}

void PlayerBridge::setAutostart(bool on) {
    QString err;
    if (!veyamux::watch::setAutostart(on, err)) showNotice(err);
    else if (on && QSettings().value("cap/name").toString().isEmpty())
        showNotice(L("Open the capture card once, so VeyAmux knows which card to wait for", "Åbn capture-kortet én gang, så VeyAmux ved, hvilket kort den skal vente på"));
    emit recChanged();
}

bool PlayerBridge::autostart() const { return veyamux::watch::autostartEnabled(); }
QString PlayerBridge::lastCaptureAudio() const { return QSettings().value("cap/audio").toString(); }

bool PlayerBridge::openCapture(const QString& path, int modeIndex, const QString& audioSource) {
    if (modeIndex < 0 || modeIndex >= int(lastModes_.size())) { setStatus(L("No capture mode selected", "Ingen capture-tilstand valgt")); return false; }
    return startCapture(path, lastModes_[size_t(modeIndex)], audioSource);
}

void PlayerBridge::openScreenCast(const QString& audioSource, bool useLast) {
    std::string why;
    if (!veyamux::ScreenCastStream::available(&why)) { setStatus(translateMessage(QString::fromStdString(why))); return; }
    if (!portal_) {
        portal_ = new veyamux::ScreenCastPortal(this);
        connect(portal_, &veyamux::ScreenCastPortal::ready, this, [this](int fd, uint node, int, int, const QString& token) {
            if (!token.isEmpty()) QSettings().setValue("cap/scToken", token);
            startCapture(QStringLiteral("pipewire:%1:%2").arg(fd).arg(node), veyamux::CaptureMode{}, scAudio_);
        });
        connect(portal_, &veyamux::ScreenCastPortal::failed, this, [this](const QString& msg, bool cancelled) {
            if (cancelled) { QSettings().remove("cap/scToken"); setStatus({}); }
            else setStatus(translateMessage(msg));
        });
    }
    scAudio_ = audioSource;
    setStatus(L("Pick a window or screen in the share dialog", "Vælg et vindue eller en skærm i delings-dialogen"));
    portal_->start(useLast ? QSettings().value("cap/scToken").toString() : QString());
}

bool PlayerBridge::startCapture(const QString& path, const veyamux::CaptureMode& mode, const QString& audioSource) {
    const bool screenCast = veyamux::CaptureSource::isScreenCast(path.toStdString());
    demoActive_ = veyamux::CaptureSource::isDemo(path.toStdString());
    veyamux::demo::releaseAll();
    if (!screenCast && portal_) portal_->closeSession();   // a capture card replaces the shared window
    std::string err;
    { QMutexLocker l(&imgMutex_); image_ = QImage(); }
    if (liveVk_) liveVk_->setActive(false);
    const bool zeroCopy = liveVk_ != nullptr;
    if (!engine_.openCapture(path.toStdString(), mode, audioSource.toStdString(), err, zeroCopy)) {
        setStatus(translateMessage(QString::fromStdString(err)));
        if (screenCast && portal_) portal_->closeSession();
        emit mediaChanged();
        return false;
    }
    curMode_ = screenCast || veyamux::CaptureSource::isDemo(path.toStdString()) || veyamux::CaptureSource::isRemotePlay(path.toStdString())
                   ? veyamux::CaptureMode{} : mode;
    curDevice_ = path;
    curAudio_ = audioSource;
    QSettings q; q.setValue("cap/device", screenCast ? QStringLiteral("screencast") : path); q.setValue("cap/audio", audioSource);
    if (const QString n = screenCast ? QString() : veyamux::watch::cardName(path); !n.isEmpty()) q.setValue("cap/name", n);   // for the plug-in watcher
    updateLsfgMultiplier();
    if (engine_.info().height > 0) aspect_ = double(engine_.info().width) / engine_.info().height;
    if (zeroCopy) { liveVk_->setSettings(s_); liveVk_->setActive(true); }
    engine_.setVolume(float(volume_));
    engine_.setAudioDelay(audioDelay_);
    engine_.setMuted(muted_);
    applyEnhance();
    setStatus({});
    emit mediaChanged();
    applySignalProfile();
    lastPlaying_ = true;
    emit playingChanged();
    return true;
}

bool PlayerBridge::openCaptureAuto(const QString& path) {
    if (path == "screencast") { openScreenCast(lastCaptureAudio(), true); return true; }
    if (veyamux::CaptureSource::isDemo(path.toStdString())) {
        const QStringList p = path.mid(5).split(':');   // "demo:<fps>[:<height>]"
        openDemo(p.value(0).toInt(), p.size() > 1 ? p.at(1).toInt() : 1080);
        return true;
    }
    if (veyamux::CaptureSource::isRemotePlay(path.toStdString())) { psConnect(path.mid(3)); return true; }
    QString card;
    for (const auto& d : veyamux::listCaptureDevices()) if (QString::fromStdString(d.path) == path) card = QString::fromStdString(d.name);
    captureModes(path);
    return openCapture(path, 0, guessAudioSource(card));
}

// ------------------------------------------------------------------ demo scene

bool PlayerBridge::demoAvailable() const { return veyamux::demo::available(); }

void PlayerBridge::openDemo(int fps, int height) {
    if (!veyamux::demo::available()) { setStatus(L("This build has no demo scene", "Denne udgave har ingen demo")); return; }
    if (portal_) portal_->closeSession();
    demoProgress_ = -1;   // the tick reports the first progress
    const QString path = height == 720 ? QStringLiteral("demo:%1:720").arg(fps == 30 ? 30 : 60)
                                       : QStringLiteral("demo:%1").arg(fps == 30 ? 30 : 60);
    startCapture(path, veyamux::CaptureMode{}, QStringLiteral("@demo"));
}

void PlayerBridge::demoKey(const QString& key, bool down) {
    using K = veyamux::demo::Key;
    static const QHash<QString, K> keys = {{"w", K::Forward}, {"s", K::Back}, {"a", K::Left}, {"d", K::Right}, {"e", K::Up}, {"q", K::Down}, {"shift", K::Fast}};
    if (auto it = keys.find(key); it != keys.end()) veyamux::demo::setKey(*it, down);
}

void PlayerBridge::demoLook(double dx, double dy) { veyamux::demo::addLook(dx, dy); }

void PlayerBridge::demoPose(int pose) {
    demoPose_ = pose >= 0 ? pose % 6 : (demoPose_ + 1) % 6;
    veyamux::demo::jumpTo(demoPose_);
}

void PlayerBridge::demoRelease() { veyamux::demo::releaseAll(); }

void PlayerBridge::demoPad(double lx, double ly, double rx, double ry, double lt, double rt, bool fast) {
    if (!demoActive()) return;
    veyamux::demo::setPad(lx, ly, rx, ry, lt, rt, fast);
}

// ------------------------------------------------------------------ frame generation / Lossless Scaling

void PlayerBridge::setRunContext(bool vulkanUi, bool lsfgActive, bool lsfgCrashed) {
    vulkanUi_ = vulkanUi;
    lsfgActive_ = lsfgActive;
    lsfgCrashed_ = lsfgCrashed;
    refreshLsfg();
    if (s_.lsfgFg()) checkDesktopVrr();
    if (lsfgActive_) {
        lsfgMultiplier_ = s_.frameGen == veyamux::FrameGenMode::LsfgFixed3 ? 3 : (s_.lsfgFg() ? 2 : 1);
        runClock_.start();
        auto* t = new QTimer(this);   // lsfg-vk writes its log once the window's Vulkan device exists
        connect(t, &QTimer::timeout, this, &PlayerBridge::updateLsfgLog);
        t->start(1500);
    }
}

// The desktop's VRR setting (KDE: kscreen-doctor). Asked at start and whenever Lossless Scaling is chosen, since the
// user may have changed it meanwhile; a notice comes once per run when both are on.
void PlayerBridge::checkDesktopVrr() {
    veyamux::display::queryVrr(this, [this](veyamux::display::Vrr v, QString screens) {
        desktopVrr_ = v == veyamux::display::Vrr::On;
        vrrScreens_ = screens;
        emit lsfgChanged();
        if (lsfgVrrWarning() && !vrrNoticed_) {
            vrrNoticed_ = true;
            showNotice(L("VRR (Adaptive Sync) is on: Lossless Scaling cannot pace its frames evenly. Set Adaptive sync to Never in the display settings",
                         "VRR (adaptiv synkronisering) er slået til: Lossless Scaling kan ikke lægge sine billeder jævnt. Sæt Adaptiv synkronisering til Aldrig i skærmindstillingerne"));
        }
    });
}

void PlayerBridge::refreshLsfg() {
    lsfg_ = lsfg::detect(QSettings().value("lsfg/folder").toString());
    emit lsfgChanged();
    // only an older lsfg-vk (1.x) installed: fetch lsfg-vk 2 by itself, once per start (VEYAMUX_LSFG_AUTOINSTALL=0: never)
    if (!lsfgAutoTried_ && !lsfg_.layerManifest.isEmpty() && !lsfg_.layerV2 && lsfgInstall_.isEmpty() &&
        qEnvironmentVariable("VEYAMUX_LSFG_AUTOINSTALL") != "0") {
        lsfgAutoTried_ = true;
        QTimer::singleShot(3000, this, &PlayerBridge::installLsfg);   // after a possible relaunch at start
    }
}

void PlayerBridge::installLsfg() {
    if (lsfgInstall_ == "busy") return;
    lsfgAutoTried_ = true;
    lsfgInstall_ = "busy";
    lsfgInstallText_ = L("Downloading lsfg-vk 2 from builds.lsfg-vk.dev…", "Henter lsfg-vk 2 fra builds.lsfg-vk.dev…");
    emit lsfgChanged();
    qInfo("Lossless Scaling: installing lsfg-vk %s", lsfg::kV2Version);
    std::thread([this, quit = psQuit_] {
        auto last = std::make_shared<std::atomic<int>>(-1);
        const auto r = lsfg::installV2([this, quit, last](int pc) {
            if (quit->load() || pc / 5 == last->load() / 5) return;
            last->store(pc);
            QMetaObject::invokeMethod(this, [this, pc] {
                if (lsfgInstall_ != "busy") return;
                lsfgInstallText_ = L("Downloading lsfg-vk 2 from builds.lsfg-vk.dev… %1 %", "Henter lsfg-vk 2 fra builds.lsfg-vk.dev… %1 %").arg(pc);
                emit lsfgChanged();
            }, Qt::QueuedConnection);
        });
        if (quit->load()) return;   // VeyAmux closed meanwhile
        QMetaObject::invokeMethod(this, [this, r] {
            qInfo("Lossless Scaling: %s", qPrintable(r.message));
            lsfgInstall_ = r.ok ? "done" : "error";
            if (r.ok) {
                lsfgInstallText_ = L("lsfg-vk 2 is installed in your home folder (~/.local).", "lsfg-vk 2 er installeret i din hjemmemappe (~/.local).");
                if (!r.backupDir.isEmpty())
                    lsfgInstallText_ += " " + L("The old lsfg-vk 1 files were moved to %1.", "De gamle lsfg-vk 1-filer er flyttet til %1.").arg(r.backupDir);
                lsfgInstallText_ += " " + L("Restart VeyAmux to use it.", "Genstart VeyAmux for at bruge det.");
                showNotice(L("lsfg-vk 2 installed", "lsfg-vk 2 installeret"));
            } else {
                lsfgInstallText_ = L("Could not install lsfg-vk 2: ", "Kunne ikke installere lsfg-vk 2: ") + r.message;
            }
            lsfg_ = lsfg::detect(QSettings().value("lsfg/folder").toString());
            emit lsfgChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

void PlayerBridge::chooseLsfgFolder() {
    const QString start = lsfg_.installDir.isEmpty() ? QDir::homePath() : lsfg_.installDir;
    const QString dir = QFileDialog::getExistingDirectory(nullptr, L("Select the Lossless Scaling folder", "Vælg mappen med Lossless Scaling"), start);
    if (dir.isEmpty()) return;
    QSettings().setValue("lsfg/folder", dir);
    refreshLsfg();
}


QString PlayerBridge::lsfgState() const {
    if (lsfgCrashed_) return "crashed";
    if (!lsfg_.installed()) return "noInstall";
    if (lsfg_.layerManifest.isEmpty()) return "noLayer";
    if (!lsfg_.layerV2) return "oldLayer";
    if (lsfg_.dllPath.isEmpty()) return "noDll";
    if (!vulkanUi_) return "noVulkanUi";
    return lsfgActive_ ? "active" : "ready";
}

bool PlayerBridge::restartNeeded() const {
    const bool ls = s_.lsfgFg();
    return ls && !lsfgActive_ && lsfgState() == "ready";
}

double PlayerBridge::displayHz() const {
    QScreen* sc = window_ ? window_->screen() : QGuiApplication::primaryScreen();
    return sc ? sc->refreshRate() : 60.0;
}

double PlayerBridge::lsfgFlowScale() const {
    // Chosen from the screen, not the window: VeyAmux is mostly used fullscreen, and every change makes lsfg-vk rebuild
    // its whole pipeline (seen in user logs when the window was resized/maximised).
    const QScreen* sc = window_ && window_->screen() ? window_->screen() : QGuiApplication::primaryScreen();
    const int h = sc ? int(sc->size().height() * sc->devicePixelRatio()) : 1080;
    return lsfg::recommendedFlowScale(h > 0 ? h : 1080);
}

void PlayerBridge::setLsfgPerformance(bool v) {
    lsfgPerformance_ = v;
    QSettings().setValue("lsfg/performance", v);
    if (lsfgActive_) lsfg::writeConfig(lsfg_, lsfgMultiplier_, v, lsfgFlowScale());   // hot-reloaded by lsfg-vk
    emit lsfgChanged();
}

void PlayerBridge::updateLsfgMultiplier() {
    if (!lsfgActive_) return;
    // only 2 or 3: lsfg-vk 2 refuses 1 (it cannot pause; turning it off restarts VeyAmux, see setFrameGenMode)
    if (!s_.lsfgFg()) return;
    const int m = s_.frameGen == veyamux::FrameGenMode::LsfgFixed3 ? 3 : 2;
    const double flow = lsfgFlowScale();
    if (m == lsfgMultiplier_ && flow == lastFlow_ && QFileInfo::exists(lsfg::configPath())) return;
    lastFlow_ = flow;
    lsfgMultiplier_ = m;
    lsfg::writeConfig(lsfg_, m, lsfgPerformance_, lsfgFlowScale());
    emit lsfgChanged();
    emit mediaChanged();
}

void PlayerBridge::updateLsfgLog() {
    QString t;
    if (lsfgActive_) {
        if (!runClock_.isValid()) runClock_.start();
        bool loaded = false;
        const QString line = lsfg::logSummary(&loaded);
        if (loaded) {
            const bool bad = line.contains("[ERROR]", Qt::CaseInsensitive) || line.contains("[WARNING]", Qt::CaseInsensitive);
            t = (bad ? L("lsfg-vk reports: ", "lsfg-vk melder: ") : L("lsfg-vk is loaded: ", "lsfg-vk er indlæst: ")) + line;
        } else if (runClock_.elapsed() > 6000) {
            t = L("lsfg-vk did not load into VeyAmux (it wrote no log to %1). Check the installation with "
                  "\"lsfg-vk-cli healthcheck\" in a terminal.",
                  "lsfg-vk blev ikke indlæst i VeyAmux (ingen log i %1). Tjek installationen med "
                  "\"lsfg-vk-cli healthcheck\" i en terminal.").arg(lsfg::logPath());
        }
    }
    if (t != lsfgLog_) {
        lsfgLog_ = t;
        if (!t.isEmpty()) qInfo("Lossless Scaling: %s", qPrintable(t));
        emit lsfgChanged();
    }
}

void PlayerBridge::retryLsfg() {
    QSettings().remove("lsfg/disabledAfterCrash");
    restartApp();
}

void PlayerBridge::restartWithSource() {
    saveSettings();
    const QString target = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
    // the old source and window arguments are replaced by the current ones
    QStringList old = QCoreApplication::arguments().mid(1), args;
    for (int i = 0; i < old.size(); ++i) {
        if (old[i] == "--capture" || old[i] == "--grab") { ++i; continue; }
        if (old[i] == "--probe-ui" || old[i] == "--fullscreen" || !old[i].startsWith("--")) continue;
        args << old[i];
    }
    if (engine_.isOpen() && engine_.isLive()) args << "--capture" << "last";
    else if (engine_.isOpen() && !openFile_.isEmpty()) args << openFile_;
    if (window_ && window_->visibility() == QWindow::FullScreen) args << "--fullscreen";
    engine_.close();   // release the capture card before the new instance opens it
    QSettings().sync();
    if (QProcess::startDetached(target, args)) QCoreApplication::quit();
    else { lsfgOffPending_ = false; showNotice(L("Restart VeyAmux to turn Lossless Scaling off", "Genstart VeyAmux for at slå Lossless Scaling fra")); }
}

void PlayerBridge::restartApp() {
    saveSettings();
    // Inside an AppImage the binary lives in a temporary mount that disappears on exit: relaunch the AppImage itself.
    const QString target = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments().mid(1);
    args.removeAll("--probe-ui");
    // Release the capture card (and audio) BEFORE the new instance starts, or it gets "device busy".
    engine_.close();
    QSettings().sync();
    if (QProcess::startDetached(target, args)) QCoreApplication::quit();
}

QString PlayerBridge::translateMessage(const QString& msg) const {
    if (lang_ == "en" || msg.isEmpty()) return msg;
    const auto table = messageTable().value(lang_).toObject();
    const auto exact = table.value(msg);
    if (exact.isString()) return exact.toString();
    // Preserve paths, device codes and progress values supplied by the runtime.
    static const char* const templates[] = {
        "NVIDIA VFG: %1%",
        "Expected 62 known SM89 kernel images; found %1",
        "Incomplete or unrecognized installation at %1. Move it aside to retry.",
        "Missing runtime function %1",
        "CUDA initialization or device enumeration failed (cuInit=%1)",
        "DLSS SR: initialize the NVIDIA NGX runtime failed (NGX 0x%1)",
        "DLSS SR: query driver capabilities failed (NGX 0x%1)",
        "DLSS SR: create Super Resolution feature failed (NGX 0x%1)",
        "DLSS SR: evaluate frame failed (NGX 0x%1)"
    };
    for (const char* pattern : templates) {
        const QString key = QString::fromLatin1(pattern);
        const auto translated = table.value(key);
        if (!translated.isString()) continue;
        const auto start = key.indexOf("%1");
        const auto after = key.sliced(start + 2);
        if (msg.size() >= start + after.size() && msg.startsWith(key.first(start)) && msg.endsWith(after))
            return translated.toString().arg(msg.sliced(start, msg.size() - start - after.size()));
    }
    const QString fasterPrefix = QStringLiteral("Faster model unavailable: ");
    if (msg.startsWith(fasterPrefix) && table.value(fasterPrefix).isString())
        return table.value(fasterPrefix).toString() + translateMessage(msg.sliced(fasterPrefix.size()));
    // messages from the engine start with one of these English texts; the rest (a path, a reason) is kept
    static const char* const prefixes[] = {
        "Audio unavailable: ",
        "Capture audio unavailable: ",
        "Audio output unavailable: ",
        "No signal from the capture card",
        "Capture card lost, reconnecting...",
        "Capture device disconnected",
        "Vulkan unavailable: ",
        "Could not set up the enhancement chain: ",
        "GPU error, falling back to passthrough: ",
        "GPU enhancement disabled (VEYAMUX_NO_GPU=1)",
        "Read error: ",
        "Could not open the file: ",
        "Could not read stream info: ",
        "The file contains no playable audio or video",
        "No decoder for the video stream",
        "Could not open the video decoder: ",
        "Could not open ",
        "The device refused pixel format ",
        "The device does not support memory-mapped streaming",
        "Out of GPU memory for FSR upscaling",
        "Out of GPU memory",
        "The capture device is in use by another program",
        "Live path unavailable",
        "The shared window was closed",
        "The screen share sends no picture",
        "The screen share portal (xdg-desktop-portal) is not running",
        "Screen share portal: ",
        "Screen share error: ",
        "Nothing was picked to share",
        "The screen share was not started",
        "PipeWire (libpipewire-0.3.so.0) was not found",
        "PipeWire: ",
        "No Vulkan graphics card for the demo",
        "The graphics card refused the demo's device",
        "The demo's graphics device was lost",
        // PlayStation Remote Play (RemotePlay.cpp)
        "Remote Play stopped", "Remote Play is already in use on the console",
        "The console refused the connection (is Remote Play enabled?)", "The console was turned off", "Remote Play ended: ",
        "The console asks for its login passcode: enter it in the Remote Play window", "The console's login passcode is wrong",
        "Choose the PlayStation in the Remote Play window first", "The console's key is damaged: register it again",
        "Could not start Remote Play", "Could not connect to the console",
        "Registration failed: check the PIN (it changes every few minutes) and that the console shows the \"Link Device\" screen",
        "The PSN account ID is not valid", "Could not start the registration", "Could not send the wake-up",
        "The console did not wake up (is \"Stay connected to the Internet\" in its rest mode settings on?)",
        "That address has no login code (it should contain \"code=\")",
        "No access token from PlayStation Network (the code is used once: log in again)",
        "PlayStation Network sent no account number", "PlayStation Network answered ",
    };
    for (const char* en : prefixes) {
        const QString e = QString::fromUtf8(en);
        if (msg.startsWith(e)) return L(en, en) + msg.mid(e.size());
    }
    // The vendor's diagnostic follows an application-defined operation name.
    for (const auto delimiter : {QStringLiteral(" ("), QStringLiteral(": ")}) {
        const auto end = msg.indexOf(delimiter);
        if (end <= 0) continue;
        const auto translated = table.value(msg.first(end));
        if (translated.isString()) return translated.toString() + msg.sliced(end);
    }
    return msg;
}
