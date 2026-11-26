# Feature detection and the host acceptance run

Probe capabilities, never sniff the host name - and the test that decides whether a host accepts the plug-in.

<!-- HOST_COMPATIBILITY_MATRIX.md lines 294-355 -->
## 1. Feature detection, not host sniffing

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

## 2. The compatibility test you should actually run

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
## 3. Hosts that appeared in 2026

The hosts this repository has run against are listed elsewhere; these two are not
in any of them, and a list that stops at the ones someone happened to own reads
as if it were closed. Both facts are **[B]** — vendor documentation, not a run
here.

| Host | What it hosts | What it demands | What that means for these templates |
| --- | --- | --- | --- |
| Pomfort Livegrade Frontier | OpenFX image-effect nodes inside a live look pipeline, applied to camera signals in real time; frontier edition only, plugin folders are the standard `/Library/OFX/Plugins` and `~/Library/OFX/Plugins` ([knowledge base](https://kb.pomfort.com/livegrade/reference/frontier-features/openfx-effect-node)) | **Metal only**: images arrive as Metal textures and never as CPU buffers, the plugin must be an RGBA filter, and it must contain native Apple silicon code; OpenFX 1.5 or newer ([Pomfort's developer documentation](https://github.com/pomfort/openfx-plugins)) | Nothing here loads in it: every GPU path in this repository is CUDA on Windows and Linux, and the CPU path is not offered by the host. It is the concrete reason a Metal template exists as a row of its own, and the first host that makes Metal the product rather than a port |
| EffectCraft | an opt-in OpenFX host (`--features openfx`) that loads native `.ofx`/`.ofx.bundle` plugins as ordinary effects, off by default and the only crate allowed `unsafe` ([PR #508](https://github.com/storytold/effectcraft/pull/508), [host docs](https://github.com/glumbis/effectcraft/blob/openfx/docs/ofx.md)) | a **CPU** image-effect host at OpenFX 1.4, with a loader that checks the API version and refuses a module for another version | The one new host a CPU build of these templates could plausibly meet today, and the reason not to claim it: nobody has run that, and the host is a fork feature at an older API version than the headers here. It is a test target for whoever has the build, not a compatibility claim |

The OpenFX project's own October 2026 status update names the first one -
"New interest from Pomfort: Livegrade Frontier now has an OpenFX host" - which is
where the list should have been updated in the first place
([ASWF TAC update, 2026-10-14](https://tac.aswf.io/meetings/2026-10-14/OpenFX_ASWF_TAC_Update_Oct_2026.pdf)).

---
