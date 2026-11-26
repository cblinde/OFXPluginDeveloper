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
* **Shape.** No tab indentation, quotes and flow brackets balanced outside block
  scalars, every block scalar indented deeper than its key, no key repeated in
  one mapping, every job with `runs-on` or `uses`, and a `uses:` that belongs to
  a step.

It is not a YAML parser. An invalid file it accepts is actionlint's job; a valid
file it rejects is a bug here, and `--selftest` feeds it the faults it exists for.
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

USES = re.compile(r"^(?P<indent> *)(?:- +)?uses: *(?P<target>\S+)(?P<rest>.*)$")
KEY = re.compile(r"^(?P<indent> *)(?:- +)?(?P<key>[A-Za-z_][\w.-]*):(?P<rest>.*)$")
BLOCK = re.compile(r"^(?P<indent> *)(?:- +)?[A-Za-z_][\w.-]*: *[|>][+-]?\d* *$")
PINNED = re.compile(r"[0-9a-f]{40}")
COMMENT = re.compile(r"(^|\s)#")


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


def check_pin(path, number, indent, target, rest, report):
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
    mappings = []          # [(indent, {keys})]
    flow_depth = 0
    block_indent = None    # indentation of the key that opened the block scalar
    block_line = 0
    jobs_indent = None
    job_indent = None
    job_has_key = {}
    pending_step = None

    for number, raw in enumerate(lines, 1):
        line = raw.rstrip()
        indent = len(line) - len(line.lstrip(" "))

        if line.lstrip(" ").startswith("\t") or "\t" in line[:indent]:
            report.error(path, number, "tab indentation; YAML forbids tabs here")
        if block_indent is not None:
            if not line.strip() or indent > block_indent:
                continue
            block_indent = None
        if not line.strip():
            continue
        if line.lstrip().startswith("#"):
            continue

        body = strip_comment(line)
        if body.count('"') % 2 or body.count("'") % 2:
            report.error(path, number, "unbalanced quote on the line: %r" % line.strip())
        flow_depth += body.count("[") - body.count("]") + body.count("{") - body.count("}")
        if flow_depth < 0:
            report.error(path, number, "a flow bracket closes before it opens")
            flow_depth = 0

        if BLOCK.match(line):
            block_indent = indent
            block_line = number
            continue

        if flow_depth:
            continue

        key = KEY.match(line)
        if key is None:
            continue
        name = key.group("key")
        if key.group("rest").strip() == "" and job_indent == indent + 2:
            pending_step = number
        # A `- ` item starts a new mapping at its own level, so two sibling steps
        # both name themselves `name:` without that being a duplicate. Without
        # this the checker reported 27 collisions in four clean workflows.
        item = None if not re.match(r"^ *- ", line) else indent
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

        if name in ("on", "jobs", "name", "permissions") and indent == 0:
            if name == "jobs":
                jobs_indent = indent
        elif jobs_indent is not None and indent == 2 and key.group("rest").strip() == "":
            job_indent = indent
            job_has_key[number] = set()
        elif job_indent is not None and indent == 4 and job_has_key:
            job_has_key[max(job_has_key)].add(name)

    for start, names in job_has_key.items():
        if "runs-on" not in names and "uses" not in names:
            report.error(path, start,
                         "this job has neither runs-on nor uses, so it cannot start")

    if flow_depth:
        report.error(path, len(lines), "a flow bracket is still open at the end of the file")

    for number, raw in enumerate(lines, 1):
        match = USES.match(raw)
        if match:
            if pending_step is not None and number < pending_step:
                report.error(path, number, "a uses: before the first step of a job")
            check_pin(path, number, len(match.group("indent")), match.group("target"),
                      match.group("rest"), report)


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
        if "\t" in text.replace("\r\n", "\n") and not any(
                line.startswith(" ") for line in text.splitlines()):
            report.error(relative, 1, "the file is not indented with spaces")
        check_shape(relative, text, report)
        print("%-46s %s" % (relative, "checked"))
    return workflows, actions


def selftest():
    """Feed the faults this script exists for, and require each to be reported."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / ".github" / "workflows").mkdir(parents=True)
        (root / ".github" / "actions" / "build").mkdir(parents=True)
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
      - name: run
        run: |
          echo "a ' quote and a [ bracket in a block scalar are fine"
"""
        (root / ".github" / "workflows" / "good.yml").write_text(good, encoding="utf-8")
        cases = {
            "tag.yml": ("name: tag\non: push\njobs:\n  a:\n    runs-on: x\n    steps:\n"
                        "      - uses: actions/checkout@v4\n"),
            "tab.yml": ("name: tab\non: push\njobs:\n  a:\n\truns-on: x\n"),
            "dup.yml": ("name: dup\non: push\njobs:\n  a:\n    runs-on: x\n    runs-on: y\n"),
            "norun.yml": ("name: norun\non: push\njobs:\n  a:\n    steps:\n"
                          "      - uses: ./local\n"),
            "quote.yml": ("name: quote\non: push\njobs:\n  a:\n    runs-on: \"x\n"),
        }
        for name, text in cases.items():
            (root / ".github" / "workflows" / name).write_text(text, encoding="utf-8")

        report = Report()
        check(root, report)
        expected = {
            "tag.yml": "not pinned to a commit",
            "tab.yml": "tab indentation",
            "dup.yml": "appears twice",
            "norun.yml": "neither runs-on nor uses",
            "quote.yml": "unbalanced quote",
        }
        failures = []
        for case, wanted in expected.items():
            hits = [problem for problem in report.problems if case in problem]
            if not any(wanted in hit for hit in hits):
                failures.append("%s: expected %r, got %r" % (case, wanted, hits))
        good_hits = [problem for problem in report.problems if "good.yml" in problem]
        if good_hits:
            failures.append("good.yml: a clean file was rejected: %r" % good_hits)
        for failure in failures:
            print("selftest: " + failure, file=sys.stderr)
        if failures:
            return 1
        print("selftest: %d faults reported, the clean file accepted" % len(expected))
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
