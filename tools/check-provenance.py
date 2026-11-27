#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the two provenance claims this repository makes about its own source.

    python3 tools/check-provenance.py [--root .] [--self-test]

Claim 1 - every source file declares the licence. `LICENSE` is BSD-3-Clause and
`THIRD_PARTY_NOTICES.md` says each source file carries
`SPDX-License-Identifier: BSD-3-Clause`. That is checkable, so it is checked: a
file without the line is a file whose licence a reader has to guess.

Claim 2 - nothing was copied from the OpenFX SDK into `src/`. The SDK is fetched
into `.ofx-ref/` for the build, and a plug-in has to call the same API the SDK's
own examples call, so a few identical lines are expected - `throwSuiteStatusException`,
`addSupportedContext`, `setRenderWindow`. Those are idioms, listed below with the
reason each one is unavoidable. Any *other* line of 45 characters or more that
also appears verbatim in the SDK is reported: that is what copying looks like, and
a line-level match is cheap enough to run on every build.

What this cannot see: a copy that was reworded, a table transcribed by hand, or a
match against a project that is not in `.ofx-ref/` (the GPL projects read for
design live online - `references/reference-projects.md` records what each one
contributed). The check is a floor, not a proof of originality.

Without `.ofx-ref/` (an offline checkout, CI before FetchContent) claim 2 is
printed as NOT VERIFIED and the exit status stays 0: a machine that has not
fetched the SDK has not answered the question, and saying so is not a failure.

Exit status: 0 both claims hold or claim 2 was not answerable, 1 a problem,
2 this tool cannot run here (no git, no files).
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

SPDX = "SPDX-License-Identifier: BSD-3-Clause"

# Extensions that can hold a `#`/`//` comment, i.e. where the line belongs. Data,
# documents and linker scripts are excluded by name or extension: a `.map` version
# script has its own comment rules, and a `.md` file is not source.
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".cxx", ".mm", ".m",
    ".py", ".sh", ".bat", ".cmd", ".ps1", ".cmake", ".comp", ".glsl", ".vert",
    ".frag", ".rs", ".js", ".ts", ".html", ".css", ".txt.cmake",
}

# Files that are source-shaped but must not carry the line.
EXEMPT_NAMES = {"LICENSE", "LICENCE", "COPYING", ".gitignore", "CMakeLists.txt"}

# Claim 2: lines that appear verbatim in the OpenFX SDK and have to, because they
# are the SDK's API and its own examples use the same spelling. Keyed by the
# stripped line; the value is why it is not a copy.
ALLOWED_SHARED = {
    "OFX::throwSuiteStatusException(kOfxStatErrUnsupported);":
        "the SDK's documented way to refuse an unsupported call",
    "OFX::throwSuiteStatusException(kOfxStatErrValue);":
        "the SDK's documented way to refuse a bad property value",
    "OFX::throwSuiteStatusException(kOfxStatErrMissingHostFeature);":
        "the SDK's documented way to refuse a missing host feature",
    "const OfxRectI& dstBounds = _dstImg->getBounds();":
        "a getter call with its return type; there is one way to write it",
    "desc.addSupportedContext(OFX::eContextFilter);":
        "a descriptor call every image effect makes",
    "desc.addSupportedContext(OFX::eContextGeneral);":
        "a descriptor call every image effect makes",
    "processor.setRenderWindow(args.renderWindow);":
        "the Support library's own render-window handover",
}

MIN_SHARED_LINE = 45
REFERENCE_DIR = ".ofx-ref"
REFERENCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".cxx"}


