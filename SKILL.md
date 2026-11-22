---
name: ofx-cuda-plugin-development
description: Build, review and debug CUDA-accelerated OpenFX plugins (DaVinci Resolve Studio, Nuke, SCRATCH, Natron) on Windows and Linux. Use when creating a new OFX plugin, adding a GPU render path to an existing one, chasing a black render / crash / memory bug in a host, or reviewing OFX+CUDA code for spec compliance.
---

# OFX + CUDA plugin development

Master reference and agent workflow for producing CUDA-accelerated OpenFX image
effects that render correctly in DaVinci Resolve Studio, Nuke, SCRATCH and
Natron, on Windows (MSVC + CUDA Toolkit) and Linux (GCC/Clang + CUDA Toolkit).

**Provenance.** **[V]** verified in this repository (compiled, measured or run);
**[I]** inferred from a header or documented behaviour, not demonstrated;
**[B]** external evidence, grade B in `HOST_COMPATIBILITY_MATRIX.md`; the
2026-11-22 Windows runs (CUDA field test and ONNX review) are archived in
[`FIELD_TEST_WINDOWS_CUDA.md`](FIELD_TEST_WINDOWS_CUDA.md). Unmarked statements
are design guidance that follows from the model, not measurements.

Read this file first. Then read the deep-dive companion documents when the task
touches their subject:

| Document | Read it when |
| --- | --- |
| [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) | Lifecycle, zero-copy, streams, memory layout, RoD/RoI, colour/alpha, TensorRT/ORT, profiling |
| [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) | Building for real: the one command, the measured artifact contract, every compile-level requirement with file:line evidence, and what remains unproven without a host |
| [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) | A behaviour differs between hosts, or a host-specific workaround is needed |
| [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) | Before shipping: the questions to answer, each with the header line or spec sentence that settles it |
| [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) | A neural network in the plug-in: integration shapes, language-runtime hazards, ONNX Runtime API, CUDA decision rules |
| [`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md) | New to the codebase: which implementations to trust, patterns to copy, anti-patterns by their tells, and the twelve questions to ask of any plug-in |
| [`PATTERN_INDEX.md`](PATTERN_INDEX.md) | "I need to do X": the pattern, the file that proves it, the symptom that means you got it wrong, which shipped AI repository to learn from, and which general-purpose suite to read for a mechanic |
| [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) | Testing a plug-in to ship it: eleven layers (L0–L10), goldens and negative controls, sanitizers, concurrency, host acceptance, failure injection, CI |
| [`testkit/`](testkit/) | Running the artifact and fake-host layers: `./run-tests.sh` loads each built `.ofx`, checks the struct, and drives describe/render without a host |
| [`templates/minimal-ofx/`](templates/minimal-ofx/) | Starting a new plug-in from the smallest scaffold a host will load, built and verified by one command |
| [`templates/minimal-cuda-ofx/`](templates/minimal-cuda-ofx/) | Starting a new plugin, or needing a known-good reference |

---

## 1. Ground rules

These are the invariants. Every one of them exists because violating it produces
a failure that is expensive to diagnose — black frames, silent corruption, or a
host crash with no stack trace. None of them is stylistic.

1. **`kOfxImageEffectPropCudaEnabled` decides the pixel location, not your
   descriptor.** The descriptor's `...CudaRenderSupported` string only says what
   you *can* do. At render time the in-args int tells you where the pixels
   actually are.
2. **Never dereference `kOfxImagePropData` on the CPU when CUDA is enabled.**
   It is a device pointer then. Reading it faults or returns garbage.
3. **Never assume `rowBytes == width * bytesPerPixel`.** Address with the
   host's pitch. It may be padded *and it may be negative* for flipped images.
   `kOfxImagePropData` is the row at `bounds.y1` (the bottom) whatever the sign
   of the stride: rebase a view once onto the top row (`data + (h-1)*rowBytes`,
   negated stride) instead of scattering flips through the code.
4. **`cudaDeviceSynchronize()` is forbidden, full stop.** Per the spec: *"the
   plug-in SHOULD NOT wait for final asynchronous operations to complete before
   returning from the render action, and SHOULD NOT call cudaDeviceSynchronize()
   at any time."* Synchronise the *stream you used* (`cudaStreamSynchronize`)
   and only when the host did not give you one.
5. **Stream present → return immediately. Stream absent → complete the work
   before returning.** Both branches are mandatory. A plugin that always returns
   immediately corrupts output in hosts that do not pass a stream.
6. **Never destroy, reset or free anything the host owns** — stream, command
   queue, context, or the image buffers. You may only free what you allocated.
7. **Never switch devices without restoring the previous one.** Use RAII
   immediately after `cudaSetDevice`.
8. **Zero-copy means zero *your* copies.** If the host gave you device pointers,
   the kernel reads them directly. A `cudaMemcpy` to your own buffer is a bug,
   not a safety measure.
9. **One pixel model, two backends.** The CPU path and the CUDA path must
   implement the same function. If they diverge you will not notice until a
   client compares a GPU render to a CPU render.
10. **RAII for every host resource.** `clipGetImage`/`fetchImage` return owned
    handles; wrap them in `std::unique_ptr` on the line they are created, so
    every `throw` releases them.
11. **Throw OFX status codes, don't return silently.** `kOfxStatErrUnsupported`,
    `kOfxStatFailed`, `kOfxStatGPURenderFailed` (`1002`),
    `kOfxStatGPUOutOfMemory` (`1001`). A silent `return` produces a black frame
    with no diagnostic anywhere.
12. **Verify, then claim.** A plugin that compiles is not a plugin that renders.
    State exactly which hosts and GPU paths you exercised.

---

## 2. Repository map

```
AGENTS.md                             router for agents: commands, read order, rules
SKILL.md                              this file
ARCHITECTURE_AND_CUDA_PIPELINE.md      the deep dive
BUILD_PLAYBOOK.md                      clean-checkout build, artifact contract, compile rules
HOST_COMPATIBILITY_MATRIX.md           per-host behaviour and workarounds
BEST_PRACTICES_AND_PERFORMANCE.md      performance rules, reference-project lessons, graded evidence
PREFLIGHT_REVIEW_CHECKLIST.md          pre-ship questions, each citing the line that settles it
AI_MODEL_INTEGRATION.md                model integration: runtimes, languages, when CUDA pays
REFERENCE_READING_GUIDE.md             reading OFX code: tiers, patterns, tells, reading plan
PATTERN_INDEX.md                       lookup by task, symptom, property; external repos
TESTING_STRATEGY.md                    testing layers, what each proves, host acceptance script
run-tests.sh                           docs check + build every template + every suite
tools/check-docs.py                    documentation drift check (also in CI)
testkit/                               dlopen + fake-host harness, negative controls
templates/minimal-ofx/
  CMakeLists.txt                       reference build: module, support sources, exports
  src/Plugin.cpp                       the smallest host-loadable plug-in
  exports.map                          version script: exactly two dynamic symbols
  build.sh / build.bat                 fetch headers, build, inspect the artifact
  test/run-tests.sh                    contract harness against the reference plug-in
templates/minimal-cuda-ofx/
  CMakeLists.txt                       bundle-producing build
  src/Plugin.h                         host wrapper declarations
  src/Plugin.cpp                       descriptor, params, dispatch, isIdentity
  src/Kernel.cuh                       CUDA-free interface header
  src/Kernel.cu                        vec4 + scalar kernels, device-guarded launcher
  test/run-tests.sh                    GPU-free test suite (equivalence, PTX, bundle)
  build.sh / build.bat                 portable out-of-source builds
templates/onnx-inference-ofx/
  build.sh / build.bat                 needs ONNXRUNTIME_ROOT; the Windows bundle
                                       carries bin\onnxruntime.dll as
                                       onnxruntime-ofx.dll next to the .ofx
```

The template is the reference implementation for this skill. When generating a
new plugin, copy its structure rather than re-deriving it; the file layout is
what makes the `OFXPLUGIN_WITH_CUDA` split possible.

For performance rules, the header quirks found by the verification pass (property
types that differ from their neighbours, names whose string values disagree) and
the anti-patterns indexed by symptom, see
[`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md).

