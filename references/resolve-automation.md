# Driving Resolve as a test host: what the API can do, and the first milestone

**Status: research, with a measured first run (2026-11-25).** The §9 milestone now
has a working script: `tests/resolve/poc.py --build-project` ran install → launch
`-nogui` → connect → build the project → apply the effect → render one PNG →
decode → check → quit on DaVinci Resolve Studio 21.2.0.5, exit 0
(`evidence/2026-11-25-windows/resolve-poc.log`). Each statement below keeps the
grade it earned: **[V]** where that run answered it, **[B]** from external
documentation, **[I]** where it was inferred and still is. Two questions in §12
closed; the run did **not** prove the plug-in's pixel path, and §13 says why.

The question this answers: can the plug-in be tested *inside the host* that
matters, automatically, without a human clicking? Short answer: the render half
is documented and plausible; the OFX half is not, and that decides the
architecture.

## 1. The short version

| Capability | What the API offers | Grade | What a test can rely on |
|---|---|---|---|
| Launch without a UI | `-nogui` command-line option | **[B]** | documented in the scripting README; says nothing about GPU or session |
| Connect from Python | `DaVinciResolveScript.scriptapp("Resolve")` after three environment variables | **[B]** | returns `None` until Resolve runs with a project open |
| Open a prepared project | `ProjectManager.LoadProject(path)` | **[B]** | returns a project object or `None` |
| Create media, timelines | `MediaPool.ImportMedia`, `MediaPool.CreateEmptyTimeline`, `AppendToTimeline` | **[B]** | documented calls, one per step |
| Insert an OFX effect | `TimelineItem.AddEffect("ofx", id)`, id from `Resolve.GetEffectsList()` | **[V]** | works on 21.2: the host listed `Open FX/OFX Plugin Developer/Reference Gain` and the item reported it attached. The shipped README has no such call, so the `.pyi` is the authority |
| Set an OFX parameter | *nothing documented* | **[V]** | `Effect` exposes `GetInfo`, `GetIsEnabled`, `SetIsEnabled`, `GetFusionComp`, `GetIndex` and `SetIndex` - no parameter access, so a non-default value needs a hand-made project |
| Render settings | `SetRenderSettings({...})`, `SetCurrentRenderFormatAndCodec(fmt, codec)` | **[B]** | key names matter; see §6 |
| Queue and start | `AddRenderJob()`, `StartRendering([ids], isInteractiveMode=False)` | **[B]** | job id, then polling |
| Job status | `GetRenderJobStatus(jobId)` → dict, or an error string | **[B]** | `JobStatus`, `CompletionPercentage`, `EstimatedTimeRemainingInMs`, `TimeTakenToRenderInMs`, `Error` |
| Clean exit | `resolve.Quit()` | **[B]** | preferred over `TerminateProcess` |
| Free vs Studio | external Python scripting is Studio-only since 21.1 | **[B]** | the free version runs scripts from its console only |

Two of those rows are the whole project. Everything else is plumbing.

## 2. What has to exist before the first line

