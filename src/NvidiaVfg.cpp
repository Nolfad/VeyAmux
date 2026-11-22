#include "NvidiaVfg.h"
#include <array>
#include <map>
#include <unistd.h>
#include <limits>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>

namespace veyamux {
namespace {
namespace fs = std::filesystem;
using Obj = void*;
using Status = int;
// NvCVImage C ABI. NvCV descriptors are allocated and destroyed by the installed SDK.
constexpr int rgba = 6, u8 = 1;
constexpr unsigned chunky = 0, cpu = 0, gpu = 1;
// Stable CUDA driver ABI on Linux x86-64 (the documented external-memory and semaphore descriptors).
// The union also contains a two-pointer Win32 arm even when passing a POSIX file descriptor.
union ExternalHandle { int fd; void* win32[2]; };
struct MemoryHandleDesc { int type; ExternalHandle handle; uint64_t size; unsigned flags, reserved[16]; };
struct MappedBufferDesc { uint64_t offset, size; unsigned flags, reserved[16]; };
struct SemaphoreHandleDesc { int type; ExternalHandle handle; unsigned flags, reserved[16]; };
// All parameters (including flags and reserved fields) are zero for a binary opaque-FD semaphore.
struct alignas(8) BinarySemaphoreParams { unsigned words[36]; };
static_assert(sizeof(MemoryHandleDesc) == 104 && sizeof(MappedBufferDesc) == 88 &&
              sizeof(SemaphoreHandleDesc) == 96 && sizeof(BinarySemaphoreParams) == 144);

struct Api {
    Status (*cuInit)(unsigned);
    Status (*cuDeviceGetCount)(int*);
    Status (*cuDeviceGet)(int*, int);
    Status (*cuDeviceGetUuid)(void*, int);
    Status (*cuDeviceGetAttribute)(int*, int, int);
    Status (*cuDevicePrimaryCtxRetain)(Obj*, int);
    Status (*cuDevicePrimaryCtxRelease)(int);
    Status (*cuCtxPushCurrent)(Obj);
    Status (*cuCtxPopCurrent)(Obj*);
    Status (*cuStreamCreate)(Obj*, unsigned);
    Status (*cuStreamSynchronize)(Obj);
    Status (*cuStreamDestroy)(Obj);
    Status (*cuImportExternalMemory)(Obj*, const MemoryHandleDesc*);
    Status (*cuExternalMemoryGetMappedBuffer)(uint64_t*, Obj, const MappedBufferDesc*);
    Status (*cuMemFree)(uint64_t);
    Status (*cuDestroyExternalMemory)(Obj);
    Status (*cuImportExternalSemaphore)(Obj*, const SemaphoreHandleDesc*);
    Status (*cuDestroyExternalSemaphore)(Obj);
    Status (*cuWaitExternalSemaphoresAsync)(const Obj*, const BinarySemaphoreParams*, unsigned, Obj);
    Status (*cuSignalExternalSemaphoresAsync)(const Obj*, const BinarySemaphoreParams*, unsigned, Obj);
    Status (*cuMemGetInfo)(size_t*, size_t*);
    Status (*cuEventCreate)(Obj*, unsigned);
    Status (*cuEventRecord)(Obj, Obj);
    Status (*cuEventQuery)(Obj);
    Status (*cuEventElapsedTime)(float*, Obj, Obj);
    Status (*cuEventDestroy)(Obj);
    Status (*cuGetErrorString)(int, const char**);
    Status (*NvVFX_CreateEffect)(const char*, Obj*);
    void (*NvVFX_DestroyEffect)(Obj);
    Status (*NvVFX_SetU32)(Obj, const char*, unsigned);
    Status (*NvVFX_SetCudaStream)(Obj, const char*, Obj);
    Status (*NvVFX_SetImage)(Obj, const char*, Obj);
    Status (*NvVFX_Load)(Obj);
    Status (*NvVFX_Run)(Obj, int);
    Status (*NvCVImage_Create)(unsigned,unsigned,int,int,unsigned,unsigned,unsigned,Obj*);
    void (*NvCVImage_Dealloc)(Obj);
    void (*NvCVImage_Destroy)(Obj);
    Status (*NvCVImage_Init)(Obj,unsigned,unsigned,int,void*,int,int,unsigned,unsigned);
    const char* (*NvCV_GetErrorStringFromCode)(Status);
};

// VFX owns process-wide worker threads. Keep loaded modules resident for the process lifetime.
struct Libraries {
    Api a{};
    int device = -1;
    std::string error;

