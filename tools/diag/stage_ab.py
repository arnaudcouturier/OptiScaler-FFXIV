"""Before / after of every stage of this fork on the one-frame dump: what each stage did to the capture frame, measured and drawn.

The dump (``DrawDiagnosticsDump = frame``, one F9 press) writes, next to its textures, the picture each stage received and the
picture it produced, cropped to the stage's render window in the texture's own format: ``stage-<NN>-<name>-in.bin`` and
``stage-<NN>-<name>-out.bin``.  A stage whose input is the stage before's output has no second file: ``facts.txt`` names the
file it is again (``same_as=``).  ``facts.txt`` also has one ``# stage NN name:`` line for EVERY stage - ``ran``, ``held``,
``combined``, ``declined: <reason>``, ``not armed`` or ``not reached this frame`` - so a stage that did nothing still says why.

For each stage that has both pictures this tool writes

* a PNG triptych, ``stage-ab\\stage-<NN>-<name>.png``: input | output | the amplified change (red where the output is brighter,
  blue where it is darker; ``--amplify`` times the luminance change, 8 by default), the two pictures with the same tone-map
  preview ``dump_to_png`` uses (``--exposure``, 1 by default);
* its entry in ``stage-ab\\stage-ab.json``, with the stage's status line and these numbers over the window's pixels:

  - ``mean_abs_change`` and ``p95_abs_change``: the absolute change of a pixel is the largest change of its colour channels, in
    the stored value (a byte / 255 for an 8-bit texture, the float itself for an FP16 one);
  - ``share_gt_1_255`` and ``share_gt_4_255``: the share of pixels whose change is more than 1/255 and more than 4/255;
  - ``luma_ratio``: mean luminance of the output over the input (the veil: above 1 the stage lifted the picture, below 1 it
    darkened it); a single-channel texture is its own luminance;
  - ``hf_ratio``: the energy of the luminance's 4-neighbour Laplacian, output over input (the blur: well below 1 the stage
    smoothed the picture, above 1 it added detail or noise);
  - ``blotch_mean`` / ``blotch_p95``: the local standard deviation of the luminance change over the FLAT 16x16 tiles of the
    input (a tile whose input luminance varies by at most 2 % of its mean, or 1/255).  A stage that changes flat areas by a
    constant has a blotch of 0; a stage that leaves patches of different strength in a flat sky has a large one.

The numbers are about the stage's own pictures: they say what the stage did to the frame, not whether it looks right.

    python -I stage_ab.py <capture-folder or its dump folder> [--out DIR] [--exposure X] [--amplify N]
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Any, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import drawdiag  # noqa: E402
import dump_to_png  # noqa: E402

STAGE_FOLDER = "stage-ab"
JSON_NAME = "stage-ab.json"
TILE = 16
LEVEL_1 = 1.0 / 255.0
LEVEL_4 = 4.0 / 255.0
FLAT_RELATIVE = 0.02

_STAGE_LINE = re.compile(r"^#\s*stage\s+(\d\d)\s+(\S+):\s*(.*)$")
_IMAGE_LINE = re.compile(r"^#\s*stage-image\s+(\S+)\s+(.*)$")
_FILE_NAME = re.compile(r"^stage-(\d\d)-(.+)-(in|out)$")


# ---------------------------------------------------------------------------------------------------------------------
# facts.txt: the stage lines and the image comments
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class StageLine:
    number: int
    name: str
    status: str  # ran | held | combined | declined | not armed | not reached
    text: str  # the status words, with the reason
    position: int | None = None  # the frame position (draw ordinal) of the stage's first report
    images: str = ""
    values: str = ""


@dataclass
class ImageNote:
    name: str  # stage-06-shafts-out (no extension)
    same_as: str | None = None
    fields: dict[str, str] = field(default_factory=dict)


@dataclass
class StageFacts:
    path: str
    stages: dict[int, StageLine] = field(default_factory=dict)
    images: dict[str, ImageNote] = field(default_factory=dict)
    skipped: dict[str, str] = field(default_factory=dict)  # stage image file stem -> reason


def _status_of(text: str) -> str:
    for word, status in (("ran", "ran"), ("held", "held"), ("combined", "combined"), ("declined", "declined"),
                         ("not armed", "not armed"), ("not reached", "not reached")):
        if text.startswith(word):
            return status
    return "unknown"


def parse_stage_facts(path: str) -> StageFacts:
    """The ``# stage`` and ``# stage-image`` comment lines (and the ``# skipped`` lines of stage files) of a facts.txt."""

    facts = StageFacts(path=path)
    if not os.path.isfile(path):
        return facts
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            text = line.strip()
            match = _IMAGE_LINE.match(text)
            if match:
                note = ImageNote(name=match.group(1))
                for token in match.group(2).split():
                    key, _, value = token.partition("=")
                    if key == "same_as":
                        note.same_as = value
                    elif value:
                        note.fields[key] = value
                facts.images[note.name] = note
                continue
            match = _STAGE_LINE.match(text)
            if match:
                parts = [part.strip() for part in match.group(3).split(" | ")]
                stage = StageLine(number=int(match.group(1)), name=match.group(2), status=_status_of(parts[0]), text=parts[0])
                for part in parts[1:]:
                    if part.startswith("frame position "):
                        stage.position = int(part[len("frame position "):])
                    elif part.startswith("images "):
                        stage.images = part[len("images "):]
                    elif part.startswith("values "):
                        stage.values = part[len("values "):]
                facts.stages[stage.number] = stage
                continue
            skipped = re.match(r"^#\s*skipped\s+(stage-\S+?)\.bin:\s*(.*)$", text)
            if skipped:
                facts.skipped[skipped.group(1)] = skipped.group(2)
    return facts


