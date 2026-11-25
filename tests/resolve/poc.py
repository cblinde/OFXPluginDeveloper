"""One frame through Resolve, end to end: the milestone, as a runnable scaffold.

The workflow is the one in `references/resolve-automation.md` section 9: validate
the bundle, install it, launch `-nogui`, connect through the scripting API, load a
prepared project, render one frame, decode it, check the pixels, quit, write a
report.

Two modes, and the difference matters:

* `--plan` runs **everything that needs no host**: it validates the paths,
  inspects the bundle layout, asks which decoder is available and prints the step
  table with `ready` / `not-run`. It writes the same JSON report as a real run, so
  a machine without Resolve still produces a record of what it could and could
  not check.
* without `--plan` it performs the real sequence and stops at the first step that
  fails. Every step that needs Resolve stays `NOT RUN` in the report until it has
  actually run somewhere.

A started Resolve is not a pass, and a `Complete` render job is not a pass: the
pass condition is a decoded file whose size and pixels survive `checks.py`.
"""

import argparse
import json
import os
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path

import checks
import decode
import resolve_api

HERE = Path(__file__).resolve().parent
REPORTS = HERE / "reports"

STEPS = ("validate", "install", "launch", "connect", "load", "render", "check", "quit")


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def default_ofx_dir():
    # NOT RUN: which directory Resolve scans for third-party OFX on Windows is a
    # guess here. `--ofx-dir` is the argument that matters; this default exists
    # so `--plan` has something to print.
    return Path("C:/Program Files/Common Files/OFX/Plugins")


def bundle_ofx(bundle):
    """The `.ofx` files inside a bundle."""
    return sorted(Path(bundle).glob("Contents/*/*.ofx"))


def validate(args):
    """Everything that can be wrong without Resolve. Returns (problems, facts)."""
    problems = []
    facts = {}

    bundle = Path(args.bundle)
    if not bundle.exists():
        problems.append(f"no bundle at {bundle}")
    elif not bundle.is_dir():
        problems.append(f"{bundle} is not a directory")
    else:
        found = bundle_ofx(bundle)
        facts["ofx"] = [str(path) for path in found]
        if not found:
            problems.append(f"{bundle} has no Contents/<arch>/*.ofx")

    if args.project:
        project = Path(args.project)
        facts["project"] = str(project)
        if not project.is_file():
            problems.append(f"no project at {project}")
        elif project.suffix.lower() != ".drp":
            problems.append(f"{project} is not a .drp")
    elif not args.plan:
        problems.append("--project is required for a real run")

    facts["decoders"] = decode.available_decoders()
    if not facts["decoders"]:
        problems.append("no decoder: install OpenImageIO or put ffmpeg and ffprobe on PATH")

    facts["ofx_dir"] = str(Path(args.ofx_dir) if args.ofx_dir else default_ofx_dir())

    if args.reference:
        reference = Path(args.reference)
        facts["reference"] = str(reference)
        if not reference.is_file():
            problems.append(f"no reference at {reference}")

    if args.resolve:
        executable = Path(args.resolve)
        facts["resolve"] = str(executable)
        if not executable.is_file():
            problems.append(f"no Resolve executable at {executable}")

    return problems, facts


def install(bundle, ofx_dir):
    """Copy the bundle into the host's OFX directory and return the target."""
    target = Path(ofx_dir) / Path(bundle).name
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(bundle, target)
    return target


def parse_expect(text):
    if not text:
        return {}
    width, _, height = text.partition("x")
    if not width.isdigit() or not height.isdigit():
        raise SystemExit(f"--expect wants WxH, got {text!r}")
    return {"width": int(width), "height": int(height)}


def step(report, name, status, detail="", seconds=None):
    entry = {"name": name, "status": status, "detail": detail}
    if seconds is not None:
        entry["seconds"] = round(seconds, 2)
    report["steps"].append(entry)
    print(f"  {name:<9} {status:<8} {detail}".rstrip())
    return entry


