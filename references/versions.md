# Versions: what is pinned, what the machine supplies, what a run observed

A build is a claim about versions, and most of them are not in this repository.
Three kinds are worth separating, because they fail differently: a **pin** is a
line in the tree, so a change is a diff somebody can review; a **machine
supplied** version is read at configure or run time and can differ between two
checkouts of the same commit; an **observed** version is a fact about one run and
a bug report's best friend. This document is the inventory of all three, with the
file that holds each pin and the command that reads each inherited one. The
numbers below were read on 2026-11-27 on the Windows machine this repository is
developed on; the verbatim output is
[`../evidence/2026-11-27-windows/toolchain-versions.log`](../evidence/2026-11-27-windows/toolchain-versions.log).

## 1. Pinned in the tree

| Component | Pin | Where the pin lives | What notices a drift |
|---|---|---|---|
| OpenFX API | tag `OFX_Release_1.5.1` | `GIT_TAG` in all five `templates/*/CMakeLists.txt`; `-DOFX_SOURCE_DIR` points at an existing checkout instead | configure prints the commit it read and writes `openfx-sdk-commit.txt`; `-DOFXPLUGIN_REQUIRE_SDK_COMMIT=ON` stops when no commit can be read ([`cmake-and-linking.md`](cmake-and-linking.md), "A tag is a name") |
| CMake | 3.21 minimum | `cmake_minimum_required(VERSION 3.21)`, line 1 of all five | older CMake refuses the configure |
| ONNX Runtime, what CI downloads | 1.32.0 | `inputs.version.default` in [`.github/actions/onnx-runtime/action.yml`](../.github/actions/onnx-runtime/action.yml) | `tools/check-docs.py` fails every other version in a release-layout path |
| ONNX Runtime, what a bundle ships | 1.20.1 for the CUDA 12 pairing | chosen by `ONNXRUNTIME_ROOT` at configure; the pairing decision is [`packaging-and-install.md`](packaging-and-install.md) §2 | `tools/check-runtime.py` reads `ORT_API_VERSION` and the provider's imports before a render |
| MSVC toolset for `.cu` | 14.44 | `MSVC_PIN` in `templates/minimal-cuda-ofx/build.bat`, passed to `vcvars64 -vcvars_ver` | a Visual Studio generator newer than CUDA 13 accepts stops the configure with the reason ([`cmake-and-linking.md`](cmake-and-linking.md) §5) |
| GitHub Actions | commit SHAs, no moving tags | `.github/workflows/*.yml`, `.github/actions/*/action.yml` | `tools/check-workflows.py` |
| Python for the tools' self-tests | no pin; `.venv/` is local | `tests/`, `tools/` | `python3 tools/probe-env.py` reports what it found |

Two of those are version pins in the ordinary sense (OpenFX, ONNX Runtime); the
CMake line is a floor, and the MSVC entry is a ceiling - CUDA 13's
`host_config.h` rejects 19.50, so the Windows CUDA build has to ask for 14.44 by
name instead of taking the newest toolset in the installation.

## 2. Supplied by the machine

Measured here on 2026-11-27, with the command that reads each one (**[V]**):

| Component | Value here | Read with |
|---|---|---|
| CMake | 4.4.4 | `cmake --version` |
| Ninja | 1.10.2 (Kitware jobserver build) | `ninja --version` |
| MSVC toolsets installed | 14.44.35207 and 14.50.35717 (VS 18 BuildTools), 14.29.30133 (VS 2019) | `vswhere -property installationPath`, then `VC/Tools/MSVC` |
| CUDA toolkit | 13.0.48 (`nvcc` V13.0.48) | `nvcc --version` |
| CUDA driver / UMD | 13.5 (13050) | the GPU harness's `runtime :` line |
| Vulkan headers | 1.4.369 (`VK_HEADER_VERSION 369`, API 1.4) | `grep VK_HEADER_VERSION .validation/vk/sdk/include/vulkan/vulkan_core.h` |
| glslang | 16.6.0 | `glslang --version` |
| Python (`.venv`) | 3.10.6, numpy 2.2.6, onnx 1.23.2, onnxruntime 1.23.2 | `python --version`, `pip list` |
| ONNX Runtime roots used | 1.20.1 (`ORT_API_VERSION 20`) and 1.32.0 (32) | `python tools/check-runtime.py <root>` |

