"""Answer the session's fixed question list from a set of capture folders, mechanically.

The questions a live session has to settle are the same every time: what the wetness parameter carries and whether one
buffer serves the whole frame, what the fullscreen wetness pass writes and who reads it, whether the reflection chain and
its textures are there, whether the frame split still fires once per frame, whether the glare chain is present and stable,
whether the screen-space ray march draws, whether the ambient probe table differs between two captures, whether the
shadow atlas still has five slices, and whether the engine pass overrides patched a composite draw or which reason it
declined for (from the capture's own ``engine.txt``, so the answer needs no log).

This tool reads every capture folder it is given and writes ``session-answers.md`` next to them (and the same text to
stdout): a header that defines the key of every question, one section per capture, one line per question with ``[O]``
(answered from the files, with the evidence row) or ``[U]`` (not answerable from these files, with what would answer it),
and a closing section that compares the captures with each other.  The short keys are only the report's section names;
the header says what each one asks.

Nothing here decides what the answers mean; it prints the rows a person would otherwise look up by hand.

    python -I session_answers.py <session-folder> [--out FILE] [--quiet]

Exit code 0 when at least one capture was read, 2 when the folder holds none.
"""

from __future__ import annotations

import os
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Iterable, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cblayouts  # noqa: E402
import cbdump_report  # noqa: E402
import drawdiag  # noqa: E402

# A captured window whose own draw count reaches this is two game frames' worth: a live frame of the game measured about
# 7,500 draws.  The number is a hint for the report, not a rule the diagnostic applies, and it never decides an answer.
TWO_FRAME_DRAWS = 12000

# The stage value the diagnostic writes for the tone map; the splitter's rule is that the window closes after it.
TONE_STAGE = 1

# The five shadow-atlas slices' viewport offsets, in texels, and the height an atlas-shaped depth view must reach.
ATLAS_TOP = (0, 2048, 4096, 6144, 8192)
ATLAS_MIN_HEIGHT = 10240

REFLECTION_MAP = "g_SamplerReflectionMap"
REFLECTION_ARRAY = "g_SamplerReflectionArray"
HIERARCHICAL_Z = "g_SamplerHierarchicalZ"
LIGHT_DIFFUSE = "g_SamplerLightDiffuse"
LIGHT_SPECULAR = "g_SamplerLightSpecular"
MASK = "g_SamplerShadowMask"
GLARE_CONSTANTS = ("cGlareParam", "cBrightPassParam", "cMergeWeight", "cGaussianBlurParam", "cSoftFocusParam")
# The engine's flare family (the halo ring, the sun sprite and the ghost step of each radial-blur pass): the three buffers
# the diagnostic samples since this build, so a capture carries the values that were never readable before.
FLARE_CONSTANTS = ("cHaloParam", "cSunParam", "cRadialBlurParam")

ANSWER_YES = "O"
ANSWER_UNKNOWN = "U"

# One line per key, printed in the report's header: the short ids are only the report's section keys, so a copied report
# has to say what each one asks.
QUESTION_KEYS = (
    ("P1", "the wetness parameter: its 8 floats, their movement, and whether one buffer serves the frame"),
    ("P1x", "the wetness value compared between captures (dry against rain)"),
    ("P2", "the fullscreen wetness pass: what it writes, and who reads that target later in the frame"),
    ("P4", "the reflection chain: which draws bind the hierarchical-Z, the reflection map and the array"),
    ("P5", "the reflection array: a real array or a 1x1 dummy, and its dimensions"),
    ("B1", "the frame split: one tone-map row per window, and a window whose rows are all in the file"),
    ("B2", "the glare chain: are its draws present, and is cGlareParam stable across the samples"),
    ("B3", "the screen-space reflection ray march: is a draw binding the hierarchical-Z present"),
    ("B4", "the ambient probe table compared between captures (night against day)"),
    ("B7", "the shadow atlas: depth-only draws over its slices, and the depth view's size"),
    ("GB", "the G-buffer: the five-target draw's formats and sizes"),
    ("LB", "the light buffers: the formats and sizes the light draws read and write"),
    ("EP", "the engine pass overrides (and the screen-space reflections beside them): did the module run in this capture's own "
           "window, or which gate stopped its draws"),
    ("FL", "the engine's flare (the halo ring, the sun sprite and the ghost discs): its three parameter buffers as the "
           "capture sampled them, and whether the frame's sun could show them"),
)


@dataclass
class Answer:
    """One question of the list: what the files say, and the rows that say it."""

    key: str
    title: str
    status: str = ANSWER_UNKNOWN
    evidence: list[str] = field(default_factory=list)
    details: list[str] = field(default_factory=list)

    @property
    def mark(self) -> str:
        return f"[{self.status}]"


@dataclass
class CaptureAnswers:
    """Every answer of one capture."""

    capture: drawdiag.Capture
    answers: list[Answer] = field(default_factory=list)

    def by_key(self, key: str) -> Answer | None:
        for answer in self.answers:
            if answer.key == key:
                return answer
        return None

    @property
    def warnings(self) -> list[str]:
        return self.capture.warnings()


# ---------------------------------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------------------------------


def size_of(entry: drawdiag.TextureEntry | None) -> str:
    if entry is None:
        return "-"
    if entry.size is not None:
        return str(entry.size)
    return entry.format or "-"


def format_size(entry: drawdiag.TextureEntry | drawdiag.DepthEntry | None) -> str:
    if entry is None:
        return "-"
    fmt = entry.format or "-"
    size = str(entry.size) if entry.size else "-"
    return f"{fmt} {size}"


def pointer_text(value: int | None) -> str:
    return drawdiag.hex_pointer(value)


def fence(lines: Sequence[str]) -> str:
    return "```\n" + "\n".join(lines) + "\n```"


def _signature(rtvs: Sequence[drawdiag.TextureEntry]) -> str:
    return " | ".join(f"{entry.slot}:{entry.format}:{entry.size}" for entry in rtvs)


def _counts(values: Iterable[str], limit: int = 6) -> str:
    counter = Counter(values)
    return ", ".join(f"{value} ({count})" for value, count in counter.most_common(limit))


def _distinct(values: Iterable[object], limit: int = 8) -> str:
    seen: list[object] = []
    for value in values:
        if value not in seen:
            seen.append(value)
    shown = ", ".join(str(value) for value in seen[:limit])
    return shown + (f", ... ({len(seen)} in all)" if len(seen) > limit else "")


def unnamed_srv_rows(rows: Iterable[drawdiag.Draw]) -> list[drawdiag.Draw]:
    """Rows whose shader bound resources but whose reflected names the capture does not have (every name is ``-``).

    A name test can only see a draw whose pixel shader's reflection reached the diagnostic.  The screen-space
    reflection chain of the build these captures came from is exactly such a case, so a "no row binds the name" answer
    has to say how many rows it could not see at all.
    """

    return [row for row in rows if row.srvs and all(entry.name is None for entry in row.srvs)]


def _rows_binding(rows: Iterable[drawdiag.Draw], name: str) -> list[drawdiag.Draw]:
    wanted = drawdiag.base_texture_name(name)
    return [row for row in rows if any(drawdiag.base_texture_name(entry.name) == wanted for entry in row.srvs)]


def draw_names(capture: drawdiag.Capture, row: drawdiag.Draw) -> tuple[str, ...]:
    """Every reflected name the row's shader declares: the row's own, or the ones ``shaders.txt`` kept for its CRC.

    The join exists for the passes the game creates while it loads: their draws carry ``-`` in every slot because the
    diagnostic only records a shader's reflection when it is created, and their creation rows in ``shaders.txt`` carry the
    same CRC and all the names.  A name test that reads only ``draws.csv`` therefore answers "no draw binds it" for a chain
    the capture holds; this answer is the one that can see it.
    """

    own = tuple(row.cb_names) + tuple(row.srv_names)
    return own if own else capture.reflected_names(row)


