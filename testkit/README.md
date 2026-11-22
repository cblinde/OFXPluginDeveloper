# testkit: a small OFX host for contract tests

`contract_test` loads a built `.ofx` with `dlopen`, resolves
`OfxGetNumberOfPlugins`/`OfxGetPlugin`, checks the `OfxPlugin` struct, and then
drives the plug-in through a hand-written host: `describe`,
`describeInContext`, `createInstance`, `isIdentity`, `render`,
`destroyInstance`, `unload` — twice, in one process.

It is shared by both templates. Neither template has its own copy, and nothing
here links the plug-in: the binary under test is whatever `build.sh` produced.

## Files

| File | What it is |
|---|---|
| `fake_host.cpp/.h` | the host: property, image-effect, parameter, multithread, memory and message suites, one plug-in descriptor, one effect instance, clips, parameters, source/destination images |
| `contract_main.cpp` | the checks and the command line (`.ofx` path + what the plug-in is expected to declare) |
| `build-contract-test.sh` | compiles the two files into one binary, finding the OFX headers from the template's CMake cache |
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

Flags: `--param name:type` (repeatable), `--clip name` (repeatable),
`--set name=value` before render, `--identity` (expect `kOfxStatOK` and the
source clip), `--not-identity-after-set`, `--byte-dst-rejected` (the plug-in must
answer `kOfxStatErrUnsupported` for an 8-bit destination), `--negative-pitch`,
`--src`/`--golden`/`--size WxH` for a golden-image render comparison.

`testkit/run-negative-controls.sh` proves the harness is not a rubber stamp. It
is deliberately not part of `./run-tests.sh`: a normal run has to stay green.

## What this host cannot prove

1. **The multi-resolution and tile protocol.** It does not set
   `kOfxImageEffectPropTilesRoD` or slice render windows, so `setSupportsTiles(true)`
   paths are untested.
2. **Clip preferences and host scheduling.** `getClipPreferences`, per-clip
   depths/PARs, and concurrent renders of several instances are not exercised;
   `multiThread` runs the function once, on one thread.
3. **How a real host reports errors.** The message suite here only records
   strings; nothing checks that an error reaches a user, that a progress bar
   appears, or that abort/cancel works mid-render.

Also note (measured, not assumed): the OFX 1.5.1 Support library's
`unloadAction` dereferences `gEffectDescriptors.find(id)` without checking for
`end()`, so an unload for a plug-in that was never described crashes inside the
library. The harness always describes before unloading for that reason.
