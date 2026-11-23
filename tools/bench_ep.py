#!/usr/bin/env python3
"""Time a model on each execution provider this machine can offer.

    python3 tools/bench_ep.py model.onnx
    python3 tools/bench_ep.py model.onnx --provider CUDAExecutionProvider --iterations 50

One row per provider: the median and p90 of the run, frames per second, and the
providers the session actually ended up with — the last column is the honest one,
because a provider that silently falls back still answers `run()` with numbers
that look fine. A provider that is not installed is reported as skipped, not
timed.

`AI_MODEL_INTEGRATION.md` §7 asks for CUDA first with a CPU fallback and a
provider the user can see. This is the measurement behind that: the same file,
the same input, one row per provider, on the machine that will run it. Nothing
here is a quality check — use the template's own render for that — and a timing
taken on a machine without a GPU proves only the CPU row.

Warm-up runs are separated from the measured ones: the first CUDA run pays for
context creation and kernel loading, which is tens of milliseconds that belong
to setup, not to a frame.

Exit status: 0 measured at least one provider, 1 none ran, 2 bad usage or a
missing dependency.
"""

import argparse
import json
import sys
import time
from pathlib import Path

from _ort_common import first_line, make_input, ort, parse_size, session_options


def percentile(sorted_values, fraction):
    """Nearest-rank on a list that is already sorted; it is never empty here,
    because a run that fails returns an error row instead of times."""
    index = min(len(sorted_values) - 1, int(round(fraction * (len(sorted_values) - 1))))
    return sorted_values[index]


def bench(path, provider, size, channels, iterations, warmup):
    """Return a result row. A provider that cannot run the model is an error row."""
    row = {"provider": provider, "error": None}
    try:
        session = ort.InferenceSession(
            str(path), sess_options=session_options(), providers=[provider]
        )
    except Exception as exc:  # noqa: BLE001 - the message is the result
        row["error"] = first_line(exc)
        return row

    feed = {session.get_inputs()[0].name: make_input(size, channels)}
    for _ in range(warmup):
        try:
            session.run(None, feed)
        except Exception as exc:  # noqa: BLE001 - the message is the result
            row["error"] = first_line(exc)
            return row

    times = []
    for _ in range(iterations):
        started = time.perf_counter()
        try:
            session.run(None, feed)
        except Exception as exc:  # noqa: BLE001 - the message is the result
            row["error"] = first_line(exc)
            return row
        times.append(time.perf_counter() - started)

    times.sort()
    median = percentile(times, 0.5)
    row.update(
        {
            # The providers the session really used: a silent fallback is a fact
            # the reader has to see next to the timing, not a failure.
            "session_providers": session.get_providers(),
            "iterations": len(times),
            "median_ms": round(median * 1000.0, 3),
            "p90_ms": round(percentile(times, 0.9) * 1000.0, 3),
            "fps": round(1.0 / median, 2),
        }
    )
    return row


def main():
    parser = argparse.ArgumentParser(
        description="Time one model per execution provider, with the providers the "
        "session actually used."
    )
    parser.add_argument("model", help="path to a .onnx file")
    parser.add_argument("--size", default="64x64", help="frame size (default: 64x64)")
    parser.add_argument(
        "--channels",
        type=int,
        default=3,
        help="channel count when the model leaves that dimension dynamic "
        "(default: 3)",
    )
    parser.add_argument(
        "--iterations", type=int, default=20, help="timed runs per provider (default: 20)"
    )
    parser.add_argument(
        "--warmup", type=int, default=3, help="untimed runs per provider (default: 3)"
    )
    parser.add_argument(
        "--provider",
        action="append",
        default=None,
        help="restrict to this provider (repeatable); default: every available one",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    path = Path(args.model)
    if not path.is_file():
        sys.stderr.write("error: no such file: {}\n".format(path))
        return 2
    if args.iterations <= 0:
        sys.stderr.write("error: --iterations wants a positive number\n")
        return 2
    if args.warmup < 0:
        sys.stderr.write("error: --warmup wants zero or more runs\n")
        return 2
    if args.channels <= 0:
        sys.stderr.write("error: --channels wants a positive number\n")
        return 2

    size = parse_size(args.size)
    available = ort.get_available_providers()
    # Azure answers every run by falling back to the CPU unless it was
    # configured with an endpoint, so timing it by default measures the CPU
    # twice. Ask for it by name to see its row.
    wanted = args.provider or [p for p in available if p != "AzureExecutionProvider"]

    rows = []
    for provider in wanted:
        if provider not in available:
            rows.append({"provider": provider, "error": "not installed"})
            continue
        rows.append(
            bench(path, provider, size, args.channels, args.iterations, args.warmup)
        )

    summary = {
        "model": str(path),
        "size": {"width": size[0], "height": size[1]},
        "channels": args.channels,
        "iterations": args.iterations,
        "warmup": args.warmup,
        "available_providers": available,
        "results": rows,
    }

    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print(
            "{}  {}x{}  {} channel(s)  {} runs after {} warm-up".format(
                path.name, size[0], size[1], args.channels, args.iterations, args.warmup
            )
        )
        print("")
        print("{:<26} {:>10} {:>10} {:>9}  {}".format(
            "provider", "median ms", "p90 ms", "fps", "session used"
        ))
        for row in rows:
            if row["error"]:
                print("{:<26} {:>10} {:>10} {:>9}  {}".format(
                    row["provider"], "-", "-", "-", row["error"]
                ))
                continue
            used = ", ".join(row["session_providers"])
            if row["session_providers"] != [row["provider"]]:
                used += "  (requested {}, fell back)".format(row["provider"])
            print("{:<26} {:>10.3f} {:>10.3f} {:>9.2f}  {}".format(
                row["provider"], row["median_ms"], row["p90_ms"], row["fps"], used
            ))
        print("")
        print("One frame of this size on the provider above; nothing here measures "
              "image quality. A GPU row is only meaningful on the machine that has "
              "the GPU.")

    measured = [r for r in rows if "median_ms" in r]
    return 0 if measured else 1


if __name__ == "__main__":
    raise SystemExit(main())
