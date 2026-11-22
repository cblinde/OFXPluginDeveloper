# Architecture and the CUDA pipeline

Deep-dive companion to [`SKILL.md`](SKILL.md). Everything here assumes the
OpenFX 1.5-era headers (`ofxImageEffect.h`, `ofxGPURender.h`, `ofxParam.h`), the
C++ Support library (`OFX::ImageEffect`, `OFX::ImageProcessor`), and CUDA 12/13.

**Provenance.** **[V]** verified in this repository (compiled, measured, or
quoted from the pinned 1.5.1 headers); **[I]** inferred from a header or from the
API contract, not demonstrated; **[B]** external evidence — a vendor document or
a shipped project, grade B in `HOST_COMPATIBILITY_MATRIX.md`. Unmarked text is
design guidance.

---

## 1. Object model in one page

A host and a plugin share nothing but C ABI structs. Three ideas cover all of it:

- **Handles** are opaque pointers: `OfxImageEffectHandle`, `OfxImageClipHandle`,
  `OfxPropertySetHandle`, `OfxParamSetHandle`.
- **Properties** are typed, indexed, string-keyed key/value bags attached to
  handles. Every constant in `SKILL.md` §5 is a key into one of those bags.
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

## 4. Zero copy: what it actually means

"Zero copy" here means **the plugin never copies pixels it could read in place**:

```
Resolve's GPU frame buffer
        |  (host passes the pointer in kOfxImagePropData)
        v
   your kernel  ------------->  the same buffer family, output image
        ^                              ^
        |                              |
   reads src directly            writes dst directly
```

What the plugin *may* copy:

- Parameters, lookup tables, small coefficient arrays — unavoidable and cheap.
- A tile into shared memory inside the kernel — free, on-chip, not a "copy" in
  this sense.

What the plugin must **not** do:

- `cudaMemcpyAsync(dst, src, ..., DeviceToDevice)` into a private staging buffer
  "to be safe". That doubles bandwidth and adds latency without adding safety.
- `cudaMemcpy` host↔device around the kernel. If you see this in a GPU render
  path, either the host is not in CUDA mode (so use the CPU path) or the design
  is wrong.

**Why the pointers are directly usable.** The OFX host creates its GPU buffers in
a CUDA context. If the host uses the CUDA *runtime* API (the usual case), that is
the per-device **primary context**, and your plugin's runtime calls bind to the
same primary context automatically. Two independent libraries that both use the
runtime API therefore share allocations with no explicit context sharing.
If the host instead uses the *driver* API and pushes its own `CUcontext`,
your runtime calls also attach to the current context — which is why you must not
switch devices carelessly (see below).

**Multi-GPU is where this breaks.** A workstation can have Resolve rendering on
GPU 1 while your thread's current device is 0. Then `kOfxImagePropData` belongs
to device 1 and a launch on device 0 raises `cudaErrorInvalidDevicePointer` (or
reads garbage). Bind to the pointer's owner:

```cpp
int device = 0;
cudaGetDevice(&device);
cudaPointerAttributes attrs{};
if (cudaPointerGetAttributes(&attrs, src) == cudaSuccess && attrs.device >= 0) {
    device = attrs.device;
}
// RAII: restore the previous device on scope exit, always.
```

This is exactly what `LaunchGainRGBA32F()` in the template does, and the
`DeviceGuard` restores the thread's device so the host's next call is not
surprised.

---

## 5. Streams: the contract, in full

The specification is short and unambiguous (`ofxGPURender.h`,
`kOfxImageEffectPropCudaStream`):

> If set: this property contains a pointer to the stream of CUDA render
> (`cudaStream_t`). In order to use it, `reinterpret_cast<cudaStream_t>(pointer)`
> is needed. The plug-in SHOULD ensure that its render action enqueues any
> asynchronous CUDA operations onto the supplied queue. The plug-in SHOULD NOT
> wait for final asynchronous operations to complete before returning from the
> render action, and SHOULD NOT call `cudaDeviceSynchronize()` at any time.
>
> If not set: the plug-in SHOULD ensure that any asynchronous operations it
> enqueues have completed before returning from the render action.

Quoted verbatim from `ofxGPURender.h` lines 608–617 **[V]**. Read that as two
modes:

