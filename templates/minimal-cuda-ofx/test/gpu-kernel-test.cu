// SPDX-License-Identifier: BSD-3-Clause
//
// Real-GPU harness for the minimal-cuda-ofx template.
//
// The template's own suite (test/run-tests.sh) emulates the kernel bodies on the
// CPU through test/shim/cuda_runtime.h. This harness does the opposite: it
// compiles the real src/Kernel.cu and executes it on actual hardware, which is
// what SKILL.md section 12 used to list as the one thing never verified on
// hardware. It now is, on one GPU: run it to reproduce or to measure a new one.
//
// Covered here and nowhere else in the repository:
//   1. Device binding in the launcher (cudaPointerGetAttributes + DeviceGuard).
//   2. Vec4-vs-scalar routing for pointers the host actually hands out, including
//      4-byte-aligned bases where a float4 load would fault.
//   3. Host-pitch arithmetic on hardware: padded scanlines and negative
//      (flipped) pitch.
//   4. Stream semantics: a host-supplied stream, the default stream, and the
//      WaitForDefaultStream() completion contract from Plugin.cpp.
//   5. Concurrent launches from several host threads on disjoint buffers.
//   6. Zero-copy on host-mapped (pinned, device-addressable) memory.
//   7. Throughput for both kernels at 1080p/4K/8K, the CPU loop they replace,
//      and the cost of the copy a zero-copy path avoids - so "one 16-byte load
//      and one 16-byte store per pixel" stops being a PTX-only claim.
//
// Build (RTX 3060 = sm_86):
//   nvcc -DOFXPLUGIN_WITH_CUDA=1 -arch=sm_86 -O2 -std=c++17 \
//        test/gpu-kernel-test.cu -o build/gpu-kernel-test.exe
//
// Exit status 0 only if every case matches the CPU reference bit for bit.

#include "../src/Kernel.cu" // the real kernels AND the real launcher

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_cases = 0;

constexpr int kPixelBytes = 16;
const float kGain[4] = { 1.75f, 0.5f, 2.25f, 1.0f };

void fail(const char* what, const char* name, int w, int h, long long sp, long long dp, int row)
{
    ++g_failures;
    if (g_failures <= 25) {
        std::printf("  FAIL %-22s %-32s w=%d h=%d sPitch=%lld dPitch=%lld row=%d\n",
                    what, name, w, h, sp, dp, row);
    }
}

float nextFloat(unsigned int& seed)
{
    seed = seed * 1664525u + 1013904223u;
    const unsigned int bits = (seed >> 9) | 0x3f000000u;
    float f;
    std::memcpy(&f, &bits, 4);
    return (f - 1.0f) * 4.0f;
}

float* asFloat(unsigned char* p)
{
    return reinterpret_cast<float*>(p); // cppcheck-suppress invalidPointerCast
}

long long absLL(long long v) { return v < 0 ? -v : v; }

struct Fixture {
    int boundsW = 0;
    int boundsH = 0;
    int ox = 0;
    int oy = 0;
    int w = 0;
    int h = 0;
    long long sp = 0;
    long long dp = 0;
    size_t srcBytes = 0;
    size_t dstBytes = 0;
    unsigned char* srcAlloc = nullptr;
    unsigned char* dstAlloc = nullptr;
    unsigned char* srcBase = nullptr;
    unsigned char* dstBase = nullptr;
    unsigned char* srcHost = nullptr;
    unsigned char* dstHost = nullptr;
};

/// A negative pitch puts the image origin at the END of the buffer: walking down
/// the image walks down in memory. That is OFX's flipped-image case.
unsigned char* imageBase(unsigned char* alloc, long long pitch, int boundsH, int shift)
{
    if (pitch >= 0) {
        return alloc + shift;
    }
    return alloc + shift + static_cast<size_t>(-pitch) * static_cast<size_t>(boundsH - 1);
}

size_t bufferBytes(long long pitch, int boundsH, int shift)
{
    return static_cast<size_t>(absLL(pitch)) * static_cast<size_t>(boundsH) +
           static_cast<size_t>(shift) + 512;
}

