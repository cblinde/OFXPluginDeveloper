# Verification status

What is verified, by what command, and what remains unverified - the honest bottom line.

<!-- SKILL.md lines 744-819 -->
<!-- README.md lines 349-438 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 27-46 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 510-531 -->
## Verification status of this skill

**Verified on the authoring machine** **[V]** (Linux x86-64, Ubuntu 24.04,
GCC 13.3.0, CUDA 12.9.41 via `./build.sh --bootstrap-cuda`, CMake 3.31.6,
OpenFX `OFX_Release_1.5.1`):

- CUDA and `CPU_ONLY=1` builds both produce
  `Contents/Linux-x86-64/MinimalCudaGain.ofx` with no compiler warnings; the CUDA
  build links `libcudart.so.12`, and `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` drops it
  from `NEEDED`.
- The artifact exports the two entry points plus five weak `W` helpers (7 dynamic
  symbols without a version script), and contains the launcher, both kernels and a
  `.nv_fatbin` section.
- Kernel PTX (`-arch=sm_89`): one 16-byte load and one 16-byte store per pixel,
  14 registers, 0 spills, 0 stack. Both kernels ran over 3,240 **[V]** emulated
  grid launches with 0 mismatches; the hoisted CPU loop is bit-identical over
  1,464 pixel components **[V]**.
- `dlopen` returns one plug-in: `OfxImageEffectPluginAPI` v1,
  `com.ofxplugindeveloper.minimalcudagain` 1.0, `setHost` and `mainEntry` set.
- `./test/run-tests.sh`, ASan+UBSan, TSan and `RUN_VALGRIND=1` are clean;
  `test/run-analysis.sh` (cppcheck + shellcheck) is clean.
- Every `kOfx*` constant cited in `ofx-api-constants.md` exists in the pinned headers, including the
  three 1.5.1 additions and `ofxColour.h`.

**Currency check, 2026-11-24** (external evidence, re-read before relying on it):

| Fact | Source | Label |
| --- | --- | --- |
| OFX 1.5.1 (2025-11-20) is still the current release; the docs build is dated 2026-10 | `AcademySoftwareFoundation/openfx` releases, `openfx.readthedocs.io` | [B] |
| Metal texture render and key modifiers are open proposals, not in 1.5.1 | `openfx` issues #283, #292 (Oct–Nov 2026) | [B] |
| CUDA 13.5 is the current toolkit; 13.x needs driver ≥ 580 and cannot compile below `sm_75` | CUDA Toolkit Release Notes 13.5, CUDA Compatibility | [B] |
| ONNX Runtime 1.32.0 (2026-11-13); CUDA packages target CUDA 13.0 + cuDNN 9 since 1.27 | ORT release notes, CUDA EP requirements table | [B] |
| DaVinci Resolve Studio 21.2 (2026-11-05) is current; 21.0.3 fixed "GPU support in OpenFX plugins" | Blackmagic forum release posts, 21.0.3 notes | [B] |
| Resolve 21 implements the OpenFX 1.5 colour-management APIs | DaVinci Resolve 21 New Features Guide | [B] |

**Still not verified (state this plainly in any downstream report):**

- **`./run-tests.sh` executes no SASS** — by design, it has no GPU step; its
  numerics are validated by emulating the thread mapping over the real kernel
  source (`performance-model.md` §3). The hardware run is
  `templates/minimal-cuda-ofx/test/gpu-kernel-test.cu`: on 2026-11-24 it was
  28/28 bit-exact and measured throughput, the CPU loop and the copy cost
  (`../README.md` §"What has actually been verified"). Multi-GPU binding and real
  async ordering across many streams remain unverified.
- **Two hosts, one frame each, both headless.** `NatronRenderer` rendered one
  256×256 frame through the template on 2026-11-24
  (`testkit/verify-bundle.bat`), pixels checked. DaVinci Resolve Studio 21.2.0.5
  ran `tests/resolve/poc.py --build-project` on 2026-11-25 and produced one
  1920×1080 PNG; its pixels are identical to the source because the plug-in is
  identity at the default gain, so it verifies the host path (scan, list, apply,
  render, decode) and not the kernel. Nuke and SCRATCH have run nothing, and
  every host-specific claim in `references/host-resolve.md` remains graded B/C.
- **The 1.5.1 render hints are documented, not exercised.** No host here sets
  `kOfxImageEffectPropNoSpatialAwareness` or `kOfxImageEffectPropThumbnailRender`,
  and the templates do not yet read them; the rules in `roi-tiling-and-render-hints.md` follow from the header
  text **[I]**.
