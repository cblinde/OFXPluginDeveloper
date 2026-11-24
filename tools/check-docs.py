#!/usr/bin/env python3
"""Check this repository's documentation for drift.

Standard library only. Run from anywhere:

    python3 tools/check-docs.py            # the tree above this script
    python3 tools/check-docs.py --root .   # an explicit root

Exit status: 0 when clean, 1 when any check fails. Every failure prints
`path:line: message`, so an editor jump or `grep` finds it.

What it checks, and why each one exists:

1. Links and anchors. A `[x](file.md#anchor)` whose file or heading is gone is
   indistinguishable from prose to a reader and to an agent.
2. Cross-references of the form `` `FILE.md` §12 `` (and `§3.5`, checked
   against a heading that starts with that number). These are not links, so no
   renderer catches them; they rot silently when sections are renumbered.
3. Count drift. The number of templates, testing layers and AI repositories is
   derived from the tree at run time and compared with the prose that counts
   them. This is what caught "both templates" after a third template landed.
4. Duplicated measurements. A figure that exists in more than one document must
   carry a provenance mark (`[V]` or `[M]`) on the line, so a change to the
   measurement cannot leave an unmarked copy behind.
5. Provenance labels. Documents use one vocabulary (`[V]/[I]/[B]` for the build
   and correctness docs, `[S]/[M]/[D]/[R]/[A]/[U]` for the performance synthesis)
   and must define every label they use.
6. `path:123` anchors. A backticked file:line is a promise to an agent that the
   symbol is there: inside this tree the file must exist and must still be that
   long. A bare header name or a path whose first segment is not in this
   repository points into an external checkout and is skipped.
7. AGENTS.md shape: present, and a router rather than an essay.
8. Heading skeleton. tools/heading-manifest.tsv records every heading the 14
   pre-restructure documents carried and the file that holds it now. A heading
   that stops existing is a failed check, so the one failure mode a
   reorganisation cannot hide - a whole section quietly lost - becomes visible
   as a line number. Renumbering is ignored on purpose: it was the point. Two
   rows that claim the same heading in the same file are an error rather than
   tolerance, because a dedupe that keeps one of them removes the other from
   coverage while leaving a believable row count.
"""

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration: everything here is derived from the tree or is a fixed
# vocabulary, never a copy of a sentence that lives in the docs.
# ---------------------------------------------------------------------------

SKIP_DIRS = {".git", ".ofx-ref", "build", "__pycache__", "node_modules"}

# `path/file.cpp:123` inside backticks: an anchor into a real file.
ANCHOR_RE = re.compile(
    r"`([A-Za-z0-9_./-]+\.(?:h|hpp|c|cc|cpp|sh|py|txt|map|cmake|json|yml)):(\d+)`")

# Provenance vocabularies. A document may use one of these families and must
# define every tag it uses. `[B]` appears in two families with the same meaning
# (external evidence), which is why it is not treated as a conflict.
FAMILIES = {
    "VIB": {
        "V": "verified in this repository",
        "I": "inferred from a header or documented behaviour, not demonstrated",
        "B": "external evidence (grade B)",
    },
    "SRDMAU": {
        "S": "verified by reading the OFX source",
        "M": "measured here with the real toolchain",
        "D": "stated by official documentation",
        "R": "stated by a reference project",
        "A": "arithmetic derived from a formula in the document",
        "U": "not verified",
    },
}
# `[B]` is shared, so a document using SRDMAU may also contain [B].
SHARED = {"B"}

# Figures that are measurements of this repository. If one appears in more than
# one document, every copy outside the source document must be marked.
MEASURED = [
    (re.compile(r"\b3,240\b"), "3,240", "grid launches"),
    (re.compile(r"\b1,464\b"), "1,464", "components compared"),
    (re.compile(r"\b98 checks\b"), "98", "ReferenceGain contract checks"),
    (re.compile(r"\b124 checks\b"), "124", "MinimalCudaGain contract checks"),
    (re.compile(r"\b116 checks\b"), "116", "OnnxInference contract checks"),
    (re.compile(r"\b647\b"), "647", "default-visibility exports"),
    (re.compile(r"\b663\b"), "663", "CUDA template default-visibility exports"),
]
# Documents that own a measurement and may state it unmarked. Both the flat and
# the references/ layout are listed so the check holds during the restructure
# that introduced references/.
MEASUREMENT_SOURCES = {"TESTING_STRATEGY.md", "BUILD_PLAYBOOK.md",
                       "references/testing-layers.md", "references/cmake-and-linking.md"}
