#include "VkContext.h"
#include "DlssUpscaler.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace veyamux {
namespace {
constexpr uint32_t kMaxSetsPerFrame = 96;
constexpr VkDeviceSize kUboRingSize = 256 * 1024;

std::string vkErr(VkResult r) { return "VkResult " + std::to_string(int(r)); }
#define VK_TRY(expr, what)                                                                 \
    do {                                                                                   \
        VkResult _r = (expr);                                                              \
        if (_r != VK_SUCCESS) { error = std::string(what) + ": " + vkErr(_r); return false; } \
    } while (0)
} // namespace

VkContext::~VkContext() {
    if (!device_) {
        if (instance_ && ownsInstance_) vkDestroyInstance(instance_, nullptr);
        return;
    }
    vkDeviceWaitIdle(device_);
    destroy(dummySampled2d_);
    destroy(dummySampled3d_);
    destroy(dummyStorage_);
    for (auto& r : uboRings_) destroy(r);
    if (pointSampler_) vkDestroySampler(device_, pointSampler_, nullptr);
    if (linearSampler_) vkDestroySampler(device_, linearSampler_, nullptr);
    for (auto p : descPools_) if (p) vkDestroyDescriptorPool(device_, p, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    if (ownsInstance_) vkDestroyInstance(instance_, nullptr);
}

uint32_t VkContext::findMemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

bool VkContext::init(std::string& error) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VeyAmux";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    // Debug labels/names from the FidelityFX backend need VK_EXT_debug_utils (only used when present).
    const auto ngxInstanceExtensions = DlssUpscaler::extensions();
    std::vector<const char*> instExts;
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> av(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, av.data());
        for (auto& e : av) {
            if (std::strcmp(e.extensionName, "VK_EXT_debug_utils") == 0) instExts.push_back("VK_EXT_debug_utils");
            else for (const auto& name : ngxInstanceExtensions)
                if (name == e.extensionName) instExts.push_back(name.c_str());
        }
    }
    ici.enabledExtensionCount = uint32_t(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();
    VK_TRY(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");
    return finishInit(error, false);
}

bool VkContext::initShared(VkInstance instance, std::string& error) {
    instance_ = instance;
    ownsInstance_ = false;
    return finishInit(error, true);
}

bool VkContext::finishInit(std::string& error, bool shared) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) {
        error = "No Vulkan devices found (install the Mesa/RADV Vulkan driver)";
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());

    const char* forced = std::getenv("VEYAMUX_VK_DEVICE");
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(devs[i], &f);
        if (!f.shaderStorageImageReadWithoutFormat || !f.shaderStorageImageWriteWithoutFormat) continue;
        int score = 1;
        switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 100; break;
        default: score = 1; break;
        }
        if (forced && std::atoi(forced) == int(i)) score = 100000;
        if (score > bestScore) { bestScore = score; best = int(i); }
    }
    if (best < 0) {
        error = "No Vulkan device supports the required storage-image features";
        return false;
    }
    phys_ = devs[best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_, &props);
    deviceName_ = props.deviceName;
    software_ = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    uboAlign_ = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 256);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
    int fam = -1;
    for (uint32_t i = 0; i < qn; ++i)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { fam = int(i); break; }
    if (fam < 0)
        for (uint32_t i = 0; i < qn; ++i)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = int(i); break; }
    if (fam < 0) { error = "No compute queue"; return false; }
    family_ = uint32_t(fam);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Enable everything the GPU supports (Vulkan 1.1-1.3 core features): Qt's renderer and the FidelityFX backend pick
    // optional paths (FP16 shaders, wave64 via subgroup size control) from what the physical device reports, so the
    // device must actually have them enabled. Robustness features are left off (they only cost performance).
    VkPhysicalDeviceProperties dp;
    vkGetPhysicalDeviceProperties(phys_, &dp);
    const bool v13 = dp.apiVersion >= VK_API_VERSION_1_3, v12 = dp.apiVersion >= VK_API_VERSION_1_2;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    if (v12) f11.pNext = &f12;
    if (v13) f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(phys_, &f2);
    f2.features.robustBufferAccess = VK_FALSE;
    if (v13) f13.robustImageAccess = VK_FALSE;
    f2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    const auto ngxDeviceExtensions = props.vendorID == 0x10de ? DlssUpscaler::extensions(instance_, phys_) : std::vector<std::string>{};
    std::vector<const char*> devExts;
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> av(ne);
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, av.data());
        auto has = [&](const char* n) { for (auto& e : av) if (std::strcmp(e.extensionName, n) == 0) return true; return false; };
        if (shared) {
            if (!(qf[family_].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { error = "No graphics+compute queue family"; return false; }
            devExts.push_back("VK_KHR_swapchain");
        }
        // Promoted to core in 1.2/1.3, but the FidelityFX backend looks for the extension names.
        for (const char* e : {"VK_EXT_subgroup_size_control", "VK_KHR_shader_float16_int8", "VK_KHR_16bit_storage",
                               "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation"})
            if (has(e)) devExts.push_back(e);
        if (props.vendorID == 0x10de) {
            for (const char* extension : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
                if (has(extension)) devExts.push_back(extension);
        }
        for (const auto& name : ngxDeviceExtensions)
            if (has(name.c_str()) && std::none_of(devExts.begin(), devExts.end(),
                    [&](const char* e) { return name == e; })) devExts.push_back(name.c_str());
    }
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    apiVersion_ = dp.apiVersion;
    VK_TRY(vkCreateDevice(phys_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, family_, 0, &queue_);

    // Luma plane: R16F when it is filterable + storage-capable, otherwise R32F.
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R16_SFLOAT, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    lumaFormat_ = ((fp.optimalTilingFeatures & need) == need) ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R32_SFLOAT;

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family_;
    VK_TRY(vkCreateCommandPool(device_, &cpi, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_TRY(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");

    // Universal descriptor set layout (matches the DXC binding shifts).
    std::vector<VkDescriptorSetLayoutBinding> b;
    auto add = [&](uint32_t binding, VkDescriptorType t) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = binding;
        x.descriptorType = t;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b.push_back(x);
    };
    for (uint32_t i = 0; i < kSampledCount; ++i) add(kBindSampled + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    for (uint32_t i = 0; i < kStorageCount; ++i) add(kBindStorage + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    add(kBindUniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    for (uint32_t i = 0; i < kSamplerCount; ++i) add(kBindSampler + i, VK_DESCRIPTOR_TYPE_SAMPLER);
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = uint32_t(b.size());
    dsl.pBindings = b.data();
    VK_TRY(vkCreateDescriptorSetLayout(device_, &dsl, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    VK_TRY(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");

    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSampledCount * kMaxSetsPerFrame},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kStorageCount * kMaxSetsPerFrame},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSetsPerFrame},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerCount * kMaxSetsPerFrame}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kMaxSetsPerFrame;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = ps;
    for (int s = 0; s < kSlots; ++s)
        VK_TRY(vkCreateDescriptorPool(device_, &dpi, nullptr, &descPools_[s]), "vkCreateDescriptorPool");

    auto mkSampler = [&](VkFilter f, VkSampler& out) {
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s.magFilter = s.minFilter = f;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        return vkCreateSampler(device_, &s, nullptr, &out);
    };
    VK_TRY(mkSampler(VK_FILTER_NEAREST, pointSampler_), "vkCreateSampler");
    VK_TRY(mkSampler(VK_FILTER_LINEAR, linearSampler_), "vkCreateSampler");

    for (auto& r : uboRings_) r = createBuffer(kUboRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false);
    // Dummies keep statically-used-but-unneeded bindings (e.g. colour-grade tables) valid.
    dummySampled2d_ = createImage(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
    dummySampled3d_ = createImage(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT, 1, true);
    dummyStorage_ = createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
    if (!uboRings_[0].buffer || !uboRings_[kSlots - 1].buffer || !dummySampled2d_.image || !dummySampled3d_.image || !dummyStorage_.image) {
        error = "Could not create base Vulkan resources";
        return false;
    }
    return true;
}

BufferRes VkContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
    BufferRes r;
    r.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &r.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, r.buffer, &mr);
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = readback ? findMemoryType(mr.memoryTypeBits, base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : UINT32_MAX;
    if (mt == UINT32_MAX) mt = findMemoryType(mr.memoryTypeBits, base);
    if (mt == UINT32_MAX) {
        vkDestroyBuffer(device_, r.buffer, nullptr);
        return {};
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(device_, &ai, nullptr, &r.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, r.buffer, nullptr);
        return {};
    }
    vkBindBufferMemory(device_, r.buffer, r.memory, 0);
    vkMapMemory(device_, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped);
    return r;
}

bool VkContext::createCudaExchange(VkDeviceSize bytes, BufferRes& result, VkDeviceSize& allocationSize,
                                   int& fd, std::string& error) {
    const auto exportFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (!exportFd) { error = "Vulkan driver lacks opaque-FD GPU memory export"; return false; }
    constexpr auto handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    query.usage = usage;
    query.handleType = handleType;
    VkExternalBufferProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(phys_, &query, &properties);
    if (!(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        error = "GPU cannot export the VFG transfer buffer"; return false;
    }

    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = handleType;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &external;
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_TRY(vkCreateBuffer(device_, &bi, nullptr, &result.buffer), "Create GPU exchange buffer");
    result.size = bytes;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, result.buffer, &mr);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.buffer = result.buffer;
    VkExportMemoryAllocateInfo exporting{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exporting.pNext = &dedicated;
    exporting.handleTypes = handleType;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &exporting;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { error = "No device-local memory type for VFG"; return false; }
    VK_TRY(vkAllocateMemory(device_, &ai, nullptr, &result.memory), "Allocate VFG GPU memory");
    VK_TRY(vkBindBufferMemory(device_, result.buffer, result.memory, 0), "Bind VFG GPU memory");
    VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    info.memory = result.memory;
    info.handleType = handleType;
    VK_TRY(exportFd(device_, &info, &fd), "Export VFG GPU memory");
    allocationSize = mr.size;
    return true;
}

void VkContext::destroy(BufferRes& b) {
    if (!device_) return;
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

void VkContext::toGeneral(VkImage image) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer c;
    vkAllocateCommandBuffers(device_, &cai, &c);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    br.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    br.image = image;
    br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &br);
    vkEndCommandBuffer(c);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    vkResetFences(device_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(device_, pool_, 1, &c);
}

bool VkContext::uploadImage(const ImageRes& image, const void* data, uint32_t rowBytes) {
    if (!image.image || !data) return false;
    BufferRes stage = createBuffer(VkDeviceSize(rowBytes) * image.height, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    if (!stage.mapped) return false;
    std::memcpy(stage.mapped, data, size_t(rowBytes) * image.height);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer c;
    vkAllocateCommandBuffers(device_, &cai, &c);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(c, stage.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(c);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    vkResetFences(device_, 1, &fence_);
    const bool ok = vkQueueSubmit(queue_, 1, &si, fence_) == VK_SUCCESS;
    if (ok) vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(device_, pool_, 1, &c);
    destroy(stage);
    return ok;
}

ImageRes VkContext::createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t depth,
                                bool volume) {
    ImageRes r;
    r.width = w;
    r.height = h;
    r.depth = depth;
    r.format = fmt;
    const bool is3d = volume || depth > 1;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, depth};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &r.image) != VK_SUCCESS) return {};
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device_, r.image, &mr);
    uint32_t mt = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = findMemoryType(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(device_, &ai, nullptr, &r.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, r.image, nullptr);
        return {};
    }
    vkBindImageMemory(device_, r.image, r.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.image;
    vi.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &r.view) != VK_SUCCESS) {
        destroy(r);
        return {};
    }
    toGeneral(r.image);
    return r;
}

void VkContext::destroy(ImageRes& i) {
    if (!device_) return;
    if (i.view) vkDestroyImageView(device_, i.view, nullptr);
    if (i.image) vkDestroyImage(device_, i.image, nullptr);
    if (i.memory) vkFreeMemory(device_, i.memory, nullptr);
    i = {};
}

ImageRes VkContext::wrapImage(VkImage image, VkFormat fmt, uint32_t w, uint32_t h) {
    ImageRes r;
    r.image = image; r.format = fmt; r.width = w; r.height = h;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &r.view) != VK_SUCCESS) return {};
    return r;
}

void VkContext::destroyView(ImageRes& i) {
    if (device_ && i.view) vkDestroyImageView(device_, i.view, nullptr);
    i = {};
}

VkPipeline VkContext::createComputePipeline(const std::vector<uint32_t>& spirv, std::string& error) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = spirv.size() * 4;
    si.pCode = spirv.data();
    VkShaderModule m;
    if (VkResult r = vkCreateShaderModule(device_, &si, nullptr, &m); r != VK_SUCCESS) {
        error = "vkCreateShaderModule " + vkErr(r);
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = pipelineLayout_;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(device_, m, nullptr);
    if (r != VK_SUCCESS) {
        error = "vkCreateComputePipelines " + vkErr(r);
        return VK_NULL_HANDLE;
    }
    return p;
}

void VkContext::destroy(VkPipeline p) {
    if (p && device_) vkDestroyPipeline(device_, p, nullptr);
}

void VkContext::beginExternal(VkCommandBuffer cb, int slot) {
    slot_ = ((slot % kSlots) + kSlots) % kSlots;
    vkResetDescriptorPool(device_, descPools_[slot_], 0);
    uboOffset_ = 0;
    active_ = cb;
}

bool VkContext::beginInteropFrame(int slot, std::string& error) {
    // Qt waits for the previous use of currentFrameSlot before beforeRendering. The
    // producer is submitted before that slot's Qt command buffer on the exact same
    // graphics queue, so the Qt frame fence protects its command buffer and descriptors.
    if (slot < 0 || slot >= kSlots) { error = "Qt frame slot is out of range"; return false; }
    auto& cb = interopCmds_[slot];
    if (!cb) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(device_, &ai, &cb), "Allocate VFG producer command buffer");
    }
    VK_TRY(vkResetCommandBuffer(cb, 0), "Reset VFG producer command buffer");
    VK_TRY(vkResetDescriptorPool(device_, descPools_[slot], 0), "Reset VFG descriptor pool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY(vkBeginCommandBuffer(cb, &bi), "Begin VFG producer command buffer");
    slot_ = slot;
    uboOffset_ = 0;
    active_ = cb;
    return true;
}

