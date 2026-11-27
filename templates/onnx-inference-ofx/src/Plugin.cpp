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
//
// Scope, so a reader does not expect more than this: a 1x filter on float RGBA.
// The model must return the shape it was handed, so an upscaler or a detection
// head needs its own runner rather than a new .onnx file
// (references/onnx-and-tensorrt.md section 4, items 5 and 6). Depth and alpha conversion
// belongs beside Pack/Unpack, and its rules are the "What the host hands you is
// not float RGBA" subsection there.

#include "Plugin.h"

// kOfxImageEffectPropCPURenderSupported lives in the GPU header, not in
// ofxImageEffect.h where the other 1.5.1 additions are: OFX split it that way
// because the CPU fallback only means something next to a GPU render.
#include "ofxGPURender.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

#include "ofxsLog.h"

#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
// windows.h defines min/max as macros, which breaks every std::max in this file.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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
    "Runs an ONNX model over the frame with ONNX Runtime. The CUDA execution "
    "provider is requested first and a CPU session is the fallback; the provider "
    "label says which one the session got and why not CUDA when it did not. "
    "Reference implementation for tiled, pitch-accurate inference plug-ins.";

const char* const kEnvironmentModelPath = "ONNX_MODEL_PATH";
// The file a bundle can ship beside its binary: <bundle>/Contents/Resources.
const char* const kResourcesModelName = "model.onnx";
const char* const kParamTileSize = "tileSize";
const char* const kParamHalo = "halo";
// Label, not a setting: the plug-in writes it when the session is created.
const char* const kParamProvider = "provider";

/// Resolve writes its own OFX lines to its log but not what a plug-in passes to
/// OFX::Log::print (measured 2026-11-26 on the CUDA template). The provider
/// label is the only place that says whether the session got CUDA or fell back
/// to the CPU, and the host's script API cannot read a parameter, so the label
/// is not observable from a test run. When ONNX_PROVIDER_LOG names a file, each
/// instance appends one line: the label, then the model it was created for.
/// Unset, which is the normal case, nothing is written and no file is created.
void recordProvider(const std::string& label, const char* modelPath)
{
    const char* target = std::getenv("ONNX_PROVIDER_LOG");
    if (target == nullptr || *target == '\0') {
        return;
    }
    std::ofstream log(target, std::ios::app);
    if (log) {
        log << label << " | " << (modelPath == nullptr ? "" : modelPath) << '\n';
    }
}

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
        // OFX hands over a byte pointer (`kOfxImagePropData`); this clip is
        // float32, which is the format the host was told about in describe().
        // cppcheck-suppress invalidPointerCast
        const float* s = reinterpret_cast<const float*>(srcBase + static_cast<long long>(y) * srcPitch);
        // cppcheck-suppress invalidPointerCast
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

// Which directory this module was loaded from, in UTF-8. A bundle that ships
// data puts it one level up, in Contents/Resources; that is how a model can
// travel with the plug-in instead of with a project.
std::string ModuleDirectory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
        return {};
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (written == 0) {
            return {};
        }
        if (written < path.size()) {
            path.resize(written);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    const std::wstring dir = path.substr(0, slash);
    const int needed = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), utf8.data(), needed,
                        nullptr, nullptr);
    return utf8;
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&ModuleDirectory), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    const std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
#endif
}

bool IsRegularFile(const std::string& path)
{
    std::ifstream probe(path, std::ios::binary);
    return probe.good();
}

// Where the model came from, so the log says which of the two sources answered.
struct ModelSource
{
    std::string path;
    std::string origin;
};

// The model path arrives from outside the parameter stack, once per instance: a
// host applies this plug-in through AddEffect and has no way to set a parameter
// (`improvement-backlog.md` row 19 measured that), so the value is the
// environment first, then the copy the bundle ships beside its binary.
ModelSource ResolveModelPath()
{
    if (const char* fromEnvironment = std::getenv(kEnvironmentModelPath)) {
        if (*fromEnvironment != '\0') {
            return {fromEnvironment, kEnvironmentModelPath};
        }
    }
    const std::string dir = ModuleDirectory();
    if (!dir.empty()) {
        const std::string shipped = dir + "/../Resources/" + kResourcesModelName;
        if (IsRegularFile(shipped)) {
            return {shipped, "Contents/Resources"};
        }
    }
    return {};
}

} // namespace

