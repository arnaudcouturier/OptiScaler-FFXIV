#pragma once
// Screen-space reflections (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the module does, in one line: before the first water draw of a frame, it marches the game's own view-position buffer against the
// scene colour the frame has rendered so far, and every water draw of that frame that would sample the engine's planar reflection map samples
// this one for that one call instead. A draw that samples the map without the water anchors keeps the engine's texel.
//
// This header holds what can be decided without a device: the slots a water reader's shader declares (stamped on the shader object, exactly as
// the ambient occlusion pass does), the pass's tuning, the draw validation, the mapping from a texel of the planar map to the scene texels behind
// it (the ambient occlusion pass's rule, reused here so the two cannot drift), the refusal counter, the per-frame memory of the engine's
// texture and the shader's constant block.
//
// The fail-open contract: every reason in Decline leaves the game's draw exactly as the game made it, with the engine's texture still bound, and
// the key off is today's picture by construction (the draw path is then one relaxed load per draw).

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC reflection this module's shader stamp is read from
#include "ext/ffxiv/engine/AoRules.h"        // the mapper an occlusion texel is found with, reused here (Mapping, Region, RegionOf)
#include "ext/ffxiv/engine/EnginePassRules.h" // the shared name table and D3D11's resource-slot ceiling

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::engine::ssr
{
// ---- the names a reader's shader is read for -------------------------------------------------------------------------------
//
// The planar map itself is the shared name table's row (engine::kNames), so the draw path recognises a reader without a parse here. What this
// module needs besides it are the surface the ray leaves (the engine's view-position buffer, which both water shaders declare) and the camera
// table the ray is projected with. The two names a water reader is told apart by are this module's own: the engine's planar reflection map is
// sampled by more than the water (the composite's own reflection path touches it in other builds), and this pass only answers for the water.
inline constexpr const char* kVPositionName = "g_SamplerVPosition";
inline constexpr const char* kCameraName = "g_CameraParameter";
inline constexpr const char* kWaveName = "g_SamplerWaveMap";
inline constexpr const char* kCausticsName = "g_SamplerCaustics";

// D3D11's pixel-shader slots: 128 textures and 14 constant buffers. A shader that declares one of these past the last slot cannot be bound with
// it there and is not a reader this pass serves.
inline constexpr uint32_t kTextureSlots = engine::kShaderResourceSlots;
inline constexpr uint32_t kConstantBufferSlots = 14;
inline constexpr uint8_t kSlotMissing = 0xFF;

// What one water reader's shader declares, as the pass reads it. A slot of kSlotMissing is a name the shader does not have; `water` is false for
// a shader that samples the planar map without being a water surface (it is a reader of the frame this pass does not answer for, and its draws
// are passed on without a refusal).
struct ReaderSlots
{
    uint8_t reflection = kSlotMissing;
    uint8_t vposition = kSlotMissing;
    uint8_t camera = kSlotMissing;
    bool water = false;

    constexpr bool HasReflection() const noexcept { return reflection < kTextureSlots; }
    constexpr bool HasVPosition() const noexcept { return vposition < kTextureSlots; }
    constexpr bool HasCamera() const noexcept { return camera < kConstantBufferSlots; }
    constexpr bool Complete() const noexcept { return HasReflection() && HasVPosition() && HasCamera(); }
};

// The packed word this module stamps on a shader that samples the engine's planar map, so the frame's first water reader reads its slots with one
// load instead of a parse: slot + 1 per field, in eight bits each (which holds every slot D3D11 can bind), and bit 24 set for a water surface.
// 0 in a field is "this shader does not declare it", and the whole word of 0 is "not stamped", which is every other shader of the game.
constexpr uint32_t PackSlots(const ReaderSlots& slots) noexcept
{
    const uint32_t reflection = slots.HasReflection() ? uint32_t(slots.reflection) + 1u : 0u;
    const uint32_t vposition = slots.HasVPosition() ? uint32_t(slots.vposition) + 1u : 0u;
    const uint32_t camera = slots.HasCamera() ? uint32_t(slots.camera) + 1u : 0u;
    const uint32_t water = slots.water ? (1u << 24) : 0u;
    return reflection | (vposition << 8) | (camera << 16) | water;
}

constexpr ReaderSlots UnpackSlots(uint32_t packed) noexcept
{
    ReaderSlots slots;
    const uint32_t reflection = packed & 0xFFu;
    const uint32_t vposition = (packed >> 8) & 0xFFu;
    const uint32_t camera = (packed >> 16) & 0xFFu;
    slots.reflection = reflection == 0 ? kSlotMissing : uint8_t(reflection - 1);
    slots.vposition = vposition == 0 ? kSlotMissing : uint8_t(vposition - 1);
    slots.camera = camera == 0 ? kSlotMissing : uint8_t(camera - 1);
    slots.water = (packed & (1u << 24)) != 0;
    return slots;
}

// Whether a shader's reflection is a water reader this pass can serve: the planar map, the view-position buffer and the camera table at slots
// D3D11 can bind, and at least one of the two water anchors that tell the water material apart from every other sampler of the map.
constexpr bool IsWaterReader(const ReaderSlots& slots) noexcept
{
    return slots.water && slots.Complete();
}

// ---- the slots one shader's bytecode declares ----------------------------------------------------------------------------
//
// The reflection the shared index reads names every resource, with the effect compiler's type suffix for a texture and its sampler
// (`g_SamplerVPosition.T`), which diag::ResourceBaseName strips. The effect compiler writes the samplers BEFORE the textures and the two bind
// points need not agree, so every slot is looked up by the kind it binds (diag::BindPointOf): a scan that accepted any binding type records the
// SAMPLER's slot, which holds a different texture at the draw. The four names this module needs are the planar map (the shared table's row), the
// view position and the camera table (its own), and the two water anchors, which are not slots this module binds: they only decide whether the
// pass answers for a shader at all. Nothing is compiled into an object here, so a test parses a shader it compiled itself and the draw path sees
// plain numbers.
constexpr size_t kMinShaderBytes = 32;

constexpr ReaderSlots SlotsOfBindings(const ext::ffxiv::diag::ShaderBinding* bindings, size_t count) noexcept
{
    ReaderSlots slots;
    if (bindings == nullptr)
        return slots;
    const uint32_t vposition = ext::ffxiv::diag::BindPointOf(bindings, count, ext::ffxiv::diag::BindKind::Texture, kVPositionName);
    if (vposition < kTextureSlots)
        slots.vposition = uint8_t(vposition);
    const uint32_t camera = ext::ffxiv::diag::BindPointOf(bindings, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kCameraName);
    if (camera < kConstantBufferSlots)
        slots.camera = uint8_t(camera);
    const uint32_t reflection = ext::ffxiv::diag::BindPointOf(bindings, count, ext::ffxiv::diag::BindKind::Texture, engine::kNames[11].name);
    if (reflection < kTextureSlots)
        slots.reflection = uint8_t(reflection);
    slots.water = ext::ffxiv::diag::ReflectionDeclares(bindings, count, kWaveName) ||
                  ext::ffxiv::diag::ReflectionDeclares(bindings, count, kCausticsName);
    return slots;
}

inline ReaderSlots SlotsOfShader(const void* code, size_t size)
{
    ReaderSlots slots;
    if (code == nullptr || size < kMinShaderBytes)
        return slots;
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    return SlotsOfBindings(parsed, count);
}

// ---- the tuning ------------------------------------------------------------------------------------------------------------
//
// The steps are the march's taps, the thickness is how deep behind a crossing ray the surface it crossed is still taken for the surface (in
// metres: a ray that passes a wall and finds another one a metre behind it has not hit the wall), the blend scales the term this module writes
// where the engine's own map has no hit (its alpha, and with it the colour: both are premultiplied by it; 1.0 = the full confidence, 0.0 = no term at
// all, the engine's map untouched), and the fade is the fraction of the picture at its edges over which a hit is trusted less and less. The
// blend's default is half (see kBlendDefault).
inline constexpr uint32_t kMinSteps = 16;
inline constexpr uint32_t kMaxSteps = 128;
inline constexpr uint32_t kStepsDefault = 48;
inline constexpr float kMinThickness = 0.05f;
inline constexpr float kMaxThickness = 2.0f;
inline constexpr float kThicknessDefault = 0.3f;
inline constexpr float kMinBlend = 0.0f;
inline constexpr float kMaxBlend = 1.0f;
// The default is half. The map's alpha is how much of the water's probe reflection and sun glint the traced reflection replaces (the water shader
// scales both by it), so a blend of 1.0 lets a fully confident hit take the probe reflection away altogether: on a shallow shore seen steeply down, a
// 100 % traced reflection with no Fresnel angle weighting next to it reads as a hard-edged cut-out over the water-bed shading, which is what a
// session's screenshot of a shallow pool showed. At 0.5 the traced reflection still carries its detail and half of the probe reflection stays under
// it. A player who wants the full mirror sets 1.0.
inline constexpr float kBlendDefault = 0.5f;
inline constexpr float kMinFade = 0.0f;
inline constexpr float kMaxFade = 0.5f;
inline constexpr float kFadeDefault = 0.15f;

// One session's tuning, from the keys.
struct Params
{
    bool on = false;
    uint32_t steps = kStepsDefault;
    float thickness = kThicknessDefault;
    float blend = kBlendDefault;
    float fade = kFadeDefault;
};

// A NaN reads as the default value, and a count out of range as its nearest end, so a broken ini value never turns the pass on with a number
// nobody chose. The clamps take any float, so an ini's float lands in the steps too (the key is a number, not an enum: the pass's cost is linear
// in it and the player sets it).
constexpr uint32_t ClampSteps(float value) noexcept
{
    if (value != value)
        return kStepsDefault;
    const float clamped = value < float(kMinSteps) ? float(kMinSteps) : (value > float(kMaxSteps) ? float(kMaxSteps) : value);
    return uint32_t(clamped);
}

constexpr float ClampThickness(float value) noexcept
{
    if (value != value)
        return kThicknessDefault;
    if (value < kMinThickness)
        return kMinThickness;
    return value > kMaxThickness ? kMaxThickness : value;
}

constexpr float ClampBlend(float value) noexcept
{
    if (value != value)
        return kBlendDefault;
    if (value < kMinBlend)
        return kMinBlend;
    return value > kMaxBlend ? kMaxBlend : value;
}

constexpr float ClampFade(float value) noexcept
{
    if (value != value)
        return kFadeDefault;
    if (value < kMinFade)
        return kMinFade;
    return value > kMaxFade ? kMaxFade : value;
}

// A blend of zero keeps the engine's texture, and the master off is the engine's picture: both run nothing and touch no draw. Written so a NaN
// blend reads as identity.
constexpr bool IsIdentity(const Params& params) noexcept { return !params.on || !(params.blend > 0.0f); }

// The feature-bit rule: the draw path is worth seeing only while the master switch is on and the blend is off zero.
constexpr bool Wants(const Params& params) noexcept { return !IsIdentity(params); }

// What this module's card reads as armed: its own key, a blend off zero, and a session that has not stood itself down. The engine pass overrides'
// master key is not part of it - the seam is entered while any feature of the module wants a draw, so the reflections' own switch arms it - which
// is why a session that enables only this card serves its water draws.
constexpr bool CardArmed(bool key, const Params& params, bool sessionArmed) noexcept { return key && Wants(params) && sessionArmed; }

// ---- what a water draw is --------------------------------------------------------------------------------------------------
//
// The engine binds one B8G8R8A8_UNORM planar map under the reflection name, at the picture's size in the captured session (1920x1080). A texture
// under that name of another format, or one of a few texels, is another draw of the frame: it is passed on and not counted as a refusal, exactly
// as the ambient occlusion pass treats the engine's 4x4 placeholder.
inline constexpr uint32_t kPlanarFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
inline constexpr uint32_t kMinPlanarWidth = 16;
inline constexpr uint32_t kMinPlanarHeight = 16;

// The scene the frame has rendered so far is the water draw's own render target: a single-sample half-float target at the display allocation's
// size, with the picture in the part of it the draw's window covers. The copy this pass keeps of it has to be one format the shader can load, so
// any other target is a frame this module refuses rather than one it reads with the wrong format.
inline constexpr uint32_t kSceneFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
inline constexpr uint32_t kVPositionFormat = 10;

// The typeless siblings of the two formats this pass needs. A texture created typeless and viewed with the typed format is the same data to every
// copy and to the shader, so a build that gives the map or the scene a typeless resource is served rather than refused for a mismatch that is not
// one (the copy is between compatible formats either way).
inline constexpr uint32_t kPlanarTypelessFormat = 90; // DXGI_FORMAT_B8G8R8A8_TYPELESS
inline constexpr uint32_t kSceneTypelessFormat = 9;   // DXGI_FORMAT_R16G16B16A16_TYPELESS

constexpr bool AcceptFormat(uint32_t format, uint32_t wanted) noexcept
{
    return format == wanted || (format == kPlanarTypelessFormat && wanted == kPlanarFormat) ||
           (format == kSceneTypelessFormat && wanted == kSceneFormat);
}

constexpr bool IsPlanarTarget(uint32_t format, uint32_t width, uint32_t height) noexcept
{
    return AcceptFormat(format, kPlanarFormat) && width >= kMinPlanarWidth && height >= kMinPlanarHeight;
}

// The camera table the pass copies: the projection's four rows, in the engine's own layout (g_CameraParameter: the inverse projection's at
// +0xE0 and the projection's at +0x120; ext/ffxiv/relight/shaders/CameraRegisters.h states the registers). The same size the ambient occlusion
// pass reads, because it is the same table.
inline constexpr uint32_t kCameraBytes = 0x160;

// The march's fixed geometry, so the picture cannot drift with a number the ini does not name. The first step leaves the surface along its
// normal (a ray that starts on the surface reads its own pixel as a hit), the steps then follow a quadratic ramp over the range
// (`kSurfaceBias + range * u * u`, `u = (k + 1) / steps`): the near field, where a reflection is sharpest, gets most of them, and the last step
// at kMaxDistance is 4.1 m at the default 48 steps instead of the 31 m a 1.45x geometric ramp produced. The march stops after kMaxDistance metres
// even when the ray has not left the picture. The refinement halves the interval the crossing sits in six times, which turns the longest step into
// 6.4 cm - inside the 0.30 m thickness, where four halvings left 1.94 m and the reflection flickered as the camera turned.
inline constexpr float kSurfaceBias = 0.02f;   // metres
inline constexpr float kMaxDistance = 100.0f;  // metres: past a screen's worth of scenery a reflection is not this pass's business
inline constexpr uint32_t kRefineSteps = 6;
inline constexpr float kConfidenceThreshold = 0.05f; // a hit trusted this little is the engine's texel, not a smear

// The self-graze rejection. The view-position buffer the water readers bind holds the ground UNDER the water (the capture measured the origin a
// metre to eight metres below the water plane), so the ray starts inside its own surface, and about half of the march's hits sat within 0.2 m of
// the origin, most of them the ray grazing that surface a texel or two away. Such a hit anchors the reflection to the origin's own neighbourhood,
// which the upscaler reads as noise that crawls with the camera. A hit closer than kSelfGrazeMetres whose texel lies within kSelfGrazeTexels of
// the origin texel is that graze: the engine's own texel answers instead. The distance rides the constant block's tuning.w (ConstantsOf), the
// texel radius is the shader's own constant, pinned to this one by ssr_frame's code table. Zero disables the rejection.
inline constexpr float kSelfGrazeMetres = 2.0f;
inline constexpr uint32_t kSelfGrazeTexels = 2;

// ---- what the pass writes into the map: replace, never add --------------------------------------------------------------------------------
//
// The water shader ADDS the map's decoded rgb under its Fresnel term and uses the map's alpha only to dim its probe reflection and the sun glint
// (analysis of the extracted water.shpk, ALPHA-ENCODING.md), so a colour written with an alpha of 0 is light on top of the full probe reflection:
// a double reflection. The engine's own map is the other convention: its rgb is PREMULTIPLIED by its alpha (rgb is exactly 0 wherever alpha is 0,
// and rgb <= alpha in 99 % of its hit texels: the benchmark capture with 216 804 engine hits), so a hit is "alpha = how much of the probe this
// reflection replaces, rgb = alpha x the reflected colour". The pass follows that convention:
//   - a planar texel whose engine alpha is above zero is the engine's own hit and stays the engine's, all four channels (ours was 1.9x as grainy
//     as the engine's there, and nothing says the traced colour is better than the engine's);
//   - where the engine has no hit and the march found one, the texel is (factor x colour, factor), factor = blend x confidence, the colour being
//     the scene's own colour (the scene target already holds the water shader's output encoding s x sqrt(C), the one the shader decodes with
//     (rgb / cb1[3].x)^2, so no scale of ours is needed);
//   - where the march found nothing, the engine's texel stays.
// The term is first written raw (R16G16B16A16_FLOAT: premultiplied rgb and alpha, plus the hit's distance along its ray in a second target) and a
// second draw filters it before it reaches the map: a mean over the neighbours that have a term of their own, weighted by their distance on the
// map and by how close their hit is to this texel's hit along the ray (a reflection of a near pole is not blurred into the sky behind it).
inline constexpr float kEngineHitAlpha = 0.0f; // an engine alpha above this is the engine's own hit (the 8-bit value 1/255 is a hit)
constexpr bool EngineHit(float alpha) noexcept { return alpha > kEngineHitAlpha; }

inline constexpr uint32_t kRawFormat = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT: the unfiltered term (premultiplied rgb, alpha)
inline constexpr uint32_t kGuideFormat = 54; // DXGI_FORMAT_R16_FLOAT: the hit's distance along its ray in metres, 0 where there is no term

// The factor a hit is written with: the blend scales the confidence, and it is both the alpha and the premultiplier of the colour.
constexpr float TermFactor(float blend, float confidence) noexcept
{
    const float factor = blend * confidence;
    return factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
}

// The filter: (2R + 1)^2 taps of the raw term. Radius 2 and the 15 % guide tolerance were chosen on the benchmark captures whose engine map was
// empty (so the stage-05 picture is the term): the 8x8-tile deviation of the decoded luminance of the term fell to 0.70x (smooth sea) and 0.47x
// (the busier capture) of itself with the spatial weights alone, against an engine map whose tiles deviate by 0.0055.
inline constexpr int kFilterRadius = 2;
inline constexpr float kGuideTolerance = 0.15f; // a hit this much (relative) farther or nearer along its ray is another object: weight 0
inline constexpr float kGuideFloor = 0.25f;     // metres: below this distance the tolerance is not made any tighter

constexpr float FilterSpatialWeight(int dx, int dy) noexcept { return 1.0f / (1.0f + float(dx * dx + dy * dy)); }

constexpr float FilterGuideWeight(float centre, float tap) noexcept
{
    const float larger = centre > tap ? centre : tap;
    const float difference = centre > tap ? centre - tap : tap - centre;
    const float scale = (larger > kGuideFloor ? larger : kGuideFloor) * kGuideTolerance;
    const float weight = 1.0f - difference / scale;
    return weight > 0.0f ? weight : 0.0f;
}

// What one water draw measured before the pass decided. `window` is the draw's render window in scene texels, which is the picture: the planar
// map and the scene copy are both sampled through it.
struct ReaderFacts
{
    bool hasPlanar = false;
    bool hasScene = false;
    bool hasVPosition = false;
    bool hasCamera = false;
    bool replay = false; // the draw is another module's replay of a game draw
    uint32_t planarFormat = 0;
    uint32_t planarWidth = 0, planarHeight = 0, planarSamples = 0;
    uint32_t sceneFormat = 0;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneSamples = 0;
    uint32_t vpositionFormat = 0;
    uint32_t vpositionWidth = 0, vpositionHeight = 0, vpositionSamples = 0;
    uint32_t cameraBytes = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, width, height of the draw's render window, in scene texels
};

// Why a water draw was left alone. None of these touches the draw: the engine's texture stays bound and the draw runs unchanged.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the master switch is off, the blend is at zero, or the module switched itself off
    NoPlanar,       // nothing is bound at the reflection slot, or it is not the engine's planar map
    PlanarShape,    // the texture under the reflection name is not a B8G8R8A8 map of a picture's size
    NoScene,        // the draw writes no single-sample scene target, or one this module cannot copy
    NoVPosition,    // nothing usable is bound at the view-position slot
    SizeMismatch,   // the scene, the view position and the render window do not agree on a size
    NoCamera,       // the draw declares no camera table, binds none, or binds one too small for the matrices
    Reentry,        // the draw came from this module's own work
    Replay,         // a character-preview replay of a game draw
    Failure,        // an owned texture, a shader or a draw could not run
    Exception,      // a call threw
};

