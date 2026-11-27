// SPDX-License-Identifier: BSD-3-Clause
//
// The Vulkan template: the reference plug-in's CPU path, plus a compute pipeline
// the plug-in owns.
//
// What makes this template different from minimal-cuda-ofx and
// minimal-opencl-ofx: those two ask the **host** for a device (a CUDA buffer, an
// OpenCL queue) and degrade when it will not give one. No host offers Vulkan
// anything - `ofxGPURender.h` has no Vulkan property, and the hosts measured here
// pass CPU images only - so this plug-in creates its own instance, device and
// queue, copies the render window in, dispatches, copies it back. Every host can
// run it, including Nuke (references/host-nuke-scratch-natron.md section 1.1),
// which is the point: per-frame copies are the price, and the kernel is the only
// thing that sees a device.
//
// The GPU path is optional at build time (`-DOFXPLUGIN_VULKAN=OFF`) and at run
// time (no driver, no device, a failed dispatch): the CPU loop below is the same
// arithmetic, so a fallback is a slower frame and never a different one.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
// OFX::ImageProcessor is declared in ofxsProcessing.h, not in ofxsImageEffect.h:
// include it yourself or the compiler reports "expected class-name before '{'".
#include "ofxsProcessing.h"
// kOfxImageEffectPropCPURenderSupported lives in the GPU header.
#include "ofxGPURender.h"

#include "VulkanMode.h"

#ifdef OFXPLUGIN_VULKAN
#include "VulkanGain.h"
#endif

namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.vulkaingain";
const char* const kPluginName = "Vulkan Gain";
const char* const kParamGain = "gain";
const char* const kParamDevice = "device";

#ifndef REFERENCE_GAIN_DEFAULT
#define REFERENCE_GAIN_DEFAULT 1.0
#endif
constexpr double kDefaultGain = REFERENCE_GAIN_DEFAULT;

/// RGBA32F: four floats per pixel.
constexpr int kComponents = 4;

////////////////////////////////////////////////////////////////////////////////
// The device, shared by every instance in this process
////////////////////////////////////////////////////////////////////////////////

#ifdef OFXPLUGIN_VULKAN
// One device for the process, created on first render. A per-instance device
// would create an instance and a device per timeline clip - the expensive part
// of Vulkan - and a host may hold many instances of one effect. The mutex
// serialises the dispatch, which the contract asks for anyway: the descriptor
// says eRenderInstanceSafe, so two instances may render at the same time, and
// one queue with one command buffer is not two queues.
vkgain::Engine& engine()
{
    // Deliberately never destroyed. The destructor releases the Vulkan instance
    // and device, and it would run while the host unloads the .ofx - on Windows
    // that is inside DllMain, under the loader lock, where calling into
    // vulkan-1.dll (including the FreeLibrary that closes the loader) deadlocks.
    // Measured 2026-11-27: the contract suite hung at its unload stage with a
    // static Engine and passes with this one. The device lives as long as the
    // process, which is what a host assumes anyway.
    static vkgain::Engine* const instance = new vkgain::Engine();
    return *instance;
}

std::mutex& engineMutex()
{
    static std::mutex mutex;
    return mutex;
}
#endif

/// True when this build compiled the device path in. A constant, not a probe:
/// the configure decided it and a bundle cannot change its mind.
constexpr bool kBuiltWithVulkan =
#ifdef OFXPLUGIN_VULKAN
    true;
#else
    false;
#endif

/// OFXPLUGIN_VULKAN_MODE, read once per process. The word is one of auto, cpu,
/// gpu (or vulkan); anything else reads as auto and is named in the label, so a
/// typo is visible instead of quietly ignored (references/vulkan-compute.md §7).
vkgain::Mode requestedMode()
{
    static const vkgain::Mode mode = [] {
        bool recognised = true;
        const vkgain::Mode parsed =
            vkgain::ParseModeOverride(std::getenv("OFXPLUGIN_VULKAN_MODE"), &recognised);
        return recognised ? parsed : vkgain::Mode::Auto;
    }();
    return mode;
}

bool gpuAvailable()
{
#ifdef OFXPLUGIN_VULKAN
    return engine().ready();
#else
    return false;
#endif
}

/// "" or the sentence a label appends when the override was not understood.
std::string overrideProblem()
{
    const char* text = std::getenv("OFXPLUGIN_VULKAN_MODE");
    if (text == nullptr || *text == '\0') {
        return std::string();
    }
    bool recognised = true;
    vkgain::ParseModeOverride(text, &recognised);
    if (recognised) {
        return std::string();
    }
    return std::string("OFXPLUGIN_VULKAN_MODE='") + text + "' is not auto, cpu or gpu";
}