# ---------------------------------------------------------------------------------------------------------------------
# Pictures and the numbers over them
# ---------------------------------------------------------------------------------------------------------------------


def _colour_planes(decoded: dump_to_png.Decoded) -> np.ndarray:
    """The decoded texture as float32 (h, w, c): red, green, blue (alpha dropped), or the one channel of a mask.

    The stored value: a byte / 255 for the 8-bit kinds, the float for the float kinds, value / 65535 for 16-bit UNORM."""

    values = decoded.values
    kind = decoded.fmt.kind
    if kind == "r10":  # R10G10B10A2: three 10-bit channels in one 32-bit word
        word = values.astype(np.uint32)
        return np.ascontiguousarray(np.stack([(word & 0x3FF), ((word >> 10) & 0x3FF), ((word >> 20) & 0x3FF)], axis=2).astype(np.float32) / 1023.0)
    if values.ndim == 2:
        values = values[:, :, None]
    array = values.astype(np.float32)
    if kind == "u8":
        array = array / 255.0
    elif kind in ("unorm16", "d16"):
        array = array / 65535.0
    elif kind == "d24s8":
        array = array / 16777215.0
    order = list(decoded.fmt.order)
    if array.shape[2] >= 3:
        wanted = [order.index(c) if c in order else i for i, c in enumerate(("r", "g", "b"))]
        array = array[:, :, wanted]
    else:
        array = array[:, :, :1]
    return np.ascontiguousarray(array)


def _luminance(array: np.ndarray) -> np.ndarray:
    if array.shape[2] >= 3:
        return array[:, :, 0] * 0.2126 + array[:, :, 1] * 0.7152 + array[:, :, 2] * 0.0722
    return array[:, :, 0]


def _laplacian_energy(luma: np.ndarray) -> float:
    if luma.shape[0] < 3 or luma.shape[1] < 3:
        return 0.0
    centre = luma[1:-1, 1:-1]
    lap = 4.0 * centre - luma[:-2, 1:-1] - luma[2:, 1:-1] - luma[1:-1, :-2] - luma[1:-1, 2:]
    return float(np.mean(lap.astype(np.float64) ** 2))


def _ratio(numerator: float, denominator: float) -> float | None:
    if not np.isfinite(numerator) or not np.isfinite(denominator) or abs(denominator) < 1e-12:
        return None
    return numerator / denominator


def _blotch(luma_in: np.ndarray, change: np.ndarray) -> tuple[float | None, float | None, float, int]:
    """The local standard deviation of the change over the flat tiles of the input: (mean, p95, flat share, flat count)."""

    height, width = luma_in.shape
    rows, columns = height // TILE, width // TILE
    if rows == 0 or columns == 0:
        return None, None, 0.0, 0
    tiles_in = luma_in[:rows * TILE, :columns * TILE].reshape(rows, TILE, columns, TILE).transpose(0, 2, 1, 3).reshape(rows, columns, -1)
    tiles_change = change[:rows * TILE, :columns * TILE].reshape(rows, TILE, columns, TILE).transpose(0, 2, 1, 3).reshape(rows, columns, -1)
    mean_in = tiles_in.mean(axis=2)
    std_in = tiles_in.std(axis=2)
    flat = std_in <= np.maximum(FLAT_RELATIVE * np.abs(mean_in), LEVEL_1)
    count = int(flat.sum())
    if count == 0:
        return None, None, 0.0, 0
    spread = tiles_change.std(axis=2)[flat]
    return float(spread.mean()), float(np.percentile(spread, 95)), count / float(rows * columns), count


