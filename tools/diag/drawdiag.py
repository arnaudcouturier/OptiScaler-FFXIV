r"""Reader for the files the developer draw diagnostic writes.

One capture is one folder: ``draws.csv`` (one row per draw), ``cb.csv`` (the sun and camera read back once a second),
``cbdump.csv`` (the bytes of the named constant buffers, sampled once a second), ``views.txt`` (the shader-resource
description of four identifying textures), ``shaders.txt`` (the kept pixel shaders' bytecode and what each one
declares, marked with the origin it was kept at), ``engine.txt`` (the engine pass overrides' counters) and, when the
one-frame dump ran, a ``dump\`` folder with raw ``.bin`` textures and ``facts.txt``.

The reader knows format 2 (no resource pointer in a resource entry) and format 3 (every SRV/RTV/DSV entry ends with the
resource pointer, every constant-buffer entry with the buffer pointer), and it never trusts a field it does not need:
unknown trailing fields are ignored, a short line is padded, a line that does not parse becomes a notice instead of an
exception, and a file cut before its ``# rows`` trailer is reported.  A reader that raises on the first surprise is
useless on a capture that arrived through a copy or a full disk; a reader that silently accepts everything hides the
very failure the trailer exists for.

Run the tools in this folder with ``python -I``; this module is standard library only.
"""

from __future__ import annotations

import csv
import os
import re
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Sequence

# ---------------------------------------------------------------------------------------------------------------------
# Notices: what a file does not carry, or carries differently from what this reader expects
# ---------------------------------------------------------------------------------------------------------------------

KIND_FORMAT = "format"
KIND_TRAILER_MISSING = "trailer-missing"
KIND_TRAILER_MISMATCH = "trailer-mismatch"
KIND_TRUNCATED = "truncated"
KIND_PARSE = "parse"
KIND_LAYOUT = "layout"
KIND_HEX = "hex"

# Format 3 is the first one whose files end with a ``# rows`` trailer; format 2 files legitimately end without one.
TRAILER_SINCE = 3

DRAWS_FIELDS = (
    "frame", "ordinal", "api", "context", "count", "instances", "start", "base", "stage", "ps_crc",
    "srv", "cb", "rtv", "dsv", "viewport", "blend", "depth", "markers",
)

MARKER_TONE = "tone"
MARKER_UPSCALE = "upscale"
MARKER_FIRST_UI = "ui1"


@dataclass(frozen=True)
class Notice:
    """One thing a reader had to report about a file, with a machine-readable kind."""

    kind: str
    text: str

    def __str__(self) -> str:  # makes ``print(notice)`` read like a warning line
        return f"{self.kind}: {self.text}"


def _notice(kind: str, text: str, notices: list[Notice]) -> None:
    """Append one notice, keeping the file's report bounded and readable."""

    entry = Notice(kind, text)
    if entry not in notices:
        notices.append(entry)


# ---------------------------------------------------------------------------------------------------------------------
# Small value parsers
# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Size:
    """A ``<width>x<height>`` size as the diagnostic writes it."""

    width: int
    height: int

    def __str__(self) -> str:
        return f"{self.width}x{self.height}"

    @property
    def pixels(self) -> int:
        return self.width * self.height


_SIZE_RE = re.compile(r"^\s*(\d+)\s*[xX]\s*(\d+)\s*$")


def parse_size(text: str | None) -> Size | None:
    """Parse ``2560x1440``; ``-``, empty or anything else gives ``None``."""

    if not text:
        return None
    match = _SIZE_RE.match(text)
    if not match:
        return None
    return Size(int(match.group(1)), int(match.group(2)))


def parse_int(text: str | None) -> int | None:
    """Parse the diagnostic's integer columns; ``-`` and empty give ``None``."""

    if text is None:
        return None
    text = text.strip()
    if not text or text == "-":
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def parse_float(text: str | None) -> float | None:
    if text is None:
        return None
    text = text.strip()
    if not text or text == "-":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def parse_pointer(text: str | None) -> int | None:
    """A resource pointer as written (lowercase hex, no ``0x``).

    ``0`` is the writer's "no resource": it is returned as ``None`` so that a join over pointers can never match two
    absent resources.  A value with ``0x`` is accepted too, because a hand-written fixture and an older build both do it.
    """

    if text is None:
        return None
    text = text.strip()
    if not text or text == "-":
        return None
    if text.lower().startswith("0x"):
        text = text[2:]
    if not text:
        return None
    try:
        value = int(text, 16)
    except ValueError:
        return None
    return value if value != 0 else None


def hex_pointer(value: int | None) -> str:
    """The pointer as the diagnostic prints it, ``-`` for none."""

    return "-" if value is None else format(value, "x")


def crc_key(crc: int | str | None) -> str:
    """A shader CRC in the one form the files are compared in: lowercase hex, no ``0x``.

    ``draws.csv`` and ``cbdump.csv`` write it as ``0xXXXXXXXX`` and ``shaders.txt`` as ``XXXXXXXX``; the numeric value is what
    identifies a shader, so every join between them goes through this function.
    """

    if crc is None:
        return ""
    if isinstance(crc, int):
        return format(crc, "x")
    text = crc.strip().lower()
    text = text[2:] if text.startswith("0x") else text
    # The numeric value is the key: ``0C0FFEE0`` in shaders.txt and the integer 0x0C0FFEE0 of a draw row are the same shader, so a
    # leading zero of the text form must not keep the two apart.
    try:
        return format(int(text, 16), "x") if text else ""
    except ValueError:
        return text


def parse_flags(text: str | None) -> tuple[int, ...] | None:
    """The ``a/b/c/d`` columns (blend, depth state) as integers; ``None`` when the column is empty or ``-``."""

    if not text or text.strip() in ("", "-"):
        return None
    parts = [part.strip() for part in text.split("/")]
    values: list[int] = []
    for part in parts:
        number = parse_int(part)
        if number is None:
            return None
        values.append(number)
    return tuple(values)


# ---------------------------------------------------------------------------------------------------------------------
# Entries of a draw row's variable-length columns
# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class TextureEntry:
    """One shader resource or render target of a draw: the slot, the reflected name (SRVs only), format, size, pointer.

    Format 2 rows carry no pointer (``pointer is None`` in every entry); format 3 rows carry one at the end.  A name of
    ``-`` means the shader declares no binding for that slot, which is different from an unbound slot: the diagnostic
    writes nothing at all for a slot the shader never declares.
    """

    raw: str
    slot: int | None
    name: str | None  # None for "-" and for render targets (they have no reflected name)
    format: str | None
    size: Size | None
    pointer: int | None

    def __str__(self) -> str:
        name = self.name or "-"
        shown = str(self.size) if self.size else (self.format or "-")
        return f"{self.slot}:{name}:{shown}:{hex_pointer(self.pointer)}"


@dataclass(frozen=True)
class BufferEntry:
    """One constant buffer of a draw: the slot, the reflected name, its bound size in bytes and (format 3) its pointer."""

    raw: str
    slot: int | None
    name: str | None
    bytes: int | None
    pointer: int | None

    def __str__(self) -> str:
        return f"{self.slot}:{self.name or '-'}:{self.bytes if self.bytes is not None else '-'}:" \
               f"{hex_pointer(self.pointer)}"


@dataclass(frozen=True)
class DepthEntry:
    """The depth view of a draw: format, size and (format 3) the texture's pointer."""

    raw: str
    format: str | None
    size: Size | None
    pointer: int | None


@dataclass(frozen=True)
class ViewportRect:
    """One ``widthxheight+left+top`` rectangle of a draw's viewport column; the half-texel offsets stay floats."""

    width: int
    height: int
    left: float
    top: float

    def __str__(self) -> str:
        return f"{self.width}x{self.height}+{self.left:g}+{self.top:g}"


