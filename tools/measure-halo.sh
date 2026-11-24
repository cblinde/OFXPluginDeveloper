#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Measure the tile overlap a model actually needs, and write the number next to
# the model so the next person does not guess it again.
#
# This is the recipe in references/onnx-and-tensorrt.md section 7 as a command: the
# overlap ladder is walked through the plug-in's own tiling code
# (templates/onnx-inference-ofx/src/AiCore.cpp, not a reimplementation), the
# frame border is excluded from the comparison, a real interior tile edge is
# required, and the result lands in <model>.halo.json beside the model.
#
#   tools/measure-halo.sh model.onnx
#   tools/measure-halo.sh model.onnx --tile 128 --ladder 8,16,24,32 --threshold 0.02
#   tools/measure-halo.sh model.onnx --check
#
# Exit codes: 0 measured (or --check confirmed), 2 the model is not tilable at
# this threshold (the JSON records "tilable": false), or the stored value is
# missing or stale - 3 setup or usage error.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SRC="${ROOT}/templates/onnx-inference-ofx/src"
BUILD="${ROOT}/build/halo"

die() { echo "error: $*" >&2; exit 3; }

usage() {
  cat <<EOF
usage: ${0##*/} <model.onnx> [measurement options]

  --check            verify <model>.halo.json against the model's bytes and hash
  --force            overwrite a stored value that belongs to a different file
  --out DIR          write the JSON into DIR instead of beside the model

Measurement options (passed through):
  --frame WxH        frame to measure on            (default 384x320)
  --tile N           core tile size                 (default 256)
  --ladder a,b,c     overlaps to try, in order      (default 32,48,64,96,128)
  --threshold T      max deviation accepted         (default 0.02)
  --border P         frame border excluded per side (default 16)

Needs ONNX Runtime: set ONNXRUNTIME_ROOT to a tree with include/ and lib/, or
run templates/onnx-inference-ofx/build.sh once and let its cache record one.
EOF
}

force=0
check=0
outdir=""
model=""
forward=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --force) force=1; shift ;;
    --check) check=1; shift ;;
    --out) outdir="${2:?--out wants a directory}"; shift 2 ;;
    --frame|--tile|--ladder|--threshold|--border)
      [[ $# -ge 2 ]] || die "$1 wants a value"
      forward+=("$1" "$2"); shift 2 ;;
    -*) die "unknown option $1 (see --help)" ;;
    *) if [[ -z "${model}" ]]; then model="$1"; else die "more than one model path"; fi; shift ;;
  esac
done
[[ -n "${model}" ]] || { usage; exit 3; }
[[ -f "${model}" ]] || die "no such model: ${model}"

sha_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "need sha256sum or shasum to key the file to its model"
  fi
}

MODEL_BYTES="$(wc -c < "${model}" | tr -d '[:space:]')"
MODEL_SHA="$(sha_of "${model}")"
JSON_DIR="${outdir:-$(dirname "${model}")}"
JSON="${JSON_DIR}/$(basename "${model}").halo.json"

stored_bytes() { sed -n 's/.*"bytes": *\([0-9][0-9]*\).*/\1/p' "$1" | head -1; }
stored_sha() { sed -n 's/.*"sha256": *"\([0-9a-f][0-9a-f]*\)".*/\1/p' "$1" | head -1; }
stored_halo() {
  local v
  v="$(sed -n 's/.*"halo": *\([0-9][0-9]*\).*/\1/p' "$1" | head -1)"
  [[ -n "${v}" ]] && echo "${v}" || echo "null"
}

if [[ ${check} -eq 1 ]]; then
  [[ -f "${JSON}" ]] || { echo "error: ${JSON} does not exist; measure first" >&2; exit 2; }
  if [[ "$(stored_sha "${JSON}")" != "${MODEL_SHA}" || "$(stored_bytes "${JSON}")" != "${MODEL_BYTES}" ]]; then
    echo "STALE: ${JSON} belongs to a different file" >&2
    echo "       stored bytes $(stored_bytes "${JSON}") sha $(stored_sha "${JSON}")" >&2
    echo "       actual bytes ${MODEL_BYTES} sha ${MODEL_SHA}" >&2
    exit 2
  fi
  echo "current: $(basename "${model}") -> halo $(stored_halo "${JSON}") in ${JSON}"
  exit 0
fi

# The measurement runs the template's own code, so it needs the ORT the template
# builds against: $ONNXRUNTIME_ROOT, or the cache of an existing build.
ORT_ROOT="${ONNXRUNTIME_ROOT:-}"
if [[ -z "${ORT_ROOT}" ]]; then
  CACHE="${ROOT}/templates/onnx-inference-ofx/build/CMakeCache.txt"
  if [[ -f "${CACHE}" ]]; then
    ORT_ROOT="$(sed -n 's/^ONNXRUNTIME_ROOT[^=]*=//p' "${CACHE}" | head -1)"
    if [[ -z "${ORT_ROOT}" ]]; then
      ortl="$(sed -n 's/^ONNXRUNTIME_LIBRARY[^=]*=//p' "${CACHE}" | head -1)"
      [[ -n "${ortl}" ]] && ORT_ROOT="$(cd "$(dirname "${ortl}")/.." && pwd)"
    fi
  fi
