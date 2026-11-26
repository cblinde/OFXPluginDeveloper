# Testing: the layers, cheapest first

L0-L10, what each proves and cannot prove, the failure-injection table, and what this repository actually runs.

<!-- TESTING_STRATEGY.md lines 1-955 -->
## Testing OFX plug-ins

How to test an OFX plug-in well enough to ship it, in the order that catches
failures cheapest-first. This is the "how"; [`checklists.md`](checklists.md)
is the "did you". Every mechanism below is either demonstrated by a test in this
repository (with its measured output) or described with the exact API it needs —
and where nothing here can test it, the section says so instead of implying
coverage.

**The one structural fact to internalise:** an OFX plug-in can be tested in two
very different ways.

| Without a host | Only with a host |
|---|---|
| The maths, packing, tiling, formats | Viewer path vs render-to-file path |
| Registration and ABI (dlopen, exports, struct fields) | Real thread scheduling and `abort()` timing |
| Error codes for bad input | Host image lifetime, caching and refcounting |
| Concurrency of your own state | Crash blacklisting, project save/reload, undo |
| Memory safety, UB, leaks | Per-host quirks (RoD, proxies, colour management) |
| Performance of everything except host calls | Whether the effect looks right on real footage |

A product needs both columns. Most plug-ins that fail in the field pass the left
column perfectly and never ran the right one.

---

## Contents