@dataclass(frozen=True)
class ViewportSet:
    """The viewport column: the number of viewports the draw set, then that many rectangles joined by ``|``.

    The writer puts the count of viewports before the first rectangle (``2:2560x1440+0.5+0.5|1280x720+0+0``), so the
    leading number is a count and not a slot: a draw with several viewports has one entry and N rectangles.  A column
    whose count and rectangle count disagree is read as written and reported, because the rectangles are what a caller
    reads and a wrong count must not throw them away.
    """

    raw: str
    count: int | None
    rects: tuple[ViewportRect, ...]

    @property
    def first(self) -> ViewportRect | None:
        return self.rects[0] if self.rects else None

    @property
    def consistent(self) -> bool:
        """Whether the leading count names as many rectangles as the column holds."""

        return self.count is None or self.count == len(self.rects)

    def __str__(self) -> str:
        prefix = f"{self.count}:" if self.count is not None else ""
        return prefix + "|".join(str(rect) for rect in self.rects)


_VIEWPORT_RECT_RE = re.compile(r"^\s*(\d+)\s*[xX]\s*(\d+)\s*([+-]\s*[-+0-9.eE]+)\s*([+-]\s*[-+0-9.eE]+)\s*$")


def _parse_viewport_rect(text: str) -> ViewportRect | None:
    match = _VIEWPORT_RECT_RE.match(text)
    if not match:
        return None
    try:
        return ViewportRect(
            width=int(match.group(1)),
            height=int(match.group(2)),
            left=float(match.group(3).replace(" ", "")),
            top=float(match.group(4).replace(" ", "")),
        )
    except ValueError:
        return None


def parse_viewports(text: str | None, notices: list[Notice] | None = None) -> ViewportSet | None:
    """Parse the viewport column: ``count:WxH+x+y`` or ``count:WxH+x+y|WxH+x+y|...``.

    A rectangle that does not parse is dropped (and reported when ``notices`` is given), so one damaged rectangle does
    not hide the others; an empty or ``-`` column is ``None``.
    """

    if not text or text.strip() in ("", "-"):
        return None
    raw = text.strip()
    body = raw
    count: int | None = None
    if ":" in body:
        head, _, tail = body.partition(":")
        count = parse_int(head)
        if count is None:
            if notices is not None:
                _notice(KIND_PARSE, f"viewport column has no leading count: {raw!r}", notices)
            return None
        body = tail
    rects: list[ViewportRect] = []
    for part in body.split("|"):
        rect = _parse_viewport_rect(part)
        if rect is None:
            if notices is not None and part.strip():
                _notice(KIND_PARSE, f"viewport rectangle dropped: {part.strip()!r}", notices)
            continue
        rects.append(rect)
    if not rects:
        return None
    viewports = ViewportSet(raw=raw, count=count, rects=tuple(rects))
    if notices is not None and not viewports.consistent:
        _notice(KIND_LAYOUT, f"viewport column counts {count} viewport(s) and holds {len(rects)}: {raw!r}", notices)
    return viewports


def parse_viewport(text: str | None) -> ViewportRect | None:
    """The draw's first viewport rectangle, which is the one a single-viewport pass sets."""

    viewports = parse_viewports(text)
    return viewports.first if viewports else None


def _split_entries(text: str | None) -> list[str]:
    """The ``|``-separated entries of a column, without the empty ones."""

    if not text or text.strip() in ("", "-"):
        return []
    return [part for part in text.split("|") if part.strip() and part.strip() != "-"]


def parse_texture_entry(text: str, kind: str = "srv") -> TextureEntry | None:
    """Parse one SRV or RTV entry; tolerate unknown trailing fields and an absent pointer.

    Shape: ``slot:name:format:size[:pointer]`` for an SRV, ``slot:format:size[:pointer]`` for a render target.  The
    column says which one it is, so no field has to be guessed: a format-2 SRV simply has no fifth field, and a
    format-2 RTV no fourth.  Extra fields after the pointer are ignored (a later format may append one).
    """

    fields = text.split(":")
    if kind == "rtv":
        if len(fields) < 3:
            return None
        slot = parse_int(fields[0])
        fmt = None if fields[1] in ("-", "") else fields[1]
        size = parse_size(fields[2])
        pointer = parse_pointer(fields[3]) if len(fields) >= 4 else None
        return TextureEntry(raw=text, slot=slot, name=None, format=fmt, size=size, pointer=pointer)
    if len(fields) < 4:
        return None
    slot = parse_int(fields[0])
    name = None if fields[1] in ("-", "") else fields[1]
    fmt = None if fields[2] in ("-", "") else fields[2]
    size = parse_size(fields[3])
    pointer = parse_pointer(fields[4]) if len(fields) >= 5 else None
    return TextureEntry(raw=text, slot=slot, name=name, format=fmt, size=size, pointer=pointer)


def parse_buffer_entry(text: str) -> BufferEntry | None:
    """Parse one constant-buffer entry: ``slot:name:bytes[:pointer]``."""

    fields = text.split(":")
    if len(fields) < 3:
        return None
    slot = parse_int(fields[0])
    name = None if fields[1] in ("-", "") else fields[1]
    size = parse_int(fields[2])
    pointer = parse_pointer(fields[3]) if len(fields) >= 4 else None
    return BufferEntry(raw=text, slot=slot, name=name, bytes=size, pointer=pointer)


def parse_depth_entry(text: str | None) -> DepthEntry | None:
    """Parse the depth view: ``format:size[:pointer]`` (format 2 has no pointer, so two fields)."""

    if not text or text.strip() in ("", "-"):
        return None
    fields = text.split(":")
    fmt = None if fields[0] in ("-", "") else fields[0]
    size = parse_size(fields[1]) if len(fields) >= 2 else None
    pointer = parse_pointer(fields[2]) if len(fields) >= 3 else None
    return DepthEntry(raw=text, format=fmt, size=size, pointer=pointer)


