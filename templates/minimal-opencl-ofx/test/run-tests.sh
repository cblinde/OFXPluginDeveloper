#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The OpenCL template's test: build the plug-in (which already inspects the
# artifact), run the shared contract harness over the CPU path - a host that
# offers no device still has to be able to use this plug-in - then compile the
# suite and run it with a real OpenCL context, queue, cl_mem buffers and image2d
# images from the host, and finally put both the contract harness and the suite
# through valgrind memcheck.
#
#   ./run-tests.sh
#   OPENCL_LOADER_LIB=/usr/lib/x86_64-linux-gnu/libOpenCL.so.1 ./run-tests.sh
#   RUN_VALGRIND=1 ./run-tests.sh     # require the memcheck stage
#
# Environment:
#   OPENCL_LOADER_LIB  the loader to dlopen        (default: the one below)
#   RUN_VALGRIND       1 = require memcheck, 0 = skip it, unset = run if installed
#   SUITE_THREADS      4 (default) | random - concurrent render threads
#
# The device is PoCL on this machine - a CPU implementation, reached through the
# ICD loader, with no vendor SDK and no CL headers installed. That is the point:
# the plug-in resolves the entry points at runtime, so what it gets tested
# against is the loader a render node has, not a stub.
#
# Exit status: 0 both paths pass, 1 a check failed, 2 the machine has no OpenCL
# device, so the suite did not run. 2 is not a pass and the log says so.
set -euo pipefail

# One memcheck pass, the same shape the other templates record: definite leaks
# and memory errors fail the run, reachable driver state does not.
VG=(valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite
    --errors-for-leak-kinds=definite --track-origins=yes)
