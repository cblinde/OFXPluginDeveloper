# OFX API constants and verified header quirks

Every property string, depth support name and named trap, with the quirks that cost debugging time.

<!-- SKILL.md lines 262-404 -->
<!-- BEST_PRACTICES_AND_PERFORMANCE.md lines 89-131 -->
<!-- PATTERN_INDEX.md lines 126-144 -->
## 1. OFX constant reference (verified against OFX 1.5.1 headers)

### 1.1 Capability properties (plugin descriptor)

All are `string × 1` except where noted. Every row was resolved against the pinned
1.5.1 tree **[V]**:

| Constant | Header | Notes |
| --- | --- | --- |
| `kOfxImageEffectPropCudaRenderSupported` | `ofxGPURender.h` | `"false"` default for a plugin |
| `kOfxImageEffectPropCudaStreamSupported` | `ofxGPURender.h` | `"true"` = you can use host streams; only then may the host set `...CudaStream` |
| `kOfxImageEffectPropOpenCLRenderSupported` | `ofxGPURender.h` | OpenCL **Buffers** render |
| `kOfxImageEffectPropOpenCLSupported` | `ofxGPURender.h` | OpenCL **Images** render; then read `kOfxImageEffectPropOpenCLImage`, not `kOfxImagePropData` |
| `kOfxImageEffectPropMetalRenderSupported` | `ofxGPURender.h` | macOS; has a third value `"needed"` |
| `kOfxImageEffectPropOpenGLRenderSupported` | `ofxGPURender.h` | **C string**; third value `"needed"`; may flip at `kOfxActionInstanceChanged` since v1.4 |
| `kOfxImageEffectPropCPURenderSupported` | `ofxGPURender.h` | **C string**, default `"true"`; added in **1.5.1** |

`kOfxImageEffectPropCPURenderSupported` is the modern way to say "GPU-only": set
`"false"` on the descriptor only when `render()` genuinely has no host-pointer
path. A host in OpenCL/Metal mode will then skip the effect instead of handing it
CPU images it cannot read — which is correct, which is why the templates, which do
have a fallback, set it to `"true"` explicitly rather than leaning on the default:
a declaration a contract can read beats one a host has to assume. All three
templates also declare `kOfxImageEffectPropNoSpatialAwareness` as `"true"`, and
`testkit/contract_main.cpp` asserts both strings and then renders once with
`kOfxImageEffectPropThumbnailRender` and `kOfxImageEffectPropNoSpatialAwareness`
set on the in-args, requiring the same frame as a full render.

**A host that does not know the property makes the write throw**, and an
unguarded write takes the whole descriptor with it. Resolve Studio 21.2.0.5
returns a status that `PropertySet::propSetString` turns into
`OFX::Exception::PropertyUnknownToHost`, logs `Create context ... failed` 76
times and never calls `render()`, while the render job still ends `Complete`
with the source unchanged (**[V]**, 2026-11-27,
[`opencl-host-resolve.log`](../evidence/2026-11-27-windows/opencl-host-resolve.log)).
Every template that writes the property guards it, so a host that ignores the
write sees the property's default (`"true"`) rather than an unavailable plug-in.

Four traps follow from this table:

- **The per-render flags are ints; these are strings.** `...CudaEnabled`,
  `...OpenCLEnabled`, `...MetalEnabled` are `int`; use `propGetInt` for those and
  `propGetString` here. Mixing them up returns `kOfxStatErrBadHandle` or garbage.
- **The OpenCL mapping is inverted in one doc block.** The property blocks are
  unambiguous (`...OpenCLRenderSupported` = Buffers, `...OpenCLSupported` =
  Images), but the `kOfxImageEffectPropOpenCLEnabled` block labels them the other
  way round. Trust the two property blocks, and discriminate at render time by
  testing whether `kOfxImageEffectPropOpenCLImage` is set.
- **`kOfxImagePropRowBytes` is not guaranteed for OpenCL Images** — "OpenCL
  Images do not have the concept of row bytes". Pitch arithmetic is a
  Buffers/CPU path rule.
- **Metal texture rendering is not in 1.5.1.** `kOfxImageEffectPropMetalTextureSupported`
  is an open proposal (`openfx#283`, Oct 2026), not a header constant. Do not
  reference it in shipped code.