The Vulkan headers and `glslang` are not pinned by this repository: the template
takes `-DVULKAN_INCLUDE_DIR` and `-DVULKAN_SHADER_COMPILER` because
`find_package(Vulkan)` only finds a LunarG install, and what this machine has is
the Vulkan-Headers tree plus glslang's release zip
([`cmake-and-linking.md`](cmake-and-linking.md), "A shader compiler is a build
dependency"). The CUDA toolkit is likewise not pinned - `build.bat` finds it
through `vswhere` and the canonical NVIDIA paths - so a build is reproducible
against a commit only together with the toolkit version it names at configure.

The Python `onnxruntime` wheel in `.venv` (1.23.2) is a **tool** dependency: it
runs the model canary and the fixture checks. It says nothing about the two
packages a bundle actually links, and the two numbers should not be quoted as
one.

## 3. Observed in a run

These are properties of one machine, not of the code, and they belong in a bug
report rather than in a build instruction (**[V]**, same log):

- GPU: NVIDIA GeForce RTX 3060, compute capability 8.6, 28 SMs, 12287 MiB.
- Vulkan device: `Vulkan 1.4.357` - the driver's API version, twelve patch
  revisions behind the headers the module was compiled against. That is legal:
  a driver negotiates the API version, not the header's patch revision, which
  is why the header count is not a requirement for a bundle.
- OpenCL: `OpenCL 3.0 CUDA` on the NVIDIA platform, through the host's loader;
  no OpenCL SDK is installed or needed.
- ONNX Runtime 1.20.1's CUDA provider imports cuBLAS/cuBLASLt 12, cuFFT 11, the
  CUDA 12 runtime and cuDNN 9; the 1.32.0 one imports cuBLASLt/cuBLAS 13 and
  arrives as a plugin EP with `CreateEpFactories`/`ReleaseEpFactory`. That pair
  of import lists is what [`packaging-and-install.md`](packaging-and-install.md)
  §2 turns into "which host will load this bundle".

## 4. How to re-read them

```bash
python3 tools/probe-env.py                    # toolchain, GPU, host apps: one report
python3 tools/check-runtime.py <ort-root>     # ORT_API_VERSION and provider exports
cmake --version; ninja --version; nvcc --version
python3 -m pip list                           # inside .venv, the tool side
git -C .ofx-ref describe --tags --exact-match # the SDK pin, as a name
git -C .ofx-ref rev-parse HEAD                # and as a commit
```

The suites print the rest as they run: the CUDA harness its `device 0:` and
`runtime :` lines, `vulkan_suite` its `device:` line, `opencl_suite` its
`platform:`/`device :` pair. Those lines are the reason
[`testing-layers.md`](testing-layers.md) §13 can quote a run at all.

## 5. What these numbers do not cover

- **One machine, one driver, one toolkit.** The Linux numbers in
  [`cmake-and-linking.md`](cmake-and-linking.md) (GCC 13.3, CMake 3.31.6) come
  from a different host; nothing here was cross-checked against them.
- **One GPU.** No second card, no vendor other than NVIDIA, no software
  rasteriser, and no macOS at all.
- **No host application version is pinned**, because none is linked: Resolve
  Studio 21.2 is the host the CUDA-12 decision was made against
  ([`packaging-and-install.md`](packaging-and-install.md) §2), Nuke 17.1 and
  SCRATCH are documented from their release notes only.
- **The pins are not a lockfile.** OpenFX is fetched at a tag, CUDA and Vulkan
  are whatever the machine has, and the SDK commit is *reported*, not enforced
  unless `OFXPLUGIN_REQUIRE_SDK_COMMIT=ON` is passed. A reproducible build is
  this document plus the log it cites, not the tree alone.

## Provenance labels

- **[V]** verified in this repository: the command ran here and its output is in
  the log this document links.
- **[I]** inferred from a header, a document or a specification, not
  demonstrated by a run here.
- **[B]** external evidence: a run or a report from another machine or project.

The definitions and the grading rules are [`cmake-and-linking.md`](cmake-and-linking.md)'s.