---

## 3. The layer contract

```
L4  Application      your effect maths (grade, keyer, model inference)
L3  Host wrapper     Plugin.cpp: RoD/RoI, isIdentity, params, dispatch, RAII
L2  Transfer         Kernel.cuh/.cu: launcher, grid, pitch, stream, errors
L1  Ingest           OFX image -> (device pointer | host pointer) + pitch + bounds
```

Rules that keep the layers honest:

- L1 is the **only** place that reads image properties.
- L2 is the **only** place that names CUDA types. `Kernel.cuh` must stay
  includable by a compiler that has never seen `cuda_runtime.h`.
- L3 never holds a CUDA type in a class member; it passes `void*` for the stream.
- L4 is a pure function of `(pixel, params, x, y)` — no globals, no statics.

The template implements exactly this: `GainProcessor::processImagesCuda()` is L1+L2
glue, `Kernel.cu` is L2, `GainPlugin` is L3, and the gain maths is L4 written
twice (once in the kernel, once in `multiThreadProcessImages`) — deliberately,
because the CPU fallback must not depend on CUDA.

---

## 4. Generation workflow

Follow the phases in order. Each phase ends in a state you can build.

### Phase 0 — Decide the contract

Write down, in one paragraph, before any code:

- Plugin identifier (reverse-DNS + name, e.g. `com.company.effectname`).
- Contexts: `filter` (`eContextFilter`) and/or `general`.
- Pixel formats accepted (start with `eBitDepthFloat` + `ePixelComponentRGBA`).
- Whether the effect is spatially aware (does output(x,y) depend only on input(x,y)?).
- Whether it is temporal (needs frames other than `time`).
- Whether `isIdentity` is expressible (if yes, it is your cheapest optimisation).

