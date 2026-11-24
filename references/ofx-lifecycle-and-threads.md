# OFX lifecycle, the layer contract and thread safety

Object model, what happens at plugin/instance/render level, which calls may block, and the five seams of the design.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 17-182 -->
<!-- SKILL.md lines 118-139 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 704-750 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 1-16 -->
## 1. Object model in one page

A host and a plugin share nothing but C ABI structs. Three ideas cover all of it:

- **Handles** are opaque pointers: `OfxImageEffectHandle`, `OfxImageClipHandle`,
  `OfxPropertySetHandle`, `OfxParamSetHandle`.
- **Properties** are typed, indexed, string-keyed key/value bags attached to
  handles. Every constant in `ofx-api-constants.md` §1 is a key into one of those bags.
- **Suites** are tables of function pointers the host exposes
  (`OfxImageEffectSuiteV1`, `OfxPropertySuiteV1`, `OfxParamSuiteV1`,
  `OfxMemorySuiteV1`, `OfxMultiThreadSuiteV1`, `OfxMessageSuiteV1/V2`,
  `OfxImageEffectOpenGLRenderSuiteV1`, `OfxProgressSuiteV1`,
  `OfxTimeLineSuiteV1`), fetched through `OfxHost::fetchSuite`.

Consequences that matter for CUDA work:

1. **Everything is discoverable at runtime, nothing is guaranteed.** Query, then
   degrade. `host->fetchSuite` returning `NULL` is a normal condition.
2. **Property lookups are cheap but not free.** Cache values that are fixed per
   instance (`_gain`, `_dstClip`), never per pixel.
3. **The plugin never owns host objects.** Handles returned by `clipGetImage`
   must be released with `clipReleaseImage`; handles returned by `fetchImage`
   are released by the `OFX::Image` destructor. Neither is yours to `free`.

The Support library hides most of this: `OFX::ImageEffect` maps actions onto
virtual methods, and `OFX::ImageProcessor` maps the GPU/CPU decision onto
`processImagesCuda()` / `multiThreadProcessImages()`.

---

## 2. Lifecycle

Two levels: **plugin** actions (one shared library, no instance) and **instance**
actions (one node in a comp).

### Plugin level

| Action | When | What you may do | What you must not do |
| --- | --- | --- | --- |
| `kOfxActionLoad` | Once, on `dlopen` | Fetch suites, read host descriptor | Touch any image, param or instance; create a CUDA context |
| `kOfxActionDescribe` | Once per API version | Set descriptor properties, capability flags, param-independent metadata | `fetchImage`, define clips |
| `kOfxImageEffectActionDescribeInContext` | Once per context | `clipDefine`, define params, pages and groups | Read param *values* |
| `kOfxActionUnload` | On `dlclose` | Release global resources | Anything that may run after the host tore down its context |

### Instance level

| Action | Support virtual | Purpose |
| --- | --- | --- |
| `kOfxActionCreateInstance` | constructor | Fetch clips and params; **do not** assume a GPU context exists |
| `kOfxImageEffectActionBeginSequenceRender` | `beginSequenceRender()` | Establish per-sequence GPU state (allocate, load model, warm up) |
| `kOfxImageEffectActionRender` | `render()` | Produce one frame/window |
| `kOfxImageEffectActionEndSequenceRender` | `endSequenceRender()` | Release per-sequence GPU state |
| `kOfxImageEffectActionIsIdentity` | `isIdentity()` | Say "output == input" without rendering |
| `kOfxImageEffectActionGetRegionOfDefinition` | `getRegionOfDefinition()` | Change output size |
| `kOfxImageEffectActionGetRegionsOfInterest` | `getRegionsOfInterest()` | Ask for a bigger input region |
| `kOfxImageEffectActionGetClipPreferences` | `getClipPreferences()` | Force output depth/components |
| `kOfxImageEffectActionGetFramesNeeded` | `getFramesNeeded()` | Temporal access |
| `kOfxActionInstanceChanged` | `changedParam()` / `changedClip()` | React to UI changes |
| `kOfxActionPurgeCaches` | `purgeCaches()` | Free what you can live without |
| `kOfxActionSyncPrivateData` | `syncPrivateData()` | Push internal state into params |
| `kOfxActionDestroyInstance` | destructor | Free your GPU resources |

