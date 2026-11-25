#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Refuse a bundle that would not load on the platform it was built for.
#
# The Linux baseline is older than the machine you build on: OFX 1.5.1 ships
# CentOS 7 builds for the vfx-2021 and vfx-2022 platform years, which makes glibc
# 2.17 the floor. A plug-in built in a current container links memcpy@GLIBC_2.14
# without noticing, and the host then dies at load time with "GLIBC_2.28 not
# found" before any OFX code runs (references/cmake-and-linking.md section 5).
#
#   tools/check-glibc.sh --max 2.17 build/bundle/Name.ofx.bundle
#   tools/check-glibc.sh --max 2.17 build/bundle/Name.ofx.bundle/Contents/Linux-x86-64
#   tools/check-glibc.sh --max 2.17 --selftest
#
# Every .ofx and every .so the paths contain (or name) is inspected with
# `readelf --version-info`; the highest GLIBC_ version found in each object is
# compared against --max. An object above the bound is a failure, and so is an
# object readelf cannot read: a bundle member nobody looked at is not a pass.
#
# Exit codes: 0 every object stays within the bound; 1 an object needs a newer
# glibc than the bound; 2 usage or setup error (no readelf, nothing to inspect,
# an unreadable object, an unparsable bound).
#
# --selftest drives the parser and the comparison with a canned readelf, so it
# runs where no ELF binary exists - the Windows box this repository is often
# exercised on, for one. It proves the comparison fails when the bound is one
# step too low; it does not prove that a real readelf reads a real .ofx the way
# the canned one does.
#
# Both `./run-tests.sh` and the `docs` CI workflow run `--selftest`: a comparison
# that silently stops failing costs a bundle at load time on the target platform,
# so the regression has to break a build without anyone remembering to look.
set -uo pipefail

SELF="${BASH_SOURCE[0]}"
BOUND=""
READELF="${READELF:-readelf}"
SELFTEST=0
PATHS=()

die() { echo "error: $*" >&2; exit 2; }

usage() {
  cat <<EOF
usage: ${0##*/} --max <glibc version> <bundle-or-path>...

  --max VERSION      the highest glibc the target platform has, e.g. 2.17
  --readelf CMD      the readelf to use (default: \$READELF, then readelf)
  --selftest         prove the comparison fails on a bound that is too low

Checks every .ofx and every .so under each path (a file is taken as one object).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --max) [[ $# -ge 2 ]] || die "--max wants a version"; BOUND="$2"; shift 2 ;;
    --readelf) [[ $# -ge 2 ]] || die "--readelf wants a command"; READELF="$2"; shift 2 ;;
    --selftest) SELFTEST=1; shift ;;
    -*) die "unknown option $1 (see --help)" ;;
    *) PATHS+=("$1"); shift ;;
  esac
done

check_bound() {
  [[ "${BOUND}" =~ ^[0-9]+(\.[0-9]+)+$ ]] \
    || die "not a glibc version: '${BOUND}' (want e.g. 2.17)"
}

# Versions in $1, one per line, ascending and deduplicated. GLIBCXX_3.4.29 is not
# a glibc version and must not be read as one, which is why the pattern needs the
# digit straight after the underscore.
versions_of() {
  local out status
  out="$("${READELF}" --version-info "$1" 2>&1)"
  status=$?
  if [[ ${status} -ne 0 ]]; then
    die "readelf failed on $1 (exit ${status}): $(printf '%s\n' "${out}" | head -3 | tr '\n' ' ')"
  fi
  printf '%s\n' "${out}" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV
  return 0
}

above() {  # $1 > $2 in version order?
  [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -uV | tail -1)" != "$2" ]]
}

collect() {
  local p
  for p in "$@"; do
    if [[ -d "${p}" ]]; then
      find "${p}" -type f \( -name '*.ofx' -o -name '*.so' -o -name '*.so.*' \) -print
    elif [[ -f "${p}" ]]; then
      printf '%s\n' "${p}"
    else
      die "no such file or directory: ${p}"
    fi
  done
}

check_paths() {
  [[ ${#PATHS[@]} -gt 0 ]] || { usage >&2; die "nothing to check"; }
  command -v "${READELF}" >/dev/null 2>&1 \
    || die "no ${READELF} on PATH; this check needs binutils (--readelf names another)"

  local objects=() seen="" obj versions highest
  local checked=0 failures=0 skipped=0
  while IFS= read -r obj; do
    [[ -n "${obj}" ]] || continue
    case "${seen}" in
      *"|${obj}|"*) continue ;;
    esac
    seen="${seen}|${obj}|"
    objects+=("${obj}")
  done < <(collect "${PATHS[@]}")

  echo "checking ${#objects[@]} object(s) against glibc ${BOUND}"
  for obj in "${objects[@]}"; do
    versions="$(versions_of "${obj}")"
    if [[ -z "${versions}" ]]; then
      printf '  --   %s  no GLIBC_ references (static, or not glibc)\n' "${obj}"
      skipped=$((skipped + 1))
      continue
    fi
    checked=$((checked + 1))
    highest="$(printf '%s\n' "${versions}" | tail -1)"
    if above "${highest}" "${BOUND}"; then
      printf '  FAIL %s  needs GLIBC_%s, bound %s\n' "${obj}" "${highest}" "${BOUND}"
      failures=$((failures + 1))
    else
      printf '  ok   %s  max GLIBC_%s\n' "${obj}" "${highest}"
    fi
  done

  [[ ${checked} -gt 0 ]] \
    || die "nothing to check: no object under these paths references GLIBC_ (${skipped} inspected)"

  if [[ ${failures} -gt 0 ]]; then
    echo "glibc check: ${failures} of ${checked} object(s) need a newer glibc than ${BOUND}" >&2
    return 1
  fi
  echo "glibc check: ${checked} object(s) within ${BOUND}$([[ ${skipped} -gt 0 ]] && echo ", ${skipped} without GLIBC_ references")"
  return 0
}

selftest() {
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/check-glibc.XXXXXX")" || die "mktemp failed"
  # Expanded now: at exit the local is gone and `set -u` would abort the cleanup.
  trap "rm -rf '${tmp}'" EXIT

  local fake="${tmp}/readelf"
  cat > "${fake}" <<'FAKE'
#!/usr/bin/env bash
# A canned `readelf --version-info`: the last argument is the object.
f=""
for a in "$@"; do f="$a"; done
case "$(basename "$f")" in
  *needs-newer*) printf '  Name: GLIBC_2.2.5  Flags: none  Version: 3\n  Name: GLIBC_2.28  Flags: none  Version: 4\n' ;;
  *cxx-only*)    printf '  Name: GLIBCXX_3.4.29  Flags: none  Version: 6\n  Name: GLIBC_2.2.5  Flags: none  Version: 3\n' ;;
  *unreadable*)  echo "readelf: Error: Not an ELF file" >&2; exit 1 ;;
  *)             printf '  Name: GLIBC_2.2.5  Flags: none  Version: 3\n  Name: GLIBC_2.17  Flags: none  Version: 5\n' ;;
