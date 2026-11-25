# ONNX Runtime and TensorRT inside a plug-in

The exact ORT API names, tensor plumbing bugs, device-pointer handoff, and the inference template walkthrough.

<!-- AI_MODEL_INTEGRATION.md lines 160-328 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 496-560 -->
<!-- AI_MODEL_INTEGRATION.md lines 401-853 -->
## 1. ONNX Runtime: the facts that matter, with exact names

Verified against the 1.32.0 headers (`ORT_API_VERSION 32`) and the ORT
documentation (**A** unless marked).

**Session lifetime.** Create one session and keep it. Session creation parses
the graph, resolves providers and allocates arenas. Nothing in the docs promises
it is cheap; every project that ships inference keeps sessions alive across
frames (**B**).

**Thread budget — the trap that bites every OFX plug-in.** `intra_op_num_threads`
defaults to `0`, which means "one thread per physical core" **per session**, with
affinity; setting an explicit value disables that affinity. `inter_op_num_threads`
only matters with `ORT_PARALLEL` execution mode (default is `ORT_SEQUENTIAL`)
(**A**, ORT threading docs). Hosts call `render()` on several threads; the ORT
default therefore multiplies. The template sets intra-op to 1 and says why. If
you are sure you render single-threaded, raise it and measure.

**Concurrency.** "Multiple threads can invoke the `Run()` method on the same
inference session object … kernels are stateless" (**A**, ORT high-level design).
Exceptions exist: DirectML documents one-thread-at-a-time, and ORT serialises
internally when an EP reports no concurrent support (**A**). Nothing documents
CUDA EP behaviour when a shared `user_compute_stream` is set — treat that as
unverified.

**Zero-copy device tensors.** `Ort::IoBinding` + `Ort::Value::CreateTensor` on
memory described by `Ort::MemoryInfo("Cuda", OrtArenaAllocator, device_id,
OrtMemTypeDefault)` wraps an existing device buffer without copying;
`BindOutputToDevice` / `BindOutput(name, memoryInfo)` lets ORT allocate the
output on device instead of downloading it (**A**). Without a binding, ORT copies
inputs to device and outputs back to host inside `Run()`.

**The host's stream.** The provider option key is exactly `"user_compute_stream"`,
set through `UpdateCUDAProviderOptionsWithValue` (string values go through
`UpdateCUDAProviderOptions`); it "implicitly sets the `has_user_compute_stream`
option", cannot be combined with an external allocator, and implicitly enables
`use_ep_level_unified_stream` (**A**). Because ORT otherwise synchronises the EP
at the end of `Run()`, the non-blocking recipe is:

```cpp
Ort::RunOptions run;
run.AddConfigEntry("disable_synchronize_execution_providers", "1");
session.Run(run, io_binding);   // caller now owns ordering on the host stream
```

`kOrtRunOptionsConfigDisableSynchronizeExecutionProviders` is the named constant;
the docs warn it "should only be used with device local memory or an ORT Value
allocated in pinned memory, otherwise the issued download will be blocking"
(**A**). This is the same asymmetry as `kOfxImageEffectPropCudaStream`: with a
host stream you return immediately, without one you must finish your own work.

**Packaging.** The pip wheel ships `libonnxruntime.so.1.x.y` and
`libonnxruntime_providers_shared.so` but **no headers** (measured here: ORT
1.32.0 wheel, headers absent). Headers come from the release tarball, or from
the same tag's `include/onnxruntime/core/session/` tree. The tarball's CMake
package references `lib64` while the tarball ships `lib`, so
`find_package(onnxruntime)` fails on the official layout — link
`-I<root>/include -L<root>/lib -lonnxruntime` yourself, which is what the
template does. CPU and GPU are separate tarballs
(`onnxruntime-linux-x64-gpu_cuda12-*.tgz`); a GPU build pins a cuDNN major
(mixing 8 and 9 is not supported) (**A**). If the release tarball is
unreachable, `pip download` + unzip supplies the shared library and the headers
come from the same tag's `include/` tree — the two halves have to be the same
version.

