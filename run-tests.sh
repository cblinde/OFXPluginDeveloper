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
# unproven is listed in references/testing-layers.md.
#
# On Windows the shell suites cannot run - nm, readelf, g++, dlopen and valgrind
# are not there - so this script dispatches to the .bat runners, which perform
# the same checks with dumpbin, cuobjdump, MSVC and LoadLibraryExW.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Documentation is part of the deliverable: fail the run when the prose drifts
# from the tree (links, anchors, counts, duplicated measurements, provenance).
echo
echo "== docs: check-docs =="
# "command -v python3" is not enough on Windows: Git Bash resolves it to the
# Microsoft Store stub, which prints an install advert and exits non-zero. Ask
# each candidate to run, and fall back to "python" when it answers.
documentation_python=""
for candidate in python3 python; do
  if command -v "${candidate}" >/dev/null 2>&1 &&
     "${candidate}" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 6) else 1)' >/dev/null 2>&1; then
    documentation_python="${candidate}"
    break
  fi
done
if [[ -z "${documentation_python}" ]]; then
  echo "error: python3 (or python) is required to check the documentation" >&2
  exit 2
fi
"${documentation_python}" "${ROOT}/tools/check-docs.py"

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
# Platform
# ---------------------------------------------------------------------------
# Everything below is POSIX: nm, readelf, g++, dlopen, valgrind. A Windows
# developer shell has none of them, and the suite used to die at the first
# missing tool - after building the plug-in successfully - which reads as a
# broken build. The same checks are written a second time as .bat runners
# (dumpbin, cuobjdump, MSVC, LoadLibraryExW), so on Windows this command hands
# the work to them instead of failing. What is promised stays the same: every
# template built, every suite that can run on this machine run.
case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    windows=1
    ;;
  *)
    windows=0
    ;;
esac

if [[ "${windows}" == "1" ]]; then
  echo
  echo "[platform] Windows: using the .bat runners (no nm, readelf or g++ here)"
  echo
  echo "== reference plug-in: minimal-ofx (build, artifact, contract) =="
  cmd //c "$(cygpath -w "${ROOT}/templates/minimal-ofx/test/run-tests.bat")" Release

  echo
  echo "== test: minimal-cuda-ofx (build, artifact, contract) =="
  cmd //c "$(cygpath -w "${ROOT}/templates/minimal-cuda-ofx/test/run-tests.bat")" Release

  if [[ "${onnx}" == "1" ]]; then
    echo
    echo "== build + test: onnx-inference-ofx =="
    ( cd "${ROOT}/templates/onnx-inference-ofx" && cmd //c "$(cygpath -w "${ROOT}/templates/onnx-inference-ofx/build.bat")" Release )
    cmd //c "$(cygpath -w "${ROOT}/templates/onnx-inference-ofx/test/test.bat")"
  else
    echo
    echo "== test: onnx-inference-ofx (skipped: ONNXRUNTIME_ROOT unset) =="
  fi

  echo
  echo "=================================================================="
  echo "all suites passed, every template that was built:"
  echo "  docs drift check                (tools/check-docs.py)"
  echo "  reference build + artifact      (minimal-ofx, dumpbin)"
  echo "  artifact + fake host            (minimal-ofx, minimal-cuda-ofx)"
  echo "  GPU kernels                     (skipped under CPU_ONLY=1)"
  if [[ "${onnx}" == "0" ]]; then
    echo "  skipped: onnx-inference-ofx     (ONNXRUNTIME_ROOT unset)"
  fi
  echo "Negative controls are separate: testkit/run-negative-controls.bat"
  echo "=================================================================="
  exit 0
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
