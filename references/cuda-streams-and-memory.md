# CUDA streams, zero-copy and GPU memory ownership

The stream contract in full, why cudaDeviceSynchronize is the wrong call in both modes, what zero-copy actually means, and who owns GPU objects.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 183-311 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 402-422 -->
## 1. Zero copy: what it actually means

"Zero copy" here means **the plugin never copies pixels it could read in place**:

```
Resolve's GPU frame buffer
        |  (host passes the pointer in kOfxImagePropData)
        v
   your kernel  ------------->  the same buffer family, output image
        ^                              ^
        |                              |
   reads src directly            writes dst directly
```

What the plugin *may* copy:

- Parameters, lookup tables, small coefficient arrays — unavoidable and cheap.
- A tile into shared memory inside the kernel — free, on-chip, not a "copy" in
  this sense.

What the plugin must **not** do:

- `cudaMemcpyAsync(dst, src, ..., DeviceToDevice)` into a private staging buffer
  "to be safe". That doubles bandwidth and adds latency without adding safety.
- `cudaMemcpy` host↔device around the kernel. If you see this in a GPU render
  path, either the host is not in CUDA mode (so use the CPU path) or the design
  is wrong.

**Why the pointers are directly usable.** The OFX host creates its GPU buffers in
a CUDA context. If the host uses the CUDA *runtime* API (the usual case), that is
the per-device **primary context**, and your plugin's runtime calls bind to the
same primary context automatically. Two independent libraries that both use the
runtime API therefore share allocations with no explicit context sharing.
If the host instead uses the *driver* API and pushes its own `CUcontext`,
your runtime calls also attach to the current context — which is why you must not
switch devices carelessly (see below).

**Multi-GPU is where this breaks.** A workstation can have Resolve rendering on
GPU 1 while your thread's current device is 0. Then `kOfxImagePropData` belongs
to device 1 and a launch on device 0 raises `cudaErrorInvalidDevicePointer` (or
reads garbage). Bind to the pointer's owner:

```cpp
int device = 0;
cudaGetDevice(&device);
cudaPointerAttributes attrs{};
if (cudaPointerGetAttributes(&attrs, src) == cudaSuccess && attrs.device >= 0) {
    device = attrs.device;
}
// RAII: restore the previous device on scope exit, always.
```

This is exactly what `LaunchGainRGBA32F()` in the template does, and the
`DeviceGuard` restores the thread's device so the host's next call is not
surprised.

---

## 2. Streams: the contract, in full

The specification is short and unambiguous (`ofxGPURender.h`,
`kOfxImageEffectPropCudaStream`):

> If set: this property contains a pointer to the stream of CUDA render
> (`cudaStream_t`). In order to use it, `reinterpret_cast<cudaStream_t>(pointer)`
> is needed. The plug-in SHOULD ensure that its render action enqueues any
> asynchronous CUDA operations onto the supplied queue. The plug-in SHOULD NOT
> wait for final asynchronous operations to complete before returning from the
> render action, and SHOULD NOT call `cudaDeviceSynchronize()` at any time.
>
> If not set: the plug-in SHOULD ensure that any asynchronous operations it
> enqueues have completed before returning from the render action.

Quoted verbatim from `ofxGPURender.h` lines 608–617 **[V]**. Read that as two
modes:

| Host supplied a stream | Plugin behaviour |
| --- | --- |
| **Yes** | Enqueue on it. Return immediately. Do not sync. Do not call `cudaDeviceSynchronize()` at any time — the quote above is a SHOULD NOT, and the host's stream is what makes waiting unnecessary. |
| **No** | Enqueue on the default stream (or your own) and **synchronise that stream before returning**. |

### Why `cudaDeviceSynchronize()` is the wrong call in both modes

It is not about performance. `cudaDeviceSynchronize()` blocks until *every*
stream on the device is idle — including the host's own queued work for other
instances, the UI, and its colour pipeline. Inside a render call it can deadlock
(some hosts submit work from another thread that needs a lock the render thread
holds) and it destroys the frame pipelining the host built. The stream you were
handed is the host's ordering primitive; respect it and nobody has to block.

### Why the sync in the no-stream case is mandatory

Without a stream, the host has no handle to order against. It will assume that
when `render()` returns, the pixels are valid — sometimes immediately, e.g. when
it maps the buffer for a preview or a write. An unsynchronised kernel then races
the host's read, producing tearing, stale frames, or a crash in the GPU driver.
This is why "always async, never sync" is wrong advice: it is only correct in
mode 1.

### Default stream pitfalls

- The legacy default stream (stream 0) synchronises implicitly with other
  blocking streams. Fine for a plugin, but it serialises: do not launch the same
  kernel on stream 0 *and* on the host's stream.
