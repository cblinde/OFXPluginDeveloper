# Pattern index

A lookup table for "I need to do X inside an OFX plug-in — what is the proven
shape, where is the code, and what breaks first?" It is deliberately an **index**,
not a tutorial: every row points at code in this repository or at a project that
shipped the pattern, and the prose that explains *why* lives in the linked
documents.

Read it three ways:

- **By task** — [§1](#1-by-task-i-need-to) when you know what you are building.
- **By symptom** — [§2](#2-by-symptom-the-render-is-wrong) when something is
  already broken.
- **By property** — [§3](#3-by-property-which-constant-does-what) when you are
  staring at a `kOfx*` name and need to know who sets it.

For the reading order that teaches the object model, see
[`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md). For why a pattern is
correct, the citation in the row is the authority; this file only routes you.

**Rule for adding a row:** a pattern gets a *proven code* pointer only if the code
is in this repository and its tests cover it. External projects are evidence about
what shipped, graded **B**, and are never presented as verified here.

---

## 1. By task ("I need to…")

| I need to… | Pattern | Proven code here | First failure if done naively |
|---|---|---|---|
| Register one plug-in, or several from one module | One `OFX::Plugin::getPluginIDs`, factories with static storage; the Support library generates the two exports | `templates/minimal-cuda-ofx/src/Plugin.cpp` (`getPluginIDs`); `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp:12`–`:15` shows two factories in one module | Factories that die with the module; a bundle that does not enumerate |
| Know whether the pixels are device or host pointers | Read `kOfxImageEffectPropCudaEnabled` **at render time**; never infer it from what you advertised | `templates/minimal-cuda-ofx/src/Plugin.cpp:79` (throws if the CUDA path is entered without it) | Treating a host pointer as device memory → fault or garbage |
| Run on the host's stream | Two branches: stream present → return immediately; absent → finish the work | `templates/minimal-cuda-ofx/src/Plugin.cpp:120`–`:126` | Blocking the host's pipeline; or returning before the work is done |
| Ship a CPU fallback | Compile-time split (`#ifdef`), and advertise the capability **only** when the path exists | `templates/minimal-cuda-ofx/src/Plugin.cpp:308`; `templates/minimal-cuda-ofx/src/Kernel.cuh` stays CUDA-free | Advertising CUDA in a build that has no CUDA path → black frames |
| Read any image the host hands over | Base pointer + `(window − bounds)` offset + signed 64-bit `rowBytes` | `GainProcessor::processImagesCuda()` and `InferencePlugin::render()`, both in this repo | Shear on padded rows; flip on negative pitch; offset renders shift |
| Preserve alpha | Copy channel 3 through untouched; only touch RGB | `templates/onnx-inference-ofx/src/Plugin.cpp:50` (`CopyAlpha`), called at `:152` | A model with 3 channels silently destroys the matte |
| Be spatially aware for the host | Override `getRegionsOfInterest`, expand in canonical coordinates | `.ofx-ref/Support/Plugins/Basic/basic.cpp:334` (SDK sample) | A visible seam at tile edges that looks like a kernel bug |
| Tile internally | `PlanTiles` + `Expand(halo)` + a feathered `TileAccumulator`, halo ≥ receptive-field radius | `templates/onnx-inference-ofx/src/AiCore.h` (contract at `:83`–`:85`) and `templates/onnx-inference-ofx/src/AiCore.cpp` | Seams; the negative control at `templates/onnx-inference-ofx/test/ai_pipeline_test.cpp:273` catches it |
| Prove the plug-in works without a host application | A fake host: `fetchSuite` for the five mandatory suites, a prop-set per object, `mainEntry` per action | `testkit/fake_host.cpp`, driven by `testkit/contract_main.cpp` and run from both `test/*/run-tests.sh` | Only testing the maths — wrong status codes, missing declarations and crashes at describe/render stay invisible |
| Run an ONNX model per frame on CPU | One session behind a pimpl, explicit intra-op thread budget, pitch-correct NCHW packing | `templates/onnx-inference-ofx/src/ModelRunner.{h,cpp}`, `src/AiCore.{h,cpp}` | Default thread count × host render threads; wrong tensor layout |
| Run a big model, or a PyTorch one | Out-of-process worker with shared memory for frames and a socket/pipe for control | Architecture in [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §2; shipped examples in §1 there | Blocking `render()` on an inference that takes seconds |
| Process a clip as a job, not per frame | Stage the clip, run a background job with progress + cancel, read results from a disk cache | LTX plug-in's "Flame never waits on the model" design (B, see §4) | A viewer that hangs on every scrub |
| Offer two backends with fallback | Prefer the C runtime (ONNX), fall back to a Python subprocess, expose the choice | `houdini-ofx-plugins`' ONNX / Python / Auto switch (B, see §4) | A silent fallback that changes numerical results between machines |
| Reduce per-frame allocation | Reuse scratch buffers keyed by frame size | **Known gap**: the inference template allocates its pack buffer per call; the pattern is a `Scratch` object resized only when the size changes | Allocation churn in the render loop; breaks any `FullySafe` claim |
| Bundle a runtime next to the `.ofx` | Copy exactly the SONAME file the binary records, with `$ORIGIN` rpath | `templates/onnx-inference-ofx/CMakeLists.txt` (single-copy bundling) | Globbing `libfoo.so*` ships the symlink **and** the 34 MB target |
| Choose a model resolution from the input | A ladder on the larger input dimension, exposed as a parameter | `CorridorKey-Runtime`'s 512/1024/1536/2048 ladder (B, see §4) | One hard-coded resolution that is wrong for half the footage |
| Handle a temporal model | Chunk (`8n+1` frames), overlap, and blend with a dissolve or flow warp | LTX plug-in's chunk/overlap design (B, see §4) | Treating a time-axis model as a per-frame filter |
| Decide *not* to use a model | Classical kernel when the operation is local and analytic | `Hush-OpenNR` (B, see §4) | Shipping weights and a runtime for something a 20-line kernel does |

Full decision procedure for the model rows:
[`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §2 (shape) and §6 (CUDA worth
it or not).

---

## 2. By symptom ("the render is wrong")

The exhaustive symptom table is
[`SKILL.md`](SKILL.md) §7 for host/GPU failures and
[`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md) §10 for
performance ones. This is the short route to the likely row above.

| Symptom | Look first at | Likely row |
|---|---|---|
| Black frame, plug-in "works" | capability advertised without a path; `CudaEnabled` not checked | §1 rows 2 and 4 |
| Image shears at odd sizes, fine at 1× | pitch assumed tight | §1 row 5 |
| Image shifted on a crop / RoD render | base pointer used as the window origin | §1 row 5 |
| Seam or grid at tile edges | halo smaller than the receptive field | §1 row 8 |
| Crash only when a second plug-in is installed | symbol visibility (Support library symbols exported) | [`SKILL.md`](SKILL.md) §6 |
| Plug-in missing from the host's list | bundle name vs binary stem, wrong `Contents/<ARCH>/` | [`SKILL.md`](SKILL.md) §6 |
| Viewer hangs while scrubbing | synchronous inference inside `render()` | §1 rows 10 and 11 |
| Host dies on shutdown or on a bad frame | runtime called during finalisation; uncaught exception across the ABI | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §3 |
| Result is plausible but wrong (colour, range) | channel order, value range, colour space | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §5 |
| Same frame differs between machines | silent Python/CPU fallback, or default thread count | §1 rows 12 and 9 |

---

## 3. By property ("which constant does what")

Only the ones that decide correctness; the complete table with header line
numbers is [`SKILL.md`](SKILL.md) §5.

| Constant | Type | Who sets it | What it decides |
|---|---|---|---|
| `kOfxImageEffectPropCudaRenderSupported` | string | you, in `describe()` | Whether you *can* accept device pointers (`.ofx-ref/include/ofxGPURender.h:555`) |
| `kOfxImageEffectPropCudaEnabled` | int | the host, per render | Where the pixels are **now** (`ofxGPURender.h:576`) |
| `kOfxImageEffectPropCudaStream` | pointer | the host | Whether you may return early (`ofxGPURender.h:617`) |
| `kOfxImagePropRowBytes` | int | the host | Stride in bytes; signed, may be negative (`ofxImageEffect.h:1353`) |
| `kOfxImagePropData` | pointer | the host | Base pointer for the image **bounds**, not the window |
| `kOfxImageEffectPropSupportsTiles` | int, default 1 | you | `false` = "give me full RoD"; not "I tile internally" (`ofxImageEffect.h:857`) |
| `kOfxImageEffectPluginRenderThreadSafety` | string, default instance-safe | you | How many renders may overlap (`ofxImageEffect.h:629`) |
| `kOfxImageEffectPropPixelDepth` / `…Components` | string | the host / clips | Which format you must accept or reject loudly |
| `kOfxStatGPURenderFailed` / `kOfxStatGPUOutOfMemory` | status | you | The codes the host understands for GPU failure (in `ofxGPURender.h`, not pulled in by `ofxsImageEffect.h`) |

---

## 4. External AI / ONNX plug-ins: what exists, what to learn, what to avoid

All of these were read from their published documentation, not built here —
evidence grade **B**. Links go to the projects, not to mirrors.

| Repository | What it is | Runtime and shape | Licence | Learn | Avoid |
|---|---|---|---|---|---|
| [`CorridorKey-Runtime`](https://github.com/alexandremendoncaalvaro/CorridorKey-Runtime) | keying/matting for Resolve + Nuke | ORT TensorRT RTX EP (Windows), ORT CUDA EP (Linux, experimental), MLX (macOS), TorchScript for one model; **out-of-process runtime service** | check repo (model weights differ) | Session broker, resolution ladder, explicit tiling toggle, per-host branches that never regress each other | Assuming one EP covers all your users — its own spec lists four tracks |
| [`corridorkey_ofx`](https://github.com/gitcapoom/corridorkey_ofx) | same model, Resolve-only wrapper | PyTorch + `torch.compile`/Triton; Python backend, named pipe + `CreateFileMapping` shared memory | check repo | The minimal sidecar protocol: control on a pipe, pixels in shared memory | Windows-only IPC; a 4 GB-class dependency installed by an installer script |
| [`Flame_LTXAlphaGen_OFX`](https://github.com/allklier/Flame_LTXAlphaGen_OFX) (forks: `flamelogik`, `tbdpost`) | matting via LTX 2.5 | Python worker; **staged job + disk cache**, not per-frame | MPL-2.0 (fork) | "Flame never waits on the model": progress window, cancel, `MATTE MISSING` instead of a silent pass-through | Three diverging forks — pin a commit and identify upstream first |
| [`MoGe-nuke`](https://github.com/sumitchatterjee13/MoGe-nuke) | depth/normals in Nuke | Python daemon on localhost TCP, **synchronous per frame** | MIT | How to package a Triton-dependent model that cannot live in the host | The synchronous design: it is the reason the LTX plug-in moved to jobs |
| [`DaVinci-Resolve-DLSS5`](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) | neural denoise | NVIDIA NGX in-process, D3D12 textures | MIT (NGX binaries not included) | In-process vendor runtime done properly: format conversion, origin flip | Assuming NGX is redistributable — it is not |
| [`Hush-OpenNR`](https://github.com/amateurmenace/Hush-OpenNR) | denoise | none (classical NLM) | MIT | When not to use a network at all | — |
| [`rvmofx`](https://github.com/smunaut/rvmofx) | background removal (RVM) | **libtorch/TorchScript in-process**, models in `Contents/Resources/` | GPL-3.0 | Proof that in-process TorchScript *can* work in Resolve 18.1 | "use the non-cxx11 version" of libtorch; no install target (manual copy); GPL-3.0 is contagious for a distributed plug-in |
| [`mattclifford1/OFX-plugins`](https://github.com/mattclifford1/OFX-plugins) | libtorch examples (CPU/CUDA) | libtorch in-process | check repo | The cheapest possible libtorch starting point | Its own README records the dead end: "Resolve cannot load GPU library dependencies when running libtorch with CUDA"; libtorch must be on `PATH` |
| [`houdini-ofx-plugins`](https://github.com/zhangxiao6776/houdini-ofx-plugins) | 8 creative OFX plugs for Houdini Copernicus, incl. depth | **ORT C++ in-process with session caching**, Python subprocess backend, `Auto` fallback; CoreML on Apple Silicon | check repo | A real two-backend design; cross-verification tests between C++ and Python | ONNX Runtime's CoreML EP "partition fragmentation" — native CoreML wins on Apple |
| [`Nuke-ONNX`](https://github.com/aaronsmithtv/Nuke-ONNX) | ONNX inference for Nuke (**NDK, not OFX**) | ORT in-process, up to 10 inputs, output normalisation | MIT | What an ONNX node looks like when the host gives you a real SDK | Nothing transfers to OFX directly; it is a different API |

### What their own documentation says to avoid

1. **libtorch ABI mismatches are not optional detail.** `rvmofx` tells you to use
   the **non-cxx11** build "at least required for use with Davinci Resolve". If
   you ship libtorch, you are shipping a C++ ABI contract with the host.
2. **Do not rely on `PATH` for a runtime.** `mattclifford1/OFX-plugins` requires
   libtorch on `PATH`; that is the failure mode users report as "the plug-in
   disappeared". Bundle it next to the `.ofx` with `$ORIGIN` instead.
3. **libtorch + CUDA in-process is a known dead end in Resolve.** The same README
   states Resolve cannot load the GPU library dependencies. If you need CUDA
   torch, that is an argument for the sidecar shape, not for more rpath flags.
4. **ONNX Runtime's CoreML EP fragments the graph.** `houdini-ofx-plugins`
   reports partition fragmentation and uses native CoreML on Apple Silicon
   instead. One EP is not a portability strategy.
5. **A per-frame synchronous daemon blocks the host.** `MoGe-nuke` does it and
   the LTX plug-in explicitly does not ("Flame never waits on the model"); the
   fix is a job with progress and a cache, not a faster socket.
6. **Licences decide architecture.** `rvmofx` is GPL-3.0 and CorridorKey's weights
   are non-commercial; if you copy code from either into a distributed plug-in,
   the licence travels with it. Reading for patterns is fine; copying is not.
7. **Forks drift.** The LTX plug-in exists in at least three repositories. Pin a
   commit and name the upstream before you learn from one.

---

## 5. If you only need the decision, not the code

| Situation | Shape | Where the reasoning is |
|---|---|---|
| Model is ONNX, ops are supported by the C runtime you can ship | in-process | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §2, §4 |
| Model needs PyTorch code, custom ops, or a Python preprocessing stack | sidecar (shared memory + control socket) | §2, §3 |
| Inference takes longer than a frame budget | staged job + disk cache | §1 above, and the LTX design in §4 |
| Operation is local and analytic | no model at all | `Hush-OpenNR` in §4 |
| GPU is available but the host does not hand over device pointers | CPU, or accept one copy per frame and measure | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §6 |
