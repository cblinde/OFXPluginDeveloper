# Field test: Windows 11, CUDA 13.0, RTX 3060 (2026-11-22)

Every **[B]** claim in this corpus comes from one external run. This document is
the evidence for it: what was executed, what the logs say, and what the run does
not prove. Nothing on this page ran in this checkout. The raw material is
archived verbatim under [`evidence/2026-11-22-windows/`](evidence/2026-11-22-windows/):
the handover report (`report.txt`) and the ten logs it cites.

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
