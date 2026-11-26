// SPDX-License-Identifier: BSD-3-Clause
#include "ClRuntime.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ClKernel.h"

// glibc 2.34 moved dlopen/dlsym/dlerror into libc and versioned them
// GLIBC_2.34; the old GLIBC_2.2.5 compatibility symbols are still in libc. On a
// 2.34 build host, linking -ldl therefore records a 2.34 requirement in the
// module - measured: the OpenCL bundle asked for GLIBC_2.34 while the other two
// bundles built here ask for 2.14, and the gate's 2.17 floor failed it. Asking
// for the old version explicitly keeps the floor, and still resolves on 2.34
// because libc carries both.
#if defined(__GLIBC__) && !defined(__UCLIBC__)
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
#endif

namespace ocl {
namespace {

// ---------------------------------------------------------------------------
// The OpenCL 1.2 ABI, as much of it as this plug-in touches
// ---------------------------------------------------------------------------

typedef int cl_int;
typedef unsigned int cl_uint;
typedef unsigned long long cl_bitfield;
typedef unsigned int cl_bool;

typedef struct _cl_context* cl_context;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;

struct cl_float4 {
    float x, y, z, w;
};

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_uint CL_QUEUE_CONTEXT = 0x1090;
constexpr cl_uint CL_QUEUE_DEVICE = 0x1091;
constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;
constexpr cl_bool CL_TRUE = 1;

struct Api {
    void* handle = nullptr;

