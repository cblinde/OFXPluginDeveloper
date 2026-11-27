#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# The Vulkan template's test, and the Linux counterpart of test/run-tests.bat.
# Both run the same five steps in the same order, and both decide the device
# step the same way:
#
#   [1/6] build     build.sh, which inspects the artifact as it goes (nm,
#                   readelf) - the same inspection the .bat runs as its step 3
#   [2/6] mode      vulkan_mode_test: the six gates that choose between the
#                   device and the CPU loop. Policy only, so it needs no Vulkan
#                   headers, no loader and no device - the same binary and the
#                   same cases as the .bat's step 2
#   [3/6] artifact  the two claims read again, here as the point of the step
#                   rather than as a side effect: exactly the two OFX exports
#                   and no undefined OFX symbol. The .bat gets the same answer
#                   from testkit/check-ofx-artifact.bat; the tools differ, the
#                   claims do not.
#   [4/6] contract  build-contract-test.sh, then the host protocol - load,
#                   describe, describeInContext, createInstance, render,
#                   destroy, unload - once per host shape, once more with both
#                   capability writes refused (the only run that executes the
#                   guard around them), and once with OFXPLUGIN_VULKAN_MODE=cpu
#                   so the label has to say the frame came off the CPU
#   [5/6] install   cmake --install into a temporary prefix and the same
#                   contract against the installed copy, because the build tree
#                   is not an install
#   [6/6] device    the device self-test: the shader, the descriptor set, the
#                   dispatch, both memory arrangements and the two refusals.
#                   Exit 0 is a pass, exit 2 is the suite's "no Vulkan device on
#                   this machine" and reports NOT VERIFIED, anything else is a
#                   failure. The .bat maps exactly those three cases.
#
# What this script does and the .bat does not: valgrind memcheck, once per host
# profile, because the reference plug-in is the one every other template is
# compared against and a leak there would be copied. It is not a numbered step -
# the .bat has none, and says so.
#
#   ./run-tests.sh
#   ./run-tests.sh nobuild                 # reuse build/ instead of build.sh
#   OFXPLUGIN_VULKAN=OFF ./run-tests.sh    # CPU-only bundle: the device step
#                                          # reports NOT VERIFIED, not a pass
#   SANITIZE=address,undefined ./run-tests.sh
#   RUN_VALGRIND=0 ./run-tests.sh          # skip memcheck
#   RUN_VALGRIND=1 ./run-tests.sh          # require it
#
# The harness lives in testkit/ so both templates certify the same behaviour.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${TPL}/../.." && pwd)"

# The Vulkan half can be configured away - CMake needs the SDK and glslc for it -
# and then there is no device suite to run. The switch is honoured here and
# passed to build.sh explicitly, so `OFXPLUGIN_VULKAN=OFF ./run-tests.sh` and an
# exported variable mean the same thing.
VULKAN="${OFXPLUGIN_VULKAN:-ON}"
NOBUILD=0
for argument in "$@"; do
  case "${argument}" in
    nobuild) NOBUILD=1 ;;
    *)
      echo "error: unknown argument '${argument}' (the only one is: nobuild)" >&2
      exit 2
      ;;
  esac
done

if [[ "${NOBUILD}" == "1" ]]; then
  echo "== [1/6] build skipped (nobuild; OFXPLUGIN_VULKAN=${VULKAN}) =="
else
  echo "== [1/6] build (artifact checks run inside build.sh; OFXPLUGIN_VULKAN=${VULKAN}) =="
  OFXPLUGIN_VULKAN="${VULKAN}" "${TPL}/build.sh"
fi

OFX_FILE="$(find "${TPL}/build/bundle" -name '*.ofx' -type f | head -1)"
if [[ -z "${OFX_FILE}" ]]; then
  echo "error: no .ofx under ${TPL}/build/bundle" >&2
  echo "       run this without 'nobuild' first, or point it at a built tree." >&2
  exit 1
fi
OFX_DIR="${OFX_SOURCE_DIR:-${ROOT}/.ofx-ref}"
HARNESS="${TPL}/build/contract_test"

# The one step that needs nothing at all: VulkanMode.h is policy only (no Vulkan
# headers, no OFX headers), so the table that decides between the device and the
# CPU loop is compiled and executed on a machine with no driver. Built
# unconditionally, so a -DOFXPLUGIN_VULKAN=OFF configure has to pass it too.
echo
echo "== [2/6] path policy (six gates, no device needed) =="
MODETEST="${TPL}/build/self-test/vulkan_mode_test"
if [[ ! -x "${MODETEST}" ]]; then
  echo "error: no ${MODETEST} - the configure did not build the policy test" >&2
  exit 1
fi
"${MODETEST}"

echo
# The build already read these two claims; a pass there and a failure here would
# mean the artifact changed between the two reads, which is worth a step of its
# own - and it is the step the .bat has (its [3/6]) with a different reader.
echo "== [3/6] artifact (exports, undefined OFX symbols) =="
if ! command -v nm >/dev/null 2>&1; then
  echo "  FAIL: nm is not on PATH, so the artifact cannot be read here" >&2
  exit 1
fi
exports="$(nm -D --defined-only "${OFX_FILE}" | awk '$2 == "T" {print $3}' | sort)"
expected="$(printf 'OfxGetNumberOfPlugins\nOfxGetPlugin' | sort)"
if [[ "${exports}" != "${expected}" ]]; then
  echo "  FAIL: the dynamic exports are not the two the OFX loader looks for" >&2
  while IFS= read -r symbol; do
    [[ -n "${symbol}" ]] && echo "    ${symbol}" >&2
  done <<< "${exports:-<none>}"
  exit 1
