#!/usr/bin/env python3
"""Write synthetic ONNX models for the paths a real fixture does not reach.

    python3 tools/make-testmodels.py --out build/testmodels

Four models, each a real graph rather than a `Constant`, because a graph that
does nothing passes every test that only asks "did it run":

    id_1x_1ch.onnx   one channel, 1x1 AveragePool: output == input
    id_1x_3ch.onnx   RGB, same
    id_1x_4ch.onnx   RGBA, same; the fourth channel is where an alpha mistake
                     shows up, so a pass-through test that forgets it stays green
    up_2x_3ch.onnx   RGB, Resize nearest x2: output is twice the input in H and W

Why `AveragePool` and not `Identity`: with a real node, a channel or plane-order
mistake produces a large deviation instead of a green check on a copy. Why
`Resize` nearest: a 2x factor that is exact and reproducible, so a runner's
"did the output grow?" check has one right answer.

What these are NOT: quality models. They carry no weights worth having and no
image semantics; they exist to exercise the channel-count, identity and
scale-factor code paths without downloading a model and without a GPU. Use
`tools/measure-halo.sh` and a real model for anything about pixels.

Dependencies: `onnx` to write the files. `--self-test` additionally needs
`onnxruntime` and `numpy`, and loads each model through ONNX Runtime's CPU
provider, so the models are proven to parse and produce the shape and values
they claim without an OFX host.

Exit status: 0 written (and verified when the self-test ran), 1 a self-test
failed, 2 a dependency is missing.
"""

import argparse
import sys
from pathlib import Path

try:
    import onnx
    from onnx import TensorProto, helper
except ImportError:  # pragma: no cover - the message is the point
    sys.stderr.write("error: this tool needs the 'onnx' package: pip install onnx\n")
    raise SystemExit(2)

OPSET = 13
# ONNX Runtime 1.20 (the version the template is built against) reads IR <= 10.
# A newer writer defaults higher; pinning keeps the models loadable there.
IR_VERSION = 8


def _input_info(channels):
    return helper.make_tensor_value_info(
        "input", TensorProto.FLOAT, ["batch", channels, "height", "width"]
    )


def _finish(graph):
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        producer_name="make-testmodels.py",
    )
    model.ir_version = IR_VERSION
    onnx.checker.check_model(model)
    return model


def identity_model(channels):
    """Output == input, through one real node so ordering errors cannot hide."""
    graph = helper.make_graph(
        [
            helper.make_node(
                "AveragePool",
                ["input"],
                ["output"],
                kernel_shape=[1, 1],
                strides=[1, 1],
                pads=[0, 0, 0, 0],
            )
        ],
        "identity_1x_{}ch".format(channels),
        [_input_info(channels)],
        [helper.make_tensor_value_info(
            "output", TensorProto.FLOAT, ["batch", channels, "height", "width"]
        )],
    )
    return _finish(graph)


def upscale_2x_model(channels):
    """Nearest-neighbour x2: exact, so the expected output has one answer."""
    scales = helper.make_tensor("scales", TensorProto.FLOAT, [4], [1.0, 1.0, 2.0, 2.0])
    graph = helper.make_graph(
        [
            helper.make_node(
                "Resize",
                ["input", "", "scales"],
                ["output"],
                mode="nearest",
                coordinate_transformation_mode="asymmetric",
                nearest_mode="floor",
            )
        ],
        "upscale_2x_{}ch".format(channels),
        [_input_info(channels)],
        [helper.make_tensor_value_info(
            "output", TensorProto.FLOAT, ["batch", channels, "height2", "width2"]
        )],
        initializer=[scales],
    )
    return _finish(graph)


MODELS = [
    ("id_1x_1ch.onnx", identity_model(1), "identity, 1 channel", "identity", 1),
    ("id_1x_3ch.onnx", identity_model(3), "identity, RGB", "identity", 3),
    ("id_1x_4ch.onnx", identity_model(4), "identity, RGBA (alpha survives)", "identity", 4),
    ("up_2x_3ch.onnx", upscale_2x_model(3), "2x factor, RGB", "upscale", 3),
]


def self_test(out_dir):
    """Load every model through ONNX Runtime and check shape and values.

    Two frame sizes, one of them odd in both axes: a model that hardcodes an
    export size passes the first and fails the second, which is the bug these
    fixtures exist to make visible.
    """
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        sys.stderr.write(
            "self-test skipped: needs onnxruntime and numpy (pip install onnxruntime numpy)\n"
        )
        return None

    failures = 0
    for name, _, what, kind, channels in MODELS:
        session = ort.InferenceSession(
            str(out_dir / name), providers=["CPUExecutionProvider"]
        )
        for height, width in ((8, 6), (13, 7)):
            x = np.arange(channels * height * width, dtype=np.float32) / 64.0
            x = x.reshape(1, channels, height, width)
            try:
                y = session.run(None, {"input": x})[0]
            except Exception as exc:  # noqa: BLE001 - the message is the result
                print("  FAIL {} at {}x{}: {}".format(name, width, height, exc))
                failures += 1
                continue
            if kind == "identity":
                expected = x
            else:
                expected = np.repeat(np.repeat(x, 2, axis=2), 2, axis=3)
            if y.shape != expected.shape or not np.array_equal(y, expected):
                print(
                    "  FAIL {} at {}x{}: shape {} (expected {}), values {}".format(
                        name,
                        width,
                        height,
                        y.shape,
                        expected.shape,
                        "differ" if y.shape == expected.shape else "not comparable",
                    )
                )
                failures += 1
            else:
                print("  ok   {} at {}x{} -> {}".format(name, width, height, y.shape))
        print("       {}".format(what))
    print(
        "self-test: {} model(s) x 2 sizes, {} failure(s)".format(len(MODELS), failures)
    )
    return failures


def main():
    parser = argparse.ArgumentParser(
        description="Write synthetic ONNX models for channel counts and a 2x factor."
    )
    parser.add_argument(
        "--out",
        default="build/testmodels",
        help="directory to write into (default: build/testmodels)",
    )
    parser.add_argument(
        "--no-self-test",
        action="store_true",
        help="only write the files, do not load them back through ONNX Runtime",
    )
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, model, what, _, _ in MODELS:
        path = out_dir / name
        onnx.save(model, str(path))
        print("wrote {} {:>6}  {}".format(path, "{} B".format(path.stat().st_size), what))

    if args.no_self_test:
        return 0
    failures = self_test(out_dir)
    if failures is None:
        return 0
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
