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
import threading
import time
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path

import checks
import decode
import resolve_api

HERE = Path(__file__).resolve().parent
REPORTS = HERE / "reports"

STEPS = ("validate", "install", "launch", "connect", "load", "build", "render", "check", "quit")


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def call_with_deadline(function, seconds, what):
    """Run one blocking host call; a host that stops answering is a failure.

    A dead host can leave a socket call waiting on a timeout nobody set, which is
    the difference between a harness that fails and a harness that hangs. The
    call therefore runs in a daemon thread and the wait is bounded here; the
    thread is deliberately not joined on the way out, because the process must be
    able to exit while it is stuck.
    """
    answer = {}

    def target():
        try:
            answer["value"] = function()
        except BaseException as error:      # reported below, not swallowed
            answer["error"] = error

    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    thread.join(seconds)
    if thread.is_alive():
        raise TimeoutError(f"{what} did not answer within {seconds}s")
    if "error" in answer:
        raise answer["error"]
    return answer["value"]


def kill_host(process, report):
    """Kill the host on purpose: the failure path, tested like the success path."""
    if process is None or process.poll() is not None:
        return
    process.kill()
    report["killed"] = {"pid": process.pid, "at": stamp()}
    print(f"  kill      sent     pid {process.pid} with the render in flight", flush=True)


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

    # `run` reads facts["resolve"] unconditionally. Setting it only when --resolve
    # was passed made every other run - the --project mode included - die with a
    # KeyError after the bundle was already installed; RESOLVE_EXE is the
    # documented fallback, and a missing value is a validation problem people can
    # read rather than a key that is not there.
    executable = Path(args.resolve) if args.resolve else (
        Path(os.environ["RESOLVE_EXE"]) if os.environ.get("RESOLVE_EXE") else None)
    if executable is not None:
        facts["resolve"] = str(executable)
        if not executable.is_file():
            problems.append(f"no Resolve executable at {executable}")
    else:
        problems.append("no Resolve executable: pass --resolve or set RESOLVE_EXE")

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


def host_log_path():
    """Resolve's own log, where it states which compute API it picked."""
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    path = (Path(appdata) / "Blackmagic Design/DaVinci Resolve/Support/logs"
            / "davinci_resolve.log")
    return path if path.is_file() else None


