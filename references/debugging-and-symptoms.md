# Debugging: symptoms, causes, anti-patterns

Symptom tables, attach points, profiling, the five crashes worth memorising, and the anti-patterns that produce them.

<!-- SKILL.md lines 537-598 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 630-703 -->
<!-- PATTERN_INDEX.md lines 64-125 -->
<!-- BUILD_PLAYBOOK.md lines 324-361 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 479-495 -->
<!-- SKILL.md lines 648-666 -->
<!-- REFERENCE_READING_GUIDE.md lines 340-361 -->
## 1. Debugging quick reference

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

Every error path must report *and* throw a specific code. §2 has the printf-format gotcha, the log content worth capturing, and the five
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
| Crashes only with a second plugin installed | Default symbol visibility (`cmake-and-linking.md` §3) |
| Crashes on scrub, fine on render | Async work outliving the render action without a stream |
| Works in Edit page, fails in Color page | Different render window / RoI; see `references/host-resolve.md` |
| Loading the plugin kills the host at startup | Static initialiser doing CUDA work; a static `cudaMalloc` at load |
| GPU path gone, label says `cannot load cufftw64_11.dll (error 127)` | A preload of everything beside the module; skip cuFFTW and guard host-owned base names (`packaging-and-install.md` §2) |
| Host crashes in its own `cudnn64_9.dll` after your plugin loads | A second module of the same base name; use the host's stack or `OFXPLUGIN_CUDA_FROM_HOST` |
| Second instance shows the first instance's value | State in a static/global instead of the instance (`testing-layers.md` §8) |
| A label says `CPU (8 threads)` but the setting changes nothing | The session was built before the parameter, or with a second options helper (`onnx-and-tensorrt.md` §4) |

**Profiling.** Start the host *under* the profiler: Nsight Systems
(`--trace=cuda,nvtx,osrt`) for the timeline, Nsight Compute per kernel, and
`compute-sanitizer --tool memcheck` for the pitch bug you cannot see. Both need
`-lineinfo` (or `-G`, debugging only).

**ABI failures.** Mixing OFX SDK versions is mostly harmless (properties are
string-keyed), but the `OfxPlugin` struct layout and the Support library's C++
class layouts are not. Never link two Support versions into one binary, and never
share C++ objects across a plugin boundary **[I]**.

---

## 2. Profiling and diagnosis

### Attach points

| Tool | Command shape | What it answers |
| --- | --- | --- |
| Nsight Systems | `nsys profile --trace=cuda,nvtx,osrt <host>` | Is my kernel on the timeline? Is the host waiting on me? Am I copying? |
| Nsight Compute | `ncu --target-processes all -k regex:GainKernel <host>` | Occupancy, memory throughput, stalls for one kernel |
| `cuda-memcheck` / compute-sanitizer | `compute-sanitizer --tool memcheck <host>` | Out-of-bounds in the kernel; the fastest way to find a pitch bug |

Because the host owns the process, start the host *under* the profiler. On a
render farm, profile a single-frame render or a still, not a sequence.

### Reading the Nsight Systems timeline

- Your kernel should appear as a thin band inside the host's render thread. If it
  is followed by a long gap where the host blocks, you are synchronising — look
  for `cudaStreamSynchronize` in mode 1 (a bug).
- Any `Memcpy HtoD`/`DtoH` inside the render band means you lost zero-copy.
- A `cudaMalloc` per frame shows up as a `cuMemAlloc` stall; move it to
  `beginSequenceRender`.
- If the kernel is missing entirely, `isEnabledCudaRender` was false: you are on
  the CPU path. That is the first thing to check, before any kernel tuning.

### Logging without a console

`OFX::Log::print()` does **not** reach the host. It appends to a file named by
`OFX_PLUGIN_LOGFILE` (default `ofxPluginLog.txt`), and Support guards the
`fopen` with `#ifdef DEBUG` (`Support/Library/ofxsLog.cpp:40-47`). CMake's
`Debug` configuration does not define `DEBUG` and neither do these templates, so
the call is silent in every build unless you add `-DDEBUG`. It is **printf-style** —
`print(const char* format, ...)` — so pass a literal format and arguments
(`print("launch failed: %s", msg)`), not a `std::string`: concatenating a
`std::string` and passing it as the format argument does not compile.

