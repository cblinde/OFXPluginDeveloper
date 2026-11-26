// SPDX-License-Identifier: BSD-3-Clause

#include "ModelRunner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
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

/// Full path of a module that is already in this process, or empty. The buffer
/// grows because an install path longer than MAX_PATH is legal: GetModuleFileNameW
/// truncates to the buffer size and returns that size when it had to, which is
/// the case the loop exists for. One helper, so the size cannot drift per call
/// site - a 260-character buffer here reports "cannot determine the plug-in
/// directory" for an install path that is perfectly valid.
std::wstring LoadedModulePath(HMODULE module)
{
    if (module == nullptr) return std::wstring();
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::wstring();
        if (length < static_cast<DWORD>(path.size())) {
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768) return std::wstring(); // longer than any legal path
        path.resize(path.size() * 2);
    }
}

/// Full path of the private runtime next to this module, or empty when the
/// module path cannot be determined.
std::wstring PrivateRuntimePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PrivateRuntimePath), &module)) {
        return std::wstring();
    }
    const std::wstring path = LoadedModulePath(module);
    if (path.empty()) return std::wstring();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring()
                                       : path.substr(0, slash + 1) + kPrivateRuntimeDll;
}

/// ASCII case fold: Windows paths compare case-insensitively, and a folder
/// comparison needs nothing more. Unicode folding is deliberately absent - two
/// spellings of a non-ASCII folder are either the file the caller asked for or
/// not a host DLL this guard has to recognise.
std::wstring Folded(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    });
    return text;
}

std::wstring FolderOf(const std::wstring& file)
{
    const size_t slash = file.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : file.substr(0, slash);
}

bool SameFolder(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && Folded(a) == Folded(b);
}

bool StartsWith(const std::wstring& name, const wchar_t* prefix)
{
    return name.compare(0, std::wcslen(prefix), prefix) == 0;
}

/// The base names onnxruntime_providers_cuda's import table holds, as groups:
/// the provider imports cudnn64_9, cublas64_12, cublasLt64_12, cufft64_11 and
/// cudart64_12, and the loader resolves each by base name (see
/// references/onnx-and-tensorrt.md section 4). cuFFTW is the same group as cuFFT but is
/// never imported by the provider, and is skipped by name.
enum class CudaGroup { other, cudnn, cublas, cufft, cudart };

CudaGroup CudaGroupOf(const std::wstring& name)
{
    if (StartsWith(name, L"cudnn")) return CudaGroup::cudnn;
    if (StartsWith(name, L"cublas")) return CudaGroup::cublas;
    if (StartsWith(name, L"cufft")) return CudaGroup::cufft; // including cufftw
    if (StartsWith(name, L"cudart")) return CudaGroup::cudart;
    return CudaGroup::other;
}

int GroupSlot(CudaGroup group)
{
    switch (group) {
        case CudaGroup::cudnn: return 0;
        case CudaGroup::cublas: return 1;
        case CudaGroup::cufft: return 2;
        case CudaGroup::cudart: return 3;
        default: return -1;
    }
}

const wchar_t* GroupName(CudaGroup group)
{
    switch (group) {
        case CudaGroup::cudnn: return L"cuDNN";
        case CudaGroup::cublas: return L"cuBLAS";
        case CudaGroup::cufft: return L"cuFFT";
        case CudaGroup::cudart: return L"the CUDA runtime";
        default: return L"CUDA";
    }
}

/// What the preload did, written once inside LoadPrivateRuntime's call_once and
/// read afterwards: the label names the stack that is in use (F32), and a CUDA
/// failure carries the file that could not be loaded (F26).
struct CudaPreload {
    /// Folder a host CUDA DLL was loaded from, empty when none was: then the
    /// bundle's own copies are the stack.
    std::wstring hostFolder;
    std::vector<std::wstring> skipped; // "name (why)"
    std::vector<std::wstring> failed;  // "name (error N)"
};

CudaPreload& CudaPreloadNotes()
{
    static CudaPreload notes;
    return notes;
}

/// UTF-8 of a wide string, for a label. WidePath's inverse.
std::string NarrowText(const std::wstring& wide)
{
    if (wide.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 0) return std::string();
    std::string narrow(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), narrow.data(),
                        needed, nullptr, nullptr);
    return narrow;
}

