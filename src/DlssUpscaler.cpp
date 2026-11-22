#include "DlssUpscaler.h"
#include "MotionEstimator.h"

#ifdef VEYAMUX_HAS_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#include <algorithm>
#include <array>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#endif

namespace veyamux {
#ifdef VEYAMUX_HAS_DLSS
namespace {
namespace fs = std::filesystem;
constexpr char projectId[] = "f7616bf9-771f-497e-8484-dfdd7b93dfa1";
constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

struct Paths {
    std::wstring cache, runtime;
    const wchar_t* runtimePtr = nullptr;
    NVSDK_NGX_FeatureCommonInfo info{};

    Paths() {
        std::error_code ec;
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        const char* home = std::getenv("HOME");
        fs::path cacheDir = xdg && *xdg ? fs::path(xdg) : (home ? fs::path(home)/".cache" : fs::path{});
        if (!cacheDir.empty()) {
            cacheDir /= "VeyAmuxNvidiaTest/ngx";
            fs::create_directories(cacheDir, ec);
        }
        const char* overrideDir = std::getenv("VEYAMUX_NGX_DIR");
        const auto exe = fs::read_symlink("/proc/self/exe", ec);
        fs::path runtimeDir = overrideDir && *overrideDir ? fs::path(overrideDir) :
            exe.parent_path().parent_path() / "lib/veyamux/ngx";
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        try {
            cache = conv.from_bytes(cacheDir.string());
            runtime = conv.from_bytes(runtimeDir.string());
        } catch (const std::range_error&) { return; }
        runtimePtr = runtime.c_str();
        info.PathListInfo = {&runtimePtr, 1};
    }

    NVSDK_NGX_FeatureDiscoveryInfo discovery() const {
        NVSDK_NGX_FeatureDiscoveryInfo d{};
        d.SDKVersion = NVSDK_NGX_Version_API;
        d.FeatureID = NVSDK_NGX_Feature_SuperSampling;
        d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        d.Identifier.v.ProjectDesc = {projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.7"};
        d.ApplicationDataPath = cache.c_str();
        d.FeatureInfo = &info;
        return d;
    }
};

const Paths& paths() { static const Paths p; return p; }

bool check(NVSDK_NGX_Result r, const char* task, std::string& error) {
    if (NVSDK_NGX_SUCCEED(r)) return true;
    std::ostringstream msg;
    msg << "DLSS SR: " << task << " failed (NGX 0x" << std::hex << unsigned(r) << ")";
    error = msg.str();
    return false;
}

// NGX retains per-device state. Live capture, preview and export can each own an
// enhancement chain; only the final client may shut down a shared Vulkan device's NGX session.
std::mutex sessionMutex;
std::map<VkDevice, unsigned> users;

bool retainDevice(VkContext& c, std::string& error) {
    std::lock_guard lock(sessionMutex);
    if (auto it = users.find(c.device()); it != users.end()) { ++it->second; return true; }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(c.physical(), &props);
    if (props.vendorID != 0x10de) { error = "DLSS SR requires an NVIDIA RTX Vulkan device"; return false; }
    const Paths& p = paths();
    const auto r = NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.7",
        p.cache.c_str(), c.instance(), c.physical(), c.device(), vkGetInstanceProcAddr, vkGetDeviceProcAddr, &p.info);
    if (!check(r, "initialize the NVIDIA NGX runtime", error)) return false;
    users.emplace(c.device(), 1);
    return true;
}

void releaseDevice(VkDevice device) {
    std::lock_guard lock(sessionMutex);
    auto it = users.find(device);
    if (it != users.end() && --it->second == 0) {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        users.erase(it);
    }
}

NVSDK_NGX_Resource_VK resource(const ImageRes& img, bool output = false) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(img.view, img.image, range, img.format,
                                                   img.width, img.height, output);
}

// NGX sampled descriptors refer to read-only images. Restore GENERAL afterwards so
// the following VeyAmux pass may bind the same input images for storage again.
void inputLayouts(VkCommandBuffer cb, const std::array<VkImage, 3>& inputs, bool before) {
    std::array<VkImageMemoryBarrier, 3> barriers{};
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto& b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = before ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = before ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = before ? VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = before ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = inputs[i]; b.subresourceRange = range;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
}
}

std::vector<std::string> DlssUpscaler::extensions(VkInstance instance, VkPhysicalDevice physical) {
    auto discovery = paths().discovery();
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const auto r = physical ?
        NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &discovery, &count, &properties) :
        NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &properties);
    std::vector<std::string> names;
    if (NVSDK_NGX_SUCCEED(r)) {
        for (uint32_t i = 0; properties && i < count; ++i) names.emplace_back(properties[i].extensionName);
    } else {
        unsigned ni = 0, nd = 0; const char** ie = nullptr; const char** de = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de))) {
            const char** ext = physical ? de : ie;
            for (unsigned i = 0; ext && i < (physical ? nd : ni); ++i) names.emplace_back(ext[i]);
        }
    }
    return names;
}

struct DlssUpscaler::Impl {
    VkContext& ctx;
    MotionEstimator motion;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    ImageRes depth, vectors;
    uint32_t outputW = 0, outputH = 0;
    bool retained = false;

