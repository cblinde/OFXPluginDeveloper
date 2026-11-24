# Region of definition, region of interest and the 1.5.1 render hints

RoD/RoI arithmetic, tile interaction and the render hints a host may or may not set.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 390-435 -->
<!-- SKILL.md lines 599-647 -->
## 7. RoD and RoI

Both are in **canonical coordinates**, which are the project's coordinate space —
not the image's pixel space at the current render scale.

### Region of Definition

Override `getRegionOfDefinition()` only when the effect changes the output
extent: a generator, a crop, a transform, a lens distortion that grows the frame.
Return the new rect in canonical coordinates and `return true`. For a per-pixel
colour operation, do nothing — the default (union of inputs) is correct, and
overriding it wrongly clips the image.

### Region of Interest

For anything reading neighbours (blur, sharpen, edge detect, morphology,
optical-flow-guided warp):

```cpp
void MyEffect::getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args,
                                    OFX::RegionOfInterestSetter& rois)
{
    // radius expressed in pixels AT THE CURRENT RENDER SCALE
    const double rx = _radius * args.renderScale.x;
    const double ry = _radius * args.renderScale.y;

    OfxRectD roi = args.regionOfInterest;
    roi.x1 -= rx; roi.x2 += rx;
    roi.y1 -= ry; roi.y2 += ry;

    rois.setRegionOfInterest(*_srcClip, roi);
}
```

The classic bug: adding the pixel radius directly to canonical coordinates.
At `renderScale = 0.5` the host gives you a half-size image, so you asked for
twice the neighbourhood you need — or, at 2× proxy scale, too little, and seams
appear at tile boundaries only in proxy mode.

Second classic bug: expanding the RoI but then addressing the kernel relative to
the *render window* rather than the *fetched image bounds*. The image you get back
may be larger than the window (that is the point of RoI), and its origin will
differ.

---

## 8. Region of interest, and the 1.5.1 render hints

If output(x,y) needs input(x±r, y±r):

1. Leave `kOfxImageEffectPropNoSpatialAwareness` **unset** on the descriptor — for
   a neighbourhood effect that is the correct declaration ("I cannot render
   without spatial awareness"). Override `getRegionsOfInterest()` and expand the
   source RoI by `r` in *canonical* coordinates, multiplying by `args.renderScale`
   (see the deep dive).
2. Fetch the source with the expanded region and remember that the image's
   **bounds** may differ from the render window. The kernel's boundary check is
   against the image bounds.
3. Test two offset windows, in both pointer origins and both stride signs, and
   assert the overlay's *position and orientation*, not that bytes changed. A
   pass-through copy is orientation-agnostic; a box in the top half of the tile
   catches a mirror. `testkit`'s `setImageOrigin`/`setNegativePitch` build all four
   layouts ([`testkit/README.md`](../testkit/README.md) says what each proves).

The two classic failures: adding a pixel radius directly to canonical coordinates
(at `renderScale = 0.5` you ask for twice the neighbourhood; at 2× too little, and
seams appear only in proxy mode), and expanding the RoI but addressing the kernel
relative to the render window rather than the fetched image bounds.

A tiled plug-in that draws has both bugs at once: the halo decides what the model
sees, the overlay's placement decides what the user sees. The drawing half is
[`ai-architectures-and-runtimes.md`](ai-architectures-and-runtimes.md) §7 and
[`ofx-lifecycle-and-threads.md`](ofx-lifecycle-and-threads.md) §13.

**`kOfxImageEffectPropNoSpatialAwareness` (1.5.1), the right way round.** Set it
`"true"` on the descriptor only if the effect is per-pixel *or* can disable its
spatial parameters on request. The host then sets the in-args property to `"true"`
for renders that must not move pixels (LUT generation, some scopes), and your
`render()`/`beginSequenceRender()` must honour that by disabling the spatial part.
If you leave it `"false"`, the host **bypasses** the effect for those renders —
correct for a blur, wrong for a gain, and invisible until a user generates a LUT.

**`kOfxImageEffectPropThumbnailRender` (1.5.1).** The host marks low-resolution
UI renders. For a model plug-in this is the difference between a responsive
timeline and a stall: return identity (or draw a badge) instead of running
inference on a 208×117 thumbnail, and disable temporal inputs in
`getFramesNeeded()` for the same render. Do not change the *result* for a
full-resolution render based on this flag.

**`kOfxImageEffectPropCPURenderSupported` (1.5.1)** belongs in the same decision:
a plug-in that can only run on the GPU says so once, instead of throwing from
`render()` on every CPU render a host attempts.

---