inline constexpr uint32_t kDeclineReasonCount = 12;
static_assert(uint32_t(Decline::Exception) + 1 == kDeclineReasonCount, "kDeclineReasonCount must cover every ssr::Decline value");

// Why a water draw was passed on before the frame's one attempt: the twelve returns the seam makes without a refusal. None of them touches the
// draw, none is a refusal (a refusal is a shape this pass meant to serve and could not, and it counts towards the switch-off), and each is
// counted and written once a session, so a session in which the pass never ran can be read instead of guessed at. The order is the order the
// seam asks them in.
enum class PassedOn : uint8_t
{
    NotReflection = 0, // the seam called for a draw whose shader carries no reflection anchor
    NotArmed,          // the card is off, its blend is at zero, or the module stood itself down for the session
    OwnWork,           // the draw is this module's own GPU work
    Replay,            // another module's replay of a game draw (the character preview, a diagnostic replay)
    NoStamp,           // the bound shader was never stamped: this module did not read its bytecode
    NotWater,          // a sampler of the map that declares no water anchor
    NoMap,             // nothing is bound under the reflection name
    OtherMap,          // the texture under that name is not the engine's planar map
    AttemptSpent,      // the frame's one attempt is spent and no texture of this module answers
    OtherBinding,      // the frame's texture was recorded for another binding
    NoOutput,          // the frame's pass left no texture to bind
    NoSlot,            // nothing was bound at the reflection slot to swap
};

