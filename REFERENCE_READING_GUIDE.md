# Reference reading guide

**If you only read one thing, read this paragraph.** An OFX plug-in is three
different conversations with the host, and almost every beginner bug is a
confusion between them. The *descriptor* conversation (`describe()`) is
declarative: you say what you can do, using **strings**. The *render* conversation
is negotiated per frame: the host tells you, with **ints and pointers**, where the
pixels actually are and whether you got a stream. The *image* conversation is
arithmetic: you receive a base pointer, a **signed** row stride and a window that
need not start at the origin, and you must not substitute `width * bytesPerPixel`
for any of them. If you internalise those three and the ownership rule around
`fetchImage()`, the rest of this guide is an index into why the reference code
looks the way it does. The single most instructive file in this repository is
[`templates/minimal-cuda-ofx/src/Plugin.cpp`](templates/minimal-cuda-ofx/src/Plugin.cpp)
read next to [`SKILL.md`](SKILL.md) §1.

**Where the paths point.** Paths beginning `.ofx-ref/` are inside a checkout of
the OpenFX standard, which is git-ignored here and **not** part of a fresh clone:

```bash
git clone --depth 1 --branch OFX_Release_1.5.1 \
  https://github.com/AcademySoftwareFoundation/openfx.git .ofx-ref
```

Line numbers for SDK files are for that tag (`OFX_Release_1.5.1`); the build in
`templates/*/CMakeLists.txt` fetches the same tag. Paths beginning `templates/`
are in this repository and every one of them was checked against the working
tree when this guide was written.

## Contents