# ---------------------------------------------------------------------------------------------------------------------
# One draw
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class Draw:
    """One row of ``draws.csv``."""

    frame: int
    ordinal: int
    api: str
    context: str
    count: int
    instances: int
    start: int
    base: int
    stage: int
    ps_crc: int | None
    ps_crc_text: str
    srvs: list[TextureEntry]
    cbs: list[BufferEntry]
    rtvs: list[TextureEntry]
    dsv: DepthEntry | None
    viewports: ViewportSet | None
    blend: tuple[int, ...] | None
    depth: tuple[int, ...] | None
    markers: frozenset[str]
    raw: str = ""

    # ---- convenience, used by every tool in this folder -----------------------------------------------------------

    @property
    def crc_text(self) -> str:
        """The pixel shader's CRC as written, ``-`` when the row names none."""

        return self.ps_crc_text

    @property
    def is_game(self) -> bool:
        return self.context == "game"

    @property
    def viewport(self) -> ViewportRect | None:
        """The first viewport rectangle of the row, which is the whole viewport for every single-viewport draw."""

        return self.viewports.first if self.viewports else None

    @property
    def target_count(self) -> int:
        return len(self.rtvs)

    def has_marker(self, marker: str) -> bool:
        return marker in self.markers

    @property
    def srv_names(self) -> list[str]:
        """The reflected names bound as shader resources, without the compiler's type suffix.

        The engine's compiler writes the resource type into the name (``g_SamplerDepth.T``), so every name a caller
        compares has to be the base name; a raw compare finds nothing for a texture.
        """

        return [base_texture_name(entry.name) for entry in self.srvs if entry.name]

    @property
    def srv_names_raw(self) -> list[str]:
        """The names exactly as the capture wrote them, suffix included."""

        return [entry.name for entry in self.srvs if entry.name]

    @property
    def srv_textures(self) -> list[TextureEntry]:
        """The SRVs that describe a texture (a name or a format and a size), not the unnamed slots."""

        return [entry for entry in self.srvs if entry.name or entry.size]

    @property
    def cb_names(self) -> list[str]:
        return [entry.name for entry in self.cbs if entry.name]

    @property
    def srv_pointers(self) -> set[int]:
        return {entry.pointer for entry in self.srvs if entry.pointer is not None}

    @property
    def rtv_pointers(self) -> set[int]:
        return {entry.pointer for entry in self.rtvs if entry.pointer is not None}

    @property
    def bound_pointers(self) -> set[int]:
        """Every resource pointer the row names: SRV textures, render targets, depth view and constant buffers."""

        pointers = set(self.srv_pointers) | set(self.rtv_pointers)
        if self.dsv is not None and self.dsv.pointer is not None:
            pointers.add(self.dsv.pointer)
        pointers |= {entry.pointer for entry in self.cbs if entry.pointer is not None}
        return pointers

    def srv_sizes(self, name: str | None = None) -> list[Size]:
        return [entry.size for entry in self.srvs if entry.size and (name is None or entry.name == name)]

    def bound(self, name: str) -> TextureEntry | None:
        """The first SRV entry bound to ``name`` (compared by base name, so ``.T``/``.S`` suffixes do not matter)."""

        wanted = base_texture_name(name)
        for entry in self.srvs:
            if base_texture_name(entry.name) == wanted:
                return entry
        return None

    def binds_any(self, names: Iterable[str]) -> bool:
        wanted = {base_texture_name(name) for name in names}
        return any(base_texture_name(entry.name) in wanted for entry in self.srvs)

    def binds_all(self, names: Iterable[str]) -> bool:
        """Whether every one of ``names`` is bound as a shader resource (compared by base name).

        A pass is identified by what it reads *together*: the one that needs the G-buffer and the view position is not
        the one that needs either of them, so the test is a subset and not an overlap.
        """

        wanted = {base_texture_name(name) for name in names}
        if not wanted:
            return False
        bound = {base_texture_name(entry.name) for entry in self.srvs}
        return wanted <= bound

    def binds_prefix(self, prefix: str) -> bool:
        return any(base_texture_name(entry.name).startswith(prefix) for entry in self.srvs)

    def cb(self, name: str) -> BufferEntry | None:
        """The constant buffer bound to ``name`` (compared by base name, like the textures)."""

        wanted = base_texture_name(name)
        for entry in self.cbs:
            if base_texture_name(entry.name) == wanted:
                return entry
        return None

    def signature(self) -> tuple:
        """What identifies the *shape* of the draw: targets, depth, the bound names and constants, the state."""

        return (
            self.api, self.count, self.stage, self.ps_crc,
            tuple((entry.slot, entry.format, str(entry.size or "")) for entry in self.rtvs),
            (self.dsv.format, str(self.dsv.size or "")) if self.dsv else None,
            tuple(sorted(self.srv_names)),
            tuple((entry.name, entry.bytes) for entry in self.cbs if entry.name),
        )


# ---------------------------------------------------------------------------------------------------------------------
# Frame windows: one ``# frame`` comment line each, plus the rows the file holds for it
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class FrameWindow:
    """One captured frame window as the file describes it, and what the file actually holds for it.

    ``draws``, ``tone``, ``ui1``, ``upscale_after`` and ``omitted`` come from the ``# frame`` line; the counts after
    ``#`` are the writer's own, and ``row_count``/``tone_rows`` are measured from the rows, which is what makes a
    disagreement visible.
    """

    frame: int
    draws: int = 0
    tone: int = 0
    ui1: int = 0
    upscale_after: int = 0
    omitted: int = 0
    extra: dict[str, int] = field(default_factory=dict)
    row_count: int = 0
    tone_rows: list[int] = field(default_factory=list)
    ui_rows: list[int] = field(default_factory=list)
    upscale_rows: list[int] = field(default_factory=list)

    @property
    def complete(self) -> bool:
        """Whether the row cap dropped nothing: a window with ``omitted`` rows is not a whole frame in the file."""

        return self.omitted == 0

    @property
    def has_tone(self) -> bool:
        return self.tone != 0

    @property
    def tone_marker_rows(self) -> list[int]:
        return self.tone_rows

    @property
    def rows_missing(self) -> int:
        return max(0, self.draws - self.row_count)


_FRAME_LINE_RE = re.compile(r"^#\s*frame\s+(\d+)\s*(.*)$")
_TRAILER_RE = re.compile(r"^#\s*rows\s+(\d+)(?:\s+frames\s+(\d+))?(?:\s+omitted\s+(\d+))?\s*$")


def _parse_frame_line(text: str, notices: list[Notice]) -> FrameWindow | None:
    match = _FRAME_LINE_RE.match(text)
    if not match:
        return None
    window = FrameWindow(frame=int(match.group(1)))
    tokens = match.group(2).split()
    index = 0
    while index + 1 < len(tokens):
        key, value = tokens[index], tokens[index + 1]
        number = parse_int(value)
        index += 2
        if number is None:
            continue
        if key == "draws":
            window.draws = number
        elif key == "tone":
            window.tone = number
        elif key == "ui1":
            window.ui1 = number
        elif key == "upscale_after":
            window.upscale_after = number
        elif key == "omitted":
            window.omitted = number
        else:
            window.extra[key] = number
    if index < len(tokens):
        _notice(KIND_PARSE, f"frame line has an odd trailing token: {text!r}", notices)
    return window


@dataclass
class Trailer:
    rows: int
    frames: int | None = None
    omitted: int | None = None
    text: str = ""


def parse_trailer(text: str) -> Trailer | None:
    match = _TRAILER_RE.match(text)
    if not match:
        return None
    return Trailer(
        rows=int(match.group(1)),
        frames=int(match.group(2)) if match.group(2) is not None else None,
        omitted=int(match.group(3)) if match.group(3) is not None else None,
        text=text,
    )


# ---------------------------------------------------------------------------------------------------------------------
# The files
# ---------------------------------------------------------------------------------------------------------------------


def _open_text(path: str) -> Iterator[str]:
    """Lines of a text file as UTF-8, with a BOM stripped and undecodable bytes replaced (a capture is not a document)."""

    with open(path, "r", encoding="utf-8-sig", errors="replace", newline="") as handle:
        for line in handle:
            yield line.rstrip("\r\n")


def _first_comment_format(lines: Sequence[str]) -> int | None:
    for line in lines:
        if line.startswith("#"):
            match = re.search(r"format\s+(\d+)", line)
            if match:
                return int(match.group(1))
    return None


class _Table:
    """A CSV-shaped file with ``#`` comments, a header line, rows and an optional trailer.

    The split is kept lazy-free and simple: the caller iterates the lines once and hands each non-comment line over.
    """

    def __init__(self, path: str) -> None:
        self.path = path
        self.comments: list[str] = []
        self.header: list[str] | None = None
        self.rows: list[dict[str, str]] = []
        self.trailer: Trailer | None = None
        self.has_truncated = False
        self.notices: list[Notice] = []

    def read(self) -> "_Table":
        if not os.path.isfile(self.path):
            _notice(KIND_PARSE, f"{os.path.basename(self.path)} was not written", self.notices)
            return self
        first = True
        for line in _open_text(self.path):
            if first:
                first = False
            if not line.strip():
                continue
            if line.startswith("#"):
                self.comments.append(line)
                if "TRUNCATED" in line:
                    self.has_truncated = True
                trailer = parse_trailer(line)
                if trailer is not None:
                    self.trailer = trailer
                continue
            if self.header is None:
                self.header = self._parse_header(line)
                continue
            values = next(csv.reader([line]))
            self.rows.append(self._row_dict(values))
        return self

    def _parse_header(self, line: str) -> list[str]:
        names = [name.strip() for name in next(csv.reader([line]))]
        if not names or not names[0]:
            _notice(KIND_LAYOUT, f"{os.path.basename(self.path)} has a header with no first column: {line!r}", self.notices)
        return names

    def _row_dict(self, values: list[str]) -> dict[str, str]:
        header = self.header or []
        if len(values) > len(header):
            _notice(KIND_LAYOUT, f"{os.path.basename(self.path)} has a row with {len(values)} fields, the header "
                                  f"names {len(header)}: extra fields ignored", self.notices)
        row = {name: "" for name in header}
        for name, value in zip(header, values):
            row[name] = value
        return row


