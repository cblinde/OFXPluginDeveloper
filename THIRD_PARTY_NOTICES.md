# Third-party notices

This repository's own code is BSD-3-Clause ([`LICENSE`](LICENSE)), and every
source file says so: `SPDX-License-Identifier: BSD-3-Clause` on the first line, or
on the line after a shebang, after `@echo off` in a batch file (a `rem` above it
would be printed on every run) and after `#version` in a GLSL shader. Two things
hold that claim: `tools/check-provenance.py` fails on a tracked source file
without the line, and it fails on a substantive line that also appears verbatim in
the OpenFX SDK. The version scripts (`*.map`) and every workflow carry it too, in a `/* */` or
a `#` comment; `.gitignore` and the data fixtures are the exceptions the tool
names, because a committed `.onnx` or `.bin` cannot hold a comment at all.

It also *uses* other people's code. Nothing below is vendored into this tree:
each item is fetched by a build, linked at run time, or downloaded by an operator
to run a local check. The licence belongs to its author, and the obligation (if
any) belongs to whoever redistributes a binary.

| Component | Licence | How it enters | Redistribution obligation |
|---|---|---|---|
| **OpenFX SDK** 1.5.1 (`ofxCore.h`, `ofxImageEffect.h`, `Support/`) | BSD-3-Clause | `FetchContent` on tag `OFX_Release_1.5.1`, shallow, into `.ofx-ref/`; or `OFX_SOURCE_DIR` for an offline build. Headers and Support sources are compiled into the `.ofx`; no SDK source is redistributed | An installed `.ofx` is a binary redistribution of SDK material: keep the OpenFX copyright notice and disclaimer with it |
| **Expat** 2.6.4 | MIT | `FetchContent` in [`testkit/official-host`](testkit/official-host/CMakeLists.txt), static, to build the official OFX host's XML cache | None for this repository — it is a build-time tool, never shipped in a bundle |
| **ONNX Runtime** (1.20.1 and 1.32.0 measured here) | MIT | [`templates/onnx-inference-ofx`](templates/onnx-inference-ofx/) links and loads it when `ONNXRUNTIME_ROOT` is provided | **Yes, when you ship it.** A bundle carrying `onnxruntime*.dll` / `libonnxruntime*.so` must carry ONNX Runtime's MIT notice. That text is committed as [`templates/onnx-inference-ofx/licenses/LICENSE`](templates/onnx-inference-ofx/licenses/LICENSE), so even a bare runtime tree (only `bin/`, `include/`, `lib/`) ships one; the configure also copies any `LICENSE`/`NOTICE`/`ThirdParty*` text it finds beside the runtime or in the package root above it, logs each one, and **fails** when a build that links ONNX Runtime ends with none ([`references/packaging-and-install.md`](references/packaging-and-install.md) §2) |
| **CUDA Toolkit** headers, `cudart`, cuBLAS/cuBLASLt, cuFFT and **cuDNN 9** | NVIDIA licence: CUDA Toolkit EULA plus the cuDNN SLA, both linked in [`references/packaging-and-install.md`](references/packaging-and-install.md) §2 | Headers and `nvcc` from the user's toolkit; the runtime libraries from NVIDIA's redistributables, copied **into the bundle** when the inference template is configured with `-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` (16 files, 1540 MB on the 1.32.0 build) | **Yes, when you ship that bundle.** NVIDIA's distribution grant covers the redistributable runtime files inside an application that has material additional functionality, and requires terms consistent with its EULA; the build copies any `LICENSE`/`NOTICE`/`ThirdParty`/`EULA` file it finds beside the runtime into `Contents/Resources` and logs each one, warns at configure time when the provider is bundled and none was found, and [`testkit/check-ofx-artifact.bat`](testkit/check-ofx-artifact.bat) refuses a bundle that carries `onnxruntime_providers_cuda` without an `NVIDIA-` text - so the texts still to add by hand are the ones no package ships (NVIDIA's EULA comes with the installer), and they cannot be forgotten quietly. Without the option - or with `-DOFXPLUGIN_CUDA_FROM_HOST=ON` - the `.ofx` carries only ONNX Runtime's provider and resolves the rest from the host or the driver |
| **Khronos Vulkan-Headers** | Apache-2.0 OR MIT, per file (**[B]**: [`LICENSE.md`](https://github.com/KhronosGroup/Vulkan-Headers/blob/main/LICENSE.md), a REUSE index) | Build-time only, and only for `templates/minimal-vulkan-ofx`: `-DVULKAN_INCLUDE_DIR` points at the checkout, because `find_package(Vulkan)` finds a LunarG install and this machine has none. No Vulkan loader or import library is shipped | None beyond keeping the notice when you redistribute a binary built from those headers; the headers are compiled in, the loader is the driver's (`vulkan-1.dll`, loaded at run time) |
| **glslang** 16.6.0 | 3-Clause BSD for glslang proper, Apache-2.0 and others for parts (**[B]**: [`LICENSE.txt`](https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt)) | Build-time only: `-DVULKAN_SHADER_COMPILER`. Compiles `shaders/gain.comp` to SPIR-V, which `cmake/spv-header.cmake` turns into a byte array inside the `.ofx` | None for the compiler (it never ships); the SPIR-V it produces is output, not a copy of glslang |
| **IREE** 2026.4 (`iree-compile`) | Apache-2.0 | Operator-run by [`tools/aot-compile-model.sh`](tools/aot-compile-model.sh) for the ahead-of-time spike; the 13.8 KB and 15.1 KB artifacts are compiler output and are not committed | None: neither the tool nor its artifacts enter a bundle, and `references/ai-architectures-and-runtimes.md` §2 describes the spike rather than shipping it |
| **Mesa lavapipe** 24.0.5 | MIT | Linux validation only: the software Vulkan device IREE's HAL enumerated in that spike, and the reason the spike cannot be closed on this machine | None: a test device, never linked or shipped |
| **Python packages** for the tools — `onnx` 1.23.2 (Apache-2.0), `onnxruntime` 1.20.1/1.32.0 (MIT), `numpy` 2.2.6 (BSD-3-Clause) | as named | `pip install` into `.venv/` by whoever runs `tools/make-testmodels.py`, `model_probe.py`, `bench_ep.py`, the canary; CI installs the same three | None: developer tooling, never in a bundle |
| **GitHub Actions** used by `.github/workflows/` — `actions/*` and `Jimver/cuda-toolkit` | MIT | CI only; every `uses:` is pinned to a 40-character commit by `tools/check-workflows.py` | None for this repository; a fork that reuses the workflows inherits their terms |
| **Natron** 2.5.0 (portable) | GPL-2.0 | Downloaded by the operator into `.validation/` (gitignored) to run Tier 2 of [`testkit/verify-bundle.bat`](testkit/verify-bundle.bat) | None: it is a *host* used to test this code, never linked, bundled or redistributed. A GPL host and a BSD plug-in loaded into it are separate works; nothing here copies Natron code |
| **DaVinci Resolve / Nuke / SCRATCH** | proprietary, not redistributable | Installed by the operator; `tests/resolve/` drives Resolve through its own scripting API | None: no host file is read into a build or shipped |

## What is not here

- No model weights, fonts or tokenizer tables are committed. If you add them, the
  licence text for each belongs with the bundle — that is the F14 finding in
  [`references/field-evidence.md`](references/field-evidence.md) and the reason
  the notices section of `references/packaging-and-install.md` exists.
- No third-party source is pasted into `src/`. The reference projects named in
  [`references/performance-model.md`](references/performance-model.md) are
  read for analysis, including GPL-3.0 ones (`OneGrade`, `rvmofx`), and nothing
  from them is copied: a GPL copy would make the BSD grant in `LICENSE`
  inapplicable to this tree.
- `templates/minimal-cuda-ofx/test/shim/cuda_runtime.h` is this repository's own
  portable stand-in for the CUDA runtime API, not a copy of a CUDA header.

## The copied-code question, measured

"The SDK's examples are in the same tree as this plug-in" is the reason the
question comes up at all — an OpenFX plug-in has to call the same API, so some
identical lines are unavoidable. `tools/check-provenance.py` makes the difference
mechanical: it compares every line of 45 characters or more in this tree's
sources against the fetched SDK and allows only a written-down list of API
idioms, each with its reason (`throwSuiteStatusException`, `addSupportedContext`,
`setRenderWindow`, the `getBounds()` call and its return type). Measured
2026-11-27 (**[V]**, this checkout): 51 matches, six distinct lines, all six on
that list, 0 problems - the number of lines compared moves with every edit to the
tree, so it is printed by the tool and not quoted here. A comment paragraph, a shader or
an algorithm copied from the SDK's examples would not be on the list and would
fail the run.

What it cannot see: a copy that was reworded, a table transcribed by hand, or a
match against a project that is not in `.ofx-ref/` — the GPL projects read for
design live online, and
[`references/reference-projects.md`](references/reference-projects.md) records
what each one contributed.

## If you build on this

Copying a template into a product is redistribution of that template's source:
keep [`LICENSE`](LICENSE) and the OpenFX notice, and add your own terms on top.
The BSD-3 grant here does not extend to any third-party component in the table
above, and it grants no trademark rights in OpenFX, NVIDIA, Blackmagic Design,
Natron or ONNX Runtime.
