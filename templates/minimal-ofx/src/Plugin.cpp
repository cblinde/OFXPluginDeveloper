// SPDX-License-Identifier: BSD-3-Clause
//
// The reference plug-in: the smallest module a host will load, describe and
// render. Everything here is deliberate:
//
//   * one factory, one instance, one clip pair, one parameter;
//   * the C++ Support library does the C API work;
//   * the only registration hook is OFX::Plugin::getPluginIDs, which the
//     Support library turns into the two exported C entry points.
//
// If you are starting a plug-in, copy this file and CMakeLists.txt. The build
// playbook (references/cmake-and-linking.md) explains every compile-level requirement.
#include <memory>

#include "ofxsImageEffect.h"
// OFX::ImageProcessor is NOT declared in ofxsImageEffect.h (the compiler reports
// "expected class-name before '{'" if you assume it is): it lives in
// ofxsProcessing.h:28, which you must include yourself.
#include "ofxsProcessing.h"
// kOfxImageEffectPropCPURenderSupported is declared in the GPU header, not in
// ofxImageEffect.h where OFX 1.5.1's other two additions are: it only means
// something next to a GPU render, so the SDK groups it there.
#include "ofxGPURender.h"

namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

// Reverse-domain identifier, ASCII, no whitespace: ofxCore.h:104-109 states the
// rule, and the host uses this string as the plug-in's identity in a project.
const char* const kPluginIdentifier = "com.ofxplugindeveloper.referencegain";
const char* const kPluginName = "Reference Gain";
const char* const kParamGain = "gain";

// The gain this build defaults to. 1.0 is a passthrough, and `isIdentity()` below
// tells the host it may skip `render()` entirely - which is why a host-side proof
// of the pixel path cannot use this default. The Resolve scripting API cannot set
// an OFX parameter (references/resolve-automation.md §15.5), so the harness builds
// with -DREFERENCE_GAIN_DEFAULT=2.0 instead and compares the frame against the
// source times two (tests/resolve/README.md).
#ifndef REFERENCE_GAIN_DEFAULT
#define REFERENCE_GAIN_DEFAULT 1.0
#endif
constexpr double kDefaultGain = REFERENCE_GAIN_DEFAULT;

/// RGBA32F: four floats per pixel.
constexpr int kComponents = 4;

////////////////////////////////////////////////////////////////////////////////
// Pixel work
////////////////////////////////////////////////////////////////////////////////

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
            // render() rejects an unconnected source, so _src is always set
            // here: a missing source is a host error, not a black frame.
            const float* srcRow = static_cast<const float*>(_src->getPixelAddress(window.x1, y));

            for (int x = window.x1; x < window.x2;
                 ++x, dstRow += kComponents, srcRow += kComponents) {
                // A gain is a colour operation: R, G and B are scaled and alpha
                // is copied. Scaling alpha by the same factor would leave the
                // un-premultiplied colour untouched and change coverage instead -
                // an opacity change wearing a gain's name, and it darkens a
                // composite over anything. The kernels in the other two templates
                // do the same, and references/colour-and-alpha.md §2 is the
                // reasoning.
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

class RefGainPlugin : public OFX::ImageEffect {
public:
    explicit RefGainPlugin(OfxImageEffectHandle handle)
        : OFX::ImageEffect(handle)
        , _dstClip(nullptr)
        , _srcClip(nullptr)
        , _gain(nullptr)
    {
        // fetchClip/fetchDoubleParam return nullptr when the host did not create
        // what describeInContext declared; report that instead of dereferencing.
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _gain = fetchDoubleParam(kParamGain);
        if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr) {
            OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
        }
    }

    void render(const OFX::RenderArguments& args) override
    {
        // Only the format we declared. Writing pixels in another format is how
        // a plug-in renders garbage instead of failing.
        if (_dstClip->getPixelDepth() != OFX::eBitDepthFloat ||
            _dstClip->getPixelComponents() != OFX::ePixelComponentRGBA) {
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

        // fetchImage returns an owning pointer; hold it in a unique_ptr so every
        // early return below still releases the host's image.
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

        // Image::getPixelAddress() returns nullptr outside the image bounds, and
        // a host may hand over a render window the image does not cover (a crop,
        // a transition, wrong bounds). ImageProcessor::process() does compare the
        // window with the destination bounds, but it answers with a bare return
        // and its assert is compiled out of a release build: the host would get
        // kOfxStatOK and an untouched frame. The branch is
        // .ofx-ref/Support/include/ofxsProcessing.h:150-157 - assert first,
        // then `return` before preProcess(), so no status leaves the process.
        // Refuse both windows here, where the status is ours to choose, and
        // before any row arithmetic runs.
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

        RefGainProcessor processor(*this);
        processor.setDstImg(dst.get());
        processor.setSrc(src.get());
        processor.setGain(static_cast<float>(_gain->getValueAtTime(args.time)));
        processor.setRenderWindow(args.renderWindow);
        processor.process();
    }

    bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                    double& identityTime) override
    {
        // A gain of exactly 1.0 is a passthrough, and the host can skip this
        // plug-in entirely. The CUDA template does the same for both gains.
        if (_gain->getValueAtTime(args.time) == 1.0) {
            identityClip = _srcClip;
            identityTime = args.time;
            return true;
        }
        return false;
    }

private:
    OFX::Clip* _dstClip;
    OFX::Clip* _srcClip;
    OFX::DoubleParam* _gain;
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
        desc.setPluginDescription("Multiplies RGBA by a constant. The reference "
                                  "plug-in for this repository's build playbook.");

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

        // OFX 1.5.1 added two questions a host may ask before it renders: can
        // this plug-in render on the CPU, and can it render without spatial
        // awareness. This is the CPU reference, so both answers are "true".
        // The second is a promise the plug-in keeps by having no spatial
        // parameters to disable - a host only uses it as a hint.
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
        }
    // A host that has never heard of the property answers with an unknown
    // status, and the support library turns that into a failed describe - the
    // whole plug-in then never loads (Resolve 21.2.0.5, measured 2026-11-27).
    try {
        // A host that has never heard of the property answers with an unknown
        // status, and the support library turns that into a failed describe - the
        // whole plug-in then never loads (Resolve 21.2.0.5, measured 2026-11-27).
        try {
            desc.getPropertySet().propSetString(kOfxImageEffectPropNoSpatialAwareness, "true");
        } catch (const OFX::Exception::PropertyUnknownToHost&) {
        }
    } catch (const OFX::Exception::PropertyUnknownToHost&) {
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
//
// The Support library calls this from OfxGetNumberOfPlugins()/OfxGetPlugin()
// (ofxsImageEffect.cpp:3039). The factory has static storage duration because
// the OfxPlugin structs it describes must outlive every host call.
void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static RefGainPluginFactory factory;
    ids.push_back(&factory);
}