def _check_trailer(table: _Table, fmt: int | None, what: str, frame_lines: int | None = None) -> None:
    """Compare the ``# rows`` trailer with what the file holds (the writer's self-check)."""

    if table.trailer is None:
        if fmt is not None and fmt >= TRAILER_SINCE:
            _notice(KIND_TRAILER_MISSING, f"{what} has no '# rows' trailer: the copy is cut off or the writer "
                                         f"did not finish", table.notices)
        else:
            _notice(KIND_TRAILER_MISSING, f"{what} has no '# rows' trailer (written before format "
                                         f"{TRAILER_SINCE}; the count cannot be checked)", table.notices)
        return
    trailer = table.trailer
    if trailer.rows != len(table.rows):
        _notice(KIND_TRAILER_MISMATCH,
                f"{what} trailer says {trailer.rows} row(s), the file holds {len(table.rows)}",
                table.notices)
    if frame_lines is not None and trailer.frames is not None and trailer.frames != frame_lines:
        _notice(KIND_TRAILER_MISMATCH,
                f"{what} trailer says {trailer.frames} frame(s), the file holds {frame_lines} '# frame' line(s)",
                table.notices)


@dataclass
class Draws:
    """``draws.csv`` of one capture."""

    path: str
    format: int | None = None
    rows: list[Draw] = field(default_factory=list)
    windows: list[FrameWindow] = field(default_factory=list)
    trailer: Trailer | None = None
    truncated: bool = False
    notices: list[Notice] = field(default_factory=list)

    # ---- lookups --------------------------------------------------------------------------------------------------

    def window(self, frame: int) -> FrameWindow | None:
        for window in self.windows:
            if window.frame == frame:
                return window
        return None

    @property
    def frames(self) -> list[int]:
        return [window.frame for window in self.windows] or sorted({row.frame for row in self.rows})

    def of_frame(self, frame: int) -> list[Draw]:
        return [row for row in self.rows if row.frame == frame]

    def markers(self, marker: str, frame: int | None = None) -> list[Draw]:
        return [row for row in self.rows if row.has_marker(marker) and (frame is None or row.frame == frame)]

    def by_ordinal(self, ordinal: int, frame: int | None = None) -> list[Draw]:
        return [row for row in self.rows if row.ordinal == ordinal and (frame is None or row.frame == frame)]

    @property
    def total_omitted(self) -> int:
        return sum(window.omitted for window in self.windows)

    def summary(self) -> str:
        frames = len(self.windows)
        return f"format {self.format if self.format is not None else '?'}: {len(self.rows)} row(s), {frames} frame(s)"


def load_draws(path: str) -> Draws:
    """Load ``draws.csv``; never raises on a malformed line, reports it instead."""

    table = _Table(path).read()
    data = Draws(path=path, format=_first_comment_format(table.comments), trailer=table.trailer,
                 truncated=table.has_truncated)
    if table.header is not None:
        missing = [name for name in DRAWS_FIELDS if name not in table.header]
        if missing:
            _notice(KIND_LAYOUT, f"draws.csv header does not name {', '.join(missing)}", data.notices)
    for raw in table.rows:
        try:
            data.rows.append(_draw_from_row(raw, data.notices))
        except (ValueError, TypeError) as error:  # a corrupted line must not stop the rest of the file
            _notice(KIND_PARSE, f"draw row dropped ({error}): {raw}", data.notices)
    for comment in table.comments:
        window = _parse_frame_line(comment, data.notices)
        if window is not None:
            data.windows.append(window)
    # Join the rows to their window: the counts and marker ordinals of the comment line against what was written.
    by_frame: dict[int, list[Draw]] = {}
    for row in data.rows:
        by_frame.setdefault(row.frame, []).append(row)
    for window in data.windows:
        rows = by_frame.get(window.frame, [])
        window.row_count = len(rows)
        window.tone_rows = [row.ordinal for row in rows if row.has_marker(MARKER_TONE)]
        window.ui_rows = [row.ordinal for row in rows if row.has_marker(MARKER_FIRST_UI)]
        window.upscale_rows = [row.ordinal for row in rows if row.has_marker(MARKER_UPSCALE)]
    rows_without_window = sorted(set(by_frame) - {window.frame for window in data.windows})
    if rows_without_window and data.windows:
        _notice(KIND_LAYOUT, f"rows for frame(s) {rows_without_window} have no '# frame' line", data.notices)
    if data.format is not None and data.format > 3:
        _notice(KIND_FORMAT, f"draws.csv says format {data.format}; this reader knows 2 and 3 and reads it as 3",
                data.notices)
    _check_trailer(table, data.format, "draws.csv", frame_lines=len(data.windows))
    data.notices.extend(table.notices)
    return data


def _draw_from_row(raw: dict[str, str], notices: list[Notice] | None = None) -> Draw:
    def text(name: str) -> str:
        return (raw.get(name) or "").strip()

    crc_text = text("ps_crc")
    srvs = [entry for part in _split_entries(text("srv")) if (entry := parse_texture_entry(part, "srv")) is not None]
    cbs = [entry for part in _split_entries(text("cb")) if (entry := parse_buffer_entry(part)) is not None]
    rtvs = [entry for part in _split_entries(text("rtv")) if (entry := parse_texture_entry(part, "rtv")) is not None]
    markers_text = text("markers")
    markers = frozenset(part for part in re.split(r"[+;,]", markers_text) if part.strip()) if markers_text else frozenset()
    return Draw(
        frame=parse_int(text("frame")) or 0,
        ordinal=parse_int(text("ordinal")) or 0,
        api=text("api"),
        context=text("context") or "game",
        count=parse_int(text("count")) or 0,
        instances=parse_int(text("instances")) or 0,
        start=parse_int(text("start")) or 0,
        base=parse_int(text("base")) or 0,
        stage=parse_int(text("stage")) or 0,
        ps_crc=parse_int(crc_text),
        ps_crc_text=crc_text if crc_text else "-",
        srvs=srvs,
        cbs=cbs,
        rtvs=rtvs,
        dsv=parse_depth_entry(text("dsv")),
        viewports=parse_viewports(text("viewport"), notices),
        blend=parse_flags(text("blend")),
        depth=parse_flags(text("depth")),
        markers=markers,
        raw=",".join(raw.get(name, "") for name in DRAWS_FIELDS),
    )


# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class CbSample:
    """One row of ``cb.csv``: the light constant buffer of one draw, read once a second."""

    frame: int
    time_s: float | None
    ps_crc: int | None
    ps_crc_text: str
    name: str
    slot: int | None
    bytes: int | None
    position: tuple[float, float, float] | None
    direction: tuple[float, float, float] | None
    diffuse: tuple[float, float, float] | None
    view: tuple[float, ...] | None
    view_projection: tuple[float, ...] | None


@dataclass
class CbReads:
    """``cb.csv`` of one capture."""

    path: str
    format: int | None = None
    rows: list[CbSample] = field(default_factory=list)
    trailer: Trailer | None = None
    truncated: bool = False
    notices: list[Notice] = field(default_factory=list)


