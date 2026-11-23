# Field test: Windows 11, CUDA 13.0, RTX 3060 (2026-11-22)

Every **[B]** claim in this corpus comes from external runs on 2026-11-22: the
Windows/GPU build below, a second Windows session that reviewed the ONNX
template (§9), and a third that built a YOLO detector from it (§10). This
document is the evidence for all three: what was executed, what the logs say,
and what the runs do not prove. Nothing on this page ran in this checkout. The
raw material is archived verbatim under
[`evidence/2026-11-22-windows/`](evidence/2026-11-22-windows/): the handover
report (`report.txt`), the ten logs it cites, the second review
(`review-onnx-upscale.txt`) and the third (`review-yolo-detector.txt`).

**Provenance.** An agent session on 2026-11-22, at the repository owner's
request, cloned `main`, ran the documented Windows workflow, fixed the build and
test plumbing that failed, re-ran everything, and handed back the report, the
logs and a patch. The patch is **not** applied verbatim: this tree has diverged
since (multi-instance contract checks, per-platform bundle directories, the
strict artifact check), so the archive is a record, not a build input. Where the
two disagree, the code and the **[V]** results in
[`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) win.

**Labels.** **[B]** external evidence, grade B in
[`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md): measured, but
not here, and not reproducible on this machine.

## 1. Environment

