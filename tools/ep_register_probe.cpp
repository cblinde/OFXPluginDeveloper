// SPDX-License-Identifier: BSD-3-Clause
//
// ep_register_probe.cpp -- the 1.22 execution-provider registration API, outside
// any plug-in.
//
// Why this exists: the inference template's move to
// `RegisterExecutionProviderLibrary` aborted the process with 0xC0000409 where
// the legacy append reports "all nodes on CUDA". The API is a 1.22 addition
// whose *provider* side is a packaging fact - the 1.20.1 provider exports only
// `OrtGetApiBase`, the 1.32.0 one exports `CreateEpFactories` - so "is the API
// broken, or is the plug-in" needs a process with nothing else in it.
//
// Build (x64 MSVC, against a 1.32.0 package):
//     cl /std:c++17 /EHsc /O2 /I<ort>/include tools\ep_register_probe.cpp ^
//        /link /LIBPATH:<ort>/lib onnxruntime.lib
//
// Usage:
//     ep_register_probe <model.onnx> <provider.dll> [--stop N]
//
// Every step prints before it runs and the result after it, flushed, so a crash
// names its own step: registration alone is `--stop 2`, the device list
// `--stop 3`, the append `--stop 6`.
//
// Reads: the model and the provider library. Writes: nothing.


#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const OrtApi* api = nullptr;
int stopAfter = 0;
int stepNumber = 0;

/// Prints the step, runs it, prints the result. Every step returns an OrtStatus
/// the caller decides how to treat.
bool Step(const char* what, OrtStatus* status)
{
    ++stepNumber;
    std::printf("step %d: %-46s ", stepNumber, what);
    std::fflush(stdout);
    if (status == nullptr) {
        std::printf("ok\n");
    } else {
        std::printf("FAILED: %s\n", api->GetErrorMessage(status));
        api->ReleaseStatus(status);
        std::fflush(stdout);
        return false;
    }
    std::fflush(stdout);
    if (stopAfter == stepNumber) {
        std::printf("stopping after step %d as asked\n", stepNumber);
        std::exit(0);
    }
    return true;
}

/// The steps that are not a single api call still want a line each.
bool Mark(const char* what)
{
    ++stepNumber;
    std::printf("step %d: %-46s ok\n", stepNumber, what);
    std::fflush(stdout);
    if (stopAfter == stepNumber) {
        std::printf("stopping after step %d as asked\n", stepNumber);
        std::exit(0);
    }
    return true;
}