**Windows, and the NuGet route.** The release zip (`onnxruntime-win-x64-<ver>.zip`)
has the layout the template expects: `include\` and `bin\onnxruntime.dll`. It
also carries `lib\onnxruntime.lib`, which the template does not link on Windows
(see the next paragraph). When the GitHub release assets are not reachable, the
NuGet package carries the same files under a different layout (**B**, field-tested
with `Microsoft.ML.OnnxRuntime` 1.20.1 and 1.32.0 - the layout did not change):

```text
https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/<version>   (.nupkg = zip)
  build/native/include/*            -> <root>/include
  runtimes/win-x64/native/*.lib     -> <root>/lib
  runtimes/win-x64/native/*.dll     -> <root>/bin
```

The split matters: `lib\onnxruntime.lib` is only the import library. Renaming
it to `onnxruntime.dll` produces a bundle that looks complete and that the
loader rejects at load time, which is why the template resolves the DLL
separately and fails the configure when it is absent (**B**).

**On Windows, do not link the runtime at all, and do not ship it as
`onnxruntime.dll`.** Windows resolves an imported DLL by module *base name* and
searches the executable's directory first, so a host that carries ONNX Runtime
never opens yours: Resolve 21 ships `onnxruntime.dll` 1.13 beside `Resolve.exe`,
a plug-in built against 1.20 asks `GetApi(20)`, receives null, and the first ORT
call is an access violation in `render` with nothing naming the plug-in (**B**,
from Resolve's minidump). `%WINDIR%\System32\onnxruntime.dll` (Windows ML) wins
the same way, so "it works on my machine" proves nothing about the host.

The template therefore compiles with `ORT_API_MANUAL_INIT`, does **not** link
`onnxruntime.lib`, copies the DLL into the bundle as `onnxruntime-ofx.dll`, and
loads that private name from the module directory at first use
(`LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`),
then `GetProcAddress("OrtGetApiBase")`, `GetApi(ORT_API_VERSION)` and
`Ort::InitApi(api)`. A missing or too-old runtime becomes an exception from
`ModelRunner` - a render error with a message - instead of a crash. The private
name is mandatory: `LoadLibrary` matches an already-loaded module by base name,
so even the full path to a file called `onnxruntime.dll` returns the host's copy.
Never `FreeLibrary` it; an `Ort::Env` created afterwards outlives the call.

**The C++ header is only half header-only, and only one half is dangerous.**
Both are *declared* in `onnxruntime_cxx_api.h`, but their bodies are in
`onnxruntime_cxx_inline.h`: `Ort::GetVersionString()` calls `OrtGetApiBase()`
directly, so calling it pulls the import-library symbol into the module —
exactly the link-time import the load-by-path design exists to avoid — and the
link *succeeds*, which is why nothing warns (**B**, and the plug-in that did it
crashed in its constructor; the two bodies are in the 1.20.1 headers **[V]**).
`Ort::GetAvailableProviders()` is the other shape: it calls
`GetApi().GetAvailableProviders(...)`, i.e. the table `Ort::InitApi()` installed,
so it is safe *after* that call. Go through the table you loaded either way —
`GetAvailableProviders` on that `OrtApi`, and `OrtGetApiBase` resolved with
`GetProcAddress` on your own handle for the version — and read the inline header
of your own ORT version rather than this paragraph: the two functions have
already differed once.
`dumpbin /dependents` on the finished `.ofx` is the check: no `onnxruntime.dll`
may appear. The names that call returns are the runtime's strings — copy them and
do **not** free them through `Ort::AllocatorWithDefaultOptions` (**B**: the first
`Free()` was an access violation), and ask once per process, because the answer
cannot change while that DLL is loaded.

Never put a runtime-owned object in a function-local static either. An
`Ort::Env env` there has its destructor run at `DLL_PROCESS_DETACH`, after the
host unloaded the plug-in and while ORT's thread pool is winding down — a
segfault *after* `kOfxActionUnload` returned `kOfxStatOK`, and a flaky one
because it depends on shutdown timing (measured elsewhere: the same binary
exited 0 once and 139 nine times **[B]**). Leak one environment per process
instead (`static Ort::Env* env = new Ort::Env(...)`); sessions and thread-pool
objects follow the same rule. A function-local static is only safe for objects
whose destructor does not call into the runtime.

**macOS.** The runtime is `libonnxruntime.<version>.dylib` and dyld records an
install name of `@rpath/libonnxruntime.<version>.dylib`, so the bundle needs an
`@loader_path` rpath entry and the file has to keep that versioned name.
Renaming it to the Linux SONAME, or recording `$ORIGIN` (an ELF-only token dyld
treats as a literal directory), produces a bundle that builds cleanly and cannot
load; `templates/onnx-inference-ofx/CMakeLists.txt` has the per-platform branch.

---

## 2. Tensor plumbing: where the silent bugs are

These are the same three failures as the CUDA template, plus three that only
appear with models.

| Failure | Symptom | Fix | Settled by |
|---|---|---|---|
| Address from `width * 4 * sizeof(float)` instead of `kOfxImagePropRowBytes` | shears on padded or bottom-up images; correct in the host you tested | pack through the byte stride, signed | `ofxImageEffect.h:1347-1351` (**A**) |
| Treat `kOfxImagePropData` as the top row | a labels-and-boxes overlay comes out mirrored while the pass-through copy still matches, so the output looks like a plate or font bug | `data` addresses `bounds.y1` (the bottom); rebase onto the top row once (`data + (h-1)*rowBytes`, negated stride). The pointer's origin is a separate axis from the stride's sign | field evidence (**B**); `ofxImageEffect.h:1353` (**A**) |
| Alpha overwritten by the model's 3 channels | matte destroyed, or opaque edges appear | pass alpha through unchanged | the model's shape contract (**A** in the template) |
| Tiles without halo | visible grid seams; each tile's padding contaminates its border | halo ≥ receptive-field radius, feather-blend the overlap | measured here: halo=0 deviates by **0.263** on a 3×3 conv where halo=1 is exact (**A**) |
| Wrong channel order | plausible but wrong colours (R/B swap) | know your model: ONNX is usually RGB, OpenCV-trained models are often BGR | model documentation (**B**) |
| Wrong value range | crushed or blown result: models trained on `[0,1]` fed `[0,4]`, or on `[-1,1]` fed `[0,1]` | normalise/denormalise explicitly and record it next to the model | model documentation (**B**) |
| Colour space mismatch | contrast/tint shift that looks like a grade error | convert with OCIO as the LTX plug-in does, or document the expected space | that project's approach (**B**) |
| Batch/`NCHW` vs `NHWC` | shape error, or a 100× slowdown from a transposition per frame | fix the layout at export, keep one layout end to end | ONNX tensor semantics (**A**) |

Measured in `templates/onnx-inference-ofx` on this machine (CPU, ORT 1.32.0,
3-channel 3×3 convolution, 64×48): whole-frame C++ output is **bit-identical**
to the Python ONNX Runtime golden (`max |diff| = 0`), tiled output with
`halo=1` is **bit-identical** to whole-frame for tile sizes 5/13/17/256 and for
1×1, 7×5, 64×3, 3×48 images, and the `halo=0` negative control differs by
**0.263** — which is the point: tiling without context is wrong, and a test that
only compares tiled-to-tiled would never notice.

---

## 3. AI pipelines: TensorRT and ONNX Runtime on OFX device pointers

The pattern for an AI-based OFX plugin (upscale, denoise, matte) is identical in
shape to the CUDA kernel path, with three additions: a model, a persistent
inference session, and a binding layer that must not copy.

### Where the pointers go

Both runtimes accept device pointers directly, which is what makes zero-copy
possible **[B]** — their published APIs, not a measurement here:

| Runtime | API | What you pass |
| --- | --- | --- |
| TensorRT | `IExecutionContext::setTensorAddress(name, ptr)` then `enqueueV3(stream)` | The OFX `kOfxImagePropData` pointer, cast to `void*` |
| ONNX Runtime | `Ort::IoBinding::BindInput/BindOutput` with `Ort::MemoryInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault)` | Same pointer |
| ONNX Runtime | `session.RunWithBinding(runOptions, ioBinding)` | — |
| ONNX Runtime | CUDA EP `user_compute_stream` provider option | The OFX `cudaStream_t`, so ORT enqueues on your stream |

**Do not** bind host memory and let the runtime copy. That is three transfers per
frame and it negates the reason for a GPU plugin.

### Shape and layout mismatch

Models want NCHW planar float32. OFX gives you interleaved RGBA. Something has to
convert, and it should be a kernel:

```
RGBA32F (interleaved)  --kernel-->  NCHW planar (+ optional resize/normalise)
                                     |
                                     v
                              inference (same stream)
                                     |
                                     v
NCHW planar            --kernel-->  RGBA32F (interleaved, cropped to RoD)
```

Both conversion kernels enqueue on the host stream, so the whole chain is ordered
without a single host-side synchronise.

### Lifecycle and the traps

1. **Load lazily.** `createInstance` may run before a CUDA context exists.
   Build the engine/session on the first `render()` (or in
   `beginSequenceRender`), keyed by `(device, input shape)`.
2. **Never rebuild per frame.** Engine deserialisation is tens to hundreds of
   milliseconds; per-frame it is unusable.
3. **Budget the VRAM.** A 4K RGBA32F frame is 66 MB; a planar NCHW float buffer
   for a 3-channel 4K tensor is 199 MB. Cache buffers per shape, free on shape
   change.
4. **Handle "no tensor cores" gracefully.** Query
   `cudaDeviceGetAttribute(cudaDevAttrComputeCapabilityMajor/Minor)` and select a
   precision (FP32/FP16/INT8) you actually support on that GPU.
5. **Free device resources before the host tears down.** Prefer
   `endSequenceRender()`; keep the destructor allocation-free. Destroying a
   TensorRT execution context during host shutdown is a known way to crash on
   exit, and an exit crash looks like your plugin to the user.
6. **Never let the runtime create its own stream and assume ordering.** If ORT/TensorRT
   will not accept yours, enqueue on your own stream and synchronise **that**
   stream before returning when the host gave you none. See §2.
7. **Ship the model as a bundle resource.** `NAME.ofx.bundle/Contents/Resources/`
   is the documented place for non-code assets; resolve it from the plugin's own
   path, never from an absolute path on the build machine.

---

## 4. The template: `templates/onnx-inference-ofx`

A complete, building OFX plug-in that runs an ONNX model, with the parts that
are usually untested under test. The session this template creates is a CPU
session, because no GPU provider was available when it was written; for a plug-in
that ships, the CUDA provider is the first path ("The provider belongs in the
same bundle" below) — that is a change to this template, not a second one.

| Piece | File | What it is |
|---|---|---|
| Session wrapper | `src/ModelRunner.{h,cpp}` | one ORT session behind a pimpl, intra-op threads explicit, errors as exceptions, `Run()` documented thread-safe |
| Tensor plumbing | `src/AiCore.{h,cpp}` | `PackRGB`/`UnpackRGB` through a signed stride, tile planning, feathered accumulation, whole and tiled inference |
| OFX glue | `src/Plugin.{h,cpp}` | window/origin addressing, alpha pass-through, lazy session load under a mutex, `InstanceSafe`, no CUDA advertised |
| Fixtures | `models/` | a 611-byte 3×3 conv plus a Python-generated golden tensor (36 KB + 36 KB), committed so tests need no Python |
| Generator | `tools/make-test-model.py` | rebuilds model and golden (`onnx` + `onnxruntime`) |
| Tests | `test/run-tests.sh`, `test/ai_pipeline_test.cpp` | 5 stages, exit-code gated, `SANITIZE=` support |

```bash
export ONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-1.32.0
./build.sh                      # configures, builds, asserts exports and bundle
./test/run-tests.sh             # packing, golden, tiling, negative control, bundle
SANITIZE=address,undefined ./test/run-tests.sh
```

What this template proves, on this machine: see §2's measurements, plus the
bundle exports exactly two symbols, records `NEEDED libonnxruntime.so.1` and
`RUNPATH $ORIGIN` on Linux (`@loader_path` on macOS, where the runtime keeps its
versioned `.dylib` name), ships one copy of the runtime, `dlopen` enumerates one
plugin (`com.ofxplugindeveloper.onnxinference` v1.0), and the session asks for
the CUDA provider before it is created as a CPU session
(`ModelRunner::AppendCudaProvider`). The outcome is visible: the plug-in writes
it into the `provider` label parameter, and the suite asserts the value, which
reads

```
CPU (1 thread) - fallback: Failed to load library
libonnxruntime_providers_shared.so with error: libonnxruntime_providers_shared.so:
cannot open shared object file ...
```

here — the request was made, it failed, the frame still rendered, and the label
says why (**V**).

What it does **not** prove: no host has rendered through it, no CUDA session has
ever been created, and no timing is claimed. The provider *request* is compiled
and its failure path is tested; the success path is not. No provider binary ships
in this repository, so on a GPU machine the remaining work is to
confirm the label flips to `CUDA (device 0, 1 thread, …)` and names the stack, measure the run with
`tools/bench_ep.py` (one row per provider, plus the providers the session really
ended up with, so a silent fallback is visible next to the number), and repeat
the bundle check with the provider DLLs in place — the recipe below is documented from the ORT API, and every number in it
is external (**B**).

### Adapting it to a real model

1. Export with one input and one output, `NCHW` float32, dynamic H/W, batch 1.
2. Set the halo to the model's receptive-field radius. For a stack of `n` 3×3
   convolutions with stride 1 that is `n` pixels; for a U-Net it is larger than
   you think — compute it, do not guess.
3. Match the value range and channel order the training code used, and write
   both down next to the model file.
4. If the model is not translation-invariant (normalisation over the whole
   frame, attention across the image), tiling changes the result — run the whole
   frame, or accept that tiles are an approximation and say so in the UI.
5. A model that changes resolution needs code, not just a different file.
   `ModelRunner::Run` requires `output shape == input shape` and copies the
   result into a buffer of the input's size; an upscaler (the usual reason to
   reach for a model here) therefore throws on the first frame, by design.
   Supporting one means returning the output shape from `Run()`, mapping it into
   the output RoD in `AiCore`, and scaling the source RoD in
   `getRegionOfDefinition` — see `roi-tiling-and-render-hints.md` §1 for the
   RoD rules and §2 of this document for the packing side.
6. A model whose output is not an image — detection (`[1, 4+N, anchors]`),
   classification (`[1, classes]`), segmentation masks, embeddings — does not fit
   this template's plumbing at all. `ModelRunner` requires
   `output shape == input shape`, and `AiCore` unpacks RGB into a frame. Keep the
   template for its session lifetime, thread budget, bundling and packing, and
   write the head decoder yourself; nothing in the runner or the unpacker
   applies.

#### What the host hands you is not float RGBA

The template accepts `eBitDepthFloat` only and passes three channels straight
through. A plug-in a colourist actually uses takes what the host gives it.

- **Depth.** `eBitDepthUByte` and `eBitDepthUShort` arrive as integer
  components: normalise by the depth's own maximum (255 / 65535, **A**) before
  the model and quantise back with rounding rather than truncation, or every pass
  drifts the frame darker by up to one code value.
- **Premultiplication.** A model trained on straight colour must not be fed
  premultiplied pixels: `RGB·A` darkens every soft edge and the network treats
  the matte as image content, so a denoiser learns to preserve the fringe it was
  supposed to remove. Read `kOfxImageEffectPropPreMultiplication`, divide by
  alpha before packing and multiply back after unpacking, and leave pixels with
  `A == 0` at zero. `colour-and-alpha.md` §1 states the rule per
  operation; this is the inference instance of it.
- **Keep the conversion OFX-free.** Depth and alpha conversion belongs beside
  `Pack*`/`Unpack*`, on plain buffers addressed through the host's signed row
  bytes (the pitch trap in §2), which is what lets a test round-trip every depth,
  padded rows and a negative stride without a host.

#### Models that only accept certain sizes

Graphs that reshape to `H/8`, `H/16` or `H/32` (SwinIR, DAT, OmniSR and most
transformer exports) throw `The input tensor cannot be reshaped to the requested
shape` at any size that is not a multiple — measured elsewhere, four of eight
candidate models failed that way, and one ran at 64×64 only (**B**). Decide it
at load, not per frame:

- **Find the alignment by probing.** One inference each at 63/72/80/96 tells you
  the factor: 63 runs → 1, 72 → 8, 80 → 16, 96 → 32. Four runs at load, not in
  the render path, and the difference between a dropdown entry and a trap.
  `tools/model_probe.py` runs exactly that ladder and prints the factor it
  implies, next to the shapes, the channel count, whether H/W are dynamic at all,
  and the output/input size ratio — so an upscaler is recognised before anyone
  writes a runner for it.
- **Pad right and bottom** to that multiple with edge replication, run, and crop
  the result back to the real size. Zero padding is content to a convolution, so
  a black border bleeds inward; replication does not.
- **A fixed input size** means scaling every tile to the export resolution and
  back. That costs quality — say so in the UI instead of letting the user find
  out from a soft frame.

`tools/make-testmodels.py` writes fixtures for exactly the paths a real model
hides: a 1/3/4-channel identity and a 2× upscaler, as small real graphs. Use them
to test channel packing, alpha pass-through and factor handling before a
downloaded model enters the loop; they say nothing about quality.

#### The halo is measured, not guessed

Item 2 above gives the principle. For a real model nobody prints the receptive
radius, so measure it — five minutes, no host. `tools/measure-halo.sh` is that
measurement as a command: it walks the ladder through the plug-in's own tiling
code (`AiCore::RunTiled`, not a reimplementation), drops a border band at the
frame edge, refuses to report a number when the frame has no interior tile edge
to test, and writes `<model>.halo.json` beside the model — byte count, SHA-256,
frame, tile, border, threshold and the deviation of every rung. `--check` then
fails when the model no longer matches that file, so a replaced model cannot
inherit a stale halo. The five rules it implements:

1. Render the whole frame and the tiled frame through the **same** function; a
   benchmark that reimplements the tiling proves nothing about the plug-in.
2. Walk a ladder (32/48/64/96/128) and take the first overlap whose maximum
   deviation from the whole frame stays under ~0.02.
3. Require at least one real tile edge in the comparison, and **exclude the frame
   border**: a tile clipped at the image edge sees content no interior tile ever
   sees, and the largest deviation lands there — reported from a shipping project
   as 0.38 on the right border of a model that is pixel-identical at interior
   edges, which would have bought an overlap three times too large (**B**).
4. Keep `2·overlap < frame height` in the ladder, otherwise the largest
   candidate overlaps the whole frame and passes trivially.
5. Overlap is a property of the model, not of the resolution: reported values
   held unchanged from a small frame to 1080p (**B**), so measure once per model
   and store the number (a file beside the model, keyed by name **and** file
   size, is enough; a replaced file must not inherit a stale value).

Recorded, not remembered: the repository's own fixture carries its measurement
in `templates/onnx-inference-ofx/models/test_conv.onnx.halo.json`. Nothing in the
template *reads* that file at run time — `halo` is a parameter the user sets, and
the JSON is what tells you which value to pass and what `--check` compares a
re-measurement against. A plug-in that loaded it during `render` would be reading
a build artefact per frame; keep the reader in the build, not in the effect. On a 256×192
frame with 128-pixel tiles, `halo=0` deviates by 0.1745 at the interior tile
boundary (128,127) and `halo=1` is exact — the same story as the `halo=0`
control in §2, from a second witness, and the reason the ladder may include 0:
a measurement that cannot produce the failure cannot be trusted to recognise
success.

The price, so "more context is safer" is not free: a tile of `T` with overlap
`H` makes the model see `(T + 2H)²` pixels instead of `T²`. At `T = 512`,
`H = 64` is +56 % and `H = 32` is +27 % — halving the overlap saves 19 % of
model time (**A**: arithmetic from the formula).

#### The provider belongs in the same bundle, and it is the first path

`ai-architectures-and-runtimes.md` §4 answers *how much* CUDA is worth. For a per-frame inference plug-in the order
is fixed — request the CUDA provider first, keep a CPU session as the fallback,
make the choice visible — and the consequence is packaging, not a second build:

- **One bundle, request the provider, fall back silently.** Append the provider
  inside a `try`; every failure — no device, no driver, missing cuDNN DLL, not
  enough VRAM — ends on a CPU session instead of an exception the host turns into
  a red frame. This template does exactly that, through the key/value API rather
  than the struct, because two of the five options it needs do not exist as
  struct fields (§5): `ModelRunner::AppendCudaProvider()` states `device_id`,
  `use_tf32=0`, `cudnn_conv_algo_search=HEURISTIC`,
  `cudnn_conv_use_max_workspace=0` and `do_copy_in_default_stream=1`, catches the
  failure and rebuilds the options for the CPU session, and `Plugin.cpp` writes
  provider plus reason into the `provider` label parameter. Two lazily built sessions are simpler than a
  retry ladder.
- **Make it visible and assert it.** Nothing else tells anyone the provider
  stopped loading: the picture stays correct and only the time changes —
  measured elsewhere at 28× on a 4x model and 11× on a denoiser (**B**). Write
  provider plus reason into a label parameter (`CUDA (RTX 3060)` versus
  `CPU (provider load failed)`) and add a test stage that fails when the expected
  provider is missing. Write that parameter **outside** the mutex that guards the
  session: a host may call back into the instance from `setValueAtTime`, so a
  label written while the render lock is held deadlocks the first render (**B**,
  `std::system_error: resource deadlock would occur`). Compose the text under the
  lock, release it, then write.
- **A read-only field is a `StringParam`, not a push button or a log line.**
  `kOfxParamTypeString` with `eStringTypeLabel` and `setEvaluateOnChange(false)`:
  hosts draw it as text, the value is derivative (never saved, never evaluated),
  and rewriting it does not invalidate a render. It is also the only form the
  test harness can read back, which is what makes "which provider ran" a check
  rather than a log nobody sees (**B** for the host rendering; the testkit side is
  **[V]**, `--label-param`).
- **Do not preload the provider DLL.** `LoadLibrary` on
  `onnxruntime_providers_cuda.dll` fails with error **1114** because its
  `DllMain` needs the mailbox that only the ORT core sets; ORT finds the provider
  beside the core by itself. Preload only what the provider imports — cuDNN,
  cuBLAS/cuBLASLt, cuFFT, cudart — or the load fails with **126** (**B**).
  `cufftw*` is **not** in that list, even though it shares the cuFFT prefix: the
  provider never imports cuFFTW, and it is the file whose 127 cost a whole GPU
  path (below).
- **cuDNN 9 on Windows is not one file, and the preload has to be a guard as
  well as a loop.** The provider imports the dispatcher (`cudnn64_9.dll`); the
  dispatcher loads its components (`cudnn_graph64_9.dll`, `cudnn_ops64_9.dll`,
  `cudnn_heuristic64_9.dll`, …) by base name at run time, and a plug-in directory
  is not on the default search path. A bundle that copied only the dispatcher did
  not fall back to the CPU: the process **exited 127** inside the provider
  (**B**). The template copies `cudnn*64_*.dll` and, on Windows, preloads by
  absolute path what the provider imports — but only where the host does not
  already own that base name:
  * **cuFFTW is skipped.** The provider never imports it, and it imports the exact
    cuFFT build it shipped with: next to a host's newer `cufft64_11.dll` the load
    fails 127 (a broken *import*, not a missing file — that is 126). An
    all-or-nothing preload turned that unused file into `cannot load
    cufftw64_11.dll (Windows error 127)` on the provider label while the whole GPU
    path stayed down (**B**).
  * **A group the host has loaded is left alone** — cuDNN, cuBLAS/cuBLASLt,
    cuFFT, cudart — and the provider label says `host CUDA stack` instead of
    `bundled CUDA`. The test has to compare the loaded module's *path*: an
    explicit load by absolute path succeeds and leaves a **second** module of the
    same base name, and an *import* is resolved by base name against whichever
    copy is loaded under it. Measured in Resolve 21.2: two `cudnn64_9.dll`
    (host 9.8.0.87, bundle 9.1.0.70) in one process, and the 9.1 components
    failing 127 against the 9.8 dispatcher (**B**). "A module with that name
    exists" is the wrong test — after your own preload it is true for your copy.
  * **One unneeded file must not end the GPU attempt.** Keep loading the rest, let
    the session decide, and name the file that failed in the label if the provider
    then fails. The template records skipped and failed names and appends them to
    the fallback reason.

  This class of bug is invisible to a fake host: it has nothing loaded, so the
  bundle's own DLLs are used and every check is green. The reproduction is
  `tools/host_cuda_probe.cpp` — one MSVC file, no dependencies, written and run
  against Resolve 21.2 by the reporter and **not compiled in this repository**
  (**[B]** for its output, **[I]** here): it loads the host's folder, then the
  bundle's by absolute path, then prints which file the process resolves for each
  base name. `FAILED (error 127)` on a component means the bundle cannot be
  preloaded next to that host and the guard is mandatory. The preload in the
  template is **[V]** since 2026-11-25: on a Windows 11 machine it found no host
  copies and the label reads `bundled CUDA`
  ([`../evidence/2026-11-25-windows/onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log)).
- **Budget the size.** The provider imports `cufft64_*` even for a model with no
  FFT in it; with cuDNN 9 + cuBLAS 12 + cuFFT a bundle was 2.4–2.7 GB, of which
  the provider alone was 643 MB (**B**). That is a product decision — one
  download for everyone, no separate GPU build.
- **The copy is a switch, not a script.** `templates/onnx-inference-ofx` ships
  `-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` (off by default) plus
  `-DOFXPLUGIN_CUDA_RUNTIME_DIRS=<dir>`: it copies the provider, the shared
  provider library and cuDNN/cuBLAS/cuBLASLt/cuFFT/cudart beside the `.ofx`,
  names every missing file as a `CMake Warning` with the load error it causes
  (126), prints the total size at configure time, and never fails a configure
  for a machine without a CUDA runtime. `-DOFXPLUGIN_CUDA_FROM_HOST=ON` is the
  other packaging: only the provider and `onnxruntime_providers_shared` travel,
  so its imports resolve to the host's cuDNN 9 / cuBLAS 12 / cuFFT 11 / cudart 12
  and a second copy of a base name cannot exist. Measured on the reference
  machine: 15 → 3 DLLs in `Contents/Win64`, 2.8 GB → 1.1 GB, and the same contract
  result (78 checks, 0 failed) with the host's folder prepended to `PATH` (**B**).
  It needs a CUDA host, so it is a deployment choice rather than a better
  default; the guard above is what makes one full bundle correct on both kinds of
  host. The option works on Windows too, and what
  decides whether it can is the provider's **import table**, not the platform:
  the Windows bundle ships the core as `onnxruntime-ofx.dll`, because that private
  name is what keeps a host's own runtime (Resolve 21: 1.13) from capturing the
  import (§1), so a provider that imports `onnxruntime.dll` by base name would
  bind to nothing (or to the host's runtime). For ORT **1.20.1** the CUDA provider
  does not: `dumpbin /dependents onnxruntime_providers_cuda.dll` lists cublas,
  cublasLt, cufft, cudart, cuDNN and `onnxruntime_providers_shared` only, and a
  bundle with all 30 files (2.8 GB) ran on a RTX 3060 and passed the contract test
  (**B**). **1.32.0** passes the same check and needs fewer files: its provider
  imports `cublas64_13`, `cublasLt64_13`, `dxgi` and
  `onnxruntime_providers_shared`, and loads cuDNN, cuFFT and cudart at run time
  instead of at load time, so 16 files (1.54 GB) carry it. That provider is also
  built as an EP plugin - it exports `CreateEpFactories` - yet a provider request
  still reaches it through the provider bridge: the legacy struct call did, and
  so does the V2 key/value call the template uses now that it states its options
  explicitly (§5). Both were run here. That is a property of one binary, not a documented contract, so the
  check belongs in the build notes of a Windows GPU bundle: run `dumpbin
  /dependents` on the provider you ship and look for `onnxruntime.dll`. If a later
  version imports it, the copy is a bundle that fails at load and the working
  shape is a sidecar process that owns ORT and the provider (§2, §3). macOS gets no copy for a simpler reason — CUDA does not
  exist there — so the bundle ships the core and the CPU path, and a Mac user who
  needs the GPU has two routes this template does not implement: ONNX Runtime's
  CoreML EP, with the partition-fragmentation caveat in §1, or a sidecar running
  MLX/Metal as `CorridorKey-Runtime` does (§1); either way the `provider` label
  must name which one ran.
  Verified on Linux with a directory of stand-in files: seven files copied into
  `Contents/<ARCH>/`, `CUDA provider bundle: 7 file(s), 7 MB` at configure time
  (**V**); the same configure without them warns seven times and still succeeds
  (**V**). No provider has ever been loaded.
- **`cudnn_conv_algo_search = heuristic`.** The ORT default benchmarks
  convolution algorithms at session creation and was reported to cost minutes at
  1080p before the first tile (**B**).
- **Tile size is VRAM-bound, not model-bound.** The same 4x transformer held
  7.25 GB at a 384² tile and crossed 11.8 GB at 640², at which point the driver
  fell into its slower path: halving the tile beats enlarging it (**B**).
  `tools/sweep-vram.sh` is that measurement as a command: it ramps the core tile
  until ONNX Runtime reports an allocation failure, grows every tile by the halo
  it reads from `ONNXLoader.profiles`, and stops at the first failure with that
  failure's first line — a size that fails for another reason is named as such,
  so a reshape limit is never mistaken for a memory ceiling. It needs the GPU;
  without one it exits 3 instead of printing an empty table. On Windows the
  ceiling frequently cannot be provoked at all: one handover ramped to 39 GB of
  process RSS without an allocation failure, because WDDM pages video memory into
  system memory instead of refusing (**B**). A window that never fails is "no
  ceiling found", not "fits", and the number that matters on such a machine is
  measured frame time at the sizes you ship.

#### One thread budget per node, and a label that proves it

ONNX Runtime's default is one intra-op thread **per physical core, per session**.
An inference plug-in is not one session: a detector plus a text tower for the
promptable models, and a host holds as many instances as the timeline has nodes.
Each of those taking a full core count with affinity is how a plug-in that
benchmarks well makes a timeline stutter. Five rules keep it one budget:

- **One helper builds every session.** The template's `MakeSessionOptions(threads)`
  sets the intra-op count, `inter_op = 1`, `ORT_SEQUENTIAL` and `ORT_ENABLE_ALL`,
  and the CPU fallback gets the same helper. A second session built with its own
  copy of those options is how two sessions drift apart.
- **That drift is not hypothetical.** In a reference plug-in the detector took the
  `intraThreads` parameter while the text tower kept a hard-coded `min(CPU/2, 4)`:
  a node at 8 ran its tower on 4 and gave no sign of it. Passing the value is half
  the fix; the other half is remembering the count each session was created with,
  or the cache that avoids rebuilding the tower per frame keeps the old number.
- **Make it a parameter whose default is what the plug-in already did**, so a
  saved project renders exactly as before, and treat a change as "re-create the
  session once", not once per frame. The template takes `intraThreads` in its
  constructor and defaults to 1, because a host already calls `render()` on
  several threads.
- **Print the count in the status label, in both modes** — `CPU (8 threads)`,
  `CUDA (device 0: …, 6 threads)`; the template writes `CPU (1 thread)` and
  `CUDA (device 0, 1 thread, host CUDA stack, tf32 off, all nodes on CUDA)`. That
  line is what makes a value
  like `--set intraThreads=3` checkable from the contract harness
  (`--label-param`): without it the parameter can be wired to nothing and no test
  notices.
- **Do not present it as a GPU speed-up.** Measured on one RTX 3060, medians of
  12 steady-state samples, 4/6/8 threads: YOLO26m at 640×480 gave 38/34/38 ms and
  YOLO26n at 1920×1080 gave 23/25/24 ms, with 22–54 ms spread inside a single
  setting — no effect, i.e. smaller than the noise. The CPU path is where it
  matters: 297/269/233 ms and 42/42/39 ms (9 samples) (**B**). A control that
  changes only one path belongs in that path's group, not beside "Use CUDA".

Honest limit: those numbers are single-instance. The multi-instance argument is
why a cap exists at all, and it is not something the reporter measured (**B**).

The request itself is compiled and its failure path is verified here: against the
CPU package of ONNX Runtime the provider request throws `Failed to load library
libonnxruntime_providers_shared.so`, and the `provider` label carries that reason
(**V**). The success branch has run since
2026-11-25: the GPU build created the session with the provider and printed
`CUDA (device 0, 1 thread, bundled CUDA)` (**[V]**,
[`../evidence/2026-11-25-windows/onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log)).
That log predates the fallback probe, so it proves the provider was created and
nothing more; the same line now reads `CUDA (device 0, 1 thread, bundled CUDA,
tf32 off, all nodes on CUDA)` - arithmetic and node coverage stated (§5, §6),
which are stronger claims than that log could make. The figures above stay external evidence from one RTX 3060
(**[B]**); the success path is now both documented from the ORT API and observed
once.

#### Detection heads: the four facts that cost a day

- **Letterbox, and keep the mapping.** YOLO-style models are trained on a square
  input: scale by `min(size/w, size/h)`, pad to the square with 114/255 (or 255,
  per the export), and store `scale` and `(padX, padY)`. Every box comes back in
  that letterboxed space; inverse-map it before drawing.
- **Read the output layout from its shape, not from a blog post.** Ultralytics
  exports `[1, 4 + classes, anchors]`; some exports are transposed to
  `[1, anchors, 4 + classes]`. Compare the two non-batch dimensions, take the
  smaller as the attribute axis, and sanity-check that the objectness/class
  column lies in `[0, 1]` — the wrong choice yields plausible numbers, so a range
  check is cheaper than a wrong overlay.
- **Decode, threshold, then NMS per class.** `cx, cy, w, h` are in input pixels;
  convert to `x1, y1, x2, y2`, drop boxes below the score threshold, run IoU NMS
  per class rather than across classes, and only then inverse-map and draw.
- **"End-to-end" export is a different contract.** When NMS lives inside the
  graph the output is a small `[1, N, 6]` table with no anchors; check the
  model's metadata before writing a decoder for a shape it does not produce.
- **Read the class names from the model, not from a table in your source.** An
  Ultralytics export writes `custom_metadata_map["names"]` as a Python dict
  literal (`{0: 'person', 1: 'bus'}`); read it when the session is created and
  keep a side-car file only as the override. A hardcoded COCO list mislabels
  every custom model past the prefix it shares with COCO — measured elsewhere: a
  35-word YOLO-World export answered class 29 with COCO's 30th name **[B]**. Be
  as tolerant as the writer is sloppy (single or double quotes, quoted or bare
  numeric keys, a plain list) and reject sparse or negative indices instead of
  attaching a name to the wrong row.
- **A promptable export can bake the class count at trace time.** In Ultralytics'
  `YOLOEDetect.forward_head` the score tensor is built with
  `int(x[-1].shape[1])`, a constant once traced, so a graph exported with three
  classes only runs with three: `Non concat axis dimensions must match: Axis 2
  has mismatched dimensions of 25200 and 8400`. Patch the reshape to use
  `nc = x[-1].shape[1]` and call `forward_head(...)` + `Detect._inference(...)`
  directly, because the `super()`-based forwards let the segmentation subclass
  in the MRO append its 32 mask coefficients **[B]**. This is invisible until a
  second prompt with a different class count is typed.
- **CLIP's `</w>` marker is part of the token.** It is appended to a word's last
  symbol *before* the BPE merges, so the vocabulary holds both `person</w>`
  (2533) and `person` (3510). A reimplementation that skips it produces valid
  ids that are one token off for every single-word prompt, and nothing reports an
  error; an embedding comparison will not catch it either (the int8 text tower
  stays near 0.96 cosine). Compare ids against reference tokenizer output
  instead — the harness in the reporting tree does exactly that for twelve
  prompts **[B]**.

#### Drawing into the frame: the overlay after the model

What the model finds is drawn on top of the frame that was just rendered, and
the pass-through copy that makes a filter correct is orientation-agnostic while
the drawing is not: an overlay ships mirrored or on the wrong side of its box and
every copy check stays green (**B** — labels beside their boxes and mirrored text
in a host). The four rules that matter:

- **Write through the destination's pitch, never `width * 16`.** The output image
  has its own base pointer, its own signed `kOfxImagePropRowBytes` and its own
  bounds (**A**: `ofxImageEffect.h:1340-1353`), and its pitch need not equal the
  source's. One row is `dstBase + (y - window.y1) * dstPitch`.
- **Offset by the window, once.** A box computed in image coordinates becomes
  `x - window.x1`, `y - window.y1`; `kOfxImagePropBounds` is what you clip
  against, not what you index with. A tile or proxy window is where an overlay
  that "works on the full frame" draws into its neighbour's image.
- **An opaque plate on a premultiplied image is `colour * a`.** The clip's
  `kOfxImageEffectPropPreMultiplication` is normally `kOfxImagePreMultiplied`
  (**A**: `ofxImageEffect.h:978-992`), so storing the straight colour makes every
  soft edge glow - the opposite of the filter's own rule, which is to copy alpha
  through untouched.
- **Rasterise from one stated origin.** Glyph quads and plate rows are computed in
  a top-down canvas; if `kOfxImagePropData` addresses `bounds.y1` (the bottom), a
  rasteriser that assumes top-down draws the whole page upside down and still
  changes pixels. Rebase once (base at `(h - 1) * rowBytes`, negated stride; §2's
  row) and keep every overlay coordinate in that one space.

Test it as a bound, not as a pixel value: "ink stays inside the plate", "nothing
past the right edge", "the box outline is in the top half of the tile". A centred
probe hides a mirror, and "bytes changed" hides everything else
([`testing-layers.md`](testing-layers.md) §5, L3). The checking side is in
[`testkit/README.md`](../testkit/README.md): `--image-origin top|bottom` and
`--negative-pitch` set the two axes independently, the two top-down layouts are
negative controls, and a pass-through copy satisfies all four while a mark drawn
at a chosen position does not. `testkit/run-negative-controls.sh` runs that as a
control: its `-DDRAW_MARK` stub writes one 8×8 mark through the destination's
bounds and pitch, `--mark 24,8` reads it back in frame order, and the build
passes in both bottom-up layouts and fails `the mark is at the mirrored row` in
both top-down ones (**V**). A 94-check `tile_window_test` that draws per layout
also exists in the reporting tree, not in this repository **[B]**.

Cited back from where the question arises: [`../SKILL.md`](../SKILL.md) §2 (writing
back is the same pitch arithmetic against the destination) and
[`../SKILL.md`](../SKILL.md) (a tiled plug-in that draws has a seam bug and a
placement bug, and they are different), and
[`reference-projects.md`](reference-projects.md) §3.3 and §3.6, which
teach the addressing rules and the tile protocol this section extends. The full rules — clipping to window and
bounds, premultiplied compositing, and `stb_easy_font` for glyphs without a GUI —
are [`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) §4.

---

## 5. What the device computes: TF32, cuDNN's search, the arena

The CUDA execution provider is not precision-neutral by default, and the header
is the place to see what the template can and cannot switch off. The legacy
`OrtCUDAProviderOptions` struct carries `device_id`, `cudnn_conv_algo_search`
(default `EXHAUSTIVE`), `gpu_mem_limit` (default `SIZE_MAX`),
`arena_extend_strategy` (default `0`, `kNextPowerOfTwo`),
`do_copy_in_default_stream` (default `1`), the user-compute-stream pair, an
`OrtArenaCfg*` and the tunable-op fields - and **no TF32 field at all**, in the
1.20.1 headers and in the 1.32.0 headers alike (**[V]**: `onnxruntime_c_api.h`,
`OrtCUDAProviderOptions`). TF32 travels only as a key/value provider option
(`use_tf32`), so a plug-in that appends the provider through the struct cannot
turn it off: it runs with the documented default, which is **1** since 1.18 -
tensor cores truncate the mantissa to 10 bits for the convolutions and matmuls
they accelerate (**[B]**, the CUDA EP option table).

Four settings decide whether a frame is reproducible on the device:

| Setting | Default | What it costs | Where it lives |
|---|---|---|---|
| `use_tf32` | `1` | `4.5e-4` from the CPU session instead of `5.4e-7` on a 1080p conv - measured, §5 below | key/value option only |
| `cudnn_conv_algo_search` | `EXHAUSTIVE` | benchmarks every algorithm against the real tensors before the first tile; the result is cached on disk, so the cost is paid once per machine and plan | struct field or key/value - the template sets `HEURISTIC` |
| `gpu_mem_limit`, `arena_extend_strategy` | `SIZE_MAX`, `kNextPowerOfTwo` | the arena never returns memory and doubles as it grows, so an instance left open holds the peak of every shape it has seen | struct field or `OrtArenaCfg` |
| `cudnn_conv_use_max_workspace` | `1` | permission for the plan to reserve up to a second copy of the activation budget; what it actually reserves depends on the plan cuDNN picks | key/value option only |

**Before that was written down, the defaults were in force, and the GPU evidence
in this repository still shows it.** Both CUDA logs were produced by a
`ModelRunner` that filled three fields of the struct and inherited the rest. The
label line in them reads `CUDA (device 0, 1 thread, bundled CUDA)` - no
`tf32 off`, because nothing had set it
([`../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log)).
Those runs are still valid for what they claim - the provider loaded, the bundle
resolves, the numbers were close - but they cannot testify about arithmetic they
never pinned down, and a tolerance bisected on such a run measures the session's
defaults as much as the plug-in. That is why the clause is in the label: a log
without it says which run it was.

The template therefore appends the provider through the key/value API and states
five keys explicitly - `device_id`, `use_tf32=0`, `cudnn_conv_algo_search=HEURISTIC`,
`cudnn_conv_use_max_workspace=0`, `do_copy_in_default_stream=1` - instead of
filling in a struct and inheriting the rest (see §4).

**Why the C API and not the C++ wrapper.** `Ort::CUDAProviderOptions` - the
wrapper that takes a `std::unordered_map` and returns a serialised string - does
not exist in ONNX Runtime 1.20.1; it arrived in a later release. The first
version of this change used it and compiled cleanly against 1.32.0, then stopped
on 1.20.1 with `error C2065: "cuda": undeclared identifier` in the same function
- which is what a CPU-only run against the older runtime is for. The template now
uses `CreateCUDAProviderOptions`, `UpdateCUDAProviderOptions` and
`SessionOptionsAppendExecutionProvider_CUDA_V2`, stable since 1.11. The price is
the C API's shape: two arrays that must stay in the same order, so the key list
carries the comment that explains the value beside it. What that buys is not a
belief; each part was measured on one RTX 3060, on a 64-channel 3×3 convolution
at 1920×1080, against the same graph on the CPU session (**[V]**,
[`../evidence/2026-11-25-windows/onnx-cuda-ep-options.log`](../evidence/2026-11-25-windows/onnx-cuda-ep-options.log)):

| Provider options | max abs difference from the CPU session |
|---|---|
| `use_tf32=1` (the EP default) | **4.472e-04** |
| `use_tf32=0` | **5.364e-07** |

The two numbers are the whole argument. Three orders of magnitude, and the TF32
one sits forty times above the `1e-5` tolerance the suite retries a GPU run at -
so a session left on the default would fail this template's golden comparison
for a reason that has nothing to do with the plug-in, and would do it on some
kernels and not others: the suite's own 64×48 model never triggered TF32 - its
deviation from the CPU golden stayed at `1e-7` with the default on
([`../evidence/2026-11-25-windows/onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log)) -
which is how a label that says CUDA and numbers that are correct can coexist. The same graph at 640×480 gives `3.95e-4` and `4.2e-7`.

The provider's own option dump - `GetCUDAProviderOptionsAsString()`, printed per
row in the evidence log - is what makes this checkable rather than assumed: it
reads `use_tf32=0;cudnn_conv_use_max_workspace=0;cudnn_conv_algo_search=HEURISTIC`
for the template's settings, so the keys demonstrably reached the provider and
were not dropped on the way.

**`cudnn_conv_use_max_workspace=0` bought nothing measurable, and that is the
honest result.** On both graphs the device memory after a run was the same with
and without it - 3122 MB versus 3124 MB at 1080p, 536 MB at 640×480 - because
the flag is a *permission*, not an allocation: it says the plan may not assume a
large scratch buffer, and the plan cuDNN picks under `HEURISTIC` for these shapes
fits the small one either way. What the setting buys is a bound on the worst
case, not a saving on this model; a model whose chosen plan wants the large
workspace is where the difference would appear, and no such model was measured
here.

**`HEURISTIC` versus `EXHAUSTIVE` could not be separated on this machine either:**
both created the session in 2.8-3.2 ms, because cuDNN keeps its chosen plans in
an on-disk cache (`ComputeCache`), so the benchmark that `EXHAUSTIVE` runs is paid
on the first session ever, not on every one. The "minutes at 1080p" figure stays
external evidence (**[B]**, §7). In a host the first session is the one a user
waits for, which is argument enough to keep it off - but it is an argument from
the cached-plan behaviour, not from a stopwatch reading here.

### Determinism: what the `1e-7` measures, and what it does not

Three defaults decide whether two runs of the same graph produce the same
numbers, and none of them is in the graph:

1. **`use_tf32` is on.** NVIDIA's option table says TF32 *"is enabled by
   default"* and that *"starting from ONNX Runtime 1.18, you can use this flag
   to disable it"* (**[B]**) - so the flag is the 1.18 addition, not the
   behaviour: 1.17 and earlier ran every accelerated convolution and matmul with
   a 10-bit mantissa and no way to say otherwise.
2. **`cudnn_conv_algo_search` defaults to `EXHAUSTIVE`**, which benchmarks
   `cudnnFindConvolutionForwardAlgorithmEx` against the real tensors and picks
   the fastest (**[B]**, same table). Fastest is a measurement of this machine
   with this driver; another machine, another cuDNN build or a colder on-disk
   `ComputeCache` can select another algorithm, and an algorithm is an
   accumulation order.
3. **`cudnn_conv_use_max_workspace` is `1`**, so the plan may assume a large
   scratch buffer - which changes which algorithms are candidates at all.

**The `5.364e-07` in the table above is what this model does with all three
pinned, and `4.472e-04` is what it does with one of them left alone.** So "the
inference template's model stays at 1e-7" is true in two measured situations and
false in a third: true with `use_tf32=0` (5.364e-07 at 1080p, 4.2e-7 at 640x480),
true for the suite's 64x48 model even with TF32 on - at that shape the provider
never reached a tensor-core path, which is how a `CUDA` label and a `1e-7`
comparison coexisted in the first GPU log - and false for the 1080p
64-channel 3x3 convolution under the default, where it is `4.472e-04`.

That is the point of a determinism section: **the 1e-7 is a property of the
shape, not of the provider.** A larger model does not inherit it. Three reasons,
in increasing strength: TF32 truncation applies wherever a tensor-core path is
chosen, so the error floor moves to `1e-4` (**[V]**, measured); a longer
reduction chain over more elements accumulates more rounding (**[I]**, arithmetic,
not a measurement here); and an `EXHAUSTIVE` search that is free to pick a
different plan on a different machine turns "same result" into "same result on
this machine" (**[I]**, from the documented behaviour of the search, with no
second-GPU run to confirm it).

What was **not** measured, and belongs next to any claim built on this: the same
session run twice and diffed - so run-to-run repeatability is asserted nowhere
in this repository; a second GPU or a second driver version; and any model larger
than 64 channels at 1920x1080. A golden comparison for a bigger model should
budget `1e-4`-scale deviation, or pin `use_tf32=0` and accept the slower
convolutions - those are the two honest options, not a tighter tolerance.

**Should the template set these explicitly? It does, and it should** - five keys
through the V2 API (§4), because a struct cannot express `use_tf32` at all.
One correction to the question as usually put: `cudnn_conv_algo_search` is set
to **`HEURISTIC`**, not to its `EXHAUSTIVE` default. The reason is the first
session, not the steady state: `EXHAUSTIVE` benchmarks every candidate against
the real tensors, and in a host the first session is what a user waits for.
Here both settings created the session in 2.8-3.2 ms because cuDNN had already
cached its plans, so the argument rests on the uncached first run (**[B]**) and
not on a stopwatch reading in this repository. `HEURISTIC` is also the more
repeatable of the two - a function of shape and filter rather than a timing race
(**[I]**). What explicit options buy is a session whose arithmetic is stated;
what they do not buy is bit-identical frames across machines, which neither
cuDNN nor this repository has ever demonstrated.

The arena is also the host-facing half of this. A plug-in that keeps a session
per instance, never sets `gpu_mem_limit` and lets the arena grow on the
power-of-two curve holds roughly twice the peak it needs, once per instance
(**[I]** - the curve is documented, the multiplier is arithmetic).
`arena_extend_strategy=1` (`kSameAsRequested`) and a stated `gpu_mem_limit` are
the two knobs; §3's device-pointer path changes where the bytes come from, not
how much the arena keeps.

## 6. "CUDA" in the label does not mean the GPU ran every node

A label that says only `CUDA (device 0, 1 thread, bundled CUDA)` is evidence that
the provider was created and the session accepted it. It is not evidence that
every node ran on the device. A node the provider does not
implement is assigned to the CPU EP without a message, the session builds, the
label stays, and the frame is still correct - slower, with the GPU idle for that
node (**[I]**: the fallback is the documented default; what it costs is a
per-model measurement nobody has made here).

The question has an exact answer, and the runtime ships the switch for it:
`session.disable_cpu_ep_fallback=1` makes **session creation fail** when the
non-CPU providers cannot cover every node, and it is invalid to combine it with an
explicitly added CPU EP (**[V]**: `onnxruntime_session_options_config_keys.h`,
`kOrtSessionOptionsDisableCPUEPFallback`). The shape that fits a plug-in is a
throwaway probe: create the session once with the flag, then create the real
session without it - a model with one unsupported node is still better off on the
device for the rest.

**Built here, and the answer is in the label.** `ModelRunner` builds that
throwaway session before the real one - same options, plus the key, plus the CUDA
EP and nothing else - and destroys it before the real session exists, so the
resident weights never double. The answer goes into the `provider` label, because
that is the line a log reader actually sees:

| Label | What it establishes |
|---|---|
| `CUDA (device 0, 1 thread, bundled CUDA, tf32 off, all nodes on CUDA)` | the probe loaded the graph with the key, so every node is on the device, and the arithmetic is the one §5 measured |
| `CUDA (…, nodes left on CPU)` | the probe failed; the runtime's reason follows in the same label after `- fallback:` |
| `CPU (1 thread) - fallback: …` | no provider at all, as before |

**The third row stopped being a passing result.** A run whose `ONNXRUNTIME_ROOT`
ships `onnxruntime_providers_cuda.dll` but whose label says `CPU (1 thread)` is a
bundle that lost its provider - which is exactly what a wiped build directory
produces when the copy switch never reaches CMake, and every pixel check still
passes on the CPU path. Both suites now ask the question before they check the
label: if the runtime root carries a CUDA provider, `CPU (…)` is a failure that
names the load error, and `build.bat` forwards `OFXPLUGIN_BUNDLE_CUDA_PROVIDER`
from the environment so the switch cannot be dropped on the way to CMake
(**[V]**: 2026-11-25, a stub root with a provider that cannot load fails the
suite with `error 126`, the same root without one passes, and the real 1.20.1
tree bundles the provider and reports `all nodes on CUDA`).

**Reading a failed GPU run.** The expensive mistake is to treat a mismatch as a
numbers problem when it is a configuration one: with TF32 on, a convolution
rounds at `4.5e-4` (§5), so a golden comparison at `1e-5` fails for a reason that
has nothing to do with the plug-in; and a run can also fail for a reason that has
nothing to do with arithmetic at all, when a node or the provider is missing. The
label decides which of the four cases a log is in, before anyone edits a
tolerance:

| The label says | The run is | What to do |
|---|---|---|
| `CUDA (… bundled CUDA)` - no `tf32 off` | the EP defaults are in force: TF32 on, `EXHAUSTIVE`, max workspace | nothing about this frame is known; set the options, re-run, then read the numbers |
| `CUDA (…, nodes left on CPU)` | part of the graph ran on the CPU EP | not a device number: timing and coverage claims are void until the node is placed |
| `CUDA (…, node coverage unknown)` | the probe could not ask - no session was built | coverage is unknown, not "all" and not "none" |
| `CUDA (… tf32 off, all nodes on CUDA)` | every node on the device, full float32 | now it is arithmetic: the remaining difference is summation order (`5.4e-7` measured), the `1e-5` retry covers it, and a larger error is a plug-in bug |

Measured on this machine with the bundled 1.32.0 and CUDA 13 (**[V]**,
[`../evidence/2026-11-25-windows/onnx-fallback-probe.log`](../evidence/2026-11-25-windows/onnx-fallback-probe.log)):
the template's `test_conv.onnx` prints the first line in both host profiles, so
its single convolution is on the device - which the old label could not say.

**What the probe does not say.** The 1.32.0 message is `This session contains
graph nodes that are assigned to the default CPU EP, but fallback to CPU EP has
been explicitly disabled by the user.` - it states the fact and names no node, so
the label says *that* something was left on the CPU, not *what*. Naming the node
needs the verbose log (`ORT_LOGGING_LEVEL_VERBOSE`, which prints the EP assignment
per node) or `optimized_model_filepath` and reading the assignment in the
optimised graph. A failed probe starts that search; it does not end it.

The probe is only worth its cost if the key does something, so that was checked
directly rather than assumed: same model, same runtime, **no provider appended**,
once with the key and once without (**[V]**, same log). Without the key the
session builds on the CPU EP; with it, creation fails with the message above. A
key that was silently ignored would make every probe pass, and the label would go
on claiming CUDA for a session that never used it - the control is what makes
"all nodes on CUDA" a claim instead of a decoration.

Two costs stay. The probe is a second session creation, the expensive half of
loading, so the answer is cached per model path: it belongs to the model, not to
the instance. And `HEURISTIC` search (§5) is what keeps that second creation
cheap.

---

## 7. Runtime versions: what a new one has to satisfy

A plug-in that links ONNX Runtime is a bet on an API surface that grows with
every release. Two questions decide whether the template's code can follow a new
version, and both are answerable from the files alone - no compiler, no GPU:

| Question | Where the answer lives | Why it is not obvious |
|---|---|---|
| Which API version does this header target, and are the calls we make declared? | `#define ORT_API_VERSION` and the declarations in `include/onnxruntime_c_api.h` | a symbol can be present in your *source* and absent from the header you built against; `\since Version N` says when it arrived |
| Can the provider in this package be **registered**, or only appended through the built-in table? | the export table of `onnxruntime_providers_cuda.dll` / `.so` | `RegisterExecutionProviderLibrary` needs `CreateEpFactories`; a provider without it is not unregistrable by policy, it is unregistrable by construction |

`tools/check-runtime.py` answers both. It parses the header and the binary's
export and import tables in the standard library - PE and ELF - so it runs on a
Windows zip, a Linux tarball and a pip wheel alike, and it says "unreadable" for
anything it cannot parse instead of skipping it.

```bash
python3 tools/check-runtime.py .validation/ort-gpu-1.32.0
python3 tools/check-runtime.py <root> --require-api 22 --require-plugin-provider
```

Measured on the two runtimes this repository builds (**[V]**,
[`../evidence/2026-11-25-windows/check-runtime.log`](../evidence/2026-11-25-windows/check-runtime.log)):

| Root | `ORT_API_VERSION` | entry points this template uses | CUDA provider exports | what the provider imports |
|---|---|---|---|---|
| 1.20.1 | **20** | 5 of 10 - the five plugin-API calls are absent | **1** (`OrtGetApiBase`): built-in provider | cuDNN 9, cuBLAS/cuBLASLt 12, cuFFT 11, cudart 12 |
| 1.32.0 | **32** | all 10 | **3** (`CreateEpFactories`, `ReleaseEpFactory`, `OrtGetApiBase`): plugin EP | cuBLAS/cuBLASLt 13 only - cuDNN, cuFFT and cudart are resolved at run time, not imported |

**What that means before anyone writes a migration.** The registration API is a
1.22 addition, and its *provider* side is a packaging fact rather than a header
fact: the 1.20.1 provider exports one symbol, so a bundle built on it can never
be registered no matter how new the headers are. Code that uses
`RegisterExecutionProviderLibrary`, `GetEpDevices` and
`SessionOptionsAppendExecutionProvider_V2` therefore needs two things at once -
`#if ORT_API_VERSION >= 22` **and** a provider that exports `CreateEpFactories`.
The attempt of 2026-11-25 had the first and not the second on the runtime it was
tested against, and the process aborted with `0xC0000409` where the built-in
append still reports `all nodes on CUDA`; it is in
[`improvement-backlog.md`](improvement-backlog.md) row 13 with the numbers.

**What the import table already settles.** The 1.32 plugin provider imports
neither cuDNN, cuFFT nor cudart, and it does not import the core library at all -
it receives the api pointer in `CreateEpFactories`. §4's preload story - load the
four CUDA groups by absolute path so the built-in provider's imports resolve -
is therefore a property of the *built-in* provider. A plugin EP brings its own
loading order, which is one more reason the two paths must not be mixed in one
label.

**The deprecation watchlist**, so a version bump is a decision and not a
surprise:

| Change | Since | Replaces | Source |
|---|---|---|---|
| `use_tf32` can be switched off at all | 1.18 | nothing - TF32 was always on before, with no flag | CUDA EP option table (**[B]**) |
| CUDA EP registration as a plugin library | 1.22 | `SessionOptionsAppendExecutionProvider_CUDA_V2` for bundles that ship their own provider | `onnxruntime_c_api.h` (**[V]**) |
| Legacy TensorRT EP | marked compatibility-only in 1.32 | "the NVIDIA TensorRT RTX plugin EP via `OrtApi::RegisterExecutionProviderLibrary`" | a comment above `OrtCUDAProviderOptions` in the 1.32 header, quoted (**[V]**) |
| cuDNN as one DLL | 9 | ten DLLs under one dispatcher (`cudnn_engines_*`, `cudnn_ops`, ...) | the bundle listing in §4 (**[V]**) |
| `ep.cuda.*` option namespace | with the plugin EP | the bare key names the built-in append takes | documented as canonical for the plugin EP (**[B]**) - **not verified here**: no run of this repository reached registration, so no `ep.cuda.*` key has ever been accepted or rejected by a runtime |

**Adding a version, in order.** (1) extract it under `.validation/`, beside the
version it replaces, never on top of it; (2) run `tools/check-runtime.py` with the
gates the code needs - if `--require-plugin-provider` fails, the code path that
needs it is not available, whatever the header says; (3) build the CPU
configuration first, then the CUDA one, because a header change shows up as a
compile error long before it shows up as a wrong number; (4) run
`test/test.bat` or `test/run-tests.sh` against the GPU root and read the `provider`
label - `bundled CUDA` and `all nodes on CUDA` are the two facts that make a GPU
run a GPU run; (5) add the row to the matrix above and the log to `evidence/`,
and keep the older rows: they are what tells the next reader which code paths
still have a runtime to serve.

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[V]** verified in this repository - the command ran here
