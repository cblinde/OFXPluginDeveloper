# Nuke, SCRATCH and Natron

The parts that differ from Resolve: no GPU buffer path, eager RoD, tiling, error surfacing, and Natron's headless renderer.

<!-- HOST_COMPATIBILITY_MATRIX.md lines 195-293 -->
## 1. Nuke

Nuke is the strictest and most spec-literal of these hosts, which makes it the
best place to find out that your code was relying on host leniency.

### 1.1 No GPU buffer path for OFX

Nuke's OFX host presents CPU images; the CUDA/OpenCL render properties are not
offered to third-party plugins **(C)**. Plugins that are GPU-accelerated in Nuke
(denoisers, for example) create and manage their own device context internally
and copy frames in and out.

Implication for architecture: if Nuke is a target, the CPU path is the product,
and GPU is an optional accelerator for other hosts. The layer split in
[`../SKILL.md`](../SKILL.md) §1 exists precisely so that this is a configuration, not a
rewrite.

If you must use the GPU inside Nuke:

- Create your own context/stream, `cudaMemcpy2DAsync` in (honouring `rowBytes`),
  run, copy out. That is a legitimate exception to "no copies", because the host
  never gave you device memory.
- Budget the transfer: 4K RGBA32F is 66 MB per direction, per frame.
- Never call `cudaDeviceSynchronize()`; sync your own stream. Nuke renders many
  tiles in parallel and a device-wide sync serialises them all.

### 1.2 Tiling and concurrency

Nuke tiles and multithreads aggressively **(C)**. Two consequences:

- A plugin that assumes `renderWindow == bounds` breaks visibly at tile seams.
  The template sidesteps this by declaring `setSupportsTiles(false)`, which Nuke
  honours — at the cost of parallelism, which is the right trade for a first
  version.
- Nuke may call `render()` concurrently on the **same instance**. Unless you have
  audited shared state, declare `eRenderInstanceSafe`, not `eRenderFullySafe`;
  or implement your own mutex around the shared part.

### 1.3 RoD and RoI are evaluated eagerly

Nuke evaluates `getRegionOfDefinition` and `getRegionsOfInterest` outside the
render, sometimes well before the user renders. These actions must be **cheap and
side-effect free**: no allocation, no GPU work, no logging spam, no reads of
images.

### 1.4 Errors are surfaced, not swallowed

Nuke reports thrown OFX statuses to the user with the message from the suite.
That is a benefit: throw specific codes and set a message via
`setPersistentMessage`/`sendMessage` so the user sees "GPU out of memory" instead
of "unknown error".

---

## 2. Assimilate SCRATCH

SCRATCH's OFX host is comparatively close to the spec and is one of the hosts the
Gyroflow OpenFX plugin explicitly supports **(B**: documented on the project's
install page and in its host-support matrix).

What to expect:

- Filter context with standard source/output clips; RoD/RoI honoured.
- GPU path availability varies with the SCRATCH build and the machine's GPU
  stack **(C)**. Treat it as "may or may not be enabled" and rely on the runtime
  flag rather than the host name.
- Fewer non-standard extensions than Resolve, so code that works in Nuke
  usually works here.

Strategy: use SCRATCH as a second data point when a Resolve bug might be your
fault. If it renders correctly in Nuke and SCRATCH but not Resolve, look for a
Resolve-specific assumption (page, working space, render size, alpha) rather than
a kernel bug.

---

## 3. Natron

Natron is open source and useful precisely because it is *not* the product you
are shipping to: it is a free, scriptable, mildly incomplete OFX host that will
happily expose a declared property you never actually handle.

What to expect:

- CPU images for OFX; no CUDA render path **(C)**.
- Tiled, multithreaded rendering.
- Incomplete support for some optional suites: always `fetchSuite` and check for
  `NULL`, including `OfxMessageSuiteV2`, progress and timeline suites.

Use Natron for:

- A zero-cost smoke test of the CPU path and the parameter UI.
- Confirming that the plugin degrades when an optional suite is missing.
- Reproducing a spec violation in isolation, without a $300 licence in the loop.

Do not use Natron to validate GPU behaviour: it will not exercise it.

---
