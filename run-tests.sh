#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# One command for the whole host-side test story of this repository:
#
#   ./run-tests.sh
#
# It checks the documentation, builds every template, and then runs, for each
# one, the unit/image tests,
# the bundle and memory checks, and the dlopen + fake-host contract test:
#
#   templates/minimal-ofx/test/run-tests.sh        (build + artifact + contract)
#   templates/minimal-cuda-ofx/test/run-tests.sh
#   templates/onnx-inference-ofx/test/run-tests.sh
#
# Environment:
#   OFX_SOURCE_DIR     openfx checkout (default: ./.ofx-ref when present,
#                      otherwise each template fetches OFX_Release_1.5.1)
#   ONNXRUNTIME_ROOT   optional: without it the ONNX template is skipped by
#                      name, not silently (see its build.sh --help)
#   CPU_ONLY=1         force the CUDA template to build its CPU path only
#   RUN_VALGRIND       auto | 1 | 0 (forwarded to both suites)
#   SANITIZE           e.g. address,undefined (forwarded; skips memcheck)
#
# Neither template needs a GPU or an OFX host application; what that leaves
# unproven is listed in TESTING_STRATEGY.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Documentation is part of the deliverable: fail the run when the prose drifts
# from the tree (links, anchors, counts, duplicated measurements, provenance).
echo
echo "== docs: check-docs =="
if command -v python3 >/dev/null 2>&1; then
  python3 "${ROOT}/tools/check-docs.py"
else
  echo "error: python3 is required to check the documentation" >&2
  exit 2
fi

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

# A missing ONNX Runtime skips one template, it does not abort the run. The
# reference and CUDA plug-ins need nothing, and exiting here meant a machine
# without ONNX Runtime never tested them at all.
onnx=1
if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  onnx=0
  echo "[onnx] ONNXRUNTIME_ROOT is unset: templates/onnx-inference-ofx is skipped by name"
  echo "       Point it at a directory with include/onnxruntime_cxx_api.h and"
  echo "       lib/libonnxruntime.so (see templates/onnx-inference-ofx/build.sh --help)."
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
if [[ "${onnx}" == "1" ]]; then
  echo "== build: onnx-inference-ofx =="
  ( cd "${ROOT}/templates/onnx-inference-ofx" && ./build.sh )
else
  echo "== build: onnx-inference-ofx (skipped: ONNXRUNTIME_ROOT unset) =="
fi

# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------
# The reference plug-in has no GPU or model dependency, so it builds and
# verifies itself first; a failure here means the scaffold or the toolchain, not
# kernel maths.
echo
echo "== reference plug-in: minimal-ofx (build, artifact, contract) =="
"${ROOT}/templates/minimal-ofx/test/run-tests.sh"

echo
echo "== test: minimal-cuda-ofx =="
"${ROOT}/templates/minimal-cuda-ofx/test/run-tests.sh"

echo
if [[ "${onnx}" == "1" ]]; then
  echo "== test: onnx-inference-ofx =="
  "${ROOT}/templates/onnx-inference-ofx/test/run-tests.sh"
else
  echo "== test: onnx-inference-ofx (skipped: ONNXRUNTIME_ROOT unset) =="
fi

echo
echo "=================================================================="
echo "all suites passed, every template that was built:"
echo "  docs drift check                (tools/check-docs.py)"
echo "  reference build + artifact      (minimal-ofx)"
echo "  unit/equivalence  (cuda)        image/golden      (cuda, inference)"
echo "  artifact + fake host (all)      memory            (all)"
if [[ "${onnx}" == "0" ]]; then
  echo "  skipped: onnx-inference-ofx     (ONNXRUNTIME_ROOT unset)"
fi
echo "Negative controls are separate: testkit/run-negative-controls.sh"
echo "=================================================================="