def _frames_text(frames: Iterable[int]) -> str:
    values = sorted(set(frames))
    return ", ".join(str(value) for value in values) if values else "-"


def _changes(first: float, other: float) -> bool:
    """Whether two numbers of one member differ by more than the noise of their own last bits."""

    scale = max(1.0, abs(float(first)), abs(float(other)))
    return abs(float(first) - float(other)) > 1e-9 * scale


# ---------------------------------------------------------------------------------------------------------------------
# P1: the wetness parameter
# ---------------------------------------------------------------------------------------------------------------------


def answer_wetness(capture: drawdiag.Capture) -> Answer:
    answer = Answer("P1", "the wetness parameter: its values, and whether one buffer carries the whole frame")
    if capture.cbdump is None:
        answer.evidence.append("cbdump.csv is not in the capture, so the 8 floats were never sampled")
        answer.evidence.append("what answers it: a capture from a build with the per-sample constant-buffer dump")
        return answer
    samples = capture.cbdump.of_name(cbdump_report.WETNESS)
    if not samples:
        answer.evidence.append("cbdump.csv has no g_WetnessParameter row: the buffer was not bound while the dump sampled")
        return answer
    answer.status = ANSWER_YES
    table = ["sample  frame  time_s  slot  ptr       f0        f1        f2        f3        f4        f5        f6        f7"]
    stats_rows: list[tuple[str, float, float, bool]] = []
    values: dict[str, list[float]] = {}
    for index, sample in enumerate(samples, start=1):
        decoded = cblayouts.decode(cbdump_report.WETNESS, sample.data)
        members = decoded.get("members", {})
        floats = [members.get(f"f{slot}") for slot in range(8)]
        for slot, value in enumerate(floats):
            if isinstance(value, float):
                values.setdefault(f"f{slot}", []).append(value)
        time_text = "-" if sample.time_s is None else f"{sample.time_s:.3f}"
        table.append(f"{index:6d}  {sample.frame:5d}  {time_text:8s}  "
                     f"{cbdump_report.fmt(sample.slot):4s}  {pointer_text(sample.pointer):8s}  "
                     + "  ".join(f"{cbdump_report.fmt(value):8s}" for value in floats))
    for slot in range(8):
        series = values.get(f"f{slot}", [])
        if series:
            stats_rows.append((f"f{slot}", min(series), max(series), any(_changes(series[0], value)
                                                                          for value in series[1:])))
    answer.evidence.append(f"cbdump.csv holds {len(samples)} sample(s) of the 32 B buffer (8 floats, no member names in "
                           f"the reflection)")
    answer.details.append(fence(table))
    moving = [row[0] for row in stats_rows if row[3]]
    answer.evidence.append("floats that move between the samples: " + (", ".join(moving) if moving else "none") +
                           " (a float that ramps with rain is the weather reading the puddle work would need)")
    if len(samples) > 1:
        answer.details.append(fence([f"{name:4s} min {cbdump_report.fmt(low):>10s}  max {cbdump_report.fmt(high):>10s}"
                                     for name, low, high, _ in stats_rows]))
    if samples[0].pointer is not None:
        # Grouped by frame: a buffer that rotates between frames is still one buffer per frame, which is the question.
        by_frame: dict[int, list[int]] = {}
        for sample in samples:
            if sample.pointer is not None:
                by_frame.setdefault(sample.frame, []).append(sample.pointer)
        per_frame = []
        for frame in sorted(by_frame):
            values = by_frame[frame]
            if len(set(values)) == 1:
                per_frame.append(f"frame {frame}: one pointer ({pointer_text(values[0])}) in {len(values)} sample(s)")
            else:
                per_frame.append(f"frame {frame}: {len(set(values))} distinct pointer(s): {_distinct(values)}")
        answer.evidence.append("buffer pointer per frame: " + "; ".join(per_frame))
        every = [pointer for values in by_frame.values() for pointer in values]
        if len(set(every)) == 1:
            answer.evidence.append(f"buffer pointer over the whole capture: constant ({pointer_text(every[0])}) in "
                                  f"{len(every)} sample(s)")
        else:
            answer.evidence.append(f"buffer pointer over the whole capture: {len(set(every))} distinct value(s): "
                                   f"{_distinct(every)} (a buffer that rotates between frames is still one buffer per "
                                   f"frame; the per-frame line above is the one that answers the question)")
    else:
        answer.evidence.append("buffer pointer across the samples: the rows carry none (a format-2 capture)")
    joins = cbdump_report.wetness_draws(capture.draws, capture)
    if joins.rows:
        if joins.pointers:
            if len(joins.pointers) == 1:
                answer.evidence.append(f"draws that bind it: {joins.rows} row(s), slot(s) {_distinct(joins.slots)}, "
                                       f"one pointer for all of them ({pointer_text(joins.pointers[0])})")
            else:
                answer.evidence.append(f"draws that bind it: {joins.rows} row(s), slot(s) {_distinct(joins.slots)}, "
                                       f"{len(joins.pointers)} distinct pointer(s): {_distinct(joins.pointers)}")
        else:
            answer.evidence.append(f"draws that bind it: {joins.rows} row(s), slot(s) {_distinct(joins.slots)} - the "
                                   f"capture has no pointer column (format 2), so one-buffer-per-frame cannot be "
                                   f"answered from it")
    else:
        answer.evidence.append("draws that bind it: none in draws.csv")
    if answer.status == ANSWER_YES:
        # Several frames may each own a buffer (a rotation); one frame that binds two is what leaves the question open.
        frame_pointers: dict[int, list[int]] = {}
        for row in capture.rows:
            entry = row.cb(cbdump_report.WETNESS)
            if entry is not None and entry.pointer is not None:
                frame_pointers.setdefault(row.frame, []).append(entry.pointer)
        shared = sorted(frame for frame, values in frame_pointers.items() if len(set(values)) > 1)
        if shared:
            answer.status = ANSWER_UNKNOWN
            answer.evidence.append(f"open: frame(s) {_frames_text(shared)} bind more than one wetness pointer, so the "
                                   f"per-frame question needs a closer look at which draws share one")
    answer.evidence.append("open: whether a float follows the weather needs two captures at different weather; inside "
                           "one capture, movement shows the value is live while the capture runs, not what it follows")
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# P2: the fullscreen wetness pass and its readers
# ---------------------------------------------------------------------------------------------------------------------


def find_wetness_passes(capture: drawdiag.Capture) -> list[drawdiag.Draw]:
    """The draws that bind the wetness buffer and read the G-buffer *and* the view position, writing one target.

    Both inputs are required: a draw that reads one of them is a different pass that happens to share the buffer.
    """

    found = []
    for row in capture.rows:
        if row.cb(cbdump_report.WETNESS) is None:
            continue
        if not row.binds_all(("g_SamplerGBuffer3", "g_SamplerViewPosition")):
            continue
        if row.target_count == 1:
            found.append(row)
    return found