bool makeFixture(const char* name, int w, int h, int ox, int oy, int padX,
                 long long srcPitchOverride, long long dstPitchOverride,
                 int srcShift, int dstShift, Fixture& f)
{
    f.w = w;
    f.h = h;
    f.ox = ox;
    f.oy = oy;
    f.boundsW = ox + w + 2;
    f.boundsH = oy + h + 2;
    const long long natural = static_cast<long long>(f.boundsW) * kPixelBytes + padX;
    f.sp = srcPitchOverride != 0 ? srcPitchOverride : natural;
    f.dp = dstPitchOverride != 0 ? dstPitchOverride : natural;
    f.srcBytes = bufferBytes(f.sp, f.boundsH, srcShift);
    f.dstBytes = bufferBytes(f.dp, f.boundsH, dstShift);

    if (cudaMalloc(reinterpret_cast<void**>(&f.srcAlloc), f.srcBytes) != cudaSuccess ||
        cudaMalloc(reinterpret_cast<void**>(&f.dstAlloc), f.dstBytes) != cudaSuccess) {
        std::printf("  FAIL %s: cudaMalloc: %s\n", name, cudaGetErrorString(cudaGetLastError()));
        ++g_failures;
        return false;
    }
    f.srcHost = static_cast<unsigned char*>(std::calloc(f.srcBytes, 1));
    f.dstHost = static_cast<unsigned char*>(std::calloc(f.dstBytes, 1));
    f.srcBase = imageBase(f.srcHost, f.sp, f.boundsH, srcShift);
    f.dstBase = imageBase(f.dstHost, f.dp, f.boundsH, dstShift);
    return true;
}

void freeFixture(Fixture& f)
{
    if (f.srcAlloc) {
        cudaFree(f.srcAlloc);
    }
    if (f.dstAlloc) {
        cudaFree(f.dstAlloc);
    }
    std::free(f.srcHost);
    std::free(f.dstHost);
    f.srcAlloc = f.dstAlloc = nullptr;
    f.srcHost = f.dstHost = nullptr;
}

void fillPattern(Fixture& f, unsigned int seed)
{
    for (int y = 0; y < f.boundsH; ++y) {
        for (int x = 0; x < f.boundsW; ++x) {
            float* p = asFloat(f.srcBase + static_cast<long long>(y) * f.sp +
                               static_cast<long long>(x) * kPixelBytes);
            for (int c = 0; c < 4; ++c) {
                p[c] = nextFloat(seed);
            }
        }
        const long long rowEnd = static_cast<long long>(f.boundsW) * kPixelBytes;
        if (absLL(f.sp) > rowEnd) {
            unsigned char* pad = f.srcBase + static_cast<long long>(y) * f.sp + rowEnd;
            std::memset(pad, 0xCD, static_cast<size_t>(absLL(f.sp) - rowEnd));
        }
    }
}

/// Byte-for-byte comparison of the device result against the same arithmetic run
/// on the host over the same pitch layout, plus an overrun check: every byte the
/// kernel is not allowed to touch must still hold the poison value.
void verifyPixels(const char* name, const Fixture& f)
{
    const long long windowBytes = static_cast<long long>(f.w) * kPixelBytes;
    for (int y = f.oy; y < f.oy + f.h; ++y) {
        const unsigned char* got = f.dstBase + static_cast<long long>(y) * f.dp +
                                   static_cast<long long>(f.ox) * kPixelBytes;
        const float* src = reinterpret_cast<const float*>(f.srcBase +
                                                          static_cast<long long>(y) * f.sp +
                                                          static_cast<long long>(f.ox) * kPixelBytes);
        for (int x = 0; x < f.w; ++x) {
            const float* g = reinterpret_cast<const float*>(got + static_cast<long long>(x) * kPixelBytes);
            for (int c = 0; c < 4; ++c) {
                const float expected = src[4LL * x + c] * kGain[c];
                if (std::memcmp(&g[c], &expected, 4) != 0) {
                    fail("value", name, f.w, f.h, f.sp, f.dp, y);
                    return;
                }
            }
        }
    }
    (void)windowBytes;

    for (int y = 0; y < f.boundsH; ++y) {
        for (int x = 0; x < f.boundsW; ++x) {
            const bool inside = x >= f.ox && x < f.ox + f.w && y >= f.oy && y < f.oy + f.h;
            const unsigned char* p = f.dstBase + static_cast<long long>(y) * f.dp +
                                     static_cast<long long>(x) * kPixelBytes;
            if (inside) {
                continue;
            }
            for (int byte = 0; byte < kPixelBytes; ++byte) {
                if (p[byte] != 0x5a) {
                    fail("overrun", name, f.w, f.h, f.sp, f.dp, y);
                    return;
                }
            }
        }
        const long long rowEnd = static_cast<long long>(f.boundsW) * kPixelBytes;
        if (absLL(f.dp) > rowEnd) {
            const unsigned char* pad = f.dstBase + static_cast<long long>(y) * f.dp + rowEnd;
            for (long long b = 0; b < absLL(f.dp) - rowEnd; ++b) {
                if (pad[b] != 0x5a) {
                    fail("padding-write", name, f.w, f.h, f.sp, f.dp, y);
                    return;
                }
            }
        }
    }
}

