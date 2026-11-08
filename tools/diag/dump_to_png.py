"""Turn the one-frame texture dump's raw ``.bin`` files into PNGs (and the float ones into ``.npy`` arrays).

The dump writes one ``.bin`` per texture plus a ``facts.txt`` line per file: the name, the DXGI format, the size, the
number of bytes, the row pitch the readback had, the draw ordinal and the resource pointer.  This tool reads that table,
decodes the bytes of every file it knows, and writes a PNG next to the capture (``dump\\png`` by default) so a material,
a wetness target or a light buffer can be looked at instead of guessed at.

What is shown, per format:

* 8-bit colour (``B8G8R8A8_UNORM``, ``R8G8B8A8_UNORM``, their ``_SRGB``/``_TYPELESS`` spellings): the stored bytes, no
  gamma change.  The bytes are the material's own; changing them would hide what the engine keeps there.
* 16-bit float colour (``R16G16B16A16_FLOAT``): a tone-mapped preview - value x ``--exposure`` (default 1), clamped to
  [0, 1], then sRGB encoded - plus a raw ``.npy`` of the unmodified floats.
* ``R11G11B10_FLOAT``: the same exposure-and-clamp preview of the three decoded floats, plus a raw ``.npy``.
* a single float channel (``R16_FLOAT``, ``R32_FLOAT``, ``R11G11B10_FLOAT``'s channels, ``D32_FLOAT``): depth-like.  Values
  already in [0, 1] are shown as they are; any other range is mapped from the minimum to the maximum of the file, which
  the tool prints, because a raw range is unreadable otherwise.
* ``R16G16_FLOAT`` (a motion vector buffer): ``0.5 * (value + 1)`` per channel, so zero motion is mid grey, red for x
  and green for y; plus the raw ``.npy``.
* ``D16_UNORM``: value / 65535 - only for a file that really holds two bytes per texel.
* ``D24_UNORM_S8_UINT``: the depth channel (the low 24 bits of each 32-bit texel) / 16777215.
* ``R8_UNORM``: value / 255 (a mask, an occlusion factor, a stencil).

A typeless format (``R16G16B16A16_TYPELESS`` and the other ``_TYPELESS`` numbers) is read as the concrete twin of the same
byte width, which is the only reading the facts line allows: the view that bound the texture, not the texture, decides
the type, and the dump records the texture's own format.

Row pitch is honoured: when the file is padded (``row_pitch`` x height bytes) the padding is dropped, and when it is
packed (width x bytes-per-pixel x height) it is read as packed.  A file whose size matches neither is read row by row
from its own stride when it is larger, and zero-padded when it is cut short, with a printed note.

Pure Python plus numpy: the PNG is written by this module (zlib), so there is no image library to install.

    python -I dump_to_png.py <capture-folder> [--out DIR] [--exposure X] [--channels] [--formats]
"""

from __future__ import annotations

import os
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Any, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import drawdiag  # noqa: E402

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
DEFAULT_OUT_FOLDER = os.path.join(drawdiag.DUMP_FOLDER, "png")


# ---------------------------------------------------------------------------------------------------------------------
# DXGI formats: the names the fork writes and the numbers it prints as ``fmt<N>``
# ---------------------------------------------------------------------------------------------------------------------

# The numbers are DXGI_FORMAT's own (the Windows SDK header's table), so a facts line that says ``fmt55`` (a format the
# fork's name table does not know) still decodes.
DXGI_NUMBERS = {
    "R32G32B32A32_TYPELESS": 1, "R32G32B32A32_FLOAT": 2, "R32G32B32A32_UINT": 3, "R32G32B32A32_SINT": 4,
    "R16G16B16A16_TYPELESS": 9, "R16G16B16A16_FLOAT": 10, "R16G16B16A16_UNORM": 11,
    "R32G32_TYPELESS": 15, "R32G32_FLOAT": 16,
    "R10G10B10A2_TYPELESS": 23, "R10G10B10A2_UNORM": 24,
    "R11G11B10_FLOAT": 26,
    "R8G8B8A8_TYPELESS": 27, "R8G8B8A8_UNORM": 28, "R8G8B8A8_UNORM_SRGB": 29,
    "R16G16_TYPELESS": 33, "R16G16_FLOAT": 34, "R16G16_UNORM": 35,
    "R32_TYPELESS": 39, "D32_FLOAT": 40, "R32_FLOAT": 41, "R32_UINT": 42,
    "R24G8_TYPELESS": 44, "D24_UNORM_S8_UINT": 45, "R24_UNORM_X8_TYPELESS": 46,
    "R8G8_TYPELESS": 48, "R8G8_UNORM": 49,
    "R16_TYPELESS": 53, "R16_FLOAT": 54, "D16_UNORM": 55, "R16_UNORM": 56,
    "R8_TYPELESS": 60, "R8_UNORM": 61, "R8_UINT": 62,
    "A8_UNORM": 65,
    "B8G8R8A8_UNORM": 87, "B8G8R8A8_TYPELESS": 90, "B8G8R8A8_UNORM_SRGB": 91,
}


