#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Write synthetic ONNX models for the paths a real fixture does not reach.

    python3 tools/make-testmodels.py --out build/testmodels

Six models, each a real graph rather than a `Constant`, because a graph that
does nothing passes every test that only asks "did it run":

    id_1x_1ch.onnx   one channel, 1x1 AveragePool: output == input
    id_1x_3ch.onnx   RGB, same
    id_1x_4ch.onnx   RGBA, same; the fourth channel is where an alpha mistake
                     shows up, so a pass-through test that forgets it stays green
    up_2x_3ch.onnx   RGB, Resize nearest x2: output is twice the input in H and W
    cpu_node_1x_3ch.onnx  RGB, Hardmax: an operator the CUDA execution provider
                     does not implement, so the provider label has to admit that
                     a node was left on the CPU EP - the half of the label a
                     one-node Conv (which the provider covers) can never produce
    conv_chain_3ch_64.onnx  RGB to 64 channels through three 3x3 convs, H and W
                     dynamic: the graph tools/bench-ep-frame.* times, and the one
                     shape where a GPU execution provider is worth its setup cost

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
# ONNX Runtime 1.20, the oldest root the template still builds against, reads
# IR <= 10. A newer writer defaults higher; pinning keeps the fixtures loadable
# by every runtime this repository has been exercised with, 1.20.1 and 1.32.0.
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


def cpu_only_node_model(channels):
    """One node the CUDA execution provider does not implement.

    `Hardmax` has no CUDA kernel in ONNX Runtime 1.20.1 (probed 2026-11-27: the
    label reads "nodes left on CPU" with the runtime's reason, and the other
    candidates - `RandomNormalLike`, `MeanVarianceNormalization` - failed session
    creation instead, which proves nothing about the label). Shape-preserving, so
    a runner that expects an image gets one, and deterministic: the expected
    output in the self-test is a one-hot along the width axis.
    """
    graph = helper.make_graph(
        [helper.make_node("Hardmax", ["input"], ["output"], axis=-1)],
        "cpu_node_1x_{}ch".format(channels),
        [_input_info(channels)],
        [helper.make_tensor_value_info(
            "output", TensorProto.FLOAT, ["batch", channels, "height", "width"]
        )],
    )
    return _finish(graph)


def _weights(name, shape, seed):
    """Deterministic pseudo-random weights, without numpy.

    The generator runs with `onnx` alone - numpy is only needed for the
    self-test - so the numbers come from a small LCG. What matters is that the
    graph carries real weights of the right shape: a Constant-filled conv would
    let a provider optimise the multiply away and time that instead.
    """
    total = 1
    for extent in shape:
        total *= extent
    values = []
    state = seed & 0xFFFFFFFF
    for _ in range(total):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        values.append((state / 0x7FFFFFFF - 0.5) * 0.2)
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape), values)


def conv_chain_model(in_channels=3, channels=64, layers=3):
    """An H/W-dynamic 3x3 conv chain: what a frame costs, not what a tile costs.

    tools/bench-ep-frame.cpp times this graph at 640x480 and 1920x1080, which is
    where references/runtime-comparison.md section 5 gets its CPU-against-CUDA
    frame times. The first conv lifts `in_channels` to `channels`, the rest stay
    at `channels`, every one is padded "same" and followed by a ReLU - so the
    output is a picture of the input's size with 64 channels, and the work per
    frame is the three convolutions and nothing else.
    """
    nodes = []
    initializers = []
    source = "input"
    for layer in range(layers):
        weight = _weights(
            "c{}_w".format(layer),
            [channels, in_channels if layer == 0 else channels, 3, 3],
            seed=101 + layer,
        )
        bias = _weights("c{}_b".format(layer), [channels], seed=201 + layer)
        initializers.extend([weight, bias])
        nodes.append(
            helper.make_node(
                "Conv",
                [source, weight.name, bias.name],
                ["r{}".format(layer)],
                kernel_shape=[3, 3],
                pads=[1, 1, 1, 1],
            )
        )
        nodes.append(
            helper.make_node(
                "Relu",
                ["r{}".format(layer)],
                ["a{}".format(layer) if layer + 1 < layers else "output"],
            )
        )
        source = "r{}".format(layer)
    graph = helper.make_graph(
        nodes,
        "conv_chain_{}ch_{}x3x3".format(in_channels, channels),
        [_input_info(in_channels)],
        [helper.make_tensor_value_info(
            "output", TensorProto.FLOAT, ["batch", channels, "height", "width"]
        )],
        initializer=initializers,
    )
    return _finish(graph)


MODELS = [
    ("id_1x_1ch.onnx", identity_model(1), "identity, 1 channel", "identity", 1),
    ("id_1x_3ch.onnx", identity_model(3), "identity, RGB", "identity", 3),
    ("id_1x_4ch.onnx", identity_model(4), "identity, RGBA (alpha survives)", "identity", 4),
    ("up_2x_3ch.onnx", upscale_2x_model(3), "2x factor, RGB", "upscale", 3),
    (
        "cpu_node_1x_3ch.onnx",
        cpu_only_node_model(3),
        "Hardmax: the CUDA EP does not implement it, so the label must say so",
        "hardmax",
        3,
    ),
    (
        "conv_chain_3ch_64.onnx",
        conv_chain_model(3, 64, 3),
        "three 3x3 convs, RGB to 64 channels: the graph the frame-time probe times",
        "conv_chain",
        3,
    ),
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
            if kind == "conv_chain":
                # The weights are pseudo-random, so there is no closed form to
                # compare against: what is asserted is the shape contract the
                # probe and the plug-in rely on - same H/W, 64 channels, finite
                # values - at both frame sizes, which is what proves H and W are
                # dynamic.
                want = (1, 64, height, width)
                if y.shape != want or not np.isfinite(y).all():
                    print(
                        "  FAIL {} at {}x{}: shape {} (expected {}), finite {}".format(
                            name, width, height, y.shape, want, bool(np.isfinite(y).all())
                        )
                    )
                    failures += 1
                else:
                    print("  ok   {} at {}x{} -> {}".format(name, width, height, y.shape))
                continue
            if kind == "identity":
                expected = x
            elif kind == "hardmax":
                # ONNX Hardmax along the last axis: 1 at the first maximum, 0
                # elsewhere. Exact, so the fixture has one right output.
                expected = np.zeros_like(x)
                np.put_along_axis(expected, x.argmax(axis=3)[..., None], 1.0, axis=3)
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
