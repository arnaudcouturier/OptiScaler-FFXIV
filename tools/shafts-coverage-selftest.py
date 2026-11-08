"""Checks for the light-shaft coverage tool: fabricated captures and logs, then the tool's own answers.

The tool replays a session's decisions from its log and its draw captures and exits non-zero on a miss, on a draw at a light or a position that
is not the frame's own sun, and on a capture clock it cannot check. A defect in that rule shows up as a wrong exit code in a session, which is
the last place it should show up, so this builds the cases the tool has to get right in a temporary directory and checks its answer and the lines
it prints:

  * the frame's own sun inside the margin and a run line at that position (the acceptance holds);
  * the same frame left alone (a miss, exit 1);
  * a draw 1,274 px from the frame's own sun (a fail, exit 1);
  * a light 48.9 degrees off the frame's own shadow row that drew (a fail, exit 1) and the same light left alone (the correct non-draw, exit 0);
  * a pose the log leaves inside a menu-open span it never closes (not a played pose, so not a miss) - a log that ends while the overlay is
    open must not turn every pose after it into a false miss;
  * a pose inside a menu span whose opening line the log does not hold, because its first lines were cut (the same answer, from the log's own
    first timestamp);
  * a menu close that is the log's first timestamped line: the pose after it is judged normally, and no span of no frames is claimed;
  * a log whose only clock anchor is the diagnostic's `capturing ... now` line, whose own second is the one the capture directory is named for:
    the capture is still placed, and the pose is judged (the fallback the note describes);
  * two `capturing` lines a second apart, where only the line in the capture's own second places it right, and a line in the next second that
    is refused rather than taken; two `capturing` lines in the capture's own second are refused as ambiguous (two captures started together);
  * a draw from a light that is not the frame's sun while the frame's own sun is behind the camera: the wrong-light FAIL names the light and
    says the sun is behind the camera, instead of reading a screen position that does not exist.

The geometry is the captured frame's: the shadow row the 2026-11-01 capture dumped at +0x20, the view-projection the module printed at 17:53:08,
and the yawed light that session drew from. The expected screen positions are computed here, from those numbers, not taken from the tool.

    python -I -B tools/shafts-coverage-selftest.py [--tool PATH] [--keep]

`--tool` points the checks at another copy of the tool (a build's own, to see the cases fail on an older one). `--keep` leaves the fixtures in
the temporary folder and prints its path. `-B` keeps the interpreter from writing a byte-code cache beside the tool, which would be a file the
repository does not track. Exit code 0 when every check passed, 1 otherwise. Nothing outside the temporary directory is written.
"""
import argparse
import contextlib
import importlib.util
import io
import os
import struct
import sys
import tempfile

# The captured frame's own numbers: the shadow row the 2026-11-01 capture dumped (g_DirectionalShadowParameter, +0x20), which negated and
# normalised is that frame's sun, the view-projection the module printed at 17:53:08, and the light that session drew from, 48.9 degrees off the
# row at the same elevation.
SHADOW_ROW = (-0.005546353757381439, -0.03677206113934517, 0.058028124272823334)
SUN_AXIS = (0.080473, 0.533533, -0.841942)
YAWED_LIGHT = (0.7624, 0.53, -0.3713)
VIEW_PROJECTION = (1.0822, -0.0, 0.8375, -432.7261,
                   -0.4082, 2.3395, 0.5277, -462.2586,
                   0.0, 0.0, 0.0, 0.1,
                   0.5884, 0.2743, -0.7606, 456.3698)
# The captured buffer's first 64 bytes: three matrix rows, the third carrying the light's axis, then the (0, 0, 0, 1) row of a light-space matrix.
# The rest of the 128 bytes is padding, as the capture's own buffer is.
SHADOW_HEX = ("36a1463d8dadefbb5e9942b3bb25003ff9a9223b0fce863ca7bc2e3c7021803e30beb5bb4d9e16bde6ae6d3dfb6f803f"
              "0000000000000000000000000000803f") + "00" * 64