For a message the user actually sees, post it through the host's message suite:
`OFX::ImageEffect::setPersistentMessage(OFX::Message::eMessageError, id, msg)`
(`Support/include/ofxsImageEffect.h:1067`). It needs message suite v2; when the
host offers only v1 the Support wrapper throws a host-missing-suite exception, so
keep the file log as well instead of trading one silent channel for a crash.
For anything that must survive a crash, also append to a file:

```cpp
static void LogToFile(const std::string& message)
{
    // Resolve the path once; %TEMP%/ofx-<plugin>.log or /tmp equivalent.
    std::ofstream out(LogPath(), std::ios::app);
    out << message << '\n';
}
```

Log the facts that distinguish the branches: host name and version, whether
CUDA was enabled for this render, the stream pointer (as `%p`), image bounds,
rowBytes, and every non-success `cudaError_t` with `cudaGetErrorString`.

### The five crashes worth memorising

1. **`cudaErrorInvalidDevicePointer`** — launched on the wrong device, or the host
   was not in CUDA mode. Check `isEnabledCudaRender` and bind with
   `cudaPointerGetAttributes`.
2. **Heap corruption on exit** — freed host-owned memory or double-freed a device
   buffer. Never free what you did not allocate.
3. **Crash when a second plugin is installed** — symbol interposition; fix
   visibility.
4. **Hang on the first frame** — `cudaDeviceSynchronize()` deadlocking against the
   host's render thread.
5. **Host refuses to start** — your plugin crashed during `kOfxActionLoad` or a
   static initialiser. No CUDA calls outside actions, and no static CUDA objects.

These five are the observed failure classes in the host reports cited in
`references/host-resolve.md` **[B]**; none of them was reproduced here.

---

## 3. By symptom ("the render is wrong")

The triage table in §1 is the short route - what to check first. This is the
long form: the cause each symptom usually has. Performance symptoms are
collected in [`performance-model.md`](performance-model.md) §3.

