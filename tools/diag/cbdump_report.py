"""Report the named constant buffers a capture sampled, member by member and over time.

For every buffer name in ``cbdump.csv`` the report shows the decoded values of each sample, the minimum, maximum and
delta of every member across the samples (which members move while the capture runs is the cheapest evidence of a knob
that follows weather or time of day), and for the ambient probe array the per-probe ``skyVisibility``,
``skyVisibilityScale``, ``envSpaceType`` and ``priority`` with a one-line verdict on the engine's own multiplier, counted
over the probes the header's ``paramNum`` says the engine fills.

For the wetness buffer it also joins the samples with the frame's draws: whether one buffer pointer carries the wetness
of every ground draw, or each draw swaps its own.  That join needs format 3; a format-2 capture has no pointer column
and the report says so instead of guessing.

    python -I cbdump_report.py <capture-folder> [--out report.txt] [--layouts] [--max-samples N]

Exit code 0 when the report was produced (an empty or format-2 capture is still a produced report), 2 when the folder has
no ``cbdump.csv`` at all and the report would be empty.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from typing import Any, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cblayouts  # noqa: E402  (after the path line: the tools run with `python -I`, which drops the script's folder)
import drawdiag  # noqa: E402

WETNESS = "g_WetnessParameter"
AMBIENT = "g_AmbientParamArray"
# A relative tolerance, so that a float that only echoes its own last bit does not read as "this member moves".
CHANGE_EPS = 1e-9
# "skyVisibilityScale is about 1.0": one part in a thousand, which is well below a slider's smallest step.
UNIT_TOLERANCE = 1e-3


# ---------------------------------------------------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------------------------------------------------


def fmt(value: Any) -> str:
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if value != value:
            return "nan"
        if value in (float("inf"), float("-inf")):
            return "inf" if value > 0 else "-inf"
        return f"{value:.6g}"
    if isinstance(value, (list, tuple)):
        return "[" + " ".join(fmt(item) for item in value) + "]"
    return str(value)


def _table(header: Sequence[str], rows: Sequence[Sequence[str]]) -> str:
    """A left-aligned fixed-width table (no dependency, and it survives a copy into a report)."""

    widths = [len(str(cell)) for cell in header]
    for row in rows:
        for index, cell in enumerate(row):
            if index < len(widths):
                widths[index] = max(widths[index], len(str(cell)))
    lines = ["  ".join(str(cell).ljust(widths[index]) for index, cell in enumerate(header)).rstrip()]
    lines.append("  ".join("-" * width for width in widths))
    for row in rows:
        lines.append("  ".join(str(cell).ljust(widths[index]) if index < len(widths) - 1 else str(cell)
                               for index, cell in enumerate(row)).rstrip())
    return "\n".join(lines)


def _changes(a: float | int, b: float | int) -> bool:
    scale = max(1.0, abs(float(a)), abs(float(b)))
    return abs(float(a) - float(b)) > CHANGE_EPS * scale


# ---------------------------------------------------------------------------------------------------------------------
# Which probes a sample fills
# ---------------------------------------------------------------------------------------------------------------------


def active_probes(decoded: dict[str, Any]) -> tuple[list[dict[str, Any]], int | None]:
    """The probes a sample says it fills: ``(probes, paramNum)``.

    The header's ``paramNum`` is the number of probes the engine fills.  When it decodes as an unsigned count between 1
    and the number of records the buffer holds, only those records are part of the frame: the rest of the 64 records is
    padding, and a verdict or a range that counted it would describe bytes nobody wrote.  Anything else (0, a value past
    the array, or a reading that is really a float's bit pattern) is reported as it decodes and every decoded record is
    counted, because refusing to answer on an odd count would hide the values that are there.  ``paramNum`` is ``None``
    only when the member did not decode at all.
    """

    probes = list(decoded.get("probes", []))
    declared = decoded.get("members", {}).get("paramNum")
    if isinstance(declared, int) and not isinstance(declared, bool):
        if 1 <= declared <= len(probes):
            return probes[:declared], declared
        return probes, None
    return probes, None


def _param_num_note(decoded: dict[str, Any], counted: int) -> str:
    """How many of the decoded probes the sample counted, and why that is or is not all of them."""

    available = len(decoded.get("probes", []))
    declared = decoded.get("members", {}).get("paramNum")
    if isinstance(declared, int) and not isinstance(declared, bool) and 1 <= declared <= available:
        return f"; paramNum {declared} of {available} decoded probe(s) counted"
    return (f"; paramNum reads {fmt(declared)}, which is not a count in 1..{available}: every decoded probe is counted"
            if available else "")


# ---------------------------------------------------------------------------------------------------------------------
# The per-buffer analysis
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class MemberStat:
    """One number of a buffer across the samples: its range and whether it moved."""

    name: str
    minimum: float
    maximum: float
    first: float
    last: float
    changed: bool
    missing: int = 0

    @property
    def delta(self) -> float:
        return self.maximum - self.minimum


@dataclass
class BufferReport:
    """Everything the report says about one sampled buffer name."""

    name: str
    known: bool
    declared: int | None
    bound: int | None
    note: str
    samples: int = 0
    short_rows: int = 0
    pointers: list[int] = field(default_factory=list)
    sample_rows: list[list[str]] = field(default_factory=list)
    sample_header: list[str] = field(default_factory=list)
    stats: list[MemberStat] = field(default_factory=list)
    probe_rows: list[list[str]] = field(default_factory=list)
    probe_header: list[str] = field(default_factory=list)
    verdicts: list[str] = field(default_factory=list)
    extra_lines: list[str] = field(default_factory=list)

    @property
    def changing(self) -> list[str]:
        return [stat.name for stat in self.stats if stat.changed]

    @property
    def pointer_constant(self) -> bool | None:
        """``True``/``False`` when the samples carry pointers, ``None`` when the file has no pointer column."""

        if not self.pointers:
            return None
        return len(self.pointers) == 1


def _member_stats(decoded: Sequence[dict[str, Any]]) -> list[MemberStat]:
    """Per-component min/max/delta over the samples, in the order the members appear in the layout."""

    order: list[str] = []
    values: dict[str, list[float]] = {}
    for sample in decoded:
        for key, value in cblayouts.components(sample).items():
            if key not in values:
                values[key] = []
                order.append(key)
            values[key].append(float(value))
    total = len(decoded)
    stats = []
    for key in order:
        series = values[key]
        stats.append(MemberStat(
            name=key,
            minimum=min(series),
            maximum=max(series),
            first=series[0],
            last=series[-1],
            changed=any(_changes(series[0], value) for value in series[1:]),
            missing=total - len(series),
        ))
    return stats


def _sample_table(samples: Sequence[drawdiag.CbDumpSample], decoded: Sequence[dict[str, Any]],
                  max_columns: int = 12) -> tuple[list[str], list[list[str]]]:
    """The per-sample rows: the sample's own fields, then the scalar members (vectors are left to the stats table)."""

    scalars = [cblayouts.scalars(sample) for sample in decoded]
    keys: list[str] = []
    for row in scalars:
        for key in row:
            if key not in keys:
                keys.append(key)
    keys = keys[:max_columns]
    header = ["#", "frame", "time_s", "slot", "ptr", "bytes"] + keys
    rows = []
    for index, (sample, row) in enumerate(zip(samples, scalars), start=1):
        rows.append([str(index), str(sample.frame), fmt(sample.time_s), fmt(sample.slot),
                     drawdiag.hex_pointer(sample.pointer), fmt(len(sample.data))] +
                    [fmt(row.get(key)) for key in keys])
    return header, rows


def _flat_table(samples: Sequence[drawdiag.CbDumpSample], decoded: Sequence[dict[str, Any]],
                max_floats: int = 8) -> tuple[list[str], list[list[str]]]:
    """The per-sample rows of a flat buffer: its first floats, and a count of the rest."""

    width = 0
    for sample in decoded:
        width = max(width, min(len(sample["floats"]), max_floats))
    header = ["#", "frame", "time_s", "ptr"] + [f"f{index}" for index in range(width)]
    if any(len(sample["floats"]) > max_floats for sample in decoded):
        header.append("more")
    rows = []
    for index, (sample, row) in enumerate(zip(samples, decoded), start=1):
        values = row["floats"]
        line = [str(index), str(sample.frame), fmt(sample.time_s), drawdiag.hex_pointer(sample.pointer)]
        line += [fmt(value) for value in values[:width]]
        if "more" in header:
            line.append(str(max(0, len(values) - max_floats)))
        rows.append(line)
    return header, rows


def _ambient_report(samples: Sequence[drawdiag.CbDumpSample], decoded: Sequence[dict[str, Any]],
                    report: BufferReport) -> None:
    """The header per sample and the per-probe table of the probe array, with the unit verdict."""

    report.extra_lines.append("probes per sample: "
                             + ", ".join(f"{row.get('probe_available')} decoded, paramNum {fmt(row.get('members', {}).get('paramNum'))}"
                                          for row in decoded))

    for index, (sample, row) in enumerate(zip(samples, decoded), start=1):
        probes, _declared = active_probes(row)
        unit = [probe for probe in probes
                if isinstance(probe.get("members", {}).get("skyVisibilityScale"), float) and
                abs(float(probe["members"]["skyVisibilityScale"]) - 1.0) <= UNIT_TOLERANCE]
        outside = [probe["index"] for probe in probes if probe not in unit]
        report.verdicts.append(
            f"sample {index} (frame {sample.frame}): skyVisibilityScale ~= 1.0 in {len(unit)} of {len(probes)} "
            f"probe(s)" + (f"; not unit at probe {outside[0]}..{outside[-1]}" if outside else "")
            + _param_num_note(row, len(probes)))
    # The per-probe table: one per sample while the capture is short enough to read, the last one otherwise.
    probe_header = ["probe", "skyVisibility", "skyVisibilityScale", "envSpaceType", "priority"]
    with_probes = [(index, row) for index, row in enumerate(decoded) if row.get("probes")]
    shown = with_probes if len(with_probes) <= 3 else with_probes[-1:]
    for index, row in shown:
        report.extra_lines.append(f"per-probe table (sample {index + 1}, frame {samples[index].frame}):")
        probe_rows = []
        for probe in row["probes"]:
            members = probe.get("members", {})
            probe_rows.append([str(probe["index"])] + [fmt(members.get(key)) for key in probe_header[1:]])
        report.extra_lines.append(_table(probe_header, probe_rows))
        # The range is over the probes the sample says it fills, not over the padding behind them.
        counted, _declared = active_probes(row)
        ranges = []
        for key in probe_header[1:]:
            values = [float(probe["members"][key]) for probe in counted
                      if isinstance(probe.get("members", {}).get(key), (int, float))]
            if values:
                ranges.append(f"{key} {fmt(min(values))}..{fmt(max(values))}")
        if ranges:
            report.extra_lines.append(f"range over the probes ({len(counted)} of {len(row['probes'])} decoded "
                                      f"probe(s)): " + ", ".join(ranges))
        if row.get("probe_short"):
            report.extra_lines.append(f"probes the buffer did not reach fully: {row['probe_short']}")


def analyse_buffer(name: str, samples: Sequence[drawdiag.CbDumpSample]) -> BufferReport:
    """Decode every sample of one buffer name and reduce it to the report's tables and statistics."""

    decoded = [cblayouts.decode(name, sample.data) for sample in samples]
    first = decoded[0] if decoded else cblayouts.decode(name, b"")
    report = BufferReport(
        name=first.get("name", name),
        known=bool(first.get("known")),
        declared=first.get("declared"),
        bound=first.get("bound"),
        note=first.get("note", ""),
        samples=len(samples),
        short_rows=sum(1 for sample in samples if sample.short),
    )
    seen: list[int] = []
    for sample in samples:
        if sample.pointer is not None and sample.pointer not in seen:
            seen.append(sample.pointer)
    report.pointers = seen
    report.stats = _member_stats(decoded)
    if first.get("flat") and not report.stats:
        report.sample_header, report.sample_rows = _flat_table(samples, decoded)
        # A flat buffer's min/max is computed over its components so the report still says which float moves.
        component_rows = [{"members": {f"f{index}": value for index, value in enumerate(row["floats"])}}
                          for row in decoded]
        report.stats = _member_stats(component_rows)
    else:
        report.sample_header, report.sample_rows = _sample_table(samples, decoded)
    if cblayouts.base_name(name) == AMBIENT:
        _ambient_report(samples, decoded, report)
    return report


# ---------------------------------------------------------------------------------------------------------------------
# The wetness pointer join with the frame's draws
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class WetnessDraws:
    """What the draws of the capture say about the wetness buffer: how many rows bind it and with which pointers."""

    rows: int = 0
    joined: int = 0 # rows that carried no name and were named through the capture's shader fixtures by CRC
    slots: list[int] = field(default_factory=list)
    pointers: list[int] = field(default_factory=list)
    passes: list[drawdiag.Draw] = field(default_factory=list)


def wetness_draws(draws: drawdiag.Draws | None, capture: drawdiag.Capture | None = None) -> WetnessDraws:
    """The draws that bind the wetness parameter.

    A row that carries no name of its own (its shader was created before any capture could arm, so the diagnostic never had its
    reflection) is named through the capture's ``shaders.txt`` by CRC when the capture is given: the fixture says which slot the
    name is declared at, and the row's unnamed buffer at that slot is the binding. The same join names the glare chain in the
    session answers.
    """

    result = WetnessDraws()
    if draws is None:
        return result
    for row in draws.rows:
        entry = row.cb(WETNESS)
        if entry is None and capture is not None and capture.shaders is not None:
            point = capture.shaders.bind_point_for_crc(row.ps_crc, WETNESS)
            if point is not None:
                entry = next((cb for cb in row.cbs if cb.slot == point and not cb.name), None)
                if entry is not None:
                    result.joined += 1
        if entry is None:
            continue
        result.rows += 1
        if entry.slot is not None and entry.slot not in result.slots:
            result.slots.append(entry.slot)
        if entry.pointer is not None and entry.pointer not in result.pointers:
            result.pointers.append(entry.pointer)
        # The fullscreen pass is the row whose shader reads the G-buffer *and* the view position and writes one target:
        # a draw that reads only one of the two is a different pass that happens to bind the same buffer.
        if row.binds_all(("g_SamplerGBuffer3", "g_SamplerViewPosition")) and row.target_count == 1:
            result.passes.append(row)
    return result


def wetness_section(capture: drawdiag.Capture, report: BufferReport | None) -> list[str]:
    """The wetness part of the report: the 8 floats per sample and the pointer question of the frame's draws."""

    lines: list[str] = []
    if report is not None:
        lines.append(_table(report.sample_header, report.sample_rows))
        lines.append("")
        if report.pointer_constant is None:
            lines.append("pointer across the samples: the rows carry none (written before format 3)")
        elif report.pointer_constant:
            lines.append(f"pointer across the samples: constant ({drawdiag.hex_pointer(report.pointers[0])}) in "
                         f"{report.samples} sample(s)")
        else:
            lines.append("pointer across the samples: varies - "
                         + ", ".join(drawdiag.hex_pointer(pointer) for pointer in report.pointers))
        if report.short_rows:
            lines.append(f"{report.short_rows} sample(s) carry fewer bytes than the buffer is bound at")
    else:
        lines.append("no cbdump.csv sample of this buffer in the capture")
    lines.append("")
    joins = wetness_draws(capture.draws, capture)
    lines.append(f"draws that bind it (draws.csv): {joins.rows} row(s), slot(s) "
                 f"{', '.join(str(slot) for slot in joins.slots) if joins.slots else '-'}")
    if joins.pointers:
        if len(joins.pointers) == 1:
            lines.append(f"pointer across the frame's draws: constant ({drawdiag.hex_pointer(joins.pointers[0])})")
        else:
            lines.append(f"pointer across the frame's draws: {len(joins.pointers)} distinct pointer(s): "
                         + ", ".join(drawdiag.hex_pointer(pointer) for pointer in joins.pointers[:8])
                         + (" ..." if len(joins.pointers) > 8 else ""))
    else:
        lines.append("pointer across the frame's draws: the capture has no pointer column (format 2); the "
                     "one-buffer-per-frame question needs format 3")
    if joins.passes:
        frames = sorted({row.frame for row in joins.passes})
        lines.append(f"fullscreen wetness pass(es): {len(joins.passes)} row(s) in frame(s) {frames}, "
                     f"CRC(s) {', '.join(sorted({row.crc_text for row in joins.passes}))}")
    else:
        lines.append("fullscreen wetness pass(es): none found by its bindings (g_WetnessParameter + "
                     "g_SamplerGBuffer3 + g_SamplerViewPosition, one render target)")
    return lines


# ---------------------------------------------------------------------------------------------------------------------
# The whole capture
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class CbReport:
    """The report of one capture."""

    capture: drawdiag.Capture
    buffers: list[BufferReport] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def buffer(self, name: str) -> BufferReport | None:
        wanted = cblayouts.base_name(name)
        for report in self.buffers:
            if cblayouts.base_name(report.name) == wanted:
                return report
        return None


def analyse_capture(capture: drawdiag.Capture, max_samples: int = 200) -> CbReport:
    report = CbReport(capture=capture)
    report.warnings = capture.warnings()
    if capture.cbdump is None:
        return report
    series: dict[str, list[drawdiag.CbDumpSample]] = {}
    order: list[str] = []
    for sample in capture.cbdump.rows:
        key = cblayouts.base_name(sample.name)
        if key not in series:
            series[key] = []
            order.append(key)
        if len(series[key]) < max_samples:
            series[key].append(sample)
    # The known names first, in the diagnostic's own order, then anything else the build sampled.
    known = [name for name in cblayouts.DUMP_NAMES if name in series]
    unknown = [name for name in order if name not in cblayouts.DUMP_NAMES]
    for name in known + unknown:
        report.buffers.append(analyse_buffer(name, series[name]))
    return report


def render(report: CbReport) -> str:
    capture = report.capture
    lines = [f"capture: {capture.folder}"]
    if capture.draws is not None:
        lines.append(f"draws.csv: {capture.draws.summary()}")
    else:
        lines.append("draws.csv: not present")
    if capture.cbdump is not None:
        rows = len(capture.cbdump.rows)
        names = capture.cbdump.names()
        lines.append(f"cbdump.csv: {rows} sample(s) of {len(names)} buffer name(s): {', '.join(names)}")
        if capture.cbdump.truncated:
            lines.append("cbdump.csv: the writer's byte cap was reached; later samples are missing")
    else:
        lines.append("cbdump.csv: not present (the capture predates the per-sample constant-buffer dump, or the "
                     "buffer dump was off)")
    for warning in report.warnings:
        lines.append(f"warning: {warning}")
    if not report.buffers:
        lines.append("")
        lines.append("nothing to decode: no named constant buffer was sampled")
        return "\n".join(lines) + "\n"

    for buffer in report.buffers:
        lines.append("")
        title = f"{buffer.name}  ({buffer.samples} sample(s)"
        if buffer.declared is not None:
            title += f", {buffer.declared} B declared"
        if buffer.bound and buffer.bound != buffer.declared:
            title += f", {buffer.bound} B bound"
        title += ")"
        lines.append("=" * min(len(title), 100))
        lines.append(title)
        lines.append("=" * min(len(title), 100))
        if buffer.note:
            lines.append(buffer.note)
        if not buffer.known:
            lines.append("no member table for this name: the floats are shown as they are, byte order")
        if buffer.short_rows:
            lines.append(f"{buffer.short_rows} sample(s) carry fewer bytes than the buffer's bound size; the "
                         f"missing members are listed as - where they matter")
        lines.append("")
        if cblayouts.base_name(buffer.name) == WETNESS:
            lines.extend(wetness_section(capture, buffer))
        else:
            if buffer.sample_rows:
                lines.append(_table(buffer.sample_header, buffer.sample_rows))
            for extra in buffer.extra_lines:
                lines.append("")
                lines.append(extra)
        if buffer.verdicts:
            lines.append("")
            for verdict in buffer.verdicts:
                lines.append("verdict: " + verdict)
        lines.append("")
        lines.append("per-member range over the samples:")
        stats = [stat for stat in buffer.stats if stat.changed]
        lines.append(f"changing members: {', '.join(stat.name for stat in stats) if stats else 'none'}")
        shown = [stat for stat in buffer.stats if stat.changed] or buffer.stats
        rows = [[stat.name, fmt(stat.minimum), fmt(stat.maximum), fmt(stat.delta), fmt(stat.first), fmt(stat.last),
                 fmt(stat.missing)] for stat in shown]
        lines.append(_table(["member", "min", "max", "delta", "first", "last", "missing"], rows))
        if buffer.pointer_constant is True:
            lines.append(f"pointer: constant ({drawdiag.hex_pointer(buffer.pointers[0])}) across the samples")
        elif buffer.pointer_constant is False:
            lines.append(f"pointer: {len(buffer.pointers)} distinct pointer(s) across the samples: "
                         + ", ".join(drawdiag.hex_pointer(pointer) for pointer in buffer.pointers[:8]))
    lines.append("")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments or arguments[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if arguments else 2
    if arguments[0] == "--layouts":
        print("\n".join(cblayouts.describe()))
        return 0
    folder = arguments[0]
    out = None
    max_samples = 200
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--out" and index + 1 < len(arguments):
            out = arguments[index + 1]
            index += 2
        elif argument == "--max-samples" and index + 1 < len(arguments):
            try:
                max_samples = max(1, int(arguments[index + 1]))
            except ValueError:
                print("--max-samples needs an integer", file=sys.stderr)
                return 2
            index += 2
        else:
            print(f"unknown argument: {argument}", file=sys.stderr)
            return 2
    capture = drawdiag.load_capture(folder)
    if capture.draws is None and capture.notices:
        print(f"{folder} is not a capture folder: {capture.notices[0].text}", file=sys.stderr)
        return 2
    report = analyse_capture(capture, max_samples=max_samples)
    text = render(report)
    sys.stdout.write(text)
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    return 0 if capture.cbdump is not None and capture.cbdump.rows else 2


if __name__ == "__main__":
    sys.exit(main())
