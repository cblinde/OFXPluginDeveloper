#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Host-side checks for this plugin: everything that does not need a GPU or an
# OFX host.
#
#   1. alignment predicate, vec4/scalar/CPU-loop equivalence, concurrent runs
#      (compiles the real src/Kernel.cu via the shim in test/shim)
#   2. PTX instruction counts, when nvcc is on PATH - a compile-time check, so
#      it is unaffected by any runtime sanitizer
#   3. bundle hygiene (exports, launcher, fatbin), when a build/ tree exists
#   4. host contract: dlopen the built .ofx and drive it through a fake host
#      (testkit/), twice, in one process - describe, describeInContext,
#      createInstance, isIdentity, render, destroyInstance, unload
#   5. valgrind memcheck over both harnesses, when valgrind is installed and no
#      sanitizer was asked for (a sanitizer runtime and memcheck cannot be
#      combined)
#
# Environment:
#   CXX           C++17 compiler                     (default: g++)
#   SANITIZE      -fsanitize value, e.g. "address,undefined" or "thread"
#   RUN_VALGRIND  1 = require memcheck, 0 = skip it, unset = run if installed
#   PTX_ARCH      PTX target for the nvcc check      (default: sm_89)
#
# Nothing here executes device code: step 1 runs the kernel bodies on the CPU so
# they can be compared byte for byte, and step 2 reads the PTX that nvcc emits.
# See references/checklists.md for what that does and does not prove.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SRC="${ROOT}/src"
CXX_BIN="${CXX:-g++}"
ARCH="${PTX_ARCH:-sm_89}"
SAN="${SANITIZE:-}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

if ! command -v "${CXX_BIN}" >/dev/null 2>&1; then
  echo "error: '${CXX_BIN}' not found; set CXX to a C++17 compiler" >&2
  exit 2
fi

# The launcher needs a GPU, so the harness compiles src/Kernel.cu only up to the
# start of the launcher namespace. Everything above that line - the four
# kernels (32F vec4, 32F scalar, 16F half, 8-bit byte),
# ScalePixel, DeviceGuard and the alignment predicate - is the real device code,
# unchanged.
awk '/^namespace mcofx \{/{exit} {print}' "${SRC}/Kernel.cu" > "${TMP}/kernels.inc"
printf '#endif // OFXPLUGIN_WITH_CUDA\n' >> "${TMP}/kernels.inc"

# Flags for the harness. A sanitizer build trades optimisation for diagnostics
# and needs frames to name a line in its report.
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

build_harness()
{
  local out="$1"
  "${CXX_BIN}" "${FLAGS[@]}" \
    -I "${HERE}/shim" -I "${SRC}" -I "${TMP}" \
    -DOFXPLUGIN_WITH_CUDA=1 \
    "${HERE}/kernel_equivalence.cpp" -o "${out}"
}

echo "== [1/5] host suite (${CXX_BIN}, sanitizer: ${SAN:-none}) =="
build_harness "${TMP}/equiv"
"${TMP}/equiv"

