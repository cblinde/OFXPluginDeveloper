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
- **The two reference documents** cover the parts you only learn from a bug
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
| [`templates/minimal-cuda-ofx/`](templates/minimal-cuda-ofx/) | The buildable template: CMake + OFX host wrapper + CUDA kernel + CPU fallback |

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
| [NVIDIA CUDA apt repository](https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64) | 12.9.41 | The `cuda-nvcc`, `cuda-nvvm`, `cuda-cudart`, `cuda-cudart-dev` and `cuda-crt` packages that `build.sh --bootstrap-cuda` unpacks without root. |

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

- The template is original code released under **BSD-3-Clause** (matching the
  OpenFX headers and Support library it links).
- The GPL-3.0 projects above were consulted as documentation and engineering
  evidence. No source, table or shader text was copied from them, so nothing here
  inherits their licence.
- `templates/minimal-cuda-ofx` compiles the upstream OpenFX Support library
  sources directly into the plugin; those remain BSD-3-Clause and are fetched at
  configure time rather than vendored. Keep the copyright notice if you vendor
  them yourself.
- Verification used the NVIDIA CUDA toolkit packages described above, which are
  governed by NVIDIA's own licence agreement, not by this repository.

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
| `src/Kernel.cu` | the maths in `GainKernelRGBA32F` (must match the CPU path) |
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

All options are environment variables, all optional:

| Variable | Applies to | Meaning |
| --- | --- | --- |
| `CUDA_HOME` | `build.sh` | Toolkit to use. Otherwise discovered via `PATH`, `/usr/local/cuda*`, `/opt/cuda`, then a bootstrapped one. |
| `CUDA_ARCHS` | both | Semicolon list, e.g. `86;89`. Default `75;80;86;89;90` (+`100;120` on CUDA ≥ 12.8). |
| `OFX_SOURCE_DIR` | both | Existing OpenFX checkout. Point at Resolve's shipped `Support/Developer/OpenFX` for an offline build. |
| `OFX_CUDA_TOOLKIT_DIR` | `build.sh` | Where `--bootstrap-cuda` installs (default `${XDG_CACHE_HOME:-$HOME/.cache}/ofx-cuda-toolkit`). |
| `CUDA_VER` / `CUDA_REPO` | `build.sh` | Toolkit series and apt repository used by the bootstrap (default `12.9`, Ubuntu 24.04). |
| `CPU_ONLY=1` | both | Skip CUDA entirely. |
| `OFX_INSTALL_DIR` | CMake | Override the install target for `install-bundle`. |
| `OFXPLUGIN_CUDA_STATIC_RUNTIME=ON` | CMake | Link `cudart_static` instead of the shared runtime. |

The build ends by printing three facts about what it produced: exported dynamic
symbol count (must be 2), whether the CUDA launcher is in the binary, and the
bundle path.

> **Note on the bootstrap.** The `nvidia-cuda-nvcc-cu12` pip wheel is *not*
> enough — it ships `ptxas` and NVVM but no `nvcc`/`cudafe++`/`nvlink`, so it
> cannot compile a translation unit. The bootstrap instead unpacks NVIDIA's
> official `.deb` packages with `ar`/`tar` into a private prefix, which needs no
> root and no system packages.

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

If the plugin stops loading after a crash, the host has blacklisted it: clear
Resolve's `OFXPluginCacheV2.xml` and re-enable it under Preferences → Video
Plugins. Details in `SKILL.md` §7.

---

## Requirements

| | Linux (verified) | Windows |
| --- | --- | --- |
| Compiler | GCC 13.3 as nvcc's host compiler | MSVC 2022 (x64) |
| CUDA | 12.9.41 toolkit, or `--bootstrap-cuda` | 12.9 toolkit |
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
  the launcher symbol and a `.nv_fatbin` section with the kernel, and passes
  `dlopen` → `OfxGetNumberOfPlugins() == 1` with the expected identifier.
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
templates/minimal-cuda-ofx/
  CMakeLists.txt                     bundle-producing build, CUDA optional
  src/Plugin.h  src/Plugin.cpp        OFX host wrapper: params, dispatch, isIdentity
  src/Kernel.cuh src/Kernel.cu        CUDA interface + kernel + launcher
  build.sh  build.bat                 toolkit discovery, bootstrap, build
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
