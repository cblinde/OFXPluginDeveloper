# Testing OFX plug-ins

How to test an OFX plug-in well enough to ship it, in the order that catches
failures cheapest-first. This is the "how"; [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md)
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

---

## 1. The layers, cheapest first

| Layer | What it tests | Cost to build | What it catches | Here |
|---|---|---|---|---|
| **L0** core | maths, packing, tiling, formats | minutes | wrong pixels, wrong tensor layout | both templates |
| **L1** artifact | exports, dlopen, plugin struct | minutes | "plug-in does not appear", symbol leakage | both templates: `testkit/`, 74 + 80 checks |
| **L2** static | property types, forbidden calls, bundle layout | minutes | type confusion, spec violations | `test/run-analysis.sh`, [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) §1 |
| **L3** fake host | actions, lifecycle, error codes, params | a day | wrong status codes, crashes on describe/create/render | both templates: `testkit/fake_host.*` |
| **L4** images | goldens, references, geometry matrix | a day | seams, shear, alpha loss, off-by-one | both templates |
| **L5** memory | ASan, UBSan, TSan, valgrind | an hour | UB, leaks, data races | both templates |
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

**Exports.** Exactly two dynamic symbols, both registry functions:

```bash
nm -D --defined-only <bundle>/Contents/Linux-x86-64/<Name>.ofx | grep ' T '
# OfxGetNumberOfPlugins
# OfxGetPlugin
```

Measured in this repository: both templates report exactly 2. Removing the
visibility block from `templates/minimal-cuda-ofx/CMakeLists.txt` and rebuilding
CPU-only reported **663**, because the statically linked Support library's
symbols become visible. That is the failure mode the rule exists to prevent.

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
recipe, and both templates run it against the binary their own `build.sh`
produced — it does not link the plug-in, it `dlopen`s it:

```bash
./run-tests.sh        # builds both templates, then runs every suite below
# per template, the step is:
testkit/build-contract-test.sh /tmp/contract_test <openfx-checkout>
/tmp/contract_test build/bundle/<Name>.ofx.bundle/Contents/Linux-x86-64/<Name>.ofx \
  --id com.ofxplugindeveloper.minimalcudagain \
  --param gain:double --param gainAlpha:double --clip Source --clip Output \
  --identity --set gain=2.0 --not-identity-after-set --byte-dst-rejected
```

