# Best Practices and Performance

A review of this repository and of the reference projects that already ship
GPU-accelerated OFX plugins, reduced to rules you can act on. Every claim
carries an evidence grade, because the difference between "the spec says" and
"someone's blog says" is the difference between a bug you can fix and a bug you
cannot reproduce.

| Grade | Meaning |
| --- | --- |
| **[S]** | Verified by reading `OFX_Release_1.5.1` source (`include/`, `Support/`) while writing this |
| **[M]** | Measured here with the real toolchain (nvcc 12.9.41, GCC 13.3, CMake 3.31.6) — static/PTX level, no GPU execution |
| **[D]** | Stated by official documentation (OpenFX reference, NVIDIA CUDA C++ Best Practices Guide) |
| **[R]** | Stated by a reference project's own README/source |
| **[A]** | Arithmetic derived from a formula in this document |
| **[U]** | Not verified here — treat as a hypothesis and measure it yourself |

Nothing in this document was benchmarked: **there is no GPU in the environment
that produced it** (`/dev/nvidia*` absent, no `nvidia-smi`). Performance figures
you see elsewhere in the repo, and any number you get from your own machine, are
the only real ones.

---

## 1. What this pass changed, and what it verified

The review ran a mechanical check of the repo against the official sources.

| Check | Result |
| --- | --- |
| Every `kOfx*` identifier cited in the documents, sources and tests exists in OFX 1.5.1 | Checked mechanically by extracting every `kOfx*` token and resolving it against the 1.5.1 tree. Of the 2 that did not resolve, **1 was a real error, now fixed** — `ARCHITECTURE_AND_CUDA_PIPELINE.md` cited `kOfxImageEffectActionInstanceChanged`; the real action is `kOfxActionInstanceChanged` (`ofxCore.h:447`). The other, `kOfxImageEffectPropUnmappedComponents` in `SKILL.md`, is a deliberate negative example ("there is no such constant"), so it stays. |
| The GPU property table in `SKILL.md` §5 against the headers | 12/12 exist; three descriptions were wrong and are fixed (wrong header for `...OpenCLRenderSupported`, OpenGL declared `C string` not `string`, and the OpenCL Buffers/Images distinction now quoted from the header) |
| A committed GPU-free test suite (`templates/minimal-cuda-ofx/test/run-tests.sh`) | predicate table + 3,240-launch kernel comparison + CPU-loop comparison + 6-thread concurrency + PTX counts + memcheck, exit-code gated; clean under ASan+UBSan, TSan and valgrind **[M]** |
| Every CUDA runtime symbol the template calls (9) is declared in the toolkit headers | all present |
| The quoted stream-contract text exists verbatim in `ofxGPURender.h` | present at lines 608–617 |
| Property types and defaults used by the template | confirmed, see §3 |
| `ImageProcessor` dispatches the GPU path once per render, not per thread | confirmed, see §5.2 |
| The CPU path is already threaded by the Support library | confirmed, see §5.4 |
| Kernel memory instructions, after vectorizing | **1 `ld.global.nc.v4.f32` + 1 `st.global.v4.f32`** per pixel, 14 registers, 0 spills, 0 stack — measured, see §6.1 |
| Vec4 path versus scalar path, executing the real kernel source on the host | **3,240 grid launches, 0 mismatches**: widths 0–63 covering every `width % 4`, heights 1/2/3/5, origins (0,0)/(5,3)/(17,9), aligned and padded and misaligned pitches — see §6.1 **[M]** |
| CPU path: hoisted row pointers versus the previous per-pixel addressing | bit-identical over 1,464 pixel components — see §5.4 **[M]** |

---

## 2. What the reference projects teach

These are not summaries of feature lists; each row is a decision that another
team already paid for.

