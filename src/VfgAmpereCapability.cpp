// Compatibility for NVIDIA's pinned Linux Video Frame Generation feature only.
// The app's loader selects the private SM86 copy only for a matching GPU. Other
// effects and the system CUDA driver always use the original CUDA function.
#include <dlfcn.h>

namespace {
using GetDeviceAttribute = int (*)(int*, int, int);
GetDeviceAttribute cudaDeviceAttribute() {
    // Keep this handle for the lifetime of the process, including SDK teardown.
    static void* driver = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
    static auto query = driver ? reinterpret_cast<GetDeviceAttribute>(dlsym(driver, "cuDeviceGetAttribute")) : nullptr;
    return query;
}
}

extern "C" __attribute__((visibility("default"))) int vxDeviceGetAttribute(int* value, int attribute, int device) {
    constexpr int computeMajor = 75;
    constexpr int computeMinor = 76;
    auto query = cudaDeviceAttribute();
    if (!query) return 3;  // CUDA_ERROR_NOT_INITIALIZED
    const int status = query(value, attribute, device);
    if (status == 0 && value && attribute == computeMinor && *value == 6) {
        int major = 0;
        if (query(&major, computeMajor, device) == 0 && major == 8) *value = 9;
    }
    return status;
}