def answer_wetness_pass(capture: drawdiag.Capture) -> Answer:
    answer = Answer("P2", "the fullscreen wetness pass: what it writes, and who reads that target later in the frame")
    passes = find_wetness_passes(capture)
    if not passes:
        answer.evidence.append("no draw binds g_WetnessParameter together with g_SamplerGBuffer3 and "
                               "g_SamplerViewPosition and exactly one render target")
        if capture.draws is not None:
            answer.evidence.append("draws.csv was read (" + capture.draws.summary() + "): the pass is not in it")
        return answer
    frames = sorted({row.frame for row in passes})
    answer.evidence.append(f"{len(passes)} pass row(s) in frame(s) {_frames_text(frames)}; "
                           f"pixel shader CRC(s) {_distinct(row.crc_text for row in passes)}")
    lines = []
    for row in passes:
        lines.append(f"frame {row.frame} ordinal {row.ordinal}: {row.api} {row.count} vertex/indices, CRC {row.crc_text}, "
                     f"target {format_size(row.rtvs[0])}, depth {format_size(row.dsv)}, pointer "
                     f"{pointer_text(row.rtvs[0].pointer)}")
    answer.details.append(fence(lines))
    # The join: a later row of the same frame whose bound resources include the pass's target pointer.
    joined = 0
    readers = []
    weak = 0
    for row in passes:
        target = row.rtvs[0]
        frame_rows = capture.draws.of_frame(row.frame) if capture.draws else []
        later = [candidate for candidate in frame_rows if candidate.ordinal > row.ordinal]
        if target.pointer is not None:
            hits = [candidate for candidate in later if target.pointer in candidate.srv_pointers]
            joined += len(hits)
            for hit in hits[:8]:
                readers.append(f"frame {hit.frame} ordinal {hit.ordinal}: {hit.api} CRC {hit.crc_text} reads it at "
                               f"slot {_distinct(entry.slot for entry in hit.srvs if entry.pointer == target.pointer)}")
            writes = [candidate for candidate in later if target.pointer in candidate.rtv_pointers]
            for hit in writes[:4]:
                readers.append(f"frame {hit.frame} ordinal {hit.ordinal}: {hit.api} CRC {hit.crc_text} writes it again")
        elif target.format and target.size:
            # Format 2: no identity, so the same format and size is the only hint there is, and it is not identity.
            weak = sum(1 for candidate in later
                       if any(entry.format == target.format and entry.size == target.size for entry in candidate.srvs))
    if joined:
        answer.status = ANSWER_YES
        answer.evidence.append(f"later rows of the same frame that bind the target's pointer as a resource: {joined}")
        answer.details.append(fence(readers))
    elif passes[0].rtvs[0].pointer is not None:
        # "Not read again" is only an answer when the file holds the window: a dropped or cut row could be the reader.
        window = capture.draws.window(passes[0].frame) if capture.draws else None
        dropped = window.omitted if window is not None else 0
        truncated = bool(capture.draws is not None and capture.draws.truncated)
        if dropped or truncated:
            reasons = []
            if dropped:
                reasons.append(f"the row cap dropped {dropped} row(s) of frame {passes[0].frame}")
            if truncated:
                reasons.append("draws.csv is marked TRUNCATED")
            answer.evidence.append("no later row of the same frame binds the target's pointer, but the file is not the "
                                   "whole window (" + ", ".join(reasons) + "): the reader may be one of the missing "
                                   "rows, so this is not an answer")
            answer.evidence.append("what answers it: a capture whose window keeps every row, or a later window of the "
                                   "same frame in which the target is read")
        else:
            answer.status = ANSWER_YES
            answer.evidence.append("no later row of the same frame binds the target's pointer: the target of this pass "
                                   "is not read again by a draw in the captured window, which holds every row")
    else:
        answer.evidence.append("the target's pointer is not in the file (format 2), so the reader join cannot run")
        if weak:
            answer.evidence.append(f"weak hint only: {weak} later row(s) bind some {format_size(passes[0].rtvs[0])} "
                                   f"resource; same format and size is not identity")
        answer.evidence.append("what answers it: a format-3 capture, whose entries carry the resource pointer")
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# P4 / P5 / B3: the reflection chain
# ---------------------------------------------------------------------------------------------------------------------


def answer_reflection_chain(capture: drawdiag.Capture) -> Answer:
    answer = Answer("P4", "the reflection chain: which draws bind the hierarchical-Z, the reflection map and the "
                          "reflection array, at which sizes")
    rows = capture.rows
    found: dict[str, list[drawdiag.Draw]] = {}
    details: dict[tuple[int, int], str] = {}
    for name in (HIERARCHICAL_Z, REFLECTION_MAP, REFLECTION_ARRAY):
        found[name] = _rows_binding(rows, name)
    if not any(found.values()):
        answer.evidence.append("no row binds any of g_SamplerHierarchicalZ, g_SamplerReflectionMap, "
                               "g_SamplerReflectionArray")
        if capture.draws is not None:
            answer.evidence.append("draws.csv was read (" + capture.draws.summary() + "); a format-1 window of a "
                                   "post-process tail cannot hold the chain")
        return answer
    for name, bound in found.items():
        if not bound:
            answer.evidence.append(f"{name}: no row binds it")
            continue
        entries = [entry for row in bound for entry in row.srvs if drawdiag.base_texture_name(entry.name) == name]
        sizes = Counter(format_size(entry) for entry in entries)
        frames = {row.frame for row in bound}
        answer.evidence.append(f"{name}: {len(bound)} row(s) in frame(s) {_frames_text(frames)}; "
                               + _counts(f"{fmt}" for fmt in sizes.elements()))
        for row in bound[:6]:
            details.setdefault((row.frame, row.ordinal),
                               f"frame {row.frame} ordinal {row.ordinal}: {row.api} CRC {row.crc_text} "
                               f"target {_signature(row.rtvs)} (binds {name})")
    # The producer of the reflection map (and of the hierarchical-Z) is found by joining pointer to pointer.
    if capture.format == 3:
        for bound_name, what in ((REFLECTION_MAP, "reflection map"), (HIERARCHICAL_Z, "hierarchical-Z")):
            wanted = {entry.pointer for row in found[bound_name] for entry in row.srvs
                      if drawdiag.base_texture_name(entry.name) == bound_name and entry.pointer is not None}
            if not wanted:
                continue
            producers = [row for row in rows if wanted & row.rtv_pointers]
            if producers:
                answer.evidence.append(f"producer(s) of the {what} by pointer: {len(producers)} row(s), "
                                       f"CRC(s) {_distinct(row.crc_text for row in producers)}, "
                                       f"target(s) {_distinct(f'{format_size(entry)} at {pointer_text(entry.pointer)}' for row in producers for entry in row.rtvs if entry.pointer in wanted)}")
                for row in producers[:8]:
                    details.setdefault((row.frame, row.ordinal),
                                       f"frame {row.frame} ordinal {row.ordinal}: {row.api} CRC {row.crc_text} "
                                       f"target {_signature(row.rtvs)} (writes the {what})")
            else:
                answer.evidence.append(f"no row writes the {what} by pointer: its producer either runs outside the "
                                       f"captured windows or the pointer is created between them (a copy has no "
                                       f"render target)")
    else:
        answer.evidence.append("the producer join needs resource pointers: this capture is not format 3")
    if details:
        answer.details.append(fence([details[key] for key in sorted(details)]))
    if not found[HIERARCHICAL_Z]:
        unnamed = unnamed_srv_rows(capture.rows)
        answer.evidence.append(f"{len(unnamed)} row(s) bind resources whose reflected names the capture does not "
                               f"carry (the SRV column is all `-`); the chain's own draws are among them in some "
                               f"captures, so a name test cannot see them: CRC(s) "
                               f"{_distinct(row.crc_text for row in unnamed)}")
    answer.status = ANSWER_YES if (found[HIERARCHICAL_Z] or found[REFLECTION_MAP]) else ANSWER_UNKNOWN
    return answer


def answer_reflection_array(capture: drawdiag.Capture) -> Answer:
    answer = Answer("P5", "the reflection array: a real array or a 1x1 dummy, and its dimensions")
    sample = capture.views.one(REFLECTION_ARRAY) if capture.views else None
    if sample is not None:
        size = sample.size
        answer.status = ANSWER_YES
        answer.evidence.append(f"views.txt: {sample.name} at ordinal {sample.ordinal}: view {sample.view_dimension}, "
                               f"mips {sample.view_mips}, format {sample.view_format}, array size {sample.array_size}, "
                               f"cubes {sample.cubes}")
        answer.evidence.append(f"the texture behind it: {size} {sample.texture_format}, array {sample.texture_array}, "
                               f"mips {sample.texture_mips}, pointer {pointer_text(sample.pointer)}")
        if size is not None and (size.width <= 8 or size.height <= 8):
            answer.evidence.append("size 8 px or less: a dummy this frame, not a probe array")
        return answer
    rows = _rows_binding(capture.rows, REFLECTION_ARRAY)
    if not rows:
        answer.evidence.append("no row binds g_SamplerReflectionArray")
        if capture.views is None:
            answer.evidence.append("views.txt is not in the capture")
        else:
            answer.evidence.append("views.txt holds: " + (", ".join(view.name for view in capture.views.samples) or "no rows"))
        return answer
    entries = [entry for row in rows for entry in row.srvs
               if drawdiag.base_texture_name(entry.name) == REFLECTION_ARRAY]
    answer.evidence.append(f"bound in {len(rows)} row(s), at {_counts(format_size(entry) for entry in entries)}")
    answer.evidence.append("the array size and mip count are not in a draw row (it records the bound view's format and "
                           "size, not its dimension): views.txt settles it")
    if capture.views is None:
        answer.evidence.append("open: this capture has no views.txt")
    return answer