| Host supplied a stream | Plugin behaviour |
| --- | --- |
| **Yes** | Enqueue on it. Return immediately. Do not sync. Do not call `cudaDeviceSynchronize()` — ever. |
| **No** | Enqueue on the default stream (or your own) and **synchronise that stream before returning**. |

### Why `cudaDeviceSynchronize()` is banned

It is not about performance. `cudaDeviceSynchronize()` blocks until *every*
stream on the device is idle — including the host's own queued work for other
instances, the UI, and its colour pipeline. Inside a render call it can deadlock
(some hosts submit work from another thread that needs a lock the render thread
holds) and it destroys the frame pipelining the host built. The stream you were
handed is the host's ordering primitive; respect it and nobody has to block.

### Why the sync in the no-stream case is mandatory

Without a stream, the host has no handle to order against. It will assume that
when `render()` returns, the pixels are valid — sometimes immediately, e.g. when
it maps the buffer for a preview or a write. An unsynchronised kernel then races
the host's read, producing tearing, stale frames, or a crash in the GPU driver.
This is why "always async, never sync" is wrong advice: it is only correct in
mode 1.

### Default stream pitfalls

- The legacy default stream (stream 0) synchronises implicitly with other
  blocking streams. Fine for a plugin, but it serialises: do not launch the same
  kernel on stream 0 *and* on the host's stream.
- Compiling with `--default-stream per-thread` changes what `nullptr` means.
  Keep the default (`legacy`) unless the whole stack agrees.
- `cudaStreamSynchronize(nullptr)` synchronises the default stream only — the
  correct call for mode 2. `cudaDeviceSynchronize()` is not.

### Ordering you get for free, and ordering you must not assume

With a host stream, your kernel is ordered after everything the host enqueued on
that stream before the render call, and the host's later reads are ordered after
your kernel. You may **not** assume anything about work on *other* streams.

### NVTX, so the profiler can see you

Wrap the launch in an NVTX range so the render shows up as a named band in Nsight
Systems instead of an anonymous kernel:

```cpp
#include <nvtx3/nvToolsExt.h>
nvtxRangePushA("MyEffect::render");
// ... launch ...
nvtxRangePop();
```

---

## 6. Memory layout, pitch and pixel formats

### The one formula

For an image with bounds `[x1,x2) × [y1,y2)`, data pointer `p`, row stride `s`
bytes, and `b` bytes per pixel:

```
address(x, y) = p + (y - y1) * s + (x - x1) * b
```

`y - y1` and `x - x1` are **not** `y` and `x`. Bounds frequently do not start at
zero: a generator with a centred RoD, a filter whose input is padded, a host
rendering a sub-window all produce negative or non-zero origins.

### Why `width * sizeof(pixel)` is wrong

Hosts pad scanlines. Concretely, RGBA32F is 16 bytes/pixel:

| Width | Naive row size | Host row size (256 B aligned) | Error if you assume naive |
| --- | --- | --- | --- |
| 1920 | 30 720 B | 30 720 B | none — which is why this bug survives testing on HD |
| 1919 | 30 704 B | 30 720 B | 16 B/row ⇒ 16 px shear after one row |
| 208 (thumbnail) | 3 328 B | 3 328 B | none |
| 117-wide odd tile | 1 872 B | 1 920 B | 48 B/row ⇒ visible skew |

And a stride can be **negative**: a vertically flipped image is legally
represented by pointing at the last row and using `-rowBytes`. Signed 64-bit
arithmetic handles it; `size_t` does not.

### Component and depth matrix

| Depth | Constant | Bytes/component | Notes |
| --- | --- | --- | --- |
| 8-bit | `kOfxBitDepthByte` | 1 | Video-range vs full-range is a host concern; ask, don't assume |
| 16-bit | `kOfxBitDepthShort` | 2 | Integer; 65535 = 1.0 convention |
| half | `kOfxBitDepthHalf` | 2 | `__half`, needs `sm_53`+; watch NaN/Inf on extremes |
| float | `kOfxBitDepthFloat` | 4 | The sane default for grading and for GPU work |

| Components | Constant | Pixel size at 32f |
| --- | --- | --- |
| RGBA | `kOfxImageComponentRGBA` | 16 B |
| RGB | `kOfxImageComponentRGB` | 12 B (no alpha!) |
| Alpha | `kOfxImageComponentAlpha` | 4 B |

