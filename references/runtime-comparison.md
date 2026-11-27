# Runtime comparison: what the exhaustive suite costs, and the same frame in four runtimes

Two questions get confused when a test suite grows. *Does it all still pass* is
answered by `./run-tests.sh` and recorded in [`testing-layers.md`](testing-layers.md)
§13. *What that costs, and how the four GPU paths compare on identical work* is a
different measurement, and it had no home: the check counts were in §13, the
per-frame numbers were wherever the argument that needed them lived, and nothing
put them side by side. This document does, on one machine on 2026-11-27
(**[V]**, [`../evidence/2026-11-27-windows/suite-timings.log`](../evidence/2026-11-27-windows/suite-timings.log)).
Where a number is another section's, the row says so and repeats it only to make
the comparison possible.

## 1. The whole suite, timed

Warm build directories - every template had been built already, so the wall clock
below is the checks, not the compiler.

| Suite | Wall clock | Assertions | Verdict |
|---|---|---|---|
| docs: `check-docs.py` + its `--selftest`, `check-workflows.py`, `check-shell.sh` | 5 s | every document (32 on the day of the run, 33 with this one), 4 workflows, 26 scripts | 0 problems |
| provenance: `check-provenance.py --self-test`, then the scan | 7 s | 99 files with an SPDX line, 12,506 long lines against the SDK | 0 problems |
| `minimal-ofx`: build, artifact, contract ×5, install/load | 10 s | 562 | `all Windows checks passed` |
| `minimal-cuda-ofx`: build, artifact, GPU harness, contract ×3, mode gates | 23 s | 486 | `all Windows checks passed` |
| `minimal-opencl-ofx`: build, artifact, contract ×3, `opencl_suite` | 15 s | 354 + 38 | `OK: the OpenCL template passed on this machine` |
| `minimal-vulkan-ofx`: build, artifact, contract ×5, `vulkan_suite` | 18 s | 562 + 26 | `all Windows checks passed` |
| `onnx-inference-ofx`: `build.bat`, then `test/test.bat` | 26 s | 778 | `all checks passed` |
| negative controls: `testkit/run-negative-controls.bat` | 12 s | 223 in 8 runs | 8 caught, every one expected |
| **total** | **116 s** | **2,806 passing + 223 deliberately failing** | - |

A cold checkout is a different number, and the difference is the compiler: this
table was taken after the run that built all five templates, which is the larger
cost and depends on the toolchain rather than on the tests. What the table is
good for is the shape: the five template suites are 92 s of the 116, the two
documentation gates 12 s, the negative controls 12 s - and no single suite needs
long enough that a contributor would skip it.

## 2. How the counters add up

**2,806** is 562 + 486 + (354 + 38) + (562 + 26) + 778, every one printed by the
suite that ran it as `contract_test: N checks, 0 failed` or `opencl_suite:` /
`vulkan_suite: N checks, 0 failed`. A check is one assertion: one struct field,
one action answer, one pixel comparison, one label.

**223** is the negative-control suite, which is not a count of passes: each of its
eight runs drives a deliberately broken plug-in and asserts that the harness
prints a `FAIL` and exits non-zero. Eight runs, eight `caught (exit 1)`, one
intended failure inside each. It exists because a suite that has never been seen
failing is a suite nobody has tested ([`testing-layers.md`](testing-layers.md)
§5.3, "Proof that it can fail").

## 3. The same 1080p gain pass, through each runtime

1920×1080 RGBA32F, one pointwise multiplication, 33.2 MB per buffer. Each row says
what is inside its number, because that is the whole argument.

| Path | ms/frame | What the number contains | Run |
|---|---|---|---|
| CUDA `vec4`, `cudaMalloc` buffers | **0.20 - 0.31** (211-329 GB/s) | kernel only; the frame is already on the device | three runs today: 0.202, 0.210, 0.314 ([`../evidence/2026-11-27-windows/cuda-suite.log`](../evidence/2026-11-27-windows/cuda-suite.log), `suite-timings.log`) |
| CUDA scalar, same buffers | 0.20 - 0.31 | same | same runs |
| CUDA pageable H2D+D2H copy of that frame | 6.5 - 8.1 (8-10 GB/s) | the transfer the kernel row leaves out | same runs |
| CPU loop, in the CUDA harness | 3.26 - 3.92 (12.5-19.3× slower than vec4) | the loop `render()` falls back to | same runs |
| Vulkan, host-visible coherent buffer | 95.4 / 102.5 | `memcpy` in, dispatch, `memcpy` out - dispatch 5.7-7.0, copies 88.3-89.2 | 20-frame suite run (`suite-timings.log`) and the 300-frame run in [`vulkan-compute.md`](vulkan-compute.md) §3 |
| Vulkan, device-local + staging | 115.1 / 117.2 | the same frame over PCIe twice; dispatch 26-28, copies 88.7-89.4 | same two runs |
| CPU loop, in the Vulkan suite | 3.25 | the same fallback, measured by another harness | `suite-timings.log` |
| OpenCL, host queue | not printed | `opencl_suite` has no clock; it asserts pixels and refusals | [`testing-layers.md`](testing-layers.md) §13 |

