# Host compatibility matrix

Deep-dive companion to [`SKILL.md`](SKILL.md). What differs between OpenFX hosts,
what breaks, and what to do instead.

**Read the evidence grade before you rely on a row.** This document distinguishes
what is in the specification from what has been observed in public bug reports
from what is simply reported by users. A compatibility matrix that does not make
that distinction is worse than none, because it turns a guess into a design
decision.

| Grade | Meaning |
| --- | --- |
| **A** | Stated in the OFX specification/headers, or deterministic from the API contract |
| **B** | Reproducible public evidence: a shipped plugin's issue tracker, vendor forum post from the host's own engineers |
| **C** | Community report or vendor documentation summary; plausible, unverified here — **test before designing around it** |

Nothing in this file was produced by running a host. No DaVinci Resolve, Nuke,
SCRATCH or Natron installation was available where this skill was authored. Treat
every row as a hypothesis with a named source, and confirm it in your own matrix
before shipping.

---

## 1. Master table

| | **DaVinci Resolve Studio** | **Nuke** | **SCRATCH** | **Natron** |
| --- | --- | --- | --- | --- |
| OFX host quality | Broad, some proprietary extensions | Close to spec, strict | Close to spec | Reference-ish, incomplete |
| CUDA buffer path (`kOfxImageEffectPropCudaEnabled`) | Yes, when GPU mode = CUDA **(B)** | Not offered to OFX plugins; CPU images **(C)** | Version-dependent **(C)** | No; CPU only **(C)** |
| CUDA stream (`kOfxImageEffectPropCudaStream`) | Not supplied — plugin must complete work before returning **(C)** | N/A | Unknown **(C)** | N/A |
| OpenCL path | Yes | No | Yes in some builds **(C)** | No |
| Metal path | No (macOS uses Metal for Resolve itself, not exposed to OFX) **(C)** | No | No | No |
| Render threading | Multiple instances in parallel; per-instance serial **(B)** | Concurrent `render` on one instance — needs `FullySafe` **(C)** | Parallel frames **(C)** | Worker pool, concurrent tiles **(C)** |
| Tiling (`kOfxImageEffectPropSupportsTiles`) | Yes; plugin may opt out | Yes; Nuke tiles aggressively **(C)** | Yes **(C)** | Yes |
| Multi-resolution (`renderScale`) | Yes (proxy / half-res previews) | Yes | Yes **(C)** | Yes |
| Temporal / random frame access | **Studio only**; free version limited **(B)** | Yes | Yes **(C)** | Yes |
| Alpha on the Color page | Inconsistent across versions **(B/C)** | Well-defined via premultiplication property | Well-defined **(C)** | Well-defined |
| Unusual render sizes | Yes — thumbnails at sizes like 208×117 **(B)** | Yes — odd tile sizes | Yes **(C)** | Yes |
| Crashing plugin handling | Blacklists plugin; needs cache clear + Preferences re-enable **(B)** | Removes plugin for the session | Logs | Logs |

Legend: **A** spec, **B** reproducible public evidence, **C** reported/unverified.

---

## 2. DaVinci Resolve Studio

Resolve is the primary target for this skill: it is where most CUDA OFX work
lands, and where most of the environment's quirks live.

### 2.1 Pages behave differently

The same plugin instance can be evaluated by three different engines:

| Page | How the effect is applied | Consequences |
| --- | --- | --- |
| **Color** | As a node in the colour pipeline, in the project's working space | Data may be linear or log depending on project colour management; alpha is often meaningless; render windows can be small (thumbnails, viewer zoom) |
| **Edit** | As an OFX filter on a clip, near-video semantics | Working space tends to be the timeline space; alpha meaningful; different render sizes |
| **Fusion** | As an OFX tool inside a Fusion comp | Different threading and caching; parameters may be evaluated at comp time, not clip time; RoD/RoI can be evaluated more eagerly |

Practical rules:

- **Never assume the page.** Read `getProjectSize()`, the image's
  `kOfxImageEffectPropPreMultiplication`, and the pixel depth at render time.
- **Never assume a single render size.** The viewer at 25% zoom, the thumbnail
  strip, and the delivery render are three different `renderWindow`s, sometimes
  in the same second.
- **Do not cache per-frame results across pages**; parameter values can be
  re-evaluated with a different `renderScale`.

### 2.2 The GPU mode switch is user-controlled and can be "Auto"

Preferences → System → Memory and GPU → *GPU processing mode* selects OpenCL,
CUDA or Metal. The default is Auto. Consequences:

- On an AMD Windows/Linux machine, Resolve will be in OpenCL mode and
  `isEnabledCudaRender` will be **false forever**. A plugin that only implements
  `processImagesCuda()` and throws otherwise is unusable on those machines. This
  is the strongest argument for the CPU fallback in the template: it is not
  politeness, it is coverage.