/// One clipped line for the label: what was skipped and what failed. Empty when
/// the preload loaded everything it found.
std::string PreloadSummary()
{
    const CudaPreload& notes = CudaPreloadNotes();
    std::string text;
    auto append = [&text](const std::wstring& item) {
        const std::string narrow = NarrowText(item);
        if (narrow.empty()) return;
        if (!text.empty()) text += ", ";
        text += narrow;
    };
    for (const std::wstring& item : notes.failed) append(item);
    for (const std::wstring& item : notes.skipped) append(item);
    if (text.empty()) return text;
    const std::string prefix = "preload: ";
    constexpr size_t kMax = 140;
    if (prefix.size() + text.size() > kMax) {
        // Truncate in place: `text = text.substr(...) + "..."` copies the kept
        // prefix into a temporary and assigns it back over itself.
        text.replace(kMax - prefix.size() - 3, std::string::npos, "...");
    }
    return prefix + text;
}

/// Loads the DLLs beside the module that ONNX Runtime's CUDA provider imports,
/// by absolute path - and not the ones the host already owns.
///
/// A GPU bundle is a directory of libraries that import each other by base name,
/// and a plug-in directory is not on the default search path: the cuDNN
/// dispatcher loads, then the process exits 127 looking for cudnn_graph64_9.dll -
/// an exit, not an exception, so the CPU fallback never runs. Loading them up
/// front is what makes the later name-based lookups find the copies already in
/// the process. Three rules bound what "them" means, each from a failure
/// measured in a real host (F26, F32):
///
///  * onnxruntime* is skipped: the core is loaded under its private name, and the
///    provider must be loaded by ORT's own mailbox handshake (1114 otherwise).
///  * cufftw* is skipped entirely. The provider never imports cuFFTW, and it
///    imports the exact cuFFT build it shipped with: with a host's newer
///    cufft64_11.dll already loaded the load fails 127, and an all-or-nothing
///    preload turned that unused file into "cannot load cufftw64_11.dll" on the
///    provider label while the GPU path stayed down.
///  * A group the host already has loaded is left alone. Windows resolves an
///    *import* by base name, so the provider binds to whichever module is loaded
///    under that name, and an explicit path-load of the bundle's copy does not
///    change that: it succeeds and leaves two cudnn64_9.dll of different minor
///    versions in one process. The components of the older copy then fail against
///    the newer dispatcher (127). The test compares the loaded module's *path*;
///    "a module with that name exists" is true for our own copy the moment we
///    load it, which is the check that lets the duplicate through.
void PreloadCudaLibraries(const std::wstring& moduleFile)
{
    const size_t slash = moduleFile.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring folder = moduleFile.substr(0, slash);

    WIN32_FIND_DATAW entry{};
    HANDLE found = FindFirstFileW((folder + L"\\*.dll").c_str(), &entry);
    if (found == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do {
        names.push_back(entry.cFileName);
    } while (FindNextFileW(found, &entry) != 0);
    FindClose(found);

    CudaPreload& notes = CudaPreloadNotes();
    // Decided before the first load: afterwards every name is ours.
    bool hostOwned[4] = { false, false, false, false };
    for (const std::wstring& name : names) {
        const CudaGroup group = CudaGroupOf(name);
        if (group == CudaGroup::other) continue;
        const std::wstring path = LoadedModulePath(GetModuleHandleW(name.c_str()));
        if (path.empty() || SameFolder(FolderOf(path), folder)) continue;
        hostOwned[GroupSlot(group)] = true;
        if (notes.hostFolder.empty()) notes.hostFolder = FolderOf(path);
    }

    bool reportedHostOwned[4] = { false, false, false, false };
    for (const std::wstring& name : names) {
        if (StartsWith(name, L"onnxruntime")) continue;
        if (StartsWith(name, L"cufftw")) {
            notes.skipped.push_back(name + L" (cuFFTW, not imported)");
            continue;
        }
        const CudaGroup group = CudaGroupOf(name);
        if (group != CudaGroup::other && hostOwned[GroupSlot(group)]) {
            // Once per group, or one cuDNN dispatcher and seven components put
            // seven copies of the same sentence into the label.
            if (!reportedHostOwned[GroupSlot(group)]) {
                reportedHostOwned[GroupSlot(group)] = true;
                notes.skipped.push_back(std::wstring(L"the host owns ") + GroupName(group));
            }
            continue;
        }
        const std::wstring file = folder + L"\\" + name;
        if (LoadLibraryExW(file.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) == nullptr) {
            notes.failed.push_back(name + L" (error " + std::to_wstring(GetLastError()) +
                                   L")");
        }
    }
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
        PreloadCudaLibraries(file);
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
        line.replace(kMax, std::string::npos, "...");
    }
    return line;
}

