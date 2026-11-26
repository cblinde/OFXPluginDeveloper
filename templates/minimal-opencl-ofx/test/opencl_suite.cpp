// SPDX-License-Identifier: BSD-3-Clause
//
// The OpenCL template's test: drive the real .ofx through the shared fake host,
// with the host handing over a real OpenCL context, command queue and cl_mem
// buffers from PoCL (or whatever OPENCL_LOADER_LIB names).
//
// What this proves that the CPU contract suite cannot:
//
//   * the plug-in advertises OpenCL Buffers render only in the shape the header
//     describes, and keeps the CPU path as its fallback;
//   * with kOfxImageEffectPropOpenCLEnabled=1 the plug-in actually writes
//     through the cl_mem it was handed - openclDestinationWritten() is false for
//     a plug-in that quietly wrote to a host pointer it never got;
//   * the kernel's addressing follows kOfxImagePropRowBytes and the bounds
//     origin, not width: a sub-window render changes only the window, checked
//     against the previous full-frame result;
//   * a negative row pitch is refused rather than walked backwards, which is
//     what kOfxImagePropRowBytes says about OpenCL Buffers.
//
// What it does not prove: anything about a GPU. PoCL is a CPU device; these are
// real driver semantics, not real hardware throughput.
//
//   ./opencl_suite <plugin.ofx>
//
// Exit status: 0 all checks passed, 1 a check failed, 2 no OpenCL device (the
// suite did not run - say so, do not call it a pass).

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"

#include "fake_host.h"

namespace {

int gChecks = 0;
int gFailed = 0;

void check(bool ok, const std::string& what)
{
    ++gChecks;
    if (!ok) ++gFailed;
    std::printf("  %s %d - %s\n", ok ? "ok" : "FAIL", gChecks, what.c_str());
}

std::vector<float> gradient(int width, int height)
{
    // Powers of two only, so the expected frame is exact in float and a
    // mismatch is addressing or arithmetic, never rounding.
    std::vector<float> rgba(static_cast<size_t>(width) * height * 4, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            rgba[i + 0] = static_cast<float>(x) * 0.125f;
            rgba[i + 1] = static_cast<float>(y) * 0.0625f;
            rgba[i + 2] = static_cast<float>(x + y) * 0.03125f;
            rgba[i + 3] = 1.0f;
        }
    }
    return rgba;
}

// Largest absolute difference over the pixels inside `window`, plus the same
// outside it when `outside` is asked for. Both images are read tightly packed,
// top-down, which is how the harness publishes them.
struct Delta {
    double inside = 0.0;
    double outside = 0.0;
};

Delta compare(const std::vector<float>& got, const std::vector<float>& want, int width, int height,
              const OfxRectI& window)
{
    Delta d;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool in = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
            for (int c = 0; c < 4; ++c) {
                const size_t i = (static_cast<size_t>(y) * width + x) * 4 + c;
                const double diff = std::fabs(static_cast<double>(got[i]) - want[i]);
                if (in) {
                    d.inside = std::max(d.inside, diff);
                } else {
                    d.outside = std::max(d.outside, diff);
                }
            }
        }
    }
    return d;
}

