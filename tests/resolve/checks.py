# SPDX-License-Identifier: BSD-3-Clause
"""Pixel assertions for the Resolve proof of concept.

Pure Python on bytes: no numpy, no OpenImageIO, so this module runs and is
testable on any machine - which is why its self-test is the one part of the
Resolve path that can be trusted here. What it cannot do is prove that the bytes
came from a host render.

The checks answer four questions, in the order a wrong render fails them: is it
the size we asked for, is it a real image, is it not a constant, and does it
match the reference within the reference's own bound.
"""


def _bytes_of(image):
    return image.pixels


def check_image(image, expect=None):
    """Return the problems found in one image; an empty list is a pass."""
    expect = expect or {}
    problems = []
    pixels = _bytes_of(image)

    if expect.get("width") is not None and image.width != expect["width"]:
        problems.append(f"width {image.width} != {expect['width']}")
    if expect.get("height") is not None and image.height != expect["height"]:
        problems.append(f"height {image.height} != {expect['height']}")
    if expect.get("channels") is not None and image.decoder == "oiio" \
            and image.channels != expect["channels"]:
        # Only oiio reports the file's own channel count; the ffmpeg path
        # normalises to RGBA, so asserting channels there would test the decoder.
        problems.append(f"channels {image.channels} != {expect['channels']}")

    if not pixels:
        problems.append("no pixels")
        return problems

    if len(pixels) != image.width * image.height * image.channels:
        problems.append(f"{len(pixels)} bytes for {image.width}x{image.height}x{image.channels}")

    distinct = set(pixels)
    if len(distinct) == 1:
        problems.append("uniform frame: every byte is the same value")
    # Colour channels only: an opaque black frame is 0,0,0,255, so a plain
    # max() over the bytes reports "not black" for the one frame most likely to
    # mean the plug-in never ran.
    if _colour_max(pixels, image.channels) == 0:
        problems.append("completely black frame (alpha ignored)")

    return problems


def _colour_max(pixels, channels):
    """The largest value across RGB, skipping alpha."""
    if not pixels:
        return 0
    if channels <= 0:
        return max(pixels)
    limit = min(3, channels)
    best = 0
    for index in range(0, len(pixels), channels):
        for component in range(limit):
            value = pixels[index + component]
            if value > best:
                best = value
    return best


def compare_to_reference(image, reference, tolerance=None):
    """Compare two decoded images; returns (problems, stats) in 0..255 units."""
    tolerance = tolerance or {"max_mae": 0.0, "max_pixel": 0.0}
    problems = []

    if (image.width, image.height) != (reference.width, reference.height):
        return ([f"size {image.width}x{image.height} != reference "
                 f"{reference.width}x{reference.height}"], {})

    a, b = _bytes_of(image), _bytes_of(reference)
    if len(a) != len(b):
        return ([f"{len(a)} bytes vs reference {len(b)}"], {})

    total = 0
    worst = 0
    for left, right in zip(a, b):
        delta = left - right if left >= right else right - left
        total += delta
        if delta > worst:
            worst = delta

    mae = total / len(a) if a else 0.0
    stats = {"mae": round(mae, 6), "max_pixel": worst, "compared_bytes": len(a)}

    if mae > tolerance["max_mae"]:
        problems.append(f"mae {mae:.3f} > {tolerance['max_mae']}")
    if worst > tolerance["max_pixel"]:
        problems.append(f"max pixel error {worst} > {tolerance['max_pixel']}")

    return problems, stats


def compare_scaled(image, reference, factor, tolerance=1.0):
    """Does `image` equal `reference` multiplied by `factor`?

    The check for a gain whose default is not 1.0, which is the only way a host
    render can be made to prove this plug-in's pixel path: at gain 1.0 the host
    may skip `render()` entirely, and the scripting API cannot set a parameter.

    The factor is applied in byte space with rounding and clamping, which is all
    an 8-bit output can carry. `tolerance` is the largest per-byte deviation
    allowed, because the host converts float to 8 bits with its own rounding.

    A frame that skipped `render()` fails loudly rather than marginally: every
    byte is off by `|factor - 1| * value`, so the largest error in the image is
    the largest value in it.
    """
    if (image.width, image.height) != (reference.width, reference.height):
        return ([f"size {image.width}x{image.height} != reference "
                 f"{reference.width}x{reference.height}"], {})

    a, b = _bytes_of(image), _bytes_of(reference)
    if len(a) != len(b):
        return ([f"{len(a)} bytes vs reference {len(b)}"], {})

    total = 0
    worst = 0
    for left, right in zip(a, b):
        expected = right * factor
        expected = 0.0 if expected < 0.0 else (255.0 if expected > 255.0 else expected)
        delta = left - int(expected + 0.5)
        delta = delta if delta >= 0 else -delta
        total += delta
        if delta > worst:
            worst = delta

    mae = total / len(a) if a else 0.0
    stats = {"gain": factor, "mae": round(mae, 6), "max_pixel": worst,
             "compared_bytes": len(a)}
    if worst > tolerance:
        problems = [f"max pixel error {worst} > {tolerance} against reference "
                    f"x{factor:g}"]
    else:
        problems = []
    return problems, stats