inline constexpr uint32_t kPassedOnCount = 12;
static_assert(uint32_t(PassedOn::NoSlot) + 1 == kPassedOnCount, "kPassedOnCount must cover every ssr::PassedOn value");

constexpr const char* PassedOnName(PassedOn reason) noexcept
{
    switch (reason)
    {
    case PassedOn::NotReflection: return "the draw is not a reflection reader";
    case PassedOn::NotArmed: return "the card is off, at blend zero, or the module stood down";
    case PassedOn::OwnWork: return "the draw is this module's own work";
    case PassedOn::Replay: return "the draw is another module's replay of a game draw";
    case PassedOn::NoStamp: return "the bound shader carries no reflection stamp: its bytecode was never read";
    case PassedOn::NotWater: return "the shader samples the map without a water anchor";
    case PassedOn::NoMap: return "nothing is bound at the reflection slot";
    case PassedOn::OtherMap: return "the texture at that slot is not the engine's planar reflection map";
    case PassedOn::AttemptSpent: return "the frame's one attempt is spent and no texture answers";
    case PassedOn::OtherBinding: return "the frame's texture was recorded for another binding";
    case PassedOn::NoOutput: return "the frame's pass left no texture to bind";
    case PassedOn::NoSlot: return "nothing was bound at the slot to swap";
    }
    return "unknown";
}

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the blend is at zero";
    case Decline::NoPlanar: return "nothing is bound at the reflection slot";
    case Decline::PlanarShape: return "the texture at that slot is not the engine's planar reflection map";
    case Decline::NoScene: return "the draw writes no scene target this module can copy";
    case Decline::NoVPosition: return "the reader declares or binds no usable view-position buffer";
    case Decline::SizeMismatch: return "the scene, the view position and the render window do not agree on a size";
    case Decline::NoCamera: return "the reader declares or binds no usable camera table";
    case Decline::Reentry: return "the call came from this module's own work";
    case Decline::Replay: return "the draw is a character-preview replay";
    case Decline::Failure: return "the owned textures or the pass could not run";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// The texels of the planar map this module computes: the ones whose whole scene block lies inside the draw's render window, which is the part the