    void* open(const fs::path& name) {
        static std::mutex lock;
        static std::map<fs::path, void*> loaded;
        std::lock_guard guard(lock);
        if (auto it = loaded.find(name); it != loaded.end()) return it->second;
        void* h = dlopen(name.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h) { const char* detail = dlerror(); error = detail ? detail : name.string(); }
        else loaded.emplace(name, h);
        return h;
    }
    template<class F> bool bind(void* h, F& f, const char* name) {
        f = reinterpret_cast<F>(dlsym(h, name));
        if (!f) error = std::string("Missing runtime function ") + name;
        return f != nullptr;
    }
    explicit Libraries(VkPhysicalDevice physical) {
        void* cuda = open("libcuda.so.1");
        if (!cuda) return;
#define CUDA_BIND(fn, symbol) if (!bind(cuda, a.fn, symbol)) return
        CUDA_BIND(cuInit, "cuInit");
        CUDA_BIND(cuDeviceGetCount, "cuDeviceGetCount");
        CUDA_BIND(cuDeviceGet, "cuDeviceGet");
        CUDA_BIND(cuDeviceGetUuid, "cuDeviceGetUuid");
        CUDA_BIND(cuDeviceGetAttribute, "cuDeviceGetAttribute");
        CUDA_BIND(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
        CUDA_BIND(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2");
        CUDA_BIND(cuCtxPushCurrent, "cuCtxPushCurrent_v2");
        CUDA_BIND(cuCtxPopCurrent, "cuCtxPopCurrent_v2");
        CUDA_BIND(cuStreamCreate, "cuStreamCreate");
        CUDA_BIND(cuStreamSynchronize, "cuStreamSynchronize");
        CUDA_BIND(cuStreamDestroy, "cuStreamDestroy_v2");
        CUDA_BIND(cuImportExternalMemory, "cuImportExternalMemory");
        CUDA_BIND(cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer");
        CUDA_BIND(cuMemFree, "cuMemFree_v2");
        CUDA_BIND(cuDestroyExternalMemory, "cuDestroyExternalMemory");
        CUDA_BIND(cuImportExternalSemaphore, "cuImportExternalSemaphore");
        CUDA_BIND(cuDestroyExternalSemaphore, "cuDestroyExternalSemaphore");
        CUDA_BIND(cuWaitExternalSemaphoresAsync, "cuWaitExternalSemaphoresAsync");
        CUDA_BIND(cuSignalExternalSemaphoresAsync, "cuSignalExternalSemaphoresAsync");
        CUDA_BIND(cuMemGetInfo, "cuMemGetInfo_v2");
        CUDA_BIND(cuEventCreate, "cuEventCreate");
        CUDA_BIND(cuEventRecord, "cuEventRecord");
        CUDA_BIND(cuEventQuery, "cuEventQuery");
        CUDA_BIND(cuEventElapsedTime, "cuEventElapsedTime");
        CUDA_BIND(cuEventDestroy, "cuEventDestroy_v2");
        CUDA_BIND(cuGetErrorString, "cuGetErrorString");
#undef CUDA_BIND
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vkGetPhysicalDeviceProperties2(physical, &props);
        int count = 0;
        const int initStatus = a.cuInit(0);
        if (initStatus || a.cuDeviceGetCount(&count) != 0) {
            error = "CUDA initialization or device enumeration failed (cuInit=" + std::to_string(initStatus) + ")";
            return;
        }
        for (int i = 0; i < count; ++i) {
            int d; std::array<unsigned char, VK_UUID_SIZE> uuid{};
            if (a.cuDeviceGet(&d, i) == 0 && a.cuDeviceGetUuid(uuid.data(), d) == 0 &&
                std::memcmp(uuid.data(), id.deviceUUID, uuid.size()) == 0) { device = d; break; }
        }
        if (device < 0) { error = "No CUDA device matches the selected Vulkan GPU"; return; }
        int major = 0, minor = 0;
        a.cuDeviceGetAttribute(&major, 75, device);  // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR/MINOR
        a.cuDeviceGetAttribute(&minor, 76, device);

        fs::path dir;
        std::error_code ec;
        if (const char* configured = std::getenv("VEYAMUX_VFX_DIR"); configured && *configured) dir = configured;
        else {
            fs::path data;
            if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) data = xdg;
            else if (const char* home = std::getenv("HOME"); home && *home) data = fs::path(home) / ".local/share";
            fs::path installed = data / "veyamux-nvidia/vfx-0.2.0.0";
            if (!data.empty() && fs::exists(installed / "libVideoFX.so", ec)) {
                dir = installed;
                if (major == 8 && minor == 6 && fs::exists(dir / "ampere/libnvVFXVideoFrameGeneration.so", ec))
                    dir /= "ampere";
            } else if (fs::exists("/usr/local/VideoFX/lib/libVideoFX.so", ec)) dir = "/usr/local/VideoFX/lib";
        }
        // The wheel's core libraries expect the other SDK DSOs under their original SONAMEs.
        // Load dependencies before the proxy, using absolute paths, also when AppImage's loader path is isolated.
        if (!dir.empty()) {
            for (const char* prefix : {"libcudart", "libnppc.so", "libnppial.so", "libnppicc.so", "libnppidei.so",
                                      "libnppif.so", "libnppig.so", "libnppim.so", "libnppist.so", "libnppitc.so", "libcudnn.so"}) {
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    if (e.path().filename().string().rfind(prefix, 0) == 0) {
                        if (!open(e.path())) return;
                        break;
                    }
                }
            }
        }
        auto sdk = [&](const char* name) { return open(dir.empty() ? fs::path(name) : dir / name); };
        void* image = sdk("libNVCVImage.so");
        if (!image) return;
        // Installed Linux SDKs can package NGX inside their feature tree. The Python wheel carries it alongside core.
        if (!dir.empty() && fs::exists(dir / "libnvngxruntime.so", ec) && !sdk("libnvngxruntime.so")) return;
        if (!sdk("libVideoFXLocal.so")) return;
        void* vfx = sdk("libVideoFX.so");
        if (!vfx) return;
        const char* fx = "libnvVFXVideoFrameGeneration.so";
        fs::path feature = dir / fx;
        fs::path ngc = dir.parent_path() / "features/nvvfxvideoframegeneration/lib" / fx;
        if (!dir.empty() && !fs::exists(feature, ec) && fs::exists(ngc, ec)) feature = ngc;
        if (!open(feature)) return;
#define BIND(lib, fn) if (!bind(lib, a.fn, #fn)) return
        BIND(vfx, NvVFX_CreateEffect);
        BIND(vfx, NvVFX_DestroyEffect);
        BIND(vfx, NvVFX_SetU32);
        BIND(vfx, NvVFX_SetCudaStream);
        BIND(vfx, NvVFX_SetImage);
        BIND(vfx, NvVFX_Load);
        BIND(vfx, NvVFX_Run);
        BIND(image, NvCVImage_Create);
        BIND(image, NvCVImage_Dealloc);
        BIND(image, NvCVImage_Destroy);
        BIND(image, NvCVImage_Init);
        BIND(image, NvCV_GetErrorStringFromCode);
#undef BIND
    }
};

struct Current {
    Api& api;
    bool pushed;
    Current(Api& a, Obj ctx) : api(a), pushed(a.cuCtxPushCurrent(ctx) == 0) {}
    ~Current() { if (pushed) { Obj prior; api.cuCtxPopCurrent(&prior); } }
};
VkBufferImageCopy region(const ImageRes& img) {
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {img.width, img.height, 1};
    return c;
}
}

struct NvidiaVfg::Session {
    VkContext& vk;
    Libraries libraries;
    Api& a = libraries.a;
    int device = -1;
    Obj context{}, stream{}, primary{}, fast{}, input[2]{}, output{};
    Stats stats;
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastPrimary{};
    // CUDA events bracket the inference on the same stream, after the incoming picture is
    // GPU-ready and before signalling Vulkan. Poll old samples without stalling playback.
    struct Sample { Obj start{}, end{}; bool fast{}; };
    std::array<Sample,8> samples{};
    uint64_t timerRead = 0, timerWrite = 0;
    bool timing = false;
    struct Semaphore { VkSemaphore vk{}; Obj cuda{}; };
    Semaphore ready{}, done{};
    BufferRes buffer{};
    Obj cudaMemory{};
    uint64_t pixels = 0;
    VkDeviceSize pitch = 0, slice = 0;
    int previous = -1;
    bool acquireNeeded = false;
    bool awaitingCuda = false;
    std::string& error;