| Symptom | Look first at |
|---|---|
| Black frame, plug-in "works" | capability advertised without a path; `CudaEnabled` not checked |
| Image shears at odd sizes, fine at 1× | pitch assumed tight |
| Image shifted on a crop / RoD render | base pointer used as the window origin |
| Labels sit on the wrong side of their box, or text reads mirrored | the rasteriser assumed the buffer's first row is the top: the pointer's origin is an axis of its own, separate from the stride's sign |
| Render reports success, frame untouched — a window outside the destination did nothing | not your loop: `OFX::ImageProcessor::process()` compares `_renderWindow` with `_dstImg->getBounds()`, `assert`s (gone under `-DNDEBUG`), then returns silently before `preProcess()`, so the status stays `kOfxStatOK` (`.ofx-ref/Support/include/ofxsProcessing.h:150-157`) |
| Seam or grid at tile edges | halo smaller than the receptive field |
| Model refuses every size but the one it was exported at | the graph reshapes to `H/8`, `H/16` or `H/32` |
| Correct picture, but 10–30× slower than the same model elsewhere | the CUDA provider never loaded (silent CPU fallback), or `EXHAUSTIVE` conv-algorithm search at session creation |
| Whole-frame render is correct, a tiled one is not, and a larger halo "fixes" it | the halo was copied from a default, not measured for this model |
| Crash only when a second plug-in is installed | symbol visibility (Support library symbols exported) |
| Plug-in missing from the host's list | bundle name vs binary stem, wrong `Contents/<ARCH>/` — on Windows also the generator-dependent output property, which puts a "successful" build into `Contents\Linux-x86-64` |
| Windows build succeeds, `.ofx` is in `Contents\Win64\Release\` | multi-config generator appended the configuration; both output properties need `$<1:…>` |
| Ampere/Ada cards JIT-compile a plug-in that targeted 7 architectures | second configure over the same build tree dropped the curated `CMAKE_CUDA_ARCHITECTURES` (nvcc's default is cached) |
| Viewer hangs while scrubbing | synchronous inference inside `render()` |
| Host dies on shutdown or on a bad frame | runtime called during finalisation; uncaught exception across the ABI |
| Result is plausible but wrong (colour, range) | channel order, value range, colour space |
| Every custom model is labelled with COCO names past the shared prefix | the class table came from a table in the source, not the model's `names` metadata |
| A promptable model fails on the second prompt, naming a `Concat`/`Reshape` node | the class count was baked at trace time (`int(x[-1].shape[1])`) |
| Host segfaults right after a successful unload (exit 139) | a runtime object in a function-local static is destroyed at `DLL_PROCESS_DETACH` |
| A push button does nothing in the harness | the fake host had no click path and answered the notification as a failure |
| `--set fontScale=2.5` fails although the plug-in is right | the contract test assumed every `--set` is a gain (`dst == src × atof(value)`) |
| Same frame differs between machines | silent Python/CPU fallback, or default thread count |
| Second instance of the effect shows the first instance's setting | a parameter cached in a static/global instead of read per instance |
| A bundle built against 1.20.1 still ships the 1.32.0 provider - or the other way round - although `ONNXRUNTIME_ROOT` names the right package | `OFXPLUGIN_CUDA_RUNTIME_DIRS` is a **cache** variable from an earlier configure and is searched first, so the provider, cuBLAS and cuDNN come from the old directory while the core comes from the new one. Measured 2026-11-26: 1.20.1's 12 MB `onnxruntime-ofx.dll` next to 1.32.0's 195 MB provider and `cublas64_13.dll`. Delete `build/`, or pass `-DOFXPLUGIN_CUDA_RUNTIME_DIRS=` again; `copy_if_different` does not notice because the files differ only in the direction nobody looks |
| MSVC: `error C2589: "(": invalid token on right side of "::"` in a file that includes `<windows.h>` | the `min`/`max` macros windows.h defines break `std::max`/`std::min`; `#define NOMINMAX` before the include (`templates/minimal-opencl-ofx/src/ClRuntime.cpp`) |
| `reinterpret_cast<void*>(::GetProcAddress(...))` will not compile on a 32-bit build | `GetProcAddress` returns a `FARPROC`, which is not an object pointer there; go through `std::uintptr_t` first, the same two-step the OpenCL template uses |
| TSan: data race in `OFX::Log::indent` with two concurrent renders | not your code: every action increments a plain global counter in the Support library (`ofxsLog.cpp:63`, called from `ofxsImageEffect.cpp:2569`) |
| `onnxruntime.dll` not found at load, though the bundle has a file by that name | the import library (`onnxruntime.lib`) was copied and renamed; Windows keeps the DLL in `bin\` |
| Host crashes in `render` on Windows; the minidump names the host's own `onnxruntime.dll` | the import was resolved by base name to the runtime beside the host executable, so `GetApi(ORT_API_VERSION)` returned null |
| The GPU path is gone and the label says `cannot load cufftw64_11.dll (Windows error 127)`, although that file sits beside the `.ofx` | 127 is `ERROR_PROC_NOT_FOUND`: cuFFTW imports the exact cuFFT build it shipped with, and the host's newer `cufft64_11.dll` was already loaded. The provider never imports cuFFTW, so skip it and preload only what the provider imports — without aborting the whole attempt on one unneeded file (`packaging-and-install.md` §2) |
| Host crashes inside its own `cudnn64_9.dll` after a GPU plug-in loads; the WER faulting module is in the host's folder | an absolute-path load of the bundle's cuDNN succeeds and leaves a **second** module of the same base name, and the provider's imports still bind by base name |
| An intra-op thread setting changes nothing, or two sessions of one plug-in disagree about it | ONNX Runtime's default is per **session**; a second session built with its own copy of the options keeps its own count |
| `--param modelChoice:int` fails as `OfxParamTypeChoice (expected OfxParamTypeInteger)` | the type name was not one of the accepted spellings and was passed through verbatim, so the failure reads as a descriptor mismatch |
| ARM Linux build lands in `Contents/Linux-x86-64` | the arch directory was hard-coded instead of derived from the target |
| Contract test passes a plug-in that is known to be wrong | the "broken" binary was never rebuilt (copied `build/` tree) or the check needs a flag (`--concurrency-param`) that was not passed |

### Build and load symptoms, by stage

Eight messages from one build, ordered by the stage that produces them. The
stage is the point: two come from the C++ compiler, one from the linker, and
two are not the toolchain complaining at all - `CMake Error` fires before a
compiler runs, and the `nm` row is this build's own artifact assertion. A
shared module links with undefined symbols by default, so a missing
registration hook or Support translation unit gets past `ld` and fails at
`dlopen`, which is why that symptom appears under two stages.

| Stage | Message | Likely cause | Fix |
|---|---|---|---|
| configure | `CMake Error … No CUDA toolset found.` | a Visual Studio generator compiles `.cu` only through NVIDIA's `BuildCustomizations` for that exact VS version; CUDA 13.0 ships none for VS 2026 | `build.bat` probes for them and falls back to Ninja + `vcvars64.bat`; see [`cmake-and-linking.md`](cmake-and-linking.md) §5 |
| configure | `Error: generator … Does not match the generator used previously` on the second Windows build | the cache in `build\CMakeCache.txt` records a different generator than this run chose | read `CMAKE_GENERATOR:INTERNAL` and reuse it, or delete `build\`; `build.bat` does the former |
| compile | `error: expected class-name before '{' token` on `public OFX::ImageProcessor` | `ofxsImageEffect.h` includes `ofxsParam.h`, `ofxsInteract.h`, `ofxsMessage.h` and the OFX C headers — never the processor header; `ImageProcessor` is declared at `Support/include/ofxsProcessing.h:28` | `#include "ofxsProcessing.h"` beside `ofxsImageEffect.h`. The message then changes to the real error (an `override` or constructor mismatch), which is progress, not a new problem |
| compile | `fatal error C1189: -- unsupported Microsoft Visual Studio version!` | the toolkit's `host_config.h` predates the installed MSVC | `vcvars64.bat -vcvars_ver=14.44` (newest accepted), or `-allow-unsupported-compiler`; matrix in [`cmake-and-linking.md`](cmake-and-linking.md) §5 |
| link | `relocation R_X86_64_PC32 against symbol ... can not be used when making a shared object; recompile with -fPIC` | an object in the module was compiled without position-independent code — a Support TU or a `.cu`. Reproduced by compiling `Support/Library/ofxsImageEffect.cpp` without `-fPIC` and linking it into a `.so` | `POSITION_INDEPENDENT_CODE ON` / `-fPIC` on the module and every translation unit, then rebuild from a clean directory |
| link, or load without `-z defs` | `dlopen` fails with `undefined symbol: _ZN3OFX6Plugin12getPluginIDsERSt6vectorIPNS_13PluginFactoryESaIS3_EE`, and the host enumerates zero plug-ins. The module itself links; add `-Wl,--no-undefined` (`-z defs`) and it becomes `undefined reference to 'OFX::Plugin::getPluginIDs(std::vector<OFX::PluginFactory*, std::allocator<OFX::PluginFactory*> >&)'` | the hook is missing, has the wrong signature, or sits in an anonymous namespace; Support calls it at `Support/Library/ofxsImageEffect.cpp:3039` | define `void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray&)` at namespace scope and push a factory with static storage — working example at `templates/minimal-ofx/src/Plugin.cpp:261` |
| load | `undefined symbol: OFX::…` at `dlopen`, or the same names as `undefined reference` once you link with `-z defs` | a Support translation unit is missing from the target, or one of your helpers is not in the source list | compile all eight `Support/Library/*.cpp` into the module (`file(GLOB …)`) and add every new file to `add_library` |
| artifact check | `nm -D --defined-only` shows 5 weak `W void std::vector<OFX::…>::_M_realloc_insert<…>` helpers beside the two entry points, and `build.sh` stops with `FAIL: the dynamic symbol table has 7 defined symbols, expected exactly 2` | `-fvisibility=hidden` does not reach weak template instantiations | pin the export list with a version script — `templates/minimal-ofx/exports.map`; `cmake-and-linking.md` §3.5 |

