# DaVinci Resolve: behaviour, quirks and blacklisting

Pages, the GPU switch, absent streams, odd sizes, crash blacklisting, Free vs Studio, and the failure case study.

<!-- HOST_COMPATIBILITY_MATRIX.md lines 25-194 -->
<!-- HOST_COMPATIBILITY_MATRIX.md lines 356-377 -->
<!-- HOST_COMPATIBILITY_MATRIX.md lines 1-24 -->
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

**Measured in both modes, 2026-11-26.** `testkit/verify-bundle.bat <build> --plugin-id
com.ofxplugindeveloper.referencegain --effect "Reference Gain" --resolve` ran tier 3
twice on Studio 21.2.0.5 - once with this switch on Auto, once on CUDA - and the
host's own log line is what separates them: Auto writes `Compute API set to
automatic, defaulting to CUDA.` and then `Selected compute API: CUDA`, the explicit
setting writes only the second line. Both sessions applied Reference Gain to one
generated 1920x1080 still, rendered `poc00108000.png` and decoded it (**[V]**,
[`resolve-tier3-auto.log`](../evidence/2026-11-26-windows/resolve-tier3-auto.log),
[`resolve-tier3-cuda.log`](../evidence/2026-11-26-windows/resolve-tier3-cuda.log)).
Two things follow for anything that tries to test this switch: the mode is stated
only in that log line, so a run has to quote it rather than assume it; and it is a
setting, not a command-line flag. Switching to CUDA wrote
`Local.GPU.Mode = CUDA` into
`%APPDATA%\Blackmagic Design\DaVinci Resolve\Preferences\config.dat`, a file that
held no such key while the switch was on Auto - 3964 bytes before the switch, 3986
with the key, and 3964 again once the key was removed, which is how the machine was
left. Whether the GUI writes an explicit `Auto` for a user who switches back was
not observed, so Auto is "the key is absent" only for the state this run started in.

**Resolve owns its CUDA stack, and it loads it at startup.** Listed from outside
while Resolve Studio 21.2.0.5 ran (2026-11-26), the process holds twelve
CUDA-related modules: `cudart64_12`, the legacy `cudart64_110`, `cublas64_12`,
`cublasLt64_12`, `cufft64_11`, `cudnn64_9` and `nvrtc64_120_0` out of its install
folder, and the driver's `nvcuda64`, `nvapi64` and `nvml` out of `System32`
([`resolve-modules.txt`](../evidence/2026-11-26-windows/resolve-modules.txt)).
The split cuDNN libraries, `cudnn_graph64_9`, `cudnn_ops64_9` and the rest, ship
in that folder but are not loaded, which is what a cuDNN dispatcher looks like
before it runs an algorithm. Two consequences: the mode switch above decides
whether your plug-in is *offered* a CUDA path, not whether CUDA is present; and a
bundle that preloads `cudnn64_9.dll` or `cudart64_12.dll` by path puts a second
module of that name into this process. What to ship instead is in
[`packaging-and-install.md`](packaging-and-install.md).

### 2.3 Streams: expect none

Resolve is not observed to populate `kOfxImageEffectPropCudaStream` **(C)**.
The practical consequence is that the *mode 2* branch in
[`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) §5 is the
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
worked table in the architecture doc `ofx-lifecycle-and-threads.md` §6.

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

Contents include the headers that build shipped (the 18.5-era SDK is OFX-1.4),
the C++ Support library, and a `GainPlugin` with CUDA/OpenCL/Metal kernels plus
temporal and random-access variants. The SDK's version is **not** evidence of the
host's API version: Resolve 21 implements OFX 1.5 colour management while the
installed developer folder may still be 1.4-era (§2.9). Point `OFX_SOURCE_DIR` at
that directory to build the template without any network access:

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

### 2.9 Version currency: 21.2, and the OFX 1.5 colour APIs

