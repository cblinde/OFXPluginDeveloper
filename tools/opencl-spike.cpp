// SPDX-License-Identifier: BSD-3-Clause
//
// Backlog row 34, the small half: can a plug-in reach a real OpenCL device from
// this checkout with no vendor SDK, no headers and no GPU?
//
// What it does, in the order a template would: open the ICD loader, pick the
// first platform and device, create a context and a command queue, then run two
// kernels through that queue - one over a `cl_mem` buffer, one over a 2D image,
// because `kOfxImageEffectPropOpenCLRenderSupported` is about images and an
// image path that only works on buffers is not the path an OFX plug-in uses.
// Each result is read back to the host and compared, so "it ran" means the
// numbers arrived.
//
// Why no OpenCL headers: this machine has `libOpenCL.so.1` and a vendor ICD
// (PoCL) but no `CL/cl.h`, exactly the situation a plug-in on a render node is
// in when the host owns the runtime. The entry points are resolved with
// `dlopen`/`dlsym` here; the types below are the OpenCL 1.2 ABI, not a
// reimplementation of the API. A template would link against the loader and use
// the host's queue instead of creating its own - that difference is the reason
// this is a spike and not the template.
//
// Exit status: 0 when both paths produced the expected numbers, 1 otherwise,
// 2 when the loader or the platform is missing (nothing to test).

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// The OpenCL ABI, as much of it as the spike touches
// ---------------------------------------------------------------------------

typedef int cl_int;
typedef unsigned int cl_uint;
typedef unsigned long long cl_ulong;
typedef unsigned long long cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint cl_bool;

typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;

struct cl_image_format {
    cl_uint image_channel_order;
    cl_uint image_channel_data_type;
};

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_uint CL_PLATFORM_NAME = 0x0902;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_DEVICE_VERSION = 0x102F;
constexpr cl_uint CL_DEVICE_IMAGE_SUPPORT = 0x1016;
constexpr cl_device_type CL_DEVICE_TYPE_ALL = 0xFFFFFFFFull;
constexpr cl_mem_flags CL_MEM_READ_WRITE = 1ull << 0;
constexpr cl_mem_flags CL_MEM_WRITE_ONLY = 1ull << 2;
constexpr cl_mem_flags CL_MEM_COPY_HOST_PTR = 1ull << 5;
constexpr cl_bool CL_TRUE = 1;
constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;
constexpr cl_uint CL_RGBA = 0x10B5;
constexpr cl_uint CL_FLOAT = 0x10DE;

// ---------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------

struct Api {
    void* handle;

