# Pattern index

A lookup table for "I need to do X inside an OFX plug-in — what is the proven
shape, where is the code, and what breaks first?" It is deliberately an **index**,
not a tutorial: every row points at code in this repository or at a project that
shipped the pattern, and the prose that explains *why* lives in the linked
documents.

Read it four ways:

- **By task** — [§1](#1-by-task-i-need-to) when you know what you are building.
- **By symptom** — [§2](#2-by-symptom-the-render-is-wrong) when something is
  already broken.
- **By property** — [§3](#3-by-property-which-constant-does-what) when you are
  staring at a `kOfx*` name and need to know who sets it.
- **By repository** — [§5](#5-the-general-purpose-suites-what-to-read-what-to-skip)
  when you would rather read shipping code than prose.

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
| Register one plug-in, or several from one module | One `OFX::Plugin::getPluginIDs`, factories with static storage; the Support library generates the two exports | `templates/minimal-ofx/src/Plugin.cpp` (the whole hook, 3 lines); `templates/minimal-cuda-ofx/src/Plugin.cpp` (`getPluginIDs`); `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp:12`–`:15` shows two factories in one module | Factories that die with the module; a bundle that does not enumerate |
| Produce a `.ofx` a host will actually load | Module named `<Name>.ofx`, the 8 Support TUs compiled in, `-fPIC`, hidden visibility **plus** a version script exporting only the two entry points | `templates/minimal-ofx/build.sh` (builds and then asserts the artifact: two dynamic symbols, no `Ofx*` undefined, no build-path RUNPATH, `dlopen` + `OfxGetPlugin`) | `-fvisibility=hidden` alone still exports the weak `std::` helpers, and without it hundreds of symbols — measured in `BUILD_PLAYBOOK.md` §3.5 |
| Know whether the pixels are device or host pointers | Read `kOfxImageEffectPropCudaEnabled` **at render time**; never infer it from what you advertised | `templates/minimal-cuda-ofx/src/Plugin.cpp:79` (throws if the CUDA path is entered without it) | Treating a host pointer as device memory → fault or garbage |
| Run on the host's stream | Two branches: stream present → return immediately; absent → finish the work | `templates/minimal-cuda-ofx/src/Plugin.cpp:120`–`:126` | Blocking the host's pipeline; or returning before the work is done |
| Ship a CPU fallback | Compile-time split (`#ifdef`), and advertise the capability **only** when the path exists | `templates/minimal-cuda-ofx/src/Plugin.cpp:315`; `templates/minimal-cuda-ofx/src/Kernel.cuh` stays CUDA-free | Advertising CUDA in a build that has no CUDA path → black frames |
| Read any image the host hands over | Base pointer + `(window − bounds)` offset + signed 64-bit `rowBytes` | `GainProcessor::processImagesCuda()` and `InferencePlugin::render()`, both in this repo | Shear on padded rows; flip on negative pitch; a top-down framebuffer whose `bounds` still start at `y1` mirrors the image; offset renders shift |
| Preserve alpha | Copy channel 3 through untouched; only touch RGB | `templates/onnx-inference-ofx/src/Plugin.cpp:55` (`CopyAlpha`), called at `:243` | A model with 3 channels silently destroys the matte |
| Draw a box, label or plate into the frame | Write through the destination pitch, offset by `window − bounds`; composite premultiplied when the image is | `ARCHITECTURE_AND_CUDA_PIPELINE.md` §13 (rules); `templates/onnx-inference-ofx/src/Plugin.cpp:81` (`CopyRect`, the same addressing for a full-window copy) | Overlay flips when the pointer is assumed top-down (its origin is not the stride sign) or the pitch is ignored; a box glows on a soft matte; a fill past a proxy window writes another clip's image |
| Be spatially aware for the host | Override `getRegionsOfInterest`, expand in canonical coordinates, clamp to the source region of definition, and read exactly the region you declared | `templates/onnx-inference-ofx/src/Plugin.cpp` (`getRegionsOfInterest` expands the window by the tile halo; `render` packs that region and writes only the window), checked by `testkit/contract_main.cpp` `--roi-expand` (the rectangle) and `--roi-context` (the pixels: an inset window is bit-identical to the whole-frame render, and stops being so against a host that ignores the declaration; `--roi-context-tolerance` repeats it on the tiled path with a measured bound); `.ofx-ref/Support/Plugins/Basic/basic.cpp:334` (SDK sample) | A visible seam at tile edges that looks like a kernel bug; a declared RoI smaller than what you read loses the border pixels with no error; a context you never read passes every rectangle check and still seams |
| Tile internally | `PlanTiles` + `Expand(halo)` + a feathered `TileAccumulator`, halo ≥ receptive-field radius | `templates/onnx-inference-ofx/src/AiCore.h` (halo contract at `:87`–`:89`) and `templates/onnx-inference-ofx/src/AiCore.cpp` | Seams; the negative control at `templates/onnx-inference-ofx/test/ai_pipeline_test.cpp:300` catches it |
| Prove the plug-in works without a host application | A fake host: `fetchSuite` for the five mandatory suites, a prop-set per object, `mainEntry` per action | `testkit/fake_host.cpp`, driven by `testkit/contract_main.cpp` and run from both `test/*/run-tests.sh` | Only testing the maths — wrong status codes, missing declarations and crashes at describe/render stay invisible |
| Run an ONNX model per frame on CPU | One session behind a pimpl, explicit intra-op thread budget, pitch-correct NCHW packing | `templates/onnx-inference-ofx/src/ModelRunner.{h,cpp}`, `src/AiCore.{h,cpp}` | Default thread count × host render threads; wrong tensor layout |
| Detect, classify or embed (output is not an image) | Session, packaging and packing from the ONNX template; write the head decoder and RoD yourself | `AI_MODEL_INTEGRATION.md` §7 (non-image models, detection heads); `templates/onnx-inference-ofx/src/ModelRunner.cpp:289` requires out shape == in shape | Feeding `[1, 4+N, anchors]` into the image unpacker; transposed layout read as attributes; NMS across classes |
| Run a big model, or a PyTorch one | Out-of-process worker with shared memory for frames and a socket/pipe for control | Architecture in [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §2; shipped examples in §1 there | Blocking `render()` on an inference that takes seconds |
| Process a clip as a job, not per frame | Stage the clip, run a background job with progress + cancel, read results from a disk cache | LTX plug-in's "Flame never waits on the model" design (B, see §4) | A viewer that hangs on every scrub |
| Offer two backends with fallback | Prefer the C runtime (ONNX), fall back to a Python subprocess, expose the choice | `houdini-ofx-plugins`' ONNX / Python / Auto switch (B, see §4) | A silent fallback that changes numerical results between machines |
| Reduce per-frame allocation | Reuse scratch buffers keyed by frame size | **Known gap**: the inference template allocates its pack buffer per call; the pattern is a `Scratch` object resized only when the size changes | Allocation churn in the render loop; breaks any `FullySafe` claim |
| Bundle a runtime next to the `.ofx` | Copy exactly the file name the loader records (SONAME on Linux, versioned dylib on macOS), with `$ORIGIN` / `@loader_path` rpath; on Windows a private name plus manual API init | `templates/onnx-inference-ofx/CMakeLists.txt` (single-copy bundling, per-platform branch; `ORT_API_MANUAL_INIT` on Windows) | Globbing `libfoo.so*` ships the symlink **and** the 34 MB target; the wrong token builds and cannot load; an import named `onnxruntime.dll` binds to the host's runtime instead of yours |
| Choose a model resolution from the input | A ladder on the larger input dimension, exposed as a parameter | `CorridorKey-Runtime`'s 512/1024/1536/2048 ladder (B, see §4) | One hard-coded resolution that is wrong for half the footage |
| Handle a temporal model | Chunk (`8n+1` frames), overlap, and blend with a dissolve or flow warp | LTX plug-in's chunk/overlap design (B, see §4) | Treating a time-axis model as a per-frame filter |
| Decide *not* to use a model | Classical kernel when the operation is local and analytic | `Hush-OpenNR` (B, see §4) | Shipping weights and a runtime for something a 20-line kernel does |
| Ship a GPU execution provider with a CPU fallback | One bundle: append the CUDA EP inside a `try`, end every failure on a CPU session, expose provider plus reason in a label parameter, keep the tile VRAM-bound and `cudnn_conv_algo_search = heuristic` | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7, "The provider belongs in the same bundle" (**B**, one RTX 3060) | A provider that stops loading is invisible — the frame stays correct and only the time changes; preloading the provider DLL fails with 1114, a missing cuDNN/cuFFT import with 126 |
| Feed a model the host's 8- or 16-bit, premultiplied pixels | Normalise by the depth's maximum, unpremultiply before packing and premultiply after unpacking, keep the conversion beside `Pack*`/`Unpack*` and OFX-free | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7, "What the host hands you is not float RGBA"; alpha rules in `ARCHITECTURE_AND_CUDA_PIPELINE.md` §8 | Byte and ushort frames are refused (the template accepts float only), and a denoiser on premultiplied data keeps the fringe it was meant to remove |
| Run a model that only accepts multiples of N, or one fixed size | Probe 63/72/80/96 at load to find the factor, pad right/bottom by edge replication and crop back, resize per tile for a fixed input and say so in the UI | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7, "Models that only accept certain sizes" (**B**: four of eight candidates) | `The input tensor cannot be reshaped to the requested shape` at every size but the export size |

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
| Black frame, plug-in "works" | capability advertised without a path; `CudaEnabled` not checked | §1 rows 3 and 5 |
| Image shears at odd sizes, fine at 1× | pitch assumed tight | §1 row 6 |
| Image shifted on a crop / RoD render | base pointer used as the window origin | §1 row 6 |
| Render reports success, frame untouched — a window outside the destination did nothing | not your loop: `OFX::ImageProcessor::process()` compares `_renderWindow` with `_dstImg->getBounds()`, `assert`s (gone under `-DNDEBUG`), then returns silently before `preProcess()`, so the status stays `kOfxStatOK` (`.ofx-ref/Support/include/ofxsProcessing.h:150-157`) | refuse both windows yourself with `kOfxStatErrValue` before `process()` — §1 row 6, both `templates/*/src/Plugin.cpp`, `--dst-bounds-inset` in [`testkit/README.md`](testkit/README.md) |
| Seam or grid at tile edges | halo smaller than the receptive field | §1 row 10 |
| Model refuses every size but the one it was exported at | the graph reshapes to `H/8`, `H/16` or `H/32` | §1, "multiples of N"; [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7 |
| Correct picture, but 10–30× slower than the same model elsewhere | the CUDA provider never loaded (silent CPU fallback), or `EXHAUSTIVE` conv-algorithm search at session creation | §1, "GPU execution provider"; [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7 |
| Whole-frame render is correct, a tiled one is not, and a larger halo "fixes" it | the halo was copied from a default, not measured for this model | §1 row 10; measurement recipe in [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7 |
| Crash only when a second plug-in is installed | symbol visibility (Support library symbols exported) | [`SKILL.md`](SKILL.md) §6 |
| Plug-in missing from the host's list | bundle name vs binary stem, wrong `Contents/<ARCH>/` — on Windows also the generator-dependent output property, which puts a "successful" build into `Contents\Linux-x86-64` | [`SKILL.md`](SKILL.md) §6, [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| Windows build succeeds, `.ofx` is in `Contents\Win64\Release\` | multi-config generator appended the configuration; both output properties need `$<1:…>` | [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| Ampere/Ada cards JIT-compile a plug-in that targeted 7 architectures | second configure over the same build tree dropped the curated `CMAKE_CUDA_ARCHITECTURES` (nvcc's default is cached) | [`SKILL.md`](SKILL.md) §6 "Architecture targets", `templates/minimal-cuda-ofx/CMakeLists.txt` |
| Viewer hangs while scrubbing | synchronous inference inside `render()` | §1 rows 14 and 15 |
| Host dies on shutdown or on a bad frame | runtime called during finalisation; uncaught exception across the ABI | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §3 |
| Result is plausible but wrong (colour, range) | channel order, value range, colour space | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §5 |
| Every custom model is labelled with COCO names past the shared prefix | the class table came from a table in the source, not the model's `names` metadata | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7, "Detection heads" |
| A promptable model fails on the second prompt, naming a `Concat`/`Reshape` node | the class count was baked at trace time (`int(x[-1].shape[1])`) | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §7, "Detection heads" |
| Host segfaults right after a successful unload (exit 139) | a runtime object in a function-local static is destroyed at `DLL_PROCESS_DETACH` | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §4 (leak the `Ort::Env`) |
| A push button does nothing in the harness | the fake host had no click path and answered the notification as a failure | [`testkit/README.md`](testkit/README.md) (`FakeHost::pressParam`) |
| `--set fontScale=2.5` fails although the plug-in is right | the contract test assumed every `--set` is a gain (`dst == src × atof(value)`) | pass `--set-status-only` ([`testkit/README.md`](testkit/README.md)) |
| Same frame differs between machines | silent Python/CPU fallback, or default thread count | §1 rows 16 and 12 |
| Second instance of the effect shows the first instance's setting | a parameter cached in a static/global instead of read per instance | [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §8 (`contract_test --concurrency-param`); [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) "Thread safety" |
| TSan: data race in `OFX::Log::indent` with two concurrent renders | not your code: every action increments a plain global counter in the Support library (`ofxsLog.cpp:63`, called from `ofxsImageEffect.cpp:2569`) | [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) "Thread safety"; audit your own shared state, the counter is cosmetic |
| `onnxruntime.dll` not found at load, though the bundle has a file by that name | the import library (`onnxruntime.lib`) was copied and renamed; Windows keeps the DLL in `bin\` | [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| Host crashes in `render` on Windows; the minidump names the host's own `onnxruntime.dll` | the import was resolved by base name to the runtime beside the host executable, so `GetApi(ORT_API_VERSION)` returned null | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §4 (private `onnxruntime-ofx.dll` + `ORT_API_MANUAL_INIT`) |
| ARM Linux build lands in `Contents/Linux-x86-64` | the arch directory was hard-coded instead of derived from the target | [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| Contract test passes a plug-in that is known to be wrong | the "broken" binary was never rebuilt (copied `build/` tree) or the check needs a flag (`--concurrency-param`) that was not passed | [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §14 |

### Compile and link symptoms

All five were reproduced against this repository's template and the pinned
`OFX_Release_1.5.1` headers. Two are listed with the stage they really appear
at: a shared module links with undefined symbols by default, so a missing
registration hook or Support translation unit fails at `dlopen`, not at `ld`.

| Message | Likely cause | Fix |
|---|---|---|
| `error: expected class-name before '{' token` on `public OFX::ImageProcessor` | `ofxsImageEffect.h` includes `ofxsParam.h`, `ofxsInteract.h`, `ofxsMessage.h` and the OFX C headers — never the processor header; `ImageProcessor` is declared at `Support/include/ofxsProcessing.h:28` | `#include "ofxsProcessing.h"` beside `ofxsImageEffect.h`. The message then changes to the real error (an `override` or constructor mismatch), which is progress, not a new problem |
| `relocation R_X86_64_PC32 against symbol ... can not be used when making a shared object; recompile with -fPIC` | an object in the module was compiled without position-independent code — a Support TU or a `.cu`. Reproduced by compiling `Support/Library/ofxsImageEffect.cpp` without `-fPIC` and linking it into a `.so` | `POSITION_INDEPENDENT_CODE ON` / `-fPIC` on the module and every translation unit, then rebuild from a clean directory |
| `nm -D --defined-only` shows 5 weak `W void std::vector<OFX::…>::_M_realloc_insert<…>` helpers beside the two entry points, and `build.sh` stops with `FAIL: the dynamic symbol table has 7 defined symbols, expected exactly 2` | `-fvisibility=hidden` does not reach weak template instantiations | pin the export list with a version script — `templates/minimal-ofx/exports.map`; `BUILD_PLAYBOOK.md` §3.5 |
| `dlopen` fails with `undefined symbol: _ZN3OFX6Plugin12getPluginIDsERSt6vectorIPNS_13PluginFactoryESaIS3_EE`, and the host enumerates zero plug-ins. The module itself links; add `-Wl,--no-undefined` (`-z defs`) and it becomes `undefined reference to 'OFX::Plugin::getPluginIDs(std::vector<OFX::PluginFactory*, std::allocator<OFX::PluginFactory*> >&)'` | the hook is missing, has the wrong signature, or sits in an anonymous namespace; Support calls it at `Support/Library/ofxsImageEffect.cpp:3039` | define `void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray&)` at namespace scope and push a factory with static storage — working example at `templates/minimal-ofx/src/Plugin.cpp:261` |
| `undefined symbol: OFX::…` at `dlopen`, or the same names as `undefined reference` once you link with `-z defs` | a Support translation unit is missing from the target, or one of your helpers is not in the source list | compile all eight `Support/Library/*.cpp` into the module (`file(GLOB …)`) and add every new file to `add_library` |
| `CMake Error … No CUDA toolset found.` | a Visual Studio generator compiles `.cu` only through NVIDIA's `BuildCustomizations` for that exact VS version; CUDA 13.0 ships none for VS 2026 | `build.bat` probes for them and falls back to Ninja + `vcvars64.bat`; see [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| `fatal error C1189: -- unsupported Microsoft Visual Studio version!` | the toolkit's `host_config.h` predates the installed MSVC | `vcvars64.bat -vcvars_ver=14.44` (newest accepted), or `-allow-unsupported-compiler`; matrix in [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| `Error: generator … Does not match the generator used previously` on the second Windows build | the cache in `build\CMakeCache.txt` records a different generator than this run chose | read `CMAKE_GENERATOR:INTERNAL` and reuse it, or delete `build\`; `build.bat` does the former |

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

## 5. The general-purpose suites: what to read, what to skip

The AI repositories in [§4](#4-external-ai-onnx-plug-ins-what-exists-what-to-learn-what-to-avoid)
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
| "Ensure explicit stream synchronisation (`cudaStreamSynchronize`, or the host stream)" | Backwards. With a host stream, the plug-in SHOULD NOT wait and SHOULD NOT call `cudaDeviceSynchronize()` at any time (`ofxGPURender.h:608`–`:610`). Wait only when no stream was supplied — see [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) |
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
| Model is ONNX, ops are supported by the C runtime you can ship | in-process | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §2, §4 |
| Model needs PyTorch code, custom ops, or a Python preprocessing stack | sidecar (shared memory + control socket) | §2, §3 |
| Inference takes longer than a frame budget | staged job + disk cache | §1 above, and the LTX design in §4 |
| Operation is local and analytic | no model at all | `Hush-OpenNR` in §4 |
| GPU is available but the host does not hand over device pointers | CPU, or accept one copy per frame and measure | [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §6 |