/// The count the session is really built with. 0 means "decide here": one
/// thread, because the host already calls render() on several threads and ORT's
/// own default is one intra-op thread per physical core *per session*. The
/// chosen number goes into the provider label - a knob that reaches nothing and
/// a label that reads like a constant are the same bug (F28, F31).
int SessionThreads(int requested)
{
    return requested > 0 ? requested : 1;
}

std::string ThreadLabel(int threads)
{
    return threads == 1 ? std::string("1 thread") : std::to_string(threads) + " threads";
}

/// The options every session gets. A function because the CUDA attempt needs a
/// fresh copy to fall back to: a failed AppendExecutionProvider_CUDA may leave
/// the options half-configured, and the CPU session must not inherit that.
Ort::SessionOptions MakeSessionOptions(int intraThreads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(SessionThreads(intraThreads));
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

/// Owns an `OrtCUDAProviderOptionsV2` until the session has copied it, and speaks
/// the C API that produces and fills it.
class CudaOptions
{
public:
    CudaOptions() { Ort::ThrowOnError(Ort::GetApi().CreateCUDAProviderOptions(&options_)); }
    ~CudaOptions() { Ort::GetApi().ReleaseCUDAProviderOptions(options_); }
    CudaOptions(const CudaOptions&) = delete;
    CudaOptions& operator=(const CudaOptions&) = delete;

    OrtCUDAProviderOptionsV2* get() const { return options_; }

    void Set(const char* const* keys, const char* const* values, size_t count)
    {
        Ort::ThrowOnError(Ort::GetApi().UpdateCUDAProviderOptions(options_, keys, values, count));
    }

    /// The provider's own "key=value;key=value" view - the only way to show that an
    /// option arrived rather than being dropped on the way in.
    std::string AsString() const
    {
        OrtAllocator* allocator = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetAllocatorWithDefaultOptions(&allocator));
        char* text = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetCUDAProviderOptionsAsString(options_, allocator, &text));
        std::string copy = text != nullptr ? text : "";
        allocator->Free(allocator, text);
        return copy;
    }

private:
    OrtCUDAProviderOptionsV2* options_ = nullptr;
};

/// Requests the CUDA execution provider with every option that decides what the
/// device computes, or how much of it the device reserves, stated explicitly.
///
/// The struct cannot say all of it: `use_tf32` and
/// `cudnn_conv_use_max_workspace` exist only as provider options, so the V2
/// key/value API is the only way to switch them off at all
/// (references/onnx-and-tensorrt.md section 5). What the struct defaults would
/// leave: EXHAUSTIVE search (benchmarks every algorithm at session creation),
/// TF32 on since 1.18 (10-bit mantissa in every accelerated convolution) and
/// cuDNN free to take the largest workspace it can find.
///
/// Every failure - no GPU, no driver, missing cuDNN, a CPU-only runtime, a
/// runtime whose provider library is not beside it - arrives as an ORT
/// exception, so the caller falls back to a CPU session instead of failing the
/// frame. The reason is kept for the label.
///
/// `applied` receives the provider's own serialisation of the options, which is
/// what makes "the option reached the provider" checkable instead of assumed.
///
/// The options are built through the **C** API, not through
/// `Ort::CUDAProviderOptions`: that C++ wrapper does not exist in ONNX Runtime
/// 1.20.1 - it arrived in a later release - while `CreateCUDAProviderOptions`
/// and `UpdateCUDAProviderOptions` have been stable since 1.11. This repository
/// builds against 1.20.1 and 1.32.0 (section 5 of the document), so the wrapper
/// would have made the template unbuildable against one of its own runtimes.
/// A step printer for the experiment path below: printed and flushed *before*
/// the call, because a 0xC0000409 takes the buffered output with it and the last
/// line written then names the call that never returned.
void V2Stage(const char* what)
{
    std::fprintf(stderr, "onnx-inference: v2 step: %s\n", what);
    std::fflush(stderr);
}

#if ORT_API_VERSION >= 22
/// The provider library beside this module, or empty when the folder is unknown.
std::wstring ProviderLibraryPath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ProviderLibraryPath), &module)) {
        return std::wstring();
    }
    const std::wstring folder = FolderOf(LoadedModulePath(module));
    return folder.empty() ? std::wstring() : folder + L"\\onnxruntime_providers_cuda.dll";
}

