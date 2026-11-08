"""Self-test for the draw-diagnostic tools: generated fixtures, then the real captures when a folder of them is given.

The generated part is a small session of two format-3 capture folders in a temporary directory: a frame with the shapes
the tools look for (a five-target G-buffer row, the wetness pass and a later reader of its target, the light draws, the
shadow atlas's five slices, an unnamed reflection-chain row, the ray march, the water consumers, the tone map), a
`cbdump.csv` with a known wetness buffer, ambient probe array, glare row and a few flat buffers, a `views.txt` describing
a cube array, and a `dump\\` folder with one 4x4 texture per supported format (packed, padded, a compressed one this tool
refuses, and a 16-bit depth written with a four-byte stride, which it also refuses).  Edge captures then pin the rules one
at a time: a window whose rows the cap dropped, a window with two tone-map rows, a 4K depth prepass next to the shadow
atlas, a wetness pass whose target no later row reads, and a draw that binds the wetness buffer with only one of the two
inputs the pass needs.  The known values are asserted, so a change in a layout offset, a delimiter, a preview curve or a
report line fails here instead of in a session.

The real part reads a folder of captures the caller names (`--captures DIR`, or `DRAWDIAG_CAPTURES` in the environment)
and checks what any capture folder must satisfy, whatever session it came from: a format-1, format-2 or format-3 file
loads and its trailer agrees with the rows; a window the row cap cut answers [U]; every depth-only draw on an
atlas-shaped depth view carries one of the five slice offsets; a five-target row writes five targets of one size; and a
raw pixel dump's real bytes decode through the depth, mask and colour paths.  Without that argument the part is skipped
with a printed note, and the generated part always runs.

    python -I selftest.py [--captures DIR] [--keep]

Exit code 0 when every check passed, 1 otherwise.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cblayouts  # noqa: E402
import cbdump_report  # noqa: E402
import drawdiag  # noqa: E402
import dump_to_png  # noqa: E402
import session_answers  # noqa: E402

# No default: a captures root holds someone's own recordings, so the caller names it and the real-capture part is skipped
# when nothing was named.  The environment variable is for a shell that runs the self-test repeatedly.
CAPTURES_ENV = "DRAWDIAG_CAPTURES"


# ---------------------------------------------------------------------------------------------------------------------
# The check harness
# ---------------------------------------------------------------------------------------------------------------------


class Suite:
    """A list of checks that keeps going after a failure, so one run reports everything that is wrong."""

    def __init__(self) -> None:
        self.passed = 0
        self.failures: list[str] = []
        self.skipped: list[str] = []

    def check(self, name: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.passed += 1
        else:
            self.failures.append(f"{name}: {detail}" if detail else name)
        return bool(condition)

    def equal(self, name: str, actual: object, expected: object) -> bool:
        return self.check(name, actual == expected, f"{actual!r} != {expected!r}")

    def almost(self, name: str, actual: float, expected: float, tolerance: float = 1e-6) -> bool:
        return self.check(name, abs(float(actual) - float(expected)) <= tolerance,
                          f"{actual!r} is not {expected!r} (+-{tolerance})")

    def contains(self, name: str, text: str, needle: str) -> bool:
        return self.check(name, needle in text, f"{needle!r} not in the text")

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append(f"{name}: {reason}")

    @contextlib.contextmanager
    def section(self, name: str):
        """Run a block; an exception becomes one failure and the next section still runs."""

        try:
            yield
        except Exception as error:  # noqa: BLE001 - the point is to report anything, not to be selective
            self.failures.append(f"{name}: raised {type(error).__name__}: {error}")


# ---------------------------------------------------------------------------------------------------------------------
# Fixture writers
# ---------------------------------------------------------------------------------------------------------------------

DRAWS_HEADER = ("frame,ordinal,api,context,count,instances,start,base,stage,ps_crc,srv,cb,rtv,dsv,viewport,blend,"
                "depth,markers")
CBDUMP_HEADER = "frame,time_s,ps_crc,name,slot,bytes,ptr,hex"
CB_HEADER = ("frame,time_s,ps_crc,cb,slot,bytes,pos_x,pos_y,pos_z,dir_x,dir_y,dir_z,diffuse_r,diffuse_g,diffuse_b,"
             + ",".join(f"view_{row}{column}" for row in range(4) for column in range(4))
             + "," + ",".join(f"vp_{row}{column}" for row in range(4) for column in range(4)))
VIEWS_HEADER = ("name,ordinal,view_dimension,view_mips,view_format,array_size,cubes,width,height,texture_array,"
                "texture_mips,texture_format,pointer")


def wetness_bytes(values: list[float]) -> bytes:
    return struct.pack("<8f", *values)


def ambient_bytes(probe_scale: tuple[int, float] | None = None, sky_visibility: float = 0.5,
                  sky_sh: float = 0.0, param_num: int = 64, param_num_float: float | None = None,
                  near_unit: tuple[int, float] | None = None) -> bytes:
    """A 12,352 B probe array with known values: skyVisibility on every probe, one non-unit scale if asked.

    ``param_num`` is the header's probe count, written as an unsigned integer; ``param_num_float`` instead writes a float
    into the same four bytes, which is how a value whose declared type the studies do not record shows up as a huge
    integer.  ``near_unit`` writes a scale that is not exactly 1.0 but inside the tool's unit tolerance, so the tolerance
    is pinned from both sides and not only by exact ones.
    """

    buffer = bytearray(cblayouts.AMBIENT_PARAM_ARRAY.size)
    struct.pack_into("<I", buffer, 0, param_num)
    if param_num_float is not None:
        struct.pack_into("<f", buffer, 0, param_num_float)
    struct.pack_into("<f", buffer, 0x04, 0.25)
    for index in range(12):
        struct.pack_into("<f", buffer, 0x10 + 4 * index, sky_sh)
    for probe in range(64):
        base = cblayouts.AMBIENT_PARAM_ARRAY.probe_offset + probe * cblayouts.AMBIENT_PROBE.size
        struct.pack_into("<f", buffer, base + 0x30, 0.125 + probe)  # color.x keeps the probe index visible
        struct.pack_into("<f", buffer, base + 0x4C, sky_visibility)
        struct.pack_into("<f", buffer, base + 0x50, 0.75)  # reflectionScale
        struct.pack_into("<f", buffer, base + 0xA8, 0.5)  # priority
        struct.pack_into("<I", buffer, base + 0xAC, 1 if probe < 32 else 2)  # envSpaceType
        struct.pack_into("<f", buffer, base + 0xB0, 1.0)  # skyVisibilityScale
        struct.pack_into("<f", buffer, base + 0xB4, 0.0 + probe / 64.0)  # ssaoMask
    if probe_scale is not None:
        probe, value = probe_scale
        struct.pack_into("<f", buffer, cblayouts.AMBIENT_PARAM_ARRAY.probe_offset
                         + probe * cblayouts.AMBIENT_PROBE.size + 0xB0, value)
    if near_unit is not None:
        probe, value = near_unit
        struct.pack_into("<f", buffer, cblayouts.AMBIENT_PARAM_ARRAY.probe_offset
                         + probe * cblayouts.AMBIENT_PROBE.size + 0xB0, value)
    return bytes(buffer)


def directional_shadow_bytes() -> bytes:
    matrix = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.1, 0.2, 0.3, 1.0]
    distance = [10.0, 20.0, 30.0, 40.0]
    parameter = [0.5, 0.25, 0.125, 0.0625]
    return struct.pack("<24f", *(matrix + distance + parameter))


def reflection_bytes() -> bytes:
    buffer = bytearray(80)
    struct.pack_into("<f", buffer, 0x00, 1.0 / 2560)  # m_PixelSize.x
    struct.pack_into("<f", buffer, 0x10, 42.0)  # m_FadeDistance.x
    struct.pack_into("<f", buffer, 0x20, 0.5)  # m_NormalBlendWeight
    struct.pack_into("<f", buffer, 0x24, 1.25)  # m_ReflectionPowerScale
    struct.pack_into("<f", buffer, 0x28, 3.0)  # m_MipLevel
    struct.pack_into("<f", buffer, 0x2C, 0.0625)  # m_RoughnessThreshold
    return bytes(buffer)


def draws_row(ordinal: int, api: str, count: int, stage: int, crc: str, srv: str = "", cb: str = "",
              rtv: str = "", dsv: str = "", viewport: str = "", markers: str = "", frame: int = 1) -> str:
    """One ``draws.csv`` row in the documented column order, with the same state on every row."""

    return (f"{frame},{ordinal},{api},game,{count},1,0,0,{stage},{crc},{srv},{cb},{rtv},{dsv},{viewport},"
            f"0/2/1/1/2/1/1/15,1/1/2/0,{markers}")


def write_capture(folder: Path, *, wet_dry: float, wet_second: float, scale_probe: tuple[int, float],
                  sky_visibility: float) -> None:
    """One format-3 capture with every shape the tools look for, and known values in its sampled buffers."""

    folder.mkdir(parents=True, exist_ok=True)
    rows: list[str] = []

    def row(ordinal: int, api: str, count: int, stage: int, crc: str, srv: str, cb: str, rtv: str, dsv: str,
            viewport: str, markers: str = "") -> None:
        rows.append(draws_row(ordinal, api, count, stage, crc, srv, cb, rtv, dsv, viewport, markers))

    row(1, "DrawIndexed", 120, 0, "0x11111111", "", "",
        "0:B8G8R8A8_UNORM:2560x1440:1001|1:B8G8R8A8_UNORM:2560x1440:1002|2:B8G8R8A8_UNORM:2560x1440:1003|"
        "3:R16G16B16A16_FLOAT:2560x1440:1004|4:B8G8R8A8_UNORM:2560x1440:1005", "D24_UNORM_S8_UINT:2560x1440:1006",
        "1:2560x1440+0.5+0.5")
    row(2, "DrawIndexed", 60, 0, "0x22222222", "0:g_SamplerGBuffer.T:B8G8R8A8_UNORM:2560x1440:1001",
        "0:g_MaterialParameter:512:2000|1:g_WetnessParameter:32:2001", "0:B8G8R8A8_UNORM:2560x1440:1002",
        "D24_UNORM_S8_UINT:2560x1440:1006", "1:2560x1440+0.5+0.5")
    # The wetness pass.  Its last entry carries a sixth field, which a later format may append and this reader ignores.
    row(3, "Draw", 3, 0, "0xADE72072",
        "0:g_SamplerGBuffer3.T:R16G16B16A16_FLOAT:2560x1440:1004|"
        "1:g_SamplerViewPosition.T:R16G16B16A16_FLOAT:2560x1440:1007:ff",
        "0:g_CommonParameter:64:2002|1:g_WetnessParameter:32:2001", "0:B8G8R8A8_UNORM:2560x1440:3001", "-",
        "1:2560x1440+0.5+0.5")
    row(4, "Draw", 4, 0, "0x33333333", "0:g_SamplerWetness.T:B8G8R8A8_UNORM:2560x1440:3001",
        "0:g_CommonParameter:64:2002", "0:R16G16B16A16_FLOAT:2560x1440:3002", "-", "1:2560x1440+0.5+0.5")
    row(5, "DrawIndexed", 12, 0, "0x44444444",
        "0:g_SamplerLightDiffuse.T:R16G16B16A16_FLOAT:2560x1440:4001|"
        "1:g_SamplerLightSpecular.T:R16G16B16A16_FLOAT:2560x1440:4002", "0:g_LightParam:512:4003",
        "0:R16G16B16A16_FLOAT:2560x1440:4004", "-", "1:2560x1440+0.5+0.5")
    for index, top in enumerate((0, 2048, 4096, 6144, 8192)):
        row(6 + index, "DrawIndexed", 12, 0, "0x55555555", "0:g_SamplerColorMap0.T:BC7_UNORM:512x512:7001",
            "0:g_MaterialParameter:512:2000", "", "fmt55:2048x10240:5001", f"1:2048x2048+0.5+{top}.5")
    # A chain row whose shader's reflection the capture does not carry: every SRV name is "-".
    row(11, "Draw", 4, 0, "0x66666666",
        "0:-:B8G8R8A8_UNORM:2560x1440:6001|1:-:R16_FLOAT:1280x720:6002", "",
        "0:R16G16B16A16_FLOAT:1280x720:6003", "-", "1:1280x720+0.5+0.5")
    row(12, "Draw", 4, 0, "0xB09A6D3F",
        "0:g_SamplerHierarchicalZ.T:R16_FLOAT:1280x720:6002|1:g_SamplerReflection.T:R16G16B16A16_FLOAT:1280x720:6003",
        "", "0:R16G16B16A16_FLOAT:1280x720:6005", "-", "1:1280x720+0.5+0.5")
    row(13, "DrawIndexed", 20, 0, "0x77777777",
        "0:g_SamplerReflectionMap.T:B8G8R8A8_UNORM:1280x720:6003|"
        "1:g_SamplerReflectionArray.T:B8G8R8A8_UNORM:256x256:6004", "0:g_WaterParameter:304:6006",
        "0:R16G16B16A16_FLOAT:2560x1440:6007", "-", "1:2560x1440+0.5+0.5")
    row(14, "Draw", 4, 1, "0x85E777EF",
        "0:-:R16G16B16A16_FLOAT:2560x1440:6008|1:-:R16G16B16A16_FLOAT:1024x1:6009",
        "0:-:16:6010|1:-:32:6011", "0:R16G16B16A16_FLOAT:2560x1440:6012", "-", "1:2560x1440+0.5+0.5", "tone")
    row(15, "Draw", 4, 0, "0x88888888", "0:-:R16G16B16A16_FLOAT:1280x720:6014",
        "0:g_CommonParameter:64:2002|2:cGlareParam:96:7003", "0:R16G16B16A16_FLOAT:1280x720:6013", "-",
        "1:1280x720+0.5+0.5")
    with open(folder / "draws.csv", "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write(DRAWS_HEADER + "\n")
        handle.write("\n".join(rows) + "\n")
        handle.write(f"# frame 1 draws {len(rows)} tone 14 ui1 0 upscale_after 0\n")
        handle.write(f"# rows {len(rows)} frames 1 omitted 0\n")

    with open(folder / "cb.csv", "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write(CB_HEADER + "\n")
        handle.write("0,10.000,0xD8CDA48A,g_LightParam,6,512,0,0.8,0.6,0,0.8,0.6,1,0.5,0.25,"
                     "0,0,-1,-0,-0.6,0.8,-0,-1.2,0.8,0.6,0,-3.4,0,-0.6,0.8,2,"
                     "0,0,-1,-0,0.6,0.8,0,1.2,0.8,0.6,0,3.4,0,0,-1,0\n")
        handle.write("0,11.000,0xD8CDA48A,g_LightParam,6,512,1,0.8,0.6,0,0.8,0.6,1,0.5,0.25,"
                     "0,0,-1,-0,-0.6,0.8,-0,-1.2,0.8,0.6,0,-3.4,0,-0.6,0.8,2,"
                     "0,0,-1,-0,0.6,0.8,0,1.2,0.8,0.6,0,3.4,0,0,-1,0\n")
        handle.write("# rows 2\n")

    cbdump_rows = [
        ("0", "10.000", "0x1", "g_WetnessParameter", "1", "32", "2001", wetness_bytes([wet_dry, 0.2, 0, 0, 0, 0, 0, 1.0])),
        ("0", "11.000", "0x1", "g_WetnessParameter", "1", "32", "2001", wetness_bytes([wet_second, 0.2, 0, 0, 0, 0, 0, 1.0])),
        ("0", "10.000", "0x2", "g_AmbientParamArray", "2", "16384", "7002",
         ambient_bytes(scale_probe, sky_visibility, 0.0, near_unit=(5, 1.0 + 5e-4))),
        ("0", "10.000", "0x3", "cGlareParam", "1", "96", "7003", struct.pack("<24f", *([0.5] * 24))),
        ("0", "11.000", "0x3", "cGlareParam", "1", "96", "7003", struct.pack("<24f", *([0.5] * 24))),
        ("0", "10.000", "0x4", "g_DirectionalShadowParameter", "4", "128", "7004", directional_shadow_bytes()
         + bytes(32)),
        ("0", "10.000", "0x5", "g_ReflectionParameter", "5", "80", "7005", reflection_bytes()),
        ("0", "10.000", "0x6", "cBrightPassParam", "1", "16", "7006", struct.pack("<4f", 0.75, 0.25, 0, 0)),
        ("0", "10.000", "0x7", "cNoiseTables0", "0", "16", "7007", struct.pack("<4f", 1, 2, 3, 4)),
    ]
    with open(folder / "cbdump.csv", "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write(CBDUMP_HEADER + "\n")
        for frame, seconds, crc, name, slot, size, pointer, data in cbdump_rows:
            handle.write(f"{frame},{seconds},{crc},{name},{slot},{size},{pointer},{data.hex()}\n")
        handle.write(f"# rows {len(cbdump_rows)}\n")

    with open(folder / "views.txt", "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, views, format 3\n")
        handle.write(VIEWS_HEADER + "\n")
        handle.write("g_SamplerReflectionArray,13,TEXTURECUBEARRAY,8,B8G8R8A8_UNORM,1,6,256,256,6,8,"
                     "B8G8R8A8_UNORM,6004\n")
        handle.write("g_SamplerReflectionMap,13,TEXTURE2D,1,B8G8R8A8_UNORM,1,0,1280,720,1,1,B8G8R8A8_UNORM,6003\n")
        handle.write("g_SamplerHierarchicalZ,12,TEXTURE2D,1,R16_FLOAT,1,0,1280,720,1,1,R16_FLOAT,6002\n")
        handle.write("g_SamplerShadow,6,TEXTURE2DARRAY,1,R16_UNORM,5,0,2048,10240,5,1,R16_UNORM,5001\n")


def _r11_word(red: float, green: float, blue: float) -> int:
    """Pack three values the way ``R11G11B10_FLOAT`` stores them: a 5-bit exponent (bias 15) and a mantissa.

    Only the values a fixture needs (a power of two, or one plus a power of two) are accepted, which keeps this encoder
    independent of the decoder's own arithmetic.
    """

    def field(value: float, mantissa_bits: int) -> int:
        if value == 0.0:
            return 0
        exponent = 0
        scaled = float(value)
        while scaled >= 2.0:
            scaled /= 2.0
            exponent += 1
        while scaled < 1.0:
            scaled *= 2.0
            exponent -= 1
        return ((exponent + 15) << mantissa_bits) | round((scaled - 1.0) * (1 << mantissa_bits))

    return field(red, 6) | (field(green, 6) << 11) | (field(blue, 5) << 22)


def write_dump(folder: Path) -> None:
    """A ``dump\\`` folder with one 4x4 texture per supported format, a padded one, a compressed one, a refused
    four-byte 16-bit depth, and one typeless spelling of each family."""

    dump = folder / drawdiag.DUMP_FOLDER
    dump.mkdir(parents=True, exist_ok=True)
    width = height = 4
    files: dict[str, bytes] = {
        "gbuffer0.bin": bytes([0, 0, 255, 255] * (width * height)),  # BGRA red
        "gbuffer1.bin": bytes([0, 255, 0, 255] * (width * height)),  # RGBA green
        "gbuffer2.bin": bytes([10, 20, 30, 255] * (width * height)),  # RGBA, typeless spelling below
        "depth.bin": np.linspace(0.0, 1.0, width * height, dtype=np.float32).tobytes(),
        "d32.bin": np.full(width * height, 0.25, dtype=np.float32).tobytes(),
        "d16.bin": np.full(width * height, 32767, dtype=np.uint16).tobytes(),
        "d16wide.bin": np.full(width * height, 16383, dtype=np.uint32).tobytes(),
        "d24.bin": np.full(width * height, 0x800000 | 5, dtype=np.uint32).tobytes(),
        "wetness.bin": bytes([128] * (width * height)),
        "weird.bin": np.full(width * height, _r11_word(1.0, 0.5, 0.25), dtype=np.uint32).tobytes(),
        "compressed.bin": bytes(64),
        "maskextra.bin": bytes([0, 0, 0, 64] * height + [1]),  # packed rows plus one trailing byte
    }
    # A single float channel with values a preview shows as they are, at its own pitch (packed, pitch 16).
    files["r16.bin"] = np.array([0.0, 0.25, 0.5, 1.0] * 4, dtype=np.float16).tobytes()
    scene = np.zeros((height, width, 4), dtype=np.float16)
    scene[:, :, 0] = 0.5
    scene[:, :, 1] = 1.0
    scene[:, :, 2] = 2.0
    scene[:, :, 3] = 1.0
    packed = scene.tobytes()
    files["scene.bin"] = b"".join(packed[index * width * 8:(index + 1) * width * 8] + b"\xaa" * 32
                                  for index in range(height))
    files["scene_typeless.bin"] = packed  # the same bytes, written as the typeless number
    motion = np.zeros((height, width, 2), dtype=np.float16)
    motion[:, :, 0] = 0.5
    motion[:, :, 1] = -0.5
    files["motion.bin"] = motion.tobytes()
    for name, data in files.items():
        (dump / name).write_bytes(data)
    facts = [
        f"gbuffer0.bin,B8G8R8A8_UNORM,{width}x{height},{4 * 4 * 4},16,1,1001",
        f"gbuffer1.bin,R8G8B8A8_UNORM_SRGB,{width}x{height},{4 * 4 * 4},16,1,1002",
        f"gbuffer2.bin,fmt27,{width}x{height},{4 * 4 * 4},16,1,1003",
        f"scene.bin,R16G16B16A16_FLOAT,{width}x{height},{4 * 4 * 8},64,2,3001",
        f"scene_typeless.bin,fmt9,{width}x{height},{4 * 4 * 8},32,2,3004",
        f"depth.bin,R32_FLOAT,{width}x{height},{4 * 4 * 4},16,3,6001",
        f"d32.bin,fmt40,{width}x{height},{4 * 4 * 4},16,3,6002",
        f"d16.bin,D16_UNORM,{width}x{height},{4 * 4 * 2},8,4,5001",
        f"d16wide.bin,fmt55,{width}x{height},{4 * 4 * 4},16,4,5002",
        f"d24.bin,D24_UNORM_S8_UINT,{width}x{height},{4 * 4 * 4},16,4,5003",
        f"motion.bin,R16G16_FLOAT,{width}x{height},{4 * 4 * 4},16,5,6003",
        f"wetness.bin,R8_UNORM,{width}x{height},{4 * 4},4,6,3002",
        f"maskextra.bin,R8_UNORM,{width}x{height},{4 * 4 + 1},4,6,3003",
        f"r16.bin,R16_FLOAT,{width}x{height},{4 * 4 * 2},16,7,6101",
        f"weird.bin,R11G11B10_FLOAT,{width}x{height},{4 * 4 * 4},16,7,6004",
        f"compressed.bin,BC7_UNORM,{width}x{height},64,16,7,6005",
    ]
    with open(dump / drawdiag.DUMP_FACTS_FILE, "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, one frame dump, format 3\n")
        handle.write("# file,format,size,bytes,row_pitch,draw_ordinal,pointer\n")
        handle.write("\n".join(facts) + "\n")
        handle.write("# skipped multisampled.bin: multisampled\n")


def write_minimal_capture(folder: Path, rows: list[str], *, frame_line: str = "", trailer: str | None = "") -> Path:
    """A capture holding exactly the rows a rule needs, so one behaviour can be pinned without the rest of the frame."""

    folder.mkdir(parents=True, exist_ok=True)
    lines = ["# FFXIV draw diagnostics, format 3", DRAWS_HEADER, *rows]
    if frame_line:
        lines.append(frame_line)
    if trailer is not None:
        lines.append(trailer)
    (folder / "draws.csv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return folder


def write_edge_captures(edge: Path) -> dict[str, Path]:
    """The small captures that pin one rule each: the frame split, the atlas shape, and the wetness pass."""

    folders: dict[str, Path] = {}
    gbuffer3 = "0:g_SamplerGBuffer3.T:R16G16B16A16_FLOAT:2560x1440:1004"
    view_position = "0:g_SamplerViewPosition.T:R16G16B16A16_FLOAT:2560x1440:1007"
    wetness_cb = "1:g_WetnessParameter:32:2001"
    one_target = "0:B8G8R8A8_UNORM:2560x1440:3001"

    # The row cap dropped rows of the window: the tone map is in the file, the rest of the frame is not.
    folders["b1-omitted"] = write_minimal_capture(
        edge / "b1-omitted",
        [draws_row(1, "Draw", 4, 0, "0x1", rtv=one_target),
         draws_row(2, "Draw", 4, 1, "0x85E777EF", rtv=one_target, markers="tone")],
        frame_line="# frame 1 draws 6000 tone 2 ui1 0 upscale_after 0 omitted 10",
        trailer="# rows 2 frames 1 omitted 10")

    # Two tone-map rows in one window that kept every row: the splitter closed at every second one.
    folders["b1-two-tone"] = write_minimal_capture(
        edge / "b1-two-tone",
        [draws_row(1, "Draw", 4, 0, "0x1", rtv=one_target),
         draws_row(2, "Draw", 4, 1, "0x85E777EF", rtv=one_target, markers="tone"),
         draws_row(3, "Draw", 4, 1, "0x85E777EF", rtv=one_target, markers="tone")],
        frame_line="# frame 1 draws 3 tone 2 ui1 0 upscale_after 0 omitted 0",
        trailer="# rows 3 frames 1 omitted 0")

    # A 4K main depth prepass next to the shadow atlas: its viewport is not a slice offset.
    folders["b7-prepas"] = write_minimal_capture(
        edge / "b7-prepas",
        [draws_row(1, "DrawIndexed", 12, 0, "0x2", dsv="D32_FLOAT:3840x2160:8001",
                   viewport="1:3840x2160+0.5+0.5"),
         draws_row(2, "DrawIndexed", 12, 0, "0x3", dsv="fmt55:2048x10240:5001",
                   viewport="1:2048x2048+0.5+0.5")],
        frame_line="# frame 1 draws 2 tone 0 ui1 0 upscale_after 0 omitted 0",
        trailer="# rows 2 frames 1 omitted 0")

    # The wetness pass writes a target no later row reads, and the window is not whole: not an answer.
    folders["p2-unread"] = write_minimal_capture(
        edge / "p2-unread",
        [draws_row(1, "Draw", 3, 0, "0xADE72072", srv=f"{gbuffer3}|{view_position}", cb=wetness_cb,
                   rtv=one_target)],
        frame_line="# frame 1 draws 6000 tone 0 ui1 0 upscale_after 0 omitted 25",
        trailer="# rows 1 frames 1 omitted 25")

    # A draw that binds the wetness buffer and the G-buffer but not the view position is not the pass.
    folders["wetness-partial"] = write_minimal_capture(
        edge / "wetness-partial",
        [draws_row(1, "Draw", 3, 0, "0x12345678", srv=gbuffer3, cb=wetness_cb, rtv=one_target)],
        frame_line="# frame 1 draws 1 tone 0 ui1 0 upscale_after 0 omitted 0",
        trailer="# rows 1 frames 1 omitted 0")

    # The glare chain's draws carry no name of their own (its shaders are created while the game loads, before the key can be
    # armed), so the capture can name them only through the same CRC's creation row in shaders.txt.
    glare = write_minimal_capture(
        edge / "glare-join",
        [draws_row(1, "Draw", 4, 0, "0xAE4A07BE", srv="0:-:R16G16B16A16_FLOAT:1920x1080:9001", cb="0:-:96:9002",
                   rtv="0:R16G16B16A16_FLOAT:1920x1080:9003", viewport="1:960x540+0.5+0.5"),
         draws_row(2, "Draw", 4, 1, "0x85E777EF", rtv=one_target, markers="tone")],
        frame_line="# frame 1 draws 2 tone 2 ui1 0 upscale_after 0 omitted 0",
        trailer="# rows 2 frames 1 omitted 0")
    write_shaders(glare, ["AE4A07BE,3388,creation,sInput.T=0,sViewPosition.T=1,sDepthPrev.T=2,sVelocity.T=3,"
                          "sGlareSourcePrev.T=4,cGlareParam=0"])
    with open(glare / "cbdump.csv", "w", encoding="utf-8", newline="") as handle:
        handle.write("# FFXIV draw diagnostics, format 3\n")
        handle.write(CBDUMP_HEADER + "\n")
        first_sample = struct.pack("<24f", *([0.5] * 24))
        second_sample = bytearray(first_sample)
        struct.pack_into("<f", second_sample, 0x40, 0.75)  # prevWeight: the one member that moves
        handle.write(f"0,10.000,0xAE4A07BE,cGlareParam,0,96,9002,{first_sample.hex()}\n")
        handle.write(f"0,11.000,0xAE4A07BE,cGlareParam,0,96,9002,{bytes(second_sample).hex()}\n")
        handle.write("# rows 2\n")
    folders["glare-join"] = glare
    return folders


def write_engine(folder: Path, *, module_on: bool = True, armed: bool = True, identity: bool = False,
                 runs: int = 0, declines: tuple[tuple[str, int], ...] = (),
                 window: tuple[int, int, tuple[tuple[str, int], ...]] | None = None,
                 last: tuple[str, str, str] | None = None) -> None:
    """One ``engine.txt``: the module's counters, field per line, in the shape the diagnostic writes it.

    ``window`` is (window runs, window declined, window decline lines); None writes a file that predates the window lines.
    """

    lines = ["# FFXIV draw diagnostics, engine pass overrides, format 3"]
    if not module_on:
        lines.append("module: off")
    else:
        lines += ["module: on", f"armed: {'yes' if armed else 'no'}", f"identity: {'yes' if identity else 'no'}",
                  f"runs: {runs}", f"declined: {sum(count for _, count in declines)}"]
        lines += [f"decline: {reason} = {count}" for reason, count in declines]
        if window is not None:
            window_runs, window_declined, window_declines = window
            lines += [f"window runs: {window_runs}", f"window declined: {window_declined}"]
            lines += [f"window decline: {reason} = {count}" for reason, count in window_declines]
        if last is None:
            lines += ["last bound bytes: -", "last target: -", "last window: -"]
        else:
            bound, target, window_text = last
            lines += [f"last bound bytes: {bound}", f"last target: {target}", f"last window: {window_text}"]
    (folder / "engine.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_shaders(folder: Path, rows: list[str], *, old_header: bool = False) -> None:
    """One ``shaders.txt``; ``old_header`` writes the two-column list a format-3 file carried before the origin."""

    if old_header:
        lines = ["# FFXIV draw diagnostics, shader fixtures, format 3", "# crc,size,name=bindPoint,..."]
    else:
        lines = ["# FFXIV draw diagnostics, shader fixtures, format 3", "# crc,size,origin,name=bindPoint,...",
                 "# origin: creation = kept when the shader was created, before any capture; capture = kept while a capture ran"]
    (folder / "shaders.txt").write_text("\n".join(lines + rows) + "\n", encoding="utf-8", newline="\n")


def write_session(root: Path) -> tuple[Path, Path]:
    """The two synthetic captures of the generated session."""

    first = root / "20261031-100000"
    second = root / "20261031-110000"
    write_capture(first, wet_dry=0.10, wet_second=0.12, scale_probe=(3, 2.0), sky_visibility=0.5)
    write_capture(second, wet_dry=0.60, wet_second=0.62, scale_probe=None, sky_visibility=0.9)
    write_dump(first)
    # The shaders kept from creation, an in-capture copy beside them, and the module's counters: the first capture's module never patched
    # a draw, the second's patched four.  The second capture's shader list is the older two-column shape, which reads as `capture`.
    write_shaders(first, ["ADE72072,8808,creation,g_WetnessParameter=4,g_SamplerGBuffer3=5,g_SamplerViewPosition=6",
                          "85E777EF,4096,creation,cGlareParam=0",
                          "01AA39D2,512,capture,g_MaterialParameter=0"])
    write_engine(first, runs=0, declines=(("the ambient buffer is neither 16,384 nor 12,352 bytes", 3),
                                          ("the draw does not write exactly one render target", 1)),
                 window=(0, 4, (("the ambient buffer is neither 16,384 nor 12,352 bytes", 3),
                                ("the draw does not write exactly one render target", 1))))
    write_shaders(second, ["11111111,64,g_LightParam=0"], old_header=True)
    write_engine(second, runs=4, declines=(("the draw is a character-preview replay", 1),),
                 window=(2, 1, (("the draw is a character-preview replay", 1),)),
                 last=("16384", "3840x2160", "0.5,0.5,3840x2160"))
    return first, second


# ---------------------------------------------------------------------------------------------------------------------
# The generated-capture checks
# ---------------------------------------------------------------------------------------------------------------------


def check_reader(suite: Suite, root: Path, first: Path, second: Path, edge: Path) -> None:
    capture = drawdiag.load_capture(str(first))
    draws = capture.draws
    suite.check("reader: draws loaded", draws is not None, "load_capture returned no draws.csv")
    if draws is None:
        return
    suite.equal("reader: format", draws.format, 3)
    suite.equal("reader: row count", len(draws.rows), 15)
    suite.equal("reader: window count", len(draws.windows), 1)
    window = draws.window(1)
    suite.check("reader: window present", window is not None)
    if window is not None:
        suite.equal("reader: window draws", window.draws, 15)
        suite.equal("reader: window tone", window.tone, 14)
        suite.equal("reader: window omitted", window.omitted, 0)
        suite.equal("reader: tone marker rows", window.tone_rows, [14])
    suite.check("reader: no notice on a complete file", not capture.all_notices(),
                f"unexpected notices: {[str(n) for n in capture.all_notices()]}")
    tone = draws.by_ordinal(14, frame=1)
    suite.check("reader: tone row found", bool(tone))
    if tone:
        suite.equal("reader: tone marker", sorted(tone[0].markers), ["tone"])
        suite.equal("reader: tone stage", tone[0].stage, 1)
        suite.equal("reader: tone crc", tone[0].ps_crc, 0x85E777EF)
    wetness = draws.by_ordinal(3, frame=1)
    suite.check("reader: wetness pass found", bool(wetness))
    if wetness:
        row = wetness[0]
        suite.equal("reader: wetness target format", row.rtvs[0].format if row.rtvs else None, "B8G8R8A8_UNORM")
        suite.equal("reader: wetness target size", str(row.rtvs[0].size) if row.rtvs else None, "2560x1440")
        suite.equal("reader: wetness target pointer", row.rtvs[0].pointer if row.rtvs else None, 0x3001)
        suite.equal("reader: wetness srv names (suffix stripped)", sorted(row.srv_names),
                    ["g_SamplerGBuffer3", "g_SamplerViewPosition"])
        suite.equal("reader: wetness srv pointer", row.srvs[0].pointer, 0x1004)
        suite.check("reader: unknown trailing field ignored", all(len(entry.raw.split(":")) >= 5 for entry in row.srvs))
        suite.check("reader: wetness binding found by base name", row.cb("g_WetnessParameter") is not None)
    gbuffer = draws.by_ordinal(1, frame=1)
    if gbuffer:
        suite.equal("reader: five targets", gbuffer[0].target_count, 5)
        suite.equal("reader: target slots", [entry.slot for entry in gbuffer[0].rtvs], [0, 1, 2, 3, 4])
        suite.equal("reader: gbuffer3 format", gbuffer[0].rtvs[3].format, "R16G16B16A16_FLOAT")
    shadow = draws.by_ordinal(9, frame=1)
    if shadow:
        suite.equal("reader: depth view format", shadow[0].dsv.format if shadow[0].dsv else None, "fmt55")
        suite.equal("reader: depth view size", str(shadow[0].dsv.size) if shadow[0].dsv else None, "2048x10240")
        suite.check("reader: shadow viewport top", shadow[0].viewport is not None
                    and abs(shadow[0].viewport.top - 6144.5) < 1e-6)
        suite.check("reader: depth-only row has no target", not shadow[0].rtvs)
    suite.check("reader: cb.csv loaded", capture.cb is not None and len(capture.cb.rows) == 2)
    if capture.cb and capture.cb.rows:
        sample = capture.cb.rows[0]
        suite.equal("reader: cb view matrix length", len(sample.view or ()), 16)
        suite.equal("reader: cb view-projection length", len(sample.view_projection or ()), 16)
        suite.equal("reader: cb view-projection w row", sample.view_projection[12:16] if sample.view_projection else None,
                    (0.0, 0.0, -1.0, 0.0))
        suite.equal("reader: cb crc", sample.ps_crc, 0xD8CDA48A)
        suite.equal("reader: cb direction", sample.direction, (0.0, 0.8, 0.6))
    suite.check("reader: cbdump loaded", capture.cbdump is not None and len(capture.cbdump.rows) == 9)
    if capture.cbdump:
        wet = capture.cbdump.of_name("g_WetnessParameter")
        suite.equal("reader: wetness samples", len(wet), 2)
        suite.check("reader: wetness sample pointer", wet and wet[0].pointer == 0x2001)
        suite.equal("reader: wetness sample bytes", wet[0].data[:4] if wet else b"", struct.pack("<f", 0.10))
        suite.check("reader: unknown buffer name kept",
                    any(row.name == "cNoiseTables0" for row in capture.cbdump.rows))
    suite.check("reader: views loaded", capture.views is not None and len(capture.views.samples) == 4)
    if capture.views:
        array = capture.views.one("g_SamplerReflectionArray")
        suite.check("reader: views array sample", array is not None)
        if array:
            suite.equal("reader: views dimension", array.view_dimension, "TEXTURECUBEARRAY")
            suite.equal("reader: views cubes", array.cubes, 6)
            suite.check("reader: views is an array", array.is_array)
    suite.check("reader: dump facts loaded", capture.dump is not None and len(capture.dump.facts) == 16)
    if capture.dump:
        suite.equal("reader: dump skipped line", [name for name, _ in capture.dump.skipped], ["multisampled.bin"])
        scene = next((fact for fact in capture.dump.facts if fact.file == "scene.bin"), None)
        suite.check("reader: scene fact present", scene is not None)
        if scene:
            suite.equal("reader: scene row pitch", scene.row_pitch, 64)
    suite.equal("reader: find_captures", [os.path.basename(folder) for folder in drawdiag.find_captures(str(root))],
                ["20261031-100000", "20261031-110000"])

    # A format-2 file still parses: rewrite the same rows without pointers and compare the counts.
    format2 = edge / "format2"
    format2.mkdir(exist_ok=True)
    lines = (first / "draws.csv").read_text(encoding="utf-8").splitlines()
    converted = ["# FFXIV draw diagnostics, format 2"]
    for line in lines:
        if line.startswith("# rows"):
            continue  # a format-2 file ends without the trailer, which is what the reader has to report
        if line.startswith("#") or line.startswith("frame,"):
            converted.append(line)
            continue
        fields = line.split(",")
        for column in (10, 11, 12):  # srv (slot:name:format:size), cb (slot:name:bytes), rtv (slot:format:size)
            keep = {10: 4, 11: 3, 12: 3}[column]
            entries = []
            for entry in fields[column].split("|") if fields[column] else []:
                entries.append(":".join(entry.split(":")[:keep]))
            fields[column] = "|".join(entries)
        if fields[13] != "-" and fields[13].count(":") >= 2:
            fields[13] = ":".join(fields[13].split(":")[:2])
        converted.append(",".join(fields))
    (format2 / "draws.csv").write_text("\n".join(converted) + "\n", encoding="utf-8", newline="\n")
    loaded2 = drawdiag.load_draws(str(format2 / "draws.csv"))
    suite.equal("reader: format 2 row count", len(loaded2.rows), 15)
    pointer_free = loaded2.by_ordinal(3, frame=1)
    suite.check("reader: format 2 has no pointer", pointer_free and pointer_free[0].rtvs
                and pointer_free[0].rtvs[0].pointer is None)
    suite.check("reader: format 2 trailer warning",
                any(notice.kind == drawdiag.KIND_TRAILER_MISSING for notice in loaded2.notices))

    # A cut copy (no trailer) and a wrong trailer are reported, not accepted.
    cut = edge / "cut-no-trailer"
    cut.mkdir(exist_ok=True)
    source = (first / "draws.csv").read_text(encoding="utf-8").splitlines()
    (cut / "draws.csv").write_text("\n".join(line for line in source if not line.startswith("# rows")) + "\n",
                                   encoding="utf-8", newline="\n")
    cut_draws = drawdiag.load_draws(str(cut / "draws.csv"))
    suite.check("reader: missing trailer reported",
                any(notice.kind == drawdiag.KIND_TRAILER_MISSING for notice in cut_draws.notices))
    wrong = edge / "cut-wrong-trailer"
    wrong.mkdir(exist_ok=True)
    (wrong / "draws.csv").write_text("\n".join("# rows 999 frames 1 omitted 0" if line.startswith("# rows") else line
                                               for line in source) + "\n", encoding="utf-8", newline="\n")
    wrong_draws = drawdiag.load_draws(str(wrong / "draws.csv"))
    suite.check("reader: wrong trailer reported",
                any(notice.kind == drawdiag.KIND_TRAILER_MISMATCH for notice in wrong_draws.notices))
    # A short entry and an odd hex row do not throw.
    short = edge / "cut-short"
    short.mkdir(exist_ok=True)
    broken = []
    for line in source:
        if line.startswith("1,1,"):  # the G-buffer row: leave one target entry with two fields only
            fields = line.split(",")
            parts = fields[12].split("|")
            parts[0] = ":".join(parts[0].split(":")[:2])
            fields[12] = "|".join(parts)
            line = ",".join(fields)
        broken.append(line)
    (short / "draws.csv").write_text("\n".join(broken) + "\n", encoding="utf-8", newline="\n")
    short_draws = drawdiag.load_draws(str(short / "draws.csv"))
    suite.equal("reader: broken entry dropped, rows kept", len(short_draws.rows), 15)
    suite.check("reader: broken entry leaves four targets",
                short_draws.by_ordinal(1, frame=1) and short_draws.by_ordinal(1, frame=1)[0].target_count == 4)
    cbdump_short = edge / "cbdump-short.csv"
    lines = [
        "# FFXIV draw diagnostics, format 3", CBDUMP_HEADER,
        "0,1.0,0x1,g_AmbientParamArray,2,16384,7002," + ambient_bytes()[:1000].hex(),
        "0,1.0,0x2,g_WetnessParameter,1,32,2001,abc",
        "# rows 2",
    ]
    cbdump_short.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    loaded_short = drawdiag.load_cbdump(str(cbdump_short))
    suite.equal("reader: short cbdump rows", len(loaded_short.rows), 2)
    suite.check("reader: odd hex reported", any(notice.kind == drawdiag.KIND_HEX for notice in loaded_short.notices))
    suite.check("reader: short sample flagged", loaded_short.rows[0].short)
    decoded_short = cblayouts.decode("g_AmbientParamArray", loaded_short.rows[0].data)
    suite.equal("reader: short ambient probes", decoded_short["probe_available"], 5)
    suite.equal("reader: short ambient probe tail", decoded_short["probe_short"], [4])
    suite.equal("reader: short hex bytes", len(loaded_short.rows[1].data), 1)

    # Name handling and small parsers.
    suite.equal("names: .T stripped", drawdiag.base_texture_name("g_SamplerDepth.T"), "g_SamplerDepth")
    suite.equal("names: .S stripped", drawdiag.base_texture_name("g_SamplerDepth.S"), "g_SamplerDepth")
    suite.equal("names: plain stays", drawdiag.base_texture_name("g_LightParam"), "g_LightParam")
    suite.equal("sizes: parse", str(drawdiag.parse_size("2560x1440")), "2560x1440")
    suite.check("sizes: '-' is no size", drawdiag.parse_size("-") is None)
    suite.check("sizes: '?' is no size", drawdiag.parse_size("?") is None)
    viewport = drawdiag.parse_viewport("1:2048x2048+0.5+8192.5")
    suite.check("viewport: parsed", viewport is not None)
    if viewport:
        suite.almost("viewport: top", viewport.top, 8192.5)
        suite.equal("viewport: height", viewport.height, 2048)
    # The leading number is the count of viewports, not a slot, and a multi-viewport draw lists its rectangles joined
    # by '|'.  Reading the count as a slot happens to work for a single viewport and silently loses the rest.
    several = drawdiag.parse_viewports("2:2560x1440+0.5+0.5|1280x720+0+0")
    suite.check("viewport: several parsed", several is not None)
    if several:
        suite.equal("viewport: count kept as a count", several.count, 2)
        suite.equal("viewport: rectangle count", len(several.rects), 2)
        suite.equal("viewport: second rectangle width", several.rects[1].width, 1280)
        suite.equal("viewport: first rectangle is the row's viewport", several.first, several.rects[0])
        suite.check("viewport: count agrees with the rectangles", several.consistent)
    wrong_count = drawdiag.parse_viewports("3:100x50+1+2|10x10+0+0")
    suite.check("viewport: a wrong count keeps the rectangles", wrong_count is not None and len(wrong_count.rects) == 2
                and not wrong_count.consistent)
    partial = drawdiag.parse_viewports("2:100x50+1+2|junk|10x10+0+0")
    suite.check("viewport: a damaged rectangle is dropped, the rest kept",
                partial is not None and len(partial.rects) == 2)
    suite.equal("viewport: no column is none", drawdiag.parse_viewports("-"), None)
    suite.equal("viewport: empty column is none", drawdiag.parse_viewports(""), None)
    suite.equal("pointers: '0' is none", drawdiag.parse_pointer("0"), None)
    suite.equal("pointers: hex read", drawdiag.parse_pointer("1a2b"), 0x1A2B)
    suite.equal("pointers: 0x read", drawdiag.parse_pointer("0x1A2B"), 0x1A2B)
    # facts.txt: the shape this reader documents, and the older one that wrote the size as two fields.  The older shape
    # shifts every column after the size, and a reader that forgets the shift hands the width to the byte count.
    old_facts = edge / "facts-old"
    old_facts.mkdir(exist_ok=True)
    (old_facts / drawdiag.DUMP_FACTS_FILE).write_text(
        "# FFXIV draw diagnostics, one frame dump, format 3\n"
        "# file,format,size,bytes,row_pitch,draw_ordinal,pointer\n"
        "colour.bin,B8G8R8A8_UNORM,4x4,64,16,7,1a2b\n"
        "older.bin,B8G8R8A8_UNORM,4,4,64,16,8,2b3c\n",
        encoding="utf-8", newline="\n")
    mixed = drawdiag.load_dump_facts(str(old_facts / drawdiag.DUMP_FACTS_FILE))
    suite.equal("facts: both shapes read", len(mixed.facts), 2)
    if len(mixed.facts) == 2:
        current, older = mixed.facts
        suite.equal("facts: current shape size", (current.width, current.height, current.bytes),
                    (4, 4, 64))
        suite.equal("facts: old shape size", (older.width, older.height), (4, 4))
        suite.equal("facts: old shape bytes (not the width)", older.bytes, 64)
        suite.equal("facts: old shape pitch", older.row_pitch, 16)
        suite.equal("facts: old shape ordinal", older.ordinal, 8)
        suite.equal("facts: old shape pointer", older.pointer, 0x2B3C)
    suite.equal("secondary capture loads", len(drawdiag.load_capture(str(second)).rows), 15)


def check_layouts(suite: Suite) -> None:
    wetness = cblayouts.layout_of("g_WetnessParameter")
    suite.check("layouts: wetness known", wetness is not None)
    if wetness:
        suite.equal("layouts: wetness declared", wetness.size, 32)
        suite.equal("layouts: wetness members", len(wetness.members), 8)
    suite.equal("layouts: ambient declared", cblayouts.AMBIENT_PARAM_ARRAY.size, 12352)
    suite.equal("layouts: ambient bound", cblayouts.AMBIENT_PARAM_ARRAY.bound, 16384)
    suite.equal("layouts: ambient probes", cblayouts.AMBIENT_PARAM_ARRAY.probe_count, 64)
    suite.equal("layouts: probe size", cblayouts.AMBIENT_PROBE.size, 192)
    offsets = {
        "g_AmbientParamArray": {"paramNum": 0x00, "skyShRecipScale": 0x04, "skySh": 0x10},
        "g_DirectionalShadowParameter": {"m_ShadowProjectionMatrix": 0x00, "m_ShadowDistance": 0x40,
                                         "m_ShadowMapParameter": 0x50},
        "g_ReflectionParameter": {"m_PixelSize": 0x00, "m_FadeDistance": 0x10, "m_NormalBlendWeight": 0x20,
                                  "m_ReflectionPowerScale": 0x24, "m_MipLevel": 0x28,
                                  "m_RoughnessThreshold": 0x2C, "m_DynamicViewportResolution": 0x30,
                                  "m_EnableAdjustSpecularMask": 0x48},
        "g_LightParam": {"m_Position": 0x00, "m_Direction": 0x10, "m_DiffuseColor": 0x20, "m_SpecularColor": 0x30,
                         "m_Attenuation": 0x40, "m_ClipMin": 0x50, "m_ClipMax": 0x60, "m_PlaneFadeScale": 0x70,
                         "m_ShadowTexMask": 0x80, "m_PlaneRayDirection": 0x90, "m_PlaneInversMatrix": 0xA0,
                         "m_WorldViewInversMatrix": 0xD0, "m_LightMapMatrix": 0x100,
                         "m_WorldViewProjectionMatrix": 0x140, "m_LightFadeValueStatic": 0x180,
                         "m_LightFadeValueDynamic": 0x184},
    }
    for layout_name, members in offsets.items():
        layout = cblayouts.LAYOUTS[layout_name]
        for member_name, offset in members.items():
            member = layout.member(member_name)
            suite.check(f"layouts: {layout_name}.{member_name} offset",
                        member is not None and member.offset == offset,
                        f"{getattr(member, 'offset', None)!r} != {offset:#x}")
    probe_members = {
        "sh": 0x00, "color": 0x30, "shRecipScale": 0x3C, "attenuationSlope": 0x40,
        "attenuationIntercept": 0x44, "attenuationValue": 0x48, "skyVisibility": 0x4C, "reflectionScale": 0x50,
        "reflectionOffset": 0x54, "bakeLightRate": 0x58, "envLocationIndex": 0x5C, "invMatrix": 0x60,
        "interpInvRangeMinus": 0x90, "interpInvRangePlus": 0x9C, "priority": 0xA8, "envSpaceType": 0xAC,
        "skyVisibilityScale": 0xB0, "ssaoMask": 0xB4,
    }
    for member_name, offset in probe_members.items():
        member = cblayouts.AMBIENT_PROBE.member(member_name)
        suite.check(f"layouts: probe.{member_name} offset", member is not None and member.offset == offset,
                    f"{getattr(member, 'offset', None)!r} != {offset:#x}")
    for name, size in (("cBrightPassParam", 16), ("cMergeWeight", 16), ("cGlareParam", 96), ("cSoftFocusParam", 16),
                       ("cToneMapParam", 32), ("cCommonTexParam", 16), ("cFilterForDark", 48), ("cDarkFilterParam", 16),
                       ("cFogParam", 128)):
        layout = cblayouts.LAYOUTS[name]
        suite.equal(f"layouts: {name} size", layout.size, size)
    for name in ("cBrightPassParam", "cMergeWeight", "cToneMapParam", "cCommonTexParam", "cFilterForDark",
                 "cDarkFilterParam", "cFogParam"):
        suite.check(f"layouts: {name} is flat", cblayouts.LAYOUTS[name].flat)
    # The glare chain's two parameter structs carry the member names of their own shaders' reflection: the accent member is
    # the one a patch or an A/B has to name, and the three float2 uv ranges are what makes cGlareParam end on 96 B.
    glare_members = {"prevProjToView": 0x00, "prevWeight": 0x40, "prevRangeScale": 0x44, "uvToPrevUvRange": 0x48,
                     "invPrevUvRange": 0x50, "currentUvPixelRange": 0x58}
    for member_name, offset in glare_members.items():
        member = cblayouts.LAYOUTS["cGlareParam"].member(member_name)
        suite.check(f"layouts: cGlareParam.{member_name} offset", member is not None and member.offset == offset,
                    f"{getattr(member, 'offset', None)!r} != {offset:#x}")
    suite.equal("layouts: cGlareParam ends on its declared size", cblayouts.LAYOUTS["cGlareParam"].end, 96)
    suite.check("layouts: cGlareParam is not flat", not cblayouts.LAYOUTS["cGlareParam"].flat)
    soft_members = {"currentUvPixelRange": 0x00, "softFocusRate": 0x08, "glareCompositeRate": 0x0C}
    for member_name, offset in soft_members.items():
        member = cblayouts.LAYOUTS["cSoftFocusParam"].member(member_name)
        suite.check(f"layouts: cSoftFocusParam.{member_name} offset", member is not None and member.offset == offset,
                    f"{getattr(member, 'offset', None)!r} != {offset:#x}")
    suite.equal("layouts: cSoftFocusParam ends on its declared size", cblayouts.LAYOUTS["cSoftFocusParam"].end, 16)
    suite.check("layouts: cSoftFocusParam is not flat", not cblayouts.LAYOUTS["cSoftFocusParam"].flat)

    buffer = ambient_bytes(probe_scale=(3, 2.0), sky_visibility=0.4, sky_sh=0.125)
    decoded = cblayouts.decode("g_AmbientParamArray", buffer)
    suite.equal("decode: ambient known", decoded["known"], True)
    suite.equal("decode: ambient paramNum", decoded["members"]["paramNum"], 64)
    suite.almost("decode: ambient skyShRecipScale", decoded["members"]["skyShRecipScale"], 0.25)
    suite.almost("decode: ambient header sh", decoded["members"]["skySh"][0], 0.125)
    suite.equal("decode: ambient probe count", decoded["probe_available"], 64)
    suite.equal("decode: ambient probe short list", decoded["probe_short"], [])
    suite.almost("decode: probe 0 skyVisibility", decoded["probes"][0]["members"]["skyVisibility"], 0.4)
    suite.almost("decode: probe 3 scale", decoded["probes"][3]["members"]["skyVisibilityScale"], 2.0)
    suite.almost("decode: probe 4 scale", decoded["probes"][4]["members"]["skyVisibilityScale"], 1.0)
    suite.equal("decode: probe envSpaceType", decoded["probes"][32]["members"]["envSpaceType"], 2)
    suite.almost("decode: probe color x", decoded["probes"][5]["members"]["color"][0], 0.125 + 5)
    wet = cblayouts.decode("g_WetnessParameter", wetness_bytes([0.1, 0.2, 0.3, 0, 0, 0, 0, 1.0]))
    suite.almost("decode: wetness f0", wet["members"]["f0"], 0.1)
    suite.almost("decode: wetness f2", wet["members"]["f2"], 0.3)
    empty = cblayouts.decode("g_WetnessParameter", b"")
    suite.equal("decode: empty wetness has no member", empty["members"], {})
    suite.equal("decode: empty wetness short list", empty["short"], ["f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"])
    suite.equal("decode: empty ambient probes", cblayouts.decode("g_AmbientParamArray", b"")["probe_available"], 0)
    light = cblayouts.decode("g_LightParam", struct.pack("<12f", *range(12)) + bytes(512 - 48))
    suite.equal("decode: light position", light["members"]["m_Position"], [0.0, 1.0, 2.0])
    suite.equal("decode: light direction", light["members"]["m_Direction"], [4.0, 5.0, 6.0])
    unknown = cblayouts.decode("cNoiseTables0", struct.pack("<4f", 1, 2, 3, 4))
    suite.equal("decode: unknown known flag", unknown["known"], False)
    suite.equal("decode: unknown floats", unknown["floats"], [1.0, 2.0, 3.0, 4.0])
    directional = cblayouts.decode("g_DirectionalShadowParameter", directional_shadow_bytes())
    suite.almost("decode: shadow distance", directional["members"]["m_ShadowDistance"][0], 10.0)
    suite.almost("decode: shadow map parameter", directional["members"]["m_ShadowMapParameter"][0], 0.5)
    suite.equal("decode: shadow tail is short when 96 B", cblayouts.decode("g_DirectionalShadowParameter",
                                                                          bytes(96))["short"], ["undeclaredTail"])
    reflection = cblayouts.decode("g_ReflectionParameter", reflection_bytes())
    suite.almost("decode: reflection mip", reflection["members"]["m_MipLevel"], 3.0)
    suite.almost("decode: reflection roughness", reflection["members"]["m_RoughnessThreshold"], 0.0625)
    flat = cblayouts.decode("cToneMapParam", struct.pack("<8f", *range(8)))
    suite.equal("decode: flat float count", len(flat["floats"]), 8)
    suite.almost("decode: flat last float", flat["floats"][-1], 7.0)
    glare = cblayouts.decode("cGlareParam", struct.pack("<24f", *range(24)))
    suite.equal("decode: glare prevWeight", glare["members"]["prevWeight"], 16.0)
    suite.equal("decode: glare uvToPrevUvRange", glare["members"]["uvToPrevUvRange"], [18.0, 19.0])
    suite.equal("decode: glare currentUvPixelRange", glare["members"]["currentUvPixelRange"], [22.0, 23.0])
    soft = cblayouts.decode("cSoftFocusParam", struct.pack("<4f", 0.5, 0.25, 0.75, 0.125))
    suite.equal("decode: soft focus currentUvPixelRange", soft["members"]["currentUvPixelRange"], [0.5, 0.25])
    suite.almost("decode: soft focus glareCompositeRate", soft["members"]["glareCompositeRate"], 0.125)

    # The members whose name says index/count/type but whose declared type the studies do not record: four bytes that
    # hold a float also read as a huge integer, so the float reading is shown next to it from 2^24 on.
    for layout_name, member_name in (("g_AmbientParamArray", "paramNum"),
                                     ("g_AmbientParamArray.probe", "envLocationIndex"),
                                     ("g_AmbientParamArray.probe", "envSpaceType")):
        layout = cblayouts.AMBIENT_PROBE if layout_name.endswith(".probe") else cblayouts.LAYOUTS[layout_name]
        member = layout.member(member_name)
        suite.check(f"layouts: {member_name} has a float reading",
                    member is not None and member.float_too and member.kind == "uint")
    as_float = cblayouts.decode("g_AmbientParamArray", ambient_bytes(param_num_float=-1.0))
    suite.equal("decode: paramNum integer reading of -1.0f", as_float["members"]["paramNum"], 0xBF800000)
    suite.almost("decode: paramNum float reading", as_float["members"]["paramNum_f32"], -1.0)
    probe_float = bytearray(ambient_bytes())
    struct.pack_into("<f", probe_float, cblayouts.AMBIENT_PARAM_ARRAY.probe_offset + 0x5C, -0.5)
    probe_decoded = cblayouts.decode("g_AmbientParamArray", bytes(probe_float))
    suite.almost("decode: envLocationIndex float reading", probe_decoded["probes"][0]["members"]["envLocationIndex_f32"],
                 -0.5)
    suite.equal("decode: a small integer reading gets no float twin",
                "envSpaceType_f32" in cblayouts.decode("g_AmbientParamArray", ambient_bytes())["probes"][0]["members"],
                False)


def check_cbdump_report(suite: Suite, first: Path) -> None:
    capture = drawdiag.load_capture(str(first))
    report = cbdump_report.analyse_capture(capture)
    names = [buffer.name for buffer in report.buffers]
    for expected in ("g_WetnessParameter", "g_AmbientParamArray", "cGlareParam", "g_DirectionalShadowParameter",
                     "g_ReflectionParameter", "cBrightPassParam", "cNoiseTables0"):
        suite.check(f"cbdump report: {expected} section", expected in names)
    wetness = report.buffer("g_WetnessParameter")
    suite.check("cbdump report: wetness buffer present", wetness is not None)
    if wetness:
        changing = [stat.name for stat in wetness.stats if stat.changed]
        suite.equal("cbdump report: wetness changing floats", changing, ["f0"])
        suite.equal("cbdump report: wetness pointer constant", wetness.pointer_constant, True)
        suite.equal("cbdump report: wetness samples", wetness.samples, 2)
    ambient = report.buffer("g_AmbientParamArray")
    suite.check("cbdump report: ambient buffer present", ambient is not None)
    if ambient:
        suite.check("cbdump report: ambient verdict", any("63 of 64" in verdict for verdict in ambient.verdicts),
                    str(ambient.verdicts))
        suite.check("cbdump report: ambient probe rows", len(ambient.extra_lines) >= 3,
                    str(len(ambient.extra_lines)))
    text = cbdump_report.render(report)
    suite.contains("cbdump report: wetness table", text, "g_WetnessParameter")
    suite.contains("cbdump report: wetness pointer line", text, "pointer across the frame's draws: constant (2001)")
    suite.contains("cbdump report: wetness pass line", text, "fullscreen wetness pass(es): 1 row(s)")
    suite.contains("cbdump report: unit verdict", text, "skyVisibilityScale ~= 1.0 in 63 of 64 probe(s)")
    suite.contains("cbdump report: the tolerance accepts a near-unit probe", text, "not unit at probe 3..3")
    suite.contains("cbdump report: changing members", text, "changing members: f0")
    suite.contains("cbdump report: glare stability", text, "cGlareParam")
    suite.contains("cbdump report: unknown buffer", text, "no member table for this name")
    out = Path(tempfile.mkdtemp(prefix="cbdump-report-"))
    try:
        path = out / "report.txt"
        with contextlib.redirect_stdout(io.StringIO()):
            code = cbdump_report.main([str(first), "--out", str(path)])
        suite.equal("cbdump report: exit code", code, 0)
        suite.check("cbdump report: file written", path.is_file() and path.stat().st_size > 0)
    finally:
        shutil.rmtree(out, ignore_errors=True)
    # A capture with draws.csv only: the report says so and still returns a produced text.
    bare = Path(tempfile.mkdtemp(prefix="drawdiag-bare-"))
    try:
        (bare / "draws.csv").write_text((first / "draws.csv").read_text(encoding="utf-8"), encoding="utf-8",
                                        newline="\n")
        bare_capture = drawdiag.load_capture(str(bare))
        bare_report = cbdump_report.analyse_capture(bare_capture)
        suite.check("cbdump report: empty capture text", "nothing to decode" in cbdump_report.render(bare_report))
        with contextlib.redirect_stdout(io.StringIO()):
            suite.equal("cbdump report: empty capture exit code", cbdump_report.main([str(bare)]), 2)
    finally:
        shutil.rmtree(bare, ignore_errors=True)


def check_dump(suite: Suite, first: Path) -> None:
    capture = drawdiag.load_capture(str(first))
    out = Path(tempfile.mkdtemp(prefix="drawdiag-png-"))
    try:
        lines = dump_to_png.convert_capture(capture, str(out), exposure=2.0, channels=True)
        text = "\n".join(lines)
        suite.contains("dump: skipped line", text, "multisampled.bin: skipped by the dump")
        suite.contains("dump: a compressed format is refused", text,
                       "compressed.bin: format 'BC7_UNORM' is not one this tool decodes")
        suite.contains("dump: a four-byte 16-bit depth is refused", text,
                       "d16wide.bin: D16_UNORM written with four bytes per texel")
        suite.check("dump: no png for a refused file", not (out / "d16wide.png").is_file())
        for name in ("gbuffer0", "gbuffer1", "gbuffer2", "scene", "scene_typeless", "depth", "d32", "d16", "d24",
                     "motion", "wetness", "maskextra", "r16", "weird"):
            suite.check(f"dump: {name}.png written", (out / f"{name}.png").is_file())
            if (out / f"{name}.png").is_file():
                width, height, channels, _ = dump_to_png.read_png(str(out / f"{name}.png"))
                suite.equal(f"dump: {name}.png size", (width, height), (4, 4))
                suite.check(f"dump: {name}.png channels", channels in (1, 3, 4))
        if (out / "gbuffer0_r.png").is_file():
            _, _, _, pixels = dump_to_png.read_png(str(out / "gbuffer0_r.png"))
            suite.equal("dump: bgra red at channel r", pixels[0], 255)
            _, _, _, green = dump_to_png.read_png(str(out / "gbuffer0_g.png"))
            suite.equal("dump: bgra red at channel g", green[0], 0)
        if (out / "scene.raw.npy").is_file():
            raw = np.load(str(out / "scene.raw.npy"))
            suite.equal("dump: scene npy shape", raw.shape, (4, 4, 4))
            suite.almost("dump: scene npy value", float(raw[0, 0, 0]), 0.5)
        facts = {fact.file: fact for fact in capture.dump.facts}
        scene_path = first / drawdiag.DUMP_FOLDER / "scene.bin"
        scene = dump_to_png.decode(facts["scene.bin"], scene_path.read_bytes())
        suite.equal("dump: scene stride", scene.stride, 64)
        suite.equal("dump: scene packed flag", scene.packed, False)
        suite.check("dump: scene row read", np.allclose(np.array(scene.values[0, 0], dtype=np.float32),
                                                        np.array([0.5, 1.0, 2.0, 1.0], dtype=np.float32)))
        wide_fact = facts["d16wide.bin"]
        try:
            dump_to_png.decode(wide_fact, (first / drawdiag.DUMP_FOLDER / "d16wide.bin").read_bytes())
            suite.check("dump: four-byte 16-bit depth refused", False, "decode returned a preview instead")
        except ValueError as error:
            suite.contains("dump: four-byte 16-bit depth says why", str(error), "four bytes per texel")
            suite.check("dump: four-byte 16-bit depth refused", True)
        two_byte = dump_to_png.decode(facts["d16.bin"], (first / drawdiag.DUMP_FOLDER / "d16.bin").read_bytes())
        suite.equal("dump: a two-byte 16-bit depth still decodes", int(two_byte.values[0, 0]), 32767)
        # R16_FLOAT: one float channel, shown as a value and not tone-mapped, plus the raw array.
        r16 = dump_to_png.decode(facts["r16.bin"], (first / drawdiag.DUMP_FOLDER / "r16.bin").read_bytes())
        suite.equal("dump: R16_FLOAT values", [round(float(value), 3) for value in r16.values.ravel()[:4]],
                    [0.0, 0.25, 0.5, 1.0])
        suite.equal("dump: R16_FLOAT black pixel", dump_to_png.preview_rgb(r16)[0, 0].tolist(), [0, 0, 0])
        suite.equal("dump: R16_FLOAT white pixel", dump_to_png.preview_rgb(r16)[0, 3].tolist(), [255, 255, 255])
        suite.contains("dump: R16_FLOAT range line", dump_to_png.summary(r16), "range 0..1")
        suite.check("dump: a packed file with a recorded pitch is noted",
                    any("packed (32 B) although facts.txt records row pitch 16" in note for note in r16.notes),
                    str(r16.notes))
        # A packed file with a byte after its rows is not described as packed-although-pitched.
        extra = dump_to_png.decode(facts["maskextra.bin"], (first / drawdiag.DUMP_FOLDER / "maskextra.bin").read_bytes())
        suite.check("dump: trailing bytes are not read as a pitch mismatch",
                    not any("although facts.txt records" in note for note in extra.notes), str(extra.notes))
        suite.check("dump: trailing bytes noted",
                    any("1 trailing byte(s) ignored" in note for note in extra.notes), str(extra.notes))
        # R11G11B10_FLOAT: three unsigned float channels, decoded from the packing itself.
        weird_fact = facts["weird.bin"]
        weird = dump_to_png.decode(weird_fact, (first / drawdiag.DUMP_FOLDER / "weird.bin").read_bytes())
        suite.equal("dump: R11G11B10 decoded values", [round(float(value), 6) for value in weird.values[0, 0]],
                    [1.0, 0.5, 0.25])
        suite.equal("dump: R11G11B10 preview red", dump_to_png.preview_rgb(weird)[0, 0].tolist()[0], 255)
        suite.equal("dump: R11G11B10 preview green (sRGB of 0.5)",
                    dump_to_png.preview_rgb(weird)[0, 0].tolist()[1], 188)
        suite.equal("dump: R11G11B10 preview blue (sRGB of 0.25)",
                    dump_to_png.preview_rgb(weird)[0, 0].tolist()[2], 137)
        suite.check("dump: R11G11B10 raw array written", (out / "weird.raw.npy").is_file())
        if (out / "weird.raw.npy").is_file():
            suite.almost("dump: R11G11B10 raw value", float(np.load(str(out / "weird.raw.npy"))[0, 0, 2]), 0.25)
        # A typeless format reads as the concrete twin of its own width.
        typeless = dump_to_png.decode(facts["scene_typeless.bin"],
                                      (first / drawdiag.DUMP_FOLDER / "scene_typeless.bin").read_bytes())
        suite.equal("dump: typeless f16x4 reads as its twin", typeless.fmt.name, "R16G16B16A16_FLOAT")
        suite.almost("dump: typeless f16x4 value", float(typeless.values[0, 0, 2]), 2.0)
        suite.contains("dump: typeless format says why", typeless.fmt.note, "typeless")
        suite.contains("dump: typeless format named in the summary", dump_to_png.summary(typeless), "typeless")
        gbuffer2 = dump_to_png.decode(facts["gbuffer2.bin"],
                                      (first / drawdiag.DUMP_FOLDER / "gbuffer2.bin").read_bytes())
        suite.equal("dump: fmt27 reads as RGBA UNORM", gbuffer2.fmt.name, "R8G8B8A8_UNORM")
        suite.equal("dump: fmt27 pixels", gbuffer2.values[0, 0].tolist(), [10, 20, 30, 255])
        pixel = dump_to_png.preview_rgb(dump_to_png.decode(facts["motion.bin"],
                                                           (first / drawdiag.DUMP_FOLDER / "motion.bin").read_bytes()))
        suite.equal("dump: motion preview pixel", pixel[0, 0].tolist(), [191, 64, 0])
        depth = dump_to_png.preview_rgb(dump_to_png.decode(facts["d24.bin"],
                                                           (first / drawdiag.DUMP_FOLDER / "d24.bin").read_bytes()))
        suite.equal("dump: d24 preview pixel", depth[0, 0].tolist(), [128, 128, 128])
        wetness = dump_to_png.preview_rgb(dump_to_png.decode(facts["wetness.bin"],
                                                               (first / drawdiag.DUMP_FOLDER / "wetness.bin").read_bytes()))
        suite.equal("dump: r8 preview pixel", wetness[0, 0].tolist(), [128, 128, 128])
        scene_pixel = dump_to_png.preview_rgb(dump_to_png.decode(facts["scene.bin"],
                                                                  (first / drawdiag.DUMP_FOLDER / "scene.bin").read_bytes()),
                                               exposure=2.0)[0, 0]
        suite.equal("dump: f16 preview clamps after exposure", scene_pixel.tolist(), [255, 255, 255])
        suite.equal("dump: fmt55 resolves", dump_to_png.resolve_format("fmt55").name, "D16_UNORM")
        for spelling, expected in (("fmt9", "R16G16B16A16_FLOAT"), ("R16G16B16A16_TYPELESS", "R16G16B16A16_FLOAT"),
                                   ("fmt33", "R16G16_FLOAT"), ("fmt39", "R32_FLOAT"), ("fmt44", "D24_UNORM_S8_UINT"),
                                   ("R24G8_TYPELESS", "D24_UNORM_S8_UINT"), ("fmt48", "R8G8_UNORM"),
                                   ("fmt60", "R8_UNORM"), ("fmt53", "R16_FLOAT")):
            resolved = dump_to_png.resolve_format(spelling)
            suite.equal(f"dump: typeless {spelling} twin", resolved.name if resolved else None, expected)
        suite.check("dump: a format it cannot read resolves to None",
                    dump_to_png.resolve_format("BC7_UNORM") is None)
        suite.check("dump: a 16-byte float format is still out of scope",
                    dump_to_png.resolve_format("R32G32B32A32_FLOAT") is None)
        short = (first / drawdiag.DUMP_FOLDER / "scene.bin").read_bytes()[:40]
        cut = dump_to_png.decode(facts["scene.bin"], short)
        suite.check("dump: short file noted", any("cut short" in note for note in cut.notes))
        suite.equal("dump: short file keeps its shape", cut.values.shape, (4, 4, 4))
        # The formats the tool must know, by name and by number.
        for name in ("B8G8R8A8_UNORM", "R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB", "R16G16B16A16_FLOAT",
                     "R16_FLOAT", "R11G11B10_FLOAT", "R32_FLOAT", "D32_FLOAT", "D16_UNORM", "D24_UNORM_S8_UINT",
                     "R16G16_FLOAT", "R8_UNORM"):
            suite.check(f"dump: format {name}", dump_to_png.resolve_format(name) is not None)
    finally:
        shutil.rmtree(out, ignore_errors=True)


def check_session(suite: Suite, root: Path, first: Path, second: Path) -> None:
    capture = drawdiag.load_capture(str(first))
    other = drawdiag.load_capture(str(second))
    answers = session_answers.answer_capture(capture)
    status = {answer.key: answer.status for answer in answers.answers}
    for key, expected in (("P1", "O"), ("P2", "O"), ("P4", "O"), ("P5", "O"), ("B1", "O"), ("B3", "O"),
                          ("B7", "O"), ("GB", "O"), ("LB", "O"), ("EP", "U")):
        suite.equal(f"session: {key} status", status.get(key), expected)
    p2 = answers.by_key("P2")
    suite.check("session: P2 readers", p2 is not None
                and any("reads it at slot" in line for line in p2.evidence + p2.details),
                str(p2.evidence + p2.details if p2 else None))
    p1 = answers.by_key("P1")
    suite.check("session: P1 pointer constant", p1 is not None
                and any("one pointer for all of them (2001)" in line for line in p1.evidence))
    suite.check("session: P1 pointer per frame", p1 is not None
                and any(line.startswith("buffer pointer per frame: frame 0: one pointer (2001)")
                        for line in p1.evidence), str(p1.evidence if p1 else None))
    suite.check("session: P1 whole-capture pointer line", p1 is not None
                and any(line.startswith("buffer pointer over the whole capture: constant") for line in p1.evidence))
    p4 = answers.by_key("P4")
    suite.check("session: P4 producer join", p4 is not None
                and any("producer(s) of the reflection map by pointer" in line for line in p4.evidence))
    p5 = answers.by_key("P5")
    suite.check("session: P5 cube array", p5 is not None
                and any("TEXTURECUBEARRAY" in line for line in p5.evidence))
    b2 = answers.by_key("B2")
    suite.check("session: B2 stable glare", b2 is not None and b2.status == "O"
                and any("stable across the samples" in line for line in b2.evidence), str(b2.evidence if b2 else None))
    b7 = answers.by_key("B7")
    suite.check("session: B7 five slices", b7 is not None
                and any("all five slice offsets" in line for line in b7.evidence))
    b1 = answers.by_key("B1")
    suite.check("session: B1 one tone row per whole window", b1 is not None and b1.status == "O"
                and any("every window holds exactly one tone-map row" in line for line in b1.evidence),
                str(b1.evidence if b1 else None))
    text = "".join(session_answers.render_capture(answers))
    suite.contains("session: capture heading", text, "## 20261031-100000")
    suite.contains("session: P1 heading", text, "### P1")
    suite.contains("session: EP heading", text, "### EP")
    suite.contains("session: EP says the module ran nothing", text, "patched no composite draw")
    suite.contains("session: wetness values", text, "0.1")
    cross = session_answers.compare_captures([capture, other])
    suite.equal("session: cross-capture answers", len(cross), 2)
    b4 = next((answer for answer in cross if answer.key == "B4"), None)
    suite.check("session: B4 differs", b4 is not None and b4.status == "O"
                and any("differ between the captures" in line for line in b4.evidence))
    p1x = next((answer for answer in cross if answer.key == "P1x"), None)
    suite.check("session: wetness across captures", p1x is not None and p1x.status == "O"
                and any("f0:" in line for line in p1x.evidence))
    out = Path(tempfile.mkdtemp(prefix="session-answers-"))
    try:
        target = out / "answers.md"
        with contextlib.redirect_stdout(io.StringIO()):
            code = session_answers.main([str(root), "--out", str(target), "--quiet"])
        suite.equal("session: main exit code", code, 0)
        body = target.read_text(encoding="utf-8")
        suite.contains("session: report has the capture", body, "20261031-100000")
        suite.contains("session: report has the second capture", body, "20261031-110000")
        suite.contains("session: report has the cross section", body, "## Across captures")
    finally:
        shutil.rmtree(out, ignore_errors=True)
    # A folder without a capture is refused.
    empty = Path(tempfile.mkdtemp(prefix="session-empty-"))
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            suite.equal("session: empty folder refused", session_answers.main([str(empty)]), 2)
    finally:
        shutil.rmtree(empty, ignore_errors=True)


def check_fixtures_and_engine(suite: Suite, first: Path, second: Path) -> None:
    """The shader fixtures' origin marker and the engine pass overrides' counters, from files the tools read."""

    capture = drawdiag.load_capture(str(first))
    fixtures = capture.shaders
    suite.check("fixtures: shaders.txt loaded", fixtures is not None, "load_capture returned no shader fixtures")
    if fixtures is not None:
        suite.equal("fixtures: format", fixtures.format, 3)
        suite.equal("fixtures: row count", len(fixtures.rows), 3)
        suite.equal("fixtures: kept from creation", len(fixtures.creation), 2)
        suite.equal("fixtures: kept while capturing", len(fixtures.capture), 1)
        suite.equal("fixtures: first row origin", fixtures.rows[0].origin, "creation")
        suite.check("fixtures: creation row names the wetness pass",
                    "g_WetnessParameter=4" in fixtures.rows[0].names, str(fixtures.rows[0].names))
        suite.equal("fixtures: row size", fixtures.rows[0].size, 8808)
        suite.check("fixtures: a complete list raises no notice", not fixtures.notices,
                    str([str(notice) for notice in fixtures.notices]))
    older = drawdiag.load_capture(str(second)).shaders
    suite.check("fixtures: the older two-column list loads", older is not None and len(older.rows) == 1,
                str(older.rows if older else None))
    if older is not None and older.rows:
        suite.equal("fixtures: a two-column row reads as a capture fixture", older.rows[0].origin, "capture")
        suite.equal("fixtures: two-column names", older.rows[0].names, ("g_LightParam=0",))

    # A malformed row is a notice, never an exception: a fixture list that arrived through a copy must stay readable.
    broken = Path(tempfile.mkdtemp(prefix="shaders-broken-"))
    try:
        write_shaders(broken, ["not a fixture row"])
        parsed = drawdiag.load_shaders(str(broken / "shaders.txt"))
        suite.equal("fixtures: a malformed row is one notice", len(parsed.notices), 1)
        suite.equal("fixtures: a malformed row keeps no row", len(parsed.rows), 0)
    finally:
        shutil.rmtree(broken, ignore_errors=True)

    engine = capture.engine
    suite.check("engine: engine.txt loaded", engine is not None, "load_capture returned no engine.txt")
    if engine is not None:
        suite.check("engine: module on", engine.key_on)
        suite.equal("engine: runs", engine.runs, 0)
        suite.equal("engine: declines", engine.declined, 4)
        suite.check("engine: the file carries this capture's window counts", engine.have_window)
        suite.equal("engine: window runs", engine.window_runs, 0)
        suite.equal("engine: window declines", engine.window_declined, 4)
        dominant = engine.dominant_decline()
        suite.check("engine: dominant decline is the most frequent reason",
                    dominant is not None and dominant[1] == 3
                    and dominant[0] == "the ambient buffer is neither 16,384 nor 12,352 bytes", str(dominant))
        window_dominant = engine.window_dominant_decline()
        suite.check("engine: window dominant decline is that of the window's own lines",
                    window_dominant is not None and window_dominant[1] == 3
                    and window_dominant[0] == "the ambient buffer is neither 16,384 nor 12,352 bytes", str(window_dominant))
        suite.equal("engine: no last-run facts before a run", engine.last_run, "")
    ran = drawdiag.load_capture(str(second)).engine
    suite.check("engine: the second capture's module ran", ran is not None and ran.ran, str(ran.fields if ran else None))
    if ran is not None:
        suite.equal("engine: runs count", ran.runs, 4)
        suite.equal("engine: declines count", ran.declined, 1)
        suite.equal("engine: the second capture's window runs", ran.window_runs, 2)
        suite.check("engine: the second capture ran in its own window", ran.ran_in_window)
        suite.contains("engine: last-run sentence", ran.last_run, "16384")
        suite.contains("engine: last-run target", ran.last_run, "3840x2160")

    # The EP rule: an answer from this capture's window when the module patched a draw there, the window's dominant reason when it did not,
    # [U] when the key was off or the folder predates engine.txt, and [U] for a file that predates the window lines.
    answer = session_answers.answer_engine(capture)
    suite.equal("engine: a module that ran nothing answers [U]", answer.status, "U")
    suite.check("engine: [U] names the dominant reason of the window",
                any("dominant decline reason in this window" in line and "3" in line for line in answer.evidence),
                str(answer.evidence))
    answer = session_answers.answer_engine(drawdiag.load_capture(str(second)))
    suite.equal("engine: a module that patched a draw answers [O]", answer.status, "O")
    suite.check("engine: [O] carries the window's runs and declines",
                any("patched 2 composite draw(s)" in line and "declined 1" in line and "this capture's window" in line
                    for line in answer.evidence), str(answer.evidence))
    suite.check("engine: [O] prints the session totals when they differ from the window",
                any("the session up to this capture: 4 run(s)" in line for line in answer.evidence), str(answer.evidence))
    # Session runs > 0 while the window saw none: the answer is [U], because the question is about the captured frames. The three [U]
    # scenes read differently too: the knob at 1.0, a module that is not armed, and an armed module that saw no composite draw.
    cases = Path(tempfile.mkdtemp(prefix="engine-window-"))
    try:
        for name in ("session-ran", "identity", "disarmed", "armed-idle", "older"):
            write_minimal_capture(cases / name, [draws_row(1, "Draw", 3, 0, "0x1")],
                                  frame_line="# frame 1 draws 1 tone 0 ui1 0 upscale_after 0 omitted 0",
                                  trailer="# rows 1 frames 1 omitted 0")
        buffer_decline = "the ambient buffer is neither 16,384 nor 12,352 bytes"
        write_engine(cases / "session-ran", runs=3, declines=((buffer_decline, 5),), window=(0, 2, ((buffer_decline, 2),)))
        write_engine(cases / "identity", runs=0, identity=True, window=(0, 0, ()))
        write_engine(cases / "disarmed", runs=0, armed=False, window=(0, 0, ()))
        write_engine(cases / "armed-idle", runs=0, window=(0, 0, ()))
        write_engine(cases / "older", runs=5)  # no window lines: a file written before them
        session_ran = session_answers.answer_engine(drawdiag.load_capture(str(cases / "session-ran")))
        suite.equal("engine: a session that ran but a window that did not answers [U]", session_ran.status, "U")
        suite.check("engine: the empty window says so and names its own dominant reason",
                    any("patched no composite draw in this capture's window" in line for line in session_ran.evidence)
                    and any(f"dominant decline reason in this window: {buffer_decline} = 2" in line for line in session_ran.evidence),
                    str(session_ran.evidence))
        suite.check("engine: the window counts are the window's, not the session's",
                    any("window counts are this capture's own frames" in line and "3 run(s)" in line
                        for line in session_ran.evidence), str(session_ran.evidence))
        suite.check("engine: [U] with the knob at 1.0 says so",
                    any("knob at 1.0: yes" in line for line in session_answers.answer_engine(
                        drawdiag.load_capture(str(cases / "identity"))).evidence), "knob at 1.0 not printed")
        suite.check("engine: [U] from an unarmed module says so",
                    any("armed: no" in line for line in session_answers.answer_engine(
                        drawdiag.load_capture(str(cases / "disarmed"))).evidence), "armed not printed")
        suite.check("engine: [U] from an armed module that saw no composite says so",
                    any("armed: yes" in line and "knob at 1.0: no" in line for line in session_answers.answer_engine(
                        drawdiag.load_capture(str(cases / "armed-idle"))).evidence), "armed/identity not printed")
        older = session_answers.answer_engine(drawdiag.load_capture(str(cases / "older")))
        suite.equal("engine: a file that predates the window lines answers [U]", older.status, "U")
        suite.check("engine: the predating file says it cannot answer for the window and gives the session counts",
                    any("no window counters" in line for line in older.evidence)
                    and any("the session up to this capture: 5 patched composite draw(s)" in line for line in older.evidence),
                    str(older.evidence))
    finally:
        shutil.rmtree(cases, ignore_errors=True)

    off = Path(tempfile.mkdtemp(prefix="engine-off-"))
    missing = Path(tempfile.mkdtemp(prefix="engine-missing-"))
    try:
        for folder in (off, missing):
            write_minimal_capture(folder, [draws_row(1, "Draw", 3, 0, "0x1")],
                                  frame_line="# frame 1 draws 1 tone 0 ui1 0 upscale_after 0 omitted 0",
                                  trailer="# rows 1 frames 1 omitted 0")
        write_engine(off, module_on=False)
        answer = session_answers.answer_engine(drawdiag.load_capture(str(off)))
        suite.equal("engine: the key being off answers [U]", answer.status, "U")
        suite.check("engine: the key being off is said", any("key was off" in line for line in answer.evidence),
                    str(answer.evidence))
        answer = session_answers.answer_engine(drawdiag.load_capture(str(missing)))
        suite.equal("engine: a folder without engine.txt answers [U]", answer.status, "U")
        suite.check("engine: a missing file is said", any("no engine.txt" in line for line in answer.evidence),
                    str(answer.evidence))
    finally:
        shutil.rmtree(off, ignore_errors=True)
        shutil.rmtree(missing, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------------------------
# The rules, one edge capture each
# ---------------------------------------------------------------------------------------------------------------------


def check_ssr_gate(suite: Suite) -> None:
    """The reflections' gate answer skips the catch-all reason: a window where thousands of non-reflection draws reached the seam and a
    few water draws were stopped at a real gate must name that gate, and a window with only the catch-all must say no water draw came."""

    stamp = "the bound shader carries no reflection stamp: its bytecode was never read"
    mixed = drawdiag.EngineStatus(path="synthetic", format=3,
                                  fields={"module": "on", "ssr module": "on", "ssr armed": "yes", "ssr runs": "0", "ssr readers": "0",
                                          "ssr declined": "0", "ssr window runs": "0", "ssr window readers": "0",
                                          "ssr window declined": "0"},
                                  ssr_passed_on=[(drawdiag.SSR_NOT_REFLECTION, 40211), (stamp, 18)],
                                  ssr_window_passed_on=[(drawdiag.SSR_NOT_REFLECTION, 4662), (stamp, 18)])
    gate = mixed.ssr_window_dominant_passed_on()
    suite.check("ssr gate: the catch-all never wins the window", gate is not None and gate[0] == stamp and gate[1] == 18, str(gate))
    session_gate = mixed.ssr_dominant_passed_on()
    suite.check("ssr gate: the catch-all never wins the session", session_gate is not None and session_gate[0] == stamp,
                str(session_gate))
    suite.equal("ssr gate: the window's catch-all count is kept apart", mixed.ssr_window_not_reflection, 4662)
    ran, lines = session_answers._ssr_summary(mixed)
    suite.check("ssr gate: the summary names the real gate", not ran and any(stamp in line for line in lines), str(lines))
    only = drawdiag.EngineStatus(path="synthetic", format=3, fields=dict(mixed.fields),
                                 ssr_passed_on=[(drawdiag.SSR_NOT_REFLECTION, 40211)],
                                 ssr_window_passed_on=[(drawdiag.SSR_NOT_REFLECTION, 4662)])
    suite.check("ssr gate: only the catch-all is no gate", only.ssr_window_dominant_passed_on() is None,
                str(only.ssr_window_dominant_passed_on()))
    ran, lines = session_answers._ssr_summary(only)
    suite.check("ssr gate: the summary says no water draw reached the module",
                not ran and any("no water draw reached the module" in line and "4662" in line for line in lines), str(lines))


def check_rules(suite: Suite, folders: dict[str, Path]) -> None:
    """The negative cases: a rule that stops holding must fail here and not in a session report."""

    # B1: the row cap dropped rows, so one tone-map row in the file does not prove one game frame per window.
    dropped = drawdiag.load_capture(str(folders["b1-omitted"]))
    answer = session_answers.answer_capture(dropped).by_key("B1")
    suite.check("rules: B1 refuses a window whose rows were dropped", answer is not None and answer.status == "U",
                str(answer.evidence if answer else None))
    if answer:
        suite.check("rules: B1 says the cap dropped rows",
                    any("the row cap dropped" in line for line in answer.evidence), str(answer.evidence))
        suite.check("rules: B1 still reports the one tone row it sees",
                    any("frame 1: 1" in line for line in answer.evidence), str(answer.evidence))

    # B1: two tone-map rows in a window that kept every row - the splitter closed at every second one.
    twice = drawdiag.load_capture(str(folders["b1-two-tone"]))
    answer = session_answers.answer_capture(twice).by_key("B1")
    suite.check("rules: B1 refuses a window with two tone rows", answer is not None and answer.status == "U",
                str(answer.evidence if answer else None))
    if answer:
        suite.check("rules: B1 names the window with two tone rows",
                    any("do not hold exactly one tone-map row" in line and "frame 1 (2)" in line
                        for line in answer.evidence), str(answer.evidence))

    # B7: a 4K main depth prepass is not a shadow-atlas slice stack.
    prepass = drawdiag.load_capture(str(folders["b7-prepas"]))
    answer = session_answers.answer_capture(prepass).by_key("B7")
    suite.check("rules: B7 answers from the atlas-shaped view", answer is not None and answer.status == "O",
                str(answer.evidence if answer else None))
    if answer:
        suite.check("rules: B7 counts only the atlas draw",
                    any(line.startswith("1 depth-only draw(s) with a viewport on an atlas-shaped depth view")
                        for line in answer.evidence), str(answer.evidence))
        suite.check("rules: B7 does not list the prepass in the atlas sizes",
                    not any("3840x2160" in line for line in answer.evidence), str(answer.evidence))

    # P2: the pass is there, no later row reads its target, and the window is not whole - not an answer.
    unread = drawdiag.load_capture(str(folders["p2-unread"]))
    suite.equal("rules: P2 finds the wetness pass", len(session_answers.find_wetness_passes(unread)), 1)
    answer = session_answers.answer_capture(unread).by_key("P2")
    suite.check("rules: P2 refuses 'not read again' on a cut window", answer is not None and answer.status == "U",
                str(answer.evidence if answer else None))
    if answer:
        suite.check("rules: P2 says why the absence is not evidence",
                    any("the row cap dropped 25 row(s)" in line for line in answer.evidence), str(answer.evidence))

    # The wetness pass needs both inputs: a draw that reads one of them is a different pass.
    partial = drawdiag.load_capture(str(folders["wetness-partial"]))
    suite.equal("rules: the wetness pass needs the view position too",
                len(session_answers.find_wetness_passes(partial)), 0)
    suite.equal("rules: the report does not call a half-bound draw the pass",
                len(cbdump_report.wetness_draws(partial.draws).passes), 0)
    suite.equal("rules: the report still counts the draw binding the buffer",
                cbdump_report.wetness_draws(partial.draws).rows, 1)

    # B2: the glare chain is created before the capture arms, so its rows carry `-` in every slot and only the CRC join onto
    # shaders.txt's creation row can name it. The moving member of the sample is named from the shader's own reflection.
    joined = drawdiag.load_capture(str(folders["glare-join"]))
    answer = session_answers.answer_capture(joined).by_key("B2")
    suite.check("rules: B2 answers from the CRC join", answer is not None and answer.status == "O",
                str(answer.evidence if answer else None))
    if answer:
        suite.check("rules: B2 names the glare constant the join found",
                    any("cGlareParam in 1 row(s)" in line for line in answer.evidence), str(answer.evidence))
        suite.check("rules: B2 names the draw the join found",
                    any("frame 1 ordinal 1" in line and "0xAE4A07BE" in line for line in answer.evidence),
                    str(answer.evidence))
        suite.check("rules: B2 says the names came from shaders.txt",
                    any("shaders.txt" in line for line in answer.evidence), str(answer.evidence))
        suite.check("rules: B2 reports the moving member by its reflected name",
                    any("prevWeight" in line for line in answer.evidence), str(answer.evidence))

    # The join's two refusals, and the wetness report's use of it: a CRC listed with two different name sets names nothing (one notice,
    # never a guess); a row that names its own resources is never renamed by the fixture; a nameless row whose fixture declares the
    # wetness buffer counts as a wetness binding at the fixture's slot.
    scratch = Path(tempfile.mkdtemp(prefix="crc-join-"))
    try:
        dup = write_minimal_capture(
            scratch / "dup",
            [draws_row(1, "Draw", 4, 0, "0xDEADBEEF", cb="0:-:96:9002", rtv="0:R16G16B16A16_FLOAT:1920x1080:9003"),
             draws_row(2, "Draw", 4, 0, "0x0BADF00D", cb="4:g_WetnessParameter:32:9004", srv="0:g_SamplerGBuffer3:B8G8R8A8_UNORM:3840x2160:9005",
                       rtv="0:R16G16B16A16_FLOAT:1920x1080:9003"),
             draws_row(3, "Draw", 4, 0, "0x0C0FFEE0", cb="4:-:32:9006", rtv="0:R16G16B16A16_FLOAT:1920x1080:9003"),
             draws_row(4, "Draw", 4, 1, "0x85E777EF", rtv="0:B8G8R8A8_UNORM:2560x1440:3001", markers="tone")],
            frame_line="# frame 1 draws 4 tone 4 ui1 0 upscale_after 0 omitted 0",
            trailer="# rows 4 frames 1 omitted 0")
        write_shaders(dup, ["DEADBEEF,3388,creation,sInput.T=0,cGlareParam=0",
                            "DEADBEEF,3388,creation,sInput.T=0,cBrightPassParam=0",
                            "0BADF00D,1608,creation,g_SamplerGBuffer3.T=0,g_WetnessParameter=7",
                            "0C0FFEE0,1608,creation,g_SamplerGBuffer3.T=0,g_SamplerViewPosition.T=1,g_WetnessParameter=4"])
        loaded = drawdiag.load_capture(str(dup))
        fixtures = loaded.shaders
        suite.equal("join: a CRC with a leading zero is one key in both forms", drawdiag.crc_key("0C0FFEE0"), drawdiag.crc_key(0x0C0FFEE0))
        suite.check("join: a CRC with two name sets is ambiguous", fixtures is not None and "DEADBEEF" in {k.upper() for k in fixtures.ambiguous},
                    str(fixtures.ambiguous if fixtures else None))
        suite.check("join: the ambiguity is one notice", fixtures is not None and sum("two different name sets" in str(n) for n in fixtures.notices) == 1,
                    str([str(n) for n in fixtures.notices] if fixtures else None))
        rows = loaded.draws.rows if loaded.draws else []
        suite.equal("join: an ambiguous CRC names no draw", loaded.reflected_names(rows[0]), ())
        suite.equal("join: a row that names its own resources keeps them (the fixture says slot 7, the row says 4)",
                    loaded.reflected_names(rows[1]), ())
        suite.check("join: the named row's own name stands", rows[1].cb("g_WetnessParameter") is not None and rows[1].cb("g_WetnessParameter").slot == 4,
                    str(rows[1].cbs))
        wet = cbdump_report.wetness_draws(loaded.draws, loaded)
        suite.equal("join: the wetness report counts the named row and the joined row", wet.rows, 2)
        suite.equal("join: the wetness report says one row was joined", wet.joined, 1)
        suite.check("join: the joined row's slot is the fixture's", 4 in wet.slots and 0x9006 in wet.pointers, str((wet.slots, wet.pointers)))
        suite.equal("join: without the capture the nameless row is invisible", cbdump_report.wetness_draws(loaded.draws).rows, 1)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    # FL: a flare draw the file can name through the shader kept from creation, and a draw it cannot name at all. The second one is not an
    # answer that the family did not run: the answer counts the rows it cannot identify instead of claiming the family was absent.
    scratch = Path(tempfile.mkdtemp(prefix="flare-"))
    try:
        named = write_minimal_capture(
            scratch / "fl-named",
            [draws_row(1, "Draw", 6, 0, "0x9B2E1FE1", srv="0:-:R16G16B16A16_FLOAT:1920x1080:9001",
                       cb="0:-:32:9002|1:-:64:9003", rtv="0:R16G16B16A16_FLOAT:1920x1080:9004")],
            frame_line="# frame 1 draws 1 tone 0 ui1 0 upscale_after 0 omitted 0",
            trailer="# rows 1 frames 1 omitted 0")
        write_shaders(named, ["9B2E1FE1,1424,creation,sInput.T=0,cHaloParam=1"])
        answer = session_answers.answer_capture(drawdiag.load_capture(str(named))).by_key("FL")
        suite.check("flare: FL names a flare draw through its creation row",
                    answer is not None and any("cHaloParam in 1 row(s)" in line for line in answer.evidence),
                    str(answer.evidence if answer else None))
        unnamed = write_minimal_capture(
            scratch / "fl-unnamed",
            [draws_row(1, "Draw", 6, 0, "0x5EED1234", srv="0:-:R16G16B16A16_FLOAT:1920x1080:9001",
                       cb="0:-:32:9002|1:-:64:9003", rtv="0:R16G16B16A16_FLOAT:1920x1080:9004")],
            frame_line="# frame 1 draws 1 tone 0 ui1 0 upscale_after 0 omitted 0",
            trailer="# rows 1 frames 1 omitted 0")
        answer = session_answers.answer_capture(drawdiag.load_capture(str(unnamed))).by_key("FL")
        suite.check("flare: FL does not claim the family did not run when a row is unnamed",
                    answer is not None and answer.status == "U",
                    str(answer.evidence if answer else None))
        if answer:
            suite.check("flare: FL counts the rows it cannot name",
                        any("1 draw(s) in this capture carry a pixel shader whose reflection the file does not hold" in line
                            for line in answer.evidence), str(answer.evidence))
            suite.check("flare: FL says either that the family is absent or that the draws are unnamed",
                        any("or the draws are here without a name" in line for line in answer.evidence), str(answer.evidence))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def check_ambient_count(suite: Suite) -> None:
    """The ambient verdict counts the probes ``paramNum`` names, not the padding behind them."""

    def sample(data: bytes) -> drawdiag.CbDumpSample:
        return drawdiag.CbDumpSample(frame=0, time_s=1.0, ps_crc=1, ps_crc_text="0x1", name="g_AmbientParamArray",
                                     slot=2, bytes=len(data), pointer=0x7002, data=data, hex_text="")

    # 64 records, one non-unit scale outside the eight the header names: the verdict is over eight probes and quiet.
    narrowed = cbdump_report.analyse_buffer(
        "g_AmbientParamArray", [sample(ambient_bytes(probe_scale=(20, 2.0), param_num=8))])
    verdict = narrowed.verdicts[0] if narrowed.verdicts else ""
    suite.contains("ambient: verdict counts the probes paramNum names", verdict, "~= 1.0 in 8 of 8 probe(s)")
    suite.contains("ambient: verdict names the count", verdict, "paramNum 8 of 64 decoded probe(s) counted")
    suite.check("ambient: a non-unit probe past paramNum is not called out", "not unit" not in verdict, verdict)
    text = "\n".join(narrowed.extra_lines)
    suite.contains("ambient: the range is over the same probes", text,
                   "range over the probes (8 of 64 decoded probe(s))")
    # envSpaceType is 1 on the first 32 probes and 2 on the rest: a range that counted all 64 would say 1..2.
    suite.contains("ambient: the range stops where the probes stop", text, "envSpaceType 1..1")
    all_probes = cbdump_report.analyse_buffer("g_AmbientParamArray", [sample(ambient_bytes(param_num=64))])
    suite.contains("ambient: all 64 probes counted when they are all named",
                   "\n".join(all_probes.extra_lines), "envSpaceType 1..2")
    signature = session_answers._ambient_signature(sample(ambient_bytes(probe_scale=(20, 2.0), param_num=8)))
    suite.equal("ambient: the cross-capture signature counts the same probes",
                signature.get("skyVisibilityScale"), (1.0, 1.0, 8))
    suite.equal("ambient: the signature masks count", signature.get("ssaoMask"), (0.0, 0.109375, 8))

    # A count that is not a count (here a float written into the field) is reported, and every decoded probe counts.
    invalid = cbdump_report.analyse_buffer("g_AmbientParamArray", [sample(ambient_bytes(param_num_float=-1.0))])
    verdict = invalid.verdicts[0] if invalid.verdicts else ""
    suite.contains("ambient: an invalid paramNum is reported", verdict,
                   "paramNum reads 3212836864, which is not a count in 1..64")
    suite.contains("ambient: an invalid paramNum counts every probe", verdict, "~= 1.0 in 64 of 64 probe(s)")
    zero = cbdump_report.analyse_buffer("g_AmbientParamArray", [sample(ambient_bytes(param_num=0))])
    suite.contains("ambient: a zero count is not a narrowing", zero.verdicts[0] if zero.verdicts else "",
                   "~= 1.0 in 64 of 64 probe(s)")


# ---------------------------------------------------------------------------------------------------------------------
# The real captures
# ---------------------------------------------------------------------------------------------------------------------


def _pixel_dump_folders(root: Path) -> list[Path]:
    """Folders under ``root`` that hold raw pixel files next to a description, whatever tool wrote them."""

    found: list[Path] = []
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in (".r32", ".bgra8", ".r8"):
            if path.parent not in found and (path.parent / "facts.txt").is_file():
                found.append(path.parent)
    return sorted(found)


def check_real_captures(suite: Suite, root: Path | None) -> None:
    """What any folder of real captures must satisfy, whatever session it came from."""

    if root is None:
        suite.skip("real captures", "no captures folder was named (--captures DIR, or DRAWDIAG_CAPTURES)")
        return
    if not root.is_dir():
        suite.skip("real captures", f"{root} is not a folder")
        return
    folders = drawdiag.find_captures(str(root))
    if not folders:
        suite.skip("real captures", f"{root} holds no capture folder (no draws.csv at or below it)")
        return
    captures = [drawdiag.load_capture(folder) for folder in folders]
    captures = [capture for capture in captures if capture.draws is not None]
    suite.check("real: captures load", bool(captures), f"{len(folders)} folder(s) found, none readable")
    if not captures:
        return
    print(f"real captures: {len(captures)} capture folder(s) under {root}")

    # Every capture: the reader's own count check agrees with the rows, and no tool raises on the file.
    for capture in captures:
        mismatched = [notice for notice in capture.all_notices()
                      if notice.kind in (drawdiag.KIND_TRAILER_MISMATCH, drawdiag.KIND_LAYOUT)]
        suite.check(f"real: {capture.name} trailer agrees with the rows", not mismatched,
                    str([str(notice) for notice in mismatched]))
        suite.check(f"real: {capture.name} answers without raising",
                    all(answer.status in ("O", "U") for answer in session_answers.answer_capture(capture).answers))
        suite.check(f"real: {capture.name} decodes its constant buffers",
                    cbdump_report.analyse_capture(capture) is not None)
        # A capture written before engine.txt existed answers EP as [U] and says so; one that has it answers from it without raising.
        answer = session_answers.answer_capture(capture).by_key("EP")
        if capture.engine is None:
            suite.check(f"real: {capture.name} has no engine.txt and answers EP [U]",
                        answer is not None and answer.status == "U"
                        and any("no engine.txt" in line for line in answer.evidence),
                        str(answer.evidence if answer else None))
        else:
            suite.check(f"real: {capture.name} answers EP from engine.txt",
                        answer is not None and answer.status in ("O", "U"), str(answer.evidence if answer else None))
            if not capture.engine.have_window:
                suite.check(f"real: {capture.name} engine.txt predates the window lines and EP says so",
                            answer is not None and any("no window counters" in line for line in answer.evidence),
                            str(answer.evidence if answer else None))

    # The tone-map row a window names is the row the file holds, and it carries its marker.
    for capture in captures:
        for window in (capture.draws.windows if capture.draws else []):
            rows = capture.draws.of_frame(window.frame)
            tone = [row for row in rows if row.stage == session_answers.TONE_STAGE]
            if window.tone and tone:
                suite.equal(f"real: {capture.name} frame {window.frame} tone ordinal",
                            tone[0].ordinal, window.tone)
                suite.check(f"real: {capture.name} frame {window.frame} tone row is marked",
                            tone[0].has_marker(drawdiag.MARKER_TONE))

    # A window the row cap cut must not be answered as one game frame.
    capped = [capture for capture in captures
              if any(window.omitted for window in (capture.draws.windows if capture.draws else []))]
    if capped:
        for capture in capped[:3]:
            answer = session_answers.answer_capture(capture).by_key("B1")
            suite.check(f"real: {capture.name} row-capped windows answer [U]",
                        answer is not None and answer.status == "U", str(answer.evidence if answer else None))
            if answer:
                suite.check(f"real: {capture.name} says the cap dropped rows",
                            any("row cap dropped" in line for line in answer.evidence))
    else:
        suite.skip("real: row-capped windows", "no capture in the folder dropped a row")

    # The shadow atlas: a depth view that tall is a slice stack, and its draws sit on the five slices.
    atlas = [capture for capture in captures
             if any(not row.rtvs and row.dsv is not None and row.dsv.size is not None
                    and row.dsv.size.height >= session_answers.ATLAS_MIN_HEIGHT for row in capture.rows)]
    if atlas:
        capture = atlas[0]
        answer = session_answers.answer_capture(capture).by_key("B7")
        suite.check(f"real: {capture.name} answers the atlas", answer is not None and answer.status == "O",
                    str(answer.evidence if answer else None))
        if answer:
            suite.check(f"real: {capture.name} carries all five slice offsets",
                        any("all five slice offsets" in line for line in answer.evidence), str(answer.evidence))
    else:
        suite.skip("real: shadow atlas", "no capture in the folder holds an atlas-shaped depth view")

    # A G-buffer row writes five targets of one size.
    five = [capture for capture in captures if any(row.target_count == 5 for row in capture.rows)]
    if five:
        capture = five[0]
        rows = [row for row in capture.rows if row.target_count == 5]
        sizes = {str(entry.size) for row in rows for entry in row.rtvs}
        suite.check(f"real: {capture.name} five-target rows write one size", len(sizes) <= 2, str(sorted(sizes)))
        suite.check(f"real: {capture.name} five-target rows name their formats",
                    all(entry.format for row in rows for entry in row.rtvs))
    else:
        suite.skip("real: five-target rows", "no capture in the folder holds one")

    # An older format still loads, with the pointer-less entries it was written with.
    older = [capture for capture in captures if capture.format is not None and capture.format < 3]
    if older:
        suite.check("real: a capture written before format 3 loads", bool(older[0].rows), older[0].name)
        suite.check("real: its rows carry no resource pointer",
                    not any(entry.pointer is not None for row in older[0].rows for entry in row.rtvs),
                    older[0].name)
    else:
        suite.skip("real: older format", "no capture in the folder was written before format 3")

    # Real pixel bytes through the decoders, at the geometry the file's own size implies (one flat row, so no shape of
    # an unknown dump is guessed).
    dump_folders = _pixel_dump_folders(root)
    if not dump_folders:
        suite.skip("real: pixel dump", "no folder under the captures folder holds raw pixel files")
    for folder in dump_folders[:4]:
        for path in sorted(folder.iterdir()):
            if not path.is_file():
                continue
            data = path.read_bytes()
            if not data:
                continue
            if path.suffix == ".r8":
                fact = drawdiag.DumpFact(file=path.name, format="R8_UNORM", width=len(data), height=1,
                                         bytes=len(data), row_pitch=len(data), ordinal=0, pointer=None)
                decoded = dump_to_png.decode(fact, data)
                suite.equal(f"real: {path.name} decodes as R8_UNORM", decoded.values.shape, (1, len(data)))
            elif path.suffix == ".bgra8":
                fact = drawdiag.DumpFact(file=path.name, format="B8G8R8A8_UNORM", width=len(data) // 4, height=1,
                                         bytes=len(data), row_pitch=len(data), ordinal=0, pointer=None)
                decoded = dump_to_png.decode(fact, data)
                suite.equal(f"real: {path.name} decodes as BGRA", decoded.values.shape, (1, len(data) // 4, 4))
            elif path.suffix == ".r32":
                for fmt in ("R32_FLOAT", "D24_UNORM_S8_UINT"):
                    fact = drawdiag.DumpFact(file=path.name, format=fmt, width=len(data) // 4, height=1,
                                             bytes=len(data), row_pitch=len(data), ordinal=0, pointer=None)
                    decoded = dump_to_png.decode(fact, data)
                    expected = (1, len(data) // 4, 1) if fmt == "R32_FLOAT" else (1, len(data) // 4)
                    suite.equal(f"real: {path.name} decodes as {fmt}", decoded.values.shape, expected)
                    if fmt == "D24_UNORM_S8_UINT":
                        depth = decoded.values.astype(np.float64) / 16777215.0
                        suite.check(f"real: {path.name} 24-bit depth stays in 0..1",
                                    0.0 <= float(depth.min()) and float(depth.max()) <= 1.0)


def check_shadow_atlas(suite: Suite, workspace: Path) -> None:
    """The god-rays march's atlas facts: the readers grouped by shader, one instance per pointer, the bias decoded."""

    import shadow_atlas  # the tool under test, beside this file

    def row(ordinal: int, crc: str, srv: str, cb: str, rtv: str, blend: str) -> str:
        return (f"1,{ordinal},DrawIndexed,game,12,1,0,0,0,{crc},{srv},{cb},{rtv},-,"
                f"1:2048x10240+0.5+0.5,{blend},1/1/2/0,")

    shadow_srv = "0:g_SamplerShadow.T:R16_UNORM:2048x8192:7001"
    mask_srv = ("0:g_SamplerDepth.T:R24_UNORM_X8_TYPELESS:3840x2160:7000"
                "|0:g_SamplerShadowMask.T:R8_UNORM:3840x2160:7002")
    parameter_at = lambda pointer: f"0:g_DirectionalShadowParameter:128:{pointer}"  # noqa: E731
    rows = [
        # The four assembly draws: one row per cascade, each a MIN-blend draw into the sun mask's R8 target, each
        # binding its own g_DirectionalShadowParameter instance (the pointers the march keys by).
        row(4022, "0x1257B24B", shadow_srv + "|1:g_SamplerDepth.T:R24_UNORM_X8_TYPELESS:3840x2160:7000",
            parameter_at(7004) + "|1:g_ShadowBiasParameter:16:7008", "0:R8_UNORM:3840x2160:7003", "1/2/2/4/2/1/1/15"),
        row(4024, "0x1257B24B", shadow_srv + "|1:g_SamplerDepth.T:R24_UNORM_X8_TYPELESS:3840x2160:7000",
            parameter_at(7005) + "|1:g_ShadowBiasParameter:16:7008", "0:R8_UNORM:3840x2160:7003", "1/2/2/4/2/1/1/15"),
        row(4026, "0x1257B24B", shadow_srv, parameter_at(7006), "0:R8_UNORM:3840x2160:7003", "1/2/2/4/2/1/1/15"),
        row(4028, "0x1257B24B", shadow_srv, parameter_at(7007), "0:R8_UNORM:3840x2160:7003", "1/2/2/4/2/1/1/15"),
        # The sun's own light draw: it reads the R8 mask and binds ONE of the four instances again at its own slot.
        row(4041, "0x9DC59665", mask_srv, parameter_at(7005), "0:R11G11B10_FLOAT:3840x2160:7009", "1/0/2/1"),
    ]
    folder = write_minimal_capture(workspace / "s0-atlas", rows,
                                   frame_line="# frame 1 draws 5 tone 0 ui1 0 upscale_after 0 omitted 0",
                                   trailer="# rows 5 frames 1 omitted 0")
    (folder / "views.txt").write_text(
        "# FFXIV draw diagnostics, views, format 3\n" + VIEWS_HEADER + "\n"
        "g_SamplerShadow,4022,TEXTURE2D,1,R16_UNORM,1,0,2048,8192,1,1,R16_TYPELESS,7001\n",
        encoding="utf-8", newline="\n")

    def parameter_bytes(tail0: float) -> bytes:
        return directional_shadow_bytes() + struct.pack("<8f", tail0, 190.0, 100000.0, 0.0, -0.5, -39.0, -30.3, 1.0)

    bias = struct.pack("<4f", 0.0, 0.00390625, 2.0, 0.0)
    cbdump_rows = [
        ("2", "195.111", "0x9DC59665", "g_DirectionalShadowParameter", "5", "128", "7005", parameter_bytes(0.1)),
        ("2", "195.112", "0x1257B24B", "g_DirectionalShadowParameter", "3", "128", "7005", parameter_bytes(0.2)),
        ("2", "195.117", "0x1257B24B", "g_DirectionalShadowParameter", "3", "128", "7004", parameter_bytes(0.3)),
        ("2", "195.118", "0x1257B24B", "g_DirectionalShadowParameter", "3", "128", "7006", parameter_bytes(0.4)),
        ("2", "195.120", "0x1257B24B", "g_DirectionalShadowBias", "2", "16", "7008", bias),
    ]
    (folder / "cbdump.csv").write_text(
        "# FFXIV draw diagnostics, format 3\n" + CBDUMP_HEADER + "\n" +
        "\n".join(f"{f},{t},{c},{n},{s},{b},{p},{d.hex()}" for f, t, c, n, s, b, p, d in cbdump_rows) +
        "\n# rows 5\n", encoding="utf-8", newline="\n")

    report = shadow_atlas.build_report(drawdiag.load_capture(str(folder)))

    suite.check("shadow: the atlas row is the plain-plane shape",
                "size 2048x8192" in report and "TEXTURE2D," in report, report[:200])
    suite.check("shadow: the assembly group by shader", "shader 1257b24b, draws 4" in report, report[:400])
    suite.check("shadow: the MIN blend is counted", "MIN-blend draws 4" in report, report[:400])
    suite.check("shadow: four cascade instances", "shadow instances 4" in report, report[:400])
    suite.check("shadow: the sun draw binds one of the four again",
                "0x7005" in report and "g_SamplerShadowMask" in report, report[:600])
    # One instance's decoded rows: the fixture's identity matrix, the distance row, and the per-instance tail.
    suite.check("shadow: the instance's matrix decodes", "[+1.000000, +0.000000, +0.000000, +0.000000" in report)
    suite.check("shadow: the instance's distance row decodes", "m_ShadowDistance = [+10.000000, +20.000000" in report)
    suite.check("shadow: the pointer cadence kept the same-name instances apart",
                "0x7004" in report and "0x7005" in report and "0x7006" in report, report[400:1200])
    suite.check("shadow: the per-instance tails are reported apart",
                "@0x60 = +0.200000" in report and "@0x60 = +0.400000" in report)
    # A capture with no atlas at all: the report is still produced and explains itself.
    empty = write_minimal_capture(workspace / "s0-atlas-empty",
                                  [row(1, "0x99999999", "0:g_SamplerColorMap0.T:BC7_UNORM:512x512:7101",
                                       "0:g_MaterialParameter:512:2000", "0:B8G8R8A8_UNORM:1280x720:7102",
                                       "0/2/1/1/2/1/1/15")],
                                  frame_line="", trailer="# rows 1 frames 1 omitted 0")
    empty_report = shadow_atlas.build_report(drawdiag.load_capture(str(empty)))
    suite.check("shadow: an atlas-free capture still produces its report",
                "no draw of the capture samples the atlas" in empty_report, empty_report[:300])


