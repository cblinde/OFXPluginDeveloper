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
| **L1** artifact | exports, dlopen, plugin struct | minutes | "plug-in does not appear", symbol leakage | the templates: `testkit/`, 110 + 136 + 160 checks (the 136 from a `CPU_ONLY=1` build, so host protocol + CPU path; the 158 are followed by 82 in the tiled-context variant) |
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

Measured (CPU-only build of the CUDA template, this machine): **110 checks, 0
failed** for `ReferenceGain`, **136 checks, 0 failed** for `MinimalCudaGain`,
**160 checks, 0 failed** for `OnnxInference`, followed by **82 checks, 0 failed**
in the tiled-context run (`tileSize=16`, `--roi-context-tolerance 0.02`).
The 136 belong to a `CPU_ONLY=1`
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

`templates/minimal-cuda-ofx/test/run-analysis.sh` runs cppcheck and shellcheck
over the template — its sources and its three scripts, not the repository. A
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

`./run-tests.sh` exits 0 with `RUN_VALGRIND=0` in about two minutes on the
authoring machine; with memcheck enabled (the default when valgrind is on
`PATH`) it takes close to five. `CPU_ONLY=1` forces the CUDA template's CPU path
when no toolkit is installed. In that mode - and on a machine without `nvcc`,
which lands in the same mode - the summary prints `NOT RUN: GPU kernels` next to
a `NOT VERIFIED` block instead of a line claiming kernels ran, so a CPU-only run
cannot be read as one that exercised the device.

Measured on the machines this repository has been exercised on: a Linux x86-64
box with no GPU and no host, and a Windows 11 box with an RTX 3060. The 2026-11-22
Windows rows are **[B]** (that run was on another machine); the 2026-11-24 rows
were run in this checkout, including the inference template's first Windows build
at all ([`onnx-inference-suite.log`](../evidence/2026-11-24-windows/onnx-inference-suite.log)).

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
| Windows artifact | 2 exports (`dumpbin /exports`), 7 cubins `sm_75`…`sm_120` (`cuobjdump -lelf`), no CUDA runtime import **[B]** |
| Windows contract | whole host protocol, 0 failures, through `LoadLibraryExW` **[B]** |
| GPU kernels, hardware | 28/28 launcher cases bit-exact on RTX 3060 sm_86; 200 queued launches, 8 host threads, zero-copy **[B]** |
| GPU memory, 2026-11-24 | `compute-sanitizer --tool memcheck` over `gpu-kernel-test.exe`: **0 errors**, 28/28 still bit-exact under the tool — device memory checked, not just the results **[V]** |
| GPU throughput, 2026-11-24 | 1080p/4K/8K, vec4 **0.209 / 0.850 / 3.479 ms** (317/312/305 GB/s), scalar within 5% — both forms bandwidth-bound, so the vec4 form's 4× instruction reduction buys no throughput. CPU loop 3.456/13.480/55.287 ms; pageable H2D+D2H copy 6.705/27.332/107.396 ms, i.e. **32× the 1080p kernel** **[V]** (table and method in `../README.md` §"What has actually been verified") |
| Artifact + fake host (reference) | 110 checks, 0 failed, two load/unload cycles, two live instances with separate values |
| Artifact + fake host (CUDA) | 136 checks, 0 failed **on a `CPU_ONLY=1` build**: two load/unload cycles, four instances and three concurrent renders, all through the host protocol and the CPU path. The CUDA launcher and kernels are absent from that build, so this row says nothing about the GPU path, which the Windows rows above cover |
| Artifact + fake host (inference) | 160 checks, 0 failed, render matches the Python golden through the OFX API, source RoI checked against the declared halo and shown to carry pixels bit for bit, four instances × three concurrent renders; a second run with `tileSize=16` adds 82 checks for the same comparison on the tiled path, with a 0.02 bound instead of bit-equality |
| Negative controls | `kOfxStatReplyDefault` describe and a wrong parameter type both rejected, with the assertion named |
| Analysis | shellcheck 0.10.0, `-x --severity=style`, over all **eleven** shell scripts of this repository: no findings (run here). cppcheck was **not installed** in this environment, so `run-analysis.sh` reports it skipped — its cppcheck half is unverified here, not clean |

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
