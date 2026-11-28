# Colour space and alpha

What the host hands you, premultiplication, the 1.5 colour APIs and where alpha lands.

<!-- ARCHITECTURE_AND_CUDA_PIPELINE.md lines 436-495 -->
## 1. Colour space and alpha

OFX had **no colour management** before 1.5, and that legacy behaviour is still
the default: unless both sides declare a style in
`kOfxImageEffectPropColourManagementStyle`, the host hands you pixels in whatever
working space it uses and expects the same space back. OFX 1.5 added the opt-in
contract in `ofxColour.h` (`kOfxImageClipPropColourspace`,
`kOfxImageEffectActionGetOutputColourspace`), and Resolve 21 implements it
(**[B]**, `host-resolve.md` §2). What the 1.5.1 tag here does and does not carry,
and what OFX 1.6 adds for *parameters* - the rule for the first colour parameter
any template gets - is in [`ofx-api-constants.md`](ofx-api-constants.md) §1.7. Either way the rules below hold;
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
  *provided* you do not touch alpha. This is settled for the gain in §2: it scales
  R, G and B and copies A, and an alpha scale has to be its own parameter.
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

## 2. What a gain may assume at each bit depth

A colour operation is only well defined once you say which number it multiplies.
The CUDA template's kernels make the assumptions below, and the harness measures
them; the point of writing them down is that a host author can disagree with a
stated assumption, but not with an unstated one.

| Depth | Bytes/pixel | What the value is | What the template does |
| --- | --- | --- | --- |
| RGBA32F | 16 | 0..1 nominal, >1 and negative legal | multiplies R, G, B (no clamp, no transform), copies alpha |
| RGBA16F | 8 | linear half float, same range as 32F | RGB to float, multiply, one round-to-nearest-even back; alpha copied as half |
| RGBA8 | 4 | **encoding undefined by OFX** | multiplies the stored R, G, B code value, rounds to nearest even, clamps to 0..255; alpha copied as a byte |

**8-bit: multiply the stored value, never linearise.** OFX does not say whether a
byte image is sRGB, Rec.709 or something a project invented, and a plug-in cannot
find out from the image alone - the colour management contract (1.5, opt-in) is
about *spaces*, not about what the bytes were encoded with. So a byte gain applies
its factor to the number the host stored, exactly as the host's own byte multiply
would. Converting to linear first and back would double-apply a transfer function
the plug-in was never told about; that is the same rule as §1.1, applied at the
quantisation step.

Three consequences worth knowing before you compare a byte render against a float
one:

- **Gain 1.0 is the identity, bit for bit.** The kernel multiplies and re-quantises
  every byte, so this is a property that has to be tested rather than assumed -
  the harness runs the whole 0..255 ramp and gets the input back.
- **The maximum difference from a float reference is half an LSB**, 0.5/255, plus
  whatever the tie rule does. Ties are the trap: round-half-to-even and
  round-half-away-from-zero disagree by one LSB whenever the product lands exactly
  on .5, which a gain of 0.5 on odd bytes does for every pixel. The kernel and its
  reference both use round-to-nearest-even (`__float2int_rn`); the harness's first
  version of the negative-pitch case used `lround` and was 35 bytes wrong out of
  340 for exactly that reason.
- **Overflow saturates, it does not wrap.** A gain of 8 on a byte is 255; 200 * 8
  is 1600, and 1600 & 0xff would be 64, which is the classic 8-bit wrap bug.

**16F: the same arithmetic as 32F, quantised once.** Both operands are halves, so
their product is exact in float, and the single conversion back is the same
rounding `__hmul2` applies. The harness compares the kernel against a float
reference with **no tolerance** and gets bit equality; against a true float result
the error is at most one half-precision ulp.

**A gain does not scale alpha. Decided, and now true of four of the five templates:**
the reference, OpenCL, Vulkan and inference templates multiply R, G and B and copy A. The
CUDA template adds `gainAlpha` as the separate control for the case where moving coverage
is what you meant. The
arithmetic settles it once the composite is written out. On premultiplied data the
stored colour is a·C; a gain of k that scales the stored RGB produces k·a·C, and the
un-premultiplied colour that a viewer sees is (k·a·C)/a = k·C with the coverage
still a. Scale alpha by the same k and that ratio becomes k·a·C / k·a = C: the colour
is untouched and only the coverage moves. That is an opacity control wearing a
gain's name - it darkens the result over any background and changes what the next
composite does, which is precisely what §1 tells a colour operation not to touch.
So `gain` multiplies R, G and B and copies A.

Where an alpha scale is genuinely wanted it has to be visible, so it gets its own
parameter rather than riding along: the CUDA template declares `gainAlpha` (default
1.0, i.e. off) next to `gain`, and its kernels take one factor per channel because
the fourth factor has a separate source. The reference plug-in and the OpenCL
kernels do not offer one, and their `gain` hint says "Multiplies R, G and B; alpha
is left alone".

The harness already stated this assumption independently of the plug-ins: `--set
gain=...` is read as "scale RGB, leave alpha" (`testkit/README.md`, "The
`--set` flag"), and the `alpha is unchanged` check fails loudly when a plug-in
disagrees. Until 2026-11-26 the reference template and both OpenCL kernels failed
it - all four channels were multiplied, and the OpenCL suite's own reference frame
had the bug baked into it, so the two mistakes agreed with each other. Both now
pass, the OpenCL suite asserts alpha per handover (Buffers, Images, image window)
instead of trusting a scaled reference frame, and the CUDA suite's
`--set gain=2.0` run covers the third path
([`alpha-gain.log`](../evidence/2026-11-26-linux/alpha-gain.log)).

What the depth kernels have in common is that none of them touches
`kOfxImageEffectPropPreMultiplication` - the host's declaration stays the truth.

**What is measured, and what is not.** The table's arithmetic and addressing are
**[V]** on the CPU through the shim's emulation (162 half shapes, 164 byte cases,
0 wrong) - a real device was never available on this machine. Quantisation against
a *host's* byte comparison, and every throughput number, are **[U]**.

---

## Provenance labels

Tags in this document mean exactly what they meant in the document it came from:

- **[B]** external evidence: a run or a report from another machine or project