PROVENANCE_MARKS = ("[V]", "[M]")

NUMBER_WORDS = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
}


def num(word):
    return int(word) if word.isdigit() else NUMBER_WORDS[word.lower()]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def markdown_files(root):
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS and not d.startswith("."))
        for name in sorted(filenames):
            if name.endswith(".md"):
                # Forward slashes on every platform: the paths are used as
                # dictionary keys and compared with literals like
                # 'references/testing-layers.md', and os.sep is '\\' on Windows.
                found.append(os.path.relpath(os.path.join(dirpath, name), root)
                             .replace(os.sep, "/"))
    return found


def headings(text):
    """heading text -> slug, using the GitHub algorithm this repo's links use."""
    out = {}
    for line in text.splitlines():
        match = re.match(r"^(#{1,6})\s+(.*)$", line)
        if not match:
            continue
        title = match.group(2).strip()
        slug = re.sub(r"[^\w\s-]", "", title.lower())
        # GitHub replaces each space, it does not collapse runs: "AI / ONNX"
        # loses the slash and keeps both spaces, so its anchor has two hyphens.
        slug = re.sub(r"\s", "-", slug)
        out[slug] = title
    return out


def section_numbers(text):
    return {int(m.group(1)) for m in re.finditer(r"^## (\d+)\.", text, re.M)}


def mask_code(line):
    return re.sub(r"`[^`]*`", "CODE", line)


def strip_comments(text):
    """Drop HTML comments before scanning prose.

    Slices carry `<!-- SKILL.md lines 599-647 -->` provenance comments. Those
    numbers are addresses in the file the text came from, not measurements, and
    the duplicated-measurement check read 647 as an export count.
    """
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


class Report:
    def __init__(self):
        self.problems = []

    def error(self, path, line, message):
        self.problems.append((path, line, message))

    def show(self):
        for path, line, message in self.problems:
            print(f"{path}:{line}: {message}")
        return 1 if self.problems else 0


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_links(root, files, texts, report):
    for path in files:
        text = texts[path]
        own_headings = headings(text)
        for match in re.finditer(r"\[[^\]]*\]\(([^)]+)\)", text):
            target = match.group(1)
            line = text.count("\n", 0, match.start()) + 1
            if target.startswith(("http://", "https://", "mailto:", "#")):
                if target.startswith("#") and target[1:] not in own_headings:
                    report.error(path, line, f"anchor #{target[1:]} does not exist in this file")
                continue
            target = target.split(" ", 1)[0]
            file_part, _, anchor = target.partition("#")
            resolved = os.path.normpath(os.path.join(root, os.path.dirname(path), file_part))
            if not os.path.exists(resolved):
                report.error(path, line, f"link target '{file_part}' does not exist")
                continue
            if anchor and resolved.endswith(".md"):
                relative = os.path.relpath(resolved, root).replace(os.sep, "/")
                if relative in texts and anchor not in headings(texts[relative]):
                    report.error(path, line, f"anchor #{anchor} does not exist in {relative}")


def check_link_numbers(root, files, texts, report):
    """`[§4](other.md#6-...)` - the number shown must be the number landed on.

    Renumbering moved a heading and its anchor together, but a link's label is
    hand-written text beside the link. `[§4](x.md#6-external-...)` reads as a
    promise about §4 and delivers §6, and no renderer objects.
    """
    for path in files:
        text = texts[path]
        for match in re.finditer(r"\[(?:§\s?)?(\d+(?:\.\d+)*)\.?[^\]]*\]\(([^)\s]+)\)", text):
            label, target = match.group(1), match.group(2)
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            file_part, _, anchor = target.partition("#")
            if not anchor:
                continue
            if file_part:
                resolved = os.path.normpath(os.path.join(root, os.path.dirname(path), file_part))
                if not resolved.endswith(".md") or not os.path.exists(resolved):
                    continue
                relative = os.path.relpath(resolved, root).replace(os.sep, "/")
            else:
                relative = path
            title = headings(texts[relative]).get(anchor) if relative in texts else None
            if title is None:
                continue                  # an anchor that does not exist: check_links
            number = re.match(r"\s*(\d+(?:\.\d+)*)", title)
            if number and number.group(1) != label:
                line = text.count("\n", 0, match.start()) + 1
                report.error(path, line,
                             f"link is labelled §{label} but #{anchor} is §{number.group(1)}")


