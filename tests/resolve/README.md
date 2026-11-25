# tests/resolve - one frame through DaVinci Resolve

The milestone from [`references/resolve-automation.md`](../../references/resolve-automation.md)
§9 and backlog row 23: install the bundle, launch Resolve with `-nogui`, connect
through the scripting API, build a project, render one frame, decode it, check the
pixels, quit, write a JSON report.

**Status: the host path ran, on 2026-11-25.** DaVinci Resolve Studio 21.2.0.5 on
Windows 11 with an RTX 3060: install, launch, connect, build the project through
the API, apply the OFX effect, render one 1920×1080 PNG, decode it, check it,
quit - seven steps, exit code 0. The log is
[`evidence/2026-11-25-windows/resolve-poc.log`](../../evidence/2026-11-25-windows/resolve-poc.log).
The `.drp` the design assumed turned out not to be necessary: the scripting API
can import a still, cut a timeline and attach an OFX effect itself. What is still
not proved is one paragraph down, and it matters more than the pass.

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

`checks.py` is pure Python on bytes and has nine cases of its own; the self-test
fails when the checks stop catching a uniform frame, a black frame, a wrong size,
a wrong channel count or a reference that differs by one pixel.

`test_stale_file.py` covers the rule `output_files.py` owns: a render is credited
with the file that appeared after the directory was snapshotted, so a second run
in the same directory that writes nothing is a failure rather than a pass on the
first run's output. It stands in for the host by writing files; nothing about the
OFX path is simulated.

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

`--project some.drp` still loads a prepared project instead. It is the mode the
design assumed and it has **never run**, because no `.drp` has been produced here.

## What a pass does not mean

The rendered frame was **pixel-identical to the source** (`mae 0.0`, `max 0`), and
that is the expected result rather than a proof: this plug-in reports
`isIdentity()` at its default gain of 1.0, so the host is entitled to skip
`render()` entirely, and nothing in the scripting API can set an OFX parameter -
`Effect` exposes `GetInfo`, `GetIsEnabled`, `SetIsEnabled`, `GetFusionComp`,
`GetIndex` and `SetIndex`, and no parameter access at all.

So the run proves the host side: the bundle is scanned
(`OFX: loading com.ofxplugindeveloper.referencegain` in Resolve's own log), listed
in `GetEffectsList()`, instantiated and attached with `AddEffect`, and a queued
job reaches `Complete` and leaves a decodable 1920×1080 frame. It does **not**
prove that this plug-in's pixel maths ran inside Resolve; the harness in
`testkit/` remains the instrument for that, and a host-side proof needs either a
project with a non-default parameter (one GUI step) or an effect whose default is
not identity (backlog row 23 carries this).

## What is not here yet

- The `.drp` projects and hand-made reference images: one still is enough for the
  built project, so they are no longer on the critical path, but a project that
  exercises a non-default parameter needs one.
- HTML and JUnit reports, the CPU/GPU comparison and the test matrix: that is
  backlog row 24, and it waits for row 23 to prove the pixel path.
- Anything that would select a render backend: Resolve's GPU mode is a user
  preference, so the plug-in has to declare which path it took and the harness has
  to read that declaration back.