/// The render window's pixel count, which is the number that reaches the
/// dispatch - not the image's size. A window spanning a host's maximum is
/// clamped instead of wrapping, so the policy never sees a small number for a
/// huge frame.
unsigned int renderPixels(const OfxRectI& window)
{
    const long long width = (long long)window.x2 - (long long)window.x1;
    const long long height = (long long)window.y2 - (long long)window.y1;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const long long pixels = width * height;
    return pixels > 0xffffffffLL ? 0xffffffffu : (unsigned int)pixels;
}

/// What the decision needs to know about this render.
vkgain::FrameFacts frameFacts(unsigned int pixels)
{
    vkgain::FrameFacts facts;
    facts.builtWithVulkan = kBuiltWithVulkan;
    facts.deviceReady = gpuAvailable();
    facts.pixels = pixels;
#ifdef OFXPLUGIN_VULKAN
    facts.maxPixels = facts.deviceReady ? engine().maxPixels() : 0;
#endif
    return facts;
}

/// The label for the CPU path: the gate's reason, the device's own reason when
/// the gate was "no device" (that string names the loader, the instance step or
/// the queue family that failed), and the override when it was not understood.
std::string cpuLabel(const std::string& reason)
{
    std::string text = "CPU - " + reason;
#ifdef OFXPLUGIN_VULKAN
    if (!engine().ready()) {
        const std::string& why = engine().reason();
        if (!why.empty()) {
            text += ": ";
            text += why;
        }
    }
#endif
    if (!overrideProblem().empty()) {
        text += "; ";
        text += overrideProblem();
    }
    return text;
}