def _synthetic(width, height, channels, value_of):
    return bytes(value_of(x, y, c) for y in range(height) for x in range(width) for c in range(channels))


class _Stub:
    def __init__(self, width, height, channels, pixels, decoder="oiio"):
        self.width = width
        self.height = height
        self.channels = channels
        self.pixels = pixels
        self.decoder = decoder


def selftest():
    """Prove the checks can fail; run with `python tests/resolve/checks.py`."""
    cases = []

    def case(name, run, want_problems):
        cases.append((name, run, want_problems))

    gradient = _Stub(4, 2, 4, _synthetic(4, 2, 4, lambda x, y, c: (x * 16 + y * 8 + c) & 0xFF))
    uniform = _Stub(4, 2, 4, bytes([17]) * (4 * 2 * 4))

    case("a gradient passes", lambda: check_image(gradient, {"width": 4, "height": 2}), [])
    case("a uniform frame is caught",
         lambda: check_image(uniform), ["uniform frame: every byte is the same value"])
    # The frame most likely to mean the plug-in never ran: opaque black, RGB zero.
    opaque_black = _Stub(4, 2, 4, bytes([0, 0, 0, 255] * (4 * 2)))
    case("an opaque black frame is caught",
         lambda: check_image(opaque_black), ["completely black frame (alpha ignored)"])
    case("a wrong size is caught", lambda: check_image(gradient, {"width": 5}), None)
    case("a wrong channel count is caught",
         lambda: check_image(gradient, {"channels": 3}), None)
    case("an identical reference passes",
         lambda: compare_to_reference(gradient, gradient)[0], [])
    # One byte moved from 0 to 200: a difference of 200 that must be caught.
    bumped = bytes([200] + list(gradient.pixels[1:]))
    case("a reference that differs by 200 is caught",
         lambda: compare_to_reference(gradient, _Stub(4, 2, 4, bumped),
                                      {"max_mae": 0.0, "max_pixel": 0})[0], None)
    case("a reference of another size is caught",
         lambda: compare_to_reference(gradient, _Stub(2, 2, 4, bytes(2 * 2 * 4)))[0], None)

    def doubled(source, offset=None, value=None):
        """The frame a gain of 2.0 should produce, in byte space."""
        scaled = bytearray(min(255, int(byte * 2.0 + 0.5)) for byte in source.pixels)
        if offset is not None:
            scaled[offset] = value
        return _Stub(source.width, source.height, source.channels, bytes(scaled))

    case("a doubled frame passes the gain check",
         lambda: compare_scaled(doubled(gradient), gradient, 2.0)[0], [])
    # The frame a host produces when it skips render(): the check exists for this.
    case("an undoubled frame is caught by the gain check",
         lambda: compare_scaled(gradient, gradient, 2.0)[0], None)
    case("a doubled frame with one byte off by 4 is caught",
         lambda: compare_scaled(doubled(gradient, 3, min(255, int(gradient.pixels[3] * 2 + 0.5) + 4)),
                                gradient, 2.0)[0], None)
    case("clamping at 255 is expected, not an error",
         lambda: compare_scaled(doubled(_Stub(1, 1, 4, bytes([200, 10, 0, 255]))),
                                _Stub(1, 1, 4, bytes([200, 10, 0, 255])), 2.0)[0], [])

    failed = 0
    for index, (name, run, want) in enumerate(cases, 1):
        try:
            problems = run()
            ok = (problems == want) if want is not None else bool(problems)
            detail = f"{len(problems)} problem(s)" if ok else f"unexpected: {problems}"
        except Exception as error:      # a check that raises is a failed check
            ok, detail = False, f"raised {type(error).__name__}: {error}"
        print(f"case {index}: {name} ... {'ok' if ok else 'FAILED'} ({detail})")
        failed += 0 if ok else 1

    print(f"self-test: {failed} case(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(selftest())