If the effect needs neighbouring pixels (blur, edge detect), see §8 RoI before
writing the kernel — the answer changes the architecture, not just the code.

### Phase 1 — Scaffold

Pick the template that matches the effect. Copying the CUDA one for a CPU-only
or model plug-in drags in a toolkit dependency and a GPU dispatch the effect
never uses, and the first thing you then do is delete them.

| Effect | Start from | Why that one |
| --- | --- | --- |
| CPU-only pixel maths (grade, blur, keyer) | `templates/minimal-ofx/` | smallest host-loadable module, no CUDA, no model |
| CUDA render path with a CPU fallback | `templates/minimal-cuda-ofx/` | stream contract, pitch arithmetic and `DeviceGuard` already correct |
| ONNX model per frame | `templates/onnx-inference-ofx/` | ORT session, packing, tiling, golden fixtures |
| Model with a non-image output (detection, classification, embedding) | `templates/onnx-inference-ofx/` for the session and packaging shape only | `ModelRunner` requires out shape == in shape and `AiCore` unpacks an image; the head decoder is yours (`AI_MODEL_INTEGRATION.md` §7) |

```
templates/<chosen>/ -> plugins/<youreffect>/
```

Rename in `CMakeLists.txt`: `PLUGIN_TARGET`, `PLUGIN_BUNDLE`. Rename in
`Plugin.cpp`: `kPluginIdentifier`, `kPluginName`, parameter names.

### Phase 2 — Describe

`describe()` sets what the plugin can do. `describeInContext()` defines clips and
params. Keep them separate; never fetch an image or touch a param value here.

Mandatory in `describe()`: supported contexts, supported bit depths, tiling and
multi-resolution flags, thread safety, GPU capability flags. Mandatory in
`describeInContext()`: the source clip (`kOfxImageEffectSimpleSourceClipName`)
and output clip (`kOfxImageEffectOutputClipName`), with
`addSupportedComponent(OFX::ePixelComponentRGBA)` on both.

### Phase 3 — Params

Every param needs `setLabels`, `setScriptName`, `setHint`, `setDefault`, and a
sense of range. Use `setDoubleType(OFX::eDoubleTypeScale)` for multipliers so the
host renders a slider rather than a number box. Group related params with
`defineGroupParam` + `setParent`, and add every param to a page with
`page->addChild`.

No hard-coded UI limits that clamp data: `setRange` is the clamp, `setDisplayRange`
is the slider. A grade that cannot exceed 1.0 is a bug.

### Phase 4 — Host wrapper

```cpp
std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));  // owned
std::unique_ptr<OFX::Image> src(_srcClip->fetchImage(args.time));  // owned

processor.setDstImg(dst.get());      // borrowed
processor.setSrcImg(src.get());      // borrowed
processor.setGPURenderArgs(args);    // <- selects CUDA / CPU inside ImageProcessor
processor.setRenderWindow(args.renderWindow);
processor.process();
```

`setGPURenderArgs` is what makes `OFX::ImageProcessor` call `processImagesCuda()`
instead of `multiThreadProcessImages()`. Forgetting it means the CUDA path never
runs and nobody notices, because the CPU path still produces a correct image.

### Phase 5 — Kernel

Requirements for every kernel in this skill:

- Boundary check (`if (x >= width || y >= height) return;`).
- Pitch in **bytes**, signed 64-bit, applied via `char*` arithmetic.
- Base pointers already offset to the render window (done host-side).
- `__restrict__` on both pointers; no atomics; no shared memory unless measured.
- Launch config `dim3(16,16)` to start; grid rounded up.
- `cudaGetLastError()` checked immediately after the launch.
- No `cudaDeviceSynchronize()` anywhere.

### Phase 6 — Build

Linux: `./build.sh` → `build/bundle/<Name>.ofx.bundle/Contents/Linux-x86-64/<Name>.ofx`
Windows: `.\build.bat` → `build\bundle\<Name>.ofx.bundle\Contents\Win64\<Name>.ofx`

Before claiming success, check the binary is where the host will look:

```bash
find build/bundle -name '*.ofx' -type f      # on Linux/macOS
nm -D --defined-only <bundle>/Contents/Linux-x86-64/<Name>.ofx | grep Ofx
```

You want exactly two dynamic symbols, `OfxGetNumberOfPlugins` and `OfxGetPlugin`.
`-fvisibility=hidden` is not enough on its own: GCC still exports five weak
`std::` template helpers (measured: 7 symbols). Pin the export list with a
version script — [`templates/minimal-ofx/exports.map`](templates/minimal-ofx/exports.map)
— or the equivalent for your platform; `BUILD_PLAYBOOK.md` §3.5 has the numbers.

### Phase 7 — Install and smoke test

