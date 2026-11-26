#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Compile the committed test model ahead of time instead of shipping a runtime.
#
#   tools/aot-compile-model.sh                       # the inference template's model
#   tools/aot-compile-model.sh model.onnx build/aot
#
# What it does, in order, and why each step is separate:
#
#   1. `iree-import-onnx` turns the .onnx into StableHLO/MLIR - the export step
#      that decides whether the graph is even expressible outside ONNX.
#   2. `iree-compile` builds a CPU artifact (`llvm-cpu`, runs here) and a Vulkan
#      artifact (`vulkan`, SPIR-V, needs a Vulkan device to run - compiling it is
#      the part that can be checked without one).
#   3. `iree-run-module` runs the CPU artifact on the committed input, and the
#      output is compared with the committed golden. That comparison is the
#      point: an ahead-of-time artifact that runs but computes something else is
#      worse than no artifact.
#
# What it does not do: load the artifact from C++. That needs the IREE runtime
# C API inside the plug-in, which is the second half of the spike and is written
# down in references/ai-architectures-and-runtimes.md, not done here.
#
# Exit status: 0 everything the machine can do passed, 3 a tool is missing
# (named, not silent), 1 a step failed or the output does not match the golden.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="${1:-${ROOT}/templates/onnx-inference-ofx/models/test_conv.onnx}"
OUT="${2:-${ROOT}/build/aot}"

for tool in iree-import-onnx iree-compile iree-run-module; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "aot: skipped - ${tool} is not on PATH (pip install iree-compiler iree-runtime)" >&2
    exit 3
  fi
done

if [[ ! -f "${MODEL}" ]]; then
  echo "aot: ${MODEL} does not exist" >&2
  exit 1
fi

mkdir -p "${OUT}"
name="$(basename "${MODEL}" .onnx)"
mlir="${OUT}/${name}.mlir"
cpu="${OUT}/${name}-cpu.vmfb"
vulkan="${OUT}/${name}-vulkan.vmfb"

echo "== import: ONNX -> MLIR/StableHLO"
iree-import-onnx "${MODEL}" -o "${mlir}"
echo "   ${mlir#"${ROOT}/"}: $(stat -c%s "${mlir}") bytes"

echo "== compile: CPU artifact"
iree-compile "${mlir}" \
  --iree-hal-target-device=local \
  --iree-hal-local-target-device-backends=llvm-cpu \
  --iree-llvmcpu-target-cpu=generic \
  -o "${cpu}"
# `generic`, not `host`: the artifact is meant to ship, and a `host` target bakes
# in this machine's AVX-512 and crashes on a CPU without it - which is the whole
# reason the toolchain warns about the default when you leave it out.
echo "   ${cpu#"${ROOT}/"}: $(stat -c%s "${cpu}") bytes"

echo "== compile: Vulkan artifact (SPIR-V, not runnable here)"
if iree-compile "${mlir}" --iree-hal-target-device=vulkan -o "${vulkan}"; then
  echo "   ${vulkan#"${ROOT}/"}: $(stat -c%s "${vulkan}") bytes"
  echo "   modules with SPIR-V: $(iree-dump-module "${vulkan}" 2>/dev/null | grep -c 'spirv' || true)"
else
  echo "   the Vulkan target did not compile; the CPU artifact above still stands" >&2
  exit 1
fi

# The fixtures sit beside the model, and the frame comes from them, not from a
# constant: H and W are symbolic in the model, `test_shape.txt` is what the input
# and the golden were generated with, and the shared resolver spells both.
model_dir="$(dirname "${MODEL}")"
input="${model_dir}/test_input.bin"
golden="${model_dir}/test_golden.bin"
shape_file="${model_dir}/test_shape.txt"
if [[ ! -f "${input}" || ! -f "${golden}" ]]; then
  echo "aot: no input/golden beside the model, so the artifact is compiled but not compared"
  exit 0
fi
if [[ ! -f "${shape_file}" ]]; then
  echo "aot: ${shape_file} is missing, so the input spec has nothing to come from" >&2
  exit 1
fi
if ! spec="$(python3 "${ROOT}/tools/_model_shape.py" "${shape_file}")"; then
  echo "aot: ${shape_file} does not describe a frame" >&2
  exit 1
fi
# A fixture regenerated at another size leaves an input of the wrong length; the
# runtime would then refuse it with a shape mismatch, which reads like an artifact
# bug rather than a stale file.
if ! python3 - "${shape_file}" "${input}" "${ROOT}/tools" <<'CHECK'
import os, sys
sys.path.insert(0, sys.argv[3])
import _model_shape

channels, height, width = _model_shape.read_shape_file(sys.argv[1])
expected = channels * height * width * 4  # float32
actual = os.path.getsize(sys.argv[2])
if actual != expected:
    print(
        f"{sys.argv[2]}: {actual} bytes, {channels}x{height}x{width} float32 needs {expected}",
        file=sys.stderr,
    )
    raise SystemExit(1)
CHECK
then
  echo "aot: the committed input does not match test_shape.txt" >&2
  exit 1
fi

echo "== run: CPU artifact on the committed input (${spec}, from test_shape.txt)"
iree-run-module --module="${cpu}" --device=local-task \
  --input="${spec}=@${input}" \
  --output="@${OUT}/${name}-out.bin"

python3 - "${OUT}/${name}-out.bin" "${golden}" <<'PY'
import sys
try:
    import numpy as np
except ImportError:
    print("   comparison skipped: numpy is not installed")
    raise SystemExit(0)
out = np.fromfile(sys.argv[1], dtype=np.float32)
gold = np.fromfile(sys.argv[2], dtype=np.float32)
if out.size != gold.size:
    print(f"   FAIL: {out.size} floats produced, the golden has {gold.size}", file=sys.stderr)
    raise SystemExit(1)
difference = float(np.max(np.abs(out - gold)))
print(f"   max |difference| = {difference:.3g}")
# Not a bit-equality gate: the CPU artifact is a different compiler and a
# different accumulation order. 1e-6 is two orders below anything a golden
# comparison in this repository calls a meaningful deviation.
if difference > 1e-6:
    print("   FAIL: the artifact does not reproduce the golden", file=sys.stderr)
    raise SystemExit(1)
print("   the ahead-of-time artifact reproduces the golden")
PY
