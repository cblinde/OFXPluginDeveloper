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
| Free vs Studio | external Python scripting is Studio-only: the SDK documents the preference under Studio and the listener on TCP 1144 as Studio's | **[B]** | on free there is no listener, so `scriptapp` cannot connect and this harness cannot run (§14.1) |
| Is it Studio? | `resolve.IsStudio()` | **[V]** | `True` on 21.2.0.5; assert it first so a free machine fails with a reason, not a timeout |

Two of those rows are the whole project. Everything else is plumbing.

## 2. What has to exist before the first line

- **A Studio licence.** The scripting README documents external scripting only
  under Studio, and the socket it opens (TCP 1144) as Studio's; the free version
  runs scripts from its own Workspace > Scripts menu and Console **[B]**. For
  this harness that is decisive rather than limiting: `poc.py` is an external
  process, so on free `scriptapp` answers `None` until the timeout and there is
  no render to check. §14.1 has the long form, and `resolve.IsStudio()` is the
  runtime check (**[V]**: `True` on 21.2.0.5).
- **`Preferences > System > General > External scripting using = Local`** **[B]**.
  Network mode listens on TCP 1144 and is a different decision (it exposes the
  API to the LAN). It was already set to `Local` on the machine that ran
  `tests/resolve/`, which is why nothing had to be clicked.