def load_cb(path: str) -> CbReads:
    table = _Table(path).read()
    data = CbReads(path=path, format=_first_comment_format(table.comments), trailer=table.trailer,
                   truncated=table.has_truncated)
    for raw in table.rows:
        values = [parse_float(raw.get(name)) for name in ("pos_x", "pos_y", "pos_z", "dir_x", "dir_y", "dir_z",
                                                          "diffuse_r", "diffuse_g", "diffuse_b")]
        view_values = [parse_float(raw.get(f"view_{row}{column}")) for row in range(4) for column in range(4)]
        view = tuple(value for value in view_values if value is not None)
        vp_values = [parse_float(raw.get(f"vp_{row}{column}")) for row in range(4) for column in range(4)]
        view_projection = tuple(value for value in vp_values if value is not None)
        crc_text = (raw.get("ps_crc") or "").strip()
        data.rows.append(CbSample(
            frame=parse_int(raw.get("frame")) or 0,
            time_s=parse_float(raw.get("time_s")),
            ps_crc=parse_int(crc_text),
            ps_crc_text=crc_text or "-",
            name=(raw.get("cb") or "").strip() or "-",
            slot=parse_int(raw.get("slot")),
            bytes=parse_int(raw.get("bytes")),
            position=tuple(values[0:3]) if all(value is not None for value in values[0:3]) else None,
            direction=tuple(values[3:6]) if all(value is not None for value in values[3:6]) else None,
            diffuse=tuple(values[6:9]) if all(value is not None for value in values[6:9]) else None,
            view=view if len(view) == 16 else None,
            view_projection=view_projection if len(view_projection) == 16 else None,
        ))
    _check_trailer(table, data.format, "cb.csv")
    data.notices.extend(table.notices)
    return data


# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class CbDumpSample:
    """One row of ``cbdump.csv``: the bytes of a named constant buffer at one sample."""

    frame: int
    time_s: float | None
    ps_crc: int | None
    ps_crc_text: str
    name: str
    slot: int | None
    bytes: int | None
    pointer: int | None
    data: bytes
    hex_text: str

    @property
    def short(self) -> bool:
        """Whether the row carries fewer bytes than the buffer's bound size (the writer caps the hex at 16 KiB)."""

        return self.bytes is not None and len(self.data) < self.bytes


@dataclass
class CbDump:
    """``cbdump.csv`` of one capture."""

    path: str
    format: int | None = None
    rows: list[CbDumpSample] = field(default_factory=list)
    trailer: Trailer | None = None
    truncated: bool = False
    notices: list[Notice] = field(default_factory=list)

    def of_name(self, name: str) -> list[CbDumpSample]:
        wanted = base_texture_name(name)
        return [row for row in self.rows if base_texture_name(row.name) == wanted]

    def names(self) -> list[str]:
        seen: list[str] = []
        for row in self.rows:
            if row.name not in seen:
                seen.append(row.name)
        return seen


def _hex_to_bytes(text: str, notices: list[Notice], where: str) -> bytes:
    text = (text or "").strip()
    if not text or text == "-":
        return b""
    if len(text) % 2:
        _notice(KIND_HEX, f"{where}: odd hex length, the last nibble was dropped", notices)
        text = text[:-1]
    try:
        return bytes.fromhex(text)
    except ValueError:
        # A single bad character must not throw away the whole row: keep the good prefix.
        good = []
        for index in range(0, len(text) - 1, 2):
            pair = text[index:index + 2]
            try:
                good.append(int(pair, 16))
            except ValueError:
                break
        _notice(KIND_HEX, f"{where}: {len(text) // 2 - len(good)} hex byte(s) dropped (bad character)",
                notices)
        return bytes(good)


def load_cbdump(path: str) -> CbDump:
    table = _Table(path).read()
    data = CbDump(path=path, format=_first_comment_format(table.comments), trailer=table.trailer,
                  truncated=table.has_truncated)
    for index, raw in enumerate(table.rows):
        crc_text = (raw.get("ps_crc") or "").strip()
        where = f"cbdump.csv row {index + 1} ({raw.get('name', '?')})"
        data.rows.append(CbDumpSample(
            frame=parse_int(raw.get("frame")) or 0,
            time_s=parse_float(raw.get("time_s")),
            ps_crc=parse_int(crc_text),
            ps_crc_text=crc_text or "-",
            name=(raw.get("name") or "").strip() or "-",
            slot=parse_int(raw.get("slot")),
            bytes=parse_int(raw.get("bytes")),
            pointer=parse_pointer(raw.get("ptr")),
            data=_hex_to_bytes(raw.get("hex", ""), data.notices, where),
            hex_text=(raw.get("hex") or "").strip(),
        ))
    _check_trailer(table, data.format, "cbdump.csv")
    data.notices.extend(table.notices)
    return data


# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ViewSample:
    """One line of ``views.txt``: what a shader-resource view and the texture behind it say about themselves."""

    name: str
    ordinal: int | None
    view_dimension: str
    view_mips: int | None
    view_format: str
    array_size: int | None
    cubes: int | None
    width: int | None
    height: int | None
    texture_array: int | None
    texture_mips: int | None
    texture_format: str
    pointer: int | None

    @property
    def size(self) -> Size | None:
        if self.width is None or self.height is None:
            return None
        return Size(self.width, self.height)

    @property
    def is_array(self) -> bool:
        return (self.array_size or 1) > 1 or (self.cubes or 0) > 0 or "ARRAY" in self.view_dimension.upper() or \
               "CUBE" in self.view_dimension.upper()


@dataclass
class Views:
    """``views.txt`` of one capture."""

    path: str
    format: int | None = None
    samples: list[ViewSample] = field(default_factory=list)
    trailer: Trailer | None = None
    notices: list[Notice] = field(default_factory=list)

    def of_name(self, name: str) -> list[ViewSample]:
        wanted = base_texture_name(name)
        return [sample for sample in self.samples if base_texture_name(sample.name) == wanted]

    def one(self, name: str) -> ViewSample | None:
        found = self.of_name(name)
        return found[0] if found else None


VIEWS_FIELDS = ("name", "ordinal", "view_dimension", "view_mips", "view_format", "array_size", "cubes", "width",
                "height", "texture_array", "texture_mips", "texture_format", "pointer")


def load_views(path: str) -> Views:
    """Load ``views.txt``; the file is small and its column set may grow, so extra columns are ignored."""

    table = _Table(path).read()
    data = Views(path=path, format=_first_comment_format(table.comments), trailer=table.trailer)
    data.notices.extend(table.notices)
    for raw in table.rows:
        data.samples.append(ViewSample(
            name=(raw.get("name") or "").strip() or "-",
            ordinal=parse_int(raw.get("ordinal")),
            view_dimension=(raw.get("view_dimension") or "").strip() or "-",
            view_mips=parse_int(raw.get("view_mips")),
            view_format=(raw.get("view_format") or "").strip() or "-",
            array_size=parse_int(raw.get("array_size")),
            cubes=parse_int(raw.get("cubes")),
            width=parse_int(raw.get("width")),
            height=parse_int(raw.get("height")),
            texture_array=parse_int(raw.get("texture_array")),
            texture_mips=parse_int(raw.get("texture_mips")),
            texture_format=(raw.get("texture_format") or "").strip() or "-",
            pointer=parse_pointer(raw.get("pointer")),
        ))
    # views.txt has no '# rows' trailer: it holds one line per identifying texture, not a row stream.
    return data


# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class DumpFact:
    """One written texture of the one-frame dump: ``dump\\<name>.bin`` and the facts that decode it."""

    file: str
    format: str
    width: int
    height: int
    bytes: int | None
    row_pitch: int | None
    ordinal: int | None
    pointer: int | None

    @property
    def size(self) -> Size:
        return Size(self.width, self.height)


