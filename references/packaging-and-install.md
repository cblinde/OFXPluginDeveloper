# Packaging, bundle layout and install

Where the host looks on disk, the four architecture directories, RPATH, licence texts and the install step.

<!-- BUILD_PLAYBOOK.md lines 294-323 -->
<!-- SKILL.md lines 405-536 -->
## 1. What the host looks for on disk

`templates/minimal-ofx/build.sh` produces:

```
ReferenceGain.ofx.bundle/
  Contents/
    Linux-x86-64/
      ReferenceGain.ofx
    Resources/
```

[V for the produced tree.] The `Contents/<arch>` naming and the plugin-directory
search paths (`/usr/OFX/Plugins`, `C:\Program Files\Common Files\OFX\Plugins`,
`/Library/OFX/Plugins`) are host behaviour, not something the build can prove
here [I — see [`host-resolve.md`](host-resolve.md)].
`Resources/` is optional [I].

The bundle also carries obligations a build log cannot show. If it ships a
model, a font or a tokenizer table, the licence text for each belongs in
`Contents/Resources` beside it — "in the repository" is not "in the bundle".
Copy the ledger as a `POST_BUILD` step with
`${CMAKE_COMMAND} -E copy_if_different`, after the asset copies, and warn
(`message(WARNING)`) instead of failing when it is absent, so a source tree
without the ledger still builds. A field-tested detector shipped 385 MB of
AGPL-3.0 and research-only weights and had its notice file only in the
repository root **[B]**.

---

## 2. Build and packaging standards

**Bundle layout.** Hosts scan `NAME.ofx.bundle/Contents/<ARCH>/NAME.ofx`, where
`ARCH` is `Win64`, `Win-arm64ec`, `Linux-x86-64`, `Linux-aarch64` or `MacOS`. The
file must end in `.ofx` on every platform and the **stem must match the bundle
name**: `MinimalCudaGain.ofx.bundle/Contents/Win64/MinimalCudaGain.ofx`.

**The layout is checked, not just described.** `testkit/check-ofx-artifact.bat`
walks up from the `.ofx` it was handed and asserts the four things a host reads
but a link table cannot show: the directory above the binary is one of the
architecture names above, its parent is `Contents`, the bundle ends in
`.ofx.bundle`, and no directory under `Contents` starts with `@` (the scan skips
those). `Info.plist` is required for a `MacOS` bundle and refused in any other,
because a host on Windows or Linux never reads it. Measured on 2026-11-27 against
four doctored bundles: `Contents/Win65/` and a stray `Contents/Info.plist` and a
`Contents/@cache/` each fail with their own line, and a `.ofx` checked outside a
bundle prints `layout: skipped` instead of inventing a verdict.

**What ships is part of the bundle.** The architecture directory decides whether
a host finds the module; the files beside it decide whether it works and whether
you may ship it. One list drives the copies, the dropdown labels and any generated
header — runtime under the loader's name, every model the UI offers, fonts,
tokenizer tables, and the **licence text for each third-party item** — so a model
cannot be offered and missing, and an unlisted file is removed and named in the
build log. Copy the licence ledger with `POST_BUILD` +
`${CMAKE_COMMAND} -E copy_if_different`, after the asset copy and before stale-file
cleanup, and make a missing ledger a `message(WARNING)` so a source tree without
it still builds. A bundle of AGPL weights that installs and renders without its
notice has shipped for eight builds in a field-tested plug-in **[B]**.

**A bundled ONNX Runtime raises the bundle's glibc floor.** The 2.17 baseline in
[`cmake-and-linking.md`](cmake-and-linking.md) §5 belongs to the bundle the
repository builds itself: on 2026-11-26 `tools/check-glibc.sh --max 2.17` found
`GLIBC_2.14` in `ReferenceGain.ofx` and `MinimalCudaGain.ofx`, but `GLIBC_2.32`
in `OnnxInference.ofx` and `GLIBC_2.28` in the `libonnxruntime.so.1` beside it
([`check-glibc.log`](../evidence/2026-11-26-linux/check-glibc.log)). The runtime
is prebuilt for a newer baseline and this container's libstdc++ adds to it, so an
inference bundle cannot be shipped to a CentOS 7 host: check the bound you
target with the same command, and treat "glibc 2.17" as a property of a bundle,
not of the repository. `./run-tests.sh` enforces the bound per bundle — 2.17 for
the reference and CUDA plug-ins, 2.32 for the inference bundle — and
`GLIBC_MAX_MINIMAL_OFX`, `GLIBC_MAX_MINIMAL_CUDA` and `GLIBC_MAX_ONNX` override a
bound when another platform is the target.

