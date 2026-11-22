#include "LiveVulkan.h"
#include <cstring>
#include <QImage>
#include <QMetaObject>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QTimer>
#include <algorithm>
#include <QDebug>
#include <chrono>
#include <cmath>

using namespace veyamux;

LiveVulkan::LiveVulkan(QQuickWindow* window, VkContext* ctx, const QString& shaderDir, QObject* parent)
    : QObject(parent), win_(window), ctx_(ctx), chain_(std::make_unique<EnhanceChain>(*ctx, shaderDir.toStdString())) {
    connect(window, &QQuickWindow::beforeRendering, this, &LiveVulkan::onBeforeRendering, Qt::DirectConnection);
}

LiveVulkan::~LiveVulkan() {
    streamQuit_ = true;
    if (streamThread_.joinable()) streamThread_.join();
}

bool LiveVulkan::init(std::string& error) {
    if (!chain_->init(error)) return false;
    streamThread_ = std::thread([this] { streamLoop(); });
    return true;
}

void LiveVulkan::setStream(int maxHeight, std::function<void(QImage, int)> sink) {
    { std::lock_guard l(sinkM_); sink_ = std::move(sink); }
    chain_->setStreamMaxHeight(maxHeight);
}

void LiveVulkan::setRecord(int maxHeight, std::function<void(veyamux::EnhanceChain::RecFrame&&)> sink) {
    { std::lock_guard l(sinkM_); recSink_ = std::move(sink); }
    chain_->setRecordMaxHeight(maxHeight);
}

void LiveVulkan::setCompare(int mode, const QImage& a, const QImage& b) {
    auto bytes = [](const QImage& img) {
        std::vector<uint8_t> v;
        if (img.isNull()) return v;
        const QImage c = img.convertToFormat(QImage::Format_RGBA8888);
        v.resize(size_t(c.width()) * c.height() * 4);
        for (int y = 0; y < c.height(); ++y) std::memcpy(v.data() + size_t(y) * c.width() * 4, c.constScanLine(y), size_t(c.width()) * 4);
        return v;
    };
    if (mode > 0) chain_->setCompareLabels(bytes(a), a.width(), a.height(), bytes(b), b.width(), b.height());
    chain_->setCompare(mode);
}