### Non-obvious rules

- **The order is Render*(Begin, Render…, End)***. Never assume one
  `BeginSequenceRender` per `Render` at the same time; a host may interleave
  several instances' sequence renders on different threads.
- **Render threads are a restricted context.** Only
  `BeginSequenceRender`, `Render`, `EndSequenceRender`, `IsIdentity`,
  `GetFramesNeeded`, `GetRegionOfDefinition`, `GetRegionsOfInterest` are legal
  there. In particular you may **not** call the param suite to *set* values from
  `render()` on another thread without locking.
- **`createInstance` runs before any GPU context is guaranteed.** Doing
  `cudaSetDevice`/`cudaMalloc` in the constructor creates a context on a device
  the host may not be using. Defer to the first `render()` or to
  `beginSequenceRender()`.
- **Destruction can happen late.** `DestroyInstance` may run while the host is
  shutting down its GPU stack. Freeing CUDA objects at that point can fault.
  Prefer releasing device memory in `endSequenceRender()` and keeping the
  destructor allocation-free (the template's destructor holds no CUDA state at
  all, which is why it is safe).

### Thread safety

State your level honestly in `describe()` via `setRenderThreadSafety`:

| Value | Contract | Use when |
| --- | --- | --- |
| `kOfxImageEffectRenderUnsafe` | One render at a time, process-wide | You share global/static GPU state |
| `kOfxImageEffectRenderInstanceSafe` | One render per instance, instances in parallel | Per-instance state only (default, safest) |
| `kOfxImageEffectRenderFullySafe` | Any number of concurrent renders per instance | You have proved there is no shared mutable state |

The template uses `eRenderInstanceSafe`, which is the correct answer for a
stateless kernel. Claiming `FullySafe` without auditing shared allocations is a
race inside the host process — it will not be caught by sanitizers you cannot
run on the host.

The claim is now exercised, not assumed: `testkit/` holds several live instances
with separate parameter values and renders one frame per instance from one thread
per instance, repeating the comparison for a `FullySafe` plug-in on a single
instance **[V]**. Two caveats that measurement produced:

1. **The declaration is only as true as the library under it.** Every OFX action
   calls `OFX::Log::indent()` (`Support/Library/ofxsImageEffect.cpp:2569`), which
   increments a plain global counter (`ofxsLog.cpp:63`) with no lock. A TSan build
   of a template therefore reports a race between two concurrent renders *inside
   the Support library*, before any plug-in code runs **[V]**. The counter is
   cosmetic, but the same shape in a session cache or a lazily built graph would
   be a real bug — audit what you add, not what you declare.
2. **TSan only sees instrumented code.** The suite's TSan pass covers the harness;
   to see races in the plug-in image, build the plug-in with `-fsanitize=thread`
   as well. A clean functional run with an uninstrumented binary proves behaviour,
   never the absence of races.

---

## 3. The GPU negotiation handshake

Advertising CUDA is a four-step negotiation. The plugin participates in all of
them, and getting the semantics of *each* step right is the difference between a
working GPU plugin and silent black frames.

```
        plugin descriptor                 host descriptor
   kOfxImageEffectPropCudaRenderSupported = "true"     <-- step 1: offer
                          |
                          v
   host decides, per render action, whether to use CUDA for this instance
                          |
                          v
   inArgs: kOfxImageEffectPropCudaEnabled = 1          <-- step 2: decision
                          |
                          v
   image kOfxImagePropData = cudaDevicePtr             <-- step 3: it is a device pointer
                          |
                          v
   inArgs: kOfxImageEffectPropCudaStream = ptr | absent <-- step 4: async contract
```

**Step 1 — offer.** `setSupportsCudaRender(true)` on the descriptor, and
`setSupportsCudaStream(true)` only if you can genuinely enqueue onto an arbitrary
`cudaStream_t`. If you set stream support and then ignore the stream, you break
ordering against the host's own work: the host may read the image buffer before
your kernel writes it.

**Step 2 — decision.** At render time, read `kOfxImageEffectPropCudaEnabled`
(the Support library exposes it as `args.isEnabledCudaRender`) **[V]**. If it is 0, the
pointers are **host** pointers, whatever the descriptor said. This is the single
most common source of "invalid device pointer" errors: a plugin that advertised
CUDA and then assumed it was always on.

