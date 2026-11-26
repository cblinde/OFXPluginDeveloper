"""The part of the proof of concept that needs a running Resolve.

Every call marked `# RUN` answered DaVinci Resolve Studio 21.2.0.5 on Windows on
2026-11-25; every call marked `# NOT RUN` has still never been executed. The run
behind those markers is `evidence/2026-11-25-windows/resolve-poc.log`.

`load_project` did get its run on 2026-11-26 and answered worse than expected:
`LoadProject` refuses a `.drp` by path - relative and absolute alike - while
`ImportProject` takes the file and leaves a project named `<name>.Cfg` behind
([`resolve-project-route.log`](../evidence/2026-11-26-windows/resolve-project-route.log)).
The import-then-open-by-name sequence is written down there and in
`references/resolve-automation.md` §5, not implemented here yet: it was never run
end to end, and a fallback that has never answered is the thing this file exists
to avoid.

The split is deliberate: `decode.py` and `checks.py` run anywhere, `poc.py`
orchestrates both, and this file is the only place that can be wrong in a way no
test here would catch. See `references/resolve-automation.md` §13 for the long
form of each call and the questions it still has.
"""

import os
import subprocess
import sys
import time
from pathlib import Path

import output_files


class NotRun(RuntimeError):
    """The step needs a host this machine does not have."""


def scripting_api_dir():
    """Where `Modules/DaVinciResolveScript.py` lives.

    RESOLVE_SCRIPT_API wins; the %PROGRAMDATA% path beside it is the fallback,
    because that is where the installer puts the module (RUN: present on the
    machine this ran on).
    """
    configured = os.environ.get("RESOLVE_SCRIPT_API")
    if configured:
        return configured
    documented = Path(os.environ.get("PROGRAMDATA", r"C:\\ProgramData")) / \
        "Blackmagic Design/DaVinci Resolve/Support/Developer/Scripting"
    return str(documented) if documented.is_dir() else None


def library_path(executable=None):
    """The `fusionscript.dll`/`.so` beside the host, when RESOLVE_SCRIPT_LIB is unset.

    The module looks for the library under `C:\\Program Files\\...` on Windows, so
    a Resolve installed on another drive fails with "DLL load failed ... module not
    found" even though the module itself imported (RUN: that is what happened
    here). Deriving the path from the executable removes a variable the caller
    would otherwise have to get right.
    """
    configured = os.environ.get("RESOLVE_SCRIPT_LIB")
    if configured:
        return configured
    for candidate in (executable, os.environ.get("RESOLVE_EXE")):
        if not candidate:
            continue
        beside = Path(candidate).parent / ("fusionscript.dll" if os.name == "nt"
                                          else "fusionscript.so")
        if beside.is_file():
            return str(beside)
    return None


def poll_delay(attempt, ceiling=2.0):
    """Seconds to wait before the next check: 0.1, 0.2, 0.4 ... capped.

    A flat `sleep(2)` costs up to two seconds on a host that was ready 100 ms
    after the last attempt, and these loops run against a GPU-strong machine
    where the host is usually the only slow part. Backing off keeps the first
    seconds responsive without spinning a core for the minutes a cold start
    takes. Measured: a `-nogui` start answered 351 s after launch, so the tail
    has to stay cheap.
    """
    return min(ceiling, 0.1 * (2 ** min(attempt, 6)))


def connect(timeout=120, poll=2.0, executable=None, process=None,
            require_project=True):
    """Return a Resolve object, with a project open unless `require_project` is off.

    `process` is the host this run started, if any. With it, a host that exits
    before answering fails the call in the next hundred milliseconds instead of
    burning the whole timeout - a start that dies after a second used to look
    exactly like one that is still loading. The check runs *after* the attempt,
    because a second `Resolve.exe` exits at once while the first one keeps
    answering: that is an attach, not a failure.

    `require_project=False` is for a caller that loads its own project: a host
    started with `-nogui` and no last project sits on the project manager, where
    `GetCurrentProject()` is empty forever and every probe of the scripting API
    tears the server down instead of attaching.
    """
    api_dir = scripting_api_dir()
    if not api_dir:
        raise NotRun("RESOLVE_SCRIPT_API is not set; see references/resolve-automation.md section 2")
    library = library_path(executable)
    if library:
        os.environ["RESOLVE_SCRIPT_LIB"] = library

    modules = os.path.join(api_dir, "Modules")
    if modules not in sys.path:
        sys.path.insert(0, modules)

    import DaVinciResolveScript as dvr  # RUN: loads fusionscript.dll from RESOLVE_SCRIPT_LIB

    deadline = time.monotonic() + timeout
    last_error = None
    attempt = 0
    while time.monotonic() < deadline:
        try:
            resolve = dvr.scriptapp("Resolve")   # RUN: None until Resolve is up
        except Exception as error:
            last_error = error
            resolve = None
        if resolve:
            manager = resolve.GetProjectManager()          # RUN
            if manager and (not require_project or manager.GetCurrentProject()):
                return resolve
        if process is not None and process.poll() is not None:
            raise RuntimeError(
                f"the host exited (code {process.returncode}) and nothing answered "
                f"the scripting API; last error: {last_error}")
        time.sleep(poll_delay(attempt, poll))
        attempt += 1
    raise TimeoutError(f"no Resolve after {timeout}s (last error: {last_error})")


