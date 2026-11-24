# AGENTS.md

A plan library plus three buildable OpenFX plug-ins. This file routes; the
reasoning lives in the documents it points at. Keep it a router.

## Commands

```bash
./run-tests.sh                              # docs check + every build + every suite
templates/minimal-ofx/build.sh              # one plug-in, from a clean checkout
templates/minimal-ofx/test/run-tests.sh     # artifact inspection + fake-host contract
templates/minimal-cuda-ofx/test/run-tests.bat  # same three checks on Windows
python3 tools/check-docs.py                 # documentation drift, ~1 s
tools/measure-halo.sh model.onnx            # tile overlap per model, writes <model>.halo.json
python3 tools/make-testmodels.py --out build/testmodels   # synthetic 1/3/4-channel and 2x models
python3 tools/model_probe.py model.onnx     # shapes, dynamic dims, size factor, alignment
python3 tools/bench_ep.py model.onnx        # median/p90 per execution provider
tools/sweep-vram.sh model.onnx              # largest tile before ORT OOM (needs a GPU)
```

`./run-tests.sh` needs `ONNXRUNTIME_ROOT` for the inference template, and
`CPU_ONLY=1` when no CUDA toolkit is installed. `tools/make-testmodels.py` needs
`onnx` to write the files and `onnxruntime` + `numpy` for its self-test; neither
is a dependency of the suites.

## Read order

| Task | Read, in this order | Skip |
|---|---|---|
| New plug-in | `SKILL.md` §4 and §6 → `BUILD_PLAYBOOK.md` → `PATTERN_INDEX.md` §1, then copy `templates/minimal-ofx/` | `AI_MODEL_INTEGRATION.md`, performance synthesis |
| Debug a symptom | `PATTERN_INDEX.md` §2 (runtime and compile/link) → `SKILL.md` §7 → `ARCHITECTURE_AND_CUDA_PIPELINE.md` | template sources, until the table names a file |
| AI-model decision | `AI_MODEL_INTEGRATION.md` §1–§2 → `PATTERN_INDEX.md` §1 → `templates/onnx-inference-ofx/` | CUDA kernel detail |
| Host-specific behaviour | `HOST_COMPATIBILITY_MATRIX.md`, whose grades are A/B/C | anything claiming a host was tested |
| Check a [B] claim | `FIELD_TEST_WINDOWS_CUDA.md`: the 2026-11-22 run, its logs, its limits | the design docs, until the evidence contradicts them |

## Hard rules

1. **Never claim a host render is verified.** No Resolve, Nuke, SCRATCH or Natron
   exists here. "Renders in a host" is always unverified.
2. **Claim nothing a compiler, `nm` or a test did not prove.** `[V]` means the
   command ran in this repository; `[I]` means inferred; `[B]` means external
   evidence. Labels are defined in `BUILD_PLAYBOOK.md`.
3. **Measure before wording a performance claim.** Timings (L7) and GPU execution
   are unbuilt; there is no GPU here.
4. **Keep the docs passing `python3 tools/check-docs.py`.** It runs in
   `./run-tests.sh` and in CI, and checks links, anchors, count drift, duplicated
   measurements and provenance labels.
5. **Never commit build output** (`build/`, `*.ofx`, `*.ptx`, `.ofx-ref/`).

Labels: **[V]** verified in this repository, **[I]** inferred, **[B]** external
evidence. The full definitions are in `BUILD_PLAYBOOK.md`.

What actually runs, with counts: `TESTING_STRATEGY.md` §13. What a build proves,
and what it cannot: `BUILD_PLAYBOOK.md` §5.
