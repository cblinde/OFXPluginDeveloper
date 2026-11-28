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
would otherwise dispatch a silently truncated frame. The fourth gate in
`VulkanMode.h` turns that into a CPU frame with the reason in the label (§7),
and `Engine::apply()` still refuses a count it is handed directly.

## 3. Two memory arrangements, and what they cost

The storage buffer has to be readable by the shader. There are two honest ways
to get the frame there, and the template builds both:

| Arrangement | Buffer | Per frame | When |
|---|---|---|---|
| host-visible coherent | one buffer, `HOST_VISIBLE \| HOST_COHERENT`, mapped | `memcpy` in, dispatch, `memcpy` out | smallest correct arrangement; always available |
| device-local + staging | `DEVICE_LOCAL` storage + host-visible staging, mapped | `memcpy` into staging, `vkCmdCopyBuffer` in, dispatch, `vkCmdCopyBuffer` out | the arrangement a real workload uses |

**Measured on one RTX 3060, 1920×1080 RGBA32F, 300 frames each, two runs
back to back in the device suite** (**[V]**,
[`../evidence/2026-11-27-windows/vulkan-300-frames.log`](../evidence/2026-11-27-windows/vulkan-300-frames.log)):

| Path | ms/frame | of which dispatch (submit → fence) | of which CPU copies |
|---|---|---|---|
| host-visible | 102.5 - 106.7 | 13.4 - 15.5 | 89.0 - 91.2 |
| device-local + staging | 117.2 - 117.6 | 26.4 - 28.1 | 89.0 - 91.2 |
| CPU loop (`render()`'s fallback) | 3.0 | - | - |

The arrangement that was added second is the slower one here, by ~14%, and the
columns say why: the copies are ~89 ms either way, and staging puts the frame
over PCIe twice instead of once (**[V]**). The block drops one warm-up frame
before it times (`vulkan_suite.cpp`), so the 4% between two 300-frame runs is
not a cold start - which is why the table above is a range and not a number.

Read it as three facts and one consequence:

- **For a pointwise multiply the GPU loses, and not by a little:** ~102-117 ms
  against 3.0 ms for the CPU loop, i.e. 34× slower at 1080p on this machine
  (**[V]**). A template that showed only the pretty number would be lying to
  whoever copies it.
- The cost is the round trip, not the shader: 33.2 MB in and 33.2 MB out per
  frame (2,073,600 pixels × 16 bytes), and the CPU-side copies alone are ~89 ms,
  i.e. ~0.37 GB/s per direction through the mapping (**[V]**).
- The device-local copies run *on the queue*, so they land in the dispatch
  column instead of the copy column - which is why that column is 28 ms there
  against 13 ms for host-visible, where the shader itself reads host memory and
  the transfers are CPU `memcpy`s. Staging pays PCIe twice for the same frame,
  and at this size that is the whole difference. Device-local earns its place
  when a frame stays resident across passes and the per-pixel work is large; a
  single pointwise pass is the worst case for it.
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
"the shader read device memory". Note what the table above implies for that
default: on this machine, for one pass over a frame that arrives and leaves as
CPU pixels, `Auto` selects the arrangement that measures 14% slower. The plug-in
keeps it because the arrangement is the one a real workload needs and the template
is the place to see both; a plug-in that knows its frames never stay resident
should ask for `HostVisible` explicitly.

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

## 7. Choosing the path: six gates, one environment override

`src/VulkanMode.h` holds the decision between the device and the CPU loop and
nothing else - no Vulkan headers, no OFX headers - so the same table is compiled
into the plug-in and into `test/vulkan_mode_test.cpp`, which executes it on a
machine with no device, no loader and no SDK (**[V]**,
[`../evidence/2026-11-27-windows/vulkan-mode-policy.log`](../evidence/2026-11-27-windows/vulkan-mode-policy.log)).
That is the split `minimal-cuda-ofx`'s `ExecutionMode.h` uses, for the same
reason: the machine that has to trust the gates is the one without the hardware.

| # | Gate | Result |
|---|---|---|
| 0 | `OFXPLUGIN_VULKAN_MODE=cpu` | CPU - the switch that makes the fallback testable where a device exists |
| 1 | built with `-DOFXPLUGIN_VULKAN=OFF` | CPU |
| 2 | no instance, device or compute queue | CPU, with the engine's own reason appended to the label |
| 3 | an empty render window | CPU |
| 4 | more pixels than `maxPixels()` | CPU; the dispatch would be truncated |
| 5 | the device, if gates 0-4 passed | VULKAN_ONLY - the reason says whether the override asked for it or auto chose it |

The override is also the cheapest gate to take: `frameFacts()` in the plug-in asks
it before it probes, so a forced-CPU instance never creates an instance, a device
or a queue - which is what makes the switch usable on a machine whose device is
busy, broken or simply not wanted, and not only on one without a driver.

Gate 5 is the one that can answer with a request or with a default: the override
and auto both land there, and the reason string is what tells them apart - the
policy test asserts both of those reasons, which is why the table has six gates
and the decision has seven answers.

**Every gate can only demote.** `gpu` on a machine without a driver renders on
the CPU and the label says why, which is the rule the CUDA template states for
its own override; `cpu` is the one gate no fact can override. The override reads
four spellings - `auto`, `cpu`, `gpu` and `vulkan`, the last as a synonym for
`gpu` because it is the word a reader of the `Device` label is most likely to
type; the label reports the gate, so both spellings read back as
`OFXPLUGIN_VULKAN_MODE=gpu (or vulkan) was set`. Anything else - a typo, or an
uppercase `CPU` - reads as auto **and** is named in the label as unrecognised:
an override that silently does nothing is how a "the CPU path was tested" claim
gets made about a run that never left the device. The policy test executes all
four spellings plus two unrecognised words.

**A CPU-only configure is the same gate, not a separate build.** With
`-DOFXPLUGIN_VULKAN=OFF` the plug-in and this test compile without
`vulkan/vulkan.h` at all - the device translation unit is only added in the
branch that found the headers, which is a fix this template needed: it used to be
listed unconditionally and the CPU-only configure could not parse it on a machine
without the SDK. That bundle's label reads
`CPU - built with -DOFXPLUGIN_VULKAN=OFF`, and the contract harness asserts the
string in the same log (**[V]**,
[`../evidence/2026-11-27-windows/vulkan-mode-policy.log`](../evidence/2026-11-27-windows/vulkan-mode-policy.log)).

**The override is what made the CPU path testable.** Before it, a machine with a
device always took the device and a machine without one always took the loop, so
no single run could assert both. Both runners now render once with
`OFXPLUGIN_VULKAN_MODE=cpu` and require the `Device` parameter to read `CPU - ...`
after the render: one named check in the contract suite, on a machine whose
device is present and working. A second run does the same with `gpux`, a word
the parser rejects: `VulkanMode.h` reports it as unrecognised, the plug-in reads
that as auto, and the label carries the raw word back - measured
`NVIDIA GeForce RTX 3060 (Vulkan 1.4.357, queue family 0, 4294967040 px per
dispatch); OFXPLUGIN_VULKAN_MODE='gpux' is not auto, cpu or gpu`, with
`--label-contains` asserting the sentence (**[V]**,
[`../evidence/2026-11-27-windows/vulkan-typo-label.log`](../evidence/2026-11-27-windows/vulkan-typo-label.log)).
**The device-less half of the same rule was inferred until it was run.** Both machines
without a device print the sentence as well, and each label carries the gate that
answered it (**[V]**, 2026-11-28,
[`../evidence/2026-11-28-windows/vulkan-typo-cpu-label.log`](../evidence/2026-11-28-windows/vulkan-typo-cpu-label.log)): a bundle configured with
`-DOFXPLUGIN_VULKAN=OFF` labels it `CPU - built with -DOFXPLUGIN_VULKAN=OFF;
OFXPLUGIN_VULKAN_MODE='gpux' is not auto, cpu or gpu`, and a device build on a loader
that finds no driver labels it `CPU - no Vulkan device answered: vkCreateInstance
failed: VK_ERROR_INCOMPATIBLE_DRIVER; OFXPLUGIN_VULKAN_MODE='gpux' is not auto, cpu or
gpu`. The second run hid every ICD by pointing `VK_DRIVER_FILES` at a path that does
not exist, and `vulkan_suite` answered `NOT VERIFIED no Vulkan device: vkCreateInstance
failed: VK_ERROR_INCOMPATIBLE_DRIVER`, so the label is not a claim the suite contradicted.
In both cases the whole runner finished with `all Windows checks passed`, the gpux
assertion running inside step 4 of §8 - which it could not do before the fix recorded
there. That half of the rule - a typo is *visible*, not merely noticed - was unproven
before it: the policy test reads the parser's return value, and the CPU run
only ever set a word the parser accepts. The `.bat` run is the one in the logs;
the `.sh` has not been executed here (§8), so what is verified about it is that
it parses, that it carries the same two runs, and that its device branch is
exercised against stubs. The policy test prints **29 checks**, and they are
the gates rather than the pixels - parsing including the typo and the
case-sensitivity, the empty window, `maxPixels` exactly and one past it, and that
neither override can promote past a gate. That the dispatch agrees with the CPU
loop is still `vulkan_suite`'s question, and it still needs the hardware.

## 8. The two runners, and the steps that decide the template

`test/run-tests.bat` and `test/run-tests.sh` run the same six steps in the same
order - build, the path-policy test, artifact, the host contract through the fake
host (two renders: one with the CPU override, one with `gpux`, a word the parser
rejects), the install round trip,
and the device self-test - and they decide the last one the same way:
**exit 0 is a pass, exit 2 is the suite's "no Vulkan device on this machine" and
reports `NOT VERIFIED`, anything else fails the run**. `NOT VERIFIED` is the
answer for a CPU-only configure as well (`OFXPLUGIN_VULKAN=OFF`, which the shell
runner passes to `build.sh` explicitly), so a machine without the SDK never sees
a green step it did not earn.

**That last sentence was wrong until 2026-11-28.** The label check at the start of
step 6 asked every build for `device=Vulkan`, so an `OFXPLUGIN_VULKAN=OFF` run failed
there - after the self-test had passed on the CPU loop - and never reached the device
step at all; `windows.yml` builds its Vulkan bundle exactly that way, so its step would
have failed on the first CI run. Both runners now expect the compile-time reason on
such a build (`OFXPLUGIN_VULKAN=OFF` in the environment is what they read, and the
shell runner uses the `VULKAN` it already resolved), the run then finishes with
`NOT VERIFIED no device suite binary - configured with OFXPLUGIN_VULKAN=OFF` and
`all Windows checks passed` (**[V]**, 2026-11-28,
[`../evidence/2026-11-28-windows/vulkan-typo-cpu-label.log`](../evidence/2026-11-28-windows/vulkan-typo-cpu-label.log)). The .sh additionally runs valgrind memcheck per host
profile, which the .bat cannot and does not claim.

The shell runner's device branch is exercised without a device - stubs that exit
0, 2 and 7, a missing binary, a non-executable file, a directory and
`OFXPLUGIN_VULKAN=OFF` - in
[`../evidence/2026-11-27-windows/vulkan-device-step-guard.log`](../evidence/2026-11-27-windows/vulkan-device-step-guard.log)
(**V**). The runner itself has not been executed end to end on Linux here: this
machine has Windows, and the repo's `run-tests.sh` sends the Windows path to the
`.bat`.

## 9. What has not been measured here

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
- **No resident-frame sequence.** §3 says staging earns its place when a frame
  stays in device memory across passes and the per-pixel work is large. That case
  is reasoned from the measurement and not measured: every run here sends one
  frame per submit and reads it back, which is the arrangement's worst case. A
  suite that dispatches *k* passes on one upload is what would turn the sentence
  into a number.
