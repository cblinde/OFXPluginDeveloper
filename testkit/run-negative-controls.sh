#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Proves that contract_test detects faults instead of merely passing.
#
# Two deliberately broken .ofx files are built from stub_bad_status.c and
# stub_bad_type.c, and each is fed to the same harness the templates use. The
# script succeeds only if the harness exits non-zero *and* names the assertion.
#
# This is intentionally NOT wired into ../run-tests.sh: a normal run must stay
# green, and a run that always has two failing tests would train people to
# ignore red output. Run it by hand after changing the harness:
#
#   testkit/run-negative-controls.sh [/path/to/openfx]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
OPENFX="${1:-${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

BIN="${TMP}/contract_test"
"${HERE}/build-contract-test.sh" "${BIN}" "${OPENFX}"
INC="${OPENFX}/include"

cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/bad_status.ofx" "${HERE}/negative-controls/stub_bad_status.c"
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/bad_type.ofx" "${HERE}/negative-controls/stub_bad_type.c"

fail=0
run_control()
{
  local title="$1" expected="$2"; shift 2
  local output status
  output="$("${BIN}" "$@" 2>&1)"
  status=$?
  printf '\n--- negative control: %s ---\n' "${title}"
  printf '%s\n' "${output}" | sed 's/^/  /'
  if [[ ${status} -eq 0 ]]; then
    echo "  CONTROL FAILED TO FAIL: contract_test exited 0" >&2
    fail=1
    return
  fi
  if [[ "${output}" != *"${expected}"* ]]; then
    echo "  CONTROL FAILED: output does not name '${expected}'" >&2
    fail=1
    return
  fi
  echo "  caught (exit ${status}) with: ${expected}"
}

run_control "describe returns kOfxStatReplyDefault" "FAIL kOfxActionDescribe: kOfxStatReplyDefault" \
  "${TMP}/bad_status.ofx" --id com.testkit.negative.badstatus --param gain:double

run_control "parameter declared with the wrong type" "FAIL param 'gain' type: OfxParamTypeInteger (expected OfxParamTypeDouble)" \
  "${TMP}/bad_type.ofx" --id com.testkit.negative.badtype --param gain:double

echo
if [[ ${fail} -ne 0 ]]; then
  echo "negative controls FAILED: the harness does not detect the faults above" >&2
  exit 1
fi
echo "negative controls: both faults detected, harness is not a rubber stamp"
