#pragma once
// Engine light controls, writer 1 (branch eng-cb): the constant-buffer patch family. The pure rules. No pch, no Windows, D3D or shader call, so every
// decision here is covered by tests/ext/ffxiv/enginelight on the host.
//
// What the family does, in one line: it scales numbers inside the constant buffers the ENGINE computes its night light from, through the engine
// pass overrides' one CbPatch primitive (ext/ffxiv/engine/CbPatch.h), for exactly one draw each, and puts the game's own buffer back afterwards.
//
//   arm             draw class                                              buffer / slot            entries (byte offsets in the buffer)
//   moon            main directional light (mask + fake specular)           g_LightParam, 512 B      +32 diffuse.xyz, +48 specular.xyz  x moon x tint_c
//   water moon      the water pass's copy of the directional light          g_LightParam, 512 B      +32, +48  x waterMoon x moonTint_c   (REPLACES the moon factor)
//   ambient         the deferred composite                                  g_AmbientParamArray      SH rows R/G/B (64+16c, stride 192, x64), header sky scale
//   water ambient   the water surface (g_AmbientParam + g_WaterParameter)   g_AmbientParam, 256 B    SH rows 0..2, sky SH rows 6..8, colour, reflection scale
//
// The weight is the night weight (writer 2's feed): every factor is Effective(p, factor, w) = lerp(1, factor, w), so by day (w = 0), with the master
// off, or with the A/B showing the engine's values, every entry is the identity and NOTHING is patched (no copy, no dispatch, no swap).

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader (plain structs, no device)
#include "ext/ffxiv/enginelight/DumpHooks.h"
#include "ext/ffxiv/enginelight/EngineLightContract.h"
#include "ext/ffxiv/engine/EnginePassRules.h"

#include <cstdint>
#include <format>
#include <string>

