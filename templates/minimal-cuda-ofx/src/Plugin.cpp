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

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "ofxsLog.h"
// ofxGPURender.h is NOT pulled in by ofxsImageEffect.h. It is the header that
// defines kOfxImageEffectPropCudaStream and the GPU status codes
// (kOfxStatGPURenderFailed, kOfxStatGPUOutOfMemory).
#include "ofxGPURender.h"

#include "ExecutionMode.h"
#include "Kernel.cuh"

namespace mcofx {
namespace {

constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;

// A default of 1.0 makes isIdentity() true, and a host is then entitled to skip
// `render()` - so a host-side proof of the pixel path cannot use it. The
// Resolve scripting API cannot set an OFX parameter (references/resolve-
// automation.md §15.5), so the harness builds the value in instead:
//   cmake -S . -B build -DGAIN_DEFAULT=2.0
#ifndef GAIN_DEFAULT
#define GAIN_DEFAULT 1.0
#endif
constexpr double kDefaultGain = GAIN_DEFAULT;

const char* const kPluginIdentifier = "com.ofxplugindeveloper.minimalcudagain";
const char* const kPluginName = "Minimal CUDA Gain";
const char* const kPluginGrouping = "OFX Plugin Developer";
const char* const kPluginDescription =
    "Per-channel gain with a zero-copy CUDA render path and a CPU fallback. "
    "Reference implementation for pitch-accurate OFX GPU plugins.";

const char* const kParamGain = "gain";
const char* const kParamGainAlpha = "gainAlpha";
const char* const kParamDevice = "device";

/// Records which pixel path a render took, for a harness that cannot ask the host.
///
/// Pixels cannot tell the two paths apart - the CPU loop and the kernel apply the
/// same four multiplies, which is the point of having both - and Resolve writes
/// its own OFX lines to its log but not what a plug-in passes to
/// OFX::Log::print (measured 2026-11-26: the host log names the bundle at load
/// time and nothing the plug-in printed). So when MCOFX_PATH_LOG names a file,
/// each render appends one line to it. Unset, which is the normal case, nothing
/// is written and no file is created.
// OFXPLUGIN_GPU_MODE=auto|cpu|cuda|hybrid. Read once per process: an env var
// that changes mid-render would make the declaration line describe a different
// render than the pixels. An unknown word is reported, not silently treated as
// auto - a typo'd override is how a run gets reported as hybrid while it never
// left CUDA_ONLY.
// The facts the gates read, gathered once per render. inputOnDevice is true by
// construction: kOfxImageEffectPropCudaEnabled is the host saying the image
// pointers are device pointers, so this path never copies a frame.
FrameFacts RenderFacts(int width, int height, std::int64_t bytesPerPixel)
{
    FrameFacts facts;
    facts.hostHandedDevicePointers = true; // processImagesCuda() only runs then
    facts.inputOnDevice = true;
    facts.frameBytes = static_cast<std::int64_t>(width) * height * bytesPerPixel;
    facts.hasIndependentCpuTask = false; // a gain has nothing else to compute
    std::int64_t freeBytes = 0;
    std::int64_t totalBytes = 0;
    facts.deviceQueryable = DeviceMemoryFreeBytes(&freeBytes, &totalBytes);
    facts.deviceFreeBytes = freeBytes;
    return facts;
}

ExecutionMode RequestedMode()
{
    static const ExecutionMode requested = [] {
        const char* text = std::getenv("OFXPLUGIN_GPU_MODE");
        bool recognised = true;
        const ExecutionMode mode = ParseModeOverride(text, &recognised);
        if (!recognised && text != nullptr) {
            OFX::Log::print("Minimal CUDA Gain: OFXPLUGIN_GPU_MODE='%s' is not "
                            "auto|cpu|cuda|hybrid; using auto",
                            text);
        }
        return mode;
    }();
    return requested;
}

/// "" or the sentence a label appends when the override was not understood.
///
/// The log line below reports the same thing, and the log is not a channel a host
/// keeps: Resolve writes its own OFX lines and none of what the plug-in prints
/// (measured 2026-11-26). A typo therefore reaches the Device parameter, which is
/// the one place a user or a harness can read it back.
std::string overrideProblem()
{
    const char* text = std::getenv("OFXPLUGIN_GPU_MODE");
    if (text == nullptr || *text == '\0') {
        return std::string();
    }
    bool recognised = true;
    ParseModeOverride(text, &recognised);
    if (recognised) {
        return std::string();
    }
    return std::string("OFXPLUGIN_GPU_MODE='") + text + "' is not auto|cpu|cuda|hybrid";
}

/// Any label text with the override sentence appended when the word was not
/// understood. One place, because the label has two authors - the gates in the
/// device path and the CPU loop the Support library calls directly - and a second
/// copy is how one of them ends up without it (this happened: the plain CPU path
/// wrote its reason and no sentence, and the contract check caught it).
std::string withOverride(std::string text)
{
    const std::string problem = overrideProblem();
    if (!problem.empty()) {
        text += "; ";
        text += problem;
    }
    return text;
}

/// The Device label for a decision: the mode, the reason, and the override when
/// it was not understood.
std::string pathLabel(const ModeDecision& decision)
{
    return withOverride(std::string(ModeName(decision.mode)) + " - " + decision.reason);
}

/// One line per render into $MCOFX_PATH_LOG, so a host run can be read back
/// afterwards. Pixels cannot answer two questions a host run is asked: which
/// path produced the frame, and whether the host supplied
/// kOfxImageEffectPropCudaStream. OFX::Log::print does not survive Resolve's own
/// log (measured 2026-11-26), and a claim about a stream nobody can see is an
/// assumption - so the stream is recorded here, per render, with its address or
/// the word "none".
void recordPath(const char* path, int width, int height, const void* stream, bool cudaPath)
{
    const char* target = std::getenv("MCOFX_PATH_LOG");
    if (target == nullptr || *target == '\0') {
        return;
    }
    std::ofstream log(target, std::ios::app);
    if (!log) {
        return;
    }
    log << path << ' ' << width << 'x' << height << " stream=";
    if (!cudaPath) {
        log << "n/a";  // the CPU path has no CUDA stream to ask about
    } else if (stream == nullptr) {
        log << "none";  // kOfxImageEffectPropCudaStream was not supplied
    } else {
        char address[32] = {0};
        std::snprintf(address, sizeof address, "%p", stream);
        log << address;
    }
    log << '\n';
}

/// Bytes per pixel for each depth the plug-in declares. Every pointer offset in
/// this file is expressed in terms of the depth's value, so a mixed-depth mistake
/// is a named constant away rather than a bare 16.
constexpr std::int64_t kPixelBytesRGBA32F = 4 * sizeof(float);
constexpr std::int64_t kPixelBytesRGBA16F = 4 * sizeof(std::uint16_t);
constexpr std::int64_t kPixelBytesRGBA8 = 4 * sizeof(std::uint8_t);

std::int64_t pixelBytes(OFX::BitDepthEnum depth)
{
    switch (depth) {
    case OFX::eBitDepthFloat:
        return kPixelBytesRGBA32F;
    case OFX::eBitDepthHalf:
        return kPixelBytesRGBA16F;
    case OFX::eBitDepthUByte:
        return kPixelBytesRGBA8;
    default:
        return 0;
    }
}

/// IEEE 754 binary16 -> binary32. Exact: every half is a float.
float halfToFloat(std::uint16_t bits)
{
    const std::uint32_t sign = (bits & 0x8000u) ? 0x80000000u : 0u;
    const std::uint32_t exponent = (bits >> 10) & 0x1fu;
    std::uint32_t mantissa = bits & 0x3ffu;
    std::uint32_t out = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            out = sign;
        } else {
            // Subnormal half: renormalise into a normal float.
            std::uint32_t e = 113u;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --e;
            }
            out = sign | (e << 23) | ((mantissa & 0x3ffu) << 13);
        }
    } else if (exponent == 0x1fu) {
        out = sign | 0x7f800000u | (mantissa << 13);
    } else {
        out = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

/// binary32 -> binary16 with round-to-nearest-even, the mode of every CUDA
/// half conversion and of __float2int_rn in the byte kernel. The device path
/// converts through float as well (ScaleHalf2), so this keeps the CPU fallback
/// and the kernel agreeing bit for bit on a half image.
std::uint16_t floatToHalf(float value)
{
    std::uint32_t f = 0;
    std::memcpy(&f, &value, sizeof(f));
    const std::uint32_t sign = (f >> 16) & 0x8000u;
    f &= 0x7fffffffu;
    if (f >= 0x47800000u) {
        // Overflow, infinity or NaN. 0x7e00 is the canonical quiet NaN.
        if (f > 0x7f800000u) return static_cast<std::uint16_t>(sign | 0x7e00u);
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (f < 0x38800000u) {
        // Result is a half subnormal (or zero): the unit is 2^-24, so the
        // mantissa is shifted right by (126 - biasedExponent) bits.
        const std::uint32_t shift = 126u - (f >> 23);
        if (shift > 25u) return static_cast<std::uint16_t>(sign);
        std::uint32_t mantissa = (f & 0x7fffffu) | 0x800000u;
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | ((f >> 13) - (112u << 10));
    const std::uint32_t rest = f & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
    return static_cast<std::uint16_t>(half);
}

/// The byte kernel's quantisation, on the host: round to nearest even, then
/// saturate at 255 rather than wrapping. Same rule as ScaleByte.
unsigned char quantiseByte(float value)
{
    const float rounded = std::nearbyint(value);
    if (!(rounded > 0.0f)) return 0;  // also catches NaN
    if (rounded > 255.0f) return 255;
    return static_cast<unsigned char>(rounded);
}

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
    // render() rejects an unconnected source before it gets here, so _src is
    // never null. Checking for it and returning would leave the destination
    // unwritten - the failure mode the silent-image bugs are made of.
    if (width <= 0 || height <= 0 || _dstImg == nullptr) {
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

    // The column offset is in pixels of this render's depth, so it comes from the
    // same place the kernel choice does. A depth without a pixel size has no
    // path here - render() already refused it, this is the belt to that brace.
    const std::int64_t bytesPerPixel = pixelBytes(_depth);
    if (bytesPerPixel == 0) {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    // Execution-mode gates (src/ExecutionMode.h, references/cuda-streams-and-
    // memory.md §4). This effect has no analysis pass, so no independent CPU work
    // can overlap the kernel and auto never chooses CPU_CUDA_HYBRID here; the
    // gates still matter, because a device without room for two frames is a
    // render that would fail rather than the CPU fallback the user can see.
    const ModeDecision decision = DecideMode(RequestedMode(), RenderFacts(
        width, height, static_cast<std::int64_t>(bytesPerPixel)));
    if (decision.mode == ExecutionMode::CpuOnly) {
        OFX::Log::print("Minimal CUDA Gain: CPU_ONLY (%s)", decision.reason);
        static_cast<GainPlugin&>(_effect).setDeviceLabel(pathLabel(decision));
        multiThreadProcessImages(_renderWindow);
        return;
    }

    const char* srcOrigin = static_cast<const char*>(_src->getPixelData())
                          + static_cast<std::int64_t>(window.y1 - srcBounds.y1) * srcPitch
                          + static_cast<std::int64_t>(window.x1 - srcBounds.x1) * bytesPerPixel;
    char* dstOrigin = static_cast<char*>(_dstImg->getPixelData())
                    + static_cast<std::int64_t>(window.y1 - dstBounds.y1) * dstPitch
                    + static_cast<std::int64_t>(window.x1 - dstBounds.x1) * bytesPerPixel;

    // One entry point per depth, chosen from the image the host handed over.
    // 32F has a scalar fallback inside its launcher (a float4 access needs
    // 16-byte addressability); the other two have one path each.
    int status = kLaunchOK;
    switch (_depth) {
    case OFX::eBitDepthFloat:
        status = LaunchGainRGBA32F(_pCudaStream, srcOrigin, dstOrigin, width, height,
                                   srcPitch, dstPitch, _gain);
        break;
    case OFX::eBitDepthHalf:
        status = LaunchGainRGBA16F(_pCudaStream, srcOrigin, dstOrigin, width, height,
                                   srcPitch, dstPitch, _gain);
        break;
    case OFX::eBitDepthUByte:
        status = LaunchGainRGBA8(_pCudaStream, srcOrigin, dstOrigin, width, height,
                                 srcPitch, dstPitch, _gain);
        break;
    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
    if (status != kLaunchOK) {
        OFX::Log::print("Minimal CUDA Gain: kernel launch failed: %s", StatusMessage(status));
        OFX::throwSuiteStatusException(kOfxStatGPURenderFailed);
    }

    // One line per render, so the host's log says which path produced the frame.
    // Pixels alone cannot: the CPU loop and the kernel apply the same multiplies,
    // which is the point of having both. This is the declaration §8 of
    // references/resolve-automation.md asks for, and the only proof a test can
    // read back when it cannot see the host's GPU preference.
    const char* depthName = _depth == OFX::eBitDepthHalf     ? "RGBA16F"
                            : _depth == OFX::eBitDepthUByte ? "RGBA8"
                                                            : "RGBA32F";
    OFX::Log::print("Minimal CUDA Gain: %s, %s %dx%d, stream %s (%s)",
                    ModeName(decision.mode), depthName, width, height,
                    _pCudaStream == nullptr ? "not supplied by the host" : "supplied by the host",
                    decision.reason);
    recordPath("CUDA", width, height, _pCudaStream, true);
    static_cast<GainPlugin&>(_effect).setDeviceLabel(pathLabel(decision));

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
    // The CPU half of the same declaration: a run whose declaration file has
    // neither line rendered pixels somewhere this code does not know about. No
    // OFX::Log::print here - a tiled or multithreaded host would call this once
    // per window, and the file is the channel the harness can read.
    recordPath("CPU", window.x2 - window.x1, window.y2 - window.y1, nullptr, false);
    static_cast<GainPlugin&>(_effect).setDeviceLabelOnce(withOverride(
        "CPU_ONLY - the host did not set kOfxImageEffectPropCudaEnabled, so the pixels are host pointers"));

    for (int y = window.y1; y < window.y2; ++y) {
        if (_effect.abort()) {
            break;
        }

        // Destination is guaranteed to be inside its bounds here:
        // ImageProcessor::process() validates the render window against them.
        //
        // One call per ROW, not per pixel: OFX::Image::getPixelAddress is
        // declared in the header but defined out of line in the Support library,
        // so calling it inside the x loop pays a function call per pixel. The
        // pixel arithmetic below is unchanged - only the addressing is hoisted.
        //
        // The loop is chosen by depth, and each one applies the same rule its
        // kernel does: float arithmetic, half through float with one rounding,
        // byte on the stored code value with saturation. That is what lets the
        // contract harness expect the same frame from a host with a GPU and one
        // without.
        switch (_depth) {
        case OFX::eBitDepthFloat: {
            float* dstRow = static_cast<float*>(_dstImg->getPixelAddress(window.x1, y));
            if (dstRow == nullptr) {
                continue;
            }
            // render() rejects an unconnected source, so _src is always set here.
            const float* srcRow = static_cast<const float*>(_src->getPixelAddress(window.x1, y));
            for (int x = window.x1; x < window.x2; ++x, dstRow += 4, srcRow += 4) {
                dstRow[0] = srcRow[0] * _gain[0];
                dstRow[1] = srcRow[1] * _gain[1];
                dstRow[2] = srcRow[2] * _gain[2];
                dstRow[3] = srcRow[3] * _gain[3];
            }
            break;
        }
        case OFX::eBitDepthHalf: {
            std::uint16_t* dstRow =
                static_cast<std::uint16_t*>(_dstImg->getPixelAddress(window.x1, y));
            if (dstRow == nullptr) {
                continue;
            }
            const std::uint16_t* srcRow =
                static_cast<const std::uint16_t*>(_src->getPixelAddress(window.x1, y));
            for (int x = window.x1; x < window.x2; ++x, dstRow += 4, srcRow += 4) {
                for (int c = 0; c < 4; ++c) {
                    dstRow[c] = floatToHalf(halfToFloat(srcRow[c]) * _gain[c]);
                }
            }
            break;
        }
        case OFX::eBitDepthUByte: {
            unsigned char* dstRow =
                static_cast<unsigned char*>(_dstImg->getPixelAddress(window.x1, y));
            if (dstRow == nullptr) {
                continue;
            }
            const unsigned char* srcRow =
                static_cast<const unsigned char*>(_src->getPixelAddress(window.x1, y));
            for (int x = window.x1; x < window.x2; ++x, dstRow += 4, srcRow += 4) {
                for (int c = 0; c < 4; ++c) {
                    dstRow[c] = quantiseByte(static_cast<float>(srcRow[c]) * _gain[c]);
                }
            }
            break;
        }
        default:
            // render() refused this depth before any pixel was touched.
            break;
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
    , _device(nullptr)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _gain = fetchDoubleParam(kParamGain);
    _gainAlpha = fetchDoubleParam(kParamGainAlpha);
    _device = fetchStringParam(kParamDevice);

    if (_dstClip == nullptr || _srcClip == nullptr || _gain == nullptr || _gainAlpha == nullptr ||
        _device == nullptr) {
        OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);
    }
}

void GainPlugin::render(const OFX::RenderArguments& args)
{
    // Reject anything we did not declare support for, loudly. Silently writing
    // the wrong pixel format is the most common source of "the plugin renders
    // black" reports. The three depths below are the ones describe() advertises;
    // eBitDepthUShort is the one OFX depth they do not cover, and it is refused
    // here rather than misinterpreted as bytes.
    if (_dstClip->getPixelComponents() != OFX::ePixelComponentRGBA ||
        pixelBytes(_dstClip->getPixelDepth()) == 0) {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    _labelWritten = false;
    GainProcessor processor(*this);
    setupAndProcess(processor, args);
}

/// A host may refuse a write to a read-only parameter during create; the label is
/// a diagnostic, not a contract, and a refused write is remembered so the next
/// render retries.
void GainPlugin::setDeviceLabel(const std::string& text)
{
    _labelWritten = true;
    if (text == _label) {
        return;
    }
    try {
        _device->setValue(text);
        _label = text;
    } catch (const OFX::Exception::Suite&) {
    }
}

void GainPlugin::setDeviceLabelOnce(const std::string& text)
{
    if (_labelWritten) {
        return;
    }
    setDeviceLabel(text);
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

    // Image::getPixelAddress() returns nullptr outside the image bounds, and a
    // host may hand over a render window the image does not cover (a crop, a
    // transition, wrong bounds). Both the CPU loop and the CUDA kernel would then
    // read or write where the plug-in has no pixel. ImageProcessor::process()
    // does compare the window with the destination bounds, but it answers with a
    // bare return and its assert is compiled out of a release build: the host
    // would get kOfxStatOK and an untouched frame. The branch is
    // .ofx-ref/Support/include/ofxsProcessing.h:150-157 - assert first, then
    // `return` before preProcess(), so no status leaves the process. Refuse both
    // windows here, where the status is ours to choose.
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

    float gain[4];
    gain[0] = gain[1] = gain[2] = static_cast<float>(_gain->getValueAtTime(args.time));
    gain[3] = static_cast<float>(_gainAlpha->getValueAtTime(args.time));

    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());
    processor.setGain(gain);
    processor.setDepth(dst->getPixelDepth());

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

    // The three depths the kernels cover. Half and byte are not decoration: each
    // has its own device kernel and its own CPU loop, and a host that hands over
    // such an image gets that path or a refusal, never a guess.
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.addSupportedBitDepth(OFX::eBitDepthHalf);
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setSupportsMultipleClipDepths(false);

    // eRenderInstanceSafe means: one render per instance at a time, but several
    // instances may render concurrently on different threads (verified against
    // RenderSafetyEnum in ofxsImageEffect.h). That is the honest answer here -
    // a render touches only its own images, params are read through
    // getValueAtTime(), and the one piece of cross-render state is the calling
    // thread's current CUDA device, which DeviceGuard saves and restores.
    // eRenderFullySafe would additionally promise concurrent renders of the SAME
    // instance, which would require every allocation to be keyed per render.
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);

    // OFX 1.5.1: the CPU fallback is a supported path, not a debugging mode, so
    // a host may fall back to it; and there is no spatial parameter to disable
    // when a host wants a spatial-free render, so both answers are "true".
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
    gainParam->setDefault(kDefaultGain);
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

    // Which path the last render took, or why the CPU loop ran instead. Read-only,
    // and the only channel a host keeps: OFX::Log::print does not survive Resolve's
    // own log, so an unrecognised OFXPLUGIN_GPU_MODE would otherwise be reported to
    // nobody (references/cuda-streams-and-memory.md section 4).
    OFX::StringParamDescriptor* device = desc.defineStringParam(kParamDevice);
    device->setLabels("Device", "Device", "Device");
    device->setScriptName(kParamDevice);
    device->setHint("Which CUDA path rendered, or why the CPU loop ran instead");
    device->setStringType(OFX::eStringTypeLabel);
    device->setDefault("");
    device->setEnabled(false);
    page->addChild(*device);
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
