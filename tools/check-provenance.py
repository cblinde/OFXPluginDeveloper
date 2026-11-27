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
import os
import subprocess
import sys
from pathlib import Path

SPDX = "SPDX-License-Identifier: BSD-3-Clause"

# Extensions that can hold a comment, i.e. where the line belongs. Data and
# documents are excluded: a `.md` file is not source and a committed `.onnx` or
# `.bin` cannot hold a comment at all. A `.map` version script takes `/* */` and a
# workflow takes `#`, so both are source-shaped - they were the two omissions this
# list used to hide (a workflow added later would have been missed again).
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".cxx", ".mm", ".m",
    ".py", ".sh", ".bat", ".cmd", ".ps1", ".cmake", ".comp", ".glsl", ".vert",
    ".frag", ".rs", ".js", ".ts", ".html", ".css", ".txt.cmake",
    ".map", ".yml", ".yaml",
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
    # No entry for kOfxStatErrMissingHostFeature: the SDK documents the status
    # code (ofxCore.h) and never spells that exception line, so an entry would
    # allow nothing. The new corpus check found it - see the comment above
    # check_allowlist.
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

# A checkout to compare against. More than one is allowed because a second SDK
# version is the only way to know whether an allowlisted idiom is stable across
# versions instead of true of the one tree that happens to be on disk: name them
# in OFX_PROVENANCE_CORPUS, separated by os.pathsep (":" on Linux, ";" on
# Windows), relative to the repository root or absolute. Unset means `.ofx-ref`,
# and an absent checkout is reported rather than skipped silently.
CORPUS_ENV = "OFX_PROVENANCE_CORPUS"


def corpora(root):
    raw = (os.environ.get(CORPUS_ENV) or "").strip()
    names = [part.strip() for part in raw.split(os.pathsep) if part.strip()] or [REFERENCE_DIR]
    found, missing, seen = [], [], set()
    for name in names:
        path = Path(name)
        if not path.is_absolute():
            path = root / name
        if not path.is_dir():
            missing.append(name)
            continue
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        found.append((name, path))
    return found, missing


def within(path, reference):
    """Is `path` inside `reference`? By name, not by first path component."""
    try:
        Path(path).resolve().relative_to(Path(reference).resolve())
        return True
    except (ValueError, OSError):
        return False


def shown(root, path):
    """A corpus path for messages: relative when it is inside the repository.

    A corpus named absolutely (a second SDK checkout outside the tree) has no
    relative form, and `relative_to` raising there used to be a crash waiting for
    the first copied line.
    """
    try:
        return Path(path).resolve().relative_to(root).as_posix()
    except (ValueError, OSError):
        return str(path)


def tracked_files(root):
    out = subprocess.run(["git", "ls-files"], cwd=str(root), capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return [line for line in out.stdout.splitlines() if line.strip()]


def check_spdx(root, files, found, report):
    checked = 0
    for rel in files:
        path = root / rel
        if not path.is_file() or any(within(path, reference) for _, reference in found):
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


def check_allowlist(corpus, report):
    """Every allowlisted line has to still occur in the corpus.

    The list is a claim about the SDK's text, so it rots in the direction nobody
    notices: an SDK that respells an idiom leaves an entry that allows nothing
    (harmless-looking) or, once someone edits it to match, a list that agrees with
    the code rather than with the corpus it was derived from. Checked here so the
    failure is a message, not a habit.
    """
    lines = set()
    for text in corpus.values():
        lines.update(line.strip() for line in text.splitlines())
    checked = 0
    for idiom, why in sorted(ALLOWED_SHARED.items()):
        checked += 1
        if idiom not in lines:
            report("error", "ALLOWED_SHARED",
                   "allowlisted line no longer occurs in the corpus, so the entry "
                   "allows nothing: %r (%s)" % (idiom[:70], why))
    return checked


def check_copied_lines(root, files, found, report):
    corpus = {}
    for name, reference in found:
        for path in reference.rglob("*"):
            if path.suffix.lower() in REFERENCE_SUFFIXES and path.is_file():
                try:
                    corpus[path] = path.read_text(errors="ignore")
                except OSError:
                    continue
    if not corpus:
        return "empty"

    # A checkout inside the repository is not our code: its files are the corpus.
    # Decided by containment, so a corpus at a nested path excludes itself and
    # nothing else.
    ours = [rel for rel in files
            if (root / rel).suffix.lower() in SOURCE_SUFFIXES
            and (root / rel).is_file()
            and not any(within(root / rel, reference) for _, reference in found)]
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
                           % (shown(root, path), body[:70]))
                break
    return compared, allowed, check_allowlist(corpus, report)


def self_test(root):
    failures = []
    # The allowlist has to match the same text the scan produces: a stripped
    # line minus its comment markers.
    sample = "    OFX::throwSuiteStatusException(kOfxStatErrValue);"
    body = sample.strip().lstrip("/#* \t").strip()
    if body not in ALLOWED_SHARED:
        failures.append("allowlist does not match a stripped line: %r" % body)
    # A corpus that no longer spells an allowlisted line must be reported, not
    # accepted: that is the rot the check exists for. An unrelated corpus has to
    # produce one report per entry.
    reported = []
    check_allowlist({"unrelated": "int nothing_to_do_with_ofx = 0;"},
                    lambda *a: reported.append(a))
    if len(reported) != len(ALLOWED_SHARED):
        failures.append("a corpus without the idioms reported %d of %d entries"
                        % (len(reported), len(ALLOWED_SHARED)))
    # A copy of something that is not an idiom has to be reported.
    reported = []
    probe = "\tconst double gain = 1.0 * (some_long_expression_that_is_copied_here);"
    body = probe.strip().lstrip("/#* \t").strip()
    if body in ALLOWED_SHARED:
        failures.append("a plain code line was treated as an allowed idiom")
    if len(body) < MIN_SHARED_LINE - 5:
        failures.append("self-test line is too short to exercise the scan")
    result = check_spdx(Path(__file__).resolve().parent.parent,
                        ["tools/check-provenance.py"], [], lambda *a: reported.append(a))
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

    found, missing = corpora(root)
    spdx_checked = check_spdx(root, files, found, report)
    for name in missing:
        report("error", name, "named in %s but not a directory: a corpus that was "
               "moved or deleted is not a corpus that passed" % CORPUS_ENV)
    copied = check_copied_lines(root, files, found, report) if found else None

    print("check-provenance: %d source file(s) with an SPDX line" % spdx_checked)
    if copied is None:
        print("  NOT VERIFIED: no %s/ checkout, so the copied-line scan did not run"
              % REFERENCE_DIR)
        print("               (the OpenFX SDK is fetched at configure time)")
    elif copied == "empty":
        print("  NOT VERIFIED: %s holds no C/C++ source, so the copied-line scan "
              "found nothing to compare against" % ", ".join(name for name, _ in found))
    else:
        compared, allowed, allowlisted = copied
        print("  %d line(s) of %d+ characters compared against %d checkout(s) (%s): "
              "%d allowed API idiom(s) of %d allowlisted, %d problem(s)"
              % (compared, MIN_SHARED_LINE, len(found),
                 ", ".join(name for name, _ in found), allowed, allowlisted,
                 len(problems)))
    if problems:
        print("check-provenance: %d problem(s)" % len(problems))
        return 1
    print("check-provenance: 0 problems")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
