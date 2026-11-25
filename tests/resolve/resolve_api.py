"""The part of the proof of concept that needs a running Resolve.

**NOT RUN.** Nothing in this module has been executed: there is no Resolve, no
Studio licence and no GPU on either machine this repository has been exercised
on. Every call into `DaVinciResolveScript` is marked `# NOT RUN` because it was
read out of the scripting documentation and never answered.

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
    # NOT RUN: the documented location is a %PROGRAMDATA% path, but the variable
    # is what the README tells you to set, so trust the variable first.
    return os.environ.get("RESOLVE_SCRIPT_API")


def connect(timeout=120, poll=2.0):
    """Return a Resolve object with a project open, or raise."""
    api_dir = scripting_api_dir()
    if not api_dir:
        raise NotRun("RESOLVE_SCRIPT_API is not set; see references/resolve-automation.md section 2")

    modules = os.path.join(api_dir, "Modules")
    if modules not in sys.path:
        sys.path.insert(0, modules)

    import DaVinciResolveScript as dvr  # NOT RUN: documented module name

    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            resolve = dvr.scriptapp("Resolve")   # NOT RUN: None until Resolve is up
        except Exception as error:
            last_error = error
            resolve = None
        if resolve:
            manager = resolve.GetProjectManager()          # NOT RUN
            if manager and manager.GetCurrentProject():    # NOT RUN
                return resolve
        time.sleep(poll)
    raise TimeoutError(f"no Resolve after {timeout}s (last error: {last_error})")


def launch(executable, nogui=True):
    """Start Resolve and return the process; readiness is `connect`'s job."""
    executable = Path(executable)
    if not executable.exists():
        raise NotRun(f"no Resolve executable at {executable}")
    arguments = [str(executable)]
    if nogui:
        arguments.append("-nogui")   # NOT RUN: documented, unmeasured on Windows
    process = subprocess.Popen(arguments, cwd=str(executable.parent))
    return process


def load_project(resolve, project_path):
    manager = resolve.GetProjectManager()                 # NOT RUN
    project = manager.LoadProject(str(project_path))      # NOT RUN
    if not project:
        raise RuntimeError(f"could not load project: {project_path}")
    return project


def render_one_frame(project, output_directory, custom_name, timeout):
    """Queue one render, wait, and return the file it says it wrote."""
    out = Path(output_directory)
    out.mkdir(parents=True, exist_ok=True)

    # The one part of this function that is tested: `output_files` decides what
    # counts as this run's file, and `test_stale_file.py` proves that a run which
    # writes nothing fails instead of grading the previous run's output.
    before = output_files.snapshot(out, custom_name)

    settings = {"TargetDir": str(out), "CustomName": custom_name, "SelectAllFrames": True}
    if not project.SetRenderSettings(settings):           # NOT RUN
        raise RuntimeError("SetRenderSettings refused the map")

    job_id = project.AddRenderJob()                       # NOT RUN: "" on failure
    if not job_id:
        raise RuntimeError("AddRenderJob returned no job id")

    if not project.StartRendering([job_id], isInteractiveMode=False):   # NOT RUN
        raise RuntimeError("StartRendering refused the job")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not project.IsRenderingInProgress():            # NOT RUN
            break
        time.sleep(2)
    else:
        project.StopRendering()                           # NOT RUN
        raise TimeoutError(f"render {job_id} exceeded {timeout}s")

    status = project.GetRenderJobStatus(job_id)           # NOT RUN: dict OR error string
    if not isinstance(status, dict):
        raise RuntimeError(f"GetRenderJobStatus returned a string: {status}")
    if status.get("JobStatus") != "Complete":
        raise RuntimeError(f"render failed: {status.get('JobStatus')} ({status.get('Error')})")

    newest = output_files.newest_new_file(out, custom_name, before)

    return {"job": job_id, "file": str(newest), "status": status}


def quit_resolve(resolve, process, grace=30):
    """Quit through the API, then through the PID this run started."""
    if resolve is not None:
        try:
            resolve.Quit()                                # NOT RUN
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