InferencePlugin::InferencePlugin(OfxImageEffectHandle handle)
    : OFX::ImageEffect(handle)
    , _dstClip(nullptr)
    , _srcClip(nullptr)
    , _modelPath()
    , _tileSize(nullptr)
    , _halo(nullptr)
    , _provider(nullptr)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _tileSize = fetchIntParam(kParamTileSize);
    _halo = fetchIntParam(kParamHalo);
    _provider = fetchStringParam(kParamProvider);

    if (_dstClip == nullptr || _srcClip == nullptr ||
        _tileSize == nullptr || _halo == nullptr || _provider == nullptr) {
        OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
    }

    // Read once, at creation: the environment outranks the shipped copy, and an
    // empty answer is the pass-through case the summary reports.
    const ModelSource source = ResolveModelPath();
    _modelPath = source.path;
    if (_modelPath.empty()) {
        OFX::Log::print("%s: no model: set %s or ship Contents/Resources/%s", kPluginName,
                        kEnvironmentModelPath, kResourcesModelName);
    } else {
        OFX::Log::print("%s: model %s (from %s)", kPluginName, _modelPath.c_str(),
                        source.origin.c_str());
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
        // Say which provider this session got. A CPU fallback leaves the
        // picture correct and only changes the clock, so this label is the one
        // place a user can see that CUDA did not load - and the contract test
        // asserts it is set and starts with "CUDA" or "CPU".
        // The provider already ends in its own bracket ("CPU (1 thread)"), so the
        // reason is appended after a dash: "CPU (1 thread) - fallback: ...".
        std::string label = fresh->Provider();
        if (!fresh->ProviderReason().empty()) {
            label += " - fallback: " + fresh->ProviderReason();
        }
        _provider->setValue(label);
        recordProvider(label, path.c_str());
        OFX::Log::print("%s: execution provider %s", kPluginName, label.c_str());
        return fresh;
    } catch (const std::exception& e) {
        OFX::Log::print("%s: cannot load '%s': %s", kPluginName, path.c_str(), e.what());
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        throw; // unreachable; throwSuiteStatusException always throws
    }
}

void InferencePlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                           OFX::RegionOfInterestSetter& rois)
{
    if (_srcClip == nullptr || !_srcClip->isConnected()) {
        return;
    }
    const int halo = std::max(0, static_cast<int>(_halo->getValueAtTime(args.time)));
    if (halo == 0) {
        // Nothing outside the window is read, so the host's own region stands.
        return;
    }
    OfxRectD roi = args.regionOfInterest;
    roi.x1 -= halo;
    roi.y1 -= halo;
    roi.x2 += halo;
    roi.y2 += halo;
    // Never outside the source, and never empty: render() clamps to the same
    // bounds, so the declaration and the pixels it reads agree.
    const OfxRectD bounds = _srcClip->getRegionOfDefinition(args.time);
    roi.x1 = std::max(roi.x1, bounds.x1);
    roi.y1 = std::max(roi.y1, bounds.y1);
    roi.x2 = std::min(roi.x2, bounds.x2);
    roi.y2 = std::min(roi.y2, bounds.y2);
    if (roi.x2 <= roi.x1 || roi.y2 <= roi.y1) {
        return;
    }
    rois.setRegionOfInterest(*_srcClip, roi);
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

    // Image::getPixelAddress() returns nullptr outside the image bounds, and the
    // addressing below is raw arithmetic on (window - bounds). A host may hand
    // over a window the source region of definition does not cover (a crop, a
    // transition, wrong bounds), so validate it once here instead of reading
    // where there is no pixel.
    const OfxRectI& sb = src->getBounds();
    if (args.renderWindow.x1 < sb.x1 || args.renderWindow.y1 < sb.y1 ||
        args.renderWindow.x2 > sb.x2 || args.renderWindow.y2 > sb.y2) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }

    // The same arithmetic writes the destination. A window the destination
    // region of definition does not cover would put pixels outside the buffer
    // the host handed over, and the write itself succeeds, so nothing reports
    // it later.
    const OfxRectI& db = dst->getBounds();
    if (args.renderWindow.x1 < db.x1 || args.renderWindow.y1 < db.y1 ||
        args.renderWindow.x2 > db.x2 || args.renderWindow.y2 > db.y2) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }

    const OfxRectI window = args.renderWindow;
    const int width = window.x2 - window.x1;
    const int height = window.y2 - window.y1;
    if (width <= 0 || height <= 0) {
        return;
    }

    const std::string& path = _modelPath;
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

    const long long srcPitch = src->getRowBytes();
    const long long dstPitch = dst->getRowBytes();
    char* dstBase = static_cast<char*>(dst->getPixelData()) +
                    static_cast<long long>(window.y1 - db.y1) * dstPitch +
                    static_cast<long long>(window.x1 - db.x1) * kPixelBytes;

    const int tileSize = static_cast<int>(_tileSize->getValueAtTime(args.time));
    // setRange(0, 256) is what the UI offers, but a host can hand back a scripted
    // value outside it. A negative halo would make AiCore's Expand() shrink the
    // data rect below the core rect, so clamp before it reaches TiledOptions;
    // AiCore clamps again for callers that do not come through this parameter.
    const int halo = std::max(0, static_cast<int>(_halo->getValueAtTime(args.time)));

    // The context getRegionsOfInterest() asked for, clamped the same way: a host
    // that honours the region hands over these pixels and the window's edge
    // tiles get real neighbours instead of a clamped border; a host that ignores
    // it leaves sb == window and this collapses to the window.
    OfxRectI ctx{window.x1 - halo, window.y1 - halo, window.x2 + halo, window.y2 + halo};
    ctx.x1 = std::max(ctx.x1, sb.x1);
    ctx.y1 = std::max(ctx.y1, sb.y1);
    ctx.x2 = std::min(ctx.x2, sb.x2);
    ctx.y2 = std::min(ctx.y2, sb.y2);
    const bool hasContext =
        ctx.x1 != window.x1 || ctx.y1 != window.y1 || ctx.x2 != window.x2 || ctx.y2 != window.y2;
    const int inW = ctx.x2 - ctx.x1;
    const int inH = ctx.y2 - ctx.y1;
    const char* inBase = static_cast<const char*>(src->getPixelData()) +
                         static_cast<long long>(ctx.y1 - sb.y1) * srcPitch +
                         static_cast<long long>(ctx.x1 - sb.x1) * kPixelBytes;

    try {
        std::vector<float> planar(static_cast<size_t>(channels) * static_cast<size_t>(width) *
                                  static_cast<size_t>(height));
        // With context from the host the model runs over the padded region and
        // only the window is written back: pixels outside the window belong to
        // the host, not to this render.
        std::vector<float> padded;
        float* inferenceOut = planar.data();
        if (hasContext) {
            padded.assign(static_cast<size_t>(channels) * static_cast<size_t>(inW) *
                              static_cast<size_t>(inH), 0.0f);
            inferenceOut = padded.data();
        }
        TiledOptions opts;
        opts.tileSize = tileSize;
        opts.halo = halo;

        if (tileSize <= 0 || (tileSize >= inW && tileSize >= inH)) {
            // Same byte-to-float contract as above: float32 clip, byte pointer.
            // cppcheck-suppress invalidPointerCast
            RunWhole(*runner, reinterpret_cast<const float*>(inBase), channels, inW, inH,
                     srcPitch, inferenceOut);
        } else {
            // cppcheck-suppress invalidPointerCast
            RunTiled(*runner, reinterpret_cast<const float*>(inBase), channels, inW, inH,
                     srcPitch, opts, inferenceOut);
        }
        if (hasContext) {
            const int dx = window.x1 - ctx.x1;
            const int dy = window.y1 - ctx.y1;
            for (int c = 0; c < channels; ++c) {
                for (int y = 0; y < height; ++y) {
                    std::memcpy(planar.data() +
                                    (static_cast<size_t>(c) * static_cast<size_t>(height) +
                                     static_cast<size_t>(y)) * static_cast<size_t>(width),
                                inferenceOut +
                                    (static_cast<size_t>(c) * static_cast<size_t>(inH) +
                                     static_cast<size_t>(y + dy)) * static_cast<size_t>(inW) +
                                    static_cast<size_t>(dx),
                                static_cast<size_t>(width) * sizeof(float));
                }
            }
        }
        // cppcheck-suppress invalidPointerCast
        UnpackRGB(planar.data(), width, height, reinterpret_cast<float*>(dstBase), dstPitch, 0, 0);
    } catch (const std::exception& e) {
        OFX::Log::print("%s: inference failed: %s", kPluginName, e.what());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
}