/// The device label, or the reason the GPU path is not in use. Written at
/// construction and again whenever a render's decision differs, so a host that
/// shows the parameter never reads a stale claim.
std::string deviceLabel(unsigned int pixels)
{
    const vkgain::ModeDecision decision = vkgain::DecideMode(requestedMode(), frameFacts(pixels));
    if (decision.mode == vkgain::Mode::CpuOnly) {
        return cpuLabel(decision.reason);
    }
#ifdef OFXPLUGIN_VULKAN
    std::string text = engine().device();
    if (!overrideProblem().empty()) {
        text += "; ";
        text += overrideProblem();
    }
    return text;
#else
    return cpuLabel(decision.reason);
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Pixel work
////////////////////////////////////////////////////////////////////////////////

/// The CPU fallback. Also the reference the self-test compares the kernel to, so
/// the two paths cannot drift without a test noticing.
void cpuGain(float* dst, const float* src, int width, float gain)
{
    for (int x = 0; x < width; ++x, dst += kComponents, src += kComponents) {
        for (int c = 0; c < kComponents - 1; ++c) {
            dst[c] = src[c] * gain;
        }
        dst[kComponents - 1] = src[kComponents - 1];
    }
}

class RefGainProcessor : public OFX::ImageProcessor {
public:
    explicit RefGainProcessor(OFX::ImageEffect& effect)
        : OFX::ImageProcessor(effect)
        , _src(nullptr)
        , _gain(1.0f)
    {
    }

    void setSrc(OFX::Image* src) { _src = src; }
    void setGain(float gain) { _gain = gain; }

    void multiThreadProcessImages(OfxRectI window) override
    {
        for (int y = window.y1; y < window.y2; ++y) {
            if (_effect.abort()) {
                break;
            }
            float* dstRow = static_cast<float*>(_dstImg->getPixelAddress(window.x1, y));
            if (dstRow == nullptr) {
                continue;
            }
            const float* srcRow = static_cast<const float*>(_src->getPixelAddress(window.x1, y));
            cpuGain(dstRow, srcRow, window.x2 - window.x1, _gain);
        }
    }

private:
    OFX::Image* _src;
    float _gain;
};

#ifdef OFXPLUGIN_VULKAN
/// Runs the window through the kernel, one contiguous buffer for the whole
/// window. Gathering first is what keeps rowBytes out of the shader: a host may
/// hand a row stride longer than the image (references/pixel-formats-and-pitch.md),
/// and a kernel indexing the host pointer directly would smear pixels across the
/// padding. This is also the "one copy in, one copy out" the template is honest
/// about - 4K RGBA32F is 66 MB per direction.
bool vulkanGain(OFX::Image* src, OFX::Image* dst, const OfxRectI& window, float gain,
                std::string& error)
{
    const int width = window.x2 - window.x1;
    const int height = window.y2 - window.y1;
    if (width <= 0 || height <= 0) {
        error = "empty render window";
        return false;
    }
    const unsigned int count = (unsigned int)width * (unsigned int)height;
    std::vector<float> frame((size_t)count * kComponents);

    for (int y = 0; y < height; ++y) {
        const float* srcRow = static_cast<const float*>(src->getPixelAddress(window.x1, window.y1 + y));
        if (srcRow == nullptr) {
            error = "no source row inside the window";
            return false;
        }
        std::memcpy(frame.data() + (size_t)y * width * kComponents, srcRow,
                    (size_t)width * kComponents * sizeof(float));
    }

    {
        std::lock_guard<std::mutex> lock(engineMutex());
        if (!engine().apply(frame.data(), count, gain, error)) {
            return false;
        }
    }

    for (int y = 0; y < height; ++y) {
        float* dstRow = static_cast<float*>(dst->getPixelAddress(window.x1, window.y1 + y));
        if (dstRow == nullptr) {
            error = "no destination row inside the window";
            return false;
        }
        std::memcpy(dstRow, frame.data() + (size_t)y * width * kComponents,
                    (size_t)width * kComponents * sizeof(float));
    }
    return true;
}
#endif

////////////////////////////////////////////////////////////////////////////////
// Effect instance
////////////////////////////////////////////////////////////////////////////////

class RefGainPlugin : public OFX::ImageEffect {
public:
    explicit RefGainPlugin(OfxImageEffectHandle handle)
        : OFX::ImageEffect(handle)
        , _dstClip(nullptr)
        , _srcClip(nullptr)
        , _gain(nullptr)
        , _device(nullptr)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _gain = fetchDoubleParam(kParamGain);
        _device = fetchStringParam(kParamDevice);
        if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr || _device == nullptr) {
            OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
        }
        // Written at construction, and again whenever a render's decision differs:
        // a read-only parameter the host can show is the only way this plug-in can
        // say "the frames went through the GPU" - Resolve offers no other channel
        // (references/resolve-automation.md §15.5).
        setLabel(deviceLabel(1));
    }

    void render(const OFX::RenderArguments& args) override
    {
        if (_dstClip->getPixelDepth() != OFX::eBitDepthFloat ||
            _dstClip->getPixelComponents() != OFX::ePixelComponentRGBA) {
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

        std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
        if (dst == nullptr) {
            if (abort()) {
                return;
            }
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }

        std::unique_ptr<OFX::Image> src;
        if (_srcClip->isConnected()) {
            src.reset(_srcClip->fetchImage(args.time));
        }
        if (src == nullptr) {
            if (abort()) {
                return;
            }
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }

        if (src->getPixelDepth() != dst->getPixelDepth() ||
            src->getPixelComponents() != dst->getPixelComponents()) {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }

        // A render window outside either image is a host error; getPixelAddress()
        // would return nullptr and the frame would look untouched
        // (ofxsProcessing.h:150-157 in the SDK).
        const OfxRectI& sb = src->getBounds();
        if (args.renderWindow.x1 < sb.x1 || args.renderWindow.y1 < sb.y1 ||
            args.renderWindow.x2 > sb.x2 || args.renderWindow.y2 > sb.y2) {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }
        const OfxRectI& db = dst->getBounds();
        if (args.renderWindow.x1 < db.x1 || args.renderWindow.y1 < db.y1 ||
            args.renderWindow.x2 > db.x2 || args.renderWindow.y2 > db.y2) {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }

        const float gain = static_cast<float>(_gain->getValueAtTime(args.time));

        // The path decision is a table, not an `if` in here: VulkanMode.h holds
        // the gates and test/vulkan_mode_test.cpp executes them on a machine with
        // no device. An override can only demote, so `gpu` without a driver still
        // renders on the CPU - and says why.
        const unsigned int pixels = renderPixels(args.renderWindow);
        const vkgain::ModeDecision decision = vkgain::DecideMode(requestedMode(), frameFacts(pixels));
#ifdef OFXPLUGIN_VULKAN
        if (decision.mode == vkgain::Mode::VulkanOnly) {
            std::string error;
            if (vulkanGain(src.get(), dst.get(), args.renderWindow, gain, error)) {
                setLabel(deviceLabel(pixels));
                return;
            }
            // A dispatch that fails mid-session is reported, not hidden: the
            // label says why the frame came off the CPU, and the frame is still
            // correct because the fallback is the same arithmetic.
            setLabel(cpuLabel(std::string("Vulkan dispatch failed: ") + error));
        } else {
            setLabel(cpuLabel(decision.reason));
        }
#else
        (void)decision;
#endif

        RefGainProcessor processor(*this);
        processor.setDstImg(dst.get());
        processor.setSrc(src.get());
        processor.setGain(gain);
        processor.setRenderWindow(args.renderWindow);
        processor.process();
    }

    bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                    double& identityTime) override
    {
        if (_gain->getValueAtTime(args.time) == 1.0) {
            identityClip = _srcClip;
            identityTime = args.time;
            return true;
        }
        return false;
    }

