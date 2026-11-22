// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal CUDA OpenFX image effect.
//
// Layout of this file:
//   1. constants and param names
//   2. GainProcessor  - the pixel work (CUDA + CPU)
//   3. GainPlugin     - the effect instance (render / isIdentity)
//   4. Factory        - describe / describeInContext / createInstance
//   5. OFX::Plugin::getPluginIDs - the one registration hook the Support
//      library requires; it generates OfxGetNumberOfPlugins/OfxGetPlugin.
#include "Plugin.h"

#include <cstdint>
#include <memory>
#include <string>

#include "ofxsLog.h"
// ofxGPURender.h is NOT pulled in by ofxsImageEffect.h. It is the header that
// defines kOfxImageEffectPropCudaStream and the GPU status codes
// (kOfxStatGPURenderFailed, kOfxStatGPUOutOfMemory).
#include "ofxGPURender.h"

#include "Kernel.cuh"

namespace mcofx {
namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.minimalcudagain";
const char* const kPluginName = "Minimal CUDA Gain";
const char* const kPluginGrouping = "OFX Plugin Developer";
const char* const kPluginDescription =
    "Per-channel gain with a zero-copy CUDA render path and a CPU fallback. "
    "Reference implementation for pitch-accurate OFX GPU plugins.";

const char* const kParamGain = "gain";
const char* const kParamGainAlpha = "gainAlpha";

/// RGBA32F only: 4 floats per pixel. Kept as a named constant because every
/// pointer offset in this file is expressed in terms of it.
constexpr std::int64_t kPixelBytes = 4 * sizeof(float);

} // namespace

////////////////////////////////////////////////////////////////////////////////
// 2. Processor
////////////////////////////////////////////////////////////////////////////////

GainProcessor::GainProcessor(OFX::ImageEffect& effect)
    : OFX::ImageProcessor(effect)
    , _src(nullptr)
{
    _gain[0] = _gain[1] = _gain[2] = _gain[3] = 1.0f;
}

void GainProcessor::setSrcImg(OFX::Image* src)
{
    _src = src;
}

void GainProcessor::setGain(const float gain[4])
{
    for (int c = 0; c < 4; ++c) {
        _gain[c] = gain[c];
    }
}

void GainProcessor::processImagesCuda()
{
#ifdef OFXPLUGIN_WITH_CUDA
    // kOfxImageEffectPropCudaEnabled (surfaced as isEnabledCudaRender) is the
    // only authority on where the pixels live for THIS render. The descriptor's
    // kOfxImageEffectPropCudaRenderSupported string just says what we can do.
    // OFX::ImageProcessor already routes on it; assert it so a future refactor
    // cannot start treating host pointers as device pointers.
    if (!_isEnabledCudaRender) {
        OFX::Log::print("%s", "Minimal CUDA Gain: CUDA path entered without kOfxImageEffectPropCudaEnabled");
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    const OfxRectI window = _renderWindow;
    const int width = window.x2 - window.x1;
    const int height = window.y2 - window.y1;
    if (width <= 0 || height <= 0 || _src == nullptr || _dstImg == nullptr) {
        return;
    }

    const OfxRectI& srcBounds = _src->getBounds();
    const OfxRectI& dstBounds = _dstImg->getBounds();

    // The host hands us base pointers for the WHOLE image, not for the window.
    // Both the row origin (y) and the column origin (x) have to be applied by
    // hand. Doing this on a device pointer is safe: it is integer arithmetic on
    // the handle, no dereference happens on the CPU. rowBytes is signed and may
    // be negative, hence the int64_t widening of both the pitch and the row.
    const std::int64_t srcPitch = _src->getRowBytes();
    const std::int64_t dstPitch = _dstImg->getRowBytes();

    const char* srcOrigin = static_cast<const char*>(_src->getPixelData())
                          + static_cast<std::int64_t>(window.y1 - srcBounds.y1) * srcPitch
                          + static_cast<std::int64_t>(window.x1 - srcBounds.x1) * kPixelBytes;
    char* dstOrigin = static_cast<char*>(_dstImg->getPixelData())
                    + static_cast<std::int64_t>(window.y1 - dstBounds.y1) * dstPitch
                    + static_cast<std::int64_t>(window.x1 - dstBounds.x1) * kPixelBytes;

    const int status = LaunchGainRGBA32F(_pCudaStream, srcOrigin, dstOrigin, width, height,
                                         srcPitch, dstPitch, _gain);
    if (status != kLaunchOK) {
        OFX::Log::print("Minimal CUDA Gain: kernel launch failed: %s", StatusMessage(status));
        OFX::throwSuiteStatusException(kOfxStatGPURenderFailed);
    }

    // The OFX contract is asymmetric and this branch is the whole contract:
    //   * host supplied kOfxImageEffectPropCudaStream -> return immediately,
    //     the host will order its own work against that stream;
    //   * no stream -> our work must have completed before render returns.
    if (_pCudaStream == nullptr) {
        const int sync = WaitForDefaultStream();
        if (sync != 0) {
            OFX::Log::print("Minimal CUDA Gain: stream sync failed: %s", StatusMessage(sync));
            OFX::throwSuiteStatusException(kOfxStatGPURenderFailed);
        }
    }
#else
    // Built without a CUDA toolchain. The descriptor never advertises CUDA in
    // that configuration, so reaching here means the host ignored the flags.
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
#endif
}

void GainProcessor::multiThreadProcessImages(OfxRectI window)
{
    for (int y = window.y1; y < window.y2; ++y) {
        if (_effect.abort()) {
            break;
        }

        // Destination is guaranteed to be inside its bounds here:
        // ImageProcessor::process() validates the render window against them.
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(window.x1, y));
        if (dstPix == nullptr) {
            continue;
        }

        for (int x = window.x1; x < window.x2; ++x) {
            const float* srcPix = _src ? static_cast<const float*>(_src->getPixelAddress(x, y)) : nullptr;
            if (srcPix != nullptr) {
                dstPix[0] = srcPix[0] * _gain[0];
                dstPix[1] = srcPix[1] * _gain[1];
                dstPix[2] = srcPix[2] * _gain[2];
                dstPix[3] = srcPix[3] * _gain[3];
            } else {
                dstPix[0] = dstPix[1] = dstPix[2] = dstPix[3] = 0.0f;
            }
            dstPix += 4;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// 3. Effect instance
////////////////////////////////////////////////////////////////////////////////

GainPlugin::GainPlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle)
    , _dstClip(nullptr)
    , _srcClip(nullptr)
    , _gain(nullptr)
    , _gainAlpha(nullptr)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _gain = fetchDoubleParam(kParamGain);
    _gainAlpha = fetchDoubleParam(kParamGainAlpha);

    if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr || _gainAlpha == nullptr) {
        OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
    }
}

