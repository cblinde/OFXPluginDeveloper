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
/// references/performance-model.md).
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

/// True when a 4-byte `__half2` access is legal for this pair of buffers.
///
/// A 16F pixel is four halves = 8 bytes = two `__half2`, so the row base and
/// the pitch have to be 4-byte addressable - half of what the RGBA32F vec4 path
/// needs, which is why the 16F kernel can take an odd width where a float4
/// access could not. Same signed-pitch rule as above: a negative multiple of 4
/// is still selectable.
inline bool IsHalf2Addressable(const void* src, const void* dst,
                               std::int64_t srcPitch, std::int64_t dstPitch)
{
    constexpr std::uintptr_t kHalf2Alignment = 4u;

    if (reinterpret_cast<std::uintptr_t>(src) % kHalf2Alignment != 0) {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(dst) % kHalf2Alignment != 0) {
        return false;
    }
    if (srcPitch % static_cast<std::int64_t>(kHalf2Alignment) != 0) {
        return false;
    }
    if (dstPitch % static_cast<std::int64_t>(kHalf2Alignment) != 0) {
        return false;
    }
    return true;
}

// The launcher interface uses C language linkage on purpose. Kernel.cu is
// compiled by nvcc (C++ host compiler) while Plugin.cpp may be compiled by a
// different host compiler; C linkage removes any dependence on both sides
// agreeing about name mangling, and it keeps the symbol greppable in the built
// module (`nm <bundle>.ofx | grep LaunchGainRGBA`, which `build.sh` runs per
// launcher and requires all three of).

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

/// Multiply-premultiplied RGBA16F by a per-channel gain, written as halves.
///
/// Same contract as LaunchGainRGBA32F, with two differences that come from the
/// pixel format rather than from taste: the arithmetic converts to float and
/// back with round-to-nearest-even (so it agrees with a half multiply bit for
/// bit, see ScaleHalf2), and there is no scalar fallback. A minimum of 4-byte
/// addressability is what a `__half2` access needs, so a buffer that is not
/// 4-byte addressable is refused with cudaErrorInvalidValue instead of faulting:
/// the plug-in can then report kOfxStatGPURenderFailed rather than corrupt a
/// frame. In practice every host allocation and every rowBytes a host derives
/// from a half image satisfies this.
extern "C" int LaunchGainRGBA16F(void* hostStream,
                                 const void* src,
                                 void* dst,
                                 int width,
                                 int height,
                                 std::int64_t srcPitch,
                                 std::int64_t dstPitch,
                                 const float gain[4]);

/// Multiply RGBA8 by a per-channel gain, on the stored code values.
///
/// OFX does not define what a byte image's code values mean - that is the host's
/// colour management - so this multiplies the stored byte, rounds to nearest and
/// saturates at 255, exactly what ScaleByte does on the device. Same contract as
/// LaunchGainRGBA32F; no alignment predicate, because a byte is addressable
/// everywhere a host can put one.
extern "C" int LaunchGainRGBA8(void* hostStream,
                               const void* src,
                               void* dst,
                               int width,
                               int height,
                               std::int64_t srcPitch,
                               std::int64_t dstPitch,
                               const float gain[4]);

/// Free and total device memory, from cudaMemGetInfo().
///
/// Returns false when the query fails (no device, no context, a driver that
/// will not answer), and the caller then treats the device as unusable: the
/// execution-mode gates demote to CPU_ONLY rather than assume there is room.
/// See src/ExecutionMode.h and references/cuda-streams-and-memory.md §4.
extern "C" bool DeviceMemoryFreeBytes(std::int64_t* freeBytes,
                                        std::int64_t* totalBytes);

/// Blocks until the default stream is idle. Only call this when the host did NOT
/// hand you a cudaStream_t: the OFX contract then requires the render action to
/// return with its work complete. See references/ofx-lifecycle-and-threads.md.
extern "C" int WaitForDefaultStream();

/// Human readable form of a value returned by the two functions above.
extern "C" const char* StatusMessage(int status);

} // namespace mcofx

#endif // OFXPLUGIN_WITH_CUDA
#endif // MCOFX_KERNEL_CUH