@dataclass
class DumpFacts:
    """``dump\\facts.txt`` of one capture, plus the files it skips and why."""

    path: str
    format: int | None = None
    facts: list[DumpFact] = field(default_factory=list)
    skipped: list[tuple[str, str]] = field(default_factory=list)
    notices: list[Notice] = field(default_factory=list)

    @property
    def folder(self) -> str:
        return os.path.dirname(self.path) or "."

    def path_of(self, fact: DumpFact) -> str:
        return os.path.join(self.folder, fact.file)


def _parse_fact_size(text: str) -> tuple[int, int] | None:
    size = parse_size(text)
    return (size.width, size.height) if size else None


def load_dump_facts(path: str) -> DumpFacts:
    """Load ``dump\\facts.txt``.

    Written lines are ``<name>.bin,<format>,<width>x<height>,<bytes>,<row_pitch>,<ordinal>,<pointer>``; an older
    path of the same writer put the size in two fields instead (``<name>.bin,<format>,<width>,<height>,<bytes>,...``)
    and is read too, with the columns after the pair shifted by one.  A texture the dump could not take is a comment
    ``# skipped <name>.bin: <reason>``.  The comment lines also carry the column reference
    (``# file,format,size,...``), which starts with ``#`` and is skipped like any other comment.
    """

    data = DumpFacts(path=path)
    if not os.path.isfile(path):
        _notice(KIND_PARSE, f"{path} was not written", data.notices)
        return data
    for line in _open_text(path):
        text = line.strip()
        if not text:
            continue
        if text.startswith("#"):
            match = re.match(r"^#\s*skipped\s+(\S+?):\s*(.*)$", text)
            if match:
                data.skipped.append((match.group(1), match.group(2)))
            for match in re.finditer(r"format\s+(\d+)", text):
                data.format = int(match.group(1))
            continue
        fields = next(csv.reader([text]))
        if len(fields) < 3:
            _notice(KIND_PARSE, f"facts line has {len(fields)} field(s): {text!r}", data.notices)
            continue
        name = fields[0].strip()
        fmt = fields[1].strip()
        size_text = fields[2].strip()
        size = parse_size(size_text)
        if size is None:
            # An older dump wrote the size as two fields (width, height): ``name,format,width,height,bytes,...``.
            if len(fields) >= 4 and parse_int(fields[2]) is not None and parse_int(fields[3]) is not None:
                size = Size(parse_int(fields[2]) or 0, parse_int(fields[3]) or 0)
                # The two size fields take the place of the single ``<width>x<height>`` field, so the columns after
                # them are the ones after the pair: bytes must not receive the width.
                rest = fields[4:]
            else:
                _notice(KIND_PARSE, f"facts line has no size: {text!r}", data.notices)
                continue
        else:
            rest = fields[3:]
        number = lambda index: parse_int(rest[index]) if len(rest) > index else None  # noqa: E731
        data.facts.append(DumpFact(
            file=name,
            format=fmt,
            width=size.width,
            height=size.height,
            bytes=number(0),
            row_pitch=number(1),
            ordinal=number(2),
            pointer=parse_pointer(rest[3]) if len(rest) > 3 else None,
        ))
    return data


# ---------------------------------------------------------------------------------------------------------------------
# The shader fixtures and the engine pass overrides' counters
# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ShaderFixture:
    """One line of ``shaders.txt``: the blob, its size, where it was kept and the reflected names.

    ``origin`` is what tells a bytecode the diagnostic copied when the game created the shader (``creation``: the
    composite and the other key passes are created while the game loads, before any capture can be armed) from one it
    copied while the capture ran (``capture``).  A format-3 file written before that column existed carries two columns
    (``crc,size``) and every row of it was kept while a capture ran.
    """

    crc: str
    size: int | None
    origin: str
    names: tuple[str, ...]

    @property
    def from_creation(self) -> bool:
        return self.origin == "creation"


@dataclass
class ShaderFixtures:
    """``shaders.txt`` of one capture: the kept bytecode, and what each one declares."""

    path: str
    format: int | None = None
    rows: list[ShaderFixture] = field(default_factory=list)
    by_crc: dict[str, ShaderFixture] = field(default_factory=dict)
    ambiguous: set[str] = field(default_factory=set)
    notices: list[Notice] = field(default_factory=list)

    @property
    def creation(self) -> list[ShaderFixture]:
        return [row for row in self.rows if row.from_creation]

    @property
    def capture(self) -> list[ShaderFixture]:
        return [row for row in self.rows if not row.from_creation]

    def for_crc(self, crc: int | None) -> ShaderFixture | None:
        """The copy kept for a pixel shader's CRC, preferring the row kept from creation."""

        return self.by_crc.get(crc_key(crc)) if crc is not None else None

    def names_for_crc(self, crc: int | None) -> tuple[str, ...]:
        """The names the kept copy of this CRC declares, without the ``=bindPoint`` written next to each.

        ``shaders.txt`` writes ``name=bindPoint`` because a shader declares each resource at one slot; a draw row writes the plain
        name, so the join between the two files drops the bind point.
        """

        # A CRC two rows declare with different name sets names nothing: a join that picked one of them would name a draw after a
        # shader it may not be, and the file said so once when it was read.
        if crc is not None and crc_key(crc) in self.ambiguous:
            return ()
        fixture = self.for_crc(crc)
        return tuple(name.partition("=")[0] for name in fixture.names) if fixture is not None else ()

    def bind_point_for_crc(self, crc: int | None, name: str) -> int | None:
        """The bind point the kept copy of this CRC declares ``name`` at, or None (unknown CRC, ambiguous CRC, name not declared)."""

        if crc is None or crc_key(crc) in self.ambiguous:
            return None
        fixture = self.for_crc(crc)
        if fixture is None:
            return None
        for entry in fixture.names:
            entry_name, _sep, point = entry.partition("=")
            if entry_name == name and point.isdigit():
                return int(point)
        return None


def load_shaders(path: str) -> ShaderFixtures:
    """Load ``shaders.txt``.

    Comment lines start with ``#`` (the first one carries the format, the second the columns); a row is
    ``crc,size,origin,name=bindPoint,...`` and a row of a file written before the origin column exists is
    ``crc,size,name=bindPoint,...``.  A malformed row becomes a notice; it never raises.
    """

    data = ShaderFixtures(path=path)
    if not os.path.isfile(path):
        _notice(KIND_PARSE, f"{path} was not written", data.notices)
        return data
    for line in _open_text(path):
        text = line.strip()
        if not text:
            continue
        if text.startswith("#"):
            match = re.search(r"format\s+(\d+)", text)
            if match:
                data.format = int(match.group(1))
            continue
        fields = [field.strip() for field in next(csv.reader([text]))]
        if len(fields) < 2:
            _notice(KIND_PARSE, f"shader fixture line has {len(fields)} field(s): {text!r}", data.notices)
            continue
        crc = fields[0]
        size = parse_int(fields[1])
        if size is None:
            _notice(KIND_PARSE, f"shader fixture line has no size: {text!r}", data.notices)
            continue
        if len(fields) > 2 and "=" in fields[2]:
            origin, names = "capture", fields[2:]  # a file written before the origin column
        elif len(fields) > 2:
            origin, names = fields[2], fields[3:]
            if origin not in ("creation", "capture"):
                _notice(KIND_PARSE, f"shader fixture origin {origin!r} is not known, read as capture", data.notices)
                origin = "capture"
        else:
            origin, names = "capture", []
        data.rows.append(ShaderFixture(crc=crc, size=size, origin=origin, names=tuple(names)))
    # The by-CRC index a draw row's join reads.  A creation row wins over a capture copy of the same blob: it is the one written
    # before any capture could rename anything, and it is the only row a shader created before the key was on can have.
    for row in data.rows:
        key = crc_key(row.crc)
        if not key:
            continue
        kept = data.by_crc.get(key)
        if kept is not None and kept.names and row.names and set(kept.names) != set(row.names) and key not in data.ambiguous:
            data.ambiguous.add(key)
            _notice(KIND_PARSE, f"shader fixture {row.crc} is listed with two different name sets: the join names none of its draws",
                    data.notices)
        if kept is None or (row.from_creation and not kept.from_creation):
            data.by_crc[key] = row
    return data


