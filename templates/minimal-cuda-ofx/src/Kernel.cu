// SPDX-License-Identifier: BSD-3-Clause
//
// CUDA device code. Compiled only when OFXPLUGIN_WITH_CUDA is defined.
#include "Kernel.cuh"

#ifdef OFXPLUGIN_WITH_CUDA

#include <cuda_fp16.h>
#include <cuda_runtime.h>

namespace {

/// The arithmetic, in one place, so the vector and scalar kernels cannot drift
/// apart. Both callers apply exactly these four multiplies in this order.
__device__ __forceinline__ float4 ScalePixel(const float4 v, const float4 gain)
{
    return make_float4(v.x * gain.x, v.y * gain.y, v.z * gain.z, v.w * gain.w);
}

/// One thread per pixel, one 16-byte load and one 16-byte store per pixel.
///
/// Only selected when both bases and both pitches are 16-byte addressable
/// (see IsVec4Addressable in Kernel.cuh): a float4 access on a pointer the host
/// allocated with 4-byte alignment faults at run time with "misaligned
/// address", so the launcher decides rather than assuming.
///
/// Addressing still goes through the host-supplied pitch in BYTES, because the
/// host may pad scanlines for alignment and rowBytes may be negative for
/// vertically flipped images.
__global__ void GainKernelRGBA32FVec4(const float* __restrict__ src,
                                      float* __restrict__ dst,
                                      const int width,
                                      const int height,
                                      const long long srcPitch,
                                      const long long dstPitch,
                                      const float4 gain)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    // Boundary check: the grid is rounded up to whole blocks. Width and height
    // need not be multiples of anything - the tail is simply not addressed.
    if (x >= width || y >= height) {
        return;
    }

    const float4* s = reinterpret_cast<const float4*>(
        reinterpret_cast<const char*>(src) + static_cast<long long>(y) * srcPitch);
    float4* d = reinterpret_cast<float4*>(
        reinterpret_cast<char*>(dst) + static_cast<long long>(y) * dstPitch);

    // 1x LDG.E.128 + 1x STG.E.128 (verified in PTX; see
    // references/performance-model.md section 6.1).
    d[x] = ScalePixel(s[x], gain);
}

/// Fallback for pointers or pitches that are not 16-byte addressable: four
/// scalar loads and four scalar stores per pixel. Same arithmetic, so the two
/// paths are bit-identical by construction, not by hope.
__global__ void GainKernelRGBA32FScalar(const float* __restrict__ src,
                                        float* __restrict__ dst,
                                        const int width,
                                        const int height,
                                        const long long srcPitch,
                                        const long long dstPitch,
                                        const float4 gain)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const float* s = reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(src) + static_cast<long long>(y) * srcPitch) + 4LL * x;
    float* d = reinterpret_cast<float*>(
        reinterpret_cast<char*>(dst) + static_cast<long long>(y) * dstPitch) + 4LL * x;

    d[0] = s[0] * gain.x;
    d[1] = s[1] * gain.y;
    d[2] = s[2] * gain.z;
    d[3] = s[3] * gain.w;
}

/// The 16F arithmetic: convert both operands to float, multiply, convert back.
///
/// Both operands are halves, so the product is exact in float (11 + 11
/// significand bits fit in 24) and the single conversion rounds once - the same
/// rounding `__hmul2` applies, so this agrees with half arithmetic bit for bit.
/// That is what lets the emulated harness compare against a float reference
/// without a tolerance.
__device__ __forceinline__ __half2 ScaleHalf2(const __half2 v, const __half2 gain)
{
    const float2 f = __half22float2(v);
    const float2 g = __half22float2(gain);
    return __floats2half2_rn(f.x * g.x, f.y * g.y);
}

