# The performance model

What costs time on the host and in the kernel, the measured numbers, and how to prove what you built.

<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 132-401 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 423-478 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 1-26 -->
## Best Practices and Performance

A review of this repository and of the reference projects that already ship
GPU-accelerated OFX plugins, reduced to rules you can act on. Every claim
carries an evidence grade, because the difference between "the spec says" and
"someone's blog says" is the difference between a bug you can fix and a bug you
cannot reproduce.

| Grade | Meaning |
| --- | --- |
| **[S]** | Verified by reading `OFX_Release_1.5.1` source (`include/`, `Support/`) while writing this |
| **[M]** | Measured here with the real toolchain (nvcc 12.9.41 / 13.0.48, GCC 13.3, CMake 3.31.6 / 4.4.4) — static and PTX level, and since 2026-11-24 also on an RTX 3060 through `templates/minimal-cuda-ofx/test/gpu-kernel-test.cu` |
| **[D]** | Stated by official documentation (OpenFX reference, NVIDIA CUDA C++ Best Practices Guide) |
| **[R]** | Stated by a reference project's own README/source |
| **[A]** | Arithmetic derived from a formula in this document |
| **[U]** | Not verified here — treat as a hypothesis and measure it yourself |

Only the gain kernel is benchmarked, and only on one RTX 3060 (2026-11-24): its
throughput, the CPU loop it replaces and the cost of a frame copy are tabulated in
[`../README.md`](../README.md) §"What has actually been verified", with the raw run in
`evidence/2026-11-24-windows/`. Every other performance statement here — about a
model, a tiled inference, another GPU, a host's scheduling — is reasoning, and the
**[U]** grade marks it as such.

---

## 1. The cost model: what actually costs time

**The GPU path is the first path, not the fallback.** For anything
compute-bound — a convolution, a model run per frame, an iterative solver —
write the GPU path first and keep the CPU path as its comparison and as what
runs when the host gives no device. The table below decides whether the GPU path
pays on your frame; it does not decide whether to have one. The model case, with
the provider and its packaging, is `ai-architectures-and-runtimes.md` §4 and §6.

Before optimising, know the shape of the budget. One render action, 1920×1080
RGBA32F, one thread per pixel:

| Cost | Scale | Notes |
| --- | --- | --- |
| Host overhead before your code (fetch image, params, suite calls) | µs–ms | Dominated by the host, not you. Unfixable from inside. |
| Your per-render CPU work (allocation, attribute queries, launch) | µs | Everything here is per *frame*, so it is multiplied by frames×threads. |
| Kernel execution | 10s–100s of µs for elementwise work | Memory-bound: bytes touched ÷ achievable bandwidth. |
| Synchronisation (only when the host gave no stream) | µs–ms | Blocks the calling thread until the GPU is idle. |
| A stray `cudaMemcpy` device→host→device | ms | The single most common accidental cost. |

**[A]** The elementwise floor: RGBA32F reads 16 B and writes 16 B per pixel, so
1920×1080 (2.07 Mpix) moves ≈66 MB. At 1 TB/s that is ≈66 µs; at 400 GB/s,
≈166 µs. If your measured kernel time is far above that, you are not
bandwidth-limited, you are doing something else wrong. If it is far *below*,
you are probably not writing the pixels you think you are.

**[A]** Launch overhead is fixed per launch, so the number of launches per frame
is the thing to control. One launch for a 1080p frame is nothing; one launch per
row is 1080× the overhead. For elementwise work, the whole frame in one grid is
almost always right.

---

## 2. Host-side practices

### 2.1 Never allocate per frame if the size did not change

`cudaMalloc`/`cudaFree` are synchronising, globally serialising operations **[D]**.
Calling them inside a render action makes your plugin a pipeline barrier: the
host cannot overlap anything with you, and every other GPU node in the graph
stalls behind your allocation.

The template avoids this entirely by never allocating — it renders in place
into the host's buffer. The moment you need scratch memory (blur, LUT, FFT),
follow the pattern:

- keep a per-instance cache keyed on `(bytes, device)`,
- grow-only, never free inside `render()`,
- free in the destructor and in `purgeCaches()` (`kOfxActionPurgeCaches`, which
  exists precisely for this **[S]**).

`ImageProcessor::preProcess()` is the natural place for per-render setup and
`postProcess()` for teardown; both are called once per render, around the
dispatch, in `ofxsProcessing.h` **[S]** — they are *not* per thread.

