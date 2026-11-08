#pragma once
// God rays: the volumetric march (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is
// covered by tests/ext/ffxiv/engine on the host.
//
// What the feature does, in one line: a quarter-res per-pixel ray march through the air in front of each pixel, each sample's visibility read
// from the engine's own cascade shadow atlas (all four cascades, each only inside its own atlas slice, the minimum - the way the engine's own
// mask writer blends them), the medium taken from the engine's own live analytic fog profile (captured per frame at the engine's fog post)
// with an artistic density boost, scattered with a Henyey-Greenstein lobe around the shadow light and integrated as an in-scatter sum with
// extinction (S = sum sigma * phase * V * T * dt), the colour from the engine's own sky-consistent rule or the radial shafts' scan-and-sky
// rule, upsampled depth-aware to the render window and composited into the tone-map owner's scene copy through the same ShaftAdded
// white-bounded expression the radial shafts use, the sky included up to its own 99th percentile. The radial shafts stay; this march is the
// last stage of the tone-map chain they own.
//
// The limits (each a fact of the data, RE report 2026-11-07): the beams follow the engine's SHADOW light, which on the one daylight frame measured is
// 15 degrees above the visible disc (the atlas has no shadow of the disc's own direction); none exist when that light is behind or beside the camera
// or is the dim moon-like light of the evening frames (a luminance gain and an aside gate say so); the range is the atlas' 150 to 190 metres.
//
// Everything that can be decided without a device lives here: the keys and their ranges, the fog constants' decode (the byte offsets the
// engine's own fog post reads, from its disassembly), the density profile, the phase, the march's bounds, the cascade selection, the
// outcomes the chain's hold semantics name, and the log and card lines. The fail-open contract: every reason leaves the game's draw
// exactly as the chain left it - the chain's last good picture when the march is chained (the hold stands), the game's own binding when
// the march is the fallback owner.
//
// The engine facts this rests on (PS-ROADMAP-FOG.md, GODRAYS-S0-DIAG-ATLAS.md, and this run's own disassembly of the mask writer's
// bytecode 0x1257B24B):
//  - the fog post (Fog_GU) binds cFogParam (128 B) + cExpHeightFogParam (64 B bound) + cC2W + cDynamicViewportResolutionParam (32 B),
//    the scene depth at t0, the sky (R16G16B16A16, half the render window) at t1 and the fog weight LUT (R16G16, 256x1) at t2;
//  - the density profile is analytic: rho(y) = d1*exp(f1*(h1-y)*0.545968) + d2*exp(f2*(h2-y)*0.545968) over cExpHeightFogParam;
//  - the shadow writer projects the pixel's VIEW-space position (reconstructed through the inverse projection at g_CameraParameter
//    +0x0E0) with the cascade's matrix, compares `shadowed when z >= gathered`, corrects a disc tap's depth by |tap| * (cb3[7].x * -15),
//    and fades by the pixel's view z through cb3[6] and cb3[4].z/cb3[5].z/cb3[5].w;
//  - the cascades share the matrix's third row (the light axis), so the light-space z mapping is cascade-independent and a sample's
//    cascade is the one whose matrix projects it inside [0,1]^2 - finest (largest uv-per-world scale) first.