**The CUDA provider belongs in the same bundle.** One bundle that appends the
provider and falls back to the CPU, never a second "GPU build": the provider plus
cuDNN, cuBLAS/cuBLASLt, cuFFT and cudart, behind
`-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` and `-DOFXPLUGIN_CUDA_RUNTIME_DIRS`, with a
warning per missing file because a missing import fails as **error 126**, not as a
message. Two host-facing facts:

- **Preload only what the provider imports, and only where the host does not own
  that base name.** Imports resolve by base name, so an explicit load of your
  `cudnn64_9.dll` beside Resolve's creates a second module of the same name and
  mixes two minor versions in one process. Skip **cuFFTW** entirely: the provider
  never imports it and it fails 127 next to a newer host cuFFT. Compare the loaded
  module's *path*, not its name, and report `host CUDA stack` vs `bundled CUDA`.
- **`-DOFXPLUGIN_CUDA_FROM_HOST=ON` is the packaging answer for a host that brings
  its own CUDA**: provider and shared library only, no cuDNN/cuBLAS/cuFFT/cudart
  of ours to collide (2.8 GB → 1.1 GB in the reporter's bundle **[B]**). It needs a
  CUDA host; it does not replace the runtime guard on a full bundle.

  **Measured in Resolve Studio 21.2.0.5, 2026-11-26 - and the run says less than
  this paragraph first claimed.** Both bundles hold four files
  (`OnnxInference.ofx`, `onnxruntime-ofx.dll`, `onnxruntime_providers_cuda.dll`,
  `onnxruntime_providers_shared.dll`) and no cuDNN, cuBLAS, cuFFT or cudart of
  their own, and at run time the host's process carried `cudnn64_9.dll` from its
  own install folder and its own `onnxruntime.dll` (**[V]**,
  [`resolve-provider-only.log`](../evidence/2026-11-26-windows/resolve-provider-only.log)).
  The plug-in itself never ran in either session: the host registers each bundle,
  then fails to create a context - 19 times with the 1.32.0 package, 151 times
  with the 1.20.1 one, and `Plugin [...] is not available!!!!` besides - and the
  frame is the source (`mae 0.0`) even with `ONNX_MODEL_PATH` pointing at a
  model. So the run establishes the first half of the claim - the host supplies
  the CUDA stack, our bundle adds no second copy of a base name - and nothing
  about our provider, which no session loaded.
  On 2026-11-27 the session half closed, with the same four-file bundle and a
  provider built for CUDA 12: `poc.py` rendered 1920x1080 with the host's
  compute API on CUDA, and the plug-in's `provider` label read
  `CUDA (device 0, 1 thread, bundled CUDA, tf32 off, arena kSameAsRequested,
  all nodes on CUDA)` - a CUDA session with every node on CUDA, not the CPU
  fallback (**[V]**,
  [`resolve-onnx-provider.log`](../evidence/2026-11-27-windows/resolve-onnx-provider.log)).
  Two changes were needed: the 1.5.1 guard has to catch
  `OFX::Exception::Suite` with `kOfxStatErrMissingHostFeature` as well as
  `PropertyUnknownToHost` (this host did not answer ErrUnknown for the write),
  and the template appends its provider label to `$ONNX_PROVIDER_LOG` because
  the host's script API cannot read a parameter. That the provider imports a
  specific CUDA major remains a constraint to design for (**[B]**, ONNX Runtime's
  provider requirements), but it is not what separated these two packages: both
  failed before a provider could load, for the reason measured on 2026-11-27
  below.
  The frame in that run is a **pass-through** because no instance was created,
  not for want of a parameter: Resolve hands a script an OFX effect object with
  `GetInfo`, `GetIndex`, `GetIsEnabled`, `SetIndex` and `SetIsEnabled` and nothing
  else, so nothing can be set from the script - which this plug-in does not need,
  because it reads `ONNX_MODEL_PATH` or `Contents/Resources/model.onnx` when it is
  created. **The Fusion route is closed too**, which
  closes the last scripting door: `effect.GetFusionComp()` returns `None` for an
  effect added with `AddEffect("ofx", …)`, and a comp added to the timeline item
  (`item.AddFusionComp()`) holds only `MediaIn`/`MediaOut` - `AddTool` refuses the
  plug-in under each identifier tried, and no frame was rendered from it (**[V]**,
  [`resolve-fusion-path.log`](../evidence/2026-11-26-windows/resolve-fusion-path.log)).
  **A prepared project is not the way either, and now for a different reason.**
  Exporting before the host has saved the project writes an empty
  `<EffectFiltersBA/>`; an export taken after the host's own `Start saving
  project` line carries the effect, its stored name is the file's base name and
  `LoadProject(<base name>)` reopens it with the effect on the item (**[V]**,
  [`resolve-project-route.log`](../evidence/2026-11-26-windows/resolve-project-route.log)
  §6). What stopped the frame was the plug-in's own `describe()`: Resolve answered
  `describe failed on plugin com.ofxplugindeveloper.onnxinference, error code 4`
  and never created an instance, so the render was the source unchanged whatever
  the model path said. The write behind that answer is measured - of everything
  `describe()` sets, the host rejects only `kOfxImageEffectPropCPURenderSupported`,
  and the support library turns that `PropertyUnknownToHost` into
  `kOfxStatErrMissingHostFeature` (4). Guarded, the same host loads the bundle,
  creates the filter context and renders the model's output
  (**[V]**, [`onnx-describe-resolve.log`](../evidence/2026-11-27-windows/onnx-describe-resolve.log)).
  The model path itself no longer needs the project - `ONNX_MODEL_PATH` or a
  `Contents/Resources/model.onnx` beside the binary ([`improvement-backlog.md`](improvement-backlog.md)
  row 19) - and "uses Resolve's own cuDNN" is not claimed here. Neither does a module
  list help settle it on its own: the host loads its own `onnxruntime.dll`, so
  `tasklist /M` names a runtime whether or not the plug-in ever created a session.

  **The provider's CUDA major is part of the bundle's contract, and it is checkable
  before a render.** The provider names its major in its imports, the Windows
  loader resolves those by base name, and a major the process cannot find
  surfaces as `Error 1114` - indistinguishable from a broken plug-in. What pairs:

  | Provider built from | imports (`dumpbin /dependents`) | loads where | fails where |
  |---|---|---|---|
  | ONNX Runtime 1.20.1 GPU package | cuBLAS/cuBLASLt 12, cuFFT 11, cudart 12, cuDNN 9 | Resolve Studio 21.2.0.5, which ships CUDA 12 | a process that offers only CUDA 13 |
  | ONNX Runtime 1.32.0 GPU package | `cublasLt64_13.dll`, `cublas64_13.dll`, `onnxruntime_providers_shared.dll`, `dxgi` | a process that can resolve CUDA 13 | a host whose only CUDA is 12 (`Error 1114`) |

  Measured 2026-11-27, and the second row is why the caveat matters: the 1.32.0
  provider-only bundle **did** load and run in Resolve on the machine it was built
  on, because the CUDA 13.0 toolkit sits on `PATH` and `cublas64_13.dll` resolves
  from there - the label read `all nodes on CUDA`. That is a separate case and must
  not be read as Resolve's own CUDA stack accepting 13: on an install with only
  what Resolve ships, the same file has nothing to bind to
  ([`onnx-provider-cuda13-deps.log`](../evidence/2026-11-27-windows/onnx-provider-cuda13-deps.log)).
  The clean Resolve configuration stays 1.20.1, which needs nothing the host does
  not already carry.

  Two places say so out loud, so a failed host load is no longer the first report.
  The configure step prints `CUDA provider bundle: the provider imports cuBLAS N`
  and warns when the cuBLAS copied beside it is another major - the stale
  `OFXPLUGIN_CUDA_RUNTIME_DIRS` cache entry that once put a 1.32 provider next to
  1.20.1's core; and `python3 tools/check-runtime.py <root-or-bundle>
  --require-cuda-major 12` fails a built bundle with both majors, the version
  pairing and the error the host raises. That command now runs by itself: the ONNX
  template's `build.bat` calls it on the bundle it just produced (12 unless
  `OFXPLUGIN_HOST_CUDA_MAJOR` names another host major, and skipped by name when
  the bundle carries no provider), and `windows.yml` repeats it on the runner, so
  the pairing fails a build instead of a host load. Read the major from the **provider**, not
  from the ONNX Runtime version number: the CUDA target is a property of the
  package, and a rebuilt provider can move without the version changing.

  Neither fact is visible to a fake host, which has nothing loaded and therefore
  always uses the bundle's own DLLs. The reproduction is
  `tools/host_cuda_probe.cpp`; the detail and the measured evidence are
  [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md).

  The host side stopped being an argument on 2026-11-26. Compiled here and run
  against Resolve Studio 21.2.0.5's folder, the probe loads all sixteen CUDA
  DLLs that sit beside `Resolve.exe`, creates a context, and resolves every base
  name to Resolve's own file
  ([`host-cuda-probe.txt`](../evidence/2026-11-26-windows/host-cuda-probe.txt));
  the running `Resolve.exe`, listed from outside, holds seven of them plus the
  driver's `nvcuda64`
  ([`resolve-modules.txt`](../evidence/2026-11-26-windows/resolve-modules.txt)).
  So a host that owns the stack exists, it loads it before any plug-in runs, and
  `-DOFXPLUGIN_CUDA_FROM_HOST=ON` is the only shape that does not put a second
  module of the same name into its process.
- **Check the provider's import table per ORT version.** `dumpbin /dependents` on
  the provider you ship, looking for `onnxruntime.dll`. Neither the 1.20.1 nor
  the 1.32.0 provider imports it, which is what makes the `onnxruntime-ofx.dll`
  sidecar work with all 30 files beside it in the first case and 16 in the
  second **[B]**; a later provider that does import it needs the sidecar shape
  instead (`ai-architectures-and-runtimes.md` §2).

**Bundling NVIDIA's libraries is a redistribution, not a build detail.**
`-DOFXPLUGIN_BUNDLE_CUDA_PROVIDER=ON` copies four things that are not ONNX
Runtime's and not this repository's: **cuDNN 9**, **cuBLAS/cuBLASLt**, **cuFFT**
and the **CUDA runtime (cudart)**. Measured on the 1.32.0 bundle: 16 files,
1540 MB, of which ONNX Runtime contributes exactly two - the provider and
`onnxruntime_providers_shared` (**[V]**, the bundle row in
[`testing-layers.md`](testing-layers.md) §13 and its evidence log). ONNX
Runtime's GPU package **does not ship** cuDNN, cuBLAS, cuFFT or cudart at all:
those come from NVIDIA's redistributables, which is why the
build takes `-DOFXPLUGIN_CUDA_RUNTIME_DIRS` and why the missing-file warnings
name them one by one. A bundle built this way therefore carries **two licences
with different owners** - ONNX Runtime's MIT and NVIDIA's EULA - and the build
copies only the licence text that lies beside what it copies:

```
-- Licence texts in Contents/Resources: 2 file(s)
--   ONNXRuntime-LICENSE (1572 bytes, from .../ort-gpu-1.32.0)
--   NVIDIA-LICENSE (51 bytes, from .../cudnn-9.14/bin)
```

The build looks for `LICENSE*`, `LICENCE*`, `NOTICE*`, `ThirdParty*`,
`THIRD_PARTY*` and `EULA*` in the ONNX Runtime root and in the directories
`OFXPLUGIN_CUDA_RUNTIME_DIRS` and their parents, and copies what it finds into
`Contents/Resources`. The `ONNXRuntime-` or `NVIDIA-` prefix records where the
file was found, not what it covers - two files called `LICENSE` would otherwise
overwrite each other, and the text inside decides what it covers. When nothing
is found the line says so (`none found beside the runtime. Add ONNX Runtime's
LICENSE and ThirdPartyNotices.txt, and NVIDIA's EULA, before shipping`) rather
than leaving the bundle silent.

What the build cannot do is invent the text. An ONNX Runtime release zip
unpacked into `include/` and `bin/` may keep its notices in the archive root
rather than in the unpacked tree, and NVIDIA's EULA ships with the toolkit
installer, not with the redistributable DLLs - both are still added by hand, and
the configure line is what makes the omission visible instead of implicit. The
copy is a configure step, not a `POST_BUILD`: the texts do not depend on a
successful compile, and a configure on a machine without a CUDA toolkit has to
report the same bundle contents.

Two runtime pairs have been measured on this machine, and they differ by more
than a factor of one and a half: the same 16 files, a different CUDA generation
behind them. Both numbers are true of their pair, so quote the pair:

| Runtime beside the `.ofx` | Files | Size | Where |
|---|---|---|---|
| ONNX Runtime 1.20.1, cuDNN 9 + cuBLAS 12 + cuFFT | 16 | 2686 MB | [`testing-layers.md`](testing-layers.md) §13, [`onnx-inference-cuda.log`](../evidence/2026-11-25-windows/onnx-inference-cuda.log) **[V]** |
| ONNX Runtime 1.32.0, cuDNN 9 + cuBLAS 13 + cuFFT 12 + cudart 13 | 16 | 1540 MB | [`testing-layers.md`](testing-layers.md) §13, [`onnx-inference-cuda-1.32.log`](../evidence/2026-11-25-windows/onnx-inference-cuda-1.32.log), re-measured 2026-11-26 **[V]** |

What grows between them is NVIDIA's, not this repository's: in the 1.32.0 bundle
cuDNN's ten components are 575 MB, cuBLASLt 458 MB and cuFFT 271 MB, while ONNX
Runtime's own provider is 187 MB and its shared provider 0.02 MB. The 2.4-2.7 GB
budget in [`onnx-and-tensorrt.md`](onnx-and-tensorrt.md) §4 is the CUDA 12 pair
with a **[B]** provider figure; a bundle built against CUDA 13 is 1.5 GB, and a
reader who takes 2686 MB for the current build over-reserves by 1.1 GB.

What NVIDIA's terms actually grant (**[B]**, the two agreements, not a lawyer's
reading):

