# testkit: a small OFX host for contract tests

`contract_test` loads a built `.ofx` with `dlopen`, resolves
`OfxGetNumberOfPlugins`/`OfxGetPlugin`, checks the `OfxPlugin` struct, and then
drives the plug-in through a hand-written host: `describe`,
`describeInContext`, `createInstance`, `isIdentity`, `render`,
`destroyInstance`, `unload` — twice, in one process. It also holds two instances
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
| `negative-controls/` | two deliberately broken plug-ins that prove the harness fails when it should |
| `run-negative-controls.sh` | builds those controls, asserts each is rejected with the right message |

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
`--set name=value` before render, `--identity` (expect `kOfxStatOK` and the
source clip), `--not-identity-after-set`, `--byte-dst-rejected` (the plug-in must
answer `kOfxStatErrUnsupported` for an 8-bit destination), `--negative-pitch`,
`--src`/`--golden`/`--size WxH` for a golden-image render comparison.

Thread safety: `--concurrency-param name` names a numeric parameter (e.g. `gain`)
that is given a different value on each instance, so a frame produced with a
neighbour's value fails on the pixels, not on a crash; `--concurrency-threads N`
(default 4) and `--concurrency-iterations N` (default 3) size the load, and
`--concurrency-threads 0` turns the block off. A plug-in declaring
`eRenderUnsafe` gets a named skip; `eRenderInstanceSafe` renders different
instances on different threads; `eRenderFullySafe` renders the same instance on
all of them.

## Assumptions the checks encode

Written down because each one is a way for a green run to mean less than it looks:

- **One numeric scale parameter.** `--concurrency-param` must name a parameter
  whose value multiplies RGB linearly (e.g. `gain`); the comparison is
  `dst == src × value` per instance, so an effect whose parameter changes
  geometry, a colour matrix or alpha separately cannot use this check as-is.
- **`--set` means "scale RGB, leave alpha".** The single-instance check asserts
  exactly that. `templates/minimal-ofx` multiplies *every* channel by `gain` —
  correct for that effect — so its suite passes no `--set` and relies on the
  contract's other checks; a plug-in that scales alpha fails that assertion for
  the right output and the wrong reason.
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

## What this host cannot prove

1. **The multi-resolution and tile protocol.** It does not set
   `kOfxImageEffectPropTilesRoD` or slice render windows, so `setSupportsTiles(true)`
   paths are untested.
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
