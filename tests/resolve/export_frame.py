"""Measure `TimelineItem.ExportFrame` on a live host: file and encodedString.

§15.6 of `references/resolve-automation.md` had this call from 21.2's `.pyi` and
marked it `[I]`, because nothing here had run it. This script runs both forms on
the project `poc.py --build-project` builds - once writing a PNG to disk, once
with no file at all - and holds the host's answer against the bytes it produced:

* `success` is true and `error` is empty,
* the answer's `width`/`height` equal the decoded PNG's size, and the render
  job's size, which is the route that already has evidence (§14.7),
* `data` is base64 that decodes to a PNG of that same size.

Pixels are compared as well, but only reported: the export may take a different
conversion path than the render job, and this measurement is about the call's
contract, not about the plug-in.

Usage:

    python tests/resolve/export_frame.py --bundle <bundle> --source <still> \
        --effect "Reference Gain" --resolve <Resolve.exe> [--out <dir>]

It needs what `poc.py --build-project` needs: a Studio seat, the OFX bundle, and
a still image. A report goes to `tests/resolve/reports/export-frame-*.json`.
"""

import argparse
import base64
import json
import os
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

import checks
import decode
import poc
import resolve_api

HERE = Path(__file__).resolve().parent
REPORTS = HERE / "reports"


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def step(report, name, status, detail=""):
    entry = {"name": name, "status": status, "detail": detail}
    report["steps"].append(entry)
    print(f"  {name:<12} {status:<8} {detail}".rstrip(), flush=True)
    return entry


def options(**overrides):
    """The arguments `poc.validate` reads, for a build-project run."""
    base = dict(bundle=None, project=None, build_project=True, source=None,
                effect=None, timeline_name="poc", ofx_dir=None, resolve=None,
                out=None, reference=None, expect_gain=None, expect=None,
                timeout=600, kill_render_after=None, plan=False, keep=False)
    base.update(overrides)
    return SimpleNamespace(**base)


def without_data(answer):
    """The answer as JSON, with the base64 payload replaced by its length."""
    if not isinstance(answer, dict):
        return {"answer": repr(answer)}
    shown = {key: value for key, value in answer.items() if key != "data"}
    if "data" in answer:
        data = answer["data"]
        shown["data"] = (f"<{len(data)} base64 chars>" if isinstance(data, str)
                         else f"<{type(data).__name__}>")
    return shown


def size_of(reported):
    if not isinstance(reported, dict):
        return None
    width, height = reported.get("width"), reported.get("height")
    if width is None or height is None:
        return None
    return int(width), int(height)


def check_answer(name, reported, image, expected_size, problems):
    """The host's answer against the decoded image it produced."""
    if not isinstance(reported, dict):
        problems.append(f"{name}: the host answered {reported!r}, not a dict")
        return
    if reported.get("success") is not True:
        problems.append(f"{name}: success is {reported.get('success')!r}, "
                        f"error {reported.get('error')!r}")
    said = size_of(reported)
    got = (image.width, image.height) if image is not None else None
    if said is None:
        problems.append(f"{name}: no width/height in the answer")
    elif got is None:
        problems.append(f"{name}: the answer says {said[0]}x{said[1]} but the "
                        "bytes did not decode")
    elif said != got:
        problems.append(f"{name}: the answer says {said[0]}x{said[1]}, the PNG "
                        f"is {got[0]}x{got[1]}")
    if got is not None and expected_size is not None and got != expected_size:
        problems.append(f"{name}: the PNG is {got[0]}x{got[1]}, the render job "
                        f"is {expected_size[0]}x{expected_size[1]}")


def compare(name, image, reference, report, problems):
    """Pixels, reported rather than graded; sizes must match to compare bytes."""
    if image is None or reference is None:
        report.setdefault("comparisons", {})[name] = "not compared"
        return
    found, stats = checks.compare_to_reference(image, reference)
    report.setdefault("comparisons", {})[name] = stats or {"problems": found}
    print(f"  {name:<12} {'same' if not found else 'differs'}  "
          f"mae {stats.get('mae')} max {stats.get('max_pixel')}", flush=True)