memcheck_one()
{
  local label="$1" log="$2"
  shift 2
  if ! "${VG[@]}" "$@" > "${log}" 2>&1; then
    echo "  FAIL: memcheck reported errors in ${label}" >&2
    grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" "${log}" | head -20 >&2
    return 1
  fi
  echo "  ${label}:"
  grep -E "ERROR SUMMARY|definitely lost|indirectly lost" "${log}" \
    | sed 's/^==[0-9]*== /    /'
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${TPL}/../.." && pwd)"

echo "[1/7] building the OpenCL plug-in (and verifying the artifact)"
"${TPL}/build.sh"

OFX_FILE="$(find "${TPL}/build/bundle" -name '*.ofx' -type f | head -1)"
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"
SUITE="${TPL}/build/opencl_suite"
HARNESS="${TPL}/build/contract_test"

echo "[3/7] compiling the shared contract harness"
"${ROOT}/testkit/build-contract-test.sh" "${HARNESS}" "${OFX_DIR}"

# The same protocol every other template certifies, with the host never offering
# a device: kOfxImageEffectPropOpenCLEnabled stays 0, which is the CPU path this
# plug-in keeps for hosts that have no OpenCL at all.
# The path decision first: PathDecision.h links no OpenCL and no OFX header, so
# this binary runs on a machine with no driver, no loader and no SDK - the gates
# between the host's queue and the CPU loop are executed rather than reviewed.
echo "[2/7] the path decision (six gates, no device needed)"
"${TPL}/build/self-test/opencl_path_test"

echo "[4/7] the host protocol, on the CPU path (3 profiles)"

# SUITE_THREADS=random picks a different concurrent-render count per run, the way
# FATE's threads=randomN does: not for determinism, but so that a race has to
# show up in some run instead of in the one configuration someone hard-coded.
THREADS="${SUITE_THREADS:-4}"
if [[ "${THREADS}" == "random" ]]; then
  THREADS=$(( (RANDOM % 4) + 1 ))
  echo "  SUITE_THREADS=random: ${THREADS} concurrent render thread(s) this run"
fi

CONTRACT_ARGS=(
  --id com.ofxplugindeveloper.openclgain
  --version 1.0
  --param gain:double
  --clip Source
  --clip Output
  --byte-dst-rejected
  --src-bounds-inset 2 --dst-bounds-inset 2
  --negative-pitch
  --concurrency-param gain
  --concurrency-threads "${THREADS}"
)
for profile in nuke resolve natron; do
  echo "  host profile: ${profile}"
  "${HARNESS}" "${OFX_FILE}" --host-profile "${profile}" "${CONTRACT_ARGS[@]}"
done

# The one thing a machine with a device cannot otherwise reach: the override makes
# PathDecision.h's first gate answer CPU, the plug-in writes that decision into the
# Device parameter after a real render, and --label-contains fails the run if the
# label does not carry it.
echo "  host profile: resolve, OFXPLUGIN_OPENCL_MODE=cpu (forced CPU path)"
OFXPLUGIN_OPENCL_MODE=cpu "${HARNESS}" "${OFX_FILE}" --host-profile resolve \
  --id com.ofxplugindeveloper.openclgain --version 1.0 --param gain:double \
  --clip Source --clip Output \
  --label-param device \
  --label-contains "device=CPU - OFXPLUGIN_OPENCL_MODE=cpu was set"

echo "[5/7] compiling the suite against the shared fake host"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -I "${ROOT}/testkit" -I "${OFX_DIR}/include" \
  "${HERE}/opencl_suite.cpp" "${ROOT}/testkit/fake_host.cpp" \
  -o "${SUITE}" -ldl

echo "  finding the OpenCL loader"
if [[ -z "${OPENCL_LOADER_LIB:-}" ]]; then
  # The PoCL used for the recorded run sits in the checkout's validation tree;
  # a machine with a system OpenCL exports OPENCL_LOADER_LIB instead (or has
  # libOpenCL.so.1 where the dynamic loader finds it by name).
  POCL_LIB="${ROOT}/.validation/cl/usr/lib/x86_64-linux-gnu"
  if [[ -f "${POCL_LIB}/libOpenCL.so.1" ]]; then
    export LD_LIBRARY_PATH="${POCL_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    if [[ -d "${ROOT}/.validation/cl/etc/OpenCL/vendors" ]]; then
      export OCL_ICD_VENDORS="${ROOT}/.validation/cl/etc/OpenCL/vendors"
    fi
    export OPENCL_LOADER_LIB="${POCL_LIB}/libOpenCL.so.1"
  fi
fi
echo "  loader : ${OPENCL_LOADER_LIB:-<libOpenCL.so.1 by name>}"
echo "  ICD    : ${OCL_ICD_VENDORS:-<loader default>}"

echo "[6/7] running the OpenCL suite against ${OFX_FILE}"
set +e
"${SUITE}" "${OFX_FILE}"
status=$?
set -e
if [[ "${status}" == "2" ]]; then
  echo "no OpenCL device on this machine: the suite did not run (not a pass)" >&2
fi

# The suite drives a driver, so it is the interesting pass under memcheck: the
# device callbacks, the two programs, and the teardown that releases them. The
# contract harness runs here too, on the CPU path, because that is the pass the
# other templates record and this plug-in keeps that path for hosts without a
# device.
echo "[7/7] valgrind memcheck"
WANT="${RUN_VALGRIND:-auto}"
if [[ "${WANT}" == "0" ]]; then
  echo "  skipped: RUN_VALGRIND=0"
elif ! command -v valgrind >/dev/null 2>&1; then
  if [[ "${WANT}" == "1" ]]; then
    echo "error: RUN_VALGRIND=1 but valgrind is not on PATH" >&2
    exit 2
  fi
  echo "  skipped: valgrind not on PATH (install valgrind; on Debian/Ubuntu the"
  echo "  tracebacks want libc6-dbg as well, and PoCL's JIT wants its own image)"
else
  rc=0
  memcheck_one "contract harness, CPU path" "${TPL}/build/vg-contract.log" \
    "${HARNESS}" "${OFX_FILE}" --host-profile resolve "${CONTRACT_ARGS[@]}" || rc=1
  if [[ "${status}" == "0" ]]; then
    memcheck_one "the OpenCL suite, both handovers" "${TPL}/build/vg-suite.log" \
      "${SUITE}" "${OFX_FILE}" || rc=1
  else
    echo "  suite: skipped, it did not pass outside memcheck (exit ${status})"
  fi
  [[ "${rc}" == "0" ]] || exit 1
fi
exit "${status}"
