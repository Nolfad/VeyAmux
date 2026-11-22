// Zero-copy live path. The capture thread hands over raw frames; the Qt scene-graph render thread records the whole
// enhancement chain into Qt's own frame command buffer (beforeRendering) on the SAME Vulkan device Qt renders with,
// and the resulting VkImage is shown directly as a texture. No CPU colour conversion, no readback, no extra copies
// (the same design as the Windows D3D12 capture path).
#pragma once
#include "CaptureSource.h"
#include "EnhanceChain.h"
#include <QImage>
#include <QObject>
#include <functional>
#include <thread>
#include <QSize>
#include <QString>
#include <atomic>
#include <vector>
#include <deque>
#include <chrono>
#include <memory>
#include <mutex>

class QQuickWindow;

class LiveVulkan : public QObject {
    Q_OBJECT
public:
    LiveVulkan(QQuickWindow* window, veyamux::VkContext* ctx, const QString& shaderDir, QObject* parent = nullptr);
    ~LiveVulkan() override;

    bool init(std::string& error);                         // load pipelines (call before the window renders)
    void submit(veyamux::RawFrame& frame);                   // capture thread; swaps: `frame` gets a used buffer back
    void setSettings(const veyamux::EnhanceSettings& s);
    void setActive(bool on);
    bool active() const { return active_.load(); }

    uint64_t imageHandle() const { return image_.load(); }  // VkImage as integer (published by the render thread)
    QSize imageSize() const { return QSize(imgW_.load(), imgH_.load()); }
    // The exact pixel size the picture covers in the window (VideoItem, GUI thread); the chain resamples to it.
    void setDisplayFit(QSize px);
    std::atomic<int> fitW_{0}, fitH_{0};
    uint64_t generation() const { return gen_.load(); }
    double lastMs() const { return lastMs_.load(); }
    // VeyAmux's share of the latency: capture driver timestamp -> GPU work done (smoothed), 0 = unknown
    double pipelineMs() const { return pipeMs_.load(); }
    float gpuMs() const { std::lock_guard l(m_); return gpuMs_; }
    std::array<float, veyamux::GpuTimer::kStages> gpuStageMs() const { std::lock_guard l(m_); return gpuStages_; }
    uint64_t framesShown() const { return shown_.load(); }   // frames handed to the display
    // steady-clock times (ms) of the last pictures put on screen (real and generated), for the frame-time overlay
    std::vector<double> shownTimes() const;
    // OBS stream: maxHeight -1 = off, 0 = original size. The sink gets every displayed picture
    // from a worker thread, with the delay that keeps the on-screen pacing.
    void setStream(int maxHeight, std::function<void(QImage, int)> sink);
    // Recording: maxHeight -1 = off, 0 = original. The sink gets every displayed picture as NV12 (worker thread).
    void setRecord(int maxHeight, std::function<void(veyamux::EnhanceChain::RecFrame&&)> sink);
    // Comparison clips (EnhanceChain::setCompare): 0 off, 1 split, 2 side by side, 3 vertical, 4 reveal; labels RGBA8888.
    void setCompare(int mode, const QImage& labelOriginal = QImage(), const QImage& labelVeyamux = QImage());
    // The next displayed picture at full size, delivered once to the callback (worker thread).
    void requestScreenshot(std::function<void(QImage)> cb);
    QString lastError() const;
    QString lastUpError() const;
    QString nvidiaVfgError() const;
    bool nvidiaVfgActive() const { return nvidiaVfgActive_.load(); }
    struct NvidiaFrameStats {
        uint64_t real = 0, generated = 0;
        double realFps = 0, generatedFps = 0, processingMs = 0;
        veyamux::NvidiaVfg::Stats models{};
    };
    // Per activation. Counts pictures published by the application, including real-only fallback.
    NvidiaFrameStats nvidiaFrameStats() const;

signals:
    void outputChanged();                                  // a new image/size/active state: items must re-sync

public slots:
    void onBeforeRendering();                              // direct connection, scene-graph render thread

private:
    QQuickWindow* win_;
    veyamux::VkContext* ctx_;
    std::unique_ptr<veyamux::EnhanceChain> chain_;
    mutable std::mutex m_;
    veyamux::RawFrame mailbox_, current_;
    bool hasNew_ = false, haveCurrent_ = false, redraw_ = false;
    veyamux::EnhanceSettings settings_;
    std::string error_;
    std::string upError_, nvidiaVfgError_;
    std::atomic<bool> nvidiaVfgActive_{false};
    using Clock = std::chrono::steady_clock;
    struct NvidiaPublication { Clock::time_point time; bool generated; };
    std::deque<NvidiaPublication> nvidiaRecent_;
    Clock::time_point nvidiaStarted_{};
    NvidiaFrameStats nvidiaStats_;
    uint64_t nvidiaEpoch_ = 0;
    void resetNvidiaStats();   // with m_ held
    void noteNvidiaPublication(uint64_t epoch, bool generated, double processingMs = -1);
    std::atomic<bool> resetFg_{true};
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> image_{0}, gen_{0};
    std::atomic<int> imgW_{0}, imgH_{0};
    std::atomic<double> lastMs_{0};
    std::atomic<double> pipeMs_{0};
    float gpuMs_ = -1;
    std::array<float, veyamux::GpuTimer::kStages> gpuStages_{};
    uint64_t seenGen_ = 0;
    // frame generation pacing
    std::chrono::steady_clock::time_point lastArrival_{};
    double frameMs_ = 1000.0 / 60.0;     // smoothed capture interval
    std::atomic<uint64_t> seq_{0};        // bumps for every real frame rendered
    std::atomic<uint64_t> lastHandle_{0};
    std::atomic<uint64_t> shown_{0};
    void noteShown();
    void scheduleRealUpdate(bool nvidia = false);
    bool showPendingReal();
    std::atomic<bool> realUpdateScheduled_{false};
    std::atomic<int> displayPeriodUs_{16667};
    mutable std::mutex timesM_;
    std::deque<double> shownTimes_;
    std::mutex sinkM_;
    std::function<void(QImage, int)> sink_;
    std::function<void(veyamux::EnhanceChain::RecFrame&&)> recSink_;
    std::function<void(QImage)> shotCb_;
    std::chrono::steady_clock::time_point realDue_{};   // FSR frame generation: when the real frame is shown
    std::thread streamThread_;
    std::atomic<bool> streamQuit_{false};
    void streamLoop();
    void publish(uint64_t handle, int w, int h, bool recreated);
};
