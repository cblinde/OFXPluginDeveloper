# Colour space and alpha

What the host hands you, premultiplication, the 1.5 colour APIs and where alpha lands.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 436-495 -->
## 8. Colour space and alpha

OFX had **no colour management** before 1.5, and that legacy behaviour is still
the default: unless both sides declare a style in
`kOfxImageEffectPropColourManagementStyle`, the host hands you pixels in whatever
working space it uses and expects the same space back. OFX 1.5 added the opt-in
contract in `ofxColour.h` (`kOfxImageClipPropColourspace`,
`kOfxImageEffectActionGetOutputColourspace`), and Resolve 21 implements it
(**[B]**, `host-resolve.md` §2.9). Either way the rules below hold;
the first one is what a declared style turns from advice into a contract:

1. **Never bake in a transform you were not asked for.** A "gain" that silently
   converts Rec.709 to linear is a bug in every project that already works in
   linear.
2. **Be explicit about what you assume, in the UI and the docs.** If your effect
   only makes sense in linear light (blurs, lens effects, physically-based
   compositing), expose a `Colour space` choice param with an explicit default of
   "as-is / no conversion".
3. **Parameters that are colour values** should be labelled with their space.

Common working spaces you will meet:

| Space | Where | Shape of the data |
| --- | --- | --- |
| Linear / scene-linear | Nuke (default), ACES projects | 0..1 nominal, values >1 legal |
| DaVinci Wide Gamut / Intermediate | Resolve colour-managed projects | log-ish, wide gamut |
| ACEScct | ACES pipelines | log with a linear toe |
| Rec.709 / sRGB | straight video, Edit page | display-referred, gamma encoded |
| Resolve "Timeline" space | Resolve, unmanaged project | whatever the clip is tagged as |

Consequence for CUDA code: **negative and >1.0 values are normal.** Do not clamp
in the kernel unless the user asked for it, do not use `saturate()` by reflex,
and do not use 8-bit intermediate maths. If you need a LUT, apply it in the space
it was built for.

**Premultiplication.** Check `kOfxImageEffectPropPreMultiplication` on the image
(`kOfxImagePreMultiplied`, `kOfxImageUnPreMultiplied`, `kOfxImageOpaque` — see
`ofxImageEffect.h` for the exact string values). Rules:

- Blur, glow, defocus, transform: only correct on **premultiplied** data. If
  you get unpremultiplied input, divide by alpha, process, multiply back.
- Colour-only operations (gain, lift/gamma/gain, matrix, LUT): safe on either,
  *provided* you do not touch alpha.
- Never write non-zero RGB where alpha is zero unless the host asked for
  premultiplied output — it produces halos on the next composite.

**Alpha quirks to expect.**

- Alpha may be absent entirely (`RGB`), in which case treat it as 1.0 for maths
  and never write it.
- Some hosts deliver "opaque" images where alpha exists but is constant; others
  deliver garbage alpha on the Color page where compositing is not the point.
  Use the premultiplication property, and verify per host/version rather than
  trusting a memory of how it used to behave.
- Resolve's Color page evaluates OFX in its own pipeline, and where alpha lands
  there has changed between releases. Test explicitly; see
  `references/host-resolve.md`.

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