def answer_raytracing(capture: drawdiag.Capture) -> Answer:
    answer = Answer("B3", "the screen-space reflection ray march: is a draw binding the hierarchical-Z present?")
    rows = _rows_binding(capture.rows, HIERARCHICAL_Z)
    if rows:
        answer.status = ANSWER_YES
        answer.evidence.append(f"{len(rows)} row(s) bind g_SamplerHierarchicalZ, in frame(s) "
                               f"{_frames_text(row.frame for row in rows)}, CRC(s) "
                               f"{_distinct(row.crc_text for row in rows)}")
        answer.evidence.append("targets of those rows: " + _distinct(_signature(row.rtvs) for row in rows))
        answer.details.append(fence([f"frame {row.frame} ordinal {row.ordinal}: {row.api} {row.count}, CRC "
                                     f"{row.crc_text}, target {_signature(row.rtvs)}, srv "
                                     f"{_distinct(format_size(entry) for entry in row.srvs if entry.name)}"
                                     for row in rows[:10]]))
    else:
        answer.evidence.append("no row binds g_SamplerHierarchicalZ")
        if capture.draws is None:
            answer.evidence.append("the capture has no draws.csv")
        else:
            answer.evidence.append("draws.csv was read (" + capture.draws.summary() + "): absence is measured in this "
                                   "window, not in the build - a window that holds no water or no reflective surface "
                                   "would also show none")
        unnamed = unnamed_srv_rows(capture.rows)
        half = [row for row in unnamed if any(entry.size is not None and entry.size.width <= 1280
                                             for entry in row.rtvs)]
        answer.evidence.append(f"{len(unnamed)} row(s) bind resources with no reflected name (SRV column all `-`); "
                               f"{len(half)} of them write a target 1280 px wide or less: a half-resolution chain "
                               f"drawn by a shader whose reflection the capture does not carry cannot be found by "
                               f"this name test")
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# B1: one tone map per window, and the window's size
# ---------------------------------------------------------------------------------------------------------------------


def answer_frame_split(capture: drawdiag.Capture) -> Answer:
    answer = Answer("B1", "the frame split: one tone-map row per window, and a window whose rows are all in the file")
    if capture.draws is None or not capture.draws.windows:
        answer.evidence.append("no '# frame' line in draws.csv")
        return answer
    lines = []
    stage_rows: dict[int, list[int]] = {}
    complete = 0
    for window in capture.draws.windows:
        rows = capture.draws.of_frame(window.frame)
        # The rule this reads: a window is one game frame when it holds exactly one tone-map row, so the count of those
        # rows is what the answer rests on - a marker count and a draw count are the writer's own bookkeeping.
        tone = [row.ordinal for row in rows if row.stage == TONE_STAGE]
        stage_rows[window.frame] = tone
        marker = len(window.tone_rows)
        highest = max((row.ordinal for row in rows), default=0)
        past_cap = window.tone != 0 and window.tone > highest
        lines.append(f"frame {window.frame}: draws {window.draws}, tone ordinal {window.tone}, rows in file "
                     f"{window.row_count}, tone-map row(s) in file {len(tone)}, tone marker row(s) {marker}, ui1 "
                     f"{window.ui1}, upscale_after {window.upscale_after}, omitted {window.omitted}"
                     + ("; the tone row is past the row cap" if past_cap else ""))
        if window.omitted == 0:
            complete += 1
    answer.details.append(fence(lines))
    window_count = len(capture.draws.windows)
    answer.evidence.append(f"tone-map row(s) the file holds, per window: "
                           + ", ".join(f"frame {frame}: {len(stage_rows[frame])}" for frame in sorted(stage_rows))
                           + f" (a window is one game frame when it holds exactly one stage-{TONE_STAGE} row)")
    omitted = sum(window.omitted for window in capture.draws.windows)
    answer.evidence.append(f"the file holds every row of {complete} of {window_count} window(s)"
                           + (f"; the per-window row cap dropped {omitted} row(s) in all, so a row that is not in the "
                              f"file is not a row that did not run" if omitted else ""))
    # Both halves are needed.  A window that keeps one tone-map row can still be two game frames when the cap dropped
    # the other, and a window whose rows are all present can still hold two tone maps (the splitter closing at every
    # second one).  Neither count alone says what the splitter did.
    without_tone = [frame for frame in sorted(stage_rows) if len(stage_rows[frame]) != 1]
    ok = not without_tone and omitted == 0
    if ok:
        answer.evidence.append("every window holds exactly one tone-map row and kept every row: consistent with one game "
                               "frame per window")
    else:
        if without_tone:
            answer.evidence.append("window(s) that do not hold exactly one tone-map row: "
                                   + ", ".join(f"frame {frame} ({len(stage_rows[frame])})" for frame in without_tone)
                                   + ": either the splitter ran twice in the window or the row is past the row cap")
        if omitted:
            answer.evidence.append(f"the row cap dropped {omitted} row(s) in all, so a window with one tone-map row in "
                                   f"the file may have had a second one dropped: the file cannot tell one game frame "
                                   f"per window from two")
    big = [window for window in capture.draws.windows if window.draws >= TWO_FRAME_DRAWS]
    if big:
        answer.evidence.append("window(s) with " + ", ".join(str(window.draws) for window in big) +
                               " draw(s): that is two game frames' worth (a live frame measured about 7,500), which "
                               "is a hint from the window's own count, not a rule this answer applies")
    else:
        answer.evidence.append("every window's own draw count is below " + str(TWO_FRAME_DRAWS) +
                               ": a hint consistent with one game frame per window")
    if capture.format is not None and capture.format < 3:
        answer.evidence.append("the capture is format 2: the trailer cannot be checked, and the row cap was 6,000")
    if capture.draws.truncated:
        answer.evidence.append("draws.csv is marked TRUNCATED: the windows at the end of the file are cut")
    answer.status = ANSWER_YES if ok else ANSWER_UNKNOWN
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# B2: the glare chain and the stability of its parameters
# ---------------------------------------------------------------------------------------------------------------------