/// The same six options, through the 1.22 registration API instead of the
/// built-in table.
///
/// This is the path that aborted the plug-in with 0xC0000409, and the reason it
/// lives behind `OFXPLUGIN_CUDA_EP_V2` rather than in the template: the probe
/// (`tools/ep_register_probe.cpp`) runs the identical sequence in a console
/// process - with the plug-in's renamed core and its CUDA preloads among the
/// switches - and every step succeeds, so what aborts is reachable only from
/// here. The variable exists so the suite can bisect it; the steps print, so a
/// run that dies says which call did not return.
///
/// The keys are the same array the legacy append uses, deliberately: a bisect
/// that changed two things at once would answer neither question.
bool AppendCudaProviderV2(Ort::SessionOptions& options, const char* const* keys,
                          const char* const* values, size_t keyCount,
                          std::string* reason, std::string* applied)
{
    const OrtApi& api = Ort::GetApi();
    OrtEnv* env = SharedEnv();

    // Registration is per environment, not per session, and the second call for
    // the same name is an error, not a no-op: measured here as a "provider
    // request failed" on the second instance's label - which then fell back to
    // the CPU and made two identical instances produce different pixels. So the
    // device list is resolved once and reused.
    static std::mutex mutex;
    static bool registered = false;
    static const OrtEpDevice* cudaDevice = nullptr;

    const std::lock_guard<std::mutex> lock(mutex);
    if (!registered) {
        const std::wstring library = ProviderLibraryPath();
        if (library.empty()) {
            *reason = "the plug-in directory cannot be determined";
            return false;
        }

        V2Stage("RegisterExecutionProviderLibrary");
        // The registration name is `const char*` on every platform; only the
        // library path is wide on Windows.
        OrtStatus* registration =
            api.RegisterExecutionProviderLibrary(env, "cuda-ofx", library.c_str());
        if (registration != nullptr) {
            // A host that unloads and reloads this plug-in in the same process
            // empties the static flag above; the runtime's registration lives in
            // the environment and survives. "already registered" therefore means
            // the library is there and its devices are still queryable - measured
            // here as the second load of the .ofx falling back to the CPU with
            // exactly that message. Every other status is a real failure.
            const std::string message = api.GetErrorMessage(registration);
            api.ReleaseStatus(registration);
            if (message.find("already registered") == std::string::npos) {
                // The stage line above names the call; this names the cause. The
                // label keeps a shortened message, and "Error loading <path>..."
                // truncated at the label's width says nothing about why the
                // registration failed.
                V2Stage(("register failed: " + message).c_str());
                throw std::runtime_error(message);
            }
        }

        V2Stage("GetEpDevices");
        const OrtEpDevice* const* devices = nullptr;
        size_t deviceCount = 0;
        Ort::ThrowOnError(api.GetEpDevices(env, &devices, &deviceCount));
        for (size_t index = 0; index < deviceCount; ++index) {
            const char* name = api.EpDevice_EpName(devices[index]);
            if (name != nullptr && std::strncmp(name, "CUDA", 4) == 0) {
                cudaDevice = devices[index];
                break;
            }
        }
        if (cudaDevice == nullptr) {
            *reason = "no CUDA device among the registered provider's devices";
            return false;
        }
        registered = true;
    }
    const OrtEpDevice* chosen = cudaDevice;

    V2Stage("SessionOptionsAppendExecutionProvider_V2");
    Ort::ThrowOnError(api.SessionOptionsAppendExecutionProvider_V2(
        options, env, &chosen, 1, keys, values, keyCount));
    V2Stage("appended");

    // Deliberately empty: this path has no provider serialisation to report. The
    // legacy append hands back the provider's own text, and the label's "arena
    // kSameAsRequested" clause is a claim about that text, not about the request
    // - so the V2 path must not fill it in with what it *asked* for.
    (void)applied;
    return true;
}
#endif

