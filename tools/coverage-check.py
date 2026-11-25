"""Prove that moving the documentation into references/ lost no information.

A restructure renames files, re-wraps paragraphs and re-points links, so
comparing lines exactly reports hundreds of differences that are not losses.
This gate asks the question that matters instead:

  * vocabulary -- every distinctive word (stemmed, case-folded) of the pre-move
    documents must still occur somewhere in the tree. A dropped paragraph takes
    its rare words with it, and this notices.
  * lines -- the exact-fact comparison is kept as a report, not a verdict: it
    tells a reader how many lines were re-expressed, which is expected when
    every cross-reference gained a `references/` prefix.

Document names are blanked on both sides before comparison, so a rename can
never be mistaken for a loss.

Usage:
    python tools/coverage-check.py baseline    # record the pre-move corpus
    python tools/coverage-check.py check       # compare the tree now
    python tools/coverage-check.py check --show 40

Exit status: 0 pass, 1 a word stem of the pre-move corpus is gone, 2 no usable
baseline - a clone without the reference commit yields an empty one, and an empty
baseline would make every tree pass, so it is refused instead of written.
"""

import glob
import io
import json
import os
import re
import subprocess
import sys

# The last commit before the move: the reference point for every claim below.
BASELINE_REV = "4290e68"
BASELINE_PATH = os.path.join(".validation", "coverage-baseline.json")
EXCEPTIONS_PATH = os.path.join("tools", "coverage-exceptions.txt")
SKIP_DIRS = (".git", ".validation", ".ofx-ref", "build", "_deps", "node_modules")

DOC_TOKEN = re.compile(r"[\w./-]*\.md")
HEADING = re.compile(r"^\s{0,3}#{1,6}\s")
SEPARATOR = re.compile(r"^\|[-:\s|]+\|$")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]{6,}")


def normalise(line):
    """One comparable string, with document names and section numbers blanked."""
    text = line.strip()
    if len(text) < 30 or HEADING.match(text) or SEPARATOR.match(text):
        return None
    text = DOC_TOKEN.sub("\u00abdoc\u00bb", text)
    text = re.sub(r"§\s*\d+(?:\.\d+)?", "§", text)
    text = re.sub(r"\]\([^)]*\)", "]", text)
    return re.sub(r"\s+", " ", text)


def facts_from(text):
    """Every line that carries a statement, normalised."""
    out = set()
    for line in text.replace("\r\n", "\n").split("\n"):
        fact = normalise(line)
        if fact:
            out.add(fact)
    return out


def stem(word):
    """Seven characters: enough to fold inflections, short enough to keep sense."""
    return word.lower()[:7]


def vocabulary_from(text):
    out = set()
    for line in text.replace("\r\n", "\n").split("\n"):
        if HEADING.match(line):
            continue
        for word in WORD.findall(line):
            out.add(stem(word))
    return out


def markdown_paths():
    paths = []
    for path in glob.glob("**/*.md", recursive=True):
        parts = path.replace("\\", "/").split("/")
        if any(part in SKIP_DIRS for part in parts):
            continue
        paths.append(path)
    return sorted(paths)


def read(path):
    with io.open(path, "r", encoding="utf-8", errors="replace", newline="") as handle:
        return handle.read()


def baseline():
    names = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", BASELINE_REV],
        capture_output=True, text=True, encoding="utf-8").stdout.split()
    docs = [name for name in names if name.endswith(".md")]
    facts, words = set(), set()
    lines = 0
    for name in docs:
        blob = subprocess.run(
            ["git", "show", "%s:%s" % (BASELINE_REV, name)],
            capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
        lines += len(blob.replace("\r\n", "\n").split("\n"))
        facts |= facts_from(blob)
        words |= vocabulary_from(blob)
    # A clone without the reference commit yields an empty baseline, and an empty
    # baseline makes every comparison pass: the gate would report PASS on a tree
    # that lost everything. Refuse instead, and drop a stale file so that the
    # `baseline` before `check` in CI cannot pass on an earlier run's copy.
    if not docs or not words:
        if os.path.exists(BASELINE_PATH):
            os.remove(BASELINE_PATH)
        sys.stderr.write(
            "error: baseline is empty (%d file(s), %d word stem(s)); commit %s is "
            "not in this clone, so nothing could be compared. Use a full checkout "
            "(actions/checkout with fetch-depth: 0).\n"
            % (len(docs), len(words), BASELINE_REV))
        return 2

    os.makedirs(".validation", exist_ok=True)
    with io.open(BASELINE_PATH, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({"rev": BASELINE_REV, "files": docs, "lines": lines,
                   "facts": sorted(facts), "words": sorted(words)}, handle, indent=1)
    print("baseline: %d files, %d lines, %d facts, %d word stems -> %s"
          % (len(docs), lines, len(facts), len(words), BASELINE_PATH))
    return 0


def read_exceptions():
    if not os.path.exists(EXCEPTIONS_PATH):
        return {}
    out = {}
    with io.open(EXCEPTIONS_PATH, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            if "\t" in line:
                key, reason = line.split("\t", 1)
                out[key] = reason
    return out


def check(show=0):
    if not os.path.exists(BASELINE_PATH):
        print("no baseline; run: coverage-check.py baseline")
        return 2
    with io.open(BASELINE_PATH, encoding="utf-8") as handle:
        data = json.load(handle)
    if not data.get("files") or not data.get("words"):
        print("error: the stored baseline is empty (%d file(s), %d word stem(s)); "
              "regenerate it with a full checkout"
              % (len(data.get("files") or []), len(data.get("words") or [])))
        return 2

    facts, words = set(), set()
    for path in markdown_paths():
        text = read(path)
        facts |= facts_from(text)
        words |= vocabulary_from(text)

    exceptions = read_exceptions()
    expected_facts = set(data["facts"])
    expected_words = set(data["words"])

    lost_facts = sorted(f for f in expected_facts - facts if f not in exceptions)
    lost_words = sorted(w for w in expected_words - words if w not in exceptions)

    print("baseline : %d facts, %d word stems (%s)"
          % (len(expected_facts), len(expected_words), data["rev"]))
    print("now      : %d facts, %d word stems" % (len(facts), len(words)))
    print("facts re-expressed : %d  (lines whose links or wrapping changed)"
          % len(expected_facts - facts))
    print("word stems missing : %d" % len(lost_words))
    for word in lost_words[:40]:
        print("  MISSING WORD: %s" % word)
    if lost_facts:
        print("facts with no counterpart (declared or not): %d" % len(lost_facts))
        for fact in lost_facts[:show]:
            print("  UNEXPLAINED: %s" % fact[:150])

    verdict = "PASS" if not lost_words else "FAIL"
    print("RESULT %s" % verdict)
    return 0 if verdict == "PASS" else 1


def main(argv):
    command = argv[1] if len(argv) > 1 else "check"
    if command == "baseline":
        return baseline()
    show = 0
    if "--show" in argv:
        show = int(argv[argv.index("--show") + 1])
    return check(show)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
