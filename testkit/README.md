# testkit: a small OFX host for contract tests

`contract_test` loads a built `.ofx` with `dlopen`, resolves
`OfxGetNumberOfPlugins`/`OfxGetPlugin`, checks the `OfxPlugin` struct, and then
drives the plug-in through a hand-written host: `describe`,
`describeInContext`, `createInstance`, `isIdentity`, `render`,
`destroyInstance`, `unload` — twice, in one process. That second cycle is what
catches a runtime-owned static: its destructor runs at unload and calls into a
module that is already half gone, so the run hangs or dies *after* the last check
passed (the rule is [`SKILL.md`](../SKILL.md) §1, rule 13, and `[stage] unload`
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

## Files

| File | What it is |
|---|---|
| `fake_host.cpp/.h` | the host: property, image-effect, parameter, multithread, memory and message suites, one plug-in descriptor, any number of effect instances with their own parameter values, clips, and per-render source/destination images |
| `contract_main.cpp` | the checks and the command line (`.ofx` path + what the plug-in is expected to declare) |
| `build-contract-test.sh` | compiles the two files into one binary, finding the OFX headers from the template's CMake cache |
| `build-contract-test.bat` | the same build under MSVC; the loader path in `contract_main.cpp` is `LoadLibraryExW` there |
| `check-ofx-artifact.bat` | Windows counterpart of the `nm`/`readelf` checks: `dumpbin /exports` must show exactly the two entry points, `cuobjdump -lelf` the cubins. It finds both tools itself (PATH, `VCToolsInstallDir`, vswhere, `CUDA_PATH`); every `build.bat` calls it with `--strict`, so a tool it cannot find fails the build instead of skipping |
| `negative-controls/` | two deliberately broken plug-ins plus one ROI stub built seven ways (plain or internally tiled with the context read or ignored, two for the label check — one that never writes the label, one that does — and one that draws a mark at a fixed position), so the harness is shown to fail when it should and to pass a plug-in that follows the pattern |
| `run-negative-controls.sh` | builds those twelve controls, asserts each fault is rejected with the right message, and that the context-reading and label-writing builds pass and the mark build passes in both bottom-up layouts while failing in both top-down ones |

## Running it

The templates call it from their own `test/run-tests.sh`; from the repository
root, `./run-tests.sh` runs everything. To point it at one plug-in by hand:

```sh
testkit/build-contract-test.sh /tmp/contract_test .ofx-ref
/tmp/contract_test path/to/MyPlugin.ofx \
  --id com.example.myplugin \
  --param gain:double --param gainAlpha:double \
  --clip Source --clip Output \
  --identity --set gain=2.0 --not-identity-after-set --byte-dst-rejected
```

Flags: `--param name:type` (repeatable; `type` is one of `double`, `int`
(or `integer`), `string`, `bool` (or `boolean`), `rgb`, `rgba`, `choice`),
`--clip name` (repeatable),
`--set name=value` before render (`rgb` values are `r,g,b`, `rgba` values
`r,g,b,a`), `--identity` (expect `kOfxStatOK` and the
source clip), `--not-identity-after-set`, `--byte-dst-rejected` (the plug-in must
answer `kOfxStatErrUnsupported` for an 8-bit destination), `--negative-pitch`,
`--image-origin top|bottom`, `--mark X,Y` (a mark the plug-in draws at a frame
position; asserted in frame coordinates, so a mirrored one fails with the row it
landed on; full frame only, since an inset destination moves the row origin),
`--src`/`--golden`/`--size WxH` for a golden-image render comparison,
`--set-status-only` when the parameter is not a gain, and `--label-param NAME`
for a string the plug-in sets itself (a read-only label): read after the first
render, the value must be **written by the plug-in** and non-empty, and the
check's name carries it — `label parameter 'provider' = "CPU (fallback: ...)"` —
which is how a suite asserts what the plug-in wrote without a second output
channel. A declared default does not count: it is what a label that was never
set shows, so `FakeHost::paramIsSet()` is part of the check rather than a
non-empty string being enough.

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
window were read, both renders would agree.

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
| Bottom + negative | top-first | row of `y1` | − | yes |
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
[`SKILL.md`](../SKILL.md) §7 can be
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
4 × 3 into a multi-minute run with no output in between: pass
`--concurrency-threads 0` there and exercise concurrency against a fast model.

## Assumptions the checks encode

Written down because each one is a way for a green run to mean less than it looks:

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

`testkit/run-negative-controls.sh` proves the harness is not a rubber stamp. It
is deliberately not part of `./run-tests.sh`: a normal run has to stay green.

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

## What this host cannot prove

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
