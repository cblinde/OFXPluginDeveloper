// SPDX-License-Identifier: BSD-3-Clause
//
// The kernel, as source, compiled by the driver at render time.
//
// It is the same arithmetic as the CPU path in Plugin.cpp - one multiply per
// colour channel, alpha copied - so the two paths can be compared pixel for
// pixel. A plug-in that
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
    // RGB only, and alpha is carried over untouched: a gain is a colour
    // operation, so it must not change coverage (see the note on the CPU path in
    // Plugin.cpp and references/colour-and-alpha.md section 2). The destination
    // is a separate buffer, so alpha still has to be written.
    pixel.xyz *= gain;
    vstore4(pixel, 0, dst + index);
}
)CLC";

inline constexpr const char* kGainImageKernelName = "gain_rgba32f_image";

// The Images handover: two image2d_t, no pitch and no row padding, addressed
// with image coordinates. The sampler is a compile-time constant so the plug-in
// does not create or release one - nearest-neighbour and clamp-to-edge, because
// the kernel never reads outside the window it was given.
inline constexpr const char* kGainImageKernelSource = R"CLC(
__constant sampler_t nearestClamp = CLK_NORMALIZED_COORDS_FALSE |
                                    CLK_ADDRESS_CLAMP_TO_EDGE |
                                    CLK_FILTER_NEAREST;

__kernel void gain_rgba32f_image(__read_only image2d_t src,
                                 __write_only image2d_t dst,
                                 const int width,
                                 const int height,
                                 const int offsetX,
                                 const int offsetY,
                                 const float4 gain)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int2 p = (int2)(offsetX + x, offsetY + y);
    float4 pixel = read_imagef(src, nearestClamp, p);
    // `gain` is a float4 here (the host hands over one factor per channel), so
    // RGB takes its three and alpha is passed through unscaled.
    pixel.xyz *= gain.xyz;
    write_imagef(dst, p, pixel);
}
)CLC";

} // namespace ocl
