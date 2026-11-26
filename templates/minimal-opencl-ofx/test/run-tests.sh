#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The OpenCL template's test: build the plug-in (which already inspects the
# artifact), run the shared contract harness over the CPU path - a host that
# offers no device still has to be able to use this plug-in - then compile the
# suite and run it with a real OpenCL context, queue and cl_mem buffers from the
# host.
#
#   ./run-tests.sh
#   OPENCL_LOADER_LIB=/usr/lib/x86_64-linux-gnu/libOpenCL.so.1 ./run-tests.sh
#
# The device is PoCL on this machine - a CPU implementation, reached through the
# ICD loader, with no vendor SDK and no CL headers installed. That is the point:
# the plug-in resolves the entry points at runtime, so what it gets tested
# against is the loader a render node has, not a stub.
#
# Exit status: 0 both paths pass, 1 a check failed, 2 the machine has no OpenCL
# device, so the suite did not run. 2 is not a pass and the log says so.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${TPL}/../.." && pwd)"

echo "[1/5] building the OpenCL plug-in (and verifying the artifact)"
"${TPL}/build.sh"

OFX_FILE="$(find "${TPL}/build/bundle" -name '*.ofx' -type f | head -1)"
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"
SUITE="${TPL}/build/opencl_suite"
HARNESS="${TPL}/build/contract_test"

echo "[2/5] compiling the shared contract harness"
"${ROOT}/testkit/build-contract-test.sh" "${HARNESS}" "${OFX_DIR}"

# The same protocol every other template certifies, with the host never offering
# a device: kOfxImageEffectPropOpenCLEnabled stays 0, which is the CPU path this
# plug-in keeps for hosts that have no OpenCL at all.
echo "[3/5] the host protocol, on the CPU path (3 profiles)"
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
  --concurrency-threads 4
)
for profile in nuke resolve natron; do
  echo "  host profile: ${profile}"
  "${HARNESS}" "${OFX_FILE}" --host-profile "${profile}" "${CONTRACT_ARGS[@]}"
done

echo "[4/5] compiling the suite against the shared fake host"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -I "${ROOT}/testkit" -I "${OFX_DIR}/include" \
  "${HERE}/opencl_suite.cpp" "${ROOT}/testkit/fake_host.cpp" \
  -o "${SUITE}" -ldl

echo "[4/5] finding the OpenCL loader"
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

echo "[5/5] running the OpenCL suite against ${OFX_FILE}"
set +e
"${SUITE}" "${OFX_FILE}"
status=$?
set -e
if [[ "${status}" == "2" ]]; then
  echo "no OpenCL device on this machine: the suite did not run (not a pass)" >&2
fi
exit "${status}"
