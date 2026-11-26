// SPDX-License-Identifier: BSD-3-Clause
//
// host_cuda_probe.cpp -- reproduce a host's CUDA loading order without the host.
//
// Why this exists: a plug-in that ships cuDNN/cuBLAS/cuFFT/cudart collides with a
// host that has its own copies loaded. "A module with that name exists" is not the
// question; the questions are which *file* a load returns and whether a second
// copy of a base name can be made to work. This tool answers both outside the
// host, so a candidate fix costs seconds instead of a host restart.
//
// Windows only, one file, no dependencies. The source is the handover's
// (references/field-evidence.md, F33), where it was compiled with MSVC and run
// against DaVinci Resolve 21.2.0.5. It does build here, with the VS 18 BuildTools
// toolchain (cl 14.50); that run is evidence/2026-11-26-windows/host-cuda-probe.txt.
// Rebuild it yourself before trusting its output on another machine.
//
// Build (x64 MSVC, no dependencies):
//     cl /std:c++17 /EHsc /O2 tools\host_cuda_probe.cpp
//
// Usage:
//     host_cuda_probe <hostDir> [--init] [--load <dir>] [--file <dll>] [--keep]
//
//     <hostDir>   folder holding the host's cudnn*/cublas*/cufft*/cudart* DLLs
//     --init      create a CUDA context the way a host does before it renders
//     --load      afterwards load every CUDA DLL from <dir> by absolute path --
//                 this is the step where a bundle's components meet the host's
//                 loader, and where error 127 (ERROR_PROC_NOT_FOUND) appears
//     --file      afterwards load one specific DLL by absolute path; repeatable, in
//                 order. The provider cannot be loaded on its own (error 1114, its
//                 DllMain needs the runtime), so pass the runtime first:
//                   --file onnxruntime-ofx.dll --file onnxruntime_providers_cuda.dll
//                 A provider that loads after that means all of its CUDA imports
//                 were satisfied by whatever is already in the process, which is
//                 the question a CUDA-free bundle answers
//     --keep      sleep so a tool can inspect the process's module list
//
// What to look for (F33):
//   * Two rows for one base name with different paths: the process holds two
//     modules of it, which is the state the runtime guard exists to prevent.
//   * `FAILED (error 127)` for a component of `--load`: a component set older than
//     the host's dispatcher cannot be loaded next to it.
//   * `--keep` plus a module listing of the *host's* process, not this one, is how
//     "the host owns this stack" was established for Resolve: seven of its DLLs
//     are in the process before any plug-in runs.
//   * `what the process resolves for each base name` -- the paths a *later*
//     import would bind to, which is what the CUDA provider's imports do.
//
// Reads: nothing. Writes: nothing. Changes: only this process's module list.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const wchar_t* const kPrefixes[] = { L"cudnn", L"cublas", L"cufft", L"cudart" };

std::wstring Lowered(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    });
    return text;
}

