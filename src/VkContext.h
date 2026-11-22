// VeyAmux for Linux - minimal Vulkan compute context.
// Shaders are the original Veyra (Windows) HLSL, compiled to SPIR-V by DXC with
//   -fvk-t-shift 0 / -fvk-u-shift 16 / -fvk-b-shift 32 / -fvk-s-shift 48
// so every shader fits ONE universal descriptor set layout (see below).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace veyamux {

constexpr uint32_t kBindSampled = 0;    // t0..t11  (SAMPLED_IMAGE)
constexpr uint32_t kSampledCount = 12;
constexpr uint32_t kBindStorage = 16;   // u0..u7   (STORAGE_IMAGE)
constexpr uint32_t kStorageCount = 8;
constexpr uint32_t kBindUniform = 32;   // b0       (UNIFORM_BUFFER)
constexpr uint32_t kBindSampler = 48;   // s0..s1   (SAMPLER)
constexpr uint32_t kSamplerCount = 2;

struct BufferRes {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct ImageRes {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, depth = 1;
};

class VkContext {
public:
    VkContext() = default;
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    bool init(std::string& error);
    // Shares an instance created elsewhere (Qt's QVulkanInstance). The device is created with VK_KHR_swapchain so Qt
    // can render with it too (QQuickGraphicsDevice::fromDeviceObjects). The instance is not destroyed by us.
    bool initShared(VkInstance instance, std::string& error);
    bool ok() const { return device_ != VK_NULL_HANDLE; }

    const std::string& deviceName() const { return deviceName_; }
    bool isSoftware() const { return software_; }
    VkDevice device() const { return device_; }
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return phys_; }
    uint32_t queueFamily() const { return family_; }
    static constexpr int kSlots = 3;
    uint32_t apiVersion() const { return apiVersion_; }
    VkCommandBuffer cmd() const { return cmd_; }
    VkCommandBuffer active() const { return active_; }   // the buffer dispatch()/computeBarrier() record into
    VkFormat lumaFormat() const { return lumaFormat_; }

    // A dedicated device-local allocation that CUDA can import through an opaque file descriptor.
    bool createCudaExchange(VkDeviceSize bytes, BufferRes& buffer, VkDeviceSize& allocationSize, int& fd, std::string& error);
    bool waitExternal(VkSemaphore semaphore, std::string& error);
    BufferRes createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostCachedReadback);
    void destroy(BufferRes& b);
    // Images are created in GENERAL layout (all VeyAmux passes use GENERAL).
    ImageRes createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                         uint32_t depth = 1, bool volume = false);
    void destroy(ImageRes& i);
    // View over an image owned by someone else (e.g. the FidelityFX backend). Destroy with destroyView().
    ImageRes wrapImage(VkImage image, VkFormat fmt, uint32_t w, uint32_t h);
    void destroyView(ImageRes& i);
    // One-shot upload of tightly packed rows into an image in GENERAL layout (waits for completion). For small tables.
    bool uploadImage(const ImageRes& image, const void* data, uint32_t rowBytes);
    // One-shot UNDEFINED -> GENERAL transition (waits for completion).
    void transitionToGeneral(VkImage image) { toGeneral(image); }

    VkPipeline createComputePipeline(const std::vector<uint32_t>& spirv, std::string& error);
    void destroy(VkPipeline p);

    // Per-frame command recording.
    void beginFrame(int slot = 0);
    // Record into a command buffer owned by someone else (Qt's frame). slot = Qt frame slot (descriptor/UBO ring).
    void beginExternal(VkCommandBuffer cb, int slot);
    // Split a Qt frame into an app-owned producer and Qt's consumer on the same queue.
    bool beginInteropFrame(int slot, std::string& error);
    bool submitInterop(VkSemaphore signal, std::string& error);
    void resumeExternal(VkCommandBuffer cb) { active_ = cb; }
    void computeBarrier();   // compute/transfer -> compute/transfer
    void dispatch(VkPipeline pipeline,
                  const ImageRes* const* sampled, uint32_t sampledCount,
                  const ImageRes* const* storage, uint32_t storageCount,
                  const void* uniform, uint32_t uniformSize,
                  uint32_t groupsX, uint32_t groupsY);
    bool submitAndWait(std::string& error, VkSemaphore signal = VK_NULL_HANDLE);

private:
    bool finishInit(std::string& error, bool shared);
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    void toGeneral(VkImage image);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer interopCmds_[kSlots]{};
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPools_[kSlots] = {};
    VkCommandBuffer active_ = VK_NULL_HANDLE;
    int slot_ = 0;
    bool ownsInstance_ = true;
    VkSampler pointSampler_ = VK_NULL_HANDLE;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    BufferRes uboRings_[kSlots];
    VkDeviceSize uboOffset_ = 0;
    VkDeviceSize uboAlign_ = 256;
    ImageRes dummySampled2d_, dummySampled3d_, dummyStorage_;
    VkFormat lumaFormat_ = VK_FORMAT_R16_SFLOAT;
    std::string deviceName_;
    bool software_ = false;
    uint32_t apiVersion_ = 0;
};

} // namespace veyamux