@dataclass(frozen=True)
class Format:
    """How the bytes of one DXGI format are laid out, and how the preview reads them."""

    name: str
    dxgi: int
    bpp: int  # bytes per texel the decoding assumes
    kind: str  # "u8", "unorm16", "f16", "f32", "d16", "d24s8", "r10"
    channels: int = 1  # 1, 2 or 4
    order: tuple[str, ...] = ("value",)  # the channel names, in memory order
    note: str = ""

    @property
    def keeps_display_values(self) -> bool:
        return self.kind == "u8"


def _format(name: str, kind: str, channels: int = 1, order: Sequence[str] = ("value",), bpp: int | None = None,
            note: str = "") -> Format:
    return Format(name=name, dxgi=DXGI_NUMBERS[name], bpp=bpp if bpp is not None else 0, kind=kind,
                  channels=channels, order=tuple(order), note=note)


FORMATS: dict[str, Format] = {}
_BY_NUMBER: dict[int, Format] = {}

_BYTE_LAYOUTS = [
    # name, kind, channels, order, bytes per texel
    ("B8G8R8A8_UNORM", "u8", 4, ("b", "g", "r", "a"), 4),
    ("B8G8R8A8_UNORM_SRGB", "u8", 4, ("b", "g", "r", "a"), 4),
    ("B8G8R8A8_TYPELESS", "u8", 4, ("b", "g", "r", "a"), 4),
    ("R8G8B8A8_UNORM", "u8", 4, ("r", "g", "b", "a"), 4),
    ("R8G8B8A8_UNORM_SRGB", "u8", 4, ("r", "g", "b", "a"), 4),
    ("R8G8B8A8_TYPELESS", "u8", 4, ("r", "g", "b", "a"), 4),
    ("R10G10B10A2_UNORM", "r10", 4, ("r", "g", "b", "a"), 4),
    ("R11G11B10_FLOAT", "r11", 3, ("r", "g", "b"), 4),
    ("R16G16B16A16_FLOAT", "f16", 4, ("r", "g", "b", "a"), 8),
    ("R16G16B16A16_UNORM", "unorm16", 4, ("r", "g", "b", "a"), 8),
    ("R16_FLOAT", "f16", 1, ("value",), 2),
    ("R16G16_FLOAT", "f16", 2, ("x", "y"), 4),
    ("R16G16_UNORM", "unorm16", 2, ("x", "y"), 4),
    ("R32_FLOAT", "f32", 1, ("value",), 4),
    ("D32_FLOAT", "f32", 1, ("depth",), 4),
    ("R32G32_FLOAT", "f32", 2, ("x", "y"), 8),
    ("D16_UNORM", "d16", 1, ("depth",), 2),
    ("R16_UNORM", "unorm16", 1, ("value",), 2),
    ("D24_UNORM_S8_UINT", "d24s8", 1, ("depth",), 4),
    ("R24_UNORM_X8_TYPELESS", "d24s8", 1, ("depth",), 4),
    ("R8_UNORM", "u8", 1, ("value",), 1),
    ("R8G8_UNORM", "u8", 2, ("r", "g"), 2),
    ("A8_UNORM", "u8", 1, ("value",), 1),
]

for _name, _kind, _channels, _order, _bpp in _BYTE_LAYOUTS:
    _format_obj = _format(_name, _kind, _channels, _order, bpp=_bpp)
    FORMATS[_format_obj.name] = _format_obj
    _BY_NUMBER[_format_obj.dxgi] = _format_obj

