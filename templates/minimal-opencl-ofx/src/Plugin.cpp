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

namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.openclgain";
const char* const kPluginName = "OpenCL Gain";
const char* const kParamGain = "gain";

#ifndef OPENCL_GAIN_DEFAULT
#define OPENCL_GAIN_DEFAULT 1.0
#endif
constexpr double kDefaultGain = OPENCL_GAIN_DEFAULT;

constexpr int kComponents = 4;

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
                for (int c = 0; c < kComponents; ++c) {
                    dstRow[c] = srcRow[c] * _gain;
                }
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
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _gain = fetchDoubleParam(kParamGain);
        if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr) {
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

        if (args.isEnabledOpenCLRender) {
            renderWithOpenCL(args, *src, *dst, gain);
            return;
        }

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
    void renderWithOpenCL(const OFX::RenderArguments& args, OFX::Image& src, OFX::Image& dst,
                          float gain)
    {
        if (args.pOpenCLCmdQ == nullptr) {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }

        // OpenCL Buffers are addressed with a non-negative row pitch:
        // ofxImageEffect.h's kOfxImagePropRowBytes note says negative rowBytes is
        // for CPU images only, "not CUDA/Metal/OpenCL Buffers". A host that sends
        // one anyway gets a refusal instead of a kernel that walks backwards.
        const int srcRowBytes = src.getRowBytes();
        const int dstRowBytes = dst.getRowBytes();
        if (srcRowBytes <= 0 || dstRowBytes <= 0 || srcRowBytes != dstRowBytes ||
            srcRowBytes % static_cast<int>(sizeof(float)) != 0) {
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

        // In this mode kOfxImagePropData is the cl_mem, not a pointer to pixels.
        void* srcBuffer = src.getPropertySet().propGetPointer(kOfxImagePropData, 0);
        void* dstBuffer = dst.getPropertySet().propGetPointer(kOfxImagePropData, 0);
        if (srcBuffer == nullptr || dstBuffer == nullptr) {
            OFX::throwSuiteStatusException(kOfxStatErrBadHandle);
        }

        std::string why;
        if (!_opencl.prepare(args.pOpenCLCmdQ, &why)) {
            throwError(why);
        }
        const OfxRectI& window = args.renderWindow;
        const OfxRectI& sb = src.getBounds();
        const OfxRectI& db = dst.getBounds();
        // Both images start their buffer at their own bounds origin, so the two
        // pitch/origin pairs are passed separately rather than assumed equal.
        if (sb.x1 != db.x1 || sb.y1 != db.y1) {
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
        // The cl_mem covers the allocation from the bounds origin; the window
        // may start inside it. A kernel handed only width and height writes the
        // corner of the frame instead of the window, which is silent until a
        // host renders a tile.
        if (!_opencl.enqueueGain(args.pOpenCLCmdQ, srcBuffer, dstBuffer,
                                 window.x2 - window.x1, window.y2 - window.y1,
                                 window.x1 - sb.x1, window.y1 - sb.y1,
                                 srcRowBytes / static_cast<int>(sizeof(float)), gain, &why)) {
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
        desc.setSupportsOpenCLBuffersRender(true);
        desc.getPropertySet().propSetString(kOfxImageEffectPropCPURenderSupported, "true");
        desc.getPropertySet().propSetString(kOfxImageEffectPropNoSpatialAwareness, "true");
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
        gain->setHint("Multiplies every channel");
        gain->setDefault(kDefaultGain);
        gain->setRange(0.0, 8.0);
        gain->setDisplayRange(0.0, 4.0);
        gain->setIncrement(0.01);
        gain->setDoubleType(OFX::eDoubleTypeScale);
        page->addChild(*gain);
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
