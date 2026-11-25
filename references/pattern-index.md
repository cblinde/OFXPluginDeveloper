# Pattern index: task and property routes

"I need to do X": the pattern, the proven code here, and the first failure if done naively.

<!-- PATTERN_INDEX.md lines 29-63 -->
<!-- PATTERN_INDEX.md lines 249-258 -->
<!-- PATTERN_INDEX.md lines 1-28 -->
## 1. By task ("I need to…")

| I need to… | Pattern | Proven code here | First failure if done naively |
|---|---|---|---|
| Register one plug-in, or several from one module | One `OFX::Plugin::getPluginIDs`, factories with static storage; the Support library generates the two exports | `templates/minimal-ofx/src/Plugin.cpp` (the whole hook, 3 lines); `templates/minimal-cuda-ofx/src/Plugin.cpp` (`getPluginIDs`); `.ofx-ref/Support/Plugins/MultiBundle/PluginRegistration.cpp:12`–`:15` shows two factories in one module | Factories that die with the module; a bundle that does not enumerate |
| Produce a `.ofx` a host will actually load | Module named `<Name>.ofx`, the 8 Support TUs compiled in, `-fPIC`, hidden visibility **plus** a version script exporting only the two entry points | `templates/minimal-ofx/build.sh` (builds and then asserts the artifact: two dynamic symbols, no `Ofx*` undefined, no build-path RUNPATH, `dlopen` + `OfxGetPlugin`) | `-fvisibility=hidden` alone still exports the weak `std::` helpers, and without it hundreds of symbols — measured in `cmake-and-linking.md` §3.5 |
| Know whether the pixels are device or host pointers | Read `kOfxImageEffectPropCudaEnabled` **at render time**; never infer it from what you advertised | `templates/minimal-cuda-ofx/src/Plugin.cpp:79` (throws if the CUDA path is entered without it) | Treating a host pointer as device memory → fault or garbage |
| Run on the host's stream | Two branches: stream present → return immediately; absent → finish the work | `templates/minimal-cuda-ofx/src/Plugin.cpp:120`–`:126` | Blocking the host's pipeline; or returning before the work is done |
| Ship a CPU fallback | Compile-time split (`#ifdef`), and advertise the capability **only** when the path exists | `templates/minimal-cuda-ofx/src/Plugin.cpp:315`; `templates/minimal-cuda-ofx/src/Kernel.cuh` stays CUDA-free | Advertising CUDA in a build that has no CUDA path → black frames |
| Read any image the host hands over | Base pointer + `(window − bounds)` offset + signed 64-bit `rowBytes` | `GainProcessor::processImagesCuda()` and `InferencePlugin::render()`, both in this repo | Shear on padded rows; flip on negative pitch; a top-down framebuffer whose `bounds` still start at `y1` mirrors the image; offset renders shift |
| Preserve alpha | Copy channel 3 through untouched; only touch RGB | `templates/onnx-inference-ofx/src/Plugin.cpp:55` (`CopyAlpha`), called at `:243` | A model with 3 channels silently destroys the matte |
| Draw a box, label or plate into the frame | Write through the destination pitch, offset by `window − bounds`; composite premultiplied when the image is | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) `onnx-and-tensorrt.md` §4, "Drawing into the frame: the overlay after the model" (the four traps after inference); `pixel-formats-and-pitch.md` §2 (full rules); `templates/onnx-inference-ofx/src/Plugin.cpp:81` (`CopyRect`, the same addressing for a full-window copy) | Overlay flips when the pointer is assumed top-down (its origin is not the stride sign) or the pitch is ignored; a box glows on a soft matte; a fill past a proxy window writes another clip's image |
| Be spatially aware for the host | Override `getRegionsOfInterest`, expand in canonical coordinates, clamp to the source region of definition, and read exactly the region you declared | `templates/onnx-inference-ofx/src/Plugin.cpp` (`getRegionsOfInterest` expands the window by the tile halo; `render` packs that region and writes only the window), checked by `testkit/contract_main.cpp` `--roi-expand` (the rectangle) and `--roi-context` (the pixels: an inset window is bit-identical to the whole-frame render, and stops being so against a host that ignores the declaration; `--roi-context-tolerance` repeats it on the tiled path with a measured bound); `.ofx-ref/Support/Plugins/Basic/basic.cpp:334` (SDK sample) | A visible seam at tile edges that looks like a kernel bug; a declared RoI smaller than what you read loses the border pixels with no error; a context you never read passes every rectangle check and still seams |
| Tile internally | `PlanTiles` + `Expand(halo)` + a feathered `TileAccumulator`, halo ≥ receptive-field radius | `templates/onnx-inference-ofx/src/AiCore.h` (halo contract at `:87`–`:89`) and `templates/onnx-inference-ofx/src/AiCore.cpp` | Seams; the negative control at `templates/onnx-inference-ofx/test/ai_pipeline_test.cpp:300` catches it |
| Prove the plug-in works without a host application | A fake host: `fetchSuite` for the five mandatory suites, a prop-set per object, `mainEntry` per action | `testkit/fake_host.cpp`, driven by `testkit/contract_main.cpp` and run from both `test/*/run-tests.sh` | Only testing the maths — wrong status codes, missing declarations and crashes at describe/render stay invisible |
| Run an ONNX model per frame on CPU | One session behind a pimpl, explicit intra-op thread budget, pitch-correct NCHW packing | `templates/onnx-inference-ofx/src/ModelRunner.{h,cpp}`, `src/AiCore.{h,cpp}` | Default thread count × host render threads; wrong tensor layout |
| Detect, classify or embed (output is not an image) | Session, packaging and packing from the ONNX template; write the head decoder and RoD yourself | `onnx-and-tensorrt.md` §4 (non-image models, detection heads); `templates/onnx-inference-ofx/src/ModelRunner.cpp:289` requires out shape == in shape | Feeding `[1, 4+N, anchors]` into the image unpacker; transposed layout read as attributes; NMS across classes |
| Run a big model, or a PyTorch one | Out-of-process worker with shared memory for frames and a socket/pipe for control | Architecture in [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §2; shipped examples in §1 there | Blocking `render()` on an inference that takes seconds |
| Process a clip as a job, not per frame | Stage the clip, run a background job with progress + cancel, read results from a disk cache | LTX plug-in's "Flame never waits on the model" design **[B]** | A viewer that hangs on every scrub |
| Offer two backends with fallback | Prefer the C runtime (ONNX), fall back to a Python subprocess, expose the choice | `houdini-ofx-plugins`' ONNX / Python / Auto switch (B, see [`reference-projects.md`](reference-projects.md)) | A silent fallback that changes numerical results between machines |
| Reduce per-frame allocation | Reuse scratch buffers keyed by frame size | **Known gap**: the inference template allocates its pack buffer per call; the pattern is a `Scratch` object resized only when the size changes | Allocation churn in the render loop; breaks any `FullySafe` claim |
| Bundle a runtime next to the `.ofx` | Copy exactly the file name the loader records (SONAME on Linux, versioned dylib on macOS), with `$ORIGIN` / `@loader_path` rpath; on Windows a private name plus manual API init | `templates/onnx-inference-ofx/CMakeLists.txt` (single-copy bundling, per-platform branch; `ORT_API_MANUAL_INIT` on Windows) | Globbing `libfoo.so*` ships the symlink **and** the 34 MB target; the wrong token builds and cannot load; an import named `onnxruntime.dll` binds to the host's runtime instead of yours |
| Choose a model resolution from the input | A ladder on the larger input dimension, exposed as a parameter | `CorridorKey-Runtime`'s 512/1024/1536/2048 ladder (B, see [`reference-projects.md`](reference-projects.md)) | One hard-coded resolution that is wrong for half the footage |
| Handle a temporal model | Chunk (`8n+1` frames), overlap, and blend with a dissolve or flow warp | LTX plug-in's chunk/overlap design (B, see [`reference-projects.md`](reference-projects.md)) | Treating a time-axis model as a per-frame filter |
| Decide *not* to use a model | Classical kernel when the operation is local and analytic | `Hush-OpenNR` (B, see [`reference-projects.md`](reference-projects.md)) | Shipping weights and a runtime for something a 20-line kernel does |
| Ship a GPU execution provider with a CPU fallback | One bundle: append the CUDA EP inside a `try`, end every failure on a CPU session, expose provider plus reason in a label parameter, keep the tile VRAM-bound and `cudnn_conv_algo_search = heuristic` | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §6, "The provider belongs in the same bundle" (**B**, one RTX 3060) | A provider that stops loading is invisible — the frame stays correct and only the time changes; preloading the provider DLL fails with 1114, a missing cuDNN/cuFFT import with 126 |
| Find out whether the GPU really ran every node | Probe once with `session.disable_cpu_ep_fallback=1`: session creation fails and names the node when the provider cannot cover the graph; build the real session without the flag | [`onnx-and-tensorrt.md`](onnx-and-tensorrt.md) §6 | Unsupported nodes are assigned to the CPU EP without a message, the label still says CUDA, the frame stays correct and only the clock tells |
| Make a CUDA result reproducible, or explain a GPU-versus-CPU difference | Set `use_tf32=0` through the key/value provider options (the legacy struct has no such field), pin `cudnn_conv_algo_search` to `HEURISTIC`, state `gpu_mem_limit` and `arena_extend_strategy` | [`onnx-and-tensorrt.md`](onnx-and-tensorrt.md) §5; the deviation measured here sits between `1e-8` and `1e-7` ([`testing-layers.md`](testing-layers.md) §13) | TF32 rounds at the third decimal place, so either the golden fails outright or a loose tolerance hides it, and the arena doubles on every growth |
| Feed a model the host's 8- or 16-bit, premultiplied pixels | Normalise by the depth's maximum, unpremultiply before packing and premultiply after unpacking, keep the conversion beside `Pack*`/`Unpack*` and OFX-free | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §6, "What the host hands you is not float RGBA"; alpha rules in `colour-and-alpha.md` §1 | Byte and ushort frames are refused (the template accepts float only), and a denoiser on premultiplied data keeps the fringe it was meant to remove |
| Run a model that only accepts multiples of N, or one fixed size | Probe 63/72/80/96 at load to find the factor, pad right/bottom by edge replication and crop back, resize per tile for a fixed input and say so in the UI | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §6, "Models that only accept certain sizes" (**B**: four of eight candidates) | `The input tensor cannot be reshaped to the requested shape` at every size but the export size |

Full decision procedure for the model rows:
[`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §2 (shape) and §4 (CUDA worth
it or not).

---

## 2. If you only need the decision, not the code

| Situation | Shape | Where the reasoning is |
|---|---|---|
| Model is ONNX, ops are supported by the C runtime you can ship | in-process | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §2, §6 |
| Model needs PyTorch code, custom ops, or a Python preprocessing stack | sidecar (shared memory + control socket) | §2, §3 |
| Inference takes longer than a frame budget | staged job + disk cache | §1 above, and the LTX design in [`reference-projects.md`](reference-projects.md) |
| Operation is local and analytic | no model at all | `Hush-OpenNR` in [`reference-projects.md`](reference-projects.md) |
| GPU is available but the host does not hand over device pointers | CPU, or accept one copy per frame and measure | [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §4 |

## 3. Pattern index

A lookup table for "I need to do X inside an OFX plug-in — what is the proven
shape, where is the code, and what breaks first?" It is deliberately an **index**,
not a tutorial: every row points at code in this repository or at a project that
shipped the pattern, and the prose that explains *why* lives in the linked
documents.

Read it four ways:

- **By task** — [§1](#1-by-task-i-need-to) when you know what you are building.
- **By symptom** — [§3](debugging-and-symptoms.md#3-by-symptom-the-render-is-wrong) when something is
  already broken.
- **By property** — [§3](ofx-api-constants.md#3-by-property-which-constant-does-what) when you are
  staring at a `kOfx*` name and need to know who sets it.
- **By repository** — [the general-purpose suites](reference-projects.md#10-the-general-purpose-suites-what-to-read-what-to-skip)
  when you would rather read shipping code than prose.

For the reading order that teaches the object model, see
[`reference-projects.md`](reference-projects.md). For why a pattern is
correct, the citation in the row is the authority; this file only routes you.

**Rule for adding a row:** a pattern gets a *proven code* pointer only if the code
is in this repository and its tests cover it. External projects are evidence about
what shipped, graded **B**, and are never presented as verified here.

---
