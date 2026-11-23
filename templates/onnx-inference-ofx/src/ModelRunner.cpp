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
#  include <mutex>
#endif

#include <onnxruntime_cxx_api.h>

namespace aiofx {
namespace {

/// One process-wide environment. Ort::Env owns the logging and thread-pool
/// machinery; creating one per session would leak state across session
/// lifetimes and, on repeated instance creation, grow the process.
///
/// Leaked on purpose. A function-local static's destructor runs at
/// DLL_PROCESS_DETACH - after the host has unloaded the plug-in and while ORT
/// is winding its thread pool down - and the process segfaults *after*
/// kOfxActionUnload returned OK. Nothing calls into the runtime during detach
/// if nothing destroys it.
Ort::Env& SharedEnv()
{
    static Ort::Env* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "ofx-onnx-inference");
    return *env;
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

#if defined(_WIN32) && defined(ORT_API_MANUAL_INIT)
/// File name of the private runtime shipped beside the module. Resolve ships
/// its own onnxruntime.dll next to Resolve.exe, and Windows resolves a module by
/// base name: loading "onnxruntime.dll" - even by full path - returns the host's
/// 1.13 copy, whose GetApi(ORT_API_VERSION) is null and crashes on first use.
/// A name no host uses cannot collide. CMakeLists.txt copies this exact name.
constexpr wchar_t kPrivateRuntimeDll[] = L"onnxruntime-ofx.dll";

/// Full path of the private runtime next to this module, or empty when the
/// module path cannot be determined. The buffer grows because install paths
/// longer than MAX_PATH are legal.
std::wstring PrivateRuntimePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PrivateRuntimePath), &module)) {
        return std::wstring();
    }
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length < static_cast<DWORD>(path.size())) break;
        if (path.size() >= 32768) return std::wstring(); // longer than any legal path
        path.resize(path.size() * 2);
    }
    if (length == 0) return std::wstring();
    const size_t slash = path.find_last_of(L"\\/", length - 1);
    return slash == std::wstring::npos ? std::wstring()
                                       : path.substr(0, slash + 1) + kPrivateRuntimeDll;
}

/// Loads the private runtime once and installs its C API. Never unloaded: an
/// Ort::Env created later outlives this call, so FreeLibrary would leave the
/// process with dangling code pointers. A failure is remembered rather than
/// retried: the plug-in constructs a ModelRunner per render attempt, and a
/// retry would re-map the DLL every frame for a cause that cannot change
/// without a host restart.
void LoadPrivateRuntime()
{
    static std::once_flag once;
    static std::string error;
    std::call_once(once, [] {
        const std::wstring file = PrivateRuntimePath();
        if (file.empty()) {
            error = "cannot determine the plug-in directory";
            return;
        }
        HMODULE library = LoadLibraryExW(
            file.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (library == nullptr) {
            error = "cannot load onnxruntime-ofx.dll beside the plug-in (Windows error " +
                    std::to_string(GetLastError()) + ")";
            return;
        }
        FARPROC symbol = GetProcAddress(library, "OrtGetApiBase");
        if (symbol == nullptr) {
            error = "onnxruntime-ofx.dll has no OrtGetApiBase";
            return;
        }
        // memcpy rather than a cast from FARPROC: the cast is not portable and
        // MSVC warns about it even when the sizes agree.
        using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
        GetApiBaseFn getApiBase = nullptr;
        static_assert(sizeof(getApiBase) == sizeof(symbol), "function pointer size");
        std::memcpy(&getApiBase, &symbol, sizeof(getApiBase));
        const OrtApi* api = getApiBase()->GetApi(ORT_API_VERSION);
        if (api == nullptr) {
            error = "onnxruntime-ofx.dll is older than ORT_API_VERSION " +
                    std::to_string(ORT_API_VERSION);
            return;
        }
        Ort::InitApi(api);
    });
    if (!error.empty()) {
        throw std::runtime_error("ModelRunner: " + error);
    }
}
#endif

} // namespace