    cl_int (*GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
    cl_int (*GetPlatformInfo)(cl_platform_id, cl_uint, size_t, void*, size_t*);
    cl_int (*GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
    cl_int (*GetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_context (*CreateContext)(const cl_bitfield*, cl_uint, const cl_device_id*,
                                void (*)(const char*, const void*, size_t, void*), void*, cl_int*);
    cl_command_queue (*CreateCommandQueue)(cl_context, cl_device_id,
                                           cl_command_queue_properties, cl_int*);
    cl_mem (*CreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
    cl_mem (*CreateImage2D)(cl_context, cl_mem_flags, const cl_image_format*,
                            size_t, size_t, size_t, void*, cl_int*);
    cl_program (*CreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
    cl_int (*BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*,
                           void (*)(cl_program, void*), void*);
    cl_int (*GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_kernel (*CreateKernel)(cl_program, const char*, cl_int*);
    cl_int (*SetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
    cl_int (*EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*,
                                   const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
    cl_int (*EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                 const void*, cl_uint, const cl_event*, cl_event*);
    cl_int (*EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                void*, cl_uint, const cl_event*, cl_event*);
    cl_int (*EnqueueReadImage)(cl_command_queue, cl_mem, cl_bool, const size_t*,
                               const size_t*, size_t, size_t, void*, cl_uint,
                               const cl_event*, cl_event*);
    cl_int (*Finish)(cl_command_queue);
    cl_int (*ReleaseMemObject)(cl_mem);
    cl_int (*ReleaseKernel)(cl_kernel);
    cl_int (*ReleaseProgram)(cl_program);
    cl_int (*ReleaseCommandQueue)(cl_command_queue);
    cl_int (*ReleaseContext)(cl_context);
};

Api g_api{};

[[noreturn]] void fail(const std::string& step, cl_int code)
{
    std::fprintf(stderr, "opencl spike: FAIL at %s: OpenCL error %d\n", step.c_str(), code);
    std::exit(1);
}

void check(const std::string& step, cl_int code)
{
    if (code != CL_SUCCESS) fail(step, code);
}

template <typename T>
T symbol(void* handle, const char* name)
{
    void* found = dlsym(handle, name);
    if (found == nullptr) {
        std::fprintf(stderr, "opencl spike: FAIL: the loader has no %s: %s\n", name, dlerror());
        std::exit(2);
    }
    T fn{};
    std::memcpy(&fn, &found, sizeof(fn));
    return fn;
}

Api load(const char* path)
{
    Api api{};
    api.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (api.handle == nullptr) {
        std::fprintf(stderr, "opencl spike: FAIL: cannot load %s: %s\n", path, dlerror());
        std::exit(2);
    }
    void* h = api.handle;
    api.GetPlatformIDs = symbol<decltype(api.GetPlatformIDs)>(h, "clGetPlatformIDs");
    api.GetPlatformInfo = symbol<decltype(api.GetPlatformInfo)>(h, "clGetPlatformInfo");
    api.GetDeviceIDs = symbol<decltype(api.GetDeviceIDs)>(h, "clGetDeviceIDs");
    api.GetDeviceInfo = symbol<decltype(api.GetDeviceInfo)>(h, "clGetDeviceInfo");
    api.CreateContext = symbol<decltype(api.CreateContext)>(h, "clCreateContext");
    api.CreateCommandQueue = symbol<decltype(api.CreateCommandQueue)>(h, "clCreateCommandQueue");
    api.CreateBuffer = symbol<decltype(api.CreateBuffer)>(h, "clCreateBuffer");
    api.CreateImage2D = symbol<decltype(api.CreateImage2D)>(h, "clCreateImage2D");
    api.CreateProgramWithSource = symbol<decltype(api.CreateProgramWithSource)>(h, "clCreateProgramWithSource");
    api.BuildProgram = symbol<decltype(api.BuildProgram)>(h, "clBuildProgram");
    api.GetProgramBuildInfo = symbol<decltype(api.GetProgramBuildInfo)>(h, "clGetProgramBuildInfo");
    api.CreateKernel = symbol<decltype(api.CreateKernel)>(h, "clCreateKernel");
    api.SetKernelArg = symbol<decltype(api.SetKernelArg)>(h, "clSetKernelArg");
    api.EnqueueNDRangeKernel = symbol<decltype(api.EnqueueNDRangeKernel)>(h, "clEnqueueNDRangeKernel");
    api.EnqueueWriteBuffer = symbol<decltype(api.EnqueueWriteBuffer)>(h, "clEnqueueWriteBuffer");
    api.EnqueueReadBuffer = symbol<decltype(api.EnqueueReadBuffer)>(h, "clEnqueueReadBuffer");
    api.EnqueueReadImage = symbol<decltype(api.EnqueueReadImage)>(h, "clEnqueueReadImage");
    api.Finish = symbol<decltype(api.Finish)>(h, "clFinish");
    api.ReleaseMemObject = symbol<decltype(api.ReleaseMemObject)>(h, "clReleaseMemObject");
    api.ReleaseKernel = symbol<decltype(api.ReleaseKernel)>(h, "clReleaseKernel");
    api.ReleaseProgram = symbol<decltype(api.ReleaseProgram)>(h, "clReleaseProgram");
    api.ReleaseCommandQueue = symbol<decltype(api.ReleaseCommandQueue)>(h, "clReleaseCommandQueue");
    api.ReleaseContext = symbol<decltype(api.ReleaseContext)>(h, "clReleaseContext");
    return api;
}

std::string info_string(cl_device_id device, cl_uint what)
{
    size_t size = 0;
    if (g_api.GetDeviceInfo(device, what, 0, nullptr, &size) != CL_SUCCESS || size == 0) return "<unknown>";
    std::string value(size, '\0');
    if (g_api.GetDeviceInfo(device, what, size, value.data(), nullptr) != CL_SUCCESS) return "<unknown>";
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::string platform_name(cl_platform_id platform)
{
    size_t size = 0;
    if (g_api.GetPlatformInfo(platform, CL_PLATFORM_NAME, 0, nullptr, &size) != CL_SUCCESS || size == 0) return "<unknown>";
    std::string value(size, '\0');
    if (g_api.GetPlatformInfo(platform, CL_PLATFORM_NAME, size, value.data(), nullptr) != CL_SUCCESS) return "<unknown>";
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

cl_program build(cl_context context, cl_device_id device, const char* source, const char* name)
{
    cl_int err = CL_SUCCESS;
    cl_program program = g_api.CreateProgramWithSource(context, 1, &source, nullptr, &err);
    check(std::string("clCreateProgramWithSource (") + name + ")", err);
    const cl_int built = g_api.BuildProgram(program, 1, &device, "", nullptr, nullptr);
    if (built != CL_SUCCESS) {
        size_t size = 0;
        g_api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
        std::vector<char> log(size + 1, '\0');
        g_api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr);
        std::fprintf(stderr, "opencl spike: FAIL: clBuildProgram (%s): build log:\n%s\n", name, log.data());
        g_api.ReleaseProgram(program);
        std::exit(1);
    }
    // The handle is opaque and the source is copied by clCreateProgramWithSource;
    // cppcheck reads the `&source` argument as a lifetime and flags the return.
    // cppcheck-suppress returnDanglingLifetime
    return program;
}

cl_kernel kernel_of(cl_program program, const char* name)
{
    cl_int err = CL_SUCCESS;
    cl_kernel kernel = g_api.CreateKernel(program, name, &err);
    check(std::string("clCreateKernel (") + name + ")", err);
    // Same shape as above: an opaque handle, and `&err` is an out-parameter that
    // does not outlive the call.
    // cppcheck-suppress returnDanglingLifetime
    return kernel;
}

// ---------------------------------------------------------------------------
// Path 1: a buffer kernel
// ---------------------------------------------------------------------------

bool buffer_path(cl_context context, cl_command_queue queue, cl_device_id device)
{
    static const char* kSource =
        "__kernel void scale(__global float* v, const float k)\n"
        "{\n"
        "    const size_t i = get_global_id(0);\n"
        "    v[i] = v[i] * k;\n"
        "}\n";

    constexpr size_t kCount = 1024;
    std::vector<float> host(kCount);
    for (size_t i = 0; i < kCount; ++i) host[i] = static_cast<float>(i) * 0.5f;

    cl_int err = CL_SUCCESS;
    cl_mem buffer = g_api.CreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                       kCount * sizeof(float), host.data(), &err);
    check("clCreateBuffer", err);
    cl_program program = build(context, device, kSource, "buffer");
    cl_kernel kernel = kernel_of(program, "scale");

    const float factor = 3.0f;
    check("clSetKernelArg (v)", g_api.SetKernelArg(kernel, 0, sizeof(cl_mem), &buffer));
    check("clSetKernelArg (k)", g_api.SetKernelArg(kernel, 1, sizeof(float), &factor));
    const size_t global = kCount;
    check("clEnqueueNDRangeKernel (buffer)",
          g_api.EnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, nullptr));
    check("clFinish (buffer)", g_api.Finish(queue));

    std::vector<float> result(kCount, 0.0f);
    check("clEnqueueReadBuffer", g_api.EnqueueReadBuffer(queue, buffer, CL_TRUE, 0,
                                                         kCount * sizeof(float), result.data(),
                                                         0, nullptr, nullptr));
    size_t wrong = 0;
    for (size_t i = 0; i < kCount; ++i) {
        if (std::fabs(result[i] - host[i] * factor) > 1e-6f) ++wrong;
    }
    // The buffer was uploaded with COPY_HOST_PTR from `host`, so the comparison
    // is against the same numbers that went in, not against a constant.
    if (wrong != 0) {
        std::fprintf(stderr, "opencl spike: FAIL: buffer kernel: %zu of %zu wrong\n", wrong, kCount);
    } else {
        std::printf("  buffer path: %zu floats, scale by %.1f, %zu wrong\n", kCount,
                    static_cast<double>(factor), wrong);
    }
    g_api.ReleaseMemObject(buffer);
    g_api.ReleaseKernel(kernel);
    g_api.ReleaseProgram(program);
    return wrong == 0;
}

// ---------------------------------------------------------------------------
// Path 2: a 2D image kernel - what an OFX OpenCL render touches
// ---------------------------------------------------------------------------

bool image_path(cl_context context, cl_command_queue queue, cl_device_id device)
{
    static const char* kSource =
        "__kernel void fill(__write_only image2d_t dst, const float4 c)\n"
        "{\n"
        "    const int2 p = (int2)(get_global_id(0), get_global_id(1));\n"
        "    write_imagef(dst, p, c);\n"
        "}\n";

    constexpr size_t kWidth = 32;
    constexpr size_t kHeight = 24;
    constexpr size_t kComponents = 4;

    cl_image_format format{};
    format.image_channel_order = CL_RGBA;
    format.image_channel_data_type = CL_FLOAT;

    cl_int err = CL_SUCCESS;
    cl_mem image = g_api.CreateImage2D(context, CL_MEM_WRITE_ONLY, &format, kWidth, kHeight,
                                       0, nullptr, &err);
    check("clCreateImage2D", err);
    cl_program program = build(context, device, kSource, "image");
    cl_kernel kernel = kernel_of(program, "fill");

    const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    check("clSetKernelArg (dst)", g_api.SetKernelArg(kernel, 0, sizeof(cl_mem), &image));
    check("clSetKernelArg (c)", g_api.SetKernelArg(kernel, 1, sizeof(colour), colour));
    const size_t global[2] = {kWidth, kHeight};
    check("clEnqueueNDRangeKernel (image)",
          g_api.EnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, nullptr, 0, nullptr, nullptr));
    check("clFinish (image)", g_api.Finish(queue));

    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {kWidth, kHeight, 1};
    std::vector<float> pixels(kWidth * kHeight * kComponents, -1.0f);
    check("clEnqueueReadImage", g_api.EnqueueReadImage(queue, image, CL_TRUE, origin, region,
                                                       0, 0, pixels.data(), 0, nullptr, nullptr));

    size_t wrong = 0;
    for (size_t i = 0; i < kWidth * kHeight; ++i) {
        for (size_t c = 0; c < kComponents; ++c) {
            if (std::fabs(pixels[i * kComponents + c] - colour[c]) > 1e-6f) ++wrong;
        }
    }
    if (wrong != 0) {
        std::fprintf(stderr, "opencl spike: FAIL: image kernel: %zu of %zu components wrong\n", wrong,
                     kWidth * kHeight * kComponents);
    } else {
        std::printf("  image path: %zux%zu RGBA float image, %zu components, %zu wrong\n", kWidth,
                    kHeight, kWidth * kHeight * kComponents, wrong);
    }
    g_api.ReleaseMemObject(image);
    g_api.ReleaseKernel(kernel);
    g_api.ReleaseProgram(program);
    return wrong == 0;
}

} // namespace

int main()
{
    const char* loader = std::getenv("OPENCL_LOADER_LIB");
    if (loader == nullptr) loader = "libOpenCL.so.1";
    std::printf("opencl spike: loader %s\n", loader);
    g_api = load(loader);

    cl_uint platform_count = 0;
    cl_int err = g_api.GetPlatformIDs(0, nullptr, &platform_count);
    check("clGetPlatformIDs", err);
    if (platform_count == 0) {
        std::fprintf(stderr, "opencl spike: FAIL: the loader knows no platform (no ICD on OCL_ICD_VENDORS)\n");
        return 2;
    }
    std::vector<cl_platform_id> platforms(platform_count);
    check("clGetPlatformIDs (list)", g_api.GetPlatformIDs(platform_count, platforms.data(), nullptr));
    std::printf("  platform: %s\n", platform_name(platforms[0]).c_str());

    cl_uint device_count = 0;
    err = g_api.GetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
    check("clGetDeviceIDs", err);
    if (device_count == 0) {
        std::fprintf(stderr, "opencl spike: FAIL: the platform lists no device\n");
        return 2;
    }
    std::vector<cl_device_id> devices(device_count);
    check("clGetDeviceIDs (list)",
          g_api.GetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr));
    cl_device_id device = devices[0];
    std::printf("  device:   %s (%s)\n", info_string(device, CL_DEVICE_NAME).c_str(),
                info_string(device, CL_DEVICE_VERSION).c_str());

    cl_uint image_support = 0;
    check("clGetDeviceInfo (image support)",
          g_api.GetDeviceInfo(device, CL_DEVICE_IMAGE_SUPPORT, sizeof(image_support), &image_support, nullptr));
    std::printf("  images:   %s\n", image_support ? "supported" : "NOT supported");

    cl_context context = g_api.CreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    check("clCreateContext", err);
    cl_command_queue queue = g_api.CreateCommandQueue(context, device, 0, &err);
    check("clCreateCommandQueue", err);

    const bool buffer_ok = buffer_path(context, queue, device);
    // An image kernel on a device without image support is a driver error, not a
    // finding about this repository - say which one it is.
    bool image_ok = false;
    if (image_support) {
        image_ok = image_path(context, queue, device);
    } else {
        std::fprintf(stderr, "opencl spike: image path not run: the device reports no image support\n");
    }

    g_api.ReleaseCommandQueue(queue);
    g_api.ReleaseContext(context);
    const int passed = (buffer_ok ? 1 : 0) + (image_ok ? 1 : 0);
    std::printf("opencl spike: %d of 2 paths ok\n", passed);
    return passed == 2 ? 0 : 1;
}