private:
    /// Writes the Device label when it changed. A host may refuse a write to a
    /// read-only parameter during create; the label is a diagnostic, not a
    /// contract, and a refused write is remembered so the next render retries.
    void setLabel(const std::string& text)
    {
        if (text == _label) {
            return;
        }
        try {
            _device->setValue(text);
            _label = text;
        } catch (const OFX::Exception::Suite&) {
        }
    }

    OFX::Clip* _dstClip;
    OFX::Clip* _srcClip;
    OFX::DoubleParam* _gain;
    OFX::StringParam* _device;
    std::string _label;
};

////////////////////////////////////////////////////////////////////////////////
// Factory
////////////////////////////////////////////////////////////////////////////////

class RefGainPluginFactory : public OFX::PluginFactoryHelper<RefGainPluginFactory> {
public:
    RefGainPluginFactory()
        : OFX::PluginFactoryHelper<RefGainPluginFactory>(kPluginIdentifier, kVersionMajor, kVersionMinor)
    {
    }

    void describe(OFX::ImageEffectDescriptor& desc) override
    {
        desc.setLabels(kPluginName, kPluginName, kPluginName);
        desc.setPluginGrouping("OFX Plugin Developer");
        desc.setPluginDescription("Multiplies RGBA by a constant on a Vulkan compute pipeline the "
                                  "plug-in owns. Falls back to the same arithmetic on the CPU when "
                                  "no Vulkan device answers; the Device parameter says which ran.");

        desc.addSupportedContext(OFX::eContextFilter);
        desc.addSupportedContext(OFX::eContextGeneral);
        desc.addSupportedBitDepth(OFX::eBitDepthFloat);

        desc.setSingleInstance(false);
        desc.setHostFrameThreading(false);
        desc.setSupportsMultiResolution(false);
        desc.setSupportsTiles(false);
        desc.setTemporalClipAccess(false);
        desc.setRenderTwiceAlways(false);
        desc.setSupportsMultipleClipPARs(false);
        desc.setSupportsMultipleClipDepths(false);
        // Several instances may render at once; the device is shared and
        // serialised inside this file, so the promise holds.
        desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);

        // Both OFX 1.5.1 properties are optional for a host, and an unguarded
        // write takes describe() down with it (measured on Resolve 21.2.0.5:
        // "describe failed ... error code 4", references/packaging-and-install.md
        // section 2). Catch both exception types: the support library maps
        // ErrUnknown to PropertyUnknownToHost and MissingHostFeature to Suite.
        try {
            desc.getPropertySet().propSetString(kOfxImageEffectPropCPURenderSupported, "true");
        } catch (const OFX::Exception::PropertyUnknownToHost&) {
        } catch (const OFX::Exception::Suite& e) {
            if (e.status() != kOfxStatErrMissingHostFeature) {
                throw;
            }
        }
        try {
            desc.getPropertySet().propSetString(kOfxImageEffectPropNoSpatialAwareness, "true");
        } catch (const OFX::Exception::PropertyUnknownToHost&) {
        } catch (const OFX::Exception::Suite& e) {
            if (e.status() != kOfxStatErrMissingHostFeature) {
                throw;
            }
        }
    }

    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) override
    {
        if (context != OFX::eContextFilter && context != OFX::eContextGeneral) {
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

        OFX::ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
        srcClip->setTemporalClipAccess(false);
        srcClip->setSupportsTiles(false);
        srcClip->setIsMask(false);

        OFX::ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
        dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
        dstClip->setSupportsTiles(false);

        OFX::PageParamDescriptor* page = desc.definePageParam("Controls");

        OFX::DoubleParamDescriptor* gain = desc.defineDoubleParam(kParamGain);
        gain->setLabels("Gain", "Gain", "Gain");
        gain->setScriptName(kParamGain);
        gain->setHint("Multiplies R, G and B; alpha is left alone");
        gain->setDefault(kDefaultGain);
        gain->setRange(0.0, 8.0);
        gain->setDisplayRange(0.0, 4.0);
        gain->setIncrement(0.01);
        gain->setDoubleType(OFX::eDoubleTypeScale);
        page->addChild(*gain);

        // Read-only: the device that ran, or the CPU and the reason. A gain is a
        // gain either way, so this is how a user tells whether the GPU path is
        // in use at all.
        OFX::StringParamDescriptor* device = desc.defineStringParam(kParamDevice);
        device->setLabels("Device", "Device", "Device");
        device->setScriptName(kParamDevice);
        device->setHint("Which Vulkan device rendered, or why the CPU path ran instead");
        device->setStringType(OFX::eStringTypeLabel);
        device->setDefault("");
        device->setEnabled(false);
        page->addChild(*device);
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override
    {
        (void)context;
        return new RefGainPlugin(handle);
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////
// Registration: the one hook
////////////////////////////////////////////////////////////////////////////////
void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static RefGainPluginFactory factory;
    ids.push_back(&factory);
}
