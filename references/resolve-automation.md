# Driving Resolve as a test host: what the API can do, and the first milestone

**Status: research only (2026-11-25).** Nothing in this document has been run
here, no code was added, no dependency was introduced and no build changed. There
is no Resolve on either machine this repository has been exercised on, so every
API statement below is graded **[B]** (external documentation) or **[I]** (read
out of a document and inferred), and none of it is **[V]**. The point of §9 is to
turn the first row of that list into a measurement.

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
| Insert an OFX effect | *nothing documented* | **[I]** | the API surface in the shipped README has no effect-insert call |
| Set an OFX parameter | *nothing documented* | **[I]** | same: parameters of a plug-in are not exposed |
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

## 11. Implementation references (none of these has been run)

The sketches below are the task's prototypes, trimmed to the calls that matter
and annotated with what has to be settled before each one is trusted. They are
references, not code in the tree: nothing imports them, nothing runs them, and
`tools/check-docs.py` does not see them.

**A. Process management.** Start the executable with `-nogui`, then poll for the
API rather than for the process:

```python
proc = subprocess.Popen([exe, "-nogui"], cwd=str(Path(exe).parent))
# a started process is not a started application: wait for scriptapp("Resolve")
```

Open questions: does `-nogui` survive a missing display on Windows; does Resolve
exit on its own after `Quit()`; how do you avoid killing a Resolve the user
already had open (answer: record the PID you started, never `taskkill /IM`).

**B. Connection.** The module path comes from the environment, never from a
literal:

```python
sys.path.insert(0, os.path.join(os.environ["RESOLVE_SCRIPT_API"], "Modules"))
import DaVinciResolveScript as dvr
resolve = dvr.scriptapp("Resolve")   # None until a project is open
```

**C. Render.** Job id in, status dict out, and the status may be a string:

```python
job = project.AddRenderJob()                 # "" on failure
project.StartRendering([job], isInteractiveMode=False)
status = project.GetRenderJobStatus(job)     # dict or error string
```

Open questions: whether `isInteractiveMode` changes anything headless; whether
`JobStatus` can sit at `Complete` before the file is flushed.

**D. Prepared projects.** One `.drp` per test case, committed, with the plug-in
applied and the range set. Generated once by hand on a known Resolve version,
never edited by the harness.

**E. Integrity.** Decode, then: dimensions, channels, finiteness, constant-frame
policy, frame count, file size.

**F. Comparison.** MAE and max error first (the repository's contract already
uses exactly that pair), RMSE/PSNR/SSIM only if a case needs them, tolerances per
reference as in `testkit/contract_main.cpp` (`name.<fuzz>.fuzz`), never a global
one.

**G. CPU vs GPU.** Two runs, two declared backends, one comparison — and the
backend declaration comes from the plug-in (§8).

**H. One entry point.** A PowerShell script that validates paths, sets the env
vars, runs the orchestrator and returns its exit code. It must also clean up:
a `finally` that quits Resolve and, if the quit fails, terminates the PID it
started.

## 12. What this document does not know

1. Whether Resolve's API can list installed OFX plug-ins at all. If it can, step 2
   of the milestone becomes an assertion instead of an inference.
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

## Provenance labels

**[V]** verified in this repository — a command ran here and its output is in
`evidence/`. **[I]** inferred from a header or a document, not demonstrated.
**[B]** external evidence: Blackmagic's own README and release notes, or a
third-party project that measured it. This document contains no **[V]** — every
statement in it is external or inferred, and the milestone in §9 is what would
produce the first.