    Session(VkContext& ctx, std::string& e) : vk(ctx), libraries(ctx.physical()), error(e) {}
    ~Session() {
        if (context) {
            Current current(a, context);
            if (current.pushed) {
                if (stream) a.cuStreamSynchronize(stream);
                // The final wait and display copy may belong to the Qt graphics submission.
                if (buffer.buffer) vkDeviceWaitIdle(vk.device());
                for (Obj fx : {fast, primary}) if (fx) a.NvVFX_DestroyEffect(fx);
                for (const auto& sample : samples) {
                    if (sample.start) a.cuEventDestroy(sample.start);
                    if (sample.end) a.cuEventDestroy(sample.end);
                }
                for (Obj image : {input[0], input[1], output}) if (image) a.NvCVImage_Destroy(image);
                if (pixels) a.cuMemFree(pixels);
                if (cudaMemory) a.cuDestroyExternalMemory(cudaMemory);
                for (auto semaphore : {done.cuda, ready.cuda}) if (semaphore) a.cuDestroyExternalSemaphore(semaphore);
                if (stream) a.cuStreamDestroy(stream);
            }
        }
        for (auto semaphore : {done.vk, ready.vk}) if (semaphore) vkDestroySemaphore(vk.device(), semaphore, nullptr);
        vk.destroy(buffer);
        if (context) a.cuDevicePrimaryCtxRelease(device);
    }
    bool cuda(const char* op, int r) {
        if (!r) return true;
        const char* what = nullptr;
        a.cuGetErrorString(r, &what);
        error = std::string(op) + ": " + (what ? what : std::to_string(r));
        return false;
    }
    bool vfx(const char* op, int r) {
        if (!r) return true;
        const char* what = a.NvCV_GetErrorStringFromCode(r);
        error = std::string(op) + " (" + std::to_string(r) + "): " + (what ? what : "Video Effects SDK error");
        return false;
    }
    bool exportSemaphore(Semaphore& semaphore) {
        const auto getFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(vk.device(), "vkGetSemaphoreFdKHR"));
        if (!getFd) { error = "Vulkan driver lacks opaque-FD semaphore export"; return false; }
        VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &external;
        if (vkCreateSemaphore(vk.device(), &ci, nullptr, &semaphore.vk) != VK_SUCCESS) {
            error = "Could not create a Vulkan/CUDA interop semaphore"; return false;
        }
        VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        info.semaphore = semaphore.vk;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        SemaphoreHandleDesc desc{};
        desc.type = 1;  // CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD
        if (getFd(vk.device(), &info, &desc.handle.fd) != VK_SUCCESS) {
            error = "Could not export Vulkan/CUDA semaphore"; return false;
        }
        if (cuda("Import GPU semaphore", a.cuImportExternalSemaphore(&semaphore.cuda, &desc))) return true;
        close(desc.handle.fd);  // CUDA takes ownership only on successful import.
        return false;
    }
    bool initExchange(uint32_t w, uint32_t h) {
        // Three disjoint images in one dedicated, device-local allocation. CUDA receives
        // their GPU addresses once; at runtime the SDK directly reads/writes those ranges.
        pitch = (VkDeviceSize(w) * 4 + 255) & ~VkDeviceSize(255);
        if (!w || !h || pitch > std::numeric_limits<int>::max() ||
            h > std::numeric_limits<VkDeviceSize>::max() / (3 * pitch)) {
            error = "Unsupported VFG image dimensions"; return false;
        }
        slice = pitch * h;
        int fd = -1;
        VkDeviceSize allocationSize = 0;
        if (!vk.createCudaExchange(3 * slice, buffer, allocationSize, fd, error)) return false;
        MemoryHandleDesc memory{};
        memory.type = 1;  // CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD
        memory.handle.fd = fd;
        memory.size = allocationSize;
        memory.flags = 1;  // CUDA_EXTERNAL_MEMORY_DEDICATED, paired with VkMemoryDedicatedAllocateInfo
        if (!cuda("Import VFG GPU memory", a.cuImportExternalMemory(&cudaMemory, &memory))) {
            close(fd); return false;
        }
        MappedBufferDesc mapping{};
        mapping.size = 3 * slice;
        if (!cuda("Map VFG GPU buffer", a.cuExternalMemoryGetMappedBuffer(&pixels, cudaMemory, &mapping))) return false;
        Obj* images[] = {&input[0], &input[1], &output};
        for (unsigned i = 0; i < 3; ++i) {
            // Only the SDK's tiny descriptor is allocated here. Init wraps the external
            // device pointer without transferring it or adopting its allocation.
            if (!vfx("Allocate image descriptor", a.NvCVImage_Create(1, 1, rgba, u8, chunky, cpu, 1, images[i]))) return false;
            a.NvCVImage_Dealloc(*images[i]);
            if (!vfx("Wrap shared CUDA image", a.NvCVImage_Init(*images[i], w, h, int(pitch),
                    reinterpret_cast<void*>(pixels + i * slice), rgba, u8, chunky, gpu))) return false;
        }
        return exportSemaphore(ready) && exportSemaphore(done);
    }
    bool loadModel(Obj& effect, uint32_t w, uint32_t h, int quality) {
        return vfx("VideoFrameGeneration", a.NvVFX_CreateEffect("VideoFrameGeneration", &effect)) &&
               vfx("CudaStream", a.NvVFX_SetCudaStream(effect, "CudaStream", stream)) &&
               vfx("InputWidth", a.NvVFX_SetU32(effect, "InputWidth", w)) &&
               vfx("InputHeight", a.NvVFX_SetU32(effect, "InputHeight", h)) &&
               vfx("Mode", a.NvVFX_SetU32(effect, "Mode", unsigned(quality))) &&
               vfx("SrcImage0", a.NvVFX_SetImage(effect, "SrcImage0", input[0])) &&
               vfx("SrcImage1", a.NvVFX_SetImage(effect, "SrcImage1", input[1])) &&
               vfx("NvVFX_Load", a.NvVFX_Load(effect)) &&
               vfx("DstImage0", a.NvVFX_SetImage(effect, "DstImage0", output)) &&
               vfx("FrameMultiplier", a.NvVFX_SetU32(effect, "FrameMultiplier", 2));
    }

