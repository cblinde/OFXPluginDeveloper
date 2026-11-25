---
name: ofx-cuda-plugin-development
description: Build, review and debug CUDA-accelerated OpenFX (OFX) plugins for DaVinci Resolve Studio, Nuke, SCRATCH and Natron on Windows and Linux. Use when creating a new OFX plugin, adding a CUDA or GPU render path to an existing one, diagnosing a black frame, mirrored image, shear, seam or host crash, fixing pitch/stride or rowBytes arithmetic, packaging a .ofx bundle (Contents/<arch>, RPATH, exports), putting an ONNX model or TensorRT engine inside a plug-in, or reviewing OFX+CUDA code against the 1.5.1 spec. Entry point routes to references/ for constants, streams, pixels, RoI, colour, host quirks, CMake, packaging, debugging, performance, ONNX, testing and checklists.
---

# OFX + CUDA plugin development

Master reference and agent workflow for CUDA-accelerated OpenFX image effects
that render correctly in DaVinci Resolve Studio, Nuke, SCRATCH and Natron, on
Windows (MSVC + CUDA Toolkit) and Linux (GCC/Clang + CUDA Toolkit).

**Provenance.** **[V]** verified in this repository (compiled, measured or run);
**[I]** inferred from a header or documented behaviour, not demonstrated;
**[B]** external evidence, grade B in `references/host-resolve.md`; the
2026-11-22 Windows runs (CUDA field test and ONNX review) are archived in
[`field-evidence.md`](references/field-evidence.md). Unmarked statements
are design guidance that follows from the model, not measurements. §5 records
what that vocabulary currently covers, including the 2026-11-24 currency check.

Read this file first, then only the companion whose subject the task touches:

This file is the entry point and stays short on purpose. The detail lives in
`references/`, one subject per file, and the table in §2 is the index. Load the
one file the task needs; load two only when the task genuinely spans them.

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
   no row bytes at all. Handle it where it can occur and do not build a device
   path around it: a CUDA buffer with a negative stride is a host defect, not an
   input to support.
4. **Honour the stream contract exactly.** Host stream present: enqueue on it and
   return without waiting — the spec says the plug-in SHOULD NOT wait for
   completion and SHOULD NOT call `cudaDeviceSynchronize()` at any time. No
   stream: enqueue on your own (or the default) stream and **synchronise that
   stream before returning**, because the host may read the pixels the moment
   `render()` returns. `cudaDeviceSynchronize()` is the wrong tool in both modes:
   it blocks every stream on the device, deadlocks against hosts that submit from
   another thread, and destroys the host's pipelining. It is a SHOULD NOT, not a
   prohibition — if you conclude a case needs it, name the property the host does
   not set, say why a stream sync cannot replace it, and write that down beside
   the call.
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
    `new` and never free it (`references/onnx-and-tensorrt.md` §1).
12. **Verify, then claim.** A plug-in that compiles is not a plug-in that renders.
    State exactly which hosts and GPU paths you exercised, in the `[V]`/`[I]`/`[B]`
    vocabulary.

---

---

## 2. Routing: which file to load

Load **one** row's file, not the set. Every file is self-contained; the paths are
relative to this file.

| Task in front of you | Load | Do not load |
| --- | --- | --- |
| A property string, a depth/support name, "which constant does X" | `references/ofx-api-constants.md` | host quirks, performance |
| Plugin/instance/render lifecycle, what may block, thread safety | `references/ofx-lifecycle-and-threads.md` | colour, packaging |
| The stream contract, `cudaDeviceSynchronize`, zero-copy, who owns a GPU object | `references/cuda-streams-and-memory.md` | pitch arithmetic |
| Whether Resolve can be driven headlessly, and what its scripting API does not expose | `references/resolve-automation.md` | the prototypes, until a machine has Studio |
| `rowBytes`, stride, padded rows, depth matrix, drawing into the frame | `references/pixel-formats-and-pitch.md` | RoD/RoI |
| RoD, RoI, tiles, `kOfxImageEffectPropNoSpatialAwareness`, render hints | `references/roi-tiling-and-render-hints.md` | colour |
| Colour management, premultiplication, where alpha lands | `references/colour-and-alpha.md` | constants |
| Resolve: pages, GPU switch, crashing-plug-in blacklisting, Free vs Studio | `references/host-resolve.md` | Nuke/SCRATCH/Natron |
| Nuke, SCRATCH, Natron: no GPU buffer path, eager RoD, headless renderer | `references/host-nuke-scratch-natron.md` | Resolve |
| Capability probing instead of host sniffing; the host acceptance run | `references/host-detection-and-acceptance.md` | per-host tables |
| CMake, link flags, `-fPIC`, visibility, the artifact contract, toolchain | `references/cmake-and-linking.md` | packaging |
| Bundle layout, `Contents/<arch>`, RPATH, licence texts, install | `references/packaging-and-install.md` | CMake internals |
| A symptom: black frame, crash, seam, mirror, slow render | `references/debugging-and-symptoms.md` | the design docs |
| "Is this fast enough", the cost model, measured numbers | `references/performance-model.md` | kernel syntax |
| ONNX Runtime or TensorRT inside the plug-in: exact API names, tensors | `references/onnx-and-tensorrt.md` | which architecture |
| Whether to use a model at all, per-language runtimes, CUDA worth it | `references/ai-architectures-and-runtimes.md` | ORT calls |
| "I need to do X" as a task list, with the proving file in this repo | `references/pattern-index.md` | reading plan |
| Testing to ship: layers L0-L10, goldens, sanitizers, CI, what runs here | `references/testing-layers.md` | host behaviour |
| What was verified, by which command, and what was not | `references/verification-status.md` | design guidance |
| A new ONNX Runtime version: what its header and provider must have, what changed when | `references/onnx-and-tensorrt.md` §7, `tools/check-runtime.py` | model choice |
| Improvements that are known, worth doing and not done, with their owner | `references/improvement-backlog.md` | reading order |
| The archived Windows GPU runs behind every external claim | `references/field-evidence.md` | current guidance |
| What other shipped plug-ins did, and the seven-step reading plan | `references/reference-projects.md` | this repo's own code |
| Pre-flight review, sign-off, the questions with their settling sentence | `references/checklists.md` | tutorials |
| Run it: artifact + contract with no host, then a headless host render | `testkit/README.md` | prose |
| Copy a working scaffold | `templates/minimal-ofx/` (CPU), `templates/minimal-cuda-ofx/` (CUDA + fallback), `templates/onnx-inference-ofx/` (model per frame) | the docs |

