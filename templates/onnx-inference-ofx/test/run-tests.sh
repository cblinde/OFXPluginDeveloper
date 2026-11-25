#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Host-side checks for the ONNX inference plug-in. No GPU, no OFX host.
#
#   1. pipeline test: packing, whole-frame inference against a Python golden,
#      tiled inference against whole-frame, and the halo=0 negative control
#   2. sanitizers, when SANITIZE is set (address,undefined or thread)
#   3. host contract: dlopen the built .ofx and drive it through a fake host
#      (testkit/), twice, in one process - the render is compared against the
#      same golden, but through the OFX API instead of AiCore directly
#   4. bundle hygiene, when a build/ tree exists
#   5. valgrind memcheck of the contract harness, when valgrind is installed
#
# Environment:
#   ONNXRUNTIME_ROOT  required: onnxruntime_cxx_api.h + libonnxruntime.so
#   CXX               C++17 compiler (default g++)
#   SANITIZE          value for -fsanitize, e.g. address,undefined
#   RUN_VALGRIND      auto (default) | 1 (require) | 0 (skip)
#
# The fixtures in models/ are committed, so this needs no Python. Regenerate
# them with: python3 tools/make-test-model.py   (needs onnx + onnxruntime)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SAN="${SANITIZE:-}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  echo "error: ONNXRUNTIME_ROOT is not set (see ./build.sh --help)" >&2
  exit 2
fi

# Does this root ship the CUDA provider? A CPU package has no
# libonnxruntime_providers_cuda.so, so the question answers itself there. When it
# does, a session that ends on the CPU is a broken bundle rather than a passing
# run - see require_gpu_label below.
PROVIDER_IN_ROOT=""
for candidate in "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_cuda.so" \
                 "${ONNXRUNTIME_ROOT}/onnxruntime/capi/libonnxruntime_providers_cuda.so" \
                 "${ONNXRUNTIME_ROOT}/bin/onnxruntime_providers_cuda.dll" \
                 "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll"; do
  if [[ -f "${candidate}" ]]; then
    PROVIDER_IN_ROOT="${candidate}"
    break
  fi
done

# Called after each label check that accepts "CPU (...)". With a CPU package
# that is the expected answer; with a GPU root it means the provider did not
# load, which is exactly what a wiped build directory produces: no provider in
# the bundle, session falls back, every pixel check still passes. The suite has
# to fail there instead of going green on the CPU path.
require_gpu_label() {
  local log="$1" what="$2"
  [[ -z "${PROVIDER_IN_ROOT}" ]] && return 0
  if grep -q "label parameter 'provider' = \"CPU" "${log}"; then
    echo "  FAIL: ${PROVIDER_IN_ROOT} exists, so this root's session had to run on the" >&2
    echo "        device - and it fell back to the CPU (contract run: ${what})." >&2
    grep "label parameter 'provider'" "${log}" | sed 's/^ *//; s/^/        /' >&2
    echo "        The label's \"- fallback: <reason>\" is the load error: either the bundle" >&2
    echo "        has no provider beside the .ofx, or its CUDA imports are missing. Build" >&2
    echo "        with OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON, and point" >&2
    echo "        OFXPLUGIN_CUDA_RUNTIME_DIRS at cuDNN 9, cuBLAS, cuFFT and cudart when they" >&2
    echo "        are not under the runtime root." >&2
    exit 1
  fi
  echo "  provider label names CUDA (the runtime root ships a CUDA provider)"
}

if [[ -n "${SAN}" ]]; then
  case ",${SAN}," in
    *,address,*) export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" ;;
  esac
  case ",${SAN}," in
    *,undefined,*) export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" ;;
  esac
fi

echo "== [1/5] inference pipeline (cmake target ai_pipeline_test, sanitizer: ${SAN:-none}) =="
# Built by the CMake target, not by a second hard-coded compiler line: the
# source list and the ONNX Runtime flags live in CMakeLists.txt once. A
# sanitizer needs its own tree because the flags are baked in at configure time,
# and reconfiguring build/ would leave it sanitized for every later run.
if [[ -n "${SAN}" ]]; then
  TEST_BUILD="${ROOT}/build-sanitize"
  CMAKE_EXTRA=(-DONNXRUNTIME_ROOT="${ONNXRUNTIME_ROOT}")
  if [[ -n "${OFX_SOURCE_DIR:-}" ]]; then
    CMAKE_EXTRA+=(-DOFX_SOURCE_DIR="${OFX_SOURCE_DIR}")
  fi
  cmake -S "${ROOT}" -B "${TEST_BUILD}" "${CMAKE_EXTRA[@]}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=${SAN} -fno-omit-frame-pointer -O1 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${SAN}"
  cmake --build "${TEST_BUILD}" --target ai_pipeline_test --parallel
  TEST_BIN="${TEST_BUILD}/ai_pipeline_test"