## 4. Reading that table without fooling yourself

- **The CUDA row is not a transfer-free version of the Vulkan row, it is a
  different question.** 330 GB/s is device-memory bandwidth; PCIe cannot carry
  it. The harness allocates with `cudaMalloc` and times the launch, so the row
  measures the kernel, while the Vulkan rows measure a frame that starts and ends
  in host memory. Putting them in one table is useful only because it shows where
  the time goes, not because 0.21 divides into 102.5.
- **The comparable pair is ~7-8 ms against ~95 ms.** Add CUDA's own transfer to
  its kernel (0.2 + 8.1 at the slow end) and a full CUDA round trip is single-digit
  milliseconds; the Vulkan host-visible path spends 88 ms in CPU `memcpy` through
  a mapped buffer, ~0.37 GB/s per direction, against ~4-5 GB/s per direction for
  `cudaMemcpy`. Both are "zero copies on paper"; one of them moves the frame at a
  tenth of the speed.
- **On a pointwise pass, the device-local arrangement loses** - 115-117 ms against
  95-102 ms - because staging pays the PCIe round trip twice. On the CUDA side the
  same fact appears as the 6.5-8.1 ms copy row: a kernel that reads host-mapped
  memory over PCIe cannot reach the 330 GB/s the device-memory row shows.
- **The CPU fallback beats both GPU paths at this size**, 3.2-3.9 ms against
  95-117 ms for Vulkan. That is the honest result for a one-operation pass with a
  frame-sized round trip, and it is why the Vulkan template's own section says
  device-local earns its place only when the frame stays resident across passes
  ([`vulkan-compute.md`](vulkan-compute.md) §3).
- **The spread is the machine, not the kernel.** Three runs of the same binary on
  the same card gave 0.202, 0.210 and 0.314 ms for `vec4`; each row above names
  the run it came from rather than pretending to one number. Any comparison finer
  than "the shader is a rounding error next to the transfer" does not survive that
  spread.
- **OpenCL is not in the comparison, on purpose.** The template renders through
  the host's queue and its suite asserts 38 checks worth of pixels, refusals and
  lifecycle; it prints no clock, so no throughput is claimed for it. Adding one
  would be a code change with its own evidence, not a table entry.

## 5. Inference runtimes: compared where they were measured

The AI path is a different comparison - an inference session's latency depends on
the model, the shape and the provider's plan - so it stays in the documents that
measured it rather than being folded into the table above:

- **CUDA EP against the CPU session, for arithmetic:** `use_tf32=1` differs from
  the CPU session by 4.472e-04 and `use_tf32=0` by 5.364e-07 on a 1080p 64-channel
  convolution, which is why the template states the option rather than inheriting
  it ([`onnx-and-tensorrt.md`](onnx-and-tensorrt.md) §5).
- **OpenVINO against the CPU provider**, medians with confidence intervals:
  [`performance-model.md`](performance-model.md) owns that table.
- **DirectML against the CPU provider** on a 1080p convolution, one machine:
  [`../evidence/2026-11-26-windows/directml-vs-cpu.log`](../evidence/2026-11-26-windows/directml-vs-cpu.log).
- **Not measured anywhere in this tree: a wall-clock CUDA-EP-against-CPU-EP frame
  time on this machine.** The inference suite asserts outputs, provider labels and
  fallbacks, not throughput. `tools/bench_ep.py` measures whatever runtime the
  installed wheel provides, and the wheel in `.venv` is CPU-only (its provider
  list is `AzureExecutionProvider`, `CPUExecutionProvider`), so a CUDA number has
  to come from the package's own DLLs - which is a run nobody has done yet, and
  is written down here as a gap rather than guessed at.

## 6. What none of this proves

- **One GPU, one driver, one toolkit.** RTX 3060, CUDA 13.5 UMD, toolkit 13.0.48,
  desktop Windows. A workstation card with more PCIe lanes moves the copy column,
  and a tiled host removes the round trip entirely - neither was measured
  ([`vulkan-compute.md`](vulkan-compute.md) §8 lists the same gap from its side).
- **Pageable copies only.** Neither path used pinned memory for the timing loop;
  a pinned host buffer is the one change most likely to move the Vulkan copy
  column, and it was not tried.
- **One pointwise pass.** No multi-pass graph, no resident frame, no tile - the
  arrangement where device-local and a persistent buffer win.
- **Wall clock here is a warm checkout.** It says what a contributor waits for
  after the first build, not what CI waits for on a cold runner.

## Provenance labels

- **[V]** verified in this repository: the command ran here, and the log is the
  one linked beside it.
- **[I]** inferred from a header or a document, not demonstrated by a run.
- **[B]** external evidence: measured somewhere else.

Definitions: [`cmake-and-linking.md`](cmake-and-linking.md).
