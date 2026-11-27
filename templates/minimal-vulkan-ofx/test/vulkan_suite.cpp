// SPDX-License-Identifier: BSD-3-Clause
//
// The device half of this template, without an OFX host: the same `Engine` the
// plug-in uses, the same CPU loop `Plugin.cpp` falls back to, and every channel
// of every pixel compared.
//
// What each case is for, because a suite that only renders 1080p proves less
// than it looks like it proves:
//
//   * 1, 61, 256 and 257 pixels - the workgroup edges. 61 is not a multiple of
//     256, so the shader's `i >= push.count` guard is what keeps the tail
//     correct; 256 is exactly one workgroup and 257 is one plus one pixel.
//   * gain 0.0 and gain 1.0 - the two values a host may short-circuit. Here they
//     check that the dispatch is a real pass: gain 0 must zero RGB and leave
//     alpha alone, gain 1 must return the frame bit for bit.
//   * 4096 px and 1920x1080 - a frame with a texture-like size, and the size a
//     host actually renders.
//   * 3840x2160 then 61 px - the buffer grows and is then reused for a smaller
//     frame, which is the path that runs on every project that changes size.
//   * five applies on one buffer - the frame goes in and out five times without
//     a re-creation, so a stale barrier or a stale descriptor shows up here.
//   * both memory arrangements, and the same frame through each - device-local
//     staging and host-visible memory must agree bit for bit, or one of the two
//     transfers is wrong.
//   * two refusals - a null frame and a zero-pixel frame - because a copy of
//     this template should not dispatch nonsense silently.
//
// Timings are printed, not asserted: a slow GPU still computes the right frame,
// and a threshold would turn this machine's numbers into a gate on the next one.
//
// Exit codes: 0 all checks passed, 1 a mismatch or a dispatch failure, 2 no
// Vulkan device (the runner reports that as NOT VERIFIED rather than as a pass,
// because a machine without a driver proves nothing about this code).
#include "VulkanGain.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using vkgain::Memory;

struct Frame {
    unsigned int count = 0;
    std::vector<float> values;
};

Frame makeFrame(unsigned int count, unsigned int seed)
{
    Frame frame;
    frame.count = count;
    frame.values.resize((size_t)count * 4);
    unsigned int state = seed | 1u;
    for (size_t i = 0; i < frame.values.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        frame.values[i] = (float)((state >> 8) & 0xffff) / 65535.0f;
    }
    return frame;
}

// The CPU path, character for character: RGB scaled, alpha untouched.
void cpuGain(std::vector<float> &values, float gain)
{
    for (size_t i = 0; i + 3 < values.size(); i += 4) {
        values[i + 0] *= gain;
        values[i + 1] *= gain;
        values[i + 2] *= gain;
    }
}

double nowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

int failures = 0;
int checks = 0;

void report(bool ok, const char *name, const char *detail)
{
    ++checks;
    if (ok) {
        std::printf("  %-34s ok   %s\n", name, detail);
    } else {
        std::printf("  %-34s FAIL %s\n", name, detail);
        ++failures;
    }
}

const char *modeName(Memory mode)
{
    return mode == Memory::DeviceLocal ? "device-local" : "host-visible";
}

/// One frame through the engine and through the CPU loop, channel by channel.
void compareCase(vkgain::Engine &engine, Memory mode, const char *name, unsigned int count,
                 float gain, unsigned int seed, float tolerance = 1e-6f)
{
    const Frame source = makeFrame(count, seed);
    std::vector<float> gpu = source.values;
    std::string error;
    char detail[192];
    if (!engine.apply(gpu.data(), count, gain, mode, error)) {
        std::snprintf(detail, sizeof(detail), "%s", error.c_str());
        report(false, name, detail);
        return;
    }
    std::vector<float> cpu = source.values;
    cpuGain(cpu, gain);

    float worst = 0.0f;
    size_t bad = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
        const float diff = std::fabs(gpu[i] - cpu[i]);
        if (diff > worst) {
            worst = diff;
        }
        if (diff > tolerance) {
            ++bad;
        }
    }
    if (bad != 0) {
        std::snprintf(detail, sizeof(detail), "%zu of %zu channels differ, worst %.3g", bad,
                      gpu.size(), (double)worst);
        report(false, name, detail);
        return;
    }

    // Alpha is copied, not computed: if the shader ever touched it, the channel
    // comparison would already say so, but the frame also has to be unchanged
    // in a value the arithmetic never writes.
    size_t alphaMoved = 0;
    for (size_t i = 3; i < gpu.size(); i += 4) {
        if (gpu[i] != source.values[i]) {
            ++alphaMoved;
        }
    }
    if (alphaMoved != 0) {
        std::snprintf(detail, sizeof(detail), "%zu alpha channels changed", alphaMoved);
        report(false, name, detail);
        return;
    }

    std::snprintf(detail, sizeof(detail), "%zu channels, worst %.3g, alpha kept", gpu.size(),
                  (double)worst);
    report(true, name, detail);
}

} // namespace