    bool initTimers() {
        for (auto& sample : samples)
            if (a.cuEventCreate(&sample.start, 0) || a.cuEventCreate(&sample.end, 0)) return false;
        return true;
    }

    bool init(uint32_t w, uint32_t h, int quality, bool regulate) {
        if (!libraries.error.empty()) { error = libraries.error; return false; }
        device = libraries.device;
        if (!cuda("cuDevicePrimaryCtxRetain", a.cuDevicePrimaryCtxRetain(&context, device))) return false;
        Current current(a, context);
        if (!current.pushed) { error = "Cannot activate the CUDA context"; return false; }
        if (!cuda("cuStreamCreate", a.cuStreamCreate(&stream, 1)) || !initExchange(w, h) ||
            !loadModel(primary, w, h, quality)) return false;
        stats.quality = stats.activeQuality = quality;
        timing = initTimers();
        if (!regulate) return true;
        if (quality == 0) { stats.fastStatus = "Low is already the fastest model."; return true; }
        if (!timing) { stats.fastStatus = "GPU model timing is unavailable."; return true; }

        // Veyra's live 2x guard: after loading the selected tier, require more than
        // 4300 MiB of headroom. Do not reserve that amount. cuMemGetInfo describes the
        // same physical device/context matched to Vulkan, including existing CUDA use.
        size_t available = 0, total = 0;
        if (a.cuMemGetInfo(&available, &total) != 0) {
            stats.fastStatus = "Could not query available VRAM."; return true;
        }
        constexpr size_t MiB = size_t(1024) * 1024;
        if (available <= 4300 * MiB) {
            stats.fastStatus = "The faster model requires more than 4300 MiB of free VRAM.";
            return true;
        }
        if (!loadModel(fast, w, h, quality - 1)) {
            stats.fastStatus = "Faster model unavailable: " + error;
            error.clear();
            if (fast) { a.NvVFX_DestroyEffect(fast); fast = nullptr; }
            return true;
        }
        stats.fastReady = true;
        return true;
    }

