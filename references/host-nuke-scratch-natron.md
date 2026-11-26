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

### What this template cannot do in Nuke

- **The device path is unreachable by design.** Nuke offers no CUDA/OpenCL render
  properties to third-party plug-ins **(C)**, so the inference template's CUDA
  attempt fails there and the label records why; the CPU session is the product.
- **No tiles.** `setSupportsTiles(false)` is honoured - correct, and it gives up
  Nuke's tile parallelism.
- **Same-instance concurrency is not claimed.** The descriptor says
  `eRenderInstanceSafe`, so Nuke may run several instances in parallel; two
  `render()` calls on one instance are outside what this template promises, and
  no suite here drives that pattern.
- **Nothing rendered in Nuke.** There is no Nuke on any machine in this
  repository, so the three points above are host facts plus their consequences,
  not a session with this build.

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

### What this template cannot do in SCRATCH

- **GPU availability is a property of the host build and the machine's stack**
  **(C)**, not something the plug-in can assume; the template falls back per
  frame and says so in the label.
- **No tiles, no multi-resolution.** The same two declarations as everywhere
  else, so SCRATCH renders it un-tiled.
- **Nothing verified in SCRATCH at all.** No install here, and what §2 says about
  the host comes from the Gyroflow project's install notes **(B)**, not from a
  session with this template.

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

### What this template cannot do in Natron

- **No GPU path for OFX images** **(C)**: Natron gets the CPU session, and the
  failed CUDA attempt shows up in the label rather than in the frame.
- **Untiled rendering.** `setSupportsTiles(false)` is safe here and leaves the
  host's tile parallelism unused.
- **One headless render is not a Natron test.** `NatronRenderer` rendered a frame
  of one template on 2026-11-24 (`evidence/2026-11-24-windows/`), which proves a
  host can load a bundle headless; it says nothing about this plug-in's CUDA path
  or about a Natron timeline.

---

**Two hosts that arrived after this list was written.** Pomfort Livegrade
Frontier, the on-set grading tool, now offers an OFX host beside its own pipeline
(**[B]**, Pomfort's 2026 product notes), and EffectCraft merged an opt-in OpenFX
host in October 2026, so a plug-in appears there only when the user enables it
(**[B]**, the project's pull request). Neither has been run here, and both fall
under the same rule as the three hosts above: probe capabilities, never the host
name - and assume a CPU image path until that host's own documentation says it
offers a GPU one.
