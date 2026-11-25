"""Read a rendered frame without adding a dependency.

Two decoders are tried, in this order: OpenImageIO if it is importable, then
`ffmpeg`/`ffprobe` on PATH. Neither is a requirement of this repository, so a
machine with both is the happy case and a machine with neither gets `NotRun`
instead of a wrong answer.

Everything here runs without Resolve, so this module is testable on any machine
that can decode one image. What it cannot tell you is whether the image came from
a render: that is `poc.py`'s problem.
"""

import json
import shutil
import subprocess


class NotRun(RuntimeError):
    """Raised when the check cannot be performed here, with the reason."""


class Image:
    def __init__(self, path, width, height, channels, pixels, decoder, source_format=None):
        self.path = path
        self.width = width
        self.height = height
        self.channels = channels
        self.pixels = pixels
        self.decoder = decoder
        self.source_format = source_format

    def __repr__(self):
        return (f"Image({self.width}x{self.height}x{self.channels}, "
                f"{self.decoder}, {self.path})")


def available_decoders():
    found = []
    try:
        import OpenImageIO  # noqa: F401
        found.append("oiio")
    except Exception:
        pass
    if shutil.which("ffmpeg") and shutil.which("ffprobe"):
        found.append("ffmpeg")
    return found


def read_image(path):
    decoders = available_decoders()
    if "oiio" in decoders:
        return _read_with_oiio(path)
    if "ffmpeg" in decoders:
        return _read_with_ffmpeg(path)
    raise NotRun("no decoder: install OpenImageIO or put ffmpeg and ffprobe on PATH")


def _read_with_oiio(path):
    import numpy as np
    import OpenImageIO as oiio

    handle = oiio.ImageInput.open(str(path))
    if handle is None:
        raise RuntimeError(f"oiio could not open {path}: {oiio.geterror()}")
    spec = handle.spec()
    data = handle.read_image(format=oiio.UINT8)
    handle.close()
    if data is None:
        raise RuntimeError(f"oiio read nothing from {path}")
    pixels = np.asarray(data, dtype=np.uint8).tobytes()
    return Image(str(path), spec.width, spec.height, spec.nchannels, pixels,
                 "oiio", str(spec.format))


def _probe(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,pix_fmt", "-of", "json", str(path)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ffprobe failed on {path}: {result.stderr.strip()}")
    streams = json.loads(result.stdout or "{}").get("streams") or []
    if not streams:
        raise RuntimeError(f"no video stream in {path}")
    return streams[0]


def _read_with_ffmpeg(path):
    stream = _probe(path)
    width, height = int(stream["width"]), int(stream["height"])
    source_format = stream.get("pix_fmt", "")
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-nostdin", "-i", str(path),
         "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg failed on {path}: {result.stderr.decode('utf-8', 'replace').strip()}")
    expected = width * height * 4
    if len(result.stdout) != expected:
        raise RuntimeError(f"ffmpeg gave {len(result.stdout)} bytes, wanted {expected}")
    # ffmpeg normalises to RGBA here, so `channels` is the decoder's choice, not
    # the file's. `source_format` carries what the container actually held.
    return Image(str(path), width, height, 4, result.stdout, "ffmpeg", source_format)