# A camera turned a quarter turn about the world's up: its rows are the world's axes in view space, so a light that is SUN_AXIS in the world is
# `TURNED_VIEW * SUN_AXIS` in this view. The tool has to turn it back before it projects with `vp`, which is what the frame at a real camera does.
TURNED_VIEW = ((0.0, 0.0, 1.0), (0.0, 1.0, 0.0), (-1.0, 0.0, 0.0))
TURNED_LIGHT = (-0.841942, 0.533533, -0.080473)
WINDOW = (1920, 1080)
POSE_TIME = 1272.711     # the capture row that dumped the shadow row of its own frame
SHADOW_TIME = 1272.666
# A pose from before the capture's own frames, which a session's `cb.csv` holds: it has no shadow row of its frame, so it is never posed, and it is
# what a clock anchored on the capture's first sample (instead of its first captured frame) would place the poses by.
EARLY_POSE_TIME = 249.144
EARLY_LIGHT = (0.425134, 0.776113, 0.465735)
FOLDER = "20261101-175308"  # the directory's clock, which the `capturing ... now` line's own second carries
WRITE_CLOCK = "17:53:08.004"  # the log time of the diagnostic's write line: 64388.004 s of the day
# The `capturing ... now` line's own second is the directory's, and the fraction is in the last half of that second: the comparison is the line's
# second, not a rounded time of day, so a rounded comparison does not match the folder it belongs to. The pose's own wall clock then is that line's
# time minus the capture's first captured frame, plus the pose's own time.
ANNOUNCE_CLOCK = "17:53:08.663"
ANNOUNCE_DECISION = "17:53:08.708"
# Two captures started a second apart: the earlier line is in the previous second and must not be taken for the later capture's own line.
NEIGHBOUR_CLOCK = "17:53:08.400"
OWN_FOLDER = "20261101-175309"
OWN_CLOCK = "17:53:09.200"
OWN_DECISION = "17:53:09.245"
SHADOW_ROW_AT = 0x20  # `g_DirectionalShadowParameter`'s row inside the 128-byte buffer
# The captured shadow buffer with its row replaced by the frame's own sun: negated and normalised, that row is -SUN_AXIS, which the capture's own
# view-projection puts behind the camera (its clip w is negative). A wrong-light FAIL there has no screen position to print.
BEHIND_ROW = SUN_AXIS


def screen_of(direction, view_projection, width, height):
    """Where a direction projects with a view-projection, computed here: the row dot the direction divided by the fourth row's, as the pass does."""
    length = sum(value * value for value in direction) ** 0.5
    unit = [value / length for value in direction]

    def row(index):
        return sum(view_projection[index * 4 + column] * unit[column] for column in range(3))

    clip_x, clip_y, clip_w = row(0), row(1), row(3)
    return (clip_x / clip_w * 0.5 + 0.5) * width, (0.5 - clip_y / clip_w * 0.5) * height


SUN_AT = screen_of(SUN_AXIS, VIEW_PROJECTION, *WINDOW)
YAWED_AT = screen_of(YAWED_LIGHT, VIEW_PROJECTION, *WINDOW)


class Checks:
    def __init__(self):
        self.passed = 0
        self.failures = []

    def check(self, name, condition, detail=""):
        if condition:
            self.passed += 1
        else:
            self.failures.append("%s: %s" % (name, detail) if detail else name)
        return condition

    def output(self, name, text, needle):
        return self.check(name, needle in text, "%r is not in the tool's output" % needle)


def shadow_buffer(row):
    """A 128-byte directional-shadow buffer with `row` at +0x20, where the tool reads the row `g_DirectionalShadowParameter` carries."""
    return (b"\0" * SHADOW_ROW_AT + struct.pack("<3f", *row) + b"\0" * (128 - SHADOW_ROW_AT - 12)).hex()


