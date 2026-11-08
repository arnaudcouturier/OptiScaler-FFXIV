"""Answer the cascade shadow atlas' facts for one capture folder: what a god-rays march is pinned on.

The march's design needs the atlas' bind identity, the per-cascade light-space matrices with their distances, and the
bias block the engine's own evaluation subtracts. The capture carries all of it:

- ``views.txt``'s ``g_SamplerShadow`` row: the atlas' view and resource formats, its size, and its pointer. A
  ``TEXTURE2DARRAY`` is the 5-slice shape; a plain ``TEXTURE2D`` tiles the cascades into the plane's rows.
- ``draws.csv``, every draw whose shader declares `g_SamplerShadow`: grouped by shader CRC, because the frame draws the
  sun mask with one shader four times (one per cascade), each draw binding its own ``g_DirectionalShadowParameter``
  instance; the sun's own light draw (it reads ``g_SamplerShadowMask``) reads one of those instances again.
- ``cbdump.csv``, the sampled values, keyed per distinct pointer: one window of the pointer cadence holds the frame's
  cascade instances, and the march ports one matrix per cascade, so pointers are never folded into one instance.
- ``engine.txt``'s ``sun:`` line, so the reader knows whether the pose faced the sun at all.
- when the one-frame dump carried it, ``dump/atlas.bin``: per 2048-row band, the decoded R16 texels' min, max and mean,
  which is the number the comparison convention is pinned on, together with the mask ``dump/wetness.bin`` facts.

    python -I shadow_atlas.py <capture-folder> [--out report.txt]

Exit code 0 when the report was produced (an empty or format-2 capture is still a produced report), 2 when the folder
has no ``draws.csv`` at all.
"""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cblayouts  # noqa: E402
import drawdiag  # noqa: E402

SHADOW_NAME = "g_SamplerShadow"
MASK_NAME = "g_SamplerShadowMask"
SHADOW_PARAMETER = "g_DirectionalShadowParameter"
SHADOW_BIAS = "g_ShadowBiasParameter"

# The blend column is `enable/src/dst/op/srcAlpha/dstAlpha/opAlpha/mask` with D3D11 enum values, so a MIN-blend draw
# carries the operation's value in the third index. The mask's assembly rows measured 4 (MIN) in the session-4 capture.
BLEND_OP_MIN = 4


def blend_op_of(draw: drawdiag.Draw) -> int | None:
    return draw.blend[3] if draw.blend is not None and len(draw.blend) > 3 else None


def band_stats(data: bytes, width: int, height: int, row_pitch: int, band_rows: int) -> list[tuple[int, float, float, float]]:
    """Per band of ``band_rows`` rows, (first row, min of the texels, max of the texels, mean of the texels).

    The bin is the packed image the readback wrote (one packed row per texture row), so the only assumptions are a
    2-byte texel and the facts line's row pitch. A band is one cascade's slice in this build's plane, which is what
    makes per-band numbers the depth convention's own evidence.
    """

    stats: list[tuple[int, float, float, float]] = []
    if width <= 0 or row_pitch < width * 2:
        return stats
    bands = (height + band_rows - 1) // band_rows
    for band in range(bands):
        first = band * band_rows
        count = min(band_rows, height - first)
        lo, hi, total = float("inf"), float("-inf"), 0
        for row in range(count):
            start = (first + row) * row_pitch
            chunk = data[start:start + width * 2]
            if len(chunk) < width * 2:
                continue
            for value in struct.unpack(f"<{width}H", chunk):
                scaled = value / 65535.0
                if scaled < lo:
                    lo = scaled
                if scaled > hi:
                    hi = scaled
                total += value
        n = count * width
        stats.append((first, lo, hi, total / (65535.0 * n) if n else 0.0))
    return stats


def decode_by_pointer(cbdump: drawdiag.CbDump | None, name: str) -> dict[int, drawdiag.CbDumpSample]:
    """The last sample of each distinct pointer of one named buffer.

    One matrix per cascade is what the port needs, so pointers are keys and never fold; a pointer sampled twice in one
    window keeps its latest bytes.
    """

    by_pointer: dict[int, drawdiag.CbDumpSample] = {}
    if cbdump is None:
        return by_pointer
    wanted = drawdiag.base_texture_name(name)
    for sample in cbdump.rows:
        if drawdiag.base_texture_name(sample.name) != wanted or sample.pointer is None:
            continue
        by_pointer[sample.pointer] = sample
    return by_pointer


