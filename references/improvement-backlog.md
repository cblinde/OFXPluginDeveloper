# Improvement backlog

Improvements that are worth doing, known to be worth doing, and not done yet -
the list that keeps them from disappearing when a session ends in the middle of
something else. Every row names the document that would own the result, because
a backlog entry that lands nowhere is a wish, not a plan.

## 1. How this list works

- **A row is a candidate, not a claim.** Nothing here describes how the code
  behaves; the documents it points at do that. Where a row quotes a number, the
  number comes from the evidence log beside it and keeps its label.
- **Every row names its owner.** When the work is done, the evidence goes into
  that document (and `testing-layers.md` §13 if it was a run) and the
  row is deleted. The backlog must not become a second source of truth: a fact
  that lives only here is a fact nobody will find.
- **Blocked is a status, not a reason to drop the row.** Three of these need
  hardware this machine does not have. They stay until someone with that machine
  runs them, because "unmeasured" is the honest state and it is easy to lose.
- **Order is by what closes the most, not by size.** The cheap ones at the top
  turn an inference into evidence; the expensive ones need a second machine.

## 2. Open items

| # | Improvement | Why it matters | Owner | Cost | Status |
|---|---|---|---|---|---|
| 10 | Measure on a second GPU and a second driver | every CUDA number in this repository comes from one RTX 3060 with one driver; `EXHAUSTIVE`'s plan choice is the place where that matters | `onnx-and-tensorrt.md` §5, `AGENTS.md` rule 3 | another GPU | blocked: no second GPU here |
| 13 | Find what makes the plug-in abort where the probe does not | the probe now also runs under the plug-in's loading conditions: `--private-core onnxruntime-ofx.dll` and `--preload` of cudart, cuBLAS, cuBLASLt, cuFFT and cuDNN, alone and together, and every combination plus each stage alone (`--stop 2`, `--stop 3`, `--stop 6`) still reaches `all steps completed` - so neither the renamed core nor the preload order is the trigger | `templates/onnx-inference-ofx/src/ModelRunner.cpp`, `onnx-and-tensorrt.md` §7 | put the V2 append behind an environment variable in `AppendCudaProvider`, run `test.bat Release` with it set, and bisect inside the plug-in - which of its steps reaches the abort | open: the probe cleared the api, the rename and the preload 2026-11-26 (`evidence/2026-11-26-windows/ep-register-probe.log`) |
| 19 | Render with the provider-only bundle inside Resolve | the provider-only bundle (`OFXPLUGIN_CUDA_FROM_HOST=ON`) and the collision guard now rest on a host that owns the stack, but not on a render inside it: the bundle has never been installed in Resolve and asked to use Resolve's own cuDNN | `packaging-and-install.md` §2, `host-resolve.md` | a Resolve project, the bundle in its OFX folder, one render | ready: 2026-11-26 measured that `Resolve.exe` loads seven CUDA DLLs of its own before any plug-in runs (`evidence/2026-11-26-windows/resolve-modules.txt`), so the host half is settled; what is missing is the render |
| 20 | Run the suites on a Windows runner | neither workflow has a Windows job - the `linux` one added on 2026-11-26 runs `ubuntu-latest` only - so every Windows result here was run by hand and the Windows-only paths have no gate at all: the MSVC generator fallback, `LoadLibraryExW` in the contract, the `dumpbin` artifact check. A Windows machine already produces local runs through the `.bat` runners | `.github/workflows/` | a `windows-latest` job running `build.bat`, both `.bat` suites and `CPU_ONLY=1`; no Valgrind (it is Linux-only), clang-cl ASan if the flags are wanted | ready: `.github/workflows/windows.yml` added 2026-11-26 - CPU-only builds of all three templates, both `.bat` suites, the ONNX CPU session, the negative controls and the tool self-checks, then a step that declares the GPU rows not run, because windows-latest has no device; the first CI run has not happened |
| 24 | Build the Resolve regression suite | the framework (matrix of projects, CPU/GPU comparison, JSON/HTML/JUnit) is engineering on a foundation that either renders or does not; the API question is answered - it lists and applies installed OFX plug-ins (2026-11-25) - so it can start from a working sequence | `references/resolve-automation.md` | the POC plus a week | ready: row 23 closed 2026-11-25 - host path, pixel path and kill path all measured |
| 35 | Template: a Vulkan compute effect | Vulkan is named nowhere in this tree, and it is the one compute API that reaches NVIDIA, AMD and Intel on Windows and Linux from the driver alone, with no vendor SDK to bundle; Nuke hands CPU memory only, so the shape is upload, dispatch, read back | `cmake-and-linking.md` | high: instance and queue setup, memory types, layout transitions, a SPIR-V build step | open |
| 36 | Load an ahead-of-time artifact from C++ instead of shipping a runtime | the compile half is measured: `tools/aot-compile-model.sh` exports the committed model to MLIR, builds a 13.8 KB `llvm-cpu` artifact that reproduces the golden within 5.96e-08, and a 15.1 KB Vulkan artifact holding SPIR-V (`evidence/2026-11-26-linux/aot-spike.log`, `ai-architectures-and-runtimes.md` §2). What is unproven is the part a plug-in actually needs - the IREE runtime C API linked into the `.ofx`, and a Vulkan artifact executed on a device | `ai-architectures-and-runtimes.md` | the second half of the spike: a C++ load, then a render on a Vulkan device | open: the compile half is done; the run half needs a **real** Vulkan device, because IREE's Vulkan HAL creates an instance against Mesa's software driver (lavapipe 24.0.5) but does not enumerate it - `--dump_devices=vulkan` lists nothing - so a CPU fallback is not a way to close this |
| 44 | Template: a Metal image effect for Apple hosts | OFX 1.5 carries Metal buffer and texture handover, and every GPU path in this repository is Windows or Linux; a Metal template covers Resolve and Nuke on macOS, which is the part of the host list nothing here can reach | `cmake-and-linking.md` | a Metal build and a host run | blocked: no Apple machine here |
| 45 | Measure an 8-bit kernel | the 16F half of row 21 is closed: `GainKernelRGBA16FHalf4` exists, needs 4-byte rows instead of 16, and its arithmetic matches a bit-identical reference over 162 shapes (`evidence/2026-11-26-linux/half-kernel.log`, `performance-model.md` section 3.6). The byte path is a different question: OFX RGBA8 arrives as 4-byte pixels that a plug-in either leaves alone or unpacks, so a gain has to round to the byte grid - and what a host expects at that rounding is the part nobody has measured | `colour-and-alpha.md`, `performance-model.md` | an unpack/repack kernel plus harness cases at 8 bits, then the same shapes | open: 16F done 2026-11-26, the byte path is what remains |

- **A bug with a reproduction.** That is a fix, not a candidate: it goes into the
  code and its evidence into the owning document in the same change.
- **A verification gap that a document already states.** "No second GPU" is
  written into §5 and into `AGENTS.md` rule 3, so it is visible without this list;
  it is repeated here only because a row is what makes someone schedule it.
- **A preference.** Two ways of writing the same CMake is not an improvement, and
  a row that argues about style crowds out the ones that need a machine.

## Provenance labels

**[V]** verified here, **[I]** inferred, **[B]** external evidence; definitions in
`cmake-and-linking.md`. Nothing in this file carries a label of its own: the rows
point at documents that do, and a row is a candidate rather than a statement about
behaviour.
