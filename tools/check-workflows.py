#!/usr/bin/env python3
"""The workflow files themselves: what floats, and what a YAML error breaks.

Why this exists: `.github/workflows/*.yml` is the one part of the tree nothing
here compiles. A `uses: actions/checkout@v4` is a moving tag that changes under a
green tick, and an indentation error surfaces only when a job starts - in CI,
after the push, in the job nobody is watching. `docs.yml` runs actionlint for the
schema and for the shell inside `run:`; that step downloads a binary and needs a
network. This script is the half that has to run locally, on a machine with
nothing installed, before the push.

What it checks, and nothing more:

* **Pins.** Every `uses:` in a workflow or in a composite action is
  `<owner>/<repo>[@<path>]@<40 hex characters>`, followed by a `# vX.Y.Z`
  comment so the pin can be read and bumped. A local `./` action is exempt.
* **Shape.** No tab indentation, no key twice in one mapping, a quoted or flow
  value that opens on a line has to close on that line, a block scalar's content
  is indented deeper than its key, every job has `runs-on` or a job-level `uses:`
  but not both, and the jobs are found by the indentation they actually use.

Two rules keep this from growing into a parser, and both were paid for by false
positives. A value is only looked at when it *starts* with a quote or a flow
bracket, so an apostrophe in a plain scalar and a `]` inside a quoted string are
none of its business - the previous version counted characters per line and
rejected a valid file for `echo "a ] b"`. And a flow collection has to close on
its own line: this tree writes them inline, a flow across lines is actionlint's
job, and the message says that rather than pretending to follow it.

It is not a YAML parser. An invalid file it accepts is actionlint's job; a valid
file it rejects is a bug here, and `--selftest` feeds it one case per check.
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

# No `indent` groups: the column a check needs is measured on the line, and a
# group nothing reads is a promise the code does not keep.
USES = re.compile(r"^ *(?:- +)?uses: *(?P<target>\S+)(?P<rest>.*)$")
KEY = re.compile(r"^ *(?:- +)?(?P<key>[A-Za-z_][\w.-]*):(?P<rest>.*)$")
BLOCK = re.compile(r"^ *(?:- +)?[A-Za-z_][\w.-]*: *[|>][+-]?\d* *$")
ITEM = re.compile(r"^ *- +(?P<rest>.*)$")
PINNED = re.compile(r"[0-9a-f]{40}")


class Report:
    def __init__(self):
        self.problems = []

    def error(self, path, line, message):
        self.problems.append("%s:%d: %s" % (path, line, message))


def strip_comment(line):
    """The line without its trailing `#` comment, quotes respected."""
    quote = None
    for index, character in enumerate(line):
        if quote:
            if character == quote:
                quote = None
        elif character in "'\"":
            quote = character
        elif character == "#" and (index == 0 or line[index - 1] in " \t"):
            return line[:index]
    return line


def value_of(line):
    """The scalar after the first `key:` or `- `, or "" when the line is a key."""
    match = KEY.match(line)
    if match is not None:
        return match.group("rest").strip()
    match = ITEM.match(line)
    if match is not None:
        return match.group("rest").strip()
    return ""


def flow_balance(text):
    """Bracket depth of one line, with quoted parts left alone."""
    depth = 0
    quote = None
    for character in text:
        if quote:
            if character == quote:
                quote = None
        elif character in "'\"":
            quote = character
        elif character in "[{":
            depth += 1
        elif character in "]}":
            depth -= 1
    return depth


def check_pin(path, number, target, rest, report):
    if target.startswith("./"):
        return
    name, _, ref = target.partition("@")
    if not PINNED.fullmatch(ref):
        report.error(path, number,
                     "%s is not pinned to a commit (%r); use the 40-character sha"
                     % (name, ref))
        return
    if not rest.strip().startswith("# v"):
        report.error(path, number,
                     "%s has no '# vX.Y.Z' comment after the pin, so nobody can read "
                     "which release it is" % name)


def check_shape(path, text, report):
    lines = text.splitlines()
    mappings = []          # [(indent, {keys seen at that indent})]
    block_indent = None    # indentation of the key that opened the block scalar
    block_line = 0
    block_key = ""
    jobs_indent = None
    job_indent = None
    job_line = None
    job_keys = {}          # {line of a job id: {keys at that job's key indent}}

    for number, raw in enumerate(lines, 1):
        line = raw.rstrip()
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)

        if stripped.startswith("\t"):
            report.error(path, number, "tab indentation; YAML forbids tabs here")

        if block_indent is not None:
            if not line.strip() or indent > block_indent:
                continue
            block_indent = None
            # A dedent to another key closes the scalar, and so does a comment: the
            # composite action carries one after its `description: >-`. A dedent to
            # anything else means the content sits at the key's own column, which
            # YAML rejects - that is the case this check exists for.
            if KEY.match(line) is None and not stripped.startswith("#"):
                report.error(
                    path, number,
                    "this line ends the block scalar of %r, opened at line %d, and is "
                    "not a key - so that scalar's content is not indented deeper than "
                    "its key" % (block_key, block_line))

        if not line.strip() or stripped.startswith("#"):
            continue

        body = strip_comment(line)
        value = value_of(body)

        if value[:1] in ("'", '"'):
            if len(value) < 2 or not value.endswith(value[0]):
                report.error(path, number,
                             "a value that opens with %s does not close it on the "
                             "same line: %r" % (value[0], value))
        elif value[:1] in ("[", "{"):
            if flow_balance(value):
                report.error(
                    path, number,
                    "a flow collection that opens here does not close on the same "
                    "line (%r); this checker does not follow a flow across lines, "
                    "actionlint does" % value)
        elif value.count('"') % 2:
            # Double quotes must pair inside a plain scalar too - `run: echo "a`
            # opens a quoted segment that never closes. Single quotes are left
            # alone on purpose: an apostrophe in `Don't break` is a plain scalar.
            report.error(path, number,
                         "an odd number of double quotes in a plain scalar: %r"
                         % value)

        if BLOCK.match(line):
            # The column of the *key*, not of the dash: `- run: |` at indent 6 needs
            # its content deeper than column 8, and comparing against the dash let a
            # body at the key's own column through.
            opened = KEY.match(line)
            block_indent = opened.start("key") if opened is not None else indent
            block_line = number
            block_key = opened.group("key") if opened is not None else ""
            continue

        key = KEY.match(line)
        if key is None:
            continue
        name = key.group("key")

        # A `- ` item starts a new mapping at its own level, so two sibling steps
        # both name themselves `name:` without that being a duplicate. Without
        # this the checker reported 27 collisions in four clean workflows.
        item = indent if ITEM.match(line) else None
        key_indent = indent + 2 if item is not None else indent
        while mappings and mappings[-1][0] > key_indent:
            mappings.pop()
        if item is not None and mappings and mappings[-1][0] == key_indent:
            mappings.pop()
        if not mappings or mappings[-1][0] < key_indent:
            mappings.append((key_indent, set()))
        elif name in mappings[-1][1]:
            report.error(path, number, "key %r appears twice in one mapping" % name)
        mappings[-1][1].add(name)

        # The jobs are found by the indentation they use, not by a hard-coded 2,
        # so a workflow that indents its job ids differently is still read.
        if name == "jobs" and indent == 0:
            jobs_indent = indent
        elif jobs_indent is not None and indent > jobs_indent:
            if job_indent is None:
                job_indent = indent
            if indent == job_indent:
                job_line = number
                job_keys[job_line] = set()
            elif indent == job_indent + 2 and job_line is not None:
                job_keys[job_line].add(name)

    for start, names in job_keys.items():
        if "runs-on" not in names and "uses" not in names:
            report.error(path, start,
                         "this job has neither runs-on nor uses, so it cannot start")
        elif "runs-on" in names and "uses" in names:
            report.error(path, start,
                         "this job has both runs-on and a job-level uses:, so it is a "
                         "run and a reusable-workflow call at once")

    for number, raw in enumerate(lines, 1):
        match = USES.match(raw)
        if match is not None:
            check_pin(path, number, match.group("target"), match.group("rest"), report)


def files(root):
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    actions = sorted((root / ".github" / "actions").glob("*/action.yml"))
    return workflows, actions


def check(root, report):
    workflows, actions = files(root)
    if not workflows:
        report.error(".github/workflows", 1, "no workflow files found")
    for path in workflows + actions:
        relative = path.relative_to(root).as_posix()
        text = path.read_bytes().decode("utf-8")
        check_shape(relative, text, report)
        print("%-46s %s" % (relative, "checked"))
    return workflows, actions


def selftest():
    """One case per check, a clean file that must pass, and an empty root."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / ".github" / "workflows").mkdir(parents=True)
        (root / ".github" / "actions" / "build").mkdir(parents=True)
        # The last two lines are the false positives the old per-line character
        # counts produced: an apostrophe in a plain scalar and a `]` inside a
        # quoted string, both valid YAML. A clean fixture that cannot exercise
        # them is how they survived.
        good = """name: good

on:
  push:
    branches: [main]

jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262  # v4.4.0
        with:
          fetch-depth: 0
      - name: Don't break
        run: |
          echo "a ' quote and a [ bracket in a block scalar are fine"
          echo "balanced ] inside quotes"
"""
        (root / ".github" / "workflows" / "good.yml").write_text(good, encoding="utf-8")
        cases = {
            "tag.yml": (
                "name: tag\non: push\njobs:\n  a:\n    runs-on: x\n    steps:\n"
                "      - uses: actions/checkout@v4  # v4.4.0\n",
                "not pinned to a commit"),
            "nocomment.yml": (
                "name: p\non: push\njobs:\n  a:\n    runs-on: x\n    steps:\n"
                "      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262\n",
                "no '# vX.Y.Z' comment"),
            "tab.yml": (
                "name: tab\non: push\njobs:\n  a:\n\truns-on: x\n",
                "tab indentation"),
            "dup.yml": (
                "name: dup\non: push\njobs:\n  a:\n    runs-on: x\n    runs-on: y\n",
                "appears twice"),
            "norun.yml": (
                "name: norun\non: push\njobs:\n  a:\n    steps:\n"
                "      - uses: ./local\n",
                "neither runs-on nor uses"),
            "both.yml": (
                "name: both\non: push\njobs:\n  a:\n    runs-on: x\n"
                "    uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262  # v4.4.0\n",
                "both runs-on and a job-level uses:"),
            "quote.yml": ("name: quote\non: push\njobs:\n  a:\n    runs-on: \"x\n",
                          "does not close it on the same line"),
            "oddquote.yml": (
                "name: odd\non: push\njobs:\n  a:\n    runs-on: x\n    steps:\n"
                "      - run: echo \"a\n",
                "odd number of double quotes"),
            "flow.yml": ("name: flow\non:\n  push:\n    branches: [main\n",
                         "does not close on the same line"),
            "block.yml": (
                "name: block\non: push\njobs:\n  a:\n    runs-on: x\n    steps:\n"
                "      - run: |\n        echo hi\n",
                "not indented deeper than its key"),
        }
        for name, (text, _) in cases.items():
            (root / ".github" / "workflows" / name).write_text(text, encoding="utf-8")

        report = Report()
        check(root, report)
        failures = []
        for case, (_, wanted) in cases.items():
            hits = [problem for problem in report.problems if case in problem]
            if not any(wanted in hit for hit in hits):
                failures.append("%s: expected %r, got %r" % (case, wanted, hits))
        good_hits = [problem for problem in report.problems if "good.yml" in problem]
        if good_hits:
            failures.append("good.yml: a clean file was rejected: %r" % good_hits)

        # No workflows at all has to be a problem, not a silent pass - an empty
        # checkout is how a gate that checks nothing looks green.
        empty = root / "empty"
        (empty / ".github" / "workflows").mkdir(parents=True)
        empty_report = Report()
        check(empty, empty_report)
        if not any("no workflow files found" in problem
                   for problem in empty_report.problems):
            failures.append("empty root: expected 'no workflow files found', got %r"
                            % empty_report.problems)

        for failure in failures:
            print("selftest: " + failure, file=sys.stderr)
        if failures:
            return 1
        print("selftest: %d faults reported, the clean file accepted, "
              "the empty root named" % len(cases))
        return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", nargs="?", default=".", help="repository root")
    parser.add_argument("--selftest", action="store_true",
                        help="check the checker against canned broken workflows")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = Path(args.root).resolve()
    report = Report()
    workflows, actions = check(root, report)
    print()
    for problem in report.problems:
        print(problem, file=sys.stderr)
    if report.problems:
        print("check-workflows: %d problem(s) in %d workflow(s) and %d action(s)"
              % (len(report.problems), len(workflows), len(actions)), file=sys.stderr)
        return 1
    print("check-workflows: %d workflow(s) and %d action(s), every remote action pinned"
          % (len(workflows), len(actions)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