Resolve Studio **21.2** (2026-11-05) is current, with project libraries kept
compatible with 20.3.2; **21.0.3** (2026-07) lists "Addressed issue with GPU
support in OpenFX plugins" among its fixes **(B**, Blackmagic forum release posts
and 21.0.3 notes). Two consequences for a plug-in author:

- **Colour management is now a host capability, not a hope.** Resolve 21
  implements the OpenFX 1.5 colour-management APIs **(B**, the Resolve 21 New
  Features Guide). A colour-manipulating effect can declare
  `kOfxImageEffectPropColourManagementStyle` and read
  `kOfxImageClipPropColourspace` instead of guessing the working space. Read the
  style from the instance and degrade to the pre-1.5 assumption (`None`, no
  conversion) when the host reports nothing; never branch on the host name.
- **Do not promote that into "Resolve supports 1.5.1".** The three 1.5.1
  additions (`kOfxImageEffectPropCPURenderSupported`,
  `kOfxImageEffectPropNoSpatialAwareness`, `kOfxImageEffectPropThumbnailRender`)
  are separate optional properties, and no public Resolve note was found that
  names them **(C)**. Set them only on the plugin descriptor, keep the default
  behaviour correct without them, and treat their absence as normal.

A GPU-mode regression fixed in a point release is also the reason the matrix
separates "the host offers CUDA" from "your kernel ran": a Resolve update can
change whether the device path is taken at all, so re-run the §1 table after one.

### 2.10 What this template cannot do in Resolve

A negative list, because a positive one hides the misses. That this bundle rendered
inside Resolve is a matrix row, not a sentence: `verification-matrix.tsv` holds the
host, the path, the date and the committed report for each run, and
`verification-status.md` §1 repeats that table so `tools/check-docs.py` can compare
the two in both directions. The host *facts* below still stay **(B)** (they come
from a Resolve Lite 16 profile dump) and what they imply for this build stays
**(I)**.

- **The Resolve-like host shape is now a profile, not a gap.**
  `contract_test --host-profile resolve` presents `supportsTiles=0` and
  `supportsMultiResolution=0`, the shape dumped from Resolve Lite 16 in
  `openfx-misc`'s `README-hosts.txt` (API 1.4, no tiles) **(B)**, and every suite
  runs it once per profile. What that still does not prove: that Resolve's own
  loader, tile decisions and proxy handling match the harness. The template
  declares `setSupportsTiles(false)` and `setSupportsMultiResolution(false)`, so
  the un-tiled path is the one Resolve would take - and now the one a run takes.
- **The device path depends on the host not capturing the runtime.** Resolve 21
  carries `onnxruntime.dll` 1.13 beside `Resolve.exe`; the bundle's renamed core
  `onnxruntime-ofx.dll` is what survives that, and the CUDA provider next to it
  is loaded from the bundle rather than from `PATH`
  ([`packaging-and-install.md`](packaging-and-install.md) §1). None of it has
  been observed inside Resolve.
- **No colour management.** The template declares no
  `kOfxImageEffectPropColourManagementStyle` and reads no clip colourspace, so a
  project in ACES or in a log working space gets whatever the host hands over,
  converted by nobody.
- **One input, no temporal access, no transition, no generator.** Four descriptor
  features the template does not declare, so a Resolve user looking for them in
  the effect's UI finds nothing ([`ofx-api-constants.md`](ofx-api-constants.md) §1.1).
- **No render of the pixel path, no crash data.** The 2026-11-25 frame is a
  pass-through, so `render()` never had to run; the blacklist behaviour in §2.5
  and the GPU-mode switch in §2.2 stay host facts to design against, not
  observations of this plug-in, and the CUDA path has never been offered to it.

---

## 3. Known real-world failure, as a case study

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

## 4. Host compatibility matrix

Deep-dive companion to [`../SKILL.md`](../SKILL.md). What differs between OpenFX hosts,
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