namespace ext::ffxiv::enginelight::cb
{
namespace eng = ext::ffxiv::engine;

// ---- the factors ----------------------------------------------------------------------------------------------------------

inline constexpr float kIdentityEpsilon = 0.001f; // a thousandth away from 1 is not a look: running a patch for it would cost a copy and a dispatch
inline constexpr float kMaxFactor = 16.0f;        // the module's own guard against an absurd published value (the card clamps tighter)

struct Rgb
{
    float c[3] { 1.0f, 1.0f, 1.0f };
};

constexpr float Finite(float v) noexcept { return v == v ? v : 1.0f; } // NaN reads as 1: a broken value never darkens the game

constexpr float Clamped(float v) noexcept
{
    v = Finite(v);
    return v < 0.0f ? 0.0f : (v > kMaxFactor ? kMaxFactor : v);
}

constexpr float Eff(const Params& p, float factor, float weight) noexcept { return Clamped(Effective(p, Finite(factor), Finite(weight))); }

constexpr bool IsIdentityFactor(float v) noexcept { return !(v > 1.0f + kIdentityEpsilon) && !(v < 1.0f - kIdentityEpsilon); }

constexpr bool IsIdentityRgb(const Rgb& m) noexcept { return IsIdentityFactor(m.c[0]) && IsIdentityFactor(m.c[1]) && IsIdentityFactor(m.c[2]); }

// The moon at its light draw: intensity x tint, per channel.
constexpr Rgb MoonFactors(const Params& p, float w) noexcept
{
    Rgb out;
    for (int i = 0; i < 3; ++i)
        out.c[i] = Eff(p, p.moonIntensity, w) * Eff(p, p.moonTint[i], w);
    return out;
}

// The water pass's copy of the same light: the water's OWN moon term replaces the moon intensity (stacking would give 0.1 x 0.2 = 0.02 on a pass that
// is 99.4 % moon; waterMoon is the whole-water target). The tint is the moon's colour, so it applies to both draws.
constexpr Rgb WaterMoonFactors(const Params& p, float w) noexcept
{
    Rgb out;
    for (int i = 0; i < 3; ++i)
        out.c[i] = Eff(p, p.waterMoon, w) * Eff(p, p.moonTint[i], w);
    return out;
}

constexpr Rgb AmbientFactors(const Params& p, float w) noexcept
{
    Rgb out;
    for (int i = 0; i < 3; ++i)
        out.c[i] = Eff(p, p.ambientIntensity, w) * Eff(p, p.ambientTint[i], w);
    return out;
}

constexpr float AmbientScalar(const Params& p, float w) noexcept { return Eff(p, p.ambientIntensity, w); }

constexpr Rgb WaterAmbientFactors(const Params& p, float w) noexcept
{
    Rgb out;
    for (int i = 0; i < 3; ++i)
        out.c[i] = Eff(p, p.waterAmbient, w) * Eff(p, p.waterTint[i], w);
    return out;
}

constexpr float WaterAmbientScalar(const Params& p, float w) noexcept { return Eff(p, p.waterAmbient, w); }

// Whether the family has anything to do at this published state: any arm off the engine's value. False with the master off, the A/B showing the
// engine's values, a day weight, or every control at 1.
constexpr bool FamilyIdentity(const Params& p, float w) noexcept
{
    return IsIdentityRgb(MoonFactors(p, w)) && IsIdentityRgb(WaterMoonFactors(p, w)) && IsIdentityRgb(AmbientFactors(p, w)) &&
           IsIdentityRgb(WaterAmbientFactors(p, w));
}

// ---- the entry tables -----------------------------------------------------------------------------------------------------

// g_LightParam as the engine binds it (the pooled ring of 512-byte buffers; the shader declares 392): the diffuse colour at +32 (cb2[2].xyz, the LD
// target) and the specular colour at +48 (cb2[3].xyz, the LS target). The .w of both float4s keeps the game's bits (multiplier 1).
inline constexpr uint32_t kLightBoundBytes = 512;
inline constexpr uint32_t kLightDiffuseOffset = 32;
inline constexpr uint32_t kLightSpecularOffset = 48;
inline constexpr uint32_t kLightEntryCount = 2;

constexpr eng::ScaleEntry RgbEntry(uint32_t byteOffset, const Rgb& m) noexcept
{
    eng::ScaleEntry entry;
    entry.byteOffset = byteOffset;
    entry.stride = 16;
    entry.count = 1;
    entry.multiplier[0] = m.c[0];
    entry.multiplier[1] = m.c[1];
    entry.multiplier[2] = m.c[2];
    return entry;
}

// The light draw's table: diffuse and specular scaled by the same per-channel factor. 0 entries = identity (no patch).
constexpr uint32_t LightEntries(const Rgb& m, eng::ScaleEntry* out, uint32_t capacity) noexcept
{
    if (out == nullptr || capacity < kLightEntryCount || IsIdentityRgb(m))
        return 0;
    out[0] = RgbEntry(kLightDiffuseOffset, m);
    out[1] = RgbEntry(kLightSpecularOffset, m);
    return eng::EntryFits(out[0], kLightBoundBytes) && eng::EntryFits(out[1], kLightBoundBytes) ? kLightEntryCount : 0u;
}

// The composite's ambient table g_AmbientParamArray (header 64 B + 64 probes of 192 B; bound 16,384 or declared 12,352). Probe k holds the SH rows
// R/G/B at +0/+16/+32 (the composite's dp4_sat(N,1; row) per channel). One entry per row carries that channel's factor, so the tint is per channel.
// The header's skyShRecipScale (byte 4, the first float4's .y) is the sky SH of the specular blend: scaled by the intensity alone, otherwise the
// specular fill leans on an unscaled sky.
inline constexpr uint32_t kAmbientRowsOffset = 64;
inline constexpr uint32_t kAmbientRowStride = eng::kAmbientProbeBytes; // 192
inline constexpr uint32_t kAmbientRowCount = eng::kAmbientProbes;      // 64
inline constexpr uint32_t kAmbientHeaderEntryCount = 1;
inline constexpr uint32_t kCompositeEntryCount = 3 + kAmbientHeaderEntryCount;

constexpr eng::ScaleEntry AmbientRowEntry(uint32_t channel, float factor) noexcept
{
    eng::ScaleEntry entry;
    entry.byteOffset = kAmbientRowsOffset + 16u * channel;
    entry.stride = kAmbientRowStride;
    entry.count = kAmbientRowCount;
    for (int i = 0; i < 4; ++i)
        entry.multiplier[i] = factor;
    return entry;
}

constexpr eng::ScaleEntry AmbientHeaderEntry(float factor) noexcept
{
    eng::ScaleEntry entry;
    entry.byteOffset = 0;
    entry.stride = 16;
    entry.count = 1;
    entry.multiplier[1] = factor; // byte 4 = skyShRecipScale
    return entry;
}

constexpr uint32_t CompositeEntries(const Params& p, float w, eng::ScaleEntry* out, uint32_t capacity) noexcept
{
    const Rgb m = AmbientFactors(p, w);
    if (out == nullptr || capacity < kCompositeEntryCount || IsIdentityRgb(m))
        return 0;
    for (uint32_t c = 0; c < 3; ++c)
        out[c] = AmbientRowEntry(c, m.c[c]);
    out[3] = AmbientHeaderEntry(AmbientScalar(p, w));
    for (uint32_t i = 0; i < kCompositeEntryCount; ++i)
        if (!eng::EntryFits(out[i], eng::kAmbientDeclaredBytes) || !eng::EntryFits(out[i], eng::kAmbientBoundBytes))
            return 0;
    return kCompositeEntryCount;
}

// The water surface's own ambient block g_AmbientParam (bound 256 B, declared 160): probe SH rows 0..2 (+0/16/32), colour.xyz + shRecipScale (+48),
// reflection scale/offset (+80), sky SH rows 6..8 (+96/112/128 in the struct = float4 rows 6..8). The rows carry the per-channel factor (the scalar
// shRecipScale/skyShRecipScale stay the engine's: no double application); the colour is the body's ambient term (per channel); the reflection
// scale/offset scale the cube's contribution by the intensity. Eight entries, exactly the table.
inline constexpr uint32_t kWaterAmbientBoundBytes = 256;
inline constexpr uint32_t kWaterAmbientDeclaredBytes = 160;
inline constexpr uint32_t kWaterProbeRowsOffset = 0;
inline constexpr uint32_t kWaterColourOffset = 48;
inline constexpr uint32_t kWaterReflectionOffset = 80;
inline constexpr uint32_t kWaterSkyRowsOffset = 96;
inline constexpr uint32_t kWaterEntryCount = 8;
static_assert(kWaterEntryCount <= eng::kMaxScaleEntries, "the water block's table fits the primitive's entries");

constexpr uint32_t WaterAmbientEntries(const Params& p, float w, eng::ScaleEntry* out, uint32_t capacity) noexcept
{
    const Rgb m = WaterAmbientFactors(p, w);
    if (out == nullptr || capacity < kWaterEntryCount || IsIdentityRgb(m))
        return 0;
    for (uint32_t c = 0; c < 3; ++c)
    {
        out[c] = RgbEntry(kWaterProbeRowsOffset + 16u * c, Rgb { { m.c[c], m.c[c], m.c[c] } });
        out[c].multiplier[3] = m.c[c];
        out[3 + c] = out[c];
        out[3 + c].byteOffset = kWaterSkyRowsOffset + 16u * c;
    }
    out[6] = RgbEntry(kWaterColourOffset, m); // colour.xyz per channel; shRecipScale (.w) keeps the engine's bits
    out[7] = eng::ScaleEntry {};
    out[7].byteOffset = kWaterReflectionOffset;
    out[7].stride = 16;
    out[7].count = 1;
    out[7].multiplier[0] = WaterAmbientScalar(p, w); // reflectionScale
    out[7].multiplier[1] = WaterAmbientScalar(p, w); // reflectionOffset
    for (uint32_t i = 0; i < kWaterEntryCount; ++i)
        if (!eng::EntryFits(out[i], kWaterAmbientDeclaredBytes) || !eng::EntryFits(out[i], kWaterAmbientBoundBytes))
            return 0;
    return kWaterEntryCount;
}

constexpr bool IsLightBufferSize(uint32_t bytes) noexcept { return bytes == kLightBoundBytes; }
constexpr bool IsWaterAmbientBufferSize(uint32_t bytes) noexcept { return bytes == kWaterAmbientBoundBytes || bytes == kWaterAmbientDeclaredBytes; }

// ---- the shader stamp -----------------------------------------------------------------------------------------------------

// What a pixel shader's reflection says, stamped once on the shader object (the pass-index pattern) so a draw reads one integer:
//   bit 0  directional light: declares g_LightParam + g_DirectionalShadowParameter + the shadow-mask sampler (the point-light volumes declare no
//          cascade parameters; the 10 shadow-mask readers declare the cascade atlas and no mask sampler)
//   bit 1  ... and g_FakeSpecularParam: the main view's directional light on this build
//   bit 2  water surface: declares g_AmbientParam + g_WaterParameter (the chara shaders declare g_AmbientParam and never g_WaterParameter)
//   bits 8..15   g_LightParam's slot + 1 (0 = none)
//   bits 16..23  g_AmbientParam's slot + 1 (0 = none)
struct ShaderKind
{
    bool directional = false;
    bool fakeSpecular = false;
    bool water = false;
    uint32_t lightSlot = 0;   // valid when directional
    uint32_t ambientSlot = 0; // valid when water
};

constexpr uint32_t PackShaderKind(const ShaderKind& k) noexcept
{
    uint32_t packed = 0;
    if (k.directional)
        packed |= 1u | ((k.lightSlot + 1u) & 0xFFu) << 8;
    if (k.directional && k.fakeSpecular)
        packed |= 2u;
    if (k.water)
        packed |= 4u | ((k.ambientSlot + 1u) & 0xFFu) << 16;
    return packed;
}

constexpr ShaderKind UnpackShaderKind(uint32_t packed) noexcept
{
    ShaderKind k;
    k.directional = (packed & 1u) != 0 && ((packed >> 8) & 0xFFu) != 0;
    k.fakeSpecular = k.directional && (packed & 2u) != 0;
    k.water = (packed & 4u) != 0 && ((packed >> 16) & 0xFFu) != 0;
    k.lightSlot = k.directional ? ((packed >> 8) & 0xFFu) - 1u : 0u;
    k.ambientSlot = k.water ? ((packed >> 16) & 0xFFu) - 1u : 0u;
    return k;
}

// What a reflection declares, as the names found (the device side parses the bytecode; this is the rule).
struct Declared
{
    bool lightParam = false;
    bool directionalShadow = false;
    bool maskSampler = false;
    bool fakeSpecular = false;
    bool ambientParam = false;
    bool waterParameter = false;
    uint32_t lightSlot = 0;
    uint32_t ambientSlot = 0;
};

constexpr ShaderKind KindOfDeclared(const Declared& d) noexcept
{
    ShaderKind k;
    if (d.lightParam && d.directionalShadow && d.maskSampler && d.lightSlot < eng::kConstantBufferSlots)
    {
        k.directional = true;
        k.fakeSpecular = d.fakeSpecular;
        k.lightSlot = d.lightSlot;
    }
    if (d.ambientParam && d.waterParameter && d.ambientSlot < eng::kConstantBufferSlots)
    {
        k.water = true;
        k.ambientSlot = d.ambientSlot;
    }
    return k;
}

// The names, as the engine's compiler reflects them. The mask is the light draw's own screen-space shadow mask; the cascade atlas' readers declare
// g_SamplerShadow instead, so only the exact mask name counts here (a build that names it otherwise leaves the family unpatched: fail open).
inline constexpr const char* kLightParamName = "g_LightParam";
inline constexpr const char* kDirectionalShadowName = "g_DirectionalShadowParameter";
inline constexpr const char* kMaskSamplerName = "g_SamplerShadowMask";
inline constexpr const char* kFakeSpecularName = "g_FakeSpecularParam";
inline constexpr const char* kAmbientParamName = "g_AmbientParam";
inline constexpr const char* kWaterParameterName = "g_WaterParameter";

inline Declared DeclaredOfBindings(const ext::ffxiv::diag::ShaderBinding* bindings, size_t count)
{
    using ext::ffxiv::diag::BindKind;
    using ext::ffxiv::diag::BindPointOf;
    using ext::ffxiv::diag::kNoBindPoint;
    Declared d;
    const uint32_t light = BindPointOf(bindings, count, BindKind::ConstantBuffer, kLightParamName);
    d.lightParam = light != kNoBindPoint;
    d.lightSlot = d.lightParam ? light : 0u;
    d.directionalShadow = BindPointOf(bindings, count, BindKind::ConstantBuffer, kDirectionalShadowName) != kNoBindPoint;
    d.maskSampler = BindPointOf(bindings, count, BindKind::Texture, kMaskSamplerName) != kNoBindPoint;
    d.fakeSpecular = BindPointOf(bindings, count, BindKind::ConstantBuffer, kFakeSpecularName) != kNoBindPoint;
    const uint32_t ambient = BindPointOf(bindings, count, BindKind::ConstantBuffer, kAmbientParamName);
    d.ambientParam = ambient != kNoBindPoint;
    d.ambientSlot = d.ambientParam ? ambient : 0u;
    d.waterParameter = BindPointOf(bindings, count, BindKind::ConstantBuffer, kWaterParameterName) != kNoBindPoint;
    return d;
}

inline ShaderKind KindOfShader(const void* code, size_t size)
{
    if (code == nullptr || size < 32)
        return ShaderKind {};
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    return KindOfDeclared(DeclaredOfBindings(parsed, count));
}

// ---- the draw classification ------------------------------------------------------------------------------------------------

// The shape the directional light's draws write: two R11G11B10 targets (the diffuse and specular light buffers) at the scene's size.
struct LightDrawFacts
{
    bool replay = false;       // another module's replay of a game draw
    uint32_t rtCount = 0;      // render targets bound (the first two are looked at)
    bool bothR11G11B10 = false;// the first two targets are R11G11B10_FLOAT
    uint32_t width = 0, height = 0;
    uintptr_t rt0 = 0, rt1 = 0; // the targets' resources: the pair identity
    uint32_t boundBytes = 0;    // g_LightParam's bound size
    bool wholeBuffer = true;    // the slot binds the whole buffer (no constant-buffer range)
};

// The frame's memory of its main directional draw: the water pass's copy is the next directional draw of the same size on ANOTHER pair.
struct FrameLight
{
    uint64_t frame = 0;
    bool haveMain = false;
    uint32_t width = 0, height = 0;
    uintptr_t rt0 = 0, rt1 = 0;

