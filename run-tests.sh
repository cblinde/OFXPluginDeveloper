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
#
# Skipping is not the same as passing, and the difference has to survive the
# log: this template's Windows build was broken for months while every run
# printed "all suites passed" and exited 0, because nothing failed - the
# template simply never ran. Three rules follow: the summary says what did NOT
# run, a CI run fails unless the hole is accepted explicitly, and the skip is
# never counted as a suite that passed.
onnx=1
if [[ -z "${ONNXRUNTIME_ROOT:-}" ]]; then
  onnx=0
  echo "[onnx] ONNXRUNTIME_ROOT is unset: templates/onnx-inference-ofx will NOT be built or tested"
  echo "       Point it at a directory with include/onnxruntime_cxx_api.h and"
  echo "       lib/libonnxruntime.so (see templates/onnx-inference-ofx/build.sh --help)."
fi

# The CUDA template runs in one of two modes, and the summary has to say which.
# CPU_ONLY=1 asks for the fallback path on purpose, and a machine without nvcc
# ends up there anyway; in neither case does a kernel run, so no summary line may
# claim one did. Same rule as the ONNX skip above: a hole is named, not rounded up
# to "passed".
gpu_mode="CUDA"
if [[ "${CPU_ONLY:-0}" == "1" ]]; then
  gpu_mode="CPU_ONLY"
elif ! command -v nvcc >/dev/null 2>&1; then
  gpu_mode="CPU_ONLY (no nvcc)"
  # build.sh exits 2 when asked for CUDA and there is no nvcc, by design; the
  # single command must still work on a machine without a toolkit.
  export CPU_ONLY=1
fi

# The line above the list. It names every hole the list then spells out: a run
# that skipped the ONNX template and the GPU step must not be summarised as if
# only one thing was missing.
summary_headline() {
  local holes=()
  [[ "${onnx}" == "1" ]] || holes+=("one template was not built")
  [[ "${gpu_mode}" == "CUDA" ]] || holes+=("the GPU step did not run")
  case ${#holes[@]} in
    0) echo "all suites passed, every template that was built:" ;;
    1) echo "all suites that ran passed, but ${holes[0]}:" ;;
    *) echo "all suites that ran passed, but ${holes[0]} and ${holes[1]}:" ;;
  esac
}

# A run that claims to cover everything is only allowed to stay green without
# the template when a human says so. CI defines CI=true, GitHub Actions
# GITHUB_ACTIONS, Azure Pipelines TF_BUILD and Buildkite BUILDKITE; any of them
# turns the skip into a failure, because that is where a silently unbuilt
# template hid.
in_ci() {
  [[ -n "${CI:-}${GITHUB_ACTIONS:-}${TF_BUILD:-}${BUILDKITE:-}" ]]
}

# The block a reader sees before the counts, so "all suites passed" is never the
# last word while one template was skipped.
report_skipped_onnx() {
  [[ "${onnx}" == "0" ]] || return 0
  echo
  echo "------------------------------------------------------------------"
  echo "NOT VERIFIED: templates/onnx-inference-ofx"
  echo "  ONNXRUNTIME_ROOT is unset, so nothing in that template was built or"
  echo "  tested: not its build, not its host contract, not its ai_pipeline test."
  echo "  The counts above do not cover it."
  echo "  Close it with: ONNXRUNTIME_ROOT=/path/to/onnxruntime ./run-tests.sh"
  echo "------------------------------------------------------------------"
}

# The GPU half of the CUDA template, when it was not exercised. It runs on the
# CPU fallback path, which is a real result - just not a kernel result.
report_skipped_gpu() {
  [[ "${gpu_mode}" != "CUDA" ]] || return 0
  echo
  echo "------------------------------------------------------------------"
  echo "NOT VERIFIED: GPU kernels (templates/minimal-cuda-ofx)"
  echo "  ${gpu_mode}: the template built and tested its CPU fallback only, so no"
  echo "  cubin was loaded and nothing ran on a device. The counts above do not"
  echo "  cover it."
  echo "  Close it with: ./run-tests.sh   (without CPU_ONLY, nvcc on PATH)"
  echo "------------------------------------------------------------------"
}

