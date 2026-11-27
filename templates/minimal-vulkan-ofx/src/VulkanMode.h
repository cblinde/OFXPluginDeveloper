// SPDX-License-Identifier: BSD-3-Clause
//
// Which path a render takes: the Vulkan device the plug-in owns, or the CPU
// loop it falls back to.
//
// This is policy only - no Vulkan headers, no OFX headers - so the same code is
// compiled into the plug-in and into test/vulkan_mode_test.cpp, which runs on a
// machine with no Vulkan device at all. That is the arrangement
// minimal-cuda-ofx's ExecutionMode.h uses for the same reason: a decision worth
// trusting is a decision a test can execute, and the machine that has to trust
// it most is the one without the hardware.
//
// The gates, in order. Each one can only demote, and none of them can promote:
// a frame that is too big for one dispatch is not going to be talked onto the
// device by an override, because the pixels would be truncated.
//
//   0. `cpu` from OFXPLUGIN_VULKAN_MODE always wins. This is the switch that
//      makes the fallback testable on a machine that has a device: the contract
//      suite renders with it set and asserts the label says CPU.
//   1. A build with -DOFXPLUGIN_VULKAN=OFF has no device code to reach.
//   2. No instance, no device or no compute queue: the CPU loop.
//   3. An empty render window has nothing to dispatch.
//   4. A frame needing more workgroups than maxComputeWorkGroupCount allows
//      would be dispatched truncated - `vulkan_suite` shows the shader's tail
//      guard keeps the part it sees correct, and nothing keeps the rest.
//   5. `gpu` from the environment, with the gates above satisfied.
//   6. Auto, with the gates above satisfied.
//
// What the decision does NOT do is decide the memory arrangement: that is
// vkgain::Memory in VulkanGain.h, measured in vulkan-compute.md section 3, and
// it is a property of a frame that has already reached the device.
#ifndef VKGAIN_MODE_H
#define VKGAIN_MODE_H

#include <cstring>

namespace vkgain {

enum class Mode {
    Auto,       ///< the device when one answers, the CPU loop otherwise
    CpuOnly,    ///< the Support library's multiThreadProcessImages()
    VulkanOnly  ///< one dispatch on the device this plug-in owns
};

inline const char* ModeName(Mode mode)
{
    switch (mode) {
    case Mode::CpuOnly: return "CPU_ONLY";
    case Mode::VulkanOnly: return "VULKAN_ONLY";
    case Mode::Auto: break;
    }
    return "AUTO";
}

/// Parses OFXPLUGIN_VULKAN_MODE. `recognised` is false for a word that is not
/// one of the three, so a typo is reported instead of being read as Auto - an
/// override that silently does nothing is how a "we tested the CPU path" claim
/// gets made about a run that never left the device.
inline Mode ParseModeOverride(const char* text, bool* recognised)
{
    *recognised = true;
    if (text == nullptr || *text == '\0') return Mode::Auto;
    if (std::strcmp(text, "auto") == 0) return Mode::Auto;
    if (std::strcmp(text, "cpu") == 0) return Mode::CpuOnly;
    if (std::strcmp(text, "gpu") == 0 || std::strcmp(text, "vulkan") == 0) {
        return Mode::VulkanOnly;
    }
    *recognised = false;
    return Mode::Auto;
}

/// What the render knows about itself when it has to choose. `pixels` is the
/// render window's pixel count, not the frame's: a host may hand a window
/// smaller than the image, and that is the number that reaches the dispatch.
struct FrameFacts {
    bool builtWithVulkan = true;   ///< -DOFXPLUGIN_VULKAN=ON and compiled in
    bool deviceReady = false;      ///< an instance, a device and a compute queue exist
    unsigned int pixels = 0;       ///< pixels in this render window
    unsigned int maxPixels = 0;    ///< one dispatch on this device, 0 when no device
};

struct ModeDecision {
    Mode mode;
    /// A static string, printed by the suite and written into the plug-in's
    /// Device label on the CPU path: the reason is the useful part of the
    /// decision when a host renders slower than the user expects.
    const char* reason;
};

/// The six gates. Both callers (the plug-in and the test) use this one
/// function; there is no second copy of the rules.
inline ModeDecision DecideMode(Mode requested, const FrameFacts& facts)
{
    if (requested == Mode::CpuOnly) {
        return { Mode::CpuOnly, "OFXPLUGIN_VULKAN_MODE=cpu was set" };
    }
    if (!facts.builtWithVulkan) {
        return { Mode::CpuOnly, "built with -DOFXPLUGIN_VULKAN=OFF" };
    }
    if (!facts.deviceReady) {
        return { Mode::CpuOnly, "no Vulkan device answered" };
    }
    if (facts.pixels == 0) {
        return { Mode::CpuOnly, "empty render window" };
    }
    if (facts.maxPixels != 0 && facts.pixels > facts.maxPixels) {
        return { Mode::CpuOnly,
                 "the frame needs more workgroups than one dispatch covers" };
    }
    if (requested == Mode::VulkanOnly) {
        return { Mode::VulkanOnly, "OFXPLUGIN_VULKAN_MODE=gpu was set" };
    }
    return { Mode::VulkanOnly, "a Vulkan device answered" };
}

} // namespace vkgain

#endif // VKGAIN_MODE_H