| Project | Practice | Why it matters | Adopted here? |
| --- | --- | --- | --- |
| **OneGrade** | *"The CPU header is the source of truth… any change to the math is a 4-file change (CPU + 3 kernels). Keep the helper names and formulas identical so they're easy to diff."* **[R]** | Four backends drifting apart is the normal failure mode of a multi-backend plugin, and it shows up as "CUDA looks different from CPU" only after a client notices. Naming parity makes the drift visible in a diff. | Partly: one kernel + one CPU loop, named to match, but no shared pipeline header yet. §6.4 says when to add one. |
| **OneGrade** | *"Confirm the GPU kernel actually shipped — a plugin built without it looks fine and just renders on the CPU"*, via `cuobjdump --list-elf …ofx`. **[R]** | A silently CPU-only build is indistinguishable from a working one in the UI. This is the single highest-value 10-second check in the whole discipline. | Yes: `build.sh` checks the launcher symbol and `.nv_fatbin`; §8 adds the `cuobjdump` form for the reader's own builds. |
| **OneGrade** | *"CUDA needs … a CUDA 13.x toolkit (12.x emits no Blackwell/sm_120 code)"*, and OpenCL validated against the CPU pipeline to *"worst deviation well under one 8-bit code value"*. **[R]** | Architecture flags and cross-backend tolerance are both testable facts, not opinions. | Partly: the template emits `100;120` when `CUDAToolkit_VERSION >= 12.8` **[S]**; cross-backend tolerance testing is described in §9.3, not implemented. |
| **OneGrade** | *"close Resolve first — the plugin is locked while it runs"* **[R]** | Installing over a loaded `.ofx` on Windows fails in a way that looks like a corrupt bundle. | Documented in `SKILL.md` §7. |
| **Open-DRT-OFX** | SDK *"cloned on demand (not vendored)"*, CMake ≥ 3.20, CI builds all three platforms on every tag. **[R]** | Vendoring the SDK freezes you on a stale spec and drags a licence into your tree; fetching keeps the headers truthful. | Yes: `FetchContent` on tag `OFX_Release_1.5.1`, shallow, or `OFX_SOURCE_DIR` for offline builds. **[S]** |
| **forge-colorx** | *"GPU where it's safe"* — Metal is gated to a host allow-list because *"Flame's macOS OFX host crashes on a Metal-advertising node"*; CUDA is opt-in at build time (`make WITH_CUDA=1`). **[R]** | Advertising a GPU capability is a promise the host acts on. Advertising one you cannot honour in a specific host is worse than not advertising it. | Philosophical yes (CUDA is advertised only when compiled in **[S]**); per-host gating is not implemented, and this is the strongest argument for keeping `HOST_COMPATIBILITY_MATRIX.md` in the loop. |
| **gyroflow-plugins** | The heavy work (gyro sync, lens profiles, optical flow) happens in the desktop app and is handed to the plugin as a project file; the plugin only renders. **[R]** | Anything expensive and per-clip belongs outside the render path. A render action is called per frame, sometimes per field, sometimes twice. | Yes in principle: the render action does no analysis, only one kernel launch. |
| **gyroflow-plugins** | Post-mortem of 32 Resolve crashes in 24 days, 31 of them the plugin releasing a host-owned `MTLCommandQueue`. **[R]** | Borrowed GPU objects belong to the host. Destroying one corrupts the host's queue and takes the process down later, somewhere else. | Yes: the stream is stored as an opaque `void*`, never created, never destroyed. **[S]** |
| **gyroflow-plugins** | Notes that applying the effect on the Edit/Color page *"should work faster"* than in Fusion. **[R]** | Host-side pipeline differences can dominate your kernel. When a user reports slowness, establish which page/composition they are on before optimising anything. | Recorded in `HOST_COMPATIBILITY_MATRIX.md`. |

**The pattern across all of them:** the winners are boring about ownership,
loud about what they actually built, and paranoid about which host they are in.
The losers are clever about kernels and silent about dispatch.

### 2.3 Counter-examples: where a big project is wrong

Being widely used is not a correctness argument. Reading the actual source of
**OneGrade** (`src/CudaKernel.cu`, GPL-3.0 — read for analysis, never copied)
shows three patterns this repository deliberately does **not** adopt:

