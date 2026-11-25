#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Configure and build the ONNX inference plug-in.
#
#   ./build.sh [Release|Debug|RelWithDebInfo|MinSizeRel] [install]
#
# Environment:
#   ONNXRUNTIME_ROOT   root holding include/onnxruntime_cxx_api.h and
#                      lib/libonnxruntime.so (required; see below)
#   OFX_SOURCE_DIR     existing OpenFX checkout (default: fetch OFX_Release_1.5.1)
#   OFX_INSTALL_DIR    host plug-in directory for the `install` step
#   BUILD_DIR          build directory (default: ./build)
#
# Where to get ONNX Runtime:
#   * release tarball, the documented C/C++ route:
#       https://github.com/microsoft/onnxruntime/releases
#       -> onnxruntime-linux-x64-<version>.tgz  (CPU)  ~10 MB
#   * a pip wheel, which ships the .so but NOT the headers:
#       pip download --no-deps onnxruntime -d /tmp/ort
#       cd /tmp/ort && unzip -q onnxruntime-*.whl
#     then add the headers from the same release tag (see
#     references/ai-architectures-and-runtimes.md, "Getting the runtime").

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="Release"
DO_INSTALL=0

for arg in "$@"; do
  case "${arg}" in
    Release|Debug|RelWithDebInfo|MinSizeRel) BUILD_TYPE="${arg}" ;;
    install) DO_INSTALL=1 ;;
    -h|--help)
      sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown argument '${arg}' (try --help)" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  echo "error: ONNXRUNTIME_ROOT is not set." >&2
  echo "  It must contain include/onnxruntime_cxx_api.h and lib/libonnxruntime.so." >&2
  echo "  Download onnxruntime-linux-x64-<ver>.tgz from" >&2
  echo "  https://github.com/microsoft/onnxruntime/releases and extract it." >&2
  exit 2
fi

if [[ ! -f "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h" &&
      ! -f "${ONNXRUNTIME_ROOT}/onnxruntime/capi/onnxruntime_cxx_api.h" ]]; then
  echo "error: ${ONNXRUNTIME_ROOT} has no onnxruntime_cxx_api.h (include/ or onnxruntime/capi/)" >&2
  exit 2
fi

BUILD_DIR="${BUILD_DIR:-${HERE}/build}"
CMAKE_ARGS=(-S "${HERE}" -B "${BUILD_DIR}" "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
            "-DONNXRUNTIME_ROOT=${ONNXRUNTIME_ROOT}")
if [[ -n "${OFX_SOURCE_DIR:-}" ]]; then
  CMAKE_ARGS+=("-DOFX_SOURCE_DIR=${OFX_SOURCE_DIR}")
fi
if [[ -n "${OFX_INSTALL_DIR:-}" ]]; then
  CMAKE_ARGS+=("-DOFX_INSTALL_DIR=${OFX_INSTALL_DIR}")
fi
# The CUDA bundle switches, forwarded the same way build.bat does: an exported
# OFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON that never reached CMake produced a bundle
# with no provider, and the suite accepted the CPU fallback.
if [[ -n "${OFXPLUGIN_BUNDLE_CUDA_PROVIDER:-}" ]]; then
  CMAKE_ARGS+=("-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=${OFXPLUGIN_BUNDLE_CUDA_PROVIDER}")
fi
if [[ -n "${OFXPLUGIN_CUDA_RUNTIME_DIRS:-}" ]]; then
  CMAKE_ARGS+=("-DOFXPLUGIN_CUDA_RUNTIME_DIRS=${OFXPLUGIN_CUDA_RUNTIME_DIRS}")
fi

echo "[build] configuring (${BUILD_TYPE})"
cmake "${CMAKE_ARGS[@]}"

echo "[build] compiling"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

BUNDLE="${BUILD_DIR}/bundle/OnnxInference.ofx.bundle"
OFX_BIN="$(find "${BUNDLE}" -type f -name '*.ofx' | head -1)"
if [[ -z "${OFX_BIN}" ]]; then
  echo "error: the build produced no .ofx under ${BUNDLE}" >&2
  exit 1
fi

command -v nm >/dev/null 2>&1 || die "nm is not installed, and this step proves the artifact with it; on Windows run build.bat (dumpbin) or test/test.bat"
EXPORTS="$(nm -D --defined-only "${OFX_BIN}" | grep -c ' T ' || true)"
if [[ "${EXPORTS}" != "2" ]]; then
  echo "error: the module exports ${EXPORTS} dynamic symbols, expected exactly 2" >&2
  nm -D --defined-only "${OFX_BIN}" | grep ' T ' >&2 || true
  exit 1
fi

# Same two artifact facts the test asserts, so a plain ./build.sh cannot report
# success on a bundle a host would reject. readelf ships with the binutils the
# rest of this build already needs (nm).
if command -v readelf >/dev/null 2>&1; then
  NEEDED="$(readelf -d "${OFX_BIN}" | grep -c 'libonnxruntime' || true)"
  # shellcheck disable=SC2016  # matching the literal text $ORIGIN, not expanding it
  RUNPATH="$(readelf -d "${OFX_BIN}" | grep -cF '$ORIGIN' || true)"
  if [[ "${NEEDED}" != "1" || "${RUNPATH}" != "1" ]]; then
    echo "error: ${OFX_BIN} must need libonnxruntime once and record \$ORIGIN" >&2
    echo "       (NEEDED=${NEEDED}, \$ORIGIN entries=${RUNPATH})" >&2
    exit 1
  fi
fi

echo
echo "Bundle: ${BUNDLE}"
echo "  ${OFX_BIN}"
echo "  exported dynamic symbols: ${EXPORTS} (expected 2)"
echo "  ONNX Runtime beside the .ofx: $(find "$(dirname "${OFX_BIN}")" -name 'libonnxruntime.*' | wc -l) file(s)"

if [[ "${DO_INSTALL}" == "1" ]]; then
  cmake --build "${BUILD_DIR}" --target install-bundle
fi