- **Three environment variables** so an external interpreter finds the bridge
  **[B]** (all three measured, §14.3):
  - `RESOLVE_SCRIPT_API` — `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting`
  - `RESOLVE_SCRIPT_LIB` — `C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll`
  - `PYTHONPATH` — `%RESOLVE_SCRIPT_API%\Modules\`

  The values are documented with quotes around them; on Windows the quotes break
  the connection, so set them without **[B]**.
- **Discovery, not hard-coding.** `%PROGRAMDATA%` is fixed, the application
  directory is not - and the registry is no help: `HKCU\Software\Blackmagic
  Design\DaVinci Resolve` does not exist and `where Resolve.exe` finds nothing
  (**[V]** on the machine that ran this, where Resolve sat on `E:`). What worked
  was looking for `<drive>:\Program Files\Blackmagic Design\DaVinci Resolve\` on
  each drive. A path hard-coded to `C:\Program Files\...` is the first thing
  that breaks on a second machine - including inside the SDK module itself.
- **A project open.** `scriptapp("Resolve")` hands back `None` while Resolve is
  still on the project manager, so "connected" is a state to poll, not a fact
  that follows from process creation **[V]**: 18.2 s from `Popen` to a live
  `scriptapp` here.

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
timeline items, render queue, Fusion comps, gallery. This section used to say it
did not expose "add effect X to this clip": the shipped README enumerates no such
call, and that absence was read as a missing capability. The shipped `.pyi` has
it — `TimelineItem.AddEffect(category, id)` with `category="ofx"` — and on 21.2
it works (**[V]**, §14.6). What the API still does not expose is a **parameter**:
`Effect` offers `GetInfo`, `GetIsEnabled`, `SetIsEnabled`, `GetFusionComp`,
`GetIndex` and `SetIndex`, and nothing else.

So the architecture splits in two, and only the second half needs prepared
projects:

- **Applying the plug-in needs no `.drp`.** Import a still, cut a timeline, call
  `AddEffect("ofx", id)` with the id from `GetEffectsList()`. That is what
  `tests/resolve/poc.py --build-project` does, and it is the shortest path from a
  built bundle to a rendered frame.
- **Varying a parameter still needs one**, because there is no setter. A project
  with the parameter already at a non-default value, or an effect whose default
  is not identity, is the only way to make the host call `render()` instead of
  skipping the effect as identity.

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

**Measured 2026-11-26.** The declaration exists and works. `OFX::Log::print` does
not reach Resolve's log - the host log names the bundle at load time and nothing
the plug-in printed - so the CUDA template appends one line per render to the file
named by `$MCOFX_PATH_LOG`, and `tests/resolve/poc.py --path-log <file>` sets that
variable for the host it launches. In the Resolve run all eight render invocations
declared `CUDA`, so the frame that matched the reference times two was produced by
the kernel, not by the fallback
(`../evidence/2026-11-26-windows/resolve-poc-cuda-gain.log`).

### What the declaration can and cannot say

It records the path and the render window, nothing else: no separate line for "the
host supplied a stream", no timing, no device index. The branch that writes `CUDA`
refuses to run unless the host set `kOfxImageEffectPropCudaEnabled`, so a CPU
render cannot write it; but "CUDA path" means "this plug-in's CUDA code ran", not
"the frame never touched a host-side CPU copy on the way out".

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

**Status, 2026-11-25:** 1, 2, 3 and 4 hold on Resolve Studio 21.2.0.5. The first
run (`evidence/2026-11-25-windows/resolve-poc.log`, exit 0, no process left
behind) proved the host path: its frame was pixel-identical to the source
because the plug-in is identity at its default gain and the API cannot set the
parameter. The second run rebuilt the bundle with `-DREFERENCE_GAIN_DEFAULT=2.0`
and required the frame to be the source times two; it matched to the byte
(`mae 0.0`, `max 0`, `resolve-poc-gain.log`), so `render()` ran inside the host.
5 holds as well: `--kill-render-after 0.5` kills the host with a job in flight,
and the run ends `fail` in 47 s with `the host exited (code 1) while render ...
was in flight`, no process left behind (`resolve-poc-kill.log`, §14.12).

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
7. ~~How long a cold start takes on the target machine, which sets the timeout
   that the whole harness inherits.~~ 18.2 s to a live `scriptapp` on the machine
   that ran this, so the 120 s default in `resolve_api.connect()` has room.
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

## 14. Testing against Resolve, hands-on

Everything in this section is what a second person needs to repeat the run, and
most of it cost a failed attempt to learn. The numbers are from
`tests/resolve/poc.py --build-project` on 2026-11-25: Windows 11, RTX 3060,
DaVinci Resolve Studio 21.2.0.5, Python 3.10.6 (64-bit), ffmpeg on `PATH`. The
log is `../evidence/2026-11-25-windows/resolve-poc.log`, and the second run - the
one that proves the pixel path - is `resolve-poc-gain.log` beside it (§14.10).

### 14.1 Studio or free, and why free cannot carry this harness

The SDK's own README decides it before any code does:

- "**In DaVinci Resolve Studio**, Preferences > System > General, you can
  configure: External scripting, i.e. whether external scripts can connect to
  Resolve (None, Local, or Network)." The preference that opens the door is
  documented under Studio.
- "DaVinci Resolve **Studio** and Fusion Studio scripting listens on port 1144
  (registered with IANA for this purpose)." The listener a remote process
  connects to is Studio's.
- The free version runs scripts **internally**: Workspace > Scripts and the
  Workspace > Console window. The API surface is a common superset, and a call
  that is Studio-only "can return with a False status" on free.

Why that makes the free version useless **for this repository** rather than merely
limited: the harness is an external Python process. `poc.py` starts the host,
then waits for `DaVinciResolveScript.scriptapp("Resolve")` to return an object.
With no listener there is nothing to return, so the call answers `None` forever
and `resolve_api.connect()` times out after its 120 s budget - no project, no
render, no report, no exit code. The internal fallback is not a substitute: a
human would paste a script into the Console on every run and read the result off
the screen, which is a manual check, cannot run unattended or in CI, and produces
nothing a test can assert on. Studio is therefore a hard prerequisite of the
milestone, not an optimisation.

Measured on the Studio install, and worth asserting in any harness so a free
machine fails fast instead of timing out:

| Call | Answer on 21.2.0.5 |
|---|---|
| `resolve.IsStudio()` | `True` |
| `resolve.GetProductName()` | `DaVinci Resolve Studio` |
| `resolve.GetVersionString()` | `21.2.0.5` |

Two more Studio-only facts that bite later: multi-frame random access for OFX
plugins (§2.6) and, per the SDK, the AI/Extras-backed functions.

### 14.2 Where the pieces are on Windows

| Piece | Default location |
|---|---|
| `Resolve.exe` | `C:\Program Files\Blackmagic Design\DaVinci Resolve\` |
| `fusionscript.dll` | beside `Resolve.exe` - **not** in the SDK folder |
| SDK module | `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules\DaVinciResolveScript.py` |
| Type stubs and README | the parent of `Modules` (`DaVinciResolveScript.pyi`, `CHANGELOG.md`) |
| OFX scan directory | `C:\Program Files\Common Files\OFX\Plugins` |
| OFX cache | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` |
| Host log | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\ResolveDebug.txt` |
| Project library | `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Resolve Project Library` |

The install path is the one thing that is never the same twice. On the machine
that produced these numbers Resolve sat on `E:`, the SDK sat on `C:`, and neither
the registry (`HKCU\Software\Blackmagic Design\DaVinci Resolve` does not exist)
nor `where Resolve.exe` found it - `%PATH%` has no entry either. What worked was
looking for `<drive>:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe`
on each drive. The SDK module's own fallback is hard-coded to `C:\Program
Files\...`, which is why 14.3 exists.

### 14.3 Environment: three variables, one of them optional in theory only

| Variable | Value | Why |
|---|---|---|
| `RESOLVE_SCRIPT_API` | the `Scripting` directory (parent of `Modules`) | where `DaVinciResolveScript` is imported from; the module does not find itself |
| `RESOLVE_SCRIPT_LIB` | `<install>\fusionscript.dll` | **mandatory when Resolve is not on `C:`** - see below |
| `PYTHONPATH` | `%RESOLVE_SCRIPT_API%\Modules` | convenience; `sys.path.insert` does the same |

Without `RESOLVE_SCRIPT_LIB` on a non-`C:` install the import fails even though
the module is found, with `ImportError: DLL load failed while importing
fusionscript: Das angegebene Modul wurde nicht gefunden.` (the message is
localised; on an English system it reads "The specified module could not be
found"). The reason is in `DaVinciResolveScript.py`: it tries `import
fusionscript`, then `RESOLVE_SCRIPT_LIB`, then a hard-coded
`C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll`. Deriving
the path from the executable's directory is what `resolve_api.library_path()`
does, and it removes a variable the caller would otherwise have to get right.

Python must be **64-bit** and 3.6 or newer; 3.10.6 worked. The `python3` that a
fresh Windows shell may resolve to can be a Store stub that prints an advert and
exits 0 - check the interpreter, not just the exit code.

### 14.4 Launch and connect

```python
process = subprocess.Popen([exe, "-nogui"], cwd=str(Path(exe).parent))
# then poll: dvr.scriptapp("Resolve") until it returns an object
```

- `-nogui` is documented as "the user interface is disabled, the various scripting
  APIs will continue to work as expected", and that held: 18.2 s from launch to a
  live `scriptapp` on this machine, with a project open and `GetProjectManager()`
  answering.
- `scriptapp` returns `None` while the host is down and an object once it is up.
  If Resolve is **already running**, the call attaches to that instance instead of
  starting another - convenient for iterating, and a reason to check for a
  running host before launching one.
- Poll, do not sleep a fixed amount: cold start varies with the project library.
  `resolve_api.connect()` polls every 2 s against a deadline and reports the last
  error it saw.
- `-nogui` ran here in an interactive desktop session with a display attached to
  the GPU. Whether it works from a service session, a scheduled task with no
  desktop, or a machine without a display is **not** measured.

### 14.5 Install the bundle before the host starts, then prove the host saw it

Resolve scans `C:\Program Files\Common Files\OFX\Plugins` at startup and records
what it found in `OFXPluginCacheV2.xml`. Three consequences:

1. **Copy the bundle before launching.** A bundle that appears afterwards is not
   in the list until the next start.
2. **The `.ofx` is locked while the host runs.** Replacing the bundle under a
   live Resolve fails with `PermissionError: [WinError 5]`; quit first.
3. **The load is in the host's log**, which is the cheapest proof that a bundle
   was seen at all: `ResolveDebug.txt` carries
   `OpenFX | OFX: loading com.ofxplugindeveloper.referencegain`.

A plugin that crashed once is blacklisted and silently skipped (§2.5): delete the
cache with the host closed, then look again.

### 14.6 Build the project through the API (this is what removes the `.drp`)

```python
manager, project = resolve_api.new_project(resolve, "poc")
resolve_api.set_timeline_size(project, 1920, 1080)          # a new project is 3840x2160
pool = project.GetMediaPool()
item = pool.ImportMedia([str(still.resolve())])[0]          # list of absolute strings
pool.CreateTimelineFromClips("poc", [{"mediaPoolItem": item}])
effect = timeline.GetItemListInTrack("video", 1)[0].AddEffect("ofx", effect_id)
```

- `ImportMedia` takes a **list of paths**, not a list of `{"FilePath": ...}`
dicts: the dict form answered `0` items on 21.2 while the string form answered 1.
- Paths must be **absolute**. A relative still imports and then fails at render
  time with `Failed (Full resolution media not found for source.png at
  01:00:00:00.)`.
- `AddEffect` takes the id the host lists, **not** the OFX identifier. Ask for it:
  `GetEffectsList()` returns dicts like
  `{"id": "Open FX/OFX Plugin Developer/Reference Gain", "category": "ofx", "name": "Reference Gain"}`
  - 225 entries on this machine, built-ins and third-party together. The id is
  `Open FX/<Grouping>/<Name>`, so match on `name` and take `id` from the same
  entry.
- `timelineItem.GetEffects("ofx")` then reports the applied effect, which is the
  assertion worth making before a render.

### 14.7 Render one frame, and what the settings are called

```python
project.SetCurrentRenderFormatAndCodec("png", "RGB8")
project.SetRenderSettings({"TargetDir": str(out.resolve()), "CustomName": "poc",
                            "SelectAllFrames": False, "MarkIn": 1, "MarkOut": 1,
                            "FormatWidth": 1920, "FormatHeight": 1080})