| What that source does | Why it is a problem | What this repo does instead |
| --- | --- | --- |
| `cudaMalloc` + `cudaMemcpyAsync` for the parameter vector and the 3D LUT inside `RunCudaKernel`, i.e. **on every render** | `cudaMalloc` is a synchronising call: it serialises with all other device work, so the plugin becomes a pipeline barrier for every other GPU node at 24–60 fps **[D]** | No allocation in the render path at all — the gain travels as kernel arguments. If scratch memory is needed, cache it per instance and release it in the destructor / `purgeCaches()` (§5.1) |
| `cudaStreamSynchronize(stream)` right after the launch, then `cudaFree` (which also synchronises the device) | Synchronising on a **host-supplied** stream is precisely what `ofxGPURender.h` says not to do: the host orders its own work against that stream and expects the render action to return **[S]** | Sync only when `_pCudaStream == nullptr`, the one case where the contract requires completion before returning (§7) |
| The kernel indexes `((y * W) + x) * 4`, assuming tightly packed rows | Correct only while `rowBytes == width * 16`; any host padding shears the image, and it silently ignores a negative pitch **[S]** | Address through `kOfxImagePropRowBytes` in bytes, widened to `int64_t` and signed (§6 of `ARCHITECTURE_AND_CUDA_PIPELINE.md`) |

This does not make OneGrade a bad plugin — it is a colour tool whose pixel work
dominates its overhead, and its authors validated it in a host. The lesson is
narrower and more useful: **the spec text is the authority, not the popularity of
the code you are reading.** When a reference implementation and `ofxGPURender.h`
disagree, the header wins — and you should be able to name the line that settled
it.

---

## 3. Verified quirks in the OFX 1.5.1 headers

Found by reading the headers during this pass. Each one has bitten somebody.

1. **`kOfxImageEffectPropCudaEnabled` is an `int X 1`, not a string.** **[S]**
   (`ofxGPURender.h`). Its neighbours — `...CudaRenderSupported` and
   `...CudaStreamSupported` — are `string X 1`. So in raw C:
   `propGetInt(host, inArgs, kOfxImageEffectPropCudaEnabled, &v)`, while the
   `...Supported` pair is `propGetString`. Mixing them up returns
   `kOfxStatErrBadHandle` or reads garbage.

2. **The three "can we?" properties have three different names.** **[S]**
   - CUDA: `kOfxImageEffectPropCudaRenderSupported`
   - Metal: `kOfxImageEffectPropMetalRenderSupported`
   - OpenCL: `kOfxImageEffectPropOpenCLSupported` ← no `Render`
   Guessing by analogy produces a property the host has never heard of, and a
   plugin that silently never gets the GPU.

3. **Some property names and their string values disagree.** **[S]**
   - `kOfxImageEffectPropSupportsMultipleClipDepths` → `"OfxImageEffectPropMultipleClipDepths"` (no `Supports`)
   - `kOfxActionOpenGLContextDetached` → `"kOfxActionOpenGLContextDetached"` (value starts with `k`)
   If you ever compare against the literal string instead of the macro, you will
   be wrong. Always use the macro.

4. **`kOfxActionInstanceChanged` has no `ImageEffect` in it**, unlike its
   siblings `kOfxImageEffectActionGetRegionOfDefinition` etc. **[S]**
   `kOfxImageEffectActionInstanceChanged` does not exist. (This repo cited it
   incorrectly until this pass.)

5. **The GPU status codes are not in the core headers.** **[S]**
   `kOfxStatGPUOutOfMemory` (1001) and `kOfxStatGPURenderFailed` (1002) live in
   `ofxGPURender.h`, which `ofxsImageEffect.h` does not include. Include it
   explicitly or the compile fails on a plugin that looked fine in the examples.

---

## 4. The cost model: what actually costs time

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

## 5. Host-side practices

### 5.1 Never allocate per frame if the size did not change

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

### 5.2 Your GPU path is called once per render — do not thread it yourself

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

### 5.3 `supportsTiles` decides how big a window you get

With `setSupportsTiles(false)` **[S]** in the template, the host should hand you
a whole-image render window. With tiles *on*, the same effect is called many
times with small windows, and your per-launch overhead becomes the dominant cost.
Turn tiles on only when an effect genuinely cannot be computed for a window
without the rest of the image — and then budget for the launch count.

Also note: `process()` silently returns when the render window is not inside the
destination bounds **[S]**. A plugin that "does nothing" is often being handed a
window it rejected; log it once before assuming the GPU failed.

### 5.4 Do not add threads to the CPU path

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

### 5.5 Abort checks belong per row, not per pixel

