// SPDX-License-Identifier: BSD-3-Clause

#include "ModelRunner.h"

#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <onnxruntime_cxx_api.h>

namespace aiofx {
namespace {

/// One process-wide environment. Ort::Env owns the logging and thread-pool
/// machinery; creating one per session would leak state across session
/// lifetimes and, on repeated instance creation, grow the process.
Ort::Env& SharedEnv()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ofx-onnx-inference");
    return env;
}

std::string CopyName(const char* name)
{
    return name != nullptr ? std::string(name) : std::string();
}

#ifdef _WIN32
/// ORT's Windows entry point takes UTF-16; the OFX string parameter is UTF-8.
/// Widening byte by byte - std::wstring(path.begin(), path.end()) - corrupts
/// every non-ASCII name, so the model at C:\Users\Müller\model.onnx is looked
/// up under mojibake and Session() fails with nothing pointing at the path.
std::wstring WidePath(const std::string& utf8)
{
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), length,
                                          nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("ModelRunner: model path is not valid UTF-8");
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), length, wide.data(),
                            needed) != needed) {
        throw std::runtime_error("ModelRunner: cannot convert the model path to UTF-16");
    }
    return wide;
}
#endif

} // namespace

struct ModelRunner::Impl {
    Ort::Session session{ nullptr };
    std::string inputName;
    std::string outputName;
    int channels = 0;
    std::vector<const char*> inputNames;
    std::vector<const char*> outputNames;
};

ModelRunner::ModelRunner(const std::string& modelPath, int intraThreads)
    : _impl(new Impl())
{
    if (modelPath.empty()) {
        throw std::runtime_error("ModelRunner: empty model path");
    }

    Ort::SessionOptions options;
    // See the header: the host already runs render() on several threads.
    const int threads = intraThreads > 0 ? intraThreads : 1;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ORT_SEQUENTIAL);
    // Full graph optimisation is the default, but state it: a plugin built with
    // ORT_ENABLE_BASIC can be several times slower with no visible difference.
    options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    // CPU EP is the only provider this template links. If the ORT build has
    // others (a -gpu package), they are NOT enabled implicitly: a plug-in must
    // opt in deliberately, see AI_MODEL_INTEGRATION.md.
    //
    // Memory patterns stay on (the ORT default). They cache the per-shape
    // allocations a session reuses on every Run(), which is exactly this
    // plug-in's access pattern: one shape, once per frame.

#ifdef _WIN32
    const std::wstring wide = WidePath(modelPath);
    _impl->session = Ort::Session(SharedEnv(), wide.c_str(), options);
#else
    _impl->session = Ort::Session(SharedEnv(), modelPath.c_str(), options);
#endif

    Ort::AllocatorWithDefaultOptions allocator;
    const size_t inputCount = _impl->session.GetInputCount();
    const size_t outputCount = _impl->session.GetOutputCount();
    if (inputCount != 1 || outputCount != 1) {
        throw std::runtime_error("ModelRunner: expected exactly one input and one output, got " +
                                 std::to_string(inputCount) + "/" + std::to_string(outputCount));
    }

    _impl->inputName = CopyName(_impl->session.GetInputNameAllocated(0, allocator).get());
    _impl->outputName = CopyName(_impl->session.GetOutputNameAllocated(0, allocator).get());

    const auto inShape = _impl->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (inShape.size() != 4) {
        throw std::runtime_error("ModelRunner: input must be 4-D NCHW, got rank " +
                                 std::to_string(inShape.size()));
    }
    if (inShape[0] != 1 && inShape[0] != -1) {
        throw std::runtime_error("ModelRunner: batch dimension must be 1 or dynamic");
    }
    if (inShape[1] <= 0) {
        throw std::runtime_error("ModelRunner: channel dimension must be static");
    }
    _impl->channels = static_cast<int>(inShape[1]);

    _impl->inputNames = { _impl->inputName.c_str() };
    _impl->outputNames = { _impl->outputName.c_str() };
}

ModelRunner::~ModelRunner() = default;

int ModelRunner::InputChannels() const
{
    return _impl->channels;
}

const std::string& ModelRunner::InputName() const
{
    return _impl->inputName;
}

const std::string& ModelRunner::OutputName() const
{
    return _impl->outputName;
}

void ModelRunner::Run(const float* input, int channels, int height, int width, float* output)
{
    if (channels != _impl->channels) {
        throw std::runtime_error("ModelRunner: model wants " + std::to_string(_impl->channels) +
                                 " channels, got " + std::to_string(channels));
    }
    if (input == nullptr || output == nullptr || height <= 0 || width <= 0) {
        throw std::runtime_error("ModelRunner: null buffer or empty image");
    }

    const int64_t shape[4] = { 1, channels, height, width };
    const size_t elements = static_cast<size_t>(channels) * static_cast<size_t>(height) *
                            static_cast<size_t>(width);

    Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    // The input tensor wraps the caller's buffer: no copy in, no allocation.
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        cpu, const_cast<float*>(input), elements, shape, 4);

    // A default-constructed RunOptions. A CUDA execution provider would need
    // "disable_synchronize_execution_providers" here; the CPU EP has nothing to
    // synchronise.
    Ort::RunOptions runOptions;

    auto outputs = _impl->session.Run(runOptions, _impl->inputNames.data(), &inputTensor, 1,
                                      _impl->outputNames.data(), 1);

    if (outputs.size() != 1 || !outputs[0].IsTensor()) {
        throw std::runtime_error("ModelRunner: model returned no tensor");
    }
    const auto outInfo = outputs[0].GetTensorTypeAndShapeInfo();
    const auto outShape = outInfo.GetShape();
    // Shape-preserving by contract: the result is copied back into a buffer of
    // the input's size. An upscaling model - the usual reason to put a model in
    // a plug-in - cannot work through this path and fails here instead of
    // writing past the buffer. Supporting one means reporting the output shape
    // from Run(), mapping it into the output RoD in AiCore, and scaling the RoD
    // in the plug-in; see "Adapting it to a real model" in
    // AI_MODEL_INTEGRATION.md.
    // Every dimension, not just the element count: 1x3x48x64 has the same
    // number of floats as 1x3x64x48, and batch 2 with half the rows matches
    // too. Either would be copied back as a plausible, wrong image.
    if (outShape.size() != 4 || outShape[0] != 1 || outShape[1] != channels ||
        outShape[2] != height || outShape[3] != width) {
        throw std::runtime_error(
            "ModelRunner: output shape does not match the input (this runner is "
            "shape-preserving; a resolution-changing model needs the changes in "
            "AI_MODEL_INTEGRATION.md)");
    }

    const float* data = outputs[0].GetTensorData<float>();
    if (data == nullptr) {
        throw std::runtime_error("ModelRunner: output tensor is not float32");
    }
    std::memcpy(output, data, elements * sizeof(float));
}

} // namespace aiofx