def check_absence_claims(root, files, texts, report):
    """A document that says a file is absent has to be right about it.

    README described this repository as having "no top-level `LICENSE` file"
    while `LICENSE` sat in the root and two other paragraphs linked to it. Every
    link and anchor check passed, because there was nothing broken to find: the
    reference was not to the file, but to its supposed absence. The claim is
    cheap to verify - it names a path - so verify it.
    """
    claims = [re.compile(r"no top-level `([^`]+)`", re.I),
              re.compile(r"no `([^`]+)` file (?:at|in) the (?:repository )?root", re.I)]
    for path in files:
        for pattern in claims:
            for match in pattern.finditer(texts[path]):
                claimed = match.group(1).strip()
                if os.path.exists(os.path.join(root, claimed)):
                    line = texts[path][:match.start()].count("\n") + 1
                    report.error(path, line,
                                 f"claims there is no `{claimed}`, but it exists")


def check_section_numbering(root, files, texts, report):
    """A numbered document's sections run 1..N in reading order.

    The numbers are a section's declared position. The split into references/
    once left files reading 7, 11, 2, 4, 5, 10, 9, 12 because each section kept
    the number it had in the root document it came from: not an ordering nobody
    needed, but a contradiction a reader meets on the second screen. It also
    makes every § reference ambiguous - `§5` may be the fifth section or a
    section that no longer exists - so the two checks belong together.

    A section with exactly one numbered child is the third version of the same
    problem: §12 of reference-projects.md carried 12.1 while its heading also
    held the practices table, so the file read as half restructured. Two or
    more children are a structure; one child is a numbering decision nobody
    finished, and either the other parts get numbers or this one loses its.
    """
    for path in files:
        numbers = [int(m.group(1)) for m in re.finditer(r"^## (\d+)\.", texts[path], re.M)]
        if not numbers:
            continue                      # an unnumbered router or readme
        if numbers != list(range(1, len(numbers) + 1)):
            first_wrong = next(i for i, n in enumerate(numbers) if n != i + 1)
            line = texts[path].splitlines().index(
                next(l for l in texts[path].splitlines()
                     if re.match(rf"^## {numbers[first_wrong]}\.", l))) + 1
            report.error(path, line,
                         f"sections run {numbers}, not 1..{len(numbers)} in order; "
                         f"number them by position so § references mean one thing")
        # The same discipline one level down: 5.1 under §2 means the heading
        # kept a number from the section it was cut out of, and a reader who
        # follows §2.1 lands nowhere.
        current, subsections, fenced = None, [], False

        def close_section():
            if current is None or len(subsections) != 1:
                return
            line_no, dotted = subsections[0]
            report.error(path, line_no,
                         f"{dotted} is the only numbered subsection of §{current}; "
                         f"a lone child reads as a half-finished restructure - "
                         f"number the section's other parts or drop this number")

        for line_no, line in enumerate(texts[path].splitlines(), 1):
            if line.strip().startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            if line.startswith("## "):
                close_section()
                section = re.match(r"^## (\d+)\.", line)
                current = int(section.group(1)) if section else None
                subsections = []
                continue
            subsection = re.match(r"^### (\d+)\.(\d+)\b", line)
            if subsection and current is not None:
                if int(subsection.group(1)) != current:
                    report.error(path, line_no,
                                 f"subsection {subsection.group(1)}.{subsection.group(2)} "
                                 f"sits in §{current}; number it {current}.N")
                else:
                    subsections.append((line_no, f"{current}.{subsection.group(2)}"))
        close_section()


def check_section_refs(root, files, texts, report):
    """`FILE.md` §12 / §3.5 - a cross-reference no renderer validates."""
    for path in files:
        text = texts[path]
        # Both spellings: `FILE.md` §5 and [`FILE.md`](FILE.md) §5. The link
        # form put `)` between the name and the §, so an optional-backtick
        # pattern skipped every reference written that way - the whole class
        # went unchecked.
        for match in re.finditer(
                r"([A-Za-z0-9_./-]+\.md)[`'\")\\]]*[ \t]*§[ \t]*(\d+(?:\.\d+)*)", text):
            target, dotted = match.group(1), match.group(2)
            parts = dotted.split(".")
            number = int(parts[0])
            line = text.count("\n", 0, match.start()) + 1
            resolved = os.path.normpath(os.path.join(root, os.path.dirname(path), target))
            if not os.path.exists(resolved):
                report.error(path, line, f"{target} does not exist (referenced as §{dotted})")
                continue
            relative = os.path.relpath(resolved, root).replace(os.sep, "/")
            if relative not in texts:
                continue
            if number not in section_numbers(texts[relative]):
                have = sorted(section_numbers(texts[relative]))
                report.error(path, line, f"{target} has no §{number} (sections: {have})")
                continue
            if len(parts) > 1:
                # A subsection is a heading whose text starts with N.M; the
                # parent section exists, so §3.5 could otherwise point at a
                # heading that was renumbered to 3.6.
                prefix = re.escape(".".join(parts))
                if not any(re.match(rf"^\s*{prefix}(?![\d.])", title)
                           for title in headings(texts[relative]).values()):
                    report.error(path, line, f"{target} has no §{dotted} heading")


