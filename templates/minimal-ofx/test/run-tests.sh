#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The reference plug-in's test: build it (which already inspects the artifact),
# then drive the shared contract harness through the whole host protocol -
# load, describe, describeInContext, createInstance, render, destroy, unload -
# twice.
#
#   ./run-tests.sh
#   SANITIZE=address,undefined ./run-tests.sh
#
# The harness lives in testkit/ so both templates certify the same behaviour.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${TPL}/../.." && pwd)"

echo "[1/3] building the reference plug-in (and verifying the artifact)"
"${TPL}/build.sh"

OFX_FILE="$(find "${TPL}/build/bundle" -name '*.ofx' -type f | head -1)"
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"
HARNESS="${TPL}/build/contract_test"

echo "[2/3] compiling the shared contract harness"
SANITIZE="${SANITIZE:-}" "${ROOT}/testkit/build-contract-test.sh" "${HARNESS}" "${OFX_DIR}"

echo "[3/3] running the host protocol against ${OFX_FILE}"
"${HARNESS}" "${OFX_FILE}" \
  --id com.ofxplugindeveloper.referencegain \
  --version 1.0 \
  --param gain:double \
  --clip Source \
  --clip Output \
  --byte-dst-rejected \
  --negative-pitch \
  --concurrency-param gain
