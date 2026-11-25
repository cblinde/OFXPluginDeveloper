# CUDA streams, zero-copy and GPU memory ownership

The stream contract in full, why cudaDeviceSynchronize is the wrong call in both modes, what zero-copy actually means, and who owns GPU objects.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 183-311 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 402-422 -->
## 1. Zero copy: what it actually means

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

## 2. Streams: the contract, in full

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
| **Yes** | Enqueue on it. Return immediately. Do not sync. Do not call `cudaDeviceSynchronize()` at any time — the quote above is a SHOULD NOT, and the host's stream is what makes waiting unnecessary. |
| **No** | Enqueue on the default stream (or your own) and **synchronise that stream before returning**. |

### Why `cudaDeviceSynchronize()` is the wrong call in both modes

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

## 3. Streams, and what changes when the host gives you one

- The stream property is `pointer X 1` in the `inArgs` of **Render**,
  **BeginSequenceRender** and **EndSequenceRender** **[S]**. Treat it as opaque:
  store as `void*`, cast at launch, never create, never destroy.
- It is only set when *both* sides advertise
  `kOfxImageEffectPropCudaStreamSupported = "true"` **[S]**. The template
  advertises it and does not depend on it.
- With a host stream: enqueue and return. The host orders its own work against
  that stream, and the spec says not to wait — *"the plug-in SHOULD NOT wait for
  final asynchronous operations to complete… SHOULD NOT call
  cudaDeviceSynchronize() at any time"* **[S]**.
- Without a stream: your work must be complete before `render` returns **[S]**.
  That is the one case where a synchronise is mandatory rather than forbidden.
  The template implements exactly this branch **[S]**.
- Do not create your own stream per frame. If you need private ordering, create
  it once per instance and let the host stream be the join point, or just use
  the host's.

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
