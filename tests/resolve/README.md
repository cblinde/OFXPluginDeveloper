# tests/resolve - one frame through DaVinci Resolve

The milestone from [`references/resolve-automation.md`](../../references/resolve-automation.md)
§9 and backlog row 23, as a scaffold: install the bundle, launch Resolve with
`-nogui`, connect through the scripting API, load a prepared project, render one
frame, decode it, check the pixels, quit, write a JSON report.

**Status: the scaffold runs, the host path does not.** There is no Resolve, no
Studio licence and no GPU on the machines this repository has been exercised on,
so every step from `install` onwards has never executed. `resolve_api.py` carries
`# NOT RUN` on each call into `DaVinciResolveScript`, and `--plan` records those
steps as `not-run` rather than pretending.

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

## What needs a machine with Resolve Studio

```bash
python tests/resolve/poc.py \
    --bundle build/bundle/Name.ofx.bundle \
    --project projects/basic_ofx_test.drp \
    --resolve "/c/Program Files/Blackmagic Design/DaVinci Resolve/Resolve.exe" \
    --ofx-dir "/c/ProgramData/Blackmagic Design/DaVinci Resolve/Support/OFX/Plugins" \
    --out build/resolve/out --expect 1920x1080 \
    --reference media/reference/basic_ofx_test_0001.png
```

Requirements, none of which this repository can check: Resolve **Studio** (the
free version does not expose external Python scripting since 21.1), the
`Local` external-scripting preference, `RESOLVE_SCRIPT_API` /
`RESOLVE_SCRIPT_LIB` / `PYTHONPATH` set, a prepared `.drp` with the plug-in
already applied, and an NVIDIA GPU in an interactive session.

## What "pass" means

A start is not a pass and a `Complete` render job is not a pass. The report says
`pass` only when a decoded frame has the expected size, is not a uniform frame, is
not black, and - if `--reference` was given - matches the reference within the
bound. The reference comparison is in 0..255 units because the decoder reads
8-bit RGBA; the contract harness's float comparison in `testkit/contract_main.cpp`
remains the stricter instrument and this scaffold does not replace it.

## What is not here yet

- The `.drp` projects and the reference images: they need a Resolve licence to
  produce, so they are not committed.
- HTML and JUnit reports, the CPU/GPU comparison and the test matrix: that is
  backlog row 24, and it waits for row 23 to pass on a real machine.
- Anything that would select a render backend: Resolve's GPU mode is a user
  preference, so the plug-in has to declare which path it took and the harness has
  to read that declaration back.