// engine's own producer fills and the part the readers' allocation-relative screen coordinate lands in. The rule is the ambient occlusion pass's
// (ao::RegionOf), fed the three numbers it reads - the planar texture's size, the scene allocation's size and the render window - so the two
// passes cannot disagree about where a texel of theirs goes.
constexpr ao::ReaderFacts MappingFacts(const ReaderFacts& facts) noexcept
{
    ao::ReaderFacts mapping;
    mapping.hasOcclusion = facts.hasPlanar;
    mapping.occlusionFormat = facts.planarFormat;
    mapping.occlusionWidth = facts.planarWidth;
    mapping.occlusionHeight = facts.planarHeight;
    mapping.occlusionSamples = facts.planarSamples;
    mapping.hasDepth = facts.hasScene;
    mapping.depthWidth = facts.sceneWidth;
    mapping.depthHeight = facts.sceneHeight;
    mapping.depthSamples = facts.sceneSamples;
    mapping.hasCamera = facts.hasCamera;
    mapping.cameraBytes = facts.cameraBytes;
    for (size_t i = 0; i < 4; ++i)
        mapping.window[i] = facts.window[i];
    return mapping;
}

constexpr ao::Mapping MappingOf(const ReaderFacts& facts) noexcept { return ao::MappingOf(MappingFacts(facts)); }

