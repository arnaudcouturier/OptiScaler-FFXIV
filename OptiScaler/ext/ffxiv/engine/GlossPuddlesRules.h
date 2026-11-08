#pragma once
// Puddles (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the module does, in one line: before the first draw of a frame that samples one of the engine's own G-buffer targets, it copies that
// target into a texture of the engine's own size and format and replaces the gloss channel of every texel whose surface faces up - where the
// engine's own wetness amount is above zero - with a value shaped by a noise field anchored to the world; every draw of that frame that would
// have sampled the engine's target samples this one for that one call instead. At the tone-map draw it then darkens the pool's lit colour and
// adds a marched reflection of the scene to it, with water's Fresnel - the part that makes a puddle a puddle rather than a shine.
//
// The engine decides the wet look itself: its material shaders read g_WetnessParameter and write the G-buffer, and its light passes, its
// composite and its forward draws read that G-buffer. This module therefore changes WHERE the ground is a puddle, not how a wet surface is
// lit: the engine's own wetness amount ramps the shape up and down with the weather (and the wetness knob's forced amount is the same number
// when the knob is armed), and the engine's own lighting does the rest. The channel it shapes is the material pack's alpha, the gloss weight the
// engine's composite reads; its red is a selector and is refused. Nothing of the game is written; a copy of the target is shaped and bound for
// one draw at a time.
//
// This header holds what can be decided without a device: the keys and their clamps, the slots a shader's reflection declares and the packed word
// it is stamped with, the gate of one reader draw, the noise and the formula (shaders/GlossPuddles.hlsl mirrors them, change the two together),
// the region of the target that lies inside the render window, the draw validation, the per-frame memory of the engine's target, the failure
// counter and the log lines.