/// One end-to-end case through the public launcher.
bool runCase(const char* name, int w, int h, int ox, int oy, int padX,
             long long srcPitchOverride, long long dstPitchOverride,
             int srcShift, int dstShift, bool expectVec4, bool useStream)
{
    ++g_cases;
    Fixture f;
    if (!makeFixture(name, w, h, ox, oy, padX, srcPitchOverride, dstPitchOverride,
                     srcShift, dstShift, f)) {
        freeFixture(f);
        return false;
    }
    fillPattern(f, 0x1234u + static_cast<unsigned int>(w * 131 + h * 17 + ox));
    std::memset(f.dstHost, 0x5a, f.dstBytes);

    const unsigned char* srcOrigin = imageBase(f.srcAlloc, f.sp, f.boundsH, srcShift) +
                                     static_cast<long long>(oy) * f.sp +
                                     static_cast<long long>(ox) * kPixelBytes;
    unsigned char* dstOrigin = imageBase(f.dstAlloc, f.dp, f.boundsH, dstShift) +
                               static_cast<long long>(oy) * f.dp +
                               static_cast<long long>(ox) * kPixelBytes;

    const bool vec = mcofx::IsVec4Addressable(srcOrigin, dstOrigin, f.sp, f.dp);
    if (vec != expectVec4) {
        std::printf("  FAIL routing %-30s got %s, expected %s\n", name, vec ? "vec4" : "scalar",
                    expectVec4 ? "vec4" : "scalar");
        ++g_failures;
    }

    cudaStream_t stream = nullptr;
    if (useStream) {
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
            fail("stream-create", name, w, h, f.sp, f.dp, 0);
            freeFixture(f);
            return false;
        }
    }

    // The uploads must be ordered against the launch. A non-blocking stream is
    // NOT ordered against work queued on the legacy default stream, so copying
    // with cudaMemcpy and then launching on the stream is a data race - the first
    // version of this harness did exactly that and the kernel read half-written
    // source rows on hardware. Copy on the same stream when there is one.
    cudaError_t copyIn;
    if (stream != nullptr) {
        copyIn = cudaMemcpyAsync(f.srcAlloc, f.srcHost, f.srcBytes, cudaMemcpyHostToDevice, stream);
        if (copyIn == cudaSuccess) {
            copyIn = cudaMemcpyAsync(f.dstAlloc, f.dstHost, f.dstBytes, cudaMemcpyHostToDevice, stream);
        }
    } else {
        copyIn = cudaMemcpy(f.srcAlloc, f.srcHost, f.srcBytes, cudaMemcpyHostToDevice);
        if (copyIn == cudaSuccess) {
            copyIn = cudaMemcpy(f.dstAlloc, f.dstHost, f.dstBytes, cudaMemcpyHostToDevice);
        }
    }
    if (copyIn != cudaSuccess) {
        fail("h2d", name, w, h, f.sp, f.dp, 0);
        if (stream) cudaStreamDestroy(stream);
        freeFixture(f);
        return false;
    }

    const int status = mcofx::LaunchGainRGBA32F(stream, srcOrigin, dstOrigin, w, h, f.sp, f.dp, kGain);
    if (status != mcofx::kLaunchOK) {
        std::printf("  FAIL launch %-30s %s\n", name, mcofx::StatusMessage(status));
        ++g_failures;
        if (stream) cudaStreamDestroy(stream);
        freeFixture(f);
        return false;
    }

    // Mirror Plugin.cpp's completion policy: stream given -> the host orders it;
    // no stream -> the render must not return before the work is done.
    if (stream != nullptr) {
        cudaStreamSynchronize(stream);
        cudaStreamDestroy(stream);
    } else {
        const int sync = mcofx::WaitForDefaultStream();
        if (sync != 0) {
            std::printf("  FAIL sync %-30s %s\n", name, mcofx::StatusMessage(sync));
            ++g_failures;
            freeFixture(f);
            return false;
        }
    }

    if (cudaMemcpy(f.dstHost, f.dstAlloc, f.dstBytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        fail("d2h", name, w, h, f.sp, f.dp, 0);
        freeFixture(f);
        return false;
    }
    // Image origin for the host mirror, matching the layout the kernel addressed.
    f.srcBase = imageBase(f.srcHost, f.sp, f.boundsH, srcShift);
    f.dstBase = imageBase(f.dstHost, f.dp, f.boundsH, dstShift);
    verifyPixels(name, f);
    freeFixture(f);
    return true;
}

