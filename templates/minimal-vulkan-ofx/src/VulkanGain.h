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
// loader is opened with LoadLibrary("vulkan-1.dll") and every entry point is
// resolved through vkGetInstanceProcAddr. The SDK's headers are needed to
// compile (they define the structs and the PFN_ types); the driver's loader is
// what the plug-in talks to.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vkgain {

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

    /// Applies `gain` to `count` RGBA32F pixels in place, alpha untouched, and
    /// returns false with `error` set when the dispatch fails. The buffer is the
    /// caller's: nothing here owns the frame.
    bool apply(float *pixels, unsigned int count, float gain, std::string &error);

private:
    bool create();
    void destroy();

    struct Impl;
    Impl *_impl;
    std::string _device;
    std::string _reason;
};

} // namespace vkgain