def answer_glare(capture: drawdiag.Capture) -> Answer:
    answer = Answer("B2", "the glare chain: are its draws present, and is cGlareParam stable across the samples?")
    bound = Counter()
    named_rows: list[drawdiag.Draw] = []
    joined_rows = 0
    for row in capture.rows:
        names = draw_names(capture, row)
        if not row.cb_names and not row.srv_names and names:
            joined_rows += 1
        matched = False
        for name in names:
            base = drawdiag.base_texture_name(name)
            if base in GLARE_CONSTANTS:
                bound[base] += 1
                matched = True
        if matched:
            named_rows.append(row)
    if bound:
        answer.evidence.append("draws that bind a glare constant buffer: "
                               + ", ".join(f"{name} in {count} row(s)" for name, count in bound.most_common()))
        answer.evidence.append("the draws, in frame and ordinal order: "
                               + ", ".join(f"frame {row.frame} ordinal {row.ordinal} (CRC {row.crc_text})"
                                           for row in named_rows[:8])
                               + (" ..." if len(named_rows) > 8 else ""))
        if joined_rows:
            answer.evidence.append(f"{joined_rows} row(s) of this capture name no resource of their own (the shader was "
                                   f"created before the capture was armed); their names come from the same CRC's row in "
                                   f"shaders.txt, which is why the chain above is visible at all")
    else:
        answer.evidence.append("no draw in the capture binds any of " + ", ".join(GLARE_CONSTANTS))
        if capture.shaders is None:
            answer.evidence.append("the capture has no shaders.txt, so a draw whose shader was created before the capture "
                                   "cannot be named at all")
        elif not joined_rows and not capture.shaders.creation:
            answer.evidence.append("shaders.txt holds no creation row, so it cannot name a draw whose own row carries none")
    samples = capture.cbdump.of_name("cGlareParam") if capture.cbdump else []
    if samples and bound:
        answer.status = ANSWER_YES
        decoded = [cblayouts.decode("cGlareParam", sample.data) for sample in samples]
        stats = cbdump_report._member_stats(decoded)
        moving = [stat.name for stat in stats if stat.changed]
        answer.evidence.append(f"cbdump.csv holds {len(samples)} sample(s) of cGlareParam: "
                               + ("stable across the samples" if not moving else "members that move: " + ", ".join(moving)))
        if moving:
            answer.evidence.append("a temporal pass whose parameter moves between frames will smear under a patch: keep "
                                   "a patch constant per scene")
    elif samples:
        answer.evidence.append(f"cbdump.csv holds {len(samples)} sample(s) of cGlareParam but no draw of this capture "
                               f"binds it")
    else:
        answer.evidence.append("cbdump.csv holds no cGlareParam sample" +
                               ("" if capture.cbdump else " (the capture has no cbdump.csv)") +
                               ": the stability of the parameter needs the per-sample dump")
    if not bound and samples:
        answer.status = ANSWER_UNKNOWN
    if bound and not samples:
        answer.evidence.append("the draws are there, but their parameter values were not sampled: the stability part "
                               "needs cbdump.csv (the per-sample constant-buffer dump)")
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# B7: the shadow atlas
# ---------------------------------------------------------------------------------------------------------------------


def answer_shadow_atlas(capture: drawdiag.Capture) -> Answer:
    answer = Answer("B7", "the shadow atlas: depth-only draws over its slices, and the depth view's size")
    depth_only = [row for row in capture.rows if not row.rtvs and row.dsv is not None]
    if not depth_only:
        answer.evidence.append("no depth-only draw (no render target, a depth view) in the capture")
        return answer
    by_size = Counter(format_size(row.dsv) for row in depth_only)
    # A slice stack is either much taller than it is wide or taller than the tallest single map the game uses.  A large
    # square-ish view (the main depth prepass of a 4K frame is 3840x2160) is a different target and must not be counted:
    # a slice offset list read off the wrong target says the atlas changed when it did not.
    atlas = [row for row in depth_only if row.dsv is not None and row.dsv.size is not None
             and (row.dsv.size.height >= ATLAS_MIN_HEIGHT or row.dsv.size.height > row.dsv.size.width)
             and row.viewports is not None and row.viewports.rects]
    tops = Counter(round(rect.top) for row in atlas for rect in row.viewports.rects)
    lines = [f"depth-only rows: {len(depth_only)}", "depth view(s): " + _counts(by_size.elements())]
    for top in sorted(tops):
        lines.append(f"  viewport top {top}: {tops[top]} row(s)")
    answer.details.append(fence(lines))
    answer.status = ANSWER_YES if atlas else ANSWER_UNKNOWN
    sizes = Counter(str(row.dsv.size) for row in atlas)
    answer.evidence.append(f"{len(atlas)} depth-only draw(s) with a viewport on an atlas-shaped depth view (height "
                           f">= {ATLAS_MIN_HEIGHT} or taller than wide); depth view(s) {_counts(sizes.elements())}")
    present = set(tops)
    if present:
        answer.evidence.append("viewport top values: " + ", ".join(str(value) for value in sorted(present)))
        missing = [value for value in ATLAS_TOP if value not in present]
        if missing:
            answer.evidence.append("not seen at top " + ", ".join(str(value) for value in missing) +
                                   f" (of {', '.join(str(value) for value in ATLAS_TOP)})")
        else:
            answer.evidence.append("all five slice offsets (0, 2048, 4096, 6144, 8192) are present: the atlas still "
                                   "has five 2048-tall slices")
    else:
        answer.evidence.append("the atlas-shaped depth view takes no draw with a recorded viewport")
    separate = [row for row in depth_only if row.dsv.size is not None and row.dsv.size.width == 2048
                and row.dsv.size.height == 2048]
    if separate:
        answer.evidence.append(f"a separate 2048x2048 depth map takes {len(separate)} depth-only draw(s)")
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# Formats: the G-buffer and the light buffers
# ---------------------------------------------------------------------------------------------------------------------


def answer_gbuffer(capture: drawdiag.Capture) -> Answer:
    answer = Answer("GB", "the G-buffer: the five-target draw's formats and sizes")
    rows = [row for row in capture.rows if row.target_count == 5]
    if not rows:
        answer.evidence.append("no five-target draw in the capture")
        return answer
    answer.status = ANSWER_YES
    signatures = Counter((_signature(row.rtvs), format_size(row.dsv)) for row in rows)
    total = len(rows)
    answer.evidence.append(f"{total} five-target row(s), {len(signatures)} distinct target/depth signature(s)")
    lines = []
    for (signature, depth), count in signatures.most_common(6):
        lines.append(f"{count:6d} row(s) ({100.0 * count / total:5.1f}%)  targets {signature}  depth {depth}")
    answer.details.append(fence(lines))
    tints = Counter(entry.format for row in rows for entry in row.rtvs)
    answer.evidence.append("target formats: " + _counts(tints.elements()))
    return answer


def answer_light_buffers(capture: drawdiag.Capture) -> Answer:
    answer = Answer("LB", "the light buffers: the formats and sizes the light draws read and write")
    diffuse = _rows_binding(capture.rows, LIGHT_DIFFUSE)
    specular = _rows_binding(capture.rows, LIGHT_SPECULAR)
    light_cb = [row for row in capture.rows if row.cb("g_LightParam") is not None]
    if not diffuse and not specular and not light_cb:
        answer.evidence.append("no row binds g_SamplerLightDiffuse/g_SamplerLightSpecular or g_LightParam")
        return answer
    answer.status = ANSWER_YES
    for name, rows in ((LIGHT_DIFFUSE, diffuse), (LIGHT_SPECULAR, specular)):
        if not rows:
            answer.evidence.append(f"{name}: no row binds it")
            continue
        entries = [entry for row in rows for entry in row.srvs if drawdiag.base_texture_name(entry.name) == name]
        answer.evidence.append(f"{name}: {len(rows)} row(s), {_counts(format_size(entry) for entry in entries)}")
    if light_cb:
        answer.evidence.append(f"draws that carry g_LightParam: {len(light_cb)} row(s), target(s) "
                               + _distinct(_signature(row.rtvs) for row in light_cb))
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# The engine pass overrides' counters (engine.txt)
# ---------------------------------------------------------------------------------------------------------------------


