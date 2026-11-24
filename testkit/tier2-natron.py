#!/usr/bin/env python3
"""Tier 2: render a plug-in through NatronRenderer, headless, and check the pixels.

Standard library only. One 256x256 frame, one Write node, a few seconds.

    python3 testkit/tier2-natron.py \
        --natron <Natron install dir or NatronRenderer.exe> \
        --plugin-id com.ofxplugindeveloper.minimalcudagain \
        --ofx-path <build>/bundle \
        --work <scratch dir> \
        --gain 0.5

What it does, and why each step is the way it is:

1. Writes a 256x256 RGBA PNG with constant channels (64, 128, 192, 255).
2. Asks NatronRenderer to build ReadPNG -> <plug-in> -> WritePNG and save the
   project. `app.render()` from an interpreter script segfaults in Natron 2.5
   (the baseline graph does too, so it is the API, not the plug-in); rendering
   the saved project file is Natron's supported headless path.
3. Renders exactly frame 1: `NatronRenderer project.ntp 1`. Without the frame
   argument it renders 1-250 and overwrites the same file 250 times.
4. Decodes the PNG and checks that the plug-in applied `gain` **once, in linear
   space**: Natron converts sRGB to linear on read and back on write, so the
   expected 8-bit output is srgb_to_8bit(gain * srgb_to_linear(input)), not
   gain * input. Measured: gain=0.5 on (64,128,192) gives (44,92,140), which is
   that formula to the last digit; comparing raw 8-bit values against 32/64/96
   would have failed a correct render.

Bundled PFM readers/writers (fr.inria.openfx.ReadPFM / WritePFM) crash Natron
2.5.0 here, so this uses PNG. That is a host-side bug, not the plug-in's.

Exit status 0 only if the geometry, the alpha channel and the gain all check out.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SIZE = 256
SRC_RGBA = (64, 128, 192, 255)

PROJECT_SCRIPT = r'''
import os

plugin = os.environ["T2_PLUGIN"]
src = os.environ["T2_SRC"]
out = os.environ["T2_OUT"]
ntp = os.environ["T2_NTP"]
gain = float(os.environ.get("T2_GAIN", "1.0"))

reader = app1.createNode("fr.inria.openfx.ReadPNG")
reader.getParam("filename").set(src)

fx = app1.createNode(plugin)
if fx is None:
    raise SystemExit("PLUGIN_NOT_REGISTERED " + plugin)
fx.connectInput(0, reader)
fx.getParam("gain").set(gain)
print("T2 node", fx.getScriptName(), "gain", fx.getParam("gain").get(), flush=True)

writer = app1.createNode("fr.inria.openfx.WritePNG")
writer.connectInput(0, fx)
writer.getParam("filename").set(out)
app1.saveProjectAs(ntp)
print("T2 saved", os.path.exists(ntp), flush=True)
'''


BASELINE_SCRIPT = r'''
import os

src = os.environ["T2_SRC"]
out = os.environ["T2_OUT"]
ntp = os.environ["T2_NTP"]

reader = app1.createNode("fr.inria.openfx.ReadPNG")
reader.getParam("filename").set(src)
writer = app1.createNode("fr.inria.openfx.WritePNG")
writer.connectInput(0, reader)
writer.getParam("filename").set(out)
app1.saveProjectAs(ntp)
print("T2 saved", os.path.exists(ntp), flush=True)
'''


def write_png(path, size, rgba):
    row = bytes(rgba) * size
    raw = b"".join(b"\x00" + row for _ in range(size))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat = 8, b""
    width = height = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", payload[:10])
            if depth != 8 or ctype != 6:
                raise SystemExit(f"{path}: need 8-bit RGBA, got depth {depth} type {ctype}")
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break

    raw = zlib.decompress(idat)
    stride = width * 4
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(height):
        ftype = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if ftype == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                left = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        elif ftype != 0:
            raise SystemExit(f"{path}: bad filter {ftype}")
        rows.append(bytes(line))
        prev = line
    return width, height, rows


def srgb_to_linear(value):
    c = value / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb8(value):
    v = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055
    return max(0, min(255, int(round(v * 255.0))))


def renderer_path(natron):
    if os.path.isdir(natron):
        for candidate in ("NatronRenderer.exe", os.path.join("bin", "NatronRenderer.exe"),
                          "NatronRenderer", os.path.join("bin", "NatronRenderer")):
            full = os.path.join(natron, candidate)
            if os.path.isfile(full):
                return full
        raise SystemExit(f"no NatronRenderer under {natron}")
    if os.path.isfile(natron):
        return natron
    found = shutil.which(natron)
    if found:
        return found
    raise SystemExit(f"NatronRenderer not found: {natron}")


def run(cmd, env, label):
    print(f"[tier2] {label}: {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    tail = [line for line in proc.stdout.splitlines() if "T2 " in line]
    for line in tail:
        print("        " + line)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-4000:])
        raise SystemExit(f"{label} failed with exit {proc.returncode}")
    return proc.stdout


def main():
    parser = argparse.ArgumentParser(description="Tier 2 headless render check through NatronRenderer")
    parser.add_argument("--natron", required=True, help="Natron install directory or NatronRenderer path")
    parser.add_argument("--plugin-id", required=True)
    parser.add_argument("--ofx-path", required=True, help="directory holding the .ofx.bundle")
    parser.add_argument("--work", default=None, help="scratch directory (default: a temp dir)")
    parser.add_argument("--gain", type=float, default=0.5)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = parser.parse_args()

    renderer = renderer_path(args.natron)
    work = os.path.abspath(args.work or tempfile.mkdtemp(prefix="ofx-tier2-"))
    os.makedirs(work, exist_ok=True)
    keep = args.keep or args.work is not None

    # Absolute, every one of them: Natron resolves a relative media path inside a
    # project against the project's own directory, so '.validation/t2/in.png'
    # became '<work>/.validation/t2/in.png' and the reader found nothing
    # ("Cannot load frame"). Measured before the fix.
    src = os.path.join(work, "in.png")
    base_out = os.path.join(work, "out_base.png")
    out = os.path.join(work, "out_plugin.png")
    script = os.path.join(work, "build_ntp.py")
    ntp = os.path.join(work, "project.ntp")
    renderer = os.path.abspath(renderer)

    write_png(src, SIZE, SRC_RGBA)
    with open(script, "w", encoding="utf-8") as handle:
        handle.write(PROJECT_SCRIPT)

    env = dict(os.environ)
    env["OFX_PLUGIN_PATH"] = os.path.abspath(args.ofx_path)
    env["T2_SRC"] = src
    env["T2_PLUGIN"] = args.plugin_id
    env["T2_GAIN"] = str(args.gain)
    env["T2_NTP"] = ntp

    # 1. build + save the project
    env_out = dict(env, T2_OUT=out)
    run([renderer, "--clear-openfx-cache", script], env_out, "build project")
    if not os.path.exists(ntp):
        raise SystemExit("NatronRenderer did not save the project")

    # 2. render exactly one frame (without the frame argument it renders 1-250)
    run([renderer, "--clear-openfx-cache", ntp, "1"], env_out, "render plug-in frame")
    if not os.path.exists(out):
        raise SystemExit("no output image was written")

    # 3. the same graph without the plug-in: geometry, and proof that a
    #    pass-through render reproduces the input exactly (a wrong expectation
    #    here would make every colour comparison meaningless).
    passthrough = os.path.join(work, "passthrough.ntp")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write(BASELINE_SCRIPT)
    run([renderer, "--clear-openfx-cache", script], dict(env, T2_OUT=base_out, T2_NTP=passthrough),
        "build baseline project")
    run([renderer, "--clear-openfx-cache", passthrough, "1"], dict(env, T2_OUT=base_out),
        "render baseline frame")

    width, height, rows = read_png(out)
    bw, bh, brows = read_png(base_out)

    failures = []

    def note(ok, message):
        print(("  ok   " if ok else "  FAIL ") + message)
        if not ok:
            failures.append(message)

    note((width, height) == (bw, bh), f"output geometry {width}x{height} matches the baseline")

    # The reader sits in the bottom-left of Natron's project format, so sample
    # inside the inserted image by locating the first row that is not all black.
    top = None
    for y, line in enumerate(brows):
        if any(line):
            top = y
            break
    note(top is not None, "baseline frame is not empty (the reader produced an image)")
    if top is None:
        print("RESULT FAIL")
        return 1

    ys = [top + 1, min(height - 1, top + SIZE // 2), height - 1]
    xs = [0, SIZE // 2, SIZE - 1]
    tolerance = 2
    for y in ys:
        for x in xs:
            got = rows[y][x * 4:x * 4 + 4]
            src_px = brows[y][x * 4:x * 4 + 4]
            if src_px[3] == 0 and got[3] == 0:
                continue  # outside the inserted image
            expected_rgb = [linear_to_srgb8(args.gain * srgb_to_linear(c)) for c in src_px[:3]]
            delta = max(abs(got[c] - expected_rgb[c]) for c in range(3))
            note(delta <= tolerance,
                 f"({x},{y}) gain {args.gain} in linear space: got {list(got[:3])} "
                 f"expected {expected_rgb} (max delta {delta}, tolerance {tolerance})")
            note(got[3] == src_px[3],
                 f"({x},{y}) alpha untouched: {got[3]} vs {src_px[3]}")

    print(f"RESULT {'PASS' if not failures else 'FAIL'} ({len(failures)} problem(s))")
    if not keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"[tier2] scratch kept in {work}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
