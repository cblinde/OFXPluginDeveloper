// SPDX-License-Identifier: BSD-3-Clause
//
// Host wrapper for a minimal, production-shaped CUDA OpenFX plugin.
#ifndef MCOFX_PLUGIN_H
#define MCOFX_PLUGIN_H

#include <string>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

namespace mcofx {

/// Applies a per-channel gain.
///
/// Deriving from OFX::ImageProcessor buys three things that are easy to get
/// wrong by hand: tiling-safe window handling, per-frame CPU thread slicing via
/// OfxMultiThreadSuite, and automatic GPU/CPU dispatch driven by the host's
/// kOfxImageEffectPropCudaEnabled flag (surfaced as RenderArguments::
/// isEnabledCudaRender by the Support library).
class GainProcessor : public OFX::ImageProcessor
{
public:
    explicit GainProcessor(OFX::ImageEffect& effect);

    void setSrcImg(OFX::Image* src);
    void setGain(const float gain[4]);
    /// The depth of the images this render receives. Read from the fetched
    /// destination image, not from a parameter, so a host that changes the clip
    /// format between renders cannot leave a stale value behind.
    void setDepth(OFX::BitDepthEnum depth) { _depth = depth; }

    /// CUDA path: zero-copy on the host's device buffers.
    void processImagesCuda() override;

    /// CPU path: scalar fallback, also used when the host does not enable CUDA.
    void multiThreadProcessImages(OfxRectI window) override;

private:
    OFX::Image* _src;   // borrowed, released by the caller
    float _gain[4];
    OFX::BitDepthEnum _depth = OFX::eBitDepthFloat;
};

class GainPlugin : public OFX::ImageEffect
{
public:
    explicit GainPlugin(OfxImageEffectHandle handle);

    void render(const OFX::RenderArguments& args) override;

    bool isIdentity(const OFX::IsIdentityArguments& args,
                    OFX::Clip*& identityClip,
                    double& identityTime) override;

    /// The Device label: which path this render took and why, plus an override
    /// word the parser rejected. Public because the processor holds the instance
    /// as OFX::ImageEffect& and is the only code that knows the decision.
    void setDeviceLabel(const std::string& text);
    /// The same, only when this render has not written a label yet: the plain CPU
    /// path never reaches the gates (the Support library calls the CPU loop
    /// directly when the host did not enable a CUDA render), and a tiled host
    /// calls that loop once per window.
    void setDeviceLabelOnce(const std::string& text);

private:
    void setupAndProcess(GainProcessor& processor, const OFX::RenderArguments& args);

    // Borrowed pointers owned by the Support library, never deleted here.
    OFX::Clip* _dstClip;
    OFX::Clip* _srcClip;
    OFX::DoubleParam* _gain;
    OFX::DoubleParam* _gainAlpha;
    OFX::StringParam* _device;
    std::string _label;
    bool _labelWritten = false;
};

class GainPluginFactory : public OFX::PluginFactoryHelper<GainPluginFactory>
{
public:
    GainPluginFactory();

    void describe(OFX::ImageEffectDescriptor& desc) override;
    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) override;
    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override;
};

} // namespace mcofx

#endif // MCOFX_PLUGIN_H
