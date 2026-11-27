// SPDX-License-Identifier: BSD-3-Clause
//
// A gain plug-in that can render two ways: through an OpenCL device the host
// owns, or on the CPU when the host does not offer one.
//
// The OFX shape this follows is "OpenCL Buffers" (ofxGPURender.h): the plug-in
// sets kOfxImageEffectPropOpenCLRenderSupported on its descriptor, and a host
// that agrees sets kOfxImageEffectPropOpenCLEnabled=1 plus
// kOfxImageEffectPropOpenCLCommandQueue on the render action. In that mode
// kOfxImagePropData carries a cl_mem instead of a host pointer, and the plug-in
// enqueues its kernel onto the host's queue. It does not wait for it.
//
// The header is not tidy about the two modes - it labels
// kOfxImageEffectPropOpenCLSupported "(Buffers)" in one paragraph and
// "(Images)" in another, and the Support library's two setters disagree with
// both. What is unambiguous is the data property: buffers arrive in
// kOfxImagePropData, images in kOfxImageEffectPropOpenCLImage, and rowBytes is
// not negative for either. That is what this file relies on, and
// references/cmake-and-linking.md records the contradiction so the next reader
// does not have to find it twice.
//
// One more thing this plug-in does not do: it never falls back to the CPU while
// OpenCL is enabled. It cannot - the buffer it was handed is a cl_mem, not a
// pointer to floats - so a kernel that fails to build is an error, not a quiet
// second path. The CPU path runs when the host says OpenCL is off.
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "ofxsImageEffect.h"
// OFX::ImageProcessor is not declared in ofxsImageEffect.h; it lives here.
#include "ofxsProcessing.h"
// kOfxImageEffectPropCPURenderSupported, kOfxImageEffectPropOpenCL* and the
// OpenCL argument fields of RenderArguments are declared with the GPU render
// support, not in ofxImageEffect.h.
#include "ofxGPURender.h"

#include "ClRuntime.h"
#include "PathDecision.h"

namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.openclgain";
const char* const kPluginName = "OpenCL Gain";
const char* const kParamGain = "gain";
const char* const kParamDevice = "device";

#ifndef OPENCL_GAIN_DEFAULT
#define OPENCL_GAIN_DEFAULT 1.0
#endif
constexpr double kDefaultGain = OPENCL_GAIN_DEFAULT;

constexpr int kComponents = 4;

////////////////////////////////////////////////////////////////////////////////
// Which path renders this frame
////////////////////////////////////////////////////////////////////////////////

/// OFXPLUGIN_OPENCL_MODE, read once per process. The word is one of auto, cpu,
/// opencl (or gpu); anything else reads as auto and is named in the label, so a
/// typo is visible instead of quietly ignored.
oclgain::Request requestedMode()
{
    static const oclgain::Request mode = [] {
        bool recognised = true;
        const oclgain::Request parsed =
            oclgain::ParseModeOverride(std::getenv("OFXPLUGIN_OPENCL_MODE"), &recognised);
        return recognised ? parsed : oclgain::Request::Auto;
    }();
    return mode;
}

/// "" or the sentence a label appends when the override was not understood.
std::string overrideProblem()
{
    const char* text = std::getenv("OFXPLUGIN_OPENCL_MODE");
    if (text == nullptr || *text == '\0') {
        return std::string();
    }
    bool recognised = true;
    oclgain::ParseModeOverride(text, &recognised);
    if (recognised) {
        return std::string();
    }
    return std::string("OFXPLUGIN_OPENCL_MODE='") + text + "' is not auto, cpu or opencl";
}