bool AppendCudaProvider(Ort::SessionOptions& options, std::string* reason,
                        std::string* applied = nullptr,
                        bool* providerSerialised = nullptr)
{
    try {
        CudaOptions cuda;
        // One value per key, same order. The comments are why each value is what
        // it is; the key order below and the value order below have to stay in
        // step, which is the price of the C API's array shape.
        const char* keys[] = {
            "device_id",
            // 0 = full float32 in the tensor-core path. 1 is the EP default since
            // 1.18, and it rounds the mantissa to 10 bits - visible in the third
            // decimal place, which is three orders above the tolerance this
            // template's suite uses.
            "use_tf32",
            // HEURISTIC asks cuDNN once. EXHAUSTIVE benchmarks every algorithm
            // against the real tensors before the first tile - minutes at 1080p
            // the first time on a machine, milliseconds afterwards, because
            // cuDNN caches the chosen plans on disk.
            "cudnn_conv_algo_search",
            // 0 = cuDNN may not take the largest workspace it finds; the plan it
            // picks has to fit the small one. 1 is the default and reserves a
            // second copy of the activation budget.
            "cudnn_conv_use_max_workspace",
            // Copies stay on the provider's stream: the only safe choice when the
            // host hands over host memory.
            "do_copy_in_default_stream",
            // kSameAsRequested grows the arena by exactly what the session asked
            // for instead of rounding to the next power of two. This is the one
            // option here that changes what a second instance costs: measured on
            // this machine, three 640x480 sessions hold 2642 MB with the default
            // and 980 MB with this, and two 1080p sessions 7284 MB against 4170 MB
            // (references/onnx-and-tensorrt.md section 5). The default's per-instance
            // figure is the single-session peak rounded up the curve.
            "arena_extend_strategy",
        };
        // No `gpu_mem_limit`, deliberately: a limit is a cap the allocator fails
        // on, not a budget it stays inside - section 5 measured the failure
        // ("Available memory of ... is smaller than requested bytes of ...") - and
        // any fixed number is wrong for the next frame size. The arena strategy
        // is the lever that lowers what an instance holds; a limit only decides
        // when it dies.
        const char* values[] = { "0", "0", "HEURISTIC", "0", "1", "kSameAsRequested" };
        const size_t keyCount = sizeof(keys) / sizeof(keys[0]);
#if ORT_API_VERSION >= 22
        // Backlog row 13: the registration API, only when asked for. A runtime
        // whose provider has no `CreateEpFactories` cannot be registered at all,
        // which is why the default stays the built-in table.
        if (const char* switchOn = std::getenv("OFXPLUGIN_CUDA_EP_V2")) {
            if (*switchOn != '\0') {
                if (providerSerialised != nullptr) *providerSerialised = false;
                return AppendCudaProviderV2(options, keys, values, keyCount, reason, applied);
            }
        }
#endif
        if (providerSerialised != nullptr) *providerSerialised = true;
        cuda.Set(keys, values, keyCount);
        Ort::ThrowOnError(Ort::GetApi().SessionOptionsAppendExecutionProvider_CUDA_V2(
            options, cuda.get()));
        if (applied) {
            *applied = cuda.AsString();
        }
        return true;
    } catch (const std::exception& e) {
        *reason = ShortMessage(e.what());
        return false;
    }
}

/// What the fallback probe found in one model.
struct NodeCoverage
{
    /// Whether the probe got as far as asking: a provider that disappeared
    /// between the two attempts leaves the question open rather than answered.
    bool probed = false;
    bool allOnDevice = false;
    /// The runtime's own words for what it could not place. On 1.32.0 that is
    /// "This session contains graph nodes that are assigned to the default CPU
    /// EP, but ..." - a statement, not a node name. The label carries the fact;
    /// naming the node needs the verbose log (section 6 of
    /// references/onnx-and-tensorrt.md).
    std::string reason;
};

