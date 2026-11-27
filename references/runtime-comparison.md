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
| docs: `check-docs.py` + its `--selftest`, `check-workflows.py`, `check-shell.sh` | 5 s | 33 documents, 4 workflows, 26 scripts | 0 problems |
| provenance: `check-provenance.py --self-test`, then the scan | 8 s | 109 files with an SPDX line, every line of 45+ characters compared against the SDK, six allowlisted idioms re-checked against it | 0 problems |
| `minimal-ofx`: build, artifact, contract ×5, install/load | 11 s | 562 | `all Windows checks passed` |
| `minimal-cuda-ofx`: build, artifact, GPU harness, contract ×3, mode gates | 24 s | 486 | `all Windows checks passed` |
| `minimal-opencl-ofx`: build, artifact, contract ×3, `opencl_suite` | 16 s | 354 + 38 | `OK: the OpenCL template passed on this machine` |
| `minimal-vulkan-ofx`: build, path policy, artifact, contract ×6 + device label, install, `vulkan_suite` | 23 s | 656 + 29 + 26 + 94 | `all Windows checks passed` |
| `onnx-inference-ofx`: `build.bat`, then `test/test.bat` | 18 s | 678 | `all checks passed` |
| negative controls: `testkit/run-negative-controls.bat` | 14 s | 223 in 8 runs | 8 caught, every one expected |
| **total** | **119.5 s** (one stopwatch; the rows add up to 119.3 s) | **2,923 passing + 223 deliberately failing** | - |

**Two numbers, and which one is which.** The eight rows are eight stages of one
pass, each stopped on its own: they add up to **119.3 s**. One stopwatch around
the same pass, from the first stage to the last, reads **119.5 s** - the 0.26 s
between them is the eight `date` calls and the shell that starts each stage, so
the sum of the rows is the work and the stopwatch is the sitting. Both are
recorded in
[`../evidence/2026-11-27-windows/suite-timings.log`](../evidence/2026-11-27-windows/suite-timings.log),
which keeps the earlier measurements beside them: a pass taken before the Vulkan
device step grew its label run and with the inference bundle carrying the CUDA
provider (120.9 s of rows, 121.1 s on the stopwatch - the provider copy was
3.4 s of the difference and the label pass 3.1 s), and, before that, eight
separately stopped commands that summed to 116 s with the negative controls in a
second pass, so that total left the minute of orchestration out. The table quotes
the stopwatch number because it is elapsed wall clock for the whole set
(**V**).

A cold checkout is a different number, and the difference is the compiler: this
table was taken after the run that built all five templates, which is the larger
cost and depends on the toolchain rather than on the tests. What the table is
good for is the shape: the five template suites are 92 s of the 119.5, the two
documentation gates 14 s, the negative controls 14 s - and no single suite needs
long enough that a contributor would skip it.

## 2. How the counters add up

**2,923** is 562 + 486 + (354 + 38) + (656 + 29 + 26 + 94) + 678, every one printed
by the suite that ran it as `contract_test: N checks, 0 failed` or
`opencl_suite:` / `vulkan_suite:` / `vulkan_mode_test: N checks, 0 failed`. The
inference row is 148 x 3 + 150 + 84: three host profiles, the step that renders
the model the bundle ships, and the tiled pass. A check is one assertion: one
struct field, one action answer, one pixel comparison, one label.

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
| Vulkan, host-visible coherent buffer | 95.4 / 102.5-106.7 | `memcpy` in, dispatch, `memcpy` out - dispatch 5.7-15.5, copies 88.3-91.2 | 20-frame suite runs and three 300-frame runs in [`vulkan-compute.md`](vulkan-compute.md) §3 |
| Vulkan, device-local + staging | 115.1 / 117.2-117.6 | the same frame over PCIe twice; dispatch 26-28, copies 88.7-91.2 | the same runs |
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
the model, the shape and the provider's plan - so its accuracy rows stay in the
documents that measured them rather than being folded into the table above. The
one thing that belonged here and was missing is the frame time, and the four rows
below are that measurement:

- **CUDA EP against the CPU session, for arithmetic:** `use_tf32=1` differs from
  the CPU session by 4.472e-04 and `use_tf32=0` by 5.364e-07 on a 1080p 64-channel
  convolution, which is why the template states the option rather than inheriting
  it ([`onnx-and-tensorrt.md`](onnx-and-tensorrt.md) §5).
- **OpenVINO against the CPU provider**, medians with confidence intervals:
  [`performance-model.md`](performance-model.md) owns that table.
- **DirectML against the CPU provider** on a 1080p convolution, one machine:
  [`../evidence/2026-11-26-windows/directml-vs-cpu.log`](../evidence/2026-11-26-windows/directml-vs-cpu.log).
### The CUDA-EP frame time, measured instead of named