def compare(before: np.ndarray, after: np.ndarray) -> dict[str, Any]:
    """The numbers of one stage from its two pictures (float32, (h, w, c), the same shape)."""

    delta = after - before
    pixel = np.abs(delta).max(axis=2)
    luma_in, luma_out = _luminance(before), _luminance(after)
    change = (luma_out - luma_in).astype(np.float32)
    blotch_mean, blotch_p95, flat_share, flat_count = _blotch(luma_in, change)
    return {
        "mean_abs_change": float(pixel.mean()),
        "p95_abs_change": float(np.percentile(pixel, 95)),
        "share_gt_1_255": float((pixel > LEVEL_1).mean()),
        "share_gt_4_255": float((pixel > LEVEL_4).mean()),
        "luma_in": float(luma_in.mean()),
        "luma_out": float(luma_out.mean()),
        "luma_ratio": _ratio(float(luma_out.mean()), float(luma_in.mean())),
        "hf_ratio": _ratio(_laplacian_energy(luma_out), _laplacian_energy(luma_in)),
        "blotch_mean": blotch_mean,
        "blotch_p95": blotch_p95,
        "flat_tile_share": flat_share,
        "flat_tiles": flat_count,
        "pixels": int(pixel.size),
    }


def _difference_panel(before: np.ndarray, after: np.ndarray, amplify: float) -> np.ndarray:
    change = (_luminance(after) - _luminance(before)) * amplify
    panel = np.zeros(change.shape + (3,), dtype=np.uint8)
    panel[:, :, 0] = np.clip(np.maximum(change, 0.0), 0.0, 1.0) * 255.0 + 0.5
    panel[:, :, 2] = np.clip(np.maximum(-change, 0.0), 0.0, 1.0) * 255.0 + 0.5
    return panel


def triptych(decoded_in: dump_to_png.Decoded, decoded_out: dump_to_png.Decoded, before: np.ndarray, after: np.ndarray, exposure: float,
             amplify: float) -> np.ndarray:
    """Input | output | amplified change, side by side with a 4-pixel white gap."""

    left = dump_to_png.preview_rgb(decoded_in, exposure)
    right = dump_to_png.preview_rgb(decoded_out, exposure)
    middle = _difference_panel(before, after, amplify)
    gap = np.full((left.shape[0], 4, 3), 255, dtype=np.uint8)
    return np.concatenate([left, gap, right, gap, middle], axis=1)


# ---------------------------------------------------------------------------------------------------------------------
# The capture
# ---------------------------------------------------------------------------------------------------------------------


def dump_folder_of(path: str) -> str:
    """The dump folder of a capture folder (or the folder itself when it is the dump folder)."""

    if os.path.isfile(os.path.join(path, drawdiag.DUMP_FACTS_FILE)):
        return path
    return os.path.join(path, drawdiag.DUMP_FOLDER)


def _fact_for(dump: drawdiag.DumpFacts, stem: str) -> drawdiag.DumpFact | None:
    for fact in dump.facts:
        if fact.file == stem + ".bin":
            return fact
    return None


def _load(dump: drawdiag.DumpFacts, stem: str) -> tuple[dump_to_png.Decoded | None, str | None]:
    fact = _fact_for(dump, stem)
    if fact is None:
        return None, f"{stem}.bin is not in facts.txt"
    path = dump.path_of(fact)
    if not os.path.isfile(path):
        return None, f"{stem}.bin is listed but missing"
    with open(path, "rb") as handle:
        data = handle.read()
    try:
        return dump_to_png.decode(fact, data), None
    except ValueError as error:
        return None, f"{stem}.bin: {error}"