    Impl(VkContext& c, const std::string& dir) : ctx(c), motion(c, dir) {}
    ~Impl() {
        clearFeature();
        if (params) NVSDK_NGX_VULKAN_DestroyParameters(params);
        if (retained) releaseDevice(ctx.device());
    }

    void clearFeature() {
        if (feature || depth.image || vectors.image) vkDeviceWaitIdle(ctx.device());
        if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
        ctx.destroy(depth);
        ctx.destroy(vectors);
    }

    bool create(VkCommandBuffer cb, const ImageRes& in, const ImageRes& out, std::string& error) {
        clearFeature();
        if (!retained) {
            retained = retainDevice(ctx, error);
            if (!retained) return false;
        }
        if (!params) {
            if (!check(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params), "query driver capabilities", error)) return false;
        }
        int available = 0;
        if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
            error = "DLSS SR unavailable on this device or driver; check the NVIDIA NGX release runtime";
            return false;
        }
        VkFormatProperties src{}, dst{};
        vkGetPhysicalDeviceFormatProperties(ctx.physical(), VK_FORMAT_R16G16B16A16_SFLOAT, &src);
        vkGetPhysicalDeviceFormatProperties(ctx.physical(), VK_FORMAT_R16G16_SFLOAT, &dst);
        if (!(src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            error = "DLSS SR: GPU cannot convert the estimated motion to RG16F";
            return false;
        }
        constexpr auto u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        depth = ctx.createImage(in.width, in.height, VK_FORMAT_R32_SFLOAT, u);
        vectors = ctx.createImage(in.width, in.height, VK_FORMAT_R16G16_SFLOAT, u);
        if (!depth.image || !vectors.image) { error = "DLSS SR: motion/depth texture allocation failed"; return false; }

        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = in.width; cp.Feature.InHeight = in.height;
        cp.Feature.InTargetWidth = out.width; cp.Feature.InTargetHeight = out.height;
        cp.Feature.InPerfQualityValue = in.width == out.width ? NVSDK_NGX_PerfQuality_Value_DLAA :
            NVSDK_NGX_PerfQuality_Value_MaxQuality;
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        if (!check(NGX_VULKAN_CREATE_DLSS_EXT1(ctx.device(), cb, 1, 1, &feature, params, &cp),
                   "create Super Resolution feature", error)) return false;
        // Input is already composited PS5 video. The console doesn't export depth.
        VkClearColorValue farPlane{};
        vkCmdClearColorImage(cb, depth.image, VK_IMAGE_LAYOUT_GENERAL, &farPlane, 1, &range);
        outputW = out.width; outputH = out.height;
        return true;
    }
};

DlssUpscaler::DlssUpscaler(VkContext& c, const std::string& dir) : impl_(std::make_unique<Impl>(c, dir)) {}
DlssUpscaler::~DlssUpscaler() = default;

bool DlssUpscaler::record(VkCommandBuffer cb, const ImageRes& in, const ImageRes& out, bool reset,
                          float frameTimeMs, std::string& error) {
    auto& s = *impl_;
    const bool created = !s.feature || s.depth.width != in.width || s.depth.height != in.height ||
                         s.outputW != out.width || s.outputH != out.height;
    if (created && !s.create(cb, in, out, error)) return false;
    reset |= created;
    const ImageRes* mv = s.motion.record(cb, in, reset, error);
    if (!mv) return false;
    s.ctx.computeBarrier();
    VkImageBlit copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffsets[1] = copy.dstOffsets[1] = {int32_t(in.width), int32_t(in.height), 1};
    vkCmdBlitImage(cb, mv->image, VK_IMAGE_LAYOUT_GENERAL, s.vectors.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_NEAREST);
    const std::array<VkImage, 3> images{in.image, s.depth.image, s.vectors.image};
    inputLayouts(cb, images, true);
    auto color = resource(in), depth = resource(s.depth), flow = resource(s.vectors), result = resource(out, true);
    NVSDK_NGX_VK_DLSS_Eval_Params e{};
    e.Feature.pInColor = &color; e.Feature.pInOutput = &result;
    e.pInDepth = &depth; e.pInMotionVectors = &flow;
    e.InRenderSubrectDimensions = {in.width, in.height};
    e.InMVScaleX = e.InMVScaleY = 1.0f; // estimator is previous-minus-current, measured in input pixels
    e.InReset = reset;
    e.InFrameTimeDeltaInMsec = frameTimeMs;
    e.InPreExposure = e.InExposureScale = 1.0f;
    const auto r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, s.feature, s.params, &e);
    inputLayouts(cb, images, false);
    s.ctx.computeBarrier();
    if (created && NVSDK_NGX_SUCCEED(r))
        std::fprintf(stderr, "DLSS SR active: %ux%u -> %ux%u (Vulkan NGX SuperSampling)\n", in.width, in.height, out.width, out.height);
    return check(r, "evaluate frame", error);
}
#else
struct DlssUpscaler::Impl {};
std::vector<std::string> DlssUpscaler::extensions(VkInstance, VkPhysicalDevice) { return {}; }
DlssUpscaler::DlssUpscaler(VkContext&, const std::string&) {}
DlssUpscaler::~DlssUpscaler() = default;
bool DlssUpscaler::record(VkCommandBuffer, const ImageRes&, const ImageRes&, bool, float, std::string& error) {
    error = "Built without the NVIDIA DLSS SR SDK";
    return false;
}
#endif

} // namespace veyamux
