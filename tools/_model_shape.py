#!/usr/bin/env python3
"""The frame that a committed model and the fixtures beside it agree on.

A model file fixes some dimensions and leaves others symbolic - the shape this
repository ships is `[1, 3, 'h', 'w']` - while `test_shape.txt` beside it records
the `channels height width` the input and the golden were generated with. Three
tools feed such a frame to a runtime, and each of them had that shape written as a
constant somewhere: `1x3x48x64` in the ahead-of-time compile, a 64x64 probe size in
`tools/model_probe.py`, a literal `[1, 3]` prefix in the canary. A fixture
regenerated at another size then still "worked" for whichever tool guessed right,
and failed confusingly for the others.

Not a command line tool for the most part: the three import it. Reading the
model's declaration needs a runtime and stays in the caller; what lives here is
the file format, the merge of the two halves, and the spelling IREE wants.

    python3 tools/_model_shape.py path/to/test_shape.txt    # 1x3x48x64xf32

Exit status for that spelling: 0 printed, 2 the file is missing or malformed.
"""

from __future__ import annotations

import sys


class ShapeError(Exception):
    """A shape file is missing or malformed, or a model disagrees with it."""


def read_shape_file(path):
    """`channels height width` -> `[channels, height, width]`.

    Three positive integers, because the file is written next to the input and the
    golden by whatever generated them, and a file that is not that is a mistake
    worth naming rather than guessing around.
    """
    try:
        with open(path, encoding="utf-8") as fh:
            values = [int(value) for value in fh.read().split()]
    except FileNotFoundError:
        raise ShapeError(f"{path} is missing")
    except ValueError:
        raise ShapeError(f"{path} holds something other than integers")
    if len(values) != 3 or min(values) <= 0:
        raise ShapeError(f'{path} must hold "channels height width", found {values}')
    return values


def resolve(declared, values, label):
    """Merge a model's declared NCHW input with the file's C, H and W.

    A dimension the model fixes is used as it is, and the file has to agree with
    it; a symbolic one is taken from the file. A disagreement means the model and
    its fixtures were regenerated separately, which is worth failing on instead of
    reshaping the frame to whatever the model happens to accept. `label` is how
    the file is named in the message - the caller's path is not always useful.
    """
    if len(declared) != 4:
        raise ShapeError(f"the model's input is {declared}, this feeds one 4-D NCHW frame")

    wanted = [1, *values]  # one frame, then channels height width
    shape = []
    for position, (declared_dim, file_value) in enumerate(zip(declared, wanted)):
        if isinstance(declared_dim, int) and declared_dim > 0:
            if declared_dim != file_value:
                raise ShapeError(
                    f"the model fixes dimension {position} at {declared_dim}, "
                    f"{label} says {file_value}"
                )
            shape.append(declared_dim)
        else:
            shape.append(file_value)
    return shape


def iree_input(shape, dtype="f32"):
    """`[1, 3, 48, 64]` -> `1x3x48x64xf32`, the way `iree-run-module` spells it."""
    return "x".join(str(dim) for dim in shape) + "x" + dtype


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: _model_shape.py path/to/test_shape.txt\n")
        return 2
    try:
        values = read_shape_file(argv[1])
    except ShapeError as exc:
        sys.stderr.write(f"{exc}\n")
        return 2
    print(iree_input([1, *values]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