esac
FAKE
  chmod +x "${fake}"

  local bundle="${tmp}/Name.ofx.bundle/Contents/Linux-x86-64"
  mkdir -p "${bundle}" "${tmp}/newer" "${tmp}/cxx" "${tmp}/unreadable" "${tmp}/empty"
  : > "${bundle}/Name.ofx"
  : > "${bundle}/libdependency.so.1.2.3"
  : > "${tmp}/newer/needs-newer.ofx"
  : > "${tmp}/cxx/cxx-only.so"
  : > "${tmp}/unreadable/unreadable.ofx"

  local failed=0 n=0
  # case_run <name> <expected exit> <expected text or ""> <args...>
  case_run() {
    local name="$1" want="$2" expect="$3"
    shift 3
    n=$((n + 1))
    local out status
    out="$("${SELF}" --readelf "${fake}" "$@" 2>&1)"
    status=$?
    if [[ ${status} -ne ${want} ]]; then
      printf 'selftest case %d: %s ... FAILED (exit %d, wanted %d)\n%s\n' \
        "${n}" "${name}" "${status}" "${want}" "${out}"
      failed=$((failed + 1))
      return
    fi
    if [[ -n "${expect}" && "${out}" != *"${expect}"* ]]; then
      printf 'selftest case %d: %s ... FAILED (exit %d was right, output lacks "%s")\n%s\n' \
        "${n}" "${name}" "${status}" "${expect}" "${out}"
      failed=$((failed + 1))
      return
    fi
    printf 'selftest case %d: %s ... ok (exit %d)\n' "${n}" "${name}" "${status}"
  }

  case_run "a bundle within the bound passes" 0 "2 object(s) within 2.17" \
    --max 2.17 "${tmp}/Name.ofx.bundle"
  case_run "a bound one step too low fails" 1 "needs GLIBC_2.17, bound 2.16" \
    --max 2.16 "${tmp}/Name.ofx.bundle"
  case_run "a newer requirement is caught and named" 1 "needs GLIBC_2.28, bound 2.17" \
    --max 2.17 "${tmp}/newer"
  case_run "a bound above the highest passes" 0 "max GLIBC_2.17" \
    --max 2.28 "${tmp}/Name.ofx.bundle"
  case_run "GLIBCXX is not read as GLIBC" 0 "max GLIBC_2.2.5" \
    --max 2.17 "${tmp}/cxx"
  case_run "nothing to inspect is an error, not a pass" 2 "nothing to check" \
    --max 2.17 "${tmp}/empty"
  case_run "an object readelf cannot read is an error" 2 "readelf failed on" \
    --max 2.17 "${tmp}/unreadable"
  case_run "an unparsable bound is refused" 2 "not a glibc version" \
    --max 2.x "${tmp}/Name.ofx.bundle"
  case_run "a missing path is refused" 2 "no such file or directory" \
    --max 2.17 "${tmp}/absent"

  echo "self-test: ${failed} case(s) failed"
  [[ ${failed} -eq 0 ]]
}

if [[ ${SELFTEST} -eq 1 ]]; then
  selftest
  exit $?
fi

check_bound
check_paths
exit $?
