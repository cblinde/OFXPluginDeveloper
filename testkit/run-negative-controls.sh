#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Proves that contract_test detects faults instead of merely passing.
#
# Fifteen controls are fed to the same harness the templates use. Two are
# separate broken plug-ins (stub_bad_status.c answers describe with ReplyDefault,
# stub_bad_type.c declares the wrong parameter type). The rest are one stub built
# several ways: plain or internally tiled with the declared context read or
# ignored, a global effect that declares no context at all, a label that is never
# written and one that is, a mark in the four pointer layouts, and a clip
# property that is re-read or cached.
#
# The script succeeds only if every broken build exits non-zero *and* names the
# assertion, and every context-reading build passes. Those pairs are what make
# `--roi-context` and `--roi-context-tolerance` self-verifying: without the
# ignored build a check could be satisfied by any plug-in, and without the
# honoured build it could be unsatisfiable. The tiled pair covers the tolerance
# variant without needing the ONNX template's model or bundle.
#
# ../run-tests.sh runs this after the template suites, and its Windows
# counterpart - run-negative-controls.bat, the same fifteen controls built with
# MSVC - after them on that platform. Both exit 0 when every fault is caught, so
# neither turns a green run red; run-tests.sh skips this stage by name when no C
# compiler is installed rather than pretending it ran. Run it directly to read
# the full harness output for one control:
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
# Same source, one decision apart: clamp the filter taps to the published
# source bounds (the context the plug-in asked for) or to the window it was
# told to render.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_ignored.ofx" -DIGNORE_CONTEXT "${HERE}/negative-controls/stub_roi_context.c"
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_honoured.ofx" "${HERE}/negative-controls/stub_roi_context.c"
# The same two decisions again, with the stub tiling internally: -DTILED splits
# the window into 32-pixel cores, so dropping the context now shows up at a
# boundary the whole-frame render does not have. This is the pair behind
# --roi-context-tolerance, which the ONNX template exercises only with its own
# bundle.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_tiled_ignored.ofx" -DTILED -DIGNORE_CONTEXT \
   "${HERE}/negative-controls/stub_roi_context.c"
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_tiled_ok.ofx" -DTILED "${HERE}/negative-controls/stub_roi_context.c"
# And the label control: the same stub declares the read-only `provider` label,
# gives it a default, and never writes it. If the harness accepted a default as
# proof, every plug-in that declared the label would pass without running.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_no_label.ofx" -DNO_LABEL "${HERE}/negative-controls/stub_roi_context.c"
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_label_ok.ofx" -DWITH_LABEL "${HERE}/negative-controls/stub_roi_context.c"
# And the other half of --roi-context: a global effect answers
# GetRegionsOfInterest with the region it was asked about, and the harness has to
# skip the "cropped window must differ" half instead of failing it.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_global.ofx" -DROI_SELF "${HERE}/negative-controls/stub_roi_context.c"
# And the clip-property pair: the stub writes what it read from the source
# clip's `mediaID` into a label. Re-reading it passes; caching the first answer
# on the clip handle must fail, which is the invalidation bug the handover names.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/clip_echo.ofx" -DCLIP_ECHO "${HERE}/negative-controls/stub_roi_context.c"
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/clip_stale.ofx" -DCLIP_ECHO_ONCE "${HERE}/negative-controls/stub_roi_context.c"
# And the drawing control: a box filter plus one mark at a fixed image position,
# so the four pointer-origin/stride layouts stop being a claim in a README.
cc -shared -fPIC -std=c99 -I "${INC}" \
   -o "${TMP}/roi_mark.ofx" -DDRAW_MARK "${HERE}/negative-controls/stub_roi_context.c"

# The harness's default source is one constant colour, and a box filter over a
# constant image has no seam to find: every window would agree no matter what
# the plug-in reads. A varying fixture is what makes the comparison mean
# something, so it is generated rather than committed.
cat > "${TMP}/make_roi_source.c" <<'EOF'
#include <stdio.h>
int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  FILE* f = fopen(argv[1], "wb");
  if (!f) return 1;
  for (int c = 0; c < 3; ++c)
    for (int y = 0; y < 48; ++y)
      for (int x = 0; x < 64; ++x) {
        float v = (float)(((x * 7 + y * 13 + c * 29) % 97) / 97.0);
        fwrite(&v, sizeof v, 1, f);
      }
  fclose(f);
  return 0;
}
EOF
cc -o "${TMP}/make_roi_source" "${TMP}/make_roi_source.c"
"${TMP}/make_roi_source" "${TMP}/roi_source.bin"
ROI_ARGS=("${TMP}/roi_honoured.ofx" --id com.testkit.control.roiok
          --clip Source --clip Output --concurrency-threads 0 --roi-context
          --size 64x48 --src "${TMP}/roi_source.bin")

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

# The mirror image of run_control: a plug-in that follows the pattern has to
# pass, otherwise a check that is red for everything would look like a working
# control.
run_pass()
{
  local title="$1"; shift
  local output status
  output="$("${BIN}" "$@" 2>&1)"
  status=$?
  printf '\n--- negative control: %s ---\n' "${title}"
  printf '%s\n' "${output}" | sed 's/^/  /'
  if [[ ${status} -ne 0 ]]; then
    echo "  CONTROL FAILED: the harness rejected a plug-in that follows the pattern" >&2
    fail=1
    return
  fi
  echo "  passed, as a plug-in following the pattern must"
}

