// Image validation and isolated GPU processing benchmark for the live NVIDIA VFG path.
#include "NvidiaVfg.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace veyamux;
namespace {
constexpr int width = 1280, height = 720;
std::vector<uint8_t> makeFrame(int offset, int w = width, int h = height) {
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (int row = 0; row < h; ++row) {
        const int y = row * height / h;
        for (int col = 0; col < w; ++col) {
            const int x = col * width / w;
            bool block = x >= 360 + offset && x < 800 + offset && y >= 180 && y < 540;
            int pattern = (((x - (block ? offset : 0)) / 12 + y / 12) & 1);
            size_t i = (size_t(row) * w + col) * 4;
            out[i] = block ? 150 + 70 * pattern : 23 + 4 * pattern;
            out[i+1] = block ? 45 + 35 * pattern : 27 + 4 * pattern;
            out[i+2] = block ? 45 : 70;
            out[i+3] = 255;
        }
    }
    return out;
}
int test(VkContext& vk, const ImageRes& img, const BufferRes& readback) {
    NvidiaVfg vfg(vk);
    if (!vfg.prepare(width, height)) { std::fprintf(stderr, "VFG startup failed: %s\n", vfg.error().c_str()); return 2; }
    if (!vfg.usesDeviceExchange()) {
        std::fprintf(stderr, "VFG test requires device-local Vulkan/CUDA exchange\n");
        return 2;
    }
    auto first = makeFrame(0), second = makeFrame(12);
    std::string err;
    for (const auto* pixels : {&first, &second}) {
        if (!vk.uploadImage(img, pixels->data(), width * 4)) { std::fprintf(stderr, "Vulkan input upload failed\n"); return 3; }
        vk.beginFrame();
        vfg.capture(vk.active(), img);
        if (!vfg.submitCapture(err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 3; }
        bool generated = false;
        if (!vfg.interpolate(pixels == &first, generated) || generated != (pixels == &second)) {
            std::fprintf(stderr, "VFG interpolation failed: %s\n", vfg.error().c_str());
            return 4;
        }
    }
    vk.beginFrame();
    vfg.upload(vk.active(), img);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(vk.active(), img.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &c);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk.active(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host, 0, nullptr, 0, nullptr);
    if (!vk.submitAndWait(err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 3; }
    const auto* out = static_cast<const uint8_t*>(readback.mapped);
    size_t changedPixels = 0, backgroundPixels = 0, objectPixels = 0, redPixels = 0;
    double errorFirst = 0, errorSecond = 0, inputSeparation = 0, backgroundError = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t i = (size_t(y) * width + x) * 4;
            if (std::memcmp(first.data()+i, second.data()+i, 3) != 0) {
                ++changedPixels;
                for (int c = 0; c < 3; ++c) {
                    errorFirst += std::abs(int(out[i+c]) - first[i+c]);
                    errorSecond += std::abs(int(out[i+c]) - second[i+c]);
                    inputSeparation += std::abs(int(first[i+c]) - second[i+c]);
                }
            } else if (x < 320 || x > 850 || y < 140 || y > 590) {
                ++backgroundPixels;
                for (int c = 0; c < 3; ++c) backgroundError += std::abs(int(out[i+c]) - first[i+c]);
            }
            if (x >= 400 && x < 770 && y >= 210 && y < 510) {
                ++objectPixels;
                redPixels += out[i] > 105 && out[i] > out[i+1] + 40 && out[i] > out[i+2] + 40;
            }
        }
    }
    const double firstRatio = errorFirst / inputSeparation, secondRatio = errorSecond / inputSeparation;
    const double backgroundMae = backgroundError / (3.0 * backgroundPixels);
    const double redFraction = double(redPixels) / objectPixels;
    std::printf("Moving pixels: %zu, difference from both inputs: %.3f / %.3f, background MAE: %.2f, red object retained: %.1f%%\n",
                changedPixels, firstRatio, secondRatio, backgroundMae, 100.0 * redFraction);
    if (backgroundMae > 10 || redFraction < 0.93 || firstRatio < 0.05 || secondRatio < 0.05 ||
        firstRatio > 1.35 || secondRatio > 1.35) {
        std::fprintf(stderr, "Output did not preserve the moving test scene or copied an endpoint. Interpolation unconfirmed.\n");
        return 5;
    }
    std::puts("NVIDIA Video Frame Generation produced a distinct intermediate frame.");
    return 0;
}

// Keep the pattern resident on the device before starting the clock. Measure
// the complete VFG exchange, including both Vulkan submissions and CUDA, at
// the user's upscaled resolution. The final submit also completes the write.
int benchmark(VkContext& vk) {
    constexpr uint32_t w = 2560, h = 1440;
    constexpr int warmup = 16, samples = 120;
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    NvidiaVfg vfg(vk);
    if (!vfg.prepare(w, h)) {
        std::fprintf(stderr, "NVIDIA VFG startup failed: %s\n", vfg.error().c_str());
        return 2;
    }
    ImageRes inputs[2]{}, output = vk.createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, usage);
    bool ok = output.image != VK_NULL_HANDLE;
    std::string error;
    for (int i = 0; ok && i < 2; ++i) {
        inputs[i] = vk.createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, usage);
        auto pixels = makeFrame(i * 12, w, h);
        ok = inputs[i].image && vk.uploadImage(inputs[i], pixels.data(), w * 4);
    }
    std::vector<double> times;
    unsigned generatedCount = 0;
    for (int i = 0; ok && i < warmup + samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        vk.beginFrame();
        vfg.capture(vk.active(), inputs[i % 2]);
        ok = vfg.submitCapture(error);
        bool generated = false;
        if (ok && !vfg.interpolate(i == 0, generated)) {
            ok = false;
            error = vfg.error();
        }
        if (ok && generated != (i > 0)) {
            ok = false;
            error = "Unexpected number of generated frames";
        }
        if (!ok || !generated) continue;
        vk.beginFrame();
        vfg.upload(vk.active(), output);
        ok = vk.submitAndWait(error);
        if (ok && i >= warmup) {
            ++generatedCount;
            times.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
        }
    }
    for (auto& input : inputs) vk.destroy(input);
    vk.destroy(output);
    if (!ok) {
        std::fprintf(stderr, "NVIDIA VFG benchmark failed: %s\n",
                     error.empty() ? "Vulkan test image allocation or upload failed" : error.c_str());
        return 4;
    }
    std::sort(times.begin(), times.end());
    double total = 0;
    for (double ms : times) total += ms;
    const size_t p95 = size_t(std::ceil(0.95 * times.size())) - 1;
    std::printf("2560x1440 VFG processing only: %u/%d intermediate frames, mean %.2f ms, p95 %.2f ms per input pair.\n",
                generatedCount, samples, total / times.size(), times[p95]);
    std::puts("Capture, upscaling, frame pacing and monitor presentation are excluded.");
    return 0;
}
}
int main(int argc, char** argv) {
    VkContext vk; std::string err;
    if (!vk.init(err)) { std::fprintf(stderr, "Vulkan startup failed: %s\n", err.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", vk.deviceName().c_str());
    if (argc == 2 && std::strcmp(argv[1], "--bench-vfg") == 0) return benchmark(vk);
    auto img = vk.createImage(width,height,VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto buffer = vk.createBuffer(width*height*4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    int result = 3;
    if (img.image && buffer.mapped) result = test(vk,img,buffer);
    else std::fprintf(stderr, "Unable to allocate Vulkan test resources\n");
    vk.destroy(buffer); vk.destroy(img);
    return result;
}
