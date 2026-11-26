// SPDX-License-Identifier: BSD-3-Clause
//
// The kernel, as source, compiled by the driver at render time.
//
// It is the same arithmetic as the CPU path in Plugin.cpp - one multiply per
// channel - so the two paths can be compared pixel for pixel. A plug-in that
// only ships the GPU path has nothing to compare it against.
//
// Addressing is the part that differs from a textbook kernel: the host hands
// over a cl_mem for the whole allocation and a row pitch in bytes, and the
// buffer's first row is the image's `bounds.y1`, not frame row 0. So the index
// is (offsetY + y) * pitch + (offsetX + x) * 4, in floats, where the offsets are
// the render window's origin relative to the buffer's first pixel. The row
// padding past `width` is never touched.

#pragma once

namespace ocl {

inline constexpr const char* kGainKernelName = "gain_rgba32f";

inline constexpr const char* kGainKernelSource = R"CLC(
__kernel void gain_rgba32f(__global const float* src,
                           __global float* dst,
                           const int width,
                           const int height,
                           const int offsetX,
                           const int offsetY,
                           const int pitchComponents,
                           const float gain)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = (offsetY + y) * pitchComponents + (offsetX + x) * 4;
    float4 pixel = vload4(0, src + index);
    pixel *= gain;
    vstore4(pixel, 0, dst + index);
}
)CLC";

} // namespace ocl