#include "ext/ffxiv/engine/EnginePassRules.h" // the packed anchor word this module claims one bit of, and the session's knob
#include "ext/ffxiv/engine/ShaftsRules.h"     // ShaftAdded (the composite's bound), Window, the sun projection, the tint rules

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace ext::ffxiv::godrays
{
// ---- the keys ------------------------------------------------------------------------------------------------------------

// The march's sample count, clamped: fewer than 16 bands visibly, more than 96 costs more than the effect is worth at quarter res
// (the radial's own kMinSamples/kMaxSamples band, the march's own key).
inline constexpr int32_t kMinSteps = 16;
inline constexpr int32_t kMaxSteps = 96;
inline constexpr int32_t kDefaultSteps = 48;

// The multiplier keys' shared range: 0 to 2, 1.0 = the engine's own value. A NaN reads as the default, so a broken value never turns
// the march into something the keys did not ask for.
inline constexpr float kMinBoost = 0.0f;
inline constexpr float kMaxBoost = 2.0f;
inline constexpr float kDefaultBoost = 1.0f;

// The march's intensity: 0 to 2, 0 = off (the feature bit goes off, the frame costs one relaxed load - the radial's own rule).
inline constexpr float kMinIntensity = 0.0f;
inline constexpr float kMaxIntensity = 2.0f;
inline constexpr float kDefaultIntensity = 1.0f;
inline constexpr float kIntensityEpsilon = 0.001f;

// GodraysDensity: the boost of the medium's density, in units of 0.001 per metre at the camera's own height (the engine's own density is 0.0009 per
// metre there, far too thin for a visible beam; 20 is the artistic constant the RE prototype found). GodraysAnisotropy: the Henyey-Greenstein lobe's
// g, 0 isotropic, 0.8 narrow (10 % of the peak at 24.6 degrees); 1 would be a delta, so the range stops short of it.
inline constexpr float kMinDensity = 0.0f;
inline constexpr float kMaxDensity = 200.0f;
inline constexpr float kDefaultDensity = 20.0f;
inline constexpr float kMinAnisotropy = 0.0f;
inline constexpr float kMaxAnisotropy = 0.95f;
inline constexpr float kDefaultAnisotropy = 0.8f;

constexpr float ClampDensity(float value) noexcept
{
    if (value != value)
        return kDefaultDensity;
    return value < kMinDensity ? kMinDensity : (value > kMaxDensity ? kMaxDensity : value);
}

constexpr float ClampAnisotropy(float value) noexcept
{
    if (value != value)
        return kDefaultAnisotropy;
    return value < kMinAnisotropy ? kMinAnisotropy : (value > kMaxAnisotropy ? kMaxAnisotropy : value);
}

// The shadow compare's taps: 0 = a point compare (the default), 1 = a two-tap boundary compare, 2 = a four-tap one. The banding knob:
// a cascade's atlas texel spans a world footprint the march reads per sample, and the engine's own surface path pays a 56-tap disc for
// what the march's air samples get in one.
inline constexpr int32_t kMinSoftness = 0;
inline constexpr int32_t kMaxSoftness = 2;
inline constexpr int32_t kDefaultSoftness = 0;

// The march target's resolution: a quarter of the render window (the default, the cost model's recommendation) or half of it.
enum class Resolution : uint8_t
{
    Quarter = 0,
    Half = 1,
};

constexpr Resolution ParseResolution(std::string_view text) noexcept
{
    return text == "half" || text == "Half" || text == "HALF" ? Resolution::Half : Resolution::Quarter;
}

constexpr const char* ResolutionName(Resolution resolution) noexcept
{
    return resolution == Resolution::Half ? "half" : "quarter";
}

// The colour key: `auto` is the radial's scan-and-sky rule (the light's own colour when the engine reports one, else the sky's colour
// around the sun's screen position), `white` is a neutral march tinted by nothing but the intensity key, `enginefog` is the engine's own
// fog colour rule - lerp(fogColor, sky(pixelUV), LUT.y) from the sky texture and the weight LUT the engine's own fog post binds.
enum class Colour : uint8_t
{
    Auto = 0,
    White = 1,
    EngineFog = 2,
};

constexpr Colour ParseColour(std::string_view text) noexcept
{
    if (text == "white" || text == "White" || text == "WHITE")
        return Colour::White;
    if (text == "enginefog" || text == "EngineFog" || text == "ENGINEFOG" || text == "engine fog")
        return Colour::EngineFog;
    return Colour::Auto;
}

constexpr const char* ColourName(Colour colour) noexcept
{
    switch (colour)
    {
    case Colour::White: return "white";
    case Colour::EngineFog: return "enginefog";
    case Colour::Auto: return "auto";
    }
    return "auto";
}

constexpr float ClampBoost(float value) noexcept
{
    if (value != value)
        return kDefaultBoost;
    return value < kMinBoost ? kMinBoost : (value > kMaxBoost ? kMaxBoost : value);
}

constexpr uint32_t ClampSteps(int32_t value) noexcept
{
    const int32_t steps = value < kMinSteps ? kMinSteps : (value > kMaxSteps ? kMaxSteps : value);
    return uint32_t(steps);
}

constexpr uint32_t ClampSoftness(int32_t value) noexcept
{
    const int32_t softness = value < kMinSoftness ? kMinSoftness : (value > kMaxSoftness ? kMaxSoftness : value);
    return uint32_t(softness);
}

constexpr float ClampIntensity(float value) noexcept
{
    if (value != value)
        return kDefaultIntensity;
    return value < kMinIntensity ? kMinIntensity : (value > kMaxIntensity ? kMaxIntensity : value);
}

// The parameters one session runs with, from the keys. The master switch is not part of them: it decides whether the module is armed.
struct Params
{
    uint32_t steps = uint32_t(kDefaultSteps);
    Resolution resolution = Resolution::Quarter;
    float fogBoost = kDefaultBoost;
    float heightLift = kDefaultBoost;
    float intensity = kDefaultIntensity;
    Colour colour = Colour::Auto;
    uint32_t softness = uint32_t(kDefaultSoftness);
    bool jitter = true;
    float phaseBoost = kDefaultBoost;
    float density = kDefaultDensity;
    float anisotropy = kDefaultAnisotropy;
};

constexpr Params BuildParams(int32_t steps, std::string_view resolution, float fogBoost, float heightLift, float intensity,
                             std::string_view colour, int32_t softness, bool jitter, float phaseBoost, float density = kDefaultDensity,
                             float anisotropy = kDefaultAnisotropy) noexcept
{
    Params params;
    params.steps = ClampSteps(steps);
    params.resolution = ParseResolution(resolution);
    params.fogBoost = ClampBoost(fogBoost);
    params.heightLift = ClampBoost(heightLift);
    params.intensity = ClampIntensity(intensity);
    params.colour = ParseColour(colour);
    params.softness = ClampSoftness(softness);
    params.jitter = jitter;
    params.phaseBoost = ClampBoost(phaseBoost);
    params.density = ClampDensity(density);
    params.anisotropy = ClampAnisotropy(anisotropy);
    return params;
}

// The feature-bit rule: the master switch and an intensity above zero. A master left on at intensity 0 is today's picture and runs
// nothing, so the draw path costs one relaxed load - the radial's own rule, the march's too.
constexpr bool Wanted(bool enabled, const Params& params) noexcept
{
    return enabled && params.intensity > kIntensityEpsilon;
}

// The march target's extent along one axis of the render window: a quarter (ceil of extent/4) or a half (ceil of extent/2). An extent
// below the division factor stays zero instead of becoming a one-texel pass, like the radial's HalfExtent.
constexpr uint32_t MarchExtent(float extent, Resolution resolution) noexcept
{
    const float divide = resolution == Resolution::Half ? 2.0f : 4.0f;
    if (!(extent >= divide))
        return 0;
    return uint32_t((extent / divide) + 0.9999f);
}

// ---- the added light, in the white the pass reads -----------------------------------------------------------------------------

// The composite gain against the frame's white. The term is the in-scatter integral S (0 to the phase value: a lit ray of an optically thick
// medium saturates at the phase), not the first march's average lit-air fraction, so the gain is the prototype's 0.7: at intensity 1 a fully lit
// ray in the lobe's centre adds 0.7 of the room the pixel has left to the white through the ShaftAdded bound.
inline constexpr float kMarchGain = 0.70f;

// The march term is clamped into this band before the composite: the captured fog constants' absolute scale is unmeasured (FOG-F9 owns
// it), and a runaway density read must not wash the frame. The ShaftAdded bound below is the real guarantee (no key can push a channel
// past the white); this clamp keeps the term itself in the range the gain constant was chosen for.
inline constexpr float kMaxTerm = 4.0f;

constexpr float TermClamped(float term) noexcept
{
    if (!(term == term) || !(term > 0.0f))
        return 0.0f;
    return term > kMaxTerm ? kMaxTerm : term;
}

// ---- the composite adds the beams, not the haze (Brief R3) --------------------------------------------------------------------------------
//
// The in-scatter integral with the engine's shadows in it (I_V) is mostly a smooth haze (the unshadowed in-scatter, which the engine's own fog post
// already adds along each view ray): on the user's coast frame the v2 composite lifted the open castle and sea by 3.95 / 4.72 %, 77-82 % of what the
// NO-SHADOW control lifted. The composite now adds only the term's structure above its own local level, the light shafts' proven shape on this term:
//
//   term' = min( max(term - lowpass(term), 0) * kBeamBoost + kBeamKeep * term , kMaxTerm )
//
// lowpass = the mean of a (2 * kBeamTapsPerSide + 1)^2 grid of march texels around the pixel's own nearest march texel, kBeamReachPixels window
// pixels per side (the grid's stride in texels follows the march's resolution). The grid is a sparse mean on purpose (81 loads, no new target, no new
// draw, nothing in the explicit-state Guard's slot spec). A term with no structure (constant, or the unshadowed integral of an open field) adds only
// the kept share. The constants were chosen on capture 1 of the 11/06 session (GodraysDensity 20, intensity 1; the light shafts' new output as the input): see fixesgodrays3REPORT.md.
inline constexpr float kBeamBoost = 0.75f;
inline constexpr float kBeamKeep = 0.10f;
inline constexpr int32_t kBeamTapsPerSide = 4;
inline constexpr float kBeamReachPixels = 48.0f;

// The grid's stride in march texels for a march resolution scale (window pixels per texel): the reach divided by the taps per side, at least 1.
constexpr int32_t BeamTapStride(float resolutionScale) noexcept
{
    const float scale = resolutionScale > 0.0f ? resolutionScale : 4.0f;
    const float texels = kBeamReachPixels / scale / float(kBeamTapsPerSide);
    const int32_t rounded = int32_t(texels + 0.5f);
    return rounded < 1 ? 1 : rounded;
}

// The march texel a pixel is centred on: the nearest one to the pixel's position in march texels (coordinate = (pixel centre - window origin) / scale - 0.5).
constexpr int32_t BeamCentreTexel(float coordinate) noexcept
{
    const float shifted = coordinate + 0.5f;
    const int32_t truncated = int32_t(shifted);
    return float(truncated) > shifted ? truncated - 1 : truncated; // floor
}

// The beam term from the upsampled term and its local level. A NaN or non-positive term is no light; a NaN level is "no structure" (only the kept share).
constexpr float BeamTerm(float term, float lowpass) noexcept
{
    if (!(term == term) || !(term > 0.0f))
        return 0.0f;
    const float structure = (lowpass == lowpass && term > lowpass) ? term - (lowpass > 0.0f ? lowpass : 0.0f) : 0.0f;
    const float shaped = structure * kBeamBoost + kBeamKeep * term;
    return shaped > kMaxTerm ? kMaxTerm : shaped;
}

// The composite's addition, one channel: the radial's own ShaftAdded expression (ShaftsRules.h), reused here so the two effects' bound
// is one rule. light = BeamTerm * intensity * kMarchGain * fade (the tint's own channel folds into term's per-channel use in the shader).
using shafts::ShaftAdded;

// The scene and depth formats, the render window and the white-point fallback: the radial's own rules, reused so the two passes refuse
// the same frames and draw against the same bound.
using shafts::IsSceneFormat;
using shafts::IsDepthFormat;
using shafts::WindowUsable;
using shafts::kFallbackWhitePoint;
using shafts::ShadowMatrixShape;
using shafts::kHoldFrames;
using shafts::HoldFade;

// The light's identity, projection and tint rules, the camera-cut line and the lamp line: the radial's own rules, reused so the march's
// light is judged by the same rule that places the radial's shafts (the two effects then always agree on what the frame's sun is).
using shafts::WorldElevationOf;
using shafts::WorldDirectionOf;
using shafts::ClassifyLight;
using shafts::LightVerdict;
using shafts::ProjectSun;
using shafts::ResolveSunTint;
using shafts::FormatNotTheSun;
using shafts::kCameraCut;
using shafts::Window;
using shafts::SceneHdrWatch;
using shafts::SceneHdrInactive;

// The atlas' sampled format (R16_UNORM, the S0 slice's answer) and the fog weight LUT's own (R16G16_FLOAT).
inline constexpr uint32_t kAtlasSampledFormat = 56; // DXGI_FORMAT_R16_UNORM
inline constexpr uint32_t kR16G16Format = 34;       // DXGI_FORMAT_R16G16_FLOAT

// ---- the engine's fog state, decoded -----------------------------------------------------------------------------------------

// What the engine's fog post binds, in bytes: cFogParam (128 B, eight float4s), cExpHeightFogParam (declared 48 B, bound 64), and
// cDynamicViewportResolutionParam (32 B; the post reads only its first float4, as a depth uv scale). The offsets below are the ones the
// post's own bytecode reads (PS-ROADMAP-FOG.md §3.2, asm Fog_GU 86-187); the material family's g_FogParameter (160 B) is a different
// buffer and the march does not read it.
inline constexpr uint32_t kFogParamBytes = 128;
inline constexpr uint32_t kExpHeightFogBytes = 64;
inline constexpr uint32_t kViewportParamBytes = 32;

// cFogParam's float4 rows, as the post reads them:
//   [0] fogColor (xyz), [1] the DI inscatter's colour (xyz) and a scalar (.w), [2] the fog state's own sun direction (xyz) and the
//   3-D volume map's gate (.w: 1.0 = the branch is off - the march does not read the volume map at all, [U-F4] stays open),
//   [3].w and [4].w the weight LUT's u parameters (u = A + B / worldDist), [4].xyz the camera's world position,
//   [5].x/.y the depth-to-distance pair (worldDist = A + B * depth), [5].z the volume map's depth scale (unread here).
inline constexpr uint32_t kFogColorAt = 0x00;
inline constexpr uint32_t kFogDiColorAt = 0x10;
inline constexpr uint32_t kFogSunDirAt = 0x20;
inline constexpr uint32_t kFogVolumeFlagAt = 0x2C;
inline constexpr uint32_t kFogLutAAt = 0x3C;
inline constexpr uint32_t kFogCameraAt = 0x40;
inline constexpr uint32_t kFogLutBAt = 0x4C;
inline constexpr uint32_t kFogDistAAt = 0x50;
inline constexpr uint32_t kFogDistBAt = 0x54;
// cExpHeightFogParam's float4 rows, as the post reads them:
//   [0] = (distance start, fallOff1, density1, refHeight1), [1] = (the opacity floor, fallOff2, density2, refHeight2),
//   [2] = (the mix scale, the DI intensity, the DI exponent, 0).
inline constexpr uint32_t kExpStartAt = 0x00;
inline constexpr uint32_t kExpFallOff1At = 0x04;
inline constexpr uint32_t kExpDensity1At = 0x08;
inline constexpr uint32_t kExpRefHeight1At = 0x0C;
inline constexpr uint32_t kExpMinOpacityAt = 0x10;
inline constexpr uint32_t kExpFallOff2At = 0x14;
inline constexpr uint32_t kExpDensity2At = 0x18;
inline constexpr uint32_t kExpRefHeight2At = 0x1C;
inline constexpr uint32_t kExpMixScaleAt = 0x20;
inline constexpr uint32_t kExpDiIntensityAt = 0x24;
inline constexpr uint32_t kExpDiExponentAt = 0x28;

// The fog state the march reads, decoded once per landed copy on the CPU (the shader takes the decoded floats, not the bytes). The default
// state carries negative sentinels (density1 = distB = -1) so an undecoded state is refused by FogStateUsable - a capture that landed half
// a pair decodes to something the gate refuses instead of something the march divides by.
struct FogState
{
    float fogColor[3] { 0.0f, 0.0f, 0.0f };
    float sunDir[3] { 0.0f, 0.0f, 1.0f }; // the fog state's own sun direction: kept for the log, the march phases with the shadow light
    float volumeFlag = 1.0f;              // cFogParam[2].w: the 3-D volume branch's gate; 1.0 = off (the march's assumption, [U-F4])
    float lutA = 0.0f, lutB = 0.0f;       // the weight LUT's u = lutA + lutB / worldDist
    float cameraPos[3] { 0.0f, 0.0f, 0.0f };
    float distA = 0.0f, distB = -1.0f; // worldDist = distA + distB * depth; distB = -1 is the undecoded sentinel
    float density1 = -1.0f, fallOff1 = 0.0f, refHeight1 = 0.0f; // density1 = -1 is the undecoded sentinel
    float density2 = 0.0f, fallOff2 = 0.0f, refHeight2 = 0.0f;
    float minOpacity = 0.0f, mixScale = 0.0f, diIntensity = 0.0f, diExponent = 1.0f;
};

// A float from the mapped bytes, alignment-safe: the staging buffer's bytes are copied into a float through memcpy, never reinterpreted
// at an alignment the buffer does not promise (the shafts' own shadow-candidate read does the same).
inline float FloatAt(const unsigned char* bytes, uint32_t offset) noexcept
{
    float value = 0.0f;
    if (bytes == nullptr)
        return value;
    std::memcpy(&value, bytes + offset, sizeof value);
    return value;
}

// The decode, from the two mapped copies. A null pointer answers the default state (the caller then refuses it as unusable), so a
// capture that landed half a pair decodes to something the gate refuses instead of something the march divides by.
inline FogState DecodeFog(const unsigned char* fog, const unsigned char* exp) noexcept
{
    FogState state;
    if (fog == nullptr || exp == nullptr)
        return state;
    state.fogColor[0] = FloatAt(fog, kFogColorAt + 0);
    state.fogColor[1] = FloatAt(fog, kFogColorAt + 4);
    state.fogColor[2] = FloatAt(fog, kFogColorAt + 8);
    state.sunDir[0] = FloatAt(fog, kFogSunDirAt + 0);
    state.sunDir[1] = FloatAt(fog, kFogSunDirAt + 4);
    state.sunDir[2] = FloatAt(fog, kFogSunDirAt + 8);
    state.volumeFlag = FloatAt(fog, kFogVolumeFlagAt);
    state.lutA = FloatAt(fog, kFogLutAAt);
    state.cameraPos[0] = FloatAt(fog, kFogCameraAt + 0);
    state.cameraPos[1] = FloatAt(fog, kFogCameraAt + 4);
    state.cameraPos[2] = FloatAt(fog, kFogCameraAt + 8);
    state.lutB = FloatAt(fog, kFogLutBAt);
    state.distA = FloatAt(fog, kFogDistAAt);
    state.distB = FloatAt(fog, kFogDistBAt);
    state.density1 = FloatAt(exp, kExpDensity1At);
    state.fallOff1 = FloatAt(exp, kExpFallOff1At);
    state.refHeight1 = FloatAt(exp, kExpRefHeight1At);
    state.density2 = FloatAt(exp, kExpDensity2At);
    state.fallOff2 = FloatAt(exp, kExpFallOff2At);
    state.refHeight2 = FloatAt(exp, kExpRefHeight2At);
    state.minOpacity = FloatAt(exp, kExpMinOpacityAt);
    state.mixScale = FloatAt(exp, kExpMixScaleAt);
    state.diIntensity = FloatAt(exp, kExpDiIntensityAt);
    state.diExponent = FloatAt(exp, kExpDiExponentAt);
    return state;
}

// Whether the decoded state is one the march can evaluate: every field finite, both densities non-negative, the distance mapping's
// slope positive (a negative slope maps depth backwards), and the DI exponent finite. The exponent's clamp to >= 1 happens in
// PhaseExponent, like the engine's own shaders do.
constexpr bool FogStateUsable(const FogState& fog) noexcept
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        if (!(fog.fogColor[i] == fog.fogColor[i]) || !(fog.cameraPos[i] == fog.cameraPos[i]) || !(fog.sunDir[i] == fog.sunDir[i]))
            return false;
    }
    if (!(fog.density1 >= 0.0f) || !(fog.density2 >= 0.0f))
        return false;
    if (!(fog.distB > 0.0f))
        return false;
    if (!(fog.diExponent == fog.diExponent))
        return false;
    if (!(fog.refHeight1 == fog.refHeight1) || !(fog.refHeight2 == fog.refHeight2))
        return false;
    return true;
}

