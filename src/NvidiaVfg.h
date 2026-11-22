// Native NVIDIA Video Effects SDK interpolation of a pair of finished RGBA8 frames.
#pragma once
#include "VkContext.h"
#include <memory>
#include <limits>

namespace veyamux {
class NvidiaVfg {
public:
    struct Stats {
        int quality = 1, activeQuality = 1;
        bool fastReady = false;
        uint64_t primaryPairs = 0, fastPairs = 0;
        double primaryMs = 0, fastMs = 0;
        std::string fastStatus;
    };
    explicit NvidiaVfg(VkContext& ctx);
    ~NvidiaVfg();
    bool prepare(uint32_t width, uint32_t height, int quality = 1, bool autoRegulate = false);
    Stats stats() const;
    void capture(VkCommandBuffer cb, const ImageRes& source);
    bool submitCapture(std::string& error);     // synchronous diagnostic's command buffer
    bool submitLiveCapture(std::string& error); // asynchronous Qt frame-slot producer
    bool usesDeviceExchange() const;
    // Enqueue CUDA after Vulkan's producer via GPU semaphore. First input seeds history.
    bool interpolate(bool reset, bool& generated, double budgetMs = std::numeric_limits<double>::infinity());
    void upload(VkCommandBuffer cb, const ImageRes& target);
    const std::string& error() const { return error_; }
private:
    struct Session;
    VkContext& ctx_;
    std::unique_ptr<Session> session_;

    uint32_t width_ = 0, height_ = 0;
    int quality_ = 1;
    bool regulate_ = false;
    std::string error_;
};
}