- Compiling with `--default-stream per-thread` changes what `nullptr` means.
  Keep the default (`legacy`) unless the whole stack agrees.
- `cudaStreamSynchronize(nullptr)` synchronises the default stream only — the
  correct call for mode 2. `cudaDeviceSynchronize()` is not.

### Ordering you get for free, and ordering you must not assume

With a host stream, your kernel is ordered after everything the host enqueued on
that stream before the render call, and the host's later reads are ordered after
your kernel. You may **not** assume anything about work on *other* streams.

### NVTX, so the profiler can see you

Wrap the launch in an NVTX range so the render shows up as a named band in Nsight
Systems instead of an anonymous kernel:

```cpp
#include <nvtx3/nvToolsExt.h>
nvtxRangePushA("MyEffect::render");
// ... launch ...
nvtxRangePop();
```

---

## 3. Streams, and what changes when the host gives you one

- The stream property is `pointer X 1` in the `inArgs` of **Render**,
  **BeginSequenceRender** and **EndSequenceRender** **[S]**. Treat it as opaque:
  store as `void*`, cast at launch, never create, never destroy.
- It is only set when *both* sides advertise
  `kOfxImageEffectPropCudaStreamSupported = "true"` **[S]**. The template
  advertises it and does not depend on it.
- With a host stream: enqueue and return. The host orders its own work against
  that stream, and the spec forbids waiting — *"the plug-in SHOULD NOT wait for
  final asynchronous operations to complete… SHOULD NOT call
  cudaDeviceSynchronize() at any time"* **[S]**.
- Without a stream: your work must be complete before `render` returns **[S]**.
  That is the one case where a synchronise is mandatory rather than forbidden.
  The template implements exactly this branch **[S]**.
- Do not create your own stream per frame. If you need private ordering, create
  it once per instance and let the host stream be the join point, or just use
  the host's.

---

## 4. Execution modes: CPU_ONLY, CUDA_ONLY, CPU_CUDA_HYBRID

Not every question here is answered by a kernel. A render can run on the CPU, on
the GPU, or with the two engines busy at once - and only one of those three is
safe to pick by hardware detection alone. This repository answers it with four
gates, one function, and a test that runs without a GPU.

| Piece | Where it lives | Status |
|---|---|---|
| The four gates, override parser, decision reasons | `templates/minimal-cuda-ofx/src/ExecutionMode.h` | **[V]** - no CUDA headers, compiled into the plug-in and into the test |
| The gate truth table and the Device label, 38 cases | `templates/minimal-cuda-ofx/test/execution_mode_test.cpp`, run by both template runners | **[V]** 38 cases, 0 failed - four of them the label, which print what they read | - log: [`cuda-label-branches.log`](../evidence/2026-11-28-windows/cuda-label-branches.log), and [`execution-modes.log`](../evidence/2026-11-27-windows/execution-modes.log) for the gates before the label cases |
| Device memory query | `DeviceMemoryFreeBytes()` in `Kernel.cu`, `cudaMemGetInfo` under the hood | **[V]** compiled and linked; the value is read at render time |
| The decision at render time | `Plugin.cpp`, `processImagesCuda()`, logged once per render with its reason **and** written into the read-only `device` parameter | **[V]** for the string: `PathLabel()` in `ExecutionMode.h` gives the mode and reason, asserted for both branches; the **property write during a CUDA render is unproven** - see 4.0 |
| Override | `OFXPLUGIN_GPU_MODE=auto|cpu|cuda|hybrid`; an unknown word is reported, not silently ignored | **[V]** - the parser in the 34-case test, and the word itself on the `device` label, asserted by a contract run in both template runners (2026-11-28, [`../evidence/2026-11-28-windows/cuda-typo-label.log`](../evidence/2026-11-28-windows/cuda-typo-label.log)) |
| Overlapping CPU work with the kernel | no template has independent CPU work to overlap | **not implemented**; see below |

### 4.0 Which side of the label has been seen

The label a host reads is one string, but it is written from two places and only
one of them has ever been observed:

| What | Proven | How |
|---|---|---|
| `CPU_ONLY - ...` reaches the `device` parameter | **yes** | a contract run whose fake host hands host pointers - the only host on this machine that renders - asserts the parameter; the typo sentence on it is in [`cuda-typo-label.log`](../evidence/2026-11-28-windows/cuda-typo-label.log) |
| `CUDA_ONLY - ...` as a string, both reasons, with and without the typo sentence | **yes** | `execution_mode_test`, four cases, through the same `PathLabel()` the plug-in calls; it prints each label, and they read `CUDA_ONLY - CUDA_ONLY requested`, `CUDA_ONLY - no independent CPU work to overlap`, and the same two with `; OFXPLUGIN_GPU_MODE='gpux' is not auto|cpu|cuda|hybrid` appended ([`cuda-label-branches.log`](../evidence/2026-11-28-windows/cuda-label-branches.log)) |
| `CUDA_ONLY - ...` reaching the `device` parameter during a **render on the GPU** | **no** | the branch runs only when a host sets `kOfxImageEffectPropCudaEnabled` and hands device pointers. `FakeHost::render()` sets that property to 0 at all three call sites, so the contract suite cannot reach it; Resolve's script server refused every connection on 2026-11-28 (§`resolve-automation.md` 14.4) and Natron is not installed here. The string is proven; the property write is not |