bool InferencePlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
                                 double& identityTime)
{
    if (_modelPath.empty()) {
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

    // OFX 1.5.1: rendering on the CPU execution provider is a supported path
    // here, and the graph has no spatial parameter to disable, so both answers
    // a host may branch on are "true".
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
        // Resolve 21.2 answers kOfxStatErrMissingHostFeature (4) for this
        // write, not ErrUnknown: the support library maps the first two
        // statuses to PropertyUnknownToHost and everything else to Suite, so
        // catching only the first left describe() throwing and the host never
        // listed the plug-in (measured 2026-11-27). The property's default is
        // already "true"; a status that is not this one is a real failure.
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
        // Resolve 21.2 answers kOfxStatErrMissingHostFeature (4) for this
        // write, not ErrUnknown: the support library maps the first two
        // statuses to PropertyUnknownToHost and everything else to Suite, so
        // catching only the first left describe() throwing and the host never
        // listed the plug-in (measured 2026-11-27). The property's default is
        // already "true"; a status that is not this one is a real failure.
        if (e.status() != kOfxStatErrMissingHostFeature) {
            throw;
        }
    }

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
    // Not a parameter: the model path comes from ONNX_MODEL_PATH or from
    // Contents/Resources/model.onnx beside the binary, read at creation. A host
    // that applies this plug-in through AddEffect cannot set a parameter, and the
    // row this replaces was never measurable because of it.
    group->setHint("Model from $ONNX_MODEL_PATH, else Contents/Resources/model.onnx; "
                   "then inference tiling");
    page->addChild(*group);

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

    // Not a control: a label the plug-in fills in once the session exists. The
    // CUDA provider is requested first and this says whether it was granted,
    // because a silent fallback is invisible until someone times a frame.
    OFX::StringParamDescriptor* provider = desc.defineStringParam(kParamProvider);
    provider->setLabels("Execution provider", "Provider", "Execution provider");
    provider->setScriptName(kParamProvider);
    provider->setHint("Which execution provider the ONNX session was created with. "
                      "CUDA is requested first; anything else carries the reason it was "
                      "not available.");
    provider->setDefault("CPU (no model loaded yet)");
    provider->setStringType(OFX::eStringTypeLabel);
    provider->setParent(*group);
    page->addChild(*provider);
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
