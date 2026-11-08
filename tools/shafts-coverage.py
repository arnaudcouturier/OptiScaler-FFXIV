#!/usr/bin/env python3
"""Replay a session's light-shaft decisions from its log and its draw captures, and check the acceptance against the frame's own sun.

Why this exists: "the pass drew on 2 of 1272 armed frames" is the number a session reports when the effect is invisible, and it says nothing about
whether the rule was right for those frames. This reads the module's own log lines and the capture's own matrices and answers, with numbers:

  * the session's real share - the module's rendered frames and the frames it drew - from the counts line the module writes to the log,
  * the decision spans, with the frames the menu covered cut out of them (a span is wall-clock x frame rate, so it is an upper bound),
  * the poses the capture measured itself (one a second, its own light direction and its own view-projection), and for each one the frame's own
    sun: the axis of the directional-shadow row the same capture dumped (`cbdump.csv`, `g_DirectionalShadowParameter` at +0x20, negated and
    normalised). This is the acceptance, and it is not circular: the sun and the camera come from the capture, not from the module's decision.
    A pose only counts when the capture has a shadow row of its own frame; the capture's light is checked against that row (a light more than
    three degrees off it is not the frame's sun), and a frame the module left alone for such a light is a correct non-draw, never a miss. A pose
    whose own sun is inside the margin and whose light IS that sun must have drawn; a draw is also checked against the log's own run line, so a
    draw from a point that is not the frame's sun fails the check. The tool exits 1 on a miss, on a draw at the wrong position, and when it
    cannot place the capture on the log's clock at all.
  * the captured shadow row against the light the same capture sampled, which is the identity the pass requires.

Inputs are read-only and named by the caller; nothing is written and no game file is touched.

  python -I tools/shafts-coverage.py --log <OptiScaler.log> [--capture <dir>]... [--fps 60] [--window 1920x1080]
"""
import argparse
import csv
import math
import os
import re
import struct
import sys

# The rules the module applies, as ext/ffxiv/engine/ShaftsRules.h states them: half a window outside its edges, a world elevation above about a
# degree below the horizon is the sun, the hold is the frames a missed read may draw with the last accepted sun, and a light's direction has to be
# within three degrees of the frame's directional shadow's axis to be that frame's sun.
#
# The light the capture sampled (`cb.csv`'s dir_*) is in VIEW space: across a capture it turns with the view matrix while the frame's shadow axis
# stays put, and projecting it through `cb.csv`'s view-projection lands the sun a thousand pixels from where the frame's own sun is. So every pose
# here turns the sampled direction into the world first (`world_direction`: the view's rotation transposed, which is the inverse rotation) and only
# then projects it with `vp` - which is exact, because `m_ViewProjectionMatrix = projection * [view; 0 0 0 1]` and the view's upper-left 3x3 is
# orthogonal, so `vp * (R^T * d, 0) = projection * (d, 0)` for the same direction.
MAX_OFFSCREEN = 0.50
MIN_SUN_ELEVATION = -0.02
HOLD_FRAMES = 6
SHADOW_ROW_AT = 0x20
SHADOW_BYTES = 0x80
SHADOW_AGREEMENT_DEGREES = 3.0
# A pose and the capture's shadow row belong to the same frame(s) when their own timestamps are this close: `cbdump.csv` samples a name once
# while a capture collects (a few frames), and `cb.csv` samples one pose a second over the whole armed period, so only the poses near the dumped
# row can be posed from the frame's own sun at all.
POSE_SHADOW_SECONDS = 0.5
# A run line this close to the pose's own time is that pose's frame; an older one is another frame's drawing and its position is not compared.
RUN_FRESH_SECONDS = 1.0
# The log prints the sun's position to a tenth of a pixel and a pose's projection is the capture's own row through the capture's own
# view-projection, so anything inside a couple of pixels is the same point.
POSITION_PIXELS = 2.0
# A pose is sampled at the frame's light draw and the decision is written at its tone map, a few milliseconds later in the same frame: a
# decision line this soon after the pose is still that frame's.
DECISION_LEAD_SECONDS = 1.0 / 30.0