- **No colour-managed test.** The colour-management paragraph in `ofx-api-constants.md` is read from
  `ofxColour.h` and Blackmagic's release note; no host and no config were
  exercised **[I]** for the plug-in side.

**Executed elsewhere, not here [B]** (2026-11-22 field test: Windows 11, RTX 3060
sm_86, CUDA 13.0 toolkit on a 13.5 driver, MSVC 19.44/19.50; logs and limits in
[`field-evidence.md`](field-evidence.md)):

- `.\build.bat` configures and builds; the result is
  `Contents\Win64\<Name>.ofx` with exactly two exports (`dumpbin /exports`) and
  seven cubins (`cuobjdump -lelf`). The contract harness, ported to
  `LoadLibraryExW`, drove the whole host protocol with 0 failures.
- The real kernels executed on the GPU: 28/28 bit-exact versus the host reference,
  including padded and negative pitches, misaligned bases, window offsets,
  zero-size windows and overrun guards.
- First throughput measurement: 1920×1080 RGBA32F, vec4 0.212 ms (~312 GB/s),
  scalar 0.222 ms (~300 GB/s) — bandwidth-bound, ~4% apart, which is why the PTX
  instruction count must not be read as a speed claim.

**Still not verified there:** no OFX host, so no pixel has been rendered by
Resolve, Nuke, SCRATCH or Natron; one GPU only; no Windows sanitizer or valgrind
pass; no Nsight capture; no multi-resolution performance work.

## 1. What has actually been verified

Built and checked on Linux x86-64 with GCC 13.3.0, CUDA 12.9.41, CMake 3.31.6 and
OpenFX `OFX_Release_1.5.1`. Lines marked **[V]** are measurements or artifact
inspections from that machine; the commands and the full results are in
`testing-layers.md` §13.

- CUDA-enabled configure + build compiles `Kernel.cu` with real `nvcc` for
  `75;80;86;89;90;100;120`, links `libcudart.so.12`, and produces
  `MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx`.
- That `.ofx` exports exactly `OfxGetNumberOfPlugins` and `OfxGetPlugin`, contains
  the launcher symbol, both kernels (`GainKernelRGBA32FVec4`,
  `GainKernelRGBA32FScalar`) and a `.nv_fatbin` section, and passes
  `dlopen` → `OfxGetNumberOfPlugins() == 1` with the expected identifier.
- `templates/minimal-ofx` builds from a tree with no OFX headers and no build
  directory with one command, and its artifact has exactly **two** defined dynamic
  symbols (a version script suppresses the five weak `std::` helpers that
  `-fvisibility=hidden` alone leaves); `dlopen` reports
  `OfxImageEffectPluginAPI` v1, `com.ofxplugindeveloper.referencegain` 1.0, with
  `setHost` and `mainEntry` set. No host was involved — see `debugging-and-symptoms.md` §5.
- The vectorized kernel emits one 16-byte load and one 16-byte store per pixel
  at PTX level (`-arch=sm_89`), with 14 registers, no spills and no stack, the
  same register count as the scalar fallback.
- Both kernels produce byte-identical output to a scalar reference across 3,240 **[V]**
  host-emulated grid launches covering widths 0–63, all four `width % 4` classes,
  odd origins, padded pitches and 4-byte-misaligned bases, and neither writes
  outside the render window.
- `./test/run-tests.sh` passes end to end (predicate table, 3,240 launches **[V]**, CPU-loop
  comparison, six concurrent kernel runs, PTX instruction counts, bundle hygiene).
  Those checks are committed, not just run once.
- The same harness is clean under AddressSanitizer + UndefinedBehaviorSanitizer,
  under ThreadSanitizer (which is given real threads by the concurrent test), and
  under valgrind memcheck (`0 errors from 0 contexts`, every heap block freed).
- `test/run-analysis.sh` reports nothing unresolved from cppcheck or shellcheck;
  the two intentional cppcheck suppressions are the byte↔float accessors that
  reproduce OFX's `void*` + row-bytes image contract, and are documented in the
  checklist.
- `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` builds and links: `libcudart.so.12` disappears
  from `NEEDED` and there are zero undefined `cuda*` symbols.
- `CPU_ONLY=1`, and "CUDA requested but no compiler", both still build and pass
  the same registration check.
- The no-root bootstrap was exercised from an empty cache directory.