bool VkContext::submitInterop(VkSemaphore signal, std::string& error) {
    const VkCommandBuffer producer = interopCmds_[slot_];
    VK_TRY(vkEndCommandBuffer(producer), "End VFG producer command buffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &producer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signal;
    VK_TRY(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "Submit VFG producer command buffer");
    return true;
}

void VkContext::beginFrame(int slot) {
    slot_ = ((slot % kSlots) + kSlots) % kSlots;
    vkResetDescriptorPool(device_, descPools_[slot_], 0);
    uboOffset_ = 0;
    active_ = cmd_;
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
}

void VkContext::computeBarrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags st = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(active_, st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void VkContext::dispatch(VkPipeline pipeline, const ImageRes* const* sampled, uint32_t sampledCount,
                         const ImageRes* const* storage, uint32_t storageCount, const void* uniform,
                         uint32_t uniformSize, uint32_t gx, uint32_t gy) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPools_[slot_];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) return;

    // fixed-size (no heap allocation per dispatch; ~20 dispatches per frame)
    struct Writes {
        VkWriteDescriptorSet a[kSampledCount + kStorageCount + kSamplerCount + 1];
        uint32_t n = 0;
        void push_back(const VkWriteDescriptorSet& x) { a[n++] = x; }
    } w;
    VkDescriptorImageInfo si[kSampledCount], sti[kStorageCount], smp[kSamplerCount];
    for (uint32_t i = 0; i < kSampledCount; ++i) {
        const ImageRes* im = (i < sampledCount && sampled[i]) ? sampled[i] : nullptr;
        // t11 is a Texture3D in ColorGrade.hlsli; every other slot is 2D.
        const ImageRes* use = im ? im : (i == 11 ? &dummySampled3d_ : &dummySampled2d_);
        si[i] = {VK_NULL_HANDLE, use->view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = kBindSampled + i;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        x.pImageInfo = &si[i];
        w.push_back(x);
    }
    for (uint32_t i = 0; i < kStorageCount; ++i) {
        const ImageRes* use = (i < storageCount && storage[i]) ? storage[i] : &dummyStorage_;
        sti[i] = {VK_NULL_HANDLE, use->view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = kBindStorage + i;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        x.pImageInfo = &sti[i];
        w.push_back(x);
    }
    smp[0] = {pointSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    smp[1] = {linearSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = kBindSampler + i;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        x.pImageInfo = &smp[i];
        w.push_back(x);
    }
    // Uniform data goes into a ring (zero-padded to >=256 B so the bound range always covers the cbuffer).
    const VkDeviceSize span = std::max<VkDeviceSize>(256, (uniformSize + 15) & ~15u);
    BufferRes& ring = uboRings_[slot_];
    if (uboOffset_ + span > ring.size) uboOffset_ = 0;
    char* dst = static_cast<char*>(ring.mapped) + uboOffset_;
    std::memset(dst, 0, size_t(span));
    if (uniform && uniformSize) std::memcpy(dst, uniform, uniformSize);
    VkDescriptorBufferInfo bi{ring.buffer, uboOffset_, span};
    uboOffset_ += (span + uboAlign_ - 1) & ~(uboAlign_ - 1);
    VkWriteDescriptorSet ub{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ub.dstSet = set;
    ub.dstBinding = kBindUniform;
    ub.descriptorCount = 1;
    ub.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ub.pBufferInfo = &bi;
    w.push_back(ub);
    vkUpdateDescriptorSets(device_, w.n, w.a, 0, nullptr);

    vkCmdBindPipeline(active_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(active_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(active_, gx, gy, 1);
}

bool VkContext::waitExternal(VkSemaphore semaphore, std::string& error) {
    // Qt owns the next graphics submission. An ordered wait on the same graphics queue covers
    // all subsequent work, including the external->graphics ownership acquire recorded there.
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &semaphore;
    si.pWaitDstStageMask = &stages;
    VK_TRY(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), "Queue CUDA completion wait");
    return true;
}

bool VkContext::submitAndWait(std::string& error, VkSemaphore signal) {
    VK_TRY(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = signal ? &signal : nullptr;
    vkResetFences(device_, 1, &fence_);
    VK_TRY(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
    VK_TRY(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 10'000'000'000ull), "vkWaitForFences");
    return true;
}

} // namespace veyamux
