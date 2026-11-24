# AGENTS.md

A plan library plus three buildable OpenFX plug-ins. This file routes; the
reasoning lives in the documents it points at. Keep it a router.

## Commands

```bash
./run-tests.sh                              # docs check + every build + every suite
templates/minimal-ofx/build.sh              # one plug-in, from a clean checkout
templates/minimal-ofx/test/run-tests.sh     # artifact inspection + fake-host contract
templates/minimal-*/test/run-tests.bat      # the same checks on Windows
python3 tools/check-docs.py                 # documentation drift, ~1 s
tools/measure-halo.sh model.onnx            # tile overlap per model, writes <model>.halo.json
python3 tools/make-testmodels.py --out build/testmodels   # synthetic 1/3/4-channel and 2x models
python3 tools/model_probe.py model.onnx     # shapes, dynamic dims, size factor, alignment
python3 tools/bench_ep.py model.onnx        # median/p90 per execution provider
tools/sweep-vram.sh model.onnx              # largest tile before ORT OOM (needs a GPU)
```

`./run-tests.sh` needs `ONNXRUNTIME_ROOT` for the inference template (a CI run
without it fails, a local one marks it NOT VERIFIED) and `CPU_ONLY=1` when no
CUDA toolkit is installed. `tools/make-testmodels.py` needs `onnx` to write the
files and `onnxruntime` + `numpy` for its self-test; neither is a dependency.

---

## Read order

| Task | Read, in this order | Skip |
|---|---|---|
| New plug-in | `SKILL.md` §1 and §3 -> `references/cmake-and-linking.md` -> copy `templates/minimal-cuda-ofx/` | host quirks until a host misbehaves |
| Debug a symptom | `references/debugging-and-symptoms.md` -> `references/pattern-index.md` -> `references/cuda-streams-and-memory.md` or `references/pixel-formats-and-pitch.md` | the design docs, until the symptom names a file |
| AI-model decision | `references/ai-architectures-and-runtimes.md` -> `references/onnx-and-tensorrt.md` -> `templates/onnx-inference-ofx/` | CUDA kernel detail |
| Check a `[B]` claim | `references/verification-status.md` -> its `evidence/` link | the design docs, until the evidence contradicts them |
| Host behaviour differs | `references/host-resolve.md` -> `references/host-nuke-scratch-natron.md`, grades A/B/C | anything claiming a host was tested |

---
## Hard rules

1. **Never claim a host render is verified beyond what ran.** `NatronRenderer` has
   rendered this template headless (`testkit/verify-bundle.bat`, 2026-11-24) — that
   is the one host fact marked **[V]**. No Resolve, Nuke or SCRATCH exists here, and
   their claims stay unverified.
2. **Claim nothing a compiler, `nm` or a test did not prove.** `[V]` means the
   command ran in this repository; `[I]` means inferred; `[B]` means external
   evidence. Labels are defined in `references/cmake-and-linking.md`.
3. **Measure before wording a performance claim.** The gain kernel's throughput,
   the CPU loop it replaces and the cost of a frame copy are measured on one
   RTX 3060 (`evidence/2026-11-24-windows/`); nothing is measured on a second GPU,
   inside a host, or with a CUDA execution provider.
4. **Keep the docs passing `python3 tools/check-docs.py`.** It runs in
   `./run-tests.sh` and in CI, and checks links, anchors, count drift, duplicated
   measurements and provenance labels.
5. **Never commit build output** (`build/`, `*.ofx`, `*.ptx`, `.ofx-ref/`).

**Labels:** **[V]** verified here, **[I]** inferred, **[B]** external evidence; definitions in `references/cmake-and-linking.md`.

What actually runs, with counts: `references/testing-layers.md` §13. What a build proves,
and what it cannot: `references/debugging-and-symptoms.md` §5.
