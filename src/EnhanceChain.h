// VeyAmux for Linux - GPU enhancement chain built from the original Veyra (Windows) HLSL shaders (compiled to SPIR-V).
//   RGBA8 -> RgbToLinear -> [AaLuma+AaFxaa] -> [SGSR, AMD FSR 1, NIS or AMD FSR 3.1 upscale] -> [Sharpen/Grade] -> ScaleBlit(sRGB encode) -> RGBA8
#pragma once
#include <chrono>
#include <atomic>
#include <mutex>
#include "FsrUpscaler.h"
#include "DlssUpscaler.h"
#include "FsrFrameGen.h"
#include "NvidiaVfg.h"
#include "MotionEstimator.h"
#include "GpuTimer.h"
#include <array>
#include "VkContext.h"
#include <map>
#include <string>
#include <vector>

namespace veyamux {

// Frame generation modes. Both are applied by the separately installed lsfg-vk Vulkan layer (Lossless Scaling) on
// VeyAmux's window after VeyAmux has shown its frame, so the chain itself does nothing for them. Values 1 and 4 were
// in-app frame generation (AMD FSR 3.1, later RIFE) and are read as LsfgFixed.
// Lossless Scaling 2x / 3x (lsfg-vk), or AMD FSR 3.1 frame generation inside VeyAmux (free, live capture only)
enum class FrameGenMode { Off = 0, LsfgFixed = 2, LsfgFixed3 = 3, FsrFree = 5, NvidiaVfg = 6 };
// Denoisers (run first, on the source-size linear image, before FXAA/upscaling/sharpening).
enum class Denoise { Off = 0, Spatial = 1, Temporal = 2, Both = 3 };
// Upscaling method used when scale > 1. AmdFsr3 falls back to SGSR if FidelityFX cannot run.
enum class Upscaler { Sgsr = 0, AmdFsr3 = 1, AmdFsr1 = 2, Nis = 3, NvidiaDlss = 4 };   // NIS = NVIDIA Image Scaling (spatial, any GPU)   // FSR 1 = EASU + RCAS (spatial, one frame)

struct EnhanceSettings {
    bool enabled = true;
    float scale = 1.0f;       // output scale factor (>=1)
    Upscaler upscaler = Upscaler::Sgsr;
    bool fxaa = false;
    bool deflicker = false;   // anti-flicker: calms pixels that flicker between frames (LxDeflicker)
    // FSR 3.1 at the native size (scale 1): AMD's "native AA", i.e. temporal anti-aliasing
    bool fsrNativeAa() const { return enabled && upscaler == Upscaler::AmdFsr3 && scale <= 1.001f && !retroTv && !pixelPerfect; }
    Denoise denoise = Denoise::Off;
    int denoiseStrength = 1;  // 0 light, 1 medium, 2 strong
    float hdrPeak = 1000.0f;  // HDR capture (P010): brightest level of the source in nits, tone-mapped to SDR white
    bool hdrOut = false;      // HDR display output (scRGB swapchain): out8_ holds PQ-coded light instead of sRGB
    float displayPeak = 1000.0f;   // HDR output: brightest level of the display in nits (HDR sources are mapped to it)
    bool vrr = false;         // live: show the game's own pictures at an even pace (for VRR displays; frame generation off)
    bool pacingRepair = false;
    int nvidiaQuality = 1;          // VFX SDK Low / Medium / High
    bool nvidiaAutoRegulate = true;
    // live: the exact pixel size the picture covers in the window (0 = unknown / pixel-perfect): the finished picture is
    // resampled to it (LxFit, Catmull-Rom) so Qt draws it 1:1 instead of stretching it bilinearly
    int fitW = 0, fitH = 0;
    bool hudLock = false;     // FSR frame generation: still HUD pixels in generated frames come from the real frame   // live with frame generation: re-time a locked game's unevenly shown pictures (MediaEngine)
    int fxaaQuality = 1;      // 0 medium, 1 high, 2 ultra (VeyAmux AaFxaa3/4/5; SMAA medium / high / ultra)
    bool smaa = false;        // anti-aliasing with SMAA 1x instead of FXAA (when fxaa is on)
    // Colour effects (Veyra's ColorDetail): fine texture, midtone clarity, dehaze; -1..1, 0 = off
    float texture = 0.0f, clarity = 0.0f, dehaze = 0.0f;
    bool detailActive() const { return texture != 0.0f || clarity != 0.0f || dehaze != 0.0f; }
    float sharpen = 0.0f;     // 0..1
    float exposure = 0.0f;    // EV
    float contrast = 1.0f;
    float saturation = 1.0f;
    FrameGenMode frameGen = FrameGenMode::Off;
    // "Retro TV": CRT-Geom (curved CRT, scanlines, aperture grille) as the last picture stage, at the output size.
    // Like frame generation it does not depend on the "Enhance" switch. retroLines: simulated lines (0 = the source's).
    bool retroTv = false;
    int retroLines = 240;
    // Pixel-perfect: no smoothing upscaler; the picture is sent at its pixel grid and the display enlarges it by a whole
    // number with sharp pixels. pixelGrid 0 = the signal's own pixels, 1 = the measured game resolution (gridW x gridH,
    // set by the player from the content measurement; 0 = unknown). Retro TV comes first when both are on.
    bool pixelPerfect = false;
    int pixelGrid = 0;
    int gridW = 0, gridH = 0;
    bool pixelActive() const { return pixelPerfect && !retroTv; }
    // Frame generation is independent of the "Enhance" switch (that one only covers upscale/AA/sharpen/grade).
    bool lsfgFg() const { return frameGen == FrameGenMode::LsfgFixed || frameGen == FrameGenMode::LsfgFixed3; }
    bool fgPaused = false;    // ~120 Hz capture: no generated frames
    bool nvidiaFg() const { return frameGen == FrameGenMode::NvidiaVfg && !fgPaused; }
    bool fsrFg() const { return frameGen == FrameGenMode::FsrFree && !fgPaused; }

