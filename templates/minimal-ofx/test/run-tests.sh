#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The reference plug-in's test: build it (which already inspects the artifact),
# then drive the shared contract harness through the whole host protocol -
# load, describe, describeInContext, createInstance, render, destroy, unload -
# twice. When valgrind is installed the same harness runs under memcheck, once
# per host profile, because the reference plug-in is the one every other
# template is compared against and a leak there would be copied.
#
#   ./run-tests.sh
#   SANITIZE=address,undefined ./run-tests.sh
#   RUN_VALGRIND=0 ./run-tests.sh      # skip memcheck
#   RUN_VALGRIND=1 ./run-tests.sh      # require it
#
# The harness lives in testkit/ so both templates certify the same behaviour.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${TPL}/../.." && pwd)"

echo "[1/4] building the reference plug-in (and verifying the artifact)"
"${TPL}/build.sh"

OFX_FILE="$(find "${TPL}/build/bundle" -name '*.ofx' -type f | head -1)"
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"
HARNESS="${TPL}/build/contract_test"

# The sanitizer is named in the log, like the other two suites do: a run whose
# header says "none" must not be mistaken for an instrumented one, and the CI
# job greps this line to prove the build it credits was built that way.
echo "[2/4] compiling the shared contract harness (sanitizer: ${SANITIZE:-none})"
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

# One list, used by the plain run and by the memcheck run below: two copies of
# eleven flags drift, and the one that drifts is the one memcheck covers.
HARNESS_ARGS=(
  --id com.ofxplugindeveloper.referencegain
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

echo "[3/4] running the host protocol against ${OFX_FILE}"
for profile in "${HOST_PROFILES[@]}"; do
  echo "  host profile: ${profile}"
  "${HARNESS}" "${OFX_FILE}" --host-profile "${profile}" "${HARNESS_ARGS[@]}"
done

echo
echo "== [4/4] valgrind memcheck =="
WANT="${RUN_VALGRIND:-auto}"
if [[ "${WANT}" == "0" ]]; then
  echo "  skipped: RUN_VALGRIND=0"
elif [[ -n "${SANITIZE:-}" ]]; then
  echo "  skipped: a -fsanitize build does not run under memcheck (both replace the"
  echo "  malloc/thread runtime, and valgrind rejects the combination)"
elif ! command -v valgrind >/dev/null 2>&1; then
  if [[ "${WANT}" == "1" ]]; then
    echo "error: RUN_VALGRIND=1 but valgrind is not on PATH" >&2
    exit 2
  fi
  echo "  skipped: valgrind not on PATH (install valgrind; on Debian/Ubuntu the"
  echo "  tracebacks want libc6-dbg as well)"
else
  # Separate, unsanitised harness: memcheck replaces the allocator itself. The
  # plug-in image is the ordinary build either way - run-tests.sh never builds
  # one with a sanitizer - so what memcheck walks is the same program a host
  # loads, with the harness's own bookkeeping in the way.
  HARNESS_VG="${TPL}/build/contract_test-memcheck"
  SANITIZE="" "${ROOT}/testkit/build-contract-test.sh" "${HARNESS_VG}" "${OFX_DIR}"
  for profile in "${HOST_PROFILES[@]}"; do
    echo "  host profile: ${profile}"
    valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=definite \
      --errors-for-leak-kinds=definite --track-origins=yes \
      "${HARNESS_VG}" "${OFX_FILE}" --host-profile "${profile}" "${HARNESS_ARGS[@]}" \
      > "${TPL}/build/vg-${profile}.log" 2>&1 || {
        echo "  FAIL: memcheck reported errors" >&2
        grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost|indirectly lost" \
          "${TPL}/build/vg-${profile}.log" | head -20 >&2
        exit 1
      }
    grep -E "contract_test:|ERROR SUMMARY|definitely lost|indirectly lost|possibly lost" \
      "${TPL}/build/vg-${profile}.log" | sed 's/^==[0-9]*== /  /'
  done
fi