### 2.2 Your GPU path is called once per render — do not thread it yourself

`ImageProcessor::process()` dispatches exactly once:

```cpp
if (_isEnabledOpenCLRender)      processImagesOpenCL();
else if (_isEnabledCudaRender)   processImagesCuda();
else if (_isEnabledMetalRender)  processImagesMetal();
else                             multiThread(nCPUs);   // CPU only
```

**[S]** (`Support/include/ofxsProcessing.h`). The GPU branches do **not** call
`multiThread`. So `processImagesCuda()` must launch one grid covering the whole
render window — if you launch per band, or spawn your own threads around it, you
multiply launches by the CPU count for no gain. The GPU is already parallel.

### 2.3 `supportsTiles` decides how big a window you get

With `setSupportsTiles(false)` **[S]** in the template, the host should hand you
a whole-image render window. With tiles *on*, the same effect is called many
times with small windows, and your per-launch overhead becomes the dominant cost.

Which setting is right is a property of the *effect*, not of the GPU:

- a **local** effect (blur, upscale, denoise) is computable for one window plus
  a halo, and the halo is what tiling costs. That is the trade; declare the halo
  the render actually reads (the templates' `halo` parameter plus the
  `--roi-expand` check are that pair) and budget for the launch count.
- a **global** effect (object detection, NMS, a letterbox scale taken from the
  window's aspect, a frame-wide statistic) has no finite halo that makes a
  window self-sufficient. Turning tiles on does not merely make it slower, it
  makes it inconsistent: the scale changes between windows, an object on a tile
  border is seen twice, half each time, and any per-window state multiplies.
  `setSupportsTiles(false)` — and `setSupportsMultiResolution(false)` with it,
  because a proxy render is a different scale — is then the only setting under
  which the output is defined.

Also note: `templates/onnx-inference-ofx` refuses a window that is not inside the
source **or** the destination bounds — `render()` throws `kOfxStatErrValue` for
either, so the host's error channel carries it instead of a frame that merely
looks rendered. An older form of this template returned quietly, and the
distinction is the point: a silent return turns "the host handed me a window I
cannot write" into a frame that is not drawn while the status says success. If you
keep a quiet return anywhere, log it once before assuming the GPU failed.

### 2.4 Do not add threads to the CPU path

The Support library already slices the render window into contiguous row bands
and runs them on `nCPUs` threads, where
`nCPUs = clamp((min(width,4096) × height) / 4096, 1, getNumCPUs())` **[S]**.
`multiThreadProcessImages` is therefore called concurrently, once per band, with
the full width and a slice of rows. Adding a thread pool inside it nests
oversubscription.

What *is* worth optimising there: the inner loop. `OFX::Image::getPixelAddress(x,y)`
is declared in the header and **defined out of line** **[S]**, so calling it per
pixel is a real function call per pixel through the Support library. Hoist the
row pointer:

```cpp
for (int y = window.y1; y < window.y2; ++y) {
    const float* srcRow = _src ? static_cast<const float*>(_src->getPixelAddress(window.x1, y)) : nullptr;
    float* dstRow = static_cast<float*>(_dstImg->getPixelAddress(window.x1, y));
    for (int x = 0; x < window.x2 - window.x1; ++x, srcRow += 4, dstRow += 4) { … }
}
```

The template now does exactly this: `multiThreadProcessImages` calls
`getPixelAddress` once per row and walks `srcRow`/`dstRow` through the pixel loop
**[S]**. The pixel arithmetic is untouched (same four multiplies, same order), and
a host comparison of the old per-pixel form against the hoisted form over 1,464 **[M]**
pixel components produced bit-identical output. The speedup is still
**[U]**: it depends on whether the host's build inlines `getPixelAddress`, which
LTO changes.

### 2.5 Abort checks belong per row, not per pixel

`_effect.abort()` is a suite call, and the host sets it when the user cancels.
Checking it per pixel costs a host call per pixel; checking per row or per
band of rows responds within a few milliseconds at 4K. `debugging-and-symptoms.md` §1 has the
long-form rules on which actions may touch it.

### 2.6 One device, restored

A multi-GPU host can call you on a thread whose current device is 0 while the
frame lives on GPU 1. `cudaPointerGetAttributes` tells you which device owns the
pointer; the template's `DeviceGuard` restores the previous device on every exit
path **[S]**. Skipping the restore leaks your device choice into the host's
threads, and the symptom appears in *another* plugin's render.

### 2.7 The Support library logs once per render

`process()` calls `OFX::Log::print("processing via CUDA")` (or `... CPU`) on
every render **[S]**. `print` appends to `ofxPluginLog.txt`
(`OFX_PLUGIN_LOGFILE`), and only when the code was compiled with `DEBUG`, which
neither CMake's `Debug` configuration nor the templates define — so the call
always costs nothing and never grows the file unless you opt in. It does not
reach the host's UI; see `debugging-and-symptoms.md` §1 for the channel that does.

---

## 3. Kernel-level performance

### 3.1 One 16-byte load and one 16-byte store per pixel

The template ships two **RGBA32F** kernels with the same arithmetic: a vectorized
fast path and a scalar fallback for the buffers that cannot legally be vectorized
(a third kernel, 16F, has no launcher yet - §3.6). Both
call `ScalePixel`, so they cannot drift apart. Measured with the real toolchain
on this repo's `Kernel.cu` (`nvcc -arch=sm_89 -ptx`, nvcc 12.9.41) **[M]**:

| Kernel | Per-pixel global memory instructions | Registers / spills |
| --- | --- | --- |
| `GainKernelRGBA32FVec4` (shipped fast path) | **1 `ld.global.nc.v4.f32` + 1 `st.global.v4.f32`** | 14 registers, 0 spills, 0 stack |
| `GainKernelRGBA32FScalar` (alignment fallback) | 4 `ld.global.nc.f32` + 4 `st.global.f32` | 14 registers, 0 spills, 0 stack |

Same register count, same arithmetic, four times fewer memory instructions on
the fast path. The compiler cannot prove that a `const float*` base plus `4*x` is
16-byte aligned, so it will not fuse the four loads — which is why the vector
kernel states the alignment by indexing through `float4*`.

The fast path is selected only when it is legal. `mcofx::IsVec4Addressable`
(`Kernel.cuh`) requires both bases **and** both pitches to be 16-byte
addressable, because a `float4` access from a 4-byte-aligned buffer is a runtime
fault ("misaligned address"), not a compile error — and nothing in the OFX
contract promises more than natural alignment for the pixel format. Signed modulo
handles negative `rowBytes` (flipped images) correctly **[S]**. Decision table,
produced by calling the shipped predicate:

| Buffers | vec4 legal | Kernel the launcher picks |
| --- | --- | --- |
| aligned base, aligned pitch | yes | `GainKernelRGBA32FVec4` |
| src base + 4 bytes | no | `GainKernelRGBA32FScalar` |
| dst base + 8 bytes | no | `GainKernelRGBA32FScalar` |
| pitch 256 + 8 | no | `GainKernelRGBA32FScalar` |
| negative pitch, multiple of 16 | yes | `GainKernelRGBA32FVec4` |
| negative pitch, not a multiple of 16 | no | `GainKernelRGBA32FScalar` |
| pitch 0 (single row) | yes | `GainKernelRGBA32FVec4` |

The two paths were then run against each other on the host, executing the real
kernel source (thread mapping emulated): **3,240 grid launches, 0 mismatches** **[M]**
— **1,152** through the vec4 path and **2,304** through the scalar fallback, with
widths 0–63 spanning all four `width % 4` classes, heights 1/2/3/5, odd origins,
padded pitches and 4-byte-misaligned bases. The harness also verified that no
byte outside the render window is written **[M]**.

**The runtime delta, measured on hardware** (2026-11-22 field test, RTX 3060
sm_86, 1920×1080 RGBA32F, 100 iterations, `cudaEvent`, CUDA 13.0) **[B]**,
archived in [`field-evidence.md`](field-evidence.md) §4:

| Kernel | ms/frame | Effective bandwidth |
|---|---|---|
| `GainKernelRGBA32FVec4` | **0.212** | ~312 GB/s |
| `GainKernelRGBA32FScalar` | **0.222** | ~300 GB/s |

Four times fewer memory instructions bought about **4%** on a ~360 GB/s card:
both paths are bandwidth-bound, so the PTX count is a correctness property, not
a speed claim.

**Shape and pitch, measured here** (2026-11-26, same harness, RTX 3060 sm_86,
100 iterations, four sizes added: an odd width with and without a 64-byte pad, and
1×1 and 63×1) **[V]**,
[`../evidence/2026-11-26-windows/kernel-shapes.log`](../evidence/2026-11-26-windows/kernel-shapes.log):

| Frame | Pitch | ms/frame | Effective bandwidth |
|---|---|---|---|
| 1920×1080 | 30720 | 0.206 | 321 GB/s |
| 1919×1081 (odd width) | 30704 | 0.206 | 322 GB/s |
| 1919×1081, 64-byte pad | 30768 | 0.208 | 320 GB/s |
| 63×1 | 1008 | 0.009 | — |
| 1×1 | 16 | 0.009 | — |

An odd width costs **nothing measurable** - 0.206 ms against 0.206 ms, 322 against
321 GB/s - because the kernel is written over a pitch and a grid padded to 16×16
blocks, so 1919 is as wide as 1920 to it. The padding is the same story: 64 extra
bytes per row are 0.2 % more bytes and land 1 % away, inside run-to-run noise. The
degenerate sizes are the interesting ones: a 1×1 frame still costs **9 µs**, which
is launch and event overhead and nothing else, so a tiled host that hands over
small windows pays a fixed price per launch rather than a per-pixel one. Shape is
therefore not a performance variable in this kernel - **format** would be, and the
template has no 16F or 8-bit kernel to measure. The vector path still earns its place (no extra cost, fewer
instructions), but a plug-in should not be architected around it as the
performance lever — reduce traffic (region of interest, half precision,
fewer passes) before widening loads. This repository has not measured any of
those alternatives on hardware.

### 3.2 Block and grid

- Threads per block: a multiple of 32, 128–256 as the initial range **[D]**. The
  template uses 16×16 = 256, inside that range, and gives 2D locality for
  separable effects.
- Add `__launch_bounds__(256)` to every kernel. NVIDIA's guidance is explicit:
  it keeps at least one block resident on future hardware and prevents the
  "too many resources requested for launch" class of failure **[D]**. The
  template does not have it yet (**[S]**: absent from `Kernel.cu`).
- Round the grid up and boundary-check (the template does **[S]**). The cost is
  one predicate; the alternative is writing past the last row.
- Prefer computing `x`/`y` from `blockIdx`/`threadIdx` over a 1D division by
  `width`: integer division per thread is measurable at 4K. The template's 2D
  grid already avoids it.

### 3.3 Registers, spills, occupancy

Compile with `-Xptxas -v` to see registers and spills per kernel **[D]**. The
template's kernel is 14 registers, 0 spills, 0 stack **[M]** — there is nothing
to fix, and that is the number to protect when you add maths. `-Xptxas=-warn-spills`
(already in the template's CUDA flags **[S]**) turns silent local-memory traffic
into a warning at build time.

Do not chase 100% occupancy. NVIDIA's own guide says higher occupancy does not
always mean faster, and that beyond roughly two-thirds it rarely helps **[D]**.

### 3.4 One definition of the maths

The moment you have both `multiThreadProcessImages` and a kernel, you have two
implementations of the same formula. Every reference project that survives does
one of two things:

- puts the maths in a shared header of `__host__ __device__` inline functions
  used by both (OneGrade's `OneGradePipeline.h` approach **[R]**), or
- keeps them separate and enforces equality with tests (§5.3).

Pick one before the second effect. Adding the second backend is cheap; reconciling
four divergent copies of a grade is not.

### 3.5 Precision and fast maths

- `-use_fast_math` (or `--use_fast_math`) turns on `-ftz=true`, approximate
  division and `__sinf`-class intrinsics **[D]**. For a gain node it is
  irrelevant; for a grade or a keyer it changes output bits. If you use it, say
  so in the plugin description, because a colourist will eventually diff two
  renders and blame you.
- Denormals flush to zero with `-ftz=true` **[D]**. HDR plates contain
  denormals; a "difference only in the blacks" report is often this.
- Never use `double` in a device kernel for image maths you could do in `float`.
  On consumer GPUs FP64 runs at a small fraction of FP32 **[D]**.

### 3.6 Half precision: a kernel, an addressing question, and no timings

`GainKernelRGBA16FHalf4` exists in `Kernel.cu`: four `__half` per pixel, 8 bytes,
two `__half2` loads and two stores, arithmetic through `float` with one
round-to-nearest-even conversion per channel. That arithmetic is exact rather
than approximate - both operands are halves, so their product fits a `float`
without rounding, and the single conversion back is the same rounding `__hmul2`
applies - which is why the harness compares against a bit-identical reference
instead of a tolerance.

**No launcher calls it and nothing advertises RGBA16F.** A kernel alone promises
nothing to a host; declaring a half clip and handing device pointers into it is a
separate promise about the host's buffers, and it is not made here.

What the kernel is good for is the addressing question, which this machine can
answer without a device: a 16F pixel is 8 bytes, so the kernel needs 4-byte rows
where the float4 path needed 16, and `IsHalf2Addressable` encodes exactly that.
The harness drives the shipped kernel - compiled through `test/shim`, thread
mapping emulated - over the frame shapes an earlier hardware run measured, at the
pitch a 16F frame has **[V]**:

| Frame | Fixture pitch | Pixels | Result |
| --- | --- | --- | --- |
| 1920x1080 | 15376 | 2,073,600 | ok |
| 1919x1081, odd width | 15368 | 2,074,439 | ok |
| 1919x1081, 64-byte pad | 15432 | 2,074,439 | ok |
| 63x1 | 520 | 63 | ok |
| 1x1 | 24 | 1 | ok |
| 17x5, negative pitch | -152 | 85 | ok |

162 shapes in all - the six above plus widths 1-33 around the 16x16 block
boundary, heights 1/2/3/5 and three pad values - **0 halves wrong**, and no byte
written outside the render window. The fixture pitch is the allocation pitch
(bounds plus two columns, plus the pad), not the frame's row stride.

### 3.7 The 8-bit path: one byte per channel, and no alignment to argue about

`GainKernelRGBA8` is the same gain at 4 bytes per pixel. There is no
`IsByteAddressable` predicate because there is nothing to prove: every pointer
and every pitch a host can hand out is byte-addressable, so an odd pitch and an
odd pixel offset are legal - the opposite of the 16-byte float4 case, and the
reason a byte buffer cannot be told apart from a 32F one by its address. The
depth has to come from `kOfxImageEffectPropPixelDepth`, never from the pointer.

The harness drives it over the same measured frames at a byte pitch, plus widths
1-33, heights 1/2/3/5 and pitches that are deliberately not a multiple of 4
**[V]**:

| Frame | Fixture pitch | Pixels | Result |
| --- | --- | --- | --- |
| 1920x1080 | 7688 | 2,073,600 | ok |
| 1919x1081, odd width | 7684 | 2,074,439 | ok |
| 1919x1081, odd pitch | 7687 | 2,074,439 | ok |
| 63x1 | 260 | 63 | ok |
| 1x1 | 12 | 1 | ok |
| 17x5, negative pitch | -76 | 85 | ok |

164 cases in all - 162 shapes plus the identity ramp and the saturation case -
**0 bytes wrong**, with nothing written into the row padding. Two properties are
checked as properties, not as shapes: gain 1.0 returns the 0..255 ramp unchanged,
and 200/128/1/255 at gain 8 becomes 255/255/8/255, so overflow saturates rather
than wrapping.

The one thing that did not survive first contact: the negative-pitch case's
reference used `lround` while the kernel rounds ties to even. At gain 0.5 on odd
bytes every product is an exact .5 tie, and the two rules differ by one LSB on
half the pixels - 35 of 340 bytes were "wrong" with both sides correct under
their own rule. The kernel's rule is `__float2int_rn` and the reference now uses
`nearbyint`; the difference is recorded because a host comparing a byte render
against a float one will meet it.

**Throughput is [U].** Nothing here ran this kernel on a device. Half precision
on a GPU is a bandwidth question - 8 bytes per pixel against 16 for RGBA32F, so
half the traffic for the same frame **[A]** - and the only honest statement about
it is that it has not been measured. The 32F numbers in §3.1 are not this
kernel's.

---

## 4. Prove what you built (10 seconds, catches the worst class of bug)

Compiled CUDA and silent CPU fallback look identical in a host's UI. Verify the
artifact, not the intent:

```bash
O=build/bundle/MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx
nm -D --defined-only "$O" | grep ' T '   # exactly 2 registry symbols, no launcher leakage
nm "$O" | grep LaunchGainRGBA32F         # the launcher survived
readelf -S "$O" | grep nv_fatbin         # device code embedded
readelf -d "$O" | grep -E 'NEEDED|RUNPATH'  # libcudart.so.12 and $ORIGIN, no build path
```

On Windows, the equivalent enumeration from a CUDA toolkit install is
`cuobjdump --list-elf <bundle>.ofx`; expect `sm_*` cubins for your GPU and
nothing at all if CUDA was left out **[R]**. `does not contain device code` is
the failure message OneGrade documents **[R]**.

To test the PTX fallback path deliberately (newer GPU than your build
architectures), `CUDA_FORCE_PTX_JIT=1` forces JIT from embedded PTX **[D]**.
Module load policy is `CUDA_MODULE_LOADING=LAZY|EAGER` **[D]**; `EAGER` makes
first-frame hitches visible instead of moving them to first render.

---

## 5. Testing what you cannot see

### 5.1 Odd sizes

Render 1919×1081, 1×1, and a 2-pixel-wide window. 1920×1080 is a power of two and
hides every stride and boundary bug you have. This is the cheapest test that
exists and the one most often skipped.

### 5.2 Identity and bypass

`isIdentity()` returning the source clip is not an optimisation detail — it is
how the host knows it can skip your node. A plugin that never claims identity
forces a full render for every untouched frame. The template implements it **[S]**.

### 5.3 Cross-backend tolerance

Once you have a CPU path and a GPU path, compare them: render a synthetic
gradient, run both, assert max absolute difference below a threshold you chose
and wrote down. OneGrade reports *"worst deviation well under one 8-bit code
value"* for OpenCL against CPU **[R]**. The same test catches a wrong pitch, a
flipped row order, and a channel swap — none of which a compile can find.

**[U]** There is no such harness in this repo yet. It needs a GPU and a host.

### 5.4 Memory safety

`compute-sanitizer` (formerly `cuda-memcheck`) catches out-of-bounds device
access and leaks **[D]**. It is slow — run it on 256×256, not 4K.

---

## 6. OpenVINO against the CPU provider, measured here

A published comparison reports OpenVINO running a detection model 1.77 to 3.51
times faster than ONNX Runtime (**B**). Rule 3 says such a number is reproduced on
the machine in front of you or not written down, so here is the reproduction, and
it does not go the same way.

Method (`../evidence/2026-11-26-linux/openvino-bench.log`):
the committed `test_conv.onnx` from the inference template, a generated
1920x1080 float32 frame, the same thread count for both runtimes, three warm-up
runs discarded, ten measured, median and p90. One-off costs are reported
separately, because OpenVINO compiles the graph while ONNX Runtime builds a
session in milliseconds. The script that produced the log went with the other
cut on 2026-11-26; the log is what remains, so this can be re-read but not
re-run without rewriting it.

| Threads | ONNX Runtime 1.32.0 CPU | OpenVINO 2026.4.1 CPU | Ratio |
| --- | --- | --- | --- |
| 2 (all cores this container has) | **15.80 ms** median, p90 16.20 | **30.77 ms** median, p90 31.08 | **OpenVINO 1.95x slower** |
| 1 | **21.90 ms** median, p90 22.17 | **25.11 ms** median, p90 25.33 | **OpenVINO 1.15x slower** |

Machine: AMD Ryzen 7 3800X, Linux 7.0.0, **2 CPUs** visible to the process. The
one-off costs: ONNX Runtime session 2-3 ms, OpenVINO compile 380-465 ms.

What the table says, and what it does not:

- On this machine and this graph the ordering is the opposite of the published
  claim: ONNX Runtime is faster both with one thread and with two. The published
  number is not wrong - it is a different CPU, a different model and a different
  conversion path, and none of that is reproduced here.
- OpenVINO got **slower** when given the second core (25.1 to 30.8 ms) while
  ONNX Runtime got faster (21.9 to 15.8 ms). On two vCPUs its thread pool costs
  more than it buys; that is a property of the setting, not of the engine.
- This is the pessimistic case for OpenVINO by construction: its CPU plugin is
  tuned for Intel, and the machine is an AMD. A verdict about "OpenVINO on this
  class of hardware" would need an Intel box and a real detection network.
- Not measured: OpenVINO as an **execution provider inside ONNX Runtime**
  (`onnxruntime-openvino`), which is the shape a plug-in would actually ship,
  and which is a different question from "OpenVINO's own runtime".

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[A]** arithmetic derived from a formula in this document
- **[B]** external evidence: a run or a report from another machine or project
- **[D]** stated by official documentation
- **[M]** measured with the real toolchain, static/PTX or on the GPU
- **[R]** stated by a reference project
- **[S]** verified by reading the OFX source
- **[U]** not verified - treat as a hypothesis and measure it yourself
