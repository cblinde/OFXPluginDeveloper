"""One frame through Resolve, end to end: the milestone, as a runnable script.

The workflow is the one in `references/resolve-automation.md` section 9: validate
the bundle, install it, launch `-nogui`, connect through the scripting API, open a
prepared project, render one frame, decode it, check the pixels, quit, write a
report.

Three modes, and the difference matters:

* `--plan` runs **everything that needs no host**: it validates the paths,
  inspects the bundle layout, asks which decoder is available and prints the step
  table with `ready` / `not-run`. It writes the same JSON report as a real run, so
  a machine without Resolve still produces a record of what it could and could
  not check.
* `--build-project` performs the real sequence with a project the **host** makes:
  one still image imported, a timeline, the OFX effect applied through
  `AddEffect`, then one PNG frame. This is the mode that needs no `.drp`.
* `--project some.drp` loads a prepared project instead, and needs the plug-in to
  be applied in it already - which needs a hand-made `.drp` nobody has produced.

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

STEPS = ("validate", "install", "launch", "connect", "load", "build", "render", "check", "quit")


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def default_ofx_dir():
    # RUN: this is the directory Resolve 21.2 scans for third-party OFX, confirmed
    # against the bundle paths in its own OFXPluginCacheV2.xml. `--ofx-dir` still
    # wins, because the host may be configured differently.
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

    if args.build_project:
        source = Path(args.source)
        facts["source"] = str(source)
        facts["effect"] = args.effect
        if not source.is_file():
            problems.append(f"no still to build the timeline from at {source}")
        if not args.effect:
            problems.append("--effect is required with --build-project")
    elif args.project:
        project = Path(args.project)
        facts["project"] = str(project)
        if not project.is_file():
            problems.append(f"no project at {project}")
        elif project.suffix.lower() != ".drp":
            problems.append(f"{project} is not a .drp")

    facts["decoders"] = decode.available_decoders()
    if not facts["decoders"]:
        problems.append("no decoder: install OpenImageIO or put ffmpeg and ffprobe on PATH")

    facts["ofx_dir"] = str(Path(args.ofx_dir) if args.ofx_dir else default_ofx_dir())

    if args.reference:
        reference = Path(args.reference)
        facts["reference"] = str(reference)
        if not reference.is_file():
            problems.append(f"no reference at {reference}")
    if args.expect_gain is not None and not args.reference:
        problems.append("--expect-gain needs --reference to scale")

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
    # Flushed, because a redirected run that is interrupted must not lose the
    # steps it already paid for: block buffering did exactly that once.
    print(f"  {name:<9} {status:<8} {detail}".rstrip(), flush=True)
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
        for name in ("install", "launch", "connect", "load", "build", "render", "quit"):
            step(report, name, "not-run", "needs a Resolve host; --plan stops here")
        step(report, "check", "ready", "decoder present; would run after the render")
        report["result"] = "not-run"
        return 0

    process = None
    resolve = None
    installed = None
    manager = None
    project = None
    project_name = None
    render_size = None
    try:
        started = time.monotonic()
        installed = install(args.bundle, facts["ofx_dir"])
        step(report, "install", "pass", str(installed), time.monotonic() - started)

        started = time.monotonic()
        process = resolve_api.launch(facts["resolve"], nogui=True)
        step(report, "launch", "pass", f"pid {process.pid}", time.monotonic() - started)

        started = time.monotonic()
        resolve = resolve_api.connect(timeout=args.timeout, executable=facts["resolve"])
        step(report, "connect", "pass",
             f"{resolve.GetProductName()} {resolve.GetVersionString()}",
             time.monotonic() - started)

        if args.build_project:
            started = time.monotonic()
            project_name = args.timeline_name
            manager, project = resolve_api.new_project(resolve, project_name)
            still = decode.read_image(args.source)        # for its size, not its pixels
            render_size = (still.width, still.height)
            resolve_api.set_timeline_size(project, still.width, still.height)
            effect_id = resolve_api.find_effect(resolve, args.effect)
            timeline, item, effect = resolve_api.build_timeline(
                project, Path(args.source), effect_id, args.timeline_name)
            placed = effect.GetInfo() or {}
            report["effect"] = {"id": effect_id, "applied": placed.get("name"),
                                "on_item": len(item.GetEffects("ofx")),
                                "timeline": timeline.GetName()}
            step(report, "build", "pass",
                 f"{project.GetName()}: {placed.get('name')} on {item.GetName()}",
                 time.monotonic() - started)
        elif args.project:
            started = time.monotonic()
            project = resolve_api.load_project(resolve, args.project)
            step(report, "load", "pass", Path(args.project).name, time.monotonic() - started)

        started = time.monotonic()
        resolve_api.set_png_output(project, args.out, "poc", mark=1, size=render_size)
        rendered = resolve_api.render_one_frame(project, args.out, "poc", args.timeout)
        step(report, "render", "pass", Path(rendered["file"]).name,
             time.monotonic() - started)

        started = time.monotonic()
        image = decode.read_image(rendered["file"])
        found = checks.check_image(image, parse_expect(args.expect))
        stats = {}
        if args.reference:
            reference = decode.read_image(args.reference)
            if args.expect_gain is None:
                problems, stats = checks.compare_to_reference(image, reference)
            else:
                # A non-default gain is the only host-side proof that this
                # plug-in's pixel path ran: at gain 1.0 the host may skip
                # `render()` and the frame is legitimately identical.
                problems, stats = checks.compare_scaled(image, reference,
                                                        args.expect_gain)
            found = found + problems
        report["image"] = {"width": image.width, "height": image.height,
                           "channels": image.channels, "decoder": image.decoder,
                           "source_format": image.source_format, **stats}
        if found:
            step(report, "check", "fail", "; ".join(found), time.monotonic() - started)
            report["result"] = "fail"
            return 1
        detail = f"{image.width}x{image.height} via {image.decoder}"
        if args.expect_gain is not None:
            detail += (f", x{args.expect_gain:g} mae {stats.get('mae')} "
                       f"max {stats.get('max_pixel')}")
        step(report, "check", "pass", detail, time.monotonic() - started)
    except Exception as error:
        step(report, "error", "fail", f"{type(error).__name__}: {error}")
        report["result"] = "fail"
        return 1
    finally:
        started = time.monotonic()
        if project is not None and manager is not None and project_name:
            resolve_api.close_project(manager, project, project_name)
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
    parser.add_argument("--project", help="a prepared .drp to load instead of building one")
    parser.add_argument("--build-project", action="store_true",
                        help="build the project, timeline and effect through the API")
    parser.add_argument("--source", help="the still image --build-project imports")
    parser.add_argument("--effect", help="the OFX menu name --build-project applies")
    parser.add_argument("--timeline-name", default="poc",
                        help="the project and timeline name --build-project creates")
    parser.add_argument("--ofx-dir", help="where the host scans for plug-ins")
    parser.add_argument("--resolve", help="path to Resolve.exe (default: $RESOLVE_EXE)")
    parser.add_argument("--out", default=str(REPORTS / "frames"), help="render target directory")
    parser.add_argument("--reference", help="an image the render must match")
    parser.add_argument("--expect-gain", type=float,
                        help="the reference times this gain, not the reference itself; "
                             "for a bundle built with -DREFERENCE_GAIN_DEFAULT")
    parser.add_argument("--expect", help="WxH the render must have")
    parser.add_argument("--timeout", type=int, default=600, help="startup and render time-out, seconds")
    parser.add_argument("--plan", action="store_true", help="run only what needs no host")
    parser.add_argument("--keep", action="store_true", help="leave the installed bundle in place")
    args = parser.parse_args(argv)
    if not args.resolve:
        args.resolve = os.environ.get("RESOLVE_EXE")

    report = {"started": stamp(), "mode": "plan" if args.plan else "run", "steps": []}
    print(f"resolve proof of concept ({report['mode']})", flush=True)
    result = run(args, report)
    report["result"] = report.get("result", "fail")
    write_report(report, args)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