bool IsCudaLibrary(const std::filesystem::path& file)
{
    const std::wstring name = Lowered(file.filename().wstring());
    if (name.size() < 5 || name.substr(name.size() - 4) != L".dll") {
        return false;
    }
    for (const wchar_t* prefix : kPrefixes) {
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> CudaLibrariesIn(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file(error) && IsCudaLibrary(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::wstring ModulePath(HMODULE module)
{
    wchar_t buffer[32768] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, 32768);
    return length == 0 ? std::wstring() : std::wstring(buffer, length);
}

/// Where the process currently gets `name` from -- the answer to "which file is
/// in use", independent of which folder asked for it.
void ReportResolved(const std::wstring& name)
{
    const std::wstring path = ModulePath(GetModuleHandleW(name.c_str()));
    std::printf("  resolved %-22ls %ls\n", name.c_str(),
                path.empty() ? L"(not loaded)" : path.c_str());
    std::fflush(stdout);
}

/// Loads every CUDA DLL of `directory` by absolute path and prints what came back.
///
/// One loop for both roles, because the load and the first half of the report are
/// the same in both and only the *diagnosis* differs: a second copy is the case
/// where error 127 means "your imports lost" and where a successful load may still
/// resolve to the other file. `secondCopy` gates exactly those two extra notes, so
/// the host listing stays a plain ok/FAILED table.
void LoadCudaLibraries(const std::filesystem::path& directory, const wchar_t* heading,
                       bool secondCopy)
{
    std::printf(secondCopy ? "\n%ls %ls\n" : "%ls %ls\n", heading, directory.c_str());
    for (const std::filesystem::path& file : CudaLibrariesIn(directory)) {
        const HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
                                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        std::printf("  load     %-22ls %s", file.filename().c_str(),
                    module != nullptr ? "ok" : "FAILED");
        if (module == nullptr) {
            const DWORD error = GetLastError();
            std::printf(" (error %lu)", static_cast<unsigned long>(error));
            if (secondCopy && error == 127L) {
                std::printf(" -- imports did not match the copy already loaded");
            }
        } else if (secondCopy && ModulePath(module) != file.wstring()) {
            std::printf(" -> but resolved %ls", ModulePath(module).c_str());
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

/// One DLL by path: the provider, in practice. A successful load here means every
/// CUDA import it has was already satisfied -- i.e. the host's stack is enough.
void LoadOneFile(const std::filesystem::path& file)
{
    const HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::printf("\nfile %ls\n  load     %-22ls %s", file.c_str(), file.filename().c_str(),
                module != nullptr ? "ok" : "FAILED");
    if (module == nullptr) {
        std::printf(" (error %lu)", static_cast<unsigned long>(GetLastError()));
    } else if (ModulePath(module) != file.wstring()) {
        std::printf(" -> but resolved %ls", ModulePath(module).c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

/// A host creates a CUDA context before it renders; a context changes what the
/// driver has initialised, which is the difference between "loaded" and "in use".
void InitHostCuda()
{
    const HMODULE runtime = GetModuleHandleW(L"cudart64_12.dll");
    if (runtime == nullptr) {
        std::printf("no cudart64_12.dll loaded, skipping context creation\n");
        return;
    }
    using SetDeviceFn = int (*)(int);
    using FreeFn = int (*)(void*);
    const auto setDevice = reinterpret_cast<SetDeviceFn>(GetProcAddress(runtime, "cudaSetDevice"));
    const auto freeDevice = reinterpret_cast<FreeFn>(GetProcAddress(runtime, "cudaFree"));
    if (setDevice != nullptr) {
        std::printf("\ncudaSetDevice(0) = %d\n", setDevice(0));
    }
    if (freeDevice != nullptr) {
        std::printf("cudaFree(0)      = %d (forces the context)\n", freeDevice(nullptr));
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf(
            "usage: host_cuda_probe <hostDir> [--init] [--load <dir>] [--file <dll>] [--keep]\n");
        return 2;
    }
    const std::filesystem::path hostDirectory(argv[1]);
    std::filesystem::path secondDirectory;
    std::vector<std::filesystem::path> files;
    bool initCuda = false;
    bool keep = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--init") == 0) {
            initCuda = true;
        } else if (std::strcmp(argv[i], "--keep") == 0) {
            keep = true;
        } else if (std::strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
            secondDirectory = argv[++i];
        } else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            files.emplace_back(argv[++i]);
        }
    }

    LoadCudaLibraries(hostDirectory, L"host stack from", false);
    if (initCuda) {
        InitHostCuda();
    }
    if (!secondDirectory.empty()) {
        LoadCudaLibraries(secondDirectory,
                          L"second copy from (a bundle's own DLLs, by absolute path)", true);
    }
    for (const std::filesystem::path& file : files) {
        LoadOneFile(file);
    }

    std::printf("\nwhat the process resolves for each base name:\n");
    for (const wchar_t* name : { L"cudnn64_9.dll", L"cublas64_12.dll", L"cublasLt64_12.dll",
                                 L"cufft64_11.dll", L"cudart64_12.dll" }) {
        ReportResolved(name);
    }

    if (keep) {
        std::printf("\npid %lu, sleeping 60 s for an external module listing\n",
                    static_cast<unsigned long>(GetCurrentProcessId()));
        std::fflush(stdout);
        Sleep(60000);
    }
    return 0;
}
