#!/usr/bin/env python3
"""Time one model on OpenVINO's CPU plugin and on ONNX Runtime's CPU provider.

    python3 tools/bench_openvino.py model.onnx
    python3 tools/bench_openvino.py model.onnx --size 1920x1080 --iterations 10

Why this exists: a published number claims OpenVINO is 1.77 to 3.51 times ONNX
Runtime on a detection model (**B**), and this repository's rule is that such a
number is reproduced on the machine in front of you or not written down. The
interesting part is not the ratio - it is that OpenVINO compiles the graph ahead
of time and ONNX Runtime does not, so a fair table has to show the compile cost
and the frame cost separately.

Method, so the numbers mean something:

  * the same file, the same generated frame, the same thread count for both;
  * warm-up runs are excluded - they pay for context and kernel setup;
  * median and p90 of the measured runs, never the mean of a cold start;
  * the machine is named in the output, because a CPU number from a two-core
    slice says nothing about the eight-core box it came from.

What it does not prove: anything about a GPU provider, about a real detection
model, or about a host rendering frames while other work runs. Use the
template's own render for correctness - this measures speed only.

Exit status: 0 both runtimes measured, 1 one of them missing or unable to run the
model, 2 bad usage or a missing file.
"""

from __future__ import annotations

import argparse
import platform
import statistics
import sys
import time

import _ort_common  # noqa: F401 - imports numpy and onnxruntime with a clear error
from _ort_common import make_input, ort, parse_size


def percentile(sorted_values, fraction):
    index = min(len(sorted_values) - 1, int(round(fraction * (len(sorted_values) - 1))))
    return sorted_values[index]


def machine():
    """One line that names what the numbers came from."""
    name = platform.machine()
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("model name"):
                    name = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    return f"{name}, {platform.system()} {platform.release()}, {len(os_cpus())} CPU(s)"


def os_cpus():
    try:
        import os

        return os.sched_getaffinity(0)
    except (AttributeError, OSError):
        return [0]


def time_runs(run, iterations, warmup):
    for _ in range(warmup):
        run()
    samples = []
    for _ in range(iterations):
        start = time.perf_counter()
        run()
        samples.append((time.perf_counter() - start) * 1000.0)
    samples.sort()
    return {
        "median_ms": statistics.median(samples),
        "p90_ms": percentile(samples, 0.9),
        "min_ms": samples[0],
        "max_ms": samples[-1],
    }


def bench_ort(path, frame, threads, iterations, warmup):
    options = ort.SessionOptions()
    options.log_severity_level = 3
    if threads:
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
    started = time.perf_counter()
    session = ort.InferenceSession(
        str(path), sess_options=options, providers=["CPUExecutionProvider"]
    )
    setup_ms = (time.perf_counter() - started) * 1000.0
    feed = {session.get_inputs()[0].name: frame}
    row = time_runs(lambda: session.run(None, feed), iterations, warmup)
    # Session creation and graph compilation are both one-off costs; timing one
    # and not the other would flatter whichever side was left out.
    row["setup_ms"] = setup_ms
    row["runtime"] = f"onnxruntime {ort.__version__} CPU"
    return row


def bench_openvino(path, frame, threads, iterations, warmup):
    import openvino as ov

    core = ov.Core()
    started = time.perf_counter()
    model = core.read_model(str(path))
    compiled = core.compile_model(
        model, "CPU", {"INFERENCE_NUM_THREADS": str(threads)} if threads else {}
    )
    row = time_runs(lambda: compiled([frame]), iterations, warmup)
    # Compile is a one-off cost per model and per machine, and it is the real
    # difference between the two: ORT builds a session too, but far less.
    row["compile_ms"] = (time.perf_counter() - started) * 1000.0
    row["runtime"] = f"openvino {ov.__version__} CPU"
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("model")
    parser.add_argument("--size", default="1920x1080", help="WxH of the generated frame")
    parser.add_argument("--channels", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument(
        "--threads",
        type=int,
        default=0,
        help="intra-op threads for both runtimes (default: each runtime's own)",
    )
    args = parser.parse_args()

    width, height = parse_size(args.size)
    frame = make_input((width, height), args.channels)
    print(f"machine : {machine()}")
    print(f"model   : {args.model}")
    print(
        f"frame   : {args.channels}x{height}x{width} float32 "
        f"({frame.nbytes / 1e6:.1f} MB), {args.iterations} runs after {args.warmup} warm-up"
    )
    print()

    rows = []
    try:
        rows.append(("ONNX Runtime", bench_ort(args.model, frame, args.threads, args.iterations, args.warmup)))
    except Exception as exc:  # noqa: BLE001 - the message is the result
        print(f"onnxruntime: {str(exc).strip().splitlines()[0][:160]}", file=sys.stderr)
    try:
        rows.append(("OpenVINO", bench_openvino(args.model, frame, args.threads, args.iterations, args.warmup)))
    except Exception as exc:  # noqa: BLE001 - the message is the result
        print(f"openvino: {str(exc).strip().splitlines()[0][:160]}", file=sys.stderr)

    if not rows:
        print("error: neither runtime ran the model", file=sys.stderr)
        return 1

    print(f"{'runtime':<12} {'median':>9} {'p90':>9} {'min':>9} {'max':>9}   detail")
    for label, row in rows:
        if "compile_ms" in row:
            detail = f"compile {row['compile_ms']:.0f} ms (one-off)"
        else:
            detail = f"session {row['setup_ms']:.0f} ms (one-off)"
        print(
            f"{label:<12} {row['median_ms']:>8.2f}ms {row['p90_ms']:>8.2f}ms "
            f"{row['min_ms']:>8.2f}ms {row['max_ms']:>8.2f}ms   {detail}"
        )
    if len(rows) == 2:
        ratio = rows[1][1]["median_ms"] / rows[0][1]["median_ms"]
        print()
        print(f"OpenVINO / ONNX Runtime on the median frame: {ratio:.2f}x")
    return 0 if len(rows) == 2 else 1


if __name__ == "__main__":
    raise SystemExit(main())