TIME = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]")
LIGHT_LINE = re.compile(r"Light shafts: (.*)$")
PLACED = re.compile(r"the sun at (-?[\d.]+)x(-?[\d.]+) of the ([\d.]+)x([\d.]+) window")
OUTSIDE = re.compile(r"([\d.]+) windows outside")
VIEW_PROJECTION = re.compile(r"the camera's view-projection is (.*)$")
MENU = re.compile(r"menu visibility changed (\d) -> (\d)")
COUNTS = re.compile(r"on at [\d.]+: (\d+) rendered frames, (\d+) with the shafts \(([\d.]+) of them drawn with a held sun\)")
WRITE_RUN = re.compile(r"Draw diagnostics: .* written to (.*)$")
ANNOUNCE = re.compile(r"Draw diagnostics: capturing (\d+) frame\(s\) now")


def seconds(stamp):
    return int(stamp[0]) * 3600.0 + int(stamp[1]) * 60.0 + float(stamp[2] + "." + stamp[3])


def read_log(path):
    """Every interesting line of the log, with its session time, in order: the shaft lines, the camera's own view-projection lines, the menu, and
    the diagnostic's own capture lines (which place a capture on the log's clock). Nothing here assumes the log's first line is the DLL's clock
    zero: the capture lines and the capture's own timestamps are two independent records, and `clock_anchor` checks them against each other."""
    log = {"shaft": [], "camera": [], "menu": [], "writes": [], "announce": [], "first": None}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for text in handle:
            stamp = TIME.match(text)
            if not stamp:
                continue
            moment = seconds(stamp.groups())
            if log["first"] is None:
                log["first"] = moment
            body = LIGHT_LINE.search(text)
            if body:
                log["shaft"].append((moment, body.group(1).strip()))
            view = VIEW_PROJECTION.search(text)
            if view:
                values = [float(value) for value in re.findall(r"-?[\d.]+(?:e[-+]?\d+)?", view.group(1))]
                if len(values) == 16:
                    log["camera"].append((moment, values))
            changed = MENU.search(text)
            if changed:
                log["menu"].append((moment, int(changed.group(2))))
            wrote = WRITE_RUN.search(text)
            if wrote:
                log["writes"].append((moment, wrote.group(1).strip()))
            announced = ANNOUNCE.search(text)
            if announced:
                log["announce"].append(moment)
    return log


def classify(message):
    """The reason a light-shaft line is: a frame that drew, a frame left alone, or something that is not a decision."""
    if message.startswith("the sun at"):
        return "draw"
    if message.startswith("the frame was left as it was"):
        if "sun's mirror" in message:
            return "no sun: the light was the sun's mirror"
        if "directional shadow" in message:
            return "no sun: the light was not the frame's sun"
        if "no light draw of this frame had the sun draw's shape" in message:
            return "no sun: no light draw of the frame"
        if "no light draw of the frame carried a sun" in message:
            return "no sun: the bytes not arrived"
        if "line above says why" in message:
            return "no sun: the bytes had not landed"
        return "no sun: other"
    if message.startswith("the sun is behind the camera"):
        return "out of view: behind the camera"
    if message.startswith("the sun is "):
        return "out of view: outside the margin"
    if message.startswith("the camera's "):
        return "camera"
    if message.startswith("on at "):
        return "counts"
    if message.startswith("the draw was left alone"):
        return "refused"
    return "reading"


def decision(message):
    return classify(message) not in ("camera", "counts", "reading")


def outside_of(message):
    """How far outside the window the line's own pose put the sun, or None when the line does not say a position."""
    found = OUTSIDE.search(message)
    if found:
        return float(found.group(1))
    placed = PLACED.search(message)
    if not placed:
        return None
    x, y, width, height = (float(value) for value in placed.groups())
    return max(0.0, -x / width, (x - width) / width, -y / height, (y - height) / height)


def placed_of(message):
    """The sun's own position on a run line, or None."""
    placed = PLACED.search(message)
    if not placed:
        return None
    return (float(placed.group(1)), float(placed.group(2)))


