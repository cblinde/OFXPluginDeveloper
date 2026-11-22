// SPDX-License-Identifier: BSD-3-Clause
//
// Host-side interface to the CUDA kernels.
//
// IMPORTANT: this header must remain includable by a plain C++ compiler with no
// CUDA Toolkit installed. Every CUDA-specific declaration lives inside
// OFXPLUGIN_WITH_CUDA, which CMake defines only when it found nvcc.
#ifndef MCOFX_KERNEL_CUH
#define MCOFX_KERNEL_CUH

#include <cstdint>

#ifdef OFXPLUGIN_WITH_CUDA

namespace mcofx {

/// Returned by every launcher. Zero means "the launch was enqueued"; any other
/// value is a cudaError_t value and can be turned into text with StatusMessage().
constexpr int kLaunchOK = 0;

/// True when a 16-byte vector access is legal for this pair of buffers.
///
/// The requirement is stronger than "RGBA32F is 16 bytes per pixel": the base
/// pointers the host hands out must themselves be 16-byte aligned, and both
/// pitches must be multiples of 16, or the second row onwards would be
/// misaligned even with an aligned base. Pitch is signed (OFX permits negative
/// rowBytes for flipped images); signed modulo still yields 0 for a negative
/// multiple of 16, which is what we want.
///
/// This lives in the header rather than in Kernel.cu so the decision can be
/// tested on a machine with no GPU (see the equivalence harness described in
/// BEST_PRACTICES_AND_PERFORMANCE.md).
inline bool IsVec4Addressable(const void* src, const void* dst,
                              std::int64_t srcPitch, std::int64_t dstPitch)
{
    constexpr std::uintptr_t kVec4Alignment = 16u;

    if (reinterpret_cast<std::uintptr_t>(src) % kVec4Alignment != 0) {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(dst) % kVec4Alignment != 0) {
        return false;
    }
    if (srcPitch % static_cast<std::int64_t>(kVec4Alignment) != 0) {
        return false;
    }
    if (dstPitch % static_cast<std::int64_t>(kVec4Alignment) != 0) {
        return false;
    }
    return true;
}

// The launcher interface uses C language linkage on purpose. Kernel.cu is
// compiled by nvcc (C++ host compiler) while Plugin.cpp may be compiled by a
// different host compiler; C linkage removes any dependence on both sides
// agreeing about name mangling, and it keeps the symbol greppable in the built
// module (`nm <bundle>.ofx | grep LaunchGainRGBA32F`).

/// Multiply-premultiplied RGBA32F by a per-channel gain.
///
/// @param hostStream  A cudaStream_t supplied by the OFX host, or nullptr to use
///                    the default stream. Never owned by this code.
/// @param src         Device pointer to the *first pixel of the render window*.
/// @param dst         Device pointer to the *first pixel of the render window*.
/// @param width       Render window width in pixels.
/// @param height      Render window height in pixels.
/// @param srcPitch    Bytes between consecutive scanlines of @p src. May be
///                    negative (flipped images are legal in OFX).
/// @param dstPitch    Bytes between consecutive scanlines of @p dst. May be negative.
/// @param gain        Per-channel multipliers, in R,G,B,A order.
extern "C" int LaunchGainRGBA32F(void* hostStream,
                                  const void* src,
                                  void* dst,
                                  int width,
                                  int height,
                                  std::int64_t srcPitch,
                                  std::int64_t dstPitch,
                                  const float gain[4]);

/// Blocks until the default stream is idle. Only call this when the host did NOT
/// hand you a cudaStream_t: the OFX contract then requires the render action to
/// return with its work complete. See ARCHITECTURE_AND_CUDA_PIPELINE.md.
extern "C" int WaitForDefaultStream();

/// Human readable form of a value returned by the two functions above.
extern "C" const char* StatusMessage(int status);

} // namespace mcofx

#endif // OFXPLUGIN_WITH_CUDA
#endif // MCOFX_KERNEL_CUH