def check_counts(root, files, texts, report):
    template_count = len(
        [d for d in os.listdir(os.path.join(root, "templates"))
         if os.path.exists(os.path.join(root, "templates", d, "CMakeLists.txt"))]
    )
    # Either layout: the L-sections live in tests/testing-layers.md once the
    # references/ split happens, and in TESTING_STRATEGY.md before it.
    layers_doc = (texts.get("references/testing-layers.md")
                  or texts.get("TESTING_STRATEGY.md", ""))
    layer_count = len(re.findall(r"^## \d+\. L\d+:", layers_doc, re.M))
    ai_doc = (texts.get("references/ai-architectures-and-runtimes.md")
              or texts.get("AI_MODEL_INTEGRATION.md", ""))
    ai_section = re.search(r"^## 1\..*?^## 2\.", ai_doc, re.M | re.S)
    ai_rows = len(re.findall(r"^\| \[", ai_section.group(0), re.M)) if ai_section else 0

    for path in files:
        for line_number, line in enumerate(texts[path].splitlines(), 1):
            plain = mask_code(line)

            if re.search(r"\b(both|two) templates\b", plain, re.I) and template_count != 2:
                report.error(path, line_number,
                             f"says 'both templates' but there are {template_count} templates "
                             f"(templates/*/CMakeLists.txt)")

            for match in re.finditer(r"\b([A-Za-z]+|\d+) layers?\b", plain):
                try:
                    stated = num(match.group(1))
                except KeyError:
                    continue
                # "two layers" is a subset ("the two layers that..."); only a
                # count of five or more can be a claim about the whole set.
                if stated >= 5 and stated != layer_count:
                    report.error(path, line_number,
                                 f"says {match.group(1)} layers but references/testing-layers.md has "
                                 f"{layer_count} L-sections (L0-L{layer_count - 1})")

            for match in re.finditer(
                    r"\b([A-Za-z]+|\d+) shipped (?:AI )?(projects|repositories)\b", plain, re.I):
                try:
                    stated = num(match.group(1))
                except KeyError:
                    continue
                if stated != ai_rows:
                    report.error(path, line_number,
                                 f"says {match.group(1)} shipped projects but "
                                 f"references/ai-architectures-and-runtimes.md §1 lists {ai_rows} repositories")


def check_measurements(root, files, texts, report):
    for pattern, label, what in MEASURED:
        owners = [p for p in files if pattern.search(strip_comments(texts[p]))]
        if len(owners) < 2:
            continue
        for path in owners:
            if path in MEASUREMENT_SOURCES:
                continue
            for line_number, line in enumerate(strip_comments(texts[path]).splitlines(), 1):
                if pattern.search(line) and not any(mark in line for mark in PROVENANCE_MARKS):
                    report.error(path, line_number,
                                 f"duplicated measurement '{label}' ({what}) without a "
                                 f"provenance mark; link to references/testing-layers.md §13 or mark "
                                 f"the line [V]")


def check_labels(root, files, texts, report):
    tag_re = re.compile(r"\[([A-Z])\]")
    for path in files:
        text = texts[path]
        used = set(tag_re.findall(text))
        if not used:
            continue
        families = []
        for name, tags in FAMILIES.items():
            if used & set(tags) - SHARED:
                families.append(name)
        # A document assembled from several sources (references/*.md) may carry
        # more than one vocabulary; the invariant that matters is that every tag
        # used is defined in the same document, which is checked below. A file
        # that mixes families and defines them all is honest; one that smuggles a
        # tag in without a legend is not.
        family = families[0] if families else None
        if len(families) > 1:
            family = None
        if family is None:
            allowed = set().union(*(set(tags) for tags in FAMILIES.values())) | SHARED
        else:
            allowed = set(FAMILIES[family]) | SHARED
        unknown = sorted(used - allowed)
        if unknown:
            report.error(path, 1, f"unknown provenance label(s) {unknown}; see the legend in "
                                  f"references/performance-model.md")
        for tag in sorted(used):
            if not re.search(rf"\*\*\[{tag}\]\*\*", text):
                line = next((i for i, l in enumerate(text.splitlines(), 1) if f"[{tag}]" in l), 1)
                report.error(path, line, f"uses [{tag}] but never defines it; add "
                                         f"'**[{tag}]** ...' to the legend")