# ---------------------------------------------------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    captures: Path | None = Path(os.environ[CAPTURES_ENV]) if os.environ.get(CAPTURES_ENV) else None
    keep = False
    index = 0
    while index < len(arguments):
        if arguments[index] == "--captures" and index + 1 < len(arguments):
            captures = Path(arguments[index + 1])
            index += 2
        elif arguments[index] == "--keep":
            keep = True
            index += 1
        elif arguments[index] in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        else:
            print(f"unknown argument: {arguments[index]}", file=sys.stderr)
            return 2
    suite = Suite()
    workspace = Path(tempfile.mkdtemp(prefix="drawdiag-selftest-"))
    try:
        session = workspace / "session"
        session.mkdir()
        first, second = write_session(session)
        edge = workspace / "edge"
        edge.mkdir()
        edge_captures = write_edge_captures(edge)
        with suite.section("reader"):
            check_reader(suite, session, first, second, edge)
        with suite.section("layouts"):
            check_layouts(suite)
        with suite.section("cbdump report"):
            check_cbdump_report(suite, first)
        with suite.section("dump to png"):
            check_dump(suite, first)
        with suite.section("fixtures and engine counters"):
            check_fixtures_and_engine(suite, first, second)
        with suite.section("session answers"):
            check_session(suite, session, first, second)
        with suite.section("rules"):
            check_rules(suite, edge_captures)
            check_ssr_gate(suite)
        with suite.section("ambient count"):
            check_ambient_count(suite)
        with suite.section("shadow atlas (godrays S0)"):
            check_shadow_atlas(suite, workspace)
        with suite.section("real captures"):
            check_real_captures(suite, captures)
    finally:
        if keep:
            print(f"fixtures kept in {workspace}")
        else:
            shutil.rmtree(workspace, ignore_errors=True)
    for note in suite.skipped:
        print(f"skip: {note}")
    if suite.failures:
        print(f"selftest: {suite.passed} check(s) passed, {len(suite.failures)} failed")
        for failure in suite.failures:
            print(f"  FAIL {failure}")
        return 1
    print(f"selftest: {suite.passed} check(s) passed, 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