fi
[[ -n "${ORT_ROOT}" && -d "${ORT_ROOT}/include" ]] || die "ONNX Runtime not found; set ONNXRUNTIME_ROOT (see --help)"
compgen -G "${ORT_ROOT}/lib/libonnxruntime.so*" >/dev/null || die "${ORT_ROOT}/lib has no libonnxruntime.so"

mkdir -p "${BUILD}"
BIN="${BUILD}/measure-halo"
# The loader asks for the SONAME (libonnxruntime.so.1), which a NuGet-style tree
# does not always carry: it ships libonnxruntime.so. Link against that, run
# against a directory that has the name the binary records.
LIBDIR="${ORT_ROOT}/lib"
if ! compgen -G "${LIBDIR}/libonnxruntime.so.1*" >/dev/null; then
  real_so="$(find "${LIBDIR}" -maxdepth 1 -name 'libonnxruntime.so' -print -quit 2>/dev/null)"
  [[ -n "${real_so}" ]] || die "${LIBDIR} has neither libonnxruntime.so.1 nor libonnxruntime.so"
  mkdir -p "${BUILD}/lib"
  ln -sf "${real_so}" "${BUILD}/lib/libonnxruntime.so.1"
  LIBDIR="${BUILD}/lib"
fi
echo "building ${BIN#"${ROOT}"/} from the template's tiling code"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra \
  -I "${SRC}" -I "${ORT_ROOT}/include" \
  "${HERE}/measure-halo.cpp" "${SRC}/AiCore.cpp" "${SRC}/ModelRunner.cpp" \
  -L"${ORT_ROOT}/lib" -Wl,-rpath,"${LIBDIR}" -lonnxruntime -ldl \
  -o "${BIN}"

LOG="$(mktemp)"
trap 'rm -f "${LOG}"' EXIT
set +e
if [[ ${#forward[@]} -gt 0 ]]; then
  "${BIN}" "${model}" "${forward[@]}" | tee "${LOG}"
else
  "${BIN}" "${model}" | tee "${LOG}"
fi
status=${PIPESTATUS[0]}
set -e
case ${status} in
  0|2) ;;
  *)
    echo "measurement failed (exit ${status}); nothing written" >&2
    exit 3
    ;;
esac

LADDER_JSON="$(sed -n 's/^ladder halo=\([0-9]*\) max=\([^ ]*\) at=(\([^)]*\)) compared=\([0-9]*\)$/{"halo": \1, "max_deviation": \2, "at": "\3", "compared_pixels": \4}/p' "${LOG}" \
  | awk '{ if (NR > 1) printf ",\n"; printf "    %s", $0 } END { printf "\n" }')"
HALO="$(sed -n 's/^result halo=\([0-9]*\)$/\1/p' "${LOG}" | head -1)"
TILE="$(sed -n 's/.* tile=\([0-9]*\).*/\1/p' "${LOG}" | head -1)"
FRAME="$(sed -n 's/.* size=\([0-9]*x[0-9]*\).*/\1/p' "${LOG}" | head -1)"
BORDER="$(sed -n 's/.* border=\([0-9]*\).*/\1/p' "${LOG}" | head -1)"
THRESHOLD="$(sed -n 's/.* threshold=\([0-9.e+-]*\).*/\1/p' "${LOG}" | head -1)"

if [[ -f "${JSON}" && ${force} -eq 0 ]]; then
  if [[ "$(stored_sha "${JSON}")" != "${MODEL_SHA}" || "$(stored_bytes "${JSON}")" != "${MODEL_BYTES}" ]]; then
    echo >&2
    echo "error: ${JSON} records a halo for a different file" >&2
    echo "       a replaced model must not inherit a stale halo; re-run with --force" >&2
    exit 3
  fi
fi

mkdir -p "${JSON_DIR}"
{
  printf '{\n'
  printf '  "model": "%s",\n' "$(basename "${model}")"
  printf '  "bytes": %s,\n' "${MODEL_BYTES}"
  printf '  "sha256": "%s",\n' "${MODEL_SHA}"
  printf '  "frame": "%s",\n' "${FRAME}"
  printf '  "tile": %s,\n' "${TILE}"
  printf '  "border": %s,\n' "${BORDER}"
  printf '  "threshold": %s,\n' "${THRESHOLD}"
  if [[ -n "${HALO}" ]]; then
    printf '  "tilable": true,\n'
    printf '  "halo": %s,\n' "${HALO}"
  else
    printf '  "tilable": false,\n'
    printf '  "halo": null,\n'
  fi
  printf '  "ladder": [\n%s\n  ],\n' "${LADDER_JSON}"
  printf '  "measured": "%s",\n' "$(date -u +%Y-%m-%d)"
  printf '  "tool": "tools/measure-halo.sh"\n'
  printf '}\n'
} > "${JSON}"

echo
if [[ -n "${HALO}" ]]; then
  echo "halo ${HALO} written to ${JSON}"
else
  echo "not tilable at threshold ${THRESHOLD}; recorded in ${JSON}"
fi
exit "${status}"
