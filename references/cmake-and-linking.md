# CMake, linking, flags and the toolchain

The one command, the artifact contract, every compile-level requirement with file:line evidence, and the toolchain traps.

<!-- BUILD_PLAYBOOK.md lines 21-293 -->
<!-- BUILD_PLAYBOOK.md lines 362-476 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 561-629 -->
<!-- BUILD_PLAYBOOK.md lines 1-20 -->
## 1. The command

```bash
templates/minimal-ofx/build.sh
```

It does four things: fetch the pinned headers when they are missing, configure,
compile, and then verify the artifact. From a tree with no `.ofx-ref/` and no
`build/`, the complete first run is:

```
[1/4] OpenFX headers missing at /tmp/refclean3/.ofx-ref; fetching OFX_Release_1.5.1
      fetched OFX_Release_1.5.1
[2/4] configuring (Release)
[3/4] compiling
[4/4] verifying /tmp/refclean3/templates/minimal-ofx/build/bundle/ReferenceGain.ofx.bundle/Contents/Linux-x86-64/ReferenceGain.ofx
/tmp/refclean3/templates/minimal-ofx/build/bundle/ReferenceGain.ofx.bundle/Contents/Linux-x86-64/ReferenceGain.ofx: ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked, BuildID[sha1]=48815822e8203ce86f4ff34f0a253417eff859c9, not stripped
  exports        : OfxGetNumberOfPlugins OfxGetPlugin
  dynamic symbols: 2 defined in total
  undefined OFX  : none (all host access goes through fetchSuite at runtime)
  RPATH/RUNPATH  : none
  dlopen         : OfxGetNumberOfPlugins() = 1
  pluginApi      : OfxImageEffectPluginAPI (v1)
  identifier     : com.ofxplugindeveloper.referencegain
  version        : 1.0
  setHost        : present
  mainEntry      : present

OK: .../ReferenceGain.ofx
```

Requirements: CMake ≥ 3.21, a C++17 compiler, `nm`/`readelf`/`file` (binutils),
`python3` for the registration check, and `git` plus network access **only on the
first run**. For an offline build or an existing checkout:

```bash
OFX_SOURCE_DIR=/path/to/openfx templates/minimal-ofx/build.sh
```

The deeper check, which drives the whole host protocol through a fake host, is
[`templates/minimal-ofx/test/run-tests.sh`](../templates/minimal-ofx/test/run-tests.sh)
— 110 checks, exit 0 [V]:

```
ok 60 - OfxGetPlugin(0) returns a plug-in
ok 69 - kOfxActionLoad
ok 70 - kOfxActionDescribe
ok 71 - kOfxImageEffectActionDescribeInContext(OfxImageEffectContextFilter)
ok 79 - kOfxImageEffectActionRender
ok 101 - render rejects a byte image
ok 102 - baseline render before the rejected window
ok 103 - render rejects a window outside the source bounds
ok 104 - destination is unchanged after the rejected render
ok 105 - baseline render inside the destination bounds
ok 106 - render rejects a window outside the destination bounds
ok 107 - destination is unchanged after the rejected render
ok 109 - kOfxActionUnload
contract_test: 110 checks, 0 failed
```

---

## 2. What the artifact must be

| Requirement | How it was checked | Result | Label |
|---|---|---|---|
| An ELF shared object (`DYN`), not an executable or a static archive | `file`, `readelf -h` | `ELF 64-bit LSB shared object, x86-64` | [V] |
| Named `<Name>.ofx` — no `lib` prefix, `.ofx` suffix | produced path | `ReferenceGain.ofx` | [V] |
| No `SONAME` — it is a module, not a library anything links against | `readelf -d` | absent | [V] |
| Exactly two defined dynamic symbols, both entry points | `nm -D --defined-only` | `OfxGetNumberOfPlugins`, `OfxGetPlugin` — nothing else | [V] |
| **No** `OfxSetHost` export | `nm -D --defined-only \| grep OfxSetHost` | 0 hits (see §4) | [V] |
| No undefined `Ofx*`/`OFX*` symbols — host access is runtime `fetchSuite` | `nm -D -u` | none | [V] |
| No `RPATH`/`RUNPATH` pointing at the build machine | `readelf -d` | none | [V] |
| `dlopen` succeeds; `OfxGetNumberOfPlugins()` returns 1; `OfxGetPlugin(0)` returns `pluginApi = "OfxImageEffectPluginAPI"`, `apiVersion = 1`, the expected identifier, non-null `setHost` and `mainEntry` | `build.sh` step 4 (`ctypes`) | all pass | [V] |
| Bundle directory layout `Contents/Linux-x86-64/<Name>.ofx` | produced path | as above | [V] |
| The same layout on Windows (`Contents/Win64/`) and macOS (`Contents/MacOS/`) | — | — | [I] |

