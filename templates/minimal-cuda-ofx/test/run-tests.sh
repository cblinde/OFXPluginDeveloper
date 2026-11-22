#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Tests for everything about this plugin that does not need a GPU:
#
#   1. alignment predicate + vec4/scalar kernel equivalence + CPU loop equivalence
#      (compiles the real src/Kernel.cu with the shim in test/shim)
#   2. PTX instruction counts, when nvcc is on PATH
#   3. bundle hygiene (exports, launcher, fatbin), when a build/ tree exists
#
# Usage:  ./test/run-tests.sh
# Needs:  a C++17 compiler. nvcc and an existing build are optional.
#
# Nothing here executes device code: step 1 runs the kernel bodies on the CPU so
# they can be compared byte for byte, and step 2 reads the PTX that nvcc emits.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SRC="${ROOT}/src"
CXX_BIN="${CXX:-g++}"
ARCH="${PTX_ARCH:-sm_89}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

if ! command -v "${CXX_BIN}" >/dev/null 2>&1; then
  echo "error: '${CXX_BIN}' not found; set CXX to a C++17 compiler" >&2
  exit 2
fi

# The launcher needs a GPU, so the harness compiles src/Kernel.cu only up to the
# start of the launcher namespace. Everything above that line - both kernels,
# ScalePixel, DeviceGuard - is the real device code, unchanged.
awk '/^namespace mcofx \{/{exit} {print}' "${SRC}/Kernel.cu" > "${TMP}/kernels.inc"
printf '#endif // OFXPLUGIN_WITH_CUDA\n' >> "${TMP}/kernels.inc"

echo "== [1/3] host equivalence suite (${CXX_BIN}) =="
"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra \
  -I "${HERE}/shim" -I "${SRC}" -I "${TMP}" \
  -DOFXPLUGIN_WITH_CUDA=1 \
  "${HERE}/kernel_equivalence.cpp" -o "${TMP}/equiv"
"${TMP}/equiv"

echo
if command -v nvcc >/dev/null 2>&1; then
  echo "== [2/3] PTX instruction counts (nvcc, -arch=${ARCH}) =="
  nvcc -DOFXPLUGIN_WITH_CUDA=1 -arch="${ARCH}" -ptx -o "${TMP}/k.ptx" "${SRC}/Kernel.cu"

  entry() {
    awk -v fn="$1" 'index($0, ".entry ") && index($0, fn) { on = 1 } on { print } on && /^}/ { exit }' \
      "${TMP}/k.ptx"
  }

  VEC="$(entry GainKernelRGBA32FVec4)"
  SCA="$(entry GainKernelRGBA32FScalar)"

  vl=$(printf '%s\n' "${VEC}" | grep -c 'ld\.global\.nc\.v4\.f32' || true)
  vs=$(printf '%s\n' "${VEC}" | grep -c 'st\.global\.v4\.f32' || true)
  sl=$(printf '%s\n' "${SCA}" | grep -c 'ld\.global\.nc\.f32' || true)
  ss=$(printf '%s\n' "${SCA}" | grep -c 'st\.global\.f32' || true)

  printf '  vec4  : %s ld.global.nc.v4.f32 + %s st.global.v4.f32   (expect 1 + 1)\n' "${vl}" "${vs}"
  printf '  scalar: %s ld.global.nc.f32    + %s st.global.f32     (expect 4 + 4)\n' "${sl}" "${ss}"

  if [[ "${vl}" != "1" || "${vs}" != "1" || "${sl}" != "4" || "${ss}" != "4" ]]; then
    echo "  FAIL: memory instructions do not match the vectorized/scalar contract" >&2
    echo "  (if vec4 shows 4 scalar loads, the compiler no longer trusts the float4 indexing)" >&2
    exit 1
  fi
else
  echo "== [2/3] PTX check skipped: nvcc not on PATH =="
fi

echo
echo "== [3/3] bundle hygiene =="
BUNDLE_OFX="$(find "${ROOT}/build/bundle" -type f -name '*.ofx' 2>/dev/null | head -1 || true)"
if [[ -n "${BUNDLE_OFX}" ]]; then
  EXPORTS="$(nm -D --defined-only "${BUNDLE_OFX}" | grep -c ' T ' || true)"
  LAUNCHER="$(nm "${BUNDLE_OFX}" | grep -c 'LaunchGainRGBA32F' || true)"
  FATBIN="$(readelf -S "${BUNDLE_OFX}" | grep -c 'nv_fatbin' || true)"
  echo "  ${BUNDLE_OFX#${ROOT}/}"
  echo "  exported dynamic symbols: ${EXPORTS} (expect 2)"
  echo "  launcher references:      ${LAUNCHER} (0 in a CPU-only build)"
  echo "  nv_fatbin sections:       ${FATBIN} (0 in a CPU-only build)"
  if [[ "${EXPORTS}" != "2" ]]; then
    echo "  FAIL: symbol visibility regression" >&2
    exit 1
  fi
else
  echo "  skipped: no build/ tree yet (run ./build.sh first)"
fi

echo
echo "all checks passed"