void GainPlugin::render(const OFX::RenderArguments& args)
{
    // Reject anything we did not declare support for, loudly. Silently writing
    // the wrong pixel format is the most common source of "the plugin renders
    // black" reports.
    if (_dstClip->getPixelDepth() != OFX::eBitDepthFloat ||
        _dstClip->getPixelComponents() != OFX::ePixelComponentRGBA) {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    GainProcessor processor(*this);
    setupAndProcess(processor, args);
}

bool GainPlugin::isIdentity(const OFX::IsIdentityArguments& args,
                            OFX::Clip*& identityClip,
                            double& identityTime)
{
    const double gain = _gain->getValueAtTime(args.time);
    const double gainAlpha = _gainAlpha->getValueAtTime(args.time);

    if (gain == 1.0 && gainAlpha == 1.0) {
        identityClip = _srcClip;
        identityTime = args.time;
        return true;
    }
    return false;
}

void GainPlugin::setupAndProcess(GainProcessor& processor, const OFX::RenderArguments& args)
{
    // fetchImage() returns an owning pointer: wrap in unique_ptr immediately so
    // every error path below releases the host image.
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

    float gain[4];
    gain[0] = gain[1] = gain[2] = static_cast<float>(_gain->getValueAtTime(args.time));
    gain[3] = static_cast<float>(_gainAlpha->getValueAtTime(args.time));

    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());
    processor.setGain(gain);

    // Must be called before process(): this is what tells ImageProcessor which
    // GPU API (if any) the host enabled for this render, plus the stream.
    processor.setGPURenderArgs(args);
    processor.setRenderWindow(args.renderWindow);

    processor.process();
}

////////////////////////////////////////////////////////////////////////////////
// 4. Factory
////////////////////////////////////////////////////////////////////////////////

GainPluginFactory::GainPluginFactory()
    : OFX::PluginFactoryHelper<GainPluginFactory>(kPluginIdentifier, kVersionMajor, kVersionMinor)
{
}

void GainPluginFactory::describe(OFX::ImageEffectDescriptor& desc)
{
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

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

    // Unsafe means the host serialises render calls across all instances. This
    // is the honest answer for a plugin that shares no per-instance GPU state
    // and lets the CUDA runtime pick the device: raising it without auditing
    // every shared allocation is a data race in the host's address space.
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);

#ifdef OFXPLUGIN_WITH_CUDA
    // Advertising CUDA makes the host pass device pointers. Only do it when a
    // CUDA path is actually compiled in, otherwise every frame renders black.
    desc.setSupportsCudaRender(true);
    desc.setSupportsCudaStream(true);
#endif
}

void GainPluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context)
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

    OFX::GroupParamDescriptor* group = desc.defineGroupParam("gainGroup");
    group->setLabels("Gain", "Gain", "Gain");
    group->setHint("Per-channel multipliers");
    page->addChild(*group);

    OFX::DoubleParamDescriptor* gainParam = desc.defineDoubleParam(kParamGain);
    gainParam->setLabels("Gain", "Gain", "Gain");
    gainParam->setScriptName(kParamGain);
    gainParam->setHint("Multiplies R, G and B");
    gainParam->setDefault(1.0);
    gainParam->setRange(0.0, 8.0);
    gainParam->setDisplayRange(0.0, 4.0);
    gainParam->setIncrement(0.01);
    gainParam->setDoubleType(OFX::eDoubleTypeScale);
    gainParam->setParent(*group);
    page->addChild(*gainParam);

    OFX::DoubleParamDescriptor* alphaParam = desc.defineDoubleParam(kParamGainAlpha);
    alphaParam->setLabels("Alpha Gain", "Alpha", "Alpha Gain");
    alphaParam->setScriptName(kParamGainAlpha);
    alphaParam->setHint("Multiplies the alpha channel independently");
    alphaParam->setDefault(1.0);
    alphaParam->setRange(0.0, 8.0);
    alphaParam->setDisplayRange(0.0, 4.0);
    alphaParam->setIncrement(0.01);
    alphaParam->setDoubleType(OFX::eDoubleTypeScale);
    alphaParam->setParent(*group);
    page->addChild(*alphaParam);
}

OFX::ImageEffect* GainPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context)
{
    (void)context;
    return new GainPlugin(handle);
}

} // namespace mcofx

////////////////////////////////////////////////////////////////////////////////
// 5. Registration
////////////////////////////////////////////////////////////////////////////////
//
// The Support library implements OfxGetNumberOfPlugins/OfxGetPlugin/OfxSetHost
// and asks the plugin which factories exist through this one function.
// The factory must outlive the module, hence the static storage duration.
void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static mcofx::GainPluginFactory factory;
    ids.push_back(&factory);
}
