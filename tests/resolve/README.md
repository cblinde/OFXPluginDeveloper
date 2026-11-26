# tests/resolve - one frame through DaVinci Resolve

The milestone from [`references/resolve-automation.md`](../../references/resolve-automation.md)
§9: install the bundle, launch Resolve with `-nogui`, connect
through the scripting API, build a project, render one frame, decode it, check the
pixels, quit, write a JSON report.

**Status: the host path ran, on 2026-11-25.** DaVinci Resolve Studio 21.2.0.5 on
Windows 11 with an RTX 3060: install, launch, connect, build the project through
the API, apply the OFX effect, render one 1920×1080 PNG, decode it, check it,
quit - seven steps, exit code 0. The log is
[`evidence/2026-11-25-windows/resolve-poc.log`](../../evidence/2026-11-25-windows/resolve-poc.log).
The `.drp` the design assumed turned out not to be necessary: the scripting API
can import a still, cut a timeline and attach an OFX effect itself. A second run
on the same day proved the **pixel path** as well: with the bundle built at a gain
of 2.0 the frame is the source times two to the byte
([`resolve-poc-gain.log`](../../evidence/2026-11-25-windows/resolve-poc-gain.log)).

The reusable form of all of it - where the host and the SDK live, the three
environment variables, launch and connect, every API call with its measured
caveat, and a table of the traps - is
[`references/resolve-automation.md`](../../references/resolve-automation.md) §14.

## What runs anywhere

```bash
python tests/resolve/poc.py --bundle build/bundle/Name.ofx.bundle --plan
python tests/resolve/poc.py --bundle build/bundle/Name.ofx.bundle --plan --expect 1920x1080
python tests/resolve/checks.py          # the pixel checks, with their self-test
python tests/resolve/test_stale_file.py # two runs in one directory, the second must fail
```

`--plan` validates everything that needs no host - the bundle layout
(`Contents/<arch>/*.ofx`), the project path, the reference, the OFX directory, a
decoder - prints one line per step and writes the same JSON report a real run
writes. Its exit code is 2 when a path is wrong.

`checks.py` is pure Python on bytes and has twelve cases of its own; the self-test
fails when the checks stop catching a uniform frame, a black frame, a wrong size,
a wrong channel count, a reference that differs by one pixel, or a frame that
skipped the render at a non-default gain.

`test_stale_file.py` covers the rule `output_files.py` owns: a render is credited
with the file that appeared after the directory was snapshotted **or changed
since**, so a second run in the same directory that writes nothing is a failure
rather than a pass on the first run's output, while a second run that overwrites
its own name counts as a write. It stands in for the host by writing files;
nothing about the OFX path is simulated.

## The run

```bash
# one still for the timeline; ffmpeg is already this harness's decoder
ffmpeg -v error -y -f lavfi -i "testsrc2=size=1920x1080:rate=24" -frames:v 1 build/resolve/source.png

python tests/resolve/poc.py \
    --bundle templates/minimal-ofx/build/bundle/ReferenceGain.ofx.bundle \
    --build-project --effect "Reference Gain" \
    --source build/resolve/source.png \
    --resolve "E:/Program Files/Blackmagic Design/DaVinci Resolve/Resolve.exe" \
    --ofx-dir "C:/Program Files/Common Files/OFX/Plugins" \
    --out build/resolve/frames --expect 1920x1080 \
    --reference build/resolve/source.png
```

`--expect-gain 2` is for a bundle built with `REFERENCE_GAIN_DEFAULT=2.0`: it
compares against the reference times two instead of the reference itself, which
is what turns the pixel path into a measurement.

`--build-project` is the mode that needs no `.drp`: the host makes the project,
imports the still, appends it to a timeline, applies the effect named on the
command line and closes up after itself. `--effect` is the effect's **menu name**,
not its OFX identifier - the host lists it as
`Open FX/OFX Plugin Developer/Reference Gain` and `AddEffect` wants that id.

Requirements measured on that machine: Resolve **Studio** (the free version does
not expose external Python scripting since 21.1), the `Local` external-scripting
preference, `RESOLVE_SCRIPT_API` or the documented `%PROGRAMDATA%` default, and a
GPU with a display attached. `RESOLVE_SCRIPT_LIB` can be omitted: it is derived
from the executable's directory, which is what makes an install on a drive other
than `C:` work at all.

`--project some.drp` loads a prepared project instead, and the path is now
measured up to the host's own answer, not past it: on 2026-11-26 a `.drp` was
exported and loaded, and `LoadProject` refused it until the path was absolute -
after which the two runs did not get a scriptable host again, so no render came out
of this mode yet. It needed four fixes first - an undefined `render_size`, a
`KeyError: 'resolve'` when `--resolve` was omitted, a `connect()` that waited for a
project the caller was about to load itself, and a relative path `LoadProject`
refuses. What a `.drp` cannot do is carry the effect: the export writes an empty
`<EffectFiltersBA/>`, so a prepared project with `modelPath` in it is a GUI artefact
([`resolve-project-route.log`](../../evidence/2026-11-26-windows/resolve-project-route.log)).

`ExportFrame` - one frame without a render job - has its own measurement.
`tests/resolve/export_frame.py` builds the same project, renders one frame
through a job and then exports that frame twice, as a file and as base64 in
the answer. Both forms answered `1920x1080` in 172 ms, the size of the job's
own PNG, and all three frames were identical to the byte
([`resolve-export-frame.log`](../../evidence/2026-11-26-windows/resolve-export-frame.log),
`../../references/resolve-automation.md` §15.6).