    cl_int (*GetCommandQueueInfo)(cl_command_queue, cl_uint, std::size_t, void*, std::size_t*);
    cl_program (*CreateProgramWithSource)(cl_context, cl_uint, const char**, const std::size_t*,
                                          cl_int*);
    cl_int (*BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*,
                           void (*)(cl_program, void*), void*);
    cl_int (*GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, std::size_t, void*, std::size_t*);
    cl_kernel (*CreateKernel)(cl_program, const char*, cl_int*);
    cl_int (*SetKernelArg)(cl_kernel, cl_uint, std::size_t, const void*);
    cl_int (*EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const std::size_t*,
                                   const std::size_t*, const std::size_t*, cl_uint, const cl_event*,
                                   cl_event*);
    cl_int (*ReleaseKernel)(cl_kernel);
    cl_int (*ReleaseProgram)(cl_program);
};

template <typename T>
bool symbol(void* handle, const char* name, T& out)
{
    const void* const found = dlsym(handle, name);
    if (found == nullptr) return false;
    std::memcpy(&out, &found, sizeof(out));
    return true;
}

bool load(Api* api, std::string* why)
{
    const char* fromEnv = std::getenv("OPENCL_LOADER_LIB");
    const char* path = (fromEnv != nullptr && *fromEnv != '\0') ? fromEnv : "libOpenCL.so.1";
    api->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (api->handle == nullptr) {
        *why = std::string("dlopen(") + path + "): " + dlerror();
        return false;
    }
    void* h = api->handle;
    const bool resolved =
        symbol(h, "clGetCommandQueueInfo", api->GetCommandQueueInfo) &&
        symbol(h, "clCreateProgramWithSource", api->CreateProgramWithSource) &&
        symbol(h, "clBuildProgram", api->BuildProgram) &&
        symbol(h, "clGetProgramBuildInfo", api->GetProgramBuildInfo) &&
        symbol(h, "clCreateKernel", api->CreateKernel) &&
        symbol(h, "clSetKernelArg", api->SetKernelArg) &&
        symbol(h, "clEnqueueNDRangeKernel", api->EnqueueNDRangeKernel) &&
        symbol(h, "clReleaseKernel", api->ReleaseKernel) &&
        symbol(h, "clReleaseProgram", api->ReleaseProgram);
    if (!resolved) {
        *why = std::string("the loader has no OpenCL 1.2 entry point: ") + dlerror();
        return false;
    }
    return true;
}

std::size_t roundUp(std::size_t value, std::size_t multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl: the loader table plus the two programs a render may need
// ---------------------------------------------------------------------------

struct Runtime::Impl {
    struct Program {
        cl_program program = nullptr;
        cl_kernel kernel = nullptr;
        bool built = false;

        void drop()
        {
            if (kernel) api->ReleaseKernel(kernel);
            if (program) api->ReleaseProgram(program);
            kernel = nullptr;
            program = nullptr;
            built = false;
        }

        const Api* api = nullptr;
    };

    Api api;
    bool loaded = false;
    std::string loadError;

    void* queue = nullptr; // identity only, never dereferenced
    cl_context context = nullptr;
    cl_device_id device = nullptr;
    Program buffers;
    Program images;

    Impl() { buffers.api = &api; images.api = &api; }

    ~Impl()
    {
        buffers.drop();
        images.drop();
        // The context, device and queue belong to the host; this plug-in never
        // retained them and never releases them.
    }

    void dropAll()
    {
        buffers.drop();
        images.drop();
        context = nullptr;
        device = nullptr;
        queue = nullptr;
    }

    bool build(Program& target, const char* source, const char* name, std::string* why)
    {
        cl_int err = CL_SUCCESS;
        cl_program program = api.CreateProgramWithSource(context, 1, &source, nullptr, &err);
        if (program == nullptr || err != CL_SUCCESS) {
            *why = std::string("clCreateProgramWithSource failed (") + name + ", err " +
                   std::to_string(err) + ")";
            return false;
        }
        if (api.BuildProgram(program, 1, &device, "", nullptr, nullptr) != CL_SUCCESS) {
            std::size_t size = 0;
            api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
            std::vector<char> log(size + 1, '\0');
            if (size > 0) {
                api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log.data(),
                                        nullptr);
            }
            *why = std::string("clBuildProgram failed (") + name + "):\n" + log.data();
            api.ReleaseProgram(program);
            return false;
        }
        cl_kernel kernel = api.CreateKernel(program, name, &err);
        if (kernel == nullptr || err != CL_SUCCESS) {
            *why = std::string("clCreateKernel failed (") + name + ", err " +
                   std::to_string(err) + ")";
            api.ReleaseProgram(program);
            return false;
        }
        target.program = program;
        target.kernel = kernel;
        target.built = true;
        return true;
    }
};

Runtime::Runtime()
    : impl_(new Impl())
{
}

Runtime::~Runtime()
{
    delete impl_;
}

bool Runtime::prepare(void* queue, bool images, std::string* why)
{
    if (queue == nullptr) {
        *why = "the host did not hand over a command queue";
        return false;
    }
    if (!impl_->loaded) {
        impl_->loaded = true;
        if (!load(&impl_->api, &impl_->loadError)) {
            *why = impl_->loadError;
            return false;
        }
    }
    Impl* impl = impl_;
    if (impl->queue != queue) {
        impl->dropAll();
        impl->queue = queue;
    }
    if ((images ? impl->images.built : impl->buffers.built)) return true;

    if (impl->context == nullptr) {
        cl_context context = nullptr;
        if (impl->api.GetCommandQueueInfo(reinterpret_cast<cl_command_queue>(queue),
                                          CL_QUEUE_CONTEXT, sizeof(context), &context,
                                          nullptr) != CL_SUCCESS ||
            context == nullptr) {
            *why = "clGetCommandQueueInfo(CL_QUEUE_CONTEXT) failed";
            return false;
        }
        cl_device_id device = nullptr;
        if (impl->api.GetCommandQueueInfo(reinterpret_cast<cl_command_queue>(queue),
                                          CL_QUEUE_DEVICE, sizeof(device), &device,
                                          nullptr) != CL_SUCCESS ||
            device == nullptr) {
            *why = "clGetCommandQueueInfo(CL_QUEUE_DEVICE) failed";
            return false;
        }
        impl->context = context;
        impl->device = device;
    }

    return images ? impl->build(impl->images, kGainImageKernelSource, kGainImageKernelName, why)
                  : impl->build(impl->buffers, kGainKernelSource, kGainKernelName, why);
}

bool Runtime::enqueueGain(void* queue, void* srcBuffer, void* dstBuffer, int width, int height,
                          int offsetX, int offsetY, int pitchComponents, float gain,
                          std::string* why) const
{
    Impl* impl = impl_;
    if (impl->queue != queue || !impl->buffers.built) {
        *why = "enqueueGain called before prepare(queue, false) succeeded for this queue";
        return false;
    }
    cl_mem src = reinterpret_cast<cl_mem>(srcBuffer);
    cl_mem dst = reinterpret_cast<cl_mem>(dstBuffer);
    Api& api = impl->api;
    cl_kernel kernel = impl->buffers.kernel;

    const int args[4] = {width, height, offsetX, offsetY};
    if (api.SetKernelArg(kernel, 0, sizeof(cl_mem), &src) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 1, sizeof(cl_mem), &dst) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 2, sizeof(int), &args[0]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 3, sizeof(int), &args[1]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 4, sizeof(int), &args[2]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 5, sizeof(int), &args[3]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 6, sizeof(int), &pitchComponents) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 7, sizeof(float), &gain) != CL_SUCCESS) {
        *why = "clSetKernelArg failed";
        return false;
    }

    // One work item per pixel, width rounded up to a multiple of 16 so a device
    // that prefers it gets a rectangular grid; the kernel returns early outside
    // the window, so the padding work items cost a branch and write nothing.
    const std::size_t global[2] = {roundUp(static_cast<std::size_t>(width), 16),
                                   static_cast<std::size_t>(height)};
    if (api.EnqueueNDRangeKernel(reinterpret_cast<cl_command_queue>(queue), kernel, 2, nullptr,
                                 global, nullptr, 0, nullptr, nullptr) != CL_SUCCESS) {
        *why = "clEnqueueNDRangeKernel failed";
        return false;
    }
    // Deliberately no clFinish: ofxGPURender.h says the plug-in should enqueue
    // onto the host's queue and not wait for it, and the host's own readback is
    // what turns the result into pixels.
    return true;
}

