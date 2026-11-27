// SPDX-License-Identifier: BSD-3-Clause
//
// Wall-clock frame time per execution provider, outside any plug-in.
//
// Why it exists: references/runtime-comparison.md names the missing number - a
// CUDA-EP-against-CPU-EP frame time for the inference template on this machine -
// and the wheel in .venv has no CUDA provider, so the number has to come from
// the bundled package's own DLLs. This program links the same onnxruntime.lib
// and loads the same onnxruntime.dll / onnxruntime_providers_cuda.dll the
// template's bundle ships, and times one Run() per frame on both providers.
//
// It was a scratch file in .validation when those numbers were first taken, which
// made the table in runtime-comparison.md section 5 reproducible only by someone
// with that file. It lives here so the table can be re-derived from the tree:
// tools/bench-ep-frame.bat (Windows) and tools/bench-ep-frame.sh build it into
// the package's own bin directory - beside the DLLs it has to load - and run it.
// The committed model models/test_conv.onnx is what the table's rows were
// measured with, at the frame sizes the rows name.
//
// The per-frame path is ModelRunner::Run()'s: wrap the caller's host buffer as
// an OrtValue (no copy, no allocation), Run() with default RunOptions, memcpy the
// output back into a host buffer. With do_copy_in_default_stream=1 and host
// bindings, Run() synchronises, so the number a host waits for is the whole
// round trip and not a launch.
//
//   bench-ep-frame <model.onnx> <W> <H> [--iterations N] [--warmup N] [--threads N]
//
// Prints, per round: session creation, the CUDA provider options string the
// provider itself serialises, the first frame separately from the warm ones
// (context creation, cuDNN init and kernel loading belong to setup), every
// measured sample, and min/median/p90/max. The CUDA output is compared with the
// CPU one, so a silent fallback shows up as 0 difference rather than as a fast
// row.

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const OrtApi& api = Ort::GetApi();

struct CudaOptions {
    OrtCUDAProviderOptionsV2* handle = nullptr;
    ~CudaOptions()
    {
        if (handle != nullptr) {
            api.ReleaseCUDAProviderOptions(handle);
        }
    }
};

/// Template's six keys, in the template's order (ModelRunner.cpp, "Requests the
/// CUDA execution provider with every option that decides what the device
/// computes"). Value order has to stay in step with key order.
Ort::SessionOptions CudaSessionOptions(CudaOptions& cuda, int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda.handle));
    const char* keys[] = { "device_id", "use_tf32", "cudnn_conv_algo_search",
                           "cudnn_conv_use_max_workspace", "do_copy_in_default_stream",
                           "arena_extend_strategy" };
    const char* values[] = { "0", "0", "HEURISTIC", "0", "1", "kSameAsRequested" };
    Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cuda.handle, keys, values, 6));
    Ort::ThrowOnError(api.SessionOptionsAppendExecutionProvider_CUDA_V2(options, cuda.handle));
    return options;
}

Ort::SessionOptions CpuSessionOptions(int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    return options;
}

std::vector<float> Input(size_t elements)
{
    std::vector<float> data(elements);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.017) * 0.5 + 0.25);
    }
    return data;
}

double Milliseconds(std::chrono::steady_clock::time_point from,
                    std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

/// One frame, exactly what ModelRunner::Run does per render.
void Frame(Ort::Session& session, const char* inputName, const char* outputName,
           const Ort::MemoryInfo& memory, std::vector<float>& input,
           std::vector<float>& output, const std::vector<int64_t>& shape)
{
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                        shape.data(), shape.size());
    auto outputs =
        session.Run(Ort::RunOptions{ nullptr }, &inputName, &tensor, 1, &outputName, 1);
    std::memcpy(output.data(), outputs[0].GetTensorData<float>(),
                input.size() * sizeof(float));
}

struct Stats {
    std::vector<double> samples;
};

Stats Measure(Ort::Session& session, const char* inputName, const char* outputName,
              const Ort::MemoryInfo& memory, std::vector<float>& input,
              std::vector<float>& output, const std::vector<int64_t>& shape, int warmup,
              int iterations)
{
    for (int i = 0; i < warmup; ++i) {
        Frame(session, inputName, outputName, memory, input, output, shape);
    }
    Stats stats;
    stats.samples.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        Frame(session, inputName, outputName, memory, input, output, shape);
        const auto finished = std::chrono::steady_clock::now();
        stats.samples.push_back(Milliseconds(started, finished));
    }
    return stats;
}

