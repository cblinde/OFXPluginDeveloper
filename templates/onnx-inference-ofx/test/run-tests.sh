#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Host-side checks for the ONNX inference plug-in. No GPU, no OFX host.
#
#   1. pipeline test: packing, whole-frame inference against a Python golden,
#      tiled inference against whole-frame, and the halo=0 negative control
#   2. sanitizers, when SANITIZE is set (address,undefined or thread)
#   3. host contract: dlopen the built .ofx and drive it through a fake host
#      (testkit/), once per host shape (nuke, resolve, natron) - the render is
#      compared against the same golden, but through the OFX API instead of
#      AiCore directly
#   4. bundle hygiene, when a build/ tree exists
#   5. valgrind memcheck of the contract harness, whole-frame and tiled, when
#      valgrind is installed
#
# Environment:
#   ONNXRUNTIME_ROOT  required: onnxruntime_cxx_api.h + libonnxruntime.so
#   CXX               C++17 compiler (default g++)
#   SANITIZE          value for -fsanitize, e.g. address,undefined
#   RUN_VALGRIND      auto (default) | 1 (require) | 0 (skip)
#   SUITE_THREADS     4 (default) | random - concurrent host render threads
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
  # The model is not a parameter any more: it arrives through ONNX_MODEL_PATH (or
  # from Contents/Resources/model.onnx), read once at instance creation. So
  # identity and pixels are two runs - one with no model in the environment, which
  # must report identity, and one with it set, which must render.
  MODEL_ENV="${ROOT}/models/test_conv.onnx"
  IDENTITY_ARGS=("${BUNDLE_OFX}"
    --id com.ofxplugindeveloper.onnxinference
    --param tileSize:int --param halo:int
    --clip Source --clip Output --identity --size 64x48)
  CONTRACT_ARGS=("${BUNDLE_OFX}"
    --id com.ofxplugindeveloper.onnxinference
    --param tileSize:int --param halo:int
    --clip Source --clip Output --size 64x48
    --src "${ROOT}/models/test_input.bin" --golden "${ROOT}/models/test_golden.bin"
    --set halo=2
    --roi-expand 2 --roi-context --label-param provider
    --negative-pitch --byte-dst-rejected
    --src-bounds-inset 2 --dst-bounds-inset 2)
  # Which reference, and with which bound, is a property of the reference rather
  # than one tolerance for everything: `--fuzz NAME` reads
  # models/test_golden.<NAME>.fuzz, so the CPU reference keeps its exact demand
  # (cpu.fuzz = 0) and only a reference a CUDA session cannot reproduce bit for
  # bit says 1e-05. The file itself resolves to models/test_golden-<platform>.bin
  # first, then models/test_golden.bin, so a reference rendered on another
  # platform sits beside the generic one instead of replacing it. Both decisions
  # are printed by the harness, so the log says which claim a run made.
  FUZZ=cpu
  [[ -n "${PROVIDER_IN_ROOT}" ]] && FUZZ=cuda

  # One process holds one host (OfxSetHost ignores every call after the first),
  # so each shape is a run of its own: the dumps in openfx-misc's README-hosts.txt
  # (nuke: tiles + multi-resolution + temporal access; resolve: none of them) and
  # the background renderer NatronRenderer presents (natron: tiles, isBackground=1,
  # no UI). Every window, RoI and pixel check is the same in all three.
  HOST_PROFILES=(nuke resolve natron)

  # SUITE_THREADS=random picks a different concurrent-render count per run, the
  # way FATE's threads=randomN does - a race has to show up in some run rather
  # than in the one configuration someone hard-coded.
  THREADS="${SUITE_THREADS:-4}"
  if [[ "${THREADS}" == "random" ]]; then
    THREADS=$(( (RANDOM % 4) + 1 ))
    echo "  SUITE_THREADS=random: ${THREADS} concurrent render thread(s) this run"
  fi

  for profile in "${HOST_PROFILES[@]}"; do
    echo "  host profile: ${profile} (reference fuzz: ${FUZZ})"
    # First without a model in the environment: a host on a machine that never
    # set ONNX_MODEL_PATH must be told identity, or it renders a pass-through
    # nobody asked for.
    env -u ONNX_MODEL_PATH "${TMP}/contract_test" "${IDENTITY_ARGS[@]}" \
      --host-profile "${profile}" 2>&1 | tee "${TMP}/${profile}-identity.log"
    ONNX_MODEL_PATH="${MODEL_ENV}" "${TMP}/contract_test" "${CONTRACT_ARGS[@]}" \
      --host-profile "${profile}" --fuzz "${FUZZ}" \
      --concurrency-threads "${THREADS}" | tee "${TMP}/${profile}.log"

    # The plug-in writes its execution provider into a label parameter. CUDA is
    # requested first and the CPU session is the fallback, so the value has to
    # name one of them and, when it is the fallback, carry the reason. Both are
    # accepted here: the harness cannot know which runtime a GPU machine links.
    # A CUDA session must say which arithmetic it runs: without "tf32 off" the
    # label describes a device session whose numbers nobody has pinned down.
    if ! grep -qE "label parameter 'provider' = \"(CUDA \(device 0, 1 thread, .*tf32 off|CPU)" "${TMP}/${profile}.log"; then
      echo "  FAIL: the provider label does not name an execution provider (${profile} profile)" >&2
      exit 1
    fi
    require_gpu_label "${TMP}/${profile}.log" "${profile} profile"
  done

  # The same pixel comparison with the plug-in's own tiling engaged: tileSize=16
  # makes the 64x48 window go through RunTiled, so the frame is assembled from
  # overlapping, feathered tiles. There the comparison is a bound rather than
  # bit-equality - TileAccumulator blends the overlap, which is exact only when
  # the overlapping tiles agree bitwise, true at halo >= receptive radius but not
  # guaranteed by construction. Measured on this model: 0 deviation with halo=2
  # and 0.1875 without the context region, so 0.02 leaves an order of magnitude
  # between the check and the failure, and matches the halo-ladder threshold in
  # references/onnx-and-tensorrt.md section 4 ("The halo is measured, not guessed").
  # One list for the tiled render, used here and by the memcheck pass below:
  # the two have to ask the plug-in the same question, or memcheck covers a
  # different run than the one the suite reports.
  TILED_ARGS=(
    --id com.ofxplugindeveloper.onnxinference
    --param tileSize:int --param halo:int
    --clip Source --clip Output --size 64x48
    --set halo=2 --set tileSize=16
    --set-status-only --concurrency-threads 0 --roi-context-tolerance 0.02
  )
  echo "  tiled render (tileSize=16), tolerance 0.02:"
  ONNX_MODEL_PATH="${MODEL_ENV}" "${TMP}/contract_test" "${BUNDLE_OFX}" "${TILED_ARGS[@]}"
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
echo "== [5/5] valgrind memcheck (contract harness, whole frame and tiled) =="
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
  ONNX_MODEL_PATH="${ROOT}/models/test_conv.onnx" \
  valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite \
    --errors-for-leak-kinds=definite --track-origins=yes \
    "${TMP}/contract_test" "${BUNDLE_OFX}" \
    --id com.ofxplugindeveloper.onnxinference \
    --param tileSize:int --param halo:int \
    --clip Source --clip Output --size 64x48 \
    --src "${ROOT}/models/test_input.bin" --golden "${ROOT}/models/test_golden.bin" \
    --byte-dst-rejected --src-bounds-inset 2 \
    --dst-bounds-inset 2 --set halo=2 --roi-expand 2 --roi-context \
    > "${TMP}/vg-contract.log" 2>&1 || {
      echo "  FAIL: memcheck reported errors" >&2
      grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" "${TMP}/vg-contract.log" | head -20 >&2
      exit 1
    }
  grep -E "ERROR SUMMARY|definitely lost|indirectly lost|possibly lost|still reachable" \
    "${TMP}/vg-contract.log" | sed 's/^==[0-9]*== /  /'

  # The tiled pass allocates the whole accumulator, its halo rings and the
  # feathering weights, and it is the path a real render of a large frame takes -
  # so it gets its own memcheck run rather than being covered by the whole-frame
  # one. Same bound, not bit-equality: see the tolerance note above.
  echo "  tiled render under the same tool:"
  ONNX_MODEL_PATH="${ROOT}/models/test_conv.onnx" \
  valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite \
    --errors-for-leak-kinds=definite --track-origins=yes \
    "${TMP}/contract_test" "${BUNDLE_OFX}" "${TILED_ARGS[@]}" \
    > "${TMP}/vg-tiled.log" 2>&1 || {
      echo "  FAIL: memcheck reported errors on the tiled pass" >&2
      grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" "${TMP}/vg-tiled.log" | head -20 >&2
      exit 1
    }
  grep -E "contract_test:|ERROR SUMMARY|definitely lost|indirectly lost|possibly lost" \
    "${TMP}/vg-tiled.log" | sed 's/^==[0-9]*== /  /'
fi

echo
echo "all checks passed"