That third row is the honest hole. Closing it needs one of two things: a fake host
that allocates with `cudaMalloc`, hands the pointers and the stream, and reads the
frame back - which is a real change to `testkit/fake_host.cpp` and is backlog row
91 - or a host run on a machine whose script server answers. Until then a reader
should treat the CUDA-side label as "the text is right, the delivery is untested",
not as "the host shows CUDA_ONLY".

### 4.1 The gates

Each gate can only demote. A host that did not hand device pointers is not going
to be argued into a CUDA render by an override: the pixels are host memory, and
a kernel pointed at them is a crash or a silent corruption, not a bug to
optimise.

1. **Host and device.** `kOfxImageEffectPropCudaEnabled` for *this render* is the
   only authority on where the pixels live - the descriptor's
   `kOfxImageEffectPropCudaRenderSupported` is a capability, not a fact. No
   property, or no device answering `cudaMemGetInfo`, means `CPU_ONLY`.
2. **VRAM.** Two frames (source and destination) plus 256 MiB of headroom must be
   free. Below that the render is either a `kOfxStatGPUOutOfMemory` or a driver
   allocator that thrashes against the host's own allocations, and the CPU
   fallback is the better answer. Exactly the requirement is enough; the rule is
   strictly below.
3. **Transfer balance.** With the pixels already on the device there is no copy,
   and a copy estimate must not demote the render - that is the normal OFX GPU
   path, where the host hands device pointers and the plug-in never touches
   PCIe. The gate exists for an effect that has to upload: when the copy is
   estimated at more than 1.25x the kernel, `CPU_ONLY`.
4. **Independent CPU work.** `CPU_CUDA_HYBRID` needs work that can run while the
   kernel does, and a kernel worth overlapping (>= 1 ms). Without both, the
   answer is `CUDA_ONLY`.

The thresholds are policy, not measurement: 256 MiB and 1.25x are stated so a
reader can disagree with a number instead of with an adjective. The 1 ms floor
comes from the same place as §2's stream rules - below that, launch and
synchronisation overhead is the work.

**The override has a channel a host keeps.** The gates log their decision through
`OFX::Log::print`, and Resolve keeps none of what a plug-in prints (measured
2026-11-26), so the same decision is written into a read-only `device` string
parameter after every render: `CPU_ONLY - the host did not set
kOfxImageEffectPropCudaEnabled, so the pixels are host pointers`, with
`; OFXPLUGIN_GPU_MODE='gpux' is not auto|cpu|cuda|hybrid` appended when the word was
not understood. The plain CPU path - the one the Support library runs when the host
never enabled a CUDA render, which never reaches the gates - writes the gate-1
reason through the same helper, because a second copy of the text is how one author
of the label ends up without the sentence (that happened on the first attempt, and
the check described here caught it). A contract run with `OFXPLUGIN_GPU_MODE=gpux` in its own
process asserts the sentence with `--label-contains` (**[V]**, 2026-11-28,
[`../evidence/2026-11-28-windows/cuda-typo-label.log`](../evidence/2026-11-28-windows/cuda-typo-label.log)). Before it, the typo was proven at the parser and
nowhere else: `execution_mode_test` reads the return value, and no contract run had
ever asked the label.

**The facts are gathered only where a device path exists.** `RenderFacts()` lives
inside `#ifdef OFXPLUGIN_WITH_CUDA`, because `DeviceMemoryFreeBytes()` is declared
in that same block of `Kernel.cuh` and nothing else calls it. Every build the
runners made until 2026-11-28 was the CUDA one, so the `CPU_ONLY=1` build - the
fallback path `windows.yml` compiles before it installs a toolkit - went
uncompiled until that step's own commands were run here and the compiler named it
(`error C3861: "DeviceMemoryFreeBytes"`) (**[V]**, 2026-11-28,
[`../evidence/2026-11-28-windows/windows-job-local.log`](../evidence/2026-11-28-windows/windows-job-local.log)).
The guard is in place and the gap that hid it is closed: `./run-tests.sh` now runs
that configure as its own step on both routes - `CPU_ONLY=1` into
`build-cpu-only`, artifact check without `--cuda`, then the contract suite - and
the step has been shown failing on the break it was written for (exit 1,
`error C3861`) and passing when the guard is back (**[V]**,
[`../evidence/2026-11-28-windows/cpu-only-compile-gate.log`](../evidence/2026-11-28-windows/cpu-only-compile-gate.log)).
**What that gate does not prove:** it uses no device. It does not compile the
device path, demands no cubin, launches no kernel and cannot reach the `CUDA_ONLY`
side of the Device label (§4.0). It answers one question - does the fallback still
compile and pass its checks - because that is the question that broke.