### 1.2 Per-render in-args (Render / BeginSequenceRender / EndSequenceRender only)

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImageEffectPropCudaEnabled` | `int` | `1` = image data pointers are CUDA device pointers |
| `kOfxImageEffectPropCudaStream` | `pointer` | `cudaStream_t`; only set when both sides support streams |
| `kOfxImageEffectPropOpenCLEnabled` | `int` | OpenCL buffers/images in use |
| `kOfxImageEffectPropOpenCLCommandQueue` | `pointer` | `cl_command_queue` |
| `kOfxImageEffectPropMetalEnabled` | `int` | Metal buffers in use |
| `kOfxImageEffectPropMetalCommandQueue` | `pointer` | `id<MTLCommandQueue>` |
| `kOfxImageEffectPropNoSpatialAwareness` | `string` | **1.5.1**, optional: host asks for a render with no spatial effects |
| `kOfxImageEffectPropThumbnailRender` | `string` | **1.5.1**, optional: host considers this a UI thumbnail |

Both are read-only in-args with default `"false"`. `ThumbnailRender` is also
present in `GetRegionOfDefinition`, `GetRegionsOfInterest`, `GetFramesNeeded`,
`GetClipPreferences` and `IsIdentity`; `NoSpatialAwareness` is specified for
Begin/Render only. Read whichever the host offers where the answer changes the
action.

### 1.3 Image handle properties

| Constant | Type | Meaning |
| --- | --- | --- |
| `kOfxImagePropData` | `pointer` | base of the row at `bounds.y1` (the bottom; its origin is independent of the stride's sign); device pointer when CUDA/OpenCL/Metal enabled |
| `kOfxImagePropRowBytes` | `int` | scanline stride in bytes; **signed, may be negative for CPU images** |
| `kOfxImagePropBounds` | `int × 4` | window of valid pixels in this image |
| `kOfxImagePropRegionOfDefinition` | `int × 4` | RoD; may exceed bounds |
| `kOfxImageEffectPropPixelDepth` | `string` | `kOfxBitDepthByte` / `Short` / `Half` / `Float` |
| `kOfxImageEffectPropComponents` | `string` | `kOfxImageComponentRGBA` / `RGB` / `Alpha` |
| `kOfxImageEffectPropPreMultiplication` | `string` | premultiplied / unpremultiplied / opaque |
| `kOfxImagePropPixelAspectRatio` | `double` | non-square pixels are legal |

`kOfxImagePropRowBytes` is an `int` in the header and in
`OFX::Image::getRowBytes()`. Widen it to `long long` before multiplying by a row
index; keeping the arithmetic in `int` overflows on a large frame. Read the
accessor once per row and reuse it.

Writing back is the same arithmetic against the **destination** image, which has
its own base pointer, its own signed pitch and its own bounds, plus traps a copy
does not have. Short version in [`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md)
`onnx-and-tensorrt.md` §4, "Drawing into the frame: the overlay after the model"; full rules in
[`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) §4.

### 1.4 Support names for the depths

The strings above are the C API; the Support library spells them as an enum, and
the 8-bit case is the one every byte-capable plug-in gets wrong first:

| C value | Support name (`OFX::`) | Notes |
| --- | --- | --- |
| `kOfxBitDepthByte` | `eBitDepthUByte` | there is no `eBitDepthByte` |
| `kOfxBitDepthShort` | `eBitDepthUShort` | 16-bit integer |
| `kOfxBitDepthHalf` | `eBitDepthHalf` | |
| `kOfxBitDepthFloat` | `eBitDepthFloat` | what every template advertises |

Advertise a depth only when `render()` handles it. A host that hands over an
undeclared depth is not converting for you; the templates accept float only and
the harness proves that a byte image is refused.

### 1.5 Colour management (OFX 1.5)

`ofxColour.h` adds a colour-management contract that the pre-1.5 skill said did
not exist. The default is still `kOfxImageEffectColourManagementNone`, so legacy
behaviour remains valid — but it is now a **choice**, not a limitation.

| Constant | Set on | Meaning |
| --- | --- | --- |
| `kOfxImageEffectPropColourManagementStyle` | plugin descriptor, host descriptor, instance | `None` (default) / `Basic` / `Core` / `Full` / `OCIO`; the host picks the highest style both sides offer and records it on the instance |
| `kOfxImageClipPropColourspace` | clip instance | the colourspace the host actually supplied; cross-reference with `OfxColourspace_<clip>` when output matches an input |
| `kOfxImageEffectActionGetOutputColourspace` | action | where a colour-manipulating effect declares its output space |

Rules:

- **Do not opt in unless you convert through the host's config.** Declaring a
  style is a promise that you read `kOfxImageClipPropColourspace` and honour it.
  A gain that claims `Basic` and then ignores the property is worse than one that
  claims nothing.
- **If you stay at `None`, do not convert either.** The old rule still holds:
  never bake a transform you were not asked for, and expose an explicit
  "as-is / no conversion" default when an effect only makes sense in linear light.
- **Resolve 21 implements the 1.5 colour APIs** (**[B]**,
  [`host-resolve.md`](host-resolve.md) §2.9). Feature-
  detect the host's style; never infer it from the host name.

### 1.6 Named traps

- The clip property for the *unmapped* component string is
  `kOfxImageClipPropUnmappedComponents` (`ofxImageEffect.h`). There is no
  `kOfxImageEffectPropUnmappedComponents`.
- `kOfxStatGPUOutOfMemory` = `1001`, `kOfxStatGPURenderFailed` = `1002`, both in
  `ofxGPURender.h`, which `ofxsImageEffect.h` does **not** include. Include it
  explicitly.
- `getRegionOfDefinition` returns `bool` and takes an out-parameter:
  `virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments&,
  OfxRectD&)`. Return `true` only when you set the rect; `false` means "use the
  union of the input clips' RoD" (`ofxsImageEffect.h:1119-1127`).
- `kOfxImageEffectPropNoSpatialAwareness` on the **descriptor** means "this plugin
  *can* render without spatial awareness", not "this plugin has none". the header has the
  semantics; getting the polarity wrong makes a blur advertise itself as a LUT.

---

### 1.7 Colour management, and what OFX 1.6 will change here

OFX 1.5.1 already carries image and clip colour management, and the headers in
this tree are that tag (`.ofx-ref`, `OFX_Release_1.5.1`): `kOfxImageEffectPropColourManagementStyle`
with its five values (`None`, `Basic`, `Core`, `Full`, `OCIO`),
`kOfxImageEffectPropColourManagementConfig`,
`kOfxImageEffectPropColourManagementAvailableConfigs` and
`kOfxImageEffectPropDisplayColourspace`, all in `ofxColour.h` **[V]**. A plug-in
that declares `None` - which is what "every colour parameter is declared None"
means in practice: the *effect* takes no position on colour - gets images in the
host's working space and says so.

What is **not** in the 1.5.1 tag is any parameter-level colour property
**[V]**: `grep -r ColourManagement .ofx-ref/include` finds the image-effect
properties above and nothing named `kOfxParamPropColourManagement`. The 1.6
documentation defines exactly that property - three values, `None` (legacy,
default), `Managed` (values are ACES2065-1/AP0, scene-linear, **not** clamped to
0..1) and `SRGB` (display-referred UI colours, clamped) - and labels it
"introduced in 1.5.1", which the tag does not bear out: treat the property as
1.6-only until a 1.6 header says otherwise **[B]**
([parameter reference](https://openfx.readthedocs.io/en/latest/Reference/ofxParameter.html)).

1.6 itself is merged but unreleased as of November 2026: the project's own
status update lists the new C++ bindings, a machine-readable property metadata
system and several new properties as merged, with the release still pending
**[B]** ([ASWF TAC update, 2026-10-14](https://tac.aswf.io/meetings/2026-10-14/OpenFX_ASWF_TAC_Update_Oct_2026.pdf)),
and the `Color Managed Color Parameter` issue sits in the 1.6 milestone **[B]**
([milestone](https://github.com/AcademySoftwareFoundation/openfx/milestone/4)).

Decisions for this repository, so the next 1.6 question has an answer:

- **The templates stay on the 1.5.1 tag** until 1.6 is released. They declare
  `apiVersion` 1 and the contract asserts it; moving early would mean building
  against an unreleased tag for features no host can require yet.
- **No template has a colour parameter**, so there is nothing to declare today.
  `gain` is a double, `tileSize` and `halo` are ints, `provider` is a string, and
  the ONNX template has no model-path parameter at all: it reads
  `ONNX_MODEL_PATH`, else `Contents/Resources/model.onnx`, at instance creation.
- **The rule for the first colour parameter** that is added: if the value takes
  part in image processing (a key colour, a fill), declare `Managed` and expect
  ACES2065-1; if it is a UI colour drawn with the draw suite, declare `SRGB`;
  otherwise leave it `None`, which is the legacy behaviour every host already
  understands.
- **When 1.6 lands**, the work is mechanical and worth one commit: bump the
  `GIT_TAG` in the three `CMakeLists.txt`, re-run the contract (it asserts the
  API version and the three 1.5.1 capability properties), and add the colour
  declaration to whichever template just gained a colour parameter.

## 2. Verified quirks in the OFX 1.5.1 headers

Found by reading the headers during this pass. Each one has bitten somebody.

1. **`kOfxImageEffectPropCudaEnabled` is an `int X 1`, not a string.** **[S]**
   (`ofxGPURender.h`). Its neighbours — `...CudaRenderSupported` and
   `...CudaStreamSupported` — are `string X 1`. So in raw C:
   `propGetInt(host, inArgs, kOfxImageEffectPropCudaEnabled, &v)`, while the
   `...Supported` pair is `propGetString`. Mixing them up returns
   `kOfxStatErrBadHandle` or reads garbage.

2. **The GPU "can we?" properties do not follow one naming scheme.** **[S]**
   - CUDA: `kOfxImageEffectPropCudaRenderSupported`
   - Metal: `kOfxImageEffectPropMetalRenderSupported`
   - OpenCL **Buffers**: `kOfxImageEffectPropOpenCLRenderSupported`
   - OpenCL **Images**: `kOfxImageEffectPropOpenCLSupported` ← no `Render`

   The OpenCL pair is two capabilities, not a typo, and the
   `kOfxImageEffectPropOpenCLEnabled` doc block swaps their parentheticals — the
   property blocks in lines 693-704 of the header are the authority, and
   `kOfxImageEffectPropOpenCLImage` being set is the runtime discriminator
   (`ofx-api-constants.md` §1).
   Guessing by analogy produces a property the host has never heard of, and a
   plugin that silently never gets the GPU.

3. **Some property names and their string values disagree.** **[S]**
   - `kOfxImageEffectPropSupportsMultipleClipDepths` → `"OfxImageEffectPropMultipleClipDepths"` (no `Supports`)
   - `kOfxActionOpenGLContextDetached` → `"kOfxActionOpenGLContextDetached"` (value starts with `k`)
   If you ever compare against the literal string instead of the macro, you will
   be wrong. Always use the macro.

4. **`kOfxActionInstanceChanged` has no `ImageEffect` in it**, unlike its
   siblings `kOfxImageEffectActionGetRegionOfDefinition` etc. **[S]**
   `kOfxImageEffectActionInstanceChanged` does not exist. (This repo cited it
   incorrectly until this pass.)

5. **The GPU status codes are not in the core headers.** **[S]**
   `kOfxStatGPUOutOfMemory` (1001) and `kOfxStatGPURenderFailed` (1002) live in
   `ofxGPURender.h`, which `ofxsImageEffect.h` does not include. Include it
   explicitly or the compile fails on a plugin that looked fine in the examples.

---

## 3. By property ("which constant does what")

Only the ones that decide correctness; the complete table with header line
numbers is [`../SKILL.md`](../SKILL.md) §1.

| Constant | Type | Who sets it | What it decides |
|---|---|---|---|
| `kOfxImageEffectPropCudaRenderSupported` | string | you, in `describe()` | Whether you *can* accept device pointers (`.ofx-ref/include/ofxGPURender.h:555`) |
| `kOfxImageEffectPropCudaEnabled` | int | the host, per render | Where the pixels are **now** (`ofxGPURender.h:576`) |
| `kOfxImageEffectPropCudaStream` | pointer | the host | Whether you may return early (`ofxGPURender.h:617`) |
| `kOfxImagePropRowBytes` | int | the host | Stride in bytes; signed, may be negative (`ofxImageEffect.h:1353`) |
| `kOfxImagePropData` | pointer | the host | Base pointer for the image **bounds**, not the window |
| `kOfxImageEffectPropSupportsTiles` | int, default 1 | you | `false` = "give me full RoD"; not "I tile internally" (`ofxImageEffect.h:857`) |
| `kOfxImageEffectPluginRenderThreadSafety` | string, default instance-safe | you | How many renders may overlap (`ofxImageEffect.h:629`) |
| `kOfxImageEffectPropPixelDepth` / `…Components` | string | the host / clips | Which format you must accept or reject loudly |
| `kOfxStatGPURenderFailed` / `kOfxStatGPUOutOfMemory` | status | you | The codes the host understands for GPU failure (in `ofxGPURender.h`, not pulled in by `ofxsImageEffect.h`) |

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
- **[S]** verified by reading the OFX source
- **[V]** verified in this repository - the command ran here