void LiveVulkan::requestScreenshot(std::function<void(QImage)> cb) {
    { std::lock_guard l(sinkM_); shotCb_ = std::move(cb); }
    chain_->requestScreenshot();
    { std::lock_guard l(m_); redraw_ = true; }   // render the current picture again even if the source is paused
    if (win_) QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

// Collects the OBS stream copies as soon as the GPU has written them (independent of when Qt renders next).
void LiveVulkan::streamLoop() {
    std::vector<EnhanceChain::StreamFrame> frames;
    std::vector<EnhanceChain::RecFrame> rec;
    while (!streamQuit_.load()) {
        rec.clear();
        chain_->pollRecord(rec);
        if (!rec.empty()) {
            std::function<void(EnhanceChain::RecFrame&&)> rs;
            { std::lock_guard l(sinkM_); rs = recSink_; }
            if (rs) for (auto& f : rec) rs(std::move(f));
        }
        EnhanceChain::StreamFrame shot;
        if (chain_->pollScreenshot(shot)) {
            std::function<void(QImage)> cb;
            { std::lock_guard l(sinkM_); cb = std::move(shotCb_); shotCb_ = nullptr; }
            if (cb) {
                QImage img(shot.w, shot.h, QImage::Format_RGBA8888);
                for (int y = 0; y < shot.h; ++y) std::memcpy(img.scanLine(y), shot.rgba.data() + size_t(y) * shot.w * 4, size_t(shot.w) * 4);
                cb(std::move(img));
            }
        }
        frames.clear();
        chain_->pollStream(frames);
        if (!frames.empty()) {
            std::function<void(QImage, int)> sink;
            { std::lock_guard l(sinkM_); sink = sink_; }
            for (auto& f : frames) {
                if (!sink) continue;
                QImage img(f.w, f.h, QImage::Format_RGBA8888);
                for (int y = 0; y < f.h; ++y) std::memcpy(img.scanLine(y), f.rgba.data() + size_t(y) * f.w * 4, size_t(f.w) * 4);
                sink(std::move(img), f.delayMs);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

QString LiveVulkan::lastUpError() const {
    std::lock_guard l(m_);
    return QString::fromStdString(upError_);
}

QString LiveVulkan::nvidiaVfgError() const {
    std::lock_guard l(m_);
    return QString::fromStdString(nvidiaVfgError_);
}

QString LiveVulkan::lastError() const {
    std::lock_guard l(m_);
    return QString::fromStdString(error_);
}

void LiveVulkan::setDisplayFit(QSize px) {
    if (px.width() == fitW_.load() && px.height() == fitH_.load()) return;
    fitW_ = px.width(); fitH_ = px.height();
    { std::lock_guard l(m_); redraw_ = true; }   // draw the current picture again at the new size
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::setSettings(const EnhanceSettings& s) {
    {
        std::lock_guard l(m_);
        if (s.nvidiaFg() != settings_.nvidiaFg() || (s.nvidiaFg() &&
                (s.nvidiaQuality != settings_.nvidiaQuality || s.nvidiaAutoRegulate != settings_.nvidiaAutoRegulate))) {
            resetNvidiaStats();
            resetFg_ = true;
        }
        if (!s.nvidiaFg() || !settings_.nvidiaFg()) { nvidiaVfgActive_ = false; nvidiaVfgError_.clear(); }
        settings_ = s;
        redraw_ = true;           // re-run the chain on the current frame (matters while paused)
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::setActive(bool on) {
    active_ = on;
    emit outputChanged();
    if (!on) {
        std::lock_guard l(m_);
        hasNew_ = haveCurrent_ = redraw_ = false;
        nvidiaVfgActive_ = false; nvidiaVfgError_.clear(); resetFg_ = true;
        lastArrival_ = {};
        frameMs_ = 1000.0 / 60.0;
        resetNvidiaStats();
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::submit(RawFrame& frame) {
    {
        std::lock_guard l(m_);
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double, std::milli>(now - lastArrival_).count();
        lastArrival_ = now;
        if (dt > 4.0 && dt < 100.0) frameMs_ = 0.85 * frameMs_ + 0.15 * dt;
        std::swap(mailbox_, frame);         // the newest frame always wins; an unrendered older one goes back for reuse
        hasNew_ = true;
    }
    QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
}

void LiveVulkan::scheduleRealUpdate(bool nvidia) {
    if (realUpdateScheduled_.exchange(true)) return;
    const auto deadline = realDue_;
    const auto left = deadline - Clock::now();
    const int fsrTimerMs = std::max(1, int(std::chrono::duration_cast<std::chrono::milliseconds>(left).count()) - 1);
    QMetaObject::invokeMethod(this, [this, deadline, nvidia, fsrTimerMs] {
        int ms = fsrTimerMs;
        if (nvidia) {
            const QScreen* screen = win_->screen();
            const double hz = screen ? screen->refreshRate() : 0;
            if (hz >= 20 && hz <= 1000) displayPeriodUs_ = int(std::lround(1e6 / hz));
            // The GUI may have been waiting on the render thread. Use the
            // absolute deadline; request the next refresh one display period
            // in advance. The render thread selects the nearest eligible tick.
            const double leftMs = std::chrono::duration<double, std::milli>(deadline - Clock::now()).count();
            ms = std::max(0, int(std::floor(leftMs - displayPeriodUs_.load() / 1000.0)));
        }
        auto update = [this] {
            realUpdateScheduled_ = false;
            if (active_.load()) win_->update();
        };
        if (ms > 0) QTimer::singleShot(ms, Qt::PreciseTimer, this, update);
        else update();
    }, Qt::QueuedConnection);
}

bool LiveVulkan::showPendingReal() {
    win_->beginExternalCommands();
    auto* rif = win_->rendererInterface();
    auto* cb = static_cast<VkCommandBuffer*>(rif->getResource(win_, QSGRendererInterface::CommandListResource));
    const bool recorded = cb && *cb;
    if (recorded) chain_->recordShowReal(*cb);
    win_->endExternalCommands();
    if (recorded) noteShown();
    return recorded;
}

void LiveVulkan::onBeforeRendering() {
    if (!active_.load()) return;
    bool fresh = false;
    EnhanceSettings s;
    Clock::time_point arrival{};
    uint64_t nvidiaEpoch = 0;
    {
        std::unique_lock l(m_);
        s = settings_;
        nvidiaEpoch = nvidiaEpoch_;
        // A timer and the next captured picture can reach Qt in the same refresh. Keep
        // that input in the mailbox until its predecessor's real frame has been presented.
        if (s.nvidiaFg() && !resetFg_.load() && !(hasNew_ && mailbox_.reset) && chain_->fgShowRealPending()) {
            const bool redrawAfterReal = hasNew_ || redraw_;
            l.unlock();
            const auto tolerance = std::chrono::microseconds(displayPeriodUs_.load() / 2);
            if (Clock::now() < realDue_ - tolerance || !showPendingReal()) scheduleRealUpdate(true);
            else {
                noteNvidiaPublication(nvidiaEpoch, false);
                if (redrawAfterReal) QMetaObject::invokeMethod(win_, "update", Qt::QueuedConnection);
            }
            return;
        }
        if (hasNew_) {
            std::swap(current_, mailbox_);
            hasNew_ = false;
            haveCurrent_ = true;
            redraw_ = false;
            fresh = true;
            arrival = lastArrival_;
        } else if (redraw_ && haveCurrent_) {
            redraw_ = false;
            fresh = true;
            if (s.nvidiaFg()) resetFg_ = true;   // settings redraw, not a new temporal input
        }
    }
    if (!fresh) {
        // FSR frame generation: the generated frame is on screen; the real one follows half a frame later
        if (chain_->fgShowRealPending() && std::chrono::steady_clock::now() >= realDue_ - std::chrono::microseconds(1500)) {
            showPendingReal();
        }
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (resetFg_.exchange(false) || current_.reset) chain_->resetFgHistory();
    if (current_.reset) { chain_->resetHistory(); current_.reset = false; }   // a video file was seeked
    LiveInput in;
    switch (current_.fmt) {
    case RawFrame::YUYV: in.fmt = LiveInput::YUYV; break;
    case RawFrame::UYVY: in.fmt = LiveInput::UYVY; break;
    case RawFrame::NV12: in.fmt = LiveInput::NV12; break;
    case RawFrame::P010: in.fmt = LiveInput::P010; break;
    default: in.fmt = LiveInput::RGBA; break;
    }
    in.data = current_.bytes();
    in.size = current_.size();
    in.w = current_.w; in.h = current_.h; in.stride = current_.stride;
    in.time = current_.captured.time_since_epoch().count() ? current_.captured : std::chrono::steady_clock::now();

    win_->beginExternalCommands();
    auto* rif = win_->rendererInterface();
    auto* cb = static_cast<VkCommandBuffer*>(rif->getResource(win_, QSGRendererInterface::CommandListResource));
    std::string err;
    double frameMs;
    { std::lock_guard l(m_); frameMs = frameMs_; }
    if (s.nvidiaFg() && s.nvidiaAutoRegulate && arrival != Clock::time_point{}) {
        // The generated picture owns the first half of the input period; the real
        // one follows it. Anchor the budget to the input's arrival, before Qt's
        // render callback, so an already delayed pair cannot start a fresh budget.
        in.generatedDue = arrival + std::chrono::microseconds(int(std::clamp(frameMs * 500.0, 3000.0, 30000.0)));
    }
    s.fitW = fitW_.load(); s.fitH = fitH_.load();   // the window's pixel size for the picture (VideoItem)
    bool ok = cb && *cb && chain_->recordLive(*cb, win_->graphicsStateInfo().currentFrameSlot, in, s, err, float(frameMs));
    win_->endExternalCommands();
    if (!ok) {
        std::lock_guard l(m_);
        error_ = err.empty() ? "Live path unavailable" : err;
        if (s.nvidiaFg() && settings_.nvidiaFg()) { nvidiaVfgError_ = error_; nvidiaVfgActive_ = false; }
        static int warned = 0;
        if (warned++ < 3) qWarning() << "LiveVulkan: record failed:" << QString::fromStdString(error_);
        return;
    }
    {
        std::lock_guard l(m_);
        error_.clear();
        if (s.nvidiaFg() && settings_.nvidiaFg() && nvidiaEpoch == nvidiaEpoch_) {
            nvidiaVfgError_ = chain_->fgError();
            nvidiaVfgActive_ = chain_->fgShowRealPending();
            nvidiaStats_.models = chain_->nvidiaModelStats();
        }
        upError_ = (s.nvidiaFg() || chain_->fgError().empty()) ? chain_->upError() : "Frame generation: " + chain_->fgError();
    }
    const ImageRes& real = chain_->liveDisplay();
    const auto tEnd = std::chrono::steady_clock::now();
    lastMs_ = std::chrono::duration<double, std::milli>(tEnd - t0).count();
    // Time inside VeyAmux: from the capture driver's timestamp to the moment the frame's GPU work is queued, plus the
    // measured GPU time - i.e. until the picture is ready for the display (scan-out and the card's own delay excluded).
    {
        std::lock_guard l(m_);
        gpuMs_ = chain_->gpuMs();
        gpuStages_ = chain_->gpuStageMs();
    }
    if (current_.captured.time_since_epoch().count()) {
        const double q = std::chrono::duration<double, std::milli>(tEnd - current_.captured).count();
        if (q >= 0 && q < 1000) {
            // NVIDIA submits GPU work asynchronously across two APIs. The Vulkan timer
            // covers only some stages; for this mode report capture-to-app-publication.
            const double g = s.nvidiaFg() ? 0 : std::max(0.0f, chain_->gpuMs());
            const double v = q + g, o = pipeMs_.load();
            pipeMs_ = o <= 0 ? v : o + 0.1 * (v - o);
        }
    }
    const bool recreated = chain_->liveGeneration() != seenGen_;   // images were recreated (size change)
    seenGen_ = chain_->liveGeneration();
    ++seq_;
    const uint64_t realHandle = uint64_t(reinterpret_cast<uintptr_t>(real.image));
    if (qEnvironmentVariableIsSet("VEYAMUX_DEBUG_FG")) {
        static int real = 0;
        ++real;
        if (real % 10 == 0) qInfo("LiveVulkan: %d frames, GPU %.3f ms, CPU %.2f ms, capture interval %.1f ms, output %ux%u%s%s", real,
                                     double(chain_->gpuMs()), lastMs_.load(), frameMs,
                                     chain_->liveOutput().width, chain_->liveOutput().height,
                                     chain_->upError().empty() ? "" : ", upscaling error: ", chain_->upError().c_str());
    }
    publish(realHandle, int(real.width), int(real.height), recreated);
    if (s.nvidiaFg()) noteNvidiaPublication(nvidiaEpoch, chain_->fgShowRealPending(), lastMs_.load());
    if (chain_->fgShowRealPending()) {
        // show the real frame half a capture interval after the generated one
        const int halfUs = int(std::clamp(frameMs * 500.0, 3000.0, 30000.0));
        realDue_ = tEnd + std::chrono::microseconds(halfUs);
        if (s.nvidiaFg() && arrival != Clock::time_point{}) {
            // A slow generation or late Qt callback can consume much of a source
            // period. Do not hold this real picture past the next source deadline:
            // its successor must still be able to reach the renderer on time.
            const auto nextInput = arrival + std::chrono::microseconds(2 * halfUs);
            realDue_ = std::min(realDue_, std::max(tEnd, nextInput));
        }
        scheduleRealUpdate(s.nvidiaFg());
    }
}

void LiveVulkan::resetNvidiaStats() {
    ++nvidiaEpoch_;
    nvidiaStarted_ = {};
    nvidiaStats_ = {};
    nvidiaRecent_.clear();
}

void LiveVulkan::noteNvidiaPublication(uint64_t epoch, bool generated, double processingMs) {
    const auto now = Clock::now();
    std::lock_guard l(m_);
    if (epoch != nvidiaEpoch_) return;  // a source or mode was changed during rendering
    if (nvidiaStarted_ == Clock::time_point{}) nvidiaStarted_ = now;
    if (generated) ++nvidiaStats_.generated;
    else ++nvidiaStats_.real;
    if (processingMs >= 0) {
        auto& average = nvidiaStats_.processingMs;
        average = average > 0 ? average + 0.1 * (processingMs - average) : processingMs;
    }
    while (!nvidiaRecent_.empty() && now - nvidiaRecent_.front().time >= std::chrono::seconds(2))
        nvidiaRecent_.pop_front();
    nvidiaRecent_.push_back({now, generated});
}

LiveVulkan::NvidiaFrameStats LiveVulkan::nvidiaFrameStats() const {
    std::lock_guard l(m_);
    auto result = nvidiaStats_;
    if (nvidiaRecent_.empty()) return result;
    const auto now = Clock::now();
    const auto start = std::max(now - std::chrono::seconds(2), nvidiaStarted_);
    const auto seconds = std::chrono::duration<double>(now - start).count();
    if (seconds < 0.25) return result;
    for (const auto& frame : nvidiaRecent_) {
        if (frame.time <= start) continue;
        if (frame.generated) ++result.generatedFps;
        else ++result.realFps;
    }
    result.realFps /= seconds;
    result.generatedFps /= seconds;
    return result;
}

void LiveVulkan::noteShown() {
    ++shown_;
    const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard l(timesM_);
    shownTimes_.push_back(t);
    while (shownTimes_.size() > 600) shownTimes_.pop_front();
}

std::vector<double> LiveVulkan::shownTimes() const {
    std::lock_guard l(timesM_);
    return {shownTimes_.begin(), shownTimes_.end()};
}

void LiveVulkan::publish(uint64_t handle, int w, int h, bool recreated) {
    noteShown();
    imgW_ = w;
    imgH_ = h;
    image_ = handle;
    if (handle != lastHandle_.load() || recreated) {
        lastHandle_ = handle;
        gen_ = gen_.load() + 1;                     // VideoItem re-wraps the image on its next sync
        emit outputChanged();
    }
}