std::vector<float> scale(const std::vector<float>& src, float gain)
{
    std::vector<float> out(src.size());
    for (size_t i = 0; i < src.size(); ++i) out[i] = src[i] * gain;
    return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.ofx>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    std::printf("[1/7] loading %s\n", path);
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        std::fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
        return 1;
    }
    auto count = reinterpret_cast<int (*)()>(dlsym(lib, "OfxGetNumberOfPlugins"));
    auto get = reinterpret_cast<OfxPlugin* (*)(int)>(dlsym(lib, "OfxGetPlugin"));
    if (count == nullptr || get == nullptr) {
        std::fprintf(stderr, "FAIL: the module does not export the two OFX entry points\n");
        return 1;
    }
    check(count() == 1, "OfxGetNumberOfPlugins() is 1");
    OfxPlugin* plugin = get(0);
    check(plugin != nullptr && std::strcmp(plugin->pluginIdentifier,
                                          "com.ofxplugindeveloper.openclgain") == 0,
          "the module identifies itself as com.ofxplugindeveloper.openclgain");

    testkit::FakeHost host;
    std::printf("[2/7] opening an OpenCL device for the host to hand over\n");
    if (!host.setOpenCL(true)) {
        std::fprintf(stderr,
                     "no OpenCL device: nothing to test (set OPENCL_LOADER_LIB / OCL_ICD_VENDORS)\n");
        return 2;
    }
    std::printf("  platform: %s\n  device  : %s\n", host.openclPlatformName().c_str(),
                host.openclDeviceName().c_str());

    host.attach(plugin);
    check(host.load() == kOfxStatOK, "load answered kOfxStatOK");
    check(host.describe() == kOfxStatOK, "describe answered kOfxStatOK");
    check(host.descriptorString(kOfxImageEffectPropOpenCLRenderSupported) == "true",
          "the descriptor advertises OpenCL Buffers render");
    check(host.descriptorString(kOfxImageEffectPropOpenCLSupported) == "true",
          "the descriptor advertises OpenCL Images render");
    check(host.descriptorString(kOfxImageEffectPropCPURenderSupported) == "true",
          "the descriptor keeps the CPU path advertised");

    const char* context = kOfxImageEffectContextFilter;
    check(host.describeInContext(context) == kOfxStatOK, "describeInContext answered kOfxStatOK");
    const int instance = host.createInstanceIndexed(context);
    check(instance >= 0, "createInstance answered a handle");
    if (instance < 0) return 1;
    check(host.setParam("gain", "2.0"), "the host set gain to 2.0");

    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    const std::vector<float> src = gradient(kWidth, kHeight);
    host.setSourcePixels(src, kWidth, kHeight);
    const OfxRectI full{0, 0, kWidth, kHeight};

    std::printf("[3/7] the CPU path, with the host offering no device\n");
    host.setOpenCL(false);
    check(host.render(0.0, full) == kOfxStatOK, "render answered kOfxStatOK without OpenCL");
    const std::vector<float> cpu = host.destination();
    check(sameBits(cpu, scale(src, 2.0f)), "the CPU frame is the source times two, bit for bit");
    check(!host.openclDestinationWritten(), "no device buffer was written while OpenCL was off");

    std::printf("[4/7] the same render with the device enabled\n");
    check(host.setOpenCL(true), "the host re-opened the device");
    check(host.render(0.0, full) == kOfxStatOK, "render answered kOfxStatOK with OpenCL enabled");
    check(host.openclDestinationWritten(),
          "the plug-in wrote through the cl_mem it was handed, not a host pointer");
    const std::vector<float> device = host.destination();
    const Delta fullDelta = compare(device, scale(src, 2.0f), kWidth, kHeight, full);
    check(fullDelta.inside == 0.0,
          "the device frame is the source times two, bit for bit (max delta " +
              std::to_string(fullDelta.inside) + ")");

    std::printf("[5/7] a render window smaller than the frame\n");
    check(host.setParam("gain", "3.0"), "the host set gain to 3.0");
    const OfxRectI window{5, 7, 29, 31};
    check(host.render(0.0, window) == kOfxStatOK, "render answered kOfxStatOK for the window");
    const std::vector<float> partial = host.destination();
    const Delta inWindow = compare(partial, scale(src, 3.0f), kWidth, kHeight, window);
    const Delta outWindow = compare(partial, scale(src, 2.0f), kWidth, kHeight, window);
    check(inWindow.inside == 0.0, "inside the window the frame is the source times three");
    check(outWindow.outside == 0.0,
          "outside the window the previous frame is untouched (the kernel wrote no padding)");

    std::printf("[6/7] the Images handover: image2d_t in kOfxImageEffectPropOpenCLImage\n");
    if (!host.openclImageSupport()) {
        std::printf("  NOT RUN: the device reports no image support (nothing to test)\n");
    } else {
        // The same frame through the other half of ofxGPURender.h: no cl_mem
        // buffer, no rowBytes - a 2D image and coordinates. The plug-in has to
        // notice from the property alone, which is what the header asks for.
        host.setOpenCLHandover(testkit::OpenCLHandover::Images);
        check(host.setParam("gain", "2.0"), "the host set gain to 2.0 for the Images pass");
        check(host.render(0.0, full) == kOfxStatOK, "render answered kOfxStatOK over image2d_t");
        check(host.openclDestinationWritten(),
              "the plug-in wrote through the image it was handed");
        const std::vector<float> imageFrame = host.destination();
        const Delta imageDelta = compare(imageFrame, scale(src, 2.0f), kWidth, kHeight, full);
        check(imageDelta.inside == 0.0,
              "the image frame is the source times two, bit for bit (max delta " +
                  std::to_string(imageDelta.inside) + ")");

        // A window that is not the whole image: the image kernel addresses with
        // coordinates, so a wrong offset would land on the wrong pixels rather
        // than smash a pointer.
        check(host.setParam("gain", "3.0"), "the host set gain to 3.0 for the image window");
        const OfxRectI imageWindow{11, 3, 45, 41};
        check(host.render(0.0, imageWindow) == kOfxStatOK,
              "render answered kOfxStatOK for an image window");
        const std::vector<float> imagePartial = host.destination();
        const Delta imageIn = compare(imagePartial, scale(src, 3.0f), kWidth, kHeight, imageWindow);
        const Delta imageOut = compare(imagePartial, scale(src, 2.0f), kWidth, kHeight, imageWindow);
        check(imageIn.inside == 0.0, "inside the image window the frame is the source times three");
        check(imageOut.outside == 0.0,
              "outside the image window the previous frame is untouched");

        // Back to buffers: the two programs have to coexist for a host that
        // offers both and changes its mind.
        host.setOpenCLHandover(testkit::OpenCLHandover::Buffers);
        check(host.setParam("gain", "2.0"), "the host set gain back to 2.0");
        check(host.render(0.0, full) == kOfxStatOK &&
                  host.openclDestinationWritten(),
              "the buffers handover still renders after the image one");
    }

    std::printf("[7/7] a negative row pitch is refused, not walked backwards\n");
    host.setNegativePitch(true);
    host.setParam("gain", "2.0");
    const OfxStatus refused = host.render(0.0, full);
    check(refused != kOfxStatOK, "render refused a negative row pitch on an OpenCL buffer");
    check(!host.openclDestinationWritten(), "the refusal wrote nothing into the device buffer");

    std::printf("opencl_suite: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
