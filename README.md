# OFXPluginDeveloper

A complete engineering reference and starting template for **CUDA-accelerated
OpenFX (OFX) plugins** — the effect plugin format used by DaVinci Resolve Studio,
Nuke, Assimilate SCRATCH and Natron.

## Purpose

OpenFX is easy to start and hard to get right. The API is small, but the parts
that decide whether a plugin renders correctly — where the pixels actually live,
what the scanline stride is, whether you may block the host's GPU queue, whether
the host even enabled the GPU path for this frame — are spread across a
specification, four host applications, and a handful of scattered open-source
plugins. Getting one of them wrong produces a black frame or a host crash with no
stack trace, in a process you cannot attach a debugger to easily.

This repository exists so that work is done once:

- **`SKILL.md`** encodes the rules and a repeatable generation workflow, so a new
  plugin starts from a known-good architecture instead of a blank file.
- **`templates/minimal-cuda-ofx/`** is a complete, compiling plugin with a
  zero-copy CUDA path *and* a CPU fallback, correct bundle packaging, and a build
  that proves what it produced.
- **The reference documents** cover the parts you only learn from a bug
  report: the GPU/stream contract, pitch arithmetic, RoD/RoI, colour and alpha,
  and per-host differences graded by evidence quality.

**Who it is for:** an engineer (or an AI agent) about to write, port, review or
debug a GPU-accelerated OFX plugin, on Linux or Windows, primarily targeting
DaVinci Resolve Studio.

