// SPDX-License-Identifier: BSD-3-Clause
//
// ONNX inference OpenFX image effect.
//
// The render path, in order, and why each step is where it is:
//
//   1. Validate the pixel format the host actually handed over. A plug-in that
//      writes float RGBA into a half image renders black and says nothing.
//   2. Turn (window origin, signed row stride) into a base pointer per image.
//      getPixelData() points at the image bounds, not at the render window.
//   3. Copy alpha through untouched: the model has three channels, and alpha is
//      not its business.
//   4. Run the model, whole-frame or tiled (AiCore).
//   5. Write RGB back through the same stride.
//
// Threading: render() may be called concurrently for different instances, so
// the runner is created under a mutex and then used read-only. ONNX Runtime
// documents that concurrent Run() on one session is safe.

#include "Plugin.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

#include "ofxsLog.h"

#include "AiCore.h"
#include "ModelRunner.h"

namespace aiofx {
namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.onnxinference";
const char* const kPluginName = "ONNX Inference";
const char* const kPluginGrouping = "OFX Plugin Developer";
const char* const kPluginDescription =
    "Runs an ONNX model over the frame with ONNX Runtime (CPU execution provider). "
    "Reference implementation for tiled, pitch-accurate inference plug-ins.";

const char* const kParamModelPath = "modelPath";
const char* const kParamTileSize = "tileSize";
const char* const kParamHalo = "halo";

constexpr std::int64_t kPixelBytes = 4 * sizeof(float); // RGBA float

// Alpha only: the model has three channels and alpha is not its business.
// 16 bytes per pixel is correct because render() rejects every other layout:
// float RGBA is the only format this plug-in accepts.
void CopyAlpha(const OFX::Image* src, OFX::Image* dst, const OfxRectI& window)
{
    const long long srcPitch = src->getRowBytes();
    const long long dstPitch = dst->getRowBytes();
    const OfxRectI& sb = src->getBounds();
    const OfxRectI& db = dst->getBounds();
    const char* srcBase = static_cast<const char*>(src->getPixelData()) +
                          static_cast<long long>(window.y1 - sb.y1) * srcPitch +
                          static_cast<long long>(window.x1 - sb.x1) * kPixelBytes;
    char* dstBase = static_cast<char*>(dst->getPixelData()) +
                    static_cast<long long>(window.y1 - db.y1) * dstPitch +
                    static_cast<long long>(window.x1 - db.x1) * kPixelBytes;

    const int w = window.x2 - window.x1;
    for (int y = 0; y < window.y2 - window.y1; ++y) {
        const float* s = reinterpret_cast<const float*>(srcBase + static_cast<long long>(y) * srcPitch);
        float* d = reinterpret_cast<float*>(dstBase + static_cast<long long>(y) * dstPitch);
        for (int x = 0; x < w; ++x) {
            d[4 * x + 3] = s[4 * x + 3];
        }
    }
}

// Copies the render window, all four channels, through the host's stride.
// Used when there is no model to run: the destination image is its own buffer,
// so copying alpha alone leaves RGB as whatever the host allocated.
void CopyRect(const OFX::Image* src, OFX::Image* dst, const OfxRectI& window)
{
    const long long srcPitch = src->getRowBytes();
    const long long dstPitch = dst->getRowBytes();
    const OfxRectI& sb = src->getBounds();
    const OfxRectI& db = dst->getBounds();
    const char* srcBase = static_cast<const char*>(src->getPixelData()) +
                          static_cast<long long>(window.y1 - sb.y1) * srcPitch +
                          static_cast<long long>(window.x1 - sb.x1) * kPixelBytes;
    char* dstBase = static_cast<char*>(dst->getPixelData()) +
                    static_cast<long long>(window.y1 - db.y1) * dstPitch +
                    static_cast<long long>(window.x1 - db.x1) * kPixelBytes;

    const size_t rowBytes = static_cast<size_t>(window.x2 - window.x1) * kPixelBytes;
    for (int y = 0; y < window.y2 - window.y1; ++y) {
        std::memcpy(dstBase + static_cast<long long>(y) * dstPitch,
                    srcBase + static_cast<long long>(y) * srcPitch, rowBytes);
    }
}

} // namespace