A 2026-11-22 field test on Windows 11 (RTX 3060, sm_86; CUDA 13.0 toolkit on a
13.5 driver; MSVC 19.44 and 19.50; CMake 4.4.4) executed what this machine could
not. Those results are **[B]** — external evidence, not reproduced in this
checkout; the measurements themselves are quoted with their logs in
[`field-evidence.md`](field-evidence.md).

- `.\build.bat` now configures and builds: generator from `vswhere`, canonical
  CUDA paths, Ninja fallback when the installed Visual Studio has no NVIDIA CUDA
  build customisations, and the cached generator reused on a second run.
- The Windows `.ofx` lands in `Contents\Win64`, exports exactly the two entry
  names (`dumpbin /exports`), and carries seven cubins, `sm_75`…`sm_120`
  (`cuobjdump -lelf`). `CPU_ONLY=1` builds through the Visual Studio generator
  without a `Release` subdirectory.
- The shared contract harness, ported to `LoadLibraryExW`, drives the Windows
  bundle through the whole host protocol with 0 failures (that run predates the
  multi-instance checks this checkout adds, so its check count is not comparable
  with the ones in `testing-layers.md` §13).
- `test/gpu-kernel-test.cu` ran the real kernels on the RTX 3060: **28/28 cases
  bit-exact** against the host reference, covering padded and negative pitches,
  4-byte-misaligned bases, render-window offsets, 0-width/0-height, overrun
  guards, 200 queued launches, eight host threads and zero-copy.
- 1920×1080 RGBA32F, 100 iterations: vec4 **0.212 ms** (~312 GB/s), scalar
  **0.222 ms** (~300 GB/s) — both bandwidth-bound, a ~4% difference.
- The same harness under `compute-sanitizer --tool memcheck`: **0 errors**, and
  28/28 still bit-exact with the tool instrumenting every access.

The harness now also measures the GPU path against the CPU loop it replaces and
against the copy a zero-copy path avoids, in one run on the same RTX 3060:

| Size | vec4 kernel | scalar kernel | CPU loop | H2D+D2H copy (pageable) |
|---|---|---|---|---|
| 1080p | 0.209 ms (317 GB/s) | 0.217 ms (306 GB/s) | 3.456 ms | 6.705 ms (9.9 GB/s) |
| 4K | 0.850 ms (312 GB/s) | 0.846 ms (314 GB/s) | 13.480 ms | 27.332 ms (9.7 GB/s) |
| 8K | 3.479 ms (305 GB/s) | 3.363 ms (316 GB/s) | 55.287 ms | 107.396 ms (9.9 GB/s) |

Three things this settles that were previously argued from PTX and arithmetic:
both kernel forms are **bandwidth-bound**, so the vec4 form's 4× instruction
reduction buys no throughput on this GPU; the kernel is ~16× the CPU loop; and a
pageable copy of one frame costs **32× the 1080p kernel** — the copy, not the
maths, is what the zero-copy contract exists to avoid. Mean of 100/30/10
iterations; one GPU, one driver, one OS build.

**Not verified here:** Nuke and SCRATCH have run nothing, and Resolve has rendered
a frame but never the pixel path (see above); the inference template's CPU session
has a Windows run behind it
([`onnx-inference-suite.log`](../evidence/2026-11-24-windows/onnx-inference-suite.log))
and so does its CUDA session, with the provider bundled from the GPU package and
cuDNN 9/cuBLAS/cuFFT/cudart taken from wheels, label
`CUDA (device 0, 1 thread, bundled CUDA)`
([`onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log)),
and again on ONNX Runtime 1.32.0, whose GPU package is built against CUDA 13 and
ships the provider as an EP plugin
([`onnx-inference-cuda-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log));
the label now reads `…, tf32 off, all nodes on CUDA`: the CUDA EP is appended
through the key/value API with `use_tf32=0`, `cudnn_conv_algo_search=HEURISTIC`
and `cudnn_conv_use_max_workspace=0` stated rather than inherited, and a
throwaway session built with `session.disable_cpu_ep_fallback=1` loaded the graph
without leaving a node on the CPU EP - a control without any provider shows that
key failing rather than being ignored
([`onnx-fallback-probe.log`](../evidence/2026-11-25-windows/onnx-fallback-probe.log)),
while the TF32 default was measured to move a 1080p convolution by `4.5e-4`
against `5.4e-7` with it off
([`onnx-cuda-ep-options.log`](../evidence/2026-11-25-windows/onnx-cuda-ep-options.log));
no host has been given device memory by any of these runs; Linux has not been run; and no kernel runs in
`./run-tests.sh`, which has no GPU step by design. See the opening section of this document.