# One representative name per DXGI number, for a facts line that writes ``fmt55`` instead of the name.
_NUMBER_NAMES = {
    1: "R32G32B32A32_TYPELESS", 2: "R32G32B32A32_FLOAT", 9: "R16G16B16A16_TYPELESS",
    10: "R16G16B16A16_FLOAT", 11: "R16G16B16A16_UNORM", 16: "R32G32_FLOAT", 23: "R10G10B10A2_TYPELESS",
    27: "R8G8B8A8_TYPELESS", 28: "R8G8B8A8_UNORM", 29: "R8G8B8A8_UNORM_SRGB", 33: "R16G16_TYPELESS",
    34: "R16G16_FLOAT", 35: "R16G16_UNORM", 39: "R32_TYPELESS", 40: "D32_FLOAT", 41: "R32_FLOAT",
    44: "R24G8_TYPELESS", 45: "D24_UNORM_S8_UINT", 46: "R24_UNORM_X8_TYPELESS", 49: "R8G8_UNORM",
    53: "R16_TYPELESS", 54: "R16_FLOAT", 55: "D16_UNORM", 56: "R16_UNORM", 60: "R8_TYPELESS", 61: "R8_UNORM",
    65: "A8_UNORM", 87: "B8G8R8A8_UNORM", 90: "B8G8R8A8_TYPELESS", 91: "B8G8R8A8_UNORM_SRGB",
}
# Every other number this table knows a name for, so that a ``fmt<N>`` line naming a format whose name the curated table
# above leaves out (``fmt26``) still resolves.
for _listed_name, _listed_number in list(DXGI_NUMBERS.items()):
    _NUMBER_NAMES.setdefault(_listed_number, _listed_name)

# Format-name spellings a build may use, mapped onto the canonical name of this table.
_ALIASES = {
    "B8G8R8A8_UNORM_SRGB": "B8G8R8A8_UNORM_SRGB",
    "R8G8B8A8_UNORM_SRGB": "R8G8B8A8_UNORM_SRGB",
}

# A texture's own format can be typeless: the view that bound it decides the type, and the same bytes read as the
# concrete twin of the same width.  The table lists the twins by DXGI number (a typeless format is written by number
# when the diagnostic's name table has no name for it), and the twin is used when this tool decodes that width.  A
# typeless format whose twin this tool cannot decode stays unresolved, which is what makes the file skip with a message
# instead of producing a preview of the wrong type.
TYPELESS_TWINS = {
    1: "R32G32B32A32_FLOAT",
    9: "R16G16B16A16_FLOAT",
    15: "R32G32_FLOAT",
    23: "R10G10B10A2_UNORM",
    27: "R8G8B8A8_UNORM",
    33: "R16G16_FLOAT",
    39: "R32_FLOAT",
    44: "D24_UNORM_S8_UINT",
    48: "R8G8_UNORM",
    53: "R16_FLOAT",
    60: "R8_UNORM",
    90: "B8G8R8A8_UNORM",
}

TYPELESS_NOTE = "a typeless format: the view decides the type, so the bytes are read as {twin}"


def _typeless_twin(fmt: Format) -> Format:
    """The concrete twin of a typeless format, with the note that says why that reading was chosen."""

    twin_name = TYPELESS_TWINS.get(fmt.dxgi)
    if twin_name is None or not fmt.name.endswith("_TYPELESS"):
        return fmt
    twin = FORMATS.get(twin_name)
    if twin is None or twin.dxgi == fmt.dxgi:
        return fmt
    return Format(name=twin.name, dxgi=fmt.dxgi, bpp=twin.bpp, kind=twin.kind, channels=twin.channels,
                  order=twin.order, note=TYPELESS_NOTE.format(twin=twin.name))


def _named_format(text: str) -> Format | None:
    """The layout of one canonical format name, or ``None``; a typeless name resolves to its concrete twin."""

    fmt = FORMATS.get(text)
    if fmt is not None:
        return _typeless_twin(fmt)
    if text in DXGI_NUMBERS:
        number = DXGI_NUMBERS[text]
        twin_name = TYPELESS_TWINS.get(number)
        twin = FORMATS.get(twin_name) if twin_name else None
        if twin is not None:
            return Format(name=twin.name, dxgi=number, bpp=twin.bpp, kind=twin.kind, channels=twin.channels,
                          order=twin.order, note=TYPELESS_NOTE.format(twin=twin.name))
    return None


def resolve_format(name: str | None) -> Format | None:
    """The layout of a facts line's format name, a ``fmt<N>`` spelling, or ``None`` when this tool cannot show it."""

    if not name:
        return None
    text = name.strip().upper()
    if text in _ALIASES:
        text = _ALIASES[text]
    # A leading "DXGI_FORMAT_" is how the SDK spells it.
    if text.startswith("DXGI_FORMAT_"):
        return resolve_format(text[len("DXGI_FORMAT_"):])
    if text.startswith("FMT") and text[3:].isdigit():
        canonical = _NUMBER_NAMES.get(int(text[3:]))
        return _named_format(canonical) if canonical else None
    return _named_format(text)