/// Answers "did every node run on the provider", by building the session once
/// with `session.disable_cpu_ep_fallback=1` and throwing it away.
///
/// Accepted CUDA request is not the same claim. A node the CUDA EP does not
/// implement is assigned to the CPU EP without a message: the session builds,
/// every check passes, the label says CUDA, and on a 3060 the clock is the only
/// thing that noticed (F32). The key turns that silence into a failed session
/// creation whose message names what was left over. It is invalid to combine it
/// with an explicitly added CPU EP, so the probe appends CUDA and nothing else.
///
/// Cached per model path: the answer belongs to the model, and this is a second
/// session creation, the expensive half of loading. The probe is destroyed before
/// the real session exists, so it does not double the resident weights.
NodeCoverage ProbeNodeCoverage(const std::string& modelPath, int intraThreads)
{
    static std::mutex mutex;
    static std::map<std::string, NodeCoverage> cache;

    const std::lock_guard<std::mutex> lock(mutex);
    const auto known = cache.find(modelPath);
    if (known != cache.end()) {
        return known->second;
    }

    NodeCoverage coverage;
    Ort::SessionOptions options = MakeSessionOptions(intraThreads);
    options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    std::string providerReason;
    if (!AppendCudaProvider(options, &providerReason)) {
        // The provider is gone between the two attempts; say so rather than
        // blame a node.
        coverage.reason = "provider request failed: " + providerReason;
    } else {
        coverage.probed = true;
        try {
#ifdef _WIN32
            const std::wstring wide = WidePath(modelPath);
            Ort::Session probe(SharedEnv(), wide.c_str(), options);
#else
            Ort::Session probe(SharedEnv(), modelPath.c_str(), options);
#endif
            (void)probe;
            coverage.allOnDevice = true;
        } catch (const std::exception& e) {
            coverage.reason = ShortMessage(e.what());
        }
    }
    cache.emplace(modelPath, coverage);
    return coverage;
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
    const int threads = SessionThreads(intraThreads);
    Ort::SessionOptions options = MakeSessionOptions(intraThreads);
    std::string providerReason;
    std::string appliedOptions;
    bool providerSerialised = true;
    if (AppendCudaProvider(options, &providerReason, &appliedOptions, &providerSerialised)) {
        // Both providers print the thread count, because that label is the only
        // evidence that the value reached the session, in either mode.
        _impl->provider = "CUDA (device 0, " + ThreadLabel(threads);
#if defined(_WIN32) && defined(ORT_API_MANUAL_INIT)
        // Which cuDNN/cuBLAS/cuFFT/cudart the provider's imports resolve to: the
        // bundle's copies, or the ones the host already had loaded (F32).
        _impl->provider += CudaPreloadNotes().hostFolder.empty() ? ", bundled CUDA"
                                                                 : ", host CUDA stack";
#endif
        // The one option that changes the numbers rather than the memory: it
        // belongs in the label, so a run says which arithmetic it measured.
        _impl->provider += ", tf32 off";
        // The label repeats the provider's own serialisation of the options, not
        // the request: a runtime that dropped the arena key has to show up as
        // "NOT applied" rather than as a label that reads like a constant.
        // The registration API reports nothing back, so a V2 run cannot claim
        // what the provider kept: it says what it asked for instead, and the
        // missing serialisation is the difference a reader has to see.
        if (!providerSerialised) {
            _impl->provider += ", options requested (arena kSameAsRequested)";
        } else {
            _impl->provider += appliedOptions.find("kSameAsRequested") != std::string::npos
                                   ? ", arena kSameAsRequested"
                                   : ", arena NOT applied";
        }
        // "CUDA" alone is the strong claim, not the true one: an unsupported node
        // sits on the CPU EP and the session still builds. The probe settles it,
        // and its answer goes where a reader looks - the label.
        const NodeCoverage coverage = ProbeNodeCoverage(modelPath, intraThreads);
        if (!coverage.probed) {
            // The probe could not ask. Saying "nodes left on CPU" would be a
            // guess about a question nobody answered.
            _impl->provider += ", node coverage unknown)";
            _impl->providerReason = coverage.reason;
        } else if (coverage.allOnDevice) {
            _impl->provider += ", all nodes on CUDA)";
        } else {
            // Not fatal on purpose: a model with one unsupported node is still
            // better off on the device for the rest, so the real session is built
            // without the flag. What must not happen is the label claiming more
            // than the session did.
            _impl->provider += ", nodes left on CPU)";
            _impl->providerReason = coverage.reason;
        }
    } else {
        _impl->provider = "CPU (" + ThreadLabel(threads) + ")";
        _impl->providerReason = providerReason;
#if defined(_WIN32) && defined(ORT_API_MANUAL_INIT)
        // A preload failure is not the reason unless the provider failed as well,
        // and then it is the first thing to look at: name the file.
        const std::string preload = PreloadSummary();
        if (!preload.empty()) {
            _impl->providerReason += "; " + preload;
        }
#endif
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
    // references/ai-architectures-and-runtimes.md.
    // Every dimension, not just the element count: 1x3x48x64 has the same
    // number of floats as 1x3x64x48, and batch 2 with half the rows matches
    // too. Either would be copied back as a plausible, wrong image.
    if (outShape.size() != 4 || outShape[0] != 1 || outShape[1] != channels ||
        outShape[2] != height || outShape[3] != width) {
        throw std::runtime_error(
            "ModelRunner: output shape does not match the input (this runner is "
            "shape-preserving; a resolution-changing model needs the changes in "
            "references/ai-architectures-and-runtimes.md)");
    }

    const float* data = outputs[0].GetTensorData<float>();
    if (data == nullptr) {
        throw std::runtime_error("ModelRunner: output tensor is not float32");
    }
    std::memcpy(output, data, elements * sizeof(float));
}

} // namespace aiofx