---

## 4. Claims this build corrected

These are the places where building disagreed with prose that already existed.

1. **"Three mandated entry points" is wrong.** `references/ofx-lifecycle-and-threads.md`
   says two, the CUDA template's CMake comment said three, and
   [`checklists.md`](checklists.md) D2 quoted the
   host call order including `OfxSetHost`. The Support library defines two
   symbols; `OfxSetHost` is declared and never defined; the host pointer arrives
   through `OfxPlugin::setHost` (`../SKILL.md` §3). The docs now say two exports plus the
   `setHost` field.
2. **`Support/Library` is not an include directory you need.** It was listed in
   all three templates that existed then; both this plug-in's TU and the Support TUs compile
   without it [V]. Only `include/` and `Support/include/` are required.
3. **The API string is `OfxImageEffectPluginAPI`.** The first version of this
   build's own assertion used `OfxImageEffectPlugin` and failed against the
   artifact (`../SKILL.md` §3) — which is exactly the mistake a newcomer makes from prose.
   The check now asserts the constant's real value and `apiVersion == 1`.

---

## 5. What this does not prove

| Question | Status |
|---|---|
| Does a real host (Resolve/Nuke/Natron/SCRATCH) load, enumerate and instantiate it? | **Natron, headless, once** (2026-11-24): `testkit/verify-bundle.bat` loads it in the official OFX host, then renders one 256×256 frame through `NatronRenderer` and checks the pixels. Resolve, Nuke and SCRATCH remain unverified, and no GUI session has run. |
| Does `describe`/`describeInContext`/`render` behave under a host's real suite set? | **Partly verified**: the fake host in [`testkit/`](../testkit/) drives those actions (110 checks), holds two instances at once and checks their separate values, but it is not a host. |
| Do the licence texts of the models, fonts and runtimes in the bundle travel with it? | **Not checked by any build.** A bundle can install and render with no notice file; only a file listing proves it. The copy step and the warning are in `packaging-and-install.md` §1. |
| Does the pixel output match a reference? | **Unverified for this plug-in.** The ONNX template has golden-file tests; the reference gain has no golden. |
| GPU, Windows/MSVC, macOS/Clang paths | **Unverified.** Only Linux x86-64 with GCC 13.3 was built. |
| Host-specific quirks (blacklist behaviour, bundle caching, install-time locking) | **Unverified.** See [`host-resolve.md`](host-resolve.md), graded B there. |