### 4.2 What hybrid means here, and what it does not

It never means splitting one frame between CPU and GPU. OFX hands a render
window to one process call, the Support library already bands the CPU path
across `nCPUs` (`performance-model.md` §2.4), and the GPU path is one grid for
the whole window (`performance-model.md` §2.2). Splitting a frame between the
engines adds a synchronisation point per tile and loses - PCIe and event waits
cost more than the divide saves at every size this repository has measured.

The overlap is between the kernel and *independent* work: a LUT or curve
precomputation, a histogram, tracking data, staging for the next frame. Three
conditions, all of them from the specification and all of them cheap to check:
the CUDA work goes on the stream the host supplied (§2 - a private stream that
the host cannot order against is a correctness bug, not an optimisation); the
CPU work touches no pixel the kernel is writing; and the result is joined before
`render()` returns in the no-stream case, or enqueued on the host's stream when
there is one.

No template in this repository has such work. A gain is four multiplies per
pixel and nothing else, so `hasIndependentCpuTask` is false in the CUDA template,
auto never selects `CPU_CUDA_HYBRID` there, and the hybrid path is exercised by
the policy test rather than by a render. Stated here rather than implied by the
enum: the mode exists, the overlap does not, and no timing of it appears in any
table in this repository.

### 4.3 When each mode is the right one

| Mode | Choose it when | In this repository |
|---|---|---|
| `CPU_ONLY` | no CUDA device or no host GPU path; not enough free VRAM; a copy that costs more than the kernel; a user who asked for the deterministic software path | every template without a GPU; any host with `kOfxImageEffectPropCudaEnabled` off |
| `CUDA_ONLY` | pixels are device-resident (zero copy), the pipeline is GPU-bound, and there is no non-pixel work to overlap | `minimal-cuda-ofx` on a host with the GPU path on |
| `CPU_CUDA_HYBRID` | the above, plus analysis or preparation work that is genuinely independent and a kernel of 1 ms or more | policy only - no template has such work yet |

### 4.4 Benchmark protocol for the three modes

The rules exist because three numbers produced differently are three numbers that
cannot be compared. `tools/bench_ep.py` already implements the percentile and
warmup parts for the inference providers; the same rules apply to a mode
comparison:

- **Matrix.** 1920x1080 and 3840x2160, at RGBA8, RGBA16F and RGBA32F, over three
  workload shapes: compute-bound (gain), transfer-bound (a frame that must be
  uploaded and read back), and GPU-plus-independent-CPU work.
- **Warm-up and isolation.** Discard cold-start runs - context creation, module
  load, JIT, model warm-up - and report cold and warm separately. Fix the clock,
  close the host, and never compare a run taken while a Resolve render was in
  flight with one taken on an idle machine.
- **Metrics.** Wall-clock per frame (ms and FPS), p50/p95/p99 latency over at
  least 100 frames, Host->Device and Device->Host time from `cudaEvent_t` pairs
  around the copies, kernel time from an event pair around the launch, CPU thread
  utilisation, and peak VRAM sampled with `cudaMemGetInfo` between frames.
- **Parity, before timing.** Every mode must produce the same frame within 1 LSB
  of the reference for the integer depths, and within the tolerance the reference
  carries for float. A faster mode that changes the picture is not a mode; that
  is what the contract suite's golden compares exist for.

What has been measured here is one kernel on one device: the throughput figures
in `performance-model.md` and the OpenCL/CUDA rows in `testing-layers.md` §13.
No CPU-vs-GPU-vs-hybrid table exists for the same frame, and none is claimed.

### 4.5 References

- OFX GPU render properties and the host-stream contract: `ofxGPURender.h` in an
  openfx checkout (`kOfxImageEffectPropCudaStream`, lines 608-617), quoted in §2.
- CUDA Programming Guide, *Asynchronous Execution*:
  <https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html>
  - streams, events, and what may overlap.
- CUDA Runtime API, *Memory Management* (`cudaMemGetInfo`, `cudaMallocHost`,
  `cudaFreeHost`):
  <https://docs.nvidia.com/cuda/cuda-runtime-api/cuda_runtime_api/group__CUDART__MEMORY.html>
- CUDA C++ Best Practices Guide, *Asynchronous and Overlapping Transfers with
  Computation* (pinned memory and `cudaMemcpyAsync`):
  <https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html>

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