| Document | What it says about this bundle |
|---|---|
| [CUDA Toolkit EULA](https://docs.nvidia.com/cuda/eula/index.html) | grants the right to *"distribute those portions of the SDK that are identified in this Agreement as distributable, as incorporated in object code format into a software application"*, and Attachment A names this bundle's files: `cudart`, `cublas`, `cublasLt`, `cufft` - *"including certain variations of these files that have version number or architecture specific information embedded in the file name"*, which is what makes `cudart64_13.dll` a redistributable rather than an unnamed derivative. `cufftw.dll` is on the list too; the provider never imports it, so the bundle does not carry it |
| Distribution requirements, same document | the application *"must have material additional functionality, beyond the included portions of the SDK"*; the distributable portions *"shall only be accessed by your application"*; and the terms you distribute under must be consistent with the EULA and protect NVIDIA's IP |
| [cuDNN SLA](https://docs.nvidia.com/deeplearning/cudnn/latest/reference/eula.html) | same shape, naming the libraries explicitly: *"the following portions of the SDK are distributable under the Agreement: the runtime files .so and .dll"* |

Three consequences for whoever ships such a bundle:

- **The plug-in has to be the application, and the host is the process.** "Material
  additional functionality" is about what the end user gets, not about the file
  count: a bundle that is only NVIDIA's runtime plus a passthrough would not meet
  it, and an effect that computes something is exactly the case the grant exists
  for. The requirement that the distributable portions *"shall only be accessed by
  your application"* is the one a host complicates - the DLLs sit inside the host's
  process and the host may load its own copy first, which is the collision case the
  bullets above describe. That is an argument for
  `-DOFXPLUGIN_CUDA_FROM_HOST=ON` beyond size.
- **The notice NVIDIA asks for is narrower than people assume.** The quoted EULA
  notice - *"This software contains source code provided by NVIDIA Corporation."* -
  is required for modifications and derivative works of **sample source code**.
  Redistributing the runtime DLLs unmodified is covered by the distribution grant
  above, not by that sentence. Copying the EULA into the bundle is the safe habit
  and the one to follow; claiming the sentence itself is mandatory would be wrong.
- **Version pinning becomes a licence fact.** Replacing `cudnn64_9.dll` with the
  host's cuDNN 9.x changes which vendor build the user runs; a bundle that pins
  its own is reproducible, and the EULA's consistency requirement is what makes
  the pinning worth stating in the release notes rather than in a build script.

None of this is in the OFX specification, which says nothing about licences,
signing or redistribution - the loader is the operating system and the terms come
from the vendor (**[I]**, from the spec reading in §1). What this repository does
and does not do:

- `THIRD_PARTY_NOTICES.md` names ONNX Runtime's MIT obligation and, since this
  section existed without it, the CUDA row there said the bundle never carries a
  CUDA runtime - true for the templates before the provider bundle, false for
  every `BUNDLE_CUDA_PROVIDER=ON` build. The row now states both cases.
- **The build copies no licence file.** `_cuda_files` in
  `templates/onnx-inference-ofx/CMakeLists.txt` is a list of DLLs and `.so` files;
  nothing in it is a `LICENSE`, `EULA` or `THIRD_PARTY` text. If you ship this
  bundle, add the two licence texts beside the binaries (a `Resources/`
  subdirectory the host never loads is the natural place) and record the four
  library versions in the release notes - the build log prints the file count and
  size, not the versions.
- **Nothing here has been reviewed by a lawyer, and no NVIDIA redistribution
  agreement beyond the public EULA exists in this repository.** The table above is
  a summary of two public documents with links, not advice.

**One thread budget per node.** An inference plug-in may hold several sessions
inside a host that holds several instances of the node. Build every session
through one `MakeSessionOptions(threads)` helper, make the intra-op count a
parameter whose default is what the plug-in already did, remember the value each
session was created with, and print it in the status label in **both** provider
modes — the label is the only evidence the setting reached the session
(`--label-param` asserts it). It is a CPU-path control: measured on one RTX 3060
it changed nothing under CUDA **[B]** (`onnx-and-tensorrt.md` §4).

**Install locations.**

| Host | Windows | Linux |
| --- | --- | --- |
| Resolve / Nuke / SCRATCH / Natron | `%COMMONPROGRAMFILES%\OFX\Plugins` | `/usr/OFX/Plugins` |
| User-local | `OFX_PLUGIN_PATH` (semicolon-separated) | `OFX_PLUGIN_PATH` (colon-separated) |

**Code signing is not in the spec, and it is not in this repository either.**
Neither macOS notarisation nor Windows Authenticode appears in the OFX
specification or in the packaging rules: the loader is the operating system, and
it has its own opinion about a plug-in the user just downloaded. What a user
meets, and what belongs next to a release:

- **macOS.** An unsigned, quarantined `.ofx.bundle` is refused on first load;
  `openfx-misc` tells users to run `xattr -dr com.apple.quarantine` on the bundle,
  which is a workaround, not a distribution strategy. The path is a Developer ID
  signature with the hardened runtime plus a notarisation ticket, and the bundle
  lives inside the host's process, so the host's own signature does not cover it
  (**[B]**, Apple's notarisation guide and the reference project's install
  notes).
- **Windows.** Authenticode (`signtool sign /fd sha256 /tr ... /td sha256`) is what
  keeps SmartScreen from calling a fresh download "unrecognised". An unsigned
  `.ofx` still loads, so the cost is user trust rather than a refused load
  (**[B]**, Microsoft's SignTool reference); a host that enforces signed plug-ins
  turns that into a load failure, and no such host has been tested here.
- **More than one binary inside the bundle.** A universal macOS bundle needs each
  slice signed, and a bundle that ships DLLs or `.so` files next to the `.ofx`
  needs those signed as well if the platform's loader checks them (**[I]**).
- **Not verified here:** nothing in this repository is signed or notarised, it
  holds no certificate, and no run has passed through a release channel. The two
  platform paragraphs are the operating systems' rules, not observations of a
  bundle.

**Firewall your symbols.** Set `CXX_VISIBILITY_PRESET hidden`,
`CUDA_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`. The CUDA one is
separate and easy to miss: `CXX_VISIBILITY_PRESET` does **not** apply to the `.cu`
translation unit, so every `extern "C"` launcher stays exported. Measured on this
template: without `CUDA_VISIBILITY_PRESET` 5 `T` symbols, with it 2 **[V]**; the
version script then removes the weak `std::` helpers as well.

Every plugin statically links its own Support library. With default visibility,
two plugins in one host process export identical mangled symbols and the loader
binds one plugin's `OFX::PropertySet` to the other's; the symptom is a crash that
appears only when a *second* plugin is installed.

Visibility is an ELF mechanism. Under MSVC both presets are no-ops and the export
set comes from the Support library's `OfxExport` (`__declspec(dllexport)`), which
still yields exactly two exported names, verified with `dumpbin /exports` on a
built Windows bundle **[B]** ([`evidence`](field-evidence.md) §3);
`exports.map` is guarded by `if(UNIX AND NOT APPLE)` for the same reason.

**CUDA runtime linkage.** Default to `CUDA::cudart` (shared): the host already has
a GPU stack. `cudart_static` removes the load-time dependency but gives each
plug-in its own runtime instance, and mixing the two in one process is undefined
(`cmake-and-linking.md` §6).

On **Windows with CUDA 13**, do not claim a `cudart64_*.dll` import without
checking: `dumpbin /dependents` on a built template lists KERNEL32, MSVCP140,
VCRUNTIME140 and the UCRT, no CUDA runtime DLL, while the device code is present
(seven cubins) and the module loads. The runtime resolves through the hybrid
runtime and the string in the image is `cudart_hybrid64.dll` **[B]**
([`evidence`](field-evidence.md) §3). The portable statement is "device
code present, runtime resolved at load"; the import list is what a host depends on.

**Architecture targets.** `CMAKE_CUDA_ARCHITECTURES` defaults to
`75;80;86;89;90` plus `100;120` when the toolkit is ≥ 12.8. A bare number emits
SASS **and** PTX, so the newest architecture still JITs on newer hardware. CUDA
13.0 removed offline compilation and library support below `sm_75` (Maxwell,
Pascal, Volta); Turing is the floor, and an nvcc deprecation warning means the
list is stale.

The CMake trap has two halves. `enable_language(CUDA)` initialises
`CMAKE_CUDA_ARCHITECTURES` itself, so a project default guarded by
`if(NOT DEFINED ...)` never runs. Capturing the variable before
`enable_language()` fixes only the first configure: the value is then cached, so
the second configure keeps nvcc's single architecture — measured: `-- CUDA
architectures: 75` and one `--generate-code`, where a clean configure had seven
**[B]** ([`evidence`](field-evidence.md) §3). The template records nvcc's
default once in an internal cache variable, only on a configure that saw no user
value, and treats a differing cached value as a user choice; a tree configured
before that marker existed is handled by reading the cache entry before
`enable_language()` and treating `UNINITIALIZED` as a `-D` value. Checked on CMake
3.31.6 and 4.4.5. A configure proves the selection only — the STATUS line and the
`--generate-code` flags. Whether the cubin loads is a host question, and whether
the kernel runs needs hardware.

**Do not bake the toolkit path into the bundle.** By default CMake records the
`cudart` directory in `RUNPATH`. The template sets `INSTALL_RPATH` with
`BUILD_WITH_INSTALL_RPATH ON`, so the `.ofx` resolves a `libcudart` beside it and
otherwise uses the host's runtime. The token is platform-specific: `$ORIGIN` for
ELF, `@loader_path` for dyld (`cmake-and-linking.md` §5); the wrong one builds fine
and cannot resolve at load.

**MSVC.** The CUDA host compiler must use the same runtime library as the rest of
the binary; CMake propagates it. Do not hand-add `-Xcompiler /MD` to a Debug build
(it must be `/MDd`).

**Separable compilation.** Only when device functions cross `.cu` files; then also
set `CUDA_RESOLVE_DEVICE_SYMBOLS ON`.

**`cmake --install` writes the layout the host scans.** The reference template
carries an `install(TARGETS ...)` rule, so `cmake --install build --prefix P`
writes `P/OFX/Plugins/ReferenceGain.ofx.bundle/Contents/<arch>/ReferenceGain.ofx`
- the same path a host looks along in §1, whether `P` is `/usr`, `/Library` or
`%COMMONPROGRAMFILES%`. Both `RUNTIME` and `LIBRARY DESTINATION` are given on
every platform because a `MODULE` target is a `RUNTIME` artefact under MSVC and a
`LIBRARY` artefact elsewhere: a Windows-only `RUNTIME` line configures and then
fails with `install TARGETS given no LIBRARY DESTINATION for module target`, and
an `if(WIN32)`/`else()` pair that names one each is the same mistake with more
lines.

The rule exists because the build tree is not an install, and the test exists
because nothing said so. `templates/minimal-ofx/test/run-tests.sh` (step `[4/5]`)
and `run-tests.bat` (step `[4/4]`) install into a throwaway prefix, then load and
render from the installed copy: a resource that only the build directory wrote, a
path relative to the build tree, or an RPATH that still points into `build/` all
survive a build-tree run and fail this one. Measured on Windows, 2026-11-27
([`install-load.log`](../evidence/2026-11-27-windows/install-load.log)): the
installed copy loads, renders and passes **90 checks, 0 failed**, and the runner
deletes the prefix afterwards. The other templates have no install rule yet, so
their bundles are still copied by hand - stated here rather than implied by the
rule existing in one place.

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[I]** inferred from a header, a specification or documented behaviour, not demonstrated here
- **[V]** verified in this repository - the command ran here