def member_text(decoded: dict, names: list[str]) -> list[str]:
    members = decoded.get("members", {})
    lines: list[str] = []
    for name in names:
        value = members.get(name)
        if value is None:
            lines.append(f"  {name}: unread")
        elif isinstance(value, (list, tuple)):
            shown = ", ".join(f"{element:+.6f}" if isinstance(element, float) else str(element) for element in value)
            lines.append(f"  {name} = [{shown}]")
        elif isinstance(value, float):
            lines.append(f"  {name} = {value:+.6f}")
        else:
            lines.append(f"  {name} = {value}")
    return lines


def build_report(capture: drawdiag.Capture) -> str:
    out: list[str] = [f"# shadow-atlas report: {Path(capture.folder).name}", ""]
    out.append("## The atlas")
    atlas = capture.views.one(SHADOW_NAME) if capture.views is not None else None
    if atlas is None:
        out.append("views.txt holds no g_SamplerShadow line: the capture had no atlas bind at all, or the diagnostic's "
                   "name list predates this build's origins.")
    else:
        out.append(", ".join([
            f"dimension {atlas.view_dimension}",
            f"view format {atlas.view_format}",
            f"resource format {atlas.texture_format}",
            f"size {atlas.width}x{atlas.height}",
            f"array {atlas.texture_array}",
            f"mips {atlas.texture_mips}",
            f"pointer {atlas.pointer:#x}" if atlas.pointer else "pointer -",
            f"first bound at ordinal {atlas.ordinal}",
        ]))
        out.append("A TEXTURE2D with mips 1 and no array is one plane whose rows carry the cascades: the march samples "
                   "atlas texel coordinates, not array indices.")
        out.append("")

    out.append("## The atlas' readers")
    readers = [draw for draw in capture.draws.rows if draw.srv_names and SHADOW_NAME in draw.srv_names] if capture.draws else []
    masks = [draw for draw in capture.draws.rows if draw.srv_names and MASK_NAME in draw.srv_names] if capture.draws else []
    if not readers:
        out.append("no draw of the capture samples the atlas: the mask was never rebuilt in the captured frames, or the "
                   "splitter's identification missed the draws (the pose may yet have had one).")
    else:
        by_crc: dict[int, list[drawdiag.Draw]] = {}
        for draw in readers:
            key = draw.ps_crc if draw.ps_crc is not None else 0
            by_crc.setdefault(key, []).append(draw)
        for crc, rows in by_crc.items():
            pointers: dict[int, list[int]] = {}
            min_blend = 0
            for draw in rows:
                entry = draw.cb(SHADOW_PARAMETER)
                if entry is not None and entry.pointer is not None:
                    pointers.setdefault(entry.pointer, []).append(draw.ordinal)
                if blend_op_of(draw) == BLEND_OP_MIN:
                    min_blend += 1
            targets = sorted({entry.format for draw in rows for entry in draw.rtvs if entry.format})
            out.append(", ".join([
                f"shader {drawdiag.crc_key(crc)}",
                f"draws {len(rows)}",
                f"MIN-blend draws {min_blend}",
                "targets " + ("|".join(targets) if targets else "-"),
                f"shadow instances {len(pointers)}",
            ]))
            for pointer, ordinals in sorted(pointers.items()):
                out.append(f"  {SHADOW_PARAMETER} instance {pointer:#x} bound at ordinals "
                           f"{','.join(str(o) for o in ordinals)}")
        for draw in masks:
            entry = draw.cb(SHADOW_PARAMETER)
            shown = f"{entry.pointer:#x}" if entry is not None and entry.pointer else "-"
            out.append(f"the sun's own draw: shader {drawdiag.crc_key(draw.ps_crc)}, ordinal {draw.ordinal}, reads "
                       f"{MASK_NAME}, its shadow instance {shown}")
    out.append("")

    out.append("## The sampled values")
    if capture.cbdump is None:
        out.append("cbdump.csv is missing: nothing to decode.")
    else:
        parameter = decode_by_pointer(capture.cbdump, SHADOW_PARAMETER)
        bias = decode_by_pointer(capture.cbdump, SHADOW_BIAS)
        if not parameter and not bias:
            out.append("no sample of the shadow parameter or the bias landed in the window: the readers ran before the "
                       "first second started, every read was over the byte cap, or this build's DLL had no pointer "
                       "cadence yet (one row a second per name folds the instances).")
        parameter_layout = cblayouts.layout_of(SHADOW_PARAMETER)
        bias_layout = cblayouts.layout_of(SHADOW_BIAS)
        parameter_names = [member.name for member in parameter_layout.members] if parameter_layout else []
        bias_names = [member.name for member in bias_layout.members] if bias_layout else []
        for pointer, sample in sorted(parameter.items()):
            out.append(f"{SHADOW_PARAMETER} instance {pointer:#x} (frame {sample.frame}, {sample.time_s:.3f}s, shader "
                       f"{sample.ps_crc_text}, slot {sample.slot}, {sample.bytes} bytes)")
            if parameter_layout is not None:
                out.extend(member_text(cblayouts.decode(SHADOW_PARAMETER, sample.data), parameter_names))
            floats = cblayouts.floats_of(sample.data)
            for row in range(0, min(len(floats), 32), 4):
                out.append(f"    @0x{row * 4:02x} = " + ", ".join(f"{value:+.6f}" for value in floats[row:row + 4]))
        if parameter:
            shared: list[str] = []
            blocks: dict[int, set[bytes]] = {}
            for sample in parameter.values():
                for offset in range(0, min(sample.bytes or 0, 0x80), 0x10):
                    blocks.setdefault(offset, set()).add(bytes(sample.data[offset:offset + 0x10]))
            for offset in sorted(blocks):
                count = len(blocks[offset])
                shared.append(f"0x{offset:02x}: {'shared' if count == 1 else f'{count} values'}")
            out.append("blocks across the instances: " + ", ".join(shared))
            out.append("A block shared across the instances is the march's constant; a block that differs per instance "
                       "is the cascade's own matrix or range. The writer's bytecode says which of the tail rows its "
                       "loops read (the godrays study, appendix A).")
        for pointer, sample in sorted(bias.items()):
            out.append(f"{SHADOW_BIAS} instance {pointer:#x} (frame {sample.frame}, {sample.time_s:.3f}s, shader "
                       f"{sample.ps_crc_text})")
            if bias_layout is not None:
                out.extend(member_text(cblayouts.decode(SHADOW_BIAS, sample.data), bias_names))
        out.append("")

    out.append("## The pose")
    if capture.engine is None:
        out.append("engine.txt is missing: the pose's sun placement has no answer.")
    else:
        sun = capture.engine.value("sun")
        if sun:
            out.append(f"sun: {sun}")
        else:
            out.append("engine.txt carries no sun line (the diag build predates it, or the module was off).")
    out.append("")

    out.append("## The atlas' texels (dump)")
    dump_facts_path = Path(capture.folder) / "dump" / "facts.txt"
    atlas_bin = Path(capture.folder) / "dump" / "atlas.bin"
    if not (dump_facts_path.is_file() and atlas_bin.is_file()):
        out.append("the one-frame dump did not carry atlas.bin: the cross-check waits for a session whose dump budget "
                   "kept the atlas (DumpRank::Atlas is displaced before any lighting target; only the sun mask and the water gate's files go before it).")
    else:
        facts = drawdiag.load_dump_facts(str(dump_facts_path))
        fact = next((entry for entry in facts.facts if entry.file == "atlas.bin"), None)
        if fact is None:
            out.append("dump/facts.txt has no atlas line: the atlas was displaced or refused (the facts file names why).")
            out.extend(f"skip: {name}: {reason}" for name, reason in facts.skipped if "atlas" in name)
        else:
            out.append(", ".join([f"format {fact.format}", f"size {fact.width}x{fact.height}",
                                  f"row pitch {fact.row_pitch}", f"bytes {fact.bytes}",
                                  f"pointer {fact.pointer:#x}" if fact.pointer else "pointer -"]))
            data = atlas_bin.read_bytes()
            for first, lo, hi, mean in band_stats(data, fact.width, fact.height, fact.row_pitch or 0, 2048)[:8]:
                out.append(f"  rows {first}..{first + 2047}: min {lo:.4f}, max {hi:.4f}, mean {mean:.4f}")
            mask_fact = next((entry for entry in facts.facts if entry.file == "wetness.bin"), None)
            if mask_fact is not None:
                out.append(f"the mask (wetness.bin) beside it: {mask_fact.width}x{mask_fact.height} in "
                           f"{mask_fact.format}, ordinal {mask_fact.ordinal}")
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    folders = [argument for argument in argv[1:] if not argument.startswith("--")]
    destination = None
    for index, argument in enumerate(argv):
        if argument == "--out":
            destination = argv[index + 1]
    if not folders:
        print(__doc__)
        return 2
    reports: list[str] = []
    for folder in folders:
        capture = drawdiag.load_capture(folder)
        reports.append(build_report(capture))
        reports.extend(f"(!) {notice}" for notice in capture.notices)
    text = "\n".join(reports)
    if destination:
        Path(destination).write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