- [1. The three tiers](#1-the-three-tiers)
- [2. The five-seam skeleton](#2-the-five-seam-skeleton)
- [3. Patterns to look for](#3-patterns-to-look-for)
  - [3.1 Registration and packaging](#31-registration-and-packaging)
  - [3.2 Declaration versus per-render truth](#32-declaration-versus-per-render-truth)
  - [3.3 Addressing](#33-addressing)
  - [3.4 Ownership and error surface](#34-ownership-and-error-surface)
  - [3.5 The GPU contract](#35-the-gpu-contract)
  - [3.6 Region of interest and tiling](#36-region-of-interest-and-tiling)
  - [3.7 Model and tensor plumbing](#37-model-and-tensor-plumbing)
- [4. Anti-patterns and their tells](#4-anti-patterns-and-their-tells)
- [5. What each shipped project contributes](#5-what-each-shipped-project-contributes)
- [6. A seven-step reading plan](#6-a-seven-step-reading-plan)
- [7. Twelve questions to ask of any OFX code](#7-twelve-questions-to-ask-of-any-ofx-code)
- [8. How the numbers here are reproduced](#8-how-the-numbers-here-are-reproduced)
- [9. What this guide does not cover](#9-what-this-guide-does-not-cover)

---

## 1. The three tiers

There are three kinds of implementation you can learn from, and they are good for
different things. Reading them in one order and trusting them in another is the
point of this section.

| Tier | Where it is | Trust it for | Do **not** trust it for |
|---|---|---|---|
| **SDK samples** | `.ofx-ref/Support/Plugins/` — ten modules registering eleven plug-ins | The object model, the describe/render split, which actions and overrides exist, parameter types | Packaging, threading, tests, pitch arithmetic, GPU streams, error policy |
| **Shipped open-source plug-ins** | CorridorKey (×2), LTX, MoGe, DLSS5, Hush — analysed in [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) | Production architecture: process separation, tiling strategy, temporal chunking, colour handling | Style and correctness — several ship blocking renders or no tiling at all |
| **This repo's templates** | `templates/minimal-cuda-ofx/`, `templates/onnx-inference-ofx/` | The failure-proofing: bundle checks, export counts, golden fixtures, negative controls, sanitizers | Scale — each is deliberately one effect |

The census of the SDK samples is itself a lesson: ten modules register eleven
plug-ins, because `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp` pushes **two** static
factories in one module (`:12`–`:15`). Nine of the ten sources use
`OFX::ImageProcessor`, which is why the samples never show you pitch arithmetic:
the base class does it for you.

Three things the samples deliberately do not show, and where to look instead:

| Missing from the samples | Look at |
|---|---|
| Packaging (bundle layout, exported symbols, `RUNPATH`) | `templates/minimal-cuda-ofx/CMakeLists.txt`, [`SKILL.md`](SKILL.md) §6 |
| A render-thread-safety statement | `templates/minimal-cuda-ofx/src/Plugin.cpp:303` and the header default quoted in §3.2 |
| Tests that fail when the pitch, alpha or tiling is wrong | `templates/*/test/`, [§8](#8-how-the-numbers-here-are-reproduced) |

One negative finding is worth recording because it is commonly assumed
otherwise: **no sample sets `kOfxImageEffectPluginRenderThreadSafety`**. A grep
over `Support/Plugins/` returns zero hits. They get away with it because the
property's documented default is already `kOfxImageEffectRenderInstanceSafe`
(`.ofx-ref/include/ofxImageEffect.h:623`–`:629`), so silence is safe but undocumented — and
`kOfxImageEffectRenderFullySafe` remains an opt-in nobody in the samples takes.

**Read order:** SDK samples → this repo's templates → shipped projects.
**Trust order:** this repo's templates → shipped projects → SDK samples.

---

## 2. The five-seam skeleton

Every plug-in you open has these five pieces, in this order. `Invert` is the
smallest complete example: `.ofx-ref/Support/Plugins/Invert/invert.cpp` registers one factory at `:253`.

1. **Registration.** `OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray&)` is the
   only hook the Support library needs; it generates `OfxGetNumberOfPlugins` and
   `OfxGetPlugin` from the array you fill. The factory must have static storage
   duration because the host holds it for the life of the module.
2. **`describe()`.** Capabilities only. No images, no parameter *values*, no
   allocation that can fail. `.ofx-ref/Support/Plugins/Invert/invert.cpp:218` is `setSupportsTiles(true)`
   inside it — that is the kind of statement this function contains.
3. **`describeInContext()`.** Clips and parameters, per context. The two clip
   names that must exist are `kOfxImageEffectSimpleSourceClipName` and
   `kOfxImageEffectOutputClipName`.
4. **Instance constructor.** `fetchClip` / `fetchParam`, then a null check that
   throws `kOfxStatErrMissingHostFeature`. Clips and params are **borrowed**.
5. **`render()`.** Validate the formats you were handed, fetch images, read
   params at `args.time`, do the work, write through the host's stride.

Everything else is optional — and **which optional override a plug-in implements
tells you what kind of plug-in it is**, before you read a line of its maths:

| Override it implements | The plug-in is | Example |
|---|---|---|
| `getClipPreferences` | a generator: it has no source clip to inherit from | `.ofx-ref/Support/Plugins/Generator/noise.cpp:149` |
| `getTimeDomain` | a retimer: it maps output frames to source frames | `.ofx-ref/Support/Plugins/Retimer/retimer.cpp:189` (it then reads the source range at `:199`) |
| `getRegionsOfInterest` | spatially aware: output(x,y) needs input(x±r, y±r) | `.ofx-ref/Support/Plugins/Basic/basic.cpp:334` |
| `isIdentity` | able to bypass itself when neutral | `.ofx-ref/Support/Plugins/Basic/basic.cpp`, `.ofx-ref/Support/Plugins/GPUGain/GPUGain.cpp`; **absent** in `.ofx-ref/Support/Plugins/Invert/invert.cpp` |
| none of them | a pointwise filter | `.ofx-ref/Support/Plugins/Invert/invert.cpp` (zero hits for either name) |

A plug-in that implements none of these and still claims `setSupportsTiles(true)`
is telling you it has no notion of context. That is fine for `Invert` (a pointwise
function needs no neighbours) and a bug for anything with a kernel radius.

---

## 3. Patterns to look for

### 3.1 Registration and packaging

- **One hook, one array, static factories.** `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp:12`–`:15`
  is the whole pattern for shipping several plug-ins from one module:
  `static DotExamplePluginFactory p1; ids.push_back(&p1);` and the same for `p2`.
  This is also why symbol visibility matters: every plug-in statically links its
  own copy of the Support library, so two modules in one host process can export
  identical mangled names.
- **Exactly two exported dynamic symbols.** Measured in this repository for both
  templates (`OfxGetNumberOfPlugins`, `OfxGetPlugin`); the command and its output
  are in [§8](#8-how-the-numbers-here-are-reproduced). The mechanism is
  `CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN ON`
  (`templates/minimal-cuda-ofx/CMakeLists.txt:105`–`:112`), and for CUDA builds a
  **second, separate** property, `CUDA_VISIBILITY_PRESET hidden`: the CXX preset
  does not apply to the `.cu` translation unit. The comment at `:107`–`:110`
  explains why the two properties cannot be collapsed into one.
- **Bundle layout is a path convention, not a build detail.**
  `Name.ofx.bundle/Contents/Linux-x86-64/Name.ofx` (or `Contents/Win64/` for
  Windows). The bundle folder name, the binary stem and the `.ofx` suffix must
  agree; hosts scan that path and report nothing when it is wrong. Both templates
  write their module into exactly that directory.
- **`RUNPATH` must not contain the build machine's paths.** Both templates set
  `INSTALL_RPATH "$ORIGIN"` with `BUILD_WITH_INSTALL_RPATH ON`, so a bundled
  runtime next to the `.ofx` resolves and nothing else is baked in.

### 3.2 Declaration versus per-render truth

This is the most important distinction in the API, and it is entirely a
type-and-owner question:

| Layer | Property | Declared type | Set by | Header |
|---|---|---|---|---|
| Descriptor | `kOfxImageEffectPropCudaRenderSupported` | **string** × 1, default `"false"` for a plug-in | you, in `describe()` | `.ofx-ref/include/ofxGPURender.h:548`–`:555` |
| Descriptor | `kOfxImageEffectPropCudaStreamSupported` | **string** × 1, default `"false"` | you | `.ofx-ref/include/ofxGPURender.h:588` |
| Render in-args | `kOfxImageEffectPropCudaEnabled` | **int** × 1 | **the host**, per render | `.ofx-ref/include/ofxGPURender.h:565`–`:576` |
| Render in-args | `kOfxImageEffectPropCudaStream` | **pointer** (`cudaStream_t`) | the host, only when both sides opted in | `.ofx-ref/include/ofxGPURender.h:617` |

`kOfxImageEffectPropCudaEnabled` is documented as a property of the `inArgs` sets
of Render, BeginSequenceRender and EndSequenceRender, with `1` meaning
"`kOfxImagePropData` of each image is a CUDA memory pointer"
(`.ofx-ref/include/ofxGPURender.h:565`–`:576`). The descriptor string only says what you *can* do;
the int says what is true *now*. If you see `propGetInt` on the first row or
`propGetString` on the third, the file is wrong.

The template makes the distinction executable rather than a comment:
`templates/minimal-cuda-ofx/src/Plugin.cpp:79` throws if
`processImagesCuda()` is reached with `_isEnabledCudaRender` false, and `:80`
logs which property was missing.

Two more properties in the same family, both of which trip people up because
their defaults are the opposite of what they assume:

- **`kOfxImageEffectPropSupportsTiles` is an int, and its default is 1** — the
  header states `Type - int X 1`, `Default - to 1 for a plugin
  and clip` (`.ofx-ref/include/ofxImageEffect.h:844`–`:857`). Tiled images are defined as "input or
  output images can contain pixel data that is only a subset of their full RoD",
  and if you say `false` the host "should supply full RoD images"
  (`.ofx-ref/include/ofxImageEffect.h:849`–`:857`). So `false` is the *deliberate* statement, and
  both templates make it: `templates/minimal-cuda-ofx/src/Plugin.cpp:289` and
  `templates/onnx-inference-ofx/src/Plugin.cpp:241` (`setSupportsTiles(false)`
  in `describe()`). `GPUGain` makes the same opt-out with a compile-time constant:
  `#define kSupportsTiles false` at `.ofx-ref/Support/Plugins/GPUGain/GPUGain.cpp:20`, applied at `:361`,
  `:411` and `:418`.
- **`kOfxImageEffectPluginRenderThreadSafety` defaults to
  `kOfxImageEffectRenderInstanceSafe`** (`.ofx-ref/include/ofxImageEffect.h:623`–`:629`), not to
  `Unsafe`. Both templates set it anyway (`templates/minimal-cuda-ofx/src/Plugin.cpp:303`,
  `templates/onnx-inference-ofx/src/Plugin.cpp:251`) because the value is a promise a
  reviewer must be able to check; the CUDA template's comment above `:303` states
  exactly what `InstanceSafe` buys and why `FullySafe` would be a lie for its
  per-render state.

### 3.3 Addressing

Four rules, and the header sentence that settles them:

1. **`kOfxImagePropRowBytes` is the stride, it is signed, and it may be
   negative.** "For various alignment reasons, a row of pixels may need to be
   padded … row bytes can be negative, which allows hosts with a native top down
   row order to pass image into OFX without having to repack pixels. Row bytes is
   not supported for OpenCL Images." (`.ofx-ref/include/ofxImageEffect.h:1345`–`:1353`.) That last
   clause is why the pitch rules apply to OpenCL *Buffers*, not OpenCL Images.
2. **The base pointer addresses the image bounds, not the render window.** The
   idiom is `base + (window.y1 - bounds.y1) * rowBytes + (window.x1 - bounds.x1) * pixelBytes`,
   and it appears in `GainProcessor::processImagesCuda()`
   (`templates/minimal-cuda-ofx/src/Plugin.cpp`) and in
   `InferencePlugin::render()` (`templates/onnx-inference-ofx/src/Plugin.cpp`).
   On a device pointer the arithmetic is still safe: it is integer arithmetic on
   the handle and dereferences nothing on the CPU — the CUDA template says so in
   the comment above the expression.
3. **Widen the pitch before you multiply.** `int * int` overflows for large
   images; both templates cast `rowBytes` to `std::int64_t`/`long long` before
   multiplying by a row index.
4. **`getPixelAddress()` is defined out of line in the Support library.** Calling
   it once per pixel is a function call per pixel. `multiThreadProcessImages()` in
   `templates/minimal-cuda-ofx/src/Plugin.cpp` hoists it to one call per row and
   says so in a comment; the scanner to spot in other code is
   `getPixelAddress` inside the `x` loop.
5. **Alpha is not yours to overwrite unless the effect is about alpha.** The
   inference template copies it through explicitly — `CopyAlpha` is defined at
   `templates/onnx-inference-ofx/src/Plugin.cpp:50` and called at `:152` before
   inference — and its test asserts the alpha channel is untouched on tight,
   padded and negative-stride images.

### 3.4 Ownership and error surface

- `fetchImage()` returns **owned** memory; `fetchClip()`/`fetchParam()` return
  **borrowed** handles. The safe idiom is `std::unique_ptr` on the same line as
  the fetch, so every later `throw` releases the host image (the host will
  otherwise run out of frames). See `setupAndProcess()` in
  `templates/minimal-cuda-ofx/src/Plugin.cpp`.
- **`abort()` is checked between rows or frames**, not once per render: a long
  effect must return promptly when the user scrubs.
- **Every error path logs and throws a specific code.** The inference template
  rejects an unsupported format with `kOfxStatErrUnsupported`
  (`templates/onnx-inference-ofx/src/Plugin.cpp:109`, `:118`, `:168`); the CUDA template
  throws `kOfxStatGPURenderFailed` when a launch or sync fails. Silently returning
  `kOfxStatOK` produces a black frame with no diagnostic, and Resolve blacklists
  plug-ins that crash, so the next launch looks like "the plug-in vanished".
- **`stdout` is invisible in every GUI host.** `OFX::Log::print` (which wraps
  `OfxMessageSuite`) is the only channel; the templates use it on every failure
  path.

### 3.5 The GPU contract

The header settles this in one paragraph, and it is worth reading verbatim
(`.ofx-ref/include/ofxGPURender.h:600`–`:616`): if `kOfxImageEffectPropCudaStream` is set, you
enqueue on that stream and "SHOULD NOT wait for final asynchronous operations to
complete before returning from the render action, and SHOULD NOT call
`cudaDeviceSynchronize()` at any time" (`:610`); if it is **not** set, you must
ensure your asynchronous work completed before returning.

That asymmetry is two branches, and both are mandatory:

```cpp
if (_pCudaStream == nullptr) { /* finish the work: sync the stream you used */ }
// else: return immediately and let the host order its work
```

It is implemented at `templates/minimal-cuda-ofx/src/Plugin.cpp:120`–`:126`
(the `WaitForDefaultStream()` call is guarded by `_pCudaStream == nullptr`). Three
further rules the template teaches by construction:

- **Bind to the device that owns the buffer, not the thread's current device.**
  `Kernel.cu` queries `cudaPointerGetAttributes` on the source and wraps
  `cudaSetDevice` in a `DeviceGuard` that restores the previous device on scope
  exit — otherwise a multi-GPU host leaks your device choice into later actions.
- **Check launch errors immediately** with `cudaGetLastError()`: asynchronous
  errors are invisible until a synchronisation point, which is precisely why the
  sync policy above matters.
- **Advertise the capability only when the path exists.** The whole block is
  behind `#ifdef OFXPLUGIN_WITH_CUDA` (`templates/minimal-cuda-ofx/src/Plugin.cpp:308`);
  a build without a toolkit must not tell the host it can take device pointers.

### 3.6 Region of interest and tiling

- **Spatially aware** means you implement `getRegionsOfInterest` and expand the
  source region by your radius in *canonical* coordinates
  (`.ofx-ref/Support/Plugins/Basic/basic.cpp:334` is the SDK's example). The failure mode is a visible seam
  at tile edges that everyone blames on the kernel.
- **`supportsTiles(true)` is not "I tile internally".** The header defines tiled
  images as images that "contain pixel data that is only a subset of their full
  RoD" (`.ofx-ref/include/ofxImageEffect.h:849`–`:857`). Saying `true` means you accept partial
  images and must produce correct output for them; saying `false` asks the host
  for full-RoD fetches. Both templates do their own tiling and therefore say
  `false` (`templates/minimal-cuda-ofx/src/Plugin.cpp:289`), with a comment that spells out
  the difference.
- **Internal tiling requires a halo at least as large as the model's or kernel's
  receptive field radius.** The contract is documented at
  `templates/onnx-inference-ofx/src/AiCore.h:83`–`:85` ("1 for a single 3×3
  convolution, more for a stack of them"), and it is enforced by a *negative
  control*: the same tiled render with `halo = 0` must disagree with whole-frame
  inference (`templates/onnx-inference-ofx/test/ai_pipeline_test.cpp:273`).
  Without that control, a tiled-vs-tiled comparison passes for the wrong reason.

### 3.7 Model and tensor plumbing

Only relevant if the effect runs a network; the full treatment is
[`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md). The four facts that decide
correctness and are *not* expressible in OFX: channel order (RGB vs BGR), value
range (`[0,1]`, `[-1,1]`, display-referred), colour space (convert with OCIO
before inference if the model was trained on display-referred images), and tensor
layout (`NCHW` vs `NHWC`). Two more that are code-review items:

- **Session once, not per render.** Session creation parses the graph and
  allocates; the inference template keeps one session behind a pimpl and a mutex
  (`templates/onnx-inference-ofx/src/Plugin.cpp`, `AcquireRunner`).
- **Steady-state render must not allocate per frame.** The tiling scratch buffers
  are reused and only resized when the frame size changes
  (`templates/onnx-inference-ofx/src/AiCore.h`/`templates/onnx-inference-ofx/src/AiCore.cpp`); the fixture model is 611 bytes and the golden output
  is committed, so the test needs no Python and no download
  (`templates/onnx-inference-ofx/models/`).

---

## 4. Anti-patterns and their tells

The "tell" column is what to search for when reviewing someone else's plug-in —
each one is a single grep away.

| Anti-pattern | Tell in the code | Why it fails |
|---|---|---|
| Pitch assumed tight | `width * 4 * sizeof(float)`, or `width * 16` | Hosts pad scanlines; the image shears at odd sizes and looks fine in one host |
| Base pointer treated as the render window | no `(window.y1 - bounds.y1)` term before the row multiply | Offset renders (crop, RoD) shift the image |
| `int` pitch arithmetic | `int pitch = getRowBytes();` | Overflow on large frames |
| `getPixelAddress()` per pixel | call inside the `x` loop | Out-of-line call per pixel in the Support library |
| `cudaDeviceSynchronize()` | `grep -rn cudaDeviceSynchronize src/` | Forbidden by the spec; serialises the host's GPU pipeline |
| Device→device `cudaMemcpy` into a private buffer | `cudaMemcpy` in a render path with device pointers | Doubles bandwidth and defeats zero-copy |
| Allocation in `render()` | `cudaMalloc`/`new`/`std::vector` growth inside the render action | Per-frame cost and fragmentation; breaks `FullySafe` claims |
| Silent failure | `catch (...) { return kOfxStatOK; }` or a bare `return` | Black frame, no log, nothing to diagnose |
| Raw `fetchImage` pointer | `OFX::Image* img = clip->fetchImage(t);` then `delete` | Any intervening `throw` leaks a host image |
| Missing `setGPURenderArgs` | `processor.process()` without it | The CUDA path silently never runs; the CPU path hides it |
| Tile claim without context | `setSupportsTiles(true)` plus no `getRegionsOfInterest` | Host hands over a tile; the effect treats it as a whole frame |
| Global session/state | `static` session, static scratch buffer | Two instances or two render threads share it |

---

## 5. What each shipped project contributes

All six were read at source level; none was built or executed here, so these are
evidence grade B and the architectures — not the numbers — are what transfer.

- **`CorridorKey-Runtime`** — the only one with a session broker and a
  model-resolution ladder (≤1000→512, ≤2000→1024, ≤3000→1536, >3000→2048) plus an
  explicit tiling toggle: evidence that resolution belongs in the UI, not in a
  constant.
- **`Flame_LTXAlphaGen_OFX`** — the only temporal design: `8n+1` frame chunks
  (145/73/33), overlaps of 24/16/12 frames, dissolve or flow-warp merge, ROI
  margins ≥64 px with sides rounded to 32, OCIO conversion before inference. If
  the model has a time axis, copy this shape.
- **`MoGe-nuke`** — the useful anti-pattern: synchronous per-frame TCP to a Python
  daemon, no tiling, viewer blocked. Read it to recognise the design not to copy.
- **`DaVinci-Resolve-DLSS5`** — in-process vendor runtime done properly (NGX):
  float RGBA → `R8G8B8A8_UNORM`, motion `R16G16_FLOAT`, depth `R32_FLOAT`, and the
  bottom-left→top-left origin flip. Origin conventions are per-host detail you
  will not derive from the spec.
- **`Hush-OpenNR`** — the counter-example that keeps the plan honest: a fast GPU
  denoiser with **no network**, built from measured noise statistics.
- **`GPUGain` (SDK)** — the only sample showing GPU negotiation, including the
  OpenCL Images-vs-Buffers distinction: the *host* decides which is used, and the
  plug-in branches on which property is set (`.ofx-ref/Support/Plugins/GPUGain/GPUGain.cpp:95`–`:109`,
  calling `getOpenCLImage()` and commenting on
  `kOfxImageEffectPropOpenCLImage` vs `kOfxImagePropData`). It also disables
  tiling (`:20`), the earliest shipped statement of the buffer/tile conflict.

---

## 6. A seven-step reading plan

| Step | Read | Look for | Time |
|---|---|---|---|
| 1 | `.ofx-ref/Support/Plugins/Invert/invert.cpp` | The minimum viable plug-in: registration (`:253`), describe, render, nothing else | 15 min |
| 2 | `.ofx-ref/Support/Plugins/Basic/basic.cpp` | `getRegionsOfInterest` (`:334`), `isIdentity`, a mask clip, several params | 30 min |
| 3 | `.ofx-ref/Support/Plugins/GPUGain/GPUGain.cpp` | GPU negotiation, OpenCL dispatch, `setGPURenderArgs` (`:322`), tiles off (`:20`) | 30 min |
| 4 | `templates/minimal-cuda-ofx/src/Plugin.cpp` | What the samples omit: per-render int vs descriptor string (`:79`), stream asymmetry (`:120`–`:126`), `InstanceSafe` (`:303`) | 45 min |
| 5 | `templates/minimal-cuda-ofx/test/run-tests.sh` | How the invariants are proven without a GPU | 20 min |
| 6 | `templates/onnx-inference-ofx/src/AiCore.cpp` + `templates/onnx-inference-ofx/test/ai_pipeline_test.cpp` | Packing, tiling, golden fixtures, the halo negative control (`:273`) | 45 min |
| 7 | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §1 and §6 | What production inference plug-ins do; when CUDA pays | 20 min |

Steps 1–3 need the `.ofx-ref` checkout (see the top of this file). Steps 4–7 need
only this repository.

---

## 7. Twelve questions to ask of any OFX code

1. Does it export exactly two symbols, with a `static` factory?
2. Does `describe()` set contexts, bit depths, tiling, thread safety and GPU
   flags — and are the GPU *capability* flags strings?
3. Does `render()` read `kOfxImageEffectPropCudaEnabled` before treating a pointer
   as device memory?
4. Does it use `kOfxImagePropRowBytes` with a signed 64-bit widening, and offset
   by `(window − bounds)`?
5. Are `fetchImage` results owned by `unique_ptr` at creation, and is `abort()`
   checked inside long loops?
6. Does every error path log **and** throw a specific `kOfxStat*`?
7. If it has a GPU path, is the "stream present → return; stream absent → finish"
   branch present, and is `cudaDeviceSynchronize` absent?
8. Does it restore the previous CUDA device after `cudaSetDevice`?
9. If it is spatially aware, does `getRegionsOfInterest` expand in canonical
   coordinates?
10. If it tiles internally, is the halo ≥ the receptive-field radius, and is there
    a test that proves the halo matters?
11. Is heavy state (session, allocation) created once and owned per instance?
12. Is there a host-free test suite that would fail if the pitch, the alpha
    channel or the tiling were wrong?

If all twelve are "yes" from the source alone, the implementation is worth
copying. That set is what `templates/` encodes and what the SDK samples and the
shipped projects each leave partly unanswered.

---

## 8. How the numbers here are reproduced

Every number in this guide was produced on the machine that wrote it. These are
the exact commands; all of them are read-only apart from building inside
`templates/*/build`.

**Exported symbols — 2 per template.** After `./build.sh` in a template:

```bash
nm -D --defined-only templates/minimal-cuda-ofx/build/bundle/MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx \
  | grep -c ' T '        # prints 2: OfxGetNumberOfPlugins, OfxGetPlugin
```

The same command on the inference template prints 2. With the visibility block
removed from `CMakeLists.txt` (and a CPU-only build), the same check prints
**663**, because the static Support library's symbols become visible — which is
the failure mode the hidden-visibility rules exist to prevent. That variant was
built from a copy outside the tree, so the shipped `CMakeLists.txt` is unchanged.

**Bundle sizes — 408K and 34M.** The CUDA template's bundle is 408K because it
ships no runtime; the inference template's is 34M because it carries one copy of
`libonnxruntime.so.1`. Reproduce with:

```bash
du -sh templates/*/build/bundle/*.ofx.bundle
```

**Model fixture — 611 bytes.** `ls -l templates/onnx-inference-ofx/models/test_conv.onnx`.

**Halo deviation — 0 with `halo=1`, 0.263276 with `halo=0`.** Reproduce from
`templates/onnx-inference-ofx`:

```bash
ONNXRUNTIME_ROOT=/path/to/onnxruntime ./test/run-tests.sh
```

The interesting lines are `max |c++ - python| over the frame … (0)` (the CPU
result is bit-identical to the Python ONNX Runtime golden), every
`tile=… halo=1 matches whole … (0)`, and
`tile=13 halo=0 disagrees with whole (expected) … (0.263276)`. The last one is
the negative control: tiling without context is wrong, and the suite proves the
difference is real rather than asserting it.

**SDK census — ten modules, eleven plug-ins.** Count the factories:

```bash
grep -rn "push_back(&" .ofx-ref/Support/Plugins/*/*.cpp
```

`.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp` contributes two; every other module one.
Nine of the ten sources include `OFX::ImageProcessor`:
`grep -l ImageProcessor .ofx-ref/Support/Plugins/*/*.cpp | wc -l` prints 9.

---

## 9. What this guide does not cover

- **No OFX host was available.** Nothing in this repository has rendered a pixel
  through Resolve, Nuke, SCRATCH or Natron; host-specific behaviour lives in
  [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) at evidence
  grade B/C.
- **No GPU was available.** No kernel or CUDA execution provider has executed on
  hardware here, so every GPU statement above is derived from headers,
  documentation or host-side emulation, never from a device run.
- **The SDK samples were read, not executed.** Their line numbers are for tag
  `OFX_Release_1.5.1` and will drift on other tags; re-grep the symbol if a line
  looks wrong.
- **No performance numbers.** Nothing here times an effect, a kernel or a model;
  the numbers in §8 are sizes, symbol counts and exactness checks.