bool Runtime::enqueueGainImages(void* queue, void* srcImage, void* dstImage, int width, int height,
                                int offsetX, int offsetY, const float gain[4],
                                std::string* why) const
{
    Impl* impl = impl_;
    if (impl->queue != queue || !impl->images.built) {
        *why = "enqueueGainImages called before prepare(queue, true) succeeded for this queue";
        return false;
    }
    cl_mem src = reinterpret_cast<cl_mem>(srcImage);
    cl_mem dst = reinterpret_cast<cl_mem>(dstImage);
    Api& api = impl->api;
    cl_kernel kernel = impl->images.kernel;

    const int args[4] = {width, height, offsetX, offsetY};
    if (api.SetKernelArg(kernel, 0, sizeof(cl_mem), &src) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 1, sizeof(cl_mem), &dst) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 2, sizeof(int), &args[0]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 3, sizeof(int), &args[1]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 4, sizeof(int), &args[2]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 5, sizeof(int), &args[3]) != CL_SUCCESS ||
        api.SetKernelArg(kernel, 6, sizeof(cl_float4), gain) != CL_SUCCESS) {
        *why = "clSetKernelArg failed (image kernel)";
        return false;
    }

    const std::size_t global[2] = {roundUp(static_cast<std::size_t>(width), 16),
                                   static_cast<std::size_t>(height)};
    if (api.EnqueueNDRangeKernel(reinterpret_cast<cl_command_queue>(queue), kernel, 2, nullptr,
                                 global, nullptr, 0, nullptr, nullptr) != CL_SUCCESS) {
        *why = "clEnqueueNDRangeKernel failed (image kernel)";
        return false;
    }
    return true;
}

} // namespace ocl