def run(args, report):
    started = time.monotonic()
    problems, facts = validate(args)
    report["facts"] = facts
    if problems:
        step(report, "validate", "fail", "; ".join(problems), time.monotonic() - started)
        report["result"] = "fail"
        return 2
    step(report, "validate", "pass",
         f"{len(facts['ofx'])} .ofx, decoders {facts['decoders']}", time.monotonic() - started)

    if args.plan:
        for name in ("install", "launch", "connect", "load", "render", "quit"):
            step(report, name, "not-run", "needs a Resolve host; --plan stops here")
        step(report, "check", "ready", "decoder present; would run after the render")
        report["result"] = "not-run"
        return 0

    process = None
    resolve = None
    installed = None
    try:
        started = time.monotonic()
        installed = install(args.bundle, facts["ofx_dir"])
        step(report, "install", "pass", str(installed), time.monotonic() - started)

        started = time.monotonic()
        process = resolve_api.launch(facts["resolve"], nogui=True)
        step(report, "launch", "pass", f"pid {process.pid}", time.monotonic() - started)

        started = time.monotonic()
        resolve = resolve_api.connect(timeout=args.timeout)
        step(report, "connect", "pass", "scriptapp answered with a project open",
             time.monotonic() - started)

        started = time.monotonic()
        project = resolve_api.load_project(resolve, args.project)
        step(report, "load", "pass", Path(args.project).name, time.monotonic() - started)

        started = time.monotonic()
        rendered = resolve_api.render_one_frame(project, args.out, "poc", args.timeout)
        step(report, "render", "pass", Path(rendered["file"]).name,
             time.monotonic() - started)

        started = time.monotonic()
        image = decode.read_image(rendered["file"])
        found = checks.check_image(image, parse_expect(args.expect))
        stats = {}
        if args.reference:
            problems, stats = checks.compare_to_reference(
                image, decode.read_image(args.reference))
            found = found + problems
        report["image"] = {"width": image.width, "height": image.height,
                           "channels": image.channels, "decoder": image.decoder,
                           "source_format": image.source_format, **stats}
        if found:
            step(report, "check", "fail", "; ".join(found), time.monotonic() - started)
            report["result"] = "fail"
            return 1
        step(report, "check", "pass", f"{image.width}x{image.height} via {image.decoder}",
             time.monotonic() - started)
    except Exception as error:
        step(report, "error", "fail", f"{type(error).__name__}: {error}")
        report["result"] = "fail"
        return 1
    finally:
        started = time.monotonic()
        resolve_api.quit_resolve(resolve, process)
        if installed is not None and not args.keep:
            shutil.rmtree(installed, ignore_errors=True)
        step(report, "quit", "pass", "API quit, then the recorded PID",
             time.monotonic() - started)

    report["result"] = "pass"
    return 0


def write_report(report, args):
    REPORTS.mkdir(parents=True, exist_ok=True)
    path = REPORTS / f"poc-{report['started'].replace(':', '-')}.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"report: {path}")
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description="one frame through Resolve")
    parser.add_argument("--bundle", required=True, help="the .ofx.bundle to install")
    parser.add_argument("--project", help="the prepared .drp to load")
    parser.add_argument("--ofx-dir", help="where the host scans for plug-ins")
    parser.add_argument("--resolve", help="path to Resolve.exe (default: $RESOLVE_EXE)")
    parser.add_argument("--out", default=str(REPORTS / "frames"), help="render target directory")
    parser.add_argument("--reference", help="an image the render must match")
    parser.add_argument("--expect", help="WxH the render must have")
    parser.add_argument("--timeout", type=int, default=600, help="startup and render time-out, seconds")
    parser.add_argument("--plan", action="store_true", help="run only what needs no host")
    parser.add_argument("--keep", action="store_true", help="leave the installed bundle in place")
    args = parser.parse_args(argv)
    if not args.resolve:
        args.resolve = os.environ.get("RESOLVE_EXE")

    report = {"started": stamp(), "mode": "plan" if args.plan else "run", "steps": []}
    print(f"resolve proof of concept ({report['mode']})")
    result = run(args, report)
    report["result"] = report.get("result", "fail")
    write_report(report, args)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
