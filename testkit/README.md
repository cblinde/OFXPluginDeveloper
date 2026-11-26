# testkit: a small OFX host for contract tests

`contract_test` loads a built `.ofx` with `dlopen`, resolves
`OfxGetNumberOfPlugins`/`OfxGetPlugin`, checks the `OfxPlugin` struct, and then
drives the plug-in through a hand-written host: `describe`,
`describeInContext`, `createInstance`, `isIdentity`, `render`,
`destroyInstance`, `unload` — twice, in one process. That second cycle is what
catches a runtime-owned static: its destructor runs at unload and calls into a
module that is already half gone, so the run hangs or dies *after* the last check
passed (the rule is [`SKILL.md`](../SKILL.md) §1, rule 11, and `[stage] unload`
on stderr says where it happened). Every phase prints such a line on **stderr** —
`render`, `mark`, `compare golden`, `compare parameter`, `concurrency`,
`byte destination`, `roi region`, `roi context`, `unload` — so a run that stops
answering names the phase it never finished, and stdout stays the check stream a
script compares. It also holds two instances
at once with different parameter values, and renders one thread per instance (or
several threads on one instance, when the plug-in declares `eRenderFullySafe`) to
check the declared render thread safety instead of trusting it.

It is shared by the templates. None of them has its own copy, and nothing
here links the plug-in: the binary under test is whatever `build.sh` produced.

## 1. Files