@dataclass
class EngineStatus:
    """``engine.txt`` of one capture: the engine pass overrides' counters, one field per line.

    The module writes ``module: off`` and nothing else while its key is off.  While it is on, the file carries the
    status, the run and decline counts, the shader index's own line (bytecodes stored, bytecodes its cap refused), one
    line per decline reason that happened and the facts of the last draw it patched (the ambient buffer's size, the
    composite's target and that draw's viewport).  The ``window`` lines are the same counts for the frames this
    capture holds, measured from a baseline the diagnostic takes when the capture arms; a file written before them
    cannot answer for one capture's window.  The screen-space reflections' block after it carries the same shape under
    ``ssr`` labels (``ssr module``, ``ssr armed``, ``ssr runs``, ``ssr readers``, ``ssr declined``, ``ssr passed on``
    and the ``ssr window`` lines), including the reasons that module passed draws on for instead of refusing them.
    """

    path: str
    format: int | None = None
    fields: dict[str, str] = field(default_factory=dict)
    declines: list[tuple[str, int]] = field(default_factory=list)
    window_declines: list[tuple[str, int]] = field(default_factory=list)
    ssr_declines: list[tuple[str, int]] = field(default_factory=list)
    ssr_passed_on: list[tuple[str, int]] = field(default_factory=list)
    ssr_window_declines: list[tuple[str, int]] = field(default_factory=list)
    ssr_window_passed_on: list[tuple[str, int]] = field(default_factory=list)
    notices: list[Notice] = field(default_factory=list)

    def value(self, name: str) -> str | None:
        return self.fields.get(name)

    @property
    def key_on(self) -> bool:
        return self.fields.get("module") == "on"

    @property
    def module_off(self) -> bool:
        return self.fields.get("module") == "off"

    @property
    def runs(self) -> int:
        return parse_int(self.fields.get("runs")) or 0

    @property
    def declined(self) -> int:
        return parse_int(self.fields.get("declined")) or 0

    @property
    def ran(self) -> bool:
        """The module patched at least one composite draw by the time the capture was written (the session's count)."""

        return self.runs > 0

    @property
    def have_window(self) -> bool:
        """The file carries this capture's own window counts, so it can answer for the frames the capture holds."""

        return "window runs" in self.fields

    @property
    def window_runs(self) -> int:
        """Composite draws the module patched inside this capture's window (0 when the file predates the lines)."""

        return parse_int(self.fields.get("window runs")) or 0

    @property
    def window_declined(self) -> int:
        """Composite draws the module left alone inside this capture's window (0 when the file predates the lines)."""

        return parse_int(self.fields.get("window declined")) or 0

    @property
    def ran_in_window(self) -> bool:
        return self.have_window and self.window_runs > 0

    def dominant_decline(self) -> tuple[str, int] | None:
        """The reason the module declined the most draws for over the session, first one named on a tie."""

        return _dominant(self.declines)

    def window_dominant_decline(self) -> tuple[str, int] | None:
        """The reason the module declined the most draws for inside this capture's window, first one named on a tie."""

        return _dominant(self.window_declines)

    # ---- the screen-space reflections' block (the second half of engine.txt) ----

    @property
    def ssr_present(self) -> bool:
        """The file carries the reflections' block (a capture written before it cannot answer for that module)."""

        return self.fields.get("ssr module") is not None

    @property
    def ssr_off(self) -> bool:
        return self.fields.get("ssr module") == "off"

    @property
    def ssr_on(self) -> bool:
        return self.fields.get("ssr module") == "on"

    @property
    def ssr_armed(self) -> bool:
        return self.fields.get("ssr armed") == "yes"

    def _ssr_int(self, name: str) -> int:
        return parse_int(self.fields.get(f"ssr {name}")) or 0

    @property
    def ssr_runs(self) -> int:
        return self._ssr_int("runs")

    @property
    def ssr_readers(self) -> int:
        return self._ssr_int("readers")

    @property
    def ssr_declined(self) -> int:
        return self._ssr_int("declined")

    @property
    def ssr_have_window(self) -> bool:
        return "ssr window runs" in self.fields

    @property
    def ssr_window_runs(self) -> int:
        return self._ssr_int("window runs")

    @property
    def ssr_window_readers(self) -> int:
        return self._ssr_int("window readers")

    @property
    def ssr_window_declined(self) -> int:
        return self._ssr_int("window declined")

    @property
    def ssr_passed_on_total(self) -> int:
        """Every draw of this session the reflections passed on, whatever the reason."""

        return sum(count for _reason, count in self.ssr_passed_on)

    @property
    def ssr_window_passed_on_total(self) -> int:
        return sum(count for _reason, count in self.ssr_window_passed_on)

    def ssr_dominant_passed_on(self) -> tuple[str, int] | None:
        """The water-side reason the reflections passed the most draws on for, first one named on a tie.

        The catch-all reason (a draw whose shader is not a reflection reader at all: every wetness, occlusion and G-buffer draw of the
        seam) is not a gate a water draw was stopped at, so it never wins here; ``ssr_not_reflection`` carries its count.
        """

        return _dominant(_water_side(self.ssr_passed_on))

    def ssr_window_dominant_passed_on(self) -> tuple[str, int] | None:
        return _dominant(_water_side(self.ssr_window_passed_on)) or _dominant(self.ssr_window_declines)

    @property
    def ssr_not_reflection(self) -> int:
        """How many draws of the session reached the reflections' seam without being a reflection reader."""

        return sum(count for reason, count in self.ssr_passed_on if reason == SSR_NOT_REFLECTION)

    @property
    def ssr_window_not_reflection(self) -> int:
        return sum(count for reason, count in self.ssr_window_passed_on if reason == SSR_NOT_REFLECTION)

    @property
    def last_run(self) -> str:
        """The last patched draw's facts as one readable sentence, or an empty string when nothing ran."""

        bound = self.fields.get("last bound bytes", "-")
        target = self.fields.get("last target", "-")
        window = self.fields.get("last window", "-")
        if "-" in (bound, target, window):
            return ""
        return f"the last patched draw: {bound} bytes of ambient table, a {target} target, a {window} viewport"


# The reflections' catch-all passed-on reason, as the module names it (PassedOnName(PassedOn::NotReflection)).
SSR_NOT_REFLECTION = "the draw is not a reflection reader"


def _water_side(passed_on: list[tuple[str, int]]) -> list[tuple[str, int]]:
    """The passed-on reasons that stopped a reflection reader, the catch-all left out."""

    return [(reason, count) for reason, count in passed_on if reason != SSR_NOT_REFLECTION]


def _dominant(declines: list[tuple[str, int]]) -> tuple[str, int] | None:
    """The reason with the largest count, first one named on a tie; None when no reason was recorded."""

    best: tuple[str, int] | None = None
    for reason, count in declines:
        if best is None or count > best[1]:
            best = (reason, count)
    return best


