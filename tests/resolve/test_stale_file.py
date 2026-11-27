# SPDX-License-Identifier: BSD-3-Clause
"""Two renders in one directory: the second must not grade the first one's file.

This is the check the Resolve POC would otherwise only have on a machine with
Resolve. It stands in for the host with `simulate_render`, which writes a file
the way a render does - nothing else about a render is simulated, and nothing
about the OFX path is claimed here.

    python tests/resolve/test_stale_file.py

Exits 0 when every case behaves, 1 when one does not.
"""

import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

import output_files

PREFIX = "poc"


def simulate_render(directory, name, size=1024, mtime=None):
    """Stand in for the host: write one file, optionally with a chosen mtime."""
    path = Path(directory) / name
    path.write_bytes(b"\x7f" * size)
    if mtime is not None:
        os.utime(path, (mtime, mtime))
    return path


def run_cases():
    results = []

    def case(name, body):
        """`body` returns (ok, detail); an exception is a failed case."""
        try:
            ok, detail = body()
        except Exception as error:
            ok, detail = False, f"raised {type(error).__name__}: {error}"
        results.append((name, ok, detail))

    def first_run_returns_its_own_file():
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            simulate_render(directory, "poc_0001.dpx")     # an earlier run
            simulate_render(directory, "poc_0002.dpx")
            before = output_files.snapshot(directory, PREFIX)
            written = simulate_render(directory, "poc_0003.dpx")
            got = output_files.newest_new_file(directory, PREFIX, before)
            return (got.name == written.name, f"returned {got.name}")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    def second_run_writes_nothing():
        """The claim: same directory, no new file, and the run fails."""
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            simulate_render(directory, "poc_0001.dpx")     # run 1 wrote this
            before = output_files.snapshot(directory, PREFIX)
            # run 2: AddRenderJob said Complete, the file is still the old one.
            try:
                got = output_files.newest_new_file(directory, PREFIX, before)
            except RuntimeError as error:
                return ("wrote nothing new" in str(error),
                        f"refused with: {error}")
            return False, f"returned the stale file {got.name}"
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    def second_run_overwriting_its_own_name_is_accepted():
        """A render into a directory that already holds its name still counts.

        This is the case the kill test exposed: a killed run leaves its partial
        frame, and the next run writes the same name.
        """
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            simulate_render(directory, "poc_0001.dpx", size=1024)
            before = output_files.snapshot(directory, PREFIX)
            written = simulate_render(directory, "poc_0001.dpx", size=2048)
            got = output_files.newest_new_file(directory, PREFIX, before)
            return (got.name == written.name and got.stat().st_size == 2048,
                    f"returned {got.name} with {got.stat().st_size} bytes")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    def fresh_file_wins_over_a_newer_stale_one():
        """Selection is by the snapshot, so a stale mtime cannot win."""
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            now = time.time()
            simulate_render(directory, "poc_0001.dpx", mtime=now + 100)
            before = output_files.snapshot(directory, PREFIX)
            written = simulate_render(directory, "poc_0002.dpx", mtime=now - 100)
            got = output_files.newest_new_file(directory, PREFIX, before)
            return (got.name == written.name,
                    f"returned {got.name}, the stale file is newer on disk")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    def a_fresh_empty_file_fails():
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            before = output_files.snapshot(directory, PREFIX)
            simulate_render(directory, "poc_0001.dpx", size=0)
            try:
                got = output_files.newest_new_file(directory, PREFIX, before)
            except RuntimeError as error:
                return ("is empty" in str(error), f"refused with: {error}")
            return False, f"accepted {got.name} with 0 bytes"
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    def another_prefix_is_not_a_candidate():
        directory = tempfile.mkdtemp(prefix="stale-file-")
        try:
            before = output_files.snapshot(directory, PREFIX)
            simulate_render(directory, "other_0001.dpx")
            try:
                got = output_files.newest_new_file(directory, PREFIX, before)
            except RuntimeError as error:
                return ("wrote nothing new" in str(error), f"refused with: {error}")
            return False, f"accepted {got.name}"
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    case("the first run returns the file it wrote", first_run_returns_its_own_file)
    case("a second run without a new file fails", second_run_writes_nothing)
    case("a second run that overwrites its own name is accepted",
         second_run_overwriting_its_own_name_is_accepted)
    case("a fresh file beats a newer stale one", fresh_file_wins_over_a_newer_stale_one)
    case("a fresh but empty file fails", a_fresh_empty_file_fails)
    case("a file with another prefix is not a candidate", another_prefix_is_not_a_candidate)

    return results


def main():
    results = run_cases()
    failed = 0
    for index, (name, ok, detail) in enumerate(results, 1):
        print(f"case {index}: {name} ... {'ok' if ok else 'FAILED'} ({detail})")
        failed += 0 if ok else 1
    print(f"stale-file test: {failed} case(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
