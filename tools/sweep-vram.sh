#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Find the largest tile a model can run in, by ramping the tile size until ONNX
# Runtime runs out of memory.
#
#   tools/sweep-vram.sh model.onnx
#   tools/sweep-vram.sh model.onnx --profiles ONNXLoader.profiles --start 256 --step 256 --max 4096
#
# The tile is square: `--start`, `--step` and `--max` are core pixels, and the
# input handed to the model is the core grown by the halo on every side — a
# plug-in that declares a halo asks the host for that context, so the memory a
# tile really costs is `(core + 2*halo)^2`, not `core^2`.
#
# The halo comes from `ONNXLoader.profiles` (the profiles file the ONNX Loader
# plug-in writes), read as text: a `halo` key outside any section, or under a
# section named after the model file (or its stem). `halo = 2`, `halo=2` and
# `halo: 2` are all accepted; `--halo N` overrides the file.
#
# The sweep stops at the first size that fails and prints that failure's first
# line, because a CUDA OOM message is the only evidence that the ceiling is
# memory and not arithmetic. A failure that is not an allocation failure is
# reported as such: a model that reshapes to H/8 fails at 63 for a reason no
# GPU can fix.
#
# Needs an NVIDIA GPU with the CUDA execution provider, which is why the
# no-GPU path is the one tested here: it exits 3 and says what is missing rather
# than printing an empty table. `--provider CPUExecutionProvider` ramps and times
# on the CPU, which exercises everything except the OOM, and says so in its
# output.
#
# Exit status: 0 the sweep ran (with or without an OOM), 1 nothing ran at all,
# 2 bad usage or a missing dependency or halo, 3 no GPU, so nothing was measured.
set -uo pipefail

usage()
{
  # The header above `set -uo pipefail` is the manual; print all of it rather
  # than a line range that drifts when the header grows.
  awk 'NR > 2 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
}

MODEL=""
PROFILES="ONNXLoader.profiles"
HALO=""
PROVIDER=""
START=256
STEP=256
MAX=4096
ITERATIONS=3
WARMUP=1

is_uint()
{
  [[ "$1" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profiles)
      [[ $# -ge 2 ]] || { echo "error: --profiles needs a path" >&2; exit 2; }
      PROFILES="$2"; shift 2 ;;
    --halo|--provider|--start|--step|--max|--iterations|--warmup)
      [[ $# -ge 2 ]] || { echo "error: $1 needs a value" >&2; exit 2; }
      case "$1" in
        --halo) HALO="$2" ;;
        --provider) PROVIDER="$2" ;;
        --start) START="$2" ;;
        --step) STEP="$2" ;;
        --max) MAX="$2" ;;
        --iterations) ITERATIONS="$2" ;;
        --warmup) WARMUP="$2" ;;
      esac
      shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    -*)
      echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    *)
      if [[ -n "${MODEL}" ]]; then
        echo "error: one model per run" >&2; exit 2
      fi
      MODEL="$1"; shift ;;
  esac
done

if [[ -z "${MODEL}" ]]; then
  usage >&2
  exit 2
fi
if [[ ! -f "${MODEL}" ]]; then
  echo "error: no such file: ${MODEL}" >&2
  exit 2
fi
if [[ -n "${HALO}" ]] && ! is_uint "${HALO}"; then
  echo "error: --halo wants a pixel count, got '${HALO}'" >&2
  exit 2
fi
for pair in "start:${START}" "step:${STEP}" "max:${MAX}" \
            "iterations:${ITERATIONS}" "warmup:${WARMUP}"; do
  if ! is_uint "${pair#*:}"; then
    echo "error: --${pair%%:*} wants a non-negative number, got '${pair#*:}'" >&2
    exit 2
  fi
done
if (( STEP == 0 )); then
  echo "error: --step must not be zero" >&2
  exit 2
fi
if (( MAX < START )); then
  echo "error: --max (${MAX}) is below --start (${START})" >&2
  exit 2
fi

PYTHON="${PYTHON:-python3}"
if ! command -v "${PYTHON}" >/dev/null 2>&1; then
  echo "error: ${PYTHON} not found; this script drives ONNX Runtime through it" >&2
  exit 2
fi

exec "${PYTHON}" - "${MODEL}" "${PROFILES}" "${HALO}" "${PROVIDER}" \
  "${START}" "${STEP}" "${MAX}" "${ITERATIONS}" "${WARMUP}" <<'PY'
"""Ramp the tile size until ONNX Runtime runs out of memory. See the header."""

import re
import shutil
import statistics
import sys
import time
from pathlib import Path

try:
    import numpy as np
    import onnxruntime as ort
except ImportError:  # pragma: no cover - the message is the point
    sys.stderr.write(
        "error: this script needs onnxruntime and numpy: pip install onnxruntime numpy\n"
    )
    raise SystemExit(2)

model_path, profiles_path, halo_arg, provider_arg, start, step, cap, iterations, warmup = (
    sys.argv[1:10]
)
start, step, cap, iterations, warmup = (
    int(start), int(step), int(cap), int(iterations), int(warmup)
)
model = Path(model_path)

# The markers a runtime uses when it has no memory left. One of these in the
# first line is what turns "it failed" into "the ceiling is memory".
OOM_MARKERS = (
    "out of memory",
    "out_of_memory",
    "oom",
    "failed to allocate",
    "bad_alloc",
    "allocator",
    "cuda_error_out_of_memory",
)