InferencePlugin::InferencePlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle)
    , _dstClip(nullptr)
    , _srcClip(nullptr)
    , _modelPath(nullptr)
    , _tileSize(nullptr)
    , _halo(nullptr)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _modelPath = fetchStringParam(kParamModelPath);
    _tileSize = fetchIntParam(kParamTileSize);
    _halo = fetchIntParam(kParamHalo);

    if (_dstClip == nullptr || _srcClip == nullptr || _modelPath == nullptr ||
        _tileSize == nullptr || _halo == nullptr) {
        OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
    }
}

std::shared_ptr<ModelRunner> InferencePlugin::AcquireRunner(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_runnerMutex);
    if (_runner && _loadedPath == path) {
        return _runner;
    }
    try {
        std::shared_ptr<ModelRunner> fresh = std::make_shared<ModelRunner>(path, 1);
        OFX::Log::print("%s: loaded %s (%d input channels)", kPluginName, path.c_str(),
                        fresh->InputChannels());
        _runner = fresh;
        _loadedPath = path;
        return fresh;
    } catch (const std::exception& e) {
        OFX::Log::print("%s: cannot load '%s': %s", kPluginName, path.c_str(), e.what());
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        throw; // unreachable; throwSuiteStatusException always throws
    }
}

void InferencePlugin::render(const OFX::RenderArguments& args)
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

    if (src->getPixelDepth() != OFX::eBitDepthFloat ||
        src->getPixelComponents() != OFX::ePixelComponentRGBA) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }

    const OfxRectI window = args.renderWindow;
    const int width = window.x2 - window.x1;
    const int height = window.y2 - window.y1;
    if (width <= 0 || height <= 0) {
        return;
    }

    std::string path;
    _modelPath->getValueAtTime(args.time, path);
    if (path.empty()) {
        // isIdentity() normally keeps us out of here, but a host may render
        // anyway (identity switched off, a preview pass, a host that ignores
        // the return value). Copy the frame: only alpha would leave RGB
        // undefined, and the destination is not a view of the source.
        CopyRect(src.get(), dst.get(), window);
        OFX::Log::print("%s: no model path set, passing the source through", kPluginName);
        return;
    }

    CopyAlpha(src.get(), dst.get(), window);

    std::shared_ptr<ModelRunner> runner = AcquireRunner(path);
    const int channels = runner->InputChannels();
    if (channels != 3) {
        OFX::Log::print("%s: model has %d channels; this plug-in feeds RGB only", kPluginName,
                        channels);
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    const OfxRectI& sb = src->getBounds();
    const OfxRectI& db = dst->getBounds();
    const long long srcPitch = src->getRowBytes();
    const long long dstPitch = dst->getRowBytes();
    const char* srcBase = static_cast<const char*>(src->getPixelData()) +
                          static_cast<long long>(window.y1 - sb.y1) * srcPitch +
                          static_cast<long long>(window.x1 - sb.x1) * kPixelBytes;
    char* dstBase = static_cast<char*>(dst->getPixelData()) +
                    static_cast<long long>(window.y1 - db.y1) * dstPitch +
                    static_cast<long long>(window.x1 - db.x1) * kPixelBytes;

    const int tileSize = static_cast<int>(_tileSize->getValueAtTime(args.time));
    const int halo = static_cast<int>(_halo->getValueAtTime(args.time));

    try {
        std::vector<float> planar(static_cast<size_t>(channels) * static_cast<size_t>(width) *
                                  static_cast<size_t>(height));
        TiledOptions opts;
        opts.tileSize = tileSize;
        opts.halo = halo;

        if (tileSize <= 0 || (tileSize >= width && tileSize >= height)) {
            RunWhole(*runner, reinterpret_cast<const float*>(srcBase), channels, width, height,
                     srcPitch, planar.data());
        } else {
            RunTiled(*runner, reinterpret_cast<const float*>(srcBase), channels, width, height,
                     srcPitch, opts, planar.data());
        }
        UnpackRGB(planar.data(), width, height, reinterpret_cast<float*>(dstBase), dstPitch, 0, 0);
    } catch (const std::exception& e) {
        OFX::Log::print("%s: inference failed: %s", kPluginName, e.what());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
}

bool InferencePlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                                 double& identityTime)
{
    std::string path;
    _modelPath->getValueAtTime(args.time, path);
    if (path.empty()) {
        identityClip = _srcClip;
        identityTime = args.time;
        return true;
    }
    return false;
}

