// SPDX-License-Identifier: BSD-3-Clause
//
// Host wrapper for an OpenFX plug-in that runs an ONNX model per frame.
#ifndef AIOFX_PLUGIN_H
#define AIOFX_PLUGIN_H

#include <memory>
#include <mutex>
#include <string>

#include "ofxsImageEffect.h"

namespace aiofx {

class ModelRunner;

/// Runs a model over the render window, in one pass or in tiles.
///
/// This deliberately does NOT derive from OFX::ImageProcessor. That base class
/// exists to slice a frame across the host's CPU threads, which is the right
/// thing for a pixel loop but the wrong thing for a model: every slice would
/// load its own copy of the weights and run its own inference. Here the host's
/// threads drive whole renders of different instances, and ONNX Runtime's own
/// thread pool does the intra-frame parallelism (see ModelRunner's thread
/// budget).
class InferencePlugin : public OFX::ImageEffect
{
public:
    explicit InferencePlugin(OfxImageEffectHandle handle);

    void render(const OFX::RenderArguments& args) override;

    bool isIdentity(const OFX::IsIdentityArguments& args,
                    OFX::Clip*& identityClip,
                    double& identityTime) override;

private:
    /// Loads the session for `path` if it is not the one already loaded.
    /// Throws OFX::Exception on failure, never a bare std::exception.
    ModelRunner& AcquireRunner(const std::string& path);

    OFX::Clip* _dstClip;         // borrowed, never deleted
    OFX::Clip* _srcClip;         // borrowed, never deleted
    OFX::StringParam* _modelPath;
    OFX::IntParam* _tileSize;
    OFX::IntParam* _halo;

    std::mutex _runnerMutex;
    std::unique_ptr<ModelRunner> _runner;
    std::string _loadedPath;
};

class InferencePluginFactory : public OFX::PluginFactoryHelper<InferencePluginFactory>
{
public:
    InferencePluginFactory();

    void describe(OFX::ImageEffectDescriptor& desc) override;
    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) override;
    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override;
};

} // namespace aiofx

#endif // AIOFX_PLUGIN_H
