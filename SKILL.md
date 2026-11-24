---
name: ofx-cuda-plugin-development
description: Build, review and debug CUDA-accelerated OpenFX plugins (DaVinci Resolve Studio, Nuke, SCRATCH, Natron) on Windows and Linux. Use when creating a new OFX plugin, adding a GPU render path to an existing one, chasing a black render / crash / memory bug in a host, or reviewing OFX+CUDA code for spec compliance.
---

# OFX + CUDA plugin development

Master reference and agent workflow for CUDA-accelerated OpenFX image effects
that render correctly in DaVinci Resolve Studio, Nuke, SCRATCH and Natron, on
Windows (MSVC + CUDA Toolkit) and Linux (GCC/Clang + CUDA Toolkit).

**Provenance.** **[V]** verified in this repository (compiled, measured or run);
**[I]** inferred from a header or documented behaviour, not demonstrated;
**[B]** external evidence, grade B in `HOST_COMPATIBILITY_MATRIX.md`; the
2026-11-22 Windows runs (CUDA field test and ONNX review) are archived in
[`FIELD_TEST_WINDOWS_CUDA.md`](FIELD_TEST_WINDOWS_CUDA.md). Unmarked statements
are design guidance that follows from the model, not measurements. §12 records
what that vocabulary currently covers, including the 2026-11-24 currency check.

Read this file first, then only the companion whose subject the task touches:

| Document | Read it when |
| --- | --- |
| [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) | Lifecycle, zero-copy, streams, pitch, RoD/RoI, colour/alpha, inference, profiling |
| [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) | A real build: the one command, the artifact contract, compile rules with `file:line` |
| [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) | Behaviour differs between hosts, or a host-specific workaround is needed |
| [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) | Before shipping: the questions, each with the sentence that settles it |
| [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) | A network in the plug-in: integration shapes, ORT API, provider packaging |
| [`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md) | Reading OFX code: tiers, patterns, anti-patterns by their tells |
| [`PATTERN_INDEX.md`](PATTERN_INDEX.md) | "I need to do X" / "it does Y": the pattern, the proving file, the symptom |
| [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) | Testing to ship it: eleven layers (L0–L10), goldens, sanitizers, CI |
| [`testkit/`](testkit/) | Running the artifact and fake-host layers without a host |
| [`templates/minimal-ofx/`](templates/minimal-ofx/) | The smallest host-loadable CPU plug-in |
| [`templates/minimal-cuda-ofx/`](templates/minimal-cuda-ofx/) | A known-good CUDA render path with a CPU fallback |

---

## 1. Ground rules

These are invariants, not style. Each one exists because violating it produces a
failure that is expensive to diagnose: black frames, silent corruption, or a host
crash with no stack trace.

1. **`kOfxImageEffectPropCudaEnabled` decides where the pixels are, not your
   descriptor.** `...CudaRenderSupported` only says what you *can* do; the
   per-render int says where the data actually is this call.
2. **Never dereference `kOfxImagePropData` on the CPU when CUDA is enabled.** It
   is a device pointer then; reading it faults or returns garbage.
3. **Never assume `rowBytes == width * bytesPerPixel`.** Address with the host's
   pitch, widened to a signed 64-bit offset. The pointer is the row at
   `bounds.y1` (the bottom) whatever the sign of the stride: rebase once onto a
   top-down view (`data + (h-1)*rowBytes`, negated stride) instead of scattering
   flips through the code. A **negative** stride is legal for CPU images only;
   `ofxImageEffect.h` excludes CUDA/Metal/OpenCL Buffers, and OpenCL Images have
   no row bytes at all.
4. **Honour the stream contract exactly.** Host stream present: enqueue on it and
   return without waiting. No stream: enqueue on your own (or the default) stream
   and **synchronise that stream before returning**. `cudaDeviceSynchronize()` is
   forbidden at any time — it blocks every stream on the device, deadlocks against
   hosts that submit from another thread, and destroys the host's pipelining.
5. **Never destroy, reset or free anything the host owns** — stream, command
   queue, context, or image buffers. You may only free what you allocated.
6. **Never switch devices without restoring the previous one.** RAII immediately
   after `cudaSetDevice`, including on every `throw` path.
7. **Zero-copy means zero *your* copies.** If the host gave device pointers, the
   kernel reads them directly. A `cudaMemcpy` into your own buffer is a bug, not a
   safety measure.
8. **One pixel model, two backends.** CPU and CUDA paths implement the same
   function; divergence is found by a client, not by you.
9. **RAII for every host image.** `clipGetImage`/`fetchImage` return owned
   handles; wrap them in `std::unique_ptr` on the line they are created.
10. **Throw specific OFX codes, never return silently.** `kOfxStatErrUnsupported`,
    `kOfxStatFailed`, `kOfxStatGPURenderFailed` (`1002`),
    `kOfxStatGPUOutOfMemory` (`1001`). A silent `return` is a black frame with no
    diagnostic anywhere.
11. **Nothing runtime-owned in a static with a destructor.** A host may unload and
    reload the module in one process. A function-local `static` whose destructor
    calls into CUDA, a thread pool or an inference runtime runs it at
    `DLL_PROCESS_DETACH` and breaks the **next** load. Allocate such state with
    `new` and never free it (`AI_MODEL_INTEGRATION.md` §4).
12. **Verify, then claim.** A plug-in that compiles is not a plug-in that renders.
    State exactly which hosts and GPU paths you exercised, in the `[V]`/`[I]`/`[B]`
    vocabulary.

---

## 2. Repository map

```
AGENTS.md                             router for agents: commands, read order, rules
SKILL.md                              this file: rules, workflow, constants, packaging
ARCHITECTURE_AND_CUDA_PIPELINE.md      lifecycle, streams, pitch, RoD/RoI, colour, inference
BUILD_PLAYBOOK.md                      clean-checkout build, artifact contract, compile rules
HOST_COMPATIBILITY_MATRIX.md           per-host behaviour, graded A/B/C
BEST_PRACTICES_AND_PERFORMANCE.md      cost model, reference-project lessons, its own S/M/D/R grades
PREFLIGHT_REVIEW_CHECKLIST.md          pre-ship questions, each citing the line that settles it
AI_MODEL_INTEGRATION.md                runtimes, languages, ORT, provider bundling, heads
REFERENCE_READING_GUIDE.md             reading OFX code: tiers, patterns, tells, reading plan
PATTERN_INDEX.md                       lookup by task, symptom and property; external repos
TESTING_STRATEGY.md                    L0–L10, goldens, negative controls, host acceptance
run-tests.sh                           docs check + every build + every suite
tools/check-docs.py                    documentation drift check (also in CI)
testkit/                               dlopen + fake-host harness, negative controls
templates/minimal-ofx/                 smallest loadable module; CMake, exports.map, build.sh
templates/minimal-cuda-ofx/            the CUDA layer split, bundle-producing build
templates/onnx-inference-ofx/          ORT session, packing, tiling, goldens, provider bundle
```

When generating a new plugin, copy a template's structure instead of re-deriving
it. `templates/minimal-cuda-ofx/` is the reference for the CUDA layer split and
the stream/pitch/device code; `templates/onnx-inference-ofx/` is the reference for
everything a model adds. For the cost model, the header quirks found by the
verification pass, and anti-patterns indexed by symptom, see
[`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md).