## 2. What this pass changed, and what it verified

The review ran a mechanical check of the repo against the official sources.

| Check | Result |
| --- | --- |
| Every `kOfx*` identifier cited in the documents, sources and tests exists in OFX 1.5.1 | Checked mechanically by extracting every `kOfx*` token and resolving it against the 1.5.1 tree. Of the 2 that did not resolve, **1 was a real error, now fixed** — `references/ofx-lifecycle-and-threads.md` cited `kOfxImageEffectActionInstanceChanged`; the real action is `kOfxActionInstanceChanged` (`ofxCore.h:447`). The other, `kOfxImageEffectPropUnmappedComponents` in `../SKILL.md`, is a deliberate negative example ("there is no such constant"), so it stays. |
| The GPU property table in `ofx-api-constants.md` §1 against the headers | 12/12 exist; three descriptions were wrong and are fixed (wrong header for `...OpenCLRenderSupported`, OpenGL declared `C string` not `string`, and the OpenCL Buffers/Images distinction now quoted from the header) |
| A committed GPU-free test suite (`templates/minimal-cuda-ofx/test/run-tests.sh`) | predicate table + 3,240-launch kernel comparison + CPU-loop comparison + 6-thread concurrency + PTX counts + memcheck, exit-code gated; clean under ASan+UBSan, TSan and valgrind **[M]** |
| Every CUDA runtime symbol the template calls (9) is declared in the toolkit headers | all present |
| The quoted stream-contract text exists verbatim in `ofxGPURender.h` | present at lines 608–617 |
| Property types and defaults used by the template | confirmed, see §3 |
| `ImageProcessor` dispatches the GPU path once per render, not per thread | confirmed, see `ofx-api-constants.md` |
| The CPU path is already threaded by the Support library | confirmed, see `ofx-api-constants.md` |
| Kernel memory instructions, after vectorizing | **1 `ld.global.nc.v4.f32` + 1 `st.global.v4.f32`** per pixel, 14 registers, 0 spills, 0 stack — measured, see `field-evidence.md` §4 |
| Vec4 path versus scalar path, executing the real kernel source on the host | **3,240 grid launches, 0 mismatches**: widths 0–63 covering every `width % 4`, heights 1/2/3/5, origins (0,0)/(5,3)/(17,9), aligned and padded and misaligned pitches — see `field-evidence.md` §4 **[M]** |
| CPU path: hoisted row pointers versus the previous per-pixel addressing | bit-identical over 1,464 pixel components — see `ofx-api-constants.md` **[M]** |

---

## 3. What is still unverified

- **One kernel measured, nothing else.** Throughput, the CPU loop it replaces and
  the copy cost are measured on one RTX 3060 (`../README.md` §"What has actually been
  verified"). The 4× instruction reduction stays PTX-level **[M]** — and measured,
  it produced no throughput difference, because both forms are bandwidth-bound. The
  instruction claim is about instructions, not speed.
- **The equivalence proof stopped at the host.** The two kernels and the two CPU
  loop forms were shown to agree byte-for-byte by executing the real kernel
  source on the CPU (`field-evidence.md` §4), which is not the same as running SASS on a GPU. The
  same checks are committed as `templates/minimal-cuda-ofx/test/run-tests.sh` so
  they can be re-run after any change.
- **Two headless hosts, one frame each.** `NatronRenderer` loaded the bundle and
  rendered a 256×256 frame through `testkit/verify-bundle.bat` on 2026-11-24, and
  Resolve Studio 21.2 did the same through `tests/resolve/poc.py` on 2026-11-25
  (**[V]** for both). Neither is a GUI session, and Resolve's frame is a
  pass-through, so its `render()` was never required to run; Nuke and SCRATCH have
  run nothing, and every host-behaviour claim in this document is graded **[R]** or
  **[S]**, never **[M]**.
- **Resolve's scanline padding** (256-byte alignment, the 208×117 thumbnail case
  in `Kernel.cu`'s comment) is a community observation carried in the template's
  source, not something measured against Resolve here. It is safe to depend on
  the *general* rule (pitch is `rowBytes`, not `width × 16`) and unwise to depend
  on the specific number.
- `cudaPointerGetAttributes` cost per launch is unmeasured here. It is cheap on
  UVA systems; if a profile ever shows it, cache the device per allocation.

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[M]** measured with the real toolchain, static/PTX or on the GPU
- **[R]** stated by a reference project
- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