def load_engine(path: str) -> EngineStatus:
    """Load ``engine.txt``; a line that is not ``label: value`` becomes a notice, not an exception."""

    data = EngineStatus(path=path)
    if not os.path.isfile(path):
        _notice(KIND_PARSE, f"{path} was not written", data.notices)
        return data
    for line in _open_text(path):
        text = line.strip()
        if not text:
            continue
        if text.startswith("#"):
            match = re.search(r"format\s+(\d+)", text)
            if match:
                data.format = int(match.group(1))
            continue
        label, separator, value = text.partition(":")
        if not separator:
            _notice(KIND_PARSE, f"engine.txt line is not `label: value`: {text!r}", data.notices)
            continue
        label = label.strip()
        value = value.strip()
        if label in ("decline", "window decline", "ssr decline", "ssr window decline", "ssr passed on", "ssr window passed on"):
            reason, _, count = value.rpartition(" = ")
            number = parse_int(count)
            if not reason or number is None:
                _notice(KIND_PARSE, f"engine.txt {label} line has no count: {text!r}", data.notices)
                continue
            target = {
                "decline": data.declines,
                "window decline": data.window_declines,
                "ssr decline": data.ssr_declines,
                "ssr window decline": data.ssr_window_declines,
                "ssr passed on": data.ssr_passed_on,
                "ssr window passed on": data.ssr_window_passed_on,
            }[label]
            target.append((reason, number))
            continue
        data.fields[label] = value
    return data


# ---------------------------------------------------------------------------------------------------------------------
# A whole capture
# ---------------------------------------------------------------------------------------------------------------------

DRAWS_FILE = "draws.csv"
CB_FILE = "cb.csv"
CBDUMP_FILE = "cbdump.csv"
VIEWS_FILE = "views.txt"
DUMP_FOLDER = "dump"
DUMP_FACTS_FILE = "facts.txt"
SHADERS_FILE = "shaders.txt"
ENGINE_FILE = "engine.txt"


@dataclass
class Capture:
    """One capture folder and everything the diagnostic wrote into it."""

    folder: str
    draws: Draws | None = None
    cb: CbReads | None = None
    cbdump: CbDump | None = None
    views: Views | None = None
    dump: DumpFacts | None = None
    shaders: ShaderFixtures | None = None
    engine: EngineStatus | None = None
    notices: list[Notice] = field(default_factory=list)

    @property
    def name(self) -> str:
        return os.path.basename(os.path.normpath(self.folder))

    @property
    def format(self) -> int | None:
        return self.draws.format if self.draws else None

    @property
    def rows(self) -> list[Draw]:
        return self.draws.rows if self.draws else []

    def all_notices(self) -> list[Notice]:
        notices = list(self.notices)
        for part in (self.draws, self.cb, self.cbdump, self.views, self.dump, self.shaders, self.engine):
            if part is not None:
                notices.extend(part.notices)
        return notices

    def warnings(self) -> list[str]:
        """The notices as text, for a tool that prints them."""

        return [f"{notice}" for notice in self.all_notices()]

    def has(self, name: str) -> bool:
        return os.path.exists(os.path.join(self.folder, name))

    def reflected_names(self, row: Draw) -> tuple[str, ...]:
        """The names the draw's own shader declares, taken from ``shaders.txt`` by CRC when the row carries none.

        A shader is only named in ``draws.csv`` if the diagnostic had its reflection, which it records when the game creates
        the shader.  The passes created while the game loads - the glare chain among them - are created before any capture can
        be armed, so every row of them carries ``-`` in every slot, even though ``shaders.txt`` holds their names from the
        creation row and the row's own ``ps_crc`` is the same value.  The join recovers those names; a row that already names a
        resource is its own answer and never consults the fixture.
        """

        if row.cb_names or row.srv_names:
            return ()
        if self.shaders is None:
            return ()
        return self.shaders.names_for_crc(row.ps_crc)


def load_capture(folder: str) -> Capture:
    """Load a capture folder, tolerating a missing file of any kind (a format-2 capture has no ``cbdump.csv``)."""

    capture = Capture(folder=os.path.abspath(folder))
    if not os.path.isdir(capture.folder):
        _notice(KIND_PARSE, f"{folder} is not a folder", capture.notices)
        return capture
    draws_path = os.path.join(capture.folder, DRAWS_FILE)
    if os.path.isfile(draws_path):
        capture.draws = load_draws(draws_path)
    else:
        _notice(KIND_PARSE, f"{DRAWS_FILE} is missing: {folder} is not a capture folder", capture.notices)
    for attribute, filename, loader in (
        ("cb", CB_FILE, load_cb),
        ("cbdump", CBDUMP_FILE, load_cbdump),
        ("views", VIEWS_FILE, load_views),
    ):
        path = os.path.join(capture.folder, filename)
        if os.path.isfile(path):
            setattr(capture, attribute, loader(path))
    facts_path = os.path.join(capture.folder, DUMP_FOLDER, DUMP_FACTS_FILE)
    if os.path.isfile(facts_path):
        capture.dump = load_dump_facts(facts_path)
    shaders_path = os.path.join(capture.folder, SHADERS_FILE)
    if os.path.isfile(shaders_path):
        capture.shaders = load_shaders(shaders_path)
    engine_path = os.path.join(capture.folder, ENGINE_FILE)
    if os.path.isfile(engine_path):
        capture.engine = load_engine(engine_path)
    return capture


# ---------------------------------------------------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------------------------------------------------


def base_texture_name(name: str | None) -> str:
    """A reflected name without the effect compiler's type suffix (``g_SamplerDepth.T`` -> ``g_SamplerDepth``).

    The engine's effect compiler appends the resource type to a texture (``.T``) and to a sampler (``.S``); constant
    buffers keep their plain name.  Every name comparison in this folder goes through this function, because a plain
    string compare finds nothing for a texture.
    """

    if not name:
        return ""
    text = name.strip()
    if len(text) > 2 and text[-2] == "." and text[-1] in ("T", "S"):
        return text[:-2]
    return text


def base_names(entries: Iterable[TextureEntry]) -> list[str]:
    return [base_texture_name(entry.name) for entry in entries]


# ---------------------------------------------------------------------------------------------------------------------
# Finding captures
# ---------------------------------------------------------------------------------------------------------------------

_PRUNE = {"shaders", DUMP_FOLDER, "png", "__pycache__", ".git"}


def find_captures(root: str, max_depth: int = 8) -> list[str]:
    """Every folder at or below ``root`` that holds a ``draws.csv``, oldest name first.

    A session writes several capture folders next to each other (``<date-time>``, ``<date-time>-2``, ...), and a bench
    run keeps them under ``game-outputs\\...``; scanning for the file is what makes both work without a path rule.
    """

    root = os.path.abspath(root)
    if os.path.isfile(os.path.join(root, DRAWS_FILE)):
        return [root]
    found: list[str] = []
    base_depth = root.rstrip(os.sep).count(os.sep)
    for current, dirs, files in os.walk(root):
        depth = current.rstrip(os.sep).count(os.sep) - base_depth
        dirs[:] = sorted(name for name in dirs if name not in _PRUNE)
        if depth >= max_depth:
            dirs[:] = []
            continue
        if DRAWS_FILE in files:
            found.append(os.path.abspath(current))
    return sorted(found)


if __name__ == "__main__":  # a folder argument prints what the reader understood, which is a cheap sanity check
    import sys

    for argument in sys.argv[1:]:
        loaded = load_capture(argument)
        print(f"{loaded.folder}: {loaded.draws.summary() if loaded.draws else 'no draws.csv'}")
        if loaded.shaders is not None:
            print(f"  shaders.txt: {len(loaded.shaders.rows)} fixture(s), {len(loaded.shaders.creation)} kept from creation")
        if loaded.engine is not None:
            module = "on" if loaded.engine.key_on else "off"
            window = (f", {loaded.engine.window_runs} run(s) and {loaded.engine.window_declined} decline(s) in this capture's window"
                      if loaded.engine.have_window else ", no window counters")
            print(f"  engine.txt: module {module}, {loaded.engine.runs} run(s), {loaded.engine.declined} decline(s){window}")
        for notice in loaded.all_notices():
            print(f"  {notice}")