- **A Studio licence.** The scripting README says "Scripting API for DaVinci
  Resolve Studio", and the 21.1 free release notes are quoted as moving Python
  scripting to Studio ("the Python API was being used to hack studio features
  into the free version") **[B]**. A free install can still run Lua and Python
  from its own console; an external process cannot connect. Confirm against the
  installed build before promising anything.
- **`Preferences > System > General > External scripting using = Local`** **[B]**.
  Network mode listens on TCP 1144 and is a different decision (it exposes the
  API to the LAN).
- **Three environment variables** so an external interpreter finds the bridge
  **[B]**:
  - `RESOLVE_SCRIPT_API` — `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting`
  - `RESOLVE_SCRIPT_LIB` — `C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll`
  - `PYTHONPATH` — `%RESOLVE_SCRIPT_API%\Modules\`

  The values are documented with quotes around them; on Windows the quotes break
  the connection, so set them without **[B]**.
- **Discovery, not hard-coding.** `%PROGRAMDATA%` is fixed, the application
  directory is not: read it from the registry or from the `Resolve` key, and let
  a config file override it. A path hard-coded to `C:\Program Files\...` is the
  first thing that breaks on a second machine.
- **A project open.** `scriptapp("Resolve")` hands back `None` while Resolve is
  still on the project manager, so "connected" is a state to poll, not a fact
  that follows from process creation **[B]**.

## 3. Headless: what `-nogui` promises, and what it does not

The scripting README has exactly two sentences about it **[B]**:

> DaVinci Resolve can be launched in a headless mode without the user interface
> using the `-nogui` command line option. When DaVinci Resolve is launched using
> this option, the user interface is disabled. However, the various scripting
> APIs will continue to work as expected.

That is a statement about the *API*, not about rendering, the GPU, or a Windows
session. One third-party project measured it on Resolve Studio 19.1.3.7 on macOS
and reports all seven pages addressable, `SetRenderSettings` + `AddRenderJob` +
`StartRendering(isInteractiveMode=False)` + polling to `Complete` + a non-empty
file **[B]**. Nothing in this research ran `-nogui` on Windows, with a GPU, or
with an OFX plug-in loaded.

Consequence for the framework: `-nogui` is the intended mode and the default
choice, but a started process proves nothing. The readiness check is
`scriptapp("Resolve")` returning a project-backed object, and the first real
assertion is a rendered file whose pixels pass §7.

## 4. The workflow, step by step

| Step | Call | Grade | The failure it hides |
|---|---|---|---|
| 1. Build | the template's `build.sh` / `build.bat` | **[V]** | nothing new |
| 2. Install | copy the bundle into the host's OFX directory | **[B]** | Resolve caches the plug-in list; a stale cache looks like a load failure |
| 3. Launch | `Resolve.exe -nogui`, or the same without the flag | **[B]** | a process that starts and never finishes initialising |
| 4. Connect | `scriptapp("Resolve")` in a poll loop | **[B]** | `None` forever when the preference or the env vars are wrong |
| 5. Project | `LoadProject(<prepared .drp>)` | **[B]** | a project that loads with the clip offline |
| 6. Timeline | `GetCurrentTimeline()` | **[B]** | an empty timeline that still renders black |
| 7. Effect | *nothing* — see §5 | **[I]** | a render that proves the plug-in was never applied |
| 8. Render | `SetRenderSettings` + `AddRenderJob` + `StartRendering` | **[B]** | a job that reports `Complete` with no file |
| 9. Validate | open the file, check dimensions, channels, finiteness, then compare | **[V]** for the comparison itself (`testkit/contract_main.cpp`) | a file that exists and is black |
| 10. Quit | `resolve.Quit()`, then wait for the process | **[B]** | a zombie Resolve that breaks the next run |

## 5. OFX on the timeline: the gap that decides the architecture

The scripting API exposes Resolve's own objects — project, media pool, timeline,
timeline items, render queue, Fusion comps, gallery. It does not expose "add
effect X to this clip and set parameter Y": the shipped API documentation
enumerates no such call **[I]**, and no third-party wrapper in this research
found one either.

So the honest architecture is the one the task already proposes: **prepared
`.drp` projects**, each with the plug-in already applied, parameters already set
and a render range already defined. The test framework varies the *project*, not
the parameters.

What follows from that, and what does not:

- Varying a parameter means another `.drp`, or a project template plus a scripted
  edit of something the API does expose. Both are acceptable; neither is free.
- Editing the `.drp` file itself is **not** an option to build on. It is an
  undocumented container (SQLite or a compressed database depending on version),
  and a schema change between Resolve versions would corrupt projects silently.
  If it is ever done, it is a generator that runs once on a known version and
  whose output is committed and reviewed.
- Fusion is the escape hatch worth measuring: `TimelineItem` exposes Fusion comps,
  and Fusion has its own scripting surface. Whether an OFX plug-in applied inside
  Fusion behaves like one applied on the colour page is an open question (§11).

## 6. Render jobs: what to trust

Documented signatures **[B]**:

- `SetCurrentRenderFormatAndCodec(fmtExt, codecId)` — extension and codec id, as
  returned by `GetRenderFormats()` / `GetRenderCodecs(fmt)`.
- `SetRenderSettings({...})` — `TargetDir`, `CustomName`, `SelectAllFrames`,
  `MarkIn`, `MarkOut` are the keys a test needs.
- `AddRenderJob()` → job id string, or `""`.
- `StartRendering([jobIds...], isInteractiveMode=False)` → `Bool`.
- `IsRenderingInProgress()` → `Bool`; `GetRenderJobStatus(jobId)` → a dict
  (`JobStatus`, `CompletionPercentage`, `EstimatedTimeRemainingInMs`,
  `TimeTakenToRenderInMs`, `Error`) **or an error string**, which is the trap: a
  caller that does `.get()` on a string gets an exception, not a status.
- `DeleteRenderJob(jobId)`, `DeleteAllRenderJobs()`.

A PASS requires all four, in this order: `JobStatus == "Complete"`; the target
file exists and is non-empty; the frame count matches the render range; the
pixels pass the validators. Anything less is "Resolve accepted the job".

## 7. Validation: what the plug-in can prove about itself

The repository already has the comparison logic the framework needs —
`testkit/contract_main.cpp` compares images with a per-reference tolerance and
has a self-test that can be shown failing. The new part is only the file format:
Resolve writes a container (MOV/EXR/DPX), not raw floats, so the reader has to
decode it. That is a dependency decision for the implementation phase, not now:
`ffmpeg` on `PATH`, OpenImageIO, or a small EXR/DPX reader. The task's own
sketch uses `oiio` + `numpy`.

Checks worth having, in order of what they catch: file exists → decodes → size
matches → channel count matches → no NaN/Inf → not constant when it should not
be → compares to the reference within the reference's own tolerance. A constant
frame is a *configurable* assertion, never a universal failure: an identity test
is supposed to be constant in the right way.

## 8. CPU vs GPU: the backend is not selected by the project

Resolve's GPU mode is a user preference, not a per-project setting
([`host-resolve.md`](host-resolve.md) §2.2 — "Auto" is a third state), so two
`.drp` files do not by themselves prove two backends ran. The only reliable
evidence is the plug-in's own report: the templates already print which path they
took (`ONNX` provider label, `isEnabledCudaRender` handling), and a plug-in under
test should expose the same in a log file or a parameter the framework can read
back. A comparison of CPU and GPU output is then a comparison of two *declared*
paths, and the declaration has to come from the plug-in, not from the harness.

## 9. The milestone: one frame, end to end

Build → install → launch → connect → open a prepared project → render one frame
→ validate the pixels → quit. Nothing else. It is the smallest thing that
exercises every unknown in §1, and it is done when:

1. a single command starts it, with no window clicked;
2. the plug-in is found by Resolve (`OfxGetNumberOfPlugins` visible in the log or
   the plug-in list through the API, if that exists — otherwise the render is the
   proof);
3. the render produces a file whose dimensions, channels and pixels match a
   reference that was made by hand once;
4. the run leaves no Resolve process behind, even when step 2 fails;
5. the failure path is as tested as the success path: kill Resolve mid-render and
   the harness reports FAIL with the log, not a hang.

Everything after that — the matrix of projects, reports, JUnit — is engineering
on a foundation that either works or does not.

## 10. Where it can run

| Environment | Verdict |
|---|---|
| Interactive Windows session with an NVIDIA GPU and Studio | the only one with evidence behind it **[B]** |
| Self-hosted Windows CI runner, logged-in session | plausible; the runner must own the desktop session, not run as a service |
| Windows service | no: a GPU context and a window station are what Resolve needs, and services have neither **[I]** |
| GitHub-hosted runner | no GPU, no licence, no persistent session **[I]** |

Licensing is part of the design: one Studio seat on the test machine, activated
once, and the harness must not touch the licence.

## 11. Implementation references

The task's prototypes A-H are kept whole in §13, as an appendix, because they are
the reference an implementation starts from. None of them has been executed: the
2026-11-25 run went through `tests/resolve/poc.py` instead, so these prototypes
are still reference code rather than evidence. Every call into
`DaVinciResolveScript` is marked `# unverified` in the appendix, and each
prototype ends with the questions that have to be answered before its first run
counts.

What each one has to settle, in the order the milestone needs them:

| Prototype | The question that decides whether it is right |
|---|---|
| A, process | whether `-nogui` returns before or after the API is up, and whether Resolve exits on its own after `Quit()` |
| B, connection | which module path the installed build actually uses, and whether `scriptapp` can return non-`None` with no project open |
| C, render | whether `JobStatus` reaches `Complete` before the file is closed, and what `isInteractiveMode` changes headless |
| D, projects | whether one `.drp` per case is affordable, or a generator is needed - and whether an OFX plug-in inside a Fusion comp renders like one on the colour page |
| E, integrity | which decoder is acceptable as a dependency (ffmpeg on `PATH`, OpenImageIO, or a small EXR/DPX reader) |
| F, comparison | whether the contract's MAE + max pair is enough for a host render, or a per-channel tolerance is needed |
| G, CPU/GPU | how the plug-in declares its backend in a way the harness can read back |
| H, entry point | how the script learns where Resolve is installed on a machine it has never seen |

## 12. What this document does not know

1. ~~Whether Resolve's API can list installed OFX plug-ins at all.~~ Answered on
   2026-11-25: it can list them, and it can apply them - see §1.
2. Whether an OFX plug-in applied inside a Fusion comp renders identically to one
   applied on the colour page.
3. Whether `-nogui` on Windows needs a logged-in desktop session, a GPU with a
   display attached, or neither.
4. Whether `GetRenderJobStatus` can report `Complete` before the file is closed.
5. Whether the free version's console can drive a render, as a fallback for a
   machine without a Studio seat.
6. What the `.drp` format actually is in the installed version, and whether a
   project saved by a newer Resolve opens in an older one.
7. How long a cold start takes on the target machine, which sets the timeout that
   the whole harness inherits.
8. Whether the plug-in's own log is reachable from the harness without reading
   the host's log directory.

Each of these is settled by one run on the machine that has Resolve. That is the
milestone.

## 13. Appendix: the full prototypes (none of them has been run)

Kept as implementation references, in the shape the task specified. The `# unverified`
comments mark calls that were read out of the scripting documentation and never
executed; a comment without that marker is ordinary Python. Nothing in this
appendix is code in the tree: no file imports it, no test runs it, and
`tools/check-docs.py` does not parse it.

### 13.1 A - process management (`runner/resolve_process.py`)

```python
# NOT RUN. Nothing here touches DaVinciResolveScript; what is unverified is
# whether -nogui is enough to reach a usable API, and that is 13.2's question.
import subprocess
from pathlib import Path


class ResolveProcess:

    def __init__(self, executable, use_nogui=True):
        self.executable = Path(executable)
        self.use_nogui = use_nogui
        self.process = None
        self.pid = None

    def start(self):
        if not self.executable.exists():
            raise FileNotFoundError(self.executable)

        arguments = [str(self.executable)]
        if self.use_nogui:
            arguments.append("-nogui")   # unverified: documented as "API keeps working"

        self.process = subprocess.Popen(arguments, cwd=str(self.executable.parent))
        # The PID we started is the only process this class may ever stop. Never
        # taskkill /IM: that kills a Resolve the user already had open.
        self.pid = self.process.pid
        return self.pid

    def is_running(self):
        return self.process is not None and self.process.poll() is None

    def died_early(self):
        # A process that exits during startup has already told you everything.
        return self.process is not None and self.process.poll() is not None

    def stop(self, grace=30):
        if not self.is_running():
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
```

Startup is **not** proven by `start()`: readiness is `connect_to_resolve()` in
13.2 returning a project-backed object, and that call owns the startup timeout.
A started process that never initialises is the failure this split exists to make
visible; a timeout kept here as well would only be a second number to keep in
step.

### 13.2 B - connection (`runner/resolve_api.py`)

```python
# NOT RUN.
import os
import sys
import time


def scripting_api_dir():
    # Windows: %PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting
    # Discovery, not a literal: the application directory is not fixed either.
    return os.environ.get("RESOLVE_SCRIPT_API")


def connect_to_resolve(timeout=120, poll=2.0):
    api_dir = scripting_api_dir()
    if not api_dir:
        raise RuntimeError("RESOLVE_SCRIPT_API is not set")

    modules = os.path.join(api_dir, "Modules")
    if modules not in sys.path:
        sys.path.insert(0, modules)

    import DaVinciResolveScript as dvr  # unverified: the module name is the documented one

    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            resolve = dvr.scriptapp("Resolve")   # unverified: None until Resolve runs
        except Exception as error:               # the bridge raises while starting up
            last_error = error
            resolve = None

        if resolve:
            manager = resolve.GetProjectManager()          # unverified
            if manager and manager.GetCurrentProject():    # unverified: project open?
                return resolve

        time.sleep(poll)

    raise TimeoutError(f"no Resolve after {timeout}s (last error: {last_error})")
```

`scriptapp("Resolve")` returning an object is not enough: the documented
behaviour is that it answers `None` until Resolve runs with a project open, so
the readiness condition includes `GetCurrentProject()`.

### 13.3 C - project and render management (`runner/render_manager.py`)

```python
# NOT RUN.
import time
from pathlib import Path


def load_project(resolve, project_path):
    manager = resolve.GetProjectManager()        # unverified
    project = manager.LoadProject(str(project_path))   # unverified
    if not project:
        raise RuntimeError(f"could not load project: {project_path}")
    return project


def render_project(project, output_directory, custom_name="test", timeout=600):
    out = Path(output_directory)
    out.mkdir(parents=True, exist_ok=True)

    settings = {
        "TargetDir": str(out),          # unverified: key names per the shipped API doc
        "CustomName": custom_name,
        "SelectAllFrames": True,
    }
    # A range test sets MarkIn/MarkOut *instead of* SelectAllFrames; setting both
    # is how a test silently renders frame 0 and calls it a pass.
    if not project.SetRenderSettings(settings):        # unverified
        raise RuntimeError("SetRenderSettings refused the map")

    # What is already in the directory decides what this run produced: a stale
    # file from the previous run would otherwise satisfy every check below.
    before = {path.name for path in out.glob(f"{custom_name}*")}

    job_id = project.AddRenderJob()                    # unverified: "" on failure
    if not job_id:
        raise RuntimeError("AddRenderJob returned no job id")

    if not project.StartRendering([job_id], isInteractiveMode=False):   # unverified
        raise RuntimeError("StartRendering refused the job")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not project.IsRenderingInProgress():        # unverified
            break
        time.sleep(2)
    else:
        project.StopRendering()                        # unverified
        raise TimeoutError(f"render {job_id} exceeded {timeout}s")

    status = project.GetRenderJobStatus(job_id)        # unverified: dict OR error string
    if not isinstance(status, dict):
        raise RuntimeError(f"GetRenderJobStatus returned a string: {status}")

    job_status = status.get("JobStatus")
    if job_status != "Complete":
        raise RuntimeError(f"render failed: {job_status} ({status.get('Error')})")

    # "Complete" is a claim about the job, not about the file: a file that is
    # still being flushed looks exactly like one that finished.
    produced = sorted(path for path in out.glob(f"{custom_name}*") if path.name not in before)
    if not produced:
        raise RuntimeError(f"job Complete but it wrote nothing new in {out}")
    newest = max(produced, key=lambda path: path.stat().st_mtime)
    if newest.stat().st_size == 0:
        raise RuntimeError(f"job Complete but {newest.name} is empty")

    return {"job": job_id, "status": status, "file": str(newest)}
```

### 13.4 D - prepared projects (`projects/`)

No code. One `.drp` per case, saved by hand on a known Resolve version, with the
plug-in applied, the parameters set and the render range defined. The harness
loads them and never edits them: the `.drp` container is undocumented and its
schema moves between versions.

| Project | Test | Purpose |
|---|---|---|
| `basic_ofx_test.drp` | basic render | the plug-in runs at all |
| `rgba_test.drp` | alpha | RGBA and alpha survive the host |
| `gpu_test.drp` | CUDA | the declared GPU path renders |
| `parameter_low.drp` / `parameter_high.drp` | parameter bounds | the edges of the parameter range |
| `resolution_test.drp` | 4K | the largest window a host without tiling hands you |

### 13.5 E - image integrity (`validation/image_integrity.py`)

```python
# NOT RUN. Needs OpenImageIO + numpy, which this repository does not have yet.
import numpy as np
import OpenImageIO as oiio


def validate_image(path, expect=None, allow_black=False):
    expect = expect or {}

    image = oiio.ImageInput.open(str(path))
    if image is None:
        raise RuntimeError(f"could not open image: {path}")

    spec = image.spec()
    pixels = image.read_image(format=oiio.FLOAT)
    image.close()

    if pixels is None:
        raise RuntimeError("empty image")
    pixels = np.asarray(pixels)
    if pixels.size == 0:
        raise RuntimeError("empty image")

    problems = []
    if "width" in expect and spec.width != expect["width"]:
        problems.append(f"width {spec.width} != {expect['width']}")
    if "height" in expect and spec.height != expect["height"]:
        problems.append(f"height {spec.height} != {expect['height']}")
    if "channels" in expect and spec.nchannels != expect["channels"]:
        problems.append(f"channels {spec.nchannels} != {expect['channels']}")

    if not np.isfinite(pixels).all():
        problems.append("NaN or Inf")

    # A constant frame is a policy, not a verdict: an identity test is meant to
    # be constant, and a black frame can be the correct answer.
    if not allow_black and float(np.max(np.abs(pixels))) == 0.0:
        problems.append("completely black")

    if problems:
        raise RuntimeError("; ".join(problems))

    return {
        "width": spec.width,
        "height": spec.height,
        "channels": spec.nchannels,
        "format": str(spec.format),
        "pixel_count": int(pixels.size),
        "status": "PASS",
    }
```

Alpha is checked by the channel count and by comparing the alpha plane in 13.6,
not by a separate rule here.

### 13.6 F - image comparison (`validation/image_comparison.py`)

```python
# NOT RUN. Same dependency as 13.5.
import numpy as np


def compare_images(reference, rendered, tolerance):
    reference = np.asarray(reference, dtype=np.float64)
    rendered = np.asarray(rendered, dtype=np.float64)

    if reference.shape != rendered.shape:
        return {"status": "FAIL", "reason":
                f"shape {rendered.shape} != reference {reference.shape}"}

    difference = np.abs(reference - rendered)
    # A single-channel image arrives as HxW; give it the channel axis so the
    # per-channel loop below does not index a row.
    if difference.ndim == 2:
        difference = difference[..., None]

    for key in ("max_mae", "max_pixel_error"):
        if key not in tolerance:
            raise KeyError(f"tolerance is missing {key}")

    mae = float(np.mean(difference))
    worst = float(np.max(difference))
    rmse = float(np.sqrt(np.mean(difference ** 2)))
    peak = 1.0
    psnr = float("inf") if rmse == 0 else 20.0 * np.log10(peak / rmse)

    # Per-channel, because alpha is not graded like colour and a single global
    # number hides a broken plane.
    per_channel = [float(np.max(difference[..., c])) for c in range(difference.shape[-1])]
    limits = tolerance.get("per_channel_max") or [tolerance["max_pixel_error"]] * len(per_channel)

    failures = []
    if mae > tolerance["max_mae"]:
        failures.append(f"mae {mae:.3e} > {tolerance['max_mae']:.3e}")
    if worst > tolerance["max_pixel_error"]:
        failures.append(f"max {worst:.3e} > {tolerance['max_pixel_error']:.3e}")
    for channel, (value, limit) in enumerate(zip(per_channel, limits)):
        if value > limit:
            failures.append(f"channel {channel} max {value:.3e} > {limit:.3e}")

    return {
        "status": "FAIL" if failures else "PASS",
        "mae": mae,
        "max_pixel_error": worst,
        "rmse": rmse,
        "psnr": psnr,
        "per_channel_max": per_channel,
        "failures": failures,
    }
```

Tolerances come from the reference, as they do in `testkit/contract_main.cpp`
(`name.<fuzz>.fuzz`), never from one global number: a CPU reference keeps its
exact demand and only the reference a device cannot reproduce bit for bit gets a
bound. Compare only after both images are in the same representation - same
depth, same transfer characteristic, same channel order.

### 13.7 G - CPU/GPU comparison (`validation/cpu_gpu_comparison.py`)

```python
# NOT RUN.
from pathlib import Path

from validation.image_comparison import compare_images


def run_cpu_gpu_comparison(render, load, backend_of, cpu_project, gpu_project,
                           output_dir, cpu_backend, gpu_backend, tolerance):
    # The backend is declared by the plug-in, not inferred from which project
    # rendered: Resolve's GPU mode is a user preference, so two projects do not
    # prove two backends. `render` writes the file; `backend_of` reads the
    # declaration the plug-in left beside it (a log line, a sidecar, a param).
    cpu = render(cpu_project, Path(output_dir) / "cpu")
    gpu = render(gpu_project, Path(output_dir) / "gpu")

    cpu_declared = backend_of(cpu["file"])
    gpu_declared = backend_of(gpu["file"])
    if cpu_declared != cpu_backend:
        raise RuntimeError(f"CPU run declared {cpu_declared!r}, wanted {cpu_backend!r}")
    if gpu_declared != gpu_backend:
        raise RuntimeError(f"GPU run declared {gpu_declared!r}, wanted {gpu_backend!r}")

    result = compare_images(load(cpu["file"]), load(gpu["file"]), tolerance)
    result["cpu"] = cpu
    result["gpu"] = gpu
    return result
```

### 13.8 H - PowerShell entry point (`tests/resolve/run_resolve_tests.ps1`)

```powershell
# NOT RUN.
param(
    [Parameter(Mandatory = $true)][string]$PluginPath,
    [string]$Configuration = "Release",
    [switch]$EnableCUDA,
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = "Stop"

# The dependencies the task asks for, checked before anything starts: a missing
# interpreter or an unset API path otherwise surfaces as "could not connect".
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "python is not on PATH; install it or call the orchestrator directly"
}
if (-not $env:RESOLVE_SCRIPT_API) {
    throw "RESOLVE_SCRIPT_API is not set; point it at the Resolve scripting folder"
}

$RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not (Test-Path $PluginPath)) { throw "plugin not found: $PluginPath" }
$Plugin = (Resolve-Path $PluginPath).Path
if (-not (Test-Path (Join-Path $RepoRoot "tests\resolve"))) {
    throw "tests\resolve is missing; run from a full checkout"
}

$env:PYTHONPATH = "$env:PYTHONPATH;$env:RESOLVE_SCRIPT_API\Modules"

$arguments = @(
    "-m", "tests.resolve.runner.test_orchestrator",
    "--plugin", $Plugin,
    "--configuration", $Configuration,
    "--timeout", $TimeoutSeconds,
    "--repo-root", $RepoRoot
)
if ($EnableCUDA) { $arguments += "--cuda" }

& python @arguments
$exit = $LASTEXITCODE

# Reports survive a failure; the process does not survive the run.
if ($exit -ne 0) {
    Write-Host "Resolve test suite failed with exit $exit; reports kept in tests/resolve/reports"
    exit $exit
}
Write-Host "Resolve test suite completed"
exit 0
```

The orchestrator it calls owns the cleanup, because the PowerShell wrapper cannot
know whether Resolve was started. A `finally` there quits through the API and
falls back to terminating the recorded PID.

### 13.9 I - orchestrator (`runner/test_orchestrator.py`)

```python
# NOT RUN. Pseudocode from the task, made concrete; every call into Resolve is
# unverified. Imports omitted: time, ResolveProcess, connect_to_resolve,
# load_project, render_project, validate_image, compare_images, load,
# load_test_cases, validate_plugin, install_plugin, write_reports, sys.
def run_test_suite(config):
    results = []
    resolve = None
    process = None

    try:
        validate_plugin(config.plugin_path)
        install_plugin(config.plugin_path, config.host_dir)
        process = ResolveProcess(config.executable, use_nogui=True)
        process.start()
        resolve = connect_to_resolve(timeout=config.startup_timeout)

        for case in load_test_cases(config.cases_dir):
            started = time.monotonic()
            try:
                project = load_project(resolve, case.project)
                rendered = render_project(project, case.output_dir,
                                          custom_name=case.name,
                                          timeout=config.timeout)
                integrity = validate_image(rendered["file"], case.expect)
                if case.reference:
                    verdict = compare_images(load(rendered["file"]),
                                             load(case.reference), case.tolerance)
                    if verdict["status"] != "PASS":
                        raise RuntimeError(f"reference mismatch: {verdict['failures']}")
                results.append({"name": case.name, "status": "PASS",
                                "seconds": time.monotonic() - started,
                                "integrity": integrity,
                                "artifacts": [rendered["file"]]})
            except Exception as error:
                results.append({"name": case.name, "status": "FAIL",
                                "seconds": time.monotonic() - started,
                                "error": str(error)})
    finally:
        if resolve is not None:
            try:
                resolve.Quit()          # unverified: the documented clean exit
            except Exception:
                pass
        if process is not None:
            process.stop()              # only the PID this run started
        # Reports are written here rather than after the loop so a crash still
        # leaves them; a reporting bug must not replace the failure it describes.
        try:
            write_reports(results, config.report_dir)   # JSON, HTML, JUnit XML
        except Exception as error:
            print(f"could not write reports: {error}", file=sys.stderr)

    return results
```

## Provenance labels

**[V]** verified in this repository — a command ran here and its output is in
`evidence/`. **[I]** inferred from a header or a document, not demonstrated.
**[B]** external evidence: Blackmagic's own README and release notes, or a
third-party project that measured it. This document contains no **[V]** — every
statement in it is external or inferred, and the milestone in §9 is what would
produce the first.