def _ssr_summary(engine: drawdiag.EngineStatus) -> tuple[bool, list[str]]:
    """The reflections' block of ``engine.txt``, as the evidence lines the EP answer prints.

    The block is the module's own answer to the question the engine block cannot answer: a water draw is served by a module the engine pass
    overrides never patch, and a session whose card read "on" and whose log had no line is settled here.  Returns (the module ran, the lines).
    """

    if not engine.ssr_present:
        return False, ["engine.txt has no screen-space reflections block: the capture predates it, so the file cannot say whether the "
                       "reflections ran"]
    if engine.ssr_off:
        return False, ["the screen-space reflections card was off in this session: no water draw could be served"]
    if not engine.ssr_armed:
        return False, [f"the reflections card was on but the module was not armed (the blend is at zero, or it stood itself down): "
                       f"{engine.ssr_runs} run(s), {engine.ssr_readers} reader(s) so far"]
    state = f"ssr runs: {engine.ssr_runs}, readers: {engine.ssr_readers}, declined: {engine.ssr_declined}"
    if engine.ssr_have_window:
        if engine.ssr_window_runs > 0 or engine.ssr_window_readers > 0:
            return True, [f"the screen-space reflections ran in this capture's window: {engine.ssr_window_runs} pass(es), "
                          f"{engine.ssr_window_readers} water draw(s) served with its own reflection ({state})"]
        gate = engine.ssr_window_dominant_passed_on()
        lines = [f"the reflections were armed and served no water draw in this capture's window ({state})"]
        if gate is not None:
            lines.append(f"the gate that stopped them in this window: {gate[0]} = {gate[1]}")
        elif engine.ssr_window_declined:
            lines.append(f"declines in this window: {engine.ssr_window_declined}, and no passed-on reason was recorded")
        elif engine.ssr_window_not_reflection:
            lines.append(f"every draw that reached the reflections' seam in this window was not a reflection reader "
                         f"({engine.ssr_window_not_reflection}): no water draw reached the module, so its shader was never stamped "
                         f"or the seam never saw a water draw")
        else:
            lines.append("neither a passed-on reason nor a decline was recorded in this window: no draw reached the reflections' seam")
        return False, lines
    gate = engine.ssr_dominant_passed_on()
    lines = [f"engine.txt carries no reflections window counters, so this capture's own frames are not in the file ({state})"]
    if engine.ssr_readers > 0 or gate is not None:
        ran = engine.ssr_readers > 0
        if ran:
            lines.append(f"the session up to this capture served {engine.ssr_readers} water draw(s) from the module")
        else:
            lines.append(f"the session up to this capture served no water draw; the dominant passed-on reason: {gate[0]} = {gate[1]}")
        return ran, lines
    return False, lines


def answer_engine(capture: drawdiag.Capture) -> Answer:
    """EP: the engine pass overrides' counters, from the capture's own engine.txt, with the reflections' counters beside them.

    The rule is the capture's own window, not the session: a module that patched at least one composite draw inside the
    frames this capture holds (``window runs``) is [O] with those counts and the facts of its last patched draw; a module
    that patched none there is [U] with the reason it declined most in that window.  The module's counters live for the
    whole session, so the plain ``runs``/``declined`` lines cannot answer this question - a later capture can show
    session runs while its own window saw none.

    Every answer prints ``armed`` and ``identity`` (the knob at 1.0), because "not armed", "the knob is at 1.0" and
    "armed, but no composite draw was bound" are three different scenes and the session's question is which one it is.
    A file that says the key was off is [U]; so is a folder without engine.txt, and a file written before the window
    lines (it gets the session counts and a note that it cannot answer for one capture's window).

    The same file's screen-space reflections block is read too: the reflections are served by a module of their own, so a
    capture whose engine block says nothing can still show that pass running, or name the gate that stopped the water
    draws (the card off, an unarmed module, a shader that was never stamped, ...).
    """

    answer = Answer("EP", "the engine pass overrides: did the module patch a composite draw, or which reason did it decline for")
    engine = capture.engine
    if engine is None:
        answer.evidence.append("this folder has no engine.txt: the capture predates the file, which every capture now writes")
        return answer
    # The reflections are read first: their card arms the draw seam on its own, so they can have run in a session whose engine pass
    # overrides key was off.
    ssr_ran, ssr_lines = _ssr_summary(engine)
    if ssr_ran:
        answer.status = ANSWER_YES
    if engine.module_off:
        answer.evidence.append("the engine pass overrides key was off for this session: the knob was never in play")
        answer.evidence.extend(ssr_lines)
        return answer
    if not engine.key_on:
        answer.evidence.append("engine.txt does not say whether the module was on: the file may be truncated")
        answer.evidence.extend(ssr_lines)
        return answer
    state = f"armed: {engine.value('armed') or '?'}, knob at 1.0: {engine.value('identity') or '?'}"
    if not engine.have_window:
        answer.evidence.append(f"engine.txt carries no window counters, so whether the module ran inside this capture's own window is "
                               f"not in the file ({state})")
        answer.evidence.append(f"the session up to this capture: {engine.runs} patched composite draw(s), {engine.declined} decline(s)")
        answer.evidence.extend(ssr_lines)
        return answer
    if engine.ran_in_window:
        answer.status = ANSWER_YES
        answer.evidence.append(f"the module patched {engine.window_runs} composite draw(s) and declined {engine.window_declined} in this "
                               f"capture's window ({state})")
        last = engine.last_run
        if last:
            answer.evidence.append(last)
        if engine.window_declines:
            answer.evidence.append("declines by reason in this window: "
                                   + "; ".join(f"{reason} = {count}" for reason, count in engine.window_declines[:8]))
    else:
        answer.evidence.append(f"the module patched no composite draw in this capture's window and declined "
                               f"{engine.window_declined} ({state})")
        dominant = engine.window_dominant_decline()
        if dominant is not None:
            answer.evidence.append(f"the dominant decline reason in this window: {dominant[0]} = {dominant[1]}")
        else:
            answer.evidence.append("no decline reason was recorded: no draw whose shader declares the ambient table and the composite's "
                                   "light was looked at in this window")
    if (engine.runs, engine.declined) != (engine.window_runs, engine.window_declined):
        answer.evidence.append(f"the session up to this capture: {engine.runs} run(s), {engine.declined} decline(s) (the window counts "
                               f"are this capture's own frames)")
    ssr_ran, ssr_lines = _ssr_summary(engine)
    if ssr_ran:
        answer.status = ANSWER_YES
    answer.evidence.extend(ssr_lines)
    return answer

# ---------------------------------------------------------------------------------------------------------------------
# FL: the engine's flare family
# ---------------------------------------------------------------------------------------------------------------------


def _members_text(decoded: dict) -> str:
    """The decoded members of one buffer as `name=value` text, compact: vectors in parentheses, the first row of a matrix."""

    parts: list[str] = []
    for name, value in decoded.get("members", {}).items():
        if isinstance(value, float):
            parts.append(f"{name}={value:.5g}")
        elif isinstance(value, (list, tuple)):
            if value and isinstance(value[0], (list, tuple)):
                parts.append(f"{name}=({value[0][0]:.4g}, {value[0][1]:.4g}, ...)")
            else:
                parts.append(f"{name}=(" + ", ".join(f"{item:.4g}" for item in value) + ")")
        else:
            parts.append(f"{name}={value}")
    return ", ".join(parts)


def _sun_text(engine: "drawdiag.EngineStatus | None") -> tuple[float, float, bool] | None:
    """The engine.txt sun line's position in the frame, `(x, y, in_window)`, or None when the file has no sun projection."""

    if engine is None:
        return None
    value = engine.value("sun")
    if not value or value == "-" or value.startswith("behind"):
        return None
    text = value.split(",")[0].strip()
    if "x" not in text:
        return None
    left, _, right = text.partition("x")
    try:
        return float(left), float(right), "in the window" in value
    except ValueError:
        return None


