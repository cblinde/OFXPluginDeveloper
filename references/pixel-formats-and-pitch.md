# Pixel formats, pitch and image properties

The one pitch formula, why width*sizeof(pixel) is wrong, the component and depth matrix, and how to draw into a frame.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 312-389 -->
<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 751-790 -->
## 6. Memory layout, pitch and pixel formats

### The one formula

For an image with bounds `[x1,x2) × [y1,y2)`, data pointer `p`, row stride `s`
bytes, and `b` bytes per pixel:

```
address(x, y) = p + (y - y1) * s + (x - x1) * b
```

`y - y1` and `x - x1` are **not** `y` and `x`. Bounds frequently do not start at
zero: a generator with a centred RoD, a filter whose input is padded, a host
rendering a sub-window all produce negative or non-zero origins.

### Why `width * sizeof(pixel)` is wrong

Hosts pad scanlines. Concretely, RGBA32F is 16 bytes/pixel:

| Width | Naive row size | Host row size (256 B aligned) | Error if you assume naive |
| --- | --- | --- | --- |
| 1920 | 30 720 B | 30 720 B | none — which is why this bug survives testing on HD |
| 1919 | 30 704 B | 30 720 B | 16 B/row ⇒ 16 px shear after one row |
| 208 (thumbnail) | 3 328 B | 3 328 B | none |
| 117-wide odd tile | 1 872 B | 1 920 B | 48 B/row ⇒ visible skew |

And a stride can be **negative**, for CPU images only: `ofxImageEffect.h`
allows a flipped image to be represented by pointing at the last row and using
`-rowBytes`, and explicitly excludes CUDA/Metal/OpenCL Buffers (OpenCL Images
have no row bytes at all). Signed 64-bit arithmetic handles it; `size_t` does
not.

### Component and depth matrix

| Depth | Constant | Bytes/component | Notes |
| --- | --- | --- | --- |
| 8-bit | `kOfxBitDepthByte` | 1 | Video-range vs full-range is a host concern; ask, don't assume |
| 16-bit | `kOfxBitDepthShort` | 2 | Integer; 65535 = 1.0 convention |
| half | `kOfxBitDepthHalf` | 2 | `__half`, needs `sm_53`+; watch NaN/Inf on extremes |
| float | `kOfxBitDepthFloat` | 4 | The sane default for grading and for GPU work |

| Components | Constant | Pixel size at 32f |
| --- | --- | --- |
| RGBA | `kOfxImageComponentRGBA` | 16 B |
| RGB | `kOfxImageComponentRGB` | 12 B (no alpha!) |
| Alpha | `kOfxImageComponentAlpha` | 4 B |

Declare what you support in `describe()` (`addSupportedBitDepth`) **and** per
clip (`addSupportedComponent`), and reject anything else in `render()` with
`kOfxStatErrUnsupported`. If you accept RGB and then read `p[3]`, you read the
next pixel's red — a bug that looks like a colour shift, not a crash.

### Window vs bounds vs RoD

| Concept | Property | Meaning |
| --- | --- | --- |
| Bounds | `kOfxImagePropBounds` | Pixels actually present in this image |
| Render window | `kOfxImageEffectPropRenderWindow` (in-args) | The subset you must write this call |
| RoD | `kOfxImagePropRegionOfDefinition` | The region the image *conceptually* covers |
| RoI | `getRegionsOfInterest` args | What you ask the host to provide on input |

`renderWindow ⊆ bounds` must hold. When tiling is enabled you get many render
calls per frame, each a slice; when it is disabled you get one. The template
declares `setSupportsTiles(false)`, so the window equals the bounds — and
`ImageProcessor::process()` asserts that, which is a cheap safety net.

### Allocating your own device memory

Prefer `OFX::ImageMemory` (wraps `OfxMemorySuiteV1`) when the allocation should be
visible to the host's memory accounting, e.g. a cached result the host may want
to purge. Use `cudaMalloc`/`cudaMallocAsync` for pure scratch. Either way:

- Allocate **once per sequence or per size change**, never per frame.
- `cudaMalloc` is synchronising; it will show up as a stall in Nsight.
- Free in `endSequenceRender()` or on size change; keep the destructor simple.

---

## 13. Drawing into the frame (boxes, labels, plates)

An overlay is still a write into a host buffer, so it fails in the same three
places a filter does — plus one of its own.

**Address through the destination, not the source.** The output image has its own
base pointer, its own `kOfxImagePropRowBytes` (signed, possibly negative) and its
own bounds. A rect computed in image coordinates must be offset by
`window − bounds`, and every row reached as
`dstBase + (y − window.y1) * dstPitch`. Reaching row `y` at `base + y * rowBytes`
is correct; assuming the first row is the top of the buffer is not, and a
negative-pitch destination flips an overlay that ignores it.

**Clip to the render window and to the bounds.** A box can lie partly outside the
window the host asked for (proxy, tile, RoI). Clip in integer coordinates before
writing: a fill that runs one row past the window is a write into another clip's
image in some hosts, and the crash appears in the host, not in your plug-in.

**Premultiplied images.** OFX images are usually premultiplied — the clip's
`kOfxImageEffectPropPreMultiplication` is `kOfxImagePreMultiplied`
(`ofxImageEffect.h:978-992`). An overlay colour must then be composited
premultiplied: `dst = dst * (1 − a) + colour * a`, and the box or plate itself
must be written as `colour * a`, not `colour`. Writing the straight colour makes
a soft matte glow — the opposite of what a filter must do with its own alpha,
which is to copy it through untouched.

**Never change alpha you were not asked to change.** A label on an opaque frame
is fine; on a frame with a matte, either keep the source alpha and draw colour
only where alpha is already 1, or composite as above and leave the result
premultiplied. Decide which, write it in the UI text, and keep it constant across
tiles.

**A font without a GUI.** `stb_easy_font` (public domain, single header) renders
glyphs as quads in screen space: no FreeType, no font file, no host UI. A filled
rect plus that is a complete label in about 150 lines.

**Draw after inference, once per frame.** Not per tile, not per thread: the
overlay is not part of the model's receptive field, and drawing it before tiling
would feed it back into the model and change the result with the tile size.