def capture(folder, light, early_pose=False, shadow_row=None, rotation=None):
    """A capture directory as the diagnostic writes one: one pose (cb.csv), one shadow row (cbdump.csv). An `early_pose` adds a pose from before
    the capture's own frames, as a session's `cb.csv` holds them, so a clock anchored on the capture's first sample instead of its first captured
    frame is a different answer. A `shadow_row` replaces the captured row, for a frame whose own sun is somewhere the captured one is not. The
    camera's view is the identity unless `rotation` says otherwise: the rows `view_00..view_22` are the world's axes in view space, which is what
    turns the sampled view-space direction back into the world before the tool projects it."""
    os.makedirs(folder)
    header = (["frame", "time_s", "ps_crc", "cb", "slot", "bytes", "pos_x", "pos_y", "pos_z", "dir_x", "dir_y", "dir_z",
               "diffuse_r", "diffuse_g", "diffuse_b"]
              + ["view_%d%d" % (r, c) for r in range(4) for c in range(4)]
              + ["vp_%d%d" % (r, c) for r in range(4) for c in range(4)])
    view = tuple(rotation) if rotation is not None else ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))

    def row(time, direction):
        values = ["0", "%.3f" % time, "0x9DC59665", "g_LightParam", "2", "512", "0", "0", "0"]
        values += ["%.6g" % value for value in direction]
        values += ["1.96", "1.86885", "1.76525"]
        for axis in view:
            values += ["%.6g" % value for value in axis] + ["0"]
        values += ["0", "0", "0", "1"]
        values += ["%.6g" % value for value in VIEW_PROJECTION]
        return ",".join(values)

    poses = [(POSE_TIME, light)]
    if early_pose:
        poses.insert(0, (EARLY_POSE_TIME, EARLY_LIGHT))
    with open(os.path.join(folder, "cb.csv"), "w", encoding="utf-8") as handle:
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write(",".join(header) + "\n")
        for time, direction in poses:
            handle.write(row(time, direction) + "\n")
    with open(os.path.join(folder, "cbdump.csv"), "w", encoding="utf-8") as handle:
        buffer = SHADOW_HEX if shadow_row is None else shadow_buffer(shadow_row)
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write("frame,time_s,ps_crc,name,slot,bytes,ptr,hex\n")
        handle.write("1,%.3f,0x1257B24B,g_DirectionalShadowParameter,3,%d,1e6b8a57120,%s\n"
                     % (SHADOW_TIME, len(buffer) // 2, buffer))


def draw_line(clock, at):
    return ("[%s] [I] ext::ffxiv::shafts::RunToneMap Light shafts: the sun at %.1fx%.1f of the %dx%d window inside a 3840x2160 scene, 48 "
            "samples, decay 0.960, density 0.90, intensity 1.00" % (clock, at[0], at[1], WINDOW[0], WINDOW[1]))


def left_alone(clock):
    return ("[%s] [I] ext::ffxiv::shafts::RunToneMap Light shafts: the frame was left as it was (no light draw of the frame carried a sun to "
            "project too: the light's bytes have not arrived yet)" % clock)


def write_line(clock, folder):
    return ("[%s] [I] ext::ffxiv::diag::WriteRun Draw diagnostics: 1 frame(s), 12 draw row(s) (0 omitted), 1 cb.csv read(s), 1 cbdump.csv "
            "sample(s) written to %s" % (clock, folder))


def announce_line(clock):
    return ("[%s] [I] ext::ffxiv::diag::Announce Draw diagnostics: capturing 1 frame(s) now (the capture key was pressed); the files are "
            "written when the last frame ends" % clock)


def menu_line(clock, visible):
    return ("[%s] [I] OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed %d -> %d blockMouse:%d blockKeyboard:%d"
            % (clock, 1 - visible, visible, visible, visible))


def write_log(path, lines):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def seconds_of(clock):
    """A log line's `HH:MM:SS.fff` as the seconds of the day the tool places a capture on."""
    hours, minutes, rest = clock.split(":")
    return int(hours) * 3600 + int(minutes) * 60 + float(rest)


def digits_of(clock):
    """A log line's `HH:MM:SS.fff` as the six digits a capture directory carries after its date, which are its own second."""
    hours, minutes, rest = clock.split(":")
    return hours + minutes + rest[0:2]


def run(tool, log, folder):
    buffer = io.StringIO()
    try:
        with contextlib.redirect_stdout(buffer):
            code = tool.main(["--log", log, "--capture", folder, "--window", "%dx%d" % WINDOW])
    except Exception as error:  # a case that crashes the tool is a failed check, not a stopped run
        code = -1
        buffer.write("[the tool raised %s: %s]" % (type(error).__name__, error))
    return code, buffer.getvalue()


def load(path):
    sys.dont_write_bytecode = True  # no __pycache__ beside the tool: the repository must not gain a file from a run
    spec = importlib.util.spec_from_file_location("shafts_coverage", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check the light-shaft coverage tool against fabricated captures and logs.")
    parser.add_argument("--tool", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "shafts-coverage.py"),
                        help="the coverage tool to check (default: the one beside this file)")
    parser.add_argument("--keep", action="store_true", help="keep the fabricated folders and print where they are")
    arguments = parser.parse_args(argv)

    tool = load(arguments.tool)
    root = tempfile.mkdtemp(prefix="shafts-coverage-selftest-")
    checks = Checks()
    print("[selftest] checking %s" % arguments.tool)
    print("[selftest] the frame's own sun projects to %.1fx%.1f and the yawed light to %.1fx%.1f of the %dx%d window"
          % (SUN_AT[0], SUN_AT[1], YAWED_AT[0], YAWED_AT[1], WINDOW[0], WINDOW[1]))
    try:
        # ---- the poses the capture's own shadow row can judge ----
        def pose_case(name, light, log_lines, shadow_row=None, rotation=None):
            folder = os.path.join(root, name, FOLDER)
            capture(folder, light, shadow_row=shadow_row, rotation=rotation)
            log = os.path.join(root, name, "OptiScaler.log")
            write_log(log, log_lines(folder))
            return run(tool, log, folder)

        code, text = pose_case("draw-at-the-row", SUN_AXIS, lambda folder: [write_line(WRITE_CLOCK, folder), draw_line(WRITE_CLOCK, SUN_AT)])
        checks.check("a draw at the frame's own sun: exit 0", code == 0, "the tool exited %d" % code)
        checks.output("a draw at the frame's own sun: the acceptance holds", text, "the acceptance holds")

        code, text = pose_case("left-alone", SUN_AXIS, lambda folder: [write_line(WRITE_CLOCK, folder), left_alone(WRITE_CLOCK)])
        checks.check("a frame with its own sun inside the margin left alone: exit 1", code == 1, "the tool exited %d" % code)
        checks.output("a frame with its own sun inside the margin left alone: MISSED", text, "MISSED")

        # The light's direction is view space: the same sun, in a camera turned a quarter turn, is a different vector, and the tool has to turn it
        # back before it projects. A tool that projected the view vector itself would put this sun somewhere else entirely.
        # The shadow's row is in the light's own space, like its direction (the two are the same three floats in a real capture), so the turned
        # camera's row is the turned light's vector and the frame's own sun it stands for is still SUN_AXIS.
        code, text = pose_case("turned-camera", TURNED_LIGHT, lambda folder: [write_line(WRITE_CLOCK, folder), draw_line(WRITE_CLOCK, SUN_AT)],
                               shadow_row=tuple(-value for value in TURNED_LIGHT), rotation=TURNED_VIEW)
        checks.check("a sun in a turned camera's view space: exit 0", code == 0, "the tool exited %d" % code)
        checks.output("a sun in a turned camera's view space: the acceptance holds", text, "the acceptance holds")

        code, text = pose_case("draw-far", SUN_AXIS, lambda folder: [write_line(WRITE_CLOCK, folder), draw_line(WRITE_CLOCK, YAWED_AT)])
        checks.check("a draw at another light's position: exit 1", code == 1, "the tool exited %d" % code)
        checks.output("a draw at another light's position: the two positions are printed", text, "px apart")

        code, text = pose_case("yawed-draw", YAWED_LIGHT, lambda folder: [write_line(WRITE_CLOCK, folder), draw_line(WRITE_CLOCK, YAWED_AT)])
        checks.check("a light that is not the sun and drew: exit 1", code == 1, "the tool exited %d" % code)
        checks.output("a light that is not the sun and drew: the angle is printed", text, "degrees from the frame's own sun")

        code, text = pose_case("yawed-left-alone", YAWED_LIGHT, lambda folder: [write_line(WRITE_CLOCK, folder), left_alone(WRITE_CLOCK)])
        checks.check("a light that is not the sun, left alone: exit 0", code == 0, "the tool exited %d" % code)
        checks.output("a light that is not the sun, left alone: the correct non-draw", text, "the correct non-draw")
        checks.check("a light that is not the sun, left alone: no miss", "MISSED" not in text, "the tool printed MISSED")

        code, text = pose_case("behind-camera-wrong-light", YAWED_LIGHT,
                               lambda folder: [write_line(WRITE_CLOCK, folder), draw_line(WRITE_CLOCK, SUN_AT)], shadow_row=BEHIND_ROW)
        checks.check("a wrong light drawn while the frame's own sun is behind the camera: exit 1", code == 1, "the tool exited %d" % code)
        checks.output("the sun behind the camera: the wrong light is named", text, "degrees from the frame's own sun")
        checks.output("the sun behind the camera: the sun is named as behind it", text, "the frame's own sun is behind the camera")
        checks.check("the sun behind the camera: no screen position is read", "the tool raised" not in text, "the tool raised instead of failing")

        # ---- a menu span the log's own ends cut ----
        code, text = pose_case("menu-never-closed", SUN_AXIS,
                               lambda folder: [write_line(WRITE_CLOCK, folder), menu_line("17:53:07.000", 1), left_alone(WRITE_CLOCK)])
        checks.check("a pose inside a menu span the log never closes: exit 0", code == 0, "the tool exited %d" % code)
        checks.check("a pose inside a menu span the log never closes: no miss", "MISSED" not in text, "the tool printed MISSED")
        checks.output("a pose inside a menu span the log never closes: not a played pose", text, "not a played pose, not a miss")
        checks.output("a pose inside a menu span the log never closes: the span is named", text, "never closes")

        code, text = pose_case("menu-log-starts-inside", SUN_AXIS,
                               lambda folder: [write_line(WRITE_CLOCK, folder), menu_line("17:53:08.900", 0), left_alone(WRITE_CLOCK)])
        checks.check("a pose inside a menu span the log's start is cut out of: exit 0", code == 0, "the tool exited %d" % code)
        checks.check("a pose inside a menu span the log's start is cut out of: no miss", "MISSED" not in text, "the tool printed MISSED")
        checks.output("a pose inside a menu span the log's start is cut out of: not a played pose", text, "not a played pose, not a miss")
        checks.output("a pose inside a menu span the log's start is cut out of: the cut is named", text, "starts inside a menu-open span")

        code, text = pose_case("menu-close-is-the-first-line", SUN_AXIS,
                               lambda folder: [menu_line("17:53:08.600", 0), announce_line(ANNOUNCE_CLOCK),
                                               left_alone(ANNOUNCE_DECISION)])
        checks.check("a menu close at the log's first line: the pose is judged, exit 1", code == 1, "the tool exited %d" % code)
        checks.output("a menu close at the log's first line: the pose after it is a miss", text, "MISSED")
        checks.check("a menu close at the log's first line: no span of no frames is claimed", "starts inside a menu-open span" not in text,
                     "the tool claimed a menu-open span that holds no frames")

        # ---- a log whose only clock anchor is the diagnostic's own `capturing ... now` line ----
        folder = os.path.join(root, "announce-only", FOLDER)
        capture(folder, SUN_AXIS, early_pose=True)
        log = os.path.join(root, "announce-only", "OptiScaler.log")
        write_log(log, [announce_line(ANNOUNCE_CLOCK), draw_line(ANNOUNCE_DECISION, SUN_AT), left_alone("17:53:10.000")])
        code, text = run(tool, log, folder)
        checks.check("a log whose only anchor is the capturing line: exit 0", code == 0, "the tool exited %d" % code)
        checks.output("a log whose only anchor is the capturing line: the clock is checked from that line", text,
                      "carries the directory's own clock")
        checks.output("a log whose only anchor is the capturing line: the pose is placed on the log's own time", text,
                      "the log at %.3f drew at" % seconds_of(ANNOUNCE_DECISION))
        checks.output("a log whose only anchor is the capturing line: the acceptance holds", text, "the acceptance holds")

        # ---- the capturing line's own second, and the two ways it is ambiguous - the tool chooses nothing - ----
        folder = os.path.join(root, "two-captures-a-second-apart", OWN_FOLDER)
        capture(folder, SUN_AXIS)
        log = os.path.join(root, "two-captures-a-second-apart", "OptiScaler.log")
        write_log(log, [announce_line(NEIGHBOUR_CLOCK), announce_line(OWN_CLOCK), draw_line(OWN_DECISION, SUN_AT)])
        code, text = run(tool, log, folder)
        checks.check("two capturing lines a second apart: exit 0", code == 0, "the tool exited %d" % code)
        checks.output("two capturing lines a second apart: the capture's own second is the one used", text,
                      "carries the directory's own clock (%s)" % OWN_FOLDER[-6:])
        checks.check("two capturing lines a second apart: the neighbouring line is not used",
                     "(%s)" % digits_of(NEIGHBOUR_CLOCK) not in text, "the tool named the neighbouring capture's clock")
        checks.check("two capturing lines a second apart: the neighbouring line's time is not the anchor",
                     "capturing` line at %.3f s" % seconds_of(NEIGHBOUR_CLOCK) not in text,
                     "the tool anchored the capture on the neighbouring capture's line")
        checks.output("two capturing lines a second apart: the pose is placed on its own line", text,
                      "the log at %.3f drew at" % seconds_of(OWN_DECISION))
        checks.output("two capturing lines a second apart: the acceptance holds", text, "the acceptance holds")

        folder = os.path.join(root, "capturing-line-in-the-next-second", FOLDER)
        capture(folder, SUN_AXIS)
        log = os.path.join(root, "capturing-line-in-the-next-second", "OptiScaler.log")
        write_log(log, [announce_line("17:53:09.400"), draw_line("17:53:09.945", SUN_AT)])
        code, text = run(tool, log, folder)
        checks.check("a capturing line in the next second: refused, exit 1", code == 1, "the tool exited %d" % code)
        checks.output("a capturing line in the next second: the reason is named", text,
                      "neither the log's capture lines nor its camera lines match")
        checks.check("a capturing line in the next second: no clock is claimed", "carries the directory's own clock" not in text,
                     "the tool placed the capture by a line in another second")

        folder = os.path.join(root, "two-captures-in-one-second", FOLDER)
        capture(folder, SUN_AXIS)
        log = os.path.join(root, "two-captures-in-one-second", "OptiScaler.log")
        write_log(log, [announce_line("17:53:08.100"), announce_line("17:53:08.900"), draw_line(ANNOUNCE_DECISION, SUN_AT)])
        code, text = run(tool, log, folder)
        checks.check("two capturing lines in one second: refused, exit 1", code == 1, "the tool exited %d" % code)
        checks.output("two capturing lines in one second: the ambiguity is named", text,
                      "cannot say which one is this capture's")
    finally:
        if arguments.keep:
            print("[selftest] the fabricated folders are in %s" % root)
        else:
            import shutil

            shutil.rmtree(root, ignore_errors=True)

    for failure in checks.failures:
        print("[selftest] FAIL %s" % failure)
    print("[selftest] passed %d, failed %d" % (checks.passed, len(checks.failures)))
    return 1 if checks.failures else 0


if __name__ == "__main__":
    sys.exit(main())