    bool isIdentity() const {
        return !retroTv && !pixelPerfect && (!enabled || (scale <= 1.001f && !fxaa && !deflicker && upscaler != Upscaler::AmdFsr3 &&
                            denoise == Denoise::Off && sharpen <= 0.001f && exposure == 0.0f &&
                            contrast == 1.0f && saturation == 1.0f && !detailActive()));
    }
};

// A raw frame handed to the zero-copy live path (everything after the upload happens on the GPU).
struct LiveInput {
    std::chrono::steady_clock::time_point generatedDue{}; // submission target for the intermediate picture
    enum Format { RGBA, YUYV, UYVY, NV12, P010 } fmt = RGBA;   // P010: HDR (PQ, BT.2020), tone-mapped to SDR
    std::chrono::steady_clock::time_point time{};               // capture time (recording timestamps)
    const uint8_t* data = nullptr;
    size_t size = 0;
    int w = 0, h = 0, stride = 0;     // stride in bytes (Y-plane stride for NV12)
    bool fullRange = false;
};

class EnhanceChain {
public:
    EnhanceChain(VkContext& ctx, std::string shaderDir);
    ~EnhanceChain();

    bool init(std::string& error);
    // Runs the chain on a tightly- or loosely-packed RGBA8 frame. Output is tightly packed RGBA8.
    // frameMs: time since the previous source frame (for the temporal passes).
    bool process(const uint8_t* rgba, int w, int h, int strideBytes, const EnhanceSettings& s,
                 std::vector<uint8_t>& out, int& outW, int& outH, std::string& error, float frameMs = 1000.0f / 30.0f,
                 int hdr = 0);   // hdr: 0 RGBA8; 1 PQ / 2 HLG = RGBA64 (16-bit R'G'B', BT.2020), tone-mapped to SDR
    // Zero-copy live path: records upload + conversion + enhancement into a command buffer that belongs to the
    // caller (Qt's frame). No submit, no fence, no readback: the result is liveOutput(), an RGBA8 GENERAL image.
    bool recordLive(VkCommandBuffer cb, int slot, const LiveInput& in, const EnhanceSettings& s, std::string& error,
                    float frameMs = 1000.0f / 60.0f);
    const ImageRes& liveOutput() const { return out8_; }
    // The image Qt shows: out8_, or with FSR frame generation a separate image that gets the generated frame first
    // and the real one half a frame later (recordShowReal).
    const ImageRes& liveDisplay() const { return fgActive_ ? disp_ : out8_; }
    void resetFgHistory() { fgReset_ = true; }
    bool fgShowRealPending() const { return fgActive_ && fgRealPending_; }
    void recordShowReal(VkCommandBuffer cb);
    const std::string& fgError() const { return fgError_; }
    NvidiaVfg::Stats nvidiaModelStats() const { return nvidiaVfg_ ? nvidiaVfg_->stats() : NvidiaVfg::Stats{}; }
    // Measured GPU time (Vulkan timestamps), smoothed: total and per stage; -1 when not available.
    float gpuMs() const { return timer_.totalMs(); }
    std::array<float, GpuTimer::kStages> gpuStageMs() const { return timer_.stageMs(); }
    std::string upError() const { return upError_; }
    FsrUpscaler* upscaler() { return fsrUp_.get(); }   // test hook
    uint64_t liveGeneration() const { return liveGen_; }
    void resetHistory() { upHistory_ = false; nrHistory_ = false; dfHistory_ = false; }
    static void outputSize(int w, int h, const EnhanceSettings& s, int& ow, int& oh);