else
  if [[ ! -f "${ROOT}/build/CMakeCache.txt" ]]; then
    echo "error: ${ROOT}/build is not configured; run ./build.sh first" >&2
    exit 2
  fi
  cmake --build "${ROOT}/build" --target ai_pipeline_test --parallel
  TEST_BIN="${ROOT}/build/ai_pipeline_test"
fi
"${TEST_BIN}" "${ROOT}/models"

echo
echo "== [2/5] host contract (dlopen + fake host) =="
BUNDLE_OFX="$(find "${ROOT}/build/bundle" -type f -name '*.ofx' 2>/dev/null | head -1 || true)"
if [[ -n "${BUNDLE_OFX}" ]]; then
  OFX_CHECKOUT="$(sed -n 's/^OFX_SOURCE_DIR:PATH=//p' "${ROOT}/build/CMakeCache.txt" 2>/dev/null | head -1 || true)"
  "${ROOT}/../../testkit/build-contract-test.sh" "${TMP}/contract_test" ${OFX_CHECKOUT:+"${OFX_CHECKOUT}"}
  echo "  ${BUNDLE_OFX#"${ROOT}"/}"
  CONTRACT_ARGS=("${BUNDLE_OFX}"
    --id com.ofxplugindeveloper.onnxinference
    --param modelPath:string --param tileSize:int --param halo:int
    --clip Source --clip Output --identity --size 64x48
    --src "${ROOT}/models/test_input.bin" --golden "${ROOT}/models/test_golden.bin"
    --set "modelPath=${ROOT}/models/test_conv.onnx" --set halo=2
    --roi-expand 2 --roi-context --label-param provider
    --not-identity-after-set --negative-pitch --byte-dst-rejected
    --src-bounds-inset 2 --dst-bounds-inset 2)
  # The golden file is the CPU runtime's output, bit for bit. A CUDA session runs
  # the same convolution with different rounding, so on a GPU machine the last
  # bits differ and an exact comparison fails for a correct pipeline. The first
  # attempt is therefore still exact - that is what proves the packing on the CPU -
  # and only a mismatch is retried with a bound four orders of magnitude below the
  # smallest error the pipeline can produce (a missing context region shows 0.1875,
  # see the tiled check below). A real fault fails both attempts.
  if ! "${TMP}/contract_test" "${CONTRACT_ARGS[@]}" | tee "${TMP}/exact.log"; then
    echo "  exact comparison failed; retrying with --golden-tolerance 1e-5 for a golden rendered by another provider"
    "${TMP}/contract_test" "${CONTRACT_ARGS[@]}" --golden-tolerance 1e-5 | tee "${TMP}/exact.log"
  fi

  # The plug-in writes its execution provider into a label parameter. CUDA is
  # requested first and the CPU session is the fallback, so the value has to
  # name one of them and, when it is the fallback, carry the reason. Both are
  # accepted here: the harness cannot know which runtime a GPU machine links.
  # A CUDA session must say which arithmetic it runs: without "tf32 off" the
  # label describes a device session whose numbers nobody has pinned down.
  if ! grep -qE "label parameter 'provider' = \"(CUDA \(device 0, 1 thread, .*tf32 off|CPU)" "${TMP}/exact.log"; then
    echo "  FAIL: the provider label does not name an execution provider" >&2
    exit 1
  fi
  require_gpu_label "${TMP}/exact.log" "main"

  # The same contract against the other host shape: supportsTiles=0 and
  # supportsMultiResolution=0, the profile Resolve's OFX host reports in the
  # dumps published in openfx-misc's README-hosts.txt. Only the two host
  # properties change - every window, RoI and pixel check stays, because a host
  # with supportsTiles=0 still renders a window smaller than the frame when its
  # own region of definition is smaller.
  echo "  the same contract against a host with supportsTiles=0 and supportsMultiResolution=0"
  if ! "${TMP}/contract_test" "${CONTRACT_ARGS[@]}" --host-profile resolve | tee "${TMP}/resolve.log"; then
    echo "  exact comparison failed on the resolve-shaped host; retrying with --golden-tolerance 1e-5"
    "${TMP}/contract_test" "${CONTRACT_ARGS[@]}" --host-profile resolve --golden-tolerance 1e-5 | tee "${TMP}/resolve.log"
  fi
  if ! grep -qE "label parameter 'provider' = \"(CUDA \(device 0, 1 thread, .*tf32 off|CPU)" "${TMP}/resolve.log"; then
    echo "  FAIL: the provider label does not name an execution provider under the resolve profile" >&2
    exit 1
  fi
  require_gpu_label "${TMP}/resolve.log" "resolve profile"

  # The same pixel comparison with the plug-in's own tiling engaged: tileSize=16
  # makes the 64x48 window go through RunTiled, so the frame is assembled from
  # overlapping, feathered tiles. There the comparison is a bound rather than
  # bit-equality - TileAccumulator blends the overlap, which is exact only when
  # the overlapping tiles agree bitwise, true at halo >= receptive radius but not
  # guaranteed by construction. Measured on this model: 0 deviation with halo=2
  # and 0.1875 without the context region, so 0.02 leaves an order of magnitude
  # between the check and the failure, and matches the halo-ladder threshold in
  # references/onnx-and-tensorrt.md section 7.
  echo "  tiled render (tileSize=16), tolerance 0.02:"
  "${TMP}/contract_test" "${BUNDLE_OFX}" \
    --id com.ofxplugindeveloper.onnxinference \
    --param modelPath:string --param tileSize:int --param halo:int \
    --clip Source --clip Output --size 64x48 \
    --set "modelPath=${ROOT}/models/test_conv.onnx" --set halo=2 --set tileSize=16 \
    --set-status-only --concurrency-threads 0 --roi-context-tolerance 0.02