job = project.AddRenderJob()
project.StartRendering([job], isInteractiveMode=False)
while project.IsRenderingInProgress():
    time.sleep(2)
status = project.GetRenderJobStatus(job)
```

- The codec argument is the **internal name** `RGB8`, not the menu label
  "RGB 8 bits": the label is refused (`False`, format stays `mov`).
  `GetRenderCodecs("png")` returns `{"RGB 16 bits": "RGB16", "RGB 8 bits": "RGB8"}`,
  so the value is the second half of each pair.
- `TargetDir` must be **absolute**: a relative path that exists makes
  `SetRenderSettings` answer `True` and `AddRenderJob` return an empty id.
- A new project is **3840x2160**, so a 1920x1080 still is rendered scaled up
  unless the timeline resolution is set (14.6) or `FormatWidth`/`FormatHeight`
  say otherwise.
- `GetRenderJobStatus` returns a dict - `JobStatus`, `CompletionPercentage`,
  `TimeTakenToRenderInMs`, and `Error` on failure - or a plain string when the
  call itself fails. One 1920x1080 PNG took 750 ms; `JobStatus` was `Complete`.
- The file is named after the **timeline's** frame number, not 1: the run wrote
  `poc00108000.png` for its single frame. Do not assume `<name>00000001.png`.
- `Complete` is a claim about the job, not about the file; `output_files.py` is
  the rule that decides which file counts (and fails a run that wrote none).

### 14.8 Tear down without leaving a host behind

```python
manager.CloseProject(project)     # a loaded project cannot be deleted
manager.DeleteProject(name)
resolve.Quit()                    # then wait for the pid, terminate, kill
```

`DeleteProject` returns `False` for the project that is currently loaded, which
reads like a permissions problem and is not one. After `Quit()` the process took
about 14 s to exit here; the harness waits, then terminates, then kills, and
asserts that no process is left.

### 14.9 The traps, as a table

| Symptom | Cause | Fix |
|---|---|---|
| `ImportError: DLL load failed while importing fusionscript` | module fell back to `C:\Program Files\...` | set `RESOLVE_SCRIPT_LIB`, or derive it from the executable |
| `connect` times out, `scriptapp` stays `None` | host not up, or a **free** install (no listener), or External scripting is `None` | wait for the host; use Studio; set the preference to `Local` |
| `GetEffectsList()` has no entry for the bundle | bundle copied after startup, or the plugin is blacklisted | install first; clear the cache with the host closed |
| `AddEffect` returns `None` | wrong id (OFX identifier instead of the listed id), or not scanned | take `id` from `GetEffectsList()` |
| `ImportMedia` returns 0 items | list of dicts, or a path the host cannot resolve | list of absolute strings |
| `AddRenderJob` returns `""` | relative `TargetDir` | absolute path |
| job `Failed (Full resolution media not found ...)` | relative import path | absolute import path |
| PNG comes out 3840x2160 | new-project default resolution | set the timeline size, or `FormatWidth`/`FormatHeight` |
| format call returns `False` | `"RGB 8 bits"` label | `"RGB8"` |
| `DeleteProject` returns `False` | project still loaded | `CloseProject` first |
| `PermissionError: [WinError 5]` on the bundle | `.ofx` locked by a running host | quit Resolve before replacing it |
| rendered pixels identical to the source | plug-in is identity at its default gain | expected, not a failure - see 14.10 |

### 14.10 What a green run does not prove

The run's frame was pixel-identical to its source (`mae 0.0`, `max 0`), and that is
the correct result rather than a pass. This plug-in returns `isIdentity()` at its
default gain of 1.0, so the host may skip `render()` entirely - and the scripting
API cannot set an OFX parameter at all. `Effect` exposes `GetInfo`,
`GetIsEnabled`, `SetIsEnabled`, `GetFusionComp`, `GetIndex` and `SetIndex`, and
nothing else; `GetInfo()` returns `id`, `name` and `category`. There is no
`SetParameter` on 21.2 and none in the shipped `.pyi`.

So that run verifies the **host path** - scan, list, instantiate, apply, queue,
`Complete`, decode - and not the pixel maths. The parameter is not the only
lever, though: the *default* is a build-time constant, and a default that is not
1.0 makes `isIdentity()` false. The second run did that and required the frame to
be the source times two:

```bash
REFERENCE_GAIN_DEFAULT=2.0 templates/minimal-ofx/build.sh
python tests/resolve/poc.py --build-project --effect "Reference Gain" \
    --source build/resolve/source.png --reference build/resolve/source.png \
    --expect-gain 2 --out build/resolve/frames-gain ...
