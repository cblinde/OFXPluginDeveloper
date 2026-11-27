// SPDX-License-Identifier: BSD-3-Clause
//
// The device half of this template, without an OFX host.
//
// Three frames go through the same Engine the plug-in uses and through the same
// CPU loop Plugin.cpp falls back to, and every channel of every pixel is
// compared:
//
//   * 61 pixels   - not a multiple of the workgroup size, so the guard in the
//                   shader (`i >= push.count`) is what keeps the tail correct;
//   * 1920x1080   - the size a host actually renders, and the one the buffer
//                   growth path is exercised on after the small frame;
//   * gain 1.0    - the identity, which a host may skip; here it checks that the
//                   dispatch is a real pass and not an early-out.
//
// Exit codes: 0 all comparisons passed, 1 a mismatch or a dispatch failure,
// 2 no Vulkan device (the runner reports that as NOT VERIFIED rather than as a
// pass, because a machine without a driver proves nothing about this code).
#include "VulkanGain.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

struct Pixels {
    unsigned int count = 0;
    std::vector<float> values;
};

Pixels makeFrame(unsigned int count, unsigned int seed)
{
    Pixels frame;
    frame.count = count;
    frame.values.resize((size_t)count * 4);
    unsigned int state = seed | 1u;
    for (size_t i = 0; i < frame.values.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        frame.values[i] = (float)((state >> 8) & 0xffff) / 65535.0f;
    }
    return frame;
}

// The CPU path, character for character: RGB scaled, alpha copied.
void cpuGain(std::vector<float> &values, float gain)
{
    for (size_t i = 0; i + 3 < values.size(); i += 4) {
        values[i + 0] *= gain;
        values[i + 1] *= gain;
        values[i + 2] *= gain;
    }
}

int compare(const char *name, const std::vector<float> &gpu, const std::vector<float> &cpu)
{
    int bad = 0;
    float worst = 0.0f;
    size_t worstAt = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
        const float diff = std::fabs(gpu[i] - cpu[i]);
        if (diff > worst) {
            worst = diff;
            worstAt = i;
        }
        if (diff > 1e-6f) {
            ++bad;
        }
    }
    if (bad != 0) {
        std::printf("  %-22s FAIL %d of %zu channels differ, worst %.3g at %zu\n", name, bad,
                    gpu.size(), (double)worst, worstAt);
        return 1;
    }
    std::printf("  %-22s pass %zu channels within 1e-6 (alpha included)\n", name, gpu.size());
    return 0;
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

    int failures = 0;
    struct Case {
        const char *name;
        unsigned int count;
        float gain;
        unsigned int seed;
    };
    const Case cases[] = {
        {"61 px, gain 2.0", 61, 2.0f, 11},
        {"1920x1080, gain 0.5", 1920u * 1080u, 0.5f, 22},
        {"1920x1080, gain 1.0", 1920u * 1080u, 1.0f, 33},
    };

    for (const Case &c : cases) {
        const Pixels source = makeFrame(c.count, c.seed);
        std::vector<float> gpu = source.values;
        std::string error;
        if (!engine.apply(gpu.data(), c.count, c.gain, error)) {
            std::printf("  %-22s FAIL %s\n", c.name, error.c_str());
            ++failures;
            continue;
        }
        std::vector<float> cpu = source.values;
        cpuGain(cpu, c.gain);
        failures += compare(c.name, gpu, cpu);
    }

    if (failures != 0) {
        std::printf("vulkan_selftest: %d case(s) failed\n", failures);
        return 1;
    }
    std::printf("vulkan_selftest: all cases passed\n");
    return 0;
}