constexpr ao::Region RegionOf(const ReaderFacts& facts) noexcept
{
    // ao::RegionOf reads the window and the two sizes only, but it also refuses a mapping that is not positive: the scene and the planar map are
    // both required to have been measured by the validation that runs before it.
    return ao::RegionOf(MappingFacts(facts));
}

// The texels of the scene the pass copies: the render window's own, floored to the texel grid and clamped to the scene allocation. Classify allows
// the window half a texel of slack past the allocation (the engine's +0.5 viewport origin), so its rounded size can ask for one texel that does not
// exist - and a copy box past the destination's size is an out-of-bounds copy, not a fail-open. The rule is the one place the two are reconciled, so
// the pass cannot clamp the copy differently from the way the rules accepted the window.
struct SceneCopy
{
    uint32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

constexpr SceneCopy SceneCopyOf(const ReaderFacts& facts) noexcept
{
    SceneCopy copy;
    const double x = double(facts.window[0]);
    const double y = double(facts.window[1]);
    const double width = double(facts.window[2]);
    const double height = double(facts.window[3]);
    if (!(x >= 0.0) || !(y >= 0.0) || !(width > 0.0) || !(height > 0.0))
        return copy; // no texels: the caller refuses the run
    copy.x = uint32_t(x);
    copy.y = uint32_t(y);
    const uint32_t wantWidth = uint32_t(width) + (double(uint32_t(width)) < width ? 1u : 0u);   // the window's rounded width
    const uint32_t wantHeight = uint32_t(height) + (double(uint32_t(height)) < height ? 1u : 0u); // the window's rounded height
    const uint32_t roomX = copy.x < facts.sceneWidth ? facts.sceneWidth - copy.x : 0u;
    const uint32_t roomY = copy.y < facts.sceneHeight ? facts.sceneHeight - copy.y : 0u;
    copy.width = wantWidth < roomX ? wantWidth : roomX;
    copy.height = wantHeight < roomY ? wantHeight : roomY;
    return copy;
}

// The validation of one water draw: the engine's planar map at a picture's size, one single-sample half-float scene target and one
// single-sample view-position buffer at the same allocation, a camera table big enough for the projection, and a render window that fits inside
// the scene and covers at least one whole texel of the planar map.
inline constexpr uint32_t kMinWindowWidth = 16;
inline constexpr uint32_t kMinWindowHeight = 16;

constexpr Decline Classify(const ReaderFacts& facts) noexcept
{
    if (facts.replay)
        return Decline::Replay;
    if (!facts.hasPlanar)
        return Decline::NoPlanar;
    if (!IsPlanarTarget(facts.planarFormat, facts.planarWidth, facts.planarHeight))
        return Decline::PlanarShape;
    // The copy of the engine's map into this module's target has no return code: a multisampled source would silently leave the previous frame's
    // texels there, so the shape test refuses it like any other non-reader.
    if (facts.planarSamples > 1)
        return Decline::PlanarShape;
    if (!facts.hasScene || facts.sceneWidth == 0 || facts.sceneHeight == 0 || facts.sceneSamples > 1 || !AcceptFormat(facts.sceneFormat, kSceneFormat))
        return Decline::NoScene;
    if (!facts.hasVPosition || facts.vpositionWidth == 0 || facts.vpositionHeight == 0 || facts.vpositionSamples > 1 ||
        !AcceptFormat(facts.vpositionFormat, kVPositionFormat))
        return Decline::NoVPosition;
    if (!facts.hasCamera || facts.cameraBytes < kCameraBytes)
        return Decline::NoCamera;
    // The march reads the position of a scene texel and the colour of the same texel, so the two allocations have to be the same one; a
    // view-position buffer at another size is a build this module has not measured.
    if (facts.vpositionWidth != facts.sceneWidth || facts.vpositionHeight != facts.sceneHeight)
        return Decline::SizeMismatch;
    if (!(facts.window[2] >= float(kMinWindowWidth)) || !(facts.window[3] >= float(kMinWindowHeight)))
        return Decline::SizeMismatch;
    if (!(facts.window[0] >= 0.0f) || !(facts.window[1] >= 0.0f))
        return Decline::SizeMismatch;
    // Half a pixel of slack: a viewport origin of 0.5 is the engine's habit and lands here.
    if (facts.window[0] + facts.window[2] > float(facts.sceneWidth) + 0.5f)
        return Decline::SizeMismatch;
    if (facts.window[1] + facts.window[3] > float(facts.sceneHeight) + 0.5f)
        return Decline::SizeMismatch;
    // A window that does not cover one whole texel of the planar map leaves nothing this module can align to the engine's own answer.
    if (RegionOf(facts).Empty())
        return Decline::SizeMismatch;
    return Decline::None;
}

// ---- the session -----------------------------------------------------------------------------------------------------------

// After this many water draws in a row are left alone, the module switches itself off for the session with one log line: a build whose first
// water draw no longer binds what the pass needs then costs one line and one frame's work instead of a try on every frame. A frame whose reader
// was refused is not retried inside the frame: the pass has to run before the first water draw of the frame, so a later reader of the same frame
// cannot be served consistently.
inline constexpr uint32_t kDeclineLimit = 64;

struct Session
{
    uint32_t declines = 0;
    bool off = false;