def answer_flare(capture: drawdiag.Capture) -> Answer:
    """FL: the flare family's draws and the values of its three parameter buffers.

    The flare draws (``Halo``, ``Sun``, the three ``RadialBlur`` passes and ``GodraysMerge``) were never sampled before this
    build: the diagnostic's constant-buffer list did not carry the three flare parameters, so a capture could name the draws
    and say nothing about their centre or strength.  This answer prints what each sample holds and compares the sun's screen
    UV from ``cHaloParam`` (or ``cSunParam``) with engine.txt's sun line, which the diagnostic projects from the light pass it
    read: two independent computations of one position, so a disagreement is visible in the report.  A row the file cannot name
    is counted rather than read as "the family did not run": a capture whose draws predate the names the diagnostic keeps for
    them holds the passes and not their names, and this answer says so instead of answering from an absence.
    """

    answer = Answer("FL", "the engine's flare: its parameter values, and whether the frame's sun could show it")
    bound = Counter()
    rows: list[drawdiag.Draw] = []
    for row in capture.rows:
        matched = False
        for name in draw_names(capture, row):
            base = drawdiag.base_texture_name(name)
            if base in FLARE_CONSTANTS:
                bound[base] += 1
                matched = True
        if matched:
            rows.append(row)
    if bound:
        answer.evidence.append("draws that bind a flare parameter: "
                               + ", ".join(f"{name} in {count} row(s)" for name, count in bound.most_common()))
        answer.evidence.append("the draws, in frame and ordinal order: "
                               + ", ".join(f"frame {row.frame} ordinal {row.ordinal} (CRC {row.crc_text})" for row in rows[:8])
                               + (" ..." if len(rows) > 8 else ""))
    else:
        # A draw whose pixel shader the capture never named can still be a flare pass: this file cannot say either way, and claiming the
        # family did not run would be an answer the file does not hold. The rows the file cannot name are counted instead, so the reader knows
        # the difference between "the flare is absent" and "the flare could not be identified".
        unnamed = sum(1 for row in capture.rows if not draw_names(capture, row))
        answer.evidence.append("no draw in this capture binds any of " + ", ".join(FLARE_CONSTANTS)
                               + " by the names this file holds: either the engine's flare family (Halo, Sun, the radial blurs) did not run "
                               "in these frames, or the draws are here without a name")
        if unnamed:
            answer.evidence.append(f"{unnamed} draw(s) in this capture carry a pixel shader whose reflection the file does not hold, so the "
                                   f"file cannot say whether the flare family ran with this build's capture: a capture of this build names them "
                                   f"from the shaders kept from creation, and its cbdump.csv samples the three buffers")

    samples_found = 0
    for name in FLARE_CONSTANTS:
        samples = capture.cbdump.of_name(name) if capture.cbdump else []
        if not samples:
            answer.evidence.append(f"cbdump.csv holds no {name} sample"
                                   + ("" if capture.cbdump else " (the capture has no cbdump.csv)"))
            continue
        samples_found += 1
        decoded = cblayouts.decode(name, samples[-1].data)
        answer.evidence.append(f"{name}: {len(samples)} sample(s); the last one holds " + _members_text(decoded))
        if name == "cRadialBlurParam":
            distinct = {tuple(round(v, 6) for v in cblayouts.decode(name, sample.data)["members"].get("centre", ()))
                        for sample in samples}
            answer.evidence.append(f"cRadialBlurParam: {len(distinct)} distinct centre(s) across the capture, so the separately "
                                   f"bound buffers of the three passes are visible as different values"
                                   if len(distinct) > 1 else
                                   "cRadialBlurParam: one centre across the capture")

    sun = _sun_text(capture.engine)
    if sun is not None:
        window = None
        last_window = capture.engine.value("last window") if capture.engine else None
        if last_window:
            size_text = last_window.rsplit(",", 1)[-1]
            if "x" in size_text:
                try:
                    window = tuple(float(part) for part in size_text.split("x"))
                except ValueError:
                    window = None
        uv = None
        for name in ("cHaloParam", "cSunParam"):
            samples = capture.cbdump.of_name(name) if capture.cbdump else []
            if samples:
                members = cblayouts.decode(name, samples[-1].data)["members"]
                if "sunUv" in members:
                    uv = members["sunUv"]
                    answer.evidence.append(f"the {name} sun UV: ({uv[0]:.4f}, {uv[1]:.4f})")
                    break
        answer.evidence.append(f"engine.txt's sun line: {sun[0]:.1f}x{sun[1]:.1f}"
                               + (", in the window" if sun[2] else ""))
        if uv is not None and window is not None:
            screen = (uv[0] * window[0], uv[1] * window[1])
            answer.evidence.append(f"the flare parameter's sun UV times the frame's window ({window[0]:.0f}x{window[1]:.0f}): "
                                   f"{screen[0]:.1f}x{screen[1]:.1f}, {abs(screen[0] - sun[0]) + abs(screen[1] - sun[1]):.1f} px from "
                                   f"the projection engine.txt carries")
    elif capture.engine is not None:
        answer.evidence.append("engine.txt's sun line: " + (capture.engine.value("sun") or "-")
                               + " (the diagnostic's own projection of the frame's sun, from the light pass it last read)")

    if bound and samples_found:
        answer.status = ANSWER_YES
    return answer


# ---------------------------------------------------------------------------------------------------------------------
# One capture
# ---------------------------------------------------------------------------------------------------------------------


def answer_capture(capture: drawdiag.Capture) -> CaptureAnswers:
    answers = CaptureAnswers(capture=capture)
    answers.answers = [
        answer_wetness(capture),
        answer_wetness_pass(capture),
        answer_reflection_chain(capture),
        answer_reflection_array(capture),
        answer_frame_split(capture),
        answer_glare(capture),
        answer_raytracing(capture),
        answer_shadow_atlas(capture),
        answer_gbuffer(capture),
        answer_light_buffers(capture),
        answer_engine(capture),
        answer_flare(capture),
    ]
    return answers


# ---------------------------------------------------------------------------------------------------------------------
# Across captures: the ambient probe table and the wetness value
# ---------------------------------------------------------------------------------------------------------------------

AMBIENT_COMPARE = ("paramNum", "skyShRecipScale", "skyVisibility", "skyVisibilityScale", "color", "ssaoMask")


def _ambient_signature(sample: drawdiag.CbDumpSample) -> dict[str, object]:
    decoded = cblayouts.decode("g_AmbientParamArray", sample.data)
    members = decoded.get("members", {})
    # Only the probes the header says the engine fills: the rest of the array is padding, and a range over it would
    # compare bytes nobody wrote.
    probes, _param_num = cbdump_report.active_probes(decoded)
    summary: dict[str, object] = {
        "paramNum": members.get("paramNum"),
        "skyShRecipScale": members.get("skyShRecipScale"),
    }
    for key in ("skyVisibility", "skyVisibilityScale", "ssaoMask"):
        values = [probe["members"][key] for probe in probes if isinstance(probe.get("members", {}).get(key), float)]
        if values:
            summary[key] = (round(min(values), 6), round(max(values), 6), len(values))
    return summary


def _wetness_signature(sample: drawdiag.CbDumpSample) -> list[float | None]:
    decoded = cblayouts.decode(cbdump_report.WETNESS, sample.data)
    members = decoded.get("members", {})
    return [members.get(f"f{slot}") for slot in range(8)]


