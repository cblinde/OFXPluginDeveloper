// SPDX-License-Identifier: BSD-3-Clause
//
// The OpenCL entry points this harness uses, resolved at runtime.
//
// A host that hands a plug-in an OpenCL buffer owns a context and a command
// queue; the harness has to own the same things to be that host. It cannot
// link against them, because the machine this runs on has `libOpenCL.so.1` and
// an ICD (PoCL) but no `CL/cl.h`, and because the loader a host picks is the
// host's business. So the entry points are resolved with dlopen/dlsym, the same
// way tools/opencl-spike.cpp does it, and the types below are the OpenCL 1.2
// ABI - not a second implementation of the API.
//
// Only what the harness needs is here: enumerate, create one context and one
// queue, carry buffers, finish, read back, release. A plug-in that wants more
// ships its own loader (templates/minimal-opencl-ofx/src/ClRuntime.cpp).
//
// Header-only and inline so the fake host and a template's suite share one
// copy without a library between them.

#pragma once

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace testkit {
namespace cl {

typedef int cl_int;
typedef unsigned int cl_uint;
typedef unsigned long long cl_bitfield;
typedef unsigned long long cl_device_type;
typedef unsigned long long cl_mem_flags;
typedef unsigned long long cl_command_queue_properties;
typedef unsigned int cl_bool;

typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_uint CL_PLATFORM_NAME = 0x0902;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_DEVICE_VERSION = 0x102F;
constexpr cl_uint CL_QUEUE_CONTEXT = 0x1090;
constexpr cl_uint CL_QUEUE_DEVICE = 0x1091;
constexpr cl_device_type CL_DEVICE_TYPE_ALL = 0xFFFFFFFFull;
constexpr cl_mem_flags CL_MEM_READ_WRITE = 1ull << 0;
constexpr cl_mem_flags CL_MEM_COPY_HOST_PTR = 1ull << 5;
constexpr cl_bool CL_TRUE = 1;

struct Api {
  void* handle = nullptr;

  cl_int (*GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
  cl_int (*GetPlatformInfo)(cl_platform_id, cl_uint, std::size_t, void*, std::size_t*);
  cl_int (*GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
  cl_int (*GetDeviceInfo)(cl_device_id, cl_uint, std::size_t, void*, std::size_t*);
  cl_context (*CreateContext)(const cl_bitfield*, cl_uint, const cl_device_id*,
                              void (*)(const char*, const void*, std::size_t, void*), void*,
                              cl_int*);
  cl_command_queue (*CreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties,
                                         cl_int*);
  cl_mem (*CreateBuffer)(cl_context, cl_mem_flags, std::size_t, void*, cl_int*);
  cl_int (*EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, std::size_t, std::size_t,
                               const void*, cl_uint, const void*, void*);
  cl_int (*EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, std::size_t, std::size_t, void*,
                              cl_uint, const void*, void*);
  cl_int (*Finish)(cl_command_queue);
  cl_int (*ReleaseMemObject)(cl_mem);
  cl_int (*ReleaseCommandQueue)(cl_command_queue);
  cl_int (*ReleaseContext)(cl_context);
};

struct DeviceInfo {
  bool ok = false;
  std::string error;
  std::string platformName;
  std::string deviceName;
  std::string deviceVersion;
  cl_device_id device = nullptr;
  Api api;
};

namespace detail {

inline std::string loaderPath()
{
  const char* fromEnv = std::getenv("OPENCL_LOADER_LIB");
  return fromEnv != nullptr && *fromEnv != '\0' ? std::string(fromEnv) : std::string("libOpenCL.so.1");
}

template <typename T>
bool symbol(void* handle, const char* name, T& out)
{
  void* found = dlsym(handle, name);
  if (found == nullptr) return false;
  std::memcpy(&out, &found, sizeof(out));
  return true;
}

inline std::string infoString(Api& api, bool platform, void* object, cl_uint what)
{
  std::size_t size = 0;
  const cl_int got = platform
                         ? api.GetPlatformInfo(reinterpret_cast<cl_platform_id>(object), what, 0,
                                               nullptr, &size)
                         : api.GetDeviceInfo(reinterpret_cast<cl_device_id>(object), what, 0,
                                             nullptr, &size);
  if (got != CL_SUCCESS || size == 0) return "<unknown>";
  std::string value(size, '\0');
  const cl_int read =
      platform ? api.GetPlatformInfo(reinterpret_cast<cl_platform_id>(object), what, size,
                                     value.data(), nullptr)
               : api.GetDeviceInfo(reinterpret_cast<cl_device_id>(object), what, size,
                                   value.data(), nullptr);
  if (read != CL_SUCCESS) return "<unknown>";
  while (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
}

} // namespace detail

// Opens the loader and picks the first platform and device, the way a host with
// one GPU installed does. A failure is reported, never fatal: "no OpenCL on this
// machine" is a fact about the machine, and the caller decides whether the test
// needs a device or can say it could not run.
inline DeviceInfo openDevice()
{
  DeviceInfo info;
  const std::string path = detail::loaderPath();
  Api& api = info.api;
  api.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (api.handle == nullptr) {
    info.error = "cannot dlopen " + path + ": " + dlerror();
    return info;
  }
  void* h = api.handle;
  const bool resolved =
      detail::symbol(h, "clGetPlatformIDs", api.GetPlatformIDs) &&
      detail::symbol(h, "clGetPlatformInfo", api.GetPlatformInfo) &&
      detail::symbol(h, "clGetDeviceIDs", api.GetDeviceIDs) &&
      detail::symbol(h, "clGetDeviceInfo", api.GetDeviceInfo) &&
      detail::symbol(h, "clCreateContext", api.CreateContext) &&
      detail::symbol(h, "clCreateCommandQueue", api.CreateCommandQueue) &&
      detail::symbol(h, "clCreateBuffer", api.CreateBuffer) &&
      detail::symbol(h, "clEnqueueWriteBuffer", api.EnqueueWriteBuffer) &&
      detail::symbol(h, "clEnqueueReadBuffer", api.EnqueueReadBuffer) &&
      detail::symbol(h, "clFinish", api.Finish) &&
      detail::symbol(h, "clReleaseMemObject", api.ReleaseMemObject) &&
      detail::symbol(h, "clReleaseCommandQueue", api.ReleaseCommandQueue) &&
      detail::symbol(h, "clReleaseContext", api.ReleaseContext);
  if (!resolved) {
    info.error = std::string("the loader has no OpenCL 1.2 entry point: ") + dlerror();
    return info;
  }

  cl_uint platformCount = 0;
  if (api.GetPlatformIDs(0, nullptr, &platformCount) != CL_SUCCESS || platformCount == 0) {
    info.error = "the loader knows no platform (no ICD on OCL_ICD_VENDORS?)";
    return info;
  }
  std::vector<cl_platform_id> platforms(platformCount);
  if (api.GetPlatformIDs(platformCount, platforms.data(), nullptr) != CL_SUCCESS) {
    info.error = "clGetPlatformIDs refused the list";
    return info;
  }
  cl_uint deviceCount = 0;
  if (api.GetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, 0, nullptr, &deviceCount) != CL_SUCCESS ||
      deviceCount == 0) {
    info.error = "the platform lists no device";
    return info;
  }
  std::vector<cl_device_id> devices(deviceCount);
  if (api.GetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, deviceCount, devices.data(), nullptr) !=
      CL_SUCCESS) {
    info.error = "clGetDeviceIDs refused the list";
    return info;
  }

  info.platformName = detail::infoString(api, true, platforms[0], CL_PLATFORM_NAME);
  info.device = devices[0];
  info.deviceName = detail::infoString(api, false, info.device, CL_DEVICE_NAME);
  info.deviceVersion = detail::infoString(api, false, info.device, CL_DEVICE_VERSION);
  info.ok = true;
  return info;
}

} // namespace cl
} // namespace testkit
