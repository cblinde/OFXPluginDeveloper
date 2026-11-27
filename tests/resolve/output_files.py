# SPDX-License-Identifier: BSD-3-Clause
"""Which file did the render write? Decided without a render.

A `Complete` job is a claim about the job, not about the file: a render that is
still flushing looks exactly like one that finished, and a file left by the
previous run in the same directory looks exactly like a fresh one. So the
Resolve path snapshots the directory before the job starts and treats a file as
this run's only if it appeared afterwards **or changed**: a render that overwrites
its own name wrote that file, and refusing it made every second run into the same
directory fail - which the kill test found the hard way, because a killed run
leaves its partial frame behind.

That rule lived inside `resolve_api.render_one_frame`, where it could only run on
a machine with Resolve. It is plain filesystem logic, so it lives here instead and
`test_stale_file.py` proves the half that matters: a second run that writes
nothing fails instead of grading the first run's file.
"""

from pathlib import Path


def snapshot(directory, prefix):
    """What the matching files look like right now; call it *before* a job.

    Names alone are not enough: a second render into the same directory writes
    the same name, so the snapshot records what would change if it did.
    """
    return {path.name: (path.stat().st_mtime_ns, path.stat().st_size)
            for path in Path(directory).glob(f"{prefix}*") if path.is_file()}


def newest_new_file(directory, prefix, before):
    """The newest file this run produced, or RuntimeError when there is none.

    "Produced" means new since the snapshot or changed since it. A file that is
    untouched is the previous run's, however recent it looks.
    """
    fresh = []
    for path in Path(directory).glob(f"{prefix}*"):
        if not path.is_file():
            continue
        stat = path.stat()
        if before.get(path.name) != (stat.st_mtime_ns, stat.st_size):
            fresh.append(path)
    if not fresh:
        raise RuntimeError(f"job Complete but it wrote nothing new in {directory}")
    newest = max(fresh, key=lambda path: path.stat().st_mtime_ns)
    if newest.stat().st_size == 0:
        raise RuntimeError(f"job Complete but {newest.name} is empty")
    return newest
