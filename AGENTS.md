# AGENTS.md

A plan library plus three buildable OpenFX plug-ins. This file routes; the
reasoning lives in the documents it points at. Keep it a router.

## Commands

```bash
./run-tests.sh                              # docs check + every build + every suite
templates/minimal-ofx/build.sh              # one plug-in, from a clean checkout
python3 tools/check-docs.py                 # documentation drift, ~1 s
python3 tools/probe-env.py                  # which toolchain this machine has
```

The rest - `check-runtime.py`, `bench_ep.py`, `model_probe.py`,
`make-testmodels.py`, `measure-halo.sh`, `sweep-vram.sh` - each with its
arguments, and the `ONNXRUNTIME_ROOT`, `CPU_ONLY` and `.venv/` rules:
`references/testing-layers.md` §13, "The tools on their own".

---

## Read order

| Task | Read, in this order | Skip |
|---|---|---|
| New plug-in | `python3 tools/probe-env.py` -> `SKILL.md` §1 and §3 -> `references/cmake-and-linking.md` -> copy `templates/minimal-cuda-ofx/` | host quirks until a host misbehaves |
| Debug a symptom | `references/debugging-and-symptoms.md` -> `references/pattern-index.md` -> `references/cuda-streams-and-memory.md` or `references/pixel-formats-and-pitch.md` | the design docs, until the symptom names a file |
| AI-model decision | `references/ai-architectures-and-runtimes.md` -> `references/onnx-and-tensorrt.md` -> `templates/onnx-inference-ofx/` | CUDA kernel detail |
| Check a `[B]` claim | `references/verification-status.md` -> its `evidence/` link | the design docs, until the evidence contradicts them |
| Host behaviour differs | `references/host-resolve.md` -> `references/host-nuke-scratch-natron.md`, grades A/B/C | anything claiming a host was tested |
| Test in Resolve | `references/resolve-automation.md` §14, §15 -> `tests/resolve/README.md` | the host quirks until a run fails |
| Plan an improvement | `references/improvement-backlog.md` -> the document that owns the subject | nothing: a row is a candidate, not a claim |

---
## Hard rules

1. **Never claim a host render is verified beyond what ran.** Resolve Studio 21.2
   ran `tests/resolve/poc.py` three times (all **[V]**): default gain (identical to
   the source, `mae 0.0`), `-DREFERENCE_GAIN_DEFAULT=2.0` (source ×2, `mae 0.0`), and
   the CUDA template at `-DGAIN_DEFAULT=2.0` declaring CUDA for every render; a
   fourth run killed the host with a job in flight. `testkit/verify-bundle.bat`
   then ran two more sessions of the reference bundle through tier 3, one with the
   GPU processing mode on Auto and one on CUDA, and the host's own compute-API line
   is in each report. `NatronRenderer` rendered one 256×256 frame through
   `testkit/`. No tiled host, no second GPU, no third host.
   Every prose claim about a host has to agree with `references/verification-matrix.tsv`.
2. **Claim nothing a compiler, `nm` or a test did not prove.** `[V]` means the
   command ran in this repository; `[I]` means inferred; `[B]` means external
   evidence. Labels are defined in `references/cmake-and-linking.md`.
3. **Measure before wording a performance claim.** The gain kernel's throughput,
   the CPU loop it replaces and the cost of a frame copy are measured on one
   RTX 3060 (`evidence/2026-11-24-windows/`), and the ONNX Runtime CUDA provider is
   measured on the same card (`references/onnx-and-tensorrt.md` §5). Nothing is
   measured on a second GPU, inside a host, or on a vendor other than NVIDIA.
4. **Keep the docs passing `python3 tools/check-docs.py`.** It runs in
   `./run-tests.sh` and in CI, and checks links, anchors, count drift, duplicated
   measurements and provenance labels.
5. **Never commit build output** (`build/`, `*.ofx`, `*.ptx`, `.ofx-ref/`).

**Labels:** **[V]** verified here, **[I]** inferred, **[B]** external evidence; definitions in `references/cmake-and-linking.md`. What actually runs, with counts: `references/testing-layers.md` §13. What a build proves, and what it cannot: `references/debugging-and-symptoms.md` §5.