Declare what you support in `describe()` (`addSupportedBitDepth`) **and** per
clip (`addSupportedComponent`), and reject anything else in `render()` with
`kOfxStatErrUnsupported`. If you accept RGB and then read `p[3]`, you read the
next pixel's red — a bug that looks like a colour shift, not a crash.

### Window vs bounds vs RoD

| Concept | Property | Meaning |
| --- | --- | --- |
| Bounds | `kOfxImagePropBounds` | Pixels actually present in this image |
| Render window | `kOfxImageEffectPropRenderWindow` (in-args) | The subset you must write this call |
| RoD | `kOfxImagePropRegionOfDefinition` | The region the image *conceptually* covers |
| RoI | `getRegionsOfInterest` args | What you ask the host to provide on input |

`renderWindow ⊆ bounds` must hold. When tiling is enabled you get many render
calls per frame, each a slice; when it is disabled you get one. The template
declares `setSupportsTiles(false)`, so the window equals the bounds — and
`ImageProcessor::process()` asserts that, which is a cheap safety net.

### Allocating your own device memory

Prefer `OFX::ImageMemory` (wraps `OfxMemorySuiteV1`) when the allocation should be
visible to the host's memory accounting, e.g. a cached result the host may want
to purge. Use `cudaMalloc`/`cudaMallocAsync` for pure scratch. Either way:

- Allocate **once per sequence or per size change**, never per frame.
- `cudaMalloc` is synchronising; it will show up as a stall in Nsight.
- Free in `endSequenceRender()` or on size change; keep the destructor simple.

---

## 7. RoD and RoI

Both are in **canonical coordinates**, which are the project's coordinate space —
not the image's pixel space at the current render scale.

### Region of Definition

Override `getRegionOfDefinition()` only when the effect changes the output
extent: a generator, a crop, a transform, a lens distortion that grows the frame.
Return the new rect in canonical coordinates and `return true`. For a per-pixel
colour operation, do nothing — the default (union of inputs) is correct, and
overriding it wrongly clips the image.

### Region of Interest

For anything reading neighbours (blur, sharpen, edge detect, morphology,
optical-flow-guided warp):

```cpp
void MyEffect::getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                    OFX::RegionOfInterestSetter& rois)
{
    // radius expressed in pixels AT THE CURRENT RENDER SCALE
    const double rx = _radius * args.renderScale.x;
    const double ry = _radius * args.renderScale.y;

    OfxRectD roi = args.regionOfInterest;
    roi.x1 -= rx; roi.x2 += rx;
    roi.y1 -= ry; roi.y2 += ry;

    rois.setRegionOfInterest(*_srcClip, roi);
}
```

The classic bug: adding the pixel radius directly to canonical coordinates.
At `renderScale = 0.5` the host gives you a half-size image, so you asked for
twice the neighbourhood you need — or, at 2× proxy scale, too little, and seams
appear at tile boundaries only in proxy mode.

Second classic bug: expanding the RoI but then addressing the kernel relative to
the *render window* rather than the *fetched image bounds*. The image you get back
may be larger than the window (that is the point of RoI), and its origin will
differ.

---

## 8. Colour space and alpha

OFX deliberately has **no colour management**. The host hands you pixels in
whatever working space it uses and expects the same space back. Therefore:

1. **Never bake in a transform you were not asked for.** A "gain" that silently
   converts Rec.709 to linear is a bug in every project that already works in
   linear.
2. **Be explicit about what you assume, in the UI and the docs.** If your effect
   only makes sense in linear light (blurs, lens effects, physically-based
   compositing), expose a `Colour space` choice param with an explicit default of
   "as-is / no conversion".
3. **Parameters that are colour values** should be labelled with their space.

Common working spaces you will meet:

| Space | Where | Shape of the data |
| --- | --- | --- |
| Linear / scene-linear | Nuke (default), ACES projects | 0..1 nominal, values >1 legal |
| DaVinci Wide Gamut / Intermediate | Resolve colour-managed projects | log-ish, wide gamut |
| ACEScct | ACES pipelines | log with a linear toe |
| Rec.709 / sRGB | straight video, Edit page | display-referred, gamma encoded |
| Resolve "Timeline" space | Resolve, unmanaged project | whatever the clip is tagged as |

