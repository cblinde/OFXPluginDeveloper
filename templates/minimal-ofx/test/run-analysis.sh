#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Static analysis over the template: no GPU, no host, no build required.
#
#   cppcheck    src/Plugin.cpp and test/ (headers are analysed through the
#               translation unit that includes them)
#   scripts     every script in the template (linted with shellcheck)
#
# The OFX SDK headers are not on this machine's include path, so cppcheck sees
# the plug-in without them: `missingInclude` is suppressed for that reason, and
# what the analysis covers is this repository's own code - the arithmetic, the
# parameter handling, the buffers - not the SDK's declarations. The build and the
# suite are what check that the SDK agrees with what this file calls.
#
# A missing tool is reported and skipped so this stays usable on a bare machine;
# set REQUIRE_ANALYSIS=1 to make a missing tool fatal (what CI should do).
#
# Intentional suppressions are inline, next to the code they cover, and the
# reason for each one is tabulated in references/checklists.md section 2.
# Nothing else is suppressed: the --suppress flags above cover headers that
# are not on this machine, never a check.

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
      -I src \
      src/Plugin.cpp test
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
