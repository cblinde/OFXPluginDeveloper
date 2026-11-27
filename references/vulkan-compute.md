# Vulkan compute from inside an OFX plug-in

**Scope.** What a Vulkan path looks like when the plug-in owns the device, which
is the only arrangement an OFX host allows. `templates/minimal-vulkan-ofx` is the
working version of everything here; `cmake-and-linking.md` §6 covers the build
side (headers yes, import library no) and this file covers the runtime side.

Labels: **[V]** ran in this repository, **[I]** inferred, **[B]** external
evidence.

## 1. Why the plug-in owns the device

No OFX host hands a Vulkan instance, device, queue or buffer to a plug-in: OFX
1.5.1 has `kOfxImageEffectPropOpenCL*` and nothing Vulkan-shaped
(`host-nuke-scratch-natron.md` §1.1). A Vulkan render therefore starts from a CPU
image and ends with a CPU image, and everything between - instance, device,
queue, memory, pipeline, submission - belongs to the plug-in.

That is not what `Gyom03/rife-ofx` does at the source level (**[B]**): it builds
NCNN with `NCNN_VULKAN` and lets NCNN own the device. What transfers is the
arrangement, not the code: one device for the plug-in's lifetime, frames copied
in and out, and no attempt to make the host agree.

## 2. The objects, in the order they have to be created

1. **Loader**, opened by name (`LoadLibraryW(L"vulkan-1.dll")` /
   `dlopen("libvulkan.so.1")`), never linked. `vkGetInstanceProcAddr` from it is
   the root for everything else; `vkGetDeviceProcAddr` from the instance is the
   root for device-level entry points.
2. **Instance** with `apiVersion = min(instance version, 1.1)`. Nothing below
   needs 1.2+ features, and asking for a version the driver does not have fails
   at creation instead of later.
3. **Physical device** chosen as "the first one with a `VK_QUEUE_COMPUTE_BIT`
   family". Picking by discrete-vs-integrated or by name is a policy; a template
   should show the mechanical choice and say so.
4. **Device** with one queue from that family, and no features requested - a
   storage buffer, push constants and a compute pipeline are all core.
5. **Descriptor set layout** (one storage buffer at binding 0), **pipeline
   layout** with a push-constant range, **shader module** from the embedded
   SPIR-V, **compute pipeline**.
6. **Descriptor pool, set, command pool, one command buffer, one fence.** One of
   each is enough: the plug-in renders one frame at a time per instance.

`Engine::maxPixels()` exists because `maxComputeWorkGroupCount[0]` is a real
limit: 4 294 967 040 pixels per dispatch on the RTX 3060 measured here (**[V]**,
`vulkan_suite`), which is far above any frame, but a device with a small limit
would otherwise dispatch a silently truncated frame. The template refuses the
frame instead.

## 3. Two memory arrangements, and what they cost

The storage buffer has to be readable by the shader. There are two honest ways
to get the frame there, and the template builds both:

| Arrangement | Buffer | Per frame | When |
|---|---|---|---|
| host-visible coherent | one buffer, `HOST_VISIBLE \| HOST_COHERENT`, mapped | `memcpy` in, dispatch, `memcpy` out | smallest correct arrangement; always available |
| device-local + staging | `DEVICE_LOCAL` storage + host-visible staging, mapped | `memcpy` into staging, `vkCmdCopyBuffer` in, dispatch, `vkCmdCopyBuffer` out | the arrangement a real workload uses |

**Measured on one RTX 3060, 1920×1080 RGBA32F, 20 frames each, in the device
suite; four runs, last three in the table** (**[V]**,
`evidence/2026-11-27-windows/vulkan-suite.log`):