    // OBS stream (live path): when maxHeight >= 0, every displayed image is downscaled to fit
    // maxHeight (0 = original size) and copied to host memory inside the live command buffer. pollStream() (any
    // thread) returns the copies the GPU has finished, oldest first.
    struct StreamFrame { std::vector<uint8_t> rgba; int w = 0, h = 0; int delayMs = 0; };
    void setStreamMaxHeight(int h) { streamMaxH_ = h; }
    void pollStream(std::vector<StreamFrame>& out);

    // Recording (live path): when maxHeight >= 0 every displayed image is converted to NV12 at that height
    // (0 = original size) and copied to host memory, like the OBS stream. pollRecord() returns the finished copies.
    struct RecFrame { std::vector<uint8_t> nv12; int w = 0, h = 0; std::chrono::steady_clock::time_point t{}; };
    void setRecordMaxHeight(int h) { recMaxH_ = h; }
    void pollRecord(std::vector<RecFrame>& out);
    // Comparison clips (live path, while recording): the recorded picture is the original next to VeyAmux's.
    // mode 0 off, 1 split, 2 side by side, 3 vertical 1080x1920, 4 reveal (a sweeping line). Labels: RGBA8 pictures
    // (sRGB, straight alpha) drawn over the clip; empty = none. Any thread.
    void setCompare(int mode) { cmpMode_ = mode; cmpStart_ = std::chrono::steady_clock::now(); }
    int compareMode() const { return cmpMode_.load(); }
    void setCompareLabels(std::vector<uint8_t> a, int aw, int ah, std::vector<uint8_t> b, int bw, int bh);
    // Screenshot (live path): the next displayed image is copied at full size; pollScreenshot() returns it once.
    void requestScreenshot() { shotWanted_ = true; }
    bool pollScreenshot(StreamFrame& out);

private:
    struct StreamSlot {
        ImageRes img; BufferRes buf; VkEvent ev = VK_NULL_HANDLE;
        bool pending = false; uint64_t seq = 0; int delayMs = 0;
    };
    static constexpr int kStreamSlots = 6;
    StreamSlot stream_[kStreamSlots];
    std::mutex streamM_;
    std::atomic<int> streamMaxH_{-1};
    uint64_t streamSeq_ = 0;
    void recordStream(VkCommandBuffer cb, const ImageRes& src, int delayMs);
    void releaseStream();
    struct RecSlot {
        ImageRes y, uv; BufferRes buf; VkEvent ev = VK_NULL_HANDLE;
        bool pending = false; uint64_t seq = 0; std::chrono::steady_clock::time_point t{};
    };
    static constexpr int kRecSlots = 6;
    RecSlot rec_[kRecSlots];
    std::atomic<int> recMaxH_{-1};
    uint64_t recSeq_ = 0;
    void recordRec(VkCommandBuffer cb, const ImageRes& src, std::chrono::steady_clock::time_point t, bool keepSize = false);
    std::atomic<int> cmpMode_{0};
    std::atomic<std::chrono::steady_clock::time_point> cmpStart_{};
    std::mutex cmpM_;
    struct Label { std::vector<uint8_t> rgba; int w = 0, h = 0; };
    Label cmpLabNew_[2];
    bool cmpLabDirty_ = false;
    ImageRes cmp_, cmpLab_[2];
    const ImageRes* recordCompare(int mode);
    StreamSlot shot_;
    std::atomic<bool> shotWanted_{false};
    void recordShot(VkCommandBuffer cb, const ImageRes& src);
    void run(const char* k, std::initializer_list<const ImageRes*> in, std::initializer_list<const ImageRes*> outs,
             const void* ubo, uint32_t uboSize, int dw, int dh, int g);
    void recordPost(const ImageRes* start, const EnhanceSettings& s);
    // HDR sources: the level the source is tone-mapped to (1.0 in the working image): SDR white, or the display's peak
    static float hdrTarget(const EnhanceSettings& s) {
        return s.hdrOut ? std::clamp(std::min(s.displayPeak, s.hdrPeak), 203.0f, 10000.0f) : 203.0f;
    }
    float frameNits_ = 203.0f;
    bool frame2020_ = false;   // HDR output: the working image holds BT.2020 (HDR source) instead of BT.709
    std::atomic<bool> outPq_{false};
    uint32_t pqFrame_ = 0;
    void retire(ImageRes& img);
    void collectRetired();
    bool ensureLive(int w, int h, int ow, int oh, std::string& error, int dw = 0, int dh = 0);
    bool loadKernel(const std::string& name, std::string& error);
    bool ensureResources(int w, int h, int ow, int oh, std::string& error, int dw = 0, int dh = 0);
    int dw_ = 0, dh_ = 0;   // out8_'s size: the window fit (LxFit) or ow x oh
    ImageRes pre8_;         // the finished picture at ow x oh before LxFit (only when dw/dh differ)
    void releaseResources();
    VkPipeline kernel(const char* name) const;