| File | What it is |
|---|---|
| `fake_host.cpp/.h` | the host: property, image-effect, parameter, multithread, memory and message suites, one plug-in descriptor, any number of effect instances with their own parameter values, clips, and per-render source/destination images |
| `contract_main.cpp` | the checks and the command line (`.ofx` path + what the plug-in is expected to declare) |
| `build-contract-test.sh` | compiles the two files into one binary, finding the OFX headers from the template's CMake cache |
| `build-contract-test.bat` | the same build under MSVC; the loader path in `contract_main.cpp` is `LoadLibraryExW` there |
| `check-ofx-artifact.bat` | Windows counterpart of the `nm`/`readelf` checks: `dumpbin /exports` must show exactly the two entry points, `cuobjdump -lelf` the cubins. It finds both tools itself (PATH, `VCToolsInstallDir`, vswhere, `CUDA_PATH`); every `build.bat` calls it with `--strict`, so a tool it cannot find fails the build instead of skipping. It counts lines with `set /a` and matches export rows on the eight-digit RVA column, never with the legacy `find` or a loose `[0-9A-Fa-f]*`, which Git-for-Windows' GNU `find` and dumpbin's `00000000 characteristics` header respectively turn into a hung build and a miscount ([`../references/cmake-and-linking.md`](../references/cmake-and-linking.md) §5) |
| `negative-controls/` | two deliberately broken plug-ins plus one ROI stub built ten ways (plain or internally tiled with the context read or ignored, two for the label check — one that never writes the label, one that does — one that draws a mark at a fixed position, and two that echo the source clip's `mediaID` label, one re-reading it per render and one caching the first answer, and one that declares no context region at all), so the harness is shown to fail when it should and to pass a plug-in that follows the pattern |
| `run-negative-controls.sh` | builds those fifteen controls, asserts each fault is rejected with the right message, and that the context-reading, label-writing and clip-re-reading builds pass while the mark build passes only in both bottom-up layouts and the clip-caching build fails on the second media id |
| `selftest-compare.sh` / `.bat` | the comparator's own test: `contract_test --selftest-compare` runs the truth table of the golden comparison (identical, sub-LSB, over the bound, size mismatch, NaN, infinity, a reference corrupt at one pixel) plus the reference lookup (platform variant, `.fuzz` sidecar, CLI override). A comparison that cannot fail is not a test, so this one is shown failing on each defect it exists to catch |

### Host profiles, references and thread counts

`--host-profile nuke|resolve|natron` decides what shape the fake host presents
(tiles, multi-resolution, temporal access, `isBackground`); a suite runs the
contract once per profile because a process holds one host. `--golden
<file>` resolves to `<name>-<platform><ext>` first and `<name><ext>` second, and
`--fuzz cpu|cuda` reads the bound from `<name>.<fuzz>.fuzz` - so a reference
carries its own tolerance instead of a global flag. `--concurrency-threads N`
is what `SUITE_THREADS=random` varies per run, 1-4, to give a race a chance to
show up in some run rather than the one a developer fixed.

## 2. Two-tier host verification

Do not answer "does a host accept this bundle?" with a mock alone. A hand-written
host can omit a mandatory suite or skip an action and still pass. These two tiers
close that gap, and `verify-bundle.bat` runs both:

```bat
.\testkit\verify-bundle.bat <build-dir-or-bundle> --plugin-id <id> [--natron <dir>] [--gain 0.5]
.\testkit\verify-bundle.bat <build> --plugin-id <id> --tier1-only      (CI without Natron)
.\testkit\verify-bundle.bat <build> --plugin-id <id> --effect "Reference Gain" --resolve   (Resolve, closed)
```

| Tier | What runs | What it settles |
| --- | --- | --- |
| 0 | layout assertion | `Contents\Win64\<Name>.ofx` with `<Name>` equal to the bundle stem, before anything executes |
| 1 | `testkit/official-host` — the OpenFX project's own `HostSupport` library and headless `cacheDemo`, built unmodified | the plug-in loads, enumerates and describes under a host that is not ours: id, contexts, render safety, supported depths |
| 2 | `testkit/tier2-natron.py` — `NatronRenderer`, one 256×256 frame, PNG in and out | a real host instantiates the effect and renders it, and the pixels come out right (gain applied once, in linear space, alpha untouched) |
| 3 | `tests/resolve/poc.py --build-project` — DaVinci Resolve Studio, one generated 1920×1080 still, project and timeline built through the scripting API | the GUI product this most likely ships into loads the bundle, applies the effect and renders a frame; the host's own compute-API line goes into the report, so an Auto session and an explicit CUDA session are told apart rather than assumed |

Tier 3 is opt-in (`--resolve`), because it needs an installed host, an interpreter
with the `DaVinciResolveScript` module on its path, and the host **closed**: Resolve
holds scripting port 15000, and a run would otherwise measure an open session and
quit it. The tier refuses to start when `Resolve.exe` is running, finds the
installer under `Program Files\Blackmagic Design\DaVinci Resolve` on any fixed
drive (the registry holds only a `Version`), and generates its still with ffmpeg so
a clean checkout can run it. `--effect <name>` is the OFX menu name, not the bundle
name - `Reference Gain`, not `ReferenceGain`.

The contract harness in this directory stays: it is the layer that can drive
single actions, odd pitches and rejected parameters far more precisely than any
host. The tiers above are what stops that precision from hiding a host-facing bug.

### What these tiers cost, measured

* **Tier 1 is not 0.1 s on a working machine.** `PluginCache` appends the system
  plugin directory after `OFX_PLUGIN_PATH` (`ofxhPluginCache.cpp:240`-`245`), so it
  describes all 84 plug-ins installed here and takes **23 s**. Match on the
  plug-in id; never assume the output contains only your plug-in.
* **Tier 2 is ~3.3 s** per run at 256×256 with one frame (Natron start-up
  dominates). Two runs (plug-in and baseline) per invocation.

### Natron and NatronRenderer pitfalls, all measured on 2.5.0

| Symptom | Cause | What the harness does instead |
| --- | --- | --- |
| segfault after `Write1 ==> Rendering started` | Natron's bundled `ReadPFM`/`WritePFM` pair crashes here; the same graph with PNG renders | uses `ReadPNG`/`WritePNG` |
| segfault inside `app1.render(...)` from a script | the API call itself fails this way — the no-plug-in baseline crashes identically | builds the graph, `saveProjectAs`, then renders the `.ntp` file |
| renders 250 frames | `NatronRenderer project.ntp` defaults to the project range | passes the frame argument: `NatronRenderer project.ntp 1` |
| hangs forever, "loading in command-line interpreter mode" | `-t script.py` enters an interpreter that waits on stdin | passes the script positionally |
| "Cannot load frame" for a file that exists | media paths inside a project resolve against the project's directory | writes absolute paths into the project |
| output is `(44,92,140)` where `(32,64,96)` was expected | Natron converts sRGB→linear on read and back on write | compares `srgb8(gain * linear(input))`, tolerance 2/255 |
| "Python was not found" from the batch | `where python` returns the Microsoft Store alias stub | each candidate must answer `python -c "import sys"` |

A plug-in that advertises `kOfxImageEffectPropCudaRenderSupported` while the host
never sets `kOfxImageEffectPropCudaEnabled` is exactly the case Tier 2 covers:
Natron renders on the CPU, so the plug-in's CPU path is what a wrong
`isEnabledCudaRender` branch would break. That is a fallback test, not a GPU test —
only `test/gpu-kernel-test.cu` runs device code.

## 3. Running it

The templates call it from their own `test/run-tests.sh`; from the repository
root, `./run-tests.sh` runs everything. To point it at one plug-in by hand:

```sh
testkit/build-contract-test.sh /tmp/contract_test .ofx-ref
/tmp/contract_test path/to/MyPlugin.ofx \
  --id com.example.myplugin \
  --param gain:double --param gainAlpha:double \
  --clip Source --clip Output \
  --identity --set gain=2.0 --not-identity-after-set --depth half --depth byte
```

Flags: `--param name:type` (repeatable; `type` is one of `double`, `int`
(or `integer`), `string`, `bool` (or `boolean`), `rgb`, `rgba`, `choice` — any
other spelling exits 2 and lists these, instead of failing later as a descriptor
mismatch that reads like a plug-in defect),
`--clip name` (repeatable),
`--set name=value` before render (`rgb` values are `r,g,b`, `rgba` values
`r,g,b,a`), `--identity` (expect `kOfxStatOK` and the
source clip), `--not-identity-after-set`, `--byte-dst-rejected` (the plug-in must
answer `kOfxStatErrUnsupported` for an 8-bit destination), `--depth float|half|byte`
(repeatable: source and destination at that depth, and the frame must be the source
times the gain quantised to it — for a plug-in that declares the depth; use
`--byte-dst-rejected` for one that does not), `--negative-pitch`,
`--image-origin top|bottom`, `--mark X,Y` (a mark the plug-in draws at a frame
position; asserted in frame coordinates, so a mirrored one fails with the row it
landed on; full frame only, since an inset destination moves the row origin),
`--src`/`--golden`/`--size WxH` for a golden-image render comparison,
`--set-status-only` when the parameter is not a gain, and `--label-param
NAME[=VALUE]` for a string the plug-in sets itself (a read-only label): read
after the first render, the value must be **written by the plug-in** and
non-empty, and the check's name carries it — `label parameter 'provider' =
"CPU (1 thread) - fallback: ..."` — which is how a suite asserts what the plug-in wrote
without a second output channel. A declared default does not count: it is what a
label that was never set shows, so `FakeHost::paramIsSet()` is part of the check
rather than a non-empty string being enough. With `=VALUE` the value must also
match, which turns the label into a readback of something the plug-in fetched
rather than only a proof that it wrote at all.

`--clip-prop NAME=VALUE` sets a string property on the source clip's property
set — the way a host names the media a clip currently carries — and reads it back
through the property suite (`FakeHost::setClipProperty()` /
`FakeHost::clipProperty()`). `--clip-prop-change NAME=VALUE` writes the same
property again after the first render and renders once more on the **same clip
handle**, then requires the label named by `--label-param` to carry the new
value: a plug-in that keys a cache on the clip handle and never re-reads the
property fails with the value it kept. The two together are how a media change
under a stable handle is tested — `mediaID` is the name the controls use; a real
host picks its own.

`--roi-expand N` asks `kOfxImageEffectActionGetRegionsOfInterest` twice — for a
region inside the frame and for the frame itself — and requires the source RoI to
be the requested region grown by `N`, clamped to the source region of definition.
`FakeHost::getRegionsOfInterest(time, requested, &sourceRoi)` is the host call
behind it; the answer is read from the outArgs property Support writes per clip
(`OfxImageClipPropRoI_<clip>`), so a plug-in that reports nothing answers
`kOfxStatReplyDefault` and fails the check by name instead of passing silently.
The plug-in's own context parameter has to match `N` (e.g. `--set halo=2
--roi-expand 2`).

`--roi-context` checks the pixels rather than the rectangle. It renders the whole
frame, then a window inset by three pixels, and requires the overlap to be
bit-identical to the whole-frame render; then it renders that window again
against a host that ignores the declaration, and requires the pixels to differ.
The second render is what gives the first one teeth: if nothing outside the
window were read, both renders would agree. That second half is a property of a
*local* effect, so it is skipped - and the skip is named in the check stream -
when the plug-in answers `GetRegionsOfInterest` with exactly the region it was
asked about: a global effect that reads only its window would otherwise fail a
check about a context it never declared. A plug-in that leaves the action at
`kOfxStatReplyDefault` is still checked, because "no answer" is not "no
context".

`--roi-context-tolerance T` is the same pair with a bound instead of
bit-equality, for a render the plug-in splits into tiles: the window must stay
within `T` of the whole-frame render when the context is available, and must
leave `T` when it is not. Use it with a small `tileSize` to exercise the tiled
path; pick `T` from measurement, below the no-context deviation and above any
feather rounding (the ONNX template uses 0.02 against a measured 0.1875).

Both flags are one measurement, not four checks: `maxRectDifference()` returns
whether the overlap is bit-identical, the largest absolute difference and the
pixel it sits at, and the bound is read in exactly one place — `T == 0` means
"identical to the bit", so the exact flag is the tolerance flag with a bound of
zero. Equality is compared on the bits rather than with `==`, which keeps a
signed zero distinct and a NaN identical to itself.

`FakeHost::setSourceCroppedToWindow(true)` is the host side of that second
render: the source image is published cropped to the window, and its origin and
size come from the window the host asked for, so a plug-in that reads past its
bounds reads pixels the host never supplied.

`--src-bounds-inset N` publishes a source region of definition inset by `N`
inside its buffer, `--dst-bounds-inset N` the same for the destination. Each one
adds a baseline render, a render the plug-in has to reject with
`kOfxStatErrValue`, and a pixel comparison that proves the rejected render wrote
nothing. Reading outside the source bounds and writing outside the destination
bounds both stay inside the allocation on purpose (the destination gains a
margin in that mode), so the failure is `first difference at float N` rather
than a crash: the question is whether the plug-in touched pixels it was not
given, and a sanitizer build would only answer a different one.

`--set` itself goes through `paramSetValueAtTime`, the call Support's
`OFX::DoubleParam::setValueAtTime` makes, so the suite's argument convention is
exercised by every test that sets a value: doubles, ints, booleans and choices
are passed **by value**, strings as a pointer, RGB/RGBA as 3 or 4 doubles. A
harness that reads a `double*` there dereferences the value's bits and crashes on
the first write a plug-in makes.

A push button is clicked with `FakeHost::pressParam(name)`: it sends
`kOfxActionInstanceChanged` with `kOfxPropType = kOfxTypeParameter`,
`kOfxPropChangeReason = kOfxChangeUserEdited`, the parameter name, the time and
the render scale `InstanceChangedArgs` reads. Only `kOfxStatFailed` and
`kOfxStatErrBadHandle` count as a rejection; `kOfxStatReplyDefault` is how a
notification is normally answered. `FakeHost::paramValue(name)` reads back what
the plug-in wrote (doubles as `%g`, ints and choices as the index, strings
verbatim, colours as `r,g,b[,a]`), which is how a test asserts that a button
handler stored something.

A parameter type the harness cannot model is never silent: the fake host records
`fake host: read of parameter 'boxColor' (OfxParamTypeRGB) is not modelled` in
its messages, so a failed action names the parameter instead of looking like a
plug-in defect. A choice parameter's declared list is readable through
`FakeHost::paramOptions(name)`, so a test can assert that a dropdown offers the
options it claims.

Image layout is two separate axes. `--negative-pitch` chooses the stride sign;
`--image-origin top|bottom` chooses which row `kOfxImagePropData` addresses
(`FakeHost::setImageOrigin`/`setNegativePitch` set them independently). All four
combinations are buildable and in-buffer:

| Layout | memory | `img->data` | stride | a real host? |
| --- | --- | --- | --- | --- |
| Bottom + positive (**default**) | bottom-first | row of `y1` | + | yes — Resolve |
| Bottom + negative | top-first | row of `y1` | − | yes for CPU images; `ofxImageEffect.h` line 1350 excludes CUDA/Metal/OpenCL buffers |
| Top + positive | top-first | row of `y2-1` | + | no: the pointer is the frame's top, `bounds` says `y1` |
| Top + negative | bottom-first | row of `y2-1` | − | no, same lie |

The two `Top` layouts are negative controls. A copy through the pointer passes
all four, and should: a pass-through is orientation-agnostic, so only a plug-in
that draws at chosen image positions exposes the difference. Assert where a mark
landed and which way up it is — comparing bytes will not catch a mirror.

One definition matters when you assert a window: `FakeHost::destination()` is the
host's own read-back and always walks frame rows upward from `bounds.y1`, whatever
origin and stride the plug-in's pointer had. For a `Top` origin that is *not* the
order the plug-in saw — the read-back is mirrored relative to the pointer handed
out. A sub-window test therefore has to un-swap the snapshot once before it can
say "nothing outside the window was touched"; the contract test never hits this,
because it renders full frames. `--mark X,Y` is written against this read-back
order on purpose, which is what makes the mirrored layout fail.

`run-negative-controls.sh` runs that as a control rather than describing it: the
`-DDRAW_MARK` build of the ROI stub draws one 8×8 mark through the destination's
bounds and pitch, and `--mark 24,8` reads it back from the host's own frame-order
view. Both bottom-up layouts pass; both top-down layouts fail with `the mark is at
the mirrored row`, which is the failure the four-layout table predicts.

The fake host serves message suite **v1 and v2**. `setPersistentMessage(...)` is
recorded as `[persistent:<type>] text (id)` and `clearPersistentMessage` as
`[persistent] cleared`, so the channel recommended in
[`SKILL.md`](../SKILL.md) §5 can be
exercised — Support throws a host-missing-suite exception when only v1 exists,
which is what a v1-only harness used to do.

Thread safety: `--concurrency-param name` names a numeric parameter (e.g. `gain`)
that is given a different value on each instance, so a frame produced with a
neighbour's value fails on the pixels, not on a crash; `--concurrency-threads N`
(default 4) and `--concurrency-iterations N` (default 3) size the load, and
`--concurrency-threads 0` turns the block off. A plug-in declaring
`eRenderUnsafe` gets a named skip; `eRenderInstanceSafe` renders different
instances on different threads; `eRenderFullySafe` renders the same instance on
all of them. A model slower than about a second per frame turns the default
4 × 3 into a multi-minute run with no output in between: measure it first with
`tools/bench_ep.py`, pass `--concurrency-threads 0` there, and exercise
concurrency against a fast model.

### Models for the channel and factor paths

The fixture model is RGB, shape-preserving and honest about its own transform,
which leaves three paths untested by it: one channel, four channels, and a model
that changes the size. `tools/make-testmodels.py` writes those without a
download, without a real model and without a GPU:

```bash
python3 tools/make-testmodels.py --out build/testmodels
```

It emits `id_1x_1ch.onnx`, `id_1x_3ch.onnx` and `id_1x_4ch.onnx` — a 1×1
`AveragePool`, so output equals input and a channel-order mistake becomes a
deviation instead of a green pass-through, with the fourth channel standing in
for alpha — and `up_2x_3ch.onnx`, a nearest `Resize`, so the output is exactly
twice the input in both axes. Each file is a real graph, not a `Constant`, and
each is loaded back through ONNX Runtime's CPU provider at two frame sizes, one
of them odd in both axes: a model with a hard-coded export size passes the first
and fails the second. `--no-self-test` writes the files without loading them.

Point the plug-in at `id_1x_3ch.onnx` (`ONNX_MODEL_PATH`; the ONNX template takes
no model-path parameter) and the check stream proves a pass-through:
with the same file as `--src` and `--golden`, `render matches the golden` is
exact. `--golden-tolerance T` turns that check into a bound (`render matches the
golden within T`), for a golden rendered by a different execution provider: a
CUDA session computes the same graph with different rounding, so bit-equality
would fail a correct pipeline. Default 0 keeps the exact check; the ONNX
template's suite retries with `1e-5` only after the exact comparison failed. `up_2x_3ch.onnx` is the fixture for the other behaviour — a runner that
only accepts shape-preserving models must refuse it, not quietly crop.

## 4. Assumptions the checks encode

Written down because each one is a way for a green run to mean less than it looks:

- **The check count follows the flags.** A run only checks what its arguments
  name, so two reports with different check counts can describe the same harness:
  compare the check *names* (which is what a control greps), and quote the flags
  with the number.
- **One numeric scale parameter.** `--concurrency-param` must name a parameter
  whose value multiplies RGB linearly (e.g. `gain`); the comparison is
  `dst == src × value` per instance, so an effect whose parameter changes
  geometry, a colour matrix or alpha separately cannot use this check as-is.
- **`--set` means "scale RGB, leave alpha" unless `--set-status-only` is
  passed.** The single-instance check asserts exactly that. `templates/minimal-ofx`
  multiplies *every* channel by `gain` — correct for that effect — so its suite
  passes no `--set` and relies on the contract's other checks; a plug-in that
  scales alpha fails that assertion for the right output and the wrong reason.
  For a parameter that is not a gain (a class filter, a font scale) use
  `--set-status-only`: it renders, asserts `kOfxStatOK` and that the write was
  accepted, and skips the `src × atof(value)` comparison. The pixels of a
  parameter that selects *where* something is drawn belong in a plug-in-specific
  test, not in this one.
- **Goldens are planar RGB float, compared exactly.** A plug-in allowed any
  tolerance needs its own comparison; this harness reports the worst deviation but
  fails on any non-zero one.
- **`eRenderFullySafe` is written but unexercised here.** No template declares it,
  so the same-instance branch is a code path with no in-repo evidence behind it.
  The worker threads also start together, which is a harder *simultaneity* test
  than a host's scheduler and a weaker *duration* test than a long-lived pool.

On Windows, `testkit/build-contract-test.bat <out.exe> <openfx-include-dir>`
compiles the same two files with MSVC, and the same flags apply. Every template's
`build.bat` calls `check-ofx-artifact.bat --strict` on the result, so the export
and device-code claims are checked on the platform too - and a missing `dumpbin`
fails the build rather than turning the check into a skip.

`testkit/run-negative-controls.sh` proves the harness is not a rubber stamp, and
`testkit/run-negative-controls.bat` is the same fifteen controls on Windows. The
`.bat` runs there. `./run-tests.sh` runs both, each on its own platform, as the
last stage before the summary, and skips the stage by name when there is no C
compiler. Both exit 0 only when every fault is caught, so neither turns a green
run red on purpose.

`--roi-context` gets a pair there rather than a single control, because one build
cannot prove it. The same stub (`negative-controls/stub_roi_context.c`) applies a
3×3 box filter and differs only in whether its taps are clamped to the published
source bounds or to the render window: with `-DIGNORE_CONTEXT` it must fail
`the inset window matches the whole frame bit for bit` at the window's own
border, and without that define the whole harness must pass. Both runs use a
generated, varying source, because over the default constant colour no box
filter can show a seam and the control would pass for the wrong reason.

`--roi-context-tolerance` gets the same pair with `-DTILED`, which splits the
window into 32-pixel cores and gives each one two pixels of context. The ignored
build then fails at a tile boundary *inside* the window (`max deviation 0.376 at
pixel (31,31)` against a 0.02 bound), and the reading build passes - so the
tolerance variant is covered without the ONNX template's model or bundle.

`--label-param` has its own pair, because "the plug-in wrote it" is a claim a
default can fake. `-DNO_LABEL` declares the `provider` label, gives it a default
and never writes it: the harness fails with `the plug-in never wrote it; what is
shown is the declared default`. `-DWITH_LABEL` writes the same label from
`render()` through the parameter suite, and the harness passes with
`label parameter 'provider' = "CPU (stub, written by render)"`. Together they
pin the predicate: the check is about the write, not about the parameter.

## 5. What this host cannot prove

1. **The multi-resolution and tile protocol.** It does not set
   `kOfxImageEffectPropTilesRoD` or slice render windows, so `setSupportsTiles(true)`
   paths are untested. The region-of-interest **action** is asked and checked
   (`--roi-expand`), and the pixels are checked both ways round (`--roi-context`:
   whole frame handed over vs. the source cropped to the window). What is
   untested is a host that slices one render into several windows, and the
   `kOfxImageEffectPropTilesRoD` protocol those slices travel with.
2. **Clip preferences and host scheduling.** `getClipPreferences` and per-clip
   depths/PARs are not exercised. `multiThread` does run real workers (up to
   four), so a pixel loop is split into bands and executed concurrently; that is
   a host-shaped thread pool, not a scheduler test. Concurrent renders of
   different instances *are* exercised; on one instance only for a plug-in that
   declares `eRenderFullySafe`. The workers start together, which is harder than
   a host's scheduler, but the harness does not prove the absence of races —
   build the plug-in with TSan for that.
3. **How a real host reports errors.** The message suite here only records
   strings; nothing checks that an error reaches a user, that a progress bar
   appears, or that abort/cancel works mid-render.

Also note (measured, not assumed): the OFX 1.5.1 Support library's
`unloadAction` dereferences `gEffectDescriptors.find(id)` without checking for
`end()`, so an unload for a plug-in that was never described crashes inside the
library. The harness always describes before unloading for that reason.
