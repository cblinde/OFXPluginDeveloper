# Pre-flight review checklist

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
| Data races | `SANITIZE=thread ./test/run-tests.sh` | The `[4/4]` concurrent test runs clean: the kernels touch only their arguments and their own indices. |
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
| D2 | Does the binary export exactly the two entry points the loader calls? | `ofxCore.h` line 558 (`OfxGetPlugin`) and lines 572-573: "The order of calls is then: 1) `OfxSetHost`, 2) `OfxGetNumberOfPlugins`, 3) `OfxGetPlugin`." | `nm -D --defined-only` must show two `T` symbols. Everything else — including the Support library — stays hidden; the template enforces this with `-fvisibility=hidden`. |
| D3 | Is the plug-in API string and version exactly what the host expects? | `ofxImageEffect.h` line 26: `kOfxImageEffectPluginApi` is `"OfxImageEffectPluginAPI"`. | The factory passes the string and a version; a typo yields a plug-in the host enumerates and then refuses to instantiate. |
| D4 | Does every error path fail loudly? | `ofxImageEffect.h` line 1610: on `kOfxStatErrMemory` the "plugin should abort whatever it was doing". | A swallowed error means the host composites a black or stale frame and calls it a render. Throw the suite status exception instead. |
| D5 | Do you match the pixel depth you were given instead of converting silently? | `ofxImageEffect.h` lines 896-901: the valid depths are Byte, Short, Half and Float, and "for a clip, this is the value set by the clip preferences action, not the raw 'actual' value". | If `render()` only handles Float, then the descriptor must only advertise Float. Anything else is a conversion you did not write. |

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
[ ] RUN_VALGRIND=1 ./test/run-tests.sh            pass / fail
[ ] ./test/run-analysis.sh                        pass / fail
[ ] A1-A6 render path                             answered in this PR
[ ] B1-B6 image addressing                        answered in this PR
[ ] C1-C4 capability negotiation                  answered in this PR
[ ] D1-D5 hosts and packaging                     answered in this PR
[ ] Not verified: <device execution / host render / timing> - stated in the PR
```