    // One refused draw. Returns true when this refusal is the one that switched the module off.
    bool NoteDecline()
    {
        if (off)
            return false;
        if (++declines >= kDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    void NoteRun() { declines = 0; }

    bool Armed() const noexcept { return !off; }
};

// ---- the per-frame memory of the engine's texture -----------------------------------------------------------------------------
//
// Nothing is remembered across a frame: the frame's first water draw records which texture the engine bound, and every later draw of that frame has
// to bind the same object to be served. The token is the caller's (ext/present's output-frame counter, one relaxed load). The captured frames bound
// one planar map throughout (the occlusion texture is the one that alternates), so this is not a per-frame ping-pong: the per-frame entry is what
// keeps a texture from an ended frame, and a view or a target the game rebuilt, from answering for a draw of the next one.
inline constexpr uint32_t kMaxServedDraws = 8192; // a token that never advances costs one pass per few thousand readers, not a stale texture

struct FrameCache
{
    uint64_t frame = 0;
    bool hasFrame = false;
    bool valid = false;  // `engine` was recorded this frame and `ours` may answer for it
    bool tried = false;  // this frame's one attempt is spent (a refusal, or the pass ran)
    uint32_t served = 0; // water draws that bound this module's texture this frame
    const void* engine = nullptr;     // the resource the engine's planar view is on
    const void* engineView = nullptr; // and its view, for the cheap same-binding test
    const void* ours = nullptr;       // this module's texture for that size

    void Reset() noexcept { *this = FrameCache {}; }

    // The token the caller read. A different frame starts a frame: the entry and the one attempt are cleared.
    void Begin(uint64_t frameToken) noexcept
    {
        if (hasFrame && frameToken == frame)
            return;
        hasFrame = true;
        frame = frameToken;
        ClearFrame();
    }

    void ClearFrame() noexcept
    {
        valid = false;
        tried = false;
        served = 0;
        engine = nullptr;
        engineView = nullptr;
        ours = nullptr;
    }

    bool Has(const void* resource) const noexcept { return valid && resource != nullptr && resource == engine; }
    const void* Ours() const noexcept { return ours; }
    bool MayRun() const noexcept { return !tried; }

    // The frame's one attempt is taken before the pass is tried, so a refusal costs one line per frame instead of one per reader.
    void TakeAttempt() noexcept { tried = true; }

    void Record(const void* engineResource, const void* view, const void* ourTexture) noexcept
    {
        engine = engineResource;
        engineView = view;
        ours = ourTexture;
        valid = true;
        tried = true;
        served = 0;
    }

    // One water draw answered from the frame's texture. A long run without a frame change expires the entry, so a token that does not advance
    // costs one pass every few thousand draws instead of a texture that stays at the first frame's picture.
    void Serve() noexcept
    {
        if (++served < kMaxServedDraws)
            return;
        ClearFrame();
    }
};

// ---- the gate of one water draw -------------------------------------------------------------------------------------------------
//
// What one water draw gets from the frame's memory, in the order the product asks it. A replay of a game draw by another module (the character
// preview, a diagnostic replay) is not a draw of the frame: it keeps the engine's own texels, is not counted as a refusal and never spends the
// frame's one attempt, so a preview opened beside the reflections shows the engine's picture while the frame's own water draws are still served.
// Once the frame's texture answers, a draw that is not a replay binds it; while it does not, only a draw the frame still has its attempt for may
// run the pass.
enum class Serve : uint8_t
{
    PassOn = 0, // a replay, or a frame with nothing to answer with
    Serve,      // the frame's own texture answers at this draw's slot
    Attempt,    // this frame's one attempt: this draw runs the pass
};

constexpr Serve ServeAnswer(const FrameCache& frame, bool replay) noexcept
{
    // A replay of a game draw by another module keeps the game's own texels and leaves the frame's one attempt to the frame's own water draw.
    if (replay)
        return Serve::PassOn;
    if (frame.valid)
        return Serve::Serve;
    if (!frame.MayRun())
        return Serve::PassOn; // this frame's one attempt is spent
    return Serve::Attempt;
}

// ---- the cost meter and the log -----------------------------------------------------------------------------------------------
inline constexpr unsigned kMaxLogLines = 24;

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

// One line a session for each passed-on reason, spent from the module's own log budget: a bit per reason, so two reasons that alternate do not
// write a line each time they alternate. The passed-on lines are not refusals, so they never touch the session's refusal streak.
struct PassedOnGate
{
    uint32_t logged = 0;

    bool Take(PassedOn reason, LogGate& budget) noexcept
    {
        if (uint32_t(reason) >= kPassedOnCount)
            return false;
        const uint32_t bit = 1u << uint32_t(reason);
        if ((logged & bit) != 0 || !budget.Take(0x100u + uint32_t(reason)))
            return false;
        logged |= bit;
        return true;
    }

    void Reset() noexcept { logged = 0; }
};

inline std::string FormatDecline(Decline reason)
{
    return std::format("Screen-space reflections: the draw was left alone ({})", DeclineName(reason));
}

// What the slot a stamped reader named held when the pass was asked about a draw: the packed word the shader carries (so a stamp that disagrees
// with the capture's own rows is visible), the slot it names, and the view the game had bound there. Plain numbers - a D3D11_SRV_DIMENSION and
// a DXGI_FORMAT value, and the resource's address as a capture prints it - so the text side names them without D3D. This is the one fact a
// session could not read before: whether the slot the module read is the slot the capture shows the planar map at.
struct ReadBinding
{
    bool have = false;
    uint32_t stamp = 0;
    uint32_t slot = kSlotMissing;
    uint32_t view = 0;   // D3D11_SRV_DIMENSION (d3d11.h), kept as a number so this header stays D3D free
    uint32_t array = 0;  // the texture's array size
    uint32_t format = 0; // the view's DXGI_FORMAT
    uint32_t width = 0, height = 0;
    uint64_t texture = 0; // the resource behind the view
};

// The name of a shader-resource view's dimension (d3d11.h's D3D11_SRV_DIMENSION values), or its number for a value this build does not name.
inline std::string SrvDimensionText(uint32_t view)
{
    switch (view)
    {
    case 1: return "BUFFER";
    case 2: return "TEXTURE1D";
    case 3: return "TEXTURE1DARRAY";
    case 4: return "TEXTURE2D";
    case 5: return "TEXTURE2DARRAY";
    case 6: return "TEXTURE2DMS";
    case 7: return "TEXTURE2DMSARRAY";
    case 8: return "TEXTURE3D";
    case 9: return "TEXTURECUBE";
    case 10: return "TEXTURECUBEARRAY";
    case 11: return "BUFFEREX";
    default: return std::format("view {}", view);
    }
}

// The name of a DXGI format the pass can meet, or its number for anything else (the same shape the draw diagnostic's columns use).
inline std::string SrvFormatText(uint32_t format)
{
    switch (format)
    {
    case kPlanarFormat: return "B8G8R8A8_UNORM";
    case kPlanarTypelessFormat: return "B8G8R8A8_TYPELESS";
    case kSceneFormat: return "R16G16B16A16_FLOAT";
    case kSceneTypelessFormat: return "R16G16B16A16_TYPELESS";
    case 2: return "R32G32B32A32_FLOAT";
    case 26: return "R11G11B10_FLOAT";
    case 28: return "R8G8B8A8_UNORM";
    case 34: return "R16G16_FLOAT";
    case 39: return "R32_TYPELESS";
    case 40: return "D32_FLOAT";
    case 41: return "R32_FLOAT";
    case 45: return "D24_UNORM_S8_UINT";
    case 46: return "R24_UNORM_X8_TYPELESS";
    case 49: return "R8G8_UNORM";
    case 54: return "R16_FLOAT";
    case 61: return "R8_UNORM";
    default: return std::format("format {}", format);
    }
}

// The read binding in the one wording the log and a capture both use: the stamp's word, the map slot the stamp names, the slot the draw path
// read, and what was bound there. "-" when a draw was passed on before any slot was read.
inline std::string FormatReadBinding(const ReadBinding& read)
{
    if (!read.have)
        return "-";
    return std::format("stamp {:#010x}, map slot {}, read slot {}, view {}, array {}, format {}, {}x{}, texture {:x}", read.stamp,
                       unsigned(UnpackSlots(read.stamp).reflection), read.slot, SrvDimensionText(read.view), read.array,
                       SrvFormatText(read.format), read.width, read.height, read.texture);
}

// The line one passed-on reason writes a session. It says the draw was not refused, because the two are read differently: a refusal is a shape
// this pass meant to serve and could not, and it counts towards the switch-off, while this is a draw the pass does not answer for. The shader's
// bytecode CRC is named where the draw's own shader is at hand, and so is the binding the slot held where the draw path read one (a pass-on for
// `OtherMap`), so a session reads which shader was not read and which texture its stamp pointed at.
inline std::string FormatPassedOn(PassedOn reason, uint32_t crc, const ReadBinding* read = nullptr)
{
    const std::string binding =
        read != nullptr && read->have ? std::format(", and the read binding was {}", FormatReadBinding(*read)) : std::string();
    if (crc != 0)
        return std::format("Screen-space reflections: the draw was passed on, which is not a refusal ({}), shader {:#010x}{}",
                           PassedOnName(reason), crc, binding);
    return std::format("Screen-space reflections: the draw was passed on, which is not a refusal ({}){}", PassedOnName(reason), binding);
}

// The card's second state line: every reason the seam passed a draw on for, so the card answers the same question the log and a capture do. A
// reason that never happened is left out, and the line is empty when none did.
inline std::string FormatPassedOnCounts(const uint32_t* counts)
{
    if (counts == nullptr)
        return {};
    std::string out;
    for (uint32_t i = 0; i < kPassedOnCount; ++i)
    {
        if (counts[i] == 0)
            continue;
        if (!out.empty())
            out += "; ";
        out += std::format("{} {}", PassedOnName(PassedOn(i)), counts[i]);
    }
    return out;
}

// The card's own transition line, written when its arming changes: whether the reflections were armed at the water is the one fact a session
// could not read anywhere, and the blend is what a card at "on" can still be running nothing with.
inline std::string FormatPublish(bool armed, float blend)
{
    return armed ? std::format("Screen-space reflections: the card is on, blend {:.2f}, so a water draw can run", double(blend))
                 : std::format("Screen-space reflections: the card is off or at blend {:.2f}, so a water draw keeps the engine's reflection",
                               double(blend));
}

inline std::string FormatSlot(uint8_t slot)
{
    return slot < kTextureSlots ? std::format("{}", unsigned(slot)) : std::string("none");
}

// The line the frame's first water draw leaves when it cannot serve the frame. It carries that draw's own shape - what it binds under each name
// and where - because a session cannot otherwise tell a scene this module cannot serve (a water draw writing no scene target) from a shader whose
// slots were never read (a stamp that never landed): both refuse with the same reason.
inline std::string FormatRefusedReader(Decline reason, const ReaderFacts& facts, const ReaderSlots& slots)
{
    return std::format("Screen-space reflections: the first water draw of the frame could not serve it ({}); planar {}x{} in format {}, scene "
                       "{}x{} in format {}, view position {}x{} at slot {}, camera {} bytes at slot {}, window {:.0f}x{:.0f} at {:.0f},{:.0f}",
                       DeclineName(reason), facts.planarWidth, facts.planarHeight, facts.planarFormat, facts.sceneWidth, facts.sceneHeight,
                       facts.sceneFormat, facts.vpositionWidth, facts.vpositionHeight, FormatSlot(slots.vposition), facts.cameraBytes,
                       FormatSlot(slots.camera), double(facts.window[2]), double(facts.window[3]), double(facts.window[0]),
                       double(facts.window[1]));
}

// The line of a run: what the pass computed, at which sizes, and what the GPU work cost once it has been measured.
inline std::string FormatRun(const Params& params, uint32_t width, uint32_t height, bool haveGpuMilliseconds, double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the pass {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Screen-space reflections: {}x{} at {} steps, thickness {:.2f} m, blend {:.2f}, fade {:.2f}{}", width, height,
                       params.steps, double(params.thickness), double(params.blend), double(params.fade), gpu);
}

// Whether the run line is due. The timestamp pair is begun by the run that draws and read by a later one, so a line written on the first run
// always reads "not measured yet" and the figure never reaches a session's log. The line waits for the first measurement instead - and a pass
// whose device would not create the queries never measures, so there it is written on the first run with the "not measured yet" clause.
constexpr bool RunLineDue(bool logged, bool measured, bool canMeasure) noexcept { return !logged && (measured || !canMeasure); }

// The card's state line adds the measured cost to the counts once a pair has landed, so a session reads the number without opening the log.
inline std::string FormatGpuCost(bool haveGpuMilliseconds, double gpuMilliseconds)
{
    return haveGpuMilliseconds ? std::format(", the pass {:.3f} ms on the GPU", gpuMilliseconds) : std::string();
}

// The line that says how many water draws of one frame bound this module's own texture. A session compares it with the number of reflection-map
// bindings the frame makes: a draw whose shader's reflection was stripped declares nothing, keeps the engine's texture and is not counted here,
// so the two numbers differ by exactly the readers this module cannot name.
inline std::string FormatServed(uint32_t served)
{
    return std::format("Screen-space reflections: {} water draws of the last frame bound this module's texture", served);
}

inline std::string FormatSwitchedOff()
{
    return std::format("Screen-space reflections: {} water draws in a row were left alone, so the module is off for this session", kDeclineLimit);
}

// ---- the shader's constant block ----------------------------------------------------------------------------------------------
//
// The pass hands its scalars in one buffer, and tests/ext/ffxiv/engine checks the layout against the shader's. A block of five float4s: the render
// window, the mapping, both allocations' sizes and the tuning, and the region of the planar map this module writes.
struct Constants
{
    float window[4] {};  // xy = the render window's origin in scene texels, zw = its size in scene texels
    float mapping[4] {}; // xy = scene texels per planar texel, zw = the planar target's size in texels
    float sizes[4] {};   // xy = the scene allocation's size in texels, z = the steps, w = the thickness in metres
    float tuning[4] {};  // x = blend, y = fade, z = the confidence threshold, w = the self-graze distance in metres (0 = off)
    float region[4] {};  // xy = the planar texel this module writes from, zw = how many texels of it
};

constexpr Constants ConstantsOf(const ReaderFacts& facts, const Params& params) noexcept
{
    Constants constants;
    const ao::Mapping mapping = MappingOf(facts);
    constants.window[0] = facts.window[0];
    constants.window[1] = facts.window[1];
    constants.window[2] = facts.window[2];
    constants.window[3] = facts.window[3];
    constants.mapping[0] = mapping.scaleX;
    constants.mapping[1] = mapping.scaleY;
    constants.mapping[2] = float(facts.planarWidth);
    constants.mapping[3] = float(facts.planarHeight);
    constants.sizes[0] = float(facts.sceneWidth);
    constants.sizes[1] = float(facts.sceneHeight);
    constants.sizes[2] = float(params.steps);
    constants.sizes[3] = params.thickness;
    constants.tuning[0] = params.blend;
    constants.tuning[1] = params.fade;
    constants.tuning[2] = kConfidenceThreshold;
    constants.tuning[3] = kSelfGrazeMetres;
    const ao::Region region = RegionOf(facts);
    constants.region[0] = float(region.x);
    constants.region[1] = float(region.y);
    constants.region[2] = float(region.width);
    constants.region[3] = float(region.height);
    return constants;
}

static_assert(sizeof(Constants) == 80, "the pass's constant block is five float4s, the same as the shader's");
} // namespace ext::ffxiv::engine::ssr