Consequence for CUDA code: **negative and >1.0 values are normal.** Do not clamp
in the kernel unless the user asked for it, do not use `saturate()` by reflex,
and do not use 8-bit intermediate maths. If you need a LUT, apply it in the space
it was built for.

**Premultiplication.** Check `kOfxImageEffectPropPreMultiplication` on the image
(`kOfxImagePreMultiplied`, `kOfxImageUnPreMultiplied`, `kOfxImageOpaque` — see
`ofxImageEffect.h` for the exact string values). Rules:

- Blur, glow, defocus, transform: only correct on **premultiplied** data. If
  you get unpremultiplied input, divide by alpha, process, multiply back.
- Colour-only operations (gain, lift/gamma/gain, matrix, LUT): safe on either,
  *provided* you do not touch alpha.
- Never write non-zero RGB where alpha is zero unless the host asked for
  premultiplied output — it produces halos on the next composite.

**Alpha quirks to expect.**

- Alpha may be absent entirely (`RGB`), in which case treat it as 1.0 for maths
  and never write it.
- Some hosts deliver "opaque" images where alpha exists but is constant; others
  deliver garbage alpha on the Color page where compositing is not the point.
  Use the premultiplication property, and verify per host/version rather than
  trusting a memory of how it used to behave.
- Resolve's Color page evaluates OFX in its own pipeline, and where alpha lands
  there has changed between releases. Test explicitly; see
  `HOST_COMPATIBILITY_MATRIX.md`.

---

## 9. AI pipelines: TensorRT and ONNX Runtime on OFX device pointers

The pattern for an AI-based OFX plugin (upscale, denoise, matte) is identical in
shape to the CUDA kernel path, with three additions: a model, a persistent
inference session, and a binding layer that must not copy.

### Where the pointers go

Both runtimes accept device pointers directly, which is what makes zero-copy
possible **[B]** — their published APIs, not a measurement here:

| Runtime | API | What you pass |
| --- | --- | --- |
| TensorRT | `IExecutionContext::setTensorAddress(name, ptr)` then `enqueueV3(stream)` | The OFX `kOfxImagePropData` pointer, cast to `void*` |
| ONNX Runtime | `Ort::IoBinding::BindInput/BindOutput` with `Ort::MemoryInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault)` | Same pointer |
| ONNX Runtime | `session.RunWithBinding(runOptions, ioBinding)` | — |
| ONNX Runtime | CUDA EP `user_compute_stream` provider option | The OFX `cudaStream_t`, so ORT enqueues on your stream |

**Do not** bind host memory and let the runtime copy. That is three transfers per
frame and it negates the reason for a GPU plugin.

### Shape and layout mismatch

Models want NCHW planar float32. OFX gives you interleaved RGBA. Something has to
convert, and it should be a kernel:

```
RGBA32F (interleaved)  --kernel-->  NCHW planar (+ optional resize/normalise)
                                     |
                                     v
                              inference (same stream)
                                     |
                                     v
NCHW planar            --kernel-->  RGBA32F (interleaved, cropped to RoD)
```

Both conversion kernels enqueue on the host stream, so the whole chain is ordered
without a single host-side synchronise.

### Lifecycle and the traps

1. **Load lazily.** `createInstance` may run before a CUDA context exists.
   Build the engine/session on the first `render()` (or in
   `beginSequenceRender`), keyed by `(device, input shape)`.
2. **Never rebuild per frame.** Engine deserialisation is tens to hundreds of
   milliseconds; per-frame it is unusable.
3. **Budget the VRAM.** A 4K RGBA32F frame is 66 MB; a planar NCHW float buffer
   for a 3-channel 4K tensor is 199 MB. Cache buffers per shape, free on shape
   change.
4. **Handle "no tensor cores" gracefully.** Query
   `cudaDeviceGetAttribute(cudaDevAttrComputeCapabilityMajor/Minor)` and select a
   precision (FP32/FP16/INT8) you actually support on that GPU.
5. **Free device resources before the host tears down.** Prefer
   `endSequenceRender()`; keep the destructor allocation-free. Destroying a
   TensorRT execution context during host shutdown is a known way to crash on
   exit, and an exit crash looks like your plugin to the user.
