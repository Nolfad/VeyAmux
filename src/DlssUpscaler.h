#pragma once
#include "VkContext.h"
#include <memory>

namespace veyamux {

class DlssUpscaler {
public:
    DlssUpscaler(VkContext& context, const std::string& shaderDirectory);
    ~DlssUpscaler();
    bool record(VkCommandBuffer command, const ImageRes& input, const ImageRes& output,
                bool reset, float frameTimeMs, std::string& error);

    // The strings are owned by the returned vector and live through vkCreateInstance/vkCreateDevice.
    static std::vector<std::string> extensions(VkInstance instance = VK_NULL_HANDLE,
                                               VkPhysicalDevice physical = VK_NULL_HANDLE);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace veyamux