def compare_captures(captures: Sequence[drawdiag.Capture]) -> list[Answer]:
    answers: list[Answer] = []
    # B4: the ambient probe table, capture against capture.
    ambient = Answer("B4", "the ambient probe table between captures: night against day")
    with_ambient = [(capture, capture.cbdump.of_name("g_AmbientParamArray")) for capture in captures if capture.cbdump]
    with_ambient = [(capture, samples) for capture, samples in with_ambient if samples]
    if len(with_ambient) < 2:
        ambient.evidence.append(f"{len(with_ambient)} capture(s) with a g_AmbientParamArray sample: the comparison needs "
                                f"two captures taken at different times of day or weather")
        if with_ambient:
            capture, samples = with_ambient[0]
            ambient.evidence.append(f"{capture.name}: {len(samples)} sample(s), "
                                   f"{_ambient_line(_ambient_signature(samples[-1]))}")
    else:
        ambient.status = ANSWER_YES
        lines = []
        signatures = {}
        for capture, samples in with_ambient:
            signature = _ambient_signature(samples[-1])
            signatures[capture.name] = signature
            lines.append(f"{capture.name}: {_ambient_line(signature)}")
        ambient.details.append(fence(lines))
        base_name, base = next(iter(signatures.items()))
        differing = []
        for other_name, other in signatures.items():
            if other_name == base_name:
                continue
            for key in AMBIENT_COMPARE:
                if base.get(key) != other.get(key):
                    differing.append(f"{key}: {base_name} {_value_text(base.get(key))} vs {other_name} "
                                     f"{_value_text(other.get(key))}")
        if differing:
            ambient.evidence.append(f"{len(differing)} member(s) differ between the captures: "
                                   + "; ".join(differing[:6]))
        else:
            ambient.evidence.append("every compared member is identical across the captures: either they were taken "
                                    "at the same time of day, or this table does not follow it")
    answers.append(ambient)

    # The wetness value across the captures: the dry -> rain -> dry sequence the puddle work needs.
    wetness = Answer("P1x", "the wetness value between captures: dry against rain")
    with_wetness = [(capture, capture.cbdump.of_name(cbdump_report.WETNESS)) for capture in captures if capture.cbdump]
    with_wetness = [(capture, samples) for capture, samples in with_wetness if samples]
    if len(with_wetness) < 2:
        wetness.evidence.append(f"{len(with_wetness)} capture(s) with a g_WetnessParameter sample: a ramp needs at "
                                f"least two captures at different weather")
        if with_wetness:
            capture, samples = with_wetness[0]
            wetness.evidence.append(f"{capture.name}: " + " ".join(cbdump_report.fmt(value)
                                                                   for value in _wetness_signature(samples[-1])))
    else:
        wetness.status = ANSWER_YES
        lines = []
        values = {}
        for capture, samples in with_wetness:
            value = _wetness_signature(samples[-1])
            values[capture.name] = value
            lines.append(f"{capture.name:24s} " + " ".join(f"{cbdump_report.fmt(item):>10s}" for item in value))
        wetness.details.append(fence(["capture" + " " * 16 + " " + " ".join(f"{'f' + str(slot):>10s}" for slot in range(8))]
                                     + lines))
        base_name, base = next(iter(values.items()))
        differing = []
        for other_name, other in values.items():
            if other_name == base_name:
                continue
            for slot, (left, right) in enumerate(zip(base, other)):
                if left is not None and right is not None and left != right:
                    differing.append(f"f{slot}: {base_name} {cbdump_report.fmt(left)} vs {other_name} "
                                     f"{cbdump_report.fmt(right)}")
        wetness.evidence.append("floats that differ: " + ("; ".join(differing[:8]) if differing else "none"))
    answers.append(wetness)
    return answers


def _value_text(value: object) -> str:
    if isinstance(value, tuple) and len(value) == 3:
        return f"{value[0]}..{value[1]} over {value[2]} probe(s)"
    return str(value)


def _ambient_line(signature: dict[str, object]) -> str:
    parts = []
    for key in ("paramNum", "skyShRecipScale"):
        if key in signature:
            parts.append(f"{key} {signature[key]}")
    for key in ("skyVisibility", "skyVisibilityScale", "ssaoMask"):
        value = signature.get(key)
        if isinstance(value, tuple):
            parts.append(f"{key} {value[0]}..{value[1]} over {value[2]} probe(s)")
    return ", ".join(parts)


# ---------------------------------------------------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------------------------------------------------


def _capture_table(captures: Sequence[drawdiag.Capture]) -> list[str]:
    lines = ["| capture | draws | frames | cbdump | views.txt | dump | notices |",
             "|---|---|---|---|---|---|---|"]
    for capture in captures:
        draws = capture.draws
        rows = len(draws.rows) if draws else 0
        frames = len(draws.windows) if draws else 0
        cbdump = len(capture.cbdump.rows) if capture.cbdump else 0
        views = len(capture.views.samples) if capture.views else 0
        dump = len(capture.dump.facts) if capture.dump else 0
        lines.append(f"| {capture.name} | {rows} row(s), format {draws.format if draws else '?'} | {frames} | "
                     f"{cbdump} sample(s) | {views} line(s) | {dump} texture(s) | {len(capture.all_notices())} |")
    return lines


def render_capture(answers: CaptureAnswers) -> list[str]:
    capture = answers.capture
    lines = [f"## {capture.name}", ""]
    lines.append(f"`{capture.folder}`")
    lines.append("")
    if answers.warnings:
        lines.append("Notes the reader raised:")
        for warning in answers.warnings:
            lines.append(f"- {warning}")
        lines.append("")
    for answer in answers.answers:
        lines.append(f"### {answer.key} - {answer.title}")
        lines.append("")
        lines.append(f"`{answer.mark}`")
        lines.append("")
        for item in answer.evidence:
            lines.append(f"- {item}")
        for detail in answer.details:
            lines.append("")
            lines.append(detail)
        lines.append("")
    return lines


def render(captures: Sequence[drawdiag.Capture], cross: Sequence[Answer], session_root: str) -> str:
    lines = ["# Session answers", ""]
    lines.append(f"Written by `tools/diag/session_answers.py` from {len(captures)} capture folder(s) under "
                 f"`{session_root}`.")
    lines.append("")
    lines.append("`[O]` = answered from the files, with the evidence row named.  `[U]` = the files cannot answer it; "
                 "the bullet says what would.  Every count is from the captured windows, which hold a part of each "
                 "frame (the row cap drops the rest and the frame line counts it).")
    lines.append("")
    lines.append("## The question keys")
    lines.append("")
    lines.append("| key | question |")
    lines.append("|---|---|")
    for key, title in QUESTION_KEYS:
        lines.append(f"| {key} | {title} |")
    lines.append("")
    lines.append("## The captures")
    lines.append("")
    lines.extend(_capture_table(captures))
    lines.append("")
    for capture in captures:
        lines.extend(render_capture(answer_capture(capture)))
    lines.append("## Across captures")
    lines.append("")
    for answer in cross:
        lines.append(f"### {answer.key} - {answer.title}")
        lines.append("")
        lines.append(f"`{answer.mark}`")
        lines.append("")
        for item in answer.evidence:
            lines.append(f"- {item}")
        for detail in answer.details:
            lines.append("")
            lines.append(detail)
        lines.append("")
    lines.append("## What this report does not answer")
    lines.append("")
    lines.append("- Whether the wetness floats mean what their movement suggests: that is a visual check, two "
                 "screenshots at the same spot.")
    lines.append("- Whether a patch on any of these buffers looks right: the numbers here are the input to that, not "
                 "the verdict.")
    lines.append("- Anything about a pass that ran outside the captured windows: the row cap keeps a part of each "
                 "frame, and a capture keeps a few frames.")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments or arguments[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if arguments else 2
    root = arguments[0]
    out: str | None = None
    quiet = False
    index = 1
    while index < len(arguments):
        if arguments[index] == "--out" and index + 1 < len(arguments):
            out = arguments[index + 1]
            index += 2
        elif arguments[index] == "--quiet":
            quiet = True
            index += 1
        else:
            print(f"unknown argument: {arguments[index]}", file=sys.stderr)
            return 2
    folders = drawdiag.find_captures(root)
    if not folders:
        print(f"{root} holds no capture folder (no draws.csv at or below it)", file=sys.stderr)
        return 2
    # "Next to the captures": the folder that holds the capture folders, unless the argument is itself one capture.
    session_root = os.path.dirname(os.path.abspath(folders[0])) if os.path.abspath(root) == os.path.abspath(folders[0]) \
        else os.path.abspath(root)
    captures = [drawdiag.load_capture(folder) for folder in folders]
    captures = [capture for capture in captures if capture.draws is not None]
    if not captures:
        print(f"no capture folder under {root} holds a readable draws.csv", file=sys.stderr)
        return 2
    cross = compare_captures(captures)
    text = render(captures, cross, session_root)
    if not quiet:
        sys.stdout.write(text)
    target = out or os.path.join(session_root, "session-answers.md")
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"\nwritten: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
