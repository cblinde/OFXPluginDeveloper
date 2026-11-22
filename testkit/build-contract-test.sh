#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Compiles the shared contract harness (fake_host.cpp + contract_main.cpp) into
# one binary. Both templates call this; the harness itself is not duplicated.
#
#   testkit/build-contract-test.sh <out-binary> [openfx-checkout]
#
# The openfx checkout defaults to OFX_SOURCE_DIR from ./build/CMakeCache.txt,
# then ../.ofx-ref, so a template's test script does not have to know where the
# headers were fetched to.
#
# Environment:
#   CXX        compiler (default g++)
#   SANITIZE   e.g. "address,undefined"; builds the harness with that sanitizer
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build-contract-test.sh <out-binary> [openfx-checkout]}"
INC="${2:-}"

if [[ -z "${INC}" && -f "${PWD}/build/CMakeCache.txt" ]]; then
  INC="$(sed -n 's/^OFX_SOURCE_DIR:PATH=//p' "${PWD}/build/CMakeCache.txt" | head -1)"
fi
if [[ -z "${INC}" && -d "${HERE}/../.ofx-ref/include" ]]; then
  INC="${HERE}/../.ofx-ref/include"
fi
if [[ -n "${INC}" && -f "${INC}/include/ofxCore.h" ]]; then
  INC="${INC}/include"
fi
if [[ ! -f "${INC}/ofxCore.h" ]]; then
  echo "error: no OFX headers found. Pass the openfx checkout, e.g." >&2
  echo "       testkit/build-contract-test.sh out /path/to/openfx" >&2
  exit 2
fi

FLAGS=(-std=c++17 -Wall -Wextra)
if [[ -n "${SANITIZE:-}" ]]; then
  FLAGS+=(-O1 -g -fno-omit-frame-pointer "-fsanitize=${SANITIZE}")
else
  FLAGS+=(-O2)
fi

"${CXX:-g++}" "${FLAGS[@]}" -I "${HERE}" -I "${INC}" \
  "${HERE}/fake_host.cpp" "${HERE}/contract_main.cpp" -o "${OUT}" -ldl