**What it is not:** not a plugin *collection*, not a wrapper library, and not a
substitute for testing in a real host. One path has been rendered by one host —
`NatronRenderer`, headless — and nothing has been rendered by Resolve, Nuke or
SCRATCH. See [What has actually been verified](#what-has-actually-been-verified).

| File | What it is |
| --- | --- |

## Where everything lives

| Path | What it is |
| --- | --- |
| `SKILL.md` | The entry point: ground rules, the build workflow, and the routing table |
| `references/` | 21 single-subject documents - constants, lifecycle, streams, pixels, RoI, colour, host quirks, CMake, packaging, debugging, performance, ONNX, AI architectures, pattern index, testing, verification status, field evidence, reference projects, checklists |
| `templates/` | Three complete plug-ins: `minimal-ofx` (CPU), `minimal-cuda-ofx` (CUDA + fallback), `onnx-inference-ofx` (model per frame) |
| `testkit/` | The host harness: `dlopen` + contract checks, the official OFX host, and the headless Natron render |
| `tools/` | `check-docs.py` (documentation drift), `coverage-check.py` (restructure proof), `make-testmodels.py`, `model_probe.py`, `bench_ep.py`, `measure-halo.sh`, `sweep-vram.sh`, `host_cuda_probe.cpp` |
| `scripts/` | `check-artifact.sh` - the POSIX twin of the testkit artifact check (exports, dependents, device code) |
| `evidence/` | Raw logs of every run the documents cite |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | BSD-3-Clause, and what the fetched dependencies oblige |

### What is inside each template

`templates/` carries no README of its own - this is the map.

```text
templates/minimal-ofx/
  CMakeLists.txt                     reference build: module, support sources, export list
  src/Plugin.cpp                     the smallest host-loadable plug-in
  exports.map                        version script: exactly two dynamic symbols
  build.sh / build.bat               fetch headers, build, inspect the artifact
  test/run-tests.sh                  contract harness against the reference plug-in
templates/minimal-cuda-ofx/
  CMakeLists.txt                     bundle-producing build, CUDA optional
  src/Plugin.h  src/Plugin.cpp       OFX host wrapper: params, dispatch, isIdentity
  src/Kernel.cuh src/Kernel.cu       CUDA interface + vec4/scalar kernels + launcher
  test/run-tests.sh                  GPU-free suite: equivalence, concurrency, PTX, bundle, memcheck
  test/run-analysis.sh               cppcheck + shellcheck over sources and scripts
  test/kernel_equivalence.cpp        host harness for kernels and CPU loop
  test/shim/                         minimal <cuda_runtime.h> for host builds
  test/gpu-kernel-test.cu            hardware harness: 28 cases, streams, zero-copy, throughput
  build.sh  build.bat                toolkit discovery, bootstrap, build + artifact check
templates/onnx-inference-ofx/
  CMakeLists.txt                     bundle build, ONNX Runtime detection + bundling
  src/Plugin.h  src/Plugin.cpp       OFX wrapper: window addressing, alpha, render
  src/ModelRunner.h  src/ModelRunner.cpp   one ORT session behind a pimpl
  src/AiCore.h  src/AiCore.cpp       packing, tiling, whole/tiled inference
  models/                            committed 611-byte model + golden tensor
  tools/make-test-model.py           regenerates model and golden
  test/run-tests.sh                  packing, golden, tiling, negative control, bundle
  test/ai_pipeline_test.cpp          the harness
  build.sh  build.bat                configure + build + report (Windows needs
                                     ONNXRUNTIME_ROOT; the DLL is bundled)
  bundle output                      Contents/<ARCH>/<Name>.ofx (Win64 on Windows,
                                     Linux-x86-64 on Linux), Resources/ with icons,
                                     models and LUTs, and the libraries the plug-in loads
```

The detail that used to live in thirteen root documents is now in `references/`,
one subject per file; `SKILL.md` §2 is the map from a task to the file that
answers it.

---

## Usage

### 1. Read the skill first

Start with [`SKILL.md`](SKILL.md). §1 (ground rules) and §10 (checklists) are the
parts that prevent the expensive mistakes; the workflow in §4 is the order to
build in. If you are an AI agent, `SKILL.md` is written to be the entry point:
it names the invariants, the exact constants, and the files to copy.

### 2. Start a plugin from the template

```bash
cp -r templates/minimal-cuda-ofx plugins/my-effect
cd plugins/my-effect
```

For a CPU-only effect start from `templates/minimal-ofx/`; for a model,
`templates/onnx-inference-ofx/`. `SKILL.md` §4 names which scaffold belongs to
which effect and why the CUDA one is the wrong start for the other two.

Then rename these, in this order:

| Where | What |
| --- | --- |
| `CMakeLists.txt` | `PLUGIN_TARGET`, `PLUGIN_BUNDLE` |
| `src/Plugin.cpp` | `kPluginIdentifier` (reverse-DNS), `kPluginName`, `kPluginGrouping`, `kPluginDescription`, parameter names |
| `src/Plugin.h` | class names, if you want them specific |
| `src/Plugin.cpp` | the maths in `multiThreadProcessImages` (CPU) |
| `src/Kernel.cu` | the maths in `ScalePixel`, which both kernels call (must match the CPU path) |
| `src/Kernel.cuh` / `Kernel.cu` | the launcher argument struct, if your parameters changed |

Leave alone: `processImagesCuda`'s pitch/offset arithmetic, the stream-policy
branch, `DeviceGuard`, the loader `CMakeLists.txt` logic, and the visibility/RPATH
settings. Those are the parts that took longest to get right.

### 3. Build

```bash
cd templates/minimal-cuda-ofx
./build.sh --bootstrap-cuda      # no toolkit, no root: fetches real nvcc + headers
./build.sh                       # toolkit already installed/discovered
./build.sh Release install       # build, then copy into /usr/OFX/Plugins
CPU_ONLY=1 ./build.sh            # CPU path only (no CUDA compile at all)
```

Windows (the leading `.\` matters when `NoDefaultCurrentDirectoryInExePath=1`):

```bat
cd templates\minimal-cuda-ofx
.\build.bat Release install
```

`build.bat` has no bootstrap — Windows toolkits come from an installer. It probes
`CUDA_HOME`, `CUDA_PATH`, `where nvcc` and the default
`%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.9`, and if all of those
miss it prints the paths it searched plus
[https://developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads)
before failing, so a CPU-only build is never produced silently.

The template's checks are one command on Windows too: `test\run-tests.bat
Release` runs the artifact check, executes `test/gpu-kernel-test.cu` on the real
GPU, and drives the built `.ofx` through the contract harness. It is the
counterpart of `test/run-tests.sh` where `g++`, `nm -D`, `readelf`, `dlopen` and
valgrind do not exist.

#### Arguments accepted by the scripts

| Script | Argument | Effect |
| --- | --- | --- |
| `build.sh` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` | Sets `CMAKE_BUILD_TYPE`. Default `Release`. |
| `build.sh` | `install` | After building, runs the `install-bundle` target, which copies the bundle into `OFX_INSTALL_DIR`. Needs write permission there. |
| `build.sh` | `--bootstrap-cuda` | When no toolkit is found, downloads and unpacks the CUDA packages, then builds with CUDA. |
| `build.sh` | `-h`, `--help` | Prints the usage header. |
| `build.bat` | `%1` (`Release`, `Debug`, …) | Passed to `cmake --build --config`. Default `Release`. |
| `build.bat` | `%2` equal to `install` | Runs the `install-bundle` target, which `minimal-cuda-ofx` and `onnx-inference-ofx` define. |

`build.sh` accepts these in any order and rejects anything else with exit code 2.
`build.bat` picks the newest Visual Studio instance that actually has the C++
toolset (through `vswhere`), reuses the generator a previous run left in
`build\CMakeCache.txt`, and falls back to Ninja when that instance has no NVIDIA
CUDA build customisations. Set `CMAKE_GENERATOR` to override it.

#### Options and environment variables

All optional. `build.sh` / `build.bat` translate their environment into CMake
cache variables, so either layer can be driven directly.

| Variable | Accepted by | Default | Meaning |
| --- | --- | --- | --- |
| `CUDA_HOME` | `build.sh` | unset | Toolkit root containing `bin/nvcc`. Discovery order when unset: `CUDA_HOME`, then `nvcc` on `PATH`, then `/usr/local/cuda`, `/usr/local/cuda-*`, `/opt/cuda`, then a directory bootstrapped by this script. Passed on as `CMAKE_CUDA_COMPILER` and `CUDAToolkit_ROOT`. |
| `CUDA_PATH` | `build.bat` | unset | Windows equivalent of `CUDA_HOME`. Discovery order: `CUDA_HOME`, `CUDA_PATH`, `where nvcc`, then `%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.9`. |
| `CUDA_ARCHS` | both | unset | Semicolon list, e.g. `86;89`, passed as `CMAKE_CUDA_ARCHITECTURES`. When unset, CMake uses `75;80;86;89;90`, plus `100;120` if the toolkit is ≥ 12.8. |
| `OFX_SOURCE_DIR` | both, and the CMake cache | empty | Use an existing OpenFX tree instead of fetching. It must contain `include/ofxImageEffect.h` — for Resolve's SDK that is its `OpenFX-1.4` subdirectory, not the `Support/Developer/OpenFX` parent. CMake aborts with a message if the file is missing; the offline build works with this set. |
| `OFX_CUDA_TOOLKIT_DIR` | `build.sh` | `${XDG_CACHE_HOME:-$HOME/.cache}/ofx-cuda-toolkit` | Where the bootstrap puts `debs/`, `stage/` and the `home` symlink. |
| `CUDA_VER` | `build.sh` | `12.9` | Toolkit series to bootstrap; package names are derived from it (`12.9` becomes `-12-9`). |
| `CUDA_REPO` | `build.sh` | `https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64` | apt repository holding the packages. The default targets Ubuntu 24.04 x86-64; set this for another distribution or architecture. |
| `CPU_ONLY` | both | unset | `1` skips discovery and bootstrap and sets `OFXPLUGIN_ENABLE_CUDA=OFF`. |
| `OFX_INSTALL_DIR` | CMake (used by the `install` step) | `/usr/OFX/Plugins`, `%COMMONPROGRAMFILES%/OFX/Plugins` on Windows, `/Library/OFX/Plugins` on macOS | Destination of `install-bundle`. |
| `OFXPLUGIN_ENABLE_CUDA` | CMake | `ON` | Compile the CUDA render path. Falls back to `OFF` with a warning if no compiler is found, so the plugin keeps its CPU path. |
| `OFXPLUGIN_CUDA_STATIC_RUNTIME` | CMake | `OFF` | `ON` links `CUDA::cudart_static` instead of the shared runtime. Verified: the resulting `.ofx` drops `libcudart.so.12` from `NEEDED` and has zero undefined `cuda*` symbols. |
| `CMAKE_CUDA_COMPILER`, `CUDAToolkit_ROOT` | CMake | set by the scripts from `CUDA_HOME` | Seam for a toolkit in an unusual location. |
| `CMAKE_BUILD_TYPE`, `CMAKE_CUDA_ARCHITECTURES`, `CMAKE_CUDA_VISIBILITY_PRESET` | CMake | set by the scripts | Standard CMake variables; use these when invoking CMake without the wrapper scripts. |

The build ends by printing what it produced: the bundle path, the `.ofx` path,
the exported dynamic symbol count (must be 2) and whether the CUDA launcher is in
the binary.

> **Note on the bootstrap.** The `nvidia-cuda-nvcc-cu12` pip wheel is *not*
> enough — it ships `ptxas` and NVVM but no `nvcc`/`cudafe++`/`nvlink`, so it
> cannot compile a translation unit. The bootstrap instead unpacks NVIDIA's
> official `.deb` packages with `ar`/`tar` into a private prefix, which needs no
> root and no system packages. It requires network access and `curl`, `tar`,
> `gzip`, `awk` and binutils (`ar`); `nm` from the same binutils is used for the
> end-of-build symbol check.

#### Known gaps in the repository

Documented rather than hidden, because each one is a trap for the next reader:

1. **No top-level `LICENSE` file.** The per-file SPDX headers are the operative
   licence statement (see [Licensing](references/reference-projects.md#licensing-and-provenance)).
2. **The Windows path was executed on another machine, not here.** The 2026-11-22
   field test built, loaded and rendered through `.\build.bat`; this checkout can
   only syntax-review it, so treat the Windows build as externally verified **[B]**
   ([the archived run](references/field-evidence.md))
   rather than as a check `./run-tests.sh` performs.
3. **`install` needs write access** to `OFX_INSTALL_DIR`. The default
   `/usr/OFX/Plugins` is root-owned on most Linux systems, so run that step with
   `sudo` or point `OFX_INSTALL_DIR` at a writable host plugin directory.
   Loading the plug-in outside a host needs `LD_LIBRARY_PATH` pointing at the
   bundled libraries, or a copy of them placed beside the `.ofx`. A toolkit
   fetched by `--bootstrap-cuda` keeps `libcudart` in `<toolkit>/lib64`.


### 4. Verify before you test in a host

Cheap checks that catch the common failures:

```bash
O=build/bundle/MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx
nm -D --defined-only "$O" | grep ' T '      # exactly 2: OfxGetNumberOfPlugins, OfxGetPlugin
nm "$O" | grep LaunchGainRGBA32F            # CUDA launcher present (if built with CUDA)
readelf -d "$O" | grep NEEDED               # libcudart.so.12 when CUDA is on
readelf -S "$O" | grep nv_fatbin            # device code is embedded
```

Then install and work through the per-host protocol in
[`host-resolve.md`](references/host-resolve.md) §7 — in a real
host, render to file, at an odd resolution such as 1919×1081 (1920×1080 hides
stride bugs).

The repository also ships a host test suite that needs no GPU:

```bash
./test/run-tests.sh
```

It compiles the real `Kernel.cu` with a shim, checks the alignment predicate,
compares both kernels over 3,240 emulated grid launches **[V]**, compares the hoisted CPU
loop against the per-pixel form, runs both kernels concurrently from six threads
on disjoint buffers, and — when `nvcc` is on `PATH` — asserts the PTX instruction
counts. Exit status is non-zero on any mismatch.

The same suite runs under the checkers that need no GPU:

```bash
SANITIZE=address,undefined ./test/run-tests.sh   # memory + UB
SANITIZE=thread            ./test/run-tests.sh   # races, in the concurrent test
RUN_VALGRIND=1             ./test/run-tests.sh   # memcheck, full leak check
./test/run-analysis.sh                           # cppcheck + shellcheck
```

Each stage prints why it skipped if a tool is missing, so a run with no sanitizer
installed cannot look like a clean run. What these checks can and cannot prove is
tabulated in [`checklists.md`](references/checklists.md) §1.

If the plugin stops loading after a crash, the host has blacklisted it: clear
Resolve's `OFXPluginCacheV2.xml` and re-enable it under Preferences → Video
Plugins. Details in `references/debugging-and-symptoms.md` §7.

---

---

## Requirements

| | Linux (verified) | Windows |
| --- | --- | --- |
| Compiler | GCC 13.3 as nvcc's host compiler | MSVC 2022 (x64) |
| CUDA | 12.9 series (nvcc 12.9.41 verified), or `--bootstrap-cuda` | 12.9 toolkit |
| Bootstrap tooling | `curl`, `tar`, `gzip`, `awk`, binutils (`ar`, `nm`) | not applicable — no bootstrap on Windows |
| CMake | ≥ 3.21 (verified 3.31.6) | ≥ 3.21 |
| OpenFX | `OFX_Release_1.5.1`, fetched automatically or `OFX_SOURCE_DIR` | same |
| GPU | not needed to build; needed to run the CUDA path | same |

---

## What has actually been verified

The full ledger, with the command behind every line and the list of what is
*not* verified, is `references/verification-status.md`. The raw runs it cites are
in `evidence/`.

---

## Licence

BSD-3-Clause, in [`LICENSE`](LICENSE); every source file declares it with an SPDX
identifier on its first line. Third-party code is fetched, not vendored, and what
each component obliges you to do is in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Nothing here grants rights in OpenFX, NVIDIA CUDA, DaVinci Resolve, Nuke, SCRATCH
or Natron beyond what their own licences give you.
