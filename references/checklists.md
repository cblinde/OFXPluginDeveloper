# Checklists: pre-flight review and sign-off

The questions to answer before shipping, each settled by a header line or a spec sentence, plus the mechanical gates.

<!-- PREFLIGHT_REVIEW_CHECKLIST.md lines 1-151 -->
<!-- SKILL.md lines 667-723 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 496-509 -->
<!-- PREFLIGHT_REVIEW_CHECKLIST.md lines 1-21 -->
## Pre-flight review checklist

A list of questions to answer about **your** plug-in before it ships. Every item
names the sentence that settles it — a header doc block, a line of the OFX
reference, or a documented CUDA rule. None of them rests on "the popular repo
does it this way", because that is how most of these bugs get copied forward.

Three ground rules:

1. **An item is answered from your source, not from memory.** Each one has a
   mechanical check; if the check needs a judgement call, the item says so.
2. **"The host didn't crash" is not an answer.** A host can ignore a wrong
   property type for years and then validate it in a point release.
3. **If an item does not apply to your plug-in, write down why.** "N/A" with no
   reason is how a checklist turns into decoration.

Run [§1](#1-mechanical-gates) first — it is automated and takes about a minute.
Then work through [§2](#2-the-questions) once per release.

---

## 1. Mechanical gates

All of these run without a GPU and without a host. They live in the template's
`test/` directory and are committed, so a reviewer can reproduce every claim.

| Gate | Command | What a pass means |
|---|---|---|
| Host suite | `./test/run-tests.sh` | Kernel bodies match a reference byte for byte, the alignment predicate decides the documented cases, the CPU loop is unchanged after hoisting, and concurrent runs do not interfere. |
| Memory safety | `SANITIZE=address,undefined ./test/run-tests.sh` | No out-of-bounds access, no use-after-free, no uninitialised read, no signed-overflow or misaligned-access undefined behaviour **in the code the harness exercises**. |
| Data races | `SANITIZE=thread ./test/run-tests.sh` | The `[4/4]` concurrent test runs clean: the kernels touch only their arguments and their own indices. Build the plug-in with `-fsanitize=thread` too, or this says nothing about the plug-in image. |
| Thread-safety claim | `contract_test … --concurrency-param <numeric>` | Two live instances keep separate values and the declared level holds: `InstanceSafe` renders different instances in parallel, `FullySafe` also one instance across threads, `Unsafe` is skipped **by name**. A pass without the flag proves only that serial renders work. |
| Leaks | `RUN_VALGRIND=1 ./test/run-tests.sh` | memcheck: `0 errors from 0 contexts`, all heap blocks freed. |
| Static analysis | `./test/run-analysis.sh` | cppcheck and shellcheck report nothing unresolved over `src/Plugin.cpp`, `test/` and the scripts. |
| ABI | `nm -D --defined-only <bundle>/*.ofx \| grep ' T '` | Exactly two dynamic exports (see [D2](#d-hosts-and-packaging)). |

Each gate also prints the reason it skipped if a tool is missing, so a green run
with no tool installed cannot be mistaken for a green run with it.

### What these gates cannot prove

Stated plainly, because a checklist that overclaims is worse than none:

| Claim | Status here | Why |
|---|---|---|
| Any kernel executed on a GPU | **Not proven** | There is no device in the verification environment; `/dev/nvidia*` is absent. The harness runs the kernel *bodies* on the CPU. |
| The plug-in renders correctly in a host | **Not proven** | No host is installed. Action dispatch, clip preferences and the OpenGL/Metal/OpenCL paths are untested. |
| Stream ordering against the host's stream | **Not proven** | Requires a device plus a host that supplies `kOfxImageEffectPropCudaStream`. The code is structured for it (see [A3](#a-the-render-action-and-device-work)) but the ordering is unverified. |
| Speed | **Not measured** | No timing is quoted anywhere in this repository. The PTX counts in `run-tests.sh` are an *instruction-selection* check, not a benchmark. |

### Sanitizer coverage, and what is genuinely inapplicable

| Check | Applies to | Reason it does or does not apply |
|---|---|---|
| ASan + UBSan | The host harness | It is the only code that runs here. Catches addressing and arithmetic errors in the same expressions the device executes. |
| TSan | Only `[4/4]`, the concurrent test | The rest of the harness is single-threaded, so TSan has nothing to observe there. The concurrent test exists *because* TSan needs real threads to say anything; a race in the kernels' index handling would show up as either a TSan report or a mismatch. |
| Valgrind memcheck | The host harness, in an unsanitised build | memcheck and a `-fsanitize` runtime both replace the allocator and thread runtime; the combination is rejected, so `run-tests.sh` runs them as separate stages and prints the reason when it skips. |
| Valgrind / ASan on the PTX step | **Never** | The PTX check reads compiler output with `awk`/`grep`. No generated code is executed, so there is nothing for a runtime checker to observe. |
| Valgrind / ASan on the bundle check | **Never** | `nm` and `readelf` are separate processes reading an ELF file. |
| clang-tidy | **Not run** | The verification machine has no clang toolchain (GCC 13 only). cppcheck covers the same classes of finding here; nothing is expected to need a suppression, but that is unmeasured rather than clean. |

---

## 2. The questions

### A. The render action and device work

| # | Question | Settled by | How to confirm |
|---|---|---|---|
| A1 | Does any render path call `cudaMalloc`/`cudaFree`, or copy host↔device, **per render**? | CUDA Programming Guide §4.3.2 *Stream-Ordered Memory Allocator*: "Application performance can be improved by avoiding potentially costly synchronization of `cudaMalloc` and `cudaFree`." The Best Practices Guide §10.3 adds that both "are expensive operations". | `grep -n "cudaMalloc\|cudaFree\|cudaMemcpy" src/` — every hit must be in setup/teardown, not inside the function the render action calls. |
| A2 | Do you ever wait on the device — `cudaDeviceSynchronize`, `cudaStreamSynchronize`, or an event `Synchronize()` — in the render path? | `ofxGPURender.h` doc block of `kOfxImageEffectPropCudaStream` (lines 608-610): "the plug-in SHOULD NOT wait for final asynchronous operations to complete before returning from the render action, and SHOULD NOT call `cudaDeviceSynchronize()` at any time." | `grep -rn "cudaDeviceSynchronize\|cudaStreamSynchronize\|cudaEventSynchronize" src/` must be empty, or every hit must be on a stream you created for your own staging work — and then explain why. |
| A3 | Is the launch on the stream the host passed in `kOfxImageEffectPropCudaStream`, and do you behave correctly when it is absent? | `ofxGPURender.h` lines 600-616: if the property is not set, "the plug-in SHOULD ensure that any asynchronous operations it enqueues have completed before returning from the render action." | Read your render function once per release and answer both halves: which stream, and what happens when the property is missing. |
| A4 | When `kOfxImageEffectPropCudaEnabled` is 0 or absent, do you treat `kOfxImagePropData` as CPU memory? | `ofxGPURender.h` lines 559-575: the host "MAY set this property" (Type - `int X 1`) only when both sides advertise CUDA; value 0 means "a CPU memory pointer". `ofxImageEffect.h` lines 1299-1305 list every shape `kOfxImagePropData` can take. | Follow the branch: what does render do when the flag reads 0? If the answer is "crash" or "silently assume CUDA", fix it. |
| A5 | Does `describe()` advertise only what `render()` actually honours, for each bit depth and each GPU API? | `ofxGPURender.h` lines 547-553: the capability property is "Default - `"false"` for a plug-in", i.e. opt-in, and the descriptor is what the host reads. | Cross-check `addSupportedBitDepth` and every `...RenderSupported` line against the branches in `render()`. The template's own suite covers the CPU half of this; the GPU half is by inspection. |
| A6 | Can `render()` return `kOfxStatReplyDefault`? | `ofxImageEffect.h` lines 403-404: "The render action *must* be trapped by the plug-in, it cannot return `kOfxStatReplyDefault`. The pixels needs be pushed I'm afraid." | `grep -n "kOfxStatReplyDefault" src/` — any hit in a render path is a bug. |

### B. Images and addressing

| # | Question | Settled by | How to confirm |
|---|---|---|---|
| B1 | Is every pixel address computed from `kOfxImagePropRowBytes`, never from `width * components * sizeof(float)`? | `ofxImageEffect.h` lines 1347-1348: "This property indicates the number of bytes in a row of pixels. This will be at least `sizeof(PIXEL) * (bounds.x2-bounds.x1)`." | `grep -n "rowBytes\|getRowBytes" src/` — and then look for any `y * w * 4`-shaped expression next to it. |
| B2 | Do you handle a **negative** row bytes? | `ofxImageEffect.h` line 1350: "row bytes can be negative, which allows hosts with a native top down row order to pass image into OFX without having to repack pixels." | The template answers this by construction: signed 64-bit (`long long`) arithmetic in the launcher and a signed modulo in `IsVec4Addressable`, so a negative multiple of 16 still takes the vector path. The predicate table in `run-tests.sh` covers it. |
| B3 | If you touch OpenCL **Images**, do you avoid row-bytes arithmetic? | `ofxImageEffect.h` line 1351: "Row bytes is not supported for OpenCL Images." | If your OpenCL path is Buffers-only, say so in `describe()` and skip this; otherwise the access pattern has to come from the image, not a stride. |
| B4 | Does your `(x, y) → address` mapping take the origin from `kOfxImagePropBounds` instead of assuming `(0, 0)`? | `ofxImageEffect.h` lines 1315-1321: "X values are x1 <= X < x2, Y values are y1 <= Y < y2"; for partial images the bounds are contained by the region of definition. | Render at a non-zero window origin. The harness does this for the kernels (`ox`, `oy` in the fixtures) but cannot do it for your host integration. |
| B5 | Is the loop bound the render **window** while the addressing uses the **bounds**? | Same lines 1315-1321, plus the render action's `kOfxImageEffectPropRenderWindow` argument. | Two different rects, two different jobs: window says which pixels to write, bounds say where the image starts. Mixing them writes outside the destination. |
| B6 | Does the kernel write only inside the window? | Nothing in the spec will catch this for you: the destination buffer is big enough, so an overrun corrupts a neighbouring region silently. | The template's `[2/4]` test fills the destination with a sentinel and asserts the sentinel survives outside the window. Do the same in yours. |

### C. Capability negotiation (the type traps)

| # | Question | Settled by | How to confirm |
|---|---|---|---|
| C1 | Are the six capability properties (`...CudaRenderSupported`, `...MetalRenderSupported`, `...OpenCLRenderSupported`, `...OpenGLRenderSupported`) read with **`propGetString`**? | `ofxGPURender.h` doc blocks: lines 548, 628 and 686 each say "Type - string X 1"; the OpenGL property is "Type - `C string X 1`". | Reading one of these with `propGetInt` is undefined behaviour that often "works" until a host validates the type. This is the single most common wrong-type bug in the GPU path. |
| C2 | Are the per-render flags (`...CudaEnabled`, `...MetalEnabled`, `...OpenCLEnabled`) read with **`propGetInt`**? | `ofxGPURender.h` line 565 ("Type - `int X 1`", CUDA) and line 645 (Metal, same). | Six strings, three ints. A single helper that guesses the type from the name is the bug. |
| C3 | Does your OpenGL capability handle the third value, `"needed"`? | `ofxGPURender.h` doc block of `kOfxImageEffectPropOpenGLRenderSupported`: "Type - `C string X 1`", valid values `"false"`, `"true"`, `"needed"`. | `"false"`/`"true"` are the easy two. `"needed"` means the host *requires* OpenGL for this render — treat it as "must", not "may". |
| C4 | Do you treat an unreadable or missing capability property as "false"? | `ofxGPURender.h` lines 547-553: "Default - `"false"` for a plug-in". | A property that cannot be fetched is not permission. Fail toward the CPU path. |

### D. Hosts and packaging

| # | Question | Settled by | How to confirm |
|---|---|---|---|
| D1 | Do you advertise Metal (or OpenCL) only when the **host** advertises it — not when the platform could support it? | `ofxGPURender.h` lines 628-634: the property lives on "plug-in descriptor (read/write), host descriptor (read only)", and the host's value is what you must agree with. | Answer with a sentence: "we set Metal true if and only if `<host property> == "true"`". A platform macro (`#ifdef __APPLE__`) is not an answer: the crash that motivated this item comes from a host that never tested the path. |
| D2 | Does the binary export exactly the two entry points the loader calls? | `ofxCore.h` line 558 (`OfxGetPlugin`) and line 566 (`OfxGetNumberOfPlugins`). `OfxSetHost` is declared at `:577` but the **Support library does not implement it**; the host hands itself over through `OfxPlugin::setHost` (`ofxCore.h:136`), which Support fills in at `ofxsImageEffect.cpp:3026`. | `nm -D --defined-only` must show exactly two dynamic symbols, both `T`. `-fvisibility=hidden` alone leaks five weak `std::` helpers (measured: 7 symbols), so pin the export list with a version script — `templates/minimal-ofx/exports.map`, proved by its `build.sh`. |
| D3 | Is the plug-in API string and version exactly what the host expects? | `ofxImageEffect.h` line 26: `kOfxImageEffectPluginApi` is `"OfxImageEffectPluginAPI"`. | The factory passes the string and a version; a typo yields a plug-in the host enumerates and then refuses to instantiate. |
| D4 | Does every error path fail loudly? | `ofxImageEffect.h` line 1610: on `kOfxStatErrMemory` the "plugin should abort whatever it was doing". | A swallowed error means the host composites a black or stale frame and calls it a render. Throw the suite status exception instead. |
| D5 | Do you match the pixel depth you were given instead of converting silently? | `ofxImageEffect.h` lines 896-901: the valid depths are Byte, Short, Half and Float, and "for a clip, this is the value set by the clip preferences action, not the raw 'actual' value". | If `render()` only handles Float, then the descriptor must only advertise Float. Anything else is a conversion you did not write. |

---

### E. Inference (only if the plug-in runs a model)

| # | Question | Settled by | How to confirm |
|---|---|---|---|
| E1 | Is `intra_op_num_threads` set explicitly? | ONNX Runtime threading docs: the default is `0`, which means "one thread per physical core" **per session**, with affinity; an explicit value disables the affinity. Hosts call `render()` on several threads, so the default multiplies. | `grep -n "SetIntraOpNumThreads" src/` — it must be there with a deliberate number, and the comment must say which assumption it encodes. |
| E2 | If you use a GPU execution provider, is the launch on `kOfxImageEffectPropCudaStream`, with EP synchronisation disabled? | `ofxGPURender.h:608-610` ("SHOULD NOT wait … SHOULD NOT call `cudaDeviceSynchronize()` at any time") plus ORT's `user_compute_stream` / `disable_synchronize_execution_providers` options. | `grep -rn "user_compute_stream\|disable_synchronize" src/`. Either both are present, or the plug-in does not advertise a GPU path. |
| E3 | Do you advertise CUDA only when every op in the model runs on the GPU provider? | `ofxGPURender.h:547-553`: the capability is opt-in ("Default - `"false"` for a plug-in") and the host's descriptor is read-only. | Check the provider's supported-op list for the exact ops in your graph; a single CPU fallback op makes the "GPU" path slower than the CPU one, and the host cannot see that. |
| E4 | Is the tile halo at least the model's receptive-field radius? | Measured in `templates/onnx-inference-ofx`: `halo=0` differs from whole-frame inference by 0.263 on a single 3×3 convolution; `halo=1` is bit-identical. | Run `./test/run-tests.sh` in that template with your model and your tile size; the negative control tells you whether your halo is doing anything. |
| E5 | Are channel order, value range and colour space documented next to the model? | The model's training contract, not OFX: see `onnx-and-tensorrt.md` §5. The LTX plug-in converts through OCIO before inference for exactly this reason. | Put the three facts in the parameter hint or a README beside the weights. A wrong range is a silent grade error, not a crash. |

---

## 3. The five failures this checklist exists to prevent

Each of these has been observed in a real open-source OFX plug-in, and each one
is cheap to check and expensive to debug in a host.

| Failure | Why it survives review | Checklist item |
|---|---|---|
| Per-render `cudaMalloc` + copy, then a sync before `cudaFree` | The plug-in still produces correct pixels; only the pipeline timing suffers, and nothing in the host reports it. | A1, A2 |
| `((y * width) + x) * 4` instead of row-bytes arithmetic | Works on every packed image, shears on the first padded or bottom-up one — often only in a specific host. | B1, B2 |
| Capability property read with `propGetInt` | The first host accepts it; the next one validates types and the plug-in vanishes. | C1, C2 |
| Metal advertised from `#ifdef __APPLE__` | Crashes in the hosts that never implemented the path; fine in the one the author used. | D1 |
| Synchronising the host's stream | Correct output, no error, but every downstream node waits for this one. | A2, A3 |

## 4. Reviewer's sign-off

Copy this into the pull request. "N/A" needs the reason next to it.

```text
[ ] ./test/run-tests.sh                          pass / fail
[ ] SANITIZE=address,undefined ./test/run-tests.sh   pass / fail
[ ] SANITIZE=thread ./test/run-tests.sh           pass / fail / no concurrent path
[ ] contract_test --concurrency-param <name>     pass / fail / declared Unsafe
[ ] RUN_VALGRIND=1 ./test/run-tests.sh            pass / fail
[ ] ./test/run-analysis.sh                        pass / fail
[ ] A1-A6 render path                             answered in this PR
[ ] B1-B6 image addressing                        answered in this PR
[ ] C1-C4 capability negotiation                  answered in this PR
[ ] D1-D5 hosts and packaging                     answered in this PR
[ ] Not verified: <device execution / host render / timing> - stated in the PR
```

## 10. Checklists

**Before the first build**

- [ ] Identifier is reverse-DNS and unique.
- [ ] Bundle folder name == binary stem == `<Name>.ofx`.
- [ ] Source and output clips defined with explicit supported components.
- [ ] `addSupportedBitDepth` matches what `render()` accepts.
- [ ] CUDA flags guarded by `#ifdef OFXPLUGIN_WITH_CUDA`.
- [ ] `Kernel.cuh` compiles without CUDA headers.
- [ ] CPU fallback implemented and reachable, or
      `kOfxImageEffectPropCPURenderSupported` set `"false"` on purpose.
- [ ] Spatial/thumbnail/colour-management declarations from Phase 0 are written
      down ([`colour-and-alpha.md`](colour-and-alpha.md),
      [`roi-tiling-and-render-hints.md`](roi-tiling-and-render-hints.md)).
- [ ] Global accesses go through the host pitch, and the hot path uses one vector
      load/store per pixel where the pointers allow it (`./test/run-tests.sh`
      asserts this in PTX).

**Before calling it done**

- [ ] Compiles clean on the intended toolchain(s); warnings read, not ignored.
- [ ] Linux: `nm -D --defined-only | grep ' T '` shows only the two entry points;
      weak `W` helpers are gone too if the version script is used
      (`templates/minimal-ofx/exports.map`).
- [ ] Windows: `dumpbin /exports <Name>.ofx` shows the same two names and nothing
      else; `cuobjdump -lelf` lists the cubins when CUDA is on. Both are checked
      by `testkit/check-ofx-artifact.bat` with `--strict`, which finds
      `dumpbin`/`cuobjdump` through `vswhere` and the CUDA roots and fails when
      it cannot.
- [ ] `dlopen`/`LoadLibraryEx` succeeds and the plugin enumerates (id, version,
      mainEntry).
- [ ] `testkit\verify-bundle.bat <build> --plugin-id <id>` — the bundle layout,
      the official OpenFX host (`HostSupport` + `cacheDemo`: load, describe,
      contexts) and a headless `NatronRenderer` render of one 256×256 frame whose
      pixels are checked in linear space. `--tier1-only` is the CI form without
      Natron. This is a host *process*, not a GUI session: see
      [`verification-status.md`](verification-status.md) for what it still leaves
      unproven.
- [ ] Rendered in at least one real host, in both GPU and CPU modes.
- [ ] Identity path verified (no render when params are neutral).
- [ ] Non-square PAR and odd resolutions tested.
- [ ] `isEnabledCudaRender` false path tested (force OpenCL/CPU).
- [ ] No `cudaDeviceSynchronize` anywhere: `grep -rn cudaDeviceSynchronize src/`.
- [ ] Every error path logs and throws.
- [ ] `./test/run-tests.sh` passes: predicate, kernel equivalence, CPU loop,
      concurrency, PTX counts, bundle hygiene.
- [ ] Windows: `test\run-tests.bat` — the artifact check, `test/gpu-kernel-test.cu`
      on the target GPU (28 cases, the only check that executes real SASS) and
      the contract harness through `LoadLibraryExW`; the Linux equivalents come
      from `./test/run-tests.sh`, and neither replaces a host render.
- [ ] Clean under `SANITIZE=address,undefined`, `SANITIZE=thread` and
      `RUN_VALGRIND=1`; `./test/run-analysis.sh` reports nothing.
- [ ] Every `kOfx*` property is read with its declared type, and every pre-ship
      question in this file has an answer.
- [ ] Verification status written down honestly ([`verification-status.md`](verification-status.md)).

---

## 11. Checklist for a new GPU effect

1. Maths in one place, or a test that proves two copies agree.
2. One launch per render; no per-frame allocation; no sync when a stream exists.
3. Pitch in bytes, `int64_t`, signed, window origin applied by hand.
4. `float4` (or `uint4`) accesses where the format allows; `__launch_bounds__` set.
5. Both `CXX_VISIBILITY_PRESET` and `CUDA_VISIBILITY_PRESET` hidden; 2 exports.
6. Artifact verified by `nm`/`readelf`/`cuobjdump`, not by trust.
7. Tested at 1919×1081, 1×1, identity, and against the CPU path.
8. Read back with `compute-sanitizer` on a small frame.
9. `.ofx` installed with the host closed; plugin cache cleared after a crash.
