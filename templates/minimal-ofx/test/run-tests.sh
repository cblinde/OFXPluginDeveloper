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

# The sanitizer is named in the log, like the other two suites do: a run whose
# header says "none" must not be mistaken for an instrumented one, and the CI
# job greps this line to prove the build it credits was built that way.
echo "[2/3] compiling the shared contract harness (sanitizer: ${SANITIZE:-none})"
SANITIZE="${SANITIZE:-}" "${ROOT}/testkit/build-contract-test.sh" "${HARNESS}" "${OFX_DIR}"

# One process holds one host - OfxSetHost ignores every call after the first -
# so each host shape is a run of its own. The shapes are the dumps published in
# openfx-misc's README-hosts.txt (nuke: tiles and multi-resolution, temporal
# access; resolve: neither tiles nor multi-resolution, no temporal access) and
# the background renderer NatronRenderer presents (natron: tiles, isBackground=1,
# no UI). Only the capabilities the plug-in reads change; every window, RoI and
# pixel check is the same in all three, because a host with supportsTiles=0 still
# renders a window smaller than the frame when its own region of definition is.
HOST_PROFILES=(nuke resolve natron)

# SUITE_THREADS=random picks a different concurrent-render count per run, the way
# FATE's threads=randomN does: not for determinism, but so that a race has to
# show up in some run instead of in the one configuration someone hard-coded.
THREADS="${SUITE_THREADS:-4}"
if [[ "${THREADS}" == "random" ]]; then
  THREADS=$(( (RANDOM % 4) + 1 ))
  echo "  SUITE_THREADS=random: ${THREADS} concurrent render thread(s) this run"
fi

echo "[3/3] running the host protocol against ${OFX_FILE}"
for profile in "${HOST_PROFILES[@]}"; do
  echo "  host profile: ${profile}"
  "${HARNESS}" "${OFX_FILE}" \
    --host-profile "${profile}" \
    --id com.ofxplugindeveloper.referencegain \
    --version 1.0 \
    --param gain:double \
    --clip Source \
    --clip Output \
    --byte-dst-rejected \
    --src-bounds-inset 2 --dst-bounds-inset 2 \
    --negative-pitch \
    --concurrency-param gain \
    --concurrency-threads "${THREADS}"
done