/// Half-precision RGBA: four `__half` per pixel, 8 bytes, two `__half2` loads
/// and two stores per pixel.
///
/// Launched by LaunchGainRGBA16F, which the plug-in reaches for when the host
/// hands over an RGBA16F image. What the kernel has to get right beyond the
/// arithmetic is the addressing: an odd width that a float4 access could not
/// take, a padded pitch, and a negative row stride.
__global__ void GainKernelRGBA16FHalf4(const __half* __restrict__ src,
                                       __half* __restrict__ dst,
                                       const int width,
                                       const int height,
                                       const long long srcPitch,
                                       const long long dstPitch,
                                       const __half2 gainRG,
                                       const __half2 gainBA)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const __half2* s = reinterpret_cast<const __half2*>(
        reinterpret_cast<const char*>(src) + static_cast<long long>(y) * srcPitch);
    __half2* d = reinterpret_cast<__half2*>(
        reinterpret_cast<char*>(dst) + static_cast<long long>(y) * dstPitch);

    // Pixel x occupies half2 slots 2x (R,G) and 2x+1 (B,A).
    d[2 * x] = ScaleHalf2(s[2 * x], gainRG);
    d[2 * x + 1] = ScaleHalf2(s[2 * x + 1], gainBA);
}

/// The 8-bit arithmetic: unpack a stored byte, multiply, quantise back.
///
/// OFX does not define the encoding of a byte image - that is the host's colour
/// management, and a plug-in that guesses it double-applies a transfer function.
/// So the multiply happens on the stored code value, the number the host would
/// have multiplied itself, with no linearisation and no gamma. The round trip at
/// gain 1.0 is the identity, which is what a "colour space aware" byte path is
/// not allowed to break.
///
/// Quantisation is round-to-nearest-even (`__float2int_rn`, the same mode as the
/// 16F kernel's conversion) and then a clamp to [0, 255]. Overflow saturates
/// instead of wrapping: a gain of 8 on a byte is 255, not 232.
__device__ __forceinline__ unsigned char ScaleByte(const unsigned char v, const float gain)
{
    const float scaled = static_cast<float>(v) * gain;
    const int rounded = __float2int_rn(scaled);
    return static_cast<unsigned char>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

/// 8-bit RGBA: four bytes per pixel, addressed through the host's byte pitch.
///
/// Unlike the float4 and `__half2` kernels there is no alignment predicate: a
/// byte pixel is four bytes and every pointer/pitch a host can hand out is
/// byte-addressable, so an odd pitch and an odd pixel offset are legal. That is
/// also why an 8-bit buffer cannot be mistaken for a 32F one by address alone -
/// the depth has to come from the image property, never from the pointer.
__global__ void GainKernelRGBA8(const unsigned char* __restrict__ src,
                                unsigned char* __restrict__ dst,
                                const int width,
                                const int height,
                                const long long srcPitch,
                                const long long dstPitch,
                                const float4 gain)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    const long long offset = static_cast<long long>(y) * srcPitch + static_cast<long long>(x) * 4;
    const unsigned char* s = src + offset;
    unsigned char* d = dst + static_cast<long long>(y) * dstPitch + static_cast<long long>(x) * 4;

    d[0] = ScaleByte(s[0], gain.x);
    d[1] = ScaleByte(s[1], gain.y);
    d[2] = ScaleByte(s[2], gain.z);
    d[3] = ScaleByte(s[3], gain.w);
}

/// Restores the thread's previous CUDA device on scope exit. Without this, a
/// plugin that switches devices for a multi-GPU host leaks that choice into
/// every later action on the same thread.
class DeviceGuard
{
public:
    explicit DeviceGuard(int device)
        : _previous(0)
        , _active(false)
    {
        // No previous device means nothing to restore. Clear the sticky error
        // so the launch below reports the real failure, not this one twice.
        if (cudaGetDevice(&_previous) != cudaSuccess) {
            cudaGetLastError();
            return;
        }
        _active = true;
        if (device != _previous && cudaSetDevice(device) != cudaSuccess) {
            // The switch failed, so the previous device is still current and
            // the destructor must not "restore" anything.
            cudaGetLastError();
            _active = false;
        }
    }