The wheel in `.venv` has no CUDA provider (its list is `AzureExecutionProvider`,
`CPUExecutionProvider`), so `tools/bench_ep.py` can time the CPU and nothing
else. This measurement links the bundled package's `onnxruntime.lib` and lets the
loader find the `onnxruntime.dll` and `onnxruntime_providers_cuda.dll` beside the
program - ONNX Runtime 1.32.0, the version the action pins and the template
bundles. A frame is `ModelRunner::Run`'s per-frame path and nothing else: wrap the
caller's host buffer as an `OrtValue`, `Run`, `memcpy` the output back, with the
template's six provider keys (`use_tf32=0` among them). Ten warm frames per
round, two rounds per run in opposite order, every sample in the log (**[V]**,
[`../evidence/2026-11-27-windows/onnx-ep-frame-timings.log`](../evidence/2026-11-27-windows/onnx-ep-frame-timings.log)):

| Frame (3-channel input, 64-channel 3×3 conv pair) | CPU EP, median of 2 rounds | CUDA EP, median of 2 rounds | CUDA, first frame |
|---|---|---|---|
| the suite's own model, 3×48×64 (9,216 values) | 0.05 ms | 0.12 / 0.10 ms | 137 ms |
| 640×480 (921,600 values) | 460 / 463 ms | 16.6 / 16.5 ms | 200 ms |
| 1920×1080, 1 thread (6.2 M values) | 2,990 / 3,025 ms | 111.7 / 112.4 ms | 410 ms |
| 1920×1080, 8 threads | 992 / 988 ms | 114.4 / 113.9 ms | 377 ms |

**At 1080p and 640×480 the GPU wins by 27x and 27.7x, and at the suite's own
shape it loses.** 112 ms against 2,990 ms is what a host that leaves the template
at its default `1 thread` waits for; at the suite's 9,216 values the CUDA frame
takes 0.10-0.12 ms against the CPU's 0.05,
because the frame is smaller than the launch and the synchronisation that brings
the answer back. That is the same shape of result as the Vulkan section's 95 ms
host round trip, and it is why a GPU path is a claim about a frame size, not about
a provider.

**Threads close the gap to 8.7x and no further.** The same 1080p graph on the CPU
with eight intra-op threads takes 988-992 ms, so the CPU row is a function of the
thread count while the GPU row is not (114.4 against 111.7, inside the
round-to-round spread). The honest statement is therefore two-sided: 27x at the
template's one thread, 8.7x against a CPU that uses eight cores.

**The first frame is not the frame.** 410 ms at 1080p against 112 warm, 137 ms at
9,216 values against 0.10 - kernel loading, once per process, on top of a session
that itself cost 225-260 ms to create here. A host that times its first render
measures that, not the steady state, which is what the two-round layout in the
log exists to keep apart. `onnx-and-tensorrt.md` §5 quotes 451 ms for a first
inference at the same shape - a median of five runs, TF32 on and the large
workspace permitted; this is one frame with the template's keys, so the two
agree in magnitude without being the same measurement.

**Still not measured:** a 1.20.1 CUDA-12 row - the same bench built against that
package crashed inside its CUDA session creation, so that pairing has no frame
time here rather than a guessed one. The gaps this measurement shares with the
rest of the document are the bullets in §6 rather than a second list here: a
second GPU, a pinned host buffer, a multi-pass graph that keeps the frame on the
device. The inference suite itself still asserts outputs, provider labels and
fallbacks, not throughput.

## 6. What none of this proves

- **One GPU, one driver, one toolkit.** RTX 3060, CUDA 13.5 UMD, toolkit 13.0.48,
  desktop Windows. A workstation card with more PCIe lanes moves the copy column,
  and a tiled host removes the round trip entirely - neither was measured
  ([`vulkan-compute.md`](vulkan-compute.md) §9 lists the same gap from its side).
- **Pageable copies only.** Neither path used pinned memory for the timing loop;
  a pinned host buffer is the one change most likely to move the Vulkan copy
  column, and it was not tried.
- **One pointwise pass.** No multi-pass graph, no resident frame, no tile - the
  arrangement where device-local and a persistent buffer win.
- **Wall clock here is a warm checkout.** It says what a contributor waits for
  after the first build, not what CI waits for on a cold runner.
- **A frame time on two graphs.** The §5 table is a convolution pair at three
  sizes; a detector, a decoder or a graph with control flow has no row, and its
  transfer-to-compute ratio is a different number.

## Provenance labels

- **[V]** verified in this repository: the command ran here, and the log is the
  one linked beside it.
- **[I]** inferred from a header or a document, not demonstrated by a run.
- **[B]** external evidence: measured somewhere else.

Definitions: [`cmake-and-linking.md`](cmake-and-linking.md).