def launch(executable, nogui=True, environment=None):
    """Start Resolve and return the process; readiness is `connect`'s job.

    `environment` is the host's environment, for the variables a plug-in reads
    from inside the host: a process this harness starts is the only place where
    setting one is possible. None means "inherit", which is the default.
    """
    executable = Path(executable)
    if not executable.exists():
        raise NotRun(f"no Resolve executable at {executable}")
    arguments = [str(executable)]
    if nogui:
        arguments.append("-nogui")   # RUN: 18.2 s to a live scriptapp on Windows
    process = subprocess.Popen(arguments, cwd=str(executable.parent), env=environment)
    return process


def load_project(resolve, project_path):
    manager = resolve.GetProjectManager()                 # RUN
    # Absolute: the host resolves a relative path against its own working
    # directory, not the caller's - the same trap SetRenderSettings has with
    # TargetDir. Measured 2026-11-26: a relative .drp that exists here is refused.
    path = Path(project_path).resolve()
    # RUN 2026-11-26: None, both relative and absolute, for a .drp this host
    # exported itself. ImportProject(path) answered True for the same file and
    # left a project named "<name>.Cfg" to be opened; that second call is the
    # missing half, so this one keeps its honest failure instead of guessing.
    project = manager.LoadProject(str(path))
    if not project:
        raise RuntimeError(
            f"could not load project: {path} (LoadProject refuses a .drp by path "
            f"on 21.2.0.5; ImportProject takes the file, see "
            f"references/resolve-automation.md section 5)")
    return project


def find_effect(resolve, name):
    """The effects-list id for the OFX plug-in whose menu name is `name`.

    `AddEffect` takes the id the host lists, which is not the OFX identifier:
    for this bundle 21.2 lists `Open FX/OFX Plugin Developer/Reference Gain`,
    while the plug-in calls itself `com.ofxplugindeveloper.referencegain`. Taking
    the id from the host is also the proof that the bundle was scanned at all.
    """
    effects = resolve.GetEffectsList() or []              # RUN: 225 entries on 21.2
    matches = [entry for entry in effects
               if entry.get("category") == "ofx" and entry.get("name") == name]
    if not matches:
        raise RuntimeError(f"no OFX effect named {name!r} among {len(effects)} effects; "
                           "is the bundle installed, and did the host scan it at startup?")
    return matches[0]["id"]


def project_manager(resolve, timeout=120):
    """The project manager, waiting for it if the host's UI is not up yet.

    `GetProjectManager()` answers None for a while after a `-nogui` start - with
    a 207 MB bundle installed it was still None at the moment `connect`
    succeeded, and the next call then failed as `'NoneType' object has no
    attribute 'ImportMedia'`, which says nothing about the cause. Wait, then say
    what was missing.
    """
    deadline = time.monotonic() + timeout
    attempt = 0
    while True:
        manager = resolve.GetProjectManager()                 # RUN
        if manager is not None or time.monotonic() >= deadline:
            break
        time.sleep(poll_delay(attempt))
        attempt += 1
    if manager is None:
        raise RuntimeError(f"the host offered no project manager within {timeout}s")
    return manager