def compute_api_lines(since):
    """The host's own statement of the compute API, from lines written since `since`.

    "Compute API set to automatic, defaulting to CUDA" is what an Auto session
    says; an explicit choice logs "Selected compute API: CUDA" without that line.
    The distinction is the point of running two sessions, and no scripting call
    exposes it, so it is read from the log instead of assumed - a report claiming
    "Auto" without this line would be a guess.

    Filtered by the line's own timestamp and not by a file offset: the host
    rotates `davinci_resolve.log` at start-up, so an offset taken before the
    launch can point past the end of the new file, and the first run that used
    one reported `unknown` for a session that had written the line.
    """
    path = host_log_path()
    if path is None:
        return []
    try:
        text = path.read_bytes().decode("utf-8", "replace")
    except OSError:
        return []
    lines = []
    for line in text.splitlines():
        if "Main.GPUConfig" not in line or "ompute API" not in line:
            continue
        stamp = re.search(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d),\d{3}", line)
        if stamp is not None:
            written = datetime.strptime(stamp.group(1), "%Y-%m-%d %H:%M:%S")
            if written < since:
                continue
        message = line.split("|", 3)[-1].strip() if "|" in line else line.strip()
        lines.append(message)
    return lines


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
        # The names and the order are the ones a real run reports, because that is
        # what --plan is for. `gpu` belongs between connect and build: it reads the
        # host's own compute-API line out of the log of the session that just
        # connected, so it needs a running host but no effect yet.
        for name, status, detail in (
                ("install", "not-run", "needs a Resolve host; --plan stops here"),
                ("launch", "not-run", "needs a Resolve host; --plan stops here"),
                ("connect", "not-run", "needs a Resolve host; --plan stops here"),
                ("gpu", "not-run",
                 "needs a running host; reads its compute-API line from the log"),
                ("build", "not-run",
                 "needs a Resolve host; --plan stops here (only with --build-project)"),
                ("load", "not-run", "needs a Resolve host; --plan stops here"),
                ("render", "not-run", "needs a Resolve host; --plan stops here"),
                ("check", "ready", "decoder present; would run after the render"),
                ("quit", "not-run", "needs a Resolve host; --plan stops here")):
            step(report, name, status, detail)
        report["result"] = "not-run"
        return 0

    process = None
    resolve = None
    effect = None
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
        # The declaration file is the harness's own channel: the host passes its
        # environment to the plug-in, and nothing else can. Cleared first, so a
        # line from a previous run cannot answer this one.
        environment = None
        if args.path_log:
            declared_path = Path(args.path_log)
            declared_path.parent.mkdir(parents=True, exist_ok=True)
            if declared_path.exists():
                declared_path.unlink()
            environment = dict(os.environ,
                               MCOFX_PATH_LOG=str(declared_path.resolve()))
        log_mark = datetime.now() - timedelta(seconds=5)
        process = resolve_api.launch(facts["resolve"], nogui=True,
                                     environment=environment)
        step(report, "launch", "pass", f"pid {process.pid}", time.monotonic() - started)

        started = time.monotonic()
        # --project loads its own project, and a host with no last project answers
        # nothing while it waits on the project manager; --build-project creates
        # one and does not care whether one was open.
        resolve = resolve_api.connect(timeout=args.timeout, executable=facts["resolve"],
                                      process=process,
                                      require_project=bool(args.build_project))
        step(report, "connect", "pass",
             f"{resolve.GetProductName()} {resolve.GetVersionString()}",
             time.monotonic() - started)

        started = time.monotonic()
        api_lines = compute_api_lines(log_mark)
        report["gpu"] = {"compute_api_automatic": any("automatic" in line
                                                      for line in api_lines),
                         "lines": api_lines}
        step(report, "gpu", "pass" if api_lines else "unknown",
             "; ".join(api_lines) or "the host logged no compute API line",
             time.monotonic() - started)

        render_size = None
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
            # The size comes from the project, not from a still this run read. When
            # the host will not state it, `size` stays None and the render uses the
            # project's own format - the alternative, an undefined name, is what
            # this branch would have hit the first time it ran at all.
            try:
                timeline = project.GetCurrentTimeline()
                width = int(timeline.GetSetting("timelineFrameWidth"))
                height = int(timeline.GetSetting("timelineFrameHeight"))
                render_size = (width, height) if width and height else None
            except Exception:                       # noqa: BLE001
                render_size = None
            step(report, "load", "pass",
                 f"{Path(args.project).name} {render_size or 'project format'}",
                 time.monotonic() - started)

        started = time.monotonic()
        resolve_api.set_png_output(project, args.out, "poc", mark=1, size=render_size)
        def killer(seconds_in_flight):
            """Called from the render loop, so the job is in flight by definition."""
            if (args.kill_render_after is not None
                    and seconds_in_flight >= args.kill_render_after):
                kill_host(process, report)

        rendered = call_with_deadline(
            lambda: resolve_api.render_one_frame(project, args.out, "poc",
                                                 args.timeout, process=process,
                                                 on_poll=killer),
            args.timeout + 30, "the render")
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
        if args.path_log and Path(args.path_log).is_file():
            declared = [line.strip() for line
                        in Path(args.path_log).read_text(encoding="utf-8").splitlines()
                        if line.strip()]
            report["declared_paths"] = declared
            step(report, "path", "pass" if declared else "unknown",
                 "; ".join(declared) or "the plug-in declared no pixel path")
    except Exception as error:
        step(report, "error", "fail", f"{type(error).__name__}: {error}")
        report["result"] = "fail"
        return 1
    finally:
        started = time.monotonic()
        # Every one of these talks to the host, and the host may be the thing that
        # just died: each call is bounded, so cleanup cannot become the hang.
        if project is not None and manager is not None and project_name:
            try:
                call_with_deadline(
                    lambda: resolve_api.close_project(manager, project, project_name),
                    20, "closing the project")
            except Exception as error:
                report["close_error"] = f"{type(error).__name__}: {error}"
        try:
            call_with_deadline(lambda: resolve_api.quit_resolve(resolve, process), 45,
                               "quitting the host")
            quit_detail = "API quit, then the recorded PID"
        except Exception as error:
            quit_detail = f"{type(error).__name__}: {error}"
            report["quit_error"] = quit_detail
        if installed is not None and not args.keep:
            shutil.rmtree(installed, ignore_errors=True)
        step(report, "quit", "pass", quit_detail,
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
    parser.add_argument("--path-log",
                        help="a file the plug-in declares its pixel path in, through "
                             "$MCOFX_PATH_LOG; Resolve's log does not carry what a "
                             "plug-in logs")
    parser.add_argument("--timeout", type=int, default=600, help="startup and render time-out, seconds")
    parser.add_argument("--kill-render-after", type=float, metavar="SECONDS",
                        help="kill the host once the job has been rendering this "
                             "long, to prove the harness fails instead of hanging")
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