def check_anchors(root, files, texts, report):
    """`path:123` must exist and must still have line 123.

    Anchors into an OFX checkout are validated when the checkout is present
    (`.ofx-ref`) and skipped otherwise, so a fresh clone without one passes.
    """
    for path in files:
        text = texts[path]
        for match in ANCHOR_RE.finditer(text):
            target, number = match.group(1), int(match.group(2))
            line_number = text.count("\n", 0, match.start()) + 1
            resolved = os.path.normpath(os.path.join(root, target))
            if not os.path.exists(resolved):
                relative = os.path.normpath(
                    os.path.join(root, os.path.dirname(path), target))
                if not os.path.exists(relative):
                    # Distinguish a promise about this tree from a pointer into an
                    # external checkout: `Support/Library/...`, `Merge/Merge.cpp`
                    # and a bare `ofxImageEffect.h` all live in the OpenFX or
                    # Resolve trees. A target whose first segment exists in this
                    # repository is a claim about this repository, and a file
                    # that is gone is exactly the drift this check catches.
                    first = target.split("/", 1)[0]
                    internal = "/" in target and first in os.listdir(root)
                    if internal and not target.startswith(".ofx-ref/"):
                        report.error(path, line_number,
                                     f"anchor {target}:{number} names a file that does not exist")
                    continue
                resolved = relative
            total = sum(1 for _ in open(resolved, encoding="utf-8", errors="replace"))
            if number > total:
                report.error(path, line_number,
                             f"anchor {target}:{number} is past the end of the file "
                             f"({total} lines)")


def check_link_shape(root, files, texts, report):
    """A link written `[label(target)` instead of `[label](target)`.

    check_links only looks at what it can parse, so a bracket lost by a rewrite
    is invisible to it: the label simply stops being a link. That happened to
    four links when the documents moved, so scan for the shape directly.
    """
    shape = re.compile(r"\[[^\[\]]*\([^()\[\]]*\.md[^()\[\]]*\)")
    for path in files:
        text = strip_comments(texts[path])
        for match in shape.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            report.error(path, line,
                         f"malformed link {match.group(0)[:60]!r}: missing `]` before `(`")


# The entry points use a bare "§3" for "§3 of SKILL.md". That was safe while
# one long file held every section. After the split into references/ the numbers
# are not contiguous any more, so a bare number that names a section SKILL no
# longer has - the checklists' old §10, verification status's old §12 - reads as
# a live pointer to nothing. Qualified citations ("references/x.md §3.8") are
# checked by check_section_refs; this one covers the short form.
BARE_SECTION_FILES = ("README.md", "AGENTS.md", "SKILL.md")

# The heading skeleton, written once after the split into references/ and
# committed. Regenerate it only on purpose: it is the record of what existed,
# not a description of the tree today.
HEADING_MANIFEST = "tools/heading-manifest.tsv"


def check_bare_sections(root, files, texts, report):
    skill_sections = section_numbers(texts.get("SKILL.md", ""))
    for path in BARE_SECTION_FILES:
        text = texts.get(path)
        if text is None:
            continue
        for line_number, line in enumerate(text.splitlines(), 1):
            for match in re.finditer(r"\u00a7\s?(L?\d+)", line):
                before = line[:match.start()]
                if ".md" in before:
                    continue          # qualified: check_section_refs has it
                number = match.group(1)
                if number.startswith("L") or int(number) not in skill_sections:
                    report.error(path, line_number,
                                 f"bare §{number} names no section of SKILL.md "
                                 f"(it has {sorted(skill_sections)}); cite the file, "
                                 f"e.g. 'references/x.md §{number}'")


def heading_titles(text):
    """Every heading's text, ignoring fenced blocks.

    A fenced block can start a line with '# ' (a shell comment in a transcript)
    and check_links' slug map tolerates that; here it would let a deleted
    heading "match" a line of output.
    """
    titles, fenced = [], False
    for line in text.splitlines():
        if line.strip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = re.match(r"^#{1,6}\s+(.*?)\s*$", line)
        if match:
            titles.append(match.group(1))
    return titles


