#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The comparator's own test. A check is not trusted until it has been seen
# failing on a defect it exists to catch (plumbline's rule, and the reason
# FFmpeg's refcmp had to learn to fail on empty input), so this runs the truth
# table a golden comparison has to satisfy against the same function the suites
# call:
#
#   identical                          -> pass, reported identical
#   one sub-LSB pixel                  -> fail exact, pass a 1e-5 bound
#   a large deviation                  -> fail even a loose bound
#   every pixel under / over the bound -> pass / fail
#   a reference of the wrong size      -> a categorical failure
#   NaN in the reference, inf in frame -> fail at any bound
#   a reference corrupt at one pixel   -> fail
#   a platform variant beside generic  -> the variant wins, and the log says so
#   a `.fuzz` sidecar                  -> supplies the bound; CLI still overrides
#
# Each case names the verdict it expects, so the script exits 1 when the
# comparator stops failing. Exit 2 means the harness could not be built.
#
#   ./testkit/selftest-compare.sh
#   OFX_SOURCE_DIR=/path/to/openfx ./testkit/selftest-compare.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "== comparator self-test (contract_test --selftest-compare) =="
if [[ ! -f "${OFX_DIR}/include/ofxCore.h" && ! -f "${OFX_DIR}/ofxCore.h" ]]; then
  echo "error: no OpenFX checkout at ${OFX_DIR}; set OFX_SOURCE_DIR" >&2
  exit 2
fi

SANITIZE="${SANITIZE:-}" "${HERE}/build-contract-test.sh" "${TMP}/contract_test" "${OFX_DIR}"
mkdir -p "${TMP}/lookup"
"${TMP}/contract_test" --selftest-compare "${TMP}/lookup"

echo "every case behaved: the comparison passes what it must and fails what it must"
