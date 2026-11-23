#!/usr/bin/env python3
"""Answer, from the model file alone, what `AI_MODEL_INTEGRATION.md` §7 asks.

    python3 tools/model_probe.py model.onnx
    python3 tools/model_probe.py model.onnx --size 64x64 --json

Reports what a plug-in has to know before it packs a frame: the input and
output tensor shapes and which of their dimensions are dynamic, how many
channels the model expects, whether the output is the same size as the input
(and by which factor it is not), whether H/W accept arbitrary sizes or need a
multiple of 8/16/32, and which execution providers this runtime has. It loads the
model once, on the CPU: whether a provider can run the file is
`tools/bench_ep.py`'s question, not this one.

The alignment probe is the one `AI_MODEL_INTEGRATION.md` §7 describes: one run
at 63, 72, 80 and 96 pixels square. A model that reshapes to H/8, H/16 or H/32
throws at the sizes that are not a multiple, so the smallest size that runs
names the factor. The factor printed here is the largest power of two dividing
that size, capped at 32 — a probe result, not a guarantee, so a model that only
runs at one odd size is reported with what it did rather than a made-up number.

No GPU, no host and no download: CPU ONNX Runtime only. Use `tools/bench_ep.py`
for timing and `tools/make-testmodels.py` when there is no model to probe yet.

Exit status: 0 probed, 1 the model could not be loaded or run, 2 bad usage or a
missing dependency.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

from _ort_common import first_line, make_input, ort, parse_size, session_options

# The ladder from AI_MODEL_INTEGRATION.md section 7, in pixels square.
LADDER = (63, 72, 80, 96)


def describe_shape(shape):
    """`[batch, 3, height, width]`, with a `?` for a dimension with no name."""
    rendered = []
    for dim in shape:
        if isinstance(dim, int):
            rendered.append(str(dim))
        elif isinstance(dim, str):
            rendered.append(dim)
        else:  # None: a dimension that is not even symbolically named
            rendered.append("?")
    return "[" + ", ".join(rendered) + "]"


def probe_once(session, size, channels):
    """Run the model once; return (output shape, error text or None)."""
    x = make_input(size, channels)
    try:
        out = session.run(None, {session.get_inputs()[0].name: x})[0]
    except Exception as exc:  # noqa: BLE001 - the message is the result
        return None, first_line(exc)
    return list(out.shape), None


def alignment(session, channels):
    """The ladder: which sizes ran, and the factor they imply."""
    runs = []
    for n in LADDER:
        shape, error = probe_once(session, (n, n), channels)
        runs.append({"size": n, "ok": shape is not None, "error": error})
    passing = [r["size"] for r in runs if r["ok"]]
    factor = None
    if passing:
        smallest = min(passing)
        factor = 1
        while factor < 32 and smallest % (factor * 2) == 0:
            factor *= 2
    return runs, factor


def main():
    parser = argparse.ArgumentParser(
        description="Report a model's shapes, dynamic dimensions, size factor, "
        "alignment and available execution providers."
    )
    parser.add_argument("model", help="path to a .onnx file")
    parser.add_argument(
        "--size",
        default="64x64",
        help="frame size for the probe runs (default: 64x64)",
    )
    parser.add_argument(
        "--channels",
        type=int,
        default=3,
        help="channel count when the model leaves that dimension dynamic "
        "(default: 3)",
    )
    parser.add_argument(
        "--no-alignment",
        action="store_true",
        help="skip the 63/72/80/96 ladder",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    path = Path(args.model)
    if not path.is_file():
        sys.stderr.write("error: no such file: {}\n".format(path))
        return 2
    if args.channels <= 0:
        sys.stderr.write("error: --channels wants a positive number\n")
        return 2

    size = parse_size(args.size)
    try:
        session = ort.InferenceSession(
            str(path), sess_options=session_options(), providers=["CPUExecutionProvider"]
        )
    except Exception as exc:  # noqa: BLE001 - the message is the result
        sys.stderr.write(
            "error: {} could not be loaded: {}\n".format(path.name, str(exc).strip())
        )
        return 1

    inputs = session.get_inputs()
    outputs = session.get_outputs()
    if len(inputs) != 1:
        sys.stderr.write(
            "error: {} has {} inputs; this tool probes single-input models, which "
            "is what an image plug-in packs\n".format(path.name, len(inputs))
        )
        return 1
    in_shape = inputs[0].shape
    out_shape = outputs[0].shape
    if len(in_shape) != 4:
        sys.stderr.write(
            "error: {} has a {}-dimensional input; this tool probes NCHW models, "
            "which is what an image plug-in packs\n".format(path.name, len(in_shape))
        )
        return 1
    channels = in_shape[1] if isinstance(in_shape[1], int) else args.channels
    dynamic_size = not (isinstance(in_shape[2], int) and isinstance(in_shape[3], int))

    base_shape, base_error = probe_once(session, size, channels)
    if base_shape is None:
        sys.stderr.write(
            "error: the model did not run at {}x{}: {}\n".format(
                size[0], size[1], base_error
            )
        )
        return 1

    if len(base_shape) != 4:
        sys.stderr.write(
            "error: the output at {}x{} is {}-dimensional, not NCHW; a size factor "
            "has no meaning for it\n".format(size[0], size[1], len(base_shape))
        )
        return 1
    factor_y = base_shape[2] / float(size[1])
    factor_x = base_shape[3] / float(size[0])

    report = {
        "model": str(path),
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "opset": None,
        "ir_version": None,
        "input": {
            "name": inputs[0].name,
            "type": inputs[0].type,
            "shape": list(in_shape),
            "dynamic": [d for d in in_shape if not isinstance(d, int)],
        },
        "output": {
            "name": outputs[0].name,
            "type": outputs[0].type,
            "shape": list(out_shape),
            "dynamic": [d for d in out_shape if not isinstance(d, int)],
        },
        "channels": channels,
        "factor": {"x": factor_x, "y": factor_y},
        "dynamic_size": dynamic_size,
        "alignment": None,
        "providers": session.get_providers(),
        "available_providers": ort.get_available_providers(),
    }

    try:  # optional: opset and IR version are in the file, not in the session
        import onnx

        model = onnx.load(str(path), load_external_data=False)
        report["opset"] = [int(o.version) for o in model.opset_import]
        report["ir_version"] = int(model.ir_version)
    except Exception:  # noqa: BLE001 - optional detail, never a failure
        pass

    if dynamic_size and not args.no_alignment:
        runs, factor = alignment(session, channels)
        report["alignment"] = {"ladder": runs, "factor": factor}

    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    in_text = describe_shape(in_shape)
    out_text = describe_shape(out_shape)
    print("model        {}  ({} B, sha256 {}{})".format(
        path.name,
        report["bytes"],
        report["sha256"][:16],
        ", opset {} IR {}".format(
            ",".join(str(o) for o in report["opset"]), report["ir_version"]
        )
        if report["opset"]
        else "",
    ))
    print("input        {}  {}  {}".format(inputs[0].name, inputs[0].type, in_text))
    print("output       {}  {}  {}".format(outputs[0].name, outputs[0].type, out_text))
    print("channels     {}".format(channels))
    if factor_x == 1 and factor_y == 1:
        print("size         1x1 - output matches the input")
    else:
        print(
            "size         {}x{} - output is {}x{} of the input at {}x{}".format(
                factor_x, factor_y, factor_x, factor_y, size[0], size[1]
            )
        )
    if dynamic_size:
        print("dynamic H/W  yes - sizes are not fixed by the export")
    else:
        print("dynamic H/W  no - this model only runs at {}x{}".format(
            in_shape[3], in_shape[2]))

    if report["alignment"]:
        ladder = report["alignment"]["ladder"]
        ran = ", ".join("{} {}".format(r["size"], "ok" if r["ok"] else "fails")
                        for r in ladder)
        factor = report["alignment"]["factor"]
        print("alignment    {}  => multiple of {}".format(ran, factor or "?"))
        for r in ladder:
            if r["error"]:
                print("             {}: {}".format(r["size"], r["error"]))
    elif args.no_alignment:
        print("alignment    not probed (--no-alignment)")

    print("providers    {}".format(", ".join(report["providers"])))
    missing = [p for p in report["available_providers"] if p not in report["providers"]]
    if missing:
        print("             available but not used here: {}".format(", ".join(missing)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
