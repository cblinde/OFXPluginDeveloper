# OFXPluginDeveloper

A working reference for building **CUDA-accelerated OpenFX plugins** for DaVinci
Resolve Studio, Nuke, SCRATCH and Natron on Windows and Linux — plus the rules
that keep them from crashing a host.

| File | What it is |
| --- | --- |
| [`SKILL.md`](SKILL.md) | Master reference: ground rules, generation workflow, verified constants, build standards, anti-patterns, checklists |
| [`ARCHITECTURE_AND_CUDA_PIPELINE.md`](ARCHITECTURE_AND_CUDA_PIPELINE.md) | Lifecycle, zero-copy, the stream contract, pitch arithmetic, RoD/RoI, colour/alpha, TensorRT/ONNX zero-copy, profiling |
| [`HOST_COMPATIBILITY_MATRIX.md`](HOST_COMPATIBILITY_MATRIX.md) | Per-host behaviour and workarounds, with every claim graded by evidence source |
| [`templates/minimal-cuda-ofx/`](templates/minimal-cuda-ofx/) | Complete, buildable plugin: CMake + OFX host wrapper + CUDA kernel + CPU fallback |

Start with `SKILL.md`.

## Quick start (Linux)

```bash
cd templates/minimal-cuda-ofx

# 1. No CUDA toolkit and no root? Bootstrap a real nvcc + runtime headers
#    (official NVIDIA apt packages, unpacked with ar/tar into a private prefix).
./build.sh --bootstrap-cuda

# 2. Or use a toolkit you already have.
CUDA_HOME=/usr/local/cuda ./build.sh

# 3. Build, then copy the bundle into /usr/OFX/Plugins.
./build.sh Release install

# 4. CPU path only, on purpose.
CPU_ONLY=1 ./build.sh
```

The build prints three proofs at the end: how many dynamic symbols are exported
(must be 2), whether the CUDA launcher is in the binary, and the bundle path.

## Quick start (Windows)

Install the CUDA Toolkit and run:

```bat
cd templates\minimal-cuda-ofx
build.bat Release install
```

`build.bat` finds the toolkit via `%CUDA_HOME%`, `%CUDA_PATH%`, `PATH` or the
default install directory, and fails with instructions if it cannot. There is no
no-root bootstrap on Windows: the toolkit is an installer, not an unpackable
package.

## Requirements

| | Linux (verified) | Windows |
| --- | --- | --- |
| Compiler | GCC 13.3 (nvcc host compiler) | MSVC 2022 x64 |
| CUDA | 12.9.41 toolkit; `75;80;86;89;90` (+`100;120` on CUDA ≥ 12.8) | same |
| CMake | ≥ 3.21 (verified 3.31.6) | ≥ 3.21 |
| OpenFX | `OFX_Release_1.5.1`, fetched automatically or via `OFX_SOURCE_DIR` | same |

No GPU is required to *build*. A GPU is required to *run* the CUDA path.

## What has actually been verified

Built and checked on Linux x86-64 with GCC 13.3.0, CUDA 12.9.41 and CMake 3.31.6:

- CUDA-enabled configure + build compiles `Kernel.cu` with real `nvcc` for
  `75;80;86;89;90;100;120`, links `libcudart.so.12`, and produces
  `MinimalCudaGain.ofx.bundle/Contents/Linux-x86-64/MinimalCudaGain.ofx`.
- The `.ofx` exports exactly `OfxGetNumberOfPlugins` and `OfxGetPlugin`, contains
  the launcher symbol and a `.nv_fatbin` section with the kernel, and passes
  `dlopen` → `OfxGetNumberOfPlugins() == 1` with the expected identifier.
- `CPU_ONLY=1`, and "CUDA requested but no compiler", both still build and pass
  the same registration check.

**Not verified:** no GPU is present, so no kernel has ever executed; no host is
installed, so no pixel has ever been rendered; the Windows/MSVC path has not been
compiled; and no performance numbers exist. See `SKILL.md` §12.

## Bundle layout

```
MinimalCudaGain.ofx.bundle/
  Contents/
    Linux-x86-64/MinimalCudaGain.ofx
    Win64/MinimalCudaGain.ofx          (Windows build)
    Resources/                          (icons, models, LUTs)
```

Install locations: `%COMMONPROGRAMFILES%\OFX\Plugins` on Windows,
`/usr/OFX/Plugins` on Linux, or anywhere on `OFX_PLUGIN_PATH`.

The bootstrapped toolkit keeps `libcudart` in `<toolkit>/lib64`; put it on
`LD_LIBRARY_PATH` (or beside the `.ofx`) when loading the plugin outside a host
that ships its own CUDA runtime.