def read_halo(path, model_file):
    """The halo for this model, from a profiles file. Returns (value, where)."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None, None
    section = None
    wanted = {model_file.name.lower(), model_file.stem.lower()}
    outside = None
    inside = None
    pattern = re.compile(r"^\s*halo\s*[:=]\s*(\d+)\s*$", re.IGNORECASE)
    header = re.compile(r"^\s*\[(.+?)\]\s*$")
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";", "//")):
            continue
        match = header.match(line)
        if match:
            section = match.group(1).strip().lower()
            continue
        match = pattern.match(line)
        if not match:
            continue
        if section is None:
            outside = outside if outside is not None else int(match.group(1))
        elif section in wanted:
            inside = inside if inside is not None else int(match.group(1))
    if inside is not None:
        return inside, "{} [{}]".format(profiles_path, model_file.name)
    if outside is not None:
        return outside, profiles_path
    return None, None


if halo_arg:
    halo, halo_where = int(halo_arg), "--halo"
else:
    halo, halo_where = read_halo(profiles_path, model)
if halo is None:
    sys.stderr.write(
        "error: no halo for {} in {}: add 'halo = 2' there (globally or under "
        "[{}]), point --profiles at the file that has it, or pass --halo N\n".format(
            model.name, profiles_path, model.name
        )
    )
    raise SystemExit(2)

provider = provider_arg or "CUDAExecutionProvider"
if "CUDA" in provider.upper() and shutil.which("nvidia-smi") is None:
    sys.stderr.write(
        "error: no nvidia-smi, so there is no NVIDIA GPU to measure{}\n"
        "       a VRAM sweep needs one: run this on the machine with the card, or\n"
        "       pass --provider CPUExecutionProvider to check the ramp without an\n"
        "       OOM (the CPU does not fail the way a GPU does)\n".format(
            "" if provider_arg else " (default provider is {})".format(provider)
        )
    )
    raise SystemExit(3)

options = ort.SessionOptions()
options.log_severity_level = 4
try:
    session = ort.InferenceSession(
        str(model), sess_options=options, providers=[provider]
    )
except Exception as exc:  # noqa: BLE001 - the message is the result
    sys.stderr.write(
        "error: {} could not be loaded on {}: {}\n".format(
            model.name, provider, str(exc).strip().splitlines()[0]
        )
    )
    raise SystemExit(1)

used = session.get_providers()
inputs = session.get_inputs()
channels = inputs[0].shape[1] if len(inputs[0].shape) > 1 else 3
if not isinstance(channels, int):
    channels = 3
feed_name = inputs[0].name

print("model        {}  ({} B)".format(model.name, model.stat().st_size))
print("halo         {} px, from {}".format(halo, halo_where))
print("provider     {}{}".format(provider, "" if used == [provider] else
                                "  (session ended up with {})".format(", ".join(used))))
if "CUDA" not in provider.upper():
    print("note         a CPU session does not run out of memory: this checks the "
          "ramp, the")
    print("             halo arithmetic and the table, not the ceiling")
print("steps        core {} .. {} step {}, input = core + 2*halo".format(start, cap, step))
print("")

header = "{:>8} {:>8} {:>11}  {}".format("core", "input", "median ms", "result")
print(header)
print("-" * len(header))

largest = None
failed_at = None
failure = None
is_oom = False

core = start
while core <= cap:
    side = core + 2 * halo
    y, x = np.mgrid[0:side, 0:side]
    planes = np.empty((channels, side, side), dtype=np.float32)
    for c in range(channels):
        planes[c] = ((x * (7 + c) + y * (13 + 3 * c)) % 101) / 101.0
    feed = {feed_name: planes.reshape(1, channels, side, side)}

    error = None
    times = []
    for _ in range(max(0, warmup)):
        try:
            session.run(None, feed)
        except Exception as exc:  # noqa: BLE001 - the message is the result
            error = str(exc).strip().splitlines()[0][:200]
            break
    if error is None:
        for _ in range(max(1, iterations)):
            started = time.perf_counter()
            try:
                session.run(None, feed)
            except Exception as exc:  # noqa: BLE001 - the message is the result
                error = str(exc).strip().splitlines()[0][:200]
                break
            times.append(time.perf_counter() - started)

    if error is None:
        median_ms = statistics.median(times) * 1000.0
        print("{:>8} {:>8} {:>11.3f}  ok".format(core, side, median_ms))
        largest = core
        core += step
        continue

    failed_at = core
    failure = error
    is_oom = any(marker in error.lower() for marker in OOM_MARKERS)
    print("{:>8} {:>8} {:>11}  {}".format(core, side, "-", "out of memory" if is_oom else "failed"))
    print("")
    print("first error  {}".format(failure))
    break

print("")
if largest is None:
    print("nothing ran: the first tile ({}) already failed".format(failed_at))
    raise SystemExit(1)
if failed_at is None:
    print("no OOM up to core {} (input {}); raise --max to keep going".format(largest, largest + 2 * halo))
    raise SystemExit(0)
if is_oom:
    print("largest core that ran: {} (input {}); OOM at core {} (input {})".format(
        largest, largest + 2 * halo, failed_at, failed_at + 2 * halo))
    print("the ceiling is memory: that is the number a tile budget starts from")
else:
    print("largest core that ran: {} (input {}); stopped at core {} for a reason "
          "that is not memory".format(largest, largest + 2 * halo, failed_at))
    print("fix that size before reading the sweep as a VRAM ceiling")
raise SystemExit(0)
PY
