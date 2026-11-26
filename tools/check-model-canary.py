#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Load the shipped test model on the installed ONNX Runtime and compare the
result with the golden that this repository commits beside it.

Why this exists: the gate in `run-tests.sh` and in `linux.yml` runs the runtime
the documents name, so an upgrade that changes a fusion or refuses the model is
only noticed when somebody bumps the pin. This is the same measurement without a
pin - the CI job that calls it installs the newest release every week - and it
prints both versions, because "the numbers moved" and "the model no longer
loads" are different news.

What it proves: the shipped graph loads on that runtime, announces the opset it
was saved with, and reproduces the committed output. What it does not prove:
anything about a GPU provider, about the C++ side, or about a model that is not
this 611-byte graph.

Exit status: 0 when the output matches within the tolerance, 1 otherwise, 2 when
a file is missing or the runtime raises.
"""

from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(ROOT, "templates", "onnx-inference-ofx", "models")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--model", default=os.path.join(MODELS, "test_conv.onnx"))
    parser.add_argument("--input", default=os.path.join(MODELS, "test_input.bin"))
    parser.add_argument("--golden", default=os.path.join(MODELS, "test_golden.bin"))
    parser.add_argument(
        "--tolerance",
        type=float,
        default=0.0,
        help="largest allowed absolute difference (default 0: the CPU execution "
        "provider reproduced the golden exactly on 1.20.1 and on 1.32.0, so a "
        "difference is drift until somebody says otherwise)",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="exit 0 with a named skip when onnxruntime or numpy is not installed, "
        "the way the fixture stage does",
    )
    args = parser.parse_args()

    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError as exc:
        message = f"canary: SKIPPED - {exc.name} is not installed"
        if args.allow_missing:
            print(message)
            return 0
        print(message, file=sys.stderr)
        return 2

    for path in (args.model, args.input, args.golden):
        if not os.path.isfile(path):
            print(f"canary: {path} is missing", file=sys.stderr)
            return 2

    # The opset is a property of the file, not of the runtime: read it from the
    # model when `onnx` is there, and say so when it is not, rather than
    # guessing. `onnx.defs.onnx_opset_version()` is the newest opset the
    # installed package knows - the second number a weekly run is looking at.
    opset = None
    newest_opset = None
    try:
        import onnx

        meta = onnx.load(args.model, load_external_data=False)
        opset = ",".join(str(o.version) for o in meta.opset_import)
        ir_version = meta.ir_version
        newest_opset = onnx.defs.onnx_opset_version()
    except Exception:  # noqa: BLE001 - the canary is about the runtime, not onnx
        ir_version = None

    options = ort.SessionOptions()
    # One thread, like the C++ side and like tools/make-test-model.py: the
    # comparison is bit-exact, and a different reduction order would make it
    # fail for a reason that is not an upgrade.
    options.intra_op_num_threads = 1
    try:
        session = ort.InferenceSession(
            args.model, sess_options=options, providers=["CPUExecutionProvider"]
        )
        shape = [1, 3]
        with open(os.path.join(MODELS, "test_shape.txt"), encoding="utf-8") as fh:
            _, height, width = (int(v) for v in fh.read().split())
        shape += [height, width]
        data = np.fromfile(args.input, dtype=np.float32)
        if data.size != shape[1] * shape[2] * shape[3]:
            print(
                f"canary: {args.input} holds {data.size} floats, expected "
                f"{shape[1] * shape[2] * shape[3]}",
                file=sys.stderr,
            )
            return 2
        tensor = data.reshape(shape)
        (output,) = session.run(None, {"input": tensor})
    except Exception as exc:  # noqa: BLE001 - any runtime error is the finding
        print(f"canary: the runtime refused the model: {exc}", file=sys.stderr)
        return 1

    golden = np.fromfile(args.golden, dtype=np.float32)
    if golden.size != output.size:
        print(
            f"canary: golden holds {golden.size} floats, the runtime produced "
            f"{output.size}",
            file=sys.stderr,
        )
        return 1
    # ravel(): the golden is a flat float32 dump, the runtime returns NCHW.
    difference = float(np.max(np.abs(output.astype(np.float32).ravel() - golden)))

    print(f"model    : {os.path.relpath(args.model, ROOT)}")
    print(f"runtime  : onnxruntime {ort.__version__}")
    if opset is not None:
        print(f"model    : opset {opset}, IR {ir_version}")
        print(f"onnx     : package knows opset {newest_opset} (not a bound, context)")
    else:
        print("model    : opset unknown (the onnx package is not installed)")
    print(f"output   : max |difference| = {difference:.9g} (tolerance {args.tolerance:g})")

    if difference > args.tolerance:
        print(
            "canary: FAIL - the runtime no longer reproduces the committed golden",
            file=sys.stderr,
        )
        return 1
    print("canary: the shipped model still loads and still matches its golden")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