Measured (CPU-only build of the CUDA template, this machine): **74 checks, 0
failed** for `MinimalCudaGain`, **80 checks, 0 failed** for `OnnxInference`. The
checks are: the two export symbols; `pluginApi`, `apiVersion`, non-empty
`pluginIdentifier` (stable across a second load), `pluginVersionMajor/Minor`;
non-null `setHost`/`mainEntry`; and then a whole `describe → describeInContext →
createInstance → isIdentity → render → destroyInstance → unload` sequence run
**twice in one process**, each time followed by `dlclose`.

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
| Property types | every descriptor `…Supported` read with `propGetString`; every per-render `…Enabled` with `propGetInt` | the strings/ints split is the most common type confusion ([`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md) §3.2) |
| No device-wide sync | `grep -rn cudaDeviceSynchronize src/` returns nothing | forbidden by `ofxGPURender.h:610` |
| No tight-stride assumption | `grep -rn "width \* 4 \* sizeof" src/` returns nothing | shear on padded rows |
| No `stdout` | `grep -rn "printf\|std::cout" src/` (except test code) | invisible in a GUI host |
| Bundle layout | bundle folder, binary stem and `.ofx` suffix agree | host scans that exact path |
| `RUNPATH` | `readelf -d` shows `$ORIGIN`, no build-machine path | plug-in fails on another machine |
| Capability guard | every `setSupportsCudaRender(true)` inside `#ifdef` | advertising what you did not build |

`templates/minimal-cuda-ofx/test/run-analysis.sh` runs cppcheck and shellcheck
over the template; the gate list as a whole is
[`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) §1.

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

**Built here: `testkit/`.** The SDK does not give you a host — `.ofx-ref/Support/`
contains the plug-in-side library, the sample plug-ins and `PropTester`, which is
itself a plug-in (it builds via `add_ofx_plugin`). So the host was written, once,
and both templates share it rather than each carrying a copy.

### 5.1 What is in it

| File | Lines | What it is |
|---|---|---|
| `testkit/fake_host.h` | public API | `FakeHost` plus the image options (padding, negative pitch, byte destination) |
| `testkit/fake_host.cpp` | ~1,100 | the five suites + message suite, the object model (descriptor, instance, clips, parameters, images) |
| `testkit/contract_main.cpp` | ~500 | the checks, the `dlopen`, and the command line |
| `testkit/build-contract-test.sh` | ~50 | compiles both into one binary; finds headers from the template's `CMakeCache.txt` |
| `testkit/negative-controls/` | 2 stubs | deliberately broken plug-ins that prove the harness fails |
| `testkit/run-negative-controls.sh` | ~70 | builds the controls, asserts each is rejected with the right message |

Coverage, precisely: all **18** `OfxPropertySuiteV1` functions (including the `*N`
and `propGetDimension` variants), all **13** `OfxImageEffectSuiteV1` functions,
all **18** `OfxParameterSuiteV1` functions, all **9** `OfxMultiThreadSuiteV1`
functions (single-threaded `multiThread`, real mutexes), both memory functions,
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
`render` writes the expected pixels (a scale of the source, or a golden file); and
a byte destination is rejected with `kOfxStatErrUnsupported`.

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

A harness that has only ever passed is untested. Two controls, run by
`testkit/run-negative-controls.sh` (deliberately **not** part of `./run-tests.sh`,
so the normal run stays green):

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

negative controls: both faults detected, harness is not a rubber stamp
```

The first stub answers `kOfxActionDescribe` with `kOfxStatReplyDefault`; the
second declares `gain` as `OfxParamTypeInteger`. Neither is caught by a compiler
and neither crashes — only reading the status code and the declarations catches
them, which is the difference between this layer and L0.

### 5.4 What the fake host still cannot prove

1. **The multi-resolution and tile protocol.** It never sets
   `kOfxImageEffectPropTilesRoD` and never slices a render window, so a
   `setSupportsTiles(true)` path is untested here.
2. **Clip preferences and host scheduling.** `getClipPreferences`, per-clip depths
   and PARs, and concurrent renders of several instances are not exercised;
   `multiThread` runs the function once, on one thread.
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
clean under ASan/UBSan (74/74 and 80/80 checks, no sanitizer report) and under
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

Measured here at the kernel level: 6 threads running the real kernel body on
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
| 12 | Force a crash once, restart | the host's blacklist; know where to clear it ([`SKILL.md`](SKILL.md) §7) |

Add host-specific rows from [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md)
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
| Sidecar stalls | bounded timeout, no hang ([`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) §3) |
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

The release gate itself is [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md):
§1 lists the mechanical gates with their commands and, importantly, what a pass
does *not* prove; §2–§4 are the questions a reviewer signs off.

---

## 13. What this repository actually runs

Reproduce all of it from a clean checkout; nothing needs a GPU or a host.

```bash
# Everything, both templates: build, unit, image, artifact, fake host, memcheck
ONNXRUNTIME_ROOT=/path/to/ort ./run-tests.sh

# Or one template at a time:
cd templates/minimal-cuda-ofx && ./build.sh && ./test/run-tests.sh
cd templates/onnx-inference-ofx && ONNXRUNTIME_ROOT=/path/to/ort \
  ./build.sh && ./test/run-tests.sh

# Proof that the harness detects faults (two deliberate failures, not in the run above)
testkit/run-negative-controls.sh
```

`./run-tests.sh` exits 0 with `RUN_VALGRIND=0` in about two minutes on the
authoring machine; with memcheck enabled (the default when valgrind is on
`PATH`) it takes close to five. `CPU_ONLY=1` forces the CUDA template's CPU path
when no toolkit is installed.

Measured on the machine that wrote this document (Linux x86-64, no GPU, no host):

| Suite | Result |
|---|---|
| CUDA kernels | 3,240 grid launches (vec4 = 1,152, scalar = 2,304), 0 failures |
| CUDA CPU loop | 1,464 components compared, 0 failures |
| CUDA concurrency | 6 threads, 0 failures, TSan-clean |
| CUDA bundle | 2 exports; valgrind `0 errors from 0 contexts` |
| Inference golden | C++ vs Python max absolute difference = 0 over the frame |
| Inference tiling | tile 5/13/17/256 and odd 1×1, 7×5, 64×3, 3×48 all `0` |
| Inference control | `halo = 0` deviates by `0.263276` (expected) |
| Inference bundle | 2 exports, `NEEDED libonnxruntime` = 1, `RUNPATH $ORIGIN` = 1 |
| Artifact + fake host (CUDA) | 74 checks, 0 failed, two load/unload cycles |
| Artifact + fake host (inference) | 80 checks, 0 failed, render matches the Python golden through the OFX API |
| Negative controls | `kOfxStatReplyDefault` describe and a wrong parameter type both rejected, with the assertion named |
| Analysis | cppcheck clean, shellcheck clean |

Not run here, and therefore not claimed: no GPU execution (the CUDA template was
built CPU-only for the runs above), no host render (no Resolve/Nuke/Natron in
this environment), no performance numbers, no Windows build, and no
multi-resolution/tile-protocol test (the fake host does not speak it — §5.4).

---

## 14. Test smells: passing for the wrong reason

| Smell | Why it proves nothing | Fix |
|---|---|---|
| Tiled output compared to tiled output | both sides share the bug | compare to whole-frame, and add the `halo = 0` control |
| Golden regenerated by the code under test | the test encodes the bug | generate goldens with an independent implementation (here: Python ONNX Runtime) |
| "It loads" / "it did not crash" | no assertion | assert the structure fields, the status codes, the pixels |
| CPU path compared to CPU path on a GPU build | never exercises the GPU path | assert which path ran, and fail if the expected one did not |
| One size, one frame | 1920×1080 hides stride bugs | geometry matrix (§6) |
| Sanitizer build skipped "because it is slow" | the only check that sees UB | run it on every PR; it is seconds |
| Test disabled "flaky" without a cause | a race, usually | TSan, then fix it; flaky is a diagnosis, not a status |
| Numbers in a commit message | unreproducible within a week | a benchmark file with the command, the machine and the distribution |

---

**Honest summary.** The layers in this document are ordered by cost. This
repository now demonstrates L0–L6 and the two layers it used to only describe:
L1 runs as `testkit/contract_main.cpp` and L3 as `testkit/fake_host.cpp`, both
against the real built binaries, with negative controls proving they can fail.
What remains genuinely unbuilt is L7 (timings), the GPU execution path, and L8
(a real host): a plug-in that has never rendered in Resolve, Nuke or Natron is
not verified, no matter how many unit and contract tests pass. That is stated in
the same terms in [`SKILL.md`](SKILL.md) §12 and
[`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md) §9.
