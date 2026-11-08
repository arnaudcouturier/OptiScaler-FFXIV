"""Answer one capture folder's fog state and its per-material shading rows, for a marcher to be pinned on.

The fog answer gathers three decoded buffers from ``cbdump.csv``, each with a member table this folder's ``cblayouts``
holds from the game's own disassembly:

- ``cFogParam`` (the ``Fog_GU`` post's own rows, the fog post's asm lines 89-179): the height-profile constants, the
  depth-to-distance pair, the LUT coordinate pair, the fog state's own sun (``sunDirection``, the sky-consistent state
  the march needs), the camera row, and the 3-D volume-map switch's live value (1.0 = the volume branch never runs).
- ``cExpHeightFogParam``: the post's two-layer profile and the DI/mix row, member by member.
- ``g_FogParameter`` (the material family's own state): the per-material fog colour and density pair, with the ride-along
  ``useHeightFogUpdate`` the polarity question reads.

The shading rows answer reads ``dump/shadertypes.bin``, the composite's structured ``g_ShaderTypeParameter`` the
one-frame dump carries (one 128-B row per material, the row index ``gbuffer0.a`` names): the row table, with which rows
carry a subsurface or hair shape (``lightingType`` / ``useSubSurfaceRate`` / ``furLength``), so the skin-key question
has named rows instead of indexed blobs.

    python -I fog_state.py <capture-folder> [--out report.txt]

Exit code 0 when the report was produced (a capture with none of the names is still a produced report), 2 when the
folder is not a capture at all.
"""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cblayouts  # noqa: E402  (after the path line: the tools run with `python -I`)
import drawdiag  # noqa: E402

FOG_PARAMS = ("cFogParam", "cExpHeightFogParam", "g_FogParameter")
MATERIAL_PARAM = "g_FogParameter"
SHADING_FILE = "shadertypes.bin"
SHADING_LAYOUT = "g_ShaderTypeParameter"
# The rows the report shows per table.  A capture cannot name every row the game uses, so the count cap keeps the
# report readable: rows past it are counted, not shown.
SHADING_MAX_ROWS_SHOWN = 24

# The display order per buffer: only the members a marcher or a knob names, the rest live in the stats line.
FOG_ORDER = {
    "cFogParam": ("fogColour", "sunDirection", "volumeMapBlend", "lutCoordA", "lutCoordB", "depthToDistanceA",
                  "depthToDistanceB", "minDepth", "volumeDepthScale", "cameraPosition", "diColour", "diIntensity"),
    "cExpHeightFogParam": ("distanceStart", "minOpacity", "fallOff1", "density1", "refHeight1", "fallOff2",
                           "density2", "refHeight2", "mixScale", "diIntensity", "diExponent", "diStartDistance"),
    "g_FogParameter": ("fogColor", "startDistance", "fogDensity", "expFogHeight", "fogHeightFallOff", "fogDensity2",
                       "expFogHeight2", "fogHeightFallOff2", "lutParamX", "lutParamY", "useHeightFogUpdate",
                       "diExponent", "cameraPos", "sunDir", "diColor", "diColorIntensity", "angleAttenStart",
                       "invAngleAttenLength", "angleAttenIntensity"),
}

# The shadertype row's own classification knobs: the subsurface/hair shape features the chara family's own shaders test.
SHADING_ORDER = ("lightingType", "subSurfaceProfileId", "subSurfaceWidth", "backScatterPower", "sheenRate",
                 "sheenTintRate", "sheenAperture", "useSubSurfaceRate", "hairScatterColorShift", "hairSpecularShift",
                 "furLength", "hairRoughnessOffsetRate", "subSurfacePower")


def _row(value: object) -> str:
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (int, float)):
        return f"{float(value):+.6g}"
    if isinstance(value, (list, tuple)):
        return "[" + " ".join(_row(item) for item in value) + "]"
    return str(value)


LUT_FILE = "fog_lut.bin"
SKY_FILE = "sky.bin"
# Bytes a texel of each format the two fog targets can be written in takes; the LUT reads the first two half-float channels of a texel.
LUT_TEXEL_BYTES = {"R16G16_FLOAT": 4, "R16G16B16A16_FLOAT": 8}