Copy the bundle into the host's plugin directory (§6). Start the host, then:

1. Does the plugin appear in the effect list under the expected group?
2. Apply it. Does the image change as expected?
3. Toggle the parameter to identity. Does `isIdentity` kick in (no re-render)?
4. Switch the host's GPU mode (CUDA ↔ CPU/OpenCL) and repeat.
5. Render to file, not just to the viewer — many bugs only appear in the
   render path.

### Phase 8 — Debug and profile

See §7. `OFX::Log::print` writes a *file* only when the translation unit was
compiled with `DEBUG` (neither CMake's `Debug` configuration nor these templates
define it); the host-visible channel is `setPersistentMessage` (§7). `stdout`
reaches nothing.

---

## 5. OFX constant reference (verified against OFX 1.5.1 headers)

GPU capability properties, set on the plugin descriptor. All are `string × 1`
except the OpenGL one, which the header declares as **`C string × 1`**. Every row
was resolved against the pinned 1.5.1 tree **[V]**:

| Constant | Header | Notes |
| --- | --- | --- |
| `kOfxImageEffectPropCudaRenderSupported` | `ofxGPURender.h` | `"false"` default for a plugin |
| `kOfxImageEffectPropCudaStreamSupported` | `ofxGPURender.h` | `"true"` = you can use host streams; only then may the host set `...CudaStream` |
| `kOfxImageEffectPropOpenCLRenderSupported` | `ofxGPURender.h` | OpenCL **Buffers** render (the header's own wording) |
| `kOfxImageEffectPropOpenCLSupported` | `ofxGPURender.h` | OpenCL **Images** render; then use `kOfxImageEffectPropOpenCLImage`, not `kOfxImagePropData` |
| `kOfxImageEffectPropMetalRenderSupported` | `ofxGPURender.h` | macOS |
| `kOfxImageEffectPropOpenGLRenderSupported` | `ofxGPURender.h` | **C string**; has a third value `"needed"`; may flip at `kOfxActionInstanceChanged` since v1.4 |

Two traps follow from that table:

- **None of these six is an int.** They are strings; the *per-render* flags below
  (`...CudaEnabled`, `...OpenCLEnabled`, `...MetalEnabled`) are ints. Use
  `propGetString` for this group and `propGetInt` for the next.
- **`kOfxImagePropRowBytes` is not guaranteed for OpenCL Images**: the header
  states row bytes need not be set, "since OpenCL Images do not have the concept
  of row bytes" - so pitch arithmetic applies to the Buffers path only.

Per-render in-args properties (valid only inside Render / BeginSequenceRender /
EndSequenceRender):

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImageEffectPropCudaEnabled` | `int` | `1` = image data pointers are CUDA device pointers |
| `kOfxImageEffectPropCudaStream` | `pointer` | `cudaStream_t`; only set when both sides support streams |
| `kOfxImageEffectPropOpenCLEnabled` | `int` | OpenCL buffers/images in use |
| `kOfxImageEffectPropOpenCLCommandQueue` | `pointer` | `cl_command_queue` |
| `kOfxImageEffectPropMetalEnabled` | `int` | Metal buffers in use |
| `kOfxImageEffectPropMetalCommandQueue` | `pointer` | `id<MTLCommandQueue>` |

Image handle properties:

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImagePropData` | `pointer` | base of the row at `bounds.y1` (the bottom; its origin is independent of the stride's sign); device pointer when CUDA/OpenCL/Metal enabled |
| `kOfxImagePropRowBytes` | `int` | scanline stride in bytes; **signed, may be negative** |
| `kOfxImagePropBounds` | `int × 4` | window of valid pixels in this image |
| `kOfxImagePropRegionOfDefinition` | `int × 4` | RoD; may exceed bounds |
| `kOfxImageEffectPropPixelDepth` | `string` | `kOfxBitDepthByte` / `Short` / `Half` / `Float` |
| `kOfxImageEffectPropComponents` | `string` | `kOfxImageComponentRGBA` / `RGB` / `Alpha` |
| `kOfxImageEffectPropPreMultiplication` | `string` | premultiplied / unpremultiplied |
| `kOfxImagePropPixelAspectRatio` | `double` | non-square pixels are legal |

`kOfxImagePropRowBytes` is an `int` in the header, and the Support accessor
(`OFX::Image::getRowBytes()`) returns `int` too. Widen it to `long long` before
multiplying by a row index, which is what the templates do; keeping the stride
and the offset arithmetic in `int` overflows on a large frame. Reading the
accessor once per row and reusing it is the other half of the same fix.

Clip property for the *unmapped* component string is
`kOfxImageClipPropUnmappedComponents` (`ofxImageEffect.h`). There is no
`kOfxImageEffectPropUnmappedComponents`; do not invent it.

Error codes worth knowing: `kOfxStatGPUOutOfMemory` = `1001`,
`kOfxStatGPURenderFailed` = `1002`. Both live in `ofxGPURender.h`, which is
**not** pulled in by `ofxsImageEffect.h` — include it explicitly or the compiler
will report them undeclared.

**Support names for the depths.** The strings above are the C API; the Support
library spells the depths as an enum, and the 8-bit case is the one every
byte-capable plug-in gets wrong first:

| C value | Support name (`OFX::`) | Notes |
| --- | --- | --- |
| `kOfxBitDepthByte` | `eBitDepthUByte` | there is no `eBitDepthByte` |
| `kOfxBitDepthShort` | `eBitDepthUShort` | 16-bit integer |
| `kOfxBitDepthHalf` | `eBitDepthHalf` | |
| `kOfxBitDepthFloat` | `eBitDepthFloat` | what every template advertises |

Advertise a depth only when `render()` handles it: the templates accept float
only, and a host that hands over a byte image the descriptor did not declare is
a crash or a black frame, not a conversion.

**`getRegionOfDefinition` returns `bool` and takes an out-parameter.** It is not
`OfxRectD getRegionOfDefinition(const Args&)`:
`virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments&, OfxRectD&)`.
Return `true` only when you set the rect; `false` means "use the default", the
union of the input clips' RoD (`ofxsImageEffect.h:1119-1127`).

---

## 6. Build and packaging standards

**Bundle layout.** Hosts scan `NAME.ofx.bundle/Contents/<ARCH>/NAME.ofx`, where
`ARCH` is `Win64`, `Linux-x86-64`, `Linux-aarch64` or `MacOS` (per the OFX
packaging spec). The file must end in `.ofx` on every platform, and the *stem must match
the bundle name*. A plugin named `MinimalCudaGain` must live at
`MinimalCudaGain.ofx.bundle/Contents/Win64/MinimalCudaGain.ofx`.

**Install locations.**

| Host | Windows | Linux |
| --- | --- | --- |
| Resolve / Nuke / SCRATCH / Natron | `%COMMONPROGRAMFILES%\OFX\Plugins` | `/usr/OFX/Plugins` |
| User-local | `OFX_PLUGIN_PATH` (semicolon-separated) | `OFX_PLUGIN_PATH` (colon-separated) |

**Firewall your symbols.** Set `CXX_VISIBILITY_PRESET hidden`,
`CUDA_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`. The CUDA one
is separate and easy to miss: `CXX_VISIBILITY_PRESET` does **not** apply to the
`.cu` translation unit, so every `extern "C"` launcher in `Kernel.cu` stays an
exported dynamic symbol. Measured on this template: without
`CUDA_VISIBILITY_PRESET` the built `.ofx` exported 5 `T` symbols, with it 2 **[V]**.
Every plugin statically links its own copy of the
Support library; with default visibility, two plugins in one host process export
identical mangled symbols and the loader binds one plugin's `OFX::PropertySet` to
the other's. The symptom is a crash that appears only when a *second* plugin is
installed. With hidden visibility `nm -D` shows two `T` symbols — that is correct,
and the launcher is still visible in the full symbol table (`nm`, lowercase `t`).

Visibility attributes are an ELF mechanism: under MSVC `CXX_VISIBILITY_PRESET`
and `CUDA_VISIBILITY_PRESET` are no-ops, and the Windows export set comes from
the Support library's `OfxExport`, which expands to `__declspec(dllexport)`.
That still yields exactly two exported names, verified with `dumpbin /exports`
on a built Windows bundle **[B]** ([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3);
the version script in
`templates/minimal-ofx/exports.map` is guarded by `if(UNIX AND NOT APPLE)` for
the same reason — on Windows there is nothing for it to do.

**CUDA runtime linkage.** Default to `CUDA::cudart` (shared): the host already
has a GPU stack. `cudart_static` removes the load-time dependency but gives each
plug-in its own runtime instance, and mixing the two in one process is undefined
— see `ARCHITECTURE_AND_CUDA_PIPELINE.md` §10.

On **Windows with CUDA 13**, do not claim a `cudart64_*.dll` import without
checking it: `dumpbin /dependents` on a built template lists only KERNEL32,
MSVCP140, VCRUNTIME140 and the UCRT, no CUDA runtime DLL, while the device code
is present (seven cubins) and the module loads. The runtime is resolved through
the hybrid runtime, and the string in the image is `cudart_hybrid64.dll` **[B]**
([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3).
The portable statement is "device code present, runtime resolved at load"; the
import list, not the intent, is what a host depends on.

**Architecture targets.** `CMAKE_CUDA_ARCHITECTURES` defaults to
`75;80;86;89;90` plus `100;120` when the toolkit is ≥ 12.8. A bare number emits
SASS **and** PTX, so the newest architecture still JITs on newer hardware. CUDA
13 removed offline compilation for Volta and older; do not target `sm_70` or
below with a modern toolkit — nvcc warns about deprecated targets, and that
warning means the list is stale.

One CMake trap, and it has two halves: `enable_language(CUDA)` initialises
`CMAKE_CUDA_ARCHITECTURES` itself with the compiler's default, so by the time a
project's own default block runs, `if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)` is
**always false** and the curated list silently never applies. Capturing the
variable before enabling the language fixes the **first** configure only: the
value is now in the cache, so a second configure over the same build tree sees
`if(NOT DEFINED …)` as false again and keeps nvcc's single architecture —
measured: the second configure printed `-- CUDA architectures: 75` and
generated one `--generate-code`, where a clean configure had seven **[B]**
([evidence](FIELD_TEST_WINDOWS_CUDA.md) §3). The
template therefore records nvcc's default once in an internal cache variable and
treats `CMAKE_CUDA_ARCHITECTURES` as a user choice only when it differs from
that default. Any project that ships a curated list needs the same trick; "it
built" after an incremental reconfigure is not evidence the list survived.

**Do not bake the toolkit path into the bundle.** By default CMake records the
directory it linked `cudart` from in `RUNPATH` — an absolute build-machine path
for a bootstrapped toolkit. The template sets `INSTALL_RPATH` with
`BUILD_WITH_INSTALL_RPATH ON`, so the `.ofx` resolves a `libcudart` placed beside
it and otherwise uses the host's own runtime. The token is platform-specific:
`$ORIGIN` for ELF, `@loader_path` for dyld (see `BUILD_PLAYBOOK.md` §7); using
the wrong one builds fine and cannot resolve at load.

**MSVC.** The CUDA host compiler must use the same runtime library as the rest of
the binary. CMake propagates this; do not hand-add `-Xcompiler /MD` to a Debug
build (it must be `/MDd` there).

**Separable compilation.** Only when device functions cross `.cu` files; then
also set `CUDA_RESOLVE_DEVICE_SYMBOLS ON`.

---

## 7. Debugging quick reference

**You cannot see `stdout`.** A GUI host has no console attached; `printf` goes
nowhere. Two channels are easily confused:

- `OFX::Log::print()` writes a **file** (`OFX_PLUGIN_LOGFILE`, default
  `ofxPluginLog.txt`, relative to the host's working directory), and Support
  guards the `fopen` with `#ifdef DEBUG` (`Support/Library/ofxsLog.cpp:40-47`).
  CMake's `Debug` configuration does **not** define `DEBUG` and neither do these
  templates, so the call writes nothing in any build unless you add `-DDEBUG`
  yourself. Keep it as an opt-in trace, or write your own file logger into a
  temp directory.
- For a message the user can see in the timeline, post it through the host's
  message suite: `OFX::ImageEffect::setPersistentMessage(OFX::Message::eMessageError,
  id, msg)` (`Support/include/ofxsImageEffect.h:1067`). It needs message suite
  v2, which not every host offers, so pair it with the file log rather than
  replacing one silent channel with another host-missing exception.

Every error path must report *and* throw a specific code.

**The host blacklists crashing plugins.** Resolve records a plugin that crashed
or hung and skips it on the next start **[B]**. After fixing a crash, the plugin may
still not load. Clear it:

- Windows: `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
  (older builds: `OFXPluginCache.xml` in `%PROGRAMDATA%\Blackmagic Design\...`).
- Then Preferences → System → Video Plugins and re-enable the plugin.

**Symptom → likely cause.**

| Symptom | First thing to check |
| --- | --- |
| Plugin not in the effect list | Bundle folder name vs binary name mismatch; wrong arch folder; missing entry points (`nm -D`) |
| Effect appears, output is black | Advertising CUDA without a CUDA path; not checking `CudaEnabled`; kernel launched on the wrong device |
| Output correct at 1× but broken at odd sizes | Pitch ignored (`width * 16` instead of `rowBytes`) |
| Crashes only with a second plugin installed | Default symbol visibility (§6) |
| Crashes on scrub, fine on render | Async work outliving the render action without a stream |
| Works in Edit page, fails in Color page | Different render window / RoI; see `HOST_COMPATIBILITY_MATRIX.md` |
| Loading the plugin kills the host at startup | Static initialiser doing CUDA work; a static `cudaMalloc` at load time |

**Profiling.** Nsight Systems attaches to the host process and shows your
kernel as a child of the host's render thread; Nsight Compute attaches per
kernel launch. Both need the plugin built with line info
(`-lineinfo`, or `-G` only for debugging since it disables most optimisation).
Because the host owns the process, start the host under the profiler rather than
attaching to a render farm. Details in `ARCHITECTURE_AND_CUDA_PIPELINE.md` §9.

**ABI failures.** Mixing a plugin built against one OFX SDK with a host that
expects another is mostly harmless (properties are string-keyed), but the
`OfxPlugin` struct layout and the C++ class layouts of the *Support library* are
not. Never link two different Support library versions into one binary, and never
share C++ objects across a plugin boundary **[I]**.

---

## 8. Region of interest, before you write the kernel

If output(x,y) needs input(x±r, y±r):

1. In `describe()`, keep spatial awareness on (do **not** set
   `kOfxImageEffectPropNoSpatialAwareness`).
2. Override `getRegionsOfInterest()` and expand the source clip's RoI by `r` in
   *canonical* coordinates (multiply by `args.renderScale` correctly — see the
   deep dive).
3. In `render()`, fetch the source with the expanded region, and remember the
   host may hand you an image whose *bounds* differ from the render window. The
   kernel's boundary check must be against the image bounds, not the window.

`getRegionsOfInterest` gets this wrong in almost every first draft, and the
failure mode is a visible seam at tile edges, which is easy to blame on the
kernel.

The tile protocol hides a second mistake: render two offset windows, in both
pointer origins and with both stride signs, and assert the overlay's *position
and orientation* - not that bytes changed. A pass-through copy is
orientation-agnostic, so it matches either way up; a box drawn in the top half of
the tile catches a mirror, a centred one does not. `testkit`'s
`setImageOrigin`/`setNegativePitch` build all four layouts, and
[`testkit/README.md`](testkit/README.md) says what each one can prove.

---

## 9. Anti-patterns

| Anti-pattern | Why it fails | Do instead |
| --- | --- | --- |
| `width * 4 * sizeof(float)` as the stride | Hosts pad rows (Resolve renders 208×117 thumbnails through 256-byte-aligned allocators) | `getRowBytes()`, signed 64-bit |
| `cudaDeviceSynchronize()` in `render` | Explicitly forbidden; serialises the host's whole GPU pipeline | Enqueue on the host stream; sync only when no stream was given |
| `cudaMemcpy` device→device to a private buffer | Doubles bandwidth and defeats the point of the OFX GPU path | Read the host pointer directly |
| `cudaMalloc` in `render` | Alloc/free per frame destroys throughput and fragments the caching allocator | Allocate in `beginSequenceRender`/lazily once, reuse, free in `endSequenceRender`/destructor |
| Static CUDA state in globals | Multiple instances share it; the host may render instances on different threads | Per-instance members, or a device-keyed cache with a mutex |
| Catching everything and returning `kOfxStatOK` | The host has no idea the frame is wrong; you get black output and no log | Map failures to the specific `kOfxStat*` code |
| `new` + manual `delete` around `fetchImage` | Any intervening `throw` leaks the host image | `std::unique_ptr` at construction |
| Omitting `setGPURenderArgs` | CUDA path silently never runs | Always call it before `process()` |
| Different maths in CPU and GPU paths | Divergence discovered by a client, not by you | One header/function as source of truth |
| Testing only in the viewer | Viewer path ≠ render path in every host | Render to file, and to the delivery codec |

---

## 10. Checklists

**Before the first build**

- [ ] Identifier is reverse-DNS and unique.
- [ ] Bundle folder name == binary stem == `<Name>.ofx`.
- [ ] Source and output clips defined with explicit supported components.
- [ ] `addSupportedBitDepth` matches what `render()` actually accepts.
- [ ] CUDA flags guarded by `#ifdef OFXPLUGIN_WITH_CUDA`.
- [ ] `Kernel.cuh` compiles without CUDA headers.
- [ ] CPU fallback implemented and reachable.
- [ ] Global accesses go through the host pitch, and the hot path uses one vector
      load/store per pixel where the pointers allow it (`./test/run-tests.sh` asserts
      this in PTX).

**Before calling it done**

- [ ] Compiles clean on the intended toolchain(s); warnings read, not ignored.
- [ ] Linux: `nm -D --defined-only | grep ' T '` shows only the two entry
      points. Weak `W` template helpers may remain; a version script removes
      those too (`templates/minimal-ofx/exports.map`).
- [ ] Windows: `dumpbin /exports <Name>.ofx` shows the same two names and
      nothing else; `cuobjdump -lelf` lists the cubins when CUDA is on. Both are
      checked by `testkit/check-ofx-artifact.bat`, which every `build.bat` calls
      with `--strict`: it locates `dumpbin`/`cuobjdump` through `vswhere` and the
      CUDA roots, because the Visual Studio generator branch has neither on
      `PATH`, and fails the build when it cannot find them.
- [ ] `dlopen`/`LoadLibraryEx` succeeds and the plugin enumerates (id, version,
      mainEntry).
- [ ] Rendered in at least one real host, in both GPU and CPU modes.
- [ ] Identity path verified (no render when params are neutral).
- [ ] Non-square PAR and odd resolutions tested.
- [ ] `isEnabledCudaRender` false path tested (force the host to OpenCL/CPU).
- [ ] No `cudaDeviceSynchronize` anywhere: `grep -rn cudaDeviceSynchronize src/`.
- [ ] Every error path logs and throws.
- [ ] `./test/run-tests.sh` passes: predicate, kernel equivalence, CPU loop,
      concurrency, PTX counts, bundle hygiene.
- [ ] Windows: `test/gpu-kernel-test.cu` run on the target GPU (`nvcc
      -arch=sm_XX`, 28 cases) — the only check that executes real SASS.
- [ ] It is also clean under `SANITIZE=address,undefined`, `SANITIZE=thread` and
      `RUN_VALGRIND=1`, and `./test/run-analysis.sh` reports nothing.
- [ ] Every `kOfx*` property you read is read with its declared type, and every
      pre-ship question in `PREFLIGHT_REVIEW_CHECKLIST.md` has an answer.
- [ ] Verification status written down honestly (see §12).

---

## 11. Mandatory git workflow

Commit and push continuously — after a new file compiles, after a working build,
after a behaviour fix, after a doc milestone:

```bash
git add <only the files in this change>
git commit -m "<type>(<scope>): <imperative summary>"
git push
```

One logical change per commit (if the subject needs "and", split it); never a
broken build on `main`; never build output or machine paths; write the body for
the next reader ("use rowBytes so padded thumbnails do not shear", not "fix
stride"); verify before pushing, not after. Types: `feat`, `fix`, `build`,
`docs`, `refactor`, `perf`, `test`, `chore`; scopes: `template`, `kernel`,
`cmake`, `skill`, `hosts`.

---

## 12. Verification status of this skill

Recorded so nobody mistakes documentation for proof.

**Verified on the authoring machine** **[V]** (Linux x86-64, Ubuntu 24.04,
GCC 13.3.0, CUDA 12.9.41 via `./build.sh --bootstrap-cuda`, CMake 3.31.6,
OpenFX `OFX_Release_1.5.1`):

- CUDA and `CPU_ONLY=1` builds both produce
  `Contents/Linux-x86-64/MinimalCudaGain.ofx` with no compiler warnings; the CUDA
  build links `libcudart.so.12`, and `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` drops it
  from `NEEDED`.
- The artifact exports exactly two strong `T` symbols plus five weak `W` helpers,
  and contains the launcher, both kernels and a `.nv_fatbin` section.
- Kernel PTX (`-arch=sm_89`): one 16-byte load and one 16-byte store per pixel,
  14 registers, 0 spills, 0 stack. Both kernels ran over 3,240 **[V]** emulated
  grid launches with 0 mismatches; the hoisted CPU loop is bit-identical.
- `dlopen` returns one plug-in: `OfxImageEffectPluginAPI` v1,
  `com.ofxplugindeveloper.minimalcudagain` 1.0, `setHost` and `mainEntry` set.
- `./test/run-tests.sh`, ASan+UBSan, TSan and `RUN_VALGRIND=1` are clean;
  `test/run-analysis.sh` (cppcheck + shellcheck) is clean.
- Every `kOfx*` constant cited in §5 exists in the pinned headers.

**Still not verified (state this plainly in any downstream report):**

- **No GPU in this environment.** Nothing in `./run-tests.sh` executes SASS here:
  numerics are validated on the host by emulating the thread mapping over the
  real kernel source (`BEST_PRACTICES_AND_PERFORMANCE.md` §6.1). Device behaviour
  beyond the single external run below — multi-GPU binding, real async ordering
  across many streams — remains unverified.
- **No host.** No Resolve, Nuke, SCRATCH or Natron is installed, so the plugin
  has never rendered a pixel, and every host-specific claim in
  `HOST_COMPATIBILITY_MATRIX.md` remains graded B/C rather than verified.
**Executed elsewhere, not here [B]** (2026-11-22 field test: Windows 11, RTX 3060
sm_86, CUDA 13.0 toolkit on a 13.5 driver, MSVC 19.44/19.50; the logs are
archived in [`FIELD_TEST_WINDOWS_CUDA.md`](FIELD_TEST_WINDOWS_CUDA.md)):

- `.\build.bat` configures and builds; the result is
  `Contents\Win64\<Name>.ofx` with exactly two exports (`dumpbin /exports`) and
  seven cubins (`cuobjdump -lelf`). The contract harness, ported to
  `LoadLibraryExW`, drove it through the whole host protocol with 0 failures
  (before this checkout's multi-instance checks existed).
- The real kernels executed on the GPU: 28/28 bit-exact versus the host
  reference, including padded and negative pitches, misaligned bases, window
  offsets, zero-size windows and overrun guards.
- First throughput measurement: 1920×1080 RGBA32F, vec4 0.212 ms (~312 GB/s),
  scalar 0.222 ms (~300 GB/s) — bandwidth-bound, ~4% apart, which is why the
  PTX instruction count must not be read as a speed claim.

**Still not verified:** no OFX host anywhere in that test either, so no pixel has
been rendered by Resolve, Nuke, SCRATCH or Natron; only one GPU was available;
no Windows sanitizer or valgrind pass; and no Nsight capture or multi-resolution
performance work.