// ---- which bound buffer is the fog's height profile ---------------------------------------------------------------------------

// The fog post binds two 64-byte buffers: cExpHeightFogParam (the height profile, 48 bytes declared) and cC2W (a clip-to-world matrix, which
// the post's bytecode multiplies by (ndc.x, ndc.y, depth, 1) and divides by w - not a rigid transform either, so no shape rule tells the
// two apart). The bytecode's reflection names the profile's slot, and that slot is what picks the buffer: the shader object carries it as a
// private-data stamp written at creation, the way the light bounce carries its composite's slots.
inline constexpr uint32_t kNoProfileSlot = 0xFFFFFFFFu;

// The stamp word: the profile's slot plus one, so that 0 means "nothing stamped". A slot D3D11 does not have is nothing.
constexpr uint64_t PackProfileSlot(uint32_t slot) noexcept
{
    return slot < ext::ffxiv::engine::kConstantBufferSlots ? uint64_t(slot) + 1u : 0u;
}

constexpr uint32_t UnpackProfileSlot(uint64_t packed) noexcept
{
    return packed >= 1u && packed <= uint64_t(ext::ffxiv::engine::kConstantBufferSlots) ? uint32_t(packed - 1u) : kNoProfileSlot;
}

// The slot whose buffer is copied as the profile: the stamped one, when the draw has a buffer of the profile's size bound there. No stamp, a
// slot the draw has nothing at, or a buffer of another size is no profile - the caller says which input is missing and the march waits.
// `boundBytes` is the byte width of the buffer bound at each of the kConstantBufferSlots slots, 0 for none.
constexpr uint32_t ProfileSlotFor(uint32_t stamped, const uint32_t* boundBytes) noexcept
{
    if (boundBytes == nullptr || stamped >= ext::ffxiv::engine::kConstantBufferSlots)
        return kNoProfileSlot;
    return boundBytes[stamped] == kExpHeightFogBytes ? stamped : kNoProfileSlot;
}

// Whether a copied 64-byte buffer is a height profile, judged on the 48 bytes the post declares and nothing past them (the 16 bytes beyond
// vary per capture): row 0 is (distance start >= 0, fallOff1 >= 0, density1 >= 0, refHeight1 finite), row 1 is (opacity floor in [0, 1],
// fallOff2 >= 0, density2 >= 0, refHeight2 finite), row 2 is (mix scale in (0, 1000], DI intensity finite, DI exponent >= 0). The matrix
// beside it fails this on its first row (a negative start), so a copy taken from the wrong slot is refused here and not decoded into a fog.
inline bool ProfileRowUsable(const unsigned char* bytes) noexcept
{
    if (bytes == nullptr)
        return false;
    const auto finite = [](float value) { return value == value && value > -1.0e30f && value < 1.0e30f; };
    const float start = FloatAt(bytes, kExpStartAt);
    const float fallOff1 = FloatAt(bytes, kExpFallOff1At);
    const float density1 = FloatAt(bytes, kExpDensity1At);
    const float refHeight1 = FloatAt(bytes, kExpRefHeight1At);
    const float minOpacity = FloatAt(bytes, kExpMinOpacityAt);
    const float fallOff2 = FloatAt(bytes, kExpFallOff2At);
    const float density2 = FloatAt(bytes, kExpDensity2At);
    const float refHeight2 = FloatAt(bytes, kExpRefHeight2At);
    const float mixScale = FloatAt(bytes, kExpMixScaleAt);
    const float diIntensity = FloatAt(bytes, kExpDiIntensityAt);
    const float diExponent = FloatAt(bytes, kExpDiExponentAt);
    return finite(start) && start >= 0.0f && finite(fallOff1) && fallOff1 >= 0.0f && finite(density1) && density1 >= 0.0f &&
           finite(refHeight1) && finite(minOpacity) && minOpacity >= 0.0f && minOpacity <= 1.0f && finite(fallOff2) && fallOff2 >= 0.0f &&
           finite(density2) && density2 >= 0.0f && finite(refHeight2) && finite(mixScale) && mixScale > 0.0f && mixScale <= 1000.0f &&
           finite(diIntensity) && finite(diExponent) && diExponent >= 0.0f;
}

// ---- which input a march that has never accepted a frame is missing ---------------------------------------------------------------

// The first accept needs the light and camera copy, the cascade copies and a usable fog state, all landed. A march that waits names the
// first one it lacks, in that order, with a line of its own for each - a log that says only "bytes have never landed" cannot tell the three
// rings apart.
enum class MarchInput : uint8_t
{
    None = 0,
    Light,
    Cascades,
    Fog,
};

constexpr MarchInput FirstMissingInput(bool lightLanded, bool cascadesLanded, bool fogLanded, bool fogUsable) noexcept
{
    if (!lightLanded)
        return MarchInput::Light;
    if (!cascadesLanded)
        return MarchInput::Cascades;
    if (!fogLanded || !fogUsable)
        return MarchInput::Fog;
    return MarchInput::None;
}

inline const char* MissingInputLine(MarchInput input) noexcept
{
    switch (input)
    {
    case MarchInput::Light:
        return "God rays: waiting for the sun light copy (its direction and colour, from the light draw's g_LightParam) and the camera copy "
               "beside it; no copy of that ring has landed, so the march has no light or camera yet";
    case MarchInput::Cascades:
        return "God rays: waiting for the shadow cascade copies (the four g_DirectionalShadowParameter instances the assembly draws bind); "
               "none has landed with a usable shadow matrix, and the march needs at least one";
    case MarchInput::Fog:
        return "God rays: waiting for a fog state the march can evaluate (the fog post's cFogParam with the cExpHeightFogParam profile at "
               "the slot its shader names); the copy has not landed, or it did not decode to a usable state";
    default:
        return "";
    }
}

// The march has an accepted state only when all four read-backs land in one frame. When each has landed on its own and the first accept still
// has not happened, that is the line.
inline constexpr const char* kReadbacksNotTogether =
    "God rays: the light, cascade and fog read-backs have each landed, but not in the same frame yet, so the march has no accepted state";

// The cascade copies landed and none of the four holds a shadow matrix of the shape the march reads.
inline constexpr const char* kCascadesUnusable =
    "God rays: the cascade copies landed, but none of the four holds a shadow matrix of the shape the march reads, so there is no shadow to march through";