The artifact inspection in `testing-layers.md` §3 proves the module is loadable and that the
registration hook produces the expected `OfxPlugin`. It cannot prove what a host
does with it.

---

## 6. Anti-patterns, by symptom

| Symptom | Usual cause | Fix |
| --- | --- | --- |
| Black frame, no error | GPU advertised but not compiled in; or `kOfxImageEffectPropCudaEnabled` read as a string | Advertise only under `#ifdef`; use `propGetInt` **[S]** |
| Plugin vanished from the menu | Host blacklisted it after a crash | Clear the host's plugin cache (§1) |
| Renders on CPU, "CUDA is on" | Device code not in the binary | `testing-layers.md` §3, `cuobjdump`/`nm` |
| Image sheared on odd widths | `width * sizeof(pixel)` instead of the host pitch | Use `rowBytes`, widen to `int64_t` **[S]** |
| Flipped vertically | Negative `rowBytes` treated as unsigned | Signed pitch, arithmetic through `char*` **[S]** |
| First frame hitches, then fine | Module load + PTX JIT on first launch | `CUDA_MODULE_LOADING=EAGER`, ship SASS for the target **[D]** |
| Other nodes stall when yours renders | `cudaMalloc`/`cudaFree`/`cudaDeviceSynchronize` inside render | Cache allocations; never sync when a stream was given **[D]** |
| Host crash minutes later, elsewhere | Plugin destroyed a borrowed GPU object | Never own the stream/queue **[R]** |
| Different result on GPU vs CPU | Two divergent copies of the maths | shared definition or tolerance test |
| Slow only in Fusion | Different host pipeline, not your kernel | Establish the page first **[R]** |