std::wstring Wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: ep-register-probe <model.onnx> <provider.dll> [--stop N]\n");
        return 2;
    }
    const std::string model = argv[1];
    const std::string provider = argv[2];
    for (int index = 3; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], "--stop") == 0) {
            stopAfter = std::atoi(argv[index + 1]);
        }
    }

    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    std::printf("OrtApi version %d from %s\n", ORT_API_VERSION, OrtGetApiBase()->GetVersionString());
    std::printf("provider library: %s\n\n", provider.c_str());
    std::fflush(stdout);

    OrtEnv* env = nullptr;
    if (!Step("CreateEnv", api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ep-register-probe", &env))) {
        return 1;
    }

    // The step that aborted inside the plug-in.
    if (!Step("RegisterExecutionProviderLibrary(cuda)",
              api->RegisterExecutionProviderLibrary(env, "cuda-probe", Wide(provider).c_str()))) {
        return 1;
    }

    const OrtEpDevice* const* devices = nullptr;
    size_t deviceCount = 0;
    if (!Step("GetEpDevices", api->GetEpDevices(env, &devices, &deviceCount))) {
        return 1;
    }
    std::printf("        %zu device(s)\n", deviceCount);
    for (size_t index = 0; index < deviceCount; ++index) {
        const OrtEpDevice* device = devices[index];
        const OrtHardwareDevice* hardware = api->EpDevice_Device(device);
        std::printf("        [%zu] ep=%s vendor=%s hw=%s type=%d\n", index,
                    api->EpDevice_EpName(device), api->EpDevice_EpVendor(device),
                    api->HardwareDevice_Vendor(hardware),
                    static_cast<int>(api->HardwareDevice_Type(hardware)));
    }
    std::fflush(stdout);
    Mark("printed the device list");

    OrtSessionOptions* options = nullptr;
    if (!Step("CreateSessionOptions", api->CreateSessionOptions(&options))) {
        return 1;
    }

    // The first device that calls itself CUDA; the plugin EP exposes the same
    // device the built-in one would.
    const OrtEpDevice* chosen = nullptr;
    for (size_t index = 0; index < deviceCount; ++index) {
        if (std::strstr(api->EpDevice_EpName(devices[index]), "CUDA") != nullptr) {
            chosen = devices[index];
            break;
        }
    }
    if (chosen == nullptr) {
        std::printf("no CUDA device in the list, cannot append\n");
        return 1;
    }
    std::printf("        chosen: %s (%s)\n", api->EpDevice_EpName(chosen), api->EpDevice_EpVendor(chosen));
    std::fflush(stdout);

    if (!Step("SessionOptionsAppendExecutionProvider_V2",
              api->SessionOptionsAppendExecutionProvider_V2(options, env, &chosen, 1, nullptr,
                                                            nullptr, 0))) {
        return 1;
    }

    OrtSession* session = nullptr;
    if (!Step("CreateSession", api->CreateSession(env, Wide(model).c_str(), options, &session))) {
        return 1;
    }

    OrtMemoryInfo* memory = nullptr;
    if (!Step("CreateCpuMemoryInfo", api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory))) {
        return 1;
    }

    const int64_t shape[4] = { 1, 3, 64, 64 };
    const size_t count = 3 * 64 * 64;
    std::vector<float> input(count, 0.25f);
    OrtValue* inputValue = nullptr;
    if (!Step("CreateTensorWithDataAsOrtValue",
              api->CreateTensorWithDataAsOrtValue(memory, input.data(), count * sizeof(float),
                                                  shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                  &inputValue))) {
        return 1;
    }

    // The model's own names: a guessed "input" is what a session rejects first.
    OrtAllocator* allocator = nullptr;
    api->GetAllocatorWithDefaultOptions(&allocator);
    char* inputName = nullptr;
    char* outputName = nullptr;
    if (!Step("SessionGetInputName", api->SessionGetInputName(session, 0, allocator, &inputName))) {
        return 1;
    }
    if (!Step("SessionGetOutputName", api->SessionGetOutputName(session, 0, allocator, &outputName))) {
        return 1;
    }
    std::printf("        input: %s, output: %s\n", inputName, outputName);
    OrtValue* output = nullptr;
    if (!Step("Run", api->Run(session, nullptr, &inputName, &inputValue, 1, &outputName, 1, &output))) {
        return 1;
    }

    OrtTensorTypeAndShapeInfo* info = nullptr;
    api->GetTensorTypeAndShape(output, &info);
    size_t elements = 0;
    api->GetTensorShapeElementCount(info, &elements);
    std::printf("        output: %zu values\n", elements);
    api->ReleaseTensorTypeAndShapeInfo(info);
    Mark("read the output shape");

    api->AllocatorFree(allocator, inputName);
    api->AllocatorFree(allocator, outputName);
    api->ReleaseValue(output);
    api->ReleaseValue(inputValue);
    api->ReleaseMemoryInfo(memory);
    api->ReleaseSession(session);
    api->ReleaseSessionOptions(options);

    if (!Step("UnregisterExecutionProviderLibrary",
              api->UnregisterExecutionProviderLibrary(env, "cuda-probe"))) {
        return 1;
    }
    api->ReleaseEnv(env);
    std::printf("\nall steps completed\n");
    return 0;
}
