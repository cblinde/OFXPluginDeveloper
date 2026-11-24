# Reference projects and the reading plan

What each shipped project contributes, what to copy, what to avoid, and the seven-step reading plan.

<!-- REFERENCE_READING_GUIDE.md lines 1-512 -->
<!-- PATTERN_INDEX.md lines 188-258 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 47-88 -->
<!-- README.md lines 59-158 -->
<!-- REFERENCE_READING_GUIDE.md lines 1-50 -->
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
[`templates/minimal-cuda-ofx/src/Plugin.cpp`](../templates/minimal-cuda-ofx/src/Plugin.cpp)
read next to [`../SKILL.md`](../SKILL.md) §1.

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
| **Shipped open-source plug-ins** | CorridorKey (×2), LTX, MoGe, DLSS5, Hush — analysed in [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) | Production architecture: process separation, tiling strategy, temporal chunking, colour handling | Style and correctness — several ship blocking renders or no tiling at all |
| **This repo's templates** | `templates/minimal-cuda-ofx/`, `templates/onnx-inference-ofx/` | The failure-proofing: bundle checks, export counts, golden fixtures, negative controls, sanitizers | Scale — each is deliberately one effect |

The census of the SDK samples is itself a lesson: ten modules register eleven
plug-ins, because `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp` pushes **two** static
factories in one module (`:12`–`:15`). Nine of the ten sources use
`OFX::ImageProcessor`, which is why the samples never show you pitch arithmetic:
the base class does it for you.

Three things the samples deliberately do not show, and where to look instead:

| Missing from the samples | Look at |
|---|---|
| Packaging (bundle layout, exported symbols, `RUNPATH`) | `templates/minimal-cuda-ofx/CMakeLists.txt`, [`../SKILL.md`](../SKILL.md) §6 |
| A render-thread-safety statement | `templates/minimal-cuda-ofx/src/Plugin.cpp:317` and the header default quoted in §3.2 |
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
  agree; hosts scan that path and report nothing when it is wrong. The templates
  write their module into exactly that directory.
- **`RUNPATH` must not contain the build machine's paths.** The templates set
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
  the templates make it: `templates/minimal-cuda-ofx/src/Plugin.cpp:303` and
  `templates/onnx-inference-ofx/src/Plugin.cpp:365` (`setSupportsTiles(false)`
  in `describe()`). `GPUGain` makes the same opt-out with a compile-time constant:
  `#define kSupportsTiles false` at `.ofx-ref/Support/Plugins/GPUGain/GPUGain.cpp:20`, applied at `:361`,
  `:411` and `:418`.
- **`kOfxImageEffectPluginRenderThreadSafety` defaults to
  `kOfxImageEffectRenderInstanceSafe`** (`.ofx-ref/include/ofxImageEffect.h:623`–`:629`), not to
  `Unsafe`. The templates set it anyway (`templates/minimal-cuda-ofx/src/Plugin.cpp:317`,
  `templates/onnx-inference-ofx/src/Plugin.cpp:375`) because the value is a promise a
  reviewer must be able to check; the CUDA template's comment above `:317` states
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
   images; the templates cast `rowBytes` to `std::int64_t`/`long long` before
   multiplying by a row index.
4. **`getPixelAddress()` is defined out of line in the Support library.** Calling
   it once per pixel is a function call per pixel. `multiThreadProcessImages()` in
   `templates/minimal-cuda-ofx/src/Plugin.cpp` hoists it to one call per row and
   says so in a comment; the scanner to spot in other code is
   `getPixelAddress` inside the `x` loop.
5. **Alpha is not yours to overwrite unless the effect is about alpha.** The
   inference template copies it through explicitly — `CopyAlpha` is defined at
   `templates/onnx-inference-ofx/src/Plugin.cpp:55` and called at `:243` before
   inference — and its test asserts the alpha channel is untouched on tight,
   padded and negative-stride images.