```

```
  check     pass     1920x1080 via ffmpeg, x2 mae 0.0 max 0
```

`mae 0.0` and `max 0` mean every one of the 8,294,400 bytes equals
`round(source x 2)` clamped at 255: the plug-in's `render()` ran inside Resolve,
its float maths survived the host's own 8-bit conversion, and the bright part of
the test image exercised the clamp. Log:
`../evidence/2026-11-25-windows/resolve-poc-gain.log`. `--expect-gain` rejects a
frame that skipped the render, and `checks.py` shows that rejection failing (case
`an undoubled frame is caught by the gain check`) - a check that has never failed
is a check nobody has tested.

What no host run has proved yet:

- the **GPU** path: this plug-in has none, and the CUDA template has never
  rendered in Resolve;
- **tiling and multi-resolution**: Resolve's profile asks for neither
  (`supportsTiles=0`), so a tiled host is the only place that code runs;
- **parameter round-trips**: setting a parameter and reading it back still needs
  the GUI, because the API has no accessor;
- **the failure path**: killing Resolve mid-render (§9, item 5).

### 14.11 The shortest reusable recipe

`tests/resolve/poc.py --build-project` is the whole sequence as one command, and
`tests/resolve/resolve_api.py` is the same sequence as functions with the
measured caveats in their docstrings. Copy the second, not the first: the script
owns argument parsing and the report, the module owns the calls. §15 is the map
behind the run: which calls exist, since which version, and what the API cannot
do at all.

### 14.12 The failure path: kill the host with a job in flight

A harness that has only ever succeeded is a harness nobody has tested.

```bash
python tests/resolve/poc.py ... --kill-render-after 0.5 --timeout 240
```

`--kill-render-after N` kills the host once the job has been rendering for `N`
seconds. The kill is issued **from the render loop** (`on_poll`), so it lands with
the job in flight by construction rather than by luck: a wall-clock timer outside
the loop fired after a fast render had already finished, which is how three
attempts in a row managed to test nothing.

```
  kill      sent     pid 6792 with the render in flight
  error     fail     RuntimeError: the host exited (code 1) while render f152f638-... was in flight
  quit      pass     API quit, then the recorded PID
