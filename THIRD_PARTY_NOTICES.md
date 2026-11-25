# Third-party notices

This repository's own code is BSD-3-Clause ([`LICENSE`](LICENSE)), and every
source file says so in its first line: `// SPDX-License-Identifier: BSD-3-Clause`.

It also *uses* other people's code. Nothing below is vendored into this tree:
each item is fetched by a build, linked at run time, or downloaded by an operator
to run a local check. The licence belongs to its author, and the obligation (if
any) belongs to whoever redistributes a binary.

| Component | Licence | How it enters | Redistribution obligation |
|---|---|---|---|
| **OpenFX SDK** 1.5.1 (`ofxCore.h`, `ofxImageEffect.h`, `Support/`) | BSD-3-Clause | `FetchContent` on tag `OFX_Release_1.5.1`, shallow; or `OFX_SOURCE_DIR` for an offline build. Headers are compiled into the `.ofx`; no SDK source is redistributed | An installed `.ofx` is a binary redistribution of SDK header material: keep the OpenFX copyright notice and disclaimer with it |
| **Expat** 2.6.4 | MIT | `FetchContent` in [`testkit/official-host`](testkit/official-host/CMakeLists.txt), static, to build the official OFX host's XML cache | None for this repository — it is a build-time tool, never shipped in a bundle |
| **ONNX Runtime** | MIT | [`templates/onnx-inference-ofx`](templates/onnx-inference-ofx/) links and loads it when `ONNXRUNTIME_ROOT` is provided | **Yes, when you ship it.** A bundle carrying `onnxruntime*.dll` / `libonnxruntime*.so` must carry ONNX Runtime's MIT notice; [`references/packaging-and-install.md`](references/packaging-and-install.md) §1 is the copy step |
| **Natron** 2.5.0 (portable) | GPL-2.0 | Downloaded by the operator into `.validation/` (gitignored) to run Tier 2 of [`testkit/verify-bundle.bat`](testkit/verify-bundle.bat) | None: it is a *host* used to test this code, never linked, bundled or redistributed. A GPL host and a BSD plug-in loaded into it are separate works; nothing here copies Natron code |
| **CUDA Toolkit** headers, `cudart`, cuBLAS/cuBLASLt, cuFFT and **cuDNN 9** | NVIDIA licence: CUDA Toolkit EULA plus the cuDNN SLA, both linked in [`references/packaging-and-install.md`](references/packaging-and-install.md) §2 | Headers and `nvcc` from the user's toolkit; the runtime libraries from NVIDIA's redistributables, copied **into the bundle** when the inference template is configured with `-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` (16 files, 1540 MB on the 1.32.0 build) | **Yes, when you ship that bundle.** NVIDIA's distribution grant covers the redistributable runtime files inside an application that has material additional functionality, and requires terms consistent with its EULA; the build copies no licence text, so add both texts yourself. Without the option - or with `-DOFXPLUGIN_CUDA_FROM_HOST=ON` - the `.ofx` carries only ONNX Runtime's provider and resolves the rest from the host or the driver |

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

## If you build on this

Copying a template into a product is redistribution of that template's source:
keep [`LICENSE`](LICENSE) and the OpenFX notice, and add your own terms on top.
The BSD-3 grant here does not extend to any third-party component in the table
above, and it grants no trademark rights in OpenFX, NVIDIA, Blackmagic Design,
Natron or ONNX Runtime.
