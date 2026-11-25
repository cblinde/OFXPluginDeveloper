"""Which file did the render write? Decided without a render.

A `Complete` job is a claim about the job, not about the file: a render that is
still flushing looks exactly like one that finished, and a file left by the
previous run in the same directory looks exactly like a fresh one. So the
Resolve path snapshots the directory before the job starts and treats only names
that appeared afterwards as candidates.

That rule lived inside `resolve_api.render_one_frame`, where it could only run on
a machine with Resolve. It is plain filesystem logic, so it lives here instead and
`test_stale_file.py` proves the half that matters: a second run that writes
nothing fails instead of grading the first run's file.
"""

from pathlib import Path


def snapshot(directory, prefix):
    """The names matching `prefix*` right now; call it *before* starting a job."""
    return {path.name for path in Path(directory).glob(f"{prefix}*") if path.is_file()}


def new_files(directory, prefix, before):
    """The files that appeared since `before`, matching `prefix*`."""
    return [path for path in Path(directory).glob(f"{prefix}*")
            if path.is_file() and path.name not in before]


def newest_new_file(directory, prefix, before):
    """The newest file this run produced, or RuntimeError when there is none."""
    fresh = new_files(directory, prefix, before)
    if not fresh:
        raise RuntimeError(f"job Complete but it wrote nothing new in {directory}")
    newest = max(fresh, key=lambda path: path.stat().st_mtime)
    if newest.stat().st_size == 0:
        raise RuntimeError(f"job Complete but {newest.name} is empty")
    return newest
