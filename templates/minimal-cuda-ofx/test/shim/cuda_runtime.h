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

#include <cmath>
#include <cstdint>
#include <cstring>

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

struct float2 {
    float x, y;
};

inline float2 make_float2(float x, float y)
{
    float2 v;
    v.x = x;
    v.y = y;
    return v;
}

// ---------------------------------------------------------------------------
// fp16: enough of cuda_fp16.h for the 16F kernel
// ---------------------------------------------------------------------------
//
// Storage is two bytes and every operation goes through float, which is what the
// kernel does as well: it converts, multiplies, converts back. The conversion is
// round-to-nearest-even - the mode of __float2half_rn and __floats2half2_rn - so
// the emulated kernel produces the same bits as a device one for every input a
// multiply can produce. A half multiply rounds an exact product once; a float
// multiply of two halves is exact (11+11 significand bits fit in 24) and the
// single conversion below rounds once, so the two agree bit for bit rather than
// approximately.
struct __half {
    unsigned short bits;
};

// 4-byte aligned like the CUDA type: a `__half2*` walks a row in 4-byte steps,
// and the emulated kernel must fault in the same places the real one does if a
// fixture ever hands it a misaligned row.
struct alignas(4) __half2 {
    __half x, y;
};

/// float -> half bits, round to nearest even, including the subs and the
/// overflow-to-infinity case.
inline unsigned short __float_to_half_bits(float value)
{
    unsigned int f;
    std::memcpy(&f, &value, 4);
    const unsigned int sign = (f >> 16) & 0x8000u;
    const unsigned int exponent = (f >> 23) & 0xffu;
    unsigned int mantissa = f & 0x7fffffu;

    if (exponent == 0xffu) { // infinity or NaN
        return static_cast<unsigned short>(sign | (mantissa ? 0x7e00u : 0x7c00u));
    }

    int newExponent = static_cast<int>(exponent) - 127 + 15;
    if (newExponent >= 0x1f) { // overflow
        return static_cast<unsigned short>(sign | 0x7c00u);
    }
    if (newExponent <= 0) { // subnormal half, or rounds to zero
        if (newExponent < -10) {
            return static_cast<unsigned short>(sign);
        }
        mantissa |= 0x800000u;
        const int shift = 14 - newExponent;
        const unsigned int kept = mantissa >> shift;
        const unsigned int rest = mantissa & ((1u << shift) - 1u);
        const unsigned int halfway = 1u << (shift - 1);
        unsigned int rounded = kept;
        if (rest > halfway || (rest == halfway && (kept & 1u) != 0)) {
            ++rounded;
        }
        return static_cast<unsigned short>(sign | rounded);
    }

    unsigned int kept = mantissa >> 13;
    const unsigned int rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (kept & 1u) != 0)) {
        ++kept;
        if (kept == 0x400u) { // carried into the exponent
            kept = 0;
            ++newExponent;
            if (newExponent >= 0x1f) {
                return static_cast<unsigned short>(sign | 0x7c00u);
            }
        }
    }
    return static_cast<unsigned short>(sign | (static_cast<unsigned int>(newExponent) << 10) | kept);
}

inline __half __float2half_rn(float value)
{
    __half h;
    h.bits = __float_to_half_bits(value);
    return h;
}

inline float __half2float(__half h)
{
    const unsigned int sign = (h.bits >> 15) & 1u;
    const unsigned int exponent = (h.bits >> 10) & 0x1fu;
    const unsigned int mantissa = h.bits & 0x3ffu;

    float value;
    if (exponent == 0) {
        value = mantissa == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1f) {
        value = mantissa == 0 ? HUGE_VALF : std::nanf("");
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u),
                           static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

inline __half2 __floats2half2_rn(float x, float y)
{
    __half2 v;
    v.x = __float2half_rn(x);
    v.y = __float2half_rn(y);
    return v;
}

inline __half2 __halves2half2(__half x, __half y)
{
    __half2 v;
    v.x = x;
    v.y = y;
    return v;
}

inline float2 __half22float2(__half2 h)
{
    return make_float2(__half2float(h.x), __half2float(h.y));
}

struct dim3 {
    unsigned int x, y, z;
    dim3(unsigned int a = 1, unsigned int b = 1, unsigned int c = 1)
        : x(a), y(b), z(c) {}
};

// Set by the harness before each emulated thread invocation. thread_local, like
// the real built-ins: two host threads emulating launches must not share these,
// otherwise the harness itself would be the data race.
extern thread_local dim3 blockIdx, blockDim, threadIdx;

// Device-management calls used by DeviceGuard. Compiling the guard proves it is
// exception-safe; the harness never constructs one.
typedef int cudaError_t;
constexpr int cudaSuccess = 0;
constexpr int cudaErrorInvalidValue = 1;

inline cudaError_t cudaGetDevice(int* d) { *d = 0; return cudaSuccess; }
inline cudaError_t cudaSetDevice(int) { return cudaSuccess; }
// Clears the sticky error the real runtime keeps; the guard in Kernel.cu calls
// it after a failed query so the launch reports its own failure.
inline cudaError_t cudaGetLastError() { return cudaSuccess; }

#endif // MCOFX_TEST_CUDA_RUNTIME_SHIM_H