The `BuildID` is identical for the same input built in two different directories
(`/tmp/refclean3` and the working tree): the build is reproducible here because
nothing embeds the source path in the output [V].

---

## 3. Compile-level requirements, with evidence

### 3.1 Two include directories, not three

```
.ofx-ref/include          ofxCore.h, ofxImageEffect.h, ofxGPURender.h, ...
.ofx-ref/Support/include  ofxsImageEffect.h, ofxsProcessing.h, ...
```

Both the plug-in translation unit and every Support library translation unit
compile with just those two (`g++ -fsyntax-only -I include -I Support/include` on
`Plugin.cpp` and on `Support/Library/ofxsImageEffect.cpp`: exit 0) [V]. Several
projects also pass `Support/Library`; it is harmless, and unnecessary for these
translation units [V].

### 3.2 `OFX::ImageProcessor` is not in `ofxsImageEffect.h`

The first build of the reference plug-in failed with:

```
Plugin.cpp:35:53: error: expected class-name before '{' token
   35 | class RefGainProcessor : public OFX::ImageProcessor {
```

`ImageProcessor` is declared in `ofxsProcessing.h:28`
(`class ImageProcessor : public OFX::MultiThread::Processor`), and
`ofxsImageEffect.h` does not include it. Including only `ofxsImageEffect.h` gives
you the effect classes but not the processor [V — that is the compiler error
above].

### 3.3 The Support library is compiled **into** the module

`Support/Library` contains 8 translation units [V: `ls … | wc -l`]:
`ofxsCore.cpp`, `ofxsImageEffect.cpp`, `ofxsInteract.cpp`, `ofxsLog.cpp`,
`ofxsMultiThread.cpp`, `ofxsParams.cpp`, `ofxsProperty.cpp`,
`ofxsPropertyValidation.cpp`.

They are compiled into the module rather than linked as a shared library. Two
reasons, one verified and one inferred:

- [V] the entry points are ordinary functions in `ofxsImageEffect.cpp`; whatever
  provides that file provides the exports.
- [I] a shared Support library would be a second copy of the same C++ types in
  the host process for every plug-in that used it, and its symbol visibility
  would be outside your control. One static copy per plug-in, hidden, is the
  shape the Support library is written for.

### 3.4 `-fPIC` is mandatory

Compiling a Support object without it and linking it as a shared object fails:

```
relocation R_X86_64_PC32 against symbol `_ZN3OFX9Exception5SuiteD1Ev'
can not be used when making a shared object; recompile with -fPIC
final link failed: bad value
```

[V — reproduced with `ofxsCore.cpp`.] In CMake this is
`POSITION_INDEPENDENT_CODE ON`; the resulting command line contains `-fPIC`
(verified in `build/compile_commands.json`).

### 3.5 `-fvisibility=hidden`, and what it buys

Same sources, same flags, one difference — hidden vs default visibility:

| Build | `nm -D --defined-only`, `T` symbols |
|---|---|
| default visibility | **647** |
| `-fvisibility=hidden -fvisibility-inlines-hidden` | **2** |

[V — both builds were run; the 647-export build lists every `OFX::GroupParam`,
`OFX::Int2DParam`, … constructor.] Every plug-in in a host process carries its own
copy of those symbols; with default visibility the dynamic loader may bind one
plug-in's `OFX::PropertySet` to another's. The symptom is a crash that appears
only when a second plug-in is installed.

That is not enough on its own: visibility attributes do not reach **weak template
instantiations**. With `-fvisibility=hidden -fvisibility-inlines-hidden` the same
artifact still exported five weak helpers (`std::vector<OFX::ContextEnum>`'s
`_M_realloc_insert`, two `std::_Rb_tree` lookups, …) — 7 defined dynamic symbols
instead of 2 [V]. The reference plug-in therefore also pins the export list with a
linker version script, [`templates/minimal-ofx/exports.map`](../templates/minimal-ofx/exports.map)
(`-Wl,--version-script=…`, GNU ld/gold/lld on Linux). After it: exactly 2 [V].
macOS (`-exported_symbols_list`) and Windows (`.def`) have equivalents [I].

The two survivors are re-exported explicitly by the Support library:

```cpp
// ofxsImageEffect.cpp:17-18
# if __GNUC__ >= 4
#  define EXPORT __attribute__((visibility("default")))
```

`EXPORT` is applied at `ofxsImageEffect.cpp:3055` (`OfxGetNumberOfPlugins`) and
`:3066` (`OfxGetPlugin`) [V, header read and confirmed by `nm`].

### 3.6 The two entry points, and the third one that is not an export

`ofxCore.h` declares three functions:

| Declaration | Line | Exported by the Support library? |
|---|---|---|
| `OfxPlugin *OfxGetPlugin(int nth)` | `:558` | **yes** (`ofxsImageEffect.cpp:3066`) |
| `int OfxGetNumberOfPlugins(void)` | `:566` | **yes** (`ofxsImageEffect.cpp:3055`) |
| `OfxStatus OfxSetHost(const OfxHost *host)` | `:577` | **no — it is not defined anywhere in `Support/`** |

`ofxCore.h:569-576` explains why: the call was added in 2020, "not specified in
earlier implementation of the API", so "host must check if the plugin implemented
it and not assume symbol exists". The Support library does not implement it. The
host pointer reaches the plug-in through the `setHost` **field** of the
`OfxPlugin` struct (`ofxCore.h:136`), which Support fills in at
`ofxsImageEffect.cpp:3026`. A host that looks for `dlsym(handle, "OfxSetHost")`
gets NULL and must fall back to calling `plugin->setHost(&host)` [V for the
absence of the symbol; I for the host-side fallback, since no host is installed
here].

### 3.7 Registration: one function, and it is required at link time

The plug-in defines exactly one hook:

```cpp
// ofxsImageEffect.h:1205-1209
namespace Plugin {
  /** @brief Plugin side function used to identify the plugin to the support library */
  void getPluginIDs(OFX::PluginFactoryArray &id);
};
```

The Support library calls it once, from `init()`
(`ofxsImageEffect.cpp:3039`), and turns each factory into an `OfxPlugin`:

```cpp
// ofxsImageEffect.cpp:3017-3028
ofxPlugin->pluginApi          = kOfxImageEffectPluginApi;
ofxPlugin->apiVersion         = 1;
ofxPlugin->pluginIdentifier   = factory->getID().c_str();
ofxPlugin->pluginVersionMajor = factory->getMajorVersion();
ofxPlugin->pluginVersionMinor = factory->getMinorVersion();
ofxPlugin->setHost            = OFX::Private::setHost;
ofxPlugin->mainEntry          = factory->getMainEntry();
```

Consequences you can rely on:

- The factory must have **static storage duration**: `getPluginIDs` runs once and
  the `OfxPlugin` structs point into it for the module's lifetime [I — from the
  code above; the reference plug-in uses a function-local `static`].
- Omitting `getPluginIDs` is a **link** error, not a runtime surprise, because
  `ofxsImageEffect.cpp` references it [I — from the reference to it at `:3039`].
- `pluginIdentifier` comes from the factory (`factory->getID()`), so the string in
  `OFX::PluginFactoryHelper<…>(id, major, minor)` is the identity the host stores
  in a project. Changing it orphans existing projects; the rule for the string is
  `ofxCore.h:106` (reverse domain, ASCII, no whitespace) [V for the
  plumbing; I for host-side project behaviour].

### 3.8 The API string and version are not yours to choose

```c
// ofxImageEffect.h:26, :30
#define kOfxImageEffectPluginApi "OfxImageEffectPluginAPI"
#define kOfxImageEffectPluginApiVersion 1
```

Support copies exactly those into the struct [V — `nm`/`dlopen` inspection of the
artifact reports `OfxImageEffectPluginAPI (v1)`]. This is a place where a
hand-written assertion is easy to get wrong: the natural guess
`"OfxImageEffectPlugin"` is a different string, and a host that validates
`pluginApi` will enumerate the plug-in and then refuse it [I for the host
refusal; the string itself is [V]].

### 3.9 C++ standard and ABI

- The build compiles as C++17 (`-std=c++17` in `compile_commands.json`) [V].
- Only C types and C structs cross the plug-in/host boundary; `std::string`,
  `std::vector` and the Support wrappers stay inside the module [I].
- Because of that, `-D_GLIBCXX_USE_CXX11_ABI` does not need to match the host
  [I]. It *does* matter for a third-party C++ runtime you call across the
  boundary (libtorch is the documented example — see
  [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §1).
- Undefined `libstdc++`/`libc` symbols are expected and correct: the host process
  provides them. `nm -D -u` on the reference plug-in lists only GLIBC/GLIBCXX
  symbols [V].

### 3.10 RPATH: only when you ship a dependency

The reference plug-in needs nothing beyond libc/libstdc++, so it records no
`RPATH` or `RUNPATH` [V]. When you do bundle a runtime next to the `.ofx`, the
pattern is `$ORIGIN` — and `readelf -d` must not contain the build machine's path
[I for other libraries; V that `$ORIGIN` is what the ONNX template records].

## 6. Reusing the scaffold

1. Copy `templates/minimal-ofx/` (about 570 lines including build, test and the
export list).
2. Rename in `CMakeLists.txt`: `PLUGIN_TARGET` (the file name the host sees) and
   keep `SUFFIX ".ofx"` / `PREFIX ""`.
3. Change `kPluginIdentifier` in `src/Plugin.cpp` to your own reverse-domain
   string, and the labels in `describe()`.
4. Keep the two `#include`s: `ofxsImageEffect.h` for the effect, and
   `ofxsProcessing.h` for `OFX::ImageProcessor` (§3.2).
