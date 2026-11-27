// SPDX-License-Identifier: BSD-3-Clause
//
// A Vulkan compute device the plug-in owns.
//
// Why this shape, and not a GPU render property: no OFX host hands a Vulkan
// queue or a Vulkan buffer to a plug-in (references/host-nuke-scratch-natron.md
// section 1.1, references/ai-architectures-and-runtimes.md). The host gives CPU
// images and nothing else, so a Vulkan path has to create its own instance,
// device and queue and move pixels in and out itself. That is what this file
// does; it is the same arrangement Gyom03/rife-ofx uses for its NCNN Vulkan
// backend inside Resolve, and the same one Nuke's third-party denoisers use.
//
// No Vulkan SDK is needed at run time and no import library at link time: the
// loader is opened with LoadLibrary("vulkan-1.dll")/dlopen and every entry point
// is resolved through vkGetInstanceProcAddr. The SDK's headers are needed to
// compile (they define the structs and the PFN_ types); the driver's loader is
// what the plug-in talks to.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vkgain {

/// How the frame reaches the device. Both arrangements are correct; they differ
/// in where the pixels live while the shader reads them.
///
/// `HostVisible` is one buffer in memory the CPU can also see - the smallest
/// arrangement that works, and on a discrete GPU that memory is system RAM read
/// and written over PCIe for every pixel, twice per frame.
///
/// `DeviceLocal` is the one a real workload uses: a storage buffer in the GPU's
/// own memory plus a host-visible staging buffer, with `vkCmdCopyBuffer` in both
/// directions. The frame still crosses PCIe - it arrives and leaves as CPU
/// pixels - but the shader reads device memory, and the transfer is one
/// contiguous copy instead of per-access traffic.
enum class Memory {
    Auto,        ///< DeviceLocal when the device offers it, else HostVisible.
    HostVisible, ///< Never fails on a machine whose device has host-visible memory.
    DeviceLocal, ///< Falls back to HostVisible, with `memory()` saying so.
};

/// What the last `apply()` cost, split the way the phases are split. The
/// numbers are for the tests and for whoever tunes this; nothing in the plug-in
/// branches on them.
struct Stats {
    double uploadMs = 0.0;   ///< CPU -> staging (memcpy, and the copy on device)
    double dispatchMs = 0.0; ///< submit and fence wait, the shader's own pass
    double downloadMs = 0.0; ///< staging -> CPU
    unsigned long long bytes = 0;
};

/// One device, created on first use and kept for the plug-in's lifetime.
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// True when an instance, a device and a compute queue exist.
    bool ready();

    /// Why the device could not be created, or "" when it could.
    const std::string &reason() const { return _reason; }

    /// "NVIDIA GeForce RTX 3060 (Vulkan 1.4.357, queue family 1)", or "" until ready().
    const std::string &device() const { return _device; }

    /// Which arrangement the device ended up with: "device-local storage +
    /// host-visible staging" or "host-visible coherent memory", or "" until an
    /// apply() has run.
    const std::string &memory() const { return _memory; }

    /// Timings of the last apply().
    const Stats &stats() const { return _stats; }

    /// Largest pixel count one dispatch can cover on this device, from its
    /// `maxComputeWorkGroupCount` and this shader's workgroup size. A frame
    /// bigger than this needs tiling, and a plug-in that ignores the limit
    /// dispatches a silently truncated frame.
    unsigned int maxPixels();

    /// Applies `gain` to `count` RGBA32F pixels in place, alpha untouched, with
    /// the `Auto` arrangement, and returns false with `error` set when the
    /// dispatch fails. The buffer is the caller's: nothing here owns the frame.
    bool apply(float *pixels, unsigned int count, float gain, std::string &error);

    /// The same, with the arrangement chosen explicitly - what the device suite
    /// uses to prove both paths agree pixel for pixel.
    bool apply(float *pixels, unsigned int count, float gain, Memory how, std::string &error);

private:
    bool create();
    void destroy();

    struct Impl;
    Impl *_impl;
    std::string _device;
    std::string _reason;
    std::string _memory;
    Stats _stats;
};

} // namespace vkgain