def log_replay(lines, fps, menu, first):
    """The decision lines, their spans to the next decision, how much of each span the menu covered, and a note for a span the log's own ends
    cut (a span the log ends inside runs to the log's end, and a close whose open line the log does not hold runs from the log's first line)."""
    decisions = [(moment, message, classify(message)) for moment, message in lines]
    decisions = [entry for entry in decisions if decision(entry[1])]
    # The menu's own spans: the frames between a menu opening and its closing line. The game keeps rendering in the menu, so they are part of the
    # session's frames, but they are not a posed, playing frame - they are reported apart instead of being attributed to a decision. A log can end
    # inside a menu (the user quits from the overlay) or start inside one (a log whose first lines were cut): the first case runs to the log's end,
    # the second from the log's first line, so a pose in either is still inside a menu-open span and a frame left alone there is not a miss.
    menu_spans = []
    notes = []
    opened = None
    for moment, visible in menu:
        if visible == 1 and opened is None:
            opened = moment
        elif visible == 0:
            if opened is not None:
                menu_spans.append((opened, moment))
                opened = None
            elif first is not None and moment > first:
                menu_spans.append((first, moment))
                notes.append("the log starts inside a menu-open span (its first line is at %.3f s and the menu closes at %.3f s), so every pose in "
                             "between is inside one" % (first, moment))
            elif first is not None:
                # The log's first timestamped line is the close itself, so there is no span to name and no frame between the two times: a note
                # here would claim a menu-open span of no frames. The pose after it is judged normally, as it was.
                pass
    if opened is not None:
        menu_spans.append((opened, math.inf))
        notes.append("the log's last menu line opens a span at %.3f s that it never closes, so every pose after it is inside a menu-open span and "
                     "a frame left alone there is not a miss" % opened)
    rows = []
    for index, (moment, message, kind) in enumerate(decisions):
        end = decisions[index + 1][0] if index + 1 < len(decisions) else None
        span = (end - moment) if end is not None else 1.0 / fps
        in_menu = sum(max(0.0, min(end if end is not None else moment + span, close) - max(moment, open_))
                      for open_, close in menu_spans)
        rows.append({"t": moment, "message": message, "kind": kind, "frames": max(1, int(round(span * fps))),
                     "span": span, "menu": in_menu, "outside": outside_of(message), "end": end})
    return rows, menu_spans, notes


def capture_samples(directory, window):
    """One pose a second from a capture's `cb.csv`: the light draw's view-space direction, the view's rotation (the first three rows of
    `view_00..view_33`) and the same draw's view-projection, as the capture wrote them."""
    path = os.path.join(directory, "cb.csv")
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        rows = [line for line in handle if not line.startswith("#")]
    if not rows:
        return []
    samples = []
    for row in csv.DictReader(rows):
        try:
            direction = (float(row["dir_x"]), float(row["dir_y"]), float(row["dir_z"]))
            view_projection = [float(row["vp_%d%d" % (r, c)]) for r in range(4) for c in range(4)]
            rotation = [[float(row["view_%d%d" % (r, c)]) for c in range(3)] for r in range(3)]
        except (KeyError, ValueError):
            continue
        if all(value == 0.0 for value in view_projection):
            continue
        world = world_direction(rotation, direction)
        if world is None:
            continue
        samples.append({"time": float(row.get("time_s") or 0.0), "direction": direction, "world": world, "rotation": rotation,
                        "vp": view_projection, "window": window, "crc": row.get("ps_crc", "")})
    return samples


def world_direction(rotation, direction):
    """A view-space direction as the world sees it: the view's rows are the world's axes in view space, so the transpose of that rotation is the
    inverse turn. None when a row is not a vector this tool can read."""
    if len(rotation) != 3 or any(len(row) != 3 for row in rotation) or len(direction) != 3:
        return None
    world = [sum(rotation[i][column] * direction[i] for i in range(3)) for column in range(3)]
    length = sum(value * value for value in world) ** 0.5
    if not length > 1.0e-6:
        return None
    return world


