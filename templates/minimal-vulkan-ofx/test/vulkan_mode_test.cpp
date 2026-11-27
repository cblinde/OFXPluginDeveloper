// SPDX-License-Identifier: BSD-3-Clause
//
// The half of this template that runs on any machine: the table of gates that
// decides between the device and the CPU loop, and the environment override
// that forces one of them.
//
// Why it exists. Every other template can prove its fallback without the
// hardware - minimal-cuda-ofx has ExecutionMode.h and test/execution_mode_test,
// minimal-opencl-ofx leaves the choice to the host's kOfxImageEffectPropOpenCL*
// properties, and the ONNX template asserts its provider label through the
// contract harness. This template always took the device when one answered, so
// on a machine with a GPU the CPU path was unreachable by a test, and on a
// machine without one only that path ran. VulkanMode.h is the one place both
// callers read, and this is the test that executes it with no device, no
// loader and no SDK - the build does not even link VulkanGain.cpp.
//
// What it does not prove: that the dispatch agrees with the CPU loop (that is
// vulkan_suite's job, and it needs a device), or that the plug-in passes the
// decision through correctly (that is the contract suite's forced-CPU run).
//
// Exit codes: 0 every case matched, 1 a mismatch.
#include "VulkanMode.h"

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what, const char* got, const char* want)
{
    ++checks;
    if (ok) {
        std::printf("  %-46s ok   %s\n", what, got);
    } else {
        std::printf("  %-46s FAIL %s (wanted %s)\n", what, got, want);
        ++failures;
    }
}

void expectMode(bool ok, const char* what, vkgain::Mode got, vkgain::Mode want)
{
    expect(ok, what, vkgain::ModeName(got), vkgain::ModeName(want));
}

vkgain::FrameFacts ready(unsigned int pixels, unsigned int maxPixels = 4294967040u)
{
    vkgain::FrameFacts facts;
    facts.builtWithVulkan = true;
    facts.deviceReady = true;
    facts.pixels = pixels;
    facts.maxPixels = maxPixels;
    return facts;
}

vkgain::FrameFacts noDevice()
{
    vkgain::FrameFacts facts;
    facts.builtWithVulkan = true;
    facts.deviceReady = false;
    facts.pixels = 1920u * 1080u;
    facts.maxPixels = 0;
    return facts;
}

} // namespace

