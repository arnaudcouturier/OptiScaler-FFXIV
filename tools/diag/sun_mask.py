"""Answer the sun/shadow mask's facts for one capture folder: what the shadow-mapping module's lever sits on.

The mask is the frame's own R8 screen-space texture, the one the sun light draw squares and multiplies (the light
term's shadow visibility). The capture carries the two halves of the answer the five-stop capture protocol needs:

- which resource is the mask, by name and ordinal: ``views.txt``'s ``g_SamplerShadowMask`` row gives the mask's
  formats, size and pointer; ``draws.csv``'s draws that bind ``g_SamplerShadowMask`` are the light draws. The atlas
  answers at ``g_SamplerShadow`` only, so the fixtures never give one pointer for both (the wetness bin is NOT the
  mask; facts.txt names them apart).
- the texels, when the one-frame dump carried it (``dump/sunmask.bin`` + ``dump/facts.txt``): the mask's min, max,
  mean, and the shares of the fully shadowed (0), the lifted middle and the fully lit (255) texels - the five-stop's
  U-a (the night texel range and the lit/shadow polarity).

    python -I sun_mask.py <capture-folder> [--out report.txt]

Exit code 0 when the report was produced (a capture missing the dump still answers from the ordinals), 2 when the
folder has no views.txt and no draws.csv at all.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import drawdiag  # noqa: E402
import dump_to_png  # noqa: E402

MASK_NAME = "g_SamplerShadowMask"


def fact_for(capture: drawdiag.Capture, name: str):
    dump = capture.dump
    if dump is None or dump.facts is None:
        return None
    for fact in dump.facts:
        if fact.file == name:
            return fact
    return None


def decode_texels(capture: drawdiag.Capture, root: Path):
    fact = fact_for(capture, "sunmask.bin")
    if fact is None:
        return None
    path = root / drawdiag.DUMP_FOLDER / "sunmask.bin"
    if not path.is_file():
        return None
    return dump_to_png.decode(fact, path.read_bytes()).values


def texel_histogram(values) -> dict:
    """Per bucket the whole mask's shares: 0 (fully shadowed), 1..254, 255 (fully lit), plus min/max/mean.

    The R8 decode answers raw mask bytes (0..255) and its consumer normalises; both shapes are read here, so a
    fixture change of the decode never silently re-buckets the shares.
    """
    counts = [0] * 256
    total = 0
    for value in values.ravel():
        f = float(value)
        level = int(round(f * 255.0)) if 0.0 <= f <= 1.0 else int(f)
        level = 0 if level < 0 else (255 if level > 255 else level)
        counts[level] += 1
        total += 1
    if total == 0:
        return {}
    zero, lit = counts[0], counts[255]
    flat = []
    for value in values.ravel():
        f = float(value)
        flat.append(f if 0.0 <= f <= 1.0 else f / 255.0)
    return {
        "texels": total,
        "share_0": zero / total,
        "share_middle": (total - zero - lit) / total,
        "share_255": lit / total,
        "min": min(flat),
        "max": max(flat),
        "mean": sum(flat) / len(flat),
    }


def build_report(capture: drawdiag.Capture, root: Path) -> str:
    lines = [f"sun mask facts for {root}"]
    if capture.draws is None and capture.views is None:
        return "\n".join(lines) + "\nno view and no draw rows: not a capture folder\n"
    view = capture.views.one(MASK_NAME) if capture.views is not None else None
    if view is not None:
        lines.append(", ".join([
            f"resource format {view.texture_format}",
            f"view format {view.view_format}",
            f"size {view.width}x{view.height}",
            f"array {view.texture_array}",
            f"first bound at ordinal {view.ordinal}",
        ]))
    binds = [d for d in (capture.draws.rows if capture.draws else []) if d.srv_names and MASK_NAME in d.srv_names]
    lines.append(f"light draws that bind {MASK_NAME}: {len(binds)}")
    if binds:
        lines.append(f"first ordinal {binds[0].ordinal}, last {binds[-1].ordinal}")
    else:
        lines.append("no draw in this capture binds the mask (or the capture missed the light draw's windows)")
    values = decode_texels(capture, root)
    if values is None:
        lines.append("texels: not written this capture (the dump did not carry sunmask.bin; the ordinals above are "
                     "the resource half of the answer)")
    else:
        h = texel_histogram(values)
        if h:
            lines.append(f"texels: {h['texels']}")
            lines.append(f"share fully shadowed (0): {h['share_0']:.4f}")
            lines.append(f"share lifted middle: {h['share_middle']:.4f}")
            lines.append(f"share fully lit (255): {h['share_255']:.4f}")
            lines.append(f"range {h['min']:.4f}..{h['max']:.4f}, mean {h['mean']:.4f}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    out_path: Path | None = None
    if "--out" in argv:
        at = argv.index("--out")
        out_path = Path(argv[at + 1])
        del argv[at:at + 2]
    if len(argv) < 1:
        print(__doc__.strip())
        return 2
    root = Path(argv[0])
    try:
        capture = drawdiag.load_capture(str(root))
    except Exception as error:
        print(f"sun_mask: load failed for {root} ({error})")
        return 2
    if capture.draws is None and capture.views is None:
        print(f"sun_mask: no capture in {root} (no draws.csv and no views.txt read)")
        return 2
    report = build_report(capture, root)
    if out_path is not None:
        out_path.write_text(report, encoding="utf-8")
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
