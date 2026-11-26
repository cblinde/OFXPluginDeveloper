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
- **Labels carry over.** A row that quotes a number keeps that number's label:
  **[V]** verified here, **[I]** inferred, **[B]** external evidence, **[U]** not
  measured - defined in `cmake-and-linking.md`.
- **Order is by what closes the most, not by size.** The cheap ones at the top
  turn an inference into evidence; the expensive ones need a second machine.

## 2. Open items

| # | Improvement | Why it matters | Owner | Cost | Status |
|---|---|---|---|---|---|
| 10 | Measure on a second GPU and a second driver | every CUDA number in this repository comes from one RTX 3060 with one driver; `EXHAUSTIVE`'s plan choice is the place where that matters | `onnx-and-tensorrt.md` §5, `AGENTS.md` rule 3 | another GPU | blocked: no second GPU here |
| 13 | Find what makes the plug-in abort where the probe does not | the V2 append now lives in `AppendCudaProvider` behind `OFXPLUGIN_CUDA_EP_V2`, each call printed and flushed before it runs, and the suite with it set reaches all of them: registration, `GetEpDevices` and the append return in every session of all three host profiles, the labels read `options requested (arena kSameAsRequested), all nodes on CUDA`, and 168+168+168+90 checks pass - **no step aborts**, so the `0xC0000409` is not in the append sequence either; the probe had already cleared the api, the rename and the preload. Swapping the provider DLL in the bundle clears the last two states a registration can meet: a missing provider (`Error 126`) and one whose DllMain fails against the core (`Error 1114`, the cuBLAS-12-against-1.32 mismatch a host is in) - both arrive as a status and a labelled CPU fallback. Two real faults surfaced on the way and are fixed: a second registration in one environment is an error (two identical instances compared CUDA against CPU), and a reloaded plug-in meets `already registered`, now read as success | `templates/onnx-inference-ofx/src/ModelRunner.cpp`, `onnx-and-tensorrt.md` §7 | the process the abort came from, not the plug-in alone: run the switch from Resolve with the 1.20.1 provider-only bundle, whose cuBLAS 12 imports resolve in that host (the 1.32.0 provider imports 13), and see whether the host still reaches it | open: the plug-in's own path cleared 2026-11-26 (`evidence/2026-11-26-windows/onnx-ep-v2-bisect.log`) |
| 19 | Render with the provider-only bundle inside Resolve | the packaging half is now measured in the host: both provider-only bundles hold four files and no cuDNN/cuBLAS/cuFFT/cudart of their own, Resolve loads the 1.20.1 build, applies it and renders a frame, and the host's process keeps its own `cudnn64_9.dll`; the 1.32.0 build is refused because its provider imports cuBLAS 13 against the host's 12 | `packaging-and-install.md` §2, `host-resolve.md` | give the model path a source that needs no parameter setter - read `ONNX_MODEL_PATH` from the environment when `modelPath` is empty (the template already reads `OFXPLUGIN_CUDA_EP_V2` that way), or take a `.onnx` beside the bundle as a sidecar - then `tests/resolve/poc.py --build-project --source ... --effect ...` builds the project over the API, applies the effect and renders one frame; a host whose effect object does expose `GetParam`/`SetParam` would do as well, and no such host is measured here | open, narrowed to the model path. The `.drp` route is closed rather than untried: `ExportProject` on 21.2.0.5 writes an empty `<EffectFiltersBA/>` while the live project holds the effect, in either signature and with or without a save first, so a project file cannot carry `modelPath` at all and an equal-length sentinel patch has nothing to aim at; `SaveProject` is not callable to change the order, and no GUI-saved `.drp` exists on this machine to test one against (`resolve-project-route.log` **[V]**). Fusion: `GetFusionComp()` `None`, `AddTool` refused under three identifiers (`resolve-fusion-path.log`). An exported archive also comes back only through `ImportProject` (`LoadProject(path)` answers `None`), which leaves `"<name>.Cfg"` to be opened by name - the one step nobody has completed. Until the env/sidecar route exists the frame is still a pass-through and the cuDNN claim is unproven (`resolve-provider-only.log`)
| 20 | Run the suites on a Windows runner | until 2026-11-26 no workflow had a Windows job - the `linux` one added the same day runs `ubuntu-latest` only - so every Windows result here was run by hand and the Windows-only paths had no gate at all: the MSVC generator fallback, `LoadLibraryExW` in the contract, the `dumpbin` artifact check. A Windows machine already produces local runs through the `.bat` runners | `.github/workflows/` | a `windows-latest` job running `build.bat`, both `.bat` suites and `CPU_ONLY=1`; no Valgrind (it is Linux-only), clang-cl ASan if the flags are wanted | ready: `.github/workflows/windows.yml` added 2026-11-26 - CPU-only builds of all three templates, both `.bat` suites, the ONNX CPU session, the negative controls and the tool self-checks, then a step that declares the GPU rows not run, because windows-latest has no device; the first CI run has not happened |
| 45 | Run the OpenCL template inside a real host | everything measured about it is device level: `minimal-opencl-ofx` passes its contract (118 x 3 profiles) and 38 suite checks through the fake host on an RTX 3060 (2026-11-26), and no host has handed the plug-in a queue. Resolve on AMD or Intel is the reachable target; Nuke and SCRATCH expose no OFX GPU buffer path at all | `testing-layers.md` §13, `evidence/2026-11-26-windows/opencl-suite.log` | one Resolve session with the effect applied | open: device level only, and `windows.yml` builds the template without claiming a device |
| 46 | Harden the inference template against real models | one 611-byte model is all it has seen: repeatability of the CUDA EP across runs, a model close to the VRAM limit, the memory-arena strategy and registering the provider outside the template's own session are measured once each or not at all. `bench_ep.py` and `sweep-vram.sh` exist for the measurement and run in no workflow | `onnx-and-tensorrt.md` §5, `performance-model.md` | a second, larger model plus a VRAM-bound case | open: one model, one card (`evidence/2026-11-26-windows/onnx-arena-strategy.log`) |
| 24 | Build the Resolve regression suite | the framework (matrix of projects, CPU/GPU comparison, JSON/HTML/JUnit) is engineering on a foundation that either renders or does not; the API question is answered - it lists and applies installed OFX plug-ins (2026-11-25) - so it can start from a working sequence | `references/resolve-automation.md` | the POC plus a week | ready: row 23 closed 2026-11-25 - host path, pixel path and kill path all measured |
| 35 | Template: a Vulkan compute effect | Vulkan is named nowhere in this tree, and it is the one compute API that reaches NVIDIA, AMD and Intel on Windows and Linux from the driver alone, with no vendor SDK to bundle; Nuke hands CPU memory only, so the shape is upload, dispatch, read back | `cmake-and-linking.md` | high: instance and queue setup, memory types, layout transitions, a SPIR-V build step | open |
| 36 | Load an ahead-of-time artifact from C++ instead of shipping a runtime | the compile half is measured: `tools/aot-compile-model.sh` exports the committed model to MLIR, builds a 13.8 KB `llvm-cpu` artifact that reproduces the golden within 5.96e-08, and a 15.1 KB Vulkan artifact holding SPIR-V (`evidence/2026-11-26-linux/aot-spike.log`, `ai-architectures-and-runtimes.md` §2). What is unproven is the part a plug-in actually needs - the IREE runtime C API linked into the `.ofx`, and a Vulkan artifact executed on a device | `ai-architectures-and-runtimes.md` | the second half of the spike: a C++ load, then a render on a Vulkan device | open: the compile half is done; the run half needs a **real** Vulkan device, because IREE's Vulkan HAL creates an instance against Mesa's software driver (lavapipe 24.0.5) but does not enumerate it - `--dump_devices=vulkan` lists nothing - so a CPU fallback is not a way to close this |
| 44 | Template: a Metal image effect for Apple hosts | OFX 1.5 carries Metal buffer and texture handover, and every GPU path in this repository is Windows or Linux; a Metal template covers Resolve and Nuke on macOS, which is the part of the host list nothing here can reach | `cmake-and-linking.md` | a Metal build and a host run | blocked: no Apple machine here |

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
