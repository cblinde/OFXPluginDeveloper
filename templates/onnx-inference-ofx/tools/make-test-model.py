#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
#
# Generates the tiny ONNX model and the golden tensors used by
# test/ai_pipeline_test.cpp.
#
# The model is deliberately boring: one 3x3 convolution with a fixed kernel and
# bias, padding 1, 3 in / 3 out channels, dynamic H and W. Three properties make
# it a useful test fixture:
#
#   * it is spatial (receptive field 3x3), so tiling without a halo is wrong,
#     which is what the negative control in the test asserts;
#   * it is deterministic and small (a few KB), so the model and its golden
#     output can be committed and the test needs no Python at run time;
#   * it has dynamic H/W, so the same file exercises odd sizes and 1x1 images.
#
# Outputs (written next to the model):
#   models/test_conv.onnx    the model
#   models/test_input.bin    float32 NCHW [1,3,H,W] input, row-major
#   models/test_golden.bin   float32 NCHW [1,3,H,W] output of the CPU EP
#   models/test_shape.txt    "channels height width" for the two .bin files
#
# Usage:  python3 tools/make-test-model.py
# Needs:  onnx, onnxruntime, numpy

import os

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, "..", "models"))

CHANNELS = 3
HEIGHT = 48
WIDTH = 64


def build_model(path: str) -> None:
    # A 3x3 sharpen-ish kernel: centre weight per channel plus a small symmetric
    # cross, tinted per channel so RGB do not collapse to the same values.
    k = np.zeros((CHANNELS, CHANNELS, 3, 3), dtype=np.float32)
    for c in range(CHANNELS):
        k[c, c, 1, 1] = 0.5
        k[c, c, 0, 1] = -0.125
        k[c, c, 1, 0] = -0.125
        k[c, c, 2, 1] = -0.125
        k[c, c, 1, 2] = -0.125
    k[0, 1, 1, 1] = 0.0625  # a little cross-channel mixing
    k[2, 1, 1, 1] = -0.0625
    bias = np.array([0.01, -0.02, 0.03], dtype=np.float32)

    node = helper.make_node(
        "Conv",
        inputs=["input", "weights", "bias"],
        outputs=["output"],
        kernel_shape=[3, 3],
        pads=[1, 1, 1, 1],
        strides=[1, 1],
        group=1,
    )

    graph = helper.make_graph(
        [node],
        "test_conv",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, CHANNELS, "h", "w"])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, CHANNELS, "h", "w"])],
        initializer=[
            numpy_helper.from_array(k, "weights"),
            numpy_helper.from_array(bias, "bias"),
        ],
    )
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", 17)],
        producer_name="ofxplugindeveloper-test-model",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, path)


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    model_path = os.path.join(OUT, "test_conv.onnx")
    build_model(model_path)

    # Deterministic input with structure in both axes, so a tile boundary lands
    # on a visible edge rather than on a flat run of pixels.
    y, x = np.meshgrid(np.arange(HEIGHT), np.arange(WIDTH), indexing="ij")
    planes = []
    for c in range(CHANNELS):
        p = ((x * 3 + y * 5 + c * 7) % 23) / 23.0 + 0.25 * np.sin(x / 4.0 + c)
        planes.append(p.astype(np.float32))
    inp = np.stack(planes, axis=0)[None, ...].astype(np.float32)

    so = ort.SessionOptions()
    so.intra_op_num_threads = 1  # match the C++ side's default; keeps bit-exactness plausible
    sess = ort.InferenceSession(model_path, sess_options=so, providers=["CPUExecutionProvider"])
    (out,) = sess.run(None, {"input": inp})

    inp.astype(np.float32).tofile(os.path.join(OUT, "test_input.bin"))
    out.astype(np.float32).tofile(os.path.join(OUT, "test_golden.bin"))
    with open(os.path.join(OUT, "test_shape.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"{CHANNELS} {HEIGHT} {WIDTH}\n")

    print(f"model   : {model_path} ({os.path.getsize(model_path)} bytes)")
    print(f"input   : {inp.shape} {inp.dtype}")
    print(f"output  : {out.shape} min={out.min():.6f} max={out.max():.6f}")
    print(f"ort     : {ort.__version__}")


if __name__ == "__main__":
    main()