// ---------------------------------------------------------------- device info

void printDevice()
{
    int device = 0;
    cudaGetDevice(&device);
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, device) != cudaSuccess) {
        std::printf("FAIL: cudaGetDeviceProperties: %s\n", cudaGetErrorString(cudaGetLastError()));
        ++g_failures;
        return;
    }
    // cudaDeviceProp::driverVersion was removed in CUDA 13; the runtime version
    // is the portable replacement and is what decides JIT compatibility.
    int runtimeVersion = 0;
    cudaRuntimeGetVersion(&runtimeVersion);
    std::printf("device %d: %s, compute capability %d.%d, %d SMs, %zu MiB\n",
                device, prop.name, prop.major, prop.minor, prop.multiProcessorCount,
                prop.totalGlobalMem / (1024 * 1024));
    std::printf("runtime : CUDA %d.%d (%d), unified addressing %s, host-mapped %s\n",
                runtimeVersion / 1000, (runtimeVersion % 1000) / 10, runtimeVersion,
                prop.unifiedAddressing ? "yes" : "no",
                prop.canMapHostMemory ? "yes" : "no");

}

// ------------------------------------------------------------------- the cases

void runGeometryCases()
{
    std::printf("[1/6] geometry, pitch and routing through the launcher\n");
    // Canonical: packed rows, aligned base and pitch -> vec4.
    runCase("packed 64x48", 64, 48, 0, 0, 0, 0, 0, 0, 0, true, false);
    // Padded pitch that stays a multiple of 16 -> still vec4.
    runCase("padded 64x48 +256", 64, 48, 0, 0, 256, 0, 0, 0, 0, true, false);
    // Pitch not a multiple of 16 -> scalar.
    runCase("pitch 64x48 +8", 64, 48, 0, 0, 8, 0, 0, 0, 0, false, false);
    // 4-byte-aligned base: a float4 load here would fault, so scalar must win.
    runCase("base+4 32x16", 32, 16, 0, 0, 0, 0, 0, 4, 4, false, false);
    // Window offset inside the image bounds (Plugin.cpp's origin arithmetic).
    runCase("window offset 40x24", 40, 24, 5, 3, 0, 0, 0, 0, 0, true, false);
    // Negative pitch, multiple of 16 -> vec4 (flipped image).
    runCase("negative pitch 48x32", 48, 32, 0, 0, 0, -(48 + 2) * 16LL, -(48 + 2) * 16LL, 0, 0,
            true, false);
    // Negative pitch, not a multiple of 16 -> scalar.
    runCase("negative pitch +8", 48, 32, 0, 0, 0, -((48 + 2) * 16LL + 8), -((48 + 2) * 16LL + 8),
            0, 0, false, false);
    // Odd sizes: every residue of the 16x16 block, plus single rows/columns.
    const int sizes[][2] = { { 1, 1 }, { 3, 5 }, { 17, 1 }, { 1, 17 }, { 63, 65 },
                             { 16, 16 }, { 32, 33 }, { 129, 7 }, { 208, 117 } };
    for (const auto& s : sizes) {
        char name[64];
        std::snprintf(name, sizeof(name), "%dx%d", s[0], s[1]);
        runCase(name, s[0], s[1], 0, 0, 0, 0, 0, 0, 0, true, false);
    }
    runCase("zero width", 0, 8, 0, 0, 0, 0, 0, 0, 0, true, false);
    runCase("zero height", 8, 0, 0, 0, 0, 0, 0, 0, 0, true, false);
}