else
  echo "  FAIL: no .ofx under build/bundle; run ./build.sh first" >&2
  exit 1
fi

echo
echo "== [3/5] model fixtures =="
for f in test_conv.onnx test_input.bin test_golden.bin test_shape.txt; do
  if [[ -f "${ROOT}/models/${f}" ]]; then
    echo "  ${f}: $(stat -c%s "${ROOT}/models/${f}") bytes"
  else
    echo "  FAIL: models/${f} is missing; run tools/make-test-model.py" >&2
    exit 1
  fi
done

echo
echo "== [4/5] bundle hygiene =="
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
  echo "  FAIL: no .ofx under build/bundle; run ./build.sh first" >&2
  exit 1
fi

echo
echo "== [5/5] valgrind memcheck (contract harness) =="
WANT="${RUN_VALGRIND:-auto}"
if [[ "${WANT}" == "0" ]]; then
  echo "  skipped: RUN_VALGRIND=0"
elif [[ -n "${SAN}" ]]; then
  echo "  skipped: a -fsanitize build does not run under memcheck"
elif ! command -v valgrind >/dev/null 2>&1; then
  if [[ "${WANT}" == "1" ]]; then
    echo "error: RUN_VALGRIND=1 but valgrind is not on PATH" >&2
    exit 2
  fi
  echo "  skipped: valgrind not on PATH"
elif [[ ! -x "${TMP}/contract_test" || -z "${BUNDLE_OFX}" ]]; then
  echo "  skipped: the contract harness did not run"
else
  # ONNX Runtime keeps a few MB in process-lifetime arenas: still reachable,
  # which is not a leak. --errors-for-leak-kinds=definite is the line that
  # matters - anything the plug-in itself drops is reported.
  valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite \
    --errors-for-leak-kinds=definite --track-origins=yes \
    "${TMP}/contract_test" "${BUNDLE_OFX}" \
    --id com.ofxplugindeveloper.onnxinference \
    --param modelPath:string --param tileSize:int --param halo:int \
    --clip Source --clip Output --identity --size 64x48 \
    --src "${ROOT}/models/test_input.bin" --golden "${ROOT}/models/test_golden.bin" \
    --set "modelPath=${ROOT}/models/test_conv.onnx" \
    --not-identity-after-set --byte-dst-rejected --src-bounds-inset 2 \
    --dst-bounds-inset 2 --set halo=2 --roi-expand 2 --roi-context \
    > "${TMP}/vg-contract.log" 2>&1 || {
      echo "  FAIL: memcheck reported errors" >&2
      grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" "${TMP}/vg-contract.log" | head -20 >&2
      exit 1
    }
  grep -E "ERROR SUMMARY|definitely lost|indirectly lost|possibly lost|still reachable" \
    "${TMP}/vg-contract.log" | sed 's/^==[0-9]*== /  /'
fi

echo
echo "all checks passed"