    VkContext& ctx_;
    std::string dir_;
    std::map<std::string, VkPipeline> kernels_;

    int w_ = 0, h_ = 0, ow_ = 0, oh_ = 0;
    ImageRes src_, lin_, linB_, luma_, up_, fin_, out8_;
    std::unique_ptr<FsrUpscaler> fsrUp_;
    std::unique_ptr<DlssUpscaler> dlssUp_;
    Upscaler previousUpscaler_ = Upscaler::Sgsr;
    std::unique_ptr<FsrFrameGen> fg_;
    std::unique_ptr<NvidiaVfg> nvidiaVfg_;
    FrameGenMode fgMode_ = FrameGenMode::Off;
    uint32_t crtFrame_ = 0;          // Retro TV interlace field
    ImageRes nisScale_, nisUsm_;     // NIS filter coefficient tables (2 x 64 RGBA32F each)
    // SMAA 1x: edges, blending weights (source size) and the two lookup tables
    ImageRes smaaEdges_, smaaWeights_, smaaArea_, smaaSearch_;
    bool ensureSmaa(int w, int h);
    // colour effects: the low-resolution base (ping-pong) and the result at the output size
    ImageRes detLow_[2], detOut_;
    const ImageRes* recordDetail(const ImageRes* in, const EnhanceSettings& s, int ow, int oh);
    BufferRes nisStaging_;
    bool recordNis(const ImageRes& src, int w, int h, int ow, int oh);
    ImageRes fgOut_, disp_;
    // HUD lock (LxHudUpdate / LxHudCompose): per-pixel state, the previous real frame (ping-pong), the composed frame
    ImageRes hudState_, hudPrev_[2], hudOut_;
    int hudCur_ = 0;
    bool hudLock_ = false, hudReset_ = true;
    bool fgActive_ = false, fgRealPending_ = false, fgReset_ = true;
    std::string fgError_;
    bool recordFg(VkCommandBuffer cb, bool nvidia, std::chrono::steady_clock::time_point generatedDue);
    void copyToDisplay(VkCommandBuffer cb, const ImageRes& src);
    // denoisers
    std::unique_ptr<MotionEstimator> nrMotion_;
    GpuTimer timer_;
    ImageRes nrOut_, nrHist_[2];
    int nrCur_ = 0;
    bool nrHistory_ = false;          // the temporal denoiser has a previous result of this sequence
    const ImageRes* recordDenoise(const ImageRes* in, const EnhanceSettings& s, const ImageRes* mv);
    // the motion of this frame (one estimate shared by anti-flicker and the temporal denoiser); null on a reset
    const ImageRes* recordMotion(const ImageRes* in);
    ImageRes dfOut_, dfPrev_[2], dfAl_[2], dfPrev2_[2];
    int dfCur_ = 0;
    bool dfHistory_ = false;
    const ImageRes* recordDeflicker(const ImageRes* in, const ImageRes* mv);
    bool upHistory_ = false;          // FSR upscaling has seen the previous frame of this sequence
    std::string upError_;
    uint64_t upFailedKey_ = 0;        // sizes at which FSR upscaling failed to start (0 = none)
    float frameMs_ = 1000.0f / 30.0f; // time since the previous source frame (for the temporal passes)
    BufferRes staging_, readback_;
    // live path
    ImageRes rawPacked_, rawY_, rawUV_, rawY16_, rawUV16_, src16_;   // src16_: HDR video (RGBA64)
    BufferRes staging16_;   // the 16-bit planes only exist for P010 (HDR)
    BufferRes liveStaging_[VkContext::kSlots];
    VkDeviceSize liveStagingSize_ = 0;
    int liveW_ = 0, liveH_ = 0;
    uint64_t liveGen_ = 0;
    bool liveMode_ = false;
    struct Retired { ImageRes img; int framesLeft; };
    std::vector<Retired> retired_;   // old output images stay alive until Qt has dropped its texture wrapper
};

} // namespace veyamux
