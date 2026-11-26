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
#   templates/minimal-opencl-ofx/test/run-tests.sh  (needs an OpenCL device)
#
# Environment:
#   OFX_SOURCE_DIR     openfx checkout (default: ./.ofx-ref when present,
#                      otherwise each template fetches OFX_Release_1.5.1)
#   ONNXRUNTIME_ROOT   optional: without it the ONNX template is skipped by
#                      name, not silently (see its build.sh --help)
#   CPU_ONLY=1         force the CUDA template to build its CPU path only
#   RUN_VALGRIND       auto | 1 | 0 (forwarded to both suites)
#   SANITIZE           e.g. address,undefined (forwarded; skips memcheck)
#   GLIBC_MAX_*        per-bundle glibc bound (defaults below; see
#                      references/cmake-and-linking.md section 5)
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
# A project .venv comes first, so the interpreter the suite is pinned to is the
# one that runs. Then: "command -v python3" is not enough on Windows, because Git
# Bash resolves it to the Microsoft Store stub, which prints an install advert
# and exits non-zero. Ask each candidate to run, and fall back to "python".
documentation_python=""
for candidate in "${ROOT}/.venv/Scripts/python.exe" "${ROOT}/.venv/bin/python" python3 python; do
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
# The ONNX Runtime pin is documentation too: one `default:` in the runtime
# action, and every document that names a version path has to agree with it. It
# is one of check-docs.py's checks, so the gate cannot run the drift check and
# skip the pin.
echo
echo "== docs =="
"${documentation_python}" "${ROOT}/tools/check-docs.py"
# The inventory rules - template list, scaffold tables, reference paths, CI
# coverage and negated host claims - are shown failing on a synthetic tree before
# a clean one is credited, the same rule the other self-tests here follow.
echo
"${documentation_python}" "${ROOT}/tools/check-docs.py" --selftest

# Every tracked shell script is parsed before anything is built, so a syntax
# error in a script this run does not happen to call - a `run-analysis.sh`, the
# OpenCL `build.sh` - fails here instead of on the machine that first runs it.
# The same tool runs in docs.yml, from the same list, so CI and this cannot
# disagree about what was checked.
echo
echo "== shell syntax =="
"${ROOT}/tools/check-shell.sh"

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
opencl="ran"
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