Two rules for this table. If the task is "make it work at all", it is §3 below,
not a reference file. If two rows seem to apply, the narrower one is the one to
load first; the other is usually a consequence.

---

## 3. Generation workflow

Each phase ends in a buildable state.

### Phase 0 — Decide the contract

Write down, before code:

- Plugin identifier: reverse-DNS plus name (`com.company.effectname`).
- Context: `filter` (`eContextFilter`) and/or `general`.
- Pixel formats: start with `eBitDepthFloat` + `ePixelComponentRGBA`.
- Spatial: does output(x,y) depend only on input(x,y)? This decides
  `kOfxImageEffectPropNoSpatialAwareness`
  (`references/roi-tiling-and-render-hints.md` §2).
- Temporal: needs frames other than `time`?
- `isIdentity`: expressible? It is the cheapest optimisation you have.
- Declarations from OFX 1.5.1 that apply: CPU fallback
  (`kOfxImageEffectPropCPURenderSupported`), thumbnail behaviour
  (`kOfxImageEffectPropThumbnailRender`), colour management
  (`kOfxImageEffectPropColourManagementStyle`,
  `references/colour-and-alpha.md`).

If the effect needs neighbouring pixels, read
`references/roi-tiling-and-render-hints.md` §2 before writing the kernel: the
answer changes the architecture, not just the code.

### Phase 1 — Scaffold

| Effect | Start from | Why that one |
| --- | --- | --- |
| CPU-only pixel maths | `templates/minimal-ofx/` | smallest host-loadable module; no CUDA, no model |
| CUDA path with CPU fallback | `templates/minimal-cuda-ofx/` | stream contract, pitch arithmetic and `DeviceGuard` already correct |
| ONNX model per frame | `templates/onnx-inference-ofx/` | session, packing, tiling, goldens; CUDA EP first, CPU fallback (`references/onnx-and-tensorrt.md` §4) |
| Model whose output is not an image | `templates/onnx-inference-ofx/` for session and packaging only | `ModelRunner` requires out shape == in shape; the head decoder is yours |

When generating a new plugin, copy a template's structure instead of re-deriving
it. `templates/minimal-cuda-ofx/` is the reference for the CUDA layer split and
the stream/pitch/device code; `templates/onnx-inference-ofx/` is the reference for
everything a model adds.

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
- No `cudaDeviceSynchronize()` in the kernel; the host wrapper synchronises the
  stream it used (rule 4).

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
means `CUDA_VISIBILITY_PRESET` is missing (`references/cmake-and-linking.md` §3.5).

### Phase 7 — Install and smoke test

Copy the bundle into the host's plugin directory (`references/packaging-and-install.md`
§1), start the host, then:

1. Does the plugin appear under the expected group?
2. Apply it: does the image change as expected?
3. Set the neutral value: does `isIdentity` short-circuit the render?
4. Switch the host's GPU mode (CUDA ↔ CPU/OpenCL) and repeat.
5. Render to file, not just to the viewer — many bugs only appear in the render
   path.

### Phase 8 — Debug and profile

`references/debugging-and-symptoms.md` §1. The host-visible channel is
`setPersistentMessage`, not `printf` and not
`OFX::Log::print` (which writes a file only in a `DEBUG` build).

---

---

## 4. Mandatory git workflow

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

---

## 5. Verification status of this skill

The full ledger - what is **V**erified, by which command, and what is not - is
`references/verification-status.md`. The short version, so it is not skipped:

- Compiled and run here: the reference and CUDA templates, the contract harness,
  the GPU kernel harness on an RTX 3060 (bit-exact), `compute-sanitizer` clean.
- Rendered by a real host: one headless `NatronRenderer` frame, pixels checked.
- Not verified: Resolve, Nuke and SCRATCH; a CUDA execution provider; Linux; a
  second GPU; multi-GPU and MIG.

State which of those you exercised whenever you report a result.