def new_project(resolve, name):
    """Create an empty project; returns (manager, project).

    A run that died mid-render leaves its project behind - the kill test does
    exactly that, and `close_project` cannot help because the host it would ask
    is gone. So a name that already exists is cleared here rather than ending the
    next run: `CreateProject` refuses a duplicate, and a harness that can only be
    run once is not a harness.
    """
    manager = project_manager(resolve)
    project = manager.CreateProject(name)                 # RUN
    if not project:
        stale = manager.LoadProject(name)                 # RUN: after a killed run
        if stale:
            manager.CloseProject(stale)
            manager.DeleteProject(name)
            project = manager.CreateProject(name)
    if not project:
        raise RuntimeError(f"CreateProject refused {name!r}, and the project of "
                           "that name could not be removed")
    return manager, project


def build_timeline(project, source, effect_id, timeline_name="poc"):
    """A one-clip timeline with the OFX effect on it, built through the API.

    This is what removes the prepared `.drp` from the milestone: the host makes
    the project, imports one still, appends it to a timeline and applies the
    effect, all before a frame is rendered. `ImportMedia` takes a list of paths
    and answers 0 items for a list of dicts (RUN: both forms tried on 21.2).
    """
    # Absolute, for the same reason as TargetDir: a relative path imports fine
    # and then fails at render time with "Full resolution media not found".
    source = Path(source).resolve()
    pool = project.GetMediaPool()                         # RUN
    imported = pool.ImportMedia([str(source)])            # RUN
    if not imported:
        raise RuntimeError(f"ImportMedia accepted no clip from {source}")
    timeline = pool.CreateTimelineFromClips(              # RUN
        timeline_name, [{"mediaPoolItem": imported[0]}])
    if not timeline:
        raise RuntimeError("CreateTimelineFromClips returned nothing")
    items = timeline.GetItemListInTrack("video", 1)       # RUN
    if not items:
        raise RuntimeError("the new timeline has no video item")
    effect = items[0].AddEffect("ofx", effect_id)         # RUN
    if not effect:
        raise RuntimeError(f"AddEffect refused {effect_id}")
    return timeline, items[0], effect


def set_timeline_size(project, width, height):
    """Match the timeline to the source, so nothing is scaled on the way out.

    A new project carries the host's own default resolution (3840x2160 on the
    machine this ran on), and a 1920x1080 still in it is rendered scaled up
    unless the timeline says otherwise (RUN: the first PNG came out 3840x2160).
    """
    return project.SetSettings({"timelineResolutionWidth": str(width),
                                "timelineResolutionHeight": str(height)})   # RUN


def set_png_output(project, output_directory, custom_name, mark=None, size=None):
    """A single PNG frame, 8 bits per channel, named `custom_name`.

    The codec argument is the internal name `RGB8`, not the menu label
    "RGB 8 bits" (RUN: the label is refused, `("png", "RGB8")` is accepted).
    The file name carries the timeline's own frame number rather than 1: the runs
    here wrote `poc00108000.png` for the only frame rendered.
    """
    # Absolute, because the host resolves TargetDir against its own working
    # directory, not the caller's: a relative path that exists here made
    # SetRenderSettings answer True and AddRenderJob hand back an empty id.
    out = Path(output_directory).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not project.SetCurrentRenderFormatAndCodec("png", "RGB8"):   # RUN
        raise RuntimeError("the host refused the PNG / RGB8 render format")
    settings = {"TargetDir": str(out), "CustomName": custom_name}
    if size:
        settings.update({"FormatWidth": size[0], "FormatHeight": size[1]})
    if mark is None:
        settings["SelectAllFrames"] = True
    else:
        settings.update({"SelectAllFrames": False, "MarkIn": mark, "MarkOut": mark})
    if not project.SetRenderSettings(settings):            # RUN
        raise RuntimeError("SetRenderSettings refused the map")
    return out


def close_project(manager, project, name):
    """Close and delete a project this run created; False if either step failed."""
    closed = manager.CloseProject(project)                # RUN
    deleted = manager.DeleteProject(name)                 # RUN
    return closed and deleted


def _exit_code(process, grace=5.0):
    """The host's exit code if it has exited, else None.

    `poll()` alone is not enough after a kill: the process object only learns the
    code once the OS has finished with it, so a call that fails immediately after
    the kill would otherwise be reported as "stopped answering" instead of
    "exited with code 1" - the difference between a guess and a fact.
    """
    if process is None:
        return None
    if process.poll() is not None:
        return process.returncode
    try:
        return process.wait(timeout=grace)
    except Exception:
        return None


