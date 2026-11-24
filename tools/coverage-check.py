#!/usr/bin/env python3
"""Prove that a restructure lost no technical content.

The claim "nothing was deleted" is not checkable by eye across 6,700 lines, so it
is checked mechanically: every non-trivial line of every Markdown file in the
repository at HEAD must still exist somewhere in the working tree afterwards.

A "fact" is a line of 30 characters or more, after normalisation:

  * link targets are removed      `[text](file.md#anchor)` -> `[text]`
  * section references collapse   `§5.3`, `§13.1` -> `§`
  * whitespace collapses, CRLF is irrelevant

Headings and table separator rows are skipped because they are structure, and
restructuring is allowed to change structure. Everything else - every table row,
every bullet, every code line, every measured number, every host quirk - is a
fact and must survive.

Usage:
  python3 tools/coverage-check.py baseline    # record HEAD before moving a file
  python3 tools/coverage-check.py check       # verify the working tree against it
  python3 tools/coverage-check.py check --show 40

Exit status: 0 when every fact is present, 1 when any is missing, 2 on a usage or
baseline error.
"""

import json
import os
import re
import subprocess
import sys

BASELINE = os.path.join(".validation", "coverage-baseline.json")
MIN_LENGTH = 30

SEPARATOR = re.compile(r"^\|[-:\s|]+\|$")


def normalise(line):
    text = line.strip()
    if len(text) < MIN_LENGTH:
        return None
    if text.startswith("#"):
        return None
    if SEPARATOR.match(text):
        return None
    text = re.sub(r"\]\([^)]*\)", "]", text)
    text = re.sub(r"§\s*\d+(?:\.\d+)?", "§", text)
    text = re.sub(r"\s+", " ", text)
    return text


def facts_from(text):
    found = set()
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        fact = normalise(line)
        if fact:
            found.add(fact)
    return found


def markdown_at_head():
    """Every tracked .md path at HEAD, with its text."""
    # encoding='utf-8' explicitly: the default is the console code page (cp1252
    # on Windows), and the documents contain em dashes and arrows that cp1252
    # cannot decode - the first run of this script died on exactly that.
    out = subprocess.run(["git", "ls-files", "*.md"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", check=True)
    paths = [p for p in out.stdout.split() if p.lower().endswith(".md")]
    texts = {}
    for path in paths:
        blob = subprocess.run(["git", "show", "HEAD:" + path], capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
        if blob.returncode == 0 and blob.stdout is not None:
            texts[path] = blob.stdout
    return texts


def markdown_in_tree():
    texts = {}
    for root, dirs, files in os.walk("."):
        # 'build' and '_deps' matter: FetchContent drops openfx/expat documentation
        # into templates/*/build/_deps, and those copies would "prove" a fact
        # survived a deletion that actually happened in our own tree.
        dirs[:] = [d for d in dirs
                   if d not in {".git", ".validation", ".ofx-ref", ".freebuff", "node_modules",
                                "build", "_deps"}]
        for name in files:
            if not name.lower().endswith(".md"):
                continue
            path = os.path.join(root, name).replace("\\", "/")
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    texts[path.lstrip("./")] = handle.read()
            except OSError:
                pass
    return texts


def baseline():
    texts = markdown_at_head()
    if not texts:
        print("error: no Markdown at HEAD", file=sys.stderr)
        return 2
    per_file = {path: sorted(facts_from(text)) for path, text in texts.items()}
    every = set()
    for values in per_file.values():
        every.update(values)
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w", encoding="utf-8") as handle:
        json.dump({"files": per_file, "facts": sorted(every)}, handle, indent=0)
    lines = sum(len(t.splitlines()) for t in texts.values())
    print(f"baseline: {len(texts)} files, {lines} lines, {len(every)} distinct facts "
          f"-> {BASELINE}")
    return 0


def check(show):
    if not os.path.exists(BASELINE):
        print(f"error: no baseline at {BASELINE}; run 'baseline' at HEAD first", file=sys.stderr)
        return 2
    with open(BASELINE, encoding="utf-8") as handle:
        data = json.load(handle)
    expected = set(data["facts"])
    texts = markdown_in_tree()
    found = set()
    for text in texts.values():
        found.update(facts_from(text))

    missing = sorted(expected - found)
    print(f"baseline facts : {len(expected)}")
    print(f"present now    : {len(expected) - len(missing)}")
    print(f"missing        : {len(missing)}")
    if missing:
        for fact in missing[:show]:
            print("  MISSING " + fact[:150])
        if len(missing) > show:
            print(f"  ... {len(missing) - show} more")
        print("RESULT FAIL")
        return 1

    # Where the facts live now, so the report shows the move actually happened.
    counts = sorted(((len(facts_from(t)), p) for p, t in texts.items()), reverse=True)
    print("files now      : " + str(len(counts)))
    for count, path in counts[:8]:
        print(f"  {count:5d}  {path}")
    print("RESULT PASS: every HEAD fact survives, none dropped")
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] not in {"baseline", "check"}:
        print(__doc__, file=sys.stderr)
        return 2
    if argv[1] == "baseline":
        return baseline()
    show = 40
    if "--show" in argv:
        index = argv.index("--show")
        if index + 1 < len(argv):
            show = int(argv[index + 1])
    return check(show)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
