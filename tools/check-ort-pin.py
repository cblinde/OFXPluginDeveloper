#!/usr/bin/env python3
"""Keep the pinned ONNX Runtime version and the documents that name it in step.

The pin lives in exactly one place - the `version` default of
`.github/actions/onnx-runtime/action.yml`, which both workflows call with no
argument - so bumping it is one field. The failure mode this check removes is the
other half: a document that says "CI installs 1.32.0" keeps saying it after the
pin moved, and the next person reads the wrong number and believes it.

What counts as naming the pin: a release-layout path such as
`onnxruntime-linux-x64-1.32.0`, which is a claim about what to download. Plain
mentions of a version elsewhere are *measurements* - "verified against 1.32.0" -
and those are supposed to keep their historical number, so they are counted and
printed, never failed.

Exit status: 1 when the pin cannot be read or a document disagrees with it, 0
otherwise.
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ACTION = os.path.join(ROOT, ".github", "actions", "onnx-runtime", "action.yml")
PIN_PATTERN = re.compile(r"onnxruntime-linux-x64-(?:gpu_cuda\d+-)?(\d+\.\d+\.\d+)")
VERSION_PATTERN = re.compile(r"(\d+\.\d+\.\d+)")


def read_pin():
    """The `default:` of the `version` input, and of no other input.

    Reading the first `default:` in the file works only while `version` is the
    first input; `package` and `platform` carry defaults too, and a second
    version-shaped one above it would be read as the pin without anything
    failing. So the walk is scoped: inside `inputs:`, track which input the line
    belongs to and take a default only from `version`.
    """
    with open(ACTION, encoding="utf-8") as fh:
        lines = fh.read().splitlines()

    in_inputs = False
    current = None
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not line.startswith(" "):
            in_inputs = stripped == "inputs:"
            current = None
            continue
        if not in_inputs:
            continue
        if len(line) - len(line.lstrip()) == 2 and stripped.endswith(":"):
            current = stripped[:-1]
            continue
        if current == "version" and stripped.startswith("default:"):
            match = VERSION_PATTERN.search(stripped)
            return match.group(1) if match else None
    return None


def documents():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [
            d
            for d in dirnames
            if d not in {".git", "evidence", "build", ".validation", ".venv", ".ort", "_deps"}
        ]
        for name in filenames:
            if name.endswith(".md"):
                yield os.path.join(dirpath, name)


def main() -> int:
    pin = read_pin()
    if pin is None:
        print(
            f"error: no default under inputs.version in {os.path.relpath(ACTION, ROOT)}",
            file=sys.stderr,
        )
        return 1
    print(f"pin: {pin} (the version default in .github/actions/onnx-runtime/action.yml)")

    claims = 0
    mismatches = []
    mentions = 0
    for path in documents():
        with open(path, encoding="utf-8") as fh:
            for number, line in enumerate(fh, start=1):
                for found in PIN_PATTERN.findall(line):
                    claims += 1
                    if found != pin:
                        mismatches.append(
                            f"{os.path.relpath(path, ROOT)}:{number}: names {found}, the pin is {pin}"
                        )
                if pin in line and not PIN_PATTERN.search(line):
                    mentions += 1

    for problem in mismatches:
        print(f"error: {problem}", file=sys.stderr)
    print(
        f"documents: {claims} release-layout path(s) naming a version, "
        f"{mentions} plain mention(s) of {pin} kept as measurements"
    )
    if mismatches:
        print("check-ort-pin: FAIL - a document and the pin disagree", file=sys.stderr)
        return 1
    print("check-ort-pin: the pin and every path that names one agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