**Step 3 — location.** With CUDA enabled, `kOfxImagePropData` for *every* clip
is a device pointer. Do not `memcpy`, `strlen`, `printf` or otherwise dereference
it on the CPU. Pointer arithmetic is safe (it is just integer maths on the
handle); dereferencing is not.

**Step 4 — asynchrony.** See §5.

**When the host says no.** If `isEnabledCudaRender` is false, fall through to the
CPU path. Do *not* copy host memory to the device to "use the fast path": you
would be adding two PCIe transfers per frame to save nothing. Conversely, if the
host says yes, do *not* stage through host memory — that is the entire point of
zero-copy.

---

## 4. The layer contract

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

## 5. End-to-end walkthrough of the template

`templates/minimal-cuda-ofx` implements every rule above in ~450 lines **[V]**
(it builds, and its artifact is inspected by `build.sh`).

```
render(args)
 └─ setupAndProcess()
     ├─ unique_ptr<Image> dst = _dstClip->fetchImage(args.time)
     ├─ unique_ptr<Image> src = _srcClip->fetchImage(args.time)
     ├─ processor.setGPURenderArgs(args)      // picks CUDA vs CPU
     ├─ processor.setRenderWindow(args.renderWindow)
     └─ processor.process()
         ├─ args.isEnabledCudaRender ? processImagesCuda() : multiThread(...)
         │
         ├─ processImagesCuda()
         │   ├─ srcPitch = _src->getRowBytes()            // signed, bytes
         │   ├─ srcOrigin = base + (win.y1 - b.y1)*pitch + (win.x1 - b.x1)*16
         │   ├─ LaunchGainRGBA32F(stream, ...)            // Kernel.cu
         │   └─ if (stream == nullptr) WaitForDefaultStream()
         │
         └─ multiThreadProcessImages(window)               // same maths, CPU
```

Where each invariant lives:

| Invariant | Location |
| --- | --- |
| Device-vs-host decision | `ImageProcessor::process()` (Support) driven by `setGPURenderArgs` |
| Pitch-correct addressing | `processImagesCuda()`, host side |
| Boundary check | both kernels — `if (x >= width \|\| y >= height) return;` |
| 16-byte access legality | `IsVec4Addressable()` gates `GainKernelRGBA32FVec4`; a misaligned base or pitch falls back to `GainKernelRGBA32FScalar` |
| Stream contract | `processImagesCuda()` — the `if (_pCudaStream == nullptr)` branch |
| Device binding | `LaunchGainRGBA32F()` + `DeviceGuard` |
| Launch error check | `cudaGetLastError()` immediately after `<<<>>>` |
| Launcher linkage | `extern "C"` in `Kernel.cuh` and `Kernel.cu`, so nvcc and the host compiler cannot disagree about mangling |
| Resource release | `std::unique_ptr<OFX::Image>` in `setupAndProcess()` |
| Identity short-circuit | `GainPlugin::isIdentity()` |

To adapt it to a new effect, change: the parameter block in `describeInContext`,
the maths in `multiThreadProcessImages`, the maths in `ScalePixel` (shared by both kernels), and
the argument struct passed to the launcher. Everything else — bundling,
visibility, stream policy, device binding — should be left alone. Those are the
parts that took the longest to get right.

---

## 6. Architecture and the CUDA pipeline

Deep-dive companion to [`../SKILL.md`](../SKILL.md). Everything here assumes the
OpenFX 1.5-era headers (`ofxImageEffect.h`, `ofxGPURender.h`, `ofxParam.h`), the
C++ Support library (`OFX::ImageEffect`, `OFX::ImageProcessor`), and CUDA 12/13.

**Provenance.** **[V]** verified in this repository (compiled, measured, or
quoted from the pinned 1.5.1 headers); **[I]** inferred from a header or from the
API contract, not demonstrated; **[B]** external evidence — a vendor document, a
shipped project, or the archived 2026-11-22 hardware run
([`field-evidence.md`](field-evidence.md)) — grade B in
`references/host-resolve.md`. Unmarked text is
design guidance.

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[V]** verified in this repository - the command ran here
