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
| 1 | Run the same CUDA session twice and diff the outputs | run-to-run repeatability is asserted nowhere in this repository; it is the one `[I]` in §5 that a script could settle in minutes | `onnx-and-tensorrt.md` §5 | one session, two runs, one diff | open |
| 2 | Measure a model larger than 64 channels at 1920x1080 | the `1e-4` floor is stated for one shape; §5 says a larger model does not inherit the `1e-7` and cannot show where it moves | `onnx-and-tensorrt.md` §5 | a model file and one run per provider | open |
| 3 | Separate `HEURISTIC` from `EXHAUSTIVE` in wall-clock time | both created the session in 2.8-3.2 ms because cuDNN's on-disk `ComputeCache` already held the plans; the argument for `HEURISTIC` rests on the uncached first run, which was never timed here | `onnx-and-tensorrt.md` §5 | delete the cache, time the first session, repeat | open |
| 4 | Find a model whose plan wants the large workspace | `cudnn_conv_use_max_workspace=0` changed nothing measurable on these shapes, so its benefit is a bound on the worst case with no case to show | `onnx-and-tensorrt.md` §5 | model search, then the same memory readout | open |
| 7 | Set `gpu_mem_limit` and `arena_extend_strategy` | the arena never returns memory and doubles as it grows, once per instance; the template sets neither, so an instance left open holds the peak of every shape it has seen | `onnx-and-tensorrt.md` §5, §3 | two keys and a measured peak per instance | open |
| 9 | Prove the `CPU_ONLY (no nvcc)` headline branch | the summary has a branch for a machine without a CUDA toolkit; every earlier run had `nvcc` on `PATH`, so the branch was written and never executed | `testing-layers.md` §13 | one `./run-tests.sh` without `CPU_ONLY`, on the 2026-11-26 container, which has no `nvcc` | open: closable here - the container that produced `evidence/2026-11-26-linux/` is the machine this row was blocked on; run the gate without `CPU_ONLY` and the headline must name that reason |
| 10 | Measure on a second GPU and a second driver | every CUDA number in this repository comes from one RTX 3060 with one driver; `EXHAUSTIVE`'s plan choice is the place where that matters | `onnx-and-tensorrt.md` §5, `AGENTS.md` rule 3 | another GPU | blocked: no second GPU here |
| 13 | Move to `RegisterExecutionProviderLibrary` + `GetEpDevices` + `AppendExecutionProvider_V2` | attempted 2026-11-25 and reverted, and the measurement is the reason: 1.20.1 cannot follow at all (its header has none of the three, `ORT_API_VERSION` 20, and its provider exports only `OrtGetApiBase` - no `CreateEpFactories`), so the code would need `#if ORT_API_VERSION >= 22`; on 1.32 the provider *does* export `CreateEpFactories` and `ReleaseEpFactory`, but registering it from inside the plug-in aborted the process with `0xC0000409` where the legacy append reports `all nodes on CUDA`. `tools/check-runtime.py` then removed the first hypothesis: the plugin provider does **not** import the core library - it receives the api pointer in `CreateEpFactories` - so the private `onnxruntime-ofx.dll` name is not what breaks it. It also imports neither cuDNN, cuFFT nor cudart, so the plugin resolves its CUDA stack at run time, which is the next thing to look at | `templates/onnx-inference-ofx/src/ModelRunner.cpp`, `onnx-and-tensorrt.md` §7 | a standalone probe outside the plug-in that registers the library and calls `GetEpDevices`, then bisect: registration alone, device list, append | open |
| 14 | Run `tools/check-runtime.py` against a real Linux `.so` | the ELF export/import parser was pointed at a real `libonnxruntime.so` on 2026-11-26 and classified it as the core library with no provider (`evidence/2026-11-26-linux/check-runtime.log`), so the export half has run on a real ELF; still unexercised are a real provider `.so` and the import table, and Mach-O is not parsed at all, so a macOS bundle would report its provider as unreadable | `tools/check-runtime.py` | the CI job already downloads `libonnxruntime.so`; pointing the tool at `$HOME/ort` there closes the export half, and a CUDA `.so` the other | open |
| 19 | Run inside a real host with a CUDA stack loaded | the provider-only bundle (`OFXPLUGIN_CUDA_FROM_HOST=ON`) and the collision guard are argued from import resolution and one fake-host probe, not from a host that owns `cudnn64_9.dll` | `packaging-and-install.md` §2, `host-resolve.md` | a host that ships CUDA | blocked: no such host here |
| 20 | Run the suites on a Windows runner | neither workflow has a Windows job - the `linux` one added on 2026-11-26 runs `ubuntu-latest` only - so every Windows result here was run by hand and the Windows-only paths have no gate at all: the MSVC generator fallback, `LoadLibraryExW` in the contract, the `dumpbin` artifact check. A Windows machine already produces local runs through the `.bat` runners | `.github/workflows/` | a `windows-latest` job running `build.bat`, both `.bat` suites and `CPU_ONLY=1`; no Valgrind (it is Linux-only), clang-cl ASan if the flags are wanted | open |
| 21 | Measure a second shape and format | every CUDA figure here is 1920×1080 RGBA32F on one RTX 3060; the kernel is bandwidth-bound, so an 8-bit 4-channel frame, an odd width and a padded pitch all move the per-pixel cost and nothing says by how much | `performance-model.md` §4 | the same harness at 1919×1081, at RGBA16F and at 1×1, plus one host render with the frame copy included | open |
| 22 | Run the acceptance script in Resolve Studio | `testkit/verify-bundle.bat` proves the bundle loads and renders in one headless host; Resolve is the host this is most likely to ship into, and its scanline padding, its tiling-off shape and its crash blacklist are documented from other people's evidence, not from a run here | `host-resolve.md`, `testkit/` | a Resolve Studio install and the same script pointed at it: one still frame, one session with GPU on and one on Auto | open |
| 24 | Build the Resolve regression suite | the framework (matrix of projects, CPU/GPU comparison, JSON/HTML/JUnit) is engineering on a foundation that either renders or does not; the API question is answered - it lists and applies installed OFX plug-ins (2026-11-25) - so it can start from a working sequence | `references/resolve-automation.md` | the POC plus a week | ready: row 23 closed 2026-11-25 - host path, pixel path and kill path all measured |
| 25 | Install valgrind and cppcheck in the Linux CI job | both checks are skipped wherever they were run by hand - memcheck for want of `libc6-dbg` at the system path, cppcheck because 2.13.0 looks for its `cfg` in a compiled-in directory that needs root - and a runner has root, so one `apt-get install` turns two "skipped, not clean" lines into real results | `testing-layers.md` §13, `checklists.md` §6 | one step in `.github/workflows/linux.yml` | open |
| 26 | Run ASan+UBSan and TSan over all three templates in CI | the TSan job added on 2026-11-26 covers `minimal-cuda-ofx` only, while the inference contract holds four instances and three concurrent renders - where a race in the session code would live - and ASan+UBSan is not in CI at all although it passed 136 checks three times on this container | `.github/workflows/linux.yml`, `testing-layers.md` §13 | two more steps, the shape of the TSan one | open |
| 27 | Call `scripts/check-artifact.sh` from the gate | the POSIX twin of the Windows artifact check just gained the strong-export fix and its executable bit, and nothing invokes it: an unreferenced tool regresses unseen, which is exactly how those two defects survived | `run-tests.sh`, `scripts/` | one stage over the three built `.ofx` | open |
| 28 | Decide the inference bundle's 2.32 glibc floor | the gate enforces 2.32 since 2026-11-26, but nobody decided whether that is acceptable: either lower it (a runtime built in an older container, or a manylinux link) or state it in the README requirements, where somebody asking "does this load on CentOS 7" looks - today it lives only in the checklist and the packaging note | `cmake-and-linking.md` §5, `../README.md` | an hour for the wording, an afternoon for the engineering | open |
| 29 | Put the ONNX Runtime NuGet recipe into the testing reference | the pip wheel ships `libonnxruntime.so` without a single header, the release tarball URL has moved, and the NuGet package carries both halves; this was rediscovered by hand on 2026-11-26 and written into the evidence report rather than into the document a reader follows | `testing-layers.md` §13 | the recipe from `evidence/2026-11-26-linux/report.txt` | open |
| 30 | Stop running the whole gate twice per push | since `.github/workflows/linux.yml` every push runs both `suites` and `docs.yml`'s `templates` job, with two runtime downloads; the shared action removed the drift, the duplicate run is still there and it makes a green tick ambiguous | `.github/workflows/docs.yml` | delete a job, keep its tool self-tests | open |
| 31 | Name the ONNX Runtime version once | 1.32.0 appears in the action default, in the documents and in the evidence folders, so a bump is a search rather than a one-field change | `.github/actions/onnx-runtime/action.yml`, `onnx-and-tensorrt.md` | one variable, then the search that proves it | open |
| 32 | Harden the two workflows | no `concurrency:` group, so a branch's push and its pull request run the same job twice; actions are pinned to tags rather than commit SHAs; and nothing lints the YAML, so a syntax error surfaces only when the job starts | `.github/workflows/` | three small edits and one `actionlint` step | open |
| 33 | Run the five execution-provider rows in one GPU session | rows 1, 2, 3, 4 and 7 need the same machine, the same model and an hour; run apart they move from session to session, together they are one block - repeatability, larger shapes, `HEURISTIC` against a cold cache, the workspace case, the arena keys | `onnx-and-tensorrt.md` §5 | one hour on the RTX 3060 machine | open |

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
