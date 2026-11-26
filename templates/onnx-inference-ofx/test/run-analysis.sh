#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Static analysis over the template: no GPU, no host, no ONNX Runtime, no build.
#
#   cppcheck    src/*.cpp and test/ (headers are analysed through the translation
#               units that include them)
#   scripts     every script in the template (linted with shellcheck)
#
# Two things are deliberately out of cppcheck's reach, and both are covered
# elsewhere:
#
#   * `onnxruntime_cxx_api.h` is in the runtime root, not in this tree, so the
#     analysis sees ModelRunner.cpp without it and `missingInclude` is suppressed
#     for that reason. Tier 2 of the suite is what compiles and runs that code -
#     it links the real runtime and compares a whole frame against the golden.
#   * `tools/make-test-model.py` is Python, so shellcheck has nothing to say
#     about it; `tools/check-docs.py` and the fixture stage are its gates.
#
# A missing tool is reported and skipped so this stays usable on a bare machine;
# set REQUIRE_ANALYSIS=1 to make a missing tool fatal (what CI should do).
#
# Intentional suppressions are inline, next to the code, and each one names its
# reason. They are tabulated in references/checklists.md.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
REQUIRE="${REQUIRE_ANALYSIS:-0}"
STATUS=0
RAN=0
SKIPPED=0

missing()
{
  if [[ "${REQUIRE}" == "1" ]]; then
    echo "error: $1 not on PATH (REQUIRE_ANALYSIS=1)" >&2
    STATUS=1
  else
    echo "  skipped: $1 not on PATH ($2)"
    SKIPPED=$((SKIPPED + 1))
  fi
}

echo "== cppcheck =="
if command -v cppcheck >/dev/null 2>&1; then
  RAN=$((RAN + 1))
  cppcheck --version
  (
    cd "${ROOT}"
    cppcheck --std=c++17 --language=c++ \
      --enable=warning,style,performance,portability --inconclusive \
      --inline-suppr --quiet --error-exitcode=1 \
      --suppress=missingIncludeSystem --suppress=missingInclude \
      -I src -I test \
      src test
  ) || STATUS=1
else
  missing cppcheck "Debian/Ubuntu: apt install cppcheck"
fi

echo
echo "== shellcheck =="
if command -v shellcheck >/dev/null 2>&1; then
  RAN=$((RAN + 1))
  shellcheck --version | grep -E '^version:'
  ( cd "${ROOT}" && shellcheck -x --severity=style build.sh test/run-tests.sh \
      test/run-analysis.sh ) || STATUS=1
else
  missing shellcheck "Debian/Ubuntu: apt install shellcheck"
fi

echo
if [[ "${STATUS}" != "0" ]]; then
  echo "static analysis: findings above" >&2
elif [[ "${SKIPPED}" != "0" ]]; then
  # A run that skipped a tool has not found the code clean, it has not looked at
  # it. Saying "clean" here is how a missing cppcheck turns into a green build.
  echo "static analysis: incomplete - ${RAN} tool(s) ran, ${SKIPPED} skipped (REQUIRE_ANALYSIS=1 makes that fatal)"
else
  echo "static analysis: clean"
fi
exit "${STATUS}"