| Path | ms/frame | of which dispatch (submit → fence) | of which CPU copies |
|---|---|---|---|
| host-visible | 96.6 - 98.5 | 5.5 - 7.1 | 90.3 - 93.0 |
| device-local + staging | 108.3 - 111.0 | 16.7 - 18.2 | 91.5 - 92.8 |
| CPU loop (`render()`'s fallback) | 3.5 - 4.0 | - | - |

Read it as three facts and one consequence:

- **For a pointwise multiply the GPU loses, and not by a little:** ~97-111 ms
  against ~3.8 ms for the CPU loop, i.e. 25-30× slower at 1080p on this machine
  (**[V]**). A template that showed only the pretty number would be lying to
  whoever copies it.
- The cost is the round trip, not the shader: 132.7 MB in and 132.7 MB out per
  frame, and the CPU-side copies alone are ~90 ms, i.e. ~1.5 GB/s per direction
  through the mapping (**[V]**).
- The device-local copies run *on the queue*, so they land in the dispatch
  column instead of the copy column - which is why that column is 17-18 ms there
  against 5-7 ms for host-visible, where the shader itself reads host memory and
  the transfers are CPU `memcpy`s.
- Consequence: a Vulkan path pays 265 MB of traffic per 1080p frame before the
  kernel does any work. It wins when the per-pixel work is large enough to make
  that traffic the cheap part - a network, a denoise, a resample, a warp - and it
  loses for anything a CPU can do in a few passes. Deciding that with numbers is
  what the suite's throughput block is for; the first run of the day measures
  cold pages and reported 19.8 ms for the CPU loop, which is why the table above
  quotes the warm runs and not that one.

`Memory::Auto` picks device-local when the device has a device-local memory type
without host visibility, and host-visible otherwise; `Engine::memory()` reports
which one ran, because "the frames went through the GPU" is not the same claim as
"the shader read device memory".

## 4. The barriers are the part that is easy to get wrong

Every hand-off between a producer and a consumer in the same command buffer needs
a barrier; without one the code works most of the time, which is worse than
failing. The template records, in order:

1. `HOST_WRITE` → `TRANSFER_READ` on the staging buffer (the CPU's `memcpy`
   happened before `vkQueueSubmit`).
2. `TRANSFER_WRITE` → `SHADER_READ|SHADER_WRITE` on the storage buffer after the
   upload copy.
3. `SHADER_WRITE` → `TRANSFER_READ` on the storage buffer before the download
   copy.
4. `TRANSFER_WRITE` → `HOST_READ` on the staging buffer after it.

The host-visible path needs the same treatment with the host stages as
`VK_PIPELINE_STAGE_HOST_BIT`: `HOST_WRITE` → shader before, `SHADER_WRITE` →
`HOST_READ` after. Coherent memory makes the *data* visible; it does not make the
*ordering* right.

## 5. Synchronisation: one fence, never a device-wide wait

The plug-in waits on the fence it submitted with (`vkWaitForFences`, one fence,
`VK_TRUE`, no timeout). `vkQueueWaitIdle`/`vkDeviceWaitIdle` would also work and
would also idle every other plug-in in the host's process - the same rule
`cuda-streams-and-memory.md` §1 states for a CUDA stream. One command buffer, one
submit, one wait per frame is the model; a plug-in that keeps several frames in
flight needs a fence per frame and a ring of buffers, which is a different
design with its own measurements.

## 6. The shader

`shaders/gain.comp` is plain GLSL compute: `layout(local_size_x = 256)`, a
`std430` storage-buffer array of `vec4`, push constants for the gain and the pixel
count, and the guard `if (i >= push.count) return;` for the tail workgroup.

The workgroup size and the C++ dispatch arithmetic have to agree -
`(count + 256 - 1) / 256` groups - and nothing checks that at compile time. The
suite therefore dispatches 61, 256 and 257 pixels: a tail that is partly outside
the buffer, exactly one workgroup, and one pixel past it (**[V]**).

SPIR-V is produced at build time by `glslc`/`glslang` and embedded as a header
(`cmake/spv-header.cmake`), so the bundle carries no `.spv`, nothing resolves a
shader path at run time, and the shader compiler is a build dependency only.

## 7. The two runners, and the one step that decides the template

`test/run-tests.bat` and `test/run-tests.sh` run the same five steps in the same
order - build and artifact, the host contract through the fake host, the install
round trip, and the device self-test - and they decide the last one the same way:
**exit 0 is a pass, exit 2 is the suite's "no Vulkan device on this machine" and
reports `NOT VERIFIED`, anything else fails the run**. `NOT VERIFIED` is the
answer for a CPU-only configure as well (`OFXPLUGIN_VULKAN=OFF`, which the shell
runner passes to `build.sh` explicitly), so a machine without the SDK never sees
a green step it did not earn. The .sh additionally runs valgrind memcheck per host
profile, which the .bat cannot and does not claim.

The shell runner's device branch is exercised without a device - stubs that exit
0, 2 and 7, a missing binary, a non-executable file, a directory and
`OFXPLUGIN_VULKAN=OFF` - in
[`../evidence/2026-11-27-windows/vulkan-device-step-guard.log`](../evidence/2026-11-27-windows/vulkan-device-step-guard.log)
(**V**). The runner itself has not been executed end to end on Linux here: this
machine has Windows, and the repo's `run-tests.sh` sends the Windows path to the
`.bat`.

## 8. What has not been measured here

- **No host render.** No host has loaded the Vulkan template and rendered a
  frame; the device suite and the host contract suite are the two halves that
  exist, and `verification-matrix.tsv` records nothing for this template in
  Resolve, Nuke or Natron.
- **No validation layers.** `VK_LAYER_KHRONOS_validation` is not installed on the
  machine these numbers come from, so the template neither enables it nor claims
  a layer-clean run. Enabling it is the first thing to do on the machine that has
  it.
- **One GPU, one driver.** NVIDIA RTX 3060, Vulkan 1.4.357. Nothing here says how
  AMD or Intel drivers behave, and nothing selects between two GPUs beyond "the
  first with a compute queue".
- **No tiling.** The frame is one buffer and the plug-in refuses anything over
  `maxPixels()`; a host that hands a 16K frame to this path would need the tiling
  `roi-tiling-and-render-hints.md` describes.
- **No in-flight pipelining.** One frame, one submit, one wait: no overlap of the
  upload of frame *n+1* with the dispatch of frame *n*, which is where further
  throughput would come from.