## What the two runs prove

**Run 1, default gain.** The rendered frame was pixel-identical to the source
(`mae 0.0`, `max 0`), and that is the expected result rather than a proof: this
plug-in reports `isIdentity()` at its default gain of 1.0, so the host is entitled
to skip `render()` entirely. What it does prove is the host side - the bundle is
scanned (`OFX: loading com.ofxplugindeveloper.referencegain` in Resolve's own
log), listed in `GetEffectsList()`, instantiated and attached with `AddEffect`,
and a queued job reaches `Complete` and leaves a decodable 1920×1080 frame.

**Run 2, gain 2.0.** The scripting API cannot set an OFX parameter - `Effect`
exposes `GetInfo`, `GetIsEnabled`, `SetIsEnabled`, `GetFusionComp`, `GetIndex`
and `SetIndex`, nothing else - but the *default* is a build constant. Rebuilt with
`-DREFERENCE_GAIN_DEFAULT=2.0`, the plug-in no longer reports identity, so the
host has to call `render()`:

```bash
REFERENCE_GAIN_DEFAULT=2.0 templates/minimal-ofx/build.sh
python tests/resolve/poc.py \
    --bundle templates/minimal-ofx/build/bundle/ReferenceGain.ofx.bundle \
    --build-project --effect "Reference Gain" \
    --source build/resolve/source.png \
    --reference build/resolve/source.png --expect-gain 2 \
    --resolve "E:/Program Files/Blackmagic Design/DaVinci Resolve/Resolve.exe" \
    --out build/resolve/frames-gain --expect 1920x1080
```

```
  check     pass     1920x1080 via ffmpeg, x2 mae 0.0 max 0
```

Every one of the 8,294,400 bytes equals `round(source x 2)` clamped at 255, so
this plug-in's `render()` ran inside Resolve, its float maths survived the host's
8-bit conversion, and the bright part of the test image exercised the clamp. Log:
[`evidence/2026-11-25-windows/resolve-poc-gain.log`](../../evidence/2026-11-25-windows/resolve-poc-gain.log).

`--expect-gain` compares against the reference times the gain instead of the
reference itself, so a frame that skipped the render is off by
`|gain - 1| x value` everywhere. `checks.py` proves that failure - `an undoubled
frame is caught by the gain check` - because a check that has never failed is a
check nobody has tested.

What neither run proves: **tiling or multi-resolution** (Resolve's profile asks
for neither), **parameter round-trips** (no accessor in the API), and the CUDA
template's stream handling - the GPU half is covered by run 3.

**Run 3, the CUDA template (2026-11-26).** The same recipe with
`templates/minimal-cuda-ofx`, built as `GAIN_DEFAULT=2.0 CUDA_ARCHS=86
templates/minimal-cuda-ofx/build.bat Release`, rendered `1920x1080` and checked
against the source times two: `mae 0.0`, `max 0`, `x2`. The CUDA template has a
CPU fallback that produces identical pixels, so the frame alone would not say
which path ran. `--path-log` answers that: it sets `$MCOFX_PATH_LOG` for the host
it launches (Resolve does not log what a plug-in prints through
`OFX::Log::print`), the plug-in appends one line per render to that file, and all
eight renders declared `CUDA` - seven full frames and one `208x117` window.
Log: [`resolve-poc-cuda-gain.log`](../../evidence/2026-11-26-windows/resolve-poc-cuda-gain.log),
script: `../../references/resolve-automation.md` §14.13.

## The failure path

A harness that has only ever succeeded is a harness nobody has tested, so the
same script can kill the host with a job in flight:

```bash
python tests/resolve/poc.py ... --kill-render-after 0.5 --timeout 240
```

The kill is issued from the render loop itself (`on_poll`), so it lands while the
job is rendering - a wall-clock timer outside the loop fired after a fast render
had already finished.

```
  kill      sent     pid 6792 with the render in flight
  error     fail     RuntimeError: the host exited (code 1) while render f152f638-... was in flight
  quit      pass     API quit, then the recorded PID
```

Exit code 1 after 47 s, no Resolve process left behind, report written
([`evidence/2026-11-25-windows/resolve-poc-kill.log`](../../evidence/2026-11-25-windows/resolve-poc-kill.log)).
`call_with_deadline` bounds the blocking calls, `_exit_code()` turns the
scripting layer's `'NoneType' object is not callable` into the host's exit code,
and cleanup is bounded too - the report records a failed close instead of
hanging on one.

Two bugs only this test could find, both state a killed run leaves behind: the
**project** (cleared by `new_project` on the next run) and the **partial frame**
(a second run into the same directory now counts an overwritten file as written).

## What is not here yet

- The `.drp` projects and hand-made reference images: one still is enough for the
  built project, and a non-default parameter no longer needs a project either
  (run 2 sets it at build time). A `.drp` would only add the GUI-authored case.
- HTML and JUnit reports, the CPU/GPU comparison and the test matrix: that is
  backlog row 24, and it no longer waits for anything: the host path, the pixel
  path and the kill path have all been measured.
- Anything that would select a render backend: Resolve's GPU mode is a user
  preference, so the plug-in has to declare which path it took and the harness has
  to read that declaration back.