// The fog post's shader carries no profile slot: it was created before the stamp existed, its reflection does not name the buffer, or the
// stamp could not be written. There is no shape rule to fall back on.
inline constexpr const char* kNoProfileStamp =
    "God rays: the fog post's shader carries no cExpHeightFogParam slot stamp (its reflection does not name the buffer, or the stamp could "
    "not be written), so the height profile cannot be picked and the fog state stays unusable";

// The stamped slot holds nothing of the profile's size at the draw.
inline std::string FormatProfileSlotEmpty(uint32_t slot, uint32_t bytes)
{
    return std::format("God rays: the fog shader names cExpHeightFogParam at cb{}, but the draw binds {} bytes there (a {}-byte buffer is "
                       "needed), so there is no height profile this frame",
                       slot, bytes, kExpHeightFogBytes);
}

// The buffer at the stamped slot is not a height profile.
inline std::string FormatProfileRefused(uint32_t slot)
{
    return std::format("God rays: the buffer at cb{} is not a height profile (its declared 48 bytes fail the profile check), so the fog state "
                       "is not used",
                       slot);
}

// The engine's own density profile at a world height, with the height falloffs scaled by `lift` (1.0 = the engine's own profile,
// 0.0 = the flat sum of the two densities, 2.0 = twice the height gradient). The 0.545968 literal is the engine's own (the asm carries
// it in three places); the exponent's sign convention is the post's own reading of cb2[0].y (fallOff1) against cb2[0].w (refHeight1).
// Not constexpr: the standard's exp is not, and the profile is evaluated at run time on the CPU and per sample on the GPU alike.
inline float FogDensityAt(const FogState& fog, float worldY, float lift) noexcept
{
    const float e1 = std::exp(fog.fallOff1 * (fog.refHeight1 - worldY) * 0.545968f * lift);
    const float e2 = std::exp(fog.fallOff2 * (fog.refHeight2 - worldY) * 0.545968f * lift);
    return fog.density1 * e1 + fog.density2 * e2;
}

// The phase function's exponent: the fog state's own DI exponent, clamped to >= 1 the way the engine's own shaders clamp it, times the
// key. A NaN reads as 1 (the engine's floor), so a broken exponent cannot turn the phase into a NaN weight.
constexpr float PhaseExponent(const FogState& fog, float boost) noexcept
{
    const float exponent = fog.diExponent == fog.diExponent && fog.diExponent > 1.0f ? fog.diExponent : 1.0f;
    const float scaled = exponent * (boost == boost ? boost : kDefaultBoost);
    return scaled < 1.0f ? 1.0f : scaled;
}

// The weight LUT's u coordinate for a world distance, the post's own mapping (u = A + B / worldDist). A distance at or below zero is
// the sky: the caller zeroes the LUT weight there, like the post's own `ge r1.z, 0, d` does.
constexpr float EngineFogLutU(const FogState& fog, float worldDist) noexcept
{
    if (!(worldDist > 0.0f))
        return 0.0f;
    return fog.lutA + fog.lutB / worldDist;
}

// The sky-lerp colour the engine's own fog builds: max(0, LUT.y * (sky - fogColor) + fogColor) per channel, the post's own expression
// (asm 97-99 and 154). The LUT weight arrives already looked up; the sky pixel's own UV is the caller's.
constexpr void EngineFogColour(const FogState& fog, float lutWeight, const float sky[3], float out[3]) noexcept
{
    const float weight = lutWeight == lutWeight && lutWeight > 0.0f ? lutWeight : 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
        const float value = weight * (sky[i] - fog.fogColor[i]) + fog.fogColor[i];
        out[i] = value > 0.0f ? value : 0.0f;
    }
}

// ---- the march's bounds -------------------------------------------------------------------------------------------------------

// The far bound of the march, in view metres: the largest of the captured cascades' distance rows' .z (the engine's own shadow
// distance, 190.0 in the captured pose), clamped into the band a sane build can produce. A cascade that carries none (a buffer the
// march cannot read) is skipped by the caller before this is asked.
inline constexpr float kMinMarchFar = 16.0f;
inline constexpr float kMaxMarchFar = 2000.0f;

constexpr float MarchFarOf(const float* cascadeFars, uint32_t count) noexcept
{
    float farthest = 0.0f;
    if (cascadeFars != nullptr)
        for (uint32_t i = 0; i < count; ++i)
        {
            const float value = cascadeFars[i];
            if (value == value && value > farthest)
                farthest = value;
        }
    if (!(farthest > 0.0f))
        return kMaxMarchFar; // nothing readable: the band's far end, so a build whose distance row moved still bounds its march
    return farthest < kMinMarchFar ? kMinMarchFar : (farthest > kMaxMarchFar ? kMaxMarchFar : farthest);
}

// The camera's own world height, from the inverse view's second row (the camera buffer's +0x040 float4): the row dotted with the view
// origin (0,0,0,1) is its .w. The fog profile is normalized by the density at this height, so the march's term is a stable fraction
// whatever absolute scale the engine's fog state carries.
constexpr float CameraWorldY(const float inverseViewRow1[4]) noexcept
{
    if (inverseViewRow1 == nullptr)
        return 0.0f;
    return inverseViewRow1[3];
}

// The camera buffer's two rows the march reads that the radial's own readers do not: the inverse projection at +0x0E0 (the march's rays are
// reconstructed through it, the same register Ao.hlsl's LoadInverseProjection reads) and the inverse view's second row at +0x040 (the world
// y of a view point). The camera layout itself is ShaftsRules.h's.
inline constexpr uint32_t kInverseProjectionAt = 0x0E0;
inline constexpr uint32_t kInverseViewRow1At = 0x040;

// ---- the frame's cascades ------------------------------------------------------------------------------------------------------

// One captured `g_DirectionalShadowParameter` instance, as the march reads it: the light-space matrix (view space in, atlas uv+depth
// out), the tail's fade row (@0x60: the two edge fades' bounds and scale), the distance row's far (@0x40.z, the march's far bound and
// the distance ramp's own bound), and whether the matrix had the shadow matrix's shape at all.
inline constexpr uint32_t kCascadeCount = 4;
inline constexpr uint32_t kCascadeBytes = 128;

struct Cascade
{
    float matrix[16] {};   // cb3[0..3] of the writer: rows of a view-space-to-light-space projection, fourth row (0,0,0,1)
    float fade[4] {};      // cb3[6] of the writer: (-bound, +bound, scale, 0) on the pixel's view z
    float distanceFar = 0.0f; // cb3[4].z: the shadow distance the distance ramp and the march's far bound read
    float rampScale = 0.0f;   // cb3[5].z: the distance ramp's scale
    float rampTarget = 1.0f;  // cb3[5].w: what the distance ramp lifts the visibility toward
    float mapRowX = 0.0f;     // cb3[7].x: the shared map row's first float, whose -15 times is the disc taps' scale
    bool shaped = false;      // the matrix had the shadow matrix's shape (ShaftsRules.h ShadowMatrixShape)
};

// The world extent one cascade's u axis spans: the reciprocal of its u row's length. A larger extent is a coarser cascade (the same
// 2048 texels over more world), which is what puts the finest cascade first in the march's selection order.
constexpr float CascadeFootprint(const Cascade& cascade) noexcept
{
    if (!cascade.shaped)
        return 0.0f;
    const float lengthSquared = cascade.matrix[0] * cascade.matrix[0] + cascade.matrix[1] * cascade.matrix[1] + cascade.matrix[2] * cascade.matrix[2];
    if (!(lengthSquared > 1.0e-12f) || !(lengthSquared == lengthSquared))
        return 0.0f;
    return 1.0f / std::sqrt(lengthSquared);
}

// Whether a cascade is one the march may project with: shaped, with a readable footprint and a fade row that is not a NaN.
constexpr bool CascadeUsable(const Cascade& cascade) noexcept
{
    if (!cascade.shaped)
        return false;
    if (!(CascadeFootprint(cascade) > 0.0f))
        return false;
    for (uint32_t i = 0; i < 4; ++i)
        if (!(cascade.fade[i] == cascade.fade[i]))
            return false;
    return cascade.distanceFar == cascade.distanceFar && cascade.rampScale == cascade.rampScale;
}

// The march's cascade order: the usable cascades, finest (smallest footprint) first, and the count of them. A sample is projected with
// each in turn and the first whose uv lands inside [0,1]^2 owns it; a sample in no cascade's frustum is lit (fail-open). `order` holds
// indices into the caller's array; unusable cascades are left out.
constexpr uint32_t CascadeOrder(const Cascade* cascades, uint32_t* order, uint32_t capacity) noexcept
{
    if (cascades == nullptr || order == nullptr || capacity < kCascadeCount)
        return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < kCascadeCount; ++i)
        if (CascadeUsable(cascades[i]))
            order[count++] = i;
    // Insertion sort by footprint ascending: the finest first. Four entries, and the array the caller holds is per-frame state.
    for (uint32_t i = 1; i < count; ++i)
    {
        const uint32_t value = order[i];
        float valueFootprint = CascadeFootprint(cascades[value]);
        uint32_t j = i;
        while (j > 0 && CascadeFootprint(cascades[order[j - 1]]) > valueFootprint)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    return count;
}

// ---- the march's compare, as the engine's own writer states it ----------------------------------------------------------------

// The atlas' texel size in uv units, the writer's own resinfo arithmetic: the smaller of the atlas' two dimensions over each. A march
// that scaled its taps by the raw dimensions instead would spread them differently on every atlas size.
constexpr void AtlasTexelSize(uint32_t width, uint32_t height, float out[2]) noexcept
{
    if (width == 0 || height == 0)
    {
        out[0] = out[1] = 0.0f;
        return;
    }
    const float smaller = float(width < height ? width : height);
    out[0] = smaller / float(width);
    out[1] = smaller / float(height);
}

// The disc scale the writer's own constants produce: cb3[7].x * -15, positive when the shared map row's first float is negative (it is
// -0.000264 in both captured instances). The taps' uv offsets and their depth corrections are both scaled by this.
constexpr float DiscScale(float mapRowX) noexcept
{
    if (!(mapRowX == mapRowX))
        return 0.0f;
    return mapRowX * -15.0f;
}

