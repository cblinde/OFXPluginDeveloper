# AI architectures, language runtimes and when CUDA pays

What shipped plug-ins do, the four architectures, per-language embedding hazards, and the decision procedure for CUDA.

<!-- AI_MODEL_INTEGRATION.md lines 25-159 -->
<!-- AI_MODEL_INTEGRATION.md lines 329-400 -->
<!-- AI_MODEL_INTEGRATION.md lines 854-869 -->
<!-- PATTERN_INDEX.md lines 145-187 -->
<!-- AI_MODEL_INTEGRATION.md lines 1-24 -->
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

No CUDA execution provider has ever been created here, so nothing below is a
model performance measurement. Where a number appears it is a size, an exact CPU
result from `templates/onnx-inference-ofx/test`, a **B**-graded figure from a
project that shipped it, or explicitly labelled as unmeasured. The CUDA *kernel*
throughput measured on 2026-11-24 is a different thing entirely — see
[`../README.md`](../README.md) §"What has actually been verified".

---