def d16_texel_width(fact: drawdiag.DumpFact, size: int) -> int:
    """How many bytes one texel of a 16-bit depth file occupies: 2 for a real 16-bit read, 4 for the wide one.

    The game's depth readback can hand out a 32-bit stride for a ``D16_UNORM`` texture: the value is 16 bits but every
    texel takes four, and which half of those four bytes holds the value is the writer's business.  A file of that size
    is therefore refused by :func:`texel_bytes` instead of being decoded as if it were packed; a file of two-byte texels
    (packed, or padded to a pitch between two and four bytes per texel) is read normally, and a cut-short file is read
    as packed so that the rows it does hold still show.
    """

    row_two = fact.width * 2
    row_four = fact.width * 4
    height = max(1, fact.height)
    pitch = fact.row_pitch
    if size == row_two * height:
        return 2
    if pitch is not None and row_two <= pitch < row_four and pitch * height <= size:
        return 2
    if size >= row_four * height or (pitch is not None and pitch >= row_four):
        return 4
    return 2


def texel_bytes(fact: drawdiag.DumpFact, fmt: Format, size: int) -> tuple[int, list[str]]:
    """How many bytes one texel really occupies, and what to say about it.

    A 16-bit depth whose file or recorded pitch says four bytes per texel cannot be decoded: the value is 16 bits and
    the two extra bytes are neither padding this tool may skip nor a value it may read, because the readback's own
    writer decides where in the four bytes the depth sits.  Rather than show a preview built from a guess, the file is
    refused with a note that says so.  Every other format is read exactly as its table says.
    """

    if fmt.kind != "d16":
        return fmt.bpp, []
    if d16_texel_width(fact, size) == 4:
        raise ValueError("D16_UNORM written with four bytes per texel: the readback's 16-bit value is not a 16-bit "
                         "depth and this tool does not guess which half of the texel holds it")
    return 2, []

    if fmt.kind != "d16":
        return fmt.bpp, []
    packed_two = fact.width * 2 * max(1, fact.height)
    four_texel = fact.width * 4 * max(1, fact.height)
    pitch = fact.row_pitch
    if size == packed_two:
        return 2, []
    if size >= four_texel or (pitch is not None and pitch >= fact.width * 4 and pitch * max(1, fact.height) <= size):
        return 4, ["16-bit depth read with a four-byte texel stride"]
    return 2, []


# ---------------------------------------------------------------------------------------------------------------------
# Bytes to arrays
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class Decoded:
    """One decoded texture: its per-texel values, the strides used and what had to be noted while reading."""

    fact: drawdiag.DumpFact
    fmt: Format
    values: Any  # numpy array: (h, w) or (h, w, channels)
    stride: int
    packed: bool
    notes: list[str]


def _row_stride(fact: drawdiag.DumpFact, bpp: int, size: int) -> tuple[int, bool, list[str]]:
    """The stride to read with, whether it is the packed one, and what to say about it.

    The dump records the row pitch the readback had and writes the file packed; an older dump writes the pitched bytes
    as they came.  So the file size decides: conformance to ``row_pitch`` first, then to the packed width, then the
    file's own ``size / height`` when it is larger than a packed row (a wider texel than the format implies).
    """

    notes: list[str] = []
    row = fact.width * bpp
    height = max(1, fact.height)
    pitch = fact.row_pitch if fact.row_pitch and fact.row_pitch >= row else None
    if pitch is not None and pitch * height <= size:
        if pitch == row:
            if size > row * height:
                notes.append(f"{size - row * height} trailing byte(s) ignored")
            return row, True, notes
        notes.append(f"read with the recorded row pitch {pitch} (packed row {row})")
        return pitch, False, notes
    if row * height <= size:
        # The note is about the packed size only: a file with rows after the packed body, or a pitched file cut short
        # before one full pitch, is not a packed file and must not be described as one.
        if pitch is not None and size == row * height:
            notes.append(f"the file is packed ({size} B) although facts.txt records row pitch {pitch}")
        if size > row * height:
            notes.append(f"{size - row * height} trailing byte(s) ignored after the packed rows")
        return row, True, notes
    if size // height >= row:
        stride = size // height
        notes.append(f"read with the file's own stride {stride} (a packed row is {row}); the texel is wider than "
                     f"the format implies")
        return stride, False, notes
    return row, True, [f"the file is cut short ({size} B for {row * height} B of packed rows): the missing rows "
                       f"are zero"]


