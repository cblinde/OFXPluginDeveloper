# AI model integration for OpenFX plug-ins

How to put a neural network inside an OFX plug-in without turning the host into
a crash reporter. Every claim is graded, every architectural recommendation is
traceable to a project that shipped it, and the parts that could not be verified
here say so.

**Evidence grades**, used throughout:

| Grade | Meaning |
|---|---|
| **A** | Read from primary source: a header, a specification page, or measured by the test suite in this repository. |
| **B** | Read from a project's own documentation or source, not independently executed here. |
| **C** | Reported second-hand (README claim, forum post, issue thread). Treat as a lead. |

This repository has no GPU and no OFX host, so nothing below is a performance
measurement. Where a number appears it is a size, an exact CPU result from
`templates/onnx-inference-ofx/test`, a **B**-graded figure from a project that
shipped it, or explicitly labelled as unmeasured.

---

## 1. What shipped plug-ins actually do

Ten repositories were read for this document: eight OFX plug-ins, one Nuke NDK
node and one set of libtorch examples. They converge on the object model and
disagree on everything else, which is itself the finding.

| Project | Task | Runtime | Language | How the model is fed |
|---|---|---|---|---|
| [`CorridorKey-Runtime`](https://github.com/alexandremendoncaalvaro/CorridorKey-Runtime) | keying / matting | ONNX Runtime + TensorRT RTX EP (Windows), ORT CUDA EP (Linux, experimental), MLX (macOS), TorchScript (one model) | C++20 | thin IPC client + separate runtime server process, shared memory, sessions pooled by a broker (**B**) |
| [`corridorkey_ofx`](https://github.com/gitcapoom/corridorkey_ofx) | keying / matting | PyTorch (`torch.compile`, Triton) | C++ OFX + Python backend | named pipe for control, shared-memory float32 RGBA frames, 16-byte header (**B**) |
| [`Flame_LTXAlphaGen_OFX`](https://github.com/allklier/Flame_LTXAlphaGen_OFX) (forks: `flamelogik`, `tbdpost`) | matting (video diffusion) | PyTorch 2.x, CUDA / MPS | C++ OFX + Python worker | **staged job + disk cache**: the host looks up mattes and never waits on the model; temporal chunks (**B**) |
| [`MoGe-nuke`](https://github.com/sumitchatterjee13/MoGe-nuke) | depth / normals | PyTorch + Triton daemon | raw OFX C API + Python daemon | localhost TCP, synchronous per frame (**B**) |
| [`DaVinci-Resolve-DLSS5`](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) | neural denoise | NVIDIA NGX (DLSS-NR) | C++ | in-process, D3D12 textures, in-tree C++ (**B**) |
| [`Hush-OpenNR`](https://github.com/amateurmenace/Hush-OpenNR) | denoise | none (classical NLM) | C++ | the useful counter-example: no network at all (**B**) |
| [`rvmofx`](https://github.com/smunaut/rvmofx) | background removal (RVM) | TorchScript via libtorch, in-process | C++ | models ship in `Contents/Resources/`; proved in Resolve 18.1, but only with the **non-cxx11** libtorch build (**B**) |
| [`mattclifford1/OFX-plugins`](https://github.com/mattclifford1/OFX-plugins) | libtorch examples (CPU/CUDA) | libtorch, in-process | C++ | the smallest libtorch starting point — and the README records that Resolve cannot load libtorch's CUDA dependencies (**B**) |
| [`houdini-ofx-plugins`](https://github.com/zhangxiao6776/houdini-ofx-plugins) | depth and 7 creative effects | ORT C++ in-process (session caching) + Python subprocess + `Auto` fallback; CoreML on Apple Silicon | C++ / Python | a real two-backend design with C++/Python cross-verification tests (**B**) |
| [`Nuke-ONNX`](https://github.com/aaronsmithtv/Nuke-ONNX) | ONNX inference for Nuke | ORT in-process, up to 10 inputs | C++ | **NDK, not OFX**; useful only as a contrast in scope (**B**) |

What to take from them:

- **The split is decided by the runtime, not by OFX.** Of the eight OFX plug-ins,
  four run the model out of process, and every one of those needs Python/PyTorch
  (`CorridorKey-Runtime`, `corridorkey_ofx`, LTX, `MoGe-nuke`). The four that keep
  it in the binary all use a C/C++ runtime: NGX (`DaVinci-Resolve-DLSS5`),
  libtorch twice (`rvmofx`, the `mattclifford1` examples) and ONNX Runtime
  (`houdini-ofx-plugins`). The rule is **Python → sidecar, C runtime →
  in-process** (**B**).
- **libtorch is the awkward middle.** It works in-process — `rvmofx` ships
  TorchScript models in `Contents/Resources/` and ran in Resolve 18.1 — but it
  demands the non-cxx11 ABI build, and the `mattclifford1` README records that
  Resolve cannot load libtorch's CUDA dependencies at all (**B**). If you need
  CUDA torch, that is an argument for the sidecar, not for more `rpath` flags.
- **In-process is viable when the runtime is C.** `DaVinci-Resolve-DLSS5` uses a
  vendor runtime (NGX) rather than a Python stack, and `houdini-ofx-plugins` runs
  ONNX Runtime with a session cache (**B**).
- **Nobody tiles by accident.** `CorridorKey-Runtime` exposes a model-resolution
  ladder chosen from the input's larger dimension (≤1000→512, ≤2000→1024,
  ≤3000→1536, >3000→2048) plus an explicit "Enable Tiling" toggle with a tile
  overlap control (**B**). `Flame_LTXAlphaGen_OFX` crops an ROI around the input
  alpha with ≥64 px margin and rounds the sides to multiples of 32 (**B**).
- **Temporal models need a different unit of work.** The LTX plug-in processes
  145/73/33-frame chunks (`8n+1`), overlapping 24/16/12 frames, and merges with a
  dissolve or an optical-flow warp (**B**). A per-frame `render()` contract does
  not fit a model with a time axis; the plug-in has to invent the chunking and
  the blending.
- **Colour space is part of the interface.** The same project converts model
  input to display-referred Rec.1886 through OCIO and leaves the host's pixels
  untouched (**B**). A model trained on sRGB output applied to scene-linear
  values is a silently wrong grade, not a crash.
- **A network is not always the answer.** `Hush-OpenNR` is a competitive,
  MIT-licensed, GPU OFX denoiser built from measured noise analysis and
  non-local means (**B**). If the operation is local and analytic, a kernel is
  cheaper to ship, faster to start, and has no weights to version.

Two negatives worth recording, because they are commonly assumed to exist: no
open-source OFX **upscaler** and no open-source OFX **learned optical flow** were
found; the AI upscalers in the wild are TouchDesigner, C# or ComfyUI nodes, and
`NNFlowVector` is commercial. `Nuke-ONNX` exists but is a Nuke NDK op, not OFX,
and its `use_gpu` knob is disabled (**B**).

### 1.1 What their own documentation says to avoid

1. **A synchronous daemon blocks the host.** `MoGe-nuke` does per-frame TCP and
   the LTX plug-in explicitly does not ("Flame never waits on the model"); the
   fix is a job with progress, cancel and a cache, not a faster socket.
2. **Do not rely on `PATH` for a runtime.** The `mattclifford1` examples require
   libtorch on `PATH`; users report that as "the plug-in disappeared". Bundle it
   beside the `.ofx` with `$ORIGIN` instead.
3. **libtorch + CUDA in-process is a known dead end in Resolve.** Same README.
4. **ONNX Runtime's CoreML EP fragments the graph.** `houdini-ofx-plugins`
   reports partition fragmentation and uses native CoreML on Apple Silicon
   instead. One execution provider is not a portability strategy.
5. **Licences travel with code.** `rvmofx` is GPL-3.0 and CorridorKey's weights
   are non-commercial; read those repositories for patterns, do not paste from
   them into a distributed plug-in.
6. **Forks drift.** The LTX plug-in exists in at least three repositories; pin a
   commit and identify upstream before learning from one.

---

## 2. The four architectures, and when each is right

| Shape | Latency | Crash isolation | Packaging | Best when |
|---|---|---|---|---|
| **In-process C/C++ runtime** (ONNX Runtime, TensorRT, libtorch, OpenVINO, NGX) | lowest: no IPC, device pointers can be shared | none: a bad model takes the host down | one `.ofx` + one runtime `.so` (~34 MB for ORT CPU, see §8) | the model is ONNX/plan/TensorRT and the runtime ships as C |
| **Sidecar process** (separate binary, shared memory or socket) | one IPC hop per frame; predictable | full: worker dies, host survives | two binaries, a protocol, and a supervisor | Python/PyTorch stack, heavy deps, or a model you do not trust |
| **Embedded language runtime** (CPython, .NET, V8, Julia) | depends on the language's call overhead and thread rules | none | you ship a runtime inside a plugin | almost never; see §3 |
| **Host-native API** (CoreML, Metal/MPS, NGX) | lowest on its platform | none | platform-specific | you are already platform-specific and the host supports it |

The decision rule that survives contact with the projects above:

1. If the model is ONNX and the ops are supported by a C runtime you can ship,
   **go in-process**. You keep zero-copy on device pointers, which is the only
   way the GPU path pays for itself (§7).
2. If the model needs PyTorch code, custom ops, or a Python-only preprocessing
   stack, **go sidecar** and design the frame transport first (shared memory,
   fixed header, explicit width/height/format).
3. If you think you need to embed CPython *inside* the `.ofx`, read §3 first.
   Every project that started there either moved the interpreter to a worker or
   wrapped it in a supervisor.

---

## 3. Language runtimes inside a plugin: what actually breaks

The OFX constraint comes first: hosts call actions on a main thread and on
render threads, and an instance must handle simultaneous main-thread and
render-thread actions; you declare your guarantee with
`kOfxImageEffectPluginRenderThreadSafety` (`Unsafe` / `InstanceSafe` /
`FullySafe`) — `ofxThreadSafety.html` and `ofxRendering.html` (**A**). Any runtime
with thread affinity must either own a thread and be marshalled to, or force you
to downgrade that guarantee.

| Language | The documented fact that decides it | What breaks in a host | Recommended shape |
|---|---|---|---|
| **Python** (CPython embedding) | `PyGILState_Ensure` "will likely crash the process" if used during interpreter finalisation, and the GIL-state APIs "are not compatible with sub-interpreters"; soft-deprecated in 3.15 in favour of `PyThreadState_Ensure` (**A**, Python C API docs + PEP 788) | a render thread that calls into Python during host shutdown; two interpreters in one process; `sys.path` that works in a terminal and not in Resolve | sidecar process, or one dedicated worker thread that owns the interpreter and receives jobs |
| **Rust** (`cdylib` + `extern "C"`) | since **1.81**, a panic in an `extern "C"` function **aborts** (RFC 2945); `#[no_mangle]` is `unsafe` because of symbol collisions (**A**, Rust Reference) | one `panic!` on a render thread kills the host with no message; two Rust plug-ins each carry their own std | plausible in-process choice: `catch_unwind` in every export, prefixed symbol names, `cdylib` |
| **C# / .NET** | NativeAOT exports only `[UnmanagedCallersOnly]` methods, blittable arguments only; an exception leaving one **fail-fasts** the process; `hostfxr` hosting supports framework-dependent deployments only (**A**, Microsoft docs + runtime issue) | a managed exception in a render callback terminates the host even with an outer `try` | NativeAOT in-process, exceptions converted to status codes at the boundary |
| **JavaScript** | V8 allows one thread per isolate and requires `v8::Locker` to switch; QuickJS: "Inside a given runtime, no multi-threading is supported" (**A**) | convenience APIs called from two render threads; V8 initialisation is process-global and cannot be safely repeated | QuickJS (+ one mutex) if you must; V8 only behind a single owning thread |
| **Julia** | the C API is "not fully thread-safe"; `jl_init()` once; API calls only from the thread that initialised Julia or Julia-started threads; `RTLD_GLOBAL` required (**A**, Julia embedding docs) | the first render on a host thread is undefined behaviour, and `RTLD_GLOBAL` exports a large symbol surface into the host | do not embed; use a process |
| **Swift / CoreML** | Swift's C++ interop header is "supported only by Clang"; CoreML needs the model compiled into an app resource and a host that is a normal macOS app (**A**) | Clang-only interop rules out MSVC toolchains; `.mlpackage` must ship in the bundle | macOS-only plug-in with an ObjC++ bridge and `@convention(c)` exports |

Licensing, which decides as often as engineering: CPython is PSF-2 (bundling
allowed), pybind11/nanobind BSD-3, V8 BSD-3, QuickJS MIT, .NET MIT on Linux but
the .NET Library License on Windows, Swift Apache-2 **with** Runtime Library
Exception, Julia MIT. A sidecar keeps all of this out of your `.ofx` entirely,
which is the strongest argument for the sidecar shape.

`nanobind` cannot embed at all ("Embedding … is unsupported", its porting guide);
`pybind11` can (`py::scoped_interpreter`, `PYBIND11_EMBEDDED_MODULE`) (**A**). If
you are embedding rather than binding, that decision is made for you.

---

## 4. ONNX Runtime: the facts that matter, with exact names

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
with `Microsoft.ML.OnnxRuntime` 1.20.1):

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

## 5. Tensor plumbing: where the silent bugs are

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

## 6. When CUDA is necessary, helpful, or a waste of time

No GPU was available for this work, so this section contains **no measured
speedups** — only the decision procedure and the structural costs, each of which
is verifiable on your own hardware in about ten minutes.

**The order is fixed: CUDA first, CPU as the fallback.** A plug-in whose model
runs per frame is a plug-in whose speed the user feels, so the first path is the
CUDA execution provider and the CPU session is what keeps the frame coming when
the provider cannot load. What the costs below decide is *how much* that is
worth and what has to be true for it to pay — not whether to write the provider
path. A CPU default is a decision to justify (a tiny model, a proxy render, a
host with no device), not the neutral starting point.

### The three costs CUDA adds, independent of FLOPs

1. **A transfer, unless the host gives you device pointers.** CUDA render is
   voluntary per render: `kOfxImageEffectPropCudaEnabled` is `int × 1` and "the
   host MAY set this property" only when both sides advertise support
   (`ofxGPURender.h:559-575`) (**A**). If it is 0, the pixels are host memory and
   the CPU path is what you should run.
2. **A context switch per launch**, plus whatever your framework does at the end
   of a `Run()` — ORT synchronises the EP unless you disable it (§4).
3. **A device-side allocation, if you make one per frame.** `cudaMalloc`/`cudaFree`
   are "expensive operations" and the documented advice is to avoid "potentially
   costly synchronization of `cudaMalloc` and `cudaFree`" (CUDA Best Practices
   §10.3, Programming Guide §4.3.2) (**A**).

### The decision procedure

1. **Is the host handing you device memory?** If no, stop: CUDA means a
   host→device copy per frame. That copy is proportional to frame bytes, and it
   is the same cost for a LUT and for a 200-layer network, so it cannot be
   amortised by a bigger model — only by already being on the device.
2. **What is the arithmetic intensity of the operation?** Elementwise work
   (gain, LUT, matrix, curves) reads and writes every pixel and does a handful of
   flops: it is bandwidth-bound, and the GPU's advantage over a vectorised CPU
   loop is a small constant, not an order of magnitude. Convolutions with many
   channels, attention, and iterative solvers are compute-bound and are where
   GPUs win by large factors.
3. **How big is the frame, and how many flops per frame?** Compute your model's
   FLOPs for the actual resolution (exporters report them; `onnx` can shape-count).
   If the per-frame work is comparable to the cost of moving the frame, CPU is
   the better engineering choice regardless of what the GPU could do.
4. **Measure both, on a frame, with a timer around the model only.** ONNX Runtime
   can profile (`SessionOptions::EnableProfiling`) and print per-node timings.
   Compare CPU vs GPU EP wall time for your real resolution, including the copy
   in and out. Anything else is folklore.

### Verdicts by workload

| Workload | Verdict |
|---|---|
| LUT / gain / colour matrix / curves | **CPU.** No model, no GPU. A kernel is a handful of flops per byte; the copy dominates. |
| Small CNN (a few layers, ≤ 64 channels) on 1080p | **CUDA first, then check the copy.** The provider is the first attempt; if moving the frame outweighs the maths on your resolution, the CPU session is the fallback — and you now have the number that says so. |
| Large CNN / transformer per frame | **CUDA**, and only with host device pointers + IO binding + no per-frame allocation. |
| Diffusion / video model | **GPU, and a sidecar**: the runtime is PyTorch, the VRAM is gigabytes, and blocking the host is unacceptable — this is what the LTX plug-in does (**B**). |
| Model with ops the GPU EP does not implement | **Neither wins**: expect silent CPU fallbacks and copies in both directions. Check the provider's supported-op list before shipping. |
| Resolve on macOS | **No CUDA at all.** Apple hosts expose Metal/OpenCL, not CUDA; the same project ships an MLX/Metal path instead (**B**). |
| Quick previews / proxies / thumbnails | **CUDA with a smaller model**, or the CPU session where session startup and the transfer dominate. Measure the pair once; do not assume the proxy case is the CPU case. |

The honest summary: **CUDA first, CPU as the fallback.** CUDA is *necessary*
when the model is compute-bound and the host already owns the pixels on the
device; it is *helpful* whenever the model runs per frame and the frame is large
(28× on a 4x model, 11× on a denoiser, measured elsewhere (**B**)); and it is
*not worth it* whenever you would have to copy the frame yourself, allocate per
render, or synchronise a stream the host gave you. Three of those four are
things a plug-in can arrange — that is the work, and it belongs on the first
path rather than on a second one.

---

## 7. The template: `templates/onnx-inference-ofx`

A complete, building OFX plug-in that runs an ONNX model, with the parts that
are usually untested under test. The session this template creates is a CPU
session, because no GPU exists in the environment it was built in; for a plug-in
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

What this template proves, on this machine: see §5's measurements, plus the
bundle exports exactly two symbols, records `NEEDED libonnxruntime.so.1` and
`RUNPATH $ORIGIN` on Linux (`@loader_path` on macOS, where the runtime keeps its
versioned `.dylib` name), ships one copy of the runtime, `dlopen` enumerates one
plugin (`com.ofxplugindeveloper.onnxinference` v1.0), and the session asks for
the CUDA provider before it is created as a CPU session
(`ModelRunner::AppendCudaProvider`). The outcome is visible: the plug-in writes
it into the `provider` label parameter, and the suite asserts the value, which
reads

```
CPU (fallback: Failed to load library libonnxruntime_providers_shared.so with
error: libonnxruntime_providers_shared.so: cannot open shared object file ...)
```

here — the request was made, it failed, the frame still rendered, and the label
says why (**V**).

What it does **not** prove: no host has rendered through it, no CUDA session has
ever been created, and no timing is claimed. The provider *request* is compiled
and its failure path is tested; the success path is not. There is no GPU and no
provider binary in this repository, so on a GPU machine the remaining work is to
confirm the label flips to `CUDA (device 0)`, measure the run (`bench`-style,
timer around `Run()`), and repeat the bundle check with the provider DLLs in
place — the recipe below is documented from the ORT API, and every number in it
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
   `getRegionOfDefinition` — see `ARCHITECTURE_AND_CUDA_PIPELINE.md` §7 for the
   RoD rules and §5 of this document for the packing side.
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
  `A == 0` at zero. `ARCHITECTURE_AND_CUDA_PIPELINE.md` §8 states the rule per
  operation; this is the inference instance of it.
- **Keep the conversion OFX-free.** Depth and alpha conversion belongs beside
  `Pack*`/`Unpack*`, on plain buffers addressed through the host's signed row
  bytes (the pitch trap in §5), which is what lets a test round-trip every depth,
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
- **Pad right and bottom** to that multiple with edge replication, run, and crop
  the result back to the real size. Zero padding is content to a convolution, so
  a black border bleeds inward; replication does not.
- **A fixed input size** means scaling every tile to the export resolution and
  back. That costs quality — say so in the UI instead of letting the user find
  out from a soft frame.

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
in `templates/onnx-inference-ofx/models/test_conv.onnx.halo.json`. On a 256×192
frame with 128-pixel tiles, `halo=0` deviates by 0.1745 at the interior tile
boundary (128,127) and `halo=1` is exact — the same story as the `halo=0`
control in §5, from a second witness, and the reason the ladder may include 0:
a measurement that cannot produce the failure cannot be trusted to recognise
success.

The price, so "more context is safer" is not free: a tile of `T` with overlap
`H` makes the model see `(T + 2H)²` pixels instead of `T²`. At `T = 512`,
`H = 64` is +56 % and `H = 32` is +27 % — halving the overlap saves 19 % of
model time (**A**: arithmetic from the formula).

#### The provider belongs in the same bundle, and it is the first path

§6 answers *how much* CUDA is worth. For a per-frame inference plug-in the order
is fixed — request the CUDA provider first, keep a CPU session as the fallback,
make the choice visible — and the consequence is packaging, not a second build:

- **One bundle, request the provider, fall back silently.** Create the session
  with `AppendExecutionProvider_CUDA` inside a `try`; every failure — no device,
  no driver, missing cuDNN DLL, not enough VRAM — ends on a CPU session instead
  of an exception the host turns into a red frame. This template already does
  exactly that: `ModelRunner::AppendCudaProvider()` sets
  `cudnn_conv_algo_search = HEURISTIC`, catches the failure and rebuilds the
  options for the CPU session, and `Plugin.cpp` writes provider plus reason into
  the `provider` label parameter. Two lazily built sessions are simpler than a
  retry ladder.
- **Make it visible and assert it.** Nothing else tells anyone the provider
  stopped loading: the picture stays correct and only the time changes —
  measured elsewhere at 28× on a 4x model and 11× on a denoiser (**B**). Write
  provider plus reason into a label parameter (`CUDA (RTX 3060)` versus
  `CPU (provider load failed)`) and add a test stage that fails when the expected
  provider is missing.
- **Do not preload the provider DLL.** `LoadLibrary` on
  `onnxruntime_providers_cuda.dll` fails with error **1114** because its
  `DllMain` needs the mailbox that only the ORT core sets; ORT finds the provider
  beside the core by itself. Preload only what the provider imports — cuDNN,
  cuBLAS/cuBLASLt, cuFFT, cudart — or the load fails with **126** (**B**).
- **Budget the size.** The provider imports `cufft64_*` even for a model with no
  FFT in it; with cuDNN 9 + cuBLAS 12 + cuFFT a bundle was 2.4–2.7 GB, of which
  the provider alone was 643 MB (**B**). That is a product decision — one
  download for everyone, no separate GPU build.
- **`cudnn_conv_algo_search = heuristic`.** The ORT default benchmarks
  convolution algorithms at session creation and was reported to cost minutes at
  1080p before the first tile (**B**).
- **Tile size is VRAM-bound, not model-bound.** The same 4x transformer held
  7.25 GB at a 384² tile and crossed 11.8 GB at 640², at which point the driver
  fell into its slower path: halving the tile beats enlarging it (**B**).

The request itself is compiled and its failure path is verified here: this
machine links the CPU package of ONNX Runtime, `AppendExecutionProvider_CUDA`
throws `Failed to load library libonnxruntime_providers_shared.so`, and the
`provider` label carries that reason (**V**). The success branch has never run —
no GPU, no provider binary — so every number above is external evidence from one
RTX 3060 (**B**), and the success path is documented from the ORT API rather
than measured.

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

---

## 8. What this document does not know

- **No performance numbers.** No GPU is present; the CPU figures above are
  exactness checks, not timings. Any claim that a model "runs in real time" here
  would be invented.
- **No host has rendered a model.** Registration and `dlopen` are verified; the
  render path is not.
- **Provider support is a moving target.** Op coverage, CUDA/cuDNN pairing and
  concurrency guarantees differ per ORT release; the page to re-read before
  shipping is ORT's execution-provider matrix for your exact version.
- **The projects in §1 were read, not built.** Their claims are graded **B**;
  their architectures are the transferable part.
- **Embedding CPython inside a `.ofx` is described, not attempted.** The
  documented hazards (finalisation crash, sub-interpreter incompatibility) are
  the reason the recommendation is "worker process".