int main()
{
    vkgain::Engine engine;
    if (!engine.ready()) {
        std::printf("NOT VERIFIED no Vulkan device: %s\n", engine.reason().c_str());
        return 2;
    }
    std::printf("device: %s\n", engine.device().c_str());

    // One throwaway frame, so `memory()` can say what Auto resolves to before
    // the cases print, and so a first-use failure is reported once here.
    {
        Frame probe = makeFrame(61, 7);
        std::string error;
        if (!engine.apply(probe.values.data(), probe.count, 1.0f, error)) {
            std::printf("vulkan_suite: the first dispatch failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("memory: auto picks %s\n", engine.memory().c_str());
    }

    const unsigned int full = 1920u * 1080u;
    const unsigned int uhd = 3840u * 2160u;

    {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "%u px per dispatch", engine.maxPixels());
        report(engine.maxPixels() >= uhd, "maxPixels covers 3840x2160", detail);
    }

    const Memory modes[] = {Memory::HostVisible, Memory::DeviceLocal};
    for (Memory mode : modes) {
        std::printf(" %s\n", modeName(mode));
        char name[96];

        std::snprintf(name, sizeof(name), "%s 1 px gain 2.0", modeName(mode));
        compareCase(engine, mode, name, 1, 2.0f, 11);
        std::snprintf(name, sizeof(name), "%s 61 px gain 2.0 (tail)", modeName(mode));
        compareCase(engine, mode, name, 61, 2.0f, 12);
        std::snprintf(name, sizeof(name), "%s 256 px gain 0.5", modeName(mode));
        compareCase(engine, mode, name, 256, 0.5f, 13);
        std::snprintf(name, sizeof(name), "%s 257 px gain 0.5", modeName(mode));
        compareCase(engine, mode, name, 257, 0.5f, 14);
        std::snprintf(name, sizeof(name), "%s 4096 px gain 0.0", modeName(mode));
        compareCase(engine, mode, name, 4096, 0.0f, 15);
        std::snprintf(name, sizeof(name), "%s 4096 px gain 1.0", modeName(mode));
        compareCase(engine, mode, name, 4096, 1.0f, 16);
        std::snprintf(name, sizeof(name), "%s 1920x1080 gain 0.5", modeName(mode));
        compareCase(engine, mode, name, full, 0.5f, 17);
        std::snprintf(name, sizeof(name), "%s 1920x1080 gain 3.75", modeName(mode));
        compareCase(engine, mode, name, full, 3.75f, 18);
        std::snprintf(name, sizeof(name), "%s 3840x2160 gain 0.25", modeName(mode));
        compareCase(engine, mode, name, uhd, 0.25f, 19);
        // Grow, then reuse the same buffer for a smaller frame.
        std::snprintf(name, sizeof(name), "%s 61 px after 4K (reuse)", modeName(mode));
        compareCase(engine, mode, name, 61, 2.0f, 20);

        // Five applies on one buffer: the frame goes in and out five times.
        {
            const Frame source = makeFrame(4096, 21);
            std::vector<float> gpu = source.values;
            std::string error;
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i) {
                ok = engine.apply(gpu.data(), source.count, 1.5f, mode, error);
            }
            std::vector<float> cpu = source.values;
            for (int i = 0; i < 5; ++i) {
                cpuGain(cpu, 1.5f);
            }
            float worst = 0.0f;
            for (size_t i = 0; gpu.size() == cpu.size() && i < gpu.size(); ++i) {
                worst = std::fmax(worst, std::fabs(gpu[i] - cpu[i]));
            }
            std::snprintf(name, sizeof(name), "%s five applies on one buffer", modeName(mode));
            char detail[128];
            std::snprintf(detail, sizeof(detail), "%s, worst %.3g", ok ? "5 dispatch(es)" : error.c_str(),
                          (double)worst);
            report(ok && gpu.size() == cpu.size() && worst <= 1e-6f, name, detail);
        }
    }

    // The two arrangements must agree bit for bit on the same frame: same
    // shader, same arithmetic, only the transfer differs.
    {
        const Frame source = makeFrame(full, 31);
        std::vector<float> hostPixels = source.values;
        std::vector<float> devicePixels = source.values;
        std::string error;
        char detail[192];
        const bool hostOk = engine.apply(hostPixels.data(), source.count, 0.75f, Memory::HostVisible, error);
        const std::string hostError = error;
        const bool deviceOk = engine.apply(devicePixels.data(), source.count, 0.75f, Memory::DeviceLocal, error);
        if (!hostOk || !deviceOk) {
            std::snprintf(detail, sizeof(detail), "%s", hostError.c_str());
            report(false, "host-visible equals device-local", detail);
        } else {
            size_t differing = 0;
            for (size_t i = 0; i < hostPixels.size(); ++i) {
                if (hostPixels[i] != devicePixels[i]) {
                    ++differing;
                }
            }
            std::snprintf(detail, sizeof(detail), "%zu of %zu channels bit-identical", differing,
                          hostPixels.size());
            report(differing == 0, "host-visible equals device-local", detail);
        }
    }

    {
        std::string error;
        const bool nullRefused = !engine.apply(nullptr, 16, 1.0f, error);
        report(nullRefused, "a null frame is refused", nullRefused ? error.c_str() : "dispatched");
        Frame empty = makeFrame(4, 41);
        const bool zeroRefused = !engine.apply(empty.values.data(), 0, 1.0f, error);
        report(zeroRefused, "a zero-pixel frame is refused",
               zeroRefused ? error.c_str() : "dispatched");
    }

    // Throughput, printed rather than asserted: device, both transfers, and the
    // CPU loop it replaces, on the same frame, measured here and now.
    {
        const int frames = 20;
        const Frame source = makeFrame(full, 51);
        std::printf(" through 1920x1080, %d frames each:\n", frames);
        for (Memory mode : modes) {
            std::vector<float> pixels = source.values;
            std::string error;
            engine.apply(pixels.data(), source.count, 1.01f, mode, error); // warm-up
            const double start = nowMs();
            double upload = 0.0, dispatch = 0.0, download = 0.0;
            bool ok = true;
            for (int i = 0; i < frames && ok; ++i) {
                ok = engine.apply(pixels.data(), source.count, 1.01f, mode, error);
                upload += engine.stats().uploadMs;
                dispatch += engine.stats().dispatchMs;
                download += engine.stats().downloadMs;
            }
            const double total = nowMs() - start;
            const double gb = (double)frames * (double)source.count * 16.0 * 2.0 / 1e9;
            if (ok) {
                std::printf("  %-14s %.2f ms/frame total, dispatch %.2f, copies %.2f "
                            "(%.0f GB/s over both directions)\n",
                            modeName(mode), total / frames, dispatch / frames,
                            (upload + download) / frames, gb / (total / 1000.0));
            } else {
                std::printf("  %-14s not measured: %s\n", modeName(mode), error.c_str());
            }
        }
        const double start = nowMs();
        std::vector<float> cpu = source.values;
        for (int i = 0; i < frames; ++i) {
            cpuGain(cpu, 1.01f);
        }
        const double cpuTotal = nowMs() - start;
        std::printf("  %-14s %.2f ms/frame (the loop render() falls back to)\n", "CPU",
                    cpuTotal / frames);
    }

    std::printf("vulkan_suite: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