def _host_call(function, process, what):
    """Call one API method; a dead host becomes a sentence, not a TypeError.

    Every call in `render_one_frame` goes through here: when the host has exited,
    the scripting layer fails in whatever way its last socket operation left
    behind (`'NoneType' object is not callable` is the common one), and the exit
    code is the fact a reader needs.
    """
    try:
        return function()
    except Exception as error:
        code = _exit_code(process)
        if code is not None:
            raise RuntimeError(f"the host exited (code {code}) while {what} was "
                               "in flight") from error
        raise


def render_one_frame(project, output_directory, custom_name, timeout, process=None,
                     poll=0.25, on_poll=None):
    """Queue one render, wait, and return the file it says it wrote.

    The render settings are `set_png_output`'s job; this function assumes the
    directory is already configured.

    `process` is the host this run started. With it, a host that dies while the
    job is in flight is reported within `poll` seconds instead of being waited
    for until the deadline - which is what the kill test
    (`--kill-render-after`) exists to prove.

    `on_poll(seconds_in_flight)` is called once per iteration, only while the job
    is still rendering. That is where the kill test pulls the plug: from this
    thread, so the kill is guaranteed to land with the job in flight, and no
    second thread touches the scripting API.
    """
    out = Path(output_directory)
    out.mkdir(parents=True, exist_ok=True)

    # The one part of this function that is tested: `output_files` decides what
    # counts as this run's file, and `test_stale_file.py` proves that a run which
    # writes nothing fails instead of grading the previous run's output.
    before = output_files.snapshot(out, custom_name)

    job_id = _host_call(project.AddRenderJob, process, "queuing the render")   # RUN: "" on failure
    if not job_id:
        raise RuntimeError("AddRenderJob returned no job id")

    if not _host_call(lambda: project.StartRendering([job_id], isInteractiveMode=False),
                      process, "starting the render"):                # RUN
        raise RuntimeError("StartRendering refused the job")

    deadline = time.monotonic() + timeout
    in_flight_since = time.monotonic()
    attempt = 0
    while time.monotonic() < deadline:
        # Liveness first: a killed host makes every later call a question nobody
        # will answer, and a socket that is merely closed can take its own time
        # to say so.
        code = _exit_code(process, grace=0)
        if code is not None:
            raise RuntimeError(f"the host exited (code {code}) while render "
                               f"{job_id} was in flight")
        try:
            rendering = project.IsRenderingInProgress()    # RUN
        except Exception as error:
            code = _exit_code(process)
            if code is not None:
                raise RuntimeError(f"the host exited (code {code}) while render "
                                   f"{job_id} was in flight") from error
            raise RuntimeError(f"the host stopped answering while render {job_id} "
                               f"was in flight: {type(error).__name__}: {error}")
        if not rendering:
            break
        if on_poll is not None:
            on_poll(time.monotonic() - in_flight_since)
        time.sleep(poll_delay(attempt, poll))
        attempt += 1
    else:
        project.StopRendering()                           # NOT RUN: no render timed out here
        raise TimeoutError(f"render {job_id} exceeded {timeout}s")

    status = _host_call(lambda: project.GetRenderJobStatus(job_id),
                        process, "reading the job status")   # RUN: dict OR error string
    if not isinstance(status, dict):
        raise RuntimeError(f"GetRenderJobStatus returned a string: {status}")
    if status.get("JobStatus") != "Complete":
        raise RuntimeError(f"render failed: {status.get('JobStatus')} ({status.get('Error')})")

    newest = output_files.newest_new_file(out, custom_name, before)

    return {"job": job_id, "file": str(newest), "status": status}


def effect_methods(effect):
    """What a host's OFX effect object exposes, measured rather than assumed.

    Resolve 21.2.0.5 answered `['GetFusionComp', 'GetIndex', 'GetInfo',
    'GetIsEnabled', 'SetIndex', 'SetIsEnabled']` on 2026-11-26 - no `GetParam`,
    no `SetParam`. A run inside that host therefore cannot name a model through
    the effect, which is why the ONNX template renders it as a pass-through and
    why `packaging-and-install.md` §2 does not claim a device render in Resolve.
    """
    return [name for name in dir(effect) if not name.startswith('_')]  # RUN


def quit_resolve(resolve, process, grace=30):
    """Quit through the API, then through the PID this run started."""
    if resolve is not None:
        try:
            resolve.Quit()                                # RUN
        except Exception:
            pass
    if process is None or process.poll() is not None:
        return
    try:
        process.wait(timeout=grace)
        return
    except subprocess.TimeoutExpired:
        pass
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)