- [1. The layers, cheapest first](#1-the-layers-cheapest-first)
- [2. L0: separate the maths from OFX](#2-l0-separate-the-maths-from-ofx)
- [3. L1: the artifact, exports, dlopen, struct fields](#3-l1-the-artifact-exports-dlopen-struct-fields)
- [4. L2: static conformance gates](#4-l2-static-conformance-gates)
- [5. L3: a fake host, for action-level tests](#5-l3-a-fake-host-for-action-level-tests)
- [6. L4: image correctness, goldens, references, negative controls](#6-l4-image-correctness-goldens-references-negative-controls)
- [7. L5: memory, UB and leaks](#7-l5-memory-ub-and-leaks)
- [8. L6: concurrency and the thread-safety claim](#8-l6-concurrency-and-the-thread-safety-claim)
- [9. L7: performance, measured or not claimed](#9-l7-performance-measured-or-not-claimed)
- [10. L8: the host acceptance script](#10-l8-the-host-acceptance-script)
- [11. L9: failure injection](#11-l9-failure-injection)
- [12. L10: CI and the release gate](#12-l10-ci-and-the-release-gate)
- [13. What this repository actually runs](#13-what-this-repository-actually-runs)
- [14. Test smells: passing for the wrong reason](#14-test-smells-passing-for-the-wrong-reason)
- [15. L3b: observing a feature that leaves no trace](#15-l3b-observing-a-feature-that-leaves-no-trace)

---

## 1. The layers, cheapest first

| Layer | What it tests | Cost to build | What it catches | Here |
|---|---|---|---|---|
| **L0** core | maths, packing, tiling, formats | minutes | wrong pixels, wrong tensor layout | the templates |
| **L1** artifact | exports, dlopen, plugin struct | minutes | "plug-in does not appear", symbol leakage | the templates: `testkit/`, 3 × (110 + 136 + 160) checks, one pass per host profile (the 136 from a `CPU_ONLY=1` build, so host protocol + CPU path; the 160 are followed by 82 in the tiled-context variant) |
| **L2** static | property types, forbidden calls, bundle layout | minutes | type confusion, spec violations | `test/run-analysis.sh`, [`checklists.md`](checklists.md) §2 |
| **L3** fake host | actions, lifecycle, error codes, params | a day | wrong status codes, crashes on describe/create/render | the templates: `testkit/fake_host.*` |
| **L4** images | goldens, references, geometry matrix | a day | seams, shear, alpha loss, off-by-one | the templates |
| **L5** memory | ASan, UBSan, TSan, valgrind | an hour | UB, leaks, data races | the templates |
| **L6** concurrency | thread-safety claims | an hour | races in per-instance state | kernel reentrancy proven |
| **L7** performance | ms/frame, allocations, peak memory | a day | per-frame allocation, sync stalls | **not measured here** |
| **L8** host | real host behaviour | hours, repeatedly | everything that only a host reveals | **not possible here** |

L0–L6 run on a laptop with no GPU and no host. That is the point: build them
first, because they are the only layers you can run on every commit.

---

## 2. L0: separate the maths from OFX

The single design decision that makes a plug-in testable: **the pixel/tensor work
must not know about OFX.** In this repository that means
`templates/onnx-inference-ofx/src/AiCore.h` contains packing, tiling and
accumulation with no ONNX Runtime types at all — its header says so and points at
the test that exercises it. The CUDA template does the same for kernels:
`templates/minimal-cuda-ofx/src/Kernel.cuh` is includable by a compiler that has
never seen `cuda_runtime.h`, which is what lets `test/kernel_equivalence.cpp`
compile the *real* kernel source on the host.

Tests that follow from this:

- **Round-trip packing** on tight rows, padded rows with a non-zero window origin,
  and a negative row stride — the three OFX cases that shear a naive packer.
- **Alpha pass-through**: write RGB, assert channel 3 is unchanged.
- **Geometry matrix**: 1×1, 3×5, 7×5, 64×3, 3×48, and one large non-power-of-two
  size (1919×1081 is the classic; 1920×1080 hides every stride bug).
- **No writes outside the window**: fill the destination with a sentinel
  (`0x5a` in `kernel_equivalence.cpp`) and assert every byte outside the render
  window still holds it.

If a core function needs a host to call it, the split is wrong — fix the
interface, not the test.

---

## 3. L1: the artifact, exports, dlopen, struct fields

A plug-in that compiles can still be a module the host will not load. Three
checks, all cheap:

**Exports.** Exactly two strong (`T`) dynamic symbols, both registry functions —
and nothing else strong:

```bash
nm -D --defined-only <bundle>/Contents/Linux-x86-64/<Name>.ofx | grep ' T '
# OfxGetNumberOfPlugins
# OfxGetPlugin
```

Measured in this repository: all three templates report exactly 2 `T` symbols.
Removing the visibility block from `templates/minimal-cuda-ofx/CMakeLists.txt`
and rebuilding CPU-only reported **663**, because the statically linked Support
library's symbols become visible. That is the failure mode the rule exists to
prevent.

Visibility attributes do not reach **weak template instantiations**: the same
builds also export five weak `W` helpers in `MinimalCudaGain`, six in
`OnnxInference` and five in `ReferenceGain` (`std::vector<OFX::ContextEnum>`'s
`_M_realloc_insert`, …). They are 7–8 defined dynamic symbols, not 2. They cannot
interpose a strong definition, but they are not intended exports either. The
reference plug-in pins the export list with a linker version script
(`templates/minimal-ofx/exports.map`), after which `nm -D --defined-only` has
exactly two rows — see `cmake-and-linking.md` §3.5.

**dlopen and enumeration.** Twenty lines, and it catches the whole class of
"file is in the right place but the host never lists it":

```c
void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
int (*n)(void) = dlsym(h, "OfxGetNumberOfPlugins");
OfxPlugin* (*g)(int) = dlsym(h, "OfxGetPlugin");
/* assert n() == 1; then for each plugin assert: */
/*   api == "OfxImageEffectPluginAPI", apiVersion == 1,                */
/*   pluginIdentifier is reverse-DNS and stable,                       */
/*   pluginVersionMajor/Minor are what you intended,                   */
/*   setHost != NULL && mainEntry != NULL                              */
```

On Windows the same twenty lines are `LoadLibraryExW` + `GetProcAddress`, and the
path is built in the same translation unit (`#ifdef _WIN32`), not in a second
copy of the checks:

```c
HMODULE h = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
int (*n)(void) = (int (*)(void))GetProcAddress(h, "OfxGetNumberOfPlugins");
OfxPlugin* (*g)(int) = (OfxPlugin* (*)(int))GetProcAddress(h, "OfxGetPlugin");
/* same assertions; FreeLibrary instead of dlclose */
```

`LOAD_WITH_ALTERED_SEARCH_PATH` is the Windows equivalent of `$ORIGIN`: it makes
the loader look next to the `.ofx` first, which is how a plug-in finds a runtime
it ships. Measured externally: the ported harness drove a Windows bundle through
the whole protocol with 0 failures **[B]**
([`field-evidence.md`](field-evidence.md) §5).

The `OfxPlugin` fields are the ones a host reads before it calls anything
(`api`, `apiVersion`, `pluginIdentifier`, `pluginVersionMajor`,
`pluginVersionMinor`, `setHost`, `mainEntry`). A null `setHost` or `mainEntry`
means the plug-in will never receive an action.

**Struct field order.** `OfxPlugin` and `OfxHost` are C structs passed across the
ABI; a mismatch is a crash at load, not a compile error. Add a `static_assert` on
`sizeof(OfxPlugin)` if you generate bindings by hand (the Rust template's
approach, if it ever ships, needs exactly this).

**Unload.** Call `dlclose` and re-`dlopen`. Static destructors, cached sessions
and thread-local state all run there; a crash at unload looks like "the host
crashes on quit" to a user.

**Built here.** `testkit/contract_main.cpp` is the shipped version of this
recipe, and the templates run it against the binary their own `build.sh`
produced — it does not link the plug-in, it `dlopen`s it:

```bash
./run-tests.sh        # builds the templates, then runs every suite below
# per template, the step is:
testkit/build-contract-test.sh /tmp/contract_test <openfx-checkout>
/tmp/contract_test build/bundle/<Name>.ofx.bundle/Contents/Linux-x86-64/<Name>.ofx \
  --id com.ofxplugindeveloper.minimalcudagain \
  --param gain:double --param gainAlpha:double --clip Source --clip Output \
  --identity --set gain=2.0 --not-identity-after-set --byte-dst-rejected \
  --concurrency-param gain
```

Measured (CPU-only build of the CUDA template, this machine): **118 checks, 0
failed** for `ReferenceGain`, **144 checks, 0 failed** for `MinimalCudaGain`,
**168 checks, 0 failed** for `OnnxInference`, followed by **90 checks, 0 failed**
in the tiled-context run (`tileSize=16`, `--roi-context-tolerance 0.02`). Each of
those runs happens once per host profile (§5), and the counts are identical:
the profile changes what the plug-in is told about the host, not what the harness
asks it to do. Three profiles, so the totals are **354** (118 × 3), **432**
(144 × 3) and **504 + 90** (168 × 3). The 144 belong to a `CPU_ONLY=1`
build, whose launcher symbol and cubin are absent by construction (the build
prints `CUDA launcher: ABSENT (CPU-only build)`, the suite prints `PTX check
skipped: nvcc not on PATH`): not one of those checks was computed by a CUDA
kernel, so they verify the host protocol and
the CPU path of the CUDA template and leave the GPU path **[I]** until
`test/gpu-kernel-test.cu` runs on hardware (§13). The
checks are: the two export symbols; `pluginApi`, `apiVersion`, non-empty
`pluginIdentifier` (stable across a second load), `pluginVersionMajor/Minor`;
non-null `setHost`/`mainEntry`; then a whole `describe → describeInContext →
createInstance → isIdentity → render → destroyInstance → unload` sequence run
**twice in one process**, each time followed by `dlclose`; and finally the
multi-instance block from §8 — a second live instance with a different
parameter value, and one render thread per instance compared against its own
settings.

`--concurrency-param gain` is what makes that last block able to prove isolation:
it gives instance *n* its own `gain`, so pixels that used a neighbour's value fail
by the difference, not by a crash. Without the flag the block still runs, but can
only compare instances that were configured identically.

**A finding from building it, worth knowing before you copy the loop:** the OFX
1.5.1 Support library's `unloadAction()`
(`.ofx-ref/Support/Library/ofxsImageEffect.cpp:2015`–`:2016`) does
`gEffectDescriptors.find(id)` and dereferences the iterator **without checking
for `end()`**. A host that unloads a plug-in it never described therefore crashes
*inside the library*, not in the plug-in. The harness always describes first, and
a second cycle must be a full cycle — `load → describe → … → unload` — not just
`load → unload`. That crash was observed here (SIGSEGV at `mainEntryStr`) before
the second cycle was made complete.

---

## 4. L2: static conformance gates

These are grep-level checks that catch spec violations before any runtime. They
cost seconds and they are the only defence against some classes of bug.

| Gate | Check | Why |
|---|---|---|
| Property types | every descriptor `…Supported` read with `propGetString`; every per-render `…Enabled` with `propGetInt` | the strings/ints split is the most common type confusion ([`reference-projects.md`](reference-projects.md) §3.2) |
| No device-wide sync | `grep -rn cudaDeviceSynchronize src/` returns nothing | forbidden by `ofxGPURender.h:610` |
| No tight-stride assumption | `grep -rn "width \* 4 \* sizeof" src/` returns nothing | shear on padded rows |
| No `stdout` | `grep -rn "printf\|std::cout" src/` (except test code) | invisible in a GUI host |
| Bundle layout | bundle folder, binary stem and `.ofx` suffix agree | host scans that exact path |
| `RUNPATH` | `readelf -d` shows `$ORIGIN`, no build-machine path | plug-in fails on another machine |
| Capability guard | every `setSupportsCudaRender(true)` inside `#ifdef` | advertising what you did not build |

`test/run-analysis.sh` runs cppcheck and shellcheck over its own template — the
sources and the scripts, not the repository. All three templates have one, and
`static-analysis` in `linux.yml` runs all three. A
missing tool is reported as skipped rather than clean: the run ends
`static analysis: incomplete - N tool(s) ran, M skipped`, and
`REQUIRE_ANALYSIS=1` turns that into a failure. The gate list as a whole is
[`checklists.md`](checklists.md) §2.

---

## 5. L3: a fake host, for action-level tests

Everything below L3 tests functions. A **fake host** tests the plug-in as the host
sees it: `setHost` → `load` → `describe` → `describeInContext` → `createInstance`
→ `render` → `destroyInstance` → `unload`. It is the highest-value thing you can
build after the core tests, and it needs no application.

The API is small and fully specified in the headers:

| Piece | Where | Notes |
|---|---|---|
| `OfxHost` — `host` + `fetchSuite` | `.ofx-ref/include/ofxCore.h:42` | the only thing `OfxSetHost` receives |
| `kOfxPropertySuite` / `OfxPropertySuiteV1` | `.ofx-ref/include/ofxProperty.h:18`, `:23` | 8 functions; the first field is `propSetPointer` (`:38`), in declaration order |
| `kOfxImageEffectSuite` / `OfxImageEffectSuiteV1` | `.ofx-ref/include/ofxImageEffect.h:1484`, `:1490` | starts with `getPropertySet` (`:1503`); you need ~10 of these for a render test |
| `kOfxMemorySuite`, `kOfxMessageSuite` | `ofxMemory.h:12`, `ofxMessage.h:14` | a plugin that allocates or logs will fetch these |

What a minimal fake host must do:

1. Own a `std::map<std::string, value>` per property set — enough to answer the
   `propGet*` calls your `describe()` and `render()` make, and to record what you
   set.
2. Return suites from `fetchSuite(host, name, version)` by name and version,
   returning `nullptr` for anything unknown — **and assert that the plug-in copes
   with `nullptr`**, because some hosts do not provide every suite.
3. Create an effect handle, call `mainEntry(handle, kOfxActionDescribe, …)` and
   verify the parameters and clips you declared.
4. Fabricate two images (source and destination) as your own buffers with a
   chosen bounds, render window, `rowBytes` and `kOfxImagePropData`, then call
   `kOfxImageEffectActionRender` and compare pixels.
5. Assert the **status code** for every failure path: unsupported pixel depth,
   null clip, missing parameter, bad model path. A plug-in that returns
   `kOfxStatOK` on failure is invisible in a host.

What a fake host cannot prove, and must not be credited with: real host image
lifetimes and caching, real thread scheduling, host-specific RoD/proxy behaviour,
colour management, and anything GPU (a fake host has no device pointers).

**One host shape is not "the host".** For a long time the harness presented one
shape - `supportsTiles=1`, `supportsMultiResolution=1`, `temporalClipAccess=1` -
and every plug-in was therefore only ever seen by a host that could tile. Real
hosts disagree, and the disagreement is dumped: `openfx-misc`'s
`README-hosts.txt` records Resolve Lite 16 as API 1.4 with `supportsTiles=0`, and
Nuke 10 as API 1.2 with tiles and temporal access. Three profiles now present
three shapes:

| `--host-profile` | API | tiles | multi-resolution | temporal access | background | from |
|---|---|---|---|---|---|---|
| `nuke` (default) | 1.2 | 1 | 1 | 1 | 0 | the dumped Nuke 10 shape: a full host with a UI |
| `resolve` | 1.4 | 0 | 0 | 0 | 0 | the dumped Resolve Lite 16 shape: no tiles, and the free version has no temporal access either |
| `natron` | 1.5 | 1 | 1 | 1 | 1 | `NatronRenderer`: a background renderer, `kOfxImageEffectHostPropIsBackground=1`, no UI to talk to |

Every suite runs the contract once per profile, because OFX allows one host per
process - `OfxSetHost` ignores every call after the first, so the second shape is
a second process, not a second flag. Only the capabilities the plug-in reads
change; every window, RoI and pixel check is the same in every run, because a
host with `supportsTiles=0` still renders a window smaller than the frame when
its own region of definition is smaller. A run prints which shape it presented,
and the harness asserts that every property the shape carries reached the
property set - the four booleans and the API version - before it judges anything
else: a profile that half-applied would turn every later failure into a plug-in
accusation. The API version is a shape field for that reason: the dumps give
Nuke 10 1.2 and Resolve Lite 16 1.4, so a plug-in that branches on the number now
meets both instead of meeting 1.5 three times. `natron` keeps 1.5 because
`README-hosts.txt` has no dump for it - a gap in the evidence, stated rather than
filled with a guess.

**A reference per platform, a tolerance per reference.** A single
`--golden-tolerance` on the command line is one global decision, so every
platform and provider gets the same loosened rule. The harness resolves the
reference instead: `name-<platform>.ext` when that file exists (`windows`,
`macos`, `linux`, known at compile time), else `name.ext`. The bound belongs to
the reference too - `name.<fuzz>.fuzz` holds one number, read with `--fuzz cpu`
or `--fuzz cuda`, with `#` comments allowed - so the CPU reference keeps its
**exact** demand and only the reference a device cannot reproduce bit for bit
says `1e-05`. The run prints both decisions (`reference: …`, `tolerance: …
(from …)`), and the CLI flag still overrides for a one-off. A sidecar that
cannot be parsed is an error, not a quiet fallback to exact.

**The comparator has its own test.** A check is not trusted until it has been
seen failing on a defect it exists to catch, so `testkit/selftest-compare.sh
[.bat]` runs the truth table against the same function the render check uses:
identical passes; a sub-LSB pixel fails exact and passes `1e-5`; a large
deviation fails even a loose bound; every pixel just under the bound passes and
just over fails; a reference of the wrong size is a categorical failure; NaN or
infinity fails at any bound; a reference corrupt at one pixel must fail; the
platform variant wins over the generic file; the sidecar supplies the bound; the
CLI overrides it. Neutering the comparator (returning "identical" always) makes
seven of the thirteen cases fail - that run is the negative control, logged in
`../evidence/2026-11-25-windows/selftest-compare.log` and repeated by
`./run-tests.sh`.

**`SUITE_THREADS=random`.** Every suite passes `--concurrency-threads` to the
harness (4 by default). With `SUITE_THREADS=random` it draws 1-4 per run, the
way FATE's `threads=randomN` does: not for determinism, but so that a race has
to show up in some run rather than in the one configuration someone hard-coded.

**Layout is a bound, not a pixel value.** "The frame changed" is satisfied by an
overlay that is mirrored, clipped or drawn outside the area you meant, which is
exactly how a full-page card reached a host upside down while every OFX check was
green **[B]**. Assert where ink may *not* be: it stays inside the panel, both
columns carry ink (one clipped column passes a single "something was drawn"
check), nothing crosses the right edge, and a small canvas takes its short
branch. Give the harness a **size** as well: `contract_test --src/--golden
/--size WxH` renders at the size you name, because every size-dependent feature
— a label at the frame edge, a wrapped readout, a full-page overlay — is
otherwise only ever exercised at the default 64×48 image. A 1280×720 help page
auto-shrunk into 320×240 is correct behaviour that hides the layout bug.

**Built here: `testkit/`.** The SDK does not give you a host — `.ofx-ref/Support/`
contains the plug-in-side library, the sample plug-ins and `PropTester`, which is
itself a plug-in (it builds via `add_ofx_plugin`). So the host was written, once,
and the templates share it rather than each carrying a copy.

### 5.1 What is in it

| File | Lines | What it is |
|---|---|---|
| `testkit/fake_host.h` | public API | `FakeHost` plus the image options (padding, negative pitch, byte destination, an inset source region of definition) |
| `testkit/fake_host.cpp` | ~1,100 | the five suites + message suite, the object model (descriptor, instance, clips, parameters, images) |
| `testkit/contract_main.cpp` | ~500 | the checks, the `dlopen`, and the command line |
| `testkit/build-contract-test.sh` | ~50 | compiles both into one binary; finds headers from the template's `CMakeCache.txt` |
| `testkit/negative-controls/` | 2 stubs + 1 ten-way | deliberately broken plug-ins that prove the harness fails, plus one ROI stub built plain or internally tiled with the context read or ignored, twice more for the label check (`-DNO_LABEL` never writes it, `-DWITH_LABEL` does), once that draws a mark (`-DDRAW_MARK`), and once that answers `GetRegionsOfInterest` with the request itself (`-DROI_SELF`, a global effect) |
| `testkit/run-negative-controls.bat` | ~270 | the same fifteen controls on Windows: `cl /LD /TC` DLLs with `/link /EXPORT` for the two entry points (a PE image exports nothing by default), the harness from `build-contract-test.bat`, and the varying source fixture written by PowerShell and size-checked; `./run-tests.sh` runs it there |
| `testkit/run-negative-controls.sh` | ~240 | builds the fifteen controls, asserts each fault is rejected with the right message **and** that the context-reading, label-writing and clip-re-reading builds pass while the mark build passes only in the bottom-up layouts and the clip-caching build fails on the second media id |

Coverage, precisely: all **18** `OfxPropertySuiteV1` functions (including the `*N`
and `propGetDimension` variants), all **13** `OfxImageEffectSuiteV1` functions,
all **18** `OfxParameterSuiteV1` functions, all **9** `OfxMultiThreadSuiteV1`
functions (`multiThread` with real workers, real mutexes), both memory functions,
and `message`. `fetchSuite` returns `nullptr` for everything else — so a plug-in
that fetches an optional suite is exercised against a host that does not have it.

It drives, in one process and twice over:

```
setHost → load → describe → describeInContext → createInstance
        → isIdentity → [set parameters] → isIdentity → render
        → destroyInstance → unload → dlclose
```

and asserts, beyond status codes: the requested context appears in
`kOfxImageEffectPropSupportedContexts`; `kOfxImageEffectPluginRenderThreadSafety`
is one of the three legal values; every expected parameter exists **with the
declared type**; every expected clip exists; `isIdentity` returns the source clip
at neutral settings and `kOfxStatReplyDefault` once a setting changes the output;
`render` writes the expected pixels (a scale of the source, or a golden file); a
byte destination is rejected with `kOfxStatErrUnsupported`; and a render window
the source region of definition does not cover is rejected with
`kOfxStatErrValue` instead of dereferencing the `nullptr` that
`Image::getPixelAddress()` returns outside the bounds, and the destination is
compared pixel for pixel across that rejected render, so a plug-in that writes
part of the frame and still reports an error fails the test. The write side is
checked the same way: a destination region of definition that does not cover the
window is refused too, and the plug-in must leave every pixel alone — otherwise
the frame would be written outside the buffer the host handed over, which no
host can detect afterwards.

The region-of-interest question is asked and answered too. `--roi-expand N`
sends `kOfxImageEffectActionGetRegionsOfInterest` for a region inside the frame
and for the frame itself, then requires the source RoI to be the requested
region grown by `N` and clamped to the source region of definition. A plug-in
that answers `kOfxStatReplyDefault` — it never reported a region — fails the
first check by name, because a silently shrunken RoI costs pixels at the frame
border and nothing else notices.

A rectangle is a promise about pixels, so `--roi-context` tests the pixels. It
renders the whole frame, then a window inset by three pixels, and requires the
overlap to be **bit-identical** to the whole-frame render: the plug-in said which
neighbours it reads and the host handed them over, so the arithmetic has to reach
the same result. It then repeats that window against a host that ignores the
declaration (`FakeHost::setSourceCroppedToWindow`, which publishes the source
cropped to the window), and requires the same pixels to **differ** — otherwise
the first comparison could pass without anything outside the window ever being
read. Measured: without the declaration honoured the first difference sits at
pixel (3,3), the window's own border.

This runs twice. With the plug-in's default tile size the window goes through the
model in one pass and the comparison is exact. The second run sets `tileSize=16`,
so the same 64×48 window goes through `RunTiled` and the frame is assembled from
overlapping, feathered tiles; there the check is a **bound**, `--roi-context-tolerance
0.02`, because `TileAccumulator` blends the overlap and bit-equality is a
property no tiling implementation can promise by construction. The bound is
justified by measurement on this model, not by taste: with `halo=2` the tiled
render deviates by **0** from the whole-frame render (overlapping tiles agree
bitwise when the halo covers the receptive radius, so the blend is weight-neutral),
and with the context gone — `halo=0`, or a host that supplies only the window —
by **0.1875**. 0.02 sits an order of magnitude below the failure and at the same
threshold `onnx-and-tensorrt.md` §4 uses for the halo ladder, so a plug-in
that drops its context fails the tiled run too rather than sneaking under a loose
bound.

### 5.2 How to reuse it for your own plug-in

1. Copy `testkit/` into your repository (it is self-contained; it needs only the
   OFX headers and `-ldl`).
2. Build it: `testkit/build-contract-test.sh out/contract_test /path/to/openfx`.
3. Call it from your test script with your own expectations:

```bash
out/contract_test out/MyPlugin.ofx \
  --id com.example.myplugin --version 1.0 \
  --param gain:double --param mode:string \
  --clip Source --clip Output \
  --identity --set gain=2.0 --not-identity-after-set --byte-dst-rejected
```

4. For an image effect whose output you can compute independently, add
   `--src input.bin --golden expected.bin --size 640x480` (planar float32,
   3 channels). The render is then compared bit-for-bit against a file the
   plug-in did not produce.
5. Edit the expectations when your declarations change — that edit is the point:
   a parameter that silently changes type is exactly the bug the harness exists
   to catch.

### 5.3 Proof that it can fail

A harness that has only ever passed is untested. Fifteen controls, run by
`testkit/run-negative-controls.sh`, with `testkit/run-negative-controls.bat` as
the Windows counterpart. `./run-tests.sh` runs each on its own platform as the
last stage before the summary, and names the skip when no C compiler is present.
Both exit 0 only when every fault is caught, so the expected failures are
asserted rather than left to turn a run red:

```
--- negative control: describe returns kOfxStatReplyDefault ---
  ok 14 - kOfxActionLoad
  FAIL kOfxActionDescribe: kOfxStatReplyDefault
  contract_test: 15 checks, 1 failed
  caught (exit 1) with: FAIL kOfxActionDescribe: kOfxStatReplyDefault

--- negative control: parameter declared with the wrong type ---
  ok 19 - param 'gain' declared
  FAIL param 'gain' type: OfxParamTypeInteger (expected OfxParamTypeDouble)
  contract_test: 20 checks, 1 failed
  caught (exit 1) with: FAIL param 'gain' type: OfxParamTypeInteger (expected OfxParamTypeDouble)

--- negative control: declared context region ignored by render ---
  ok 25 - roi context: whole-frame render
  ok 26 - roi context: destination is readable
  ok 27 - roi context: render of an inset window
  FAIL roi context: the inset window matches the whole frame bit for bit:
       max deviation 0.201604 at pixel (60,3) channel 1: 0.475372 vs 0.273769,
       which the exact check does not allow
  contract_test: 28 checks, 1 failed
  caught (exit 1) with: FAIL roi context: the inset window matches the whole frame bit for bit

--- negative control: declared context region honoured by render ---
  ok 61 - roi context: the inset window matches the whole frame bit for bit
  ok 62 - roi context: render of the same window without a context region
  ok 63 - roi context: without a context region the same window differs
  contract_test: 66 checks, 0 failed
  passed, as a plug-in following the pattern must

--- negative control: internally tiled render ignores the declared context ---
  ok 25 - roi context: whole-frame render
  ok 26 - roi context: destination is readable
  ok 27 - roi context: render of an inset window
  FAIL roi context: the inset window stays within the tolerance of the whole frame:
       max deviation 0.375716 at pixel (31,31) channel 2: 0.920962 vs 0.545246
       (tolerance 0.020000)
  contract_test: 28 checks, 1 failed
  caught (exit 1) with: FAIL roi context: the inset window stays within the tolerance of the whole frame

--- negative control: internally tiled render honours the declared context ---
  ok 61 - roi context: the inset window stays within the tolerance of the whole frame
  ok 62 - roi context: render of the same window without a context region
  ok 63 - roi context: without a context region the same window leaves the tolerance
  contract_test: 66 checks, 0 failed
  passed, as a plug-in following the pattern must

--- negative control: label parameter declared and never written ---
  ok 23 - kOfxImageEffectActionRender
  FAIL label parameter 'provider' = "CPU (declared, never written)": the plug-in
       never wrote it; what is shown is the declared default, which is not
       evidence that anything ran
  contract_test: 24 checks, 1 failed
  caught (exit 1) with: FAIL label parameter 'provider' =

--- negative control: label parameter written by the plug-in ---
  ok 24 - label parameter 'provider' = "CPU (stub, written by render)"
  ok 52 - label parameter 'provider' = "CPU (stub, written by render)"
  contract_test: 52 checks, 0 failed
  passed, as a plug-in following the pattern must

--- negative control: mark drawn in a bottom-up frame arrives where the frame says ---
  ok 28 - overlay mark at (24,8)
  contract_test: 64 checks, 0 failed
  passed, as a plug-in following the pattern must

--- negative control: mark drawn into a top-down frame is mirrored ---
  FAIL overlay mark at (24,8): the mark is at the mirrored row: the rasteriser
       assumed the pointer's first row is the frame's top
  contract_test: 28 checks, 1 failed
  caught (exit 1) with: FAIL overlay mark at (24,8)

negative controls: every fault detected, the pattern still passes, harness is not a rubber stamp
```

The ROI pairs are the interesting ones, because a single build could not prove
either: the same C stub (`stub_roi_context.c`) applies a 3×3 box filter and
differs only in whether it clamps its taps to the published source bounds or to
the render window. Built with `-DIGNORE_CONTEXT` it must fail the bit-for-bit
check at the window's own border; built as is, it must pass the whole harness.
With `-DTILED` the stub splits the window into 32-pixel cores and gives each two
pixels of context, so the same difference now shows up at a tile boundary inside
the window and must fail the tolerance check (`--roi-context-tolerance 0.02`,
measured 0.3757 against the bound). All four runs use a generated, varying source
fixture — over the harness's default constant colour a box filter has no seam to
find and the controls would pass for the wrong reason.

The first stub answers `kOfxActionDescribe` with `kOfxStatReplyDefault`; the
second declares `gain` as `OfxParamTypeInteger`. Neither is caught by a compiler
and neither crashes — only reading the status code and the declarations catches
them, which is the difference between this layer and L0.

### 5.4 What the fake host still cannot prove

1. **The multi-resolution and tile protocol.** It never sets
   `kOfxImageEffectPropTilesRoD` and never slices a render window, so a
   `setSupportsTiles(true)` path is untested here.
2. **Clip preferences and host scheduling.** `getClipPreferences` and per-clip
   depths/PARs are not exercised; `multiThread` runs real workers (up to four),
   which is a host-shaped pool rather than a scheduler, and the worker threads
   of `renderConcurrently` start together by construction rather than with a
   host's scheduling. Concurrent renders of
   **different instances** are exercised (§8); the same instance from several
   threads is only run for a plug-in that declares `eRenderFullySafe`.
3. **How a real host reports errors.** The message suite only records strings —
   nothing here proves an error reaches a user, that progress appears, or that
   cancel works mid-render.

---

## 6. L4: image correctness, goldens, references, negative controls

Three independent techniques, in increasing order of value:

**Committed goldens with provenance.** `templates/onnx-inference-ofx/models/`
holds a 611-byte ONNX model and a 36 KB golden generated by the Python ONNX
Runtime (`tools/make-test-model.py`). The test needs no network, no Python and no
download; the provenance is in the generator, so the golden is not a copy of the
code under test. Measured: the C++ path is **bit-identical** to that golden
(`max |diff| = 0`).

**A reference implementation you can argue with.** The CUDA template compares
three things: the vec4 kernel, the scalar kernel, and a plain CPU
reference loop — byte for byte, over widths 0–63 (every residue mod 4), heights
1/2/3/5, three pitches including one not 16-byte aligned, two base alignments,
and nine window origins. Measured: **3,240 emulated grid launches**
(vec4 = 1,152, scalar = 2,304), 0 failures, plus the sentinel check that nothing
outside the window was written.

**Negative controls.** A test that cannot fail is decoration. The inference
template runs the same tiled comparison with `halo = 0` and asserts it
**disagrees** with whole-frame inference; measured deviation **0.263276**. If
that number ever drops to zero, the halo is no longer doing anything and the
positive test has stopped proving anything. Add this control to any test whose
pass condition is "A equals B".

Additional cases worth covering:

| Case | Assertion |
|---|---|
| Alpha | channel 3 unchanged after render |
| Identity | neutral params return the source clip and no render work |
| Determinism | same input twice → byte-identical output |
| Odd sizes | 1×1, 3×5, 7×5, 64×3, 3×48, 1919×1081 |
| Padded / flipped | padded `rowBytes`; negative `rowBytes` |
| Tolerance | bit-exact where the code path is identical; a written threshold + ULP budget where it is not (GPU vs CPU) |

The tolerance policy must be **written down next to the test**. "Within 1e-5" is
a decision; a bare `EXPECT_NEAR` is not.

---

## 7. L5: memory, UB and leaks

| Tool | Command | Catches | Cannot see |
|---|---|---|---|
| ASan + UBSan | `SANITIZE=address,undefined ./test/run-tests.sh` | heap overflows, use-after-free, signed overflow, misaligned access | host-visible leaks (images you never released) |
| TSan | `SANITIZE=thread ./test/run-tests.sh` | data races in your per-instance state | races inside the host |
| valgrind | `RUN_VALGRIND=1 ./test/run-tests.sh` | uninitialised reads, leaks, invalid frees | CUDA/GPU allocations |
| valgrind, contract | `RUN_VALGRIND=1 ./test/run-tests.sh` (step 5) | the same, but over `dlopen` + describe + render paths a unit test never reaches | allocations made by the host application |
| allocation counter | global `operator new` counter around a render | per-frame allocation (a performance bug with a memory symptom) | allocations inside a runtime you link |

Measured here: the CUDA template's suite is clean under ASan/UBSan and under
valgrind (`ERROR SUMMARY: 0 errors from 0 contexts`), for the kernel harness
**and** for the contract harness. The inference template's contract harness is
clean under ASan/UBSan (every check, no sanitizer report) and under
valgrind: `0 errors from 0 contexts`, `definitely lost: 0 bytes`,
`indirectly lost: 0 bytes`, with 15,965 bytes still reachable in 21 blocks —
that is ONNX Runtime's process-lifetime arena plus the Support library's static
descriptors, not a leak, which is why the memcheck step uses
`--errors-for-leak-kinds=definite`.

Two things ASan will never see, and you must test instead:

- **Host image handles.** `fetchImage()` returns an owned reference; a leaked one
  is a leak *in the host*, and the host will eventually stop rendering. Test it by
  counting fetch/release pairs in a fake host, or by watching a host's memory over
  a long playback.
- **Device memory.** Only `compute-sanitizer` (NVIDIA) or the platform equivalent
  sees that, and only for the code you actually launch.

---

## 8. L6: concurrency and the thread-safety claim

The claim and the test must match. `kOfxImageEffectPluginRenderThreadSafety`
(`ofxImageEffect.h:629`, default instance-safe) has three values, and each maps to
a test:

| Claim | What must be true | Test |
|---|---|---|
| `eRenderUnsafe` | nothing beyond one render at a time | no test needed; the host serialises |
| `eRenderInstanceSafe` | per-instance state is safe, instances are independent | N instances rendered on N threads; output byte-identical to serial |
| `eRenderFullySafe` | the **same** instance may render concurrently | all buffers keyed per render; TSan clean under that load |

The first two rows are now code, not advice. `contract_test` creates a second
instance while the first is live, gives each its own value for
`--concurrency-param`, renders both, and compares each frame with its own
settings; then it renders one frame per instance from one thread per instance
(`renderConcurrently`) and repeats the comparison, with a worker's own
source/destination images so a race found there belongs to the plug-in.
`eRenderFullySafe` selects the same instance for every worker instead. An
`eRenderUnsafe` plug-in gets a named skip, not a silent pass.

Measured here: `ReferenceGain`, `MinimalCudaGain` and `OnnxInference` (all
declaring `eRenderInstanceSafe`) pass four instances × three concurrent renders,
each frame matching its own parameter value, **0 failures**; the harness is
TSan-clean. The plug-in binaries are not TSan-instrumented in that run, so it
proves behaviour, not the absence of races. A TSan build of the reference plug-in
does report one, and it is worth knowing where: every action calls
`OFX::Log::indent()` (`ofxsImageEffect.cpp:2569`), which increments a plain global
`gIndent` (`ofxsLog.cpp:63`), so **two concurrent renders race inside the Support
library** before any plug-in code matters. The counter is cosmetic, but the claim
"instance-safe" is only as true as the library under it — a session or cache with
the same shape would be a real bug.

The check bites: a copy of the reference that multiplies by a constant instead of
reading `gain` fails `instance 0 used its own value, not its neighbour's` with
`max deviation 0.375` **[V]** — that is the shape of the bug the check exists to
catch, and the failure names the instance, not just a pixel. What it still does
not cover: a plug-in that declares `eRenderFullySafe` (no template does, so that
branch is written but unexercised here), and races in code that was not built with
TSan (see §7).

Measured at the kernel level: 6 threads running the real kernel body on
disjoint buffers, **0 failures**, TSan-clean — which is what reentrancy of a
function means. That is *not* a plug-in-level thread-safety test, and the
difference matters: a plug-in also has parameters, sessions and caches.

Two more concurrency cases a host will produce and a unit test will not:

- **Parameter mutation during render.** The host may change a parameter on the
  main thread while `render()` runs elsewhere. Reading through
  `getValueAtTime()` is the contract; cache nothing that a parameter can change.
- **Describe while idle, render while describe?** The host will not, but it *will*
  call `describe` twice and `createInstance` on several threads. A fake host can
  replay both sequences.

---

## 9. L7: performance, measured or not claimed

Rules that make a timing number mean something:

1. **Warm up.** First frames pay module load, JIT/PTX, session creation, caches.
   Report first-frame separately from steady state, never averaged together.
2. **Time the unit you name.** "ms per 1920×1080 frame" — not "fast".
3. **Report a distribution.** At least p50 and p95 over ≥100 frames; a mean hides
   the stalls that users feel.
4. **Count allocations and peak memory** alongside time; a per-frame `malloc` is
   a latency bug even when the mean looks fine.
5. **State the machine.** CPU, GPU, driver, host version, resolution, bit depth.
6. **Compare like with like.** GPU path vs CPU path on the same frames and the
   same tolerance; a GPU path that is not bit-identical needs a stated threshold.
7. **Measure on an idle machine.** A run that overlapped a `ninja` build reported
   168 ms/frame for a model that takes 45 ms: the *ratio* survived, the number did
   not (**B**). Say what else was running, or the figure is a machine's mood, not
   the plug-in's cost.

Nothing in this repository is timed: there is no GPU and no host, so every
performance statement in these documents is deliberately absent. If you add a
benchmark, put its numbers in a file next to it — a number in a commit message is
lost the moment the machine changes.

What to profile, and with what: Nsight Systems for the whole frame (is your kernel
serialised behind the host's?), Nsight Compute for the kernel itself, and
`compute-sanitizer` for correctness (slow: run it at 256×256, not 4K).

---

## 10. L8: the host acceptance script

Run this before every release, in at least two hosts, and write down the result.
It is the only layer that tests the product.

| # | Step | What it reveals |
|---|---|---|
| 1 | Plug-in appears in the expected group, with the expected name | registration, bundle layout |
| 2 | Apply to a clip; image changes | the basic render path |
| 3 | **Render to file**, not just the viewer | viewer and render paths differ in every host |
| 4 | Scrub and stop mid-render | `abort()` handling, cancellation |
| 5 | Change resolution / proxy mode | RoD, resolution-dependent code, scale handling |
| 6 | Crop and re-frame the node | window vs bounds arithmetic |
| 7 | Two instances of the plug-in at once | per-instance state, session sharing |
| 8 | Install a **second** OFX plug-in and restart | symbol visibility collisions |
| 9 | Toggle the host's GPU mode (CUDA ↔ CPU) | capability negotiation, CPU fallback |
| 10 | Save, quit, reopen the project | parameter serialisation, no required external state |
| 11 | Animate a parameter and undo/redo | `getValueAtTime`, no hidden state |
| 12 | Force a crash once, restart | the host's blacklist; know where to clear it ([`debugging-and-symptoms.md`](debugging-and-symptoms.md) §1) |

Add host-specific rows from [`host-resolve.md`](host-resolve.md)
for the hosts you actually support.

---

## 11. L9: failure injection

Most production bugs are the plug-in's behaviour when something else is wrong.
Each of these has a defined correct answer; test that you produce it.

| Injected failure | Correct behaviour |
|---|---|
| Model file missing / unreadable | log + `kOfxStatErrUnsupported` (or identity), never a crash |
| Model file corrupt | same, with the framework's message |
| Model with the wrong channel count | refuse loudly, do not silently reinterpret |
| Clip not connected | identity, or a defined constant image |
| Zero-size or negative render window | return without touching memory |
| Huge frame (8K) | refuse or process, but no overflow: widen pitches |
| NaN / Inf in input | propagate or clamp by policy; never trap |
| 16-bit input when only float is declared | `kOfxStatErrUnsupported` |
| Read-only model directory | a message, not a crash on first render |
| Host aborts mid-render | return promptly, release images |
| Sidecar/worker dies | bounded retry or clean error; host survives |
| Sidecar stalls | bounded timeout, no hang ([`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §3) |
| Device out of memory | `kOfxStatGPUOutOfMemory` |
| GPU kernel launch fails | `kOfxStatGPURenderFailed`, with a log |

The pattern behind all of them: **log what happened and return a code the host
understands.** Silence is the only wrong answer.

---

## 12. L10: CI and the release gate

| Frequency | Run |
|---|---|
| Every commit | build, L0 tests, L1 exports, L2 greps, clang-format/shellcheck |
| Every PR | the above + ASan/UBSan + cppcheck + the doc link check |
| Nightly | TSan, valgrind, the full geometry matrix, the largest frame sizes |
| Release | everything + L8 in two hosts + the bundle assertions + a written verification status |

CI rules that keep the suite honest: no network in tests (commit the fixtures —
the 611-byte model exists for this), fixed seeds, a timeout on every test (a hang
must fail, not stall the queue), and **no test whose pass condition is "it did not
crash"**.

Each workflow runs its half of the gate once per push: `docs.yml` checks the
documents, their coverage against `4290e68`, the two tool self-tests and the shell
syntax, `linux.yml` runs the template suites, the sanitizers and the glibc stage.
The `templates` job that ran the whole gate a second time was deleted, so a green
tick has one meaning and one runtime download. Both files carry
`permissions: contents: read`, a `concurrency` group that cancels a superseded
pull-request run and leaves `main` alone, and pinned actions by commit SHA with
the tag in a trailing comment (`actions/checkout` v4.4.0,
`actions/upload-artifact` v4.6.2). `actionlint` 1.7.7 reads both files in
`docs.yml`, after checking the digest of its own tarball; it reports nothing on
the current tree (run here, 2026-11-26).

A third workflow, `canary.yml`, runs on a schedule and installs nothing pinned:
`tools/check-model-canary.py` loads the shipped model on the newest ONNX Runtime
and compares it with the committed golden. The pinned gate proves the version
this repository targets; the weekly job is what notices that the next version
changed a fusion or refused the model, which is a decision, not a revert.

The release gate itself is [`checklists.md`](checklists.md):
§1 lists the mechanical gates with their commands and, importantly, what a pass
does *not* prove; §2–§4 are the questions a reviewer signs off.

---

## 13. What this repository actually runs

Reproduce all of it from a clean checkout; nothing needs a GPU or a host.

```bash
# Everything, every template: reference build, unit, image, artifact, fake host, memcheck
ONNXRUNTIME_ROOT=/path/to/ort ./run-tests.sh
# Without ONNXRUNTIME_ROOT the inference template is skipped: the reference and
# CUDA suites still build and run, the summary prints a NOT VERIFIED block, and
# any CI run (CI/GITHUB_ACTIONS/TF_BUILD/BUILDKITE set) fails unless
# ALLOW_SKIPPED_TEMPLATES=1 accepts the hole.
./run-tests.sh

# Or one template at a time:
cd templates/minimal-ofx && ./build.sh && ./test/run-tests.sh
cd templates/minimal-cuda-ofx && ./build.sh && ./test/run-tests.sh
cd templates/onnx-inference-ofx && ONNXRUNTIME_ROOT=/path/to/ort \
  ./build.sh && ./test/run-tests.sh

# Proof that the harness detects faults. ./run-tests.sh above runs one of these
# on its own platform; run them directly to read a single control's full output.
# Both exit 0 when every fault is caught.
testkit/run-negative-controls.sh
cmd //c testkit/run-negative-controls.bat      # the same controls, Windows
```

The Python side has one pinned home, and the entry points prefer it over whatever
`python3` happens to be: `run-tests.sh`, `templates/minimal-ofx/build.sh` and
`testkit/verify-bundle.bat` all look for `.venv/` in the repository root first,
then fall back to `python3` and `python` - which is what CI does, because a
checkout there has no `.venv`.

```bash
python -m venv .venv
.venv/Scripts/python.exe -m pip install numpy==2.2.6 onnx==1.23.2 onnxruntime==1.23.2
```

Those three versions are the ones the fixture gate and the Resolve self-tests
passed with on 2026-11-25. The gate itself only asks for
`import numpy, onnx, onnxruntime`, so a newer set works until it does not - and
the 2026-11-25 reason for pinning was that numpy 2.x against onnxruntime 1.14
exited 139 instead of failing a test.

### The tools on their own

The commands above run everything; each of these answers one question and is
worth reaching for while debugging rather than through the suite:

| Command | Question it answers |
|---|---|
| `python3 tools/probe-env.py` | which compiler, generator, CUDA toolkit and Python this machine has; `--self-test` removes a capability and has to report not-ready |
| `python3 tools/check-docs.py` | documentation drift - links, anchors, counts, provenance labels, section references, the heading skeleton; ~1 s. It also holds the ONNX Runtime pin: every document that names an `onnxruntime-linux-x64-<version>` path has to agree with the `default:` of the `version` input in `.github/actions/onnx-runtime/action.yml` (the input, not the first default in the file - `package` and `platform` carry defaults too). A file that no longer has that shape - no `inputs:` block, no `version` entry, no default under it, a default that is not a version - is a named failure, not a pass that checked nothing, and plain version mentions are measurements, not failed |
| `python3 tools/check-runtime.py <ort-root>` | `ORT_API_VERSION`, the entry points this repository calls, provider exports and the CUDA imports; `--require-api` and `--require-plugin-provider` gate a root, `--self-test` proves the reading on fixtures it builds |
| `python3 tools/check-model-canary.py` | does the shipped model still load on **whatever runtime is installed**, and does it still reproduce the committed golden? Prints the runtime version and the model's opset; unpinned by design, so the weekly `canary` job can ask it of the newest release |
| `python3 tools/make-testmodels.py --out <dir>` | synthetic 1/3/4-channel and 2x ONNX models, so nothing needs a download |
| `python3 tools/model_probe.py model.onnx` | shapes, dynamic dims, size factor, and the alignment multiple the model wants. The frame it probes is the model's own where the model fixes it, else the `test_shape.txt` beside it, else `--size` (64x64 only as a last resort) |
| `python3 tools/_model_shape.py models/test_shape.txt` | the frame a model and its fixtures agree on: reads `channels height width`, and prints the IREE spelling (`1x3x48x64xf32`). The merge of a model's declared input with that file lives here, and `model_probe.py`, `check-model-canary.py` and `aot-compile-model.sh` all call it instead of carrying the shape as a constant |
| `python3 tools/bench_ep.py model.onnx` | median/p90 per execution provider |
| `tools/aot-compile-model.sh [model] [outdir]` | compiles the committed model ahead of time with IREE - ONNX to MLIR, an `llvm-cpu` artifact and a Vulkan/SPIR-V artifact - then runs the CPU one and compares it with the golden; the input spec and the fixture paths come from the model's directory (`test_shape.txt`), and an input whose length disagrees with that file is a failure with both numbers, not a runtime shape error. Names a missing tool instead of failing silently |
| `tools/measure-halo.sh model.onnx` | tile overlap per model, writes `<model>.halo.json` |
| `tools/measure-halo.cpp` | the same measurement without a shell, walking the plug-in's own `RunWhole`/`RunTiled` so the number is the one the plug-in will need (Windows, MSVC or clang) |
| `tools/sweep-vram.sh model.onnx` | largest tile before ORT runs out of VRAM (needs a GPU) |
| `tools/host_cuda_probe.cpp` | which CUDA loader wins inside a host process, reproduced without the host: the module list and the file a name resolves to (Windows) |
| `tools/ep_register_probe.cpp` | whether the 1.22 registration API works outside a plug-in: registration, device list, append, run, unregister, with `--stop N` to bisect |

Every comparison the harness trusts is shown failing before it is credited with
passing. Three of them have their own test: `testkit/selftest-compare.sh` corrupts
a reference, `tools/check-glibc.sh --selftest` lowers the bound until the check
must refuse, and `tools/probe-env.py --self-test` removes a capability until the
suite that needs it stops reading ready - that one runs on both routes too,
because it needs no toolchain. The glibc one runs in `./run-tests.sh` on **both routes** - the
POSIX one and the Windows one, because it feeds a **canned** `readelf` and so
needs neither binutils nor an ELF binary - and as a step in the `docs` workflow,
where it fails a pull request before the template builds start. What it proves is
the comparison and the argument handling, in nine cases: a bundle within the
bound passes, while a bound one step too low, a requirement above the bound, an
object `readelf` cannot read, an empty tree, an unparsable bound and a missing
path each produce the exit code and the line they should - and a bound above the
highest or a file mentioning only `GLIBCXX_3.4.29` passes instead of failing on a
version that is not glibc.

What it does not prove is that a real `readelf` reports a real `.ofx` the way the
canned one does - that still needs a Linux build and
`tools/check-glibc.sh --max 2.17 <bundle>`.

The restructure gate runs in the same job: `tools/coverage-check.py` derives its
baseline from commit `4290e68` - which is why that job checks out with
`fetch-depth: 0` - and fails when a distinctive word of the pre-move corpus no
longer occurs anywhere in the tree, so what it proves is that moving the
documents into `references/` lost no vocabulary, not that every sentence survived
word for word.

`./run-tests.sh` exits 0 with `RUN_VALGRIND=0` in about two minutes on the
authoring machine; with memcheck enabled (the default when valgrind is on
`PATH`) it takes close to five. `CPU_ONLY=1` forces the CUDA template's CPU path
when no toolkit is installed. In that mode - and on a machine without `nvcc`,
which lands in the same mode - the summary prints `NOT RUN: GPU kernels` next to
a `NOT VERIFIED` block instead of a line claiming kernels ran, so a CPU-only run
cannot be read as one that exercised the device.

**Where the inference template's ONNX Runtime tree comes from.** `ONNXRUNTIME_ROOT`
must contain `include/onnxruntime_cxx_api.h` and `lib/libonnxruntime.so`. The pip
wheel is not enough - it ships the shared library without a single header, which
is how a run ends in "build is not configured" two steps later. The NuGet package
carries both halves; this is the recipe that produced the 1.32.0 tree behind the
2026-11-26 rows, and `.github/actions/onnx-runtime` does the same in CI:

```bash
curl -fsSL -o /tmp/ort.nupkg \
  https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.32.0/microsoft.ml.onnxruntime.1.32.0.nupkg
python3 - <<'PY'
import os, shutil, zipfile
root = ".validation/ort-1.32.0"
os.makedirs(f"{root}/include", exist_ok=True)
os.makedirs(f"{root}/lib", exist_ok=True)
with zipfile.ZipFile("/tmp/ort.nupkg") as z:
    for n in z.namelist():
        if n.startswith("build/native/include/") and n.endswith(".h"):
            with z.open(n) as s, open(f"{root}/include/{os.path.basename(n)}", "wb") as d:
                shutil.copyfileobj(s, d)
        elif n == "runtimes/linux-x64/native/libonnxruntime.so":
            with z.open(n) as s, open(f"{root}/lib/libonnxruntime.so", "wb") as d:
                shutil.copyfileobj(s, d)
PY
export ONNXRUNTIME_ROOT="$PWD/.validation/ort-1.32.0"
```

`tools/check-runtime.py "$ONNXRUNTIME_ROOT"` reads the result back: the API
version the header declares, whether the entry points this repository calls are
there, and what the provider libraries export.

**On Windows the same recipe is one package and three directories.**
`Microsoft.ML.OnnxRuntime.Gpu.Windows` carries the headers, the core library, the
CUDA provider and the TensorRT provider in a single `.nupkg`, and the template
wants `include\onnxruntime_cxx_api.h`, `bin\onnxruntime.dll` and
`lib\onnxruntime.lib`. Verified on 2026-11-26 by unpacking the package this
checkout already had and building the template against the result:

```bash
curl -fsSL -o ort-gpu-win.nupkg \
  https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.gpu.windows/1.32.0/microsoft.ml.onnxruntime.gpu.windows.1.32.0.nupkg
python3 - <<'PY'
import os, shutil, zipfile
root = ".validation/ort-gpu-1.32.0"
for sub in ("include", "lib", "bin"):
    os.makedirs(f"{root}/{sub}", exist_ok=True)
with zipfile.ZipFile("ort-gpu-win.nupkg") as package:
    for name in package.namelist():
        base = os.path.basename(name)
        if "/native/include/" in name and name.endswith(".h"):
            target = f"{root}/include/{base}"
        elif name.startswith("runtimes/win-x64/native/") and name.endswith(".dll"):
            target = f"{root}/bin/{base}"
        elif name.startswith("runtimes/win-x64/native/") and name.endswith(".lib"):
            target = f"{root}/lib/{base}"
        else:
            continue
        with package.open(name) as source, open(target, "wb") as destination:
            shutil.copyfileobj(source, destination)
PY
export ONNXRUNTIME_ROOT="$PWD/.validation/ort-gpu-1.32.0"
```

Three details are where a hand-written extraction goes wrong first: the GPU
package keeps its headers under `buildTransitive/native/include` where the core
package uses `build/native/include` (matching on `/native/include/` covers both);
the DLLs and the import libraries go to different directories, and the template
links neither - it loads `bin\onnxruntime.dll` from beside the `.ofx`; and the
download is 246 MB, so cache the unpacked tree, not the package.

What that run produced from the 1.20.1 package in this checkout - the 1.32.0 tree
behind the 2026-11-26 rows has the same `include`/`lib`/`bin` layout - is nine
headers, four DLLs and four import libraries. `tools/check-runtime.py` reads
`ORT_API_VERSION 20` out of it and reports the CUDA provider exporting one symbol
and importing cuBLASLt 12, cuBLAS 12, cuFFT 11, cudart 12 and cuDNN 9; with
`ONNXRUNTIME_ROOT` pointing at it, `templates\onnx-inference-ofx\build.bat`
produced `build\bundle\OnnxInference.ofx.bundle\Contents\Win64\OnnxInference.ofx`
([log](../evidence/2026-11-26-windows/ort-nuget-recipe.log)).

**The CUDA DLLs are not in that package.** The provider imports them and they come
from the CUDA Toolkit or the `nvidia-*` wheels, which is what
`-DOFXPLUGIN_CUDA_RUNTIME_DIRS` is for; the bundle in that log reports 16 files at
1540 MB. Shipping them is a licence question before it is a download step:
[`packaging-and-install.md`](packaging-and-install.md).

**The version this repository pins is a single default.**
`.github/actions/onnx-runtime/action.yml` carries it, both workflows call that
action without a version argument, and no workflow names one of its own - so a
bump is that one field plus whatever in the documents states which version a
measurement was taken with, because those sentences are claims about an evidence
log rather than pins.

Measured on the machines this repository has been exercised on: a Linux x86-64
box with no GPU and no host, and a Windows 11 box with an RTX 3060. The 2026-11-22
Windows rows are **[B]** (that run was on another machine); the 2026-11-24 rows
were run in this checkout, including the inference template's first Windows build
at all ([`onnx-inference-suite.log`](../evidence/2026-11-24-windows/onnx-inference-suite.log)),
and the 2026-11-25 rows are the same suite on the CUDA execution provider with
ONNX Runtime 1.20.1 and then 1.32.0
([`onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log),
[`onnx-inference-cuda-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log)).

| Suite | Result |
|---|---|
| CUDA kernels | 3,240 grid launches (vec4 = 1,152, scalar = 2,304), 0 failures |
| CUDA CPU loop | 1,464 components compared, 0 failures |
| CUDA concurrency | 6 threads, 0 failures, TSan-clean |
| CUDA bundle | 2 `T` exports (+5 weak `W`); valgrind `0 errors from 0 contexts` |
| CUDA architectures, user choice | fresh `-DCMAKE_CUDA_ARCHITECTURES=90` → `-- CUDA architectures: 90`; exactly one `--generate-code=arch=compute_90,code=[compute_90,sm_90]` token; a reconfigure without the flag keeps `90` |
| CUDA architectures, default list | fresh configure → `75;80;86;89;90;100;120`; a reconfigure keeps the list (nvcc's default `52` recorded once as the marker) |
| CUDA architectures, pre-marker tree | a tree configured by the pre-marker file (cached `52`, no marker) prints that `52` is nvcc's default and applies `75;80;86;89;90;100;120`; the next configure keeps them. CMake 3.31.6 and 4.4.5 |
| CUDA architectures, pre-marker tree with `-D 90` | the same tree configured with `-DCMAKE_CUDA_ARCHITECTURES=90` (`UNINITIALIZED` in the cache) stays on `90`, with no default-list notice |
| Reference build | one command from a tree with no headers/build dir; 2 dynamic symbols total, `dlopen` + `OfxGetPlugin` → 1 plug-in |
| Inference golden | C++ vs Python max absolute difference = 0 over the frame |
| Inference tiling | tile 5/13/17/256 and odd 1×1, 7×5, 64×3, 3×48 all `0` |
| Inference control | `halo = 0` deviates by `0.263276` (expected) |
| Inference bundle | 2 `T` exports (+6 weak `W`), `NEEDED libonnxruntime` = 1, `RUNPATH $ORIGIN` = 1 |
| Inference suite, Windows (ORT 1.20.1) | first Windows build of the template: host contract **160 + 82 checks, 0 failed**, golden difference `0`, tile 5/13/17/256 and the odd sizes `0`, 2 exports, `onnxruntime-ofx.dll` beside the `.ofx` ([`onnx-inference-suite.log`](../evidence/2026-11-24-windows/onnx-inference-suite.log)) |
| Inference suite, Windows, CUDA provider (ORT 1.20.1) | provider and its CUDA imports bundled beside the `.ofx` (16 files, 2686 MB), label `CUDA (device 0, 1 thread, bundled CUDA)`, same contract **0 failed**; the exact golden is off by between `1e-8` and `1e-7` - rounding order of the device - so the run only passes through the `1e-5` retry ([`onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log)) |
| Inference suite, Windows, CUDA provider (ORT 1.32.0, CUDA 13) | the same suite on the current runtime: provider shipped as an EP plugin, 16 files and 1540 MB beside the `.ofx`, same label, same `0 failed`, same `1e-5` retry, and no change to the template's source ([`onnx-inference-cuda-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log)) |
| One-hole headline, Windows (ORT 1.20.1, `CPU_ONLY=1`) | `ONNXRUNTIME_ROOT` set but no CUDA path: the summary reads `all suites that ran passed, but the GPU step did not run:` with `NOT RUN: GPU kernels (CPU_ONLY)` under it - one hole, named, not rounded up. The inference template still ran on the GPU package's provider: label `… tf32 off, all nodes on CUDA`, **160 + 160 + 82 checks, 0 failed**, and the build against the older headers is what caught the C++-only option wrapper that 1.20.1 does not have ([`onnx-1.20.1-cpu-only.log`](../evidence/2026-11-25-windows/onnx-1.20.1-cpu-only.log)) |
| Inference fallback probe, Windows (ORT 1.32.0, CUDA 13) | a throwaway session with `session.disable_cpu_ep_fallback=1` loads the graph, so the label reads `CUDA (device 0, 1 thread, bundled CUDA, tf32 off, all nodes on CUDA)` in both host profiles; the control - same model, same runtime, no provider appended - fails with the key and builds without it, so the key is doing the work ([`onnx-fallback-probe.log`](../evidence/2026-11-25-windows/onnx-fallback-probe.log)) |
| Host profiles, all three templates | the contract runs once per host shape (`--host-profile nuke`, `resolve` and `natron`, §5): **118 × 3** for `ReferenceGain`, **144 × 3** for `MinimalCudaGain`, **168 × 3 + 90** for `OnnxInference` (the 90 is the tiled pass), 0 failed in every pass, identical counts in all three shapes |
| Inference suite, Windows, CPU (ORT 1.32.0) | the arithmetic did not move with the runtime: the golden rendered by 1.20.1 matches **exactly**, no retry, and the CUDA attempt fails by design with the reason in the label - which is what isolates a provider change from a runtime change ([`onnx-inference-cpu-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cpu-1.32.log)) |
| Windows artifact | 2 exports (`dumpbin /exports`), 7 cubins `sm_75`…`sm_120` (`cuobjdump -lelf`), no CUDA runtime import **[B]** |
| Windows contract | whole host protocol, 0 failures, through `LoadLibraryExW` **[B]** |
| GPU kernels, hardware | 28/28 launcher cases bit-exact on RTX 3060 sm_86; 200 queued launches, 8 host threads, zero-copy **[B]** |
| GPU memory, 2026-11-24 | `compute-sanitizer --tool memcheck` over `gpu-kernel-test.exe`: **0 errors**, 28/28 still bit-exact under the tool — device memory checked, not just the results **[V]** |
| GPU throughput, 2026-11-24 | 1080p/4K/8K, vec4 **0.209 / 0.850 / 3.479 ms** (317/312/305 GB/s), scalar within 5% — both forms bandwidth-bound, so the vec4 form's 4× instruction reduction buys no throughput. CPU loop 3.456/13.480/55.287 ms; pageable H2D+D2H copy 6.705/27.332/107.396 ms, i.e. **32× the 1080p kernel** **[V]** (table and method in `../README.md` §"What has actually been verified") |
| Artifact + fake host (reference) | 118 checks per host profile (3 × 118 in a suite run), 0 failed, two load/unload cycles, two live instances with separate values |
| Artifact + fake host (CUDA) | 144 checks per host profile (3 × 144), 0 failed **on a `CPU_ONLY=1` build**: two load/unload cycles, four instances and three concurrent renders, all through the host protocol and the CPU path. The CUDA launcher and kernels are absent from that build, so this row says nothing about the GPU path, which the Windows rows above cover |
| Artifact + fake host (inference) | 168 checks per host profile (3 × 168), 0 failed, render matches the Python golden through the OFX API, source RoI checked against the declared halo and shown to carry pixels bit for bit, four instances × three concurrent renders; a second run with `tileSize=16` adds 90 checks for the same comparison on the tiled path, with a 0.02 bound instead of bit-equality |
| ONNX Runtime pin, document and action (2026-11-26) | the pin rule inside `tools/check-docs.py` (it was its own script until 2026-11-26) reads the pin from the composite action (1.32.0) and finds one release path in the documentation that names a version - it agrees; the other mentions are measurements and are counted, not failed. The control moves the action default to 1.33.0 and exits 1 naming `onnx-and-tensorrt.md:268`; a second control puts a version-shaped default in another input above `version`, which an unscoped first-match read would have taken as the pin, and the pin still reads 1.32.0. Four shape controls fail with the part that is missing named - no `inputs:` block, no `version` entry, `version` without a default, a default of `latest` - and a moved action file fails on the read, so the check cannot pass by finding nothing ([`ort-pin.log`](../evidence/2026-11-26-linux/ort-pin.log)) |
| Ahead-of-time compile, ONNX to SPIR-V and CPU (2026-11-26) | `tools/aot-compile-model.sh` with IREE 2026.4: the committed model imports to 1808 bytes of MLIR, compiles to a **13.8 KB** `llvm-cpu` artifact and a **15.1 KB** Vulkan artifact carrying SPIR-V, and the CPU artifact reproduces the golden at max difference **5.96e-08**. The Vulkan artifact is compiled, not run - no Vulkan device here ([`aot-spike.log`](../evidence/2026-11-26-linux/aot-spike.log)) |
| OpenVINO against the CPU provider (2026-11-26) | the comparison as it was run on that date (the script that produced it was cut the same day; the log is the record) on the committed model at 1920x1080, AMD Ryzen 7 3800X with 2 CPUs visible: ONNX Runtime 1.32.0 **15.80 ms** median at 2 threads and **21.90 ms** at 1, OpenVINO 2026.4.1 **30.77 ms** and **25.11 ms** - OpenVINO 1.95x and 1.15x slower, the opposite of the published 1.77-3.51x claim, with a 380-465 ms compile on top. Details and the limits in `performance-model.md` §6 ([`openvino-bench.log`](../evidence/2026-11-26-linux/openvino-bench.log)) |
| Model canary, shipped model on the installed runtime (2026-11-26) | `tools/check-model-canary.py` on this machine's ONNX Runtime 1.32.0: model opset 17 / IR 8, output compared with the committed golden, **max difference 0** at tolerance 0; the control - the same golden shifted by 0.5 - exits 1 and names the difference. The frame comes from `test_shape.txt` and the model's declared input shape, not from a constant: a shape file claiming four channels fails with `the model fixes dimension 1 at 3 ... says 4`, one claiming width 65 fails on the input size, both exit 2 The tool is what `canary.yml` runs weekly against an unpinned install ([`model-canary.log`](../evidence/2026-11-26-linux/model-canary.log)) |
| OFX 1.5.1 capability properties (2026-11-26) | all three templates declare `kOfxImageEffectPropCPURenderSupported` and `kOfxImageEffectPropNoSpatialAwareness` as `"true"` in their descriptor, and the contract asserts both strings plus a render the host flags with the two 1.5.1 hints (`kOfxImageEffectPropThumbnailRender`, `kOfxImageEffectPropNoSpatialAwareness` on `inArgs`), which has to produce the same frame as a full render: **118 / 144 / 168 checks** per profile and **90** on the tiled pass, 0 failed. The negative-control stubs carry the same two declarations, so a control still fails for its own fault and not for a missing property ([`v151-properties.log`](../evidence/2026-11-26-linux/v151-properties.log)) |
| glibc bound, real bundles (2026-11-26) | the `./run-tests.sh` stage, `tools/check-glibc.sh` per bundle at its own bound: `ReferenceGain` and `MinimalCudaGain` `GLIBC_2.14` against 2.17, `OnnxInference` `GLIBC_2.32` (bundled ONNX Runtime 2.28) against 2.32 — a bundle above its bound fails the run ([`check-glibc.log`](../evidence/2026-11-26-linux/check-glibc.log)) |
| Linux gate, 2026-11-26 | `CPU_ONLY=1 ./run-tests.sh` from a fast-forwarded tree: exit 0, documentation 30 documents 0 problems, 110 + 136 + (160 × 3 + 82) checks 0 failed, negative controls and comparator green, and the one hole named as `NOT RUN: GPU kernels (CPU_ONLY)`. ASan+UBSan ran the CUDA host suite separately, 136 × 3 clean ([`run-tests.log`](../evidence/2026-11-26-linux/run-tests.log), [`asan-ubsan-cuda.log`](../evidence/2026-11-26-linux/asan-ubsan-cuda.log)) |
| The `CPU_ONLY (no nvcc)` branch (2026-11-26) | `./run-tests.sh` without `CPU_ONLY` on a container with no toolkit: the build stage prints `== build: minimal-cuda-ofx (CPU_ONLY (no nvcc)) ==`, the headline reads `all suites that ran passed, but the GPU step did not run`, and the `NOT VERIFIED` block names that reason and how to close it - the branch existed since the summary was written and had never been the one taken ([`run-tests-no-nvcc.log`](../evidence/2026-11-26-linux/run-tests-no-nvcc.log)) |
| Artifact check, all three bundles (2026-11-26) | the gate's own stage, `scripts/check-artifact.sh` over each built `.ofx`: two strong exports and the loader's dependents per bundle, with the weak C++ helpers named rather than counted (5 for the CUDA plug-in, 6 for the inference one), and the device-code flag passed only when the run compiled CUDA ([`artifact-stage.log`](../evidence/2026-11-26-linux/artifact-stage.log)) |
| Runtime inspection, real provider library (2026-11-26) | `tools/check-runtime.py` over the libraries of the `onnxruntime-gpu` 1.32.0 wheel: `libonnxruntime_providers_cuda.so` reports 4 exports, classifies as a plugin EP, names `CreateEpFactories` and `ReleaseEpFactory`, imports cuBLASLt 13, cuBLAS 13 and CUDA runtime 13, and does not import the core library by name; the TensorRT provider is classified as a built-in provider. Mach-O is still not parsed, so a macOS bundle's provider reads as unreadable ([`check-runtime-gpu-provider.log`](../evidence/2026-11-26-linux/check-runtime-gpu-provider.log)) |
| Sanitizers, all three templates (2026-11-26) | TSan under `setarch -R` and ASan+UBSan, each template in its own run: 110 × 3 for `ReferenceGain`, 136 × 3 for `MinimalCudaGain`, 160 × 3 + 82 for `OnnxInference`, 0 failed and no sanitizer report in any of the six runs. The harnesses are instrumented, the plug-in images are ordinary builds - which is what `run-tests.sh` does when `SANITIZE` is set, and the reason the coverage is the host contract rather than the kernel ([`sanitizers/`](../evidence/2026-11-26-linux/sanitizers)) |
| Negative controls | `kOfxStatReplyDefault` describe and a wrong parameter type both rejected, with the assertion named |
| Analysis, all three templates (2026-11-26) | `REQUIRE_ANALYSIS=1 <template>/test/run-analysis.sh` with shellcheck 0.11.0 and **cppcheck 2.13.0** (`--enable=warning,style,performance,portability --inconclusive`): `minimal-ofx` was clean on the first run, `minimal-cuda-ofx` was already clean, and `onnx-inference-ofx` needed eight findings looked at - five `invalidPointerCast` on the OFX byte-buffer-to-float contract (suppressed inline, next to the reason), one raw case-fold loop turned into `std::transform`, and two `text = text.substr(...) + "..."` self-assignments turned into `replace`. All three end `static analysis: clean`, exit 0 ([`analysis-all-templates.log`](../evidence/2026-11-26-linux/analysis-all-templates.log)) |
| cppcheck, before it could run (2026-11-26) | cppcheck had never actually run here: the Ubuntu package reads `std.cfg` from a compiled-in path, so it needed a runner's root rather than a reason - the same run that produced these results also showed it working ([`valgrind-cppcheck.log`](../evidence/2026-11-26-linux/valgrind-cppcheck.log)) |
| memcheck, both harnesses (2026-11-26) | `CPU_ONLY=1 RUN_VALGRIND=1` on the CUDA host suite and the inference contract harness with valgrind **3.22.0**: stage `[5/5] valgrind memcheck`, `ERROR SUMMARY: 0 errors from 0 contexts`, exit 0 in both. It needs `libc6-dbg`, which `apt install valgrind` does **not** pull — without it memcheck aborts before its first check on the `memcmp` redirection. 3.27.1, the version on the pool head, crashes in `m_debuginfo/image.c` on this glibc, which is why the CI job asserts behaviour (`memcheck /bin/true`) and not a version ([`valgrind-cppcheck.log`](../evidence/2026-11-26-linux/valgrind-cppcheck.log)) |

### nvcc without a GPU

Device code does not need hardware to compile: `nvcc`, `ptxas` and `nvlink` are
CPU programs, and only loading the cubin and running it needs a GPU. That makes
the architecture selection checkable on any Linux box and in CI, which is how
the arch handling in [`cmake-and-linking.md`](cmake-and-linking.md) §5 was measured.

```bash
cd templates/minimal-cuda-ofx
./build.sh --bootstrap-cuda      # no root, no GPU; ~100 MB under ~/.cache
TK="${OFX_CUDA_TOOLKIT_DIR:-$HOME/.cache/ofx-cuda-toolkit}/home"

cmake -S . -B /tmp/arch-check -G Ninja \
  -DOFXPLUGIN_ENABLE_CUDA=ON \
  -DCMAKE_CUDA_COMPILER="$TK/bin/nvcc" -DCUDAToolkit_ROOT="$TK" \
  -DCMAKE_CUDA_ARCHITECTURES=90   # drop this line to get the curated list
# add -DOFX_SOURCE_DIR=/path/to/openfx to skip the OpenFX network fetch
# -- CUDA architectures: 90 (from CMAKE_CUDA_ARCHITECTURES)
grep -o -- '--generate-code[^ ]*' /tmp/arch-check/build.ninja | tr -d '"'
# --generate-code=arch=compute_90,code=[compute_90,sm_90]
cmake --build /tmp/arch-check   # nvcc + link, still no GPU
```

`--bootstrap-cuda` reads NVIDIA's `Packages.gz` index and unpacks five `.deb`
files with `ar` and `tar`; the packages, versions and what each contributes are
listed in [`../README.md`](reference-projects.md#artifacts-the-build-pulls-in). It needs
`curl`, `tar`, `gzip`, `awk` and `ar` — no `apt`, no root, and nothing is
installed outside `$OFX_CUDA_TOOLKIT_DIR`. Verified here with CUDA 12.9.41 on a
machine with no GPU: configure, a full `Kernel.cu` compile for `sm_90` and the
link all succeed.

What such a check proves is the **selection**, not execution: the
`-- CUDA architectures:` line, the `--generate-code` tokens in the generated
build files, and that nvcc accepts those targets and emits the object. It says
nothing about a driver loading the cubin or the kernel computing correctly —
that is what `test/gpu-kernel-test.cu` is for, and it needs hardware (§13).

Not run here, and therefore not claimed: no GPU execution in this environment
(the CUDA template is built CPU-only for the runs above), no host render (no
Resolve/Nuke/Natron anywhere in these tests), no timing measured here, and no
multi-resolution/tile-protocol test (the fake host does not speak it — §5.4).
The Windows build, the Windows contract run, the hardware kernel cases and the
throughput numbers come from a 2026-11-22 field test on another machine and are
marked **[B]**, archived in
[`field-evidence.md`](field-evidence.md);
`test/gpu-kernel-test.cu` is the harness that produced them and ships so they can
be repeated on any GPU:

```bash
nvcc -arch=sm_86 -O2 -std=c++17 test/gpu-kernel-test.cu -o build/gpu-kernel-test.exe
build/gpu-kernel-test.exe        # 28 cases, hardware only
```

---

## 14. Test smells: passing for the wrong reason

| Smell | Why it proves nothing | Fix |
|---|---|---|
| Tiled output compared to tiled output | both sides share the bug | compare to whole-frame, and add the `halo = 0` control |
| Golden regenerated by the code under test | the test encodes the bug | generate goldens with an independent implementation (here: Python ONNX Runtime) |
| "It loads" / "it did not crash" | no assertion | assert the structure fields, the status codes, the pixels |
| CPU path compared to CPU path on a GPU build | never exercises the GPU path | assert which path ran, and fail if the expected one did not |
| One size, one frame | 1920×1080 hides stride bugs | geometry matrix (§6) |
| An overlay asserted as "the frame changed" | a mirrored, clipped or off-canvas overlay changes pixels too; a pass-through copy matches at either orientation | assert bounds and orientation at a real frame size (`--size`), with a mark that is not centred (§5) |
| Sanitizer build skipped "because it is slow" | the only check that sees UB | run it on every PR; it is seconds |
| Test disabled "flaky" without a cause | a race, usually | TSan, then fix it; flaky is a diagnosis, not a status |
| Numbers in a commit message | unreproducible within a week | a benchmark file with the command, the machine and the distribution |
| A "broken" control that passes | the control was built from a copied tree whose `build/` still held the old artefact, so CMake had nothing to rebuild — measured here while proving the instance check: the deliberately "fixed gain" plug-in passed until `build/` was deleted | copy sources only, or delete `build/` first; a negative control is only a control when the binary demonstrably changed |
| A flag that encodes an assumption nobody wrote down | `--set gain=2.0 --not-identity-after-set` checks "render == source × value, alpha unchanged", which only holds for a parameter that scales RGB alone; `templates/minimal-ofx` scales *every* channel, so passing `--set` there fails on alpha even though the plug-in is right | state the assumption beside the flag, or assert only what the spec fixes (RGB); use a separate alpha parameter when alpha is meant to be independent |
| "Declared thread-safe" treated as tested | a serial render exercises none of it, and an uninstrumented TSan run says nothing about the plug-in image | render several instances from several threads (§8), and build the plug-in with `-fsanitize=thread` to see races inside it |

## 15. L3b: observing a feature that leaves no trace

(Open feedback point F16.) A detection cache, a skipped inference, an early-out
when the model's answer has not changed: none of them shows up in pixels. The
frame is identical whether the shortcut ran or the full path did, so a golden
comparison — or any assertion on the output — is satisfied by both. These paths
need an observation the plug-in publishes, and the observation needs its own
rules, because a log line is easy to assert on and easy to assert on wrongly.

There is no cache in this repository's templates (`[I]`, design only). What does
run here (`[V]`) is the shape the rules rest on: the `provider` label publishes
an invisible fact — which execution provider the instance actually got — and
`--label-param` asserts on it, down to refusing a declared default that the
plug-in never wrote (§5.3). The contract test also already collects the plug-in's
messages (`host.messageText()`, printed under `--- plug-in messages ---`), so a
message is available to assert on; nothing asserts on it yet.

1. **The log line is the contract, not a debugging aid.** Once a test depends on
   a message, its wording, its level and its presence are test surface: write it
   down, keep it stable, and make each line identify *which* decision it reports
   (`cache hit key=...`, `inference skipped`) rather than a bare "ok". A line that
   can only be read by a human is a feature without a test; a line the test
   asserts on is exercised by the test. Deleting or rephrasing it is a test
   change, which is the point.
2. **Assert deltas, never absolute counts.** A host and a warm-up render decide
   how many times the plug-in is called, so `hits == 3` encodes the harness, not
   the plug-in. Take the count before and after the frames under test and assert
   the difference: N frames of unchanged input, N-1 hits. Pair the delta with its
   complement — with the cache disabled by a switch, the same N frames must give
   zero hits and a higher inference count — or the count alone proves only that a
   counter exists.
3. **Know the log's lifetime, and its end.** A per-instance buffer cannot show
   that a *second* instance reused the cache; a process-global logger mixes
   instances and threads, so a delta taken around one render includes other
   threads' lines. Decide which of the two the feature needs and say so. A ring
   buffer keeps the *newest* lines and drops the oldest silently, so an assertion
   on an early line passes only while little else was logged — assert on the last
   line, or size the buffer and assert that nothing was dropped.
4. **A probe in the image counts only if it belongs to one code path.** Marking
   the frame to prove the short path ran works exactly when nothing else can draw
   that mark — and then the negative control must show the mark *absent* with the
   cache bypassed. Otherwise "the frame changed" is true for any path that draws,
   which is the overlay smell in §14: the probe tests a second feature, not this
   one. Prefer the log line, and keep the pixel probe for what only pixels can
   prove.
5. **Make it a state log, not a frame log.** One line per rendered frame is a
   flushed syscall per frame — 216,000 for an hour at 60 fps — and a file largest
   exactly when a user needs to read it. Write on the first few frames of an
   instance, then only when the state or the reason changes, and let the test
   follow that shape: assert the **last** line, and add one check that renders
   twenty times and requires that neither the line count nor the byte size moved
   (**B**). That check is what keeps the rule from rotting. Compare the
   *configuration* (which filters were applied), not the composed line, whose
   numbers change every frame — otherwise "changed" is always true and the log is
   a frame log wearing a state-log comment. And when a line grows past three
   conversions, assemble it from a `std::string` instead of a long `printf`: with
   no MSVC equivalent of `__attribute__((format))`, one missing argument reads
   past the varargs and takes the host down on the first render (**B**).
6. **A file that exists is not a file the plug-in wrote.** An installer or a
   previous run leaves a file behind, so a log test that only opens it passes with
   empty content. Write the process id in the header and require it, and
   `std::remove()` the file at the start of the block so the first render is the
   one that must create it — otherwise the test counts the previous run's lines
   (**B**: `model runs +-4` from a stale file).

What this still does not establish: that the cache is *correct* (same answer as
the full path on the frame that follows a hit), only that it was taken. That
comparison is a golden test of its own, and it is the one that catches a cache
keyed on the wrong thing.

---

**Honest summary.** The layers in this document are ordered by cost. This
repository now demonstrates L0–L6 and the two layers it used to only describe:
L1 runs as `testkit/contract_main.cpp` and L3 as `testkit/fake_host.cpp`, both
against the real built binaries, with negative controls proving they can fail.
What remains genuinely unbuilt is L7 (timings), the GPU execution path, and L8
(a real host): a plug-in that has never rendered in Resolve, Nuke or Natron is
not verified, no matter how many unit and contract tests pass. That is stated in
the same terms in [`../SKILL.md`](../SKILL.md) §5 and
[`reference-projects.md`](reference-projects.md) §9.

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[V]** verified in this repository - the command ran here