    void collectTimes() {
        while (timing && timerRead != timerWrite) {
            const auto& sample = samples[timerRead % samples.size()];
            const int status = a.cuEventQuery(sample.end);
            if (status == 600) return;  // CUDA_ERROR_NOT_READY
            float ms = 0;
            if (status || a.cuEventElapsedTime(&ms, sample.start, sample.end)) {
                timing = false;
                if (stats.fastReady) stats.fastStatus = "GPU model timing is unavailable.";
                stats.fastReady = false;
                return;
            }
            ++timerRead;
            if (ms > 0 && std::isfinite(ms)) {
                // Veyra's low-envelope estimator. Accept lower samples immediately;
                // raise the estimate gradually when a cold run or transient stalls.
                double& cost = sample.fast ? stats.fastMs : stats.primaryMs;
                cost = cost > 0 && ms >= cost ? cost + .1 * (ms - cost) : ms;
            }
        }
    }

    bool useFaster(double budget, Clock::time_point now) const {
        if (!stats.fastReady || !std::isfinite(budget) || stats.primaryMs <= 0) return false;
        const bool retry = now - lastPrimary >= std::chrono::seconds(4);
        const double primaryCost = stats.primaryMs * (retry ? .7 : 1.);
        // budget is the remaining window at handoff; the caller has subtracted upstream
        // backlog. Keep 1 ms clear of the presentation edge, as in Veyra. Prefer the
        // chosen tier whenever it fits, or if timing has shown no gain from the next tier.
        return primaryCost + 1. > budget &&
               (stats.fastMs <= 0 || stats.fastMs < primaryCost);
    }