```

Exit code 1 after 47 s, no Resolve process left behind, report written
(`../evidence/2026-11-25-windows/resolve-poc-kill.log`). Three things make that
work, and each one was added because its absence showed up as a hang or a lie:

- **Every call in the render path is bounded.** `call_with_deadline` runs the
  blocking call in a daemon thread and gives up after its budget; a host that
  stops answering becomes a failure instead of a wait.
- **Liveness is checked between polls, and the exit code is waited for.** The
  scripting layer reports a dead host as whatever its last socket call left
  behind (`'NoneType' object is not callable` was the common one), so `_exit_code()`
  waits briefly and turns that into `exited (code 1)`.
- **Cleanup cannot hang either.** Closing the project and quitting the host are
  bounded calls too; on a dead host they fail, and the report records it
  (`close_error`) instead of blocking.

Two harness bugs only this test could find, both of them the same shape - state
that a killed run cannot clean up after itself:

- **The project survived the kill.** `CreateProject` refuses a name that already
  exists, so the next run died before rendering anything. `new_project` now
  clears a stale project of the same name.
- **The output directory survived too**, and `newest_new_file` accepted only names
  that did not exist before - so a second run into the same directory could never
  pass. It now accepts a file that is new *or changed*, and `test_stale_file.py`
  keeps the stale case failing (case 2) while the overwrite case passes (case 3).

### 14.13 The CUDA template inside the host: a kernel, not the fallback

The CPU template proved that `render()` runs in Resolve (§14.10). The CUDA
template needs one step more, because it has two paths that produce the *same*
pixels: the CPU loop is the fallback for a host that does not enable CUDA render,
so a matching frame alone says nothing about which one ran. 2026-11-26, same
machine and same project recipe as §14.6:

```bash
GAIN_DEFAULT=2.0 CUDA_ARCHS=86 templates/minimal-cuda-ofx/build.bat Release
python tests/resolve/poc.py --build-project --effect "Minimal CUDA Gain" \
    --bundle templates/minimal-cuda-ofx/build/bundle/MinimalCudaGain.ofx.bundle \
    --source build/resolve/source.png --reference build/resolve/source.png \
    --expect-gain 2 --expect 1920x1080 \
    --path-log build/resolve/frames-cuda-gain/path.log ...