// One tap's adjusted depth: the sample's light-space z pulled toward the camera by the tap's radius times the disc scale - the writer's
// own `r3.w = z - radius * scale`, which is what keeps a tilted disc's taps comparing against the depths the light sees at their own
// positions. A point compare (softness 0) has no tap and no correction.
constexpr float TapDepth(float z, float tapRadius, float discScale) noexcept
{
    return z - tapRadius * discScale;
}

// The compare itself: shadowed when the tap's depth is at or beyond the gathered texel's depth, the writer's own `ge`. A NaN texel (a
// read the atlas view refuses) is a shadowed sample, not a lit one - the same "no light on a broken read" rule the shafts' depth reads
// follow, so a broken atlas takes beams away instead of filling the frame with glow.
constexpr bool ShadowedAt(float tapDepth, float gathered) noexcept
{
    if (!(gathered == gathered))
        return true;
    if (!(tapDepth == tapDepth))
        return true;
    return tapDepth >= gathered;
}

// The two edge fades and the distance ramp, on the pixel's own view z, exactly the writer's tail:
//   fade = min(sat((-viewZ - fade.x) * fade.z), sat((viewZ + fade.y) * fade.z)),
//   shadow' = 1 - (1 - shadow) * fade,
//   distFade = 1 - sat((viewZ + distanceFar) * rampScale), squared,
//   visibility = shadow' + distFade^2 * (rampTarget - shadow').
// With the captured constants the fades and the ramp are the band edges: instance ...29220 lights inside 0.1 m and beyond 190 m,
// instance ...b320 lights beyond ~25 m - the march inherits the engine's own band edges by carrying the same rows.
constexpr float CascadeVisibility(float shadow, float viewZ, const float fade[4], float distanceFar, float rampScale, float rampTarget) noexcept
{
    if (!(shadow == shadow))
        return 0.0f; // a broken compare is a shadowed sample, never a lit one
    const float fadeA = (viewZ == viewZ && fade[0] == fade[0] && fade[2] == fade[2])
                            ? (-viewZ - fade[0]) * fade[2] : 0.0f;
    const float fadeB = (viewZ == viewZ && fade[1] == fade[1] && fade[2] == fade[2])
                            ? (viewZ + fade[1]) * fade[2] : 0.0f;
    const float satA = fadeA < 0.0f ? 0.0f : (fadeA > 1.0f ? 1.0f : fadeA);
    const float satB = fadeB < 0.0f ? 0.0f : (fadeB > 1.0f ? 1.0f : fadeB);
    const float edge = satA < satB ? satA : satB;
    const float shadowEdged = 1.0f - (1.0f - shadow) * edge;
    float distFade = 0.0f;
    if (viewZ == viewZ && distanceFar == distanceFar && rampScale == rampScale)
    {
        const float raw = (viewZ + distanceFar) * rampScale;
        const float sat = raw < 0.0f ? 0.0f : (raw > 1.0f ? 1.0f : raw);
        distFade = 1.0f - sat;
    }
    const float lift = distFade * distFade;
    const float target = rampTarget == rampTarget ? rampTarget : 1.0f;
    return shadowEdged + lift * (target - shadowEdged);
}

// ---- the outcomes the chain's hold semantics name ------------------------------------------------------------------------------

// What one tone-map draw came to for the march. The chain's hold semantics (the fix line's fail-closed wave) name two of these:
//   Answered   - the march ran and its own copy of the scene is what t0 reads for that one draw;
//   HoldStands - the march declined as a chain member: the texture it was handed (the chain's last good picture, the earlier stages'
//                output of this frame) stays at t0. The march does not fall through to the engine's texel and does not bind an
//                unlit scene; the hold stands.
//   EngineOwn  - the march declined as the fallback owner: the game's own binding stands (the brief's fail-open; the hold applies to
//                the chained case).
//   Skipped    - the frame has nothing to march (no atlas, no fog state, no light yet): not a refusal, not counted against the
//                module's streak, and the same bind semantics as the outcome it would have had.
enum class Outcome : uint8_t
{
    Answered = 0,
    HoldStands = 1,
    EngineOwn = 2,
    Skipped = 3,
};

constexpr const char* OutcomeName(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::Answered: return "answered";
    case Outcome::HoldStands: return "declined, the chain's hold stands";
    case Outcome::EngineOwn: return "declined, the frame is the engine's own";
    case Outcome::Skipped: return "skipped";
    }
    return "unknown";
}

// Why a tone-map draw was left alone. Every one of these leaves the game's draw exactly as the chain left it, and each is one line of
// the log the first time it happens.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the key is off, the intensity is 0, or the module stood down for the session
    NoAtlas,        // no atlas capture this frame (the assembly draws have not been seen, or their captures are stale)
    NoFogState,     // no fog capture landed (or the decoded state is not one the march can evaluate)
    NoLight,        // no shadow light this frame (the read refused it, or its bytes have not landed)
    NoCamera,       // the camera's bytes arrived and are not this build's camera (a build mismatch: counted)
    NotToneMap,     // the draw's shape is another post pass (passed on, nothing counted)
    RenderTarget,   // not exactly one render target, or it is the scene itself
    TargetFormat,   // the tone map's target is not FP16
    SceneFormat,    // the scene is not FP16
    SizeMismatch,   // the scene, the depth and the window do not belong to one frame
    Window,         // the render window is empty or does not fit the scene
    SceneHdr,       // the engine's scene HDR path owns the tone chain (a configuration, not a build this module cannot serve)
    CharaReplay,    // a character-preview replay of a game draw
    PassFailed,     // the pass could not build its textures, shaders or draws
    Exception,      // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the intensity is 0";
    case Decline::NoAtlas: return "no cascade shadow atlas this frame";
    case Decline::NoFogState: return "no fog state this frame";
    case Decline::NoLight: return "no shadow light this frame";
    case Decline::NoCamera: return "the light draw's camera buffer did not hold the camera's matrices";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's render target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene, the depth and the render window do not belong to one frame";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::SceneHdr: return "the engine's scene HDR path replaces the tone chain this march draws before";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draws";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// Which declines count toward the module's own stand-down: the ones that say this build cannot serve the march. A frame with no atlas,
// no fog state or no light is a frame without the march in it, not a failure; the scene HDR path and the replay are configurations.
constexpr bool CountsTowardsStandDown(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::NoCamera:
    case Decline::RenderTarget:
    case Decline::TargetFormat:
    case Decline::SceneFormat:
    case Decline::SizeMismatch:
    case Decline::Window:
    case Decline::PassFailed:
    case Decline::Exception:
        return true;
    case Decline::NoAtlas:
    case Decline::NoFogState:
    case Decline::NoLight:
    case Decline::NotToneMap:
    case Decline::SceneHdr:
    case Decline::CharaReplay:
    case Decline::FeatureOff:
    case Decline::None:
        return false;
    }
    return false;
}

// Why a frame had no march in it, in the three reasons the card counts apart. One `skipped` number with its decomposition is what a
// session reads first when the beams are missing (the radial's card's own lesson).
enum class SkipReason : uint8_t
{
    NoAtlas = 0,   // the frame's assembly draws did not yield the atlas and its cascades (or their captures are stale)
    NoFogState = 1, // the fog post's constants have not landed, or the decode is not one the march can evaluate
    NoLight = 2,   // no shadow light this frame: the read refused it, or its bytes have not landed
};

struct CardCounts
{
    uint32_t skipped = 0;
    uint32_t noAtlas = 0;
    uint32_t noFog = 0;
    uint32_t noLight = 0;
};

// The one place a frame is counted as skipped: `skipped` and exactly one of the three reasons move together.
constexpr void CountSkip(CardCounts& counts, SkipReason reason) noexcept
{
    ++counts.skipped;
    switch (reason)
    {
    case SkipReason::NoAtlas: ++counts.noAtlas; return;
    case SkipReason::NoFogState: ++counts.noFog; return;
    case SkipReason::NoLight: ++counts.noLight; return;
    }
}

// ---- the log and the card -----------------------------------------------------------------------------------------------------

inline constexpr unsigned kMaxLogLines = 32;

// One count per Decline, indexed by the enum's value, so a caller can carry the table without a guess at its length.
inline constexpr uint32_t kDeclineReasonCount = uint32_t(Decline::Exception) + 1;

struct LogGate
{
    uint32_t last = 0;
    bool have = false;
    unsigned lines = 0;

    bool Take(uint32_t tag) noexcept
    {
        if (lines >= kMaxLogLines || (have && tag == last))
            return false;
        have = true;
        last = tag;
        lines++;
        return true;
    }
};

// A declined tone-map draw, with the outcome the chain's hold semantics name: the chained decline leaves the chain's last good picture
// at t0 (the hold stands), the owner decline leaves the game's own binding. The two wordings are distinct on purpose - a session that
// reads "the hold stands" knows the earlier stages' work is still on screen, and "the frame is the engine's own" knows nothing of the
// chain drew at all.
inline std::string FormatDecline(Decline reason, Outcome outcome)
{
    if (outcome == Outcome::HoldStands)
        return std::format("God rays: the march declined ({}), so the chain's hold stands", DeclineName(reason));
    if (outcome == Outcome::EngineOwn)
        return std::format("God rays: the march declined ({}), so the frame is the engine's own", DeclineName(reason));
    return std::format("God rays: the march declined ({})", DeclineName(reason));
}

inline std::string FormatSkip(SkipReason reason)
{
    switch (reason)
    {
    case SkipReason::NoAtlas: return "God rays: the frame was left as it was (no cascade shadow atlas this frame)";
    case SkipReason::NoFogState: return "God rays: the frame was left as it was (no fog state this frame)";
    case SkipReason::NoLight: return "God rays: the frame was left as it was (no shadow light this frame)";
    }
    return "God rays: the frame was left as it was";
}

// The line a session gets when the engine's scene HDR comes on while the march's key is on: that path replays the engine's tone chain in
// its own branch, so this module's draw path never runs and the march cannot draw. One line per transition into that state.
inline constexpr const char* kSceneHdrInactive =
    "God rays: the volumetric march is inactive while scene HDR is on, because that path replaces the tone chain this march draws before";

// The line a session gets when the march's own reads have stopped landing: the frames are skipped, one line says so, and the streak is
// not what moves the module to its stand-down (a stalled read-back is not a build this module cannot serve).
inline constexpr const char* kReadbackStale =
    "God rays: the frame's fog, light and cascade bytes stopped arriving, so the march is leaving the frames alone";

