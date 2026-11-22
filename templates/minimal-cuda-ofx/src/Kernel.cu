// SPDX-License-Identifier: BSD-3-Clause
//
// CUDA device code. Compiled only when OFXPLUGIN_WITH_CUDA is defined.
#include "Kernel.cuh"

#ifdef OFXPLUGIN_WITH_CUDA

#include <cuda_runtime.h>

namespace {

/// One thread per pixel. Addressing is done through the host-supplied pitch in
/// BYTES rather than assuming width * sizeof(pixel), because:
///   * the host may pad scanlines for alignment (Resolve renders 208x117
///     thumbnails through a 256-byte aligned allocator), and
///   * rowBytes may be negative for vertically flipped images.
__global__ void GainKernelRGBA32F(const float* __restrict__ src,
                                  float* __restrict__ dst,
                                  const int width,
                                  const int height,
                                  const long long srcPitch,
                                  const long long dstPitch,
                                  const float4 gain)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    // Boundary check: the grid is rounded up to whole blocks.
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
    {
        cudaGetDevice(&_previous);
        if (device != _previous) {
            cudaSetDevice(device);
        }
    }

    ~DeviceGuard()
    {
        if (_previous != _current()) {
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
};

} // namespace

namespace mcofx {

int LaunchGainRGBA32F(void* hostStream,
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
    GainKernelRGBA32F<<<grid, block, 0, stream>>>(
        static_cast<const float*>(src),
        static_cast<float*>(dst),
        width,
        height,
        static_cast<long long>(srcPitch),
        static_cast<long long>(dstPitch),
        make_float4(gain[0], gain[1], gain[2], gain[3]));

    // Immediate launch errors (bad grid, no device, invalid pointer kind).
    // Asynchronous execution errors are only observable at a synchronisation
    // point, which is precisely why the sync policy matters (see header).
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) {
        return static_cast<int>(launch);
    }
    return kLaunchOK;
}

int WaitForDefaultStream()
{
    return static_cast<int>(cudaStreamSynchronize(nullptr));
}

const char* StatusMessage(int status)
{
    return cudaGetErrorString(static_cast<cudaError_t>(status));
}

} // namespace mcofx

#endif // OFXPLUGIN_WITH_CUDA
