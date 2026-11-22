#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Host-side checks for the ONNX inference plug-in. No GPU, no OFX host.
#
#   1. pipeline test: packing, whole-frame inference against a Python golden,
#      tiled inference against whole-frame, and the halo=0 negative control
#   2. sanitizers, when SANITIZE is set (address,undefined or thread)
#   3. bundle hygiene, when a build/ tree exists
#
# Environment:
#   ONNXRUNTIME_ROOT  required: onnxruntime_cxx_api.h + libonnxruntime.so
#   CXX               C++17 compiler (default g++)
#   SANITIZE          value for -fsanitize, e.g. address,undefined
#
# The fixtures in models/ are committed, so this needs no Python. Regenerate
# them with: python3 tools/make-test-model.py   (needs onnx + onnxruntime)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
CXX_BIN="${CXX:-g++}"
SAN="${SANITIZE:-}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  echo "error: ONNXRUNTIME_ROOT is not set (see ./build.sh --help)" >&2
  exit 2
fi

ORT_INC="${ONNXRUNTIME_ROOT}/include"
[[ -f "${ORT_INC}/onnxruntime_cxx_api.h" ]] || ORT_INC="${ONNXRUNTIME_ROOT}/onnxruntime/capi"
ORT_LIB="${ONNXRUNTIME_ROOT}/lib"
[[ -f "${ORT_LIB}/libonnxruntime.so" || -f "${ORT_LIB}/libonnxruntime.so.1" ]] || \
  ORT_LIB="${ONNXRUNTIME_ROOT}/onnxruntime/capi"

FLAGS=(-std=c++17 -Wall -Wextra -pthread)
if [[ -n "${SAN}" ]]; then
  FLAGS+=(-O1 -g -fno-omit-frame-pointer "-fsanitize=${SAN}")
  case ",${SAN}," in
    *,address,*) export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" ;;
  esac
  case ",${SAN}," in
    *,undefined,*) export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" ;;
  esac
else
  FLAGS+=(-O2)
fi

echo "== [1/3] inference pipeline (${CXX_BIN}, sanitizer: ${SAN:-none}) =="
"${CXX_BIN}" "${FLAGS[@]}" \
  -I "${ROOT}/src" -I "${ORT_INC}" \
  "${HERE}/ai_pipeline_test.cpp" "${ROOT}/src/AiCore.cpp" "${ROOT}/src/ModelRunner.cpp" \
  -o "${TMP}/ai_pipeline_test" \
  -L "${ORT_LIB}" -lonnxruntime -Wl,-rpath,"${ORT_LIB}"
"${TMP}/ai_pipeline_test" "${ROOT}/models"

echo
echo "== [2/3] model fixtures =="
for f in test_conv.onnx test_input.bin test_golden.bin test_shape.txt; do
  if [[ -f "${ROOT}/models/${f}" ]]; then
    echo "  ${f}: $(stat -c%s "${ROOT}/models/${f}") bytes"
  else
    echo "  FAIL: models/${f} is missing; run tools/make-test-model.py" >&2
    exit 1
  fi
done

echo
echo "== [3/3] bundle hygiene =="
BUNDLE_OFX="$(find "${ROOT}/build/bundle" -type f -name '*.ofx' 2>/dev/null | head -1 || true)"
if [[ -n "${BUNDLE_OFX}" ]]; then
  EXPORTS="$(nm -D --defined-only "${BUNDLE_OFX}" | grep -c ' T ' || true)"
  NEEDED="$(readelf -d "${BUNDLE_OFX}" | grep -c 'libonnxruntime' || true)"
  RPATH="$(readelf -d "${BUNDLE_OFX}" | grep -c "\$ORIGIN" || true)"
  echo "  ${BUNDLE_OFX#"${ROOT}"/}"
  echo "  exported dynamic symbols: ${EXPORTS} (expect 2)"
  echo "  NEEDED libonnxruntime:    ${NEEDED} (expect 1)"
  echo "  RUNPATH \$ORIGIN:          ${RPATH} (expect 1)"
  if [[ "${EXPORTS}" != "2" || "${NEEDED}" != "1" || "${RPATH}" != "1" ]]; then
    echo "  FAIL: packaging regression" >&2
    exit 1
  fi
else
  echo "  skipped: no build/ tree yet (run ./build.sh first)"
fi

echo
echo "all checks passed"
