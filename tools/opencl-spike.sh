#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Backlog row 34, the small half: build and run tools/opencl-spike.cpp against
# whatever OpenCL the machine has.
#
#   OPENCL_LOADER_LIB=/path/to/libOpenCL.so.1 ./tools/opencl-spike.sh
#
# The loader is named by env var rather than found: a render node gets its
# OpenCL from the host, and guessing which `libOpenCL.so.1` a build picked is
# how a spike ends up proving something about a different runtime than the one
# the plug-in will see. The ICD is chosen the usual way, through OCL_ICD_VENDORS
# (or the loader's default search).
#
# Nothing here needs a GPU, a vendor SDK or the OpenCL headers - see the comment
# at the top of the .cpp for why that is the point, not a shortcut.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
CXX_BIN="${CXX:-g++}"

if [[ -z "${OPENCL_LOADER_LIB:-}" ]]; then
  echo "error: OPENCL_LOADER_LIB is not set (e.g. /usr/lib/x86_64-linux-gnu/libOpenCL.so.1)" >&2
  exit 2
fi

OUT="${ROOT}/build/opencl-spike"
mkdir -p "${ROOT}/build"

echo "== building the spike =="
"${CXX_BIN}" --version | head -1
"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra -Werror -o "${OUT}" \
  "${HERE}/opencl-spike.cpp" -ldl

echo
echo "== running it =="
echo "  OPENCL_LOADER_LIB=${OPENCL_LOADER_LIB}"
echo "  OCL_ICD_VENDORS=${OCL_ICD_VENDORS:-<loader default>}"
exec "${OUT}"
