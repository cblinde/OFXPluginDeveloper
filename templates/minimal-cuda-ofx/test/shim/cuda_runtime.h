// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal stand-in for <cuda_runtime.h>, used only by the host test harness.
//
// The point is to compile the REAL device code in src/Kernel.cu with a host
// compiler so both kernels can be executed on the CPU and compared byte for
// byte. Nothing here emulates GPU memory: the kernels receive ordinary host
// pointers and the harness drives blockIdx/threadIdx itself.
//
// Keep this in sync with the CUDA built-ins the kernels actually use. If a
// kernel starts using something new (warp intrinsics, __shared__, atomics), it
// will fail to compile here - which is the intended signal that the harness can
// no longer cover that code path.
#ifndef MCOFX_TEST_CUDA_RUNTIME_SHIM_H
#define MCOFX_TEST_CUDA_RUNTIME_SHIM_H

#include <cstdint>

struct float4 {
    float x, y, z, w;
};

inline float4 make_float4(float x, float y, float z, float w)
{
    float4 v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

#define __global__
#define __device__
#define __forceinline__ inline
#define __restrict__

struct dim3 {
    unsigned int x, y, z;
    dim3(unsigned int a = 1, unsigned int b = 1, unsigned int c = 1)
        : x(a), y(b), z(c) {}
};

// Set by the harness before each emulated thread invocation.
extern dim3 blockIdx, blockDim, threadIdx;

// Device-management calls used by DeviceGuard. Compiling the guard proves it is
// exception-safe; the harness never constructs one.
typedef int cudaError_t;
constexpr int cudaSuccess = 0;
constexpr int cudaErrorInvalidValue = 1;

inline cudaError_t cudaGetDevice(int* d) { *d = 0; return cudaSuccess; }
inline cudaError_t cudaSetDevice(int) { return cudaSuccess; }

#endif // MCOFX_TEST_CUDA_RUNTIME_SHIM_H