---

## 7. Anti-patterns and their tells

Reviewing someone else's plug-in is mostly a grep exercise: each tell below is a
search, and the row says what the code found that way will do to a frame. The
list is ordered roughly by how often it is the cause.

| Anti-pattern | Tell in the code | Why it fails | Do instead |
|---|---|---|---|
| Pitch assumed tight (`width * 4 * sizeof(float)` as the stride) | `width * 4 * sizeof(float)`, or `width * 16` | Hosts pad rows - Resolve renders 208x117 thumbnails through 256-byte-aligned allocators - so the image shears at odd sizes and looks fine in one host | `getRowBytes()`, signed 64-bit |
| Base pointer treated as the render window | no `(window.y1 - bounds.y1)` term before the row multiply | Offset renders (crop, RoD) shift the image | Offset by the window's position inside the bounds before the row multiply |
| `int` pitch arithmetic | `int pitch = getRowBytes();` | Overflow on large frames | `int64_t` for the pitch and every offset derived from it |
| `getPixelAddress()` per pixel | call inside the `x` loop | An out-of-line call per pixel in the Support library | Compute the first address once, then advance by the pitch |
| `cudaDeviceSynchronize()` in `render` | `grep -rn cudaDeviceSynchronize src/` | Explicitly forbidden; serialises the host's whole GPU pipeline | Enqueue on the host stream; sync only the stream you used, only when none was given |
| Device-to-device `cudaMemcpy` into a private buffer | `cudaMemcpy` in a render path with device pointers | Doubles bandwidth and defeats zero-copy | Read the host pointer directly |
| Allocation in `render()` | `cudaMalloc`/`new`/`std::vector` growth inside the render action | Per-frame cost and fragmentation; breaks `FullySafe` claims | Allocate once per sequence/size, reuse, release in `endSequenceRender()` or `purgeCaches()` |
| Freeing device memory in the destructor | `cudaFree` in the destructor | The host may destroy the instance while tearing down its GPU stack; a CUDA call there can fault | Keep the destructor free of CUDA calls; release in `endSequenceRender`/`purgeCaches` |
| Static CUDA state, or a session in a global | `static` session, static scratch buffer | Two instances or two render threads share it | Per-instance members, or a device-keyed cache with a mutex |
| Catching everything and returning `kOfxStatOK` | `catch (...) { return kOfxStatOK; }` or a bare `return` | The host composites a wrong frame with no log | Map failures to specific `kOfxStat*` codes |
| `new` + manual `delete` around `fetchImage` | `OFX::Image* img = clip->fetchImage(t);` then `delete` | Any intervening `throw` leaks the host image | `std::unique_ptr` at construction |
| Omitting `setGPURenderArgs` | `processor.process()` without it | The CUDA path silently never runs; the CPU path hides it | Always call it before `process()` |
| Tile claim without context | `setSupportsTiles(true)` plus no `getRegionsOfInterest` | The host hands over a tile and the effect treats it as a whole frame | Implement `getRegionsOfInterest` and render only what was asked for |
| Different maths in the CPU and GPU paths | two copies of the loop, one under `#ifdef` | Divergence found by a client | One header or function as the source of truth |
| Testing only in the viewer | no render-to-file step anywhere in the plan | The viewer path is not the render path | Render to file, and to the delivery codec |
| Drawing the overlay before tiling | overlay drawn inside the tile loop | The overlay enters the model's receptive field and changes with tile size | Draw after inference, once per frame |

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[D]** stated by official documentation
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[R]** stated by a reference project
- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