def tracked_files(root):
    out = subprocess.run(["git", "ls-files"], cwd=str(root), capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return [line for line in out.stdout.splitlines() if line.strip()]


def check_spdx(root, files, report):
    checked = 0
    for rel in files:
        path = root / rel
        if rel.startswith(REFERENCE_DIR + "/") or not path.is_file():
            continue
        if path.name in EXEMPT_NAMES:
            continue
        if path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        checked += 1
        try:
            head = path.read_text(errors="replace").splitlines()[:4]
        except OSError as exc:
            report("error", rel, "cannot read: %s" % exc)
            continue
        if not any(SPDX in line for line in head):
            report("error", rel, "no '%s' in the first four lines" % SPDX)
    return checked


def check_copied_lines(root, files, report):
    reference = root / REFERENCE_DIR
    if not reference.is_dir():
        return None
    corpus = {}
    for path in reference.rglob("*"):
        if path.suffix.lower() in REFERENCE_SUFFIXES and path.is_file():
            try:
                corpus[path] = path.read_text(errors="ignore")
            except OSError:
                continue
    if not corpus:
        return None

    ours = [rel for rel in files
            if rel.split("/")[0] != REFERENCE_DIR
            and (root / rel).suffix.lower() in SOURCE_SUFFIXES
            and (root / rel).is_file()]
    compared = 0
    allowed = 0
    for rel in ours:
        text = (root / rel).read_text(errors="ignore")
        for number, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if len(stripped) < MIN_SHARED_LINE:
                continue
            if stripped.startswith(("#include", "#import", "// SPDX", "# SPDX")):
                continue
            body = stripped.lstrip("/#* \t").strip()
            if len(body) < MIN_SHARED_LINE - 5:
                continue
            compared += 1
            for path, text_of in corpus.items():
                if body not in text_of:
                    continue
                if body in ALLOWED_SHARED:
                    allowed += 1
                else:
                    report("error", "%s:%d" % (rel, number),
                           "appears verbatim in %s and is not an allowed API idiom: %r"
                           % (path.relative_to(root).as_posix(), body[:70]))
                break
    return compared, allowed


def self_test(root):
    failures = []
    # The allowlist has to match the same text the scan produces: a stripped
    # line minus its comment markers.
    sample = "    OFX::throwSuiteStatusException(kOfxStatErrValue);"
    body = sample.strip().lstrip("/#* \t").strip()
    if body not in ALLOWED_SHARED:
        failures.append("allowlist does not match a stripped line: %r" % body)
    # A copy of something that is not an idiom has to be reported.
    reported = []
    probe = "\tconst double gain = 1.0 * (some_long_expression_that_is_copied_here);"
    body = probe.strip().lstrip("/#* \t").strip()
    if body in ALLOWED_SHARED:
        failures.append("a plain code line was treated as an allowed idiom")
    if len(body) < MIN_SHARED_LINE - 5:
        failures.append("self-test line is too short to exercise the scan")
    result = check_spdx(Path(__file__).resolve().parent.parent,
                        ["tools/check-provenance.py"], lambda *a: reported.append(a))
    if result != 1 or reported:
        failures.append("this tool's own file did not pass the SPDX check")
    for line in failures:
        print("  FAIL: %s" % line)
    print("self-test: %d failure(s)" % len(failures))
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()

    if args.self_test:
        return self_test(root)

    files = tracked_files(root)
    if files is None:
        print("check-provenance: no git repository at %s; nothing checked" % root)
        return 2

    problems = []

    def report(kind, where, message):
        problems.append((kind, where, message))
        print("  %s: %s: %s" % (kind, where, message))

    spdx_checked = check_spdx(root, files, report)
    copied = check_copied_lines(root, files, report)

    print("check-provenance: %d source file(s) with an SPDX line" % spdx_checked)
    if copied is None:
        print("  NOT VERIFIED: no %s/ checkout, so the copied-line scan did not run"
              % REFERENCE_DIR)
        print("               (the OpenFX SDK is fetched at configure time)")
    else:
        compared, allowed = copied
        print("  %d line(s) of %d+ characters compared against the OpenFX SDK: %d allowed "
              "API idiom(s), %d problem(s)"
              % (compared, MIN_SHARED_LINE, allowed, len(problems)))
    if problems:
        print("check-provenance: %d problem(s)" % len(problems))
        return 1
    print("check-provenance: 0 problems")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