---

## 3. The layer contract

```
L4  Application      your effect maths (grade, keyer, model inference)
L3  Host wrapper     Plugin.cpp: RoD/RoI, isIdentity, params, dispatch, RAII
L2  Transfer         Kernel.cuh/.cu: launcher, grid, pitch, stream, errors
L1  Ingest           OFX image -> (device pointer | host pointer) + pitch + bounds
```

- L1 is the **only** place that reads image properties.
- L2 is the **only** place that names CUDA types. `Kernel.cuh` must compile
  without `cuda_runtime.h`.
- L3 never holds a CUDA type in a member; it passes `void*` for the stream.
- L4 is a pure function of `(pixel, params, x, y)` — no globals, no statics.

The template implements exactly this: `GainProcessor::processImagesCuda()` is
L1+L2 glue, `Kernel.cu` is L2, `GainPlugin` is L3, and the gain maths is written
twice (kernel and `multiThreadProcessImages`) because the CPU fallback must not
depend on CUDA.

---

## 4. Generation workflow

Each phase ends in a buildable state.

### Phase 0 — Decide the contract

Write down, before code:

- Plugin identifier: reverse-DNS plus name (`com.company.effectname`).
- Context: `filter` (`eContextFilter`) and/or `general`.
- Pixel formats: start with `eBitDepthFloat` + `ePixelComponentRGBA`.
- Spatial: does output(x,y) depend only on input(x,y)? This decides
  `kOfxImageEffectPropNoSpatialAwareness` (§8).
- Temporal: needs frames other than `time`?
- `isIdentity`: expressible? It is the cheapest optimisation you have.
- Declarations from OFX 1.5.1 that apply: CPU fallback
  (`kOfxImageEffectPropCPURenderSupported`), thumbnail behaviour
  (`kOfxImageEffectPropThumbnailRender`), colour management
  (`kOfxImageEffectPropColourManagementStyle`, §5).

If the effect needs neighbouring pixels, read §8 before writing the kernel: the
answer changes the architecture, not just the code.

### Phase 1 — Scaffold

| Effect | Start from | Why that one |
| --- | --- | --- |
| CPU-only pixel maths | `templates/minimal-ofx/` | smallest host-loadable module; no CUDA, no model |
| CUDA path with CPU fallback | `templates/minimal-cuda-ofx/` | stream contract, pitch arithmetic and `DeviceGuard` already correct |
| ONNX model per frame | `templates/onnx-inference-ofx/` | session, packing, tiling, goldens; CUDA EP first, CPU fallback (`AI_MODEL_INTEGRATION.md` §7) |
| Model whose output is not an image | `templates/onnx-inference-ofx/` for session and packaging only | `ModelRunner` requires out shape == in shape; the head decoder is yours |

Copy to `plugins/<youreffect>/`, then rename `PLUGIN_TARGET` and `PLUGIN_BUNDLE`
in `CMakeLists.txt`, and `kPluginIdentifier`, `kPluginName` and the parameter
names in `Plugin.cpp`.

### Phase 2 — Describe

`describe()` sets what the plugin can do; `describeInContext()` defines clips and
params. Keep them separate; fetch no image and read no param value in either.

Mandatory in `describe()`: supported contexts, bit depths, tiling and
multi-resolution flags, thread safety, GPU capability flags, and any 1.5.1
declaration from Phase 0. Mandatory in `describeInContext()`: the source clip
(`kOfxImageEffectSimpleSourceClipName`) and output clip
(`kOfxImageEffectOutputClipName`), each with
`addSupportedComponent(OFX::ePixelComponentRGBA)`.

### Phase 3 — Params

Every param needs `setLabels`, `setScriptName`, `setHint`, `setDefault`, and a
sensible range. `setDoubleType(OFX::eDoubleTypeScale)` makes a slider instead of a
number box. Group related params with `defineGroupParam` + `setParent`, and add
every param to a page with `page->addChild`.

`setRange` is the clamp; `setDisplayRange` is the slider. A grade that cannot
exceed 1.0 is a bug.

### Phase 4 — Host wrapper

```cpp
std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));  // owned
std::unique_ptr<OFX::Image> src(_srcClip->fetchImage(args.time));  // owned

processor.setDstImg(dst.get());      // borrowed
processor.setSrcImg(src.get());      // borrowed
processor.setGPURenderArgs(args);    // selects CUDA / CPU inside ImageProcessor
processor.setRenderWindow(args.renderWindow);
processor.process();
```

`setGPURenderArgs` is what makes `OFX::ImageProcessor` call `processImagesCuda()`
instead of `multiThreadProcessImages()`. Omitting it means the CUDA path never
runs and nobody notices, because the CPU path still produces a correct image.