#include "ext/ffxiv/diag/ShaderBindings.h" // the reflected bound-resource table of a shader's bytecode (pure, no D3D)
#include "ext/ffxiv/engine/EnginePassRules.h"  // the shared name table, the anchors word, D3D11's slot ceilings

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ext::ffxiv::engine::gloss
{
// ---- the names a reader's shader is read for ---------------------------------------------------------------------------------
//
// The four G-buffer targets are rows of the shared name table (engine::kNames), so the draw path recognises a reader without a parse here.
// What this module needs besides them is the depth the world position comes from and the camera table its matrices come from; they are read
// from the same reflection, per shader, because each family of readers binds them at its own slot (the sun light draws declare the depth at 0
// and the normal at 7, the composite declares the normal at 9 and no depth at all).
inline constexpr const char* kDepthName = "g_SamplerDepth";
inline constexpr const char* kCameraName = "g_CameraParameter";

// The view-space position of every pixel, which the tone-map stage marches instead of a depth: the composite declares it beside the G-buffers it
// reads, so the draw that hands this module the frame's camera and normal also hands it the positions the reflection is computed from.
inline constexpr const char* kViewPositionName = "g_SamplerViewPosition";

// The engine's G-buffer targets, in the order the target key numbers them: target 1 is the material pack, target 4 the primary world-space normal
// (the game clears it to zero in the sky and writes the ground's unit normal on everything it drew).
inline constexpr const char* kTargetNames[4] = { "g_SamplerGBuffer1", "g_SamplerGBuffer2", "g_SamplerGBuffer3", "g_SamplerGBuffer" };
inline constexpr uint32_t kTargetCount = 4;

// The texture ceiling is the shared one (engine::kShaderResourceSlots, 128) and not a smaller number of its own: a slot is a slot whoever
// reads it. The constant-buffer ceiling is D3D11's 14.
inline constexpr uint32_t kTextureSlots = engine::kShaderResourceSlots;
inline constexpr uint32_t kConstantBufferSlots = 14;
inline constexpr uint8_t kSlotMissing = 0xFF;

// What one reader's shader declares, as the pass reads it. A slot of kSlotMissing is a name the shader does not have.
struct Inputs
{
    uint8_t depth = kSlotMissing;
    uint8_t camera = kSlotMissing;
    uint8_t vposition = kSlotMissing; // g_SamplerViewPosition, on the one draw of the frame that declares it (the deferred composite)
    uint8_t targets[kTargetCount] { kSlotMissing, kSlotMissing, kSlotMissing, kSlotMissing };

    constexpr bool HasDepth() const noexcept { return depth < kTextureSlots; }
    constexpr bool HasCamera() const noexcept { return camera < kConstantBufferSlots; }
    constexpr bool HasViewPosition() const noexcept { return vposition < kTextureSlots; }
    constexpr bool HasTarget(uint32_t index) const noexcept { return index < kTargetCount && targets[index] < kTextureSlots; }
    // Whether this shader can run the PASS for the target the key names: the target itself, the normal (the G-buffer target the up-facing test
    // reads), the depth the world position comes from and the camera table. Only the frame's one attempt asks for all four; a draw that declares
    // the target alone is still served once the frame's texture exists (ServeAnswer), because the composite that turns the target into the picture
    // declares nothing else.
    constexpr bool Complete(uint32_t index) const noexcept { return HasTarget(index) && HasTarget(3) && HasDepth() && HasCamera(); }
};

// The slots one shader's reflection declares, from the binding table a bytecode parse produced. Only a texture binding is a slot this module
// reads: the effect compiler declares a texture and its sampler under one variable name and writes the pair into the reflection as "<name>.T"
// (the texture) and "<name>.S" (the sampler state), and it writes the samplers before the textures. A scan that accepted any binding type would
// therefore record the SAMPLER's slot for the depth and for every target - a slot that holds a blue-noise or a shadow atlas in the draw - and
// shape the wrong engine texture. The camera table is a constant buffer and is the one binding that is read by that type. The first texture
// that carries a name wins, so a shader that declares the same name twice is read at the slot the reflection lists first.
inline Inputs InputsFromBindings(const diag::ShaderBinding* bindings, size_t count) noexcept
{
    Inputs inputs;
    if (bindings == nullptr)
        return inputs;
    // Every slot is looked up by the kind it binds (diag::BindPointOf), so a sampler that carries a texture's name is never the slot this pass
    // reads; the ceiling checks are the caller's rule that a name past D3D11's last slot is one the shader cannot bind.
    const uint32_t depth = diag::BindPointOf(bindings, count, diag::BindKind::Texture, kDepthName);
    if (depth < kTextureSlots)
        inputs.depth = uint8_t(depth);
    const uint32_t camera = diag::BindPointOf(bindings, count, diag::BindKind::ConstantBuffer, kCameraName);
    if (camera < kConstantBufferSlots)
        inputs.camera = uint8_t(camera);
    const uint32_t viewPosition = diag::BindPointOf(bindings, count, diag::BindKind::Texture, kViewPositionName);
    if (viewPosition < kTextureSlots)
        inputs.vposition = uint8_t(viewPosition);
    for (uint32_t t = 0; t < kTargetCount; ++t)
    {
        const uint32_t target = diag::BindPointOf(bindings, count, diag::BindKind::Texture, kTargetNames[t]);
        if (target < kTextureSlots)
            inputs.targets[t] = uint8_t(target);
    }
    return inputs;
}

// The packed word this module stamps on a shader that reads a G-buffer target, so the frame's first reader reads its slots with one call
// instead of a parse: slot + 1 per field, in eight bits each, which holds every slot D3D11 can bind (0..127 for a texture, 0..13 for the
// camera table). 0 in a field is "this shader does not declare it", and the whole word of 0 is "not stamped", which is every other shader of
// the game. The word lives on the shader object itself, so no table of shaders can lose it however many the game creates.
constexpr uint64_t PackInputs(const Inputs& inputs) noexcept
{
    uint64_t packed = 0;
    packed |= inputs.HasDepth() ? uint64_t(inputs.depth) + 1u : 0u;
    packed |= inputs.HasCamera() ? (uint64_t(inputs.camera) + 1u) << 8 : 0u;
    for (uint32_t i = 0; i < kTargetCount; ++i)
        packed |= inputs.HasTarget(i) ? (uint64_t(inputs.targets[i]) + 1u) << (16 + 8 * i) : 0u;
    packed |= inputs.HasViewPosition() ? (uint64_t(inputs.vposition) + 1u) << 48 : 0u;
    return packed;
}

constexpr Inputs UnpackInputs(uint64_t packed) noexcept
{
    Inputs inputs;
    const uint32_t depth = uint32_t(packed & 0xFFull);
    const uint32_t camera = uint32_t((packed >> 8) & 0xFFull);
    inputs.depth = depth == 0 ? kSlotMissing : uint8_t(depth - 1);
    inputs.camera = camera == 0 ? kSlotMissing : uint8_t(camera - 1);
    for (uint32_t i = 0; i < kTargetCount; ++i)
    {
        const uint32_t slot = uint32_t((packed >> (16 + 8 * i)) & 0xFFull);
        inputs.targets[i] = slot == 0 ? kSlotMissing : uint8_t(slot - 1);
    }
    const uint32_t vposition = uint32_t((packed >> 48) & 0xFFull);
    inputs.vposition = vposition == 0 ? kSlotMissing : uint8_t(vposition - 1);
    return inputs;
}

// ---- the keys ----------------------------------------------------------------------------------------------------------------

// The channel of the target's texel this module replaces. `r`, `g`, `b` and `a` are the four the engine's 8-bit targets store, in the order
// the shader's own .x/.y/.z/.w names them (a B8G8R8A8 target's red is the shader's .x, its alpha the .w). The default is the material pack's
// alpha, the only continuous gloss weight the engine's composite reads; see ChannelServable for the one pair the pass refuses.
enum class Channel : uint8_t
{
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

// The keys of the card. `on` is the card's own master switch and the only one that decides whether the module runs at all: while it is on the
// module sets the shared draw seam's feature bit itself, so the engine pass module's own master key is not needed and does not shape anything on
// its own.
//
// Two stages run from these keys: the G-buffer gloss write once a frame before the light passes (`coverage`, `scale`, `softness`, `target`,
// `channel`, `wet`), and the tone-map stage at the tone-map draw (`coverage`, `scale`, `softness`, `reflection`, `reflectionStrength`,
// `darkening`, `ripple`, `reflectionSteps`). Both read the same field with the same wetness, so the pool the gloss pass shapes and the pool the
// tone stage darkens are one shape; the tone stage replaces the pool's colour, which is why the gloss keys keep the wet stone around it.
struct Params
{
    bool on = false;
    float coverage = 0.42f;  // 0..1: the share of the up-facing ground that becomes puddle at full rain
    float scale = 6.0f;      // 0.05..16: metres one noise cell spans, so 6.0 is a pool a few metres across
    float softness = 0.03f;  // 0..0.5: the half-width of a puddle's edge, in noise units
    uint32_t target = 1;    // 1..4: which of the engine's G-buffer targets carries the gloss channel
    Channel channel = Channel::A;
    float wet = 1.0f;       // 0..1: the channel value of a mirror-wet surface (1.0: the engine's own gloss sits at 0.83 on wet ground)
    // The tone-map stage: a marched mirror of the scene inside the pool, and the darkening that makes the pool read as water rather than varnish.
    bool reflection = true;          // run the mirror at all; off is the darkening alone, which is the A/B a session needs
    float reflectionStrength = 0.6f; // 0..1 of the Fresnel reflection the march's hit is added with
    float darkening = 0.45f;         // 0..1: how much of the lit colour a full puddle takes away (1.0 is black, 0.0 today's picture)
    float ripple = 0.35f;            // 0..1: how much the static ripple field tilts the mirror's normal
    uint32_t reflectionSteps = 24;   // 8..48: the march's taps, the one key that changes what the stage costs
    float reach = 24.0f;             // metres: the pool's reach at the tone map (kReachDefault); beyond it the stage keeps the engine's texel (0 = no limit)
};

constexpr float kCoverageDefault = 0.42f;
constexpr float kScaleDefault = 6.0f;
constexpr float kSoftnessDefault = 0.03f;
constexpr uint32_t kTargetDefault = 1;
constexpr float kWetDefault = 1.0f;
constexpr bool kReflectionDefault = true;
constexpr float kReflectionStrengthDefault = 0.6f;
constexpr float kDarkeningDefault = 0.45f;
constexpr float kRippleDefault = 0.35f;
constexpr uint32_t kStepsDefault = 24;
// The pool's reach. The tone-map stage shades the texels of a G-buffer surface it believes it stands on - but a
// forward draw (the ocean, the lakes, the rivers) never writes the G-buffer, so at its pixels the stage sees the
// UP-FACING SEA FLOOR beneath and paints its pools onto the water the engine drew over it. The capture that measured
// the artefact put the water blotches between 25 and 40 m of view distance (camera at eye height), while the beach
// pools a player actually reads sit inside 24 m: the reach is the one pure fact that separates them today. 0 disables
// the gate; the band's top is what a shoreline-facing camera can still name a pool at.
constexpr float kReachDefault = 24.0f;
constexpr float kMinReach = 0.0f;
constexpr float kMaxReach = 64.0f;


constexpr float kMinCoverage = 0.0f;
constexpr float kMaxCoverage = 1.0f;
constexpr float kMinScale = 0.05f;
// The cap is what a session hit: a field of one-metre cells was speckle, five metres was still speckle (both seen in a session), and the
// reference's pools are metres across, so the ceiling has to be above the largest pool a player may want. The default is the value this field was
// measured at: 6 m cells with a coverage of 0.42 put about a third of the up-facing ground in pools whose median is 4.3 m across.
constexpr float kMaxScale = 16.0f;
constexpr float kMinSoftness = 0.0f;
constexpr float kMaxSoftness = 0.5f;
constexpr float kMinWet = 0.0f;
constexpr float kMaxWet = 1.0f;
constexpr float kMinReflectionStrength = 0.0f;
constexpr float kMaxReflectionStrength = 1.0f;
constexpr float kMinDarkening = 0.0f;
constexpr float kMaxDarkening = 1.0f;
constexpr float kMinRipple = 0.0f;
constexpr float kMaxRipple = 1.0f;
constexpr uint32_t kMinSteps = 8;
constexpr uint32_t kMaxSteps = 48;


// A key out of range is clamped, and a NaN reads as the default, so a broken ini value can neither turn the pass on with no visible effect nor
// drive it past the ranges the shader was written for. The scale has a floor because a cell of zero metres is not a noise field.
constexpr float ClampKey(float value, float low, float high, float fallback) noexcept
{
    if (value != value)
        return fallback;
    if (value < low)
        return low;
    return value > high ? high : value;
}

// The coverage key is the one that decides whether the pass runs at all, so a broken value reads as zero rather than as the default: a NaN in an
// ini can then never turn the feature on, only leave today's picture.
constexpr float ClampCoverage(float value) noexcept
{
    return value != value ? kMinCoverage : ClampKey(value, kMinCoverage, kMaxCoverage, kCoverageDefault);
}

// The target key: 1..4 in the ini, 0..3 in the shader's constants. Anything outside reads as the default, which is the material pack the
// engine's own wet look was measured on.
constexpr uint32_t ClampTarget(int32_t value) noexcept
{
    if (value < 1 || value > int32_t(kTargetCount))
        return kTargetDefault;
    return uint32_t(value);
}

// The march's step count: out of range reads as the default, and a broken value can neither cost a frame nor stop the mirror.
constexpr uint32_t ClampSteps(int32_t value) noexcept
{
    if (value < int32_t(kMinSteps) || value > int32_t(kMaxSteps))
        return kStepsDefault;
    return uint32_t(value);
}

// The reach's clamp: a NaN reads as the default, below zero reads as off, and anything above the band's top is the top.
constexpr float ClampReach(float value) noexcept
{
    if (value != value)
        return kReachDefault;
    if (value < kMinReach)
        return kMinReach;
    return value > kMaxReach ? kMaxReach : value;
}

// The reach gate itself, the CPU reference of shaders/PuddleReflection.hlsl's: a texel whose view distance runs past
// the reach keeps the engine's own texel. A reach of 0 (the key's off) never refuses.
constexpr bool WithinReach(float viewDepth, const Params& params) noexcept
{
    return !(params.reach > 0.0f) || (viewDepth > 0.0f && viewDepth < params.reach);
}

// The channel key's text. Anything but a single letter this module knows reads as the default (`a`), so a typo reads as the pass's own default
// rather than as an unpredictable channel of the engine's G-buffer - or as the one pair the pass has to refuse.
constexpr Channel ParseChannel(std::string_view text) noexcept
{
    if (text.size() != 1)
        return Channel::A;
    switch (text[0])
    {
    case 'r': return Channel::R;
    case 'g': return Channel::G;
    case 'b': return Channel::B;
    default: return Channel::A;
    }
}

constexpr const char* ChannelName(Channel channel) noexcept
{
    switch (channel)
    {
    case Channel::R: return "r";
    case Channel::G: return "g";
    case Channel::B: return "b";
    default: return "a";
    }
}

constexpr Params BuildParams(bool on, float coverage = kCoverageDefault, float scale = kScaleDefault, float softness = kSoftnessDefault,
                             int32_t target = int32_t(kTargetDefault), Channel channel = Channel::A, float wet = kWetDefault,
                             bool reflection = kReflectionDefault, float reflectionStrength = kReflectionStrengthDefault,
                             float darkening = kDarkeningDefault, float ripple = kRippleDefault, int32_t steps = int32_t(kStepsDefault),
                             float reach = kReachDefault) noexcept
{
    Params params;
    params.on = on;
    params.coverage = ClampCoverage(coverage);
    params.scale = ClampKey(scale, kMinScale, kMaxScale, kScaleDefault);
    params.softness = ClampKey(softness, kMinSoftness, kMaxSoftness, kSoftnessDefault);
    params.target = ClampTarget(target);
    params.channel = channel;
    params.wet = ClampKey(wet, kMinWet, kMaxWet, kWetDefault);
    params.reflection = reflection;
    params.reflectionStrength = ClampKey(reflectionStrength, kMinReflectionStrength, kMaxReflectionStrength, kReflectionStrengthDefault);
    params.darkening = ClampKey(darkening, kMinDarkening, kMaxDarkening, kDarkeningDefault);
    params.ripple = ClampKey(ripple, kMinRipple, kMaxRipple, kRippleDefault);
    params.reflectionSteps = ClampSteps(steps);
    params.reach = ClampReach(reach);
    return params;
}

// The index of the target in kTargetNames (0..3), from the key's 1..4.
constexpr uint32_t TargetIndex(const Params& params) noexcept
{
    return params.target >= 1 && params.target <= kTargetCount ? params.target - 1u : kTargetDefault - 1u;
}

// ---- the gate of one reader draw -------------------------------------------------------------------------------------------------
//
// A draw is a reader when its shader declares the G-buffer target the key names, and nothing else is asked of it: the depth, the normal and the
// camera table are what the PASS needs once a frame, not what a draw needs to be handed the finished texture. The composite and the forward
// draws of a frame declare the material pack and nothing else (they sample the light targets it was already lit into), and they are the draws
// that turn the target into the picture, so gating the swap on the pass's own inputs served the light draws and left the composite reading the
// engine's texture.
enum class Serve : uint8_t
{
    PassOn = 0, // a replay, not a reader of that target, or the frame has nothing to answer with yet
    Serve,      // the frame's own texture answers at this draw's target slot
    Attempt,    // the frame's first reader that can run the pass: this draw runs it
};

// What one reader draw gets from the frame's memory. The order is the product's: a replay of a game draw by another module (the character
// preview, a diagnostic replay) is asked first and passed on before anything is read or counted, so it never spends the frame's one attempt; a
// draw that does not declare the target is passed on before any device call; once the frame's texture exists, that is all a draw needs; while it
// does not, only a draw that declares the pass's four inputs may spend the frame's one attempt (and only if it has not been spent).
constexpr Serve ServeAnswer(const Inputs& inputs, uint32_t targetIndex, bool frameValid, bool mayRun, bool replay) noexcept
{
    // A replay of a game draw by another module keeps the game's own texels and leaves the frame's one attempt to the frame's own reader.
    if (replay)
        return Serve::PassOn;
    if (!inputs.HasTarget(targetIndex))
        return Serve::PassOn;
    if (frameValid)
        return Serve::Serve;
    if (!mayRun || !inputs.Complete(targetIndex))
        return Serve::PassOn;
    return Serve::Attempt;
}

// Whether a draw's shader reads one of the engine's G-buffer targets: the shared word's reader bit, which the index sets for a shader that
// declares any of the four names.
constexpr bool IsGlossAnchors(engine::Anchors anchors) noexcept { return (anchors & engine::kGlossReaderBit) != 0; }

// Whether the target key names a target this pass can shape. The four names are the engine's own G-buffer rows, but only two of them are targets
// this module can copy and write: 1 and 2 are 8-bit targets of the engine's own format, 3 is the 16-bit target (another format, which the copy
// and the shape cannot answer) and 4 is the world-space normal the up-facing test itself reads (shaping a channel of it would hand every reader
// of the frame - the light draws, the composite, every forward draw - a corrupted normal, and the pass's own up test would not notice, because it
// reads the engine's texture). A key set to either is today's picture, and the log says once which value was refused.
constexpr bool TargetServable(uint32_t target) noexcept { return target == 1 || target == 2; }

// Whether the channel key names a channel this pass may replace on that target. The engine's material pack (target 1) carries a gloss in its
// alpha: the composite reads `gloss = sat((a - 0.2) * 1.25)` and lerps the material's own value towards it. Its red is not a gloss but a
// selector: the composite and the frame's wetness pass both compare that byte against 1.0 and use the answer to choose which surface encoding
// applies (the normal itself as a specular colour, on the ground the up-facing normal squared), so replacing it flips a material class instead of
// setting a gloss - a full write turns every puddled texel a saturated green. That one pair is refused; every other channel of the two servable
// targets is the engine's own data with the meaning the key's default carries.
constexpr bool ChannelServable(uint32_t target, Channel channel) noexcept { return !(target == 1 && channel == Channel::R); }

// The identity rule: the card's switch is off, there is no coverage to draw, the target key names a target this pass cannot shape, or the channel
// key names one it must not write on that target. A master left on with the coverage at zero is today's picture, so it costs the same as off, and
// a broken coverage value reads as zero (ClampCoverage), which is the same picture.
constexpr bool IsIdentity(const Params& params) noexcept
{
    return !params.on || !(params.coverage > kMinCoverage) || !TargetServable(params.target) ||
           !ChannelServable(params.target, params.channel);
}

// The feature-bit rule: the draw path is worth seeing only while the switch is on and there is a shape to draw.
constexpr bool Wants(const Params& params) noexcept { return !IsIdentity(params); }

// One number per knob setting, so the pass's per-frame memory can tell that a slider moved (a texture shaped for the old value does not answer
// the new one). A mix of the bit patterns; only equality matters, so no polynomial is needed. Never 0: the cache treats 0 as "no epoch read".
constexpr uint32_t EpochOf(const Params& params) noexcept
{
    const uint32_t on = params.on ? 0x9E3779B9u : 0u;
    const uint32_t coverage = std::bit_cast<uint32_t>(params.coverage) * 0x85EBCA6Bu;
    const uint32_t scale = std::bit_cast<uint32_t>(params.scale) * 0xC2B2AE35u;
    const uint32_t softness = std::bit_cast<uint32_t>(params.softness) * 0x27D4EB2Fu;
    const uint32_t target = params.target * 0x165667B1u;
    const uint32_t channel = uint32_t(params.channel) * 0x9E3779B1u;
    const uint32_t wet = std::bit_cast<uint32_t>(params.wet) * 0x7FEB352Du;
    const uint32_t reflection = (params.reflection ? 0x9E3779B9u : 0u) * 0x85EBCA77u;
    const uint32_t strength = std::bit_cast<uint32_t>(params.reflectionStrength) * 0xC2B2AE3Du;
    const uint32_t darkening = std::bit_cast<uint32_t>(params.darkening) * 0x27D4EB2Fu;
    const uint32_t ripple = std::bit_cast<uint32_t>(params.ripple) * 0x165667B1u;
    const uint32_t steps = params.reflectionSteps * 0x9E3779B1u;
    return (on ^ coverage ^ scale ^ softness ^ target ^ channel ^ wet ^ reflection ^ strength ^ darkening ^ ripple ^ steps) | 1u;
}

// ---- the noise and the formula -----------------------------------------------------------------------------------------------
//
// Two octaves of a value noise on a lattice that wraps, so the field has no seam and the same world point always gets the same value: the
// puddles stay where they are while the camera moves, and two frames of a still scene agree (the upscaler is not a denoiser, and a term that
// differs between two frames arrives as noise). The lattice hash is integer arithmetic only, in the order shaders/GlossPuddles.hlsl writes it,
// and the WARP test compares the shader's output with a CPU reference of THIS code, which is what makes the formula testable at all.
inline constexpr uint32_t kNoisePeriod = 64;      // lattice cells before the field repeats (a 64 m tile at scale 1.0)
inline constexpr float kFirstOctave = 0.65f;      // the second octave takes the rest, so the sum stays inside 0..1
inline constexpr float kSecondOctave = 1.0f - kFirstOctave;
inline constexpr float kSoftnessFloor = 0.0005f;  // a zero width would divide by zero in the smoothstep

// Below this the shaders copy the engine's texel instead of shaping it: the spot where the amount rounds to nothing, so a dry frame is a pure copy
// even where the noise field reaches its top.
inline constexpr float kWetnessFloor = 0.0005f;

// floor() as the shader's floor() answers it, written out so the two implementations agree on a negative coordinate.
constexpr float FloorOf(float value) noexcept
{
    const float trunc = float(int32_t(value));
    return value < trunc ? trunc - 1.0f : trunc;
}

// The lattice hash: two multiplies, two shifts and a third multiply of 32-bit integers, then the top 24 bits as a number in 0..1. 24 bits is
// exactly the mantissa of a float, so a 24-bit value converts to float without a rounding argument between the two implementations.
constexpr uint32_t HashOf(int32_t x, int32_t y) noexcept
{
    uint32_t h = (uint32_t(x) * 0x9E3779B1u) ^ (uint32_t(y) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h >> 8;
}

constexpr float LatticeValue(int32_t x, int32_t y) noexcept { return float(HashOf(x, y)) * (1.0f / 16777216.0f); }

constexpr float Lerp(float low, float high, float weight) noexcept { return low + (high - low) * weight; }

// One octave of the value noise at a lattice coordinate `x`,`y` in cells: the four corners around it, interpolated with the smoothstep weights.
// `period` (a power of two) is the octave's own lattice period, so the higher octave wraps twice as fast and the sum is periodic.
inline float ValueNoise(float x, float y, uint32_t period) noexcept
{
    const int32_t mask = int32_t(period) - 1;
    const float fx = x - FloorOf(x);
    const float fy = y - FloorOf(y);
    const int32_t ix = int32_t(FloorOf(x));
    const int32_t iy = int32_t(FloorOf(y));
    const float wx = fx * fx * (3.0f - 2.0f * fx);
    const float wy = fy * fy * (3.0f - 2.0f * fy);
    const float v00 = LatticeValue(ix & mask, iy & mask);
    const float v10 = LatticeValue((ix + 1) & mask, iy & mask);
    const float v01 = LatticeValue(ix & mask, (iy + 1) & mask);
    const float v11 = LatticeValue((ix + 1) & mask, (iy + 1) & mask);
    return Lerp(Lerp(v00, v10, wx), Lerp(v01, v11, wx), wy);
}

// The field at a world position, in metres: one cell of `metersPerCell` for the first octave, half of it for the second. The lattice is folded
// into the period first, which is exact for a power-of-two period and keeps the smoothstep weights precise at a large zone coordinate (a few
// thousand metres of a playable area in a 32-bit float).
inline float Noise(float worldX, float worldZ, float metersPerCell) noexcept
{
    const float period = float(kNoisePeriod);
    const float cell = metersPerCell > kMinScale ? metersPerCell : kMinScale;
    const float px = worldX / cell;
    const float pz = worldZ / cell;
    const float fx = px - period * FloorOf(px / period);
    const float fz = pz - period * FloorOf(pz / period);
    return kFirstOctave * ValueNoise(fx, fz, kNoisePeriod) + kSecondOctave * ValueNoise(fx * 2.0f, fz * 2.0f, kNoisePeriod * 2u);
}

constexpr float Smoothstep(float edgeLow, float edgeHigh, float value) noexcept
{
    const float t = (value - edgeLow) / (edgeHigh - edgeLow);
    const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

// saturate() as the shader answers it, including a NaN (which reads as 0 there): `!(value > 0)` is true for a NaN.
constexpr float Saturate(float value) noexcept { return !(value > 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value); }

// The level the field is thresholded at: full rain with a coverage of 0.5 puts half the up-facing ground above it, and the level rises to 1.0
// as the engine's wetness falls to zero. That is the whole "puddles form and dry" rule: no timer and no history of this module's own, the
// engine's own weather ramp.
constexpr float Threshold(float coverage, float wetness) noexcept { return 1.0f - Saturate(coverage) * Saturate(wetness); }

// The width the smoothstep is given: the key, floored, so a softness of zero is a hard edge rather than a division by zero.
constexpr float EdgeWidth(float softness) noexcept { return softness > kSoftnessFloor ? softness : kSoftnessFloor; }

// How much of a world position is puddle, 0..1.
inline float PuddleFactor(float noise, float coverage, float wetness, float softness) noexcept
{
    const float level = Threshold(coverage, wetness);
    const float width = EdgeWidth(softness);
    return Smoothstep(level - width, level + width, noise);
}

// ---- the exposure gate: pools only where the engine's own ground is wet -------------------------------------------------------
//
// The engine decides per texel how wet its own ground looks and writes it into the material pack's alpha, the gloss weight the composite reads.
// On exposed ground that alpha follows the weather: about 0.83 at a wetness of 1 and 0.83 times the wetness below it (measured on grass, earth
// and rock outdoors). On ground under cover it is 0: a stone hall at full rain read 0.000 on all 494,317 of its floor texels. The pool field is a
// noise of the world position and knows nothing about cover, so it painted pools on that floor. The gate asks the engine instead: the pool weight
// is scaled by how close the engine's own alpha is to what exposed ground shows at this wetness. The tone stage reads the same alpha and applies the same function (below).
//
//   ratio    = alpha / (k * max(wetness, floor))
//   exposure = smoothstep(low, high, ratio)
//
// k is the engine's own alpha at full exposure and wetness 1, measured on exposed ground; it is the one fitted number, and three frames carry
// it (the rain hall 0.000 at wetness 1.0, the open ground of an earlier rainy capture 0.831 at wetness 1.0 with a 5th percentile of 0.545, open
// rock 0.141 at wetness 0.187). The band is a soft edge and its ends come from those frames, not from a fit: the top is at or below 0.65 because
// the open ground's 5th percentile is a ratio of 0.66 and still has to read fully exposed, and the bottom is above one 8-bit step of the alpha
// at the lowest wetness the ratio is taken at (1/255 over k times the floor is 0.09), so quantisation noise on ground the engine left dry
// opens nothing.
//
// The floor keeps the division away from a tiny wetness. Below it the denominator stops shrinking, so an exposed texel's ratio falls with the
// wetness (alpha is 0.83 x wetness there, over a fixed k x floor) and the exposure goes to zero with the pools themselves, instead of reading
// the quantised alpha of a drizzle as a huge ratio. A NaN or negative alpha is not exposed ground and reads as 0: a pool is never the default.
inline constexpr float kGateK = 0.83f;
inline constexpr float kGateWetnessFloor = 0.05f;
inline constexpr float kGateBandLow = 0.15f;
inline constexpr float kGateBandHigh = 0.5f;

constexpr float GateRatio(float engineAlpha, float wetness) noexcept
{
    const float alpha = !(engineAlpha > 0.0f) ? 0.0f : engineAlpha; // NaN and negative read as none (a +infinity stays: it saturates below)
    const float wet = Saturate(wetness);
    const float denominator = kGateK * (wet > kGateWetnessFloor ? wet : kGateWetnessFloor);
    return alpha / denominator;
}

constexpr float Exposure(float engineAlpha, float wetness) noexcept
{
    return Smoothstep(kGateBandLow, kGateBandHigh, GateRatio(engineAlpha, wetness));
}

// Whether the gate has an engine gloss to read on this target: the alpha it reads is the material pack's, which is target 1. On another target
// the alpha is not the gloss weight, so the gate has nothing to ask and the pass draws no pool there (the log says so once).
constexpr bool GateReadable(uint32_t target) noexcept { return target == 1; }

// What the shaped channel comes to: the engine's own value where the surface is dry or not a puddle, the wet value where it is one, and the
// engine's own ramp in between. At a wetness of zero it is the engine's value to the bit, which is what makes a dry frame a pure copy.
constexpr float GlossValue(float engine, float factor, float wetness, float wet) noexcept
{
    return engine + (wet - engine) * (Saturate(wetness) * Saturate(factor));
}

// The wetness amount the puddles must gate on. The value the engine's own draws read is not always the value the engine's wetness buffer holds:
// while the wetness knob is armed it patches that buffer for the material draws only, and the copy this module takes in the same frame holds the
// unpatched bytes. The gate therefore takes the knob's forced amount whenever the knob asks for one (0.0 and up), and the engine's own amount
// otherwise; below zero is the knob's own "off", which is the engine's weather. A knob of 1.0 and an engine buffer at 0 then shape the frame the
// engine drew wet, which is what the knob is for.
constexpr float WetnessFor(float engine, float knob) noexcept { return knob >= 0.0f ? knob : engine; }

// The wetness knob as the puddles see it: only while the engine pass overrides switch is on can its slider force the weather; with that switch off
// the slider is disabled in the menu and its stored value is ignored here, so a forgotten override never draws puddles on a dry day.
constexpr float WetnessKnobFor(bool overridesOn, float overrideValue) noexcept
{
    return overridesOn ? engine::ClampWetnessOverride(overrideValue) : engine::kWetnessOverrideDefault;
}

// Whether a frame has any wetness to shape at all: a known reading above the floor. With none, both stages keep the engine's own picture without
// copying a texture or swapping a draw.
constexpr bool WetEnough(bool known, float amount) noexcept { return known && amount > kWetnessFloor; }

// The wetness gate's dwell: once ANY reading lands above the floor the stage stays armed until the buffer reads an
// exact 0.0 (a fresh dry), whichever frame the read-back lands on. The engine's ramp is monotone while rain sets in,
// but the read-back can land stale-low for a frame at the crossing; without the dwell that frame would flip the stage
// off and the next one back on - a visible flicker exactly when the puddles start loading. The latch is shared (one
// bit in the pass both stages read), so the pool's two answers cannot disagree inside a frame; it is updated on the
// frame's wetness copy - the read's own tick - and cleared by the pass's own reset (a publish-off, a teardown) and
// by the exact zero itself.
constexpr bool WetEnoughDwell(bool latched, bool known, float amount) noexcept
{
    return latched ? amount > 0.0f : WetEnough(known, amount);
}

// ---- the frame's two answers: what a skip shows, and what a refusal holds -------------------------------------
//
// The pools' two stages amplify one frame in two places (the gloss write shapes the composite's gloss channel; the
// tone-map stage darkens that composite's picture and adds the mirror), and a frame in which they disagree is what
// a wetness ramp reads as a flash: the tone-map stage ran while the composite it read was the engine's own UNshaped
// picture - the darkening landed on several of the ramp's first frames, which is the pool painting itself black for a
// second, then the alpha arriving ("it loads the rest") - and the skipped frames in the middle were the other half of
// the flip, because every skip bound the engine's own texture, erasing one stage's answer for a frame. These rules
// settle what the draw path may do at those seams.

// What the tone-map stage does with one wet frame it was asked to answer.
enum class ToneAnswer : uint8_t
{
    None = 0, // nothing answers: the engine's own picture, which is what a dry frame (or a ramp's first frame) wants
    Hold,     // the stage's last good answer, bound at t0 for the one draw: a skip that is never seen as a gap
    Run,      // the pass runs on the frame's own scene
};

// The one decision of a wet tone-map frame: run the pass only when the frame's composite reads the gloss stage's own
// shape (one answer for the frame), hold the last answer whenever it does not or the pass could not run, and paint
// nothing only when there is no last answer yet - which is the ramp's first frame, whose composite is unshaped too, so
// both stages' absence is the same answer and nothing is painted twice. A dry frame is above the wetness gate and
// answers None regardless: the engine's picture carries no pools, which is the correct answer for it.
constexpr ToneAnswer ToneAnswerFor(bool wet, bool shapeOnFrame, bool glossAlive, bool haveLastAnswer) noexcept
{
    if (!wet || !glossAlive)
        return ToneAnswer::None; // a dry frame, or a gloss side that can no longer shape: the engine's picture, never the frozen hold
    if (shapeOnFrame)
        return ToneAnswer::Run;
    return haveLastAnswer ? ToneAnswer::Hold : ToneAnswer::None;
}

// Whether the composite of `askedToken` reads the gloss stage's shape: the stamp answers for its own frame only, so a
// token from a previous frame (a skipped attempt between stamps) reads as absent, whatever the stamp's own wet flag
// said. The pure rule the pass's ShapePresent forwards, so the host test drives the real semantics.
constexpr bool ShapePresentOnFrame(bool shapeWet, uint64_t shapeToken, uint64_t askedToken) noexcept
{
    return shapeWet && shapeToken == askedToken;
}

// Whether a binding is the object the pass's shape was computed for: the same view object first (the engine binds one
// view for the whole frame), then the same resource behind it (a second view of the same resource is served too). The
// hold answers the reader draws that still bind that object and leaves every other binding - a cut's own buffers, a
// loading screen's - on the engine's texture.
constexpr bool AnswersSameBinding(const void* holdResource, const void* holdView, const void* boundResource,
                                  const void* boundView) noexcept
{
    if (holdView != nullptr && holdView == boundView)
        return true;
    return holdResource != nullptr && boundResource != nullptr && holdResource == boundResource;
}

// Whether a frame whose own attempt was refused may answer its readers from the pass's hold: only while the wetness
// gate holds (a dry frame's engine picture has no pools, and a held shape would keep them), only while the pass owns
// a shape at all, and never past a camera the layout check refuses (a different scene is not one more frame of rain).
constexpr bool HoldAnswersShape(bool latched, bool haveShape, bool cameraServable) noexcept
{
    return latched && haveShape && cameraServable;
}

// Whether a decoded world-space normal faces up far enough to hold standing water: its y is the cosine against the world's up axis, and a
// surface steeper than sixty degrees is a wall, not a puddle floor. FFXIV is Y-up.
inline constexpr float kUpCos = 0.5f;
constexpr bool FacesUp(float normalY) noexcept { return normalY > kUpCos; }

// A decoded normal has to be a unit vector: a cleared or half-written G-buffer texel decodes to something shorter or longer, and it is not a
// surface this module can say anything about.
inline constexpr float kNormalLengthLow = 0.5f;
inline constexpr float kNormalLengthHigh = 1.5f;

// The whole formula in one call, for the CPU reference of the WARP test and for the documentation: one texel of the target out, as the channel
// this module replaces. `insideRegion` is false outside the draw's own render window, where the pass keeps the engine's value. `engineAlpha` is the
// engine's own gloss weight at the texel (the target's alpha before this pass writes it), which the exposure gate reads.
inline float ShapedChannel(float engine, float engineAlpha, bool insideRegion, bool hasSurface, float normalY, float worldX, float worldZ,
                           float wetness, const Params& params) noexcept
{
    if (!insideRegion || !hasSurface || !FacesUp(normalY) || !(wetness > 0.0f))
        return engine;
    const float noise = Noise(worldX, worldZ, params.scale);
    const float exposure = GateReadable(params.target) ? Exposure(engineAlpha, wetness) : 0.0f;
    return GlossValue(engine, PuddleFactor(noise, params.coverage, wetness, params.softness) * exposure, wetness, params.wet);
}

// ---- the camera --------------------------------------------------------------------------------------------------------------
//
// g_CameraParameter's layout is the one ext/ffxiv/relight/shaders/CameraRegisters.h states (the reflected layout of the engine's own light
// shader): the view matrix's three rows at register 0, the inverse view's three at 3, the view-projection's at 6, the inverse
// view-projection's at 10, the inverse projection's at 14 and the projection's at 18. The pass copies the first kCameraBytes of the table and
// the shader reads the inverse view's three rows and the inverse projection's four; the host test that builds a camera buffer of this shape
// includes the same header.
inline constexpr uint32_t kCameraBytes = 0x160;
static_assert(kCameraBytes == 22 * 16, "the copied range covers the camera registers 0..21 (through the projection's last row)");

// ---- the engine's target -----------------------------------------------------------------------------------------------------

// DXGI_FORMAT_B8G8R8A8_UNORM: four of the engine's five G-buffer targets are this format (the fifth, the world-space normal at index 3, is
// R16G16B16A16_FLOAT and is the one target this pass cannot shape - it reads the normal from it).
inline constexpr uint32_t kTargetFormat = 87;

// The floor the target and the depth must be at: the composite's own floor (EnginePassRules.h's kMinSceneWidth/Height), so a preview or an
// effect's scratch surface can never be mistaken for the frame's G-buffer.
inline constexpr uint32_t kMinTargetWidth = 320;
inline constexpr uint32_t kMinTargetHeight = 180;

// The depth formats the pass can read a view position out of: the 24-bit one the engine's build samples its depth as
// (R24_UNORM_X8_TYPELESS, measured) and the 32-bit variants for a build that switches. The values are DXGI_FORMATs; GlossPuddlesApply.cpp pins
// them against the real enum. The shader reads the depth's first channel through the engine's own typed view, so every one of these decodes the
// same way; a colour texture under the depth's name is what this refuses.
constexpr bool IsDepthFormat(uint32_t format) noexcept
{
    return format == 45 || // DXGI_FORMAT_D24_UNORM_S8_UINT
           format == 46 || // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
           format == 40 || // DXGI_FORMAT_D32_FLOAT
           format == 41 || // DXGI_FORMAT_R32_FLOAT
           format == 42 || // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
           format == 43;   // DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
}

// The engine's wetness buffer: 32 bytes, its first float the wetness amount (EnginePassRules.h's kWetnessBytes). The pass copies it GPU side
// once per frame and reads .x. A frame whose material draws have not handed it over yet keeps the previous value, and the buffer starts at
// zero, which is the formula's identity: the pass then copies the engine's target unchanged.
inline constexpr uint32_t kWetnessBytes = engine::kWetnessBytes;

// ---- the region of the target this module shapes ------------------------------------------------------------------------------
//
// The engine renders the picture into a window at the render size inside an allocation at the display size (measured: 1920x1080 at 0.5,0.5
// inside 3840x2160), and every reader samples the target with the same allocation-relative coordinate. The target, the depth and the normal
// are all that allocation, so one texel of the target stands for the depth and the normal texel of the same index: there is no ratio here,
// unlike the ambient occlusion pass's half-resolution texture. This module shapes the texels whose whole area lies inside the render window;
// every other texel keeps the engine's own value, copied there before the pass writes the region.
struct Region
{
    uint32_t x = 0, y = 0, width = 0, height = 0;

    constexpr bool Empty() const noexcept { return width == 0 || height == 0; }
    constexpr uint32_t Right() const noexcept { return x + width; }
    constexpr uint32_t Bottom() const noexcept { return y + height; }
};

// Truncation and the next integer up, without <cmath>: a texel index, so the arithmetic has to be the same in the rules, in the shader's
// constant block and in a host test.
constexpr uint32_t FloorToIndex(float value) noexcept
{
    if (!(value > 0.0f))
        return 0;
    return uint32_t(value);
}

constexpr uint32_t CeilToIndex(float value) noexcept
{
    const uint32_t whole = FloorToIndex(value);
    return float(whole) < value ? whole + 1u : whole;
}

// What one reader draw measured before the pass decided. `target` is the texture at the target's slot, the depth and the normal are the two
// textures the pass reads, and `window` is the draw's render window in target texels.
struct ReaderFacts
{
    bool hasTarget = false;
    bool hasDepth = false;
    bool hasNormal = false;
    bool hasCamera = false;
    bool replay = false; // the draw is another module's replay of a game draw
    uint32_t targetFormat = 0;
    uint32_t targetWidth = 0, targetHeight = 0, targetSamples = 0;
    uint32_t depthFormat = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthSamples = 0;
    uint32_t normalWidth = 0, normalHeight = 0, normalSamples = 0;
    uint32_t cameraBytes = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, width, height of the draw's render window, in target texels
};

constexpr Region RegionOf(const ReaderFacts& facts) noexcept
{
    Region region;
    if (facts.targetWidth == 0 || facts.targetHeight == 0)
        return region;
    // A texel only half inside the window does not stand for the picture (its depth belongs to whatever the window leaves out), so the region
    // takes the texels that fit whole: the first one that starts at or after the window's left edge, up to the last one that ends at or before
    // its right edge. The engine's half-pixel viewport origin is why the first texel of a window at 0.5 is texel 1, exactly as the ambient
    // occlusion pass maps its own texture.
    const uint32_t left = CeilToIndex(facts.window[0]);
    const uint32_t right = FloorToIndex(facts.window[0] + facts.window[2]);
    const uint32_t top = CeilToIndex(facts.window[1]);
    const uint32_t bottom = FloorToIndex(facts.window[1] + facts.window[3]);
    region.x = left < facts.targetWidth ? left : facts.targetWidth;
    region.y = top < facts.targetHeight ? top : facts.targetHeight;
    const uint32_t x1 = right < facts.targetWidth ? right : facts.targetWidth;
    const uint32_t y1 = bottom < facts.targetHeight ? bottom : facts.targetHeight;
    region.width = x1 > region.x ? x1 - region.x : 0;
    region.height = y1 > region.y ? y1 - region.y : 0;
    return region;
}

// ---- the draw validation -------------------------------------------------------------------------------------------------------

// Why a reader was left alone. None of these touches the draw: the engine's target stays bound and the draw runs unchanged.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the master switch is off, the coverage is at zero, or the module switched itself off
    NoTarget,       // nothing is bound at the target's slot
    TargetShape,    // the texture at that slot is not one of the engine's 8-bit G-buffer targets at a scene size
    NoDepth,        // the draw declares no depth, binds none, or binds a multisampled or empty one
    DepthFormat,    // the depth is not a format this module can read a view position out of
    NoNormal,       // the same for the G-buffer normal
    NoCamera,       // the draw declares no camera table, binds none, or binds one too small for the matrices
    CameraLayout,   // the camera table is not the layout the shaders read (checked once, on the first pass of the session)
    CameraNotReady, // the camera table's copy has not landed on the CPU yet: a frame of delay, not a refusal
    SizeMismatch,   // the target, the depth and the normal are not one allocation, or the render window does not fit in it
    Reentry,        // the draw came from this module's own work
    Replay,         // a character-preview replay of a game draw
    Failure,        // an owned texture, a shader or a draw could not run
    Exception,      // a call threw
};

inline constexpr uint32_t kDeclineReasonCount = uint32_t(Decline::Exception) + 1;

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the coverage is at zero";
    case Decline::NoTarget: return "nothing is bound at the G-buffer target's slot";
    case Decline::TargetShape: return "the texture at that slot is not one of the engine's 8-bit G-buffer targets";
    case Decline::NoDepth: return "the reader declares or binds no usable depth";
    case Decline::DepthFormat: return "the depth is not a format this module can read a view position out of";
    case Decline::NoNormal: return "the reader declares or binds no usable G-buffer normal";
    case Decline::NoCamera: return "the reader declares or binds no usable camera table";
    case Decline::CameraLayout: return "the camera table is not the layout the shaders read";
    case Decline::CameraNotReady: return "the camera table's copy has not landed yet";
    case Decline::SizeMismatch: return "the target, the depth and the normal do not agree on a size";
    case Decline::Reentry: return "the call came from this module's own work";
    case Decline::Replay: return "the draw is a character-preview replay";
    case Decline::Failure: return "the owned texture or the pass could not run";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// Which refusals are this build's failure rather than this scene's shape, and so the only ones that walk the module towards its switch-off. A
// frame whose first complete reader binds nothing usable under one of the names (a forward pass, a preview, a second view, a camera table that is
// still loading) is a scene this pass cannot serve; the next frame can be another scene, and standing the module down for the session over it is
// what makes a walk through such a scene expensive. What does count is a refusal that says this build cannot do the work at all: an owned
// texture or a draw that could not run, a call that threw, and a target of a format or size the copy and the shape cannot answer.
constexpr bool CountsTowardsSwitchOff(Decline reason) noexcept
{
    return reason == Decline::Failure || reason == Decline::Exception || reason == Decline::TargetShape;
}

// The validation of one reader draw: one of the engine's own 8-bit G-buffer targets, a single-sample depth and normal at the same allocation,
// a camera table big enough for the matrices the shader reads, and a render window that fits inside the target and covers at least one whole
// texel of it.
inline constexpr uint32_t kMinWindowWidth = 16;
inline constexpr uint32_t kMinWindowHeight = 16;

constexpr Decline Classify(const ReaderFacts& facts) noexcept
{
    if (facts.replay)
        return Decline::Replay;
    if (!facts.hasTarget)
        return Decline::NoTarget;
    if (facts.targetFormat != kTargetFormat || facts.targetWidth < kMinTargetWidth || facts.targetHeight < kMinTargetHeight)
        return Decline::TargetShape;
    // The pass copies the engine's texels into its own single-sample target before it writes them, and that copy has no return code: a
    // multisampled source would silently leave the previous frame's texels there, so the shape test refuses it like any other non-reader.
    if (facts.targetSamples > 1)
        return Decline::TargetShape;
    if (!facts.hasDepth || facts.depthWidth == 0 || facts.depthHeight == 0)
        return Decline::NoDepth;
    if (facts.depthSamples > 1)
        return Decline::NoDepth;
    if (!IsDepthFormat(facts.depthFormat))
        return Decline::DepthFormat;
    if (!facts.hasNormal || facts.normalWidth == 0 || facts.normalHeight == 0)
        return Decline::NoNormal;
    if (facts.normalSamples > 1)
        return Decline::NoNormal;
    if (!facts.hasCamera || facts.cameraBytes < kCameraBytes)
        return Decline::NoCamera;
    // The world position reads the depth at a texel and the up-facing test reads the normal of the same texel, and the shape writes the target
    // of the same texel: three allocations at another size are a build this module has not measured.
    if (facts.normalWidth != facts.depthWidth || facts.normalHeight != facts.depthHeight)
        return Decline::SizeMismatch;
    if (facts.targetWidth != facts.depthWidth || facts.targetHeight != facts.depthHeight)
        return Decline::SizeMismatch;
    if (!(facts.window[2] >= float(kMinWindowWidth)) || !(facts.window[3] >= float(kMinWindowHeight)))
        return Decline::SizeMismatch;
    if (!(facts.window[0] >= 0.0f) || !(facts.window[1] >= 0.0f))
        return Decline::SizeMismatch;
    // Half a pixel of slack: a viewport origin of 0.5 is the engine's habit and lands here.
    if (facts.window[0] + facts.window[2] > float(facts.targetWidth) + 0.5f)
        return Decline::SizeMismatch;
    if (facts.window[1] + facts.window[3] > float(facts.targetHeight) + 0.5f)
        return Decline::SizeMismatch;
    // A window that does not cover one whole texel leaves nothing this module can shape.
    if (RegionOf(facts).Empty())
        return Decline::SizeMismatch;
    return Decline::None;
}

// ---- the session -------------------------------------------------------------------------------------------------------------

// After this many failures in a row (CountsTowardsSwitchOff), the module switches itself off for the session with one log line: a build whose
// readers no longer bind what the pass needs then costs one line and one frame's work instead of a try on every frame. A frame whose reader was
// skipped is not retried inside the frame: the pass has to run before the first reader of the frame, so a later reader of the same frame cannot
// be served consistently. A skip is not a failure: it leaves the streak where it stands.
//
// The streak is this module's own and not the shared one (EnginePassRules.h's Session): that one's switch-off is the whole engine pass module's
// and would take the ambient and wetness knobs down with the puddles.
inline constexpr uint32_t kDeclineLimit = 64;

struct Session
{
    uint32_t declines = 0;
    bool off = false;

    // One refused reader. Returns true when this refusal is the one that switched the module off.
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

// ---- the per-frame memory of the engine's target ---------------------------------------------------------------------------------
//
// The engine binds one G-buffer target for the whole frame, and the readers of a frame all bind that same object; the next frame's set is
// created or reused by the engine and can be a different object, so nothing is remembered across a frame. The frame's first complete reader
// records which object it bound, and every later draw of that frame has to bind the same object to be served. The token is the caller's
// (ext/present's output-frame counter, one relaxed load).
inline constexpr uint32_t kMaxServedDraws = 8192; // a token that never advances costs one pass per few thousand readers, not a stale texture

struct FrameCache
{
    uint64_t frame = 0;
    bool hasFrame = false;
    bool valid = false; // `engine` was recorded this frame and `ours` may answer for it
    bool tried = false; // this frame's one attempt is spent (a refusal, or the pass ran)
    uint32_t served = 0; // reader draws that bound our texture this frame
    const void* engine = nullptr;     // the resource the engine's target view is on
    const void* engineView = nullptr; // and its view, for the cheap same-binding test
    const void* ours = nullptr;       // this module's texture for that size and format

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

    // The frame's one attempt is taken before the pass is tried, so a refusal costs one log line per frame instead of one per reader.
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

    // One reader answered from the frame's texture. A long run without a frame change expires the entry, so a token that does not advance
    // costs one pass every few thousand readers instead of a texture that stays at the first frame's picture.
    void Serve() noexcept
    {
        if (++served < kMaxServedDraws)
            return;
        ClearFrame();
    }
};

// ---- the log ---------------------------------------------------------------------------------------------------------------

// One line per change of state, and at most this many a session (the fork's habit for a developer-visible gate).
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

inline std::string FormatDecline(Decline reason) { return std::format("Puddles: the draw was left alone ({})", DeclineName(reason)); }

inline std::string FormatSlot(uint8_t slot) { return slot < kTextureSlots ? std::format("{}", unsigned(slot)) : std::string("none"); }

// The line the frame's first complete reader leaves when it cannot serve the frame. It carries that reader's own shape - what it binds under
// each name and where - because a session cannot otherwise tell a scene this module cannot serve (a forward draw with no depth) from a shader
// whose slots were never read.
inline std::string FormatRefusedReader(Decline reason, const ReaderFacts& facts, const Inputs& inputs, uint32_t targetIndex)
{
    return std::format("Puddles: the first reader of the frame could not serve it ({}); G-buffer {} {}x{} in format {} at slot {}, depth {}x{} "
                       "in format {} at slot {}, normal {}x{} at slot {}, camera {} bytes at slot {}, window {:.0f}x{:.0f} at {:.0f},{:.0f}",
                       DeclineName(reason), targetIndex + 1u, facts.targetWidth, facts.targetHeight, facts.targetFormat,
                       FormatSlot(inputs.targets[targetIndex < kTargetCount ? targetIndex : 0]), facts.depthWidth, facts.depthHeight,
                       facts.depthFormat, FormatSlot(inputs.depth), facts.normalWidth, facts.normalHeight, FormatSlot(inputs.targets[3]),
                       facts.cameraBytes, FormatSlot(inputs.camera), double(facts.window[2]), double(facts.window[3]),
                       double(facts.window[0]), double(facts.window[1]));
}

// The line of a run: what the pass shaped, at which size, with which keys, and what the GPU work cost once it has been measured. The wetness
// figure is the amount the shader gates on, with the two values it was chosen from: the engine's own amount (read back a frame behind, so the
// line says when it is not known yet) and the wetness knob's forced value, or `off`. The channel value the puddles shape towards is the `wet`
// field, which is a different number on purpose: `wetness` says how wet the frame is, `wet` says what a mirror-wet surface's channel holds.
inline std::string FormatRun(const Params& params, uint32_t width, uint32_t height, float wetness, float engine, bool engineKnown, float knob)
{
    const std::string knobText = knob >= 0.0f ? std::format("{:.2f}", double(knob)) : std::string("off");
    const std::string engineText = engineKnown ? std::format("{:.2f}", double(engine)) : std::string("not read");
    // With the knob off and no engine read landed yet, the shape gated on nothing this line could name: say so rather than print a figure.
    const std::string wetnessText = (!engineKnown && knob < 0.0f) ? std::string("not known") : std::format("{:.2f}", double(wetness));
    return std::format("Puddles: G-buffer {}'s {} channel over {}x{}, coverage {:.2f}, scale {:.2f} m, softness {:.2f}, wet {:.2f}, wetness "
                       "{} (engine {}, knob {})",
                       params.target, ChannelName(params.channel), width, height, double(params.coverage), double(params.scale),
                       double(params.softness), double(params.wet), wetnessText, engineText, knobText);
}

// The line that says how many reader draws of one frame bound this module's own texture. A session compares it with the number of bindings of
// that target the frame makes: a draw whose shader's reflection was stripped declares nothing, keeps the engine's texture and is not counted
// here, so the two numbers differ by exactly the readers this module cannot name.
inline std::string FormatServed(uint32_t served)
{
    return std::format("Puddles: {} reader draws of the last frame bound this module's texture", served);
}

inline std::string FormatSwitchedOff()
{
    return std::format("Puddles: {} frames in a row could not run the pass, so the puddles are off for this session", kDeclineLimit);
}

// The line a target key this pass cannot shape leaves, once per publish: the value is a configuration, not a build failure, so it is refused before
// any device call and never counts towards the switch-off.
inline std::string FormatTargetRefused(uint32_t target)
{
    return std::format("Puddles: G-buffer target {} is not one this pass can shape (3 is not an 8-bit target, 4 is the normal it reads), so the "
                       "engine's own target is bound unchanged",
                       target);
}

// The line a channel key this pass must not write on that target leaves, once per distinct pair and for the same reasons.
inline std::string FormatChannelRefused(uint32_t target, Channel channel)
{
    return std::format("Puddles: G-buffer {}'s {} channel is not one this pass can shape (the engine's composite and its wetness pass read that "
                       "byte as a selector and choose the surface encoding by it, so writing it tints the ground), so the engine's own target is "
                       "bound unchanged",
                       target, ChannelName(channel));
}

// ---- the tone-map stage: the pool's colour --------------------------------------------------------------------------------------
//
// The distinct pass of the puddles: at the tone-map draw, before the tone map reads the scene, the pool's lit colour is darkened and a marched
// mirror of the scene is added to it. The field, the wetness and the edges are the gloss pass's own (Noise, PuddleFactor, WetnessFor), read with
// the same keys, so the shape the gloss pass wrote into the G-buffer and the shape this stage darkens are one shape.
//
// Everything below is host arithmetic: the shader (shaders/PuddleReflection.hlsl) and the WARP test compute the same expressions, and the CPU
// reference in the test is this block.

// How much of a texel is pool, 0..1. `saturate(wetness)` multiplies the gloss pass's own factor so a dry frame is a pure copy even where the
// noise field reaches its top (the factor alone is nonzero for a noise above 1 - the smoothstep's far edge).
inline float PoolMask(float noise, float coverage, float wetness, float softness) noexcept
{
    return Saturate(wetness) * PuddleFactor(noise, coverage, wetness, softness);
}

// ---- the tone stage's gate: the same exposure, on the same frame's engine gloss ------------------------------------------------
//
// The darkening, the mirror and the rings are the tone stage's, and they are what a player sees of a pool, so the gate that keeps the gloss stage's
// pool out from under cover has to keep these out too. The tone stage reads the engine's material pack (G-buffer 1) AS THE FRAME'S COMPOSITE BOUND
// IT, at a sixth slot, and scales the pool mask by the very same Exposure the gloss stage applies (one function here, one HLSL copy in
// shaders/PuddleGate.hlsli that both shaders include). It reads the engine's own texture and not the gloss stage's copy because the copy's alpha is
// raised inside a pool: an exposure taken from it would be more open than the gloss stage's in the soft band, and the two stages would shape two
// pools. The engine's texture is this frame's by construction - the view is the composite's own binding, written at every composite (a composite
// that binds none leaves none) - so a frame can never be gated by an earlier frame's texture, which is the pairing fault of the black frame.
//
// What the stage does when it has no gate: nothing is drawn. A null view at the slot reads as an alpha of 0, which is an exposure of 0, so the pass
// hands the scene back to the bit. The reasons a gate can be missing are below, each with its own line in the log.

// The material pack is an 8-bit BGRA texture (a view of it is B8G8R8A8_UNORM, or its sRGB twin, whose alpha is linear all the same).
constexpr bool IsGateFormat(uint32_t format) noexcept { return format == 87u || format == 91u; }

enum class ToneGateReason : uint8_t
{
    None = 0,      // the frame's composite bound a material pack the stage can read
    TargetNotPack, // the target key names another G-buffer: it carries no engine gloss, and the gloss stage draws none there either
    NoSlot,        // the composite declares no material pack (or binds nothing at its slot)
    BadFormat,     // what is bound there is not an 8-bit BGRA texture, so its alpha is not a gloss weight
    SizeMismatch,  // it is not the size of the scene, so a texel of it is not the scene's texel
};
inline constexpr uint32_t kToneGateReasonCount = uint32_t(ToneGateReason::SizeMismatch) + 1;

// Whether, and why not, this frame's composite gives the stage a gate. The order is the order of the questions: the key, the slot, the format, the
// size.
constexpr ToneGateReason ToneGateReasonOf(bool targetReadable, bool slotBound, uint32_t viewFormat, uint32_t gateWidth, uint32_t gateHeight,
                                          uint32_t sceneWidth, uint32_t sceneHeight) noexcept
{
    if (!targetReadable)
        return ToneGateReason::TargetNotPack;
    if (!slotBound)
        return ToneGateReason::NoSlot;
    if (!IsGateFormat(viewFormat))
        return ToneGateReason::BadFormat;
    if (gateWidth != sceneWidth || gateHeight != sceneHeight)
        return ToneGateReason::SizeMismatch;
    return ToneGateReason::None;
}

// The line a frame without a gate leaves, once per change of state. Every one of them says the tone stage draws no pool.
inline std::string FormatToneGateMissing(ToneGateReason reason)
{
    switch (reason)
    {
    case ToneGateReason::TargetNotPack:
        return "Puddles: the key names a G-buffer target that carries no engine gloss, so the tone stage reads none and draws no pool (the gloss stage "
               "draws none there either)";
    case ToneGateReason::NoSlot:
        return "Puddles: this frame's composite binds no material pack (G-buffer 1), so the tone stage has no engine gloss to read and draws no pool";
    case ToneGateReason::BadFormat:
        return "Puddles: the material pack this frame's composite binds is not an 8-bit BGRA texture, so the tone stage cannot read the engine's gloss "
               "from it and draws no pool";
    case ToneGateReason::SizeMismatch:
        return "Puddles: the material pack this frame's composite binds is not the size of the scene, so the tone stage cannot read the engine's gloss "
               "texel by texel and draws no pool";
    default:
        return "Puddles: the tone stage has its gate";
    }
}

// How much of a texel is pool once the engine's own gloss has been asked: the ungated mask times the exposure. Zero where the engine renders the
// ground dry, the ungated mask on exposed ground, and a soft ramp between.
inline float GatedPoolMask(float noise, float coverage, float wetness, float softness, float engineAlpha) noexcept
{
    return PoolMask(noise, coverage, wetness, softness) * Exposure(engineAlpha, wetness);
}

// Water's F0 at normal incidence and Schlick's approximation: 2 % straight on, 100 % at grazing. It is what makes a mirror out of the
// reference's low camera and a weak sheen out of a high one, and it is why the look must be judged from a low camera.
inline constexpr float kWaterF0 = 0.02f;
inline float SchlickWater(float cosine) noexcept
{
    const float m = 1.0f - Saturate(cosine);
    const float m2 = m * m;
    return kWaterF0 + (1.0f - kWaterF0) * m2 * m2 * m;
}

// The gain the marched hit is added with: the pool's mask, the key's strength and the Fresnel of the view angle. Zero outside the pool, zero at a
// strength of zero, never negative, and never above one, so the reflection can only add the scene's own colour.
inline float PuddleReflectionGain(float mask, float cosine, float strength) noexcept
{
    return Saturate(mask) * Saturate(strength) * SchlickWater(cosine);
}

// What a pool keeps of its lit colour: a full puddle keeps `1 - darkening` of it. A water film absorbs, and the reference's ground is dark even
// where no reflection lands, which is why this is the part that has to work before the mirror does.
inline float PuddleDarkening(float mask, float darkening) noexcept { return 1.0f - Saturate(mask) * Saturate(darkening); }

// One colour channel of one texel of the tone stage: the darkened scene plus the marched colour. `hit` is 0 on a miss, and the darkened scene is
// what a miss keeps - the engine's own probe reflection of that texel, darkened, so a miss is never black and never a hole.
inline float PuddleTexel(float scene, float hit, float mask, float cosine, float strength, float darkening) noexcept
{
    return scene * PuddleDarkening(mask, darkening) + hit * PuddleReflectionGain(mask, cosine, strength);
}

// The ripple's fixed shape. The slope is read from the same noise field, two samples `kRippleStep` metres apart, on a cell of its own so the
// wobble is the same size on a pool of any scale; a static gradient, never an animated one (an animated perturbed direction is exactly what a
// temporal upscaler turns into noise).
inline constexpr float kRippleStep = 0.05f;  // metres between the two field samples the slope is read from
inline constexpr float kRippleCell = 1.0f;   // metres one ripple wave spans
inline constexpr float kRippleSlope = 0.2f;  // the slope a ripple of 1.0 tilts the mirror's normal by

// The mirror's surface tolerance, in metres: the distance a surface may lie behind the crossing and still be the one the ray crossed. A pool is
// a flat plane, so this only absorbs the stone's own relief.
inline constexpr float kMirrorThickness = 0.3f;

// The stage's own identity, which is not the gloss pass's: the switch, the coverage and the two things this stage can add. A pool with the
// reflection off and the darkening at zero is the frame's own picture, so the stage is not run and nothing is copied.
constexpr bool ToneIdentity(const Params& params) noexcept
{
    return !params.on || !(params.coverage > kMinCoverage) ||
           (!(params.darkening > kMinDarkening) && (!params.reflection || !(params.reflectionStrength > kMinReflectionStrength)));
}

// ---- the tone-map draw's shape -----------------------------------------------------------------------------------------------
//
// The tone map reads the scene the frame's composite wrote as its t0 and writes one FP16 target, so the stage recognises the draw without a
// shader CRC: t0 is a single-sample 2D view whose resource is the composite's own render target, and the draw writes one other FP16 texture at
// the same size. The five inputs the stage reads are measured once at the composite (`ToneInputs`) and cannot be re-read at the tone draw, which
// declares only t0 and its colour LUT.
struct ToneFacts
{
    uint32_t renderTargets = 0; // exactly one, and not the scene itself
    uint32_t targetWidth = 0, targetHeight = 0;
    uint32_t targetFormat = 0;
    bool targetIsScene = false; // the draw writes the captured scene: the post-tone copy, not the tone map
    uint32_t sceneFormat = 0;   // t0
    uint32_t sceneWidth = 0, sceneHeight = 0;
    bool sceneSampled = false;  // t0 is a single-sample 2D view
    bool sceneIsScene = false;  // t0's resource is the render target the frame's composite wrote
    uint32_t viewPositionWidth = 0, viewPositionHeight = 0;
    bool viewPositionSampled = false;
    uint32_t normalWidth = 0, normalHeight = 0;
    bool normalSampled = false;
    uint32_t cameraBytes = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f };
    bool replay = false; // the draw is another module's replay of a game draw
};

// The scene formats the stage can write: the composite's own (FP16) and the 11-11-10 the posts of some builds use. The stage copies the scene
// and hands the copy to the tone map at t0, so anything the tone map cannot sample is refused here.
constexpr bool IsToneSceneFormat(uint32_t format) noexcept { return format == 10u || format == 26u; } // R16G16B16A16_FLOAT, R11G11B10_FLOAT

inline constexpr uint32_t kMinToneWidth = 320;
inline constexpr uint32_t kMinToneHeight = 180;

enum class ToneDecline : uint8_t
{
    None = 0,
    NotToneMap,   // the draw is not the frame's tone map (another post pass, or one that writes the scene back)
    Replay,       // a character-preview replay of a game draw
    RenderTarget, // not exactly one render target, or it is the scene itself
    TargetFormat, // the tone map's target is not a scene format at the scene's size
    SceneFormat,  // t0 is not a single-sample 2D scene-format view of the captured scene
    Inputs,       // the frame's composite never handed over the view positions, the normal and the camera table
    SizeMismatch, // the scene, the view positions, the normal and the render window are not one allocation
    Window,       // the render window is empty or does not fit the scene
    PassFailed,   // the pass could not build its textures, shader or draw
    Exception,    // a call threw
};

inline constexpr uint32_t kToneDeclineReasonCount = uint32_t(ToneDecline::Exception) + 1;

constexpr const char* ToneDeclineName(ToneDecline reason) noexcept
{
    switch (reason)
    {
    case ToneDecline::None: return "run";
    case ToneDecline::NotToneMap: return "the draw is not the frame's tone map";
    case ToneDecline::Replay: return "the draw is a character-preview replay";
    case ToneDecline::RenderTarget: return "the tone map does not write exactly one other target";
    case ToneDecline::TargetFormat: return "the tone map's target is not a scene format at the scene's size";
    case ToneDecline::SceneFormat: return "t0 is not the scene the frame's composite wrote";
    case ToneDecline::Inputs: return "no draw of this frame handed over the view positions, the normal and the camera";
    case ToneDecline::SizeMismatch: return "the scene, the view positions and the normal are not one allocation";
    case ToneDecline::Window: return "the render window is empty or does not fit the scene";
    case ToneDecline::PassFailed: return "the pass could not build its textures or its draw";
    case ToneDecline::Exception: return "a call threw";
    }
    return "unknown";
}

constexpr ToneDecline ClassifyTone(const ToneFacts& facts) noexcept
{
    if (facts.replay)
        return ToneDecline::Replay;
    // "t0 is the frame's own scene" comes first: a draw of the same family that writes the scene back (the engine reuses it for the post-tone
    // copy) or that reads another texture is not the tone map, and it is passed on without counting, or one such draw a frame would stand the
    // stage down before its own draw came round.
    if (!facts.sceneSampled || !facts.sceneIsScene)
        return ToneDecline::NotToneMap;
    if (facts.targetIsScene)
        return ToneDecline::NotToneMap;
    if (facts.renderTargets != 1)
        return ToneDecline::RenderTarget;
    if (!IsToneSceneFormat(facts.targetFormat) || facts.targetWidth != facts.sceneWidth || facts.targetHeight != facts.sceneHeight)
        return ToneDecline::TargetFormat;
    if (!IsToneSceneFormat(facts.sceneFormat))
        return ToneDecline::SceneFormat;
    if (!facts.viewPositionSampled || !facts.normalSampled || !(facts.cameraBytes >= kCameraBytes))
        return ToneDecline::Inputs;
    if (facts.sceneWidth < kMinToneWidth || facts.sceneHeight < kMinToneHeight)
        return ToneDecline::SizeMismatch;
    if (facts.viewPositionWidth != facts.sceneWidth || facts.viewPositionHeight != facts.sceneHeight || facts.normalWidth != facts.sceneWidth ||
        facts.normalHeight != facts.sceneHeight)
        return ToneDecline::SizeMismatch;
    if (!(facts.window[2] >= float(kMinWindowWidth)) || !(facts.window[3] >= float(kMinWindowHeight)))
        return ToneDecline::Window;
    if (!(facts.window[0] >= 0.0f) || !(facts.window[1] >= 0.0f))
        return ToneDecline::Window;
    if (facts.window[0] + facts.window[2] > float(facts.sceneWidth) + 0.5f || facts.window[1] + facts.window[3] > float(facts.sceneHeight) + 0.5f)
        return ToneDecline::Window;
    return ToneDecline::None;
}

// Only this build's own failures walk the stage towards its switch-off: a frame whose tone map is a shape this build cannot serve is a scene to
// pass on, and the next frame can be another one.
constexpr bool ToneCountsTowardsSwitchOff(ToneDecline reason) noexcept
{
    return reason == ToneDecline::PassFailed || reason == ToneDecline::Exception;
}

// The line the fallback leaves once per publish while the engine's scene HDR path is on. That path replays the engine's own tone chain in its own
// branch, so the scene this stage shades would not be the picture it presents; the light shafts' branch, which owns the tone map, never runs under
// HDR at all, so this is the fallback's own gate and nothing is counted against the module for it.
inline constexpr const char* kToneSceneHdrInactive =
    "Puddles: inactive while scene HDR is on, because that path replaces the tone chain this stage draws before";

// The texels the window covers at all: the first one that starts at or after the window's left edge, up to the last one that ends at or before its
// right edge. The engine's half-pixel viewport origin is why the first texel of a window at 0.5 is texel 1, exactly as the gloss pass maps its own
// region. This is the region the draw writes and the texels a capture counts.
constexpr Region WholeTexels(float x, float y, float width, float height, uint32_t allocationWidth, uint32_t allocationHeight) noexcept
{
    Region region;
    if (allocationWidth == 0 || allocationHeight == 0 || !(width > 0.0f) || !(height > 0.0f))
        return region;
    const uint32_t left = CeilToIndex(x);
    const uint32_t top = CeilToIndex(y);
    const uint32_t right = FloorToIndex(x + width);
    const uint32_t bottom = FloorToIndex(y + height);
    region.x = left < allocationWidth ? left : allocationWidth;
    region.y = top < allocationHeight ? top : allocationHeight;
    const uint32_t x1 = right < allocationWidth ? right : allocationWidth;
    const uint32_t y1 = bottom < allocationHeight ? bottom : allocationHeight;
    region.width = x1 > region.x ? x1 - region.x : 0;
    region.height = y1 > region.y ? y1 - region.y : 0;
    return region;
}

// The part of the scene this stage copies into its own texture: the window's whole texels and one texel of border, because the tone map samples
// the scene with its own filter and a bilinear tap at the window's edge reads the texel outside it. Clamped to the allocation, so a window that
// touches an edge copies no further than the texture goes.
constexpr Region ToneCopyOf(const ToneFacts& facts) noexcept
{
    Region region;
    if (facts.sceneWidth == 0 || facts.sceneHeight == 0)
        return region;
    const int64_t left = int64_t(CeilToIndex(facts.window[0])) - 1;
    const int64_t top = int64_t(CeilToIndex(facts.window[1])) - 1;
    const int64_t right = int64_t(FloorToIndex(facts.window[0] + facts.window[2])) + 1;
    const int64_t bottom = int64_t(FloorToIndex(facts.window[1] + facts.window[3])) + 1;
    region.x = uint32_t(left > 0 ? left : 0);
    region.y = uint32_t(top > 0 ? top : 0);
    const uint32_t x1 = uint32_t(right < int64_t(facts.sceneWidth) ? right : int64_t(facts.sceneWidth));
    const uint32_t y1 = uint32_t(bottom < int64_t(facts.sceneHeight) ? bottom : int64_t(facts.sceneHeight));
    region.width = x1 > region.x ? x1 - region.x : 0;
    region.height = y1 > region.y ? y1 - region.y : 0;
    return region;
}

// The texels of that copy the draw below does not write: the one-texel ring around the render window, split into at most four bands (the rows above
// and below it, then the columns beside its own rows, so no texel is copied twice). The draw's viewport is exactly the window's whole texels, so
// every texel inside it comes from the pass and only this ring has to be the scene's own - which is what makes a frame's copy a few thousand texels
// instead of the window's whole sixteen megabytes, and what lets the stage run on a scene another pass has already written.
struct RegionBands
{
    Region band[4] {};
    uint32_t count = 0;
};

constexpr RegionBands ToneBorderOf(const ToneFacts& facts) noexcept
{
    RegionBands bands;
    const Region outer = ToneCopyOf(facts);
    if (outer.Empty())
        return bands;
    const Region inner = WholeTexels(facts.window[0], facts.window[1], facts.window[2], facts.window[3], facts.sceneWidth, facts.sceneHeight);
    // A window that covers no whole texel is no window at all (ClassifyTone refuses it); the whole copy is then the only safe answer.
    if (inner.Empty())
    {
        bands.band[0] = outer;
        bands.count = 1;
        return bands;
    }
    const Region parts[4] {
        Region { outer.x, outer.y, outer.width, inner.y > outer.y ? inner.y - outer.y : 0 },
        Region { outer.x, inner.Bottom(), outer.width, outer.Bottom() > inner.Bottom() ? outer.Bottom() - inner.Bottom() : 0 },
        Region { outer.x, inner.y, inner.x > outer.x ? inner.x - outer.x : 0, inner.height },
        Region { inner.Right(), inner.y, outer.Right() > inner.Right() ? outer.Right() - inner.Right() : 0, inner.height },
    };
    for (const Region& part : parts)
        if (!part.Empty())
            bands.band[bands.count++] = part;
    return bands;
}

// ---- the tone-map stage's constant block --------------------------------------------------------------------------------------
//
// The same five float4s the gloss pass uses, mirrored by shaders/PuddleReflection.hlsl and pinned by the host test. The wetness itself is not in
// the block: the shader reads the gloss pass's own 32-byte buffer, so both stages gate on one value.
struct ToneConstants
{
    float window[4] {};  // xy = the render window's origin in scene texels, zw = its size in scene texels
    float sizes[4] {};   // xy = the scene allocation's size, z = the march's steps, w = the raindrops' clock in seconds (0 = the still ripple)
    float tuning[4] {};  // x = coverage, y = metres per noise cell, z = the edge's half-width, w = the darkening
    float control[4] {}; // x = the reflection's strength (0 = no mirror), y = the ripple, z = the wetness floor, w = the up-facing cosine
    float region[4] {};  // xy = the scene texel this stage writes from, zw = how many texels of it
    float rain[4] {};    // x = how strongly it is raining on the pools now, 0..1 (RainStep); the rest is padding
    float reach[4] {};   // x = the pool's reach in metres (0 = no limit), the tone stage's own gate; yzw is padding
};

// How long the raindrops' clock runs before it wraps, in seconds: the shader takes sin() and frac() of it, and a float keeps a fraction of a
// millisecond for a couple of minutes and a few milliseconds at the end of the window - far below one ring's frames - so a session wraps it here
// instead of letting a morning's uptime coarsen the animation.
inline constexpr float kRippleTimePeriod = 1024.0f;

// The clock the shader's raindrops are driven by, from whatever the caller read: zero is a deterministic run (a host test, or a session whose
// clock has not been read) and keeps the still ripple the static key has always drawn, so every reference frame is unchanged. A negative or NaN
// reading is no clock either, and a value past the period is wrapped into it, so the constant block never holds a number the shader has to guard.
constexpr float RippleTimeOf(float seconds) noexcept
{
    if (!(seconds > 0.0f) || !(seconds < 1.0e7f))
        return 0.0f;
    if (seconds < kRippleTimePeriod)
        return seconds;
    const float wraps = float(int64_t(seconds / kRippleTimePeriod));
    return seconds - wraps * kRippleTimePeriod;
}

// ---- is it raining? -------------------------------------------------------------------------------------------------------------
//
// The splash rings belong to falling rain, while the pools belong to a wet floor: a floor stays wet for about a minute after the rain stops, and the
// wetness knob can wet a floor on a dry day. The engine publishes one number, its wetness amount, and it climbs while rain falls and sinks while the
// ground dries (captured: 1.00, then 0.85 six seconds later, then 0.00 within a minute). So the direction of that number is the signal: a rise means
// rain, a fall means it stopped, a flat stretch keeps the last answer, and a dry floor is never rain. Only the engine's own value is read, so the
// knob cannot start the rings. A wet floor with no history (a game start or a zone entered in the rain) counts as rain until the number falls.
inline constexpr float kRainWetnessStep = 0.002f; // the change that counts as a move; small moves add up against the reference until they cross it
inline constexpr float kRainFadeSeconds = 2.5f;   // how long the rings take to come in and to die away
inline constexpr float kRainFadeStepMaxSeconds = 0.25f; // the jump the whole fade may take in one step: a camera cut, a menu or the first call after a publish can make deltaSeconds tens of seconds, and an uncapped step would saturate the rings in one frame - the quick flicker when the puddles start loading

struct RainTracker
{
    bool known = false;     // an engine value has been read
    bool raining = false;   // the last answer
    float reference = 0.0f; // the value the next move is measured from
    float amount = 0.0f;    // what the shader gets: the answer, faded over kRainFadeSeconds
};

constexpr void RainStep(RainTracker& tracker, float engineWetness, bool engineKnown, float deltaSeconds) noexcept
{
    // The value the moves are measured from lives in the same band the amount fades in. The engine's own reading can
    // overshoot the band (a 1.02 while it rains, then a 1.00): unsaturated, the rise arms the rings and the next
    // reading reads as a fall and kills them - the rings pulsing mid-rain. The saturate makes the overshoot a flat
    // stretch, which keeps the last answer, exactly what a steady rain is.
    const float raw = engineWetness != engineWetness ? 0.0f : engineWetness;
    const float wetness = raw < 0.0f ? 0.0f : (raw > 1.0f ? 1.0f : raw);
    if (!engineKnown || !(wetness > kWetnessFloor))
    {
        tracker.raining = false; // no reading yet, or a dry floor: nothing is falling on it
        tracker.reference = engineKnown ? wetness : tracker.reference;
        tracker.known = engineKnown;
    }
    else if (!tracker.known)
    {
        tracker.known = true;
        tracker.raining = true;
        tracker.reference = wetness;
    }
    else if (wetness >= tracker.reference + kRainWetnessStep)
    {
        tracker.raining = true;
        tracker.reference = wetness;
    }
    else if (wetness <= tracker.reference - kRainWetnessStep)
    {
        tracker.raining = false;
        tracker.reference = wetness;
    }
    // The fade's step is capped: one call moves the amount by at most the cap's share of the fade, so a stamp gap
    // eases the rings in over a few frames instead of saturating them in one.
    const float delta = deltaSeconds > kRainFadeStepMaxSeconds ? kRainFadeStepMaxSeconds
                                                              : (deltaSeconds > 0.0f ? deltaSeconds : 0.0f);
    const float step = delta / kRainFadeSeconds;
    const float target = tracker.raining ? 1.0f : 0.0f;
    if (tracker.amount < target)
        tracker.amount = tracker.amount + step < target ? tracker.amount + step : target;
    else if (tracker.amount > target)
        tracker.amount = tracker.amount - step > target ? tracker.amount - step : target;
}

// `rain` is RainTracker::amount. Zero stops the clock too, so the shader's still-pool path runs and a frame with no rain costs no ring. The default
// is full rain, so a caller that passes a clock and nothing else draws the rings it asked for.
constexpr ToneConstants ToneConstantsOf(const ToneFacts& facts, const Params& params, float timeSeconds = 0.0f, float rain = 1.0f) noexcept
{
    ToneConstants constants;
    constants.window[0] = facts.window[0];
    constants.window[1] = facts.window[1];
    constants.window[2] = facts.window[2];
    constants.window[3] = facts.window[3];
    constants.sizes[0] = float(facts.sceneWidth);
    constants.sizes[1] = float(facts.sceneHeight);
    constants.sizes[2] = float(params.reflection ? params.reflectionSteps : 0u);
    const float rainAmount = rain > 0.0f ? (rain < 1.0f ? rain : 1.0f) : 0.0f; // a NaN fails the first test: no rain
    constants.sizes[3] = rainAmount > 0.0f ? RippleTimeOf(timeSeconds) : 0.0f;
    constants.rain[0] = rainAmount;
    constants.tuning[0] = params.coverage;
    constants.tuning[1] = params.scale;
    constants.tuning[2] = EdgeWidth(params.softness);
    constants.tuning[3] = params.darkening;
    constants.control[0] = params.reflection ? params.reflectionStrength : 0.0f;
    constants.control[1] = params.ripple;
    constants.control[2] = kWetnessFloor;
    constants.control[3] = kUpCos;
    // The region is the whole texels of the window, the same rule the gloss pass writes its own target with.
    const Region region = WholeTexels(facts.window[0], facts.window[1], facts.window[2], facts.window[3], facts.sceneWidth, facts.sceneHeight);
    constants.region[0] = float(region.x);
    constants.region[1] = float(region.y);
    constants.region[2] = float(region.width);
    constants.region[3] = float(region.height);
    constants.reach[0] = params.reach;
    return constants;
}

static_assert(sizeof(ToneConstants) == 112, "the tone stage's constant block is seven float4s, the same as the shader's");

// ---- what the tone stage measured ---------------------------------------------------------------------------------------------
//
// Two shares, accumulated on the GPU while a draw-diagnostics capture is armed: how much of the picture is pool, and how much of the pool the
// march found something for. They are the two numbers that say whether the stage did anything, which a capture otherwise has to guess from a
// picture nobody has seen.
struct ToneStats
{
    uint32_t poolTexels = 0; // texels whose mask is above one half
    uint32_t hitTexels = 0;  // ... of which the march found a surface
    uint32_t texels = 0;     // the render window's texels

    constexpr float PoolShare() const noexcept { return texels != 0 ? float(poolTexels) / float(texels) : 0.0f; }
    constexpr float HitShare() const noexcept { return poolTexels != 0 ? float(hitTexels) / float(poolTexels) : 0.0f; }
};

// The one line a session gets when the tone stage first runs, with what it shaded and (once a measurement has landed) what it cost.
inline std::string FormatToneRun(const Params& params, const ToneFacts& facts, const ToneStats& stats, bool statsKnown, bool measured, double gpuMs)
{
    const std::string cost = measured ? std::format(", the pass {:.4f} ms on the GPU", gpuMs) : std::string();
    const std::string shares = statsKnown ? std::format(", pool {:.3f} of the frame, the march hit {:.3f} of it", double(stats.PoolShare()),
                                                         double(stats.HitShare()))
                                         : std::string();
    return std::format("Puddles: the pool's colour at the tone map over {}x{} (window {:.0f}x{:.0f}), darkening {:.2f}, reflection {} at {:.2f} "
                       "with ripple {:.2f} over {} steps, scale {:.2f} m, coverage {:.2f}{}{}",
                       facts.sceneWidth, facts.sceneHeight, double(facts.window[2]), double(facts.window[3]), double(params.darkening),
                       params.reflection ? "on" : "off", double(params.reflectionStrength), double(params.ripple), params.reflectionSteps,
                       double(params.scale), double(params.coverage), shares, cost);
}

// engine.txt's block for this module. Written even while the card is off, like the other blocks, so a capture answers whether the module was in
// play at all; the pool and hit shares are the last measurement, which only a capture takes.
struct PuddleStatus
{
    bool keyOn = false;
    bool glossArmed = false; // the G-buffer stage can run now
    bool toneArmed = false;  // the tone stage can run now
    uint32_t glossRuns = 0;
    uint32_t glossReaders = 0;
    uint32_t toneRuns = 0;
    uint32_t glossDeclined = 0;
    uint32_t toneDeclined = 0;
    std::vector<std::pair<std::string, uint32_t>> declines; // one entry per reason with a nonzero count, prefixed by the stage
    bool statsKnown = false;
    float poolShare = 0.0f;
    float hitShare = 0.0f;
    bool haveWindow = false; // this capture's own window: the tone runs since the baseline
    uint32_t windowToneRuns = 0;
};

inline std::string PuddleStatusText(const PuddleStatus& status)
{
    std::string text;
    text += std::string("puddle module: ") + (status.keyOn ? "on" : "off") + "\n";
    if (!status.keyOn)
        return text; // the card is off: the file's own answer is that the module was never in play
    text += std::string("puddle gloss armed: ") + (status.glossArmed ? "yes" : "no") + "\n";
    text += std::string("puddle tone armed: ") + (status.toneArmed ? "yes" : "no") + "\n";
    text += "puddle gloss runs: " + std::to_string(status.glossRuns) + "\n";
    text += "puddle gloss readers: " + std::to_string(status.glossReaders) + "\n";
    text += "puddle tone runs: " + std::to_string(status.toneRuns) + "\n";
    text += "puddle declined: " + std::to_string(status.glossDeclined + status.toneDeclined) + "\n";
    for (const auto& [reason, count] : status.declines)
        if (count != 0)
            text += "puddle decline: " + reason + " = " + std::to_string(count) + "\n";
    if (status.haveWindow)
        text += "puddle window tone runs: " + std::to_string(status.windowToneRuns) + "\n";
    if (status.statsKnown)
    {
        text += std::format("puddle pool share: {:.3f}\n", double(status.poolShare));
        text += std::format("puddle march hit share: {:.3f}\n", double(status.hitShare));
    }
    else
    {
        text += std::string("puddle pool share: -\n");
        text += std::string("puddle march hit share: -\n");
    }
    return text;
}

// ---- what the gate removed ------------------------------------------------------------------------------------------------------
//
// A frame the gate acted on reads as a share: of the texels the pool field covers, how many the exposure took below a half. The pass measures it
// on the GPU (a flag per texel, bound only on the runs that ask) and counts a strided read of an earlier run's copy with a no-wait map, so the
// log can say whether a rain-indoors session loses most of its pools and an open street loses few. A run asks every kGateStatsEvery runs and
// the count reads every kGateStatsStride-th texel in each direction.
inline constexpr uint32_t kGateStatsEvery = 120;
inline constexpr uint32_t kGateStatsStride = 4;
inline constexpr uint32_t kGateMinPoolSamples = 16; // sampled pool texels below which a measurement is not reported

struct GateStats
{
    uint32_t poolTexels = 0; // sampled texels whose pool weight is above one half before the gate
    uint32_t keptTexels = 0; // ... of which are still above one half after it

    // Whether enough pool was in view for the share to say anything about cover: a frame that looks at the sky or a wall has no pool, and a
    // share of 0 over no pool is not "open".
    constexpr bool Measured() const noexcept { return poolTexels >= kGateMinPoolSamples; }

    // The share the gate took away, 0 when nothing was measured (and never negative, whatever the counts say).
    constexpr float RemovedShare() const noexcept
    {
        if (poolTexels == 0 || keptTexels >= poolTexels)
            return 0.0f;
        return float(poolTexels - keptTexels) / float(poolTexels);
    }
};

// The three states the log reports a change of: under cover (most of the pool field removed), open (little removed) and the mixed frames between,
// so a session walking in and out of a building leaves a handful of lines and not one per frame.
inline constexpr float kGateOpenBelow = 0.2f;
inline constexpr float kGateCoveredFrom = 0.8f;

enum class GateLevel : uint8_t
{
    Open = 0,
    Mixed,
    Covered,
};

constexpr GateLevel GateLevelOf(float removedShare) noexcept
{
    if (!(removedShare >= kGateOpenBelow))
        return GateLevel::Open; // low, and a NaN
    return removedShare >= kGateCoveredFrom ? GateLevel::Covered : GateLevel::Mixed;
}

// The line a change of gate state leaves. A rain-indoors frame reads a high number and an open street a low one, in the same sentence.
inline std::string FormatGateShare(const GateStats& stats)
{
    return std::format("Puddles: the engine's own ground is dry under {:.0f}% of the pool field (gate), the pools show on the rest",
                       double(stats.RemovedShare()) * 100.0);
}

// The line a target the gate cannot read leaves, once per publish: the key is a configuration, and the pass draws no pool on it.
inline std::string FormatGateUnreadable(uint32_t target)
{
    return std::format("Puddles: G-buffer target {} carries no engine gloss the gate can read (the material pack, G-buffer 1, does), so no pool is "
                       "drawn on it by either stage: both stages need the engine's gloss in G-buffer 1",
                       target);
}

// ---- the shader's constant block -----------------------------------------------------------------------------------------------
//
// The pass hands its scalars in one buffer, and tests/ext/ffxiv/engine checks the layout against the shader's. A block of five float4s: the
// render window, the target's size, the tuning, the channel and the up-facing test, and the region of the target this module writes.
struct Constants
{
    float window[4] {};  // xy = the render window's origin in target texels, zw = its size in target texels
    float sizes[4] {};   // xy = the target's size, zw = unused, w = unused
    float tuning[4] {};  // x = coverage, y = metres per noise cell, z = the edge's half-width, w = the wet value
    float control[4] {}; // x = the channel index (0..3), y = the up-facing cosine, z = the wetness floor, w = 1 when the gate can read the target's alpha (target 1), else 0
    float region[4] {};  // xy = the target texel this module shapes from, zw = how many texels of it
};



constexpr Constants ConstantsOf(const ReaderFacts& facts, const Params& params) noexcept
{
    Constants constants;
    constants.window[0] = facts.window[0];
    constants.window[1] = facts.window[1];
    constants.window[2] = facts.window[2];
    constants.window[3] = facts.window[3];
    constants.sizes[0] = float(facts.targetWidth);
    constants.sizes[1] = float(facts.targetHeight);
    constants.tuning[0] = params.coverage;
    constants.tuning[1] = params.scale;
    constants.tuning[2] = EdgeWidth(params.softness);
    constants.tuning[3] = params.wet;
    constants.control[0] = float(uint32_t(params.channel));
    constants.control[1] = kUpCos;
    constants.control[2] = kWetnessFloor;
    constants.control[3] = GateReadable(params.target) ? 1.0f : 0.0f;
    const Region region = RegionOf(facts);
    constants.region[0] = float(region.x);
    constants.region[1] = float(region.y);
    constants.region[2] = float(region.width);
    constants.region[3] = float(region.height);
    return constants;
}

static_assert(sizeof(Constants) == 80, "the pass's constant block is five float4s, the same as the shader's");
} // namespace ext::ffxiv::engine::gloss