def _fog_texture_section(out: list[str], folder: Path) -> None:
    """The fog post's two typed dump targets: the shared weight LUT as its float pairs, and the sky colour's shape."""

    out.append("## dump/fog_lut.bin and dump/sky.bin (the fog post's weight LUT and sky colour)")
    facts_path = folder / "dump" / drawdiag.DUMP_FACTS_FILE
    if not facts_path.is_file():
        out.append("the capture's dump/facts.txt is missing: the one-frame dump did not run.")
        return
    facts = drawdiag.load_dump_facts(str(facts_path))
    for file, label in ((LUT_FILE, "weight LUT, sLut"), (SKY_FILE, "sky colour, sSky")):
        fact = next((entry for entry in facts.facts if entry.file == file), None)
        if fact is None:
            skipped = next((reason for name, reason in facts.skipped if name == file), None)
            out.append(f"{file} ({label}): not carried: "
                       + (f"it was skipped ({skipped})." if skipped
                          else "this build predates the typed fog targets, or no draw declared the fog post's parameter buffer."))
            continue
        out.append(f"{file} ({label}): format {fact.format}, {fact.width}x{fact.height}, {fact.bytes} B, "
                   f"row pitch {fact.row_pitch}, draw ordinal {fact.ordinal}, "
                   f"pointer {hex(fact.pointer) if fact.pointer else '-'}")
        path = folder / "dump" / file
        if not path.is_file():
            out.append(f"  facts.txt names {file} but the file is missing on disk.")
            continue
        if file != LUT_FILE:
            continue
        texel = LUT_TEXEL_BYTES.get(fact.format)
        if texel is None:
            out.append(f"  the LUT is {fact.format}: this reader decodes {', '.join(LUT_TEXEL_BYTES)} only.")
            continue
        data = path.read_bytes()
        count = min(fact.width * fact.height, len(data) // texel)
        pairs = [struct.unpack_from("<2e", data, index * texel) for index in range(count)]
        out.append(f"  {count} texel(s) of the first two half-float channels (u = texel index / {max(count - 1, 1)}):")
        if pairs:
            reds, greens = [pair[0] for pair in pairs], [pair[1] for pair in pairs]
            out.append(f"  R range [{_row(min(reds))}, {_row(max(reds))}], G range [{_row(min(greens))}, {_row(max(greens))}]")
        for start in range(0, count, 4):
            out.append("  " + " | ".join(f"{index}: {_row(pairs[index][0])} {_row(pairs[index][1])}"
                                         for index in range(start, min(start + 4, count))))


def build_report(capture: drawdiag.Capture) -> str:
    """The whole report: the fog buffers' decoded rows, then the shading rows the dump carried."""

    folder = Path(capture.folder)
    out = [f"# fog-state report: {folder.name}", ""]
    counts: list[str] = []
    if capture.cbdump is None:
        out.append("cbdump.csv is missing: the fog rows have no samples to decode.")
    else:
        rows = {name: [sample for sample in capture.cbdump.rows
                       if drawdiag.base_texture_name(sample.name) == name]
                for name in FOG_PARAMS}
        counts = [f"{name} {len(payload)} sample(s)" for name, payload in rows.items() if payload]
        if not counts:
            out.append("no sample of any fog name landed in cbdump.csv: a capture from a build that did not keep the fog")
            out.append("post's own shader or list its buffers (neither the fixture list nor the dump list held them).  The")
            out.append("reader stays valid: run it again on a capture from a build with the names landed.")
    out.append("fog cbdump samples: " + (", ".join(counts) if counts else "none"))
    out.append("")

    for name in FOG_PARAMS:
        out.append(f"## {name}")
        samples = [] if capture.cbdump is None else [
            sample for sample in capture.cbdump.rows if drawdiag.base_texture_name(sample.name) == name]
        if not samples:
            out.append("no sample: nothing to decode (the run's capture gap, or the buffer was bound but not sampled).")
            out.append("")
            continue
        layout = cblayouts.LAYOUTS[name]
        order = [member for member in layout.members if member.name in FOG_ORDER[name]]
        decoded = [cblayouts.decode(name, sample.data) for sample in samples]
        for index, (sample, row) in enumerate(zip(samples, decoded), start=1):
            seconds = f"{sample.time_s:.3f}" if sample.time_s is not None else "-"
            out.append(f"sample {index} (frame {sample.frame}, {seconds}s, shader {sample.ps_crc_text}, "
                       f"slot {sample.slot}, {len(sample.data)} of {sample.bytes if sample.bytes is not None else '-'} B, "
                       f"pointer {hex(sample.pointer) if sample.pointer else '-'})")
            for member in order:
                value = row["members"].get(member.name)
                out.append(f"  {member.name} = {_row(value) if value is not None else 'unread'}"
                           + (f"  # {member.note}" if member.note and value is not None else ""))
            if row["short"]:
                out.append(f"  members the buffer did not reach: {', '.join(row['short'])}")
        # The min/max over the samples, in the member order the report listed.
        component_order = []
        for member in order:
            positions = member.size // 4
            if positions == 1:
                component_order.append(member.name)
            else:
                for position in range(positions):
                    component_order.append(f"{member.name}[{position}]")
        shown: list[tuple[str, float]] = []
        for component in component_order:
            values = [value for row in decoded for value in [cblayouts.components(row).get(component)]
                      if isinstance(value, (int, float))]
            if len({round(v, 6) for v in values}) > 1:
                shown.append((component, max(values)))
        out.append("members that moved over the samples: "
                   + (", ".join(f"{name} (max {value:+.6g})" for name, value in shown) if shown else "none"))
        out.append("")

    out.append("## dump/shadertypes.bin (the composite's structured g_ShaderTypeParameter)")
    facts_path = folder / "dump" / drawdiag.DUMP_FACTS_FILE
    data_path = folder / "dump" / SHADING_FILE
    if not facts_path.is_file():
        out.append("the capture's dump/facts.txt is missing: the one-frame dump did not run.")
    else:
        facts = drawdiag.load_dump_facts(str(facts_path))
        fact = next((entry for entry in facts.facts if entry.file == SHADING_FILE), None)
        if fact is None:
            skipped = next((reason for name, reason in facts.skipped if name == SHADING_FILE), None)
            out.append("the dump did not carry " + SHADING_FILE + ": "
                       + (f"it was skipped ({skipped})." if skipped
                          else "this build predates the structured-SRV dump, or the bound refused it."))
        elif not data_path.is_file():
            out.append(f"facts.txt names {SHADING_FILE} but the file is missing on disk.")
        else:
            out.append(", ".join([f"format {fact.format}", f"rows {fact.width}x{fact.height}",
                                  f"bytes {fact.bytes}", f"row pitch {fact.row_pitch}",
                                  f"draw ordinal {fact.ordinal}",
                                  f"pointer {hex(fact.pointer) if fact.pointer else '-'}"]))
            data = data_path.read_bytes()
            layout = cblayouts.LAYOUTS[SHADING_LAYOUT]
            row_count = (len(data) // layout.size) if layout.size else 0
            order = [member for member in layout.members if member.name in SHADING_ORDER]
            shown = min(row_count, SHADING_MAX_ROWS_SHOWN)
            out.append(f"{row_count} row(s) in the file, {shown} shown"
                       + (f" (the rest counted, not shown: {SHADING_MAX_ROWS_SHOWN} cap)" if shown < row_count else ""))
            for index in range(shown):
                row = cblayouts.decode(SHADING_LAYOUT, data[index * layout.size:(index + 1) * layout.size])
                members = row["members"]
                shape = ",".join(
                    f"{member.name}={_row(members[member.name]) if member.name in members else 'unread'}"
                    for member in order)
                out.append(f"row {index}: {shape}")
            rows_shown = []
            for index in range(shown):
                row = cblayouts.decode(SHADING_LAYOUT, data[index * layout.size:(index + 1) * layout.size])
                members = row["members"]
                subsurface = (members.get("lightingType") == 1
                              or (isinstance(members.get("useSubSurfaceRate"), float) and members["useSubSurfaceRate"] != 0.0))
                hair = (isinstance(members.get("furLength"), float) and members["furLength"] != 0.0)
                if subsurface or hair:
                    kind = "subsurface" if subsurface else "hair"
                    rows_shown.append(f"{index} ({kind})")
            out.append("rows that name a subsurface or a hair shape: "
                       + (", ".join(rows_shown) if rows_shown else "none among the shown rows")
                       + "  # the row a pixel's gbuffer0.a names is the one the composite loads")
    out.append("")
    _fog_texture_section(out, folder)
    out.append("")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    folders = [argument for argument in arguments if not argument.startswith("--")]
    destination = None
    for index, argument in enumerate(arguments):
        if argument == "--out" and index + 1 < len(arguments):
            destination = arguments[index + 1]
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
        Path(destination).write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
