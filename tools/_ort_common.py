"""The three things `model_probe.py` and `bench_ep.py` otherwise copied.

Not a command line tool: the two scripts import it and keep their own argument
parsing and output. It exists because both need the same deterministic input, the
same `--size` parsing, a quiet session and the first line of a runtime error —
each of which was verbatim in both files.

The import failure is handled here rather than in every caller: a missing
onnxruntime is a setup problem, so it is one message and exit 2.
"""

import sys

try:
    import numpy as np
    import onnxruntime as ort
except ImportError:  # pragma: no cover - the message is the point
    sys.stderr.write(
        "error: this tool needs onnxruntime and numpy: pip install onnxruntime numpy\n"
    )
    raise SystemExit(2)


def make_input(size, channels):
    """One deterministic NCHW frame: a constant image has no structure to see."""
    width, height = size
    y, x = np.mgrid[0:height, 0:width]
    planes = np.empty((channels, height, width), dtype=np.float32)
    for c in range(channels):
        planes[c] = ((x * (7 + c) + y * (13 + 3 * c)) % 101) / 101.0
    return planes.reshape(1, channels, height, width)


def parse_size(text):
    """`WxH`, or `N` for a square, as (width, height)."""
    parts = text.lower().split("x")
    try:
        width, height = int(parts[0]), int(parts[-1])
    except ValueError:
        raise SystemExit("error: --size wants WxH, e.g. 64x64")
    if width <= 0 or height <= 0:
        raise SystemExit("error: --size wants positive numbers")
    return width, height


def session_options():
    """Quiet sessions: the probe fails on purpose at some sizes, and both tools
    print their own one-line copy of whatever the runtime said."""
    options = ort.SessionOptions()
    options.log_severity_level = 3
    return options


def first_line(exc, limit=160):
    """The first line of a runtime error, clipped: an OOM message is long."""
    return str(exc).strip().splitlines()[0][:limit]