/// The status a handover fault throws. The two that cannot be anything but an
/// error keep the codes this render threw before the checks moved into
/// PathDecision.h: a cl_mem is not a host pointer, so there is no CPU path to
/// demote to.
OfxStatus handoverStatus(oclgain::HandoverFault fault)
{
    switch (fault) {
    case oclgain::HandoverFault::NoQueue:
    case oclgain::HandoverFault::PartialImages:
        return kOfxStatErrValue;
    case oclgain::HandoverFault::NoBuffers:
        return kOfxStatErrBadHandle;
    case oclgain::HandoverFault::BadPitch:
    case oclgain::HandoverFault::BadOrigin:
        return kOfxStatErrUnsupported;
    case oclgain::HandoverFault::None:
        break;
    }
    return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// Pixel work, CPU path
////////////////////////////////////////////////////////////////////////////////

class GainProcessor : public OFX::ImageProcessor {
public:
    explicit GainProcessor(OFX::ImageEffect& effect)
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
            if (srcRow == nullptr) {
                continue;
            }
            for (int x = window.x1; x < window.x2;
                 ++x, dstRow += kComponents, srcRow += kComponents) {
                // RGB only; alpha is copied. A gain is a colour operation, and
                // scaling alpha too would change coverage instead of colour -
                // references/colour-and-alpha.md section 2.
                for (int c = 0; c < kComponents - 1; ++c) {
                    dstRow[c] = srcRow[c] * _gain;
                }
                dstRow[kComponents - 1] = srcRow[kComponents - 1];
            }
        }
    }

private:
    OFX::Image* _src;
    float _gain;
};

////////////////////////////////////////////////////////////////////////////////
// Effect instance
////////////////////////////////////////////////////////////////////////////////