void runStreamCases()
{
    std::printf("[2/6] stream semantics\n");
    runCase("host stream 320x180", 320, 180, 0, 0, 0, 0, 0, 0, 0, true, true);
    runCase("host stream +4 base", 320, 180, 0, 0, 0, 0, 0, 4, 4, false, true);

    // Many launches on one stream, then one sync: the host's normal pattern.
    const int w = 257;
    const int h = 129;
    const long long pitch = static_cast<long long>(w) * kPixelBytes;
    unsigned char* src = nullptr;
    unsigned char* dst = nullptr;
    if (cudaMalloc(reinterpret_cast<void**>(&src), static_cast<size_t>(pitch) * h) != cudaSuccess ||
        cudaMalloc(reinterpret_cast<void**>(&dst), static_cast<size_t>(pitch) * h) != cudaSuccess) {
        ++g_failures;
        return;
    }
    std::vector<unsigned char> host(static_cast<size_t>(pitch) * h, 0);
    for (size_t i = 0; i < host.size(); i += 4) {
        const float v = static_cast<float>((i / 4) % 97) * 0.25f;
        std::memcpy(&host[i], &v, 4);
    }
    cudaMemcpy(src, host.data(), host.size(), cudaMemcpyHostToDevice);
    cudaMemcpy(dst, host.data(), host.size(), cudaMemcpyHostToDevice);
    cudaStream_t stream = nullptr;
    cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    int launches = 0;
    for (int i = 0; i < 200; ++i) {
        if (mcofx::LaunchGainRGBA32F(stream, src, dst, w, h, pitch, pitch, kGain) != mcofx::kLaunchOK) {
            ++g_failures;
            break;
        }
        ++launches;
    }
    const cudaError_t s = cudaStreamSynchronize(stream);
    if (s != cudaSuccess) {
        std::printf("  FAIL 200 sequential launches: %s\n", cudaGetErrorString(s));
        ++g_failures;
    }
    cudaStreamDestroy(stream);
    cudaFree(src);
    cudaFree(dst);
    std::printf("  %d queued launches on one non-blocking stream, one sync: ok\n", launches);
}

