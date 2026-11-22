// SPDX-License-Identifier: BSD-3-Clause
//
// One ONNX Runtime session, hidden behind a pimpl so that nothing else in the
// plug-in has to include onnxruntime_cxx_api.h (or agree with it about ABI).
//
// Design rules, each with a reason:
//
//  * The session is created once and reused for every render. Creating a
//    session parses the graph, resolves the execution provider and allocates
//    arenas; doing that per frame is the single largest avoidable cost in an
//    inference plug-in.
//  * `intraThreads` is explicit and defaults to 1. ONNX Runtime's default is 0,
//    which means "one thread per physical core" per session. OFX hosts call
//    render() on several threads at once, so the default multiplies the host's
//    thread count by the core count and oversubscribes the machine.
//  * Run() is called concurrently from host render threads. ORT documents that
//    several threads may call Run() on one session; kernels are stateless. The
//    guard rail is that this class keeps no per-call state.
//  * Nothing here touches CUDA. A CUDA execution provider needs the host's
//    stream (kOfxImageEffectPropCudaStream) to be useful; see
//    AI_MODEL_INTEGRATION.md for the exact recipe and why it is not compiled
//    into this template.

#ifndef AIOFX_MODEL_RUNNER_H
#define AIOFX_MODEL_RUNNER_H

#include <memory>
#include <string>

namespace aiofx {

class ModelRunner
{
public:
    /// Loads `modelPath`. Throws std::runtime_error with the ORT message on
    /// failure (missing file, unsupported op, provider unavailable).
    ModelRunner(const std::string& modelPath, int intraThreads);
    ~ModelRunner();

    ModelRunner(const ModelRunner&) = delete;
    ModelRunner& operator=(const ModelRunner&) = delete;

    /// Planar float32 NCHW, [1, channels, height, width] in and out.
    /// `input` and `output` must each hold channels*height*width floats.
    /// Thread-safe: no state is kept between calls.
    void Run(const float* input, int channels, int height, int width, float* output);

    /// Input channel count the model was exported with (3 for the template).
    int InputChannels() const;

    const std::string& InputName() const;
    const std::string& OutputName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace aiofx

#endif // AIOFX_MODEL_RUNNER_H