| Component | Value | Why it matters |
|---|---|---|
| OS | Windows 10.0.26300 (Windows 11 26H1) | the only Windows run in the corpus |
| GPU | RTX 3060, compute 8.6, 28 SMs, 12287 MiB | `sm_86` is one of the seven built architectures |
| Driver | 621.24, CUDA UMD 13.5 | newer than either installed toolkit |
| CUDA Toolkit | 13.0.48 on `PATH`; 12.9 also installed | the corpus was written against 12.9 |
| CMake | 4.4.4 | this checkout verifies with 3.31.6 |
| Generators | VS 18 2026, 17 2022, 16 2019, 15 2017, Ninja | the matrix in [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 come from this |
| Visual Studio | BuildTools 18 (2026), MSVC 19.44 **and** 19.50 | 19.50 is what `nvcc` 13.0 rejects |
| | BuildTools 2022 installed **without** `VC/Tools/MSVC` | the `vswhere -requires` case |
| Shell | Git Bash (MSYS2) plus `cmd`; **no** `g++`, `clang`, `nm`, `readelf`, `valgrind` | why the Linux suites cannot run there |
| Policy | `NoDefaultCurrentDirectoryInExePath=1` | why `.\\build.bat` is the documented spelling |

## 2. What was executed

| # | Test | Result |
|---|---|---|
| T1 | documented Windows build, `build.bat Release` | failed at four independent points, passed after the fixes |
| T2 | artifact contract: `dumpbin /exports`, `/dependents`, `cuobjdump -lelf` | 2 exports, 7 cubins, no CUDA runtime import |
| T3 | host contract harness ported to `LoadLibraryExW` | 74 checks, 0 failed |
| T4 | CPU-only build through the Visual Studio generator | passes, `.ofx` in `Contents\\Win64`, no `/Release` suffix |
| T5 | reference template's new `build.bat`: VS, cached, Debug, Ninja | all four land in `Contents\\Win64` |
| T6 | `test/gpu-kernel-test.cu` on the GPU | 28/28 launcher cases, bit-exact |
| T7 | throughput, `cudaEvent` | vec4 0.212 ms, scalar 0.222 ms at 1080p |
| T8 | `ctypes.WinDLL` load and enumerate | 1 plug-in, correct id/version |
| T9 | `run-tests.sh`, `test/run-tests.sh`, `run-analysis.sh` | not runnable on Windows (no toolchain for them) |
| T10 | render in Resolve/Nuke/SCRATCH/Natron | **not possible**: no host installed |

## 3. The artifact on Windows

`dumpbin` and `cuobjdump` on the built bundle (report §5.3; the build tail that
produced it is in
[`t7-final-cuda.log`](evidence/2026-11-22-windows/t7-final-cuda.log)):

```text
dumpbin /exports
  1    0 00010580 OfxGetNumberOfPlugins
  2    1 000105A0 OfxGetPlugin
  -> 2 number of functions, 2 number of names
dumpbin /dependents
  KERNEL32, MSVCP140, VCRUNTIME140, VCRUNTIME140_1, api-ms-win-crt-*
cuobjdump -lelf
  MinimalCudaGain.1.sm_75.cubin ... MinimalCudaGain.7.sm_120.cubin
```

That log ends with `Linking CXX shared module
bundle\\MinimalCudaGain.ofx.bundle\\Contents\\Win64\\MinimalCudaGain.ofx` and
prints `-- CUDA architectures: 75;80;86;89;90;100;120 (default list)` — the
evidence behind the platform-directory and architecture-list claims. (Its last
script line still prints the old, false `links cudart64_*.dll` message: that log
predates the F8 wording fix, which is exactly why the import list above, not the
script's sentence, is what the corpus cites.) The `no CUDA runtime import` line is the
reason the scripts report device code and imports separately instead of claiming
a `cudart64_*.dll` dependency; the image contains the string
`cudart_hybrid64.dll`.

The negative-control log
([`t10-guard-negative.log`](evidence/2026-11-22-windows/t10-guard-negative.log))
is the proof that the `Contents\\Win64` assertion actually fires: with the
defect reintroduced, MSBuild writes `Contents\\Linux-x86-64\\Release\\...` and
the build fails with `BUILD FAILED: no .ofx in Contents\\Win64` plus the path it
did produce.

## 4. The kernels on hardware

[`t2-gputest-build.log`](evidence/2026-11-22-windows/t2-gputest-build.log):

```text
device 0: NVIDIA GeForce RTX 3060, compute capability 8.6, 28 SMs, 12287 MiB
runtime : CUDA 13.5 (13050), unified addressing yes, host-mapped yes
[2/6] stream semantics
  200 queued launches on one non-blocking stream, one sync: ok
[3/6] concurrent launches from host threads
  8 threads, 0 failing
[4/6] zero-copy on host-mapped memory
  128x64 on pinned mapped memory, no device allocation: ok
[5/6] both kernels launched directly (bypassing the launcher)
  vec4 (direct)   333x101 exact match
  scalar (direct) 333x101 exact match
28 launcher cases, 0 failure(s)
PASS: hardware matches the CPU reference bit for bit
```

The case list is in report §5.1: packed 64×48, padded pitch, pitch +8
(not 16-aligned, so the scalar path), a base pointer +4 bytes, window offset
inside bounds, negative pitch in both alignments, sizes 1×1, 3×5, 17×1, 1×17,
63×65, 16×16, 32×33, 129×7, 208×117, 0-width and 0-height, and the overrun
assertion that every byte outside the render window and every padding byte stays
untouched.

Throughput at 1920×1080 RGBA32F, 100 iterations, `cudaEvent`, CUDA 13.0:

| Kernel | ms/frame | Effective bandwidth |
|---|---|---|
| vec4 | 0.212 | about 312 GB/s |
| scalar | 0.222 | about 300 GB/s |

Roughly 4 % on a card with about 360 GB/s of peak: both paths are
bandwidth-bound, which is why
[`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md) §6.1
states that the vectorised path is a correctness and instruction-count win, not
the performance lever.

## 5. The host protocol through `LoadLibraryExW`

[`t3c-contract-final.log`](evidence/2026-11-22-windows/t3c-contract-final.log)
ends with `contract_test: 74 checks, 0 failed` and `CONTRACT_EXIT=0`: load,
enumerate, describe, create, isIdentity, set parameter, render, byte-destination
rejection, destroy, unload, twice. The harness of that run predates this tree's
multi-instance and concurrency checks, so 74 is not comparable with the current
counts (see [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §13); what it establishes
is that the Windows loader path and the OFX actions behave the same as on Linux.

## 6. What this run does not prove

- **No host.** Nothing rendered in Resolve, Nuke, SCRATCH or Natron; every
  host-specific claim stays grade B/C in
  [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md).
- **One GPU, one driver, one OS build.** No multi-GPU binding, no MIG, no
  Windows 10, no Quadro/RTX A series.
- **No sanitizer and no valgrind on Windows.** The TSan/LSan/ASan results in
  [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) are Linux-only.
- **Timing is one card.** The GB/s figures are bandwidth arithmetic on a single
  RTX 3060, not a vendor benchmark.
- **The patch is not this tree.** The archive documents what the external run
  needed on 2026-11-22; the current files were changed again afterwards, and the
  fixes here are verified by `[V]` results, not by the archive.

## 7. Where each claim is used

| Claim | Cited in |
|---|---|
| Windows build, bundle directory, generator matrix | [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7, [`README.md`](README.md), [`SKILL.md`](SKILL.md) §12 |
| 2 exports, 7 cubins, no CUDA runtime import | [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §13, [`SKILL.md`](SKILL.md) §6 |
| 28/28 kernel cases, stream ordering, zero-copy | [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §13, [`SKILL.md`](SKILL.md) §12 |
| 0.212 / 0.222 ms at 1080p | [`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md) §6.1 |
| 74-check host contract through `LoadLibraryExW` | [`TESTING_STRATEGY.md`](TESTING_STRATEGY.md) §3 |
| `--strict` checker lookup, `System32` ONNX Runtime shadowing | §9, [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |

## 8. Reproducing it

The archive holds the commands and their output; the harnesses are in the tree.
On a Windows machine with an NVIDIA GPU and the CUDA toolkit:

```bat
.\build.bat Release
testkit\build-contract-test.bat contract_test.exe "%OFX_SOURCE_DIR%\include"
contract_test.exe build\bundle\MinimalCudaGain.ofx.bundle --id com.ofxplugindeveloper.minimalcudagain
nvcc -arch=sm_86 templates\minimal-cuda-ofx\test\gpu-kernel-test.cu -o gpu-kernel-test.exe
gpu-kernel-test.exe
```

The GPU harness prints its device line first; if it reports no CUDA device, the
run is invalid, not a pass.

## 9. The second review: an ONNX upscale plug-in (2026-11-22)

A separate Windows session cloned `main`, copied `templates/onnx-inference-ofx/`
for an upscaling plug-in, and reported nine findings (B1–B9) against a tree that
already contained the first field test's fixes. The report is archived verbatim
as
[`review-onnx-upscale.txt`](evidence/2026-11-22-windows/review-onnx-upscale.txt).
Its environment: Windows 11 10.0.26300, MSVC 19.50 / VS 18 2026, CMake 4.4.4,
ONNX Runtime 1.20.1 from NuGet, OpenFX 1.5.1. It built a bundle, checked its
exports and imports with `dumpbin`, ran the template's numerical pipeline test
and loaded the plug-in through `ctypes`; no host was installed there either.

Two findings were Windows-only failures the earlier run could not have caught,
because neither the ONNX template nor the `--strict` checker existed in its test
set (**[B]**: executed there, not here):

| Finding | What was observed | Where it is fixed |
| --- | --- | --- |
| B1 | `check-ofx-artifact.bat ... --strict` printed `Der Befehl "C:\Program" …` and `FAILED - dumpbin not found` on a plain `cmd` with Visual Studio installed: the `vswhere -find` invocation ends in a quoted argument, so `for /f` lost its outer quotes and cmd ran `C:\Program`. Every `build.bat` that calls the checker with `--strict` then failed after a correct build. | the checker asks vswhere for `-property installationPath` and globs `VC\Tools\MSVC\*` for `dumpbin.exe`; the quoting rule is documented in [`BUILD_PLAYBOOK.md`](BUILD_PLAYBOOK.md) §7 |
| B2 | the template's `ai_pipeline_test.exe` loaded `onnxruntime.dll` **1.17.1 from `System32`** instead of the 1.20.1 beside the `.ofx`, and died with `requested API version [20] is not available` (exit 139). Copying the DLL next to the `.exe` made the suite pass, including the `halo=0` control at 0.263. | the ONNX CMake build copies the runtime beside the test executable as well as into the bundle |
| B3–B9 | uninitialised RGB when the model path is empty, byte-wise UTF-8→UTF-16 widening of the model path, the CUDA scaffold named as the only start point, `install` accepted but ignored by the ONNX `build.bat`, the shape-preserving runner against upscaling, no NuGet recipe, and three cosmetic items | fixed in the commits that follow this document; each is now stated where a reader meets it |

The loader rule behind B2 is worth stating once, because it applies to the
plug-in as much as to the test: an import is resolved from the executable's own
directory first, then `System32`, then `PATH`. A host that loads the `.ofx` with
plain `LoadLibraryW` gets the `System32` copy of any DLL it also imports;
`LoadLibraryExW` with `LOAD_WITH_ALTERED_SEARCH_PATH` gets the one in
`Contents\Win64`, which is what the contract harness does.

---

## 10. The third review: a YOLO object detector (2026-11-22)

A third Windows session built `com.ai.ofx.YoloObjectDetector` — ONNX Runtime
YOLOv8/v11 detection, overlay drawing, byte and float, RGB and RGBA — from the
skill alone, and reported eight defects (F1–F8), four gaps (G1–G4) and one
deprecation (P1). A later pass from the same session added seven findings
(F9–F15), listed below with the first eight. The report is archived as
[`review-yolo-detector.txt`](evidence/2026-11-22-windows/review-yolo-detector.txt).
Its environment: Windows 11, VS 18 Build Tools only (MSVC 14.50.35717, no VS 2022
instance), CMake 4.4.4, Ninja 1.10, ONNX Runtime 1.20.1 from NuGet, OpenFX 1.5.1.
No OFX host and no Apple machine were available there, so the two loader
findings are marked inferred (**[I]**) — they are the first findings in this
corpus that this checkout cannot measure, only read.

| Finding | What it was | Where it is fixed |
| --- | --- | --- |
| F1 | `INSTALL_RPATH "$ORIGIN"` under `if(NOT WIN32)`: dyld treats `$ORIGIN` as a literal directory, so a Mac bundle records an rpath that cannot resolve | `templates/onnx-inference-ofx/CMakeLists.txt` and `templates/minimal-cuda-ofx/CMakeLists.txt` split `APPLE` (`@loader_path`) from other UNIX (`$ORIGIN`) |
| F2 | the ONNX runtime was copied as `libonnxruntime.so.1` in the `else()` branch, which includes `APPLE`, where the install name is `@rpath/libonnxruntime.<ver>.dylib` | the APPLE branch resolves the symlink and keeps the versioned dylib name; `BUILD_PLAYBOOK.md` §7 has the row |
| F3 | `canonicalType()` mapped only four types, and the fake host's get/set handled only double/int/string/bool, so a colour knob first failed declaration (`OfxParamTypeRGB (expected rgb)`) and then failed `render` with `kOfxStatErrUnsupported` and no message **[B]** | `contract_main.cpp` maps `rgb`/`rgba`/`choice`; `fake_host.cpp` stores and returns RGB/RGBA components and treats choice as an int; an unmodelled read or write now names the parameter and type in `messages()`. Re-checked here with a fixture that declares, reads and sets all three: 136 checks, 0 failed **[V]** |
| F5 | every recipe recommended `OFX::Log::print`, which opens its file only under `#ifdef DEBUG` (`ofxsLog.cpp:40-47`) — and CMake's `Debug` configuration does not define `DEBUG`, so the call is silent in every build | `SKILL.md` §4 Phase 8 and §7, `ARCHITECTURE_AND_CUDA_PIPELINE.md` (logging), `REFERENCE_READING_GUIDE.md` and `BEST_PRACTICES_AND_PERFORMANCE.md` §5.7 now say the log is DEBUG-only and name `setPersistentMessage` as the host-visible channel |
| F6 | the Windows bundle linked `onnxruntime.lib` and shipped `onnxruntime.dll`; a host with its own runtime (Resolve 21 ships 1.13) resolved the import by base name, `GetApi(20)` returned null, and `render` access-violated **[B]** | Windows builds compile with `ORT_API_MANUAL_INIT`, do not link the import library, ship `onnxruntime-ofx.dll`, and load that private name from the module directory at first use; `build.bat` asserts the private file and `AI_MODEL_INTEGRATION.md` §4 documents the trap. The Windows branch itself is **[I]** here — no MSVC in this checkout |
| F7 | a `kOfxParamTypeChoice` knob could be declared but not read or set: `paramGetValueAtTime` answered `kOfxStatErrUnsupported`, Support threw, and `render` failed after `describe`/`createInstance` with no message; the declared option list could not be read back either | choice shares the integer path in all four get/set functions and in `setParam`; `FakeHost::paramOptions(name)` returns `kOfxParamPropChoiceOption` in order; `testkit/README.md` documents the type vocabulary |
| F8 | nothing checked where a pixel *landed*, and the fake host derived `kOfxImagePropData`'s origin from the stride's sign, so the top-down-framebuffer layout could not be built and a mirror-blind overlay passed. The plug-in shipped mirrored labels to Resolve **[B]** | `FakeHost` takes the origin (`setImageOrigin`) and the stride sign (`setNegativePitch`) as separate axes; all four layouts are in-buffer and documented; `SKILL.md` rule 3 and §8, `AI_MODEL_INTEGRATION.md` §5, `PATTERN_INDEX.md` state the y-up rule and the position/orientation test. The plug-in-side rebase and its 94-check tile test are **[B]** (reporter's tree); the harness axis is **[V]** here |
| F9 | the detector's class names came from a hardcoded COCO table instead of the model's `names` metadata, so every custom or baked-vocabulary model was mislabelled past the shared prefix **[B]** | `AI_MODEL_INTEGRATION.md` §7 "Detection heads": read `custom_metadata_map["names"]` at session creation, side-car file only as override, tolerate the writer's quoting and reject sparse indices |
| F10 | `--set` compared the frame with `src × atof(value)`, so a class filter or font scale failed although the plug-in was right **[B]** | `--set-status-only` renders and checks the status and the accepted write, skipping the gain comparison; the usage text and `testkit/README.md` state the assumption |
| F11 | a promptable YOLOE export baked the class count at trace time, and CLIP's `</w>` marker was missing before the merges **[B]** | `AI_MODEL_INTEGRATION.md` §7 "Detection heads": the `int(x[-1].shape[1])` patch and the token-id cross-check |
| F12 | an `Ort::Env` in a function-local static was destroyed at `DLL_PROCESS_DETACH`: segfault after a successful unload, flaky **[B]** | `templates/onnx-inference-ofx/src/ModelRunner.cpp` leaks one `new Ort::Env`; `AI_MODEL_INTEGRATION.md` §4 states the rule for env, session and thread-pool objects |
| F13 | the suite setters read scalar arguments as pointers, so the first `setValueAtTime` from a plug-in crashed; push buttons had no click path **[B]** | both setters read `double`/`int` by value; `setParam` now drives `paramSetValueAtTime` instead of writing fields, so every `--set` test exercises the convention; `FakeHost::pressParam` sends `kOfxActionInstanceChanged` |
| F14 | nothing asked whether the licence texts of bundled models, fonts and tokenizer tables travel in the bundle **[B]** | `SKILL.md` §6 and `BUILD_PLAYBOOK.md` §3.11: `POST_BUILD` + `copy_if_different`, warn instead of fail, after the asset copies |
| F15 | a text overlay cannot be judged at 320×240, and "the frame changed" passes for a mirrored or clipped page **[B]** | `TESTING_STRATEGY.md` §5 and §14: layout is a bound, assert where ink may not be, and render at a named `--size` |
| F4 | `SKILL.md` §6 omitted `Linux-aarch64`, which the packaging spec and `BUILD_PLAYBOOK.md` §7 both list | `SKILL.md` §6 names all four arch directories |
| G1 | the Support names `eBitDepthUByte` (there is no `eBitDepthByte`) and the `bool getRegionOfDefinition(..., OfxRectD&)` signature were stated nowhere | `SKILL.md` §5 depth table and the action's real signature |
| G2 | the routing table sent every per-frame model to the ONNX template, whose runner requires `out shape == in shape`; a detection head cannot use it | `SKILL.md` §4 row, `AI_MODEL_INTEGRATION.md` §7 item 6, `PATTERN_INDEX.md` |
| G3 | no guidance for drawing into the frame: destination pitch, bounds vs render window, premultiplied overlay, font | `ARCHITECTURE_AND_CUDA_PIPELINE.md` §13 and a `PATTERN_INDEX.md` row |
| G4 | no detection-model guidance: letterbox mapping, `[1, 4+N, anchors]` vs transposed, per-class NMS, end-to-end exports | `AI_MODEL_INTEGRATION.md` §7, "Detection heads" |
| F17 | the testkit's `Top` origin mirrors the *read-back* relative to the pointer it hands out, so a sub-window assertion under `--image-origin top` reads as an out-of-window write | `testkit/README.md` states what `destination()` means — frame rows from `bounds.y1`, whatever the pointer did — and that a sub-window test has to un-swap the snapshot once; the contract test only renders full frames, which is why it never hit this |
| F20 | `Ort::GetVersionString()`/`GetAvailableProviders()` pull `OrtGetApiBase` into a load-by-path module, the provider-name strings must not be freed through `Ort::AllocatorWithDefaultOptions`, a label written under the render lock deadlocks the first render, and a read-only field is a `StringParam` with `eStringTypeLabel` | `AI_MODEL_INTEGRATION.md` §4 (the half header-only trap, and `dumpbin /dependents` must show no `onnxruntime.dll`) and §7 (write the label outside the lock; `eStringTypeLabel` + `setEvaluateOnChange(false)`, which is also what `--label-param` can read) |
| F21 | `BEST_PRACTICES_AND_PERFORMANCE.md` §5.3 described a silent `process()` return that the template stopped doing | §5.3 now says `render()` throws `kOfxStatErrValue` for a window outside the source **or** destination bounds, and keeps the log-once advice for effects that still return quietly |
| F22 | the contract harness does not compile with MSVC: the lambda named `near` in the destination-layout check is the `near` macro from `minwindef.h` (C2513) | covered: renamed to `nearPx`, with the reason in a comment. The harness builds and the control suite passes with the new name **[V]**; the MSVC build itself is **unverified** — no Windows machine here **[I]** |
| F23 | the Windows CUDA provider bundle was refused at configure time on the premise that the provider imports `onnxruntime.dll` | **wrong premise, corrected**: `dumpbin /dependents` on ORT 1.20.1 lists cublas, cublasLt, cufft, cudart, cuDNN and `onnxruntime_providers_shared` only, so the private core name is compatible, and a 30-file bundle ran on a RTX 3060 **[B]**. The `FATAL_ERROR` is gone, `AI_MODEL_INTEGRATION.md` §7 and `SKILL.md` §6 state the measured condition and the per-version `dumpbin` check (§4 already corrected for `GetAvailableProviders`). Configure verified on Linux with stand-in files: `CUDA provider bundle: 7 file(s)` **[V]**; the Windows copy path itself is unverified **[I]** |
| F24 | `--roi-context`'s negative half demands that the cropped window differ, which a global effect that declares no context can never satisfy — a false FAIL with a message that blames the plug-in | covered: the harness asks `GetRegionsOfInterest` for the inset window and, when the answer is the request itself, skips that half with `roi context: second half skipped - the plug-in declares no context region` in the check stream. `ReplyDefault` is still checked; the new `-DROI_SELF` stub proves both the pass and the named skip (control 15) **[V]** |
| F25 | a Windows GPU bundle copied only the cuDNN dispatcher and loaded only the core, and the golden comparison had no bound for a GPU render | **partly covered**: `CMakeLists.txt` globs `cudnn*64_*.dll` (every component, list-copied), `ModelRunner` preloads every non-`onnxruntime*` DLL beside the module by absolute path on Windows, and the harness has `--golden-tolerance T` (default 0), which the ONNX suite uses at `1e-5` only after an exact comparison failed **[V]** for the flag and the Linux configure; the preload and the component copy are **unverified** Windows code **[I]** |
| H3-1 | the handover said `AI_MODEL_INTEGRATION.md` claims `GetAvailableProviders()` lives in the import library | confirmed and corrected: in ORT 1.20.1 only `GetVersionString()` calls `OrtGetApiBase()`; `GetAvailableProviders()` is inline over `GetApi()`, the table `InitApi()` installs. §4 now says so, with the header evidence and the instruction to read the inline header of the shipped version **[V]** |
| H3-2.2 | no policy for retrying a failed tile or render, or for deciding when a retry is worth it | open: the shape is a pure decision function (`DecideTileRetry(failure, attempt, budget)`) tested without a GPU, and a harness flag that makes the first N renders fail; neither exists yet |
| H3-2.3 | "UI lines are identical across build variants" is asserted without a measurement | open: the cheap proof is two tile sizes rendered through `--set tileSize=` and compared with `--golden-tolerance`, which the harness can do now (F25); naming it a check rather than a demo is still to do |
| H3-2.4 | frame time is quoted from arithmetic instead of measurement | covered as guidance: §7 measures frame time at shipped sizes, and F25's `bench_ep`/`sweep-vram` note that a Windows ramp may never fail. No benchmark script for a 1080p frame exists |
| H3-2.5 | CUDA OOM is often not reproducible on Windows, so "it fits" cannot be proven by a ramp | covered: §7 says a ramp to 39 GB of process RSS without an allocation failure is WDDM paging, that "never fails" means "no ceiling found", and that measured frame time is the number to ship **[B]** |
| H3-2.6 | `halo.json` is written and checked by the tool, but nothing in the template reads it | covered as scope: §7 states the file is a build-time input for the `halo` parameter and `--check`, never a render-time ledger. A reader in the effect would be a per-frame file read |
| H3-3.6 | the number of checks depends on the arguments, so a count from one handover is not comparable with another's | covered: `testkit/README.md` says the check count follows the flags a run passes, and the numbers in `TESTING_STRATEGY.md` name the run they belong to |

| H2-2.1 | runtime state with a destructor is torn down at unload (their `Ort::Env` and session cache in function-local statics); the contract test hung after `render done` | covered: `SKILL.md` §1 rule 13; `testkit/README.md` says the second load/unload cycle is what catches it; `[stage] unload` on stderr names the phase (`47aca13`) **[V]** |
| H2-2.2 | no guidance for an effect that changes the output size (RoD growth) | covered as the three call sites: `AI_MODEL_INTEGRATION.md` §7 item 5 — return the output shape from `Run()`, map it in `AiCore`, scale the source RoD in `getRegionOfDefinition` — plus `ARCHITECTURE_AND_CUDA_PIPELINE.md` §7. The template itself stays a 1x filter (H2-T2) |
| H2-2.3 | depths and premultiplication appear only as a property table, with no instruction | covered: `AI_MODEL_INTEGRATION.md` §7 "What the host hands you is not float RGBA" — 255/65535 normalisation, divide by alpha before the model and multiply back, conversion OFX-free — and `ARCHITECTURE_AND_CUDA_PIPELINE.md` §8 per operation |
| H2-2.4 | tile overlap is a parameter with no way to measure it | covered: §7 "The halo is measured, not guessed" (ladder, 0.02 bound, a real tile edge required, frame border excluded, `2·overlap < frame height`) and `tools/measure-halo.sh`, which writes `<model>.halo.json` keyed by name, byte count and SHA-256 |
| H2-2.5 | models that need multiples of 8/16/32, or a fixed input size | covered: §7 "Models that only accept certain sizes" — alignment by probing 63/72/80/96, edge replication rather than zeros, fixed size costs quality and belongs in the UI. Not implemented in the template (H2-T3) |
| H2-2.6 | the CUDA EP as the standard case for an inference plug-in, not an appendix | covered: `AI_MODEL_INTEGRATION.md` §6/§7 request CUDA first and fall back to the CPU, one bundle instead of a GPU build, the provider visible in a label that `--label-param` asserts **[V]**, and the Windows bundle now enabled with a per-version `dumpbin /dependents` check (F23). The timing ratios stay **[B]** — nothing here measures a GPU |
| H2-T1 | the template accepts float only, which is the rare case in Resolve | deliberate scope, now stated in the `templates/onnx-inference-ofx/src/Plugin.cpp` header (H2-T5); the rules for taking other depths are §7's "What the host hands you is not float RGBA" |
| H2-T2 | the template has no `getRegionOfDefinition`, so an upscaler cannot be built from it | open in code: an upscaler needs its own runner, and §7 item 5 names the three places. The header's scope paragraph points there |
| H2-T3 | `ModelRunner` has no alignment, padding or resize to a fixed input size | open in code, by design: a shape-changing model is refused (`output shape == input shape`), and §7 states the rule for the plug-in that supports one |
| H2-T4 | `AiCore` has no premultiply handling | open in code: the template declares float RGBA and copies alpha through untouched; the divide/multiply rule is §7's depth-and-alpha subsection |
| H2-T5 | "ONNX template" reads like it covers upscalers and detection heads | covered: a scope paragraph in the `Plugin.cpp` header — 1x filter on float RGBA, shape-preserving model, pointers to §7 items 5 and 6 (`9b8cfd5`) |
| H2-T6 | §7's "no GPU execution provider is compiled in" was honest but had no consequence | covered: §6 and §7 now say when to add it (model computes per frame, host already on the GPU) and that it goes into the same bundle |
| H2-T-bat | their `build.bat` clone had `-G "Visual Studio 17 2022"` hard-wired and broke on VS 18 | covered: both `build.bat` files resolve 18/17 through `vswhere`; nothing to change |
| H2-4.1 | the fake host had no `getRegionsOfInterest`, so the RoI path was untested | covered: `--roi-expand` and `--roi-context` record the answer and check it against the declared halo, with `[stage] roi region` / `roi context` naming the phase; their clone predates both |
| H2-4.2 | no progress output, so a hang was not attributable to a phase | covered: `[stage] render`, `mark`, `compare golden`, `compare parameter`, `concurrency`, `byte destination`, `roi region`, `roi context`, `unload`, all on stderr (`9b8cfd5`, `47aca13`) **[V]** |
| H2-4.3 | `--concurrency-threads` defaults to 4×3, which is minutes of silence for a slow model | covered as advice: `testkit/README.md` says to pass `--concurrency-threads 0` for a model slower than ~1 s/frame. A `--time-budget` flag is **open** |
| H2-4.4 | the fake host cannot set a string property on a clip, so a plug-in that must notice new media behind the same clip handle (cache invalidation) is untestable | covered: `FakeHost::setClipProperty()` / `clipProperty()`, `--clip-prop NAME=VALUE`, and `--clip-prop-change NAME=VALUE`, which re-renders on the same handle and asserts through the plug-in's label that it re-read the value. The control pair proves both directions: the re-reading build passes, the one that caches the first answer fails with `= "take1" (expected "take2")` **[V]** |
| H2-§7 | ship `model_probe`, `bench_ep`, `make-testmodels.py`, `sweep-vram.sh` and `test.bat` as source, not prose | **mostly covered**: `tools/make-testmodels.py` writes 1/3/4-channel 1×1-`AveragePool` identities and a nearest-`Resize` 2× model and self-tests them through ONNX Runtime's CPU provider; `tools/model_probe.py` reports shapes, dynamic H/W, channels, the output/input ratio and the 63/72/80/96 alignment ladder (verified against a model that needs a multiple of 64: `63 fails, 72 ok` → `multiple of 8`); `tools/bench_ep.py` times each available provider and prints the provider the session really used. All three run without a model download and without a GPU **[V]**. `tools/sweep-vram.sh` exists and its no-GPU path is verified — no `nvidia-smi` exits 3 with the reason, a CPU ramp prints the table and stops at the first failure with its message **[V]** — but the OOM ceiling itself is **unverified**: no GPU here. `templates/onnx-inference-ofx/test/test.bat` now exists as the Windows counterpart of `run-tests.sh` — written, never executed **[I]**, because no Windows machine exists here. Nothing in this row is open any more; `test.bat` is unverified code, not missing code |
| P1 | `FetchContent_Populate()` with `CMP0169 OLD` in all three templates | all three use `FetchContent_MakeAvailable()` with `SOURCE_SUBDIR cmake/do-not-build`; verified here by a real network clone and `./run-tests.sh` **[V]**, and again on Linux with CMake 4.4.5 — the ≥ 3.30 case P1 names — where a fresh configure from the network clone printed no CMP0169 warning and the suite passed 160 + 82 **[V]** |

The `H2-` rows are the *second* handover (ONNX Loader, 22.–23.11.2026, German):
its `§2.x` are skill-text gaps, `T*` template items and `§4` testkit items. The
prefix keeps them apart from the F-numbers above, which are a different report on
a different plug-in. What stays open is deliberate: the template is a 1x float
filter, so upscaling, alignment/padding and premultiplied input need a runner of
their own even though the rules are written down, and the tool suite the second
handover asks to ship is built here: `make-testmodels.py`, `model_probe`,
`bench_ep`, `sweep-vram.sh` and `test.bat` all exist. The first three run without a
GPU; the sweep's own GPU ceiling is the one part no machine here can prove, and
`test.bat` has never been executed because there is no Windows machine — both are
named as unverified rather than as working.

What that run confirms, and this checkout could not have produced:

- **The NuGet fallback is load-bearing.** `github.com/microsoft/onnxruntime/
  releases/download/...` returned HTTP 404 from that network for every version
  tried, while
  `https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/1.20.1`
  returned 200 (108 MB) and its layout mapped onto `<root>/include|lib|bin`
  **[B]**. The paragraph in `AI_MODEL_INTEGRATION.md` §4 is why the build
  existed at all.
- **Exactly two exports on MSVC** **[B]**: `dumpbin /exports` on the built
  `.ofx` lists `OfxGetNumberOfPlugins` and `OfxGetPlugin` and nothing else, with
  no `/EXPORT` flag and only `CXX_VISIBILITY_PRESET hidden` (a documented no-op
  there) — the claim in `SKILL.md` §6 holds.
- **`testkit` is reusable by a very different architecture.** Unmodified, it
  drove a stateful detector behind a mutex with byte and float, RGB and RGBA and
  no image maths, through `describe` → `describeInContext` → `createInstance` →
  `isIdentity` → `render` → `destroyInstance` → `unload`, twice, with two live
  instances, four concurrent render threads and a negative-pitch destination:
  **110 checks, 0 failed** **[B]**. That is the third independent architecture
  the harness has accepted, and none of the three needed a harness change.
- **`BUILD_PLAYBOOK.md` §7 is right for a VS 18-only machine** **[B]**: the
  `Visual Studio 17 2022` generator could not configure, and Ninja after
  `vcvars64.bat` worked first try. The `vswhere.exe` line that `vcvars64.bat`
  prints on that host is noise, not a skill defect.
- **The licence gap is the quietest one in the list** **[B]**: F14 did not break
  a build or a render. The notice file sat in the repository root while the
  installed bundle carried 385 MB of third-party weights, so nothing in the
  plug-in's own tests could notice it. That is why the copy step is a build step
  and not a checklist item.

The architectural note worth carrying forward is the same one F2 makes on
Windows and macOS alike: the loader's search is a **name** contract (SONAME,
install name, DLL file name), and a copy step that invents a name — or an rpath
token from another platform's loader — builds cleanly and fails only at load. F1
and F2 are one bug seen twice, which is why both loader-bearing templates were
changed, not just the one that was reported.