    ~DeviceGuard()
    {
        if (_active && _previous != _current()) {
            cudaSetDevice(_previous);
        }
    }

    DeviceGuard(const DeviceGuard&) = delete;
    DeviceGuard& operator=(const DeviceGuard&) = delete;

private:
    static int _current()
    {
        int d = 0;
        cudaGetDevice(&d);
        return d;
    }

    int _previous;
    bool _active;
};

} // namespace

namespace mcofx {

extern "C" int LaunchGainRGBA32F(void* hostStream,
                                  const void* src,
                                  void* dst,
                                  int width,
                                  int height,
                                  std::int64_t srcPitch,
                                  std::int64_t dstPitch,
                                  const float gain[4])
{
    if (width <= 0 || height <= 0) {
        return kLaunchOK;
    }
    if (src == nullptr || dst == nullptr) {
        return static_cast<int>(cudaErrorInvalidValue);
    }

    // Bind to the device that actually owns the host's buffer. In a multi-GPU
    // workstation the host may be rendering on GPU 1 while this thread's
    // current device is 0; launching there would fault on the pointer.
    int device = 0;
    cudaGetDevice(&device);
    cudaPointerAttributes attributes{};
    const cudaError_t query = cudaPointerGetAttributes(&attributes, src);
    if (query == cudaSuccess && attributes.device >= 0) {
        device = attributes.device;
    } else {
        // Not a registered device pointer (host-mapped or managed memory, or an
        // older driver): fall through on the current device.
        cudaGetLastError();
    }
    const DeviceGuard guard(device);

    const dim3 block(16, 16, 1);
    const dim3 grid(static_cast<unsigned>((width + block.x - 1) / block.x),
                    static_cast<unsigned>((height + block.y - 1) / block.y),
                    1);

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(hostStream);
    const float4 g = make_float4(gain[0], gain[1], gain[2], gain[3]);
    const long long sp = static_cast<long long>(srcPitch);
    const long long dp = static_cast<long long>(dstPitch);

    // Choose the vector path only when it is actually safe. The host owns these
    // allocations; nothing in the OFX contract promises more than natural
    // alignment for the pixel format, and a float4 load from a 4-byte-aligned
    // buffer is a runtime fault, not a compile error.
    if (IsVec4Addressable(src, dst, srcPitch, dstPitch)) {
        GainKernelRGBA32FVec4<<<grid, block, 0, stream>>>(
            static_cast<const float*>(src),
            static_cast<float*>(dst),
            width,
            height,
            sp,
            dp,
            g);
    } else {
        GainKernelRGBA32FScalar<<<grid, block, 0, stream>>>(
            static_cast<const float*>(src),
            static_cast<float*>(dst),
            width,
            height,
            sp,
            dp,
            g);
    }

    // Immediate launch errors (bad grid, no device, invalid pointer kind).
    // Asynchronous execution errors are only observable at a synchronisation
    // point, which is precisely why the sync policy matters (see header).
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) {
        return static_cast<int>(launch);
    }
    return kLaunchOK;
}