class OpenCLGainPlugin : public OFX::ImageEffect {
public:
    explicit OpenCLGainPlugin(OfxImageEffectHandle handle)
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
        if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr ||
            _device == nullptr) {
            OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
        }
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

        // A host may hand over a window the image does not cover. The status is
        // ours to choose here, so it is an error rather than an untouched frame.
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

        oclgain::Facts facts;
        facts.hostEnabledOpenCL = args.isEnabledOpenCLRender;
        facts.queuePresent = args.pOpenCLCmdQ != nullptr;
        const oclgain::Decision decision = oclgain::DecidePath(requestedMode(), facts);

        if (decision.path == oclgain::Path::OpenCL) {
            setLabel(std::string("OpenCL - ") + decision.reason);
            renderWithOpenCL(args, *src, *dst, gain);
            return;
        }

        // The CPU loop cannot read a cl_mem: kOfxImagePropData is a buffer
        // handle, not a host pointer, and multiplying it would render the
        // handle's bytes. The override is the only way to arrive here with
        // OpenCL enabled, and it is a refusal with the reason in the label, not
        // a silent second path.
        if (args.isEnabledOpenCLRender) {
            setLabel(std::string("CPU - ") + decision.reason +
                     "; the host handed a cl_mem, which the CPU loop cannot read");
            OFX::Log::error(true, "OpenCL render refused: %s", decision.reason);
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

        setLabel(std::string("CPU - ") + decision.reason);

        GainProcessor processor(*this);
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
    /// Writes the Device label when it changed, with the sentence that names an
    /// override the parser did not understand. A host may refuse a write during
    /// create; the label is a diagnostic, not a contract, and a refused write is
    /// retried on the next render.
    void setLabel(std::string text)
    {
        const std::string problem = overrideProblem();
        if (!problem.empty()) {
            text += "; ";
            text += problem;
        }
        if (text == _label) {
            return;
        }
        try {
            _device->setValue(text);
            _label = text;
        } catch (const OFX::Exception::Suite&) {
        }
    }

    void renderWithOpenCL(const OFX::RenderArguments& args, OFX::Image& src, OFX::Image& dst,
                          float gain)
    {
        const OfxRectI& window = args.renderWindow;
        const OfxRectI& sb = src.getBounds();
        const OfxRectI& db = dst.getBounds();

        // Which handover is this? ofxGPURender.h: "if a plug-in supports both, it
        // must use kOfxImageEffectPropOpenCLImage to determine if Images or
        // Buffers are being used for a given render action". The property is
        // absent in Buffers mode, so its presence is the answer - and a host that
        // sets it on one image and not the other is broken, not half-broken.
        void* srcImage = src.getPropertySet().propGetPointer(kOfxImageEffectPropOpenCLImage, 0);
        void* dstImage = dst.getPropertySet().propGetPointer(kOfxImageEffectPropOpenCLImage, 0);
        // Buffers mode: kOfxImagePropData is the cl_mem, not a pointer to pixels.
        void* srcBuffer = src.getPropertySet().propGetPointer(kOfxImagePropData, 0);
        void* dstBuffer = dst.getPropertySet().propGetPointer(kOfxImagePropData, 0);

        // Every check the render used to do inline, in one function that a
        // machine without a device can test: src/PathDecision.h.
        oclgain::HandoverFacts hf;
        hf.queuePresent = args.pOpenCLCmdQ != nullptr;
        hf.imagesHandover = srcImage != nullptr || dstImage != nullptr;
        hf.sourceImage = srcImage != nullptr;
        hf.destImage = dstImage != nullptr;
        hf.sourceBuffer = srcBuffer != nullptr;
        hf.destBuffer = dstBuffer != nullptr;
        hf.sourceRowPitch = src.getRowBytes();
        hf.destRowPitch = dst.getRowBytes();
        hf.sameBoundsOrigin = sb.x1 == db.x1 && sb.y1 == db.y1;
        const oclgain::HandoverFault fault = oclgain::CheckHandover(hf);
        if (fault != oclgain::HandoverFault::None) {
            OFX::Log::error(true, "OpenCL render refused: %s",
                            oclgain::HandoverFaultText(fault));
            OFX::throwSuiteStatusException(handoverStatus(fault));
        }

        std::string why;
        if (hf.imagesHandover) {
            if (!_opencl.prepare(args.pOpenCLCmdQ, /*images=*/true, &why)) {
                throwError(why);
            }
            const float gains[4] = {gain, gain, gain, 1.0f};
            if (!_opencl.enqueueGainImages(args.pOpenCLCmdQ, srcImage, dstImage,
                                           window.x2 - window.x1, window.y2 - window.y1,
                                           window.x1 - sb.x1, window.y1 - sb.y1, gains, &why)) {
                throwError(why);
            }
            return;
        }

        if (!_opencl.prepare(args.pOpenCLCmdQ, /*images=*/false, &why)) {
            throwError(why);
        }
        // Both images start their buffer at their own bounds origin, so the two
        // pitch/origin pairs are passed separately rather than assumed equal;
        // that is already checked above, before the handover is chosen.
        // The cl_mem covers the allocation from the bounds origin; the window
        // may start inside it. A kernel handed only width and height writes the
        // corner of the frame instead of the window, which is silent until a
        // host renders a tile.
        if (!_opencl.enqueueGain(args.pOpenCLCmdQ, srcBuffer, dstBuffer,
                                 window.x2 - window.x1, window.y2 - window.y1,
                                 window.x1 - sb.x1, window.y1 - sb.y1,
                                 hf.sourceRowPitch / static_cast<int>(sizeof(float)), gain,
                                 &why)) {
            throwError(why);
        }
        // No wait: the queue is the host's, and ofxGPURender.h asks the plug-in
        // not to finish it. A host that wants the pixels reads them back.
    }

    static void throwError(const std::string& why)
    {
        // The status a host can act on, and the driver's own words in the log.
        OFX::Log::error(true, "OpenCL render failed: %s", why.c_str());
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    OFX::Clip* _dstClip;
    OFX::Clip* _srcClip;
    OFX::DoubleParam* _gain;
    OFX::StringParam* _device;
    std::string _label;
    ocl::Runtime _opencl;
};

////////////////////////////////////////////////////////////////////////////////
// Factory
////////////////////////////////////////////////////////////////////////////////

class OpenCLGainPluginFactory : public OFX::PluginFactoryHelper<OpenCLGainPluginFactory> {
public:
    OpenCLGainPluginFactory()
        : OFX::PluginFactoryHelper<OpenCLGainPluginFactory>(kPluginIdentifier, kVersionMajor,
                                                            kVersionMinor)
    {
    }

    void describe(OFX::ImageEffectDescriptor& desc) override
    {
        desc.setLabels(kPluginName, kPluginName, kPluginName);
        desc.setPluginGrouping("OFX Plugin Developer");
        desc.setPluginDescription("Multiplies RGBA by a constant, on an OpenCL device when the "
                                  "host offers one and on the CPU otherwise.");

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
        desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);

        // Both answers are "yes": the CPU path keeps the plug-in usable on a
        // host without OpenCL, and the OpenCL path is offered when there is one.
        // setSupportsOpenCLBuffersRender() writes the property only when the host
        // advertises it (ofxsImageEffect.cpp:669-675), so a host that has no
        // OpenCL sees a plug-in with no OpenCL claim rather than a broken one.
        // Both handovers of ofxGPURender.h: Buffers (cl_mem in kOfxImagePropData,
        // written through the buffer kernel) and Images (image2d_t in
        // kOfxImageEffectPropOpenCLImage, written through the image kernel). A
        // host that offers only one gets the one it offers.
        // OFXPLUGIN_OPENCL_MODE=cpu is applied here too, and that is the half
        // that makes the override useful on a machine that has a device: a host
        // cannot hand a plug-in a cl_mem it never asked for, so dropping the two
        // claims is what turns the override into a CPU render instead of an
        // error inside the first render. The claims are skipped rather than set
        // to false, so a host that has no OpenCL sees exactly what it saw
        // before.
        if (requestedMode() != oclgain::Request::CpuOnly) {
            desc.setSupportsOpenCLBuffersRender(true);
            desc.setSupportsOpenCLImagesRender(true);
        }
        // OFX 1.5.1 added this, and a host is not required to know it: the write
        // then throws, and an unguarded one takes the whole descriptor down with
        // it. Resolve Studio 21.2 does exactly that - every context creation fails
        // and the plug-in never renders (2026-11-27,
        // evidence/2026-11-27-windows/opencl-host-resolve.log). The property's
        // default is already "true", so a host that ignores the write sees the
        // same answer.
        try {
            desc.getPropertySet().propSetString(kOfxImageEffectPropCPURenderSupported, "true");
        } catch (const OFX::Exception::PropertyUnknownToHost&) {
        } catch (const OFX::Exception::Suite& e) {
            // MissingHostFeature: the host does not implement the property at
            // all. The support library maps only ErrUnknown/ErrUnsupported to
            // PropertyUnknownToHost, so this status arrives as Suite.
            if (e.status() != kOfxStatErrMissingHostFeature) {
                throw;
            }
        }
        // A host that has never heard of the property answers with an unknown
        // status, and the support library turns that into a failed describe - the
        // whole plug-in then never loads (Resolve 21.2.0.5, measured 2026-11-27).
        try {
            desc.getPropertySet().propSetString(kOfxImageEffectPropNoSpatialAwareness, "true");
        } catch (const OFX::Exception::PropertyUnknownToHost&) {
        } catch (const OFX::Exception::Suite& e) {
            // MissingHostFeature: the host does not implement the property at
            // all. The support library maps only ErrUnknown/ErrUnsupported to
            // PropertyUnknownToHost, so this status arrives as Suite.
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

        // Read-only: which path ran and why. A gain is a gain either way, so this
        // is how a user - and the runner's forced-CPU step - tells the host's
        // OpenCL queue from the loop, and why the loop ran when it did.
        OFX::StringParamDescriptor* device = desc.defineStringParam(kParamDevice);
        device->setLabels("Device", "Device", "Device");
        device->setScriptName(kParamDevice);
        device->setHint("Which path rendered, or why the CPU path ran instead");
        device->setStringType(OFX::eStringTypeLabel);
        device->setDefault("");
        device->setEnabled(false);
        page->addChild(*device);
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override
    {
        (void)context;
        return new OpenCLGainPlugin(handle);
    }
};

} // namespace

void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static OpenCLGainPluginFactory factory;
    ids.push_back(&factory);
}