InferencePluginFactory::InferencePluginFactory()
    : OFX::PluginFactoryHelper<InferencePluginFactory>(kPluginIdentifier, kVersionMajor, kVersionMinor)
{
}

void InferencePluginFactory::describe(OFX::ImageEffectDescriptor& desc)
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
    // Tiling is an internal implementation detail here, not the OFX tile
    // protocol: we ask the host for the full render window and cut it up
    // ourselves. Advertising setSupportsTiles(true) would mean honouring
    // kOfxImageEffectPropTilesRoD and neighbours, which is a different feature.
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setSupportsMultipleClipDepths(false);

    // One render per instance at a time; several instances may render at once.
    // The session is immutable after loading and Run() is documented as safe to
    // call concurrently, so this is the honest - and the strongest claim that
    // does not require per-render state.
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);

    // No CUDA capability is advertised: the template links the CPU execution
    // provider. Advertising it would make the host hand over device pointers.
    desc.setSupportsCudaRender(false);
}

void InferencePluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc,
                                               OFX::ContextEnum context)
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

    OFX::GroupParamDescriptor* group = desc.defineGroupParam("modelGroup");
    group->setLabels("Model", "Model", "Model");
    group->setHint("ONNX model file and inference tiling");
    page->addChild(*group);

    OFX::StringParamDescriptor* path = desc.defineStringParam(kParamModelPath);
    path->setLabels("Model (.onnx)", "Model", "Model (.onnx)");
    path->setScriptName(kParamModelPath);
    path->setHint("Absolute path to an ONNX model: 1 input, 1 output, NCHW float32, 3 channels. "
                  "Empty means pass the source through.");
    path->setDefault("");
    path->setStringType(OFX::eStringTypeFilePath);
    path->setParent(*group);
    page->addChild(*path);

    OFX::IntParamDescriptor* tile = desc.defineIntParam(kParamTileSize);
    tile->setLabels("Tile Size", "Tile", "Tile Size");
    tile->setScriptName(kParamTileSize);
    tile->setHint("Core tile edge in pixels. 0 runs the whole frame in one pass.");
    tile->setDefault(256);
    tile->setRange(0, 8192);
    tile->setDisplayRange(0, 1024);
    tile->setParent(*group);
    page->addChild(*tile);

    OFX::IntParamDescriptor* halo = desc.defineIntParam(kParamHalo);
    halo->setLabels("Tile Halo", "Halo", "Tile Halo");
    halo->setScriptName(kParamHalo);
    halo->setHint("Context pixels added around each tile. Must be at least the model's "
                  "receptive field radius, or tile seams are wrong.");
    halo->setDefault(8);
    halo->setRange(0, 256);
    halo->setDisplayRange(0, 64);
    halo->setParent(*group);
    page->addChild(*halo);
}

OFX::ImageEffect* InferencePluginFactory::createInstance(OfxImageEffectHandle handle,
                                                         OFX::ContextEnum context)
{
    (void)context;
    return new InferencePlugin(handle);
}

} // namespace aiofx

////////////////////////////////////////////////////////////////////////////////
// Registration: the Support library generates OfxGetNumberOfPlugins/OfxGetPlugin
// from this one hook, which is why the module exports exactly two symbols.
void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static aiofx::InferencePluginFactory factory;
    ids.push_back(&factory);
}