    bool evaluate(Obj effect, int cur) {
        // Rebind both images for every pair, even when alternating models makes
        // the pointer parity repeat. Their pixels have changed since the last run.
        return vfx("SrcImage0", a.NvVFX_SetImage(effect, "SrcImage0", input[previous])) &&
               vfx("SrcImage1", a.NvVFX_SetImage(effect, "SrcImage1", input[cur])) &&
               vfx("ShotChange", a.NvVFX_SetU32(effect, "ShotChange", 0)) &&
               vfx("FrameIndex", a.NvVFX_SetU32(effect, "FrameIndex", 1)) &&
               vfx("NvVFX_Run", a.NvVFX_Run(effect, 1));
    }

    bool runPair(int cur, double budget) {
        collectTimes();
        const auto now = Clock::now();
        bool quicker = useFaster(budget, now);
        Sample* sample = timing && timerWrite - timerRead < samples.size()
            ? &samples[timerWrite % samples.size()] : nullptr;
        bool timed = sample && !a.cuEventRecord(sample->start, stream);
        if (!evaluate(quicker ? fast : primary, cur)) {
            if (!quicker) return false;
            // If the optional effect rejects a pair, retain the selected model for
            // this and future pairs. Leave the optional handle alive until stream drain.
            stats.fastReady = false;
            stats.fastStatus = "Faster model unavailable: " + error;
            error.clear();
            quicker = false;
            timed = false;
            if (!evaluate(primary, cur)) return false;
        }
        if (timed && !a.cuEventRecord(sample->end, stream)) {
            sample->fast = quicker;
            ++timerWrite;
        }
        stats.activeQuality = stats.quality - int(quicker);
        if (quicker) ++stats.fastPairs;
        else { ++stats.primaryPairs; lastPrimary = now; }
        return true;
    }

