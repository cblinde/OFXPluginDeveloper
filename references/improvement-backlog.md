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
| 13 | Find what makes the plug-in abort where the probe does not | `tools/ep_register_probe.cpp` runs registration, `GetEpDevices`, the V2 append, a session, a run and the unregister against the same 1.32.0 provider, all successful - so the API is not what raised `0xC0000409` in the plug-in; the difference left is the plug-in's own loading: a private `onnxruntime-ofx.dll` and the CUDA groups preloaded before the registration | `templates/onnx-inference-ofx/src/ModelRunner.cpp`, `onnx-and-tensorrt.md` §7 | put the V2 path behind an environment variable, run the suite, then remove one difference at a time - private core name first, preload second, registration name third | ready: the probe answered the API half 2026-11-26 (`evidence/2026-11-26-windows/ep-register-probe.log`) |
| 19 | Render with the provider-only bundle inside Resolve | the provider-only bundle (`OFXPLUGIN_CUDA_FROM_HOST=ON`) and the collision guard now rest on a host that owns the stack, but not on a render inside it: the bundle has never been installed in Resolve and asked to use Resolve's own cuDNN | `packaging-and-install.md` §2, `host-resolve.md` | a Resolve project, the bundle in its OFX folder, one render | ready: 2026-11-26 measured that `Resolve.exe` loads seven CUDA DLLs of its own before any plug-in runs (`evidence/2026-11-26-windows/resolve-modules.txt`), so the host half is settled; what is missing is the render |
| 20 | Run the suites on a Windows runner | neither workflow has a Windows job - the `linux` one added on 2026-11-26 runs `ubuntu-latest` only - so every Windows result here was run by hand and the Windows-only paths have no gate at all: the MSVC generator fallback, `LoadLibraryExW` in the contract, the `dumpbin` artifact check. A Windows machine already produces local runs through the `.bat` runners | `.github/workflows/` | a `windows-latest` job running `build.bat`, both `.bat` suites and `CPU_ONLY=1`; no Valgrind (it is Linux-only), clang-cl ASan if the flags are wanted | ready: `.github/workflows/windows.yml` added 2026-11-26 - CPU-only builds of all three templates, both `.bat` suites, the ONNX CPU session, the negative controls and the tool self-checks, then a step that declares the GPU rows not run, because windows-latest has no device; the first CI run has not happened |
| 21 | Measure a 16F and an 8-bit kernel | shape is settled - 1919×1081, a 64-byte pad and 1×1 all measured 2026-11-26 (`evidence/2026-11-26-windows/kernel-shapes.log`), and the odd width costs nothing because the kernel runs over a pitch - but the template has no kernel for a half or byte frame, so the format half of this row cannot be measured until one exists | `performance-model.md` §3, `templates/minimal-cuda-ofx/src/Kernel.cu` | a `__half` vec4 kernel plus the 8-bit unpack the OFX side would need, then the same harness at both depths | open: needs the kernel first, and `colour-and-alpha.md` would own what a 16F path promises |
| 22 | Run the acceptance script in Resolve Studio | `testkit/verify-bundle.bat` proves the bundle loads and renders in one headless host; Resolve is the host this is most likely to ship into, and its scanline padding, its tiling-off shape and its crash blacklist are documented from other people's evidence, not from a run here | `host-resolve.md`, `testkit/` | a Resolve Studio install and the same script pointed at it: one still frame, one session with GPU on and one on Auto | open |
| 24 | Build the Resolve regression suite | the framework (matrix of projects, CPU/GPU comparison, JSON/HTML/JUnit) is engineering on a foundation that either renders or does not; the API question is answered - it lists and applies installed OFX plug-ins (2026-11-25) - so it can start from a working sequence | `references/resolve-automation.md` | the POC plus a week | ready: row 23 closed 2026-11-25 - host path, pixel path and kill path all measured |
| 25 | Install valgrind and cppcheck in the Linux CI job | both checks are skipped wherever they were run by hand - memcheck for want of `libc6-dbg` at the system path, cppcheck because 2.13.0 looks for its `cfg` in a compiled-in directory that needs root - and a runner has root, so one `apt-get install` turns two "skipped, not clean" lines into real results | `testing-layers.md` §13, `checklists.md` §6 | one step in `.github/workflows/linux.yml` | open |
| 34 | Template: an OpenCL image effect | the OFX GPU path that needs no vendor SDK: `ofxGPURender.h` in this tree carries `kOfxImageEffectPropOpenCLRenderSupported` (images) and `kOfxImageEffectPropOpenCLSupported` (buffers), and OpenCL 3.1 shipped in May 2026 with NVIDIA, AMD and Intel conformant - but AMD's effort is in HIP and NVIDIA's in CUDA, so it is the portable path, not the fast one | `cmake-and-linking.md` | a template, a kernel and a suite; the GPU probe already exists | open |
| 35 | Template: a Vulkan compute effect | Vulkan is named nowhere in this tree, and it is the one compute API that reaches NVIDIA, AMD and Intel on Windows and Linux from the driver alone, with no vendor SDK to bundle; Nuke hands CPU memory only, so the shape is upload, dispatch, read back | `cmake-and-linking.md` | high: instance and queue setup, memory types, layout transitions, a SPIR-V build step | open |
| 36 | Compile a model to SPIR-V instead of shipping a runtime | StableHLO through IREE or TVM compiles one artifact to SPIR-V for Vulkan and to Metal and CUDA from the same source, which is a far smaller deployment than an execution-provider zoo; the trade is an ahead-of-time compile in CI and no provider fallback to hide behind | `ai-architectures-and-runtimes.md` | a spike: export, compile, load from C++ | open |
| 37 | DirectML and WinML as the Windows GPU path without CUDA | ONNX Runtime's DirectML provider is in sustained engineering and new Windows work moved to WinML, while this repository offers CUDA or CPU and nothing for a Windows machine with an AMD or Intel GPU | `onnx-and-tensorrt.md` | one session against a DirectML build, same golden, on the RTX 3060 | open |
| 39 | Measure OpenVINO against the CPU provider here | OpenVINO is Apache-2.0 and this machine is an Intel i7-8700K, so the comparison is runnable; one published detection test reports 1.77 to 3.51 times ONNX Runtime (**B**), and rule 3 says such a number is reproduced here or not written down | `performance-model.md` | an afternoon: install, convert the test model, both providers | open |
| 41 | Prepare for OFX 1.6 | most of 1.6 is merged and unreleased - machine-readable property metadata, C++ bindings, colour-managed parameters, display time offset - and a template written against 1.5.1 will meet it; the colour part matters most here because `ofxColour.h` is already in this tree's header set and every colour parameter in the templates is declared None | `ofx-api-constants.md`, `colour-and-alpha.md` | a spike, then one decision per parameter | open |
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