`_effect.abort()` is a suite call, and the host sets it when the user cancels.
Checking it per pixel costs a host call per pixel; checking per row or per
band of rows responds within a few milliseconds at 4K. `SKILL.md` §7 has the
long-form rules on which actions may touch it.

### 5.6 One device, restored

A multi-GPU host can call you on a thread whose current device is 0 while the
frame lives on GPU 1. `cudaPointerGetAttributes` tells you which device owns the
pointer; the template's `DeviceGuard` restores the previous device on every exit
path **[S]**. Skipping the restore leaks your device choice into the host's
threads, and the symptom appears in *another* plugin's render.

### 5.7 The Support library logs once per render

`process()` calls `OFX::Log::print("processing via CUDA")` (or `... CPU`) on
every render **[S]**. `print` appends to `ofxPluginLog.txt`
(`OFX_PLUGIN_LOGFILE`), and only when Support was built with `DEBUG` — so in
Release it costs a call that does nothing, and in Debug it grows a file by one
line per render. Neither reaches the host's UI; see `SKILL.md` §7 for the channel
that does.

---

## 6. Kernel-level performance

### 6.1 One 16-byte load and one 16-byte store per pixel

The template ships two kernels with the same arithmetic: a vectorized fast path
and a scalar fallback for the buffers that cannot legally be vectorized. Both
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
archived in [`FIELD_TEST_WINDOWS_CUDA.md`](FIELD_TEST_WINDOWS_CUDA.md) §4:

| Kernel | ms/frame | Effective bandwidth |
|---|---|---|
| `GainKernelRGBA32FVec4` | **0.212** | ~312 GB/s |
| `GainKernelRGBA32FScalar` | **0.222** | ~300 GB/s |

Four times fewer memory instructions bought about **4%** on a ~360 GB/s card:
both paths are bandwidth-bound, so the PTX count is a correctness property, not
a speed claim. The vector path still earns its place (no extra cost, fewer
instructions), but a plug-in should not be architected around it as the
performance lever — reduce traffic (region of interest, half precision,
fewer passes) before widening loads. This repository has not measured any of
those alternatives on hardware.

### 6.2 Block and grid

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

### 6.3 Registers, spills, occupancy

