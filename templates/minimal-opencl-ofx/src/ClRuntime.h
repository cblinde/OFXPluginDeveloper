// SPDX-License-Identifier: BSD-3-Clause
//
// The OpenCL entry points this plug-in uses, resolved from the loader at
// runtime.
//
// A host that renders with OpenCL owns the context, the device and the command
// queue; the plug-in is handed a queue and the cl_mem objects it may write. It
// still has to compile its kernel for *that* device, which needs
// clCreateProgramWithSource and friends - and those are what this file resolves.
//
// Nothing here is linked in: the machine this repository is developed on has
// `libOpenCL.so.1` and an ICD but no `CL/cl.h`, and a render node is the same
// situation. The types in ClRuntime.cpp are the OpenCL 1.2 ABI, not a
// reimplementation of the API; the loader's name comes from
// OPENCL_LOADER_LIB and defaults to libOpenCL.so.1.
//
// One Runtime per effect instance: the program is built once per context and
// reused while the host keeps handing over the same queue. Building a program
// per render works and is what a simpler plug-in would do - it is also how a
// plug-in ends up spending its render budget in the driver's compiler.

#pragma once

#include <string>

namespace ocl {

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Compiles the gain kernel for the context and device behind `queue`.
    ///
    /// `images` selects which of the two kernels this render needs: the buffer
    /// kernel of the OpenCL Buffers handover or the image2d_t kernel of the
    /// Images handover. Safe to call from every render - it returns true without
    /// rebuilding when the queue and the mode have not changed, and it keeps the
    /// other mode's program so a host that switches back does not pay for a
    /// second compile. On failure `why` says which call failed and, for a
    /// compile error, carries the driver's build log.
    bool prepare(void* queue, bool images, std::string* why);

    /// Enqueues the gain over `width` x `height` pixels of the render window.
    ///
    /// `srcBuffer` and `dstBuffer` are the cl_mem of the two images, taken from
    /// kOfxImagePropData in OpenCL Buffers mode; `pitchComponents` is
    /// kOfxImagePropRowBytes / sizeof(float); `offsetX`/`offsetY` are the render
    /// window's origin relative to the buffer's first pixel. Getting the offset
    /// wrong is how a kernel renders the top-left corner of the frame for a
    /// window somewhere else, which is what the template's suite checks. Does not
    /// wait: ofxGPURender.h tells the plug-in not to finish the host's queue, and
    /// the host's readback is what makes the result observable.
    // const: enqueuing changes the kernel's arguments, not this object's state.
    bool enqueueGain(void* queue, void* srcBuffer, void* dstBuffer, int width, int height,
                     int offsetX, int offsetY, int pitchComponents, float gain,
                     std::string* why) const;

    /// Enqueues the gain over the Images handover: `srcImage` and `dstImage` are
    /// cl_mem image2d_t taken from kOfxImageEffectPropOpenCLImage. An image has
    /// no row stride, so the addressing is pure coordinates - the offsets are
    /// the window origin inside the image, and the engine cannot be told to step
    /// a pointer even by mistake.
    bool enqueueGainImages(void* queue, void* srcImage, void* dstImage, int width, int height,
                           int offsetX, int offsetY, const float gain[4],
                           std::string* why) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace ocl