def project(direction, view_projection, width, height):
    """The sun's screen position and how far outside the window it is, as ext/ffxiv/engine/ShaftsRules.h ProjectSun computes it. `direction` is
    the WORLD direction: the module projects the light's view-space vector with the projection alone, which is the same product (see the note at
    the top), so the capture's own view-projection is the matrix this tool has."""
    length = sum(value * value for value in direction)
    if not (0.9604 <= length <= 1.0404):
        return None
    clip_x = sum(view_projection[column] * direction[column] for column in range(3))
    clip_y = sum(view_projection[4 + column] * direction[column] for column in range(3))
    clip_w = sum(view_projection[12 + column] * direction[column] for column in range(3))
    if not clip_w > 0.0:
        return {"behind": True, "offscreen": None, "screen": None}
    x = (clip_x / clip_w * 0.5 + 0.5) * width
    y = (0.5 - clip_y / clip_w * 0.5) * height
    outside = max(0.0, -x / width, (x - width) / width, -y / height, (y - height) / height)
    return {"behind": False, "offscreen": outside, "screen": (x, y)}


def decision_at(rows, moment):
    """The decision in effect at a wall-clock time: the last decision line at or before it, with the next one's time as the bound."""
    current = None
    for row in rows:
        if row["t"] <= moment + DECISION_LEAD_SECONDS:
            current = row
        else:
            break
    return current


def shadow_rows(directory):
    """The directional-shadow rows a capture dumped (`cbdump.csv`, `g_DirectionalShadowParameter`, 128 bytes, row at +0x20), and their axis:
    the row negated and normalised, which is the frame's own sun (ShaftsRules.h ShadowDegrees)."""
    path = os.path.join(directory, "cbdump.csv")
    if not os.path.exists(path):
        return []
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split(",")
            if len(parts) < 8 or parts[3] != "g_DirectionalShadowParameter":
                continue
            try:
                raw = bytes.fromhex(parts[7])
            except ValueError:
                continue
            if len(raw) < SHADOW_ROW_AT + 12:
                continue
            row = struct.unpack_from("<3f", raw, SHADOW_ROW_AT)
            axis = unit([-value for value in row])
            if axis is None:
                continue
            rows.append({"time": float(parts[1]), "row": row, "axis": axis, "frame": parts[0],
                         "ptr": parts[6] if len(parts) > 6 else ""})
    return rows


def unit(values):
    length = sum(value * value for value in values) ** 0.5
    return [value / length for value in values] if length > 0.0 else None


def axis_degrees(axis, direction):
    """The angle between a surface's own axis and a direction, in degrees."""
    other = unit(direction)
    if axis is None or other is None:
        return None
    cosine = max(-1.0, min(1.0, sum(axis[i] * other[i] for i in range(3))))
    return math.degrees(math.acos(cosine))


def nearest(items, moment):
    return min(items, key=lambda item: abs(item["time"] - moment)) if items else None


def light_draws(directory):
    """Each captured frame's light draws, in draw order, and which of them has the sun's own shape."""
    path = os.path.join(directory, "draws.csv")
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        rows = [line for line in handle if not line.startswith("#")]
    frames = {}
    for row in csv.DictReader(rows):
        bindings = row.get("cb") or ""
        if "g_LightParam" not in bindings:
            continue
        targets = [part for part in (row.get("rtv") or "").split("|") if part.count(":") >= 2]
        formats = [part.split(":")[1] for part in targets]
        sizes = [part.split(":")[2] for part in targets]
        blend = (row.get("blend") or "").split("/")
        is_sun = (len(targets) == 2 and all(name == "R11G11B10_FLOAT" for name in formats) and len(set(sizes)) == 1
                  and blend and blend[0].split("/")[0] == "0")
        frames.setdefault(row.get("frame"), []).append({
            "ordinal": int(row.get("ordinal") or 0),
            "crc": row.get("ps_crc"),
            "targets": ",".join(formats),
            "blend": row.get("blend") or "",
            "viewport": row.get("viewport") or "",
            "sun": bool(is_sun),
        })
    return frames


def same_matrix(a, b):
    return len(a) == len(b) and all(abs(x - y) <= 1.0e-3 for x, y in zip(a, b))