namespace {

/// One line, clipped: ORT's provider errors are a paragraph with the file and
/// line of the check inside them, and this string goes into a label parameter.
std::string ShortMessage(const std::string& text)
{
    const size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    // ORT prefixes its own location and error number:
    //   /onnxruntime_src/.../provider_bridge_ort.cc:1488 ... [ONNXRuntimeError] : 1 : FAIL : <why>
    // The "why" is the part a user can act on; the rest is noise in a label.
    const std::string marker = " : FAIL : ";
    const size_t at = line.find(marker);
    if (at != std::string::npos) {
        line = line.substr(at + marker.size());
    }
    constexpr size_t kMax = 120;
    if (line.size() > kMax) {
        line = line.substr(0, kMax) + "...";
    }
    return line;
}

/// The options every session gets. A function because the CUDA attempt needs a
/// fresh copy to fall back to: a failed AppendExecutionProvider_CUDA may leave
/// the options half-configured, and the CPU session must not inherit that.
Ort::SessionOptions MakeSessionOptions(int intraThreads)
{
    Ort::SessionOptions options;
    // See the header: the host already runs render() on several threads.
    const int threads = intraThreads > 0 ? intraThreads : 1;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ORT_SEQUENTIAL);
    // Full graph optimisation is the default, but state it: a plugin built with
    // ORT_ENABLE_BASIC can be several times slower with no visible difference.
    options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    // Memory patterns stay on (the ORT default). They cache the per-shape
    // allocations a session reuses on every Run(), which is exactly this
    // plug-in's access pattern: one shape, once per frame.
    return options;
}

/// Requests the CUDA execution provider. Every failure - no GPU, no driver,
/// missing cuDNN, a CPU-only runtime, a runtime whose provider library is not
/// beside it - arrives as an ORT exception, so the caller falls back to a CPU
/// session instead of failing the frame. The reason is kept for the label.
///
/// Verified on this machine (CPU package, ORT 1.20.1): the call throws "Failed
/// to load library libonnxruntime_providers_shared.so" and does not crash.
/// Verified nowhere: the success branch. There is no GPU here.
bool AppendCudaProvider(Ort::SessionOptions& options, std::string* reason)
{
    try {
        OrtCUDAProviderOptions cuda{};
        cuda.device_id = 0;
        // ORT benchmarks convolution algorithms at session creation by default,
        // which costs minutes at 1080p before the first tile (external
        // measurement, AI_MODEL_INTEGRATION.md section 7).
        cuda.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
        // Copies stay on the provider's stream, which is the default here and
        // the only safe choice when the host hands over host memory.
        cuda.do_copy_in_default_stream = 1;
        options.AppendExecutionProvider_CUDA(cuda);
        return true;
    } catch (const std::exception& e) {
        *reason = ShortMessage(e.what());
        return false;
    }
}

} // namespace

struct ModelRunner::Impl {
    Ort::Session session{ nullptr };
    std::string inputName;
    std::string outputName;
    std::string provider = "CPU";
    std::string providerReason;
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

#if defined(_WIN32) && defined(ORT_API_MANUAL_INIT)
    // Before the first Ort:: object exists: the C++ wrappers call through this API.
    LoadPrivateRuntime();
#endif

    // CUDA first. Both attempts build their own options, so a failed provider
    // append cannot leave the CPU session half-configured.
    Ort::SessionOptions options = MakeSessionOptions(intraThreads);
    std::string providerReason;
    if (AppendCudaProvider(options, &providerReason)) {
        _impl->provider = "CUDA (device 0)";
    } else {
        _impl->provider = "CPU";
        _impl->providerReason = providerReason;
        options = MakeSessionOptions(intraThreads);
    }

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

const std::string& ModelRunner::Provider() const
{
    return _impl->provider;
}

const std::string& ModelRunner::ProviderReason() const
{
    return _impl->providerReason;
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
