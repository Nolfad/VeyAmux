// Upscaling check: veyamux-upscale-test <shaderDir> <in.rgba> <w> <h> <frames> <out.rgba> <sgsr|fsr|fsr1|nis|dlss|crt|crt480|crt0|pp|ppWxH> [scale] [motionSign]
// Runs the full enhancement chain (EnhanceChain::process) frame by frame and writes every upscaled frame.
#include "EnhanceChain.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace veyamux;
int main(int argc, char** argv) {
    if (argc < 8) { std::fprintf(stderr, "usage: %s <shaderDir> <in.rgba> <w> <h> <frames> <out.rgba> <sgsr|fsr|fsr1|nis|dlss|crt|crt480|crt0|pp|ppWxH> [scale] [sign]\n", argv[0]); return 2; }
    const int W = atoi(argv[3]), H = atoi(argv[4]), N = atoi(argv[5]);
    const bool fsr = std::strcmp(argv[7], "fsr") == 0;
    if (argc > 9) setenv("VEYAMUX_FSR_UP_SIGN", argv[9], 1);   // read when the upscaler is created
    VkContext ctx; std::string err;
    if (!ctx.init(err)) { std::fprintf(stderr, "vk: %s\n", err.c_str()); return 1; }
    std::printf("GPU: %s\n", ctx.deviceName().c_str());
    EnhanceChain chain(ctx, argv[1]);
    if (!chain.init(err)) { std::fprintf(stderr, "init: %s\n", err.c_str()); return 1; }
    EnhanceSettings s;
    s.scale = argc > 8 ? float(atof(argv[8])) : 2.0f;
    s.upscaler = fsr ? Upscaler::AmdFsr3 : (std::strcmp(argv[7], "fsr1") == 0 ? Upscaler::AmdFsr1 : std::strcmp(argv[7], "nis") == 0 ? Upscaler::Nis : Upscaler::Sgsr);
    if (std::strcmp(argv[7], "dlss") == 0) s.upscaler = Upscaler::NvidiaDlss;
    // "crt" / "crt480" / "crt0": Retro TV (CRT-Geom) with 240 / 480 / source lines
    if (std::strncmp(argv[7], "crt", 3) == 0) { s.retroTv = true; s.retroLines = argv[7][3] ? std::atoi(argv[7] + 3) : 240; }
    // "pp" = pixel-perfect at the signal's pixels, "pp320x240" = back to that measured grid
    if (std::strncmp(argv[7], "pp", 2) == 0) {
        s.pixelPerfect = true;
        if (std::sscanf(argv[7] + 2, "%dx%d", &s.gridW, &s.gridH) == 2) s.pixelGrid = 1;
    }
    if (const char* aa = std::getenv("VEYAMUX_TEST_AA")) {   // "fxaa,q" or "smaa,q" (q 0..2)
        s.fxaa = true; s.smaa = std::strncmp(aa, "smaa", 4) == 0;
        if (const char* c = std::strchr(aa, ',')) s.fxaaQuality = std::atoi(c + 1);
    }
    if (const char* c = std::getenv("VEYAMUX_TEST_COMPARE")) {   // comparison clip layout 1..4, with two test labels
        chain.setCompare(std::atoi(c));
        std::vector<uint8_t> a(160 * 40 * 4), b(200 * 40 * 4);
        for (size_t i = 0; i < a.size(); i += 4) { a[i] = 20; a[i + 1] = 20; a[i + 2] = 20; a[i + 3] = 200; }
        for (size_t i = 0; i < b.size(); i += 4) { b[i] = 255; b[i + 1] = 120; b[i + 2] = 60; b[i + 3] = 230; }
        chain.setCompareLabels(a, 160, 40, b, 200, 40);
    }
    if (const char* d = std::getenv("VEYAMUX_TEST_DETAIL"))   // "texture,clarity,dehaze" in -1..1
        std::sscanf(d, "%f,%f,%f", &s.texture, &s.clarity, &s.dehaze);
    if (const char* nr = std::getenv("VEYAMUX_TEST_NR")) {   // "mode,strength": 1 spatial, 2 temporal, 3 both
        int m = 0, k = 1; std::sscanf(nr, "%d,%d", &m, &k);
        s.denoise = Denoise(m); s.denoiseStrength = k;
    }
    if (const char* sh = std::getenv("VEYAMUX_TEST_SHARPEN")) s.sharpen = float(atof(sh));
    std::vector<uint8_t> frame(size_t(W) * H * 4), out;
    FILE* in = std::fopen(argv[2], "rb"); FILE* fo = std::fopen(argv[6], "wb");
    if (!in || !fo) return 1;
    double total = 0; int n = 0, ow = 0, oh = 0;
    for (int i = 0; i < N; ++i) {
        if (std::fread(frame.data(), 1, frame.size(), in) != frame.size()) break;
        const auto t0 = std::chrono::steady_clock::now();
        if (!chain.process(frame.data(), W, H, W * 4, s, out, ow, oh, err, 1000.0f / 30.0f)) {
            std::fprintf(stderr, "process: %s\n", err.c_str()); return 1;
        }
        total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++n;
        std::fwrite(out.data(), 1, out.size(), fo);
        if (const char* mvPath = std::getenv("VEYAMUX_DUMP_MV")) {   // append the motion field FSR used, every frame
            BufferRes b = ctx.createBuffer(VkDeviceSize(W) * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            for (int which = 0; which < 1; ++which) {
                ctx.beginFrame();
                chain.upscaler()->debugCopyMotion(ctx.cmd(), b.buffer, which);
                VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(ctx.cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
                ctx.submitAndWait(err);
                FILE* m = std::fopen(mvPath, i == 0 && which == 0 ? "wb" : "ab");
                std::fwrite(b.mapped, 1, size_t(W) * H * 8, m); std::fclose(m);   // RGBA16F: mv.xy, confidence
            }
            ctx.destroy(b);
        }
    }
    std::fclose(fo); std::fclose(in);
    std::printf("%s %dx%d -> %dx%d: %d frames, %.1f ms/frame%s%s\n", fsr ? FsrUpscaler::versionString() : argv[7], W, H, ow, oh, n,
                n ? total / n : 0.0, chain.upError().empty() ? "" : ", error: ", chain.upError().c_str());
    return chain.upError().empty() ? 0 : 3;
}
