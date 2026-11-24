# Tier 1 — the official OpenFX host

This directory builds the **reference host implementation published by the OpenFX
project** and runs its own headless example against a bundle:

```
openfx/HostSupport/src/*.cpp          the host side of the standard: property,
                                      memory, parameter, multithread, message
                                      and image-effect suites
openfx/HostSupport/examples/cacheDemo the standard's headless host example,
                                      compiled unmodified
```

Nothing here re-implements a host. The suites a plug-in receives are the ones its
maintainers ship, which is the point: a hand-written mock can pass while missing a
mandatory suite, and the failure then shows up in Resolve or Nuke instead.

## Build

The OpenFX checkout is the repository's usual `.ofx-ref` (gitignored), or any
checkout passed as `OFX_SOURCE_DIR`:

```bat
git clone --depth 1 --branch OFX_Release_1.5.1 ^
  https://github.com/AcademySoftwareFoundation/openfx.git .ofx-ref

cmake -S testkit\official-host -B testkit\official-host\build -G "Visual Studio 18 2026" -A x64
cmake --build testkit\official-host\build --config Release --parallel
```

`expat` (the host's plugin-cache XML parser) is fetched as source and built
**statically**: the first attempt left `cacheDemo.exe` unable to start with
`libexpat.dll: cannot open shared object file`, which is a poor property for a
verification binary.

## Run

```bat
set OFX_PLUGIN_PATH=<build>\bundle
testkit\official-host\build\Release\cacheDemo.exe
```

It prints one block per plug-in (`Plug-in:<id>`, `Filepath:`, `Contexts:`) and
writes `newCache.xml` next to the working directory, with every property each
plug-in declared during `kOfxActionDescribe`.

## What this proves, and what it does not

Proven, on hardware, against `templates/minimal-cuda-ofx` **[V]**:

* `OfxGetNumberOfPlugins` / `OfxGetPlugin` are called and the module loads
  (`kOfxActionLoad`, then `kOfxActionDescribe`) under a host that is not ours;
* the plug-in's declared properties survive the trip: grouping
  `OFX Plugin Developer`, `OfxImageEffectRenderInstanceSafe`,
  `OfxImageEffectPropSupportedContexts` = Filter + General, float-only pixel
  depths, single-instance off, temporal access off;
* no `message`/`error` line is attributed to our plug-in id.

**Not** proven by `cacheDemo`: `kOfxImageEffectActionDescribeInContext` content
(the example's `newInstance()` returns `NULL`, so parameters and clips are never
instantiated) and `kOfxImageEffectActionRender`. Those need a host that creates
instances — Tier 2 (Natron, `testkit/verify-bundle.bat`) or the finer-grained
contract harness in `testkit/`.

## Caveat: `OFX_PLUGIN_PATH` is additive

`PluginCache::PluginCache()` appends the platform's standard plugin directory
after the entries in `OFX_PLUGIN_PATH` (`ofxhPluginCache.cpp:240`-`245`):

```
#if defined(_WIN32)
  _pluginPath.push_back(getStdOFXPluginPath());
  _pluginPath.push_back("C:\\Program Files\\Common Files\\OFX\\Plugins");
```

So this host scans the system directory too, no matter what `OFX_PLUGIN_PATH`
says. Measured here: 84 plug-ins described and **23 s** for one run, because the
machine has ~40 commercial bundles installed, several of which print their own
licence or load errors. Two consequences:

* match on the plug-in id, never on "the output contains only our plug-in";
* a Tier 1 run is not a 0.1 s check on a machine with a populated plugin
  directory. It is fast (< 1 s) only where that directory is empty.

The isolation is still worth setting: it makes our bundle the *first* thing
scanned, and it keeps a build directory from being picked up by accident.