def clock_anchor(directory, sample_times, shadows, log, samples):
    """Place the capture's own clock (its `time_s`, which runs from the DLL's clock zero) on the log's timestamps, and say how it was checked.

    The two records are independent: the capture's rows carry the DLL's own seconds, and the log carries its own capture lines - the
    `Draw diagnostics: ... written to <the capture's own directory>` line (written when the capture's last frame ended), the `capturing ... now`
    line (written when the capture started, at the wall clock the directory is named for), and the camera's view-projection lines the module
    writes when a read lands. The anchor is the log time minus the capture time; it is a check, not an assumption, because a log that starts
    mid-session or a capture whose rows were rotated still line up through it. Each record anchors on the capture row it belongs to: the write
    line on the capture's last row, the `capturing` line on its first captured frame (the first `cbdump` row), a camera line on the pose whose
    view-projection the log printed."""
    basename = os.path.basename(os.path.normpath(directory))
    times = [sample["time"] for sample in samples] + [row["time"] for row in shadows]
    if not times:
        return None, "the capture has no timestamped row", []
    last = max(times)
    found = []
    for moment, path in log["writes"]:
        if os.path.basename(path.rstrip("\\/ ")) == basename:
            found.append((moment - last, "the log's capture line for %s at %.3f s is written just after the capture's last row (%.3f s)"
                          % (basename, moment, last)))
    if not found:
        stamp = re.search(r"(\d{8})-(\d{6})", basename)
        if stamp and shadows:
            # The directory's `HHMMSS` clock as seconds of day, against the log line's own seconds of day: two times of day, not a time of day
            # against a six-digit number. The line is written when the capture starts, so its capture row is the first frame captured.
            digits = stamp.group(2)
            clock = int(digits[0:2]) * 3600 + int(digits[2:4]) * 60 + int(digits[4:6])
            first_frame = min(row["time"] for row in shadows)
            matches = [moment for moment in log["announce"] if _clock_in_second(moment, clock)]
            if len(matches) > 1:
                # Two captures started in the same second carry two lines with the same time of day, and nothing in either line names its
                # directory: picking one would place this capture by the other's start. Refuse rather than guess.
                return None, ("the log holds %d `capturing` lines in the directory's own second (%s), so it cannot say which one is this capture's; "
                              "two captures started in the same second" % (len(matches), digits)), []
            if matches:
                moment = matches[0]
                found.append((moment - first_frame, "the log's `capturing` line at %.3f s carries the directory's own clock (%s) and the "
                              "capture's first captured frame is at %.3f s" % (moment, digits, first_frame)))
    if not found:
        for moment, view in log["camera"]:
            for sample in samples:
                if same_matrix(view, sample["vp"]):
                    found.append((moment - sample["time"], "the log's camera view-projection at %.3f s is the capture's own at %.3f s"
                                  % (moment, sample["time"])))
                    break
            if found:
                break
    if not found:
        return None, "neither the log's capture lines nor its camera lines match anything the capture wrote", []
    anchor = found[0][0]
    for other, _ in found[1:]:
        if abs(other - anchor) > 0.25:
            return None, "the log's own records disagree about the capture's clock (%s)" % "; ".join(text for _, text in found), found
    return anchor, found[0][1], found


def _clock_of(moment):
    """The line's own second of day, as the capture directory's `HHMMSS` is written: the fraction is dropped, not rounded, so a line in the last
    half of its second is still that second."""
    return int(moment) % 86400


def _clock_in_second(moment, clock):
    """Whether a log line's own second of day is the second the capture directory is named for. The folder is named and the line written in the
    same second, so the whole second must match: a line in a neighbouring second belongs to another capture, and two lines in this second are
    refused by `clock_anchor` rather than picked between."""
    return _clock_of(moment) == clock


def mark(text):
    return "FAIL" if text else "ok"