### Phase 5 — Kernel

- Boundary check against the **image bounds**, not the render window.
- Pitch in bytes, signed 64-bit, applied through `char*` arithmetic.
- Base pointers already offset to the render window (host-side).
- `__restrict__` on both pointers; no atomics; no shared memory unless measured.
- `dim3(16,16)` to start; grid rounded up.
- `cudaGetLastError()` checked immediately after the launch.
- No `cudaDeviceSynchronize()` anywhere.

### Phase 6 — Build

Linux: `./build.sh` → `build/bundle/<Name>.ofx.bundle/Contents/Linux-x86-64/<Name>.ofx`
Windows: `.\build.bat` → `build\bundle\<Name>.ofx.bundle\Contents\Win64\<Name>.ofx`

Verify the artifact, not the intent:

```bash
find build/bundle -name '*.ofx' -type f
nm -D --defined-only <bundle>/Contents/Linux-x86-64/<Name>.ofx | grep ' T '
```

The requirement is **exactly two exported entry-point names**,
`OfxGetNumberOfPlugins` and `OfxGetPlugin`. Total dynamic-symbol count depends on
the build: `-fvisibility=hidden` alone still exports five weak `std::` helpers
(measured: 7 symbols **[V]**); the version script in
`templates/minimal-ofx/exports.map` removes those too, to 2. A count of 5 `T`
means `CUDA_VISIBILITY_PRESET` is missing (§6).

### Phase 7 — Install and smoke test

Copy the bundle into the host's plugin directory (§6), start the host, then:

1. Does the plugin appear under the expected group?
2. Apply it: does the image change as expected?
3. Set the neutral value: does `isIdentity` short-circuit the render?
4. Switch the host's GPU mode (CUDA ↔ CPU/OpenCL) and repeat.
5. Render to file, not just to the viewer — many bugs only appear in the render
   path.

### Phase 8 — Debug and profile

§7. The host-visible channel is `setPersistentMessage`, not `printf` and not
`OFX::Log::print` (which writes a file only in a `DEBUG` build).

---

## 5. OFX constant reference (verified against OFX 1.5.1 headers)

### 5.1 Capability properties (plugin descriptor)

All are `string × 1` except where noted. Every row was resolved against the pinned
1.5.1 tree **[V]**:

| Constant | Header | Notes |
| --- | --- | --- |
| `kOfxImageEffectPropCudaRenderSupported` | `ofxGPURender.h` | `"false"` default for a plugin |
| `kOfxImageEffectPropCudaStreamSupported` | `ofxGPURender.h` | `"true"` = you can use host streams; only then may the host set `...CudaStream` |
| `kOfxImageEffectPropOpenCLRenderSupported` | `ofxGPURender.h` | OpenCL **Buffers** render |
| `kOfxImageEffectPropOpenCLSupported` | `ofxGPURender.h` | OpenCL **Images** render; then read `kOfxImageEffectPropOpenCLImage`, not `kOfxImagePropData` |
| `kOfxImageEffectPropMetalRenderSupported` | `ofxGPURender.h` | macOS; has a third value `"needed"` |
| `kOfxImageEffectPropOpenGLRenderSupported` | `ofxGPURender.h` | **C string**; third value `"needed"`; may flip at `kOfxActionInstanceChanged` since v1.4 |
| `kOfxImageEffectPropCPURenderSupported` | `ofxGPURender.h` | **C string**, default `"true"`; added in **1.5.1** |

`kOfxImageEffectPropCPURenderSupported` is the modern way to say "GPU-only": set
`"false"` on the descriptor only when `render()` genuinely has no host-pointer
path. A host in OpenCL/Metal mode will then skip the effect instead of handing it
CPU images it cannot read — which is correct, and is why the templates, which do
have a fallback, leave the default alone. A host missing the property assumes the
default.

Four traps follow from this table:

- **The per-render flags are ints; these are strings.** `...CudaEnabled`,
  `...OpenCLEnabled`, `...MetalEnabled` are `int`; use `propGetInt` for those and
  `propGetString` here. Mixing them up returns `kOfxStatErrBadHandle` or garbage.
- **The OpenCL mapping is inverted in one doc block.** The property blocks are
  unambiguous (`...OpenCLRenderSupported` = Buffers, `...OpenCLSupported` =
  Images), but the `kOfxImageEffectPropOpenCLEnabled` block labels them the other
  way round. Trust the two property blocks, and discriminate at render time by
  testing whether `kOfxImageEffectPropOpenCLImage` is set.
- **`kOfxImagePropRowBytes` is not guaranteed for OpenCL Images** — "OpenCL
  Images do not have the concept of row bytes". Pitch arithmetic is a
  Buffers/CPU path rule.
- **Metal texture rendering is not in 1.5.1.** `kOfxImageEffectPropMetalTextureSupported`
  is an open proposal (`openfx#283`, Oct 2026), not a header constant. Do not
  reference it in shipped code.