```

```
  connect   pass     DaVinci Resolve Studio 21.2.0.5             27.9 s
  build     pass     poc: Minimal CUDA Gain on source.png          1.9 s
  render    pass     poc00108000.png                               7.1 s
  check     pass     1920x1080 via ffmpeg, x2 mae 0.0 max 0        3.0 s
  path      pass     CUDA 1920x1080; ... CUDA 208x117; ...
  quit      pass     API quit, then the recorded PID              17.0 s
```

- `GAIN_DEFAULT` is the CUDA template's half of §14.10's lever, for the same
  reason: at 1.0 `isIdentity()` is true and the host may skip `render()`. The
  build reports `Default gain: 2.0`, `CUDA architectures: 86`, one cubin and two
  exports.
- The frame is the source times two **to the byte** over 8,294,400 bytes, so the
  plug-in's arithmetic ran inside the host and survived its 8-bit PNG conversion.
- The **declaration** is what makes it a GPU result. In all eight render
  invocations - seven at 1920×1080 and one small `208x117` window the host asked
  for on its own - the plug-in wrote `CUDA`. `processImagesCuda` refuses to run
  unless the host set `kOfxImageEffectPropCudaEnabled`, so a CPU render cannot
  write that line.
- What it still does not show: whether the host handed the plug-in a CUDA stream
  (the line records path and window only, §8), a render window larger than one
  image or a tiled one (this plug-in declares neither), a second GPU, or any
  throughput number - the 7.1 s of `render` cover hot-start host overhead and one
  frame.

Log: `../evidence/2026-11-26-windows/resolve-poc-cuda-gain.log`; report
`tests/resolve/reports/poc-2026-11-26T14-49-40+00-00.json`.

## 15. The API surface a plug-in developer needs

§14 is the run; this is the map behind it - which calls exist, since when, what
they were measured doing here, and what the API cannot do at all. It is written
down because two of these questions cost a failed attempt each, and a third
(the parameter) is the reason a green run still proves nothing about pixels.

### 15.1 The `.pyi` is the authority, and the `README.md` is not

`Support/Developer/Scripting/` ships three things worth opening: `README.md` (699
lines of prose), `DaVinciResolveScript.pyi` (3645 lines of signatures with
docstrings) and `CHANGELOG.md` (dated, per release). They do not agree. Counted
on 21.2.0.5:

| Name | in `README.md` | in `DaVinciResolveScript.pyi` |
|---|---|---|
| `AddEffect` | 0 | 2 |
| `GetEffectsList` | 0 | 3 |
| `DeleteEffects` | 0 | 2 |
| `ExportFrame` | 0 | 7 |
| `IsStudio` | 0 | 1 |

Everything §14 does with effects is absent from the README. Search the `.pyi`
(`grep -n "def AddEffect" DaVinciResolveScript.pyi`) and the `CHANGELOG.md` for
the version, and do not conclude from the README that a call does not exist.

### 15.2 Since which version

From the SDK `CHANGELOG.md`, not from memory:

| Call | First release | Impossible without it |
|---|---|---|
| `Resolve.GetEffectsList`, `TimelineItem.AddEffect`, `GetEffects`, `DeleteEffects`, `Effect.GetInfo`, `Get/SetIsEnabled`, `Get/SetIndex`, `GetFusionComp` | **21.1.2** - the release notes call them "Clip Effect Actions" and "Effect Properties" | listing or applying an OFX effect by name; a script can then only work from a prepared `.drp` (§5) |
| `ExportFrame` with `roi*`, `maxDimension` and `quality`; `ExportThumbnails`; `Graph.GetNodeEnabled`; `GetCDL` | **21.2** | single-frame export exists, but without region, scaling and JPEG quality |
| `Resolve.IsStudio` | absent from `CHANGELOG.md` altogether; present in 21.2's `.pyi` | the release it appeared in is unknown - §15.3 has the test that needs no such call |

So §14 needs **≥ 21.1.2** and was measured on 21.2.0.5. On 21.1.1 and older the
whole "list, apply, render" route is unavailable - which is the argument for
keeping the `.drp` fallback documented as a route rather than as a leftover.

### 15.3 Studio or free, without `IsStudio`

`GetProductName()` answered `DaVinci Resolve Studio` on the measured host
(`../evidence/2026-11-25-windows/resolve-poc.log`, step `connect`): the product
name carries the edition, and that test works on versions whose `.pyi` has no
`IsStudio`. It is what `tools/probe-env.py --connect` falls back to. `IsStudio()`
itself has never been answered by a host in this repository - it is `[I]`.

What the name does **not** decide: nothing here records what the free version
calls itself, so a product name without the word `Studio` answers `null`, not
`false` - the Resolve suite then reads `unknown` with that reason instead of
`blocked`, which would claim the machine cannot run it. Only a name that carries
the edition is evidence of the edition.

### 15.4 The objects a plug-in developer touches

`[V]` means a run in `tests/resolve/` called it and the log is in `evidence/`;
`[I]` means the signature is in the `.pyi` and this repository has not called it.

| Object | Call | Measured | Note |
|---|---|---|---|
| - | `scriptapp("Resolve")` | `[V]` | 18.2 s cold under `-nogui`; answers `None`, not an exception, when there is no host (§14.4) |
| `Resolve` | `GetProductName`, `GetVersionString` | `[V]` | the product name carries the edition (§15.3) |
| `Resolve` | `GetEffectsList(mediaType=None)` | `[V]` | 225 entries as `{category, id, name}`; `mediaType` is `"video"`, `"photo"` or `"audio"` |
| `Resolve` | `GetProjectManager` | `[V]` | |
| `ProjectManager` | `CreateProject`, `LoadProject`, `DeleteProject` | `[V]` | `CloseProject` first, or `DeleteProject` is refused (§14.9) |
| `Project` | `GetMediaPool`, `SetSettings` | `[V]` | a new project carries the host's 3840×2160 here (§14.6) |
| `Project` | `SetCurrentRenderFormatAndCodec("png", "RGB8")`, `AddRenderJob`, `StartRendering`, `IsRendering`, `GetRenderJobStatus` | `[V]` | §14.7 |
| `MediaPool` | `ImportMedia([str(path)])` | `[V]` | a list of absolute strings; a list of dicts imports 0 items |
| `MediaPool` | `CreateTimelineFromClips(name, [{"mediaPoolItem": item}])` | `[V]` | |
| `Timeline` | `GetItemListInTrack("video", 1)` | `[V]` | |
| `TimelineItem` | `AddEffect("ofx", id)` | `[V]` | answers an `Effect` or `None`; the id is the host's, not the bundle identifier |
| `TimelineItem` | `GetEffects("ofx")`, `DeleteEffects([effect])` | `[I]` | the removal path, unexercised here |
| `TimelineItem` | `ExportFrame(offset, options)` | `[V]` | one frame as a file or as base64 in 172 ms; the answer's size matches the PNG (§15.6) |
| `Effect` | `GetInfo()`, `GetIsEnabled`, `SetIsEnabled`, `GetIndex`, `SetIndex` | `[I]` | `GetInfo()` is `{category, id, name}` - no parameter list |

### 15.5 What the API cannot do

| Want | Answer |
|---|---|
| set or read an OFX parameter | nothing. `Effect` has six methods (§14.10), there is no `SetParameter` in 21.2's `.pyi`, and `FusionComp` is declared as `class FusionComp: ...` - a bare stub without methods, so the Fusion route does not exist either. The lever that does work is the **default**: rebuild with `-DREFERENCE_GAIN_DEFAULT=2.0` and the host cannot skip `render()` (§14.10) |
| read a frame's pixels | no. Pixels leave only as a file or an encoded string, through a render job or `ExportFrame` |
| learn which backend the plug-in took | no. Not in the API, and Resolve logs nothing a plug-in prints through `OFX::Log::print`; the plug-in has to declare it, which the CUDA template does through `$MCOFX_PATH_LOG` (§8, §14.13) |
| learn the OFX identifier from the host | no. `GetEffectsList` answers the host's id; the bundle's own id is in its `OFX` support XML |
| set a parameter with a click instead | no. The scripting API has no UI automation |

### 15.6 The cheaper frame: `ExportFrame`, measured on 21.2

`TimelineItem.ExportFrame(frameOffset, exportFrameOptions)` renders one frame
without a render job, a `TargetDir` or a job id - which is exactly where §14.7's
two traps live. Both documented forms ran on 2026-11-26 against the project
`poc.py --build-project` makes: `tests/resolve/export_frame.py` installed the
bundle, built the project, rendered one frame through a job, then exported that
same frame twice. **[V]**

| Form | The answer | What it produced | Time |
|---|---|---|---|
| `{"outputType": "file", "filePath": <png>}` | `{"success": True, "width": 1920, "height": 1080}` | a 136,433-byte PNG on disk | 172 ms |
| `{"outputType": "encodedString", "encodedImageFormat": "png"}` | the same three keys plus `data`, 181,912 base64 characters | 136,433 bytes, `\x89PNG` magic, decodes with ffmpeg | 172 ms |

What the numbers settle:

- **No `error` key on success.** The `.pyi` declares it optional and the host
  leaves it out, so a caller must not read `result["error"]` after a pass.
- **`width`/`height` are the exported frame's size.** Both forms said
  `1920x1080`, both PNGs decoded to `1920x1080`, and the render job's
  `poc00108000.png` was `1920x1080` as well: the answer agrees with the bytes.
- **`data` is base64 of a PNG.** 181,912 characters is exactly the base64 length
  of 136,433 bytes, `base64.b64decode(..., validate=True)` accepted it, and the
  decoded file starts with `\x89PNG`. The two PNGs even share their md5, so the
  "no temp file" form is the same file the `file` form writes.
- **The pixels are the render job's.** `file vs job`, `encoded vs job` and
  `encoded vs file` all read `mae 0.0`, `max 0` over 8,294,400 bytes, so a
  validation loop can swap the job for `ExportFrame` without changing what it
  measures.
- **It is cheaper here.** 172 ms per export against 1.19 s of wall time for the
  job, whose own `TimeTakenToRenderInMs` was 738.

Log: `../evidence/2026-11-26-windows/resolve-export-frame.log`; the JSON report
is `tests/resolve/reports/export-frame-<timestamp>.json`.

Not exercised: `maxDimension`, `quality` and the `roi*` options, JPEG output, and
any frame other than frame 0 of a one-clip timeline. The export renders inside
the host, so what comes back is the host's 8-bit PNG conversion, not the
plug-in's own buffer.

### 15.7 Where to look next

- `tests/resolve/resolve_api.py` - the calls as functions, each docstring carrying
  what the measurement taught.
- `tests/resolve/export_frame.py` - the `ExportFrame` measurement, with its
  report and the numbers §15.6 quotes.
- §14 above - the run itself, with the numbers and the traps.
- `tests/resolve/README.md` - how to repeat it, and what it does not prove.
- `references/host-resolve.md` - what the **plug-in** may assume about the host,
  as opposed to what a **script** can make the host do.

## Provenance labels

**[V]** verified in this repository — a command ran here and its output is in
`evidence/`. **[I]** inferred from a header or a document, not demonstrated.
**[B]** external evidence: Blackmagic's own README and release notes, or a
third-party project that measured it. §14 is **[V]** (the 2026-11-25 run); the
rest of this document is external or inferred, and §12 says which parts are
still inferences.