// The line a session gets when the march's captures have never landed: the maps are not ready and the frames are left alone (the radial's
// own read-back line's march sibling).
inline constexpr const char* kReadbackNotLanding =
    "God rays: the frame's fog, light and cascade bytes have never landed, so the maps are not ready and the frames are left alone";

// The line a session gets when the camera's bytes arrived and are not this build's camera: a build mismatch, counted toward the stand-down.
inline constexpr const char* kCameraRefused =
    "God rays: the light draw's camera buffer does not hold this build's camera, so the march cannot reconstruct its rays";

// The one line that says the module stood itself down for the session.
inline std::string FormatSwitchedOff(uint32_t limit)
{
    return std::format("God rays: {} of the module's draws in a row were left alone, so it is off for this session", limit);
}

// The card's counters. `skipped` is the number of frames left alone without the march, and the three reasons are its whole decomposition.
struct CardNumbers
{
    float intensity = kDefaultIntensity;
    uint64_t frames = 0;  // the module's own count of rendered frames: the base of the share
    uint32_t runs = 0;    // tone-map draws this march composited into
    uint32_t held = 0;    // of those, the frames drawn with the last accepted state (this frame's own read had not landed)
    uint32_t declined = 0;
    uint32_t skipped = 0;
    uint32_t noAtlas = 0;
    uint32_t noFog = 0;
    uint32_t noLight = 0;
    uint32_t refused = 0; // the camera refusals (a build mismatch), counted toward the stand-down
    uint32_t reads = 0;   // times the frame's state was read back
    uint32_t notReady = 0; // read-backs the GPU had not finished (never a refusal)
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t marchWidth = 0, marchHeight = 0; // the march target's own size, for the cost a session reads
};

inline std::string FormatCard(const CardNumbers& numbers)
{
    return std::format("on at {:.2f}: {} rendered frames, {} with the march ({} of them drawn with a held state), {} left alone, {} "
                       "with nothing to march (no atlas: {}, no fog state: {}, no shadow light: {}); the read-back: {} reads, {} not "
                       "ready, {} camera refusals; the last march ran at {}x{} of a {}x{} window",
                       double(numbers.intensity), numbers.frames, numbers.runs, numbers.held, numbers.declined, numbers.skipped,
                       numbers.noAtlas, numbers.noFog, numbers.noLight, numbers.reads, numbers.notReady, numbers.refused,
                       numbers.marchWidth, numbers.marchHeight, numbers.windowWidth, numbers.windowHeight);
}

// How often the card's numbers are written to the log, in rendered frames: about a minute at 60 Hz (the radial's own cadence).
inline constexpr uint64_t kCountsLogFrames = 3600;

// The on-cost line, in the numbers the design counts it with: the march's GPU time (one timestamp pair around its draws, read a run or
// two later and never waited for) and what the frame's tone maps came to.
inline std::string FormatCost(bool haveGpuMilliseconds, double gpuMilliseconds, uint64_t frames, uint32_t runs, uint32_t declined)
{
    const std::string gpu = haveGpuMilliseconds ? std::format("{:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string("its GPU time not measured yet");
    return std::format("God rays: {} over {} frames ({} left alone); one march a frame, {}", gpu, frames, declined, runs);
}

// The line of a frame that ran: the light's direction, the march's own size, the term's range and the keys, so a session reads what the
// march did with the frame instead of guessing it from the picture.
inline std::string FormatRun(const Params& params, uint32_t windowWidth, uint32_t windowHeight, uint32_t marchWidth, uint32_t marchHeight,
                             const float direction[3], float fade, float whitePoint, float termPeak)
{
    const float dx = direction != nullptr ? direction[0] : 0.0f;
    const float dy = direction != nullptr ? direction[1] : 0.0f;
    const float dz = direction != nullptr ? direction[2] : 0.0f;
    // A term peak below zero is "not measured yet": the stats read-back lands a frame or two after the first run (the first march printed a literal 0 here).
    const std::string peak = termPeak >= 0.0f ? std::format("{:.3f}", double(termPeak)) : std::string("not measured yet");
    return std::format("God rays: the march ran at {}x{} of the {}x{} window, direction ({:.4f}, {:.4f}, {:.4f}), fade {:.2f}, white "
                       "{:.3f}, term peak {}, {} steps, {} res, fog {:.2f}, lift {:.2f}, phase {:.2f}, density {:.2f}, anisotropy {:.2f}, "
                       "softness {}, jitter {}, intensity {:.2f}, colour {}",
                       marchWidth, marchHeight, windowWidth, windowHeight, double(dx), double(dy), double(dz), double(fade),
                       double(whitePoint), peak, params.steps, ResolutionName(params.resolution), double(params.fogBoost),
                       double(params.heightLift), double(params.phaseBoost), double(params.density), double(params.anisotropy), params.softness,
                       params.jitter ? "on" : "off", double(params.intensity), ColourName(params.colour));
}

// The line the capture writes when a view is first secured: what was stolen and how (an own view on the game's resource, or an owned
// copy), so a session reads the steal's legality from the log instead of trusting it.
inline std::string FormatSecured(const char* what, bool ownView, uint32_t width, uint32_t height, uint32_t format)
{
    return std::format("God rays: the {} secured as {} ({}x{}, format {})", what, ownView ? "an own view on the game's resource" : "an owned copy",
                       width, height, format);
}

// ---- the anchors this module claims in the engine module's shared word --------------------------------------------------------

// The fog post's recognition and its slot travel in the shared map (EnginePassRules.h): kFogPostBit is the map's last free flag bit
// (bit 63) and the fog post's cFogParam slot rides the light shafts' slot field under the same cannot-coexist guard the sky's own sun
// parameters use. The atlas readers claim no bit of their own: the assembly draws declare g_LightParam (the writer's own bias block
// reads it), so they already carry the light shafts' bit and reach the march's capture through it. This module keeps no private copy
// of the map, so a renumber is one edit there.
static_assert(uint32_t(engine::Knob::Godrays) == 3, "the god rays' refusal knob moved");

// The march's state hold: the last accepted frame state (the light, the fog constants and the cascades ride one accept), held for at
// most kHoldFrames rendered frames with the radial's own fade weights (ShaftsRules.h, reused above), dropped by a camera cut. The march's
// term is scaled by the hold's weight, so a missed read costs a little light instead of a whole frame of the effect - the radial's own
// hold, one unit up.

// ---- the beams that follow the engine's own shadows: the second march's pure rules --------------------------------------------------------

// Constants the shader, the pass and the tests share.
inline constexpr float kSigmaPerMetre = 0.001f;   // GodraysDensity 1 = this much extinction per metre at the camera's own height
inline constexpr float kAirBias = 0.002f;         // the air sample's depth bias on the atlas compare (the prototype's; the engine's writer biases through the surface normal, an air sample has none)
inline constexpr uint32_t kAtlasSlices = 4;       // the atlas holds one cascade per slice, each 1/4 of its height
inline constexpr uint32_t kNoSlice = 0xFFFFFFFFu; // a cascade whose matrix and fade row name no slice: it never shadows
inline constexpr uint32_t kSkyBins = 128;         // the sky ceiling's histogram, per channel, over [0, white]
inline constexpr float kSkyCeilingMargin = 0.98f;
inline constexpr float kSkyPercentile = 0.99f;
inline constexpr uint32_t kMinSkySamples = 64;    // fewer sky samples than this and the sky is left alone
inline constexpr uint32_t kSkySampleStride = 4;   // every 4th pixel each way: a 1/16 sample of the window
inline constexpr float kLightDimLuminance = 0.05f;
inline constexpr float kLightFullLuminance = 0.60f;
inline constexpr float kAsideCosine = 0.30f;      // 72.5 degrees: a light farther than this from every pixel ray casts no beams
inline constexpr float kUpsampleDepthSigma = 0.05f; // the bilateral upsample's relative depth difference at which a neighbour's weight is 1/e
inline constexpr float kSkyDepthEpsilon = 1.0e-6f;
inline constexpr float kFarDistance = 1.0e6f;     // the linear distance a sky pixel stands for in the upsample's weights
inline constexpr float kAxisAgreementDegrees = shafts::kShadowAgreementDegrees;
inline constexpr float kDegreesPerRadian = 57.29577951308232f;

// The lobe the phase boost narrows or widens: GodraysPhaseBoost keeps its documented meaning (1 neutral, higher concentrates the beams toward the light,
// lower spreads them) by scaling the lobe's narrowness 1 - g. A boost of zero is the isotropic lobe; a NaN reads as neutral.
inline float EffectiveAnisotropy(float anisotropy, float phaseBoost) noexcept
{
    const float g = ClampAnisotropy(anisotropy);
    const float boost = phaseBoost == phaseBoost ? phaseBoost : kDefaultBoost;
    if (!(boost > 0.0f))
        return 0.0f;
    const float effective = 1.0f - (1.0f - g) / boost;
    return effective < 0.0f ? 0.0f : (effective > kMaxAnisotropy ? kMaxAnisotropy : effective);
}

// Henyey-Greenstein normalised to 1 at the light (cos = 1): (1 - g)^3 / (1 + g^2 - 2 g cos)^1.5. g 0 is 1 everywhere; g 0.8 keeps 10 % at 24.6 degrees.
inline float HenyeyGreenstein(float cosTheta, float g) noexcept
{
    const float gg = ClampAnisotropy(g);
    const float c = cosTheta != cosTheta ? 0.0f : (cosTheta < -1.0f ? -1.0f : (cosTheta > 1.0f ? 1.0f : cosTheta));
    // 1 + g^2 - 2 g cos written as (1 - g)^2 + 2 g (1 - cos): exact at cos = 1, where the plain form cancels to a few parts in 10^5 at g 0.95.
    const float denominator = (1.0f - gg) * (1.0f - gg) + 2.0f * gg * (1.0f - c);
    const float cube = (1.0f - gg) * (1.0f - gg) * (1.0f - gg);
    return cube / std::pow(denominator > 1.0e-6f ? denominator : 1.0e-6f, 1.5f);
}

// The angle at which the lobe has fallen to `fraction` of its peak.
inline float LobeAngleDegrees(float g, float fraction) noexcept
{
    const float gg = ClampAnisotropy(g);
    if (!(gg > 0.0f) || !(fraction > 0.0f) || !(fraction < 1.0f))
        return 180.0f;
    const float cube = (1.0f - gg) * (1.0f - gg) * (1.0f - gg);
    const float c = (1.0f + gg * gg - std::pow(cube / fraction, 2.0f / 3.0f)) / (2.0f * gg);
    return std::acos(c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c)) * kDegreesPerRadian;
}