6. **Never let the runtime create its own stream and assume ordering.** If ORT/TensorRT
   will not accept yours, enqueue on your own stream and synchronise **that**
   stream before returning when the host gave you none. See §5.
7. **Ship the model as a bundle resource.** `NAME.ofx.bundle/Contents/Resources/`
   is the documented place for non-code assets; resolve it from the plugin's own
   path, never from an absolute path on the build machine.

---

## 10. Toolchain notes that cause real failures

### `nvcc` flags

| Flag | Why |
| --- | --- |
| `-lineinfo` | Line mapping for Nsight Compute with minimal optimisation loss. Ship release builds with it. |
| `-G` | Full device debug. Disables most optimisation — debug builds only. |
| `--use_fast_math` | Off by default. Enables it changes results (denormals, `sin`/`cos` precision). A grading plugin should not silently change numerics. |
| `-Xptxas -v` | Register/spill report; check it when occupancy is bad. |
| `-arch=sm_XY` / CMake `CUDA_ARCHITECTURES` | Target list, see `SKILL.md` §6. |

### PTX and first-launch cost

A binary that carries only PTX for the newest architecture JIT-compiles on first
use, which shows up as a multi-second stall when the user first applies the
effect. Ship SASS for the architectures you care about (`75;80;86;89;90;100;120`)
and PTX for the newest only.

### Static vs dynamic `cudart`

| | `cudart` (shared) | `cudart_static` |
| --- | --- | --- |
| Depends on runtime DLL/SO at load | Yes | No |
| CUDA context | Shared with the host | Per-plugin instance of the runtime |
| Failure mode | Missing/incompatible `libcudart` → plugin fails to load | Two runtimes in one process; undefined behaviour if they both manage the same allocations |
| Recommendation | Default | Only for closed deployment targets, and never alongside another dynamic-cudart plugin |

The mismatch that bites: the host ships its own CUDA runtime version (Resolve
bundles one), your plugin links another, and the two disagree about the ABI of
`cudaPointerAttributes` or the primary context refcount. The visible symptom is
"plugin does not load" or a crash in `cuInit`. Keep the plugin's `cudart`
requirement at or below the version the host ships, and prefer the runtime
interface that has been stable for years.

### Windows specifics

- The `.ofx` is a DLL: `PREFIX ""`, `SUFFIX ".ofx"`.
- MSVC runtime must match across the CUDA host compiler and the C++ TUs
  (`/MD` Release, `/MDd` Debug). Mixing allocators across that boundary corrupts
  the heap, usually much later than the actual bug.
- Delay-loading (`/DELAYLOAD:cudart64_*.dll` + `delayimp.lib`) lets the plugin
  load on machines without CUDA and fail only when the GPU path is used — with a
  clean error instead of a load failure the host reports as "plugin broken".
- Do not install to `C:\Program Files\...` from the build; use
  `build.bat Release install` and let the user run it elevated.

### Linux specifics

- Shared objects must be PIC (`POSITION_INDEPENDENT_CODE ON`); CUDA objects too.
- `-fvisibility=hidden` plus explicit exports for the two mandated entry points
  (the Support library already marks them). Two plugins with default visibility
  will interpose each other's Support symbols **[I]**.
- The visibility flag has to reach the CUDA objects too. In CMake that is a
  **separate** target property, `CUDA_VISIBILITY_PRESET`: `CXX_VISIBILITY_PRESET`
  alone leaves every `extern "C"` launcher in `Kernel.cu` exported (measured on
  this template: 5 `T` symbols instead of 2) **[V]**.
- `libcudart.so.*` must be findable: either installed by the toolkit, or shipped
  next to the `.ofx` and found via `RPATH=$ORIGIN`. Shipping it is often the
  difference between "works on my machine" and "works on the colourist's".

---

## 11. Profiling and diagnosis

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

`OFX::Log::print()` routes through `OfxMessageSuite`. It is **printf-style** —
`print(const char* format, ...)` — so pass a literal format and arguments
(`print("launch failed: %s", msg)`), not a `std::string`: concatenating a
`std::string` and passing it as the format argument does not compile.
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
`HOST_COMPATIBILITY_MATRIX.md` **[B]**; none of them was reproduced here.

---

## 12. End-to-end walkthrough of the template

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