Writing *into* the frame rather than copying it through repeats all five rules
against the destination image, which has its own pitch and bounds, and adds
three drawing-specific traps: placement in frame coordinates, premultiplied
plates, and a single rasterising origin. That contract is
[`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §7, "Drawing into the frame:
the overlay after the model".

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
  (`templates/onnx-inference-ofx/src/Plugin.cpp:174`, `:198`); the CUDA template
  throws `kOfxStatGPURenderFailed` when a launch or sync fails. Silently returning
  `kOfxStatOK` produces a black frame with no diagnostic, and Resolve blacklists
  plug-ins that crash, so the next launch looks like "the plug-in vanished".
- **`stdout` is invisible in every GUI host.** `OFX::Log::print` is not the
  host channel either: it appends to a file, and Support guards the `fopen` with
  `#ifdef DEBUG` (`ofxsLog.cpp:40-47`) — which CMake's `Debug` configuration and
  the templates do not define. The templates use it on every
  failure path, so treat it as a developer-build trace; a user-visible error
  needs the host's message suite (`setPersistentMessage`, `debugging-and-symptoms.md` §7).

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

It is implemented at `templates/minimal-cuda-ofx/src/Plugin.cpp:119`–`:127`
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
  behind `#ifdef OFXPLUGIN_WITH_CUDA` (`templates/minimal-cuda-ofx/src/Plugin.cpp:319`);
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
  for full-RoD fetches. The templates do their own tiling and therefore say
  `false` (`templates/minimal-cuda-ofx/src/Plugin.cpp:303`), with a comment that spells out
  the difference.
- **Internal tiling requires a halo at least as large as the model's or kernel's
  receptive field radius.** The contract is documented at
  `templates/onnx-inference-ofx/src/AiCore.h:87`–`:89` ("1 for a single 3×3
  convolution, more for a stack of them"), and it is enforced by a *negative
  control*: the same tiled render with `halo = 0` must disagree with whole-frame
  inference (`templates/onnx-inference-ofx/test/ai_pipeline_test.cpp:300`).
  Without that control, a tiled-vs-tiled comparison passes for the wrong reason.
- **A tiled plug-in that also draws has two independent failure modes.** The halo
  decides what the model sees; the overlay's placement decides what the viewer
  sees. A seam and a shifted box are different bugs, and only the second one
  survives an orientation-blind assertion (a copy matches either way up). The
  drawing contract — destination pitch, frame coordinates, premultiplied plates,
  one rasterising origin — is
  [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §7, "Drawing into the frame:
  the overlay after the model".

### 3.7 Model and tensor plumbing

Only relevant if the effect runs a network; the full treatment is
[`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md). The four facts that decide
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
| 4 | `templates/minimal-cuda-ofx/src/Plugin.cpp` | What the samples omit: per-render int vs descriptor string (`:79`), stream asymmetry (`:119`–`:127`), `InstanceSafe` (`:317`) | 45 min |
| 5 | `templates/minimal-cuda-ofx/test/run-tests.sh` | How the invariants are proven without a GPU | 20 min |
| 6 | `templates/onnx-inference-ofx/src/AiCore.cpp` + `templates/onnx-inference-ofx/test/ai_pipeline_test.cpp` | Packing, tiling, golden fixtures, the halo negative control (`:300`) | 45 min |
| 7 | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §1 and §6 | What production inference plug-ins do; when CUDA pays | 20 min |

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

Figures marked **[V]** were produced in this repository (compiled or measured);
the commands are in `testing-layers.md` §13. Figures marked **[I]** are
inferred and have not been reproduced.

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
**663** **[V]**, because the static Support library's symbols become visible — which is
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

- **One host, headless.** `NatronRenderer` has rendered a frame through the
  template (`testkit/verify-bundle.bat`, 2026-11-24). Resolve, Nuke and SCRATCH
  have not, so their behaviour still lives in
  [`host-resolve.md`](host-resolve.md) at grade B/C.
- **One GPU, and only the gain kernel.** The 2026-11-24 run executes the real
  kernels on an RTX 3060 and measures them; no CUDA execution provider executes,
  and nothing runs on a second GPU. Statements about models, other GPUs and host
  scheduling stay derived from headers and documentation.
- **The SDK samples were read, not executed.** Their line numbers are for tag
  `OFX_Release_1.5.1` and will drift on other tags; re-grep the symbol if a line
  looks wrong.
- **No performance numbers.** Nothing here times an effect, a kernel or a model;
  the numbers in §8 are sizes, symbol counts and exactness checks.

## 5. The general-purpose suites: what to read, what to skip

The AI repositories in [§4](ai-architectures-and-runtimes.md#4-external-ai-onnx-plug-ins-what-exists-what-to-learn-what-to-avoid)
answer "how do I ship a model". These answer "how do I ship a plug-in at all",
and they are where the host protocol is exercised at a scale one person cannot
write alone.

Metadata below was checked on **2026-11-22**: every URL resolved with
`git ls-remote`, the licence was read from the repository's own licence file, and
the `openfx-misc` pointers in [§5.1](#51-mechanics-and-the-exact-file-to-read)
were verified in a shallow clone. Still grade **B** — none of it was built here.

| Repository | Licence | What it is | Read it for | Do not read it for |
|---|---|---|---|---|
| [`NatronGitHub/openfx-misc`](https://github.com/NatronGitHub/openfx-misc) | GPL-2.0 | 92 node directories — the widest single OFX collection | Declared contexts, clip and parameter shapes, temporal actions; exact files in [§5.1](#51-mechanics-and-the-exact-file-to-read) | GPU work: the repository contains **0** `.cu` or `.cl` files |
| [`NatronGitHub/openfx-io`](https://github.com/NatronGitHub/openfx-io) | GPL-2.0 | readers/writers on OpenImageIO and FFmpeg, plus OpenColorIO | Generator context, host memory, colour-config initialisation | Pixel effects — it is I/O |
| [`NatronGitHub/openfx-arena`](https://github.com/NatronGitHub/openfx-arena) | GPL-2.0 | Cairo/Pango text and vector, audio-driven parameters | Overlay drawing, dynamic outputs, non-pixel inputs | GPU code — the drawing is CPU-side Cairo |
| [`NatronGitHub/openfx-gmic`](https://github.com/NatronGitHub/openfx-gmic) | dual-licensed — read its `COPYING` | G'MIC bridged into OFX | Generating parameters from a foreign engine at `describe` time | Copying: two licences apply and the engine's is not yours |
| [`NatronGitHub/openfx-opencv`](https://github.com/NatronGitHub/openfx-opencv) | no licence file at the root — check before copying | OpenCV bridged into OFX | `cv::Mat` ↔ OFX image conversion | Modern host builds — its README still badges `devernay/openfx-opencv` |
| [`gillesvink/opendefocus`](https://github.com/gillesvink/opendefocus) | EUPL-1.2 | Rust convolution library (published on crates.io) with an OFX front end | A large, living defocus implementation, and how a non-C++ core is packaged | Licensing: EUPL is copyleft |
| [`purzbeats/purzos-ofx`](https://github.com/purzbeats/purzos-ofx) | MIT | 64 single-file plug-ins, one shared header, built on the Support library | Determinism as a feature: randomness hashed from `(seed, frame, position)`, byte-identical renders | GPU code — CPU pixel loops |
| [`tuttleofx/TuttleOFX`](https://github.com/tuttleofx/TuttleOFX) | no licence at the root — check subprojects | framework plus bundled plug-ins, default branch `develop` | A C++ abstraction over the C API at framework scale | Assuming it is maintained — check activity first |

### 5.1 Mechanics and the exact file to read

All paths are inside `openfx-misc` at its 2026-11-22 `master` (92 node
directories). The size of each file is part of the lesson: the mechanic is never
the one-liner an index promises.

| You need | Open |
|---|---|
| Several input clips, negotiated at render time | `Merge/Merge.cpp:1559` (`eContextGeneral`); the node is 2,055 lines |
| Time remapping, and telling the host which frames you will read | `Retime/Retime.cpp:428` (the comment that states the contract) and `:477` (`getFramesNeeded`) |
| A different frame on every scanline | `SlitScan/SlitScan.cpp:296` (`getFramesNeededRange`), `:315` (`getFramesNeeded`) |
| A frozen frame | `FrameHold/FrameHold.cpp:77` |
| An RoI that follows a warp | `Distortion/Distortion.cpp:2980` (`getRegionsOfInterest`); the node is 6,634 lines |
| Writing parameters from inside `render` | `ImageStatistics/ImageStatistics.cpp:98` — the comment explains why the write is deferred |
| An OpenGL render suite, with a GPU on/off parameter | `Shadertoy/Shadertoy.cpp:1240`, `:2296` (`setSupportsOpenGLRender`) |
| Overlay interacts | 18 node sources use `OverlayInteract`; start with `Transform/Transform.cpp`, `Crop/Crop.cpp`, `Card3D/Card3D.cpp` |

### 5.2 Before you trust a third-party index

A "50 OFX plug-ins" index circulating as an AI prompt mixes real repository
names with API that does not exist. Every row below is one `grep` against
`.ofx-ref/include`; run the equivalent check before copying any symbol out of an
index.

| The index says | OFX 1.5.1 says |
|---|---|
| `ofxCUDAImageEffectSuite->getCudaStream(handle, &stream)` | There is no CUDA suite. `grep -rn "CUDA.*Suite" .ofx-ref/include/` matches a comment. The stream arrives as `kOfxImageEffectPropCudaStream` in the render inArgs (`ofxGPURender.h:617`), and only if the effect advertised `kOfxImageEffectPropCudaStreamSupported` (`:588`) |
| "Ensure explicit stream synchronisation (`cudaStreamSynchronize`, or the host stream)" | Backwards. With a host stream, the plug-in SHOULD NOT wait and SHOULD NOT call `cudaDeviceSynchronize()` at any time (`ofxGPURender.h:608`–`:610`). Wait only when no stream was supplied — see [`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) |
| Transition progress parameter `TransitionMain` | `kOfxImageEffectTransitionParamName`, value `"Transition"` (`ofxImageEffect.h:1475`) |
| "`ImageStatistics`: atomic GPU reduction kernels" | One CPU translation unit, `ImageStatistics/ImageStatistics.cpp` (2,959 lines). No GPU code exists anywhere in that repository |
| `GainPlugin`, `TemporalBlurPlugin`, … under `DaVinci-Resolve-OpenFX-Samples` | No public Blackmagic repository of that name; the only GitHub match is an archived third-party copy. The sample plug-ins ship inside the Resolve installer, which is why the public SDK is the ASWF `openfx` repository |

Two habits follow. An index's *count* is nodes, not repositories: the "50" above
names 9 repositories, and 38 of its entries are directories inside
`openfx-misc`. And an index that shows CUDA mechanics for `openfx-misc` is
describing code that is not there, so treat its API names as unverified until a
header agrees.

## 6. If you only need the decision, not the code

| Situation | Shape | Where the reasoning is |
|---|---|---|
| Model is ONNX, ops are supported by the C runtime you can ship | in-process | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §2, §4 |
| Model needs PyTorch code, custom ops, or a Python preprocessing stack | sidecar (shared memory + control socket) | §2, §3 |
| Inference takes longer than a frame budget | staged job + disk cache | §1 above, and the LTX design in §4 |
| Operation is local and analytic | no model at all | `Hush-OpenNR` in §4 |
| GPU is available but the host does not hand over device pointers | CPU, or accept one copy per frame and measure | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §6 |

## 2. What the reference projects teach

These are not summaries of feature lists; each row is a decision that another
team already paid for.

| Project | Practice | Why it matters | Adopted here? |
| --- | --- | --- | --- |
| **OneGrade** | *"The CPU header is the source of truth… any change to the math is a 4-file change (CPU + 3 kernels). Keep the helper names and formulas identical so they're easy to diff."* **[R]** | Four backends drifting apart is the normal failure mode of a multi-backend plugin, and it shows up as "CUDA looks different from CPU" only after a client notices. Naming parity makes the drift visible in a diff. | Partly: one kernel + one CPU loop, named to match, but no shared pipeline header yet. §6.4 says when to add one. |
| **OneGrade** | *"Confirm the GPU kernel actually shipped — a plugin built without it looks fine and just renders on the CPU"*, via `cuobjdump --list-elf …ofx`. **[R]** | A silently CPU-only build is indistinguishable from a working one in the UI. This is the single highest-value 10-second check in the whole discipline. | Yes: `build.sh` checks the launcher symbol and `.nv_fatbin`; §8 adds the `cuobjdump` form for the reader's own builds. |
| **OneGrade** | *"CUDA needs … a CUDA 13.x toolkit (12.x emits no Blackwell/sm_120 code)"*, and OpenCL validated against the CPU pipeline to *"worst deviation well under one 8-bit code value"*. **[R]** | Architecture flags and cross-backend tolerance are both testable facts, not opinions. | Partly: the template emits `100;120` when `CUDAToolkit_VERSION >= 12.8` **[S]**; cross-backend tolerance testing is described in §9.3, not implemented. |
| **OneGrade** | *"close Resolve first — the plugin is locked while it runs"* **[R]** | Installing over a loaded `.ofx` on Windows fails in a way that looks like a corrupt bundle. | Documented in `debugging-and-symptoms.md` §7. |
| **Open-DRT-OFX** | SDK *"cloned on demand (not vendored)"*, CMake ≥ 3.20, CI builds all three platforms on every tag. **[R]** | Vendoring the SDK freezes you on a stale spec and drags a licence into your tree; fetching keeps the headers truthful. | Yes: `FetchContent` on tag `OFX_Release_1.5.1`, shallow, or `OFX_SOURCE_DIR` for offline builds. **[S]** |
| **forge-colorx** | *"GPU where it's safe"* — Metal is gated to a host allow-list because *"Flame's macOS OFX host crashes on a Metal-advertising node"*; CUDA is opt-in at build time (`make WITH_CUDA=1`). **[R]** | Advertising a GPU capability is a promise the host acts on. Advertising one you cannot honour in a specific host is worse than not advertising it. | Philosophical yes (CUDA is advertised only when compiled in **[S]**); per-host gating is not implemented, and this is the strongest argument for keeping `references/host-resolve.md` in the loop. |
| **gyroflow-plugins** | The heavy work (gyro sync, lens profiles, optical flow) happens in the desktop app and is handed to the plugin as a project file; the plugin only renders. **[R]** | Anything expensive and per-clip belongs outside the render path. A render action is called per frame, sometimes per field, sometimes twice. | Yes in principle: the render action does no analysis, only one kernel launch. |
| **gyroflow-plugins** | Post-mortem of 32 Resolve crashes in 24 days, 31 of them the plugin releasing a host-owned `MTLCommandQueue`. **[R]** | Borrowed GPU objects belong to the host. Destroying one corrupts the host's queue and takes the process down later, somewhere else. | Yes: the stream is stored as an opaque `void*`, never created, never destroyed. **[S]** |
| **gyroflow-plugins** | Notes that applying the effect on the Edit/Color page *"should work faster"* than in Fusion. **[R]** | Host-side pipeline differences can dominate your kernel. When a user reports slowness, establish which page/composition they are on before optimising anything. | Recorded in `references/host-resolve.md`. |

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
| The kernel indexes `((y * W) + x) * 4`, assuming tightly packed rows | Correct only while `rowBytes == width * 16`; any host padding shears the image, and it silently ignores a negative pitch **[S]** | Address through `kOfxImagePropRowBytes` in bytes, widened to `int64_t` and signed (§6 of `references/ofx-lifecycle-and-threads.md`) |

This does not make OneGrade a bad plugin — it is a colour tool whose pixel work
dominates its overhead, and its authors validated it in a host. The lesson is
narrower and more useful: **the spec text is the authority, not the popularity of
the code you are reading.** When a reference implementation and `ofxGPURender.h`
disagree, the header wins — and you should be able to name the line that settled
it.

---

## Sources

Everything here was derived from primary specifications and from reading real
plugins. Nothing was written from memory of an API. Where a claim could not be
checked against a running host it is graded in
[`host-resolve.md`](host-resolve.md) rather than stated
as fact.

### Primary specifications and SDKs

| Source | Version used | What it contributed |
| --- | --- | --- |
| [Academy Software Foundation `openfx`](https://github.com/AcademySoftwareFoundation/openfx) | tag `OFX_Release_1.5.1` | The headers themselves (`ofxCore.h`, `ofxImageEffect.h`, `ofxGPURender.h`, `ofxParam.h`), the C++ Support library (`Support/include`, `Support/Library`), and the packaging rules. Fetched automatically by CMake, or pointed at with `OFX_SOURCE_DIR`. |
| [OpenFX reference documentation](https://openfx.readthedocs.io/) | 1.5.1 | `ofxRendering` (render action, multi-thread safety), `ofxThreadSafety` (which actions are legal on render threads), `ofxPackaging` (`.ofx.bundle` layout, install paths, arch directory names). |
| [`ofxGPURender.h`](https://github.com/AcademySoftwareFoundation/openfx/blob/OFX_Release_1.5.1/include/ofxGPURender.h) | 1.5.1 | The authoritative wording of the CUDA stream contract — the "SHOULD NOT wait … SHOULD NOT call `cudaDeviceSynchronize()` at any time" clause that `../SKILL.md` §1 and the template's sync branch implement. CUDA/Metal/OpenCL render properties were introduced in OFX 1.5. |
| [`Support/Plugins/GPUGain`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Support/Plugins/GPUGain) | 1.5.1 | The canonical `OFX::ImageProcessor` GPU-dispatch pattern (CUDA/OpenCL/Metal/CPU) that the template's processor derives from. **Deliberately corrected:** it derives width from `bounds` and ignores `rowBytes`, which shears on padded scanlines. |
| [`Support/Plugins/Basic`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Support/Plugins/Basic), [`Examples/Invert`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Examples/Invert) | 1.5.1 | Parameter/descriptor idioms, and a raw-C (no Support library) plugin used to cross-check what the wrapper does for you. |
| [DaVinci Resolve OFX samples](https://github.com/illusionyy/DaVinci-Resolve-OpenFX-Samples) (public mirror of the SDK shipped in Resolve 18.5) | OFX-1.4 headers + Support | Resolve's own `GainPlugin`, `TemporalBlurPlugin`, `RandomFrameAccessPlugin` and `DissolveTransitionPlugin`, all with CUDA/OpenCL/Metal kernels. Confirms Resolve's GPU expectations and the multi-frame access model. The SDK is installed with Resolve at `Support/Developer/OpenFX/` (`C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX` on Windows, `/opt/resolve/Developer/OpenFX` on Linux) and can be passed as `OFX_SOURCE_DIR` for a fully offline build. |
| [Blackmagic Design forum](https://forum.blackmagicdesign.com/) | — | Confirms there is no separate "Resolve plugin SDK": OFX is the plugin route, and Resolve supports the OpenCL/CUDA extension to OFX. Used for the Resolve-specific rows in the host matrix (graded B). |
| [NVIDIA CUDA C++ Programming Guide](https://docs.nvidia.com/cuda/cuda-programming-guide/) / [Runtime API](https://docs.nvidia.com/cuda/cuda-runtime-api/) | 12.9 | Kernel launch, streams, `cudaPointerGetAttributes`, device/primary-context semantics, pitch and alignment behaviour, and the architecture list (§6 of `../SKILL.md`). |
| [NVIDIA CUDA apt repository](https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64) | 12.9 series | Source of the toolkit packages that `build.sh --bootstrap-cuda` unpacks without root. Itemised, with versions, in [Artifacts the build pulls in](#artifacts-the-build-pulls-in). |

### Artifacts the build pulls in

Everything below is obtained at build time and is **not** stored in this
repository. Versions are what upstream served when this was verified (November
2026); the bootstrap deliberately resolves the newest package in the `12.9`
series from the repository index instead of pinning exact versions, so a later
run may pick up a newer patch release.

| Artifact | Version | Obtained by | What it contributes |
| --- | --- | --- | --- |
| OpenFX source tree | git tag `OFX_Release_1.5.1`, shallow clone | `FetchContent_Declare` in `CMakeLists.txt` (skipped when `OFX_SOURCE_DIR` is set) | All OFX headers, plus the eight `Support/Library/*.cpp` translation units compiled into the plugin: `ofxsCore`, `ofxsImageEffect`, `ofxsInteract`, `ofxsLog`, `ofxsMultiThread`, `ofxsParams`, `ofxsProperty`, `ofxsPropertyValidation`. Fetched, never vendored. |
| `Packages.gz` | live index | `build.sh --bootstrap-cuda`, via `curl` | The package-to-filename map; this is what lets the script name the `.deb` files without hard-coding versions. |
| `cuda-nvcc-12-9` | 12.9.41-1 | bootstrap: `ar` + `tar` into `$OFX_CUDA_TOOLKIT_DIR/stage` | `nvcc`, `cudafe++`, `ptxas`, `nvlink`, `fatbinary`, `bin2c` — the compiler that actually builds `Kernel.cu`. |
| `cuda-nvvm-12-9` | 12.9.41-1 | bootstrap | `nvvm/libdevice/libdevice.10.bc` and the NVVM libraries nvcc uses for device code generation. |
| `cuda-cudart-12-9` | 12.9.37-1 | bootstrap | `libcudart.so.12` — the plugin's `NEEDED` entry (`cudart64_*.dll` on Windows). |
| `cuda-cudart-dev-12-9` | 12.9.37-1 | bootstrap | `include/cuda_runtime.h` and friends — the only CUDA header the template includes — plus the link-time `libcudart.so` stub. |
| `cuda-crt-12-9` | 12.9.41-1 | bootstrap | `include/crt/*.h` device-runtime headers nvcc needs for `<<<>>>` launches and vector types. |
| `CUDA::cudart` or `CUDA::cudart_static` | from the toolkit above | `find_package(CUDAToolkit REQUIRED)` plus `target_link_libraries` | The import target the module links. Default is the shared runtime (`NEEDED libcudart.so.12`); `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` swaps in `libcudart_static.a`, which ships in `cuda-cudart-dev`. |

**Nothing else is fetched.** There are no git submodules (`git submodule status`
is empty), no `.patch` files, no vendored third-party directory, and no package
manager is invoked — `pip`, `apt`, `vcpkg` and `conan` are never called by any
script here. The only third-party code compiled into the plugin is the OpenFX
Support library, fetched at configure time.

The bootstrap leaves three things under `$OFX_CUDA_TOOLKIT_DIR`: `debs/` (the
downloaded packages and the index), `stage/` (their unpacked contents) and a
`home` symlink to the extracted `cuda-12.9` tree. Delete that directory to
reclaim roughly 100 MB.

### Open-source implementations analysed

These were read for architecture and failure modes. **No code was copied from
them** — all are GPL-3.0 and this repository's template is BSD-3-Clause, so they
informed design decisions only (see [Licensing](#licensing-and-provenance)).

| Project | What it demonstrated | What this repo took from it |
| --- | --- | --- |
| [`gyroflow/gyroflow-plugins`](https://github.com/gyroflow/gyroflow-plugins) | Rust OFX plugin with FFI into a native sensor engine; probes host GPU support per render (`get_cuda_render_supported`, `get_metal_command_queue`, …); runs in Resolve and SCRATCH; installer/UX for host plugins. | The "probe the host, never sniff the host name" rule, and the multi-host support matrix. |
| [`gyroflow-plugins` issue #64](https://github.com/gyroflow/gyroflow-plugins/issues/64) | 32 Resolve crashes in 24 days; 31 attributable to the plugin releasing a host-owned `MTLCommandQueue`. | The ownership rule — never destroy a borrowed GPU object — and the "GPU lifetime bugs surface in host code" case study in `host-resolve.md` §8. |
| [`Dec18studios/Open-DRT-OFX`](https://github.com/Dec18studios/Open-DRT-OFX) | CMake build for Metal (macOS) + CUDA (Windows/Linux) with a clean host-wrapper / kernel split and a single transform definition reused by both backends; cross-platform CI packaging. | The one-pixel-model-two-backends rule and the CMake structure (bundle output directories, per-architecture folders). |
| [`MattGrdinic/OneGrade`](https://github.com/MattGrdinic/OneGrade) | Single-node Resolve grade: vendored OFX 1.4 Support, grouped parameter UI, `OneGradePipeline.h` as the single source of truth mirrored by `MetalKernel.mm` / `OpenCLKernel.cpp` / `CudaKernel.cu` with a CPU fallback. | Confirmation that vendored Support + one pipeline header + per-backend kernels is the durable shape; parameter grouping/paging idioms. |
| [`cnoellert/forge-colorx`](https://github.com/cnoellert/forge-colorx) | Runtime expression evaluation compiled once per render; host coverage across Flame, Resolve, Nuke and Natron; documents host-specific UI quirks. | Evidence for the host-quirk rows, and the "compile per render, evaluate per pixel" separation that the template's processor/launcher split follows. |
| [`rewired/signal-cult`](https://github.com/rewired/signal-cult) | A suite of CUDA/CPU OFX plugins for Resolve on Windows sharing one shell; per-plugin source tree with adapter/core separation; explicit CUDA 12.8 toolchain paths. | Multi-plugin-in-one-bundle and Windows CUDA build practice; the reason `../SKILL.md` calls out symbol visibility (two plugins, one process). |
| [`baldavenger/BaldavengerOFX`](https://github.com/baldavenger/BaldavengerOFX) | The plugin set Blackmagic engineers point developers at for Resolve: OFX written specifically for Resolve, nearly all with a CUDA kernel, several with OpenCL. | Reference for Resolve-specific behaviour (choice parameters, CUDA+OpenCL dispatch) and for what "works in Resolve" actually looks like. |

### Licensing and provenance

- The template source files are original code, each marked
  `SPDX-License-Identifier: BSD-3-Clause` in its header (matching the OpenFX
  headers and Support library it links). Note that the repository currently has
  **no top-level `LICENSE` file** — the SPDX headers are the operative statement.
- The GPL-3.0 projects above were consulted as documentation and engineering
  evidence. No source, table or shader text was copied from them, so nothing here
  inherits their licence.
- `templates/minimal-cuda-ofx` compiles the upstream OpenFX Support library
  sources directly into the plugin; those remain BSD-3-Clause and are fetched at
  configure time rather than vendored. Keep the copyright notice if you vendor
  them yourself.
- Verification used the NVIDIA CUDA toolkit packages described above, which are
  governed by NVIDIA's own licence agreement, not by this repository.

### Deliberately not included

Listed so nobody goes looking for them, or mistakes an incomplete dependency for
an oversight:

| Not used | Why |
| --- | --- |
| TensorRT, ONNX Runtime | `onnx-and-tensorrt.md` §9 describes the zero-copy integration pattern (device pointers, `IoBinding`, the CUDA EP compute stream), but the template implements no inference path. Adding either would put a multi-hundred-megabyte dependency into a starter template. |
| NVTX (`nvtx3/nvToolsExt.h`) | Appears only as a profiling snippet in `cuda-streams-and-memory.md` §5. The template neither includes nor links it. |
| OpenCL, Metal, OpenGL backends | `OFX::ImageProcessor` can dispatch to them, but only CUDA and CPU are implemented; the others are out of scope. |
| DaVinci Resolve scripting API, DCTL | Different extension mechanisms, not OFX. Useful alongside a plugin, not part of one. |
| pip CUDA wheels (`nvidia-cuda-nvcc-cu12`, `nvidia-cuda-runtime-cu12`) | Tried and rejected for the bootstrap: the nvcc wheel ships `ptxas` and NVVM but no `nvcc`/`cudafe++`/`nvlink`, so it cannot compile a translation unit. |
| The GPL reference plugins | Read as engineering evidence only; never vendored or copied — see [Licensing](#licensing-and-provenance). |

---

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
[`templates/minimal-cuda-ofx/src/Plugin.cpp`](../templates/minimal-cuda-ofx/src/Plugin.cpp)
read next to [`../SKILL.md`](../SKILL.md) §1.

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

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[D]** stated by official documentation
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[R]** stated by a reference project
- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