- On Apple silicon, CUDA does not exist at all. If you advertise CUDA on macOS,
  you are advertising a path that cannot run.
- Users switch modes to troubleshoot; your plugin must survive both.

### 2.3 Streams: expect none

Resolve is not observed to populate `kOfxImageEffectPropCudaStream` **(C)**.
The practical consequence is that the *mode 2* branch in
[`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) §5 is the
one that runs in Resolve almost all the time: you must complete your launch before
`render()` returns.

This is why the template synchronises the default stream when
`_pCudaStream == nullptr`, and why "never synchronise" is wrong advice. The
correct instruction is "never synchronise *when the host gave you a stream*".

If you advertise `setSupportsCudaStream(true)` and Resolve never uses it, nothing
breaks — but the flag buys nothing there either. Advertise it for hosts that do
use it; keep the no-stream branch correct for Resolve.

### 2.4 Odd render sizes and row-byte alignment

Resolve renders thumbnails and zoomed viewers at sizes like 208×117. Public
plugin-development experience reports that uploading those to GPU APIs which
require 256-byte row alignment fails validation unless rows are padded **(B)**.

For a raw CUDA pointer path there is no upload — but the lesson transfers: the
host's `rowBytes` is authoritative and is not `width * bytesPerPixel`. See the
worked table in the architecture doc §6.

### 2.5 Crashing or hanging plugins are blacklisted

Resolve records the outcome of plugin loading and skips plugins that previously
crashed or hung, and the Preferences → Video Plugins list lets the user re-enable
them **(B)**. The cache lives at:

| Platform | Path |
| --- | --- |
| Windows | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` (older builds: `OFXPluginCache.xml`, and `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\`) |
| macOS | `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/` |
| Linux | `~/.local/share/DaVinciResolve/` (varies by build) |

Operational consequence: after you fix a crash, **the plugin may still not appear**.
Clear the cache (host closed), then re-enable in Preferences. Do this before
concluding that a fix did not work — this wastes more debugging hours than any
kernel bug.

### 2.6 Free vs Studio

Multi-frame random access for OFX plugins is a **Studio** feature (introduced
around 12.5) **(B)**. A temporal plugin (retime, optical flow, temporal denoise)
that depends on `getFramesNeeded()` will receive only the current frame in the
free version. Design temporal effects to degrade to a single frame rather than
producing wrong output silently.

### 2.7 Resolve's own developer material

Resolve ships OpenFX samples with the application; they are the closest thing to
an official reference for its GPU expectations:

| Platform | Path |
| --- | --- |
| Windows | `C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX\` |
| Linux | `/opt/resolve/Developer/OpenFX/` |
| macOS | `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/` |

Contents include the OFX-1.4 headers, the C++ Support library, and a `GainPlugin`
with CUDA/OpenCL/Metal kernels plus temporal and random-access variants. Point
`OFX_SOURCE_DIR` at that directory to build the template without any network
access:

```bash
OFX_SOURCE_DIR=/opt/resolve/Developer/OpenFX ./build.sh
```

(Use the `OpenFX-1.4` subdirectory if the SDK nests one.)

### 2.8 Do not rely on Resolve-only extensions

Resolve has historically exposed non-standard proprietary conform extensions and
does not implement every optional OFX suite **(B)** — a Blackmagic engineer has
said as much publicly. If you need one, feature-detect it (`fetchSuite` returning
`NULL`) and degrade. A plugin that hard-requires a Resolve extension cannot be
tested anywhere else, and cannot be validated against the spec.

---

## 3. Nuke

Nuke is the strictest and most spec-literal of these hosts, which makes it the
best place to find out that your code was relying on host leniency.

### 3.1 No GPU buffer path for OFX

Nuke's OFX host presents CPU images; the CUDA/OpenCL render properties are not
offered to third-party plugins **(C)**. Plugins that are GPU-accelerated in Nuke
(denoisers, for example) create and manage their own device context internally
and copy frames in and out.

Implication for architecture: if Nuke is a target, the CPU path is the product,
and GPU is an optional accelerator for other hosts. The layer split in
[`SKILL.md`](SKILL.md) §3 exists precisely so that this is a configuration, not a
rewrite.

If you must use the GPU inside Nuke:

- Create your own context/stream, `cudaMemcpy2DAsync` in (honouring `rowBytes`),
  run, copy out. That is a legitimate exception to "no copies", because the host
  never gave you device memory.
- Budget the transfer: 4K RGBA32F is 66 MB per direction, per frame.
- Never call `cudaDeviceSynchronize()`; sync your own stream. Nuke renders many
  tiles in parallel and a device-wide sync serialises them all.

### 3.2 Tiling and concurrency

Nuke tiles and multithreads aggressively **(C)**. Two consequences:

- A plugin that assumes `renderWindow == bounds` breaks visibly at tile seams.
  The template sidesteps this by declaring `setSupportsTiles(false)`, which Nuke
  honours — at the cost of parallelism, which is the right trade for a first
  version.
- Nuke may call `render()` concurrently on the **same instance**. Unless you have
  audited shared state, declare `eRenderInstanceSafe`, not `eRenderFullySafe`;
  or implement your own mutex around the shared part.

### 3.3 RoD and RoI are evaluated eagerly

Nuke evaluates `getRegionOfDefinition` and `getRegionsOfInterest` outside the
render, sometimes well before the user renders. These actions must be **cheap and
side-effect free**: no allocation, no GPU work, no logging spam, no reads of
images.

### 3.4 Errors are surfaced, not swallowed

Nuke reports thrown OFX statuses to the user with the message from the suite.
That is a benefit: throw specific codes and set a message via
`setPersistentMessage`/`sendMessage` so the user sees "GPU out of memory" instead
of "unknown error".

---

## 4. Assimilate SCRATCH

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

## 5. Natron

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

## 6. Feature detection, not host sniffing

The matrix above is for *design*; the code must not branch on host name. Branch
on capabilities:

```cpp
// Once, in describe() or onLoad(): what can this host do at all?
OFX::ImageEffectHostDescription* host = OFX::getImageEffectHostDescription();

const bool hostHasCuda        = host && host->supportsCudaRender;
const bool hostHasCudaStream  = host && host->supportsCudaStream;
const bool hostHasOpenCL      = host && host->supportsOpenCLRender;
const bool hostHasMetal       = host && host->supportsMetalRender;
const bool hostMultipleDepths = host && host->supportsMultipleClipDepths;
const bool hostTiles          = host && host->supportsTiles;

// Per render: did the host actually choose it THIS time?
if (args.isEnabledCudaRender) { /* device pointers */ }
else                          { /* host pointers  */ }
```

Same for suites:

```cpp
const OfxMessageSuiteV2* messages =
    static_cast<const OfxMessageSuiteV2*>(OFX::fetchSuite(kOfxMessageSuite, 2, /*optional*/ true));
if (messages) { /* richer messages */ } else { /* V1 or silent */ }
```

Never write `if (hostName == "DaVinci Resolve")`. It breaks on the next rebrand,
lies in a render farm with mixed versions, and hides real bugs behind a
workaround that was only needed in 2019.

---

## 7. The compatibility test you should actually run

Per host, per GPU mode, per page where applicable:

| # | Test | Catches |
| --- | --- | --- |
| 1 | Plugin appears in the effect list | Bundle name/arch/entry-point problems |
| 2 | Apply at default settings → image unchanged (`isIdentity`) | Identity not wired, or a silent gain≠1 |
| 3 | Change one param → correct render | The basic path |
| 4 | Render to file at 1920×1080 | Viewer-only bugs |
| 5 | Render at 1919×1081 | Pitch/stride bugs |
| 6 | Render a thumbnail / zoomed viewer | Odd render sizes, `renderWindow ≠ bounds` |
| 7 | Proxy / half-res playback | `renderScale` handling |
| 8 | Switch GPU mode (CUDA ↘ CPU/OpenCL) | The CPU fallback actually existing |
| 9 | Two instances of the effect in one comp | Shared state, thread safety |
| 10 | Two **different** plugins installed | Symbol visibility |
| 11 | Non-square pixel aspect clip | PAR handling |
| 12 | Log/ACES project vs Rec.709 project | Colour assumptions |
| 13 | Clip with alpha, then without (`RGB`) | Component-count assumptions |
| 14 | Scrub the timeline while rendering | Async lifetime bugs |
| 15 | Quit the host after rendering | Teardown crashes (device memory freed too late) |

Record the result as a table in the plugin's own repository. That table is the
only compatibility claim you can defend.

---

## 8. Known real-world failure, as a case study

`gyroflow/gyroflow-plugins` issue #64 (August 2026) documents 32 DaVinci Resolve
crashes in 24 days, 31 of them attributable to one line: the plugin released a
`MTLCommandQueue` it did not own. Sixteen of the crashes were
`[queue commandBuffer]` on a freed queue **(B)**.

The lesson generalises exactly to CUDA:

- The host's stream, command queue, context and buffers are **borrowed**. You may
  enqueue; you may not release.
- A GPU object lifetime bug often survives the immediate frame and crashes later,
  in a different host subsystem, with a stack that names the *host's* code. That
  is why the crash report blames the host and why the plugin author never sees it.
- Refcount/ownership mistakes in GPU interop are not caught by CPU-side tests,
  sanitizers or code review of the kernel. They are caught by a rule: if you did
  not allocate it, you do not destroy it.

The template encodes that rule structurally: `GainProcessor` holds no CUDA
handles at all, the stream is a borrowed `void*`, and the destructor releases
nothing but its own C++ members.
