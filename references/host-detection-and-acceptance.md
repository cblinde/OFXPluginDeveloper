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