def heading_key(text):
    """The comparison form: words only, no markup, no section number.

    The restructure renumbered sections deliberately (old §11 became §4), so a
    number cannot be part of a heading's identity; losing the number is fine,
    losing the words is not.
    """
    text = re.sub(r"[`*_]", "", text)
    text = re.sub(r"^\s*(L?\d+(?:\.\d+)*)[.)]?\s+", "", text)
    return re.sub(r"\s+", " ", text).strip().lower()


def check_heading_manifest(root, files, texts, report):
    path = os.path.join(root, HEADING_MANIFEST)
    if not os.path.exists(path):
        report.error(HEADING_MANIFEST, 1,
                     "missing; it is the record of which headings a restructure "
                     "had to preserve")
        return

    rows, declared = [], None
    for number, line in enumerate(read(path).splitlines(), 1):
        if line.startswith("# headings:"):
            try:
                declared = int(line.split(":", 1)[1])
            except ValueError:
                report.error(HEADING_MANIFEST, number, f"unreadable count: {line!r}")
            continue
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 4:
            report.error(HEADING_MANIFEST, number,
                         f"{len(parts)} columns, expected 4 "
                         f"(source, heading, home file, current heading)")
            continue
        rows.append((number, parts))

    if declared is not None and declared != len(rows):
        report.error(HEADING_MANIFEST, 1,
                     f"declares {declared} headings but lists {len(rows)}; a "
                     f"partial write or a truncated row would hide sections")

    present = {}
    for doc in files:
        present[doc] = {heading_key(title) for title in heading_titles(texts[doc])}

    # A row that duplicates another row's claim covers nothing extra: the two
    # normalise to one heading, so a later dedupe keeps whichever it meets first
    # and the count still looks right. That is how PATTERN_INDEX.md's Contents
    # row disappeared after the split. Reject the shape, name both lines, and let
    # the fix be one row that lists both sources.
    seen = {}
    for number, (source, heading, home, current) in rows:
        claim = (home, heading_key(current or heading))
        if claim in seen:
            first_number, first_heading, first_source = seen[claim]
            report.error(HEADING_MANIFEST, number,
                         f"duplicate row: '{heading}' from {source} claims the same "
                         f"heading in {home} as line {first_number} "
                         f"('{first_heading}' from {first_source}); make it one row "
                         f"whose first column names both documents")
            continue
        seen[claim] = (number, heading, source)

    for number, (source, heading, home, current) in rows:
        if home not in present:
            report.error(HEADING_MANIFEST, number,
                         f"'{heading}' from {source} maps to {home}, which is not "
                         f"a document in this tree")
            continue
        wanted = current or heading
        if heading_key(wanted) not in present[home]:
            report.error(HEADING_MANIFEST, number,
                         f"'{heading}' (from the pre-split {source}) is no longer "
                         f"a heading of {home}; if it was renamed, put the new "
                         f"text in the fourth column, otherwise the section was lost")


def check_agents(root, report):
    path = os.path.join(root, "AGENTS.md")
    if not os.path.exists(path):
        report.error("AGENTS.md", 1, "missing; it is the entry point for generic coding agents")
        return
    lines = read(path).splitlines()
    if not 15 <= len(lines) <= 60:
        report.error("AGENTS.md", 1,
                     f"{len(lines)} lines; keep it a router (15-60 lines), not an essay")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = parser.parse_args()
    root = os.path.abspath(args.root)

    files = markdown_files(root)
    texts = {path: read(os.path.join(root, path)) for path in files}

    report = Report()
    check_links(root, files, texts, report)
    check_link_numbers(root, files, texts, report)
    check_absence_claims(root, files, texts, report)
    check_section_numbering(root, files, texts, report)
    check_section_refs(root, files, texts, report)
    check_counts(root, files, texts, report)
    check_measurements(root, files, texts, report)
    check_labels(root, files, texts, report)
    check_anchors(root, files, texts, report)
    check_link_shape(root, files, texts, report)
    check_bare_sections(root, files, texts, report)
    check_heading_manifest(root, files, texts, report)
    check_agents(root, report)

    status = report.show()
    if status == 0:
        print(f"check-docs: {len(files)} documents, 0 problems")
    else:
        print(f"check-docs: {len(report.problems)} problem(s) in {len(files)} documents")
    return status


if __name__ == "__main__":
    sys.exit(main())