echo
if command -v nvcc >/dev/null 2>&1; then
  echo "== [2/5] PTX instruction counts (nvcc, -arch=${ARCH}) =="
  nvcc -DOFXPLUGIN_WITH_CUDA=1 -arch="${ARCH}" -ptx -o "${TMP}/k.ptx" "${SRC}/Kernel.cu"

  entry()
  {
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
  echo "== [2/5] PTX check skipped: nvcc not on PATH =="
fi

echo
echo "== [3/5] bundle hygiene =="
BUNDLE_OFX="$(find "${ROOT}/build/bundle" -type f -name '*.ofx' 2>/dev/null | head -1 || true)"
if [[ -n "${BUNDLE_OFX}" ]]; then
  EXPORTS="$(nm -D --defined-only "${BUNDLE_OFX}" | grep -c ' T ' || true)"
  LAUNCHER="$(nm "${BUNDLE_OFX}" | grep -c 'LaunchGainRGBA32F' || true)"
  FATBIN="$(readelf -S "${BUNDLE_OFX}" | grep -c 'nv_fatbin' || true)"
  echo "  ${BUNDLE_OFX#"${ROOT}"/}"
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
echo "== [4/5] host contract (dlopen + fake host) =="
BUNDLE_OFX="$(find "${ROOT}/build/bundle" -type f -name '*.ofx' 2>/dev/null | head -1 || true)"
if [[ -n "${BUNDLE_OFX}" ]]; then
  OFX_CHECKOUT="$(sed -n 's/^OFX_SOURCE_DIR:PATH=//p' "${ROOT}/build/CMakeCache.txt" 2>/dev/null | head -1 || true)"
  "${ROOT}/../../testkit/build-contract-test.sh" "${TMP}/contract_test" ${OFX_CHECKOUT:+"${OFX_CHECKOUT}"}
  echo "  ${BUNDLE_OFX#"${ROOT}"/}"

  # One process holds one host (OfxSetHost ignores every call after the first),
  # so each shape is a run of its own: the dumps in openfx-misc's README-hosts.txt
  # (nuke: tiles + multi-resolution + temporal access; resolve: none of them) and
  # the background renderer NatronRenderer presents (natron: tiles, isBackground=1).
  HOST_PROFILES=(nuke resolve natron)

  # SUITE_THREADS=random picks a different concurrent-render count per run, the
  # way FATE's threads=randomN does - the race has to show up in some run rather
  # than in the one configuration someone hard-coded.
  THREADS="${SUITE_THREADS:-4}"
  if [[ "${THREADS}" == "random" ]]; then
    THREADS=$(( (RANDOM % 4) + 1 ))
    echo "  SUITE_THREADS=random: ${THREADS} concurrent render thread(s) this run"
  fi

  for profile in "${HOST_PROFILES[@]}"; do
    echo "  host profile: ${profile}"
    "${TMP}/contract_test" "${BUNDLE_OFX}" \
      --host-profile "${profile}" \
      --id com.ofxplugindeveloper.minimalcudagain \
      --param gain:double --param gainAlpha:double --clip Source --clip Output \
      --identity --negative-pitch --set gain=2.0 --not-identity-after-set --byte-dst-rejected \
      --src-bounds-inset 2 --dst-bounds-inset 2 \
      --concurrency-param gain \
      --concurrency-threads "${THREADS}"
  done
else
  echo "  skipped: no build/ tree yet (run ./build.sh first)"
fi

echo
echo "== [5/5] valgrind memcheck =="
WANT="${RUN_VALGRIND:-auto}"
if [[ "${WANT}" == "0" ]]; then
  echo "  skipped: RUN_VALGRIND=0"
elif [[ -n "${SAN}" ]]; then
  echo "  skipped: a -fsanitize build does not run under memcheck (both replace the"
  echo "  malloc/thread runtime, and valgrind rejects the combination)"
elif ! command -v valgrind >/dev/null 2>&1; then
  if [[ "${WANT}" == "1" ]]; then
    echo "error: RUN_VALGRIND=1 but valgrind is not on PATH" >&2
    exit 2
  fi
  echo "  skipped: valgrind not on PATH (install valgrind; on Debian/Ubuntu the"
  echo "  tracebacks want libc6-dbg as well)"
else
  # Separate, unsanitised build: memcheck replaces the allocator itself.
  "${CXX_BIN}" -std=c++17 -O1 -g -pthread -Wall -Wextra \
    -I "${HERE}/shim" -I "${SRC}" -I "${TMP}" \
    -DOFXPLUGIN_WITH_CUDA=1 \
    "${HERE}/kernel_equivalence.cpp" -o "${TMP}/equiv-vg"
  valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --track-origins=yes \
    "${TMP}/equiv-vg" > "${TMP}/vg.log" 2>&1 || {
      echo "  FAIL: memcheck reported errors" >&2
      grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost|indirectly lost|possibly lost" \
        "${TMP}/vg.log" | head -20 >&2
      exit 1
    }
  grep -E "ERROR SUMMARY|definitely lost|indirectly lost|possibly lost|still reachable" \
    "${TMP}/vg.log" | sed 's/^==[0-9]*== /  /'

  # Same memcheck pass over the contract harness (dlopen + fake host).
  if [[ -x "${TMP}/contract_test" && -n "${BUNDLE_OFX}" ]]; then
    valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite \
      --errors-for-leak-kinds=definite --track-origins=yes \
      "${TMP}/contract_test" "${BUNDLE_OFX}" \
      --id com.ofxplugindeveloper.minimalcudagain \
      --param gain:double --param gainAlpha:double --clip Source --clip Output \
      --identity --negative-pitch --set gain=2.0 --not-identity-after-set --byte-dst-rejected \
      --src-bounds-inset 2 --dst-bounds-inset 2 \
      --concurrency-param gain \
      > "${TMP}/vg-contract.log" 2>&1 || {
        echo "  FAIL: memcheck reported errors in the contract test" >&2
        grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" \
          "${TMP}/vg-contract.log" | head -20 >&2
        exit 1
      }
    echo "  contract harness:"
    grep -E "ERROR SUMMARY|definitely lost|indirectly lost" "${TMP}/vg-contract.log" \
      | sed 's/^==[0-9]*== /    /'
  fi
fi

echo
echo "all checks passed"
