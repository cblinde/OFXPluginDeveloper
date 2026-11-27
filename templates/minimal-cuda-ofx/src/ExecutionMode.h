// SPDX-License-Identifier: BSD-3-Clause
//
// Which execution mode a render uses: CPU_ONLY, CUDA_ONLY or CPU_CUDA_HYBRID.
//
// This is policy only - no CUDA headers, no OFX headers - so the same code is
// compiled into the plug-in and into the host-side test in
// test/kernel_equivalence.cpp, which runs on a machine with no GPU. That is the
// same arrangement as IsVec4Addressable() in Kernel.cuh: a decision worth
// trusting is a decision a test can execute.
//
// The four gates, in order. Each one can only demote: a host that did not hand
// device pointers is not going to be talked into a CUDA render by an override,
// because the pixels are not on the device to begin with.
//
//   0. An explicit CPU_ONLY request always wins.
//   1. Host and device: without kOfxImageEffectPropCudaEnabled for this render,
//      or without a device that answers cudaMemGetInfo, the answer is CPU_ONLY.
//   2. VRAM: two frames (source and destination) plus 256 MiB of headroom must
//      be free on the device, or the render would either fail with
//      kOfxStatGPUOutOfMemory or thrash the driver's allocator.
//   3. Transfer balance: when the pixels are NOT already on the device and the
//      copy is estimated to cost more than 1.25x the kernel, CPU_ONLY - moving
//      a frame over PCIe to save nothing is the case the specification calls out.
//   4. Independent CPU work: CPU_CUDA_HYBRID needs work that can run while the
//      kernel does, and a kernel worth overlapping (>= 1 ms). Without both, the
//      answer is CUDA_ONLY - the host's own threads already handle the pixels.
//
// "Hybrid" here never means splitting one frame between CPU and GPU. OFX hands
// a render window to one process call and the Support library already bands the
// CPU path across nCPUs; splitting pixel work between the two engines adds a
// synchronisation point per tile and loses. The overlap is between the kernel
// and *independent* work (analysis, LUT or curve preparation, staging), which
// is what the specification asks for and what references/cuda-streams-and-
// memory.md §4 states in full.
#ifndef MCOFX_EXECUTION_MODE_H
#define MCOFX_EXECUTION_MODE_H

#include <cstdint>
#include <cstring>

namespace mcofx {

enum class ExecutionMode {
    Auto,          // decide from the facts below
    CpuOnly,       // the Support library's multiThreadProcessImages()
    CudaOnly,      // one grid on the host's stream (or the default stream)
    CpuCudaHybrid  // the kernel plus independent CPU work alongside it
};

inline const char* ModeName(ExecutionMode mode)
{
    switch (mode) {
    case ExecutionMode::CpuOnly: return "CPU_ONLY";
    case ExecutionMode::CudaOnly: return "CUDA_ONLY";
    case ExecutionMode::CpuCudaHybrid: return "CPU_CUDA_HYBRID";
    case ExecutionMode::Auto: break;
    }
    return "AUTO";
}

/// Parses OFXPLUGIN_GPU_MODE. `recognised` is false for an unknown word, so a
/// typo is reported instead of being read as Auto - a mode override that
/// silently does nothing is how a "we tested hybrid" claim gets made about a
/// run that never left CUDA_ONLY.
inline ExecutionMode ParseModeOverride(const char* text, bool* recognised)
{
    *recognised = true;
    if (text == nullptr || *text == '\0') return ExecutionMode::Auto;
    if (std::strcmp(text, "auto") == 0) return ExecutionMode::Auto;
    if (std::strcmp(text, "cpu") == 0) return ExecutionMode::CpuOnly;
    if (std::strcmp(text, "cuda") == 0) return ExecutionMode::CudaOnly;
    if (std::strcmp(text, "hybrid") == 0) return ExecutionMode::CpuCudaHybrid;
    *recognised = false;
    return ExecutionMode::Auto;
}

/// What the render knows about itself when it has to choose.
struct FrameFacts {
    /// kOfxImageEffectPropCudaEnabled for this render: the host says the image
    /// pointers are device pointers. This is the only authority on where the
    /// pixels live; the descriptor's "supports CUDA" string is not.
    bool hostHandedDevicePointers = false;
    /// cudaGetDeviceCount()/cudaMemGetInfo() succeeded.
    bool deviceQueryable = false;
    std::int64_t deviceFreeBytes = 0;
    std::int64_t frameBytes = 0;      // one image, at this render's depth
    /// Pixels already on the device (the OFX GPU path): no Host->Device copy.
    bool inputOnDevice = true;
    /// Work that can run while the kernel does, and that is not pixel work.
    bool hasIndependentCpuTask = false;
    /// Microseconds-scale cost of the kernel, 0 when not yet known.
    double estimatedKernelMs = 0.0;
    /// Host->Device plus Device->Host for this frame, 0 when not applicable.
    double estimatedCopyMs = 0.0;
};

struct ModeDecision {
    ExecutionMode mode;
    /// A static string, logged once per render: the reason is the useful part
    /// of the decision when a host renders slower than the user expects.
    const char* reason;
};

/// The four gates. Both callers (the plug-in and the test) use this one
/// function; there is no second copy of the rules.
inline ModeDecision DecideMode(ExecutionMode requested, const FrameFacts& facts)
{
    constexpr std::int64_t kHeadroomBytes = 256LL * 1024LL * 1024LL;

    if (requested == ExecutionMode::CpuOnly) {
        return { ExecutionMode::CpuOnly, "CPU_ONLY requested" };
    }
    if (!facts.hostHandedDevicePointers) {
        return { ExecutionMode::CpuOnly,
                 "the host did not set kOfxImageEffectPropCudaEnabled, so the pixels are host pointers" };
    }
    if (!facts.deviceQueryable) {
        return { ExecutionMode::CpuOnly, "no CUDA device answered cudaMemGetInfo" };
    }
    if (facts.deviceFreeBytes < 2 * facts.frameBytes + kHeadroomBytes) {
        return { ExecutionMode::CpuOnly,
                 "free device memory is below two frames plus 256 MiB" };
    }
    if (!facts.inputOnDevice && facts.estimatedKernelMs > 0.0 &&
        facts.estimatedCopyMs > 1.25 * facts.estimatedKernelMs) {
        return { ExecutionMode::CpuOnly,
                 "the Host<->Device copy is estimated to cost more than the kernel" };
    }
    if (requested == ExecutionMode::CpuCudaHybrid) {
        return { ExecutionMode::CpuCudaHybrid, "CPU_CUDA_HYBRID requested" };
    }
    if (requested == ExecutionMode::CudaOnly) {
        return { ExecutionMode::CudaOnly, "CUDA_ONLY requested" };
    }
    if (facts.hasIndependentCpuTask && facts.estimatedKernelMs >= 1.0) {
        return { ExecutionMode::CpuCudaHybrid,
                 "a kernel of 1 ms or more with independent CPU work to overlap" };
    }
    return { ExecutionMode::CudaOnly, "no independent CPU work to overlap" };
}

} // namespace mcofx

#endif // MCOFX_EXECUTION_MODE_H