def _decode_r11g11b10(body: np.ndarray, height: int, width: int) -> np.ndarray:
    """The three float channels of an ``R11G11B10_FLOAT`` texel.

    The layout is the SDK's unsigned-float one: 11 bits of red, 11 of green and 10 of blue, each with a 5-bit exponent
    (bias 15) and a mantissa of 6 or 5 bits below it.  There is no sign bit, no infinity and no NaN: a zero exponent
    means the smaller subnormal range and any other exponent carries the implied leading 1, exactly as an IEEE half
    does.  Decoding the packing here keeps the preview independent of any image library.
    """

    packed = body.view(np.uint32).reshape(height, width)
    red = _unsigned_float(packed & np.uint32(0x7FF), 6)
    green = _unsigned_float((packed >> np.uint32(11)) & np.uint32(0x7FF), 6)
    blue = _unsigned_float((packed >> np.uint32(22)) & np.uint32(0x3FF), 5)
    return np.stack([red, green, blue], axis=-1).astype(np.float32)


def _unsigned_float(bits: np.ndarray, mantissa_bits: int) -> np.ndarray:
    """One packed unsigned float: ``mantissa_bits`` below a 5-bit exponent with bias 15."""

    mantissa = (bits & np.uint32((1 << mantissa_bits) - 1)).astype(np.float64)
    exponent = (bits >> np.uint32(mantissa_bits)).astype(np.int64)
    subnormal = exponent == 0
    power = np.where(subnormal, 1 - 15, exponent - 15).astype(np.int64)
    scale = np.ldexp(1.0, power)
    return mantissa / float(1 << mantissa_bits) * scale + np.where(subnormal, 0.0, scale)


