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
substitute for testing in a real host. No pixel in this repository has been
rendered by a host — see [What has actually been verified](#what-has-actually-been-verified).

| File | What it is |
| --- | --- |
| [`SKILL.md`](SKILL.md) | Master reference: ground rules, generation workflow, verified constants, build standards, anti-patterns, checklists, mandatory git workflow |
| [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) | Lifecycle, zero-copy, the stream contract, pitch arithmetic, RoD/RoI, colour/alpha, TensorRT/ONNX zero-copy, toolchain, profiling |
| [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) | Resolve/Nuke/SCRATCH/Natron behaviour and workarounds, every claim graded A/B/C by evidence source |
| [`BEST_PRACTICES_AND_PERFORMANCE.md`](BEST_PRACTICES_AND_PERFORMANCE.md) | Review-level synthesis: what the reference projects teach, verified header quirks, host- and kernel-side performance rules with measured PTX evidence, anti-patterns by symptom |
| [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) | The questions to answer before shipping, each one settled by a header line or a spec sentence, plus the automated gates that answer half of them |
| [`AI_MODEL_INTEGRATION.md`](AI_MODEL_INTEGRATION.md) | Putting a neural network in an OFX plug-in: what six shipped projects do, Python/Rust/C#/JS/Julia embedding hazards, ONNX Runtime's exact API, and when CUDA is worth it |
| [`REFERENCE_READING_GUIDE.md`](REFERENCE_READING_GUIDE.md) | New to OFX: the three implementation tiers, the patterns worth copying, anti-patterns by their tells, and a seven-step reading plan — every claim anchored to a file and line |
| [`templates/minimal-cuda-ofx/`](templates/minimal-cuda-ofx/) | The buildable template: CMake + OFX host wrapper + CUDA kernel + CPU fallback |
| [`templates/onnx-inference-ofx/`](templates/onnx-inference-ofx/) | The buildable inference template: ONNX Runtime session, pitch-correct packing, tiled inference, golden-file tests |

---

## Sources

Everything here was derived from primary specifications and from reading real
plugins. Nothing was written from memory of an API. Where a claim could not be
checked against a running host it is graded in
[`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) rather than stated
as fact.

### Primary specifications and SDKs

| Source | Version used | What it contributed |
| --- | --- | --- |
| [Academy Software Foundation `openfx`](https://github.com/AcademySoftwareFoundation/openfx) | tag `OFX_Release_1.5.1` | The headers themselves (`ofxCore.h`, `ofxImageEffect.h`, `ofxGPURender.h`, `ofxParam.h`), the C++ Support library (`Support/include`, `Support/Library`), and the packaging rules. Fetched automatically by CMake, or pointed at with `OFX_SOURCE_DIR`. |
| [OpenFX reference documentation](https://openfx.readthedocs.io/) | 1.5.1 | `ofxRendering` (render action, multi-thread safety), `ofxThreadSafety` (which actions are legal on render threads), `ofxPackaging` (`.ofx.bundle` layout, install paths, arch directory names). |
| [`ofxGPURender.h`](https://github.com/AcademySoftwareFoundation/openfx/blob/OFX_Release_1.5.1/include/ofxGPURender.h) | 1.5.1 | The authoritative wording of the CUDA stream contract — the "SHOULD NOT wait … SHOULD NOT call `cudaDeviceSynchronize()` at any time" clause that `SKILL.md` §1 and the template's sync branch implement. CUDA/Metal/OpenCL render properties were introduced in OFX 1.5. |
| [`Support/Plugins/GPUGain`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Support/Plugins/GPUGain) | 1.5.1 | The canonical `OFX::ImageProcessor` GPU-dispatch pattern (CUDA/OpenCL/Metal/CPU) that the template's processor derives from. **Deliberately corrected:** it derives width from `bounds` and ignores `rowBytes`, which shears on padded scanlines. |
| [`Support/Plugins/Basic`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Support/Plugins/Basic), [`Examples/Invert`](https://github.com/AcademySoftwareFoundation/openfx/tree/OFX_Release_1.5.1/Examples/Invert) | 1.5.1 | Parameter/descriptor idioms, and a raw-C (no Support library) plugin used to cross-check what the wrapper does for you. |
| [DaVinci Resolve OFX samples](https://github.com/illusionyy/DaVinci-Resolve-OpenFX-Samples) (public mirror of the SDK shipped in Resolve 18.5) | OFX-1.4 headers + Support | Resolve's own `GainPlugin`, `TemporalBlurPlugin`, `RandomFrameAccessPlugin` and `DissolveTransitionPlugin`, all with CUDA/OpenCL/Metal kernels. Confirms Resolve's GPU expectations and the multi-frame access model. The SDK is installed with Resolve at `Support/Developer/OpenFX/` (`C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX` on Windows, `/opt/resolve/Developer/OpenFX` on Linux) and can be passed as `OFX_SOURCE_DIR` for a fully offline build. |
| [Blackmagic Design forum](https://forum.blackmagicdesign.com/) | — | Confirms there is no separate "Resolve plugin SDK": OFX is the plugin route, and Resolve supports the OpenCL/CUDA extension to OFX. Used for the Resolve-specific rows in the host matrix (graded B). |
| [NVIDIA CUDA C++ Programming Guide](https://docs.nvidia.com/cuda/cuda-programming-guide/) / [Runtime API](https://docs.nvidia.com/cuda/cuda-runtime-api/) | 12.9 | Kernel launch, streams, `cudaPointerGetAttributes`, device/primary-context semantics, pitch and alignment behaviour, and the architecture list (§6 of `SKILL.md`). |
| [NVIDIA CUDA apt repository](https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64) | 12.9 series | Source of the toolkit packages that `build.sh --bootstrap-cuda` unpacks without root. Itemised, with versions, in [Artifacts the build pulls in](#artifacts-the-build-pulls-in). |

### Artifacts the build pulls in

Everything below is obtained at build time and is **not** stored in this
repository. Versions are what upstream served when this was verified (November
2026); the bootstrap deliberately resolves the newest package in the `12.9`
series from the repository index instead of pinning exact versions, so a later
run may pick up a newer patch release.

| Artifact | Version | Obtained by | What it contributes |
| --- | --- | --- | --- |
| OpenFX source tree | git tag `OFX_Release_1.5.1`, shallow clone | `FetchContent_Declare` in `CMakeLists.txt` (skipped when `OFX_SOURCE_DIR` is set) | All OFX headers, plus the eight `Support/Library/*.cpp` translation units compiled into the plugin: `ofxsCore`, `ofxsImageEffect`, `ofxsInteract`, `ofxsLog`, `ofxsMultiThread`, `ofxsParams`, `ofxsProperty`, `ofxsPropertyValidation`. Fetched, never vendored. |
| `Packages.gz` | live index | `build.sh --bootstrap-cuda`, via `curl` | The package-to-filename map; this is what lets the script name the `.deb` files without hard-coding versions. |
| `cuda-nvcc-12-9` | 12.9.41-1 | bootstrap: `ar` + `tar` into `$OFX_CUDA_TOOLKIT_DIR/stage` | `nvcc`, `cudafe++`, `ptxas`, `nvlink`, `fatbinary`, `bin2c` — the compiler that actually builds `Kernel.cu`. |
| `cuda-nvvm-12-9` | 12.9.41-1 | bootstrap | `nvvm/libdevice/libdevice.10.bc` and the NVVM libraries nvcc uses for device code generation. |
| `cuda-cudart-12-9` | 12.9.37-1 | bootstrap | `libcudart.so.12` — the plugin's `NEEDED` entry (`cudart64_*.dll` on Windows). |
| `cuda-cudart-dev-12-9` | 12.9.37-1 | bootstrap | `include/cuda_runtime.h` and friends — the only CUDA header the template includes — plus the link-time `libcudart.so` stub. |
| `cuda-crt-12-9` | 12.9.41-1 | bootstrap | `include/crt/*.h` device-runtime headers nvcc needs for `<<<>>>` launches and vector types. |
| `CUDA::cudart` or `CUDA::cudart_static` | from the toolkit above | `find_package(CUDAToolkit REQUIRED)` plus `target_link_libraries` | The import target the module links. Default is the shared runtime (`NEEDED libcudart.so.12`); `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` swaps in `libcudart_static.a`, which ships in `cuda-cudart-dev`. |

**Nothing else is fetched.** There are no git submodules (`git submodule status`
is empty), no `.patch` files, no vendored third-party directory, and no package
manager is invoked — `pip`, `apt`, `vcpkg` and `conan` are never called by any
script here. The only third-party code compiled into the plugin is the OpenFX
Support library, fetched at configure time.

The bootstrap leaves three things under `$OFX_CUDA_TOOLKIT_DIR`: `debs/` (the
downloaded packages and the index), `stage/` (their unpacked contents) and a
`home` symlink to the extracted `cuda-12.9` tree. Delete that directory to
reclaim roughly 100 MB.

### Open-source implementations analysed

These were read for architecture and failure modes. **No code was copied from
them** — all are GPL-3.0 and this repository's template is BSD-3-Clause, so they
informed design decisions only (see [Licensing](#licensing-and-provenance)).

| Project | What it demonstrated | What this repo took from it |
| --- | --- | --- |
| [`gyroflow/gyroflow-plugins`](https://github.com/gyroflow/gyroflow-plugins) | Rust OFX plugin with FFI into a native sensor engine; probes host GPU support per render (`get_cuda_render_supported`, `get_metal_command_queue`, …); runs in Resolve and SCRATCH; installer/UX for host plugins. | The "probe the host, never sniff the host name" rule, and the multi-host support matrix. |
| [`gyroflow-plugins` issue #64](https://github.com/gyroflow/gyroflow-plugins/issues/64) | 32 Resolve crashes in 24 days; 31 attributable to the plugin releasing a host-owned `MTLCommandQueue`. | The ownership rule — never destroy a borrowed GPU object — and the "GPU lifetime bugs surface in host code" case study in `HOST_COMPATIBILITY_MATRIX.md` §8. |
| [`Dec18studios/Open-DRT-OFX`](https://github.com/Dec18studios/Open-DRT-OFX) | CMake build for Metal (macOS) + CUDA (Windows/Linux) with a clean host-wrapper / kernel split and a single transform definition reused by both backends; cross-platform CI packaging. | The one-pixel-model-two-backends rule and the CMake structure (bundle output directories, per-architecture folders). |
| [`MattGrdinic/OneGrade`](https://github.com/MattGrdinic/OneGrade) | Single-node Resolve grade: vendored OFX 1.4 Support, grouped parameter UI, `OneGradePipeline.h` as the single source of truth mirrored by `MetalKernel.mm` / `OpenCLKernel.cpp` / `CudaKernel.cu` with a CPU fallback. | Confirmation that vendored Support + one pipeline header + per-backend kernels is the durable shape; parameter grouping/paging idioms. |
| [`cnoellert/forge-colorx`](https://github.com/cnoellert/forge-colorx) | Runtime expression evaluation compiled once per render; host coverage across Flame, Resolve, Nuke and Natron; documents host-specific UI quirks. | Evidence for the host-quirk rows, and the "compile per render, evaluate per pixel" separation that the template's processor/launcher split follows. |
| [`rewired/signal-cult`](https://github.com/rewired/signal-cult) | A suite of CUDA/CPU OFX plugins for Resolve on Windows sharing one shell; per-plugin source tree with adapter/core separation; explicit CUDA 12.8 toolchain paths. | Multi-plugin-in-one-bundle and Windows CUDA build practice; the reason `SKILL.md` calls out symbol visibility (two plugins, one process). |
| [`baldavenger/BaldavengerOFX`](https://github.com/baldavenger/BaldavengerOFX) | The plugin set Blackmagic engineers point developers at for Resolve: OFX written specifically for Resolve, nearly all with a CUDA kernel, several with OpenCL. | Reference for Resolve-specific behaviour (choice parameters, CUDA+OpenCL dispatch) and for what "works in Resolve" actually looks like. |

### Licensing and provenance

- The template source files are original code, each marked
  `SPDX-License-Identifier: BSD-3-Clause` in its header (matching the OpenFX
  headers and Support library it links). Note that the repository currently has
  **no top-level `LICENSE` file** — the SPDX headers are the operative statement.
- The GPL-3.0 projects above were consulted as documentation and engineering
  evidence. No source, table or shader text was copied from them, so nothing here
  inherits their licence.
- `templates/minimal-cuda-ofx` compiles the upstream OpenFX Support library
  sources directly into the plugin; those remain BSD-3-Clause and are fetched at
  configure time rather than vendored. Keep the copyright notice if you vendor
  them yourself.
- Verification used the NVIDIA CUDA toolkit packages described above, which are
  governed by NVIDIA's own licence agreement, not by this repository.

### Deliberately not included

Listed so nobody goes looking for them, or mistakes an incomplete dependency for
an oversight:

| Not used | Why |
| --- | --- |
| TensorRT, ONNX Runtime | `ARCHITECTURE_AND_CUDA_PIPELINE.md` §9 describes the zero-copy integration pattern (device pointers, `IoBinding`, the CUDA EP compute stream), but the template implements no inference path. Adding either would put a multi-hundred-megabyte dependency into a starter template. |
| NVTX (`nvtx3/nvToolsExt.h`) | Appears only as a profiling snippet in `ARCHITECTURE_AND_CUDA_PIPELINE.md` §5. The template neither includes nor links it. |
| OpenCL, Metal, OpenGL backends | `OFX::ImageProcessor` can dispatch to them, but only CUDA and CPU are implemented; the others are out of scope. |
| DaVinci Resolve scripting API, DCTL | Different extension mechanisms, not OFX. Useful alongside a plugin, not part of one. |
| pip CUDA wheels (`nvidia-cuda-nvcc-cu12`, `nvidia-cuda-runtime-cu12`) | Tried and rejected for the bootstrap: the nvcc wheel ships `ptxas` and NVVM but no `nvcc`/`cudafe++`/`nvlink`, so it cannot compile a translation unit. |
| The GPL reference plugins | Read as engineering evidence only; never vendored or copied — see [Licensing](#licensing-and-provenance). |

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

Windows:

```bat
cd templates\minimal-cuda-ofx
build.bat Release install
```

`build.bat` has no bootstrap — Windows toolkits come from an installer. It probes
`CUDA_HOME`, `CUDA_PATH`, `where nvcc` and the default
`%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.9`, and if all of those
miss it prints the paths it searched plus
[https://developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads)
before failing, so a CPU-only build is never produced silently.

#### Arguments accepted by the scripts

| Script | Argument | Effect |
| --- | --- | --- |
| `build.sh` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` | Sets `CMAKE_BUILD_TYPE`. Default `Release`. |
| `build.sh` | `install` | After building, runs the `install-bundle` target, which copies the bundle into `OFX_INSTALL_DIR`. Needs write permission there. |
| `build.sh` | `--bootstrap-cuda` | When no toolkit is found, downloads and unpacks the CUDA packages, then builds with CUDA. |
| `build.sh` | `-h`, `--help` | Prints the usage header. |
| `build.bat` | `%1` (`Release`, `Debug`, …) | Passed to `cmake --build --config`. Default `Release`. |
| `build.bat` | `%2` equal to `install` | Runs the `install-bundle` target. |

`build.sh` accepts these in any order and rejects anything else with exit code 2.
`build.bat` uses the `Visual Studio 17 2022` generator with `-A x64`; edit it or
pass `-G` for another toolchain.

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
   licence statement (see [Licensing](#licensing-and-provenance)).
2. **`build.bat` has never been executed** — it is syntax-reviewed only. Until it
   is run under MSVC, treat the Windows path as unproven.
3. **`install` needs write access** to `OFX_INSTALL_DIR`. The default
   `/usr/OFX/Plugins` is root-owned on most Linux systems, so run that step with
   `sudo` or point `OFX_INSTALL_DIR` at a writable host plugin directory.


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
[`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) §7 — in a real
host, render to file, at an odd resolution such as 1919×1081 (1920×1080 hides
stride bugs).

The repository also ships a host test suite that needs no GPU:

```bash
./test/run-tests.sh
```

It compiles the real `Kernel.cu` with a shim, checks the alignment predicate,
compares both kernels over 3,240 emulated grid launches, compares the hoisted CPU
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
tabulated in [`PREFLIGHT_REVIEW_CHECKLIST.md`](PREFLIGHT_REVIEW_CHECKLIST.md) §1.

If the plugin stops loading after a crash, the host has blacklisted it: clear
Resolve's `OFXPluginCacheV2.xml` and re-enable it under Preferences → Video
Plugins. Details in `SKILL.md` §7.

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

## What has actually been verified

Built and checked on Linux x86-64 with GCC 13.3.0, CUDA 12.9.41, CMake 3.31.6 and
OpenFX `OFX_Release_1.5.1`:

- CUDA-enabled configure + build compiles `Kernel.cu` with real `nvcc` for
  `75;80;86;89;90;100;120`, links `libcudart.so.12`, and produces
  `MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx`.
- That `.ofx` exports exactly `OfxGetNumberOfPlugins` and `OfxGetPlugin`, contains
  the launcher symbol, both kernels (`GainKernelRGBA32FVec4`,
  `GainKernelRGBA32FScalar`) and a `.nv_fatbin` section, and passes
  `dlopen` → `OfxGetNumberOfPlugins() == 1` with the expected identifier.
- The vectorized kernel emits one 16-byte load and one 16-byte store per pixel
  at PTX level (`-arch=sm_89`), with 14 registers, no spills and no stack, the
  same register count as the scalar fallback.
- Both kernels produce byte-identical output to a scalar reference across 3,240
  host-emulated grid launches covering widths 0–63, all four `width % 4` classes,
  odd origins, padded pitches and 4-byte-misaligned bases, and neither writes
  outside the render window.
- `./test/run-tests.sh` passes end to end (predicate table, 3,240 launches, CPU-loop
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

**Not verified:** no GPU is present, so no kernel has ever executed; no host is
installed, so no pixel has ever been rendered; the Windows/MSVC path has not been
compiled; and no performance numbers exist. See `SKILL.md` §12.

## Repository layout

```
SKILL.md                            master reference and workflow
ARCHITECTURE_AND_CUDA_PIPELINE.md    deep dive
HOST_COMPATIBILITY_MATRIX.md         per-host differences
PREFLIGHT_REVIEW_CHECKLIST.md        pre-ship questions, each with the line that settles it
AI_MODEL_INTEGRATION.md              model runtimes, language embedding, CUDA decision rules
REFERENCE_READING_GUIDE.md           how to read OFX code: tiers, patterns, anti-patterns, reading plan
templates/minimal-cuda-ofx/
  CMakeLists.txt                     bundle-producing build, CUDA optional
  src/Plugin.h  src/Plugin.cpp        OFX host wrapper: params, dispatch, isIdentity
  src/Kernel.cuh src/Kernel.cu        CUDA interface + vec4/scalar kernels + launcher
  test/run-tests.sh                    GPU-free suite: equivalence, concurrency, PTX, bundle, memcheck
  test/run-analysis.sh                 cppcheck + shellcheck over sources and scripts
  test/kernel_equivalence.cpp          host harness for kernels and CPU loop
  test/shim/                           minimal <cuda_runtime.h> for host builds
  build.sh  build.bat                 toolkit discovery, bootstrap, build
templates/onnx-inference-ofx/
  CMakeLists.txt                     bundle build, ONNX Runtime detection + bundling
  src/Plugin.h  src/Plugin.cpp        OFX wrapper: window addressing, alpha, render
  src/ModelRunner.h  src/ModelRunner.cpp   one ORT session behind a pimpl
  src/AiCore.h  src/AiCore.cpp        packing, tiling, whole/tiled inference
  models/                            committed 611-byte model + golden tensor
  tools/make-test-model.py           regenerates model and golden
  test/run-tests.sh                  packing, golden, tiling, negative control, bundle
  test/ai_pipeline_test.cpp          the harness
  build.sh                           configure + build + report
```

Bundle layout produced by the build:

```
MinimalCudaGain.ofx.bundle/
  Contents/
    Linux-x86-64/MinimalCudaGain.ofx
    Win64/MinimalCudaGain.ofx          (Windows build)
    Resources/                          (icons, models, LUTs)
```

Install locations: `%COMMONPROGRAMFILES%\OFX\Plugins` on Windows,
`/usr/OFX/Plugins` on Linux, or any directory on `OFX_PLUGIN_PATH`.

The bootstrapped toolkit keeps `libcudart` in `<toolkit>/lib64`; put it on
`LD_LIBRARY_PATH` (or beside the `.ofx`) when loading the plugin outside a host
that ships its own CUDA runtime.
