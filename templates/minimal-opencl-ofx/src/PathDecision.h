// SPDX-License-Identifier: BSD-3-Clause
//
// Which path renders this frame, decided in one place that a machine without a
// device can test.
//
// The OpenCL template's first version let the host decide and nothing else: the
// host either set kOfxImageEffectPropOpenCLEnabled and handed a cl_mem, or it
// did not and the CPU loop ran. That is the right rule, and it made the two
// paths untestable in the way the Vulkan template's gates were untestable - a
// machine with a device never ran the loop, a machine without one never ran the
// kernel, and no run could assert that the decision is *reported*.
//
// So the rule lives here, as a pure function of facts, with three callers: the
// plug-in, test/opencl_path_test.cpp (which needs no device, no loader and no
// SDK), and the runner's forced-CPU step, which checks that the label the
// plug-in writes names this decision.
//
// Two things are deliberately separate:
//   * DecidePath answers which path runs. Every gate here can only demote to
//     the CPU, because the CPU loop is the path that always exists.
//   * CheckHandover answers whether the host's OpenCL handover is usable. A
//     failure here is an *error*, not a demotion: the image the host handed is a
//     cl_mem, so there is no pixels-on-the-CPU to fall back to - reading it as a
//     float* renders the pointer's contents. That is why the second function
//     returns faults instead of a Path.
//
// OFXPLUGIN_OPENCL_MODE overrides the first function and never the second:
//   auto (default)  - the host decides, as above
//   cpu             - the CPU path, and the plug-in does not advertise OpenCL in
//                     describe(), so a host cannot hand it a cl_mem in the first
//                     place; a host that does it anyway gets an error, not a
//                     frame of garbage
//   opencl / gpu    - asks for the device, and is demoted to the CPU with the
//                     reason in the label when the host did not enable OpenCL
// Anything else is unrecognised: it reads as auto and the label says so, so a
// typo is visible instead of silently doing nothing.

#pragma once

#include <cstring>