5. Implement the pixel work in `multiThreadProcessImages`; add parameters in
   `describeInContext` and fetch them in the constructor.
6. Run `templates/minimal-ofx/build.sh`; if it fails, read the failing step — the
   script names the assertion, not just the symptom.
7. Replace the expectations in `test/run-tests.sh` (`--id`, `--param`, `--clip`)
   with yours; the contract harness stays shared.

For the host-side rules that a compile cannot check (streams, RoD/RoI, threads,
alpha), continue with
[`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) and
[`checklists.md`](checklists.md).

---

## 7. Windows: the toolchain matrix that decides whether the build works

The Linux build is a compiler and CMake. The Windows build is a **three-way
match** between CUDA, MSVC and the generator, and each pair can fail on its own.
Every row below was reproduced on a machine with VS 2026 (MSVC 19.50) and VS 2022
Build Tools installed without the C++ workload **[B]** (2026‑11‑22 field test,
Windows 11, RTX 3060, CUDA 13.0.48, CMake 4.4.4; environment, logs and the
limits of that run are in
[`field-evidence.md`](field-evidence.md)).

| Pair | What breaks | Symptom | Fix |
|---|---|---|---|
| VS generator ↔ NVIDIA customisations | A `.cu` file needs NVIDIA's MSBuild `BuildCustomizations` for that exact VS version; CUDA 13.0 ships none for VS 2026 | `CMake Error … No CUDA toolset found.` | Use Ninja and call `vcvars64.bat` first; `build.bat` probes `…\BuildCustomizations\CUDA *.props` and falls back automatically |
| nvcc ↔ MSVC | `cuda\include\crt\host_config.h` rejects compilers newer than the toolkit knows | `fatal error C1189: -- unsupported Microsoft Visual Studio version!` | `vcvars64.bat -vcvars_ver=14.44` (the newest accepted), or `-allow-unsupported-compiler` if you accept the risk |
| generator ↔ output property | `MODULE` targets use `RUNTIME_OUTPUT_DIRECTORY` under MSVC multi-config and `LIBRARY_OUTPUT_DIRECTORY` under Ninja | build succeeds, `.ofx` lands in `Contents\Linux-x86-64\` (or `Win64\Release\`) and no host scans it | set both properties to the same `$<1:…>`-wrapped arch directory, chosen from `WIN32`/`APPLE` |
| macOS ↔ rpath token and runtime file name | `$ORIGIN` is an ELF token dyld treats as a literal directory, and `libonnxruntime.so.1` is a Linux SONAME | the bundle builds, the Mac host cannot resolve the runtime, and the build says nothing | `INSTALL_RPATH "@loader_path"` on `APPLE` and `$ORIGIN` on other UNIX; copy the resolved `libonnxruntime.<ver>.dylib` under its own name (its install name is `@rpath/…`) |
| generator ↔ cached build tree | the second run may pick a different generator than the cache records | `Error: generator … Does not match the generator used previously` | reuse `CMAKE_GENERATOR:INTERNAL` from `build\CMakeCache.txt`, refuse loudly when that generator cannot build CUDA |
| CUDA ↔ architecture list | `enable_language(CUDA)` caches nvcc's default; a second configure sees a defined variable | `-- CUDA architectures: 75`, one `--generate-code`, SASS only for Volta | record nvcc's default once, and only on a configure that saw no user `CMAKE_CUDA_ARCHITECTURES`; treat a differing value as a user choice. On a tree with no record, tell a `-D` value from `enable_language()`'s by the cache entry's type (`UNINITIALIZED` vs `STRING`) |
| CUDA 13 ↔ runtime import | the Windows image does not import `cudart64_*.dll` even with device code present | `dumpbin /dependents` lists no CUDA DLL | say "device code present, runtime resolved at load"; never print the import you expected |
| ONNX Runtime ↔ import library | the Windows package keeps headers in `include\`, the import library in `lib\onnxruntime.lib` and the DLL in `bin\onnxruntime.dll` | the build succeeds, the bundle carries a file named `onnxruntime.dll` that the loader rejects, and the plug-in fails at load | on Windows do **not** link the `.lib`: compile with `ORT_API_MANUAL_INIT`, copy the DLL as `onnxruntime-ofx.dll` and load it by full path (`onnx-and-tensorrt.md` §4); Linux/macOS link as usual. The template fails the configure when the DLL is missing |
| host runtime ↔ bundled runtime | Windows resolves an import by base name, and the host's directory comes first: Resolve 21 ships `onnxruntime.dll` 1.13, Windows ML ships one in `System32` | `render` access-violates; the minidump names the host's `onnxruntime.dll` and not the bundle's | private file name + `ORT_API_MANUAL_INIT` + `LoadLibraryExW` from the module directory; a bound import cannot be overridden by copying a file beside the `.ofx` |
| `vswhere` ↔ `for /f` | a `for /f` backtick command whose first **and** last characters are quotes loses both before `cmd /c` runs it, so the program path is split at its first space | `Der Befehl "C:\Program" ist entweder falsch geschrieben oder konnte nicht gefunden werden.` — then the checker reports `dumpbin not found` and every `build.bat` aborts after a correct build | end the command with an unquoted argument (`-property installationPath`) and locate the tool with a `for /d` glob; never pass a quoted `-find` pattern there |
| platform ↔ architecture directory | the directory name is part of the contract: `Linux-x86-64`, `Linux-aarch64`, `MacOS`, `Win64` | an ARM Linux build links, reports success and lands in `Contents/Linux-x86-64`, which no host scans | derive the directory from `WIN32`/`APPLE`/`CMAKE_SYSTEM_PROCESSOR` once, and set both output properties to it |
| `find` ↔ Git-for-Windows | Git Bash puts its own `usr\bin` ahead of `System32`, and `find.exe` exists in both. The legacy `find /c /v ""` idiom used to count lines is parsed by **GNU** find, which reads `/c` as a path | thousands of `find: '/c/…': Permission denied` lines and a build that appears to hang *after* a correct link; measured on the CUDA template | count with `set /a` in a `for /f` loop, or call `%SystemRoot%\System32\find.exe` by full path. `findstr`, `where`, `dumpbin` and `cuobjdump` have no MSYS namesake and are safe |
| `dumpbin` ↔ export-row regex | the file-header line `00000000 characteristics` matches a loose `[0-9A-Fa-f]*` RVA group, because `c` is a hex digit | `FAIL: dumpbin reports 3 exported names, expected 2` on a DLL with exactly two | match the eight-digit RVA column explicitly (`[0-9A-Fa-f]` repeated eight times), never `[0-9A-Fa-f]*` |
| `--cuda` ↔ CPU-only build | `build.bat` asked the checker for cubins even when it had just built the CPU path, and `cuobjdump`'s "does not contain device code" info line counted as a cubin | the CPU-only build printed `device code: 1 cubin(s)` about a binary with no device code, and passed only because of it | pass `--cuda` only when a CUDA path was compiled, and count only lines that name a `.cubin` |

**Artifact check, Windows side.** `nm -D`, `readelf` and `dlopen` have direct
equivalents, and `testkit/check-ofx-artifact.bat` runs them so the claim is a
measurement:

```
dumpbin /exports    <Name>.ofx   # exactly OfxGetNumberOfPlugins + OfxGetPlugin
dumpbin /dependents <Name>.ofx   # what the loader must find
cuobjdump -lelf     <Name>.ofx   # cubins for every architecture in the list
```

All three `build.bat` scripts call it with `--strict` after the build, and the
checker resolves the tools itself: `dumpbin` from `PATH`, `%VCToolsInstallDir%`,
the newest `VC\Tools\MSVC\*` under vswhere's `-property installationPath`, or the
`Program Files\Microsoft Visual Studio\<year>` layout,
`cuobjdump` from `PATH`, `%CUDA_PATH%`, `%CUDA_HOME%` or the toolkit roots. That
lookup is not optional: a Visual Studio generator build never calls `vcvars64`,
so neither tool is on `PATH` there, and trusting `PATH` turned the artifact check
into a printed skip in exactly the branch most people use. With `--strict` a
missing tool fails the build (exit 3); standalone use without the flag keeps the
named skip. From the field test: 2 exports, 7 cubins
`sm_75`…`sm_120`, and no CUDA runtime import **[B]**
([`field-evidence.md`](field-evidence.md) §3).

**One command for this template's suites on Windows.**
`templates\minimal-cuda-ofx\test\run-tests.bat Release` is the counterpart of
`test/run-tests.sh` on the platform where `g++`, `nm -D`, `readelf`, `dlopen` and
valgrind do not exist: the artifact check, `test/gpu-kernel-test.cu` executed on
the real GPU, and the contract harness through `LoadLibraryExW` **[V]** (Windows
11, RTX 3060 sm_86, CUDA 13.0.48, MSVC 19.44: 2 exports, 7 cubins, 28/28 kernel
cases, 136 contract checks). The CPU-emulated equivalence suite and memcheck stay
Linux-only, and the script says so rather than skipping them silently.

**Start the script with `.\build.bat`.** With `NoDefaultCurrentDirectoryInExePath=1`
set (a common policy), `cmd.exe` will not find a bare `build.bat` in the current
directory; the failure text says "misspelled or could not be found" and names
nothing else.

**The ONNX template is a bundle of two binaries.** `templates/onnx-inference-ofx/build.bat`
requires `ONNXRUNTIME_ROOT` with `include\onnxruntime_cxx_api.h` and
`bin\onnxruntime.dll` (the `.lib` is not linked on Windows); it asserts both the `.ofx` in
`Contents\Win64` and the private `onnxruntime-ofx.dll` beside it, then runs the
artifact check. That second assertion covers both ways to ship a bundle that
cannot work: an import library renamed to `.dll`, and the old `onnxruntime.dll`
name that a host's own runtime pre-empts even when the file is present. The Linux script has the equivalent check via `readelf -d` (`NEEDED libonnxruntime.so.1`). The `.ofx` here is also the one template whose
failure mode is *two* files, so the contract harness needs the bundle directory,
not just the `.ofx` path.

**One command for the ONNX template's suites.** `templates\onnx-inference-ofx\test\test.bat
Release` is the Windows counterpart of `test/run-tests.sh`: it validates
`ONNXRUNTIME_ROOT` (a raw NuGet extraction is accepted and normalised into
`build\_ort`), puts the runtime's `bin\` on `PATH`, builds unless the second
argument is `nobuild`, runs `ai_pipeline_test models`, then the contract harness
twice — exact golden, then the plug-in's own tiling at tolerance 0.02 — and ends
with the fixture list and `check-ofx-artifact.bat --strict`. It has never run, so
it is **[I]** until someone executes it on Windows; the steps mirror the Linux
script, and a failure there is as likely to be this script as the plug-in.

**What stays Linux-only.** The CPU-emulated kernel suite
(`test/kernel_equivalence.cpp`) calls `posix_memalign`, which MSVC does not have,
and the shell suites need `g++`, `nm`, `readelf` and `valgrind`. On Windows the
real checks are the contract harness (`testkit/build-contract-test.bat`) and the
hardware harness (`test/gpu-kernel-test.cu`); the emulated suite is not ported,
so its evidence remains Linux-only.

## 10. Toolchain notes that cause real failures

### `nvcc` flags

| Flag | Why |
| --- | --- |
| `-lineinfo` | Line mapping for Nsight Compute with minimal optimisation loss. Ship release builds with it. |
| `-G` | Full device debug. Disables most optimisation — debug builds only. |
| `--use_fast_math` | Off by default. Enables it changes results (denormals, `sin`/`cos` precision). A grading plugin should not silently change numerics. |
| `-Xptxas -v` | Register/spill report; check it when occupancy is bad. |
| `-arch=sm_XY` / CMake `CUDA_ARCHITECTURES` | Target list, see `packaging-and-install.md` §6. |

### PTX and first-launch cost

A binary that carries only PTX for the newest architecture JIT-compiles on first
use, which shows up as a multi-second stall when the user first applies the
effect. Ship SASS for the architectures you care about (`75;80;86;89;90;100;120`)
and PTX for the newest only.

### Static vs dynamic `cudart`

| | `cudart` (shared) | `cudart_static` |
| --- | --- | --- |
| Depends on runtime DLL/SO at load | Yes on Linux and older toolkits | No |
| CUDA context | Shared with the host | Per-plugin instance of the runtime |
| Failure mode | Missing/incompatible `libcudart` → plugin fails to load | Two runtimes in one process; undefined behaviour if they both manage the same allocations |
| Recommendation | Default | Only for closed deployment targets, and never alongside another dynamic-cudart plugin |

On **Windows with CUDA 13** the packaged runtime is a hybrid: a built template's
import table lists no `cudart64_*.dll` and uses `cudart_hybrid64.dll`, while the
device code is present and the module loads (**[B]**, `packaging-and-install.md` §6). "Depends at
load" is therefore a Linux/older-toolkit statement, not a universal one; check the
import list of the binary you ship.

The mismatch that bites: the host ships its own CUDA runtime version (Resolve
bundles one), your plugin links another, and the two disagree about the ABI of
`cudaPointerAttributes` or the primary context refcount. The visible symptom is
"plugin does not load" or a crash in `cuInit`. Keep the plugin's `cudart`
requirement at or below the version the host ships, and prefer the runtime
interface that has been stable for years.

### Windows specifics

- The `.ofx` is a DLL: `PREFIX ""`, `SUFFIX ".ofx"`.
- MSVC runtime must match across the CUDA host compiler and the C++ TUs
  (`/MD` Release, `/MDd` Debug). Mixing allocators across that boundary corrupts
  the heap, usually much later than the actual bug.
- Delay-loading (`/DELAYLOAD:cudart64_*.dll` + `delayimp.lib`) lets the plugin
  load on machines without CUDA and fail only when the GPU path is used — with a
  clean error instead of a load failure the host reports as "plugin broken".
- Do not install to `C:\Program Files\...` from the build; use
  `build.bat Release install` and let the user run it elevated.

### Linux specifics

- Shared objects must be PIC (`POSITION_INDEPENDENT_CODE ON`); CUDA objects too.
- `-fvisibility=hidden` plus explicit exports for the two mandated entry points
  (the Support library already marks them). Two plugins with default visibility
  will interpose each other's Support symbols **[I]**.
- The visibility flag has to reach the CUDA objects too. In CMake that is a
  **separate** target property, `CUDA_VISIBILITY_PRESET`: `CXX_VISIBILITY_PRESET`
  alone leaves every `extern "C"` launcher in `Kernel.cu` exported (measured on
  this template: 5 `T` symbols instead of 2) **[V]**.
- `libcudart.so.*` must be findable: either installed by the toolkit, or shipped
  next to the `.ofx` and found via `RPATH=$ORIGIN`. Shipping it is often the
  difference between "works on my machine" and "works on the colourist's".

---

## Build playbook: from a clean checkout to a `.ofx` a host will load

Most build prose is written from headers. This file is written from a build.
[`templates/minimal-ofx/`](../templates/minimal-ofx/) is compiled by one command
from a tree that has no OFX headers and no build directory, and the command then
inspects the `.ofx` it produced instead of trusting the source.

Every claim below is labelled:

- **[V]** verified in this repository — the command was run and its output is
  quoted, on Linux x86-64 with GCC 13.3 and CMake 3.31.6.
- **[I]** inferred — from a header or from documented host behaviour, but not
  demonstrated by a build or a host here.

The headers are the repository's pinned checkout of
`AcademySoftwareFoundation/openfx` at tag `OFX_Release_1.5.1`; every `file:line`
below refers to that tree (`.ofx-ref/`, created by the build if absent).

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[V]** verified in this repository - the command ran here