# The OpenCL template's suite, when the machine had no device to hand over.
report_skipped_opencl() {
  [[ "${opencl:-ran}" == "no-device" ]] || return 0
  echo
  echo "------------------------------------------------------------------"
  echo "NOT A PASS: templates/minimal-opencl-ofx - NOT VERIFIED"
  echo "  No OpenCL platform or device: the plug-in was built and its artifact"
  echo "  checked, and the suite exited 2 without rendering anything through a"
  echo "  queue. The counts above do not cover its device path, so this run is not"
  echo "  evidence that the device path works."
  echo "  Close it with: apt install pocl-opencl-icd libpocl2-common"
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

# The self-tests every route runs: five checks of a tool or a module, none of
# which needs a host, a GPU or a C compiler. The halo check hashes a file, the
# glibc one feeds a canned readelf, the Resolve ones are plain Python, and the
# fixture self-test needs one package. Written once, because both routes run the
# same list.
run_tool_self_tests() {
  echo
  echo "== halo value: the stored overlap must still belong to this model =="
  "${ROOT}/tools/measure-halo.sh" \
    "${ROOT}/templates/onnx-inference-ofx/models/test_conv.onnx" --check

  # A comparison can regress while every template stays unchanged, so the tool
  # that compares glibc versions is shown failing before it is credited.
  echo
  echo "== glibc bound self-test: the comparison must fail on a bound that is too low =="
  "${ROOT}/tools/check-glibc.sh" --max 2.17 --selftest

  # The workflows are the one part of the tree nothing here compiles: a floating
  # action tag or an indentation error otherwise surfaces in CI, after the push.
  echo
  echo "== workflow pins: a tag-pinned or malformed workflow must be reported =="
  "${documentation_python}" "${ROOT}/tools/check-workflows.py" --selftest
  "${documentation_python}" "${ROOT}/tools/check-workflows.py"

  # A provider is refused by a host whose CUDA major differs, and the host's
  # report is `Error 1114` with no file named. The gate reads the major from the
  # provider's import names, so its fixtures are the 13-against-12 pair a host
  # rejects and the 12-against-12 pair it accepts.
  echo
  echo "== runtime inspection: a CUDA 13 provider must be named against a CUDA 12 host =="
  "${documentation_python}" "${ROOT}/tools/check-runtime.py" --self-test

  # The probe decides which templates and suites a machine may use, so its
  # decision table is shown failing for a missing capability before a "ready"
  # is credited.
  echo
  echo "== environment probe: a missing capability must block its suite =="
  "${documentation_python}" "${ROOT}/tools/probe-env.py" --self-test

  # The Resolve suite needs a host; its two host-free halves do not.
  echo
  echo "== resolve pixel checks: a uniform, black or wrong-sized frame must fail =="
  "${documentation_python}" "${ROOT}/tests/resolve/checks.py"
  echo
  echo "== resolve stale-file test: a second run in one directory must not pass =="
  "${documentation_python}" "${ROOT}/tests/resolve/test_stale_file.py"

  # Writing the fixtures and loading them back is a test of the tool. It imports
  # all three packages, so all three are asked for in one line - a missing one
  # would otherwise end the run inside an import traceback instead of naming the
  # hole. Past the gate the tool's exit code is the verdict.
  echo
  if ! "${documentation_python}" -c 'import numpy, onnx, onnxruntime' >/dev/null 2>&1; then
    echo "== test fixtures (skipped: numpy, onnx or onnxruntime is not installed)"
    fixtures="skipped"
    return 0
  fi
  echo "== test fixtures: synthetic models must load and match their shapes =="
  "${documentation_python}" "${ROOT}/tools/make-testmodels.py" --out "${ROOT}/build/test-fixtures"
  fixtures="ran"
}

summary_tool_tests() {
  echo "  halo value check                (models/test_conv.onnx)"
  echo "  glibc bound self-test           (tools/check-glibc.sh --selftest)"
  echo "  environment probe self-test     (tools/probe-env.py --self-test)"
  echo "  runtime inspection self-test    (tools/check-runtime.py --self-test)"
  echo "  resolve pixel checks            (tests/resolve/checks.py)"
  echo "  resolve stale-file test         (tests/resolve/test_stale_file.py)"
  if [[ "${fixtures}" == "ran" ]]; then
    echo "  test fixtures                   (tools/make-testmodels.py)"
  else
    echo "  NOT RUN: test fixtures          (numpy, onnx or onnxruntime is missing)"
  fi
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

# The same rule for the fourth template: on a CI machine a missing device is a
# suite that did not run, not a machine that did its best. The block above says
# what is missing; this makes the step red, because a green tick over a device
# path nobody exercised is the reading that block exists to prevent.
require_opencl_in_ci() {
  [[ "${opencl:-ran}" == "no-device" ]] || return 0
  in_ci || return 0
  if [[ "${ALLOW_SKIPPED_TEMPLATES:-0}" == "1" ]]; then
    echo "warning: CI without an OpenCL device, accepted by ALLOW_SKIPPED_TEMPLATES=1" >&2
    return 0
  fi
  echo "error: NOT A PASS - this is a CI run without an OpenCL device, so the" >&2
  echo "       minimal-opencl-ofx suite rendered nothing through a queue. A job" >&2
  echo "       that ran it on a CPU device (pocl-opencl-icd) is the only pass." >&2
  echo "       Install one, or set ALLOW_SKIPPED_TEMPLATES=1 to accept the hole." >&2
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

  echo
  echo "== test: minimal-opencl-ofx (build, artifact, contract, OpenCL suite) =="
  # Exit 2 means "no OpenCL device on this machine", which the runner prints as
  # NOT A PASS. It is not a build failure, so the run continues and the summary
  # carries the distinction instead of the exit code deciding it.
  cmd //c "$(cygpath -w "${ROOT}/templates/minimal-opencl-ofx/test/run-tests.bat")"
  opencl_win=$?

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

  run_tool_self_tests

  echo
  echo "== comparator self-test: the golden comparison must fail on a corrupt reference =="
  cmd //c "$(cygpath -w "${ROOT}/testkit/selftest-compare.bat")"
  echo
  echo
  echo "=================================================================="
  summary_headline
  echo "  docs drift check                (tools/check-docs.py, incl. the ONNX Runtime pin)"
  echo "  reference build + artifact      (minimal-ofx, dumpbin)"
  echo "  artifact + fake host            (minimal-ofx, minimal-cuda-ofx)"
  # All four templates now have a .bat runner: the OpenCL one loads its module
  # with LoadLibraryA and the driver's OpenCL.dll where the POSIX script uses
  # dlopen and libOpenCL.so.1, so the suite that used to be named "POSIX-only"
  # runs here too - against a real device, or as a named NOT A PASS when the
  # machine has none.
  if [[ "${opencl_win:-0}" == "0" ]]; then
    echo "  OpenCL device suite             (minimal-opencl-ofx, contract_test + opencl_suite)"
  elif [[ "${opencl_win:-0}" == "2" ]]; then
    echo "  NOT RUN: OpenCL device suite    (minimal-opencl-ofx: no OpenCL device)"
  else
    echo "  FAILED: OpenCL device suite     (minimal-opencl-ofx, exit ${opencl_win})"
  fi
  if [[ "${gpu_mode}" == "CUDA" ]]; then
    echo "  GPU kernels                     (as test/run-tests.bat reported above)"
  else
    echo "  NOT RUN: GPU kernels            (${gpu_mode})"
  fi
  echo "  negative controls               (testkit/run-negative-controls.bat)"
  summary_tool_tests
  echo "  comparator self-test            (testkit/selftest-compare.bat)"
  if [[ "${onnx}" == "0" ]]; then
    echo "  NOT RUN: onnx-inference-ofx     (ONNXRUNTIME_ROOT unset)"
  fi
  echo "RUN_VALGRIND, SANITIZE and the glibc bound are POSIX-only: no valgrind,"
  echo "no readelf, and a PE image has no GLIBC_ version table"
  echo "=================================================================="
  report_skipped_onnx
  report_skipped_gpu
  if [[ "${opencl_win:-0}" == "2" ]]; then
    opencl="no-device"
  fi
  require_onnx_in_ci
  require_opencl_in_ci
  # 2 is "no device on this machine", which the summary already names; anything
  # else non-zero is a failed suite and has to leave the script red.
  [[ "${opencl_win:-0}" == "0" || "${opencl_win:-0}" == "2" ]] || exit 1
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

# The OpenCL template asks the machine for a device, and a machine without one
# has not failed - it has not run the suite. Exit 2 is the suite's word for that,
# so it becomes a named hole below instead of a red run.
echo
echo "== test: minimal-opencl-ofx (build, artifact, OpenCL suite) =="
opencl="ran"
set +e
"${ROOT}/templates/minimal-opencl-ofx/test/run-tests.sh"
status=$?
set -e
if [[ "${status}" == "2" ]]; then
  opencl="no-device"
elif [[ "${status}" != "0" ]]; then
  exit "${status}"
fi

echo
if [[ "${onnx}" == "1" ]]; then
  echo "== test: onnx-inference-ofx =="
  "${ROOT}/templates/onnx-inference-ofx/test/run-tests.sh"
else
  echo "== test: onnx-inference-ofx (skipped: ONNXRUNTIME_ROOT unset) =="
fi

# ---------------------------------------------------------------------------
# glibc bound
# ---------------------------------------------------------------------------
# The floor is a property of the bundle, not of the repository. The two plug-ins
# this repository builds from its own sources target the 2.17 baseline the OFX
# SDK is built against; the inference bundle ships a prebuilt ONNX Runtime that
# needs 2.28 and, built with this container's toolchain, links 2.32 in the
# plug-in itself. Each number is a claim in references/cmake-and-linking.md
# section 5, not a slice of a newer measurement.
#
# It runs here because a hand-run check found the inference bundle above the
# 2.17 floor on 2026-11-26 while this script stayed green: a bound that only
# appears in a checklist is a bound nobody enforces. Point GLIBC_MAX_* at another
# number when you target another platform - the default is the claim, the
# override is the experiment.
glibc_max_minimal="${GLIBC_MAX_MINIMAL_OFX:-2.17}"
glibc_max_cuda="${GLIBC_MAX_MINIMAL_CUDA:-2.17}"
glibc_max_onnx="${GLIBC_MAX_ONNX:-2.32}"

glibc_gate() {
  local name="$1" bound="$2"
  shift 2
  local bundle
  for bundle in "$@"; do
    if [[ ! -d "${bundle}" ]]; then
      echo "error: ${name}: no bundle at ${bundle}" >&2
      echo "       the build stage did not produce it, so nothing was checked" >&2
      exit 2
    fi
    echo "-- ${name}: glibc <= ${bound}"
    "${ROOT}/tools/check-glibc.sh" --max "${bound}" "${bundle}"
  done
}

echo
echo "== glibc bound: every built bundle against the floor its template targets =="
glibc_gate "minimal-ofx" "${glibc_max_minimal}" \
  "${ROOT}"/templates/minimal-ofx/build/bundle/*.ofx.bundle
glibc_gate "minimal-cuda-ofx" "${glibc_max_cuda}" \
  "${ROOT}"/templates/minimal-cuda-ofx/build/bundle/*.ofx.bundle
glibc_gate "minimal-opencl-ofx" "${glibc_max_minimal}" \
  "${ROOT}"/templates/minimal-opencl-ofx/build/bundle/*.ofx.bundle
if [[ "${onnx}" == "1" ]]; then
  glibc_gate "onnx-inference-ofx" "${glibc_max_onnx}" \
    "${ROOT}"/templates/onnx-inference-ofx/build/bundle/*.ofx.bundle
else
  echo "-- onnx-inference-ofx: NOT CHECKED (ONNXRUNTIME_ROOT unset, no bundle built)"
fi

# ---------------------------------------------------------------------------
# Artifact check, read back from the built image
# ---------------------------------------------------------------------------
# The suites each inspect their own bundle, but `scripts/check-artifact.sh` had
# no caller at all - which is how it kept an executable bit missing and a wrong
# export count for weeks while every run stayed green. It reads the artifact
# rather than the sources: the export table, the loader's dependencies, and,
# when the build had a toolkit, the device code in the image. The CUDA flag is
# passed only for a run that actually compiled one, so a CPU_ONLY build is not
# asked to prove cubins it does not have.
echo
echo "== artifact check: exports and dependents of every built .ofx =="
artifact_check() {
  local name="$1"
  local candidates=( "${ROOT}/templates/${name}/build/bundle/"*.ofx.bundle/Contents/*/*.ofx )
  local ofx="${candidates[0]}"
  if [[ ! -f "${ofx}" ]]; then
    echo "error: ${name}: no .ofx under build/bundle" >&2
    echo "       the build stage did not produce it, so nothing was checked" >&2
    exit 2
  fi
  if [[ "${name}" == "minimal-cuda-ofx" && "${gpu_mode}" == "CUDA" ]]; then
    "${ROOT}/scripts/check-artifact.sh" "${ofx}" --cuda
  else
    "${ROOT}/scripts/check-artifact.sh" "${ofx}"
  fi
}
artifact_check minimal-ofx
artifact_check minimal-cuda-ofx
artifact_check minimal-opencl-ofx
if [[ "${onnx}" == "1" ]]; then
  artifact_check onnx-inference-ofx
else
  echo "-- onnx-inference-ofx: NOT CHECKED (ONNXRUNTIME_ROOT unset, no bundle built)"
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

run_tool_self_tests

# The comparator's own test: the suites trust it, so it has to be shown failing
# on a corrupt reference before it is credited with passing a good one.
echo
echo "== comparator self-test: the golden comparison must fail on a corrupt reference =="
"${ROOT}/testkit/selftest-compare.sh"

echo
echo "=================================================================="
summary_headline
echo "  docs drift check                (tools/check-docs.py, incl. the ONNX Runtime pin)"
echo "  comparator self-test            (testkit/selftest-compare.sh)"
summary_tool_tests
echo "  reference build + artifact      (minimal-ofx)"
echo "  unit/equivalence  (cuda)        image/golden      (cuda, inference)"
echo "  artifact + fake host (all)      memory            (all)"
if [[ "${opencl}" == "ran" ]]; then
  echo "  OpenCL device path (PoCL)       render through a real context and queue"
else
  echo "  NOT RUN: OpenCL device path     (no OpenCL device on this machine)"
fi
echo "  glibc bound per bundle          (2.17 x3 / 2.32, cmake-and-linking.md §5)"
echo "  artifact check per bundle       (exports, dependents, device code)"
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
report_skipped_opencl
require_onnx_in_ci
require_opencl_in_ci