void runConcurrentCases()
{
    std::printf("[3/6] concurrent launches from host threads\n");
    struct Job {
        int w, h, shift;
    };
    const Job jobs[] = { { 97, 61, 0 }, { 320, 180, 0 }, { 33, 400, 4 }, { 512, 17, 0 },
                         { 64, 64, 0 }, { 17, 63, 4 }, { 200, 100, 0 }, { 1, 4096, 0 } };
    std::vector<int> results;
    std::vector<std::thread> threads;
    results.assign(sizeof(jobs) / sizeof(jobs[0]), -1);
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); ++i) {
        threads.emplace_back([&jobs, i, &results] {
            char name[64];
            std::snprintf(name, sizeof(name), "thread%d %dx%d", static_cast<int>(i), jobs[i].w,
                          jobs[i].h);
            results[i] = runCase(name, jobs[i].w, jobs[i].h, 0, 0, 0, 0, 0, jobs[i].shift,
                                 jobs[i].shift, jobs[i].shift == 0, true)
                             ? 0
                             : 1;
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    int bad = 0;
    for (int r : results) {
        bad += r != 0 ? 1 : 0;
    }
    std::printf("  %zu threads, %d failing\n", threads.size(), bad);
}

void runZeroCopyCase()
{
    std::printf("[4/6] zero-copy on host-mapped memory\n");
    if (![] {
            cudaDeviceProp p{};
            int d = 0;
            cudaGetDevice(&d);
            cudaGetDeviceProperties(&p, d);
            return p.canMapHostMemory != 0;
        }()) {
        std::printf("  skipped: device cannot map host memory\n");
        return;
    }
    const int w = 128;
    const int h = 64;
    const long long pitch = static_cast<long long>(w) * kPixelBytes;
    const size_t bytes = static_cast<size_t>(pitch) * h;

    float* hostSrc = nullptr;
    float* hostDst = nullptr;
    // cudaHostAlloc beats cudaMallocHost here: it is one API, same semantics.
    if (cudaHostAlloc(reinterpret_cast<void**>(&hostSrc), bytes, cudaHostAllocMapped) != cudaSuccess ||
        cudaHostAlloc(reinterpret_cast<void**>(&hostDst), bytes, cudaHostAllocMapped) != cudaSuccess) {
        std::printf("  FAIL cudaHostAlloc: %s\n", cudaGetErrorString(cudaGetLastError()));
        ++g_failures;
        return;
    }
    unsigned int seed = 42u;
    for (size_t i = 0; i < bytes / 4; ++i) {
        hostSrc[i] = nextFloat(seed);
        hostDst[i] = 0.0f;
    }
    void* devSrc = nullptr;
    void* devDst = nullptr;
    if (cudaHostGetDevicePointer(&devSrc, hostSrc, 0) != cudaSuccess ||
        cudaHostGetDevicePointer(&devDst, hostDst, 0) != cudaSuccess) {
        std::printf("  FAIL cudaHostGetDevicePointer: %s\n", cudaGetErrorString(cudaGetLastError()));
        ++g_failures;
        cudaFreeHost(hostSrc);
        cudaFreeHost(hostDst);
        return;
    }

    const int status = mcofx::LaunchGainRGBA32F(nullptr, devSrc, devDst, w, h, pitch, pitch, kGain);
    if (status != mcofx::kLaunchOK) {
        std::printf("  FAIL mapped launch: %s\n", mcofx::StatusMessage(status));
        ++g_failures;
    } else {
        mcofx::WaitForDefaultStream();
        size_t bad = 0;
        for (int i = 0; i < w * h * 4; ++i) {
            const int c = i % 4;
            if (hostDst[i] != hostSrc[i] * kGain[c]) {
                ++bad;
            }
        }
        if (bad != 0) {
            std::printf("  FAIL mapped zero-copy: %zu mismatching components\n", bad);
            ++g_failures;
        } else {
            std::printf("  %dx%d on pinned mapped memory, no device allocation: ok\n", w, h);
        }
    }
    cudaFreeHost(hostSrc);
    cudaFreeHost(hostDst);
}

void runDirectKernelCases()
{
    std::printf("[5/6] both kernels launched directly (bypassing the launcher)\n");
    const int w = 333;
    const int h = 101;
    const long long pitch = static_cast<long long>(w) * kPixelBytes;
    const size_t bytes = static_cast<size_t>(pitch) * h;
    unsigned char* src = nullptr;
    unsigned char* dst = nullptr;
    cudaMalloc(reinterpret_cast<void**>(&src), bytes);
    cudaMalloc(reinterpret_cast<void**>(&dst), bytes);
    std::vector<unsigned char> host(bytes, 0);
    unsigned int seed = 7u;
    for (size_t i = 0; i < bytes / 4; ++i) {
        const float v = nextFloat(seed);
        std::memcpy(&host[i * 4], &v, 4);
    }
    cudaMemcpy(src, host.data(), bytes, cudaMemcpyHostToDevice);

    const dim3 block(16, 16);
    const dim3 grid(static_cast<unsigned>((w + 15) / 16), static_cast<unsigned>((h + 15) / 16));
    const float4 g = make_float4(kGain[0], kGain[1], kGain[2], kGain[3]);

    std::vector<unsigned char> got(bytes, 0);
    const char* names[2] = { "vec4 (direct)", "scalar (direct)" };
    for (int which = 0; which < 2; ++which) {
        cudaMemset(dst, 0x5a, bytes);
        if (which == 0) {
            GainKernelRGBA32FVec4<<<grid, block>>>(reinterpret_cast<const float*>(src),
                                                   reinterpret_cast<float*>(dst), w, h, pitch,
                                                   pitch, g);
        } else {
            GainKernelRGBA32FScalar<<<grid, block>>>(reinterpret_cast<const float*>(src),
                                                     reinterpret_cast<float*>(dst), w, h, pitch,
                                                     pitch, g);
        }
        const cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess) {
            std::printf("  FAIL %s: %s\n", names[which], cudaGetErrorString(err));
            ++g_failures;
            continue;
        }
        mcofx::WaitForDefaultStream();
        cudaMemcpy(got.data(), dst, bytes, cudaMemcpyDeviceToHost);
        size_t bad = 0;
        for (int y = 0; y < h && bad == 0; ++y) {
            const float* s = reinterpret_cast<const float*>(host.data() + static_cast<size_t>(y) * pitch);
            const float* d = reinterpret_cast<const float*>(got.data() + static_cast<size_t>(y) * pitch);
            for (long long i = 0; i < static_cast<long long>(w) * 4; ++i) {
                const int c = static_cast<int>(i % 4);
                if (d[i] != s[i] * kGain[c]) {
                    ++bad;
                    break;
                }
            }
        }
        if (bad != 0) {
            std::printf("  FAIL %s: mismatch\n", names[which]);
            ++g_failures;
        } else {
            std::printf("  %-14s %dx%d exact match\n", names[which], w, h);
        }
    }
    cudaFree(src);
    cudaFree(dst);
}

void runThroughput()
{
    std::printf("[6/6] throughput: the kernel, the CPU loop it replaces, and a copy\n");

    // Fixed sizes so a number can be compared with the next run and with another
    // machine. 8K is here because it is where the frame stops fitting in cache and
    // the kernel is purely bandwidth-bound.
    struct Size {
        int w;
        int h;
        int gpuIterations;
        int cpuIterations;
        const char* name;
    };
    const Size sizes[] = {
        {1920, 1080, 100, 5, "1080p"},
        {3840, 2160, 30, 2, "4K"},
        {7680, 4320, 10, 1, "8K"},
    };

    const float4 g = make_float4(kGain[0], kGain[1], kGain[2], kGain[3]);
    cudaEvent_t start;
    cudaEvent_t stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    for (const Size& size : sizes) {
        const long long pitch = static_cast<long long>(size.w) * kPixelBytes;
        const size_t bytes = static_cast<size_t>(pitch) * size.h;
        unsigned char* src = nullptr;
        unsigned char* dst = nullptr;
        unsigned char* host = static_cast<unsigned char*>(std::malloc(bytes));
        if (host == nullptr || cudaMalloc(reinterpret_cast<void**>(&src), bytes) != cudaSuccess ||
            cudaMalloc(reinterpret_cast<void**>(&dst), bytes) != cudaSuccess) {
            std::printf("  skipped %s: cannot allocate two buffers of %.1f MB\n", size.name,
                        static_cast<double>(bytes) / 1e6);
            std::free(host);
            cudaFree(src);
            cudaFree(dst);
            continue;
        }
        cudaMemset(src, 0x3f, bytes);
        std::memset(host, 0x3f, bytes);

        const dim3 block(16, 16);
        const dim3 grid(static_cast<unsigned>((size.w + 15) / 16),
                        static_cast<unsigned>((size.h + 15) / 16));

        std::printf("  %s %dx%d: %.1f MPix, %.1f MB per buffer\n", size.name, size.w, size.h,
                    static_cast<double>(size.w) * size.h / 1e6,
                    static_cast<double>(bytes) / 1e6);

        double msVec4 = 0.0;
        for (int which = 0; which < 2; ++which) {
            for (int i = 0; i < 5; ++i) {
                if (which == 0) {
                    GainKernelRGBA32FVec4<<<grid, block>>>(
                        reinterpret_cast<const float*>(src), reinterpret_cast<float*>(dst), size.w,
                        size.h, pitch, pitch, g);
                } else {
                    GainKernelRGBA32FScalar<<<grid, block>>>(
                        reinterpret_cast<const float*>(src), reinterpret_cast<float*>(dst), size.w,
                        size.h, pitch, pitch, g);
                }
            }
            cudaEventRecord(start);
            for (int i = 0; i < size.gpuIterations; ++i) {
                if (which == 0) {
                    GainKernelRGBA32FVec4<<<grid, block>>>(
                        reinterpret_cast<const float*>(src), reinterpret_cast<float*>(dst), size.w,
                        size.h, pitch, pitch, g);
                } else {
                    GainKernelRGBA32FScalar<<<grid, block>>>(
                        reinterpret_cast<const float*>(src), reinterpret_cast<float*>(dst), size.w,
                        size.h, pitch, pitch, g);
                }
            }
            cudaEventRecord(stop);
            cudaEventSynchronize(stop);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, start, stop);
            const double perFrame = ms / size.gpuIterations;
            // A full frame read plus a full frame written, per iteration.
            const double gbs = (static_cast<double>(bytes) * 2.0) / (perFrame / 1000.0) / 1e9;
            if (which == 0) {
                msVec4 = perFrame;
            }
            std::printf("    %-6s %8.3f ms/frame  %7.1f GB/s effective\n",
                        which == 0 ? "vec4" : "scalar", perFrame, gbs);
        }

        // The CPU loop the kernel replaces: same arithmetic, same bytes, in place.
        // This is the comparison that decides whether the GPU path earns its copy.
        {
            const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            for (int iteration = 0; iteration < size.cpuIterations; ++iteration) {
                float4* pixels = reinterpret_cast<float4*>(host);
                const size_t count = bytes / sizeof(float4);
                for (size_t i = 0; i < count; ++i) {
                    float4 value = pixels[i];
                    value.x *= g.x;
                    value.y *= g.y;
                    value.z *= g.z;
                    value.w *= g.w;
                    pixels[i] = value;
                }
            }
            const std::chrono::steady_clock::time_point ended = std::chrono::steady_clock::now();
            const double msCpu =
                std::chrono::duration<double, std::milli>(ended - began).count() / size.cpuIterations;
            std::printf("    CPU    %8.3f ms/frame  %7.1fx slower than vec4\n", msCpu,
                        msCpu / msVec4);
        }

        // What a copy-based path pays and a zero-copy path does not: the frame in
        // and back out over PCIe, pageable memory, no pinned staging buffer.
        {
            const int copies = 5;
            const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            for (int i = 0; i < copies; ++i) {
                cudaMemcpy(src, host, bytes, cudaMemcpyHostToDevice);
                cudaMemcpy(host, src, bytes, cudaMemcpyDeviceToHost);
            }
            const std::chrono::steady_clock::time_point ended = std::chrono::steady_clock::now();
            const double msCopy =
                std::chrono::duration<double, std::milli>(ended - began).count() / copies;
            const double gbs = (static_cast<double>(bytes) * 2.0) / (msCopy / 1000.0) / 1e9;
            std::printf("    copy   %8.3f ms/frame  %7.1f GB/s over PCIe (H2D+D2H, pageable)\n",
                        msCopy, gbs);
        }

        std::free(host);
        cudaFree(src);
        cudaFree(dst);
    }

    cudaEventDestroy(start);
    cudaEventDestroy(stop);
}

} // namespace

int main()
{
    std::printf("== the minimal-cuda-ofx kernels on real hardware ==\n");
    printDevice();

    // A failed context makes every later check meaningless; say so once.
    if (cudaFree(nullptr) != cudaSuccess) {
        std::printf("FAIL: no usable CUDA device: %s\n", cudaGetErrorString(cudaGetLastError()));
        return 1;
    }

    runGeometryCases();
    runStreamCases();
    runConcurrentCases();
    runZeroCopyCase();
    runDirectKernelCases();
    runThroughput();

    std::printf("\n%d launcher cases, %d failure(s)\n", g_cases, g_failures);
    if (g_failures == 0) {
        std::printf("PASS: hardware matches the CPU reference bit for bit\n");
        return 0;
    }
    std::printf("FAIL\n");
    return 1;
}