    void Begin(uint64_t token) noexcept
    {
        if (token != frame)
            *this = FrameLight { token };
    }
    void NoteMain(const LightDrawFacts& f) noexcept
    {
        haveMain = true;
        width = f.width;
        height = f.height;
        rt0 = f.rt0;
        rt1 = f.rt1;
    }
};

enum class LightKind : uint8_t
{
    None = 0, // not a draw this family patches (a volume, a shadow-mask reader, a preview's light, a replay): uncounted, never a decline
    Main,
    Water,
};

inline constexpr uint32_t kMinSceneWidth = eng::kMinSceneWidth;
inline constexpr uint32_t kMinSceneHeight = eng::kMinSceneHeight;

constexpr bool SceneShaped(const LightDrawFacts& f) noexcept
{
    return !f.replay && f.rtCount >= 2 && f.bothR11G11B10 && f.width >= kMinSceneWidth && f.height >= kMinSceneHeight;
}

constexpr LightKind ClassifyLight(const ShaderKind& shader, const LightDrawFacts& f, const FrameLight& frame) noexcept
{
    if (!shader.directional || !SceneShaped(f))
        return LightKind::None;
    if (shader.fakeSpecular)
        return LightKind::Main;
    // The water pass's copy: no fake specular, the same size as this frame's main draw, on a different pair of targets. The character preview's
    // directional light is not fake-specular either, but its targets are the preview's size.
    if (frame.haveMain && f.width == frame.width && f.height == frame.height && (f.rt0 != frame.rt0 || f.rt1 != frame.rt1))
        return LightKind::Water;
    return LightKind::None;
}

// The water surface: its stamp, not a replay, and the ambient block at a size the engine binds it.
struct WaterDrawFacts
{
    bool replay = false;
    uint32_t boundBytes = 0;
    bool wholeBuffer = true;
};

constexpr bool IsWaterCandidate(const ShaderKind& shader, const WaterDrawFacts& f) noexcept
{
    return shader.water && !f.replay && IsWaterAmbientBufferSize(f.boundBytes) && f.wholeBuffer;
}

// ---- the published state: pending, latched at the frame boundary -------------------------------------------------------------

// Writer 2 publishes whenever Params or the weight change (any thread that can run a config load or the menu). The draw path latches the pending
// state at the first arm of a new output frame, so a frame is patched under one state ("applied from the next frame"). `Asks` covers both: a switch
// off is seen at the next boundary, never mid-frame, and a switch on arms the seam at once.
constexpr Params EngineValues() noexcept
{
    Params p;
    p.master = false;
    return p;
}

struct Published
{
    Params params = EngineValues(); // a module never published stays at the engine's values (Params{} itself has master = true and moon 0.1)
    float weight = 0.0f;
    uint32_t sequence = 0;
};

struct Latch
{
    Published active;
    uint64_t frame = 0;
    bool hasFrame = false;
    uint32_t seen = 0;