// The medium's extinction at a sample: the density key in units of kSigmaPerMetre, times the profile normalised at the camera's own height.
constexpr float MediumSigma(float density, float normalisedDensity) noexcept { return density * kSigmaPerMetre * normalisedDensity; }

// The in-scatter integral S = sum sigma * phase * V * T * dt, T = exp(-tau), tau accruing through shadowed air too (the extinction of a ray does not
// stop at an occluder). The step is the shader's own loop body; the host reference and the shader run the same expression.
struct InscatterState
{
    float sum = 0.0f;
    float tau = 0.0f;
};

inline void InscatterStep(InscatterState& state, float sigma, float phase, float visibility, float dt, bool extinction = true) noexcept
{
    const float transmittance = extinction ? std::exp(-state.tau) : 1.0f;
    state.sum += sigma * phase * visibility * transmittance * dt;
    state.tau += sigma * dt;
}

inline float InscatterIntegral(const float* sigmas, const float* visibilities, int32_t count, float dt, float phase, bool extinction) noexcept
{
    InscatterState state;
    for (int32_t i = 0; i < count; ++i)
        InscatterStep(state, sigmas[i], phase, visibilities[i], dt, extinction);
    return state.sum;
}

// ---- a cascade's slice of the atlas -----------------------------------------------------------------------------------------------------

// The projected v of the middle of a cascade's own full-weight distance band along the view axis: the band is where the fade row (cb3[6]: the near bound,
// the far bound, the scale) is 1 - from fade.x + 1/scale to fade.y - 1/scale in view distance - and the cascade's matrix maps it into its own slice, well
// inside it (>= 0.06 of the slice's 0.25 on all sixteen matrices of the four rc6 frames). NaN when the rows are unreadable.
inline float CascadeBandMidV(const float matrix[16], const float fade[4]) noexcept
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (matrix == nullptr || fade == nullptr)
        return nan;
    for (uint32_t i = 0; i < 16; ++i)
        if (!(matrix[i] == matrix[i]))
            return nan;
    if (!(fade[0] == fade[0]) || !(fade[1] == fade[1]) || !(fade[2] == fade[2]) || !(fade[2] > 0.0f))
        return nan;
    const float edge = 1.0f / fade[2];
    const float bandNear = (fade[0] + edge) > 0.0f ? (fade[0] + edge) : 0.0f;
    const float bandFar = fade[1] - edge;
    if (!(bandFar > bandNear))
        return nan;
    const float distance = 0.5f * (bandNear + bandFar);
    const float w = -matrix[14] * distance + matrix[15];
    if (!(w > 1.0e-6f))
        return nan;
    return (-matrix[6] * distance + matrix[7]) / w;
}

// The slice the cascade writes into: floor(4 v) of that point, or kNoSlice when it is not inside the atlas. Independent of how many cascades landed.
inline uint32_t CascadeSlice(const float matrix[16], const float fade[4]) noexcept
{
    const float v = CascadeBandMidV(matrix, fade);
    if (!(v >= 0.0f) || !(v < 1.0f))
        return kNoSlice;
    const uint32_t slice = uint32_t(v * float(kAtlasSlices));
    return slice < kAtlasSlices ? slice : kNoSlice;
}

// A sample belongs to a cascade only inside that cascade's slice: [k/4, (k+1)/4) of the atlas' height.
constexpr bool SliceContains(float v, uint32_t slice) noexcept
{
    if (slice >= kAtlasSlices || !(v == v))
        return false;
    return v >= float(slice) * 0.25f && v < float(slice + 1u) * 0.25f;
}

// ---- the sky ceiling ----------------------------------------------------------------------------------------------------------------------

// The bin of a value in the histogram over [0, range]; a NaN, a negative or a value past the range lands in a bin of the histogram, never outside.
constexpr uint32_t SkyBinOf(float value, float range, uint32_t bins) noexcept
{
    if (!(value == value) || !(value > 0.0f) || !(range > 0.0f) || bins == 0)
        return 0;
    const float scaled = value / range * float(bins);
    return scaled >= float(bins) ? bins - 1u : uint32_t(scaled);
}

// The `fraction` quantile of the histogram, interpolated inside its bin by the rank's position among the bin's samples (so it is not an upper edge a
// bin above the exact value). 0 for an empty histogram.
inline float HistogramPercentile(const uint32_t* bins, uint32_t binCount, float range, float fraction) noexcept
{
    if (bins == nullptr || binCount == 0)
        return 0.0f;
    float total = 0.0f;
    for (uint32_t i = 0; i < binCount; ++i)
        total += float(bins[i]);
    if (!(total > 0.0f))
        return 0.0f;
    const float rank = fraction * total;
    float before = 0.0f;
    for (uint32_t i = 0; i < binCount; ++i)
    {
        const float count = float(bins[i]);
        if (count > 0.0f && before + count >= rank)
        {
            float inside = (rank - before) / count;
            inside = inside < 0.0f ? 0.0f : (inside > 1.0f ? 1.0f : inside);
            return (float(i) + inside) * range / float(binCount);
        }
        before += count;
    }
    return range;
}

// The ceiling one sky channel may reach: 0.98 of the sampled 99th percentile, or 0 (the sky is left alone) with fewer than kMinSkySamples samples.
inline float SkyCeilingOf(const uint32_t* bins, uint32_t binCount, float range, uint32_t samples) noexcept
{
    if (samples < kMinSkySamples)
        return 0.0f;
    return kSkyCeilingMargin * HistogramPercentile(bins, binCount, range, kSkyPercentile);
}

// What a sky pixel's channel may add: the ShaftAdded answer, but never past the ceiling. A ceiling of 0 (or a NaN) allows nothing.
constexpr float SkyLimited(float add, float scene, float ceiling) noexcept
{
    if (!(ceiling > 0.0f) || !(add == add) || !(scene == scene))
        return 0.0f;
    const float room = ceiling - scene;
    const float allowed = room > 0.0f ? room : 0.0f;
    return add < allowed ? add : allowed;
}

// ---- the bilateral upsample --------------------------------------------------------------------------------------------------------------

// The linear view distance of a depth value through the inverse projection (the third and fourth rows' depth terms): a sky pixel, a NaN or a degenerate
// w stand for kFarDistance, so a sky pixel only ever weights sky pixels.
inline float LinearDistanceOf(const float inverseProjection[16], float depth) noexcept
{
    if (!(depth > kSkyDepthEpsilon))
        return kFarDistance;
    const float z = inverseProjection[10] * depth + inverseProjection[11];
    const float w = inverseProjection[14] * depth + inverseProjection[15];
    if (!(std::fabs(w) > 1.0e-20f))
        return kFarDistance;
    const float distance = std::fabs(z / w);
    return distance == distance ? distance : kFarDistance;
}

// The weight of a low-resolution neighbour for a pixel: exp(-(relative depth difference / sigma)^2). A sigma of 0 is the plain bilinear read.
inline float BilateralDepthWeight(float distancePixel, float distanceTexel, float sigma) noexcept
{
    if (!(sigma > 0.0f))
        return 1.0f;
    const float larger = distancePixel > distanceTexel ? distancePixel : distanceTexel;
    if (!(larger > 0.0f))
        return 1.0f;
    const float relative = std::fabs(distancePixel - distanceTexel) / larger / sigma;
    return std::exp(-relative * relative);
}

// ---- the light: class, direction, verdict ------------------------------------------------------------------------------------------------

// The luminance gain: 0 at 0.05, 1 at 0.6, a smoothstep between, so a moon-like light never runs at sun strength. A NaN is no light.
constexpr float LightLuminanceGain(float luminance) noexcept
{
    if (!(luminance == luminance))
        return 0.0f;
    float t = (luminance - kLightDimLuminance) / (kLightFullLuminance - kLightDimLuminance);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// The cosine between a view-space direction and the nearest pixel ray of the frame (the inverse projection's rays through the whole [-1, 1]^2 ndc
// rectangle), so a light above the frame's top edge reads its distance from the edge, not from the corner. A grid, then a local refinement.
inline float NearestRayCosine(const float inverseProjection[16], const float direction[3]) noexcept
{
    if (inverseProjection == nullptr || direction == nullptr || !(direction[0] == direction[0]) || !(direction[1] == direction[1]) ||
        !(direction[2] == direction[2]))
        return std::numeric_limits<float>::quiet_NaN();
    const float* ip = inverseProjection;
    const auto cosineAt = [&](float x, float y) {
        const float r0 = ip[0] * x + ip[1] * y + ip[3];
        const float r1 = ip[4] * x + ip[5] * y + ip[7];
        const float r2 = ip[8] * x + ip[9] * y + ip[11];
        const float length = std::sqrt(r0 * r0 + r1 * r1 + r2 * r2);
        if (!(length > 1.0e-12f))
            return -2.0f;
        return (r0 * direction[0] + r1 * direction[1] + r2 * direction[2]) / length;
    };
    constexpr int kCellsX = 128, kCellsY = 72;
    float bestX = 0.0f, bestY = 0.0f, best = -2.0f;
    for (int j = 0; j <= kCellsY; ++j)
        for (int i = 0; i <= kCellsX; ++i)
        {
            const float x = -1.0f + 2.0f * float(i) / float(kCellsX);
            const float y = -1.0f + 2.0f * float(j) / float(kCellsY);
            const float c = cosineAt(x, y);
            if (c > best)
            {
                best = c;
                bestX = x;
                bestY = y;
            }
        }
    float step = 2.0f / float(kCellsY);
    for (int round = 0; round < 24; ++round)
    {
        bool moved = false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                float x = bestX + float(dx) * step, y = bestY + float(dy) * step;
                x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
                y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
                const float c = cosineAt(x, y);
                if (c > best)
                {
                    best = c;
                    bestX = x;
                    bestY = y;
                    moved = true;
                }
            }
        if (!moved)
            step *= 0.5f;
    }
    return best;
}