fi
echo "  exports: $(echo "${exports}" | tr '\n' ' ')"
undefined="$(nm -D -u "${OFX_FILE}" | awk '{print $NF}' | grep -E '^(Ofx|OFX)' || true)"
if [[ -n "${undefined}" ]]; then
  echo "  FAIL: undefined OFX symbols - the host must have to resolve none of them" >&2
  while IFS= read -r symbol; do
    echo "    ${symbol}" >&2
  done <<< "${undefined}"
  exit 1
fi
echo "  undefined OFX symbols: none"

# The sanitizer is named in the log, like the other two suites do: a run whose
# header says "none" must not be mistaken for an instrumented one, and the CI
# job greps this line to prove the build it credits was built that way.
echo
echo "== [4/6] host contract (dlopen + fake host; sanitizer: ${SANITIZE:-none}) =="
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
  --id com.ofxplugindeveloper.vulkaingain
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

for profile in "${HOST_PROFILES[@]}"; do
  echo "  host profile: ${profile}"
  "${HARNESS}" "${OFX_FILE}" --host-profile "${profile}" "${HARNESS_ARGS[@]}"
done

# Resolve 21.2.0.5 answers a write of kOfxImageEffectPropCPURenderSupported with
# kOfxStatErrUnknown, the support library turns that into a failed describe and
# the plug-in never loads ("error code 4"). The guard around the two capability
# writes is what keeps it loading, and this run is what executes the guard: with
# the guard removed, describe fails here with kOfxStatErrMissingHostFeature.
echo "  host profile: resolve, refusing both capability writes"
"${HARNESS}" "${OFX_FILE}" --host-profile resolve \
  --reject-property kOfxImageEffectPropCPURenderSupported \
  --reject-property kOfxImageEffectPropNoSpatialAwareness \
  "${HARNESS_ARGS[@]}"

# The one path a machine with a device cannot reach otherwise: the override makes
# VulkanMode.h's first gate say CPU, the plug-in writes that decision into the
# Device parameter after a real render, and --label-contains fails the run if the
# label does not carry it. Without the switch this assertion could only be made
# on a machine that has no Vulkan driver - and the value of the switch is exactly
# that the fallback is exercised where the device exists. On a machine with no
# device the same label comes from the second gate, so the run passes there too;
# only this run proves the override reaches the render.
echo "  host profile: resolve, OFXPLUGIN_VULKAN_MODE=cpu (forced CPU path)"
OFXPLUGIN_VULKAN_MODE=cpu "${HARNESS}" "${OFX_FILE}" --host-profile resolve \
  --id com.ofxplugindeveloper.vulkaingain --version 1.0 --param gain:double \
  --clip Source --clip Output \
  --label-param device --label-contains device=CPU

echo
# The build tree is not an install: a host loads what is under OFX/Plugins, so
# `cmake --install` into a throwaway prefix is the move that exposes a path or a
# dependency that only ever resolved inside the build directory (an $ORIGIN RPATH
# that pointed back at build/, a resource only the build wrote there). The harness
# then renders one frame from the installed copy.
echo "== [5/6] install and load the installed copy =="
INSTALL_PREFIX="$(mktemp -d)"
cmake --install "${TPL}/build" --prefix "${INSTALL_PREFIX}" > "${INSTALL_PREFIX}/install.log" 2>&1 || {
  cat "${INSTALL_PREFIX}/install.log" >&2
  exit 1
}
INSTALLED="$(find "${INSTALL_PREFIX}/OFX/Plugins" -name "*.ofx" -type f | head -1)"
if [[ -z "${INSTALLED}" ]]; then
  echo "  FAIL: cmake --install wrote no .ofx under ${INSTALL_PREFIX}/OFX/Plugins" >&2
  exit 1
fi
echo "  ${INSTALLED}"
"${HARNESS}" "${INSTALLED}" --host-profile resolve \
  --id com.ofxplugindeveloper.vulkaingain --version 1.0 --param gain:double \
  --clip Source --clip Output
rm -rf "${INSTALL_PREFIX}"

echo
echo "== [6/6] device suite (Vulkan vs CPU) =="
# The half a host run cannot prove cheaply: the shader, the descriptor set, the
# dispatch, both memory arrangements and the two refusals. Guarded rather than
# assumed - a machine with no Vulkan device, or a CPU-only configure, reports
# NOT VERIFIED instead of a green step, because a pass on hardware that is not
# there would be a lie. The suite's own header defines its exit codes: 0 pass,
# 2 no device, anything else a failure.
#
# Returns 0 for a pass or a named NOT VERIFIED and 1 for a failure, so the three
# cases of the .bat map onto the same three here.
run_device_suite() {
  local suite="$1"
  if [[ "${VULKAN}" == "OFF" ]]; then
    echo "  NOT VERIFIED no device suite binary - configured with OFXPLUGIN_VULKAN=OFF"
    return 0
  fi
  if [[ ! -e "${suite}" ]]; then
    echo "  NOT VERIFIED no device suite binary at ${suite} - the configure did not build it"
    return 0
  fi
  if [[ ! -f "${suite}" ]]; then
    echo "  FAIL: ${suite} exists but is not a regular file" >&2
    return 1
  fi
  if [[ ! -x "${suite}" ]]; then
    echo "  FAIL: ${suite} exists but is not executable" >&2
    return 1
  fi
  local status=0
  "${suite}" || status=$?
  case "${status}" in
    0) return 0 ;;
    2)
      echo "  NOT VERIFIED the suite found no Vulkan device on this machine"
      return 0
      ;;
    *)
      echo "  FAIL: the device suite reported a failure (exit ${status})" >&2
      return 1
      ;;
  esac
}

run_device_suite "${TPL}/build/self-test/vulkan_suite" || exit 1

echo
echo "== memcheck (Linux only; the .bat has no step for it) =="
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

echo
echo "all Linux checks passed"