    // Returns true when `pending` became the active state.
    bool Begin(uint64_t token, const Published& pending) noexcept
    {
        if (hasFrame && token == frame)
            return false;
        hasFrame = true;
        frame = token;
        if (pending.sequence == seen)
            return false;
        active = pending;
        seen = pending.sequence;
        return true;
    }
};

constexpr bool PublishedIdentity(const Published& s) noexcept { return FamilyIdentity(s.params, s.weight); }

// Whether the composite's ambient entries replace the shadows module's night ambient lift: while the engine light master is on, the card's ambient
// control owns the composite's ambient (NightAmbientLift defaults to 1.0, so nothing changes in the default configuration).
constexpr bool OwnsAmbient(const Published& s) noexcept { return s.params.master; }

// ---- the stand-down ---------------------------------------------------------------------------------------------------------

// The family's own accounting: only a draw this family asked for and could not patch counts, and the streak is the family's own (it never feeds the
// engine module's Session, whose limit switches the shipped knobs off).
inline constexpr uint32_t kFailureLimit = 64;

struct Session
{
    uint32_t failures = 0;
    bool off = false;

    bool NoteFailure() noexcept // true when this failure is the one that switched the family off
    {
        if (off)
            return false;
        if (++failures >= kFailureLimit)
        {
            off = true;
            return true;
        }
        return false;
    }
    void NoteRun() noexcept { failures = 0; }
};

// ---- the F9 rows -----------------------------------------------------------------------------------------------------------

enum class PatchKind : uint8_t
{
    Moon = 0,
    WaterMoon,
    Composite,
    WaterAmbient,
};

constexpr const char* PatchKindName(PatchKind k) noexcept
{
    switch (k)
    {
    case PatchKind::Moon: return "moon";
    case PatchKind::WaterMoon: return "water-moon";
    case PatchKind::Composite: return "composite-ambient";
    case PatchKind::WaterAmbient: return "water-ambient";
    }
    return "?";
}

inline constexpr size_t kDumpMaxBytes = 262144;
inline constexpr uint32_t kDumpMaxRows = 160;

inline std::string DumpHeader()
{
    return "# Engine light controls: the constant buffers the cb patch family changed, before (the game's) and after (the patched copy the draw read)\n"
           "# kind: moon / water-moon = g_LightParam (+0 position, +16 direction, +32 diffuse, +48 specular); composite-ambient = g_AmbientParamArray "
           "(header 64 B, then probe 0); water-ambient = g_AmbientParam (160 declared bytes)\n"
           "frame,kind,slot,game_ptr,bytes_bound,weight,factor_r,factor_g,factor_b,before_hex,after_hex\n";
}

inline std::string Hex(const unsigned char* data, size_t count)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

inline std::string DumpRow(uint64_t frame, PatchKind kind, uint32_t slot, uint64_t gamePtr, uint32_t bytesBound, float weight, const Rgb& factor,
                           const unsigned char* before, const unsigned char* after, size_t count)
{
    return std::format("{},{},{},{:x},{},{:.3f},{:.4f},{:.4f},{:.4f},{},{}\n", frame, PatchKindName(kind), slot, gamePtr, bytesBound, double(weight),
                       double(factor.c[0]), double(factor.c[1]), double(factor.c[2]), Hex(before, count), Hex(after, count));
}
} // namespace ext::ffxiv::enginelight::cb
