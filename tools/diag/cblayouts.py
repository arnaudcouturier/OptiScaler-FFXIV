"""The layouts of the named constant buffers the lighting and post passes carry.

Each layout is the table the engine's own shader reflection declares: member names, byte offsets and the size the buffer
is bound at.  The offsets are the ones the effect compiler writes into the bytecode of the live build (``g_LightParam``
declares 400 B and is bound at 512 B, ``g_AmbientParamArray`` declares 12,352 B and is bound at 16,384 B, the flat post
buffers are single float4 rows); where a member's value type is not visible in the reflection, the type used here is the
one the name implies and says so in its note.

Reading arbitrary bytes is the point: :func:`decode` never raises.  A buffer that is shorter than its layout fills what
it can and lists the members it did not reach, because a capture can carry a truncated hex row (the writer caps it at
16 KiB) and an older build can bind a smaller buffer than the one measured here.

Run the tools in this folder with ``python -I``; this module is standard library only.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any

# ---------------------------------------------------------------------------------------------------------------------
# The layout description
# ---------------------------------------------------------------------------------------------------------------------

# Member kinds and the bytes each element costs.  ``float``/``uint`` take ``count`` elements (a scalar when count is 1),
# ``float3``/``float4`` are the vectors the shaders declare, ``matrix`` is a row-major 4x4 (16 floats) and ``matrix3x4``
# the 3x4 a probe's inverse matrix uses (12 floats).
_KIND_BYTES = {
    "float": 4,
    "uint": 4,
    "float3": 12,
    "float4": 16,
    "matrix": 64,
    "matrix3x4": 48,
}

_KIND_COUNT = {
    "float": 1,
    "uint": 1,
    "float3": 3,
    "float4": 4,
    "matrix": 16,
    "matrix3x4": 12,
}


@dataclass(frozen=True)
class Member:
    """One declared member: ``name`` at ``offset``, of ``kind`` and ``count`` elements."""

    name: str
    offset: int
    kind: str = "float"
    count: int = 1
    note: str = ""
    # A member whose name says "index" or "type" is read as an integer, but the studies this table came from do not
    # record the declared type.  Four bytes that hold a float also read as a huge integer, so such a member is decoded a
    # second time as a float when its integer reading is above _UINT_AS_FLOAT_LIMIT (see decode_layout).
    float_too: bool = False

    @property
    def size(self) -> int:
        return _KIND_BYTES[self.kind] * self.count


# Above this an integer reading of a four-byte field can only be a float's bit pattern (a count, an index or an enum
# fits in 16 million with room to spare), which is why the second, float reading is shown from there on.
_UINT_AS_FLOAT_LIMIT = 1 << 24


@dataclass(frozen=True)
class Layout:
    """A named buffer: its declared members, its sizes and (for an array-of-struct) the repeated record."""

    name: str
    members: tuple[Member, ...]
    declared: int | None = None  # the size the shader declares, when it differs from the member table's end
    bound: int | None = None  # the size the game binds, when it is known and larger
    probe: "Layout | None" = None  # the repeated record of an array-of-struct buffer
    probe_offset: int = 0
    probe_count: int = 0
    flat: bool = False  # a buffer whose whole content is a float list; no member names are known
    note: str = ""

    def member(self, name: str) -> Member | None:
        for candidate in self.members:
            if candidate.name == name:
                return candidate
        return None

    @property
    def end(self) -> int:
        return max((member.offset + member.size for member in self.members), default=0)

    @property
    def size(self) -> int:
        """The bytes the table describes (declared size when the shader declares one, else the last member's end)."""

        return self.declared if self.declared is not None else self.end


def _f(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "float", 1, note)


# The integer members: their names say "count", "index" or "type", the studies this table came from do not record
# their declared type, and a float stored in the same four bytes reads as a large integer.  Each is therefore decoded as
# an integer *and* (when that reading is above 2^24) as a float, under the key ``<name>_f32``.
def _u(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "uint", 1, note, float_too=True)


def _f3(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "float3", 1, note)


def _f4(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "float4", 1, note)


def _matrix(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "matrix", 1, note)


def _matrix3x4(name: str, offset: int, note: str = "") -> Member:
    return Member(name, offset, "matrix3x4", 1, note)


# ---------------------------------------------------------------------------------------------------------------------
# g_WetnessParameter: the engine's wetness value, bound on the ground material draws
# ---------------------------------------------------------------------------------------------------------------------

# 32 B, one opaque struct member in the reflection: the compiler gives no member names for it, so the eight floats are
# reported as f0..f7.  What each one means is exactly what the capture has to settle; a capture that shows one of them
# ramping with rain is the whole answer the puddle work needs.
WETNESS_PARAMETER = Layout(
    name="g_WetnessParameter",
    members=tuple(_f(f"f{index}", index * 4) for index in range(8)),
    declared=32,
    note="8 floats, member names not present in the reflection; f0..f7 are byte order",
)

# ---------------------------------------------------------------------------------------------------------------------
# g_AmbientParamArray: the ambient probe table the composite reads
# ---------------------------------------------------------------------------------------------------------------------

# 64 B header + 64 records of 192 B = 12,352 B declared; the game binds 16,384 B.
AMBIENT_HEADER = Layout(
    name="g_AmbientParamArray.header",
    members=(
        _u("paramNum", 0x00, "the probe count the engine fills"),
        _f("skyShRecipScale", 0x04),
        # 0x08..0x10 is padding the reflection does not name
        Member("skySh", 0x10, "float", 12, "3 x float4: the sky's L1 spherical-harmonic ambient term"),
    ),
    declared=64,
)

AMBIENT_PROBE = Layout(
    name="g_AmbientParamArray.probe",
    members=(
        Member("sh", 0x00, "float", 12, "3 x float4, the probe's own SH"),
        _f3("color", 0x30),
        _f("shRecipScale", 0x3C),
        _f("attenuationSlope", 0x40),
        _f("attenuationIntercept", 0x44),
        _f("attenuationValue", 0x48),
        _f("skyVisibility", 0x4C),
        _f("reflectionScale", 0x50),
        _f("reflectionOffset", 0x54),
        _f("bakeLightRate", 0x58),
        _u("envLocationIndex", 0x5C, "an index per the name; read as an integer"),
        _matrix3x4("invMatrix", 0x60),
        _f3("interpInvRangeMinus", 0x90),
        _f3("interpInvRangePlus", 0x9C),
        _f("priority", 0xA8),
        _u("envSpaceType", 0xAC, "an enum per the name; read as an integer"),
        _f("skyVisibilityScale", 0xB0, "the engine's own multiplier on skyVisibility: 1.0 is vanilla"),
        _f("ssaoMask", 0xB4),
        # 0xB8..0xC0 is padding the reflection does not name
    ),
    declared=192,
)

AMBIENT_PARAM_ARRAY = Layout(
    name="g_AmbientParamArray",
    members=AMBIENT_HEADER.members,
    declared=12352,
    bound=16384,
    probe=AMBIENT_PROBE,
    probe_offset=64,
    probe_count=64,
    note="64 B header + 64 probes x 192 B; bound at 16,384 B so 4,032 B past the declared end are unnamed",
)

# ---------------------------------------------------------------------------------------------------------------------
# g_DirectionalShadowParameter: the cascade projection and the per-cascade scale/bias
# ---------------------------------------------------------------------------------------------------------------------

DIRECTIONAL_SHADOW_PARAMETER = Layout(
    name="g_DirectionalShadowParameter",
    members=(
        _matrix("m_ShadowProjectionMatrix", 0x00),
        _f4("m_ShadowDistance", 0x40),
        _f4("m_ShadowMapParameter", 0x50),
        Member("undeclaredTail", 0x60, "float", 8, "beyond the declared 96 B: the game binds 128 B"),
    ),
    declared=96,
    bound=128,
    note="96 B declared, 128 B bound",
)

# ---------------------------------------------------------------------------------------------------------------------
# g_ReflectionParameter: the screen-space reflection chain's knobs
# ---------------------------------------------------------------------------------------------------------------------

# 80 B in the reflection pass; the water family binds 128 B.  The five scalars at 0x20..0x2C are the hand-tuned ones
# (normal blend, power, mip, roughness threshold); everything else positions the effect.
REFLECTION_PARAMETER = Layout(
    name="g_ReflectionParameter",
    members=(
        _f4("m_PixelSize", 0x00, "the half-resolution step in pixels"),
        _f4("m_FadeDistance", 0x10),
        _f("m_NormalBlendWeight", 0x20),
        _f("m_ReflectionPowerScale", 0x24),
        _f("m_MipLevel", 0x28),
        _f("m_RoughnessThreshold", 0x2C),
        _f4("m_DynamicViewportResolution", 0x30),
        Member("reserved", 0x40, "float", 2, "between the resolution vector and the specular-mask switch"),
        _f("m_EnableAdjustSpecularMask", 0x48),
    ),
    declared=80,
    bound=128,
    note="80 B in the reflection chain, 128 B in the water variants",
)

# ---------------------------------------------------------------------------------------------------------------------
# g_LightParam: the light a draw shades
# ---------------------------------------------------------------------------------------------------------------------

# 400 B declared, 512 B bound.  The first four members are the ones the diagnostic's own cb.csv reads (position,
# direction, diffuse colour), the rest is the full table so that a patch's offsets can be checked against it.
LIGHT_PARAM = Layout(
    name="g_LightParam",
    members=(
        _f3("m_Position", 0x00),
        _f3("m_Direction", 0x10),
        _f3("m_DiffuseColor", 0x20),
        _f3("m_SpecularColor", 0x30),
        _f4("m_Attenuation", 0x40),
        _f4("m_ClipMin", 0x50),
        _f4("m_ClipMax", 0x60),
        _f4("m_PlaneFadeScale", 0x70),
        _f4("m_ShadowTexMask", 0x80, "which cascade slices are valid"),
        _f4("m_PlaneRayDirection", 0x90),
        _matrix("m_PlaneInversMatrix", 0xA0),
        _matrix("m_WorldViewInversMatrix", 0xD0),
        _matrix("m_LightMapMatrix", 0x100),
        _matrix("m_WorldViewProjectionMatrix", 0x140),
        _f("m_LightFadeValueStatic", 0x180),
        _f("m_LightFadeValueDynamic", 0x184),
        Member("trailing", 0x188, "float", 2, "the last 8 B of the declared 400 B carry no name this table knows"),
    ),
    declared=400,
    bound=512,
    note="400 B declared, 512 B bound",
)

# ---------------------------------------------------------------------------------------------------------------------
# The flat post buffers: one or more float4 rows, no member names known
# ---------------------------------------------------------------------------------------------------------------------


def _flat(name: str, size: int, note: str = "") -> Layout:
    return Layout(name=name, members=(), declared=size, flat=True,
                  note=note or f"{size} B = {size // 4} floats")


# g_CameraParameter: the view, projection and previous-frame matrices every shader of the frame reads. The members, their
# offsets and the 944 B size are the engine's own shader reflection (the deferred composite 0x3ED01E99): the view is
# declared as three rows (a float3x4, 48 B a piece) and everything else as 4x4s, and an offline pass rebuilds the same
# table from these bytes.
CAMERA_PARAMETER = Layout(
    name="g_CameraParameter",
    members=(
        _matrix3x4("m_ViewMatrix", 0x000, "row-major 3x4: the view's three rows"),
        _matrix3x4("m_InverseViewMatrix", 0x030, "row-major 3x4"),
        _matrix("m_ViewProjectionMatrix", 0x060),
        _matrix("m_InverseViewProjectionMatrix", 0x0A0),
        _matrix("m_InverseProjectionMatrix", 0x0E0, "what a view position is rebuilt from"),
        _matrix("m_ProjectionMatrix", 0x120, "the matrix the reflection pass projects with"),
        _matrix("m_MainViewToProjectionMatrix", 0x160),
        _matrix3x4("m_ViewMatrixPrev", 0x1A0),
        _matrix3x4("m_InverseViewMatrixPrev", 0x1D0),
        _matrix("m_ViewProjectionMatrixPrev", 0x200),
        _matrix("m_InverseViewProjectionMatrixPrev", 0x240),
        _matrix("m_ProjectionMatrixPrev", 0x280),
        _matrix("m_InverseProjectionMatrixPrev", 0x2C0),
        _matrix("m_ProjToProjPrevMatrix", 0x300),
        _matrix3x4("m_ViewToViewPrevMatrix", 0x340),
        _matrix3x4("m_MainViewToWorldMatrix", 0x370),
        _f4("m_JitterVelocity", 0x3A0, "the frame's jitter difference, for a temporal pass"),
    ),
    declared=944,
    note="944 B: seventeen members, the view matrices as 3x4 rows and the projections as 4x4 (0x3ED01E99's reflection)",
)

CBRIGHT_PASS_PARAM = _flat("cBrightPassParam", 16, "the bright-pass threshold and knee")
CMERGE_WEIGHT = _flat("cMergeWeight", 16, "the blend weight of each glare tier")
# The glare chain's two parameter structs, whose member names and offsets are the shader's own reflection (the temporal
# accumulator 0xAE4A07BE and the composite 0x749A1AA3). They were flat float lists before: the names are what lets a report
# say which of them moves between two samples instead of "one of the 24 floats".
CGLARE_PARAM = Layout(
    name="cGlareParam",
    members=(
        _matrix("prevProjToView", 0x00, "row-major 4x4: the previous frame's projection to the view"),
        _f("prevWeight", 0x40),
        _f("prevRangeScale", 0x44),
        Member("uvToPrevUvRange", 0x48, "float", 2, "x = the range the previous uv is scaled into, y = its offset"),
        Member("invPrevUvRange", 0x50, "float", 2),
        Member("currentUvPixelRange", 0x58, "float", 2),
    ),
    declared=96,
    note="96 B: the temporal glare accumulator's one struct, three float2 members at the end",
)

CSOFT_FOCUS_PARAM = Layout(
    name="cSoftFocusParam",
    members=(
        Member("currentUvPixelRange", 0x00, "float", 2),
        _f("softFocusRate", 0x08),
        _f("glareCompositeRate", 0x0C, "the rate the glare composite adds the glare buffer to the scene with"),
    ),
    declared=16,
    note="16 B: the glare composite's rates",
)

CTONEMAP_PARAM = _flat("cToneMapParam", 32, "the tone-map LUT parameters")
CCOMMON_TEX_PARAM = _flat("cCommonTexParam", 16, "the tone-map draw's common texture parameters")
CFILTER_FOR_DARK = _flat("cFilterForDark", 48, "the colour filter's shadow-range matrix (3 x float4)")
CDARK_FILTER_PARAM = _flat("cDarkFilterParam", 16, "the colour filter's dark-range parameters")
CBG_AMBIENT_PARAMETER = _flat("g_BGAmbientParameter", 16, "the composite's ambient scalar row; the layout is not known")
# The fog buffers.  ``cFogParam`` was flat before (8 x float4, no member names); the Fog_GU pass's own disassembly
# (the fxc dump of the 0x57A9C406 blob) reads every member the table below names: u = lutCoordA
# + lutCoordB / worldDist through the LUT, the colour base lerp from fogColour toward the sky, the sun-angle inscatter
# from diColour + sunDirection, the two-layer height profile at the pixel's own world y, and the 3-D volume-map branch
# the volumeMapBlend switch opens.  ``row3xyz``/``row6``/``row7`` are the rows that shader does not read.
CFOG_PARAM = Layout(
    name="cFogParam",
    members=(
        _f4("fogColour", 0x00, "[0]: the colour the fog lerps toward (its .w is not read at this pass)"),
        _f3("diColour", 0x10, "[1].xyz: the sun-angle inscatter's colour"),
        _f("diIntensity", 0x1C, "[1].w: the inscatter's gain"),
        _f3("sunDirection", 0x20, "[2].xyz: the direction the inscatter's angle terms dot (the fog state's own sun)"),
        _f("volumeMapBlend", 0x2C, "[2].w: the 3-D volume map's blend switch; 1.0 = the volume branch never runs"),
        Member("row3xyz", 0x30, "float", 3, "not read by the Fog_GU asm (the LUT's u rides [3].w)"),
        _f("lutCoordA", 0x3C, "[3].w: the LUT's u: u = [3].w + [4].w / worldDist"),
        _f3("cameraPosition", 0x40, "[4].xyz: the fog post's own camera position (the ray is the pixel's point minus it)"),
        _f("lutCoordB", 0x4C, "[4].w: the LUT u's slope on 1/worldDist"),
        _f("depthToDistanceA", 0x50, "[5].x: worldDist = [5].x + [5].y * depth"),
        _f("depthToDistanceB", 0x54, "[5].y: the slope above"),
        _f("volumeDepthScale", 0x58, "[5].z: the volume map's depth-axis scale (sqrt(max(0, depth - 0.5) * here))"),
        _f("minDepth", 0x5C, "[5].w: the depth sample floor the projection reconstructs with"),
        Member("row6", 0x60, "float", 4, "not read by the Fog_GU asm"),
        Member("row7", 0x70, "float", 4, "not read by the Fog_GU asm"),
    ),
    declared=128,
    note="128 B = 8 x float4; the members are the Fog_GU asm's reads (the fog postbody asm lines 89-179)",
)

# The Fog_GU post's height profile and its inscatter/opacity block, from the same asm: t' = exp(-(dist - start) * pop(y))
# with pop(y) = density1 * exp(fallOff1 * (refHeight1 - y) * 0.545968) + layer 2 the same way, floored at minOpacity;
# the mix weight = sat((1 - t') * mixScale), and the inscatter's gain/exponent/start follow (asm lines 107-160).
CEXP_HEIGHT_FOG_PARAM = Layout(
    name="cExpHeightFogParam",
    members=(
        _f("distanceStart", 0x00, "[0].x: the distance where the fog's own distance term starts"),
        _f("fallOff1", 0x04, "[0].y: layer 1's falloff (the asm clamps it at 1e-4)"),
        _f("density1", 0x08, "[0].z: layer 1's density"),
        _f("refHeight1", 0x0C, "[0].w: layer 1's reference height"),
        _f("minOpacity", 0x10, "[1].x: the opacity floor the distance term clamps to"),
        _f("fallOff2", 0x14, "[1].y: layer 2's falloff (the asm clamps it at 1e-4)"),
        _f("density2", 0x18, "[1].z: layer 2's density"),
        _f("refHeight2", 0x1C, "[1].w: layer 2's reference height"),
        _f("mixScale", 0x20, "[2].x: the weight the opaque distance term is scaled by (the post's mix = sat((1-t')*here))"),
        _f("diIntensity", 0x24, "[2].y: the sun-angle inscatter's second gain (beside cFogParam[1].w)"),
        _f("diExponent", 0x28, "[2].z: the sun-angle exponent (the asm clamps it at 1)"),
        _f("diStartDistance", 0x2C, "[2].w: the DI inscatter's own start, subtracted from the distance-from-start"),
        Member("boundTail", 0x30, "float", 4, "beyond the declared 48 B: the game binds 64 B"),
    ),
    declared=48,
    bound=64,
    note="48 B declared, 64 B bound: two layer rows plus the DI/mix row",
)

# The material family's own fog state: the FogParameter struct the skin/hair blobs' reflection carries (the layout is the
# compiler's own, reproduced from blob 0x555C97C8's fxc dump).  The cameraPos/sunDir rows are the two a march readback
# takes without any other machinery; useHeightFogUpdate is the height branch's mix weight, whose polarity (transmittance
# at the materials, opacity at the post) is the open question a live sample of this buffer settles.
G_FOG_PARAMETER = Layout(
    name="g_FogParameter",
    members=(
        _f4("fogColor", 0x00),
        Member("fogIntensity", 0x10, "float", 2, "unread by the skin blob's fog block asm"),
        Member("fogStart", 0x18, "float", 2, "unread by the skin blob's fog block asm"),
        Member("depthToDistance", 0x20, "float", 2, "the post path maps world distance = [5].x + [5].y * depth"),
        _f("lutParamX", 0x28, "u = lutParamX + lutParamY * (-viewZ)"),
        _f("lutParamY", 0x2C, "the LUT u's slope above"),
        _f("startDistance", 0x30),
        _f("fogMinOpacity", 0x34),
        _f("fogDensity", 0x38),
        _f("expFogHeight", 0x3C),
        _f("fogHeightFallOff", 0x40),
        _f("fogDensity2", 0x44),
        _f("expFogHeight2", 0x48),
        _f("fogHeightFallOff2", 0x4C),
        _f("diStartDistance", 0x50),
        _f("padding", 0x54, "unused in the reflection"),
        _f("diExponent", 0x58),
        _f("useHeightFogUpdate", 0x5C, "the height branch's mix weight (the polarity question's live value)"),
        _f3("diColor", 0x60),
        _f("diColorIntensity", 0x6C, "the DI gain (squared in the asm)"),
        _f3("cameraPos", 0x70),
        _f("padding1", 0x7C, "the reflection's padding1"),
        _f3("sunDir", 0x80),
        _f("padding2", 0x8C, "the reflection's padding2"),
        _f("angleAttenStart", 0x90),
        _f("invAngleAttenLength", 0x94),
        _f("angleAttenIntensity", 0x98),
        _f("padding3", 0x9C, "the reflection's padding3"),
    ),
    declared=160,
    note="160 B FogParameter struct; member names and offsets blob 0x555C97C8's reflection lists",
)

# The chara draws' own ambient block (cb8/cb9): the AMBIENT_LIGHT struct the chara blob 0x2752B88F's reflection names.
# Same shape family as the composite's 64-probe table (g_AmbientParamArray above), but a single per-draw row with the
# sky's own SH beside the per-env one, and envLocationIndexPrev/InterpRate at the root.
AMBIENT_LIGHT_PARAM = Layout(
    name="g_AmbientParam",
    members=(
        Member("sh", 0x00, "float", 12, "3 x float4 (AMBIENT_LIGHT.param.sh): the draw's per-env SH"),
        _f3("color", 0x30),
        _f("shRecipScale", 0x3C),
        _f("attenuationSlope", 0x40),
        _f("attenuationIntercept", 0x44),
        _f("attenuationValue", 0x48),
        _f("skyVisibility", 0x4C),
        _f("reflectionScale", 0x50),
        _f("reflectionOffset", 0x54),
        _f("bakeLightRate", 0x58),
        _u("envLocationIndex", 0x5C, "an index per the name (the reflection gives no type)"),
        Member("skySh", 0x60, "float", 12, "3 x float4 (AMBIENT_LIGHT.skySh): the sky's own SH beside the per-env one"),
        _f("skyShRecipScale", 0x90),
        _u("envLocationIndexPrev", 0x94, "an index per the name (the reflection gives no type)"),
        _f("envLocationInterpRate", 0x98),
        _f("invAmbientScale", 0x9C),
    ),
    declared=160,
    note="160 B AMBIENT_LIGHT struct: the chara draws' own row (blob 0x2752B88F's reflection)",
)

# The composite's common scale rows, the CommonParameter struct the chara blob 0x2752B88F's reflection declares (four
# float4 rows: render target size, viewport, and the frame's misc scales whose semantics are not decoded).  This is also
# the chara material's cb1: most of the frame's passes bind one, so the rows are sampleable nearly every second.
COMMON_PARAMETER = Layout(
    name="g_CommonParameter",
    members=(
        _f4("m_RenderTarget", 0x00),
        _f4("m_Viewport", 0x10),
        _f4("m_Misc", 0x20),
        _f4("m_Misc2", 0x30),
    ),
    declared=64,
    note="64 B CommonParameter struct: m_RenderTarget/m_Viewport/m_Misc/m_Misc2 (blob 0x2752B88F's reflection)",
)

# The game's classic DoF weight pass's parameter: the Weight_Update disassembly pins the byte offsets, but this table
# has no member semantics beyond them and no live sample yet, so the floats are reported per offset and the member
# decode waits for a capture taken while the game's own depth of field runs.
CWEIGHT_SAMPLING_PARAM = _flat(
    "cWeightSamplingParam", 192,
    "192 B: the classic DoF weight pass's parameter (cocParam@0, blur rates@24/28, prevProjToView@32, prevWeight@96 "
    "- flat until a live sample lands)")

# The composite's per-material shading table: the ShaderTypeParameter struct row (a structured SRV the one-frame dump
# takes at shadertypes.bin, one 128-B row per material, the row index gbuffer0.a names).  The names and offsets are
# composite blob 0x3ED01E99's own reflection (a capture's fixture list keeps the dxbc).
SHADER_TYPE_PARAMETER = Layout(
    name="g_ShaderTypeParameter",
    members=(
        _u("lightingType", 0x00, "the branch key (=1: the subsurface light)"),
        _u("subSurfaceProfileId", 0x04, "the sub-surface profile's index"),
        _f("subSurfaceWidth", 0x08),
        _f("backScatterPower", 0x0C),
        _f("sheenRate", 0x10),
        _f("sheenTintRate", 0x14),
        _f("sheenAperture", 0x18),
        _f("useSubSurfaceRate", 0x1C),
        _f("hairScatterColorShift", 0x20),
        _f3("hairSpecularShift", 0x24),
        _f("furLength", 0x30),
        Member("hairRoughnessOffsetRate", 0x34, "float", 2),
        _f("subSurfacePower", 0x3C),
        Member("reserve", 0x40, "float", 16, "float4[4]: the composite's own header names Reserve"),
    ),
    declared=128,
    note="128 B/row; the row index is the G-buffer's gbuffer0.a byte (the composite decodes a*255+0.5)",
)

# The engine's flare family: the halo ring, the sun sprite and the ghost step of each radial-blur pass. The members are the
# shaders' own reads (each is a float4 list indexed by `[row].component`): the two that matter most are the sun's screen UV
# (`[0].zw`, which the shafts module's own projection can be compared with) and the ring's colour and radius.
HALO_PARAM = Layout(
    name="cHaloParam",
    members=(
        Member("uvScale", 0x00, "float", 2, "[0].xy: the uv scale the distance from the sun is measured in"),
        Member("sunUv", 0x08, "float", 2, "[0].zw: the sun's screen UV, the ring's centre"),
        _f("skyMaskSharpness", 0x10, "[1].x: how sharply the ring follows the sky's own mask"),
        _f("threshold", 0x14, "[1].y: the sky mask's threshold"),
        _f("ringRadius", 0x18, "[1].z: the ring's radius, in the units uvScale measures"),
        _f("maskWeight", 0x1C, "[1].w: the sky mask's weight"),
        _f3("ringColour", 0x20, "[2].xyz: the colour the ring adds"),
        _f("maskFill", 0x2C, "[2].w: the fill the mask leaves"),
    ),
    declared=48,
    bound=64,
    note="48 B declared, 64 B bound: three float4 rows, the ring's own colour and the sun's screen UV",
)

SUN_PARAM = Layout(
    name="cSunParam",
    members=(
        Member("discScale", 0x00, "float", 2, "[0].xy: the sun disc's scale"),
        Member("sunUv", 0x08, "float", 2, "[0].zw: the sun's screen UV, the sprite's centre"),
        _f("rayFrequency", 0x10, "[1].x: the angular ray pattern's frequency"),
        _f("rayPhase", 0x14, "[1].y: its phase"),
        _f("rayGainX", 0x18, "[1].z: a gain of the ray pattern"),
        _f("rayGainY", 0x1C, "[1].w: the other gain"),
        _f4("perChannelFalloffs", 0x20, "[2].xyzw: one radial falloff exponent per channel"),
        _f3("colourFloorRgb", 0x30, "[3].xyz: the colour the glow falls back to"),
        _f("colourFloorW", 0x3C, "[3].w: that colour's weight"),
        _f3("outputColour", 0x40, "[4].xyz: the colour the sprite is multiplied by"),
        _f("intensity", 0x4C, "[4].w: the sprite's strength"),
    ),
    declared=80,
    bound=128,
    note="80 B declared, 128 B bound: the disc scale, the sun's screen UV, the ray pattern and the output colour",
)

RADIAL_BLUR_PARAM = Layout(
    name="cRadialBlurParam",
    members=(
        Member("centre", 0x00, "float", 2, "[0].xy: the zoom's centre (a different buffer per radial-blur pass)"),
        _f("decay", 0x08, "[0].z: the weight the taps fall off by"),
        _f("step", 0x0C, "[0].w: how far each tap moves towards the centre - the ghost spacing"),
    ),
    declared=16,
    note="16 B: the zoom centre, the decay and the step one radial-blur pass drags the sun sprite by",
)

# The engine's own shadow bias, bound beside the atlas in the same draws (the mask's assembly writer reads it as cb2[0]).
# Its member names come from that shader's bytecode, not from any reflection: the offset's width `[0].y` is used whole or
# halved per pixel, and `[0].z` is the capacity the |dot(normal, light)| is divided by before it prunes the offset.
SHADOW_BIAS_PARAMETER = Layout(
    name="g_ShadowBiasParameter",
    members=(
        _f("offsetX", 0x00, "[0].x: not read by the mask writer's shadow block"),
        _f("offsetWidth", 0x04, "[0].y: the normal offset's width in the light-space units the projection works in"),
        _f("dotCapacity", 0x08, "[0].z: the |dot(normal, to-light)| capacity that scales the offset down terminally"),
        _f("unusedW", 0x0C, "[0].w: not read by the mask writer's shadow block"),
    ),
    declared=16,
    note="16 B: the bias the engine's own shadow evaluation subtracts before it compares the atlas",
)

LAYOUTS: dict[str, Layout] = {
    layout.name: layout
    for layout in (
        WETNESS_PARAMETER,
        AMBIENT_PARAM_ARRAY,
        DIRECTIONAL_SHADOW_PARAMETER,
        REFLECTION_PARAMETER,
        LIGHT_PARAM,
        CAMERA_PARAMETER,
        CBRIGHT_PASS_PARAM,
        CMERGE_WEIGHT,
        CGLARE_PARAM,
        CSOFT_FOCUS_PARAM,
        CTONEMAP_PARAM,
        CCOMMON_TEX_PARAM,
        CFILTER_FOR_DARK,
        CDARK_FILTER_PARAM,
        CFOG_PARAM,
        CBG_AMBIENT_PARAMETER,
        HALO_PARAM,
        SUN_PARAM,
        RADIAL_BLUR_PARAM,
        SHADOW_BIAS_PARAMETER,
        G_FOG_PARAMETER,
        CEXP_HEIGHT_FOG_PARAM,
        CWEIGHT_SAMPLING_PARAM,
        AMBIENT_LIGHT_PARAM,
        COMMON_PARAMETER,
        SHADER_TYPE_PARAMETER,
    )
}

# The buffer names a capture samples, in the order the diagnostic writes them.
DUMP_NAMES = tuple(layout.name for layout in (
    WETNESS_PARAMETER,
    AMBIENT_PARAM_ARRAY,
    DIRECTIONAL_SHADOW_PARAMETER,
    REFLECTION_PARAMETER,
    CBG_AMBIENT_PARAMETER,
    CAMERA_PARAMETER,
    CBRIGHT_PASS_PARAM,
    CMERGE_WEIGHT,
    CGLARE_PARAM,
    CTONEMAP_PARAM,
    CCOMMON_TEX_PARAM,
    CFILTER_FOR_DARK,
    CDARK_FILTER_PARAM,
    CFOG_PARAM,
    HALO_PARAM,
    SUN_PARAM,
    RADIAL_BLUR_PARAM,
    SHADOW_BIAS_PARAMETER,
    G_FOG_PARAMETER,
    CEXP_HEIGHT_FOG_PARAM,
    CWEIGHT_SAMPLING_PARAM,
    AMBIENT_LIGHT_PARAM,
    COMMON_PARAMETER,
))


# ---------------------------------------------------------------------------------------------------------------------
# Reading bytes
# ---------------------------------------------------------------------------------------------------------------------


def base_name(name: str | None) -> str:
    """A reflected name without the effect compiler's type suffix (``.T``/``.S``), which a buffer never has anyway."""

    if not name:
        return ""
    text = name.strip()
    if len(text) > 2 and text[-2] == "." and text[-1] in ("T", "S"):
        return text[:-2]
    return text


def layout_of(name: str | None) -> Layout | None:
    """The layout of a reflected buffer name, or ``None`` when this table does not know it."""

    return LAYOUTS.get(base_name(name))


def _available(data: bytes, offset: int, size: int) -> bool:
    return offset >= 0 and offset + size <= len(data)


def _scalar(data: bytes, offset: int, kind: str) -> Any | None:
    if not _available(data, offset, 4):
        return None
    if kind == "uint":
        return struct.unpack_from("<I", data, offset)[0]
    return struct.unpack_from("<f", data, offset)[0]


def _vector(data: bytes, offset: int, count: int) -> list[float] | None:
    size = 4 * count
    if not _available(data, offset, size):
        return None
    return list(struct.unpack_from(f"<{count}f", data, offset))


def _member_value(data: bytes, member: Member) -> Any | None:
    if not _available(data, member.offset, member.size):
        return None
    if member.kind == "uint":
        return struct.unpack_from("<I", data, member.offset)[0]
    if member.kind == "float":
        values = list(struct.unpack_from(f"<{member.count}f", data, member.offset))
        return values[0] if member.count == 1 else values
    return _vector(data, member.offset, _KIND_COUNT[member.kind] * member.count)


def floats_of(data: bytes, limit: int = 4096) -> list[float]:
    """Every whole float the buffer holds, up to ``limit`` (a diagnostic aid for a buffer this table does not know)."""

    count = min(len(data) // 4, limit)
    return list(struct.unpack_from(f"<{count}f", data, 0)) if count else []


def decode_layout(layout: Layout, data: bytes) -> dict[str, Any]:
    """Decode ``data`` with ``layout``: member values, the members the buffer did not reach, and any repeated probes."""

    result: dict[str, Any] = {
        "name": layout.name,
        "known": True,
        "flat": layout.flat,
        "bytes": len(data),
        "declared": layout.declared,
        "bound": layout.bound,
        "note": layout.note,
        "members": {},
        "floats": [],
        "short": [],
        "probes": [],
        "probe_available": 0,
        "probe_short": [],
    }
    if layout.flat:
        result["floats"] = floats_of(data)
        if layout.declared is not None and len(data) < layout.declared:
            result["short"].append("floats")
        return result
    for member in layout.members:
        value = _member_value(data, member)
        if value is None:
            result["short"].append(member.name)
        else:
            result["members"][member.name] = value
            if member.float_too and isinstance(value, int) and value > _UINT_AS_FLOAT_LIMIT:
                # The same four bytes as a float: the member's declared type is not recorded, and no count, index or
                # enum reaches this far, so the integer reading alone would be a bit pattern printed as a number.
                float_value = struct.unpack_from("<f", data, member.offset)[0]
                result["members"][f"{member.name}_f32"] = float_value
    if layout.probe is not None:
        probes = []
        for index in range(layout.probe_count):
            offset = layout.probe_offset + index * layout.probe.size
            if offset >= len(data):
                break
            probe = decode_layout(layout.probe, data[offset:])
            probe["index"] = index
            probes.append(probe)
            if probe["short"]:
                result["probe_short"].append(index)
        result["probes"] = probes
        result["probe_available"] = len(probes)
    return result


def decode(name: str | None, data: bytes | bytearray | memoryview) -> dict[str, Any]:
    """Decode the bytes of a named buffer.

    A name this table does not know still decodes: the buffer comes back as a float list (``known`` is ``False``) so a
    report can show it instead of dropping the sample.  A short buffer never raises; the members it did not reach are in
    ``short`` and the probes it did not reach are in ``probe_short``.
    """

    raw = bytes(data or b"")
    layout = layout_of(name)
    if layout is None:
        return {
            "name": base_name(name) or "-",
            "known": False,
            "flat": True,
            "bytes": len(raw),
            "declared": None,
            "bound": None,
            "note": "no layout in this table",
            "members": {},
            "floats": floats_of(raw),
            "short": [],
            "probes": [],
            "probe_available": 0,
            "probe_short": [],
        }
    result = decode_layout(layout, raw)
    result["name"] = base_name(name) or layout.name
    return result


def member_names(name: str | None) -> list[str]:
    """The member names of a layout, in offset order (empty for a flat or an unknown buffer)."""

    layout = layout_of(name)
    if layout is None or layout.flat:
        return []
    return [member.name for member in layout.members]


def probe_names(name: str | None = "g_AmbientParamArray") -> list[str]:
    layout = layout_of(name)
    if layout is None or layout.probe is None:
        return []
    return [member.name for member in layout.probe.members]


def scalars(decoded: dict[str, Any]) -> dict[str, float | int]:
    """The decoded members that are single values, which is what a min/max/delta over samples wants."""

    return {key: value for key, value in decoded.get("members", {}).items()
            if isinstance(value, (int, float)) and not isinstance(value, bool)}


def components(decoded: dict[str, Any]) -> dict[str, float | int]:
    """Every scalar component, with a vector member split into ``name[0]``, ``name[1]``, ... .

    This is the flat view a per-member min/max/delta over samples uses: one entry per number the buffer carries, so a
    change in one component of a float4 is visible instead of drowned by the others.
    """

    result: dict[str, float | int] = {}
    for key, value in decoded.get("members", {}).items():
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            result[key] = value
        elif isinstance(value, list):
            for index, item in enumerate(value):
                if isinstance(item, (int, float)) and not isinstance(item, bool):
                    result[f"{key}[{index}]"] = item
    return result


def describe() -> list[str]:
    """One line per known layout, for ``--layouts`` and the selftest's sanity checks."""

    lines = []
    for layout in LAYOUTS.values():
        sizes = f"{layout.size} B declared"
        if layout.bound:
            sizes += f", {layout.bound} B bound"
        if layout.probe is not None:
            sizes += f", {layout.probe_count} x {layout.probe.size} B probes"
        lines.append(f"{layout.name:32s} {sizes}")
    return lines


if __name__ == "__main__":
    for line in describe():
        print(line)