def run(args, report):
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    problems, facts = poc.validate(options(
        bundle=args.bundle, source=args.source, effect=args.effect,
        ofx_dir=args.ofx_dir, resolve=args.resolve, out=str(out),
        timeout=args.timeout))
    report["facts"] = facts
    if problems:
        step(report, "validate", "fail", "; ".join(problems))
        report["result"] = "fail"
        return 2
    still = decode.read_image(args.source)
    step(report, "validate", "pass",
         f"{len(facts['ofx'])} .ofx, decoders {facts['decoders']}, "
         f"source {still.width}x{still.height}")

    resolve = None
    process = None
    installed = None
    manager = project = None
    name = args.timeline_name
    found = []
    try:
        installed = poc.install(args.bundle, facts["ofx_dir"])
        step(report, "install", "pass", str(installed))

        process = resolve_api.launch(facts["resolve"], nogui=True)
        step(report, "launch", "pass", f"pid {process.pid}")

        started = time.monotonic()
        resolve = resolve_api.connect(timeout=args.timeout,
                                     executable=facts["resolve"], process=process)
        step(report, "connect", "pass",
             f"{resolve.GetProductName()} {resolve.GetVersionString()}")

        manager, project = resolve_api.new_project(resolve, name)
        resolve_api.set_timeline_size(project, still.width, still.height)
        effect_id = resolve_api.find_effect(resolve, args.effect)
        timeline, item, _effect = resolve_api.build_timeline(
            project, Path(args.source), effect_id, name)
        step(report, "build", "pass",
             f"{project.GetName()}: effect {effect_id!r} on "
             f"{item.GetName()} ({len(item.GetEffects('ofx'))} on the item)")

        # The render job first: it is the route with evidence, so its frame is
        # the size every export has to reproduce.
        job_dir = out / "job"
        resolve_api.set_png_output(project, job_dir, "poc", mark=1,
                                   size=(still.width, still.height))
        started = time.monotonic()
        rendered = poc.call_with_deadline(
            lambda: resolve_api.render_one_frame(project, job_dir, "poc",
                                                 args.timeout, process=process),
            args.timeout + 60, "the render job")
        job_image = decode.read_image(rendered["file"])
        report["job"] = {"file": str(rendered["file"]),
                         "size": [job_image.width, job_image.height],
                         "seconds": round(time.monotonic() - started, 3),
                         "status": rendered["status"]}
        step(report, "render", "pass",
             f"{Path(rendered['file']).name} {job_image.width}x{job_image.height}")
        expected = (job_image.width, job_image.height)

        # Form 1: a file on disk.
        target = out / "export-file.png"
        if target.exists():
            target.unlink()             # so "the file exists" is this call's doing
        started = time.monotonic()
        answer = poc.call_with_deadline(
            lambda: item.ExportFrame(0, {"outputType": "file",
                                         "filePath": str(target)}),
            180, "ExportFrame (file)")
        reported = round(time.monotonic() - started, 3)
        report["export_file"] = {"seconds": reported, "answer": without_data(answer),
                                 "type": type(answer).__name__,
                                 "path": str(target), "exists": target.is_file(),
                                 "bytes": target.stat().st_size if target.is_file() else None}
        if not target.is_file():
            step(report, "export-file", "fail",
                 f"no file at {target}; answer {without_data(answer)}")
            report["result"] = "fail"
            return 1
        image = decode.read_image(target)
        report["export_file"]["decoded"] = [image.width, image.height]
        check_answer("ExportFrame(file)", answer, image, expected, found)
        step(report, "export-file", "pass",
             f"{image.width}x{image.height}, answer {size_of(answer)}, "
             f"{reported * 1000:.0f} ms, {report['export_file']['bytes']} bytes")

        # Form 2: no file at all, base64 in the answer.
        started = time.monotonic()
        encoded = poc.call_with_deadline(
            lambda: item.ExportFrame(0, {"outputType": "encodedString",
                                         "encodedImageFormat": "png"}),
            180, "ExportFrame (encodedString)")
        seconds = round(time.monotonic() - started, 3)
        data = encoded.get("data") if isinstance(encoded, dict) else None
        report["export_encoded"] = {"seconds": seconds,
                                    "answer": without_data(encoded),
                                    "type": type(encoded).__name__}
        payload = b""
        if isinstance(data, str) and data:
            try:
                payload = base64.b64decode(data, validate=True)
            except Exception as error:
                found.append(f"ExportFrame(encodedString): data is not base64: "
                             f"{type(error).__name__}: {error}")
        else:
            found.append(f"ExportFrame(encodedString): no data in the answer "
                         f"({without_data(encoded)})")
        encoded_image = None
        if payload:
            target = out / "export-encoded.png"
            target.write_bytes(payload)
            report["export_encoded"].update({"path": str(target), "bytes": len(payload),
                                             "magic": payload[:8].decode("latin-1")})
            try:
                encoded_image = decode.read_image(target)
                report["export_encoded"]["decoded"] = [encoded_image.width,
                                                       encoded_image.height]
            except Exception as error:
                found.append(f"ExportFrame(encodedString): the bytes do not decode: "
                             f"{type(error).__name__}: {error}")
        check_answer("ExportFrame(encodedString)", encoded, encoded_image, expected, found)
        detail = (f"{encoded_image.width}x{encoded_image.height}, "
                  f"answer {size_of(encoded)}, {seconds * 1000:.0f} ms, "
                  f"{len(payload)} bytes of base64") if encoded_image else \
                 f"answer {without_data(encoded)}"
        step(report, "export-str", "pass" if encoded_image else "fail", detail)

        compare("file vs job", image, job_image, report, found)
        compare("encoded vs job", encoded_image, job_image, report, found)
        compare("encoded vs file", encoded_image, image, report, found)
    except Exception as error:
        step(report, "error", "fail", f"{type(error).__name__}: {error}")
        report["result"] = "fail"
        return 1
    finally:
        if project is not None and manager is not None:
            try:
                poc.call_with_deadline(
                    lambda: resolve_api.close_project(manager, project, name),
                    20, "closing the project")
            except Exception as error:
                report["close_error"] = f"{type(error).__name__}: {error}"
        try:
            poc.call_with_deadline(lambda: resolve_api.quit_resolve(resolve, process),
                                   45, "quitting the host")
        except Exception as error:
            report["quit_error"] = f"{type(error).__name__}: {error}"
        if installed is not None and not args.keep:
            shutil.rmtree(installed, ignore_errors=True)

    report["checks"] = found
    if found:
        report["result"] = "fail"
        for problem in found:
            print(f"  FAIL {problem}", flush=True)
        return 1
    report["result"] = "pass"
    print("  the answer and the bytes agree", flush=True)
    return 0