def analyze(folder: str, out: str | None = None, exposure: float = 1.0, amplify: float = 8.0, write: bool = True) -> dict[str, Any]:
    """Analyse one capture's stage pictures; writes the triptychs and ``stage-ab.json`` unless ``write`` is false.

    Returns the document that goes into the JSON: ``{"capture", "stages": [...]}``, every stage of the dump in stage order."""

    dump_dir = dump_folder_of(folder)
    facts_path = os.path.join(dump_dir, drawdiag.DUMP_FACTS_FILE)
    dump = drawdiag.load_dump_facts(facts_path)
    stage_facts = parse_stage_facts(facts_path)
    out_dir = out or os.path.join(dump_dir, STAGE_FOLDER)
    if write:
        os.makedirs(out_dir, exist_ok=True)
    document: dict[str, Any] = {"capture": os.path.abspath(folder), "facts": os.path.abspath(facts_path), "stages": []}
    if not stage_facts.stages:
        document["note"] = "facts.txt has no '# stage' lines: this dump was written by a build without the per-stage dump"
    for number in sorted(stage_facts.stages):
        line = stage_facts.stages[number]
        stem_in = f"stage-{number:02d}-{line.name}-in"
        stem_out = f"stage-{number:02d}-{line.name}-out"
        entry: dict[str, Any] = {
            "number": number,
            "name": line.name,
            "status": line.status,
            "status_text": line.text,
            "frame_position": line.position,
            "images": line.images,
            "values": line.values,
            "metrics": None,
            "triptych": None,
        }
        notes: list[str] = []
        reference = stage_facts.images.get(stem_in)
        in_stem = reference.same_as if reference is not None and reference.same_as else stem_in
        if reference is not None and reference.same_as:
            entry["input_is"] = reference.same_as
        for stem in (stem_in, stem_out):
            if stem in stage_facts.skipped:
                notes.append(f"{stem}.bin was skipped: {stage_facts.skipped[stem]}")
        decoded_in, problem_in = _load(dump, in_stem)
        decoded_out, problem_out = _load(dump, stem_out)
        for stem, decoded, problem in ((in_stem, decoded_in, problem_in), (stem_out, decoded_out, problem_out)):
            if decoded is None and not any(stem in n for n in notes) and line.status in ("ran", "held") and not line.images.startswith("none"):
                notes.append(problem or f"{stem}: not decoded")
        if decoded_in is not None and decoded_out is not None:
            before, after = _colour_planes(decoded_in), _colour_planes(decoded_out)
            entry["format"] = decoded_out.fact.format
            entry["size"] = [decoded_out.fact.width, decoded_out.fact.height]
            if before.shape != after.shape:
                notes.append(f"the two pictures differ in shape ({before.shape} vs {after.shape}): not compared")
            else:
                entry["metrics"] = compare(before, after)
                if write:
                    image = triptych(decoded_in, decoded_out, before, after, exposure, amplify)
                    png = os.path.join(out_dir, f"stage-{number:02d}-{line.name}.png")
                    dump_to_png.write_png(png, image)
                    entry["triptych"] = os.path.relpath(png, out_dir).replace("\\", "/")
        if notes:
            entry["notes"] = notes
        document["stages"].append(entry)
    if write:
        with open(os.path.join(out_dir, JSON_NAME), "w", encoding="utf-8") as handle:
            json.dump(document, handle, indent=2)
            handle.write("\n")
    return document


def report(document: dict[str, Any]) -> str:
    """One readable table of the document: a line per stage with its status and the headline numbers."""

    lines = [f"capture {document['capture']}"]
    if "note" in document:
        lines.append(document["note"])
    for stage in document["stages"]:
        text = f"  {stage['number']:02d} {stage['name']:<16} {stage['status_text']}"
        metrics = stage.get("metrics")
        if metrics:
            def fmt(value: float | None, digits: int = 4) -> str:
                return "-" if value is None else f"{value:.{digits}f}"

            text += (f" | mean {fmt(metrics['mean_abs_change'])} p95 {fmt(metrics['p95_abs_change'])}"
                     f" >1/255 {metrics['share_gt_1_255'] * 100:.1f}% >4/255 {metrics['share_gt_4_255'] * 100:.1f}%"
                     f" | luma x{fmt(metrics['luma_ratio'])} hf x{fmt(metrics['hf_ratio'])} blotch {fmt(metrics['blotch_mean'])}")
        for note in stage.get("notes", []):
            text += f" [{note}]"
        lines.append(text)
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    folder: str | None = None
    out: str | None = None
    exposure = 1.0
    amplify = 8.0
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--out" and index + 1 < len(arguments):
            out = arguments[index + 1]
            index += 2
        elif argument == "--exposure" and index + 1 < len(arguments):
            exposure = float(arguments[index + 1])
            index += 2
        elif argument == "--amplify" and index + 1 < len(arguments):
            amplify = float(arguments[index + 1])
            index += 2
        elif argument in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        elif argument.startswith("-"):
            print(f"unknown argument: {argument}", file=sys.stderr)
            return 2
        else:
            folder = argument
            index += 1
    if folder is None:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    document = analyze(folder, out, exposure, amplify)
    print(report(document))
    return 0


if __name__ == "__main__":
    sys.exit(main())