    int next() const { return previous == 0 ? 1 : 0; }
    void ownership(VkCommandBuffer cb, bool acquire) {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.buffer = buffer.buffer;
        b.size = VK_WHOLE_SIZE;
        b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : vk.queueFamily();
        b.dstQueueFamilyIndex = acquire ? vk.queueFamily() : VK_QUEUE_FAMILY_EXTERNAL;
        constexpr VkAccessFlags transfer = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcAccessMask = acquire ? 0 : transfer;
        b.dstAccessMask = acquire ? transfer : 0;
        vkCmdPipelineBarrier(cb, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             acquire ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 1, &b, 0, nullptr);
    }
    void acquire(VkCommandBuffer cb) {
        if (acquireNeeded) { ownership(cb, true); acquireNeeded = false; }
    }
    VkBufferImageCopy copy(const ImageRes& img, int slot) const {
        auto r = region(img);
        r.bufferRowLength = uint32_t(pitch / 4);
        r.bufferOffset = slot * slice;
        return r;
    }
    bool run(bool reset, bool& generated, double budget) {
        Current current(a, context);
        if (!current.pushed) { error = "Cannot activate the CUDA context"; return false; }
        const BinarySemaphoreParams binary{};
        if (!cuda("Wait for Vulkan picture", a.cuWaitExternalSemaphoresAsync(&ready.cuda, &binary, 1, stream))) return false;
        const int cur = next();
        const bool pair = !reset && previous >= 0;
        if (pair && !runPair(cur, budget)) return false;
        if (!cuda("Signal CUDA completion", a.cuSignalExternalSemaphoresAsync(&done.cuda, &binary, 1, stream))) return false;
        if (!vk.waitExternal(done.vk, error)) return false;
        previous = cur;
        generated = pair;
        acquireNeeded = true;
        awaitingCuda = false;
        return true;
    }
};

NvidiaVfg::NvidiaVfg(VkContext& ctx) : ctx_(ctx) {}
NvidiaVfg::~NvidiaVfg() = default;

bool NvidiaVfg::usesDeviceExchange() const { return session_ && session_->pixels != 0; }

NvidiaVfg::Stats NvidiaVfg::stats() const {
    if (session_) return session_->stats;
    Stats unavailable;
    unavailable.quality = unavailable.activeQuality = quality_;
    return unavailable;
}

bool NvidiaVfg::prepare(uint32_t w, uint32_t h, int quality, bool regulate) {
    quality = std::clamp(quality, 0, 2);
    if (width_ == w && height_ == h && quality_ == quality && regulate_ == regulate) {
        if (session_ && session_->awaitingCuda) {
            // The display could not allocate its image after submission, so nobody consumed ready.
            // Reuse a freshly initialized pair of semaphores and reseed the history.
            session_.reset();
        } else return session_ && error_.empty();
    }
    quality_ = quality; regulate_ = regulate;
    width_ = w; height_ = h;
    session_.reset();
    error_.clear();
    session_ = std::make_unique<Session>(ctx_, error_);
    if (session_->init(w, h, quality, regulate)) return true;
    session_.reset();
    return false;
}
void NvidiaVfg::capture(VkCommandBuffer cb, const ImageRes& src) {
    auto& s = *session_;
    s.acquire(cb);  // reset/first-frame priming can skip upload()
    ctx_.computeBarrier();
    auto r = s.copy(src, s.next());
    vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, s.buffer.buffer, 1, &r);
    s.ownership(cb, false);
}
bool NvidiaVfg::submitCapture(std::string& error) {
    const bool ok = ctx_.submitAndWait(error, session_->ready.vk);
    session_->awaitingCuda = true;
    return ok;
}
bool NvidiaVfg::submitLiveCapture(std::string& error) {
    const bool ok = ctx_.submitInterop(session_->ready.vk, error);
    session_->awaitingCuda = true;
    return ok;
}
bool NvidiaVfg::interpolate(bool reset, bool& generated, double budget) {
    generated = false;
    if (session_ && session_->awaitingCuda && session_->run(reset, generated, budget)) return true;
    session_.reset();
    return false;
}
void NvidiaVfg::upload(VkCommandBuffer cb, const ImageRes& dst) {
    auto& s = *session_;
    s.acquire(cb);
    auto r = s.copy(dst, 2);
    vkCmdCopyBufferToImage(cb, s.buffer.buffer, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    ctx_.computeBarrier();
}
}