void PrintStats(const char* label, const Stats& stats)
{
    std::vector<double> sorted = stats.samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t count = sorted.size();
    const double median = sorted[count / 2];
    const double p90 = sorted[std::min(count - 1, static_cast<size_t>(std::lround(0.9 * (count - 1))))];
    double sum = 0.0;
    for (double value : sorted) {
        sum += value;
    }
    std::printf("%s %zu frame(s), ms/frame:\n  ", label, count);
    for (size_t i = 0; i < stats.samples.size(); ++i) {
        std::printf("%.2f%s", stats.samples[i], i + 1 == stats.samples.size() ? "\n" : " ");
    }
    std::printf("  min %.2f  median %.2f  p90 %.2f  max %.2f  mean %.2f  (%.2f fps at the median)\n",
                sorted.front(), median, p90, sorted.back(), sum / static_cast<double>(count),
                1000.0 / median);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: bench-ep-frame <model.onnx> <W> <H> [--iterations N] [--warmup N] "
                    "[--threads N]\n");
        return 2;
    }
    const std::string model = argv[1];
    const int width = std::atoi(argv[2]);
    const int height = std::atoi(argv[3]);
    int iterations = 10;
    int warmup = 3;
    int threads = 1;
    for (int index = 4; index < argc; ++index) {
        if (std::strcmp(argv[index], "--iterations") == 0 && index + 1 < argc) {
            iterations = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--warmup") == 0 && index + 1 < argc) {
            warmup = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--threads") == 0 && index + 1 < argc) {
            threads = std::atoi(argv[++index]);
        }
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "bench-ep-frame");
    const std::vector<int64_t> shape{ 1, 3, height, width };
    std::vector<float> input = Input(static_cast<size_t>(3) * height * width);
    std::vector<float> outputCpu(input.size(), 0.0f);
    std::vector<float> outputCuda(input.size(), 0.0f);
    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::printf("model: %s  %dx%d, 3 channel(s), %zu values per frame\n", model.c_str(), width,
                height, input.size());
    std::printf("runtime: ONNX Runtime %s, API %d, linking the onnxruntime.dll beside this exe\n",
                Ort::GetVersionString().c_str(), ORT_API_VERSION);
    std::printf("intra-op threads %d, ORT_ENABLE_ALL, per-frame OrtValue over the host buffer\n\n",
                threads);

    const std::wstring wide(model.begin(), model.end());
    Ort::AllocatorWithDefaultOptions allocator;

    // CPU session, created first: its numbers must not carry the CUDA context.
    const auto cpuStarted = std::chrono::steady_clock::now();
    Ort::Session cpuSession(env, wide.c_str(), CpuSessionOptions(threads));
    const auto cpuCreated = std::chrono::steady_clock::now();
    const auto cpuInput = cpuSession.GetInputNameAllocated(0, allocator);
    const auto cpuOutput = cpuSession.GetOutputNameAllocated(0, allocator);
    std::printf("CPU session created in %.1f ms, input '%s' output '%s'\n",
                Milliseconds(cpuStarted, cpuCreated), cpuInput.get(), cpuOutput.get());

    const auto cpuFirstStarted = std::chrono::steady_clock::now();
    Frame(cpuSession, cpuInput.get(), cpuOutput.get(), memory, input, outputCpu, shape);
    std::printf("CPU first frame (untimed run): %.2f ms\n",
                Milliseconds(cpuFirstStarted, std::chrono::steady_clock::now()));

    // CUDA session, then the provider's own serialisation of its options.
    CudaOptions cudaOptions;
    const auto cudaStarted = std::chrono::steady_clock::now();
    Ort::Session cudaSession(env, wide.c_str(), CudaSessionOptions(cudaOptions, threads));
    const auto cudaCreated = std::chrono::steady_clock::now();
    const auto cudaInput = cudaSession.GetInputNameAllocated(0, allocator);
    const auto cudaOutput = cudaSession.GetOutputNameAllocated(0, allocator);
    std::printf("CUDA session created in %.1f ms, input '%s' output '%s'\n",
                Milliseconds(cudaStarted, cudaCreated), cudaInput.get(), cudaOutput.get());
    char* optionString = nullptr;
    if (api.GetCUDAProviderOptionsAsString(cudaOptions.handle, allocator, &optionString) == nullptr) {
        std::printf("CUDA provider options: %s\n", optionString);
        api.AllocatorFree(allocator, optionString);
    } else {
        std::printf("CUDA provider options: not readable on this runtime\n");
    }

    const auto cudaFirstStarted = std::chrono::steady_clock::now();
    Frame(cudaSession, cudaInput.get(), cudaOutput.get(), memory, input, outputCuda, shape);
    std::printf("CUDA first frame (untimed run): %.2f ms\n\n",
                Milliseconds(cudaFirstStarted, std::chrono::steady_clock::now()));

    // Did the CUDA row compute anything different from the CPU row? TF32 off
    // puts the difference at the rounding floor; a fallback would put it at zero.
    float worst = 0.0f;
    for (size_t i = 0; i < input.size(); ++i) {
        worst = std::max(worst, std::fabs(outputCuda[i] - outputCpu[i]));
    }
    std::printf("CUDA against CPU on the same frame: max |d| = %.3e\n\n", worst);

    // Two rounds, opposite order: a thermal or clock ramp that favours whichever
    // ran second would show as a difference between the rounds.
    std::printf("== round 1: CPU then CUDA ==\n");
    PrintStats("CPU ", Measure(cpuSession, cpuInput.get(), cpuOutput.get(), memory, input,
                               outputCpu, shape, warmup, iterations));
    PrintStats("CUDA", Measure(cudaSession, cudaInput.get(), cudaOutput.get(), memory, input,
                               outputCuda, shape, warmup, iterations));

    std::printf("\n== round 2: CUDA then CPU ==\n");
    PrintStats("CUDA", Measure(cudaSession, cudaInput.get(), cudaOutput.get(), memory, input,
                               outputCuda, shape, warmup, iterations));
    PrintStats("CPU ", Measure(cpuSession, cpuInput.get(), cpuOutput.get(), memory, input,
                               outputCpu, shape, warmup, iterations));
    return 0;
}