### 5.2 Per-render in-args (Render / BeginSequenceRender / EndSequenceRender only)

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImageEffectPropCudaEnabled` | `int` | `1` = image data pointers are CUDA device pointers |
| `kOfxImageEffectPropCudaStream` | `pointer` | `cudaStream_t`; only set when both sides support streams |
| `kOfxImageEffectPropOpenCLEnabled` | `int` | OpenCL buffers/images in use |
| `kOfxImageEffectPropOpenCLCommandQueue` | `pointer` | `cl_command_queue` |
| `kOfxImageEffectPropMetalEnabled` | `int` | Metal buffers in use |
| `kOfxImageEffectPropMetalCommandQueue` | `pointer` | `id<MTLCommandQueue>` |
| `kOfxImageEffectPropNoSpatialAwareness` | `string` | **1.5.1**, optional: host asks for a render with no spatial effects (§8) |
| `kOfxImageEffectPropThumbnailRender` | `string` | **1.5.1**, optional: host considers this a UI thumbnail (§8) |

Both are read-only in-args with default `"false"`. `ThumbnailRender` is also
present in `GetRegionOfDefinition`, `GetRegionsOfInterest`, `GetFramesNeeded`,
`GetClipPreferences` and `IsIdentity`; `NoSpatialAwareness` is specified for
Begin/Render only. Read whichever the host offers where the answer changes the
action.

### 5.3 Image handle properties

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImagePropData` | `pointer` | base of the row at `bounds.y1` (the bottom; its origin is independent of the stride's sign); device pointer when CUDA/OpenCL/Metal enabled |
| `kOfxImagePropRowBytes` | `int` | scanline stride in bytes; **signed, may be negative for CPU images** |
| `kOfxImagePropBounds` | `int × 4` | window of valid pixels in this image |
| `kOfxImagePropRegionOfDefinition` | `int × 4` | RoD; may exceed bounds |
| `kOfxImageEffectPropPixelDepth` | `string` | `kOfxBitDepthByte` / `Short` / `Half` / `Float` |
| `kOfxImageEffectPropComponents` | `string` | `kOfxImageComponentRGBA` / `RGB` / `Alpha` |
| `kOfxImageEffectPropPreMultiplication` | `string` | premultiplied / unpremultiplied / opaque |
| `kOfxImagePropPixelAspectRatio` | `double` | non-square pixels are legal |

`kOfxImagePropRowBytes` is an `int` in the header and in
`OFX::Image::getRowBytes()`. Widen it to `long long` before multiplying by a row
index; keeping the arithmetic in `int` overflows on a large frame. Read the
accessor once per row and reuse it.

Writing back is the same arithmetic against the **destination** image, which has
its own base pointer, its own signed pitch and its own bounds, plus traps a copy
does not have. Short version in [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md)
§7, "Drawing into the frame: the overlay after the model"; full rules in
[`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) §13.

### 5.4 Support names for the depths

The strings above are the C API; the Support library spells them as an enum, and
the 8-bit case is the one every byte-capable plug-in gets wrong first:

| C value | Support name (`OFX::`) | Notes |
| --- | --- | --- |
| `kOfxBitDepthByte` | `eBitDepthUByte` | there is no `eBitDepthByte` |
| `kOfxBitDepthShort` | `eBitDepthUShort` | 16-bit integer |
| `kOfxBitDepthHalf` | `eBitDepthHalf` | |
| `kOfxBitDepthFloat` | `eBitDepthFloat` | what every template advertises |

Advertise a depth only when `render()` handles it. A host that hands over an
undeclared depth is not converting for you; the templates accept float only and
the harness proves that a byte image is refused.

### 5.5 Colour management (OFX 1.5)

`ofxColour.h` adds a colour-management contract that the pre-1.5 skill said did
not exist. The default is still `kOfxImageEffectColourManagementNone`, so legacy
behaviour remains valid — but it is now a **choice**, not a limitation.

| Constant | Set on | Meaning |
| --- | --- | --- |
| `kOfxImageEffectPropColourManagementStyle` | plugin descriptor, host descriptor, instance | `None` (default) / `Basic` / `Core` / `Full` / `OCIO`; the host picks the highest style both sides offer and records it on the instance |
| `kOfxImageClipPropColourspace` | clip instance | the colourspace the host actually supplied; cross-reference with `OfxColourspace_<clip>` when output matches an input |
| `kOfxImageEffectActionGetOutputColourspace` | action | where a colour-manipulating effect declares its output space |

Rules:

- **Do not opt in unless you convert through the host's config.** Declaring a
  style is a promise that you read `kOfxImageClipPropColourspace` and honour it.
  A gain that claims `Basic` and then ignores the property is worse than one that
  claims nothing.
- **If you stay at `None`, do not convert either.** The old rule still holds:
  never bake a transform you were not asked for, and expose an explicit
  "as-is / no conversion" default when an effect only makes sense in linear light.
- **Resolve 21 implements the 1.5 colour APIs** (**[B]**,
  [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) §2.9). Feature-
  detect the host's style; never infer it from the host name.

### 5.6 Named traps

- The clip property for the *unmapped* component string is
  `kOfxImageClipPropUnmappedComponents` (`ofxImageEffect.h`). There is no
  `kOfxImageEffectPropUnmappedComponents`.
- `kOfxStatGPUOutOfMemory` = `1001`, `kOfxStatGPURenderFailed` = `1002`, both in
  `ofxGPURender.h`, which `ofxsImageEffect.h` does **not** include. Include it
  explicitly.
- `getRegionOfDefinition` returns `bool` and takes an out-parameter:
  `virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments&,
  OfxRectD&)`. Return `true` only when you set the rect; `false` means "use the
  union of the input clips' RoD" (`ofxsImageEffect.h:1119-1127`).
- `kOfxImageEffectPropNoSpatialAwareness` on the **descriptor** means "this plugin
  *can* render without spatial awareness", not "this plugin has none". §8 has the
  semantics; getting the polarity wrong makes a blur advertise itself as a LUT.

---

## 6. Build and packaging standards

**Bundle layout.** Hosts scan `NAME.ofx.bundle/Contents/<ARCH>/NAME.ofx`, where
`ARCH` is `Win64`, `Linux-x86-64`, `Linux-aarch64` or `MacOS`. The file must end
in `.ofx` on every platform and the **stem must match the bundle name**:
`MinimalCudaGain.ofx.bundle/Contents/Win64/MinimalCudaGain.ofx`.

**What ships is part of the bundle.** The architecture directory decides whether
a host finds the module; the files beside it decide whether it works and whether
you may ship it. One list drives the copies, the dropdown labels and any generated
header — runtime under the loader's name, every model the UI offers, fonts,
tokenizer tables, and the **licence text for each third-party item** — so a model
cannot be offered and missing, and an unlisted file is removed and named in the
build log. Copy the licence ledger with `POST_BUILD` +
`${CMAKE_COMMAND} -E copy_if_different`, after the asset copy and before stale-file
cleanup, and make a missing ledger a `message(WARNING)` so a source tree without
it still builds. A bundle of AGPL weights that installs and renders without its
notice has shipped for eight builds in a field-tested plug-in **[B]**.

**The CUDA provider belongs in the same bundle.** One bundle that appends the
provider and falls back to the CPU, never a second "GPU build": the provider plus
cuDNN, cuBLAS/cuBLASLt, cuFFT and cudart, behind
`-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` and `-DOFXPLUGIN_CUDA_RUNTIME_DIRS`, with a
warning per missing file because a missing import fails as **error 126**, not as a
message. Two host-facing facts:

- **Preload only what the provider imports, and only where the host does not own
  that base name.** Imports resolve by base name, so an explicit load of your
  `cudnn64_9.dll` beside Resolve's creates a second module of the same name and
  mixes two minor versions in one process. Skip **cuFFTW** entirely: the provider
  never imports it and it fails 127 next to a newer host cuFFT. Compare the loaded
  module's *path*, not its name, and report `host CUDA stack` vs `bundled CUDA`.
- **`-DOFXPLUGIN_CUDA_FROM_HOST=ON` is the packaging answer for a host that brings
  its own CUDA**: provider and shared library only, no cuDNN/cuBLAS/cuFFT/cudart
  of ours to collide (2.8 GB → 1.1 GB in the reporter's bundle **[B]**). It needs a
  CUDA host; it does not replace the runtime guard on a full bundle.

  Neither fact is visible to a fake host, which has nothing loaded and therefore
  always uses the bundle's own DLLs. The reproduction is
  `tools/host_cuda_probe.cpp`; the detail and the measured evidence are
  [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7.
- **Check the provider's import table per ORT version.** `dumpbin /dependents` on
  the provider you ship, looking for `onnxruntime.dll`. ORT 1.20.1's provider does
  not import it, which is what makes the `onnxruntime-ofx.dll` sidecar work with
  all 30 files beside it **[B]**; a later provider that does import it needs the
  sidecar shape instead (`AI_MODEL_INTEGRATION.md` §2).

**One thread budget per node.** An inference plug-in may hold several sessions
inside a host that holds several instances of the node. Build every session
through one `MakeSessionOptions(threads)` helper, make the intra-op count a
parameter whose default is what the plug-in already did, remember the value each
session was created with, and print it in the status label in **both** provider
modes — the label is the only evidence the setting reached the session
(`--label-param` asserts it). It is a CPU-path control: measured on one RTX 3060
it changed nothing under CUDA **[B]** (`AI_MODEL_INTEGRATION.md` §7).

**Install locations.**

| Host | Windows | Linux |
| --- | --- | --- |
| Resolve / Nuke / SCRATCH / Natron | `%COMMONPROGRAMFILES%\OFX\Plugins` | `/usr/OFX/Plugins` |
| User-local | `OFX_PLUGIN_PATH` (semicolon-separated) | `OFX_PLUGIN_PATH` (colon-separated) |

**Firewall your symbols.** Set `CXX_VISIBILITY_PRESET hidden`,
`CUDA_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`. The CUDA one is
separate and easy to miss: `CXX_VISIBILITY_PRESET` does **not** apply to the `.cu`
translation unit, so every `extern "C"` launcher stays exported. Measured on this
template: without `CUDA_VISIBILITY_PRESET` 5 `T` symbols, with it 2 **[V]**; the
version script then removes the weak `std::` helpers as well.

Every plugin statically links its own Support library. With default visibility,
two plugins in one host process export identical mangled symbols and the loader
binds one plugin's `OFX::PropertySet` to the other's; the symptom is a crash that
appears only when a *second* plugin is installed.

Visibility is an ELF mechanism. Under MSVC both presets are no-ops and the export
set comes from the Support library's `OfxExport` (`__declspec(dllexport)`), which
still yields exactly two exported names, verified with `dumpbin /exports` on a
built Windows bundle **[B]** ([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3);
`exports.map` is guarded by `if(UNIX AND NOT APPLE)` for the same reason.

**CUDA runtime linkage.** Default to `CUDA::cudart` (shared): the host already has
a GPU stack. `cudart_static` removes the load-time dependency but gives each
plug-in its own runtime instance, and mixing the two in one process is undefined
(`ARCHITECTURE_AND_CUDA_PIPELINE.md` §10).

On **Windows with CUDA 13**, do not claim a `cudart64_*.dll` import without
checking: `dumpbin /dependents` on a built template lists KERNEL32, MSVCP140,
VCRUNTIME140 and the UCRT, no CUDA runtime DLL, while the device code is present
(seven cubins) and the module loads. The runtime resolves through the hybrid
runtime and the string in the image is `cudart_hybrid64.dll` **[B]**
([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3). The portable statement is "device
code present, runtime resolved at load"; the import list is what a host depends on.

**Architecture targets.** `CMAKE_CUDA_ARCHITECTURES` defaults to
`75;80;86;89;90` plus `100;120` when the toolkit is ≥ 12.8. A bare number emits
SASS **and** PTX, so the newest architecture still JITs on newer hardware. CUDA
13.0 removed offline compilation and library support below `sm_75` (Maxwell,
Pascal, Volta); Turing is the floor, and an nvcc deprecation warning means the
list is stale.

The CMake trap has two halves. `enable_language(CUDA)` initialises
`CMAKE_CUDA_ARCHITECTURES` itself, so a project default guarded by
`if(NOT DEFINED ...)` never runs. Capturing the variable before
`enable_language()` fixes only the first configure: the value is then cached, so
the second configure keeps nvcc's single architecture — measured: `-- CUDA
architectures: 75` and one `--generate-code`, where a clean configure had seven
**[B]** ([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3). The template records nvcc's
default once in an internal cache variable, only on a configure that saw no user
value, and treats a differing cached value as a user choice; a tree configured
before that marker existed is handled by reading the cache entry before
`enable_language()` and treating `UNINITIALIZED` as a `-D` value. Checked on CMake
3.31.6 and 4.4.5. A configure proves the selection only — the STATUS line and the
`--generate-code` flags. Whether the cubin loads is a host question, and whether
the kernel runs needs hardware.

**Do not bake the toolkit path into the bundle.** By default CMake records the
`cudart` directory in `RUNPATH`. The template sets `INSTALL_RPATH` with
`BUILD_WITH_INSTALL_RPATH ON`, so the `.ofx` resolves a `libcudart` beside it and
otherwise uses the host's runtime. The token is platform-specific: `$ORIGIN` for
ELF, `@loader_path` for dyld (`BUILD_PLAYBOOK.md` §7); the wrong one builds fine
and cannot resolve at load.

**MSVC.** The CUDA host compiler must use the same runtime library as the rest of
the binary; CMake propagates it. Do not hand-add `-Xcompiler /MD` to a Debug build
(it must be `/MDd`).

**Separable compilation.** Only when device functions cross `.cu` files; then also
set `CUDA_RESOLVE_DEVICE_SYMBOLS ON`.

---

## 7. Debugging quick reference

**You cannot see `stdout`.** A GUI host has no console; `printf` goes nowhere. Two
channels are easily confused:

- `OFX::Log::print()` writes a **file** (`OFX_PLUGIN_LOGFILE`, default
  `ofxPluginLog.txt`, relative to the host's working directory), and Support
  guards the `fopen` with `#ifdef DEBUG` (`Support/Library/ofxsLog.cpp:40-47`).
  CMake's `Debug` configuration does not define `DEBUG` and neither do these
  templates, so the call writes nothing unless you add `-DDEBUG`. Keep it as an
  opt-in trace, or write your own file logger into a temp directory.
- For a message the user sees in the timeline:
  `OFX::ImageEffect::setPersistentMessage(OFX::Message::eMessageError, id, msg)`
  (`Support/include/ofxsImageEffect.h:1067`). It needs message suite v2, which
  not every host offers, so pair it with a file log instead of replacing one
  silent channel with a missing-suite exception.

Every error path must report *and* throw a specific code. `ARCHITECTURE_AND_CUDA_PIPELINE.md`
§11 has the printf-format gotcha, the log content worth capturing, and the five
crash classes worth memorising.

**Close the host before you touch its plugin folder.** Windows keeps a loaded
`.ofx` mapped, so installing over it fails in a way that looks like a corrupt
bundle; on every platform the running host has already enumerated the old file.

**The host blacklists crashing plugins.** Resolve records a plugin that crashed or
hung and skips it on the next start **[B]**. After fixing a crash the plugin may
still not load. Clear it, with the host closed:

- Windows: `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
  (older builds: `OFXPluginCache.xml` under `%PROGRAMDATA%\Blackmagic Design\...`).
- Then Preferences → System → Video Plugins and re-enable the plugin.

**Symptom → likely cause.**

| Symptom | First thing to check |
| --- | --- |
| Plugin not in the effect list | Bundle folder name vs binary stem; wrong arch folder; missing entry points (`nm -D`) |
| Effect appears, output is black | Advertising CUDA without a CUDA path; not checking `CudaEnabled`; launch on the wrong device |
| Correct at 1×, broken at odd sizes | Pitch ignored (`width * 16` instead of `rowBytes`) |
| Crashes only with a second plugin installed | Default symbol visibility (§6) |
| Crashes on scrub, fine on render | Async work outliving the render action without a stream |
| Works in Edit page, fails in Color page | Different render window / RoI; see `HOST_COMPATIBILITY_MATRIX.md` |
| Loading the plugin kills the host at startup | Static initialiser doing CUDA work; a static `cudaMalloc` at load |
| GPU path gone, label says `cannot load cufftw64_11.dll (error 127)` | A preload of everything beside the module; skip cuFFTW and guard host-owned base names (§6) |
| Host crashes in its own `cudnn64_9.dll` after your plugin loads | A second module of the same base name; use the host's stack or `OFXPLUGIN_CUDA_FROM_HOST` |
| Second instance shows the first instance's value | State in a static/global instead of the instance (`TESTING_STRATEGY.md` §8) |
| A label says `CPU (8 threads)` but the setting changes nothing | The session was built before the parameter, or with a second options helper (§6) |

**Profiling.** Start the host *under* the profiler: Nsight Systems
(`--trace=cuda,nvtx,osrt`) for the timeline, Nsight Compute per kernel, and
`compute-sanitizer --tool memcheck` for the pitch bug you cannot see. Both need
`-lineinfo` (or `-G`, debugging only). Details in
`ARCHITECTURE_AND_CUDA_PIPELINE.md` §9.

**ABI failures.** Mixing OFX SDK versions is mostly harmless (properties are
string-keyed), but the `OfxPlugin` struct layout and the Support library's C++
class layouts are not. Never link two Support versions into one binary, and never
share C++ objects across a plugin boundary **[I]**.

---

## 8. Region of interest, and the 1.5.1 render hints

If output(x,y) needs input(x±r, y±r):

1. Leave `kOfxImageEffectPropNoSpatialAwareness` **unset** on the descriptor — for
   a neighbourhood effect that is the correct declaration ("I cannot render
   without spatial awareness"). Override `getRegionsOfInterest()` and expand the
   source RoI by `r` in *canonical* coordinates, multiplying by `args.renderScale`
   (see the deep dive).
2. Fetch the source with the expanded region and remember that the image's
   **bounds** may differ from the render window. The kernel's boundary check is
   against the image bounds.
3. Test two offset windows, in both pointer origins and both stride signs, and
   assert the overlay's *position and orientation*, not that bytes changed. A
   pass-through copy is orientation-agnostic; a box in the top half of the tile
   catches a mirror. `testkit`'s `setImageOrigin`/`setNegativePitch` build all four
   layouts ([`testkit/README.md`](testkit/README.md) says what each proves).

The two classic failures: adding a pixel radius directly to canonical coordinates
(at `renderScale = 0.5` you ask for twice the neighbourhood; at 2× too little, and
seams appear only in proxy mode), and expanding the RoI but addressing the kernel
relative to the render window rather than the fetched image bounds.

A tiled plug-in that draws has both bugs at once: the halo decides what the model
sees, the overlay's placement decides what the user sees. The drawing half is
[`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7 and
[`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) §13.

**`kOfxImageEffectPropNoSpatialAwareness` (1.5.1), the right way round.** Set it
`"true"` on the descriptor only if the effect is per-pixel *or* can disable its
spatial parameters on request. The host then sets the in-args property to `"true"`
for renders that must not move pixels (LUT generation, some scopes), and your
`render()`/`beginSequenceRender()` must honour that by disabling the spatial part.
If you leave it `"false"`, the host **bypasses** the effect for those renders —
correct for a blur, wrong for a gain, and invisible until a user generates a LUT.

**`kOfxImageEffectPropThumbnailRender` (1.5.1).** The host marks low-resolution
UI renders. For a model plug-in this is the difference between a responsive
timeline and a stall: return identity (or draw a badge) instead of running
inference on a 208×117 thumbnail, and disable temporal inputs in
`getFramesNeeded()` for the same render. Do not change the *result* for a
full-resolution render based on this flag.

**`kOfxImageEffectPropCPURenderSupported` (1.5.1)** belongs in the same decision:
a plug-in that can only run on the GPU says so once, instead of throwing from
`render()` on every CPU render a host attempts.

---

## 9. Anti-patterns

| Anti-pattern | Why it fails | Do instead |
| --- | --- | --- |
| `width * 4 * sizeof(float)` as the stride | Hosts pad rows (Resolve renders 208×117 thumbnails through 256-byte-aligned allocators) | `getRowBytes()`, signed 64-bit |
| `cudaDeviceSynchronize()` in `render` | Explicitly forbidden; serialises the host's whole GPU pipeline | Enqueue on the host stream; sync only the stream you used, only when none was given |
| `cudaMemcpy` device→device to a private buffer | Doubles bandwidth and defeats the GPU path | Read the host pointer directly |
| `cudaMalloc` in `render` | Alloc/free per frame destroys throughput and fragments the allocator | Allocate once per sequence/size, reuse, release in `endSequenceRender()` or `purgeCaches()` |
| Freeing device memory in the destructor | The host may destroy the instance while tearing down its GPU stack; a CUDA call there can fault | Keep the destructor free of CUDA calls; release in `endSequenceRender`/`purgeCaches` |
| Static CUDA state in globals | Multiple instances share it; instances render on different threads | Per-instance members, or a device-keyed cache with a mutex |
| Catching everything and returning `kOfxStatOK` | The host composites a wrong frame with no log | Map failures to specific `kOfxStat*` codes |
| `new` + manual `delete` around `fetchImage` | Any intervening `throw` leaks the host image | `std::unique_ptr` at construction |
| Omitting `setGPURenderArgs` | The CUDA path silently never runs | Always call it before `process()` |
| Different maths in CPU and GPU paths | Divergence found by a client | One header/function as source of truth |
| Testing only in the viewer | Viewer path ≠ render path | Render to file, and to the delivery codec |
| Drawing the overlay before tiling | The overlay enters the model's receptive field and changes with tile size | Draw after inference, once per frame |

---

## 10. Checklists

**Before the first build**

- [ ] Identifier is reverse-DNS and unique.
- [ ] Bundle folder name == binary stem == `<Name>.ofx`.
- [ ] Source and output clips defined with explicit supported components.
- [ ] `addSupportedBitDepth` matches what `render()` accepts.
- [ ] CUDA flags guarded by `#ifdef OFXPLUGIN_WITH_CUDA`.
- [ ] `Kernel.cuh` compiles without CUDA headers.
- [ ] CPU fallback implemented and reachable, or
      `kOfxImageEffectPropCPURenderSupported` set `"false"` on purpose.
- [ ] Spatial/thumbnail/colour-management declarations from Phase 0 are written
      down (§5, §8).
- [ ] Global accesses go through the host pitch, and the hot path uses one vector
      load/store per pixel where the pointers allow it (`./test/run-tests.sh`
      asserts this in PTX).

**Before calling it done**

- [ ] Compiles clean on the intended toolchain(s); warnings read, not ignored.
- [ ] Linux: `nm -D --defined-only | grep ' T '` shows only the two entry points;
      weak `W` helpers are gone too if the version script is used
      (`templates/minimal-ofx/exports.map`).
- [ ] Windows: `dumpbin /exports <Name>.ofx` shows the same two names and nothing
      else; `cuobjdump -lelf` lists the cubins when CUDA is on. Both are checked
      by `testkit/check-ofx-artifact.bat` with `--strict`, which finds
      `dumpbin`/`cuobjdump` through `vswhere` and the CUDA roots and fails when
      it cannot.
- [ ] `dlopen`/`LoadLibraryEx` succeeds and the plugin enumerates (id, version,
      mainEntry).
- [ ] Rendered in at least one real host, in both GPU and CPU modes.
- [ ] Identity path verified (no render when params are neutral).
- [ ] Non-square PAR and odd resolutions tested.
- [ ] `isEnabledCudaRender` false path tested (force OpenCL/CPU).
- [ ] No `cudaDeviceSynchronize` anywhere: `grep -rn cudaDeviceSynchronize src/`.
- [ ] Every error path logs and throws.
- [ ] `./test/run-tests.sh` passes: predicate, kernel equivalence, CPU loop,
      concurrency, PTX counts, bundle hygiene.
- [ ] Windows: `test\run-tests.bat` — the artifact check, `test/gpu-kernel-test.cu`
      on the target GPU (28 cases, the only check that executes real SASS) and
      the contract harness through `LoadLibraryExW`; the Linux equivalents come
      from `./test/run-tests.sh`, and neither replaces a host render.
- [ ] Clean under `SANITIZE=address,undefined`, `SANITIZE=thread` and
      `RUN_VALGRIND=1`; `./test/run-analysis.sh` reports nothing.
- [ ] Every `kOfx*` property is read with its declared type, and every pre-ship
      question in `PREFLIGHT_REVIEW_CHECKLIST.md` has an answer.
- [ ] Verification status written down honestly (§12).

---

## 11. Mandatory git workflow

Commit and push continuously — after a new file compiles, a working build, a
behaviour fix, a doc milestone:

```bash
git add <only the files in this change>
git commit -m "<type>(<scope>): <imperative summary>"
git push
```

One logical change per commit (if the subject needs "and", split it); never a
broken build on `main`; never build output or machine paths; write the body for
the next reader ("use rowBytes so padded thumbnails do not shear", not "fix
stride"); verify before pushing, not after. Types: `feat`, `fix`, `build`, `docs`,
`refactor`, `perf`, `test`, `chore`; scopes: `template`, `kernel`, `cmake`,
`skill`, `hosts`.

---

## 12. Verification status of this skill

**Verified on the authoring machine** **[V]** (Linux x86-64, Ubuntu 24.04,
GCC 13.3.0, CUDA 12.9.41 via `./build.sh --bootstrap-cuda`, CMake 3.31.6,
OpenFX `OFX_Release_1.5.1`):

- CUDA and `CPU_ONLY=1` builds both produce
  `Contents/Linux-x86-64/MinimalCudaGain.ofx` with no compiler warnings; the CUDA
  build links `libcudart.so.12`, and `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` drops it
  from `NEEDED`.
- The artifact exports the two entry points plus five weak `W` helpers (7 dynamic
  symbols without a version script), and contains the launcher, both kernels and a
  `.nv_fatbin` section.
- Kernel PTX (`-arch=sm_89`): one 16-byte load and one 16-byte store per pixel,
  14 registers, 0 spills, 0 stack. Both kernels ran over 3,240 **[V]** emulated
  grid launches with 0 mismatches; the hoisted CPU loop is bit-identical over
  1,464 pixel components **[V]**.
- `dlopen` returns one plug-in: `OfxImageEffectPluginAPI` v1,
  `com.ofxplugindeveloper.minimalcudagain` 1.0, `setHost` and `mainEntry` set.
- `./test/run-tests.sh`, ASan+UBSan, TSan and `RUN_VALGRIND=1` are clean;
  `test/run-analysis.sh` (cppcheck + shellcheck) is clean.
- Every `kOfx*` constant cited in §5 exists in the pinned headers, including the
  three 1.5.1 additions and `ofxColour.h`.

**Currency check, 2026-11-24** (external evidence, re-read before relying on it):

| Fact | Source | Label |
| --- | --- | --- |
| OFX 1.5.1 (2025-11-20) is still the current release; the docs build is dated 2026-10 | `AcademySoftwareFoundation/openfx` releases, `openfx.readthedocs.io` | [B] |
| Metal texture render and key modifiers are open proposals, not in 1.5.1 | `openfx` issues #283, #292 (Oct–Nov 2026) | [B] |
| CUDA 13.5 is the current toolkit; 13.x needs driver ≥ 580 and cannot compile below `sm_75` | CUDA Toolkit Release Notes 13.5, CUDA Compatibility | [B] |
| ONNX Runtime 1.32.0 (2026-11-13); CUDA packages target CUDA 13.0 + cuDNN 9 since 1.27 | ORT release notes, CUDA EP requirements table | [B] |
| DaVinci Resolve Studio 21.2 (2026-11-05) is current; 21.0.3 fixed "GPU support in OpenFX plugins" | Blackmagic forum release posts, 21.0.3 notes | [B] |
| Resolve 21 implements the OpenFX 1.5 colour-management APIs | DaVinci Resolve 21 New Features Guide | [B] |

**Still not verified (state this plainly in any downstream report):**

- **No GPU in this environment.** Nothing in `./run-tests.sh` executes SASS here:
  numerics are validated on the host by emulating the thread mapping over the real
  kernel source (`BEST_PRACTICES_AND_PERFORMANCE.md` §6.1). Device behaviour beyond
  the external run below — multi-GPU binding, real async ordering across many
  streams — remains unverified.
- **No host.** No Resolve, Nuke, SCRATCH or Natron is installed, so the plugin has
  never rendered a pixel, and every host-specific claim in
  `HOST_COMPATIBILITY_MATRIX.md` remains graded B/C rather than verified.
- **The 1.5.1 render hints are documented, not exercised.** No host here sets
  `kOfxImageEffectPropNoSpatialAwareness` or `kOfxImageEffectPropThumbnailRender`,
  and the templates do not yet read them; the rules in §8 follow from the header
  text **[I]**.
- **No colour-managed test.** The colour-management paragraph in §5.5 is read from
  `ofxColour.h` and Blackmagic's release note; no host and no config were
  exercised **[I]** for the plug-in side.

**Executed elsewhere, not here [B]** (2026-11-22 field test: Windows 11, RTX 3060
sm_86, CUDA 13.0 toolkit on a 13.5 driver, MSVC 19.44/19.50; logs and limits in
[`FIELD_TEST_WINDOWS_CUDA.md`](FIELD_TEST_WINDOWS_CUDA.md)):

- `.\build.bat` configures and builds; the result is
  `Contents\Win64\<Name>.ofx` with exactly two exports (`dumpbin /exports`) and
  seven cubins (`cuobjdump -lelf`). The contract harness, ported to
  `LoadLibraryExW`, drove the whole host protocol with 0 failures.
- The real kernels executed on the GPU: 28/28 bit-exact versus the host reference,
  including padded and negative pitches, misaligned bases, window offsets,
  zero-size windows and overrun guards.
- First throughput measurement: 1920×1080 RGBA32F, vec4 0.212 ms (~312 GB/s),
  scalar 0.222 ms (~300 GB/s) — bandwidth-bound, ~4% apart, which is why the PTX
  instruction count must not be read as a speed claim.

**Still not verified there:** no OFX host, so no pixel has been rendered by
Resolve, Nuke, SCRATCH or Natron; one GPU only; no Windows sanitizer or valgrind
pass; no Nsight capture; no multi-resolution performance work.