def write_report(report):
    REPORTS.mkdir(parents=True, exist_ok=True)
    path = REPORTS / f"export-frame-{report['started'].replace(':', '-')}.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"report: {path}")
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="measure TimelineItem.ExportFrame in both documented forms")
    parser.add_argument("--bundle", required=True, help="the .ofx.bundle to install")
    parser.add_argument("--source", required=True, help="the still the timeline is built from")
    parser.add_argument("--effect", required=True, help="the OFX effect's menu name")
    parser.add_argument("--timeline-name", default="poc", help="the project and timeline name")
    parser.add_argument("--ofx-dir", help="where the host scans for plug-ins")
    parser.add_argument("--resolve", help="path to Resolve.exe (default: $RESOLVE_EXE)")
    parser.add_argument("--out", default=str(HERE / "reports" / "export-frame"),
                        help="where the frames go")
    parser.add_argument("--timeout", type=int, default=600,
                        help="startup and render time-out, seconds")
    parser.add_argument("--keep", action="store_true",
                        help="leave the installed bundle in place")
    args = parser.parse_args(argv)
    if not args.resolve:
        args.resolve = os.environ.get("RESOLVE_EXE")

    report = {"started": stamp(), "steps": []}
    print("ExportFrame measurement", flush=True)
    result = run(args, report)
    report["result"] = report.get("result", "fail")
    write_report(report)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
