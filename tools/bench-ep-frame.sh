#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Build tools/bench-ep-frame.cpp against an ONNX Runtime package and run it: one
# frame through the CPU and the CUDA execution providers, the measurement
# references/runtime-comparison.md section 5 quotes.
#
#   tools/bench-ep-frame.sh <ort-root> <model.onnx> <W> <H> [options]
#   tools/bench-ep-frame.sh .validation/ort-gpu-1.32.0 models/test_conv.onnx 1920 1080
#
# <ort-root> is a package with include/, lib/libonnxruntime.so and bin/ - the
# bundled one, not the CPU-only wheel in .venv, because the wheel has no CUDA
# provider. The binary is written into that package's bin/ directory and run with
# an $ORIGIN rpath, so the libonnxruntime.so and libonnxruntime_providers_cuda.so
# beside it are the ones a bundle would ship.
#
# Options after the frame size are the program's own: --iterations N, --warmup N,
# --threads N. What a sample is, and why the first frame is printed separately,
# is in the program's header.
#
# The 112 ms in runtime-comparison.md was measured on Windows, so the Windows
# numbers come from tools/bench-ep-frame.bat; this script exists because the
# measurement is not platform-specific and a render farm is not Windows.
#
# Exit codes: 0 ran, 2 usage or a missing package file, 1 the build or the
# program failed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die2() { echo "error: $*" >&2; exit 2; }

if [[ $# -lt 4 ]]; then
  cat >&2 <<'EOF'
usage: bench-ep-frame.sh <ort-root> <model.onnx> <W> <H> [--iterations N] [--warmup N] [--threads N]

  ort-root is a package with include/, lib/libonnxruntime.so and bin/ - the
  bundled one, not the CPU-only wheel in .venv.
EOF
  exit 2
fi

ORT="$1"
MODEL="$2"
WIDTH="$3"
HEIGHT="$4"
shift 4

[[ -f "${ORT}/include/onnxruntime_cxx_api.h" ]] \
  || die2 "'${ORT}/include/onnxruntime_cxx_api.h' not found - is '${ORT}' an ONNX Runtime package?"
[[ -f "${MODEL}" ]] || die2 "'${MODEL}' not found"

LIB=""
for candidate in "${ORT}/lib/libonnxruntime.so" "${ORT}/lib/libonnxruntime.dylib" \
                 "${ORT}/lib/libonnxruntime.so.1"; do
  if [[ -e "${candidate}" ]]; then LIB="${candidate}"; break; fi
done
[[ -n "${LIB}" ]] || die2 "no libonnxruntime.so/.dylib under '${ORT}/lib'"

CXX="${CXX:-}"
if [[ -z "${CXX}" ]]; then
  for candidate in clang++ g++; do
    if command -v "${candidate}" >/dev/null 2>&1; then CXX="${candidate}"; break; fi
  done
fi
[[ -n "${CXX}" ]] || die2 "no C++ compiler found; set CXX"

mkdir -p "${ORT}/bin"
echo "[build] ${CXX} -O2 tools/bench-ep-frame.cpp -> ${ORT}/bin/bench-ep-frame"
"${CXX}" -std=c++17 -O2 \
  -I "${ORT}/include" "${HERE}/bench-ep-frame.cpp" \
  -o "${ORT}/bin/bench-ep-frame" \
  -L "${ORT}/lib" -lonnxruntime \
  -Wl,-rpath,'$ORIGIN' \
  || { echo "error: the probe did not compile" >&2; exit 1; }

echo
exec "${ORT}/bin/bench-ep-frame" "${MODEL}" "${WIDTH}" "${HEIGHT}" "$@"