def decode(fact: drawdiag.DumpFact, data: bytes, fmt: Format | None = None) -> Decoded:
    """Decode one dump file into an array (never raises on a short or odd-sized file).

    Two things are refused rather than guessed, both with a ``ValueError``: a format this tool has no decoder for, and a
    16-bit depth file whose texels are four bytes wide (see :func:`texel_bytes`).
    """

    resolved = fmt or resolve_format(fact.format)
    if resolved is None:
        raise ValueError(f"no decoder for format {fact.format!r}")
    height, width = max(1, fact.height), max(1, fact.width)
    notes: list[str] = []
    bpp, texel_notes = texel_bytes(fact, resolved, len(data))
    notes.extend(texel_notes)
    stride, packed, stride_notes = _row_stride(fact, bpp, len(data))
    notes.extend(stride_notes)
    row_bytes = width * bpp
    rows_available = min(height, len(data) // stride)
    matrix = np.zeros((height, stride), dtype=np.uint8)
    for row in range(rows_available):
        chunk = data[row * stride:(row + 1) * stride]
        matrix[row, :min(len(chunk), stride)] = np.frombuffer(chunk, dtype=np.uint8)[:stride]
    body = np.ascontiguousarray(matrix[:, :row_bytes])
    if resolved.kind == "f16":
        values = body.view(np.float16).astype(np.float32).reshape(height, width, resolved.channels)
    elif resolved.kind == "r11":
        values = _decode_r11g11b10(body, height, width)
    elif resolved.kind == "f32":
        values = body.view(np.float32).reshape(height, width, resolved.channels)
    elif resolved.kind == "unorm16":
        values = body.view(np.uint16).reshape(height, width, resolved.channels)
    elif resolved.kind == "d16":
        values = body.view(np.uint16).reshape(height, width)
    elif resolved.kind == "d24s8":
        # The depth channel is the low 24 bits; the top 8 carry the stencil, which a depth preview must not show.
        values = (body.view(np.uint32) & np.uint32(0xFFFFFF)).reshape(height, width)
    elif resolved.kind == "r10":
        values = body.view(np.uint32).reshape(height, width)
    else:  # u8
        values = body.reshape(height, width, resolved.channels) if resolved.channels > 1 else body[:, :width]
    return Decoded(fact, resolved, values, stride, packed, notes)


# ---------------------------------------------------------------------------------------------------------------------
# Preview colour
# ---------------------------------------------------------------------------------------------------------------------


def srgb_encode(value: np.ndarray) -> np.ndarray:
    """The sRGB transfer curve, so a linear value shown as a byte looks like the byte a display expects."""

    clipped = np.clip(value, 0.0, 1.0)
    return np.where(clipped <= 0.0031308, clipped * 12.92, 1.055 * np.power(clipped, 1.0 / 2.4) - 0.055)


def _to_u8(value: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(np.asarray(value, dtype=np.float64) * 255.0), 0, 255).astype(np.uint8)


def _value_plane(flat: np.ndarray) -> np.ndarray:
    """A single float channel as a display plane: shown as it is in [0, 1], mapped min..max otherwise."""

    finite = np.isfinite(flat)
    minimum = float(flat[finite].min()) if finite.any() else 0.0
    maximum = float(flat[finite].max()) if finite.any() else 0.0
    if minimum >= 0.0 and maximum <= 1.0:
        return _to_u8(flat)
    span = maximum - minimum
    if span <= 0:
        return _to_u8(np.zeros_like(flat))
    return _to_u8((flat - minimum) / span)


def channel_planes(decoded: Decoded, exposure: float = 1.0) -> dict[str, np.ndarray]:
    """The per-channel display values, one ``uint8`` plane per channel, named as the format names them."""

    fmt = decoded.fmt
    values = decoded.values
    planes: dict[str, np.ndarray] = {}
    if fmt.kind == "u8":
        if values.ndim == 2:
            return {fmt.order[0]: values.astype(np.uint8)}
        for index, name in enumerate(fmt.order):
            planes[name] = values[:, :, index].astype(np.uint8)
        return planes
    if fmt.kind == "unorm16":
        scaled = np.clip(np.asarray(values, dtype=np.float64) / 65535.0, 0, 1)
        if values.ndim == 2:
            return {fmt.order[0]: _to_u8(scaled)}
        for index, name in enumerate(fmt.order):
            planes[name] = _to_u8(scaled[:, :, index])
        return planes
    if fmt.kind == "r11":
        linear = np.nan_to_num(np.asarray(values, dtype=np.float64), nan=0.0, posinf=1.0, neginf=0.0)
        tone = srgb_encode(np.clip(linear * exposure, 0.0, 1.0))
        return {name: _to_u8(tone[:, :, index]) for index, name in enumerate(fmt.order)}
    if fmt.kind == "f16":
        linear = np.nan_to_num(np.asarray(values, dtype=np.float64), nan=0.0, posinf=1.0, neginf=0.0)
        if fmt.channels == 2:
            # A motion vector: -1..1 with zero at the middle.
            planes["x"] = _to_u8(np.clip(0.5 * (linear[:, :, 0] + 1.0), 0, 1))
            planes["y"] = _to_u8(np.clip(0.5 * (linear[:, :, 1] + 1.0), 0, 1))
            planes["zero"] = np.zeros(linear.shape[:2], dtype=np.uint8)
            return planes
        if fmt.channels == 1:
            # One float channel (a hierarchical-Z, a mask): the same value view a single float gets.
            return {fmt.order[0]: _value_plane(linear[:, :, 0])}
        tone = srgb_encode(np.clip(linear * exposure, 0.0, 1.0))
        for index, name in enumerate(fmt.order):
            planes[name] = _to_u8(tone[:, :, index])
        return planes
    if fmt.kind == "f32":
        raw = np.nan_to_num(np.asarray(values, dtype=np.float64), nan=0.0, posinf=0.0, neginf=0.0)
        if raw.ndim == 3 and raw.shape[2] == 2:
            planes["x"] = _to_u8(np.clip(0.5 * (raw[:, :, 0] + 1.0), 0, 1))
            planes["y"] = _to_u8(np.clip(0.5 * (raw[:, :, 1] + 1.0), 0, 1))
            return planes
        flat = raw.reshape(raw.shape[0], raw.shape[1])
        planes[fmt.order[0]] = _value_plane(flat)
        return planes
    if fmt.kind == "d16":
        return {fmt.order[0]: _to_u8(np.asarray(values, dtype=np.float64) / 65535.0)}
    if fmt.kind == "d24s8":
        depth = np.asarray(values, dtype=np.float64) / 16777215.0
        return {fmt.order[0]: _to_u8(depth)}
    if fmt.kind == "r10":
        packed = np.asarray(values, dtype=np.uint32)
        return {
            "r": _to_u8((packed & np.uint32(0x3FF)).astype(np.float64) / 1023.0),
            "g": _to_u8(((packed >> np.uint32(10)) & np.uint32(0x3FF)).astype(np.float64) / 1023.0),
            "b": _to_u8(((packed >> np.uint32(20)) & np.uint32(0x3FF)).astype(np.float64) / 1023.0),
            "a": _to_u8(((packed >> np.uint32(30)) & np.uint32(0x3)).astype(np.float64) / 3.0),
        }
    raise ValueError(f"no preview for kind {fmt.kind}")


def preview_rgb(decoded: Decoded, exposure: float = 1.0) -> np.ndarray:
    """A three-channel ``uint8`` preview of one decoded texture, ready to write as a PNG."""

    planes = channel_planes(decoded, exposure)
    height, width = decoded.values.shape[0], decoded.values.shape[1]
    if len(planes) == 1:  # a mask or a depth: grey, so a value reads as a value
        grey = next(iter(planes.values()))
        return np.dstack([grey, grey, grey])
    rgb = np.zeros((height, width, 3), dtype=np.uint8)
    rgb[:, :, 0] = planes.get("r", planes.get("x", np.zeros((height, width), dtype=np.uint8)))
    rgb[:, :, 1] = planes.get("g", planes.get("y", np.zeros((height, width), dtype=np.uint8)))
    rgb[:, :, 2] = planes.get("b", np.zeros((height, width), dtype=np.uint8))
    return rgb


def summary(decoded: Decoded, exposure: float = 1.0) -> str:
    """One line about what the file held, including the range a depth-like preview had to map."""

    fmt = decoded.fmt
    parts = [fmt.name, f"{decoded.fact.width}x{decoded.fact.height}", f"stride {decoded.stride}"]
    if not decoded.packed:
        parts.append("padded")
    parts.extend(decoded.notes)
    if fmt.note:
        parts.append(fmt.note)
    values = decoded.values
    if fmt.kind in ("f32", "r11", "d16", "d24s8", "f16", "unorm16") and getattr(values, "size", 0):
        raw = np.asarray(values, dtype=np.float64)
        raw = raw.reshape(raw.shape[0], raw.shape[1], -1) if raw.ndim == 3 else raw[:, :, None]
        finite = np.isfinite(raw)
        if finite.any():
            low = float(raw[finite].min())
            high = float(raw[finite].max())
            if fmt.kind == "f32":
                if low >= 0.0 and high <= 1.0:
                    parts.append(f"range {low:.6g}..{high:.6g}")
                else:
                    parts.append(f"range {low:.6g}..{high:.6g} mapped to 0..255")
            elif fmt.kind == "d16":
                parts.append(f"depth {low / 65535.0:.4f}..{high / 65535.0:.4f}")
            elif fmt.kind == "d24s8":
                parts.append(f"depth {low / 16777215.0:.4f}..{high / 16777215.0:.4f}")
            elif fmt.kind == "unorm16":
                parts.append(f"range {low / 65535.0:.4f}..{high / 65535.0:.4f}")
            elif fmt.kind == "r11":
                parts.append(f"linear range {low * exposure:.6g}..{high * exposure:.6g} after exposure")
            elif fmt.kind == "f16" and fmt.channels == 4:
                parts.append(f"linear range {low * exposure:.6g}..{high * exposure:.6g} after exposure")
            elif fmt.kind == "f16" and fmt.channels == 1:
                if low >= 0.0 and high <= 1.0:
                    parts.append(f"range {low:.6g}..{high:.6g}")
                else:
                    parts.append(f"range {low:.6g}..{high:.6g} mapped from the file's own minimum and maximum")
    return " ".join(parts)


# ---------------------------------------------------------------------------------------------------------------------
# PNG writing (no image library)
# ---------------------------------------------------------------------------------------------------------------------


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def png_bytes(image: np.ndarray) -> bytes:
    """A complete PNG file for a ``uint8`` array of shape (h, w) or (h, w, channels); channels 1, 2, 3 or 4."""

    array = np.ascontiguousarray(image, dtype=np.uint8)
    if array.ndim == 2:
        height, width = array.shape
        channels = 1
    elif array.ndim == 3 and array.shape[2] in (1, 2, 3, 4):
        height, width, channels = array.shape
        if channels == 1:
            array = array[:, :, 0]
    else:
        raise ValueError(f"cannot write a PNG from an array of shape {array.shape}")
    colour_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    raw = bytearray()
    for row in range(height):
        raw.append(0)  # filter type 0 (none): a preview is read by an eye, not by a compressor
        raw += array[row].tobytes()
    header = struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)
    return (PNG_SIGNATURE + _png_chunk(b"IHDR", header) +
            _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _png_chunk(b"IEND", b""))