int main()
{
    std::printf("VulkanMode: the six gates, no device needed\n");

    // ---- parsing: three words, a typo, and nothing at all --------------------
    {
        bool recognised = false;
        expectMode(vkgain::ParseModeOverride(nullptr, &recognised) == vkgain::Mode::Auto &&
                       recognised,
                   "OFXPLUGIN_VULKAN_MODE unset reads as auto", vkgain::Mode::Auto,
                   vkgain::Mode::Auto);

        recognised = false;
        const vkgain::Mode empty = vkgain::ParseModeOverride("", &recognised);
        expectMode(empty == vkgain::Mode::Auto && recognised,
                   "an empty string reads as auto", empty, vkgain::Mode::Auto);

        recognised = false;
        const vkgain::Mode autoWord = vkgain::ParseModeOverride("auto", &recognised);
        expectMode(autoWord == vkgain::Mode::Auto && recognised, "\"auto\" reads as auto",
                   autoWord, vkgain::Mode::Auto);

        recognised = false;
        const vkgain::Mode cpu = vkgain::ParseModeOverride("cpu", &recognised);
        expectMode(cpu == vkgain::Mode::CpuOnly && recognised, "\"cpu\" reads as CPU_ONLY", cpu,
                   vkgain::Mode::CpuOnly);

        recognised = false;
        const vkgain::Mode gpu = vkgain::ParseModeOverride("gpu", &recognised);
        expectMode(gpu == vkgain::Mode::VulkanOnly && recognised, "\"gpu\" reads as VULKAN_ONLY",
                   gpu, vkgain::Mode::VulkanOnly);

        recognised = false;
        const vkgain::Mode vulkan = vkgain::ParseModeOverride("vulkan", &recognised);
        expectMode(vulkan == vkgain::Mode::VulkanOnly && recognised,
                   "\"vulkan\" reads as VULKAN_ONLY", vulkan, vkgain::Mode::VulkanOnly);

        // The typo is the case that matters: Auto and "not understood" have to
        // be distinguishable, or the label can never say the override was
        // ignored.
        recognised = true;
        const vkgain::Mode typo = vkgain::ParseModeOverride("CUDA", &recognised);
        expectMode(typo == vkgain::Mode::Auto && !recognised,
                   "an unknown word reads as auto, not recognised", typo, vkgain::Mode::Auto);

        recognised = true;
        const vkgain::Mode cased = vkgain::ParseModeOverride("CPU", &recognised);
        expectMode(cased == vkgain::Mode::Auto && !recognised,
                   "the override is case-sensitive", cased, vkgain::Mode::Auto);
    }

    // ---- names, as the label and the log print them -------------------------
    expect(std::strcmp(vkgain::ModeName(vkgain::Mode::Auto), "AUTO") == 0, "ModeName(auto)",
           vkgain::ModeName(vkgain::Mode::Auto), "AUTO");
    expect(std::strcmp(vkgain::ModeName(vkgain::Mode::CpuOnly), "CPU_ONLY") == 0,
           "ModeName(cpu)", vkgain::ModeName(vkgain::Mode::CpuOnly), "CPU_ONLY");
    expect(std::strcmp(vkgain::ModeName(vkgain::Mode::VulkanOnly), "VULKAN_ONLY") == 0,
           "ModeName(gpu)", vkgain::ModeName(vkgain::Mode::VulkanOnly), "VULKAN_ONLY");

    // ---- gate 0: the override wins, whatever the hardware says --------------
    {
        const auto decision = vkgain::DecideMode(vkgain::Mode::CpuOnly, ready(1920u * 1080u));
        expectMode(decision.mode == vkgain::Mode::CpuOnly, "cpu override beats a ready device",
                   decision.mode, vkgain::Mode::CpuOnly);
        expect(std::strstr(decision.reason, "OFXPLUGIN_VULKAN_MODE=cpu") != nullptr,
               "cpu override names itself in the reason", decision.reason,
               "OFXPLUGIN_VULKAN_MODE=cpu");
    }

    // ---- gate 1: a CPU-only build ------------------------------------------
    {
        vkgain::FrameFacts facts = ready(1920u * 1080u);
        facts.builtWithVulkan = false;
        const auto decision = vkgain::DecideMode(vkgain::Mode::Auto, facts);
        expectMode(decision.mode == vkgain::Mode::CpuOnly, "a CPU-only build stays on the CPU",
                   decision.mode, vkgain::Mode::CpuOnly);
        expect(std::strstr(decision.reason, "OFF") != nullptr, "the reason names the configure",
               decision.reason, "-DOFXPLUGIN_VULKAN=OFF");
    }

    // ---- gate 2: no device, and no override can conjure one ----------------
    {
        const auto decision = vkgain::DecideMode(vkgain::Mode::Auto, noDevice());
        expectMode(decision.mode == vkgain::Mode::CpuOnly, "no device means the CPU loop",
                   decision.mode, vkgain::Mode::CpuOnly);
        expect(std::strstr(decision.reason, "no Vulkan device") != nullptr,
               "the reason says no device answered", decision.reason, "no Vulkan device");

        const auto forced = vkgain::DecideMode(vkgain::Mode::VulkanOnly, noDevice());
        expectMode(forced.mode == vkgain::Mode::CpuOnly,
                   "gpu override is demoted without a device", forced.mode,
                   vkgain::Mode::CpuOnly);
        expect(std::strstr(forced.reason, "no Vulkan device") != nullptr,
               "the demotion gives the device reason, not the override", forced.reason,
               "no Vulkan device");
    }

    // ---- gate 3: an empty window -------------------------------------------
    {
        const auto decision = vkgain::DecideMode(vkgain::Mode::Auto, ready(0));
        expectMode(decision.mode == vkgain::Mode::CpuOnly, "an empty window is not dispatched",
                   decision.mode, vkgain::Mode::CpuOnly);
        expect(std::strstr(decision.reason, "empty") != nullptr, "the reason says empty",
               decision.reason, "empty render window");
    }

    // ---- gate 4: one dispatch's ceiling ------------------------------------
    {
        // Exactly the ceiling is fine; one pixel past it is the truncation the
        // suite's tail guard would hide.
        const auto atLimit = vkgain::DecideMode(vkgain::Mode::Auto, ready(1000u, 1000u));
        expectMode(atLimit.mode == vkgain::Mode::VulkanOnly, "a frame at maxPixels dispatches",
                   atLimit.mode, vkgain::Mode::VulkanOnly);

        const auto overLimit = vkgain::DecideMode(vkgain::Mode::Auto, ready(1001u, 1000u));
        expectMode(overLimit.mode == vkgain::Mode::CpuOnly, "maxPixels + 1 goes to the CPU",
                   overLimit.mode, vkgain::Mode::CpuOnly);
        expect(std::strstr(overLimit.reason, "workgroups") != nullptr,
               "the reason names the dispatch limit", overLimit.reason, "workgroups");

        const auto forcedOver = vkgain::DecideMode(vkgain::Mode::VulkanOnly, ready(1001u, 1000u));
        expectMode(forcedOver.mode == vkgain::Mode::CpuOnly,
                   "gpu override cannot promote a truncated frame", forcedOver.mode,
                   vkgain::Mode::CpuOnly);
    }

    // ---- gates 5 and 6: the device, by override and by auto ----------------
    {
        const auto forced = vkgain::DecideMode(vkgain::Mode::VulkanOnly, ready(1920u * 1080u));
        expectMode(forced.mode == vkgain::Mode::VulkanOnly, "gpu override with a device dispatches",
                   forced.mode, vkgain::Mode::VulkanOnly);
        expect(std::strstr(forced.reason, "OFXPLUGIN_VULKAN_MODE=gpu") != nullptr,
               "the override names itself in the reason", forced.reason,
               "OFXPLUGIN_VULKAN_MODE=gpu");

        const auto automatic = vkgain::DecideMode(vkgain::Mode::Auto, ready(1920u * 1080u));
        expectMode(automatic.mode == vkgain::Mode::VulkanOnly, "auto takes the device when it answers",
                   automatic.mode, vkgain::Mode::VulkanOnly);
        expect(std::strstr(automatic.reason, "device answered") != nullptr,
               "auto gives the device as its reason", automatic.reason, "device answered");
    }

    std::printf("vulkan_mode_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
