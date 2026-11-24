// SPDX-License-Identifier: BSD-3-Clause
//
// CUDA device code. Compiled only when OFXPLUGIN_WITH_CUDA is defined.
#include "Kernel.cuh"

#ifdef OFXPLUGIN_WITH_CUDA

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

extern "C" int WaitForDefaultStream()
{
    return static_cast<int>(cudaStreamSynchronize(nullptr));
}

extern "C" const char* StatusMessage(int status)
{
    return cudaGetErrorString(static_cast<cudaError_t>(status));
}

} // namespace mcofx

#endif // OFXPLUGIN_WITH_CUDA