def write_png(path: str, image: np.ndarray) -> None:
    with open(path, "wb") as handle:
        handle.write(png_bytes(image))


def read_png(path: str) -> tuple[int, int, int, bytes]:
    """Read back the first IDAT of a PNG this module wrote: (width, height, channels, unfiltered bytes).

    Only what the selftest needs: one IDAT, filter type 0 on every row, 8 bits per channel.
    """

    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    offset = len(PNG_SIGNATURE)
    width = height = channels = 0
    idat = bytearray()
    while offset + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, offset)
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour_type = struct.unpack(">IIBB", payload[:10])
            if depth != 8:
                raise ValueError("only 8-bit PNGs are written")
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[colour_type]
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = width * channels + 1
    if any(raw[row * stride] != 0 for row in range(height)):
        raise ValueError("a written PNG used a filter")
    pixels = b"".join(raw[row * stride + 1:(row + 1) * stride] for row in range(height))
    return width, height, channels, pixels


# ---------------------------------------------------------------------------------------------------------------------
# The tool
# ---------------------------------------------------------------------------------------------------------------------


def convert_capture(capture: drawdiag.Capture, out_folder: str | None = None, exposure: float = 1.0,
                    channels: bool = False) -> list[str]:
    """Convert every dumped texture of a capture; returns the lines to print (and prints nothing itself)."""

    lines: list[str] = []
    if capture.dump is None:
        lines.append(f"no {drawdiag.DUMP_FOLDER}\\{drawdiag.DUMP_FACTS_FILE} in {capture.folder}: nothing was dumped")
        return lines
    facts = capture.dump
    for name, reason in facts.skipped:
        lines.append(f"{name}: skipped by the dump: {reason}")
    out = out_folder or os.path.join(capture.folder, DEFAULT_OUT_FOLDER)
    os.makedirs(out, exist_ok=True)
    for fact in facts.facts:
        path = facts.path_of(fact)
        if not os.path.isfile(path):
            lines.append(f"{fact.file}: facts.txt lists it but the file is missing")
            continue
        fmt = resolve_format(fact.format)
        if fmt is None:
            lines.append(f"{fact.file}: format {fact.format!r} is not one this tool decodes")
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            decoded = decode(fact, data, fmt)
        except ValueError as error:
            lines.append(f"{fact.file}: {error}")
            continue
        base = os.path.splitext(os.path.basename(fact.file))[0]
        image = preview_rgb(decoded, exposure)
        png_path = os.path.join(out, base + ".png")
        write_png(png_path, image)
        lines.append(f"{fact.file} -> {os.path.basename(png_path)}  ({summary(decoded, exposure)})")
        if channels:
            for channel, plane in channel_planes(decoded, exposure).items():
                channel_path = os.path.join(out, f"{base}_{channel}.png")
                write_png(channel_path, plane)
                lines.append(f"  channel {channel}: {os.path.basename(channel_path)}")
        if fmt.kind in ("f16", "f32", "r11"):
            array = np.asarray(decoded.values)
            npy_path = os.path.join(out, base + ".raw.npy")
            np.save(npy_path, array)
            lines.append(f"  raw floats: {os.path.basename(npy_path)} shape {array.shape} dtype {array.dtype}")
    lines.append(f"{len(facts.facts)} dumped texture(s), written under {out}")
    return lines


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments or arguments[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if arguments else 2
    if arguments[0] == "--formats":
        for name in sorted(FORMATS):
            fmt = FORMATS[name]
            print(f"{name:28s} {fmt.dxgi:3d}  {fmt.bpp} B/texel  {fmt.kind}  {','.join(fmt.order)}")
        return 0
    folder = arguments[0]
    out: str | None = None
    exposure = 1.0
    channels = False
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--out" and index + 1 < len(arguments):
            out = arguments[index + 1]
            index += 2
        elif argument == "--exposure" and index + 1 < len(arguments):
            try:
                exposure = float(arguments[index + 1])
            except ValueError:
                print("--exposure needs a number", file=sys.stderr)
                return 2
            index += 2
        elif argument == "--channels":
            channels = True
            index += 1
        else:
            print(f"unknown argument: {argument}", file=sys.stderr)
            return 2
    capture = drawdiag.load_capture(folder)
    if capture.draws is None and capture.dump is None:
        print(f"{folder} is not a capture folder", file=sys.stderr)
        return 2
    lines = convert_capture(capture, out, exposure, channels)
    for line in lines:
        print(line)
    return 0 if capture.dump is not None else 2


if __name__ == "__main__":
    sys.exit(main())
