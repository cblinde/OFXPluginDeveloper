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
| 5 | Copy the licence texts into the bundle | the build copies DLLs and `.so` files and no `LICENSE`, `EULA` or `THIRD_PARTY` text; §2 says what a shipper owes NVIDIA, and the build could do half of it | `packaging-and-install.md` §2 | one `POST_BUILD` copy of two files, plus a version line in the log | open |
| 6 | Reconcile the two bundle-size figures | `packaging-and-install.md` §2 says 16 files, 1540 MB for the 1.32.0/CUDA 13 pair; `onnx-and-tensorrt.md` §4 budgets 2.4-2.7 GB for cuDNN 9 + cuBLAS 12 + cuFFT. Both are true of *their* runtime pair, and a reader cannot tell which applies to the bundle in front of them | `packaging-and-install.md` §2, `onnx-and-tensorrt.md` §4 | state both pairs side by side, or one table keyed by runtime | open |
| 7 | Set `gpu_mem_limit` and `arena_extend_strategy` | the arena never returns memory and doubles as it grows, once per instance; the template sets neither, so an instance left open holds the peak of every shape it has seen | `onnx-and-tensorrt.md` §5, §3 | two keys and a measured peak per instance | open |
| 8 | Compare the cached ONNX Runtime paths case-insensitively on Windows | the containment test is now correct, but `.../ORT` and `.../ort` count as different roots and trigger a needless re-search; harmless, one line, needs a test | `templates/onnx-inference-ofx/CMakeLists.txt` | one `string(TOLOWER)` pair and a build tree | open |
| 9 | Prove the `CPU_ONLY (no nvcc)` headline branch | the summary has a branch for a machine without a CUDA toolkit; every run here has `nvcc` on `PATH`, so the branch is written and never executed | `testing-layers.md` §13 | a machine or container without the toolkit | blocked: no such machine here |
| 10 | Measure on a second GPU and a second driver | every CUDA number in this repository comes from one RTX 3060 with one driver; `EXHAUSTIVE`'s plan choice is the place where that matters | `onnx-and-tensorrt.md` §5, `AGENTS.md` rule 3 | another GPU | blocked: no second GPU here |
| 11 | Give the router room again | `AGENTS.md` sits at its 60-line cap: the row pointing at this list only fit because two trailing lines were merged, so the next entry forces another merge - and the read-order table is the part that keeps growing | `AGENTS.md` | group the read-order rows by task family, or move the commands block into a reference | open |
| 12 | Count the CUDA files once | `OFXPLUGIN_CUDA_RUNTIME_DIRS` naming a directory the template already searches made the configure message say `26 file(s), 3720 MB` while the bundle itself had 16 CUDA files: the copy list keeps duplicate paths, and `copy_if_different` hides it while the reported size does not | `templates/onnx-inference-ofx/CMakeLists.txt` | de-duplicate `_cuda_files` before counting, then re-read both bundle sizes | open |
| 13 | Move to `RegisterExecutionProviderLibrary` + `GetEpDevices` + `AppendExecutionProvider_V2` | attempted 2026-11-25 and reverted, and the measurement is the reason: 1.20.1 cannot follow at all (its header has none of the three, `ORT_API_VERSION` 20, and its provider exports only `OrtGetApiBase` - no `CreateEpFactories`), so the code would need `#if ORT_API_VERSION >= 22`; on 1.32 the provider *does* export `CreateEpFactories` and `ReleaseEpFactory`, but registering it from inside the plug-in aborted the process with `0xC0000409` where the legacy append reports `all nodes on CUDA` - so the cause is not found yet and the path cannot ship as it stands | `templates/onnx-inference-ofx/src/ModelRunner.cpp`, `onnx-and-tensorrt.md` §5 | isolate with a standalone probe outside the plug-in, then decide whether the bundle needs the standalone `onnxruntime-ep-cuda*` plugin package rather than the provider from the GPU package | open |
| 14 | Run inside a real host with a CUDA stack loaded | the provider-only bundle (`OFXPLUGIN_CUDA_FROM_HOST=ON`) and the collision guard are argued from import resolution and one fake-host probe, not from a host that owns `cudnn64_9.dll` | `packaging-and-install.md` §2, `host-resolve.md` | a host that ships CUDA | blocked: no such host here |

## 3. What does not belong here

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