// A light farther than 72.5 degrees from every pixel ray casts no beams into this frame: it is behind or beside the camera. An unreadable cosine is gated.
constexpr bool LightAside(float nearestCosine) noexcept { return !(nearestCosine >= kAsideCosine); }

// The shadow light's axis as the cascades state it: minus the normalised third row of the light-space matrix (view space, toward the light).
inline bool CascadeAxisOf(const float matrix[16], float out[3]) noexcept
{
    if (matrix == nullptr || out == nullptr)
        return false;
    const float x = matrix[8], y = matrix[9], z = matrix[10];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 1.0e-9f) || !(length == length))
        return false;
    out[0] = -x / length;
    out[1] = -y / length;
    out[2] = -z / length;
    return true;
}

// The angle between the light draw's direction and a cascade's axis, in degrees; negative when either is unreadable.
inline float AxisAgreementDegrees(const float matrix[16], const float lightDirection[3]) noexcept
{
    if (matrix == nullptr || lightDirection == nullptr)
        return -1.0f;
    return shafts::ShadowDegrees(matrix + 8, lightDirection, true);
}

inline bool AxisAgrees(const float matrix[16], const float lightDirection[3]) noexcept
{
    const float degrees = AxisAgreementDegrees(matrix, lightDirection);
    return degrees >= 0.0f && degrees <= kAxisAgreementDegrees;
}

// What became of the frame's light: it runs (at its luminance gain), it is behind or beside the camera, the light draw's direction is not the cascades'
// axis (so it is not the shadow light), or there was no cascade to judge it against. The order is the order of the checks.
enum class LightGate : uint8_t
{
    Run = 0,
    Aside,
    AxisMismatch,
    NoCascade,
};

constexpr const char* LightGateName(LightGate gate) noexcept
{
    switch (gate)
    {
    case LightGate::Run: return "run";
    case LightGate::Aside: return "aside";
    case LightGate::AxisMismatch: return "axis";
    case LightGate::NoCascade: return "nocascade";
    }
    return "unknown";
}

struct LightJudgement
{
    LightGate gate = LightGate::NoCascade;
    float luminance = 0.0f;
    float gain = 0.0f;
    float elevationDegrees = 0.0f;
    float nearestCosine = -1.0f;
    float nearestDegrees = 180.0f;
    float axisDegrees = -1.0f;
};

inline LightJudgement JudgeLight(const float lightDirection[3], const float diffuse[3], float elevationSine, const float inverseProjection[16],
                                 const float* finestCascadeMatrix) noexcept
{
    LightJudgement judgement;
    judgement.luminance = shafts::Luminance(diffuse);
    judgement.gain = LightLuminanceGain(judgement.luminance);
    const float sine = elevationSine != elevationSine ? 0.0f : (elevationSine < -1.0f ? -1.0f : (elevationSine > 1.0f ? 1.0f : elevationSine));
    judgement.elevationDegrees = std::asin(sine) * kDegreesPerRadian;
    if (finestCascadeMatrix == nullptr)
        return judgement; // NoCascade
    judgement.axisDegrees = AxisAgreementDegrees(finestCascadeMatrix, lightDirection);
    if (!(judgement.axisDegrees >= 0.0f && judgement.axisDegrees <= kAxisAgreementDegrees))
    {
        judgement.gate = LightGate::AxisMismatch;
        return judgement;
    }
    judgement.nearestCosine = NearestRayCosine(inverseProjection, lightDirection);
    const float clamped = judgement.nearestCosine != judgement.nearestCosine ? -1.0f
                          : (judgement.nearestCosine < -1.0f ? -1.0f : (judgement.nearestCosine > 1.0f ? 1.0f : judgement.nearestCosine));
    judgement.nearestDegrees = std::acos(clamped) * kDegreesPerRadian;
    judgement.gate = LightAside(judgement.nearestCosine) ? LightGate::Aside : LightGate::Run;
    return judgement;
}

// The once-per-change tag of a judgement: the gate and the gain in eighths.
constexpr uint32_t LightGateKey(const LightJudgement& judgement) noexcept
{
    const uint32_t eighths = uint32_t(judgement.gain * 8.0f + 0.5f);
    return 0x300u | (uint32_t(judgement.gate) << 4) | (eighths > 8u ? 8u : eighths);
}

inline std::string FormatLightJudgement(const LightJudgement& j)
{
    switch (j.gate)
    {
    case LightGate::Run:
        return std::format("God rays: the shadow light is luminance {:.3f}, elevation {:.1f} deg, {:.1f} deg from the nearest pixel ray, its axis agrees with the "
                           "shadow cascades to {:.2f} deg: the beams run at gain {:.2f}",
                           double(j.luminance), double(j.elevationDegrees), double(j.nearestDegrees), double(j.axisDegrees), double(j.gain));
    case LightGate::Aside:
        return std::format("God rays: the shadow light is behind or beside the camera ({:.1f} deg from the nearest pixel ray, luminance {:.3f}, elevation {:.1f} "
                           "deg): no beams, the frame is left alone",
                           double(j.nearestDegrees), double(j.luminance), double(j.elevationDegrees));
    case LightGate::AxisMismatch:
        return std::format("God rays: the light draw's direction is {:.1f} deg from the shadow cascades' axis (the limit is {:.0f}), so it is not the shadow "
                           "light: the frame is left alone",
                           double(j.axisDegrees), double(kAxisAgreementDegrees));
    case LightGate::NoCascade:
        break;
    }
    return "God rays: no shadow cascade to judge the light against: the frame is left alone";
}

// ---- the fog sun, read from the 128-byte cFogParam copy on its own -------------------------------------------------------------------------

// cFogParam[2].xyz (+0x20) is the fog post's own sun, a unit vector in WORLD space (its y is the visible sun's elevation sine: 0.183 on the daylight frame,
// -0.109 / -0.058 / -0.012 on the three evening frames). It needs neither the height profile nor a usable march state, so the night curve can read it on
// the frames where the profile is refused. False for a vector that is not a unit vector within 10 %.
inline bool FogSunOf(const unsigned char* fog, float out[3]) noexcept
{
    if (fog == nullptr || out == nullptr)
        return false;
    const float x = FloatAt(fog, kFogSunDirAt + 0), y = FloatAt(fog, kFogSunDirAt + 4), z = FloatAt(fog, kFogSunDirAt + 8);
    if (!(x == x) || !(y == y) || !(z == z))
        return false;
    const float lengthSquared = x * x + y * y + z * z;
    if (!(lengthSquared >= 0.81f && lengthSquared <= 1.21f))
        return false;
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return true;
}

// ---- the stats texture and the stage values --------------------------------------------------------------------------------------------------

// The stats compute pass writes a 2x1 RGBA32F texture: texel 0 = (the sky ceilings r, g, b; the term's peak), texel 1 = (the sky sample count; 0, 0, 0).
inline constexpr uint32_t kStatsTexels = 2;

struct StatsRead
{
    bool landed = false;
    float skyCeiling[3] {};
    uint32_t skySamples = 0;
    float termPeak = 0.0f;
    uint64_t writtenFrame = 0;
};

inline void DecodeStats(const float raw[8], StatsRead& out) noexcept
{
    const auto clean = [](float v) { return (v == v && v > 0.0f && v < 1.0e9f) ? v : 0.0f; };
    out.landed = true;
    for (uint32_t c = 0; c < 3; ++c)
        out.skyCeiling[c] = clean(raw[c]);
    out.termPeak = clean(raw[3]);
    out.skySamples = uint32_t(clean(raw[4]));
}

// What the last COMPLETED run measured, for the stage dump's values string. The string is read when the capture window opens, before the chain runs, so
// these numbers are one run old (the report says so).
struct StageMeasure
{
    bool have = false;
    float termPeak = 0.0f;
    float whitePoint = 0.0f;
    float fade = 0.0f;
    float sunScreen[2] {};
    float tint[3] {};
    float luminance = 0.0f;
    float elevationDegrees = 0.0f;
    float nearestDegrees = 0.0f;
    LightGate gate = LightGate::Run;
};

inline std::string FormatStageValues(const Params& params, const StageMeasure& m)
{
    std::string text = std::format("steps={} intensity={:.2f} fogBoost={:.2f} heightLift={:.2f} softness={} jitter={} phaseBoost={:.2f} density={:.2f} anisotropy={:.2f}",
                                   params.steps, double(params.intensity), double(params.fogBoost), double(params.heightLift), params.softness,
                                   params.jitter ? 1 : 0, double(params.phaseBoost), double(params.density), double(params.anisotropy));
    if (!m.have)
        return text + " measured=0";
    // The frame's white point, fade, sun pixel and tint are not here (2026-11-07 integration): the draw path notes them as this frame's own through the
    // stage dump's late hook (StageNoteFrame after the run), so the line carries each word once and never a previous run's value of it.
    text += std::format(" termPeak={:.3f} lightLum={:.3f} elev={:.1f} angle={:.1f} gate={}", double(m.termPeak), double(m.luminance),
                        double(m.elevationDegrees), double(m.nearestDegrees), LightGateName(m.gate));
    return text;
}

// The stats read-back's line, written when the first read lands and again when the term's peak moves by a factor of two: the number the first march's
// run line printed as a literal 0, and the sky ceilings the composite caps the sky at.
inline std::string FormatStatsLine(const StatsRead& stats)
{
    return std::format("God rays: term peak {:.3f}; sky ceilings ({:.3f}, {:.3f}, {:.3f}) from {} sky samples{}", double(stats.termPeak), double(stats.skyCeiling[0]),
                       double(stats.skyCeiling[1]), double(stats.skyCeiling[2]), stats.skySamples,
                       stats.skySamples < kMinSkySamples ? " (too few: the sky is left alone)" : "");
}

// The line a session gets when the fog post's constants are being copied only for the night curve (the march is off).
inline constexpr const char* kFogSunOnlyLine =
    "God rays: the march is off, but the fog post's sun row is being copied for shadow mapping's night curve (one 128-byte copy a frame, no march, no scene copy)";
} // namespace ext::ffxiv::godrays