extern "C" int LaunchGainRGBA16F(void* hostStream,
                                 const void* src,
                                 void* dst,
                                 int width,
                                 int height,
                                 std::int64_t srcPitch,
                                 std::int64_t dstPitch,
                                 const float gain[4])
{
    if (width <= 0 || height <= 0) {
        return kLaunchOK;
    }
    if (src == nullptr || dst == nullptr) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    // `__half2` is 4 bytes wide, so both bases and both pitches must be
    // 4-byte addressable. There is no scalar fallback on purpose: a half image
    // from a real host always is, and a silent second kernel would be one more
    // path nobody runs.
    if (!IsHalf2Addressable(src, dst, srcPitch, dstPitch)) {
        return static_cast<int>(cudaErrorInvalidValue);
    }

    int device = 0;
    cudaGetDevice(&device);
    cudaPointerAttributes attributes{};
    const cudaError_t query = cudaPointerGetAttributes(&attributes, src);
    if (query == cudaSuccess && attributes.device >= 0) {
        device = attributes.device;
    } else {
        cudaGetLastError();
    }
    const DeviceGuard guard(device);

    const dim3 block(16, 16, 1);
    const dim3 grid(static_cast<unsigned>((width + block.x - 1) / block.x),
                    static_cast<unsigned>((height + block.y - 1) / block.y),
                    1);

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(hostStream);
    const long long sp = static_cast<long long>(srcPitch);
    const long long dp = static_cast<long long>(dstPitch);

    // The gain goes over as halves because the kernel multiplies half2 pairs:
    // converting here means the rounding happens once, not per pixel.
    GainKernelRGBA16FHalf4<<<grid, block, 0, stream>>>(
        static_cast<const __half*>(src),
        static_cast<__half*>(dst),
        width,
        height,
        sp,
        dp,
        __floats2half2_rn(gain[0], gain[1]),
        __floats2half2_rn(gain[2], gain[3]));

    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) {
        return static_cast<int>(launch);
    }
    return kLaunchOK;
}

extern "C" int LaunchGainRGBA8(void* hostStream,
                               const void* src,
                               void* dst,
                               int width,
                               int height,
                               std::int64_t srcPitch,
                               std::int64_t dstPitch,
                               const float gain[4])
{
    if (width <= 0 || height <= 0) {
        return kLaunchOK;
    }
    if (src == nullptr || dst == nullptr) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    // No alignment predicate: a byte pixel is four bytes and every pointer and
    // pitch a host can hand out is byte addressable, which is why an odd pitch
    // and an odd pixel offset are legal here.

    int device = 0;
    cudaGetDevice(&device);
    cudaPointerAttributes attributes{};
    const cudaError_t query = cudaPointerGetAttributes(&attributes, src);
    if (query == cudaSuccess && attributes.device >= 0) {
        device = attributes.device;
    } else {
        cudaGetLastError();
    }
    const DeviceGuard guard(device);

    const dim3 block(16, 16, 1);
    const dim3 grid(static_cast<unsigned>((width + block.x - 1) / block.x),
                    static_cast<unsigned>((height + block.y - 1) / block.y),
                    1);

    cudaStream_t stream = reinterpret_cast<cudaStream_t>(hostStream);
    const float4 g = make_float4(gain[0], gain[1], gain[2], gain[3]);

    GainKernelRGBA8<<<grid, block, 0, stream>>>(
        static_cast<const unsigned char*>(src),
        static_cast<unsigned char*>(dst),
        width,
        height,
        static_cast<long long>(srcPitch),
        static_cast<long long>(dstPitch),
        g);

    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) {
        return static_cast<int>(launch);
    }
    return kLaunchOK;
}

extern "C" bool DeviceMemoryFreeBytes(std::int64_t* freeBytes, std::int64_t* totalBytes)
{
    if (freeBytes == nullptr || totalBytes == nullptr) {
        return false;
    }
    std::size_t freeNow = 0;
    std::size_t totalNow = 0;
    if (cudaMemGetInfo(&freeNow, &totalNow) != cudaSuccess) {
        return false;
    }
    *freeBytes = static_cast<std::int64_t>(freeNow);
    *totalBytes = static_cast<std::int64_t>(totalNow);
    return true;
}

int WaitForDefaultStream()
{
    return static_cast<int>(cudaStreamSynchronize(nullptr));
}

extern "C" const char* StatusMessage(int status)
{
    return cudaGetErrorString(static_cast<cudaError_t>(status));
}

} // namespace mcofx

#endif // OFXPLUGIN_WITH_CUDA