namespace oclgain {

enum class Path { Cpu, OpenCL };

enum class Request { Auto, CpuOnly, OpenCLOnly };

inline const char* PathName(Path path)
{
    switch (path) {
    case Path::Cpu: return "CPU";
    case Path::OpenCL: return "OPENCL";
    }
    return "?";
}

inline const char* RequestName(Request request)
{
    switch (request) {
    case Request::CpuOnly: return "CPU_ONLY";
    case Request::OpenCLOnly: return "OPENCL_ONLY";
    case Request::Auto: break;
    }
    return "AUTO";
}

/// Parses OFXPLUGIN_OPENCL_MODE. Three spellings are understood - `auto`, `cpu` and
/// `opencl` with its synonym `gpu` - and `recognised` is false for anything else,
/// so a typo is reported instead of being read as auto: an override that silently
/// does nothing is how a "the CPU path was tested" claim gets made about a run
/// that never left the device. `gpu` is not a second behaviour, it is the same
/// device request under the word a reader of the label is most likely to type.
inline Request ParseModeOverride(const char* text, bool* recognised)
{
    *recognised = true;
    if (text == nullptr || *text == '\0') return Request::Auto;
    if (std::strcmp(text, "auto") == 0) return Request::Auto;
    if (std::strcmp(text, "cpu") == 0) return Request::CpuOnly;
    if (std::strcmp(text, "opencl") == 0 || std::strcmp(text, "gpu") == 0) {
        return Request::OpenCLOnly;
    }
    *recognised = false;
    return Request::Auto;
}

/// What the render knows about itself when it has to choose. Both facts come
/// from the render action: kOfxImageEffectPropOpenCLEnabled and whether the host
/// supplied kOfxImageEffectPropOpenCLCommandQueue.
struct Facts {
    bool hostEnabledOpenCL = false;
    bool queuePresent = false;
};

struct Decision {
    Path path;
    /// A static string, written into the plug-in's Device label: the reason is
    /// the useful part of the decision when a host renders slower than the user
    /// expects.
    const char* reason;
};

/// The gates, in the order the plug-in asks them. The override is first because
/// it is the cheapest and the only one an operator can take without changing the
/// host; the host's answer is second because nothing below it matters when the
/// host has not offered a device.
inline Decision DecidePath(Request requested, const Facts& facts)
{
    if (requested == Request::CpuOnly) {
        return { Path::Cpu, "OFXPLUGIN_OPENCL_MODE=cpu was set" };
    }
    if (!facts.hostEnabledOpenCL) {
        if (requested == Request::OpenCLOnly) {
            return { Path::Cpu,
                     "OFXPLUGIN_OPENCL_MODE=opencl was set, but the host did not enable OpenCL" };
        }
        return { Path::Cpu, "the host did not enable OpenCL on this render" };
    }
    // The host says yes and gives no queue. The handover check turns that into
    // kOfxStatErrValue; the path is still the device's, because there is no
    // CPU path for a cl_mem.
    if (!facts.queuePresent) {
        return { Path::OpenCL, "the host enabled OpenCL but handed no command queue" };
    }
    if (requested == Request::OpenCLOnly) {
        return { Path::OpenCL, "OFXPLUGIN_OPENCL_MODE=opencl was set" };
    }
    return { Path::OpenCL, "the host's OpenCL queue" };
}

/// What can be wrong with an OpenCL handover. Each maps to the status the
/// plug-in throws, and the first five are the checks the render used to do inline.
enum class HandoverFault {
    None,
    NoQueue,
    PartialImages,
    NoBuffers,
    BadPitch,
    BadOrigin
};

inline const char* HandoverFaultText(HandoverFault fault)
{
    switch (fault) {
    case HandoverFault::None: return "";
    case HandoverFault::NoQueue:
        return "the host enabled OpenCL but handed no kOfxImageEffectPropOpenCLCommandQueue";
    case HandoverFault::PartialImages:
        return "only one of the two images carries kOfxImageEffectPropOpenCLImage";
    case HandoverFault::NoBuffers:
        return "kOfxImagePropData holds no cl_mem in OpenCL Buffers mode";
    case HandoverFault::BadPitch:
        return "the row pitch is not a positive multiple of 4 bytes, or differs between the "
               "two images";
    case HandoverFault::BadOrigin:
        return "the source and destination bounds start at different pixels";
    }
    return "";
}

/// The facts of one handover, as the render reads them off the two images.
struct HandoverFacts {
    bool queuePresent = false;
    /// kOfxImageEffectPropOpenCLImage is present, so this is the Images handover
    /// (ofxGPURender.h: the property is absent in Buffers mode, so its presence is
    /// the answer).
    bool imagesHandover = false;
    bool sourceImage = false;
    bool destImage = false;
    bool sourceBuffer = false;
    bool destBuffer = false;
    int sourceRowPitch = 0;
    int destRowPitch = 0;
    bool sameBoundsOrigin = true;
};

/// Empty when the handover can be used, otherwise the first thing wrong with it.
///
/// The pitch is checked only inside the Buffers branch, because that is the only
/// handover that has one: `ofxImageEffect.h`'s note on `kOfxImagePropRowBytes`
/// allows a negative pitch "for CPU images only, not CUDA/Metal/OpenCL Buffers"
/// and not at all for OpenCL Images, where the addressing is pure coordinates.
/// A check in front of the branch would refuse a legal Images render for a field
/// the render never reads.
inline HandoverFault CheckHandover(const HandoverFacts& facts)
{
    if (!facts.queuePresent) return HandoverFault::NoQueue;
    if (!facts.sameBoundsOrigin) return HandoverFault::BadOrigin;
    if (facts.imagesHandover) {
        if (facts.sourceImage != facts.destImage) return HandoverFault::PartialImages;
        return HandoverFault::None;
    }
    if (!facts.sourceBuffer || !facts.destBuffer) return HandoverFault::NoBuffers;
    if (facts.sourceRowPitch <= 0 || facts.destRowPitch <= 0 ||
        facts.sourceRowPitch != facts.destRowPitch ||
        facts.sourceRowPitch % static_cast<int>(sizeof(float)) != 0) {
        return HandoverFault::BadPitch;
    }
    return HandoverFault::None;
}

} // namespace oclgain