Compile with `-Xptxas -v` to see registers and spills per kernel **[D]**. The
template's kernel is 14 registers, 0 spills, 0 stack **[M]** — there is nothing
to fix, and that is the number to protect when you add maths. `-Xptxas=-warn-spills`
(already in the template's CUDA flags **[S]**) turns silent local-memory traffic
into a warning at build time.

Do not chase 100% occupancy. NVIDIA's own guide says higher occupancy does not
always mean faster, and that beyond roughly two-thirds it rarely helps **[D]**.

### 6.4 One definition of the maths

The moment you have both `multiThreadProcessImages` and a kernel, you have two
implementations of the same formula. Every reference project that survives does
one of two things:

- puts the maths in a shared header of `__host__ __device__` inline functions
  used by both (OneGrade's `OneGradePipeline.h` approach **[R]**), or
- keeps them separate and enforces equality with tests (§9.3).

Pick one before the second effect. Adding the second backend is cheap; reconciling
four divergent copies of a grade is not.

### 6.5 Precision and fast maths

- `-use_fast_math` (or `--use_fast_math`) turns on `-ftz=true`, approximate
  division and `__sinf`-class intrinsics **[D]**. For a gain node it is
  irrelevant; for a grade or a keyer it changes output bits. If you use it, say
  so in the plugin description, because a colourist will eventually diff two
  renders and blame you.
- Denormals flush to zero with `-ftz=true` **[D]**. HDR plates contain
  denormals; a "difference only in the blacks" report is often this.
- Never use `double` in a device kernel for image maths you could do in `float`.
  On consumer GPUs FP64 runs at a small fraction of FP32 **[D]**.

---

## 7. Streams, and what changes when the host gives you one

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

## 8. Prove what you built (10 seconds, catches the worst class of bug)

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

## 9. Testing what you cannot see

### 9.1 Odd sizes

Render 1919×1081, 1×1, and a 2-pixel-wide window. 1920×1080 is a power of two and
hides every stride and boundary bug you have. This is the cheapest test that
exists and the one most often skipped.

### 9.2 Identity and bypass

`isIdentity()` returning the source clip is not an optimisation detail — it is
how the host knows it can skip your node. A plugin that never claims identity
forces a full render for every untouched frame. The template implements it **[S]**.

### 9.3 Cross-backend tolerance

Once you have a CPU path and a GPU path, compare them: render a synthetic
gradient, run both, assert max absolute difference below a threshold you chose
and wrote down. OneGrade reports *"worst deviation well under one 8-bit code
value"* for OpenCL against CPU **[R]**. The same test catches a wrong pitch, a
flipped row order, and a channel swap — none of which a compile can find.

**[U]** There is no such harness in this repo yet. It needs a GPU and a host.

### 9.4 Memory safety

`compute-sanitizer` (formerly `cuda-memcheck`) catches out-of-bounds device
access and leaks **[D]**. It is slow — run it on 256×256, not 4K.

---

## 10. Anti-patterns, by symptom

| Symptom | Usual cause | Fix |
| --- | --- | --- |
| Black frame, no error | GPU advertised but not compiled in; or `kOfxImageEffectPropCudaEnabled` read as a string | Advertise only under `#ifdef`; use `propGetInt` **[S]** |
| Plugin vanished from the menu | Host blacklisted it after a crash | Clear the host's plugin cache (`SKILL.md` §7) |
| Renders on CPU, "CUDA is on" | Device code not in the binary | §8, `cuobjdump`/`nm` |
| Image sheared on odd widths | `width * sizeof(pixel)` instead of the host pitch | Use `rowBytes`, widen to `int64_t` **[S]** |
| Flipped vertically | Negative `rowBytes` treated as unsigned | Signed pitch, arithmetic through `char*` **[S]** |
| First frame hitches, then fine | Module load + PTX JIT on first launch | `CUDA_MODULE_LOADING=EAGER`, ship SASS for the target **[D]** |
| Other nodes stall when yours renders | `cudaMalloc`/`cudaFree`/`cudaDeviceSynchronize` inside render | Cache allocations; never sync when a stream was given **[D]** |
| Host crash minutes later, elsewhere | Plugin destroyed a borrowed GPU object | Never own the stream/queue **[R]** |
| Different result on GPU vs CPU | Two divergent copies of the maths | §6.4, shared definition or tolerance test |
| Slow only in Fusion | Different host pipeline, not your kernel | Establish the page first **[R]** |

---

## 11. Checklist for a new GPU effect

1. Maths in one place, or a test that proves two copies agree.
2. One launch per render; no per-frame allocation; no sync when a stream exists.
3. Pitch in bytes, `int64_t`, signed, window origin applied by hand.
4. `float4` (or `uint4`) accesses where the format allows; `__launch_bounds__` set.
5. Both `CXX_VISIBILITY_PRESET` and `CUDA_VISIBILITY_PRESET` hidden; 2 exports.
6. Artifact verified by `nm`/`readelf`/`cuobjdump`, not by trust.
7. Tested at 1919×1081, 1×1, identity, and against the CPU path.
8. Read back with `compute-sanitizer` on a small frame.
9. `.ofx` installed with the host closed; plugin cache cleared after a crash.

---

## 12. What is still unverified

- **No runtime measurement of anything.** No GPU exists here. The 4×
  instruction reduction is PTX-level **[M]**; the speedup is a hypothesis, and
  no number is given for it.
- **The equivalence proof stopped at the host.** The two kernels and the two CPU
  loop forms were shown to agree byte-for-byte by executing the real kernel
  source on the CPU (§6.1), which is not the same as running SASS on a GPU. The
  same checks are committed as `templates/minimal-cuda-ofx/test/run-tests.sh` so
  they can be re-run after any change.
- **No host has ever loaded these plugins.** Every host-behaviour claim in this
  document is graded **[R]** or **[S]**, never **[M]**.
- **Resolve's scanline padding** (256-byte alignment, the 208×117 thumbnail case
  in `Kernel.cu`'s comment) is a community observation carried in the template's
  source, not something measured against Resolve here. It is safe to depend on
  the *general* rule (pitch is `rowBytes`, not `width × 16`) and unwise to depend
  on the specific number.
- `cudaPointerGetAttributes` cost per launch is unmeasured here. It is cheap on
  UVA systems; if a profile ever shows it, cache the device per allocation.