# Called after the summary, so a CI failure still carries the full result.
require_onnx_in_ci() {
  [[ "${onnx}" == "0" ]] || return 0
  in_ci || return 0
  if [[ "${ALLOW_SKIPPED_TEMPLATES:-0}" == "1" ]]; then
    echo "warning: CI without ONNXRUNTIME_ROOT, accepted by ALLOW_SKIPPED_TEMPLATES=1" >&2
    return 0
  fi
  echo "error: this is a CI run without ONNXRUNTIME_ROOT, so the" >&2
  echo "       onnx-inference-ofx template was never exercised. That is how its" >&2
  echo "       Windows build stayed broken while every run went green." >&2
  echo "       Provide ONNXRUNTIME_ROOT, or set ALLOW_SKIPPED_TEMPLATES=1 to" >&2
  echo "       accept the hole in this run on purpose." >&2
  exit 1
}

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
  echo "== negative controls: broken plug-ins must fail, the pattern must pass =="
  cmd //c "$(cygpath -w "${ROOT}/testkit/run-negative-controls.bat")"

  echo
  echo "== comparator self-test: the golden comparison must fail on a corrupt reference =="
  cmd //c "$(cygpath -w "${ROOT}/testkit/selftest-compare.bat")"
  echo
  echo
  echo "=================================================================="
  summary_headline
  echo "  docs drift check                (tools/check-docs.py)"
  echo "  reference build + artifact      (minimal-ofx, dumpbin)"
  echo "  artifact + fake host            (minimal-ofx, minimal-cuda-ofx)"
  if [[ "${gpu_mode}" == "CUDA" ]]; then
    echo "  GPU kernels                     (as test/run-tests.bat reported above)"
  else
    echo "  NOT RUN: GPU kernels            (${gpu_mode})"
  fi
  echo "  negative controls               (testkit/run-negative-controls.bat)"
  echo "  comparator self-test            (testkit/selftest-compare.bat)"
  if [[ "${onnx}" == "0" ]]; then
    echo "  NOT RUN: onnx-inference-ofx     (ONNXRUNTIME_ROOT unset)"
  fi
  echo "RUN_VALGRIND and SANITIZE are POSIX-only; this route has no valgrind"
  echo "=================================================================="
  report_skipped_onnx
  report_skipped_gpu
  require_onnx_in_ci
  exit 0
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo
echo "== build: minimal-cuda-ofx (${gpu_mode}) =="
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

# The controls assert their own failures and exit 0 when every fault is caught,
# so running them here keeps a green run green. They need a C compiler, which a
# suite run already implies; without one the stage says so instead of vanishing.
echo
if command -v cc >/dev/null 2>&1; then
  echo "== negative controls: broken plug-ins must fail, the pattern must pass =="
  "${ROOT}/testkit/run-negative-controls.sh"
  negative_controls="ran"
else
  echo "== negative controls (skipped: no cc) =="
  negative_controls="skipped"
fi

# The comparator's own test: the suites trust it, so it has to be shown failing
# on a corrupt reference before it is credited with passing a good one.
echo
echo "== comparator self-test: the golden comparison must fail on a corrupt reference =="
"${ROOT}/testkit/selftest-compare.sh"

echo
echo "=================================================================="
summary_headline
echo "  docs drift check                (tools/check-docs.py)"
echo "  comparator self-test            (testkit/selftest-compare.sh)"
echo "  reference build + artifact      (minimal-ofx)"
echo "  unit/equivalence  (cuda)        image/golden      (cuda, inference)"
echo "  artifact + fake host (all)      memory            (all)"
if [[ "${gpu_mode}" == "CUDA" ]]; then
  echo "  GPU kernels                     (minimal-cuda-ofx, on the device)"
else
  echo "  NOT RUN: GPU kernels            (${gpu_mode})"
fi
if [[ "${onnx}" == "0" ]]; then
  echo "  NOT RUN: onnx-inference-ofx     (ONNXRUNTIME_ROOT unset)"
fi
if [[ "${negative_controls}" == "ran" ]]; then
  echo "  negative controls               (testkit/run-negative-controls.sh)"
else
  echo "  NOT RUN: negative controls      (no cc)"
fi
echo "=================================================================="
report_skipped_onnx
report_skipped_gpu
require_onnx_in_ci