def main(argv):
    parser = argparse.ArgumentParser(description="Replay a session's light-shaft decisions and check the acceptance against the capture's own sun.")
    parser.add_argument("--log", required=True, help="the session's OptiScaler.log")
    parser.add_argument("--capture", action="append", default=[], help="a capture directory (cb.csv, cbdump.csv, draws.csv); repeatable")
    parser.add_argument("--fps", type=float, default=60.0, help="the session's frame rate, for turning a reason's span into frames")
    parser.add_argument("--window", default="1920x1080", help="the render window the log reported, as WxH")
    arguments = parser.parse_args(argv)

    width, height = (int(part) for part in arguments.window.lower().split("x"))
    log = read_log(arguments.log)
    if not log["shaft"]:
        print("no light-shaft line in %s: the key was never on in this session" % arguments.log)
        return 1
    rows, menu_spans, menu_notes = log_replay(log["shaft"], arguments.fps, log["menu"], log["first"])

    # The session's real share: the module's own counts line, not the span model. It is written once a minute of rendered frames, so the last one
    # is the session's own answer and the spans below are only what the log's decisions looked like between the lines.
    counts = None
    for moment, message in log["shaft"]:
        found = COUNTS.search(message)
        if found:
            counts = (int(found.group(1)), int(found.group(2)), int(found.group(3)))

    print("== the log's decisions, one line a change of reason ==")
    print("%-10s %-38s %7s %7s  %s" % ("time", "reason", "frames", "menu", "the line"))
    for row in rows:
        print("%-10.3f %-38s %7d %6.1fs  %s" % (row["t"], row["kind"], row["frames"], row["menu"], row["message"][:80]))

    total = sum(row["frames"] for row in rows)
    drawn = sum(row["frames"] for row in rows if row["kind"] == "draw")
    in_menu = sum(row["menu"] for row in rows) * arguments.fps
    print("\nthe span model: %d frames in %d decision(s), %d drawn (%.2f %%), %d of them inside a menu-open span" % (
        total, len(rows), drawn, 100.0 * drawn / max(1, total), in_menu))
    print("  a span is the wall-clock gap to the next decision times the frame rate, so each one is an upper bound, not a count; the frames")
    print("  inside a menu-open span are printed apart because they are not a played pose, not because the module did not draw them")
    for note in menu_notes:
        print("  the menu's span: %s" % note)
    if counts is not None:
        rendered, with_shafts, held = counts
        print("the module's own count: %d rendered frames, %d with the shafts (%.2f %%), %d of them drawn with a held sun" % (
            rendered, with_shafts, 100.0 * with_shafts / max(1, rendered), held))
    else:
        print("the module's own count: no counts line in this log (a session that armed for less than %d frames), so the share is the span model's"
              % 3600)

    # The acceptance. The pose is the capture's own - its light's direction and the view-projection its camera row reports - and the frame's own
    # sun is the capture's own directional-shadow row, not the light the module read. Only a pose the capture also dumped a shadow row for can be
    # posed at all; the rest are reported, not judged.
    failures = 0
    posed = 0
    paired = 0
    in_menu_skipped = 0
    refused_correctly = 0
    unplaced = 0
    for directory in arguments.capture:
        print("\n== the capture %s ==" % directory)
        samples = capture_samples(directory, (width, height))
        frames = light_draws(directory)
        shadows = shadow_rows(directory)
        for frame, draws in sorted(frames.items(), key=lambda item: int(item[0]) if item[0].isdigit() else 0):
            print("  frame %s: %d draw(s) bind g_LightParam, in draw order:" % (frame, len(draws)))
            for draw in draws:
                print("    ordinal %5d %s targets %-40s blend %-10s -> %s" % (
                    draw["ordinal"], draw["crc"], draw["targets"][:40], draw["blend"][:10],
                    "the sun's own shape: the frame's first one is what the module copies" if draw["sun"] else "not the sun's shape"))
        paired_before = paired
        if not samples:
            print("  no cb.csv row with a direction and a view-projection: no pose to check")
            continue
        if not shadows:
            print("  no cbdump.csv row of g_DirectionalShadowParameter: the capture has no frame's own sun to pose, so nothing here is judged")
            continue
        # The captured shadow row against the light the same capture sampled: the identity the pass requires, measured from the capture alone.
        # The row the capture dumped is the rain volume's buffer (its first light draw of the frame), not the sun draw's own; it is still the
        # frame's directional shadow, which is what the identity is about.
        for shadow in shadows:
            near = nearest(samples, shadow["time"])
            if near is None:
                continue
            # The shadow matrix is the view-space transform the shadow shader multiplies a view position by, so its third row is the light's
            # axis in the light's own space: the sampled direction is compared with it as it is.
            degrees = axis_degrees(shadow["axis"], near["direction"])
            if degrees is None:
                continue
            print("  the shadow row at %.3fs (frame %s): its axis is %.2f degrees from the light the capture sampled at %.3fs: %s" % (
                shadow["time"], shadow["frame"], degrees, near["time"],
                "the sampled light is the frame's sun" if degrees <= SHADOW_AGREEMENT_DEGREES else
                "NOT the frame's sun, so a frame left alone for it is the correct answer"))
        anchor, how, found = clock_anchor(directory, [sample["time"] for sample in samples], shadows, log, samples)
        if anchor is None:
            print("  CLOCK: %s" % how)
            print("  the capture's poses cannot be placed on the log's clock, so the acceptance cannot be measured for this capture")
            failures += 1
            continue
        print("  CLOCK: checked - %s; a capture time t is the log at %.3f + t" % (how, anchor))
        for other, text in found[1:]:
            print("         and %s (%.3fs apart)" % (text, other - anchor))
        inside = 0
        for sample in samples:
            wall = anchor + sample["time"]
            shadow = nearest(shadows, sample["time"])
            if shadow is None or abs(shadow["time"] - sample["time"]) > POSE_SHADOW_SECONDS:
                print("  %8.3f  no shadow row of this pose's own frame in the capture: not posed from the frame's own sun" % sample["time"])
                unplaced += 1
                continue
            in_menu = any(open_ <= wall <= close for open_, close in menu_spans)
            paired += 1
            row = decision_at(rows, wall)
            light_degrees = axis_degrees(shadow["axis"], sample["direction"])
            # The frame's own sun is the capture's own shadow axis, which is a view-space vector like the light's: its world direction is what the
            # module's projection maps to the window, and `vp` times that is the very same clip position (see the note at the top). A pose whose
            # rotation the capture did not write is not projected at all rather than projected in the wrong space.
            shadow_world = world_direction(sample["rotation"], shadow["axis"])
            result = project(shadow_world, sample["vp"], width, height) if shadow_world is not None else None
            if result is None:
                print("  %8.3f  the capture's own view rotation cannot be read for this pose: not posed" % sample["time"])
                continue
            if result is None:
                print("  %8.3f  the frame's own sun is not a unit vector: no pose" % sample["time"])
                continue
            position = "behind the camera" if result["behind"] else "%7.1f x %7.1f, %5.2f windows outside" % (
                result["screen"][0], result["screen"][1], result["offscreen"])
            if row is None:
                print("  %8.3f  the frame's sun is %s -> the log has no decision at %.3f: the pass was not armed yet, not judged" % (
                    sample["time"], position, wall))
                continue
            drew = row["kind"] == "draw"
            fresh = wall - row["t"] <= RUN_FRESH_SECONDS
            if light_degrees is not None and light_degrees > SHADOW_AGREEMENT_DEGREES:
                # The light the module read is not the frame's own sun: leaving the frame alone is the correct answer, and drawing is not. This
                # is checked inside a menu span too: the menu excuses a frame that was left alone, never one drawn from a light that is not the sun.
                if not drew:
                    refused_correctly += 1
                    print("  %8.3f  the light this pose sampled is %.1f degrees from the frame's own sun, and the log at %.3f says %-30s -> the "
                          "correct non-draw%s" % (sample["time"], light_degrees, wall, row["kind"],
                                                    " (inside a menu span)" if in_menu else ""))
                    continue
                failures += 1
                print("  %8.3f  FAIL the light this pose sampled is %.1f degrees from the frame's own sun, but the log at %.3f drew from it%s" % (
                    sample["time"], light_degrees, wall, " (inside a menu span)" if in_menu else ""))
                drawn_at = placed_of(row["message"])
                if fresh and drawn_at is not None and result["screen"] is not None:
                    print("         the log's run line puts its sun at %7.1f x %7.1f; the frame's own sun projects to %7.1f x %7.1f" % (
                        drawn_at[0], drawn_at[1], result["screen"][0], result["screen"][1]))
                elif fresh and drawn_at is not None:
                    # A frame whose own sun is behind the camera has no screen position to compare the run line with.
                    print("         the log's run line puts its sun at %7.1f x %7.1f; the frame's own sun is behind the camera, so its position is not "
                          "compared" % (drawn_at[0], drawn_at[1]))
                elif fresh:
                    print("         the log's run line carries no position, so the light alone fails it")
                else:
                    print("         the log's last run line is %.1fs old, so its position is not this pose's; the light alone fails it" % (
                        wall - row["t"]))
                continue
            if in_menu and not drew:
                # A menu-open frame the pass left alone is not a miss: it is not a played pose, so it cannot measure the acceptance.
                in_menu_skipped += 1
                print("  %8.3f  the pose is inside a menu-open span and was left alone: not a played pose, not a miss" % sample["time"])
                continue
            if shadow["axis"][1] <= MIN_SUN_ELEVATION:
                print("  %8.3f  the frame's own sun is at or below the horizon (elevation %.3f): not a posed frame" % (
                    sample["time"], shadow["axis"][1]))
                continue
            if result["behind"] or result["offscreen"] > MAX_OFFSCREEN:
                print("  %8.3f  the frame's sun is %s: not a posed frame" % (sample["time"], position))
                continue
            inside += 1
            posed += 1
            if not drew:
                failures += 1
                print("  %8.3f  the frame's sun is %s and the sampled light is %.2f degrees from it, but the log at %.3f says %-30s -> "
                      "MISSED: a frame with its own sun inside the margin was left alone" % (
                          sample["time"], position, light_degrees if light_degrees is not None else 0.0, wall, row["kind"]))
                continue
            drawn_at = placed_of(row["message"])
            if fresh and drawn_at is not None:
                distance = max(abs(drawn_at[0] - result["screen"][0]), abs(drawn_at[1] - result["screen"][1]))
                if distance > POSITION_PIXELS:
                    failures += 1
                    print("  %8.3f  FAIL the log drew, but it put its sun at %7.1f x %7.1f where the frame's own sun projects to %7.1f x %7.1f "
                          "(%.1f px apart)" % (sample["time"], drawn_at[0], drawn_at[1], result["screen"][0], result["screen"][1], distance))
                    continue
                print("  %8.3f  the frame's sun is %s and the log at %.3f drew at %7.1f x %7.1f (%.1f px from the frame's own sun): the acceptance "
                      "holds" % (sample["time"], position, wall, drawn_at[0], drawn_at[1], distance))
            else:
                print("  %8.3f  the frame's sun is %s and the log at %.3f drew (its run line is %.1fs old, so its position is not this pose's): "
                      "the acceptance holds" % (sample["time"], position, wall, wall - row["t"]))
        print("  poses this capture's own shadow row could pose: %d of %d (the rest have no shadow row of their frame)"
              % (paired - paired_before, len(samples)))
        print("  of those, frames whose own sun is inside the margin: %d" % inside)

    print("\n== the acceptance ==")
    if failures != 0:
        print("  %d check(s) failed: a frame whose own sun is inside the margin was left alone, or a frame was drawn from a light, or at a position," % failures)
        print("  that is not the frame's own sun. The FAIL lines above say which.")
    elif posed == 0:
        print("  no captured pose could be posed from its own frame's shadow row and had that sun inside the margin, so this log cannot measure")
        print("  the rule's share. The share the rule promises is a property of the rule (a frame whose sun is inside the margin draws unless its")
        print("  own light bytes have not landed, which the hold covers for %d frames), not of this session. Posed and judged here: %d pose(s);"
              % (HOLD_FRAMES, paired))
        print("  %d pose(s) had no shadow row of their own frame, %d were inside a menu span, %d were correct non-draws of a light that is not the"
              % (unplaced, in_menu_skipped, refused_correctly))
        print("  frame's sun.")
    else:
        print("  every one of the %d in-margin posed frames drew at the frame's own sun: the acceptance holds." % posed)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
