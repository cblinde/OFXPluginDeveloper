#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# One command for the whole host-side test story of this repository:
#
#   ./run-tests.sh
#
# It builds both templates and then runs, for each one, the unit/image tests,
# the bundle and memory checks, and the dlopen + fake-host contract test:
#
#   templates/minimal-cuda-ofx/test/run-tests.sh
#   templates/onnx-inference-ofx/test/run-tests.sh
#
# Environment:
#   OFX_SOURCE_DIR     openfx checkout (default: ./.ofx-ref when present,
#                      otherwise each template fetches OFX_Release_1.5.1)
#   ONNXRUNTIME_ROOT   required for the ONNX template (see its build.sh --help)
#   CPU_ONLY=1         force the CUDA template to build its CPU path only
#   RUN_VALGRIND       auto | 1 | 0 (forwarded to both suites)
#   SANITIZE           e.g. address,undefined (forwarded; skips memcheck)
#
# Neither template needs a GPU or an OFX host application; what that leaves
# unproven is listed in TESTING_STRATEGY.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${OFX_SOURCE_DIR:-}" && -f "${ROOT}/.ofx-ref/include/ofxCore.h" ]]; then
  OFX_SOURCE_DIR="${ROOT}/.ofx-ref"
fi
if [[ -n "${OFX_SOURCE_DIR:-}" ]]; then
  if [[ ! -f "${OFX_SOURCE_DIR}/include/ofxCore.h" ]]; then
    echo "error: OFX_SOURCE_DIR='${OFX_SOURCE_DIR}' has no include/ofxCore.h" >&2
    exit 2
  fi
  export OFX_SOURCE_DIR
  echo "[ofx] using ${OFX_SOURCE_DIR}"
fi

if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  echo "error: ONNXRUNTIME_ROOT is not set; the ONNX template cannot build." >&2
  echo "  Point it at a directory with include/onnxruntime_cxx_api.h and" >&2
  echo "  lib/libonnxruntime.so (see templates/onnx-inference-ofx/build.sh --help)." >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
cuda_mode="CUDA"
if [[ "${CPU_ONLY:-0}" == "1" ]] || ! command -v nvcc >/dev/null 2>&1; then
  cuda_mode="CPU_ONLY"
  # build.sh exits 2 when asked for CUDA and there is no nvcc, by design; the
  # single command must still work on a machine without a toolkit.
  export CPU_ONLY=1
fi
echo
echo "== build: minimal-cuda-ofx (${cuda_mode}) =="
( cd "${ROOT}/templates/minimal-cuda-ofx" && ./build.sh )

echo
echo "== build: onnx-inference-ofx =="
( cd "${ROOT}/templates/onnx-inference-ofx" && ./build.sh )

# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------
echo
echo "== test: minimal-cuda-ofx =="
"${ROOT}/templates/minimal-cuda-ofx/test/run-tests.sh"

echo
echo "== test: onnx-inference-ofx =="
"${ROOT}/templates/onnx-inference-ofx/test/run-tests.sh"

echo
echo "=================================================================="
echo "all suites passed for both templates:"
echo "  unit/equivalence  (both)        image/golden      (both)"
echo "  artifact + fake host (both)     memory            (both)"
echo "Negative controls are separate: testkit/run-negative-controls.sh"
echo "=================================================================="