# A pass that also has to name a line: without that, a check could pass because it
# silently did something else - which is exactly the failure a skip is meant to
# prevent.
run_pass_mentioning()
{
  local title="$1" needle="$2"; shift 2
  local output status
  output="$("${BIN}" "$@" 2>&1)"
  status=$?
  printf '\n--- negative control: %s ---\n' "${title}"
  printf '%s\n' "${output}" | sed 's/^/  /'
  if [[ ${status} -ne 0 ]]; then
    echo "  CONTROL FAILED: the harness rejected a plug-in that follows the pattern" >&2
    fail=1
    return
  fi
  if [[ "${output}" != *"${needle}"* ]]; then
    echo "  CONTROL FAILED: passed without naming '${needle}'" >&2
    fail=1
    return
  fi
  echo "  passed and named: ${needle}"
}

run_control "describe returns kOfxStatReplyDefault" "FAIL kOfxActionDescribe: kOfxStatReplyDefault" \
  "${TMP}/bad_status.ofx" --id com.testkit.negative.badstatus --param gain:double

run_control "parameter declared with the wrong type" "FAIL param 'gain' type: OfxParamTypeInteger (expected OfxParamTypeDouble)" \
  "${TMP}/bad_type.ofx" --id com.testkit.negative.badtype --param gain:double

run_control "declared context region ignored by render" \
  "FAIL roi context: the inset window matches the whole frame bit for bit" \
  "${TMP}/roi_ignored.ofx" --id com.testkit.negative.roiignored \
  --clip Source --clip Output --concurrency-threads 0 --roi-context \
  --size 64x48 --src "${TMP}/roi_source.bin"

run_pass "declared context region honoured by render" "${ROI_ARGS[@]}"

run_pass_mentioning "global effect declares no context" \
  "second half skipped - the plug-in declares no context region" \
  "${TMP}/roi_global.ofx" --id com.testkit.control.globalroi \
  --clip Source --clip Output --concurrency-threads 0 --roi-context \
  --size 64x48 --src "${TMP}/roi_source.bin"

TILED_ARGS=(--clip Source --clip Output --concurrency-threads 0
            --roi-context-tolerance 0.02 --size 64x48 --src "${TMP}/roi_source.bin")

run_control "internally tiled render ignores the declared context" \
  "FAIL roi context: the inset window stays within the tolerance of the whole frame" \
  "${TMP}/roi_tiled_ignored.ofx" --id com.testkit.negative.roiignoredtiled \
  "${TILED_ARGS[@]}"

run_pass "internally tiled render honours the declared context" \
  "${TMP}/roi_tiled_ok.ofx" --id com.testkit.control.roioktiled "${TILED_ARGS[@]}"

LABEL_ARGS=(--clip Source --clip Output --concurrency-threads 0
            --size 64x48 --src "${TMP}/roi_source.bin")

run_control "label parameter declared and never written" \
  "FAIL label parameter 'provider' = " \
  "${TMP}/roi_no_label.ofx" --id com.testkit.negative.nolabel \
  --label-param provider "${LABEL_ARGS[@]}"

run_pass "label parameter written by the plug-in" \
  "${TMP}/roi_label_ok.ofx" --id com.testkit.control.labelok \
  --label-param provider "${LABEL_ARGS[@]}"

CLIP_ARGS=(--clip Source --clip Output --concurrency-threads 0
           --size 64x48 --src "${TMP}/roi_source.bin"
           --label-param "mediaId=take1" --clip-prop mediaID=take1
           --clip-prop-change mediaID=take2)

run_pass "clip property change reaches a plug-in that re-reads it" \
  "${TMP}/clip_echo.ofx" --id com.testkit.control.clipecho "${CLIP_ARGS[@]}"

run_control "plug-in caches the clip property on the handle" \
  "FAIL label parameter 'mediaId' follows the clip property" \
  "${TMP}/clip_stale.ofx" --id com.testkit.negative.clipstale "${CLIP_ARGS[@]}"

MARK_ARGS=(--clip Source --clip Output --concurrency-threads 0
           --size 64x48 --src "${TMP}/roi_source.bin" --mark "24,8")

run_pass "mark drawn in a bottom-up frame arrives where the frame says" \
  "${TMP}/roi_mark.ofx" --id com.testkit.control.mark "${MARK_ARGS[@]}"
run_pass "mark drawn with a bottom-up, negative-pitch frame" \
  "${TMP}/roi_mark.ofx" --id com.testkit.control.mark --negative-pitch "${MARK_ARGS[@]}"

run_control "mark drawn into a top-down frame is mirrored" \
  "FAIL overlay mark at (24,8)" \
  "${TMP}/roi_mark.ofx" --id com.testkit.control.mark --image-origin top \
  "${MARK_ARGS[@]}"

run_control "mark drawn into a top-down, negative-pitch frame is mirrored" \
  "FAIL overlay mark at (24,8)" \
  "${TMP}/roi_mark.ofx" --id com.testkit.control.mark --image-origin top \
  --negative-pitch "${MARK_ARGS[@]}"

echo
if [[ ${fail} -ne 0 ]]; then
  echo "negative controls FAILED: the harness does not detect the faults above" >&2
  exit 1
fi
echo "negative controls: every fault detected, the pattern still passes, harness is not a rubber stamp"
