#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Parse every shell script in the tree, without running any of them.
#
# Why the whole list and not five names: docs.yml used to spell out
# `bash -n run-tests.sh` plus four template scripts, and the tree holds 22
# others - every `run-analysis.sh`, `testkit/selftest-compare.sh`, the OpenCL
# `build.sh`, `tools/aot-compile-model.sh`. A syntax error in one of those
# reached main because nothing parsed it. `git ls-files` is the list now: a file
# that is not in git is not shipped, and a new script is checked the moment it is
# added, in CI and in `run-tests.sh` at the same time.
#
#   tools/check-shell.sh              # every tracked *.sh
#   tools/check-shell.sh --selftest   # prove a broken script is reported
#
# `bash -n` reads and parses, it does not execute: a script that needs a compiler,
# a device or a host is still checked here.
#
# Exit codes: 0 every script parses; 1 at least one does not; 2 setup error (no
# bash, or no script to check at all - an empty list is not a pass).
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# One file: parse it, print what bash said, return its verdict.
parse() {
  local output
  if ! output=$(bash -n "$1" 2>&1); then
    printf '%s\n' "$output" | sed 's/^/    /'
    return 1
  fi
  return 0
}

# The list, NUL separated. Inside a checkout that is exactly the tracked set;
# outside one (an unpacked archive) the same skip list run-tests.sh uses, so a
# build tree cannot contribute generated scripts.
list_scripts() {
  if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$root" ls-files -z -- '*.sh'
  else
    ( cd "$root" && find . -name '*.sh' \
        -not -path './.git/*' -not -path './.ofx-ref/*' \
        -not -path './.validation/*' -not -path '*/build/*' -print0 )
  fi
}

if [[ "${1:-}" == "--selftest" ]]; then
  temporary=$(mktemp -d)
  trap 'rm -rf "$temporary"' EXIT
  printf '#!/usr/bin/env bash\necho fine\n' > "$temporary/good.sh"
  printf '#!/usr/bin/env bash\nfor name in one two three; do\n  echo "$name"\n' \
    > "$temporary/broken.sh"
  status=0
  if ! parse "$temporary/good.sh"; then
    echo "selftest: a clean script was rejected" >&2
    status=1
  fi
  if parse "$temporary/broken.sh"; then
    echo "selftest: a script without its 'done' was accepted" >&2
    status=1
  fi
  if ((status == 0)); then
    echo "selftest: the broken script is reported, the clean one accepted"
  fi
  exit "$status"
fi

files=()
while IFS= read -r -d '' file; do
  files+=("$file")
done < <(list_scripts)

if ((${#files[@]} == 0)); then
  echo "check-shell: no shell script found under $root" >&2
  exit 2
fi

failed=0
for file in "${files[@]}"; do
  if ! parse "$root/$file"; then
    echo "check-shell: $file does not parse" >&2
    failed=$((failed + 1))
  fi
done

if ((failed)); then
  echo "check-shell: $failed of ${#files[@]} script(s) do not parse" >&2
  exit 1
fi
echo "check-shell: ${#files[@]} script(s) parse"
