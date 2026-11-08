#pragma once
// Ambient occlusion (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the module does, in one line: at the reader of a frame that completes the pass's three inputs, it computes its own occlusion from the
// game's depth and its G-buffer normal into a texture of the engine's own size and format, and every later draw of that frame that would have
// sampled the engine's texture samples ours for that one call instead. The readers of the frame that ran before that one keep the engine's
// texture, so a frame is served from its completing reader onward and not before (the section "the frame's three inputs" says which frames
// that splits and when).
//
// This header holds what can be decided without a device: the inputs one of those readers declares, the pass's tuning and its identity
// rules, the mapping from an occlusion texel to the depth texel behind it, the draw validation (which draw is a reader, which is the
// engine's 4x4 placeholder, which cannot be served), the refusal counter and the per-frame memory of the engine's texture.
//
// The fail-open contract: every reason in Decline leaves the game's draw exactly as the game made it, with the engine's texture still bound,
// and the key off is today's picture by construction (the draw path is then one relaxed load per draw).

#include "ext/ffxiv/diag/ShaderBindings.h"     // the reflection's plain rows: what a bytecode parse produced
#include "ext/ffxiv/engine/EnginePassRules.h" // the shared name table and D3D11's resource-slot ceiling

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::engine::ao
{
// ---- the names a reader's shader is read for -------------------------------------------------------------------------------
//
// The occlusion texture itself is the shared name table's row (engine::kNames, so the draw path recognises a reader without a parse
// here). What this module needs besides it is the depth the occluders are found in, the normal that orients the search and the camera
// matrices the view position comes from. They are read from the same reflection, per shader, because each family binds them at its own
// slot: the sun light draws declare the depth at 0 and the normal at 7, the composite declares the normal at 9 and no depth at all, and
// a material draw declares its own set.
inline constexpr const char* kDepthName = "g_SamplerDepth";
inline constexpr const char* kNormalName = "g_SamplerGBuffer"; // G-buffer 0, the primary world-space normal (zero in the sky)
inline constexpr const char* kCameraName = "g_CameraParameter";

// D3D11's pixel-shader slots. The occlusion name is looked up in the shared table; these three are this module's own. The texture ceiling is
// the shared one (engine::kShaderResourceSlots, 128) and not a smaller number of its own: a slot is a slot whoever reads it, and a reader that
// declares its depth at 20 is the same draw as one that declares it at 0. The constant-buffer ceiling is D3D11's 14.
inline constexpr uint32_t kTextureSlots = engine::kShaderResourceSlots;
inline constexpr uint32_t kConstantBufferSlots = 14;
inline constexpr uint8_t kSlotMissing = 0xFF;

// What one reader's shader declares, as the pass reads it. A slot of kSlotMissing is a name the shader does not have.
struct Inputs
{
    uint8_t depth = kSlotMissing;
    uint8_t normal = kSlotMissing;
    uint8_t camera = kSlotMissing;

    constexpr bool HasDepth() const noexcept { return depth < kTextureSlots; }
    constexpr bool HasNormal() const noexcept { return normal < kTextureSlots; }
    constexpr bool HasCamera() const noexcept { return camera < kConstantBufferSlots; }
    constexpr bool Complete() const noexcept { return HasDepth() && HasNormal() && HasCamera(); }
};

// The three slots one reader's shader declares, from the binding table a bytecode parse produced. Only a TEXTURE binding is a slot this module
// reads: the effect compiler declares a texture and its sampler under one variable name and writes the pair into the reflection as "<name>.T"
// (the texture) and "<name>.S" (the sampler state), and it writes the samplers BEFORE the textures. A scan that accepted any binding type
// records the SAMPLER's slot instead - a slot that holds the shader's next texture at the draw - so the search reads a buffer that is not the
// G-buffer normal (measured: G-buffer 3, whose texels decode to no unit normal, which turned the whole term into 1.0 and deleted the engine's
// occlusion). The camera table is a constant buffer and is the one binding read by that type. The first texture carrying a name wins, so a
// shader that declares the same name twice is read at the slot its reflection lists first.
constexpr Inputs InputsOfBindings(const diag::ShaderBinding* bindings, size_t count) noexcept
{
    Inputs inputs;
    if (bindings == nullptr)
        return inputs;
    // The lookup is by the kind each slot binds (diag::BindPointOf), which is what keeps a sampler out of a texture's slot; the ceiling checks
    // below say a shader that declares the name past D3D11's last slot is as good as one that does not declare it.
    const uint32_t depth = diag::BindPointOf(bindings, count, diag::BindKind::Texture, kDepthName);
    if (depth < kTextureSlots)
        inputs.depth = uint8_t(depth);
    const uint32_t normal = diag::BindPointOf(bindings, count, diag::BindKind::Texture, kNormalName);
    if (normal < kTextureSlots)
        inputs.normal = uint8_t(normal);
    const uint32_t camera = diag::BindPointOf(bindings, count, diag::BindKind::ConstantBuffer, kCameraName);
    if (camera < kConstantBufferSlots)
        inputs.camera = uint8_t(camera);
    return inputs;
}

// The packed word this module stamps on a shader that reads the occlusion texture, so the frame's first reader reads its inputs with one load
// instead of a parse: slot + 1 per field, in eight bits each, which holds every slot D3D11 can bind (0..127 for a texture, 0..13 for the camera
// table). 0 in a field is "this shader does not declare it", and the whole word of 0 is "not indexed", which is every other shader of the game.
constexpr uint32_t PackInputs(const Inputs& inputs) noexcept
{
    const uint32_t depth = inputs.HasDepth() ? uint32_t(inputs.depth) + 1u : 0u;
    const uint32_t normal = inputs.HasNormal() ? uint32_t(inputs.normal) + 1u : 0u;
    const uint32_t camera = inputs.HasCamera() ? uint32_t(inputs.camera) + 1u : 0u;
    return depth | (normal << 8) | (camera << 16);
}

constexpr Inputs UnpackInputs(uint32_t packed) noexcept
{
    Inputs inputs;
    const uint32_t depth = packed & 0xFFu;
    const uint32_t normal = (packed >> 8) & 0xFFu;
    const uint32_t camera = (packed >> 16) & 0xFFu;
    inputs.depth = depth == 0 ? kSlotMissing : uint8_t(depth - 1);
    inputs.normal = normal == 0 ? kSlotMissing : uint8_t(normal - 1);
    inputs.camera = camera == 0 ? kSlotMissing : uint8_t(camera - 1);
    return inputs;
}

// ---- what one shader object answers about its slots ----------------------------------------------------------------------------
//
// The product stores this packed word on the shader object itself (one SetPrivateData at creation, AoSwap.cpp) and reads it back at the frame's
// first reader, so the memory is exactly as long-lived as the shader and nothing has to be looked up: the game creates tens of thousands of pixel
// shaders a session, and a fixed table of them loses the ones a frame needs - a bounded table took every material shader (they all declare the
// camera table) and the sun light shader's entry was gone before the first reader ever asked for it. 0 is the word of a shader this module never
// read, which is every other shader of the game and a refusal at the draw path, never a pass with guessed slots.

// ---- the tuning ------------------------------------------------------------------------------------------------------------
//
// The radius is metres of world around a pixel; 1 m is about a doorway's darkness at the corner of a wall. The strength scales how much of
// the computed occlusion is applied (1.0 is the algorithm's own answer, above 1 exaggerates it). The blend takes the product of this module's
// visibility factor with the engine's own: 1.0 applies the engine's occlusion and this module's, multiplied, and 0.0 is the engine's picture.
inline constexpr float kMinRadius = 0.1f;
inline constexpr float kMaxRadius = 4.0f;
inline constexpr float kRadiusDefault = 1.0f;
inline constexpr float kMinStrength = 0.0f;
inline constexpr float kMaxStrength = 2.0f;
inline constexpr float kStrengthDefault = 1.0f;
inline constexpr float kMinBlend = 0.0f;
inline constexpr float kMaxBlend = 1.0f;
inline constexpr float kBlendDefault = 0.5f;

// The horizon search's quality: the steps taken in each of the two directions. Low is for a laptop, high for a still comparison; the
// search costs one depth load per step and per direction, so the cost is linear in this number.
enum class Quality : uint8_t
{
    Low = 0,
    Medium = 1,
    High = 2,
};

constexpr uint32_t Steps(Quality quality) noexcept
{
    return quality == Quality::Low ? 4u : (quality == Quality::High ? 16u : 8u);
}

// The ini's text. Anything else is the default, so a typo reads as today's picture rather than as an unpredictable number of steps.
constexpr Quality ParseQuality(std::string_view text) noexcept
{
    if (text == "low")
        return Quality::Low;
    if (text == "high")
        return Quality::High;
    return Quality::Medium;
}

constexpr const char* QualityName(Quality quality) noexcept
{
    return quality == Quality::Low ? "low" : (quality == Quality::High ? "high" : "medium");
}

constexpr float ClampRadius(float value) noexcept
{
    if (value != value)
        return kRadiusDefault;
    if (value < kMinRadius)
        return kMinRadius;
    return value > kMaxRadius ? kMaxRadius : value;
}

constexpr float ClampStrength(float value) noexcept
{
    if (value != value)
        return kStrengthDefault;
    if (value < kMinStrength)
        return kMinStrength;
    return value > kMaxStrength ? kMaxStrength : value;
}

constexpr float ClampBlend(float value) noexcept
{
    if (value != value)
        return kBlendDefault;
    if (value < kMinBlend)
        return kMinBlend;
    return value > kMaxBlend ? kMaxBlend : value;
}

// One session's tuning, from the keys.
struct Params
{
    bool on = false;
    float radius = kRadiusDefault;
    float strength = kStrengthDefault;
    float blend = kBlendDefault;
    Quality quality = Quality::Medium;
};

// A strength of zero applies no occlusion at all and a blend of zero keeps the engine's texture, so both are today's picture: the pass does
// not run, nothing is created and no draw is touched. Written so a NaN reads as identity, like the other knobs of this module.
constexpr bool IsIdentity(const Params& params) noexcept
{
    return !params.on || !(params.strength > 0.0f) || !(params.blend > 0.0f);
}

// The feature-bit rule: the draw path is worth seeing only while the master switch is on and at least one value is off the identity.
constexpr bool Wants(const Params& params) noexcept { return !IsIdentity(params); }

// What one texel of our occlusion comes to: the computed occlusion scaled by the strength, cosine-limits intact (1 = unoccluded), and then
// mixed with the engine's own value by the blend. Both ends of the range are computed in float and written as 8 bits, so the pass and the
// host test agree to the first decimal.
constexpr float Factor(float occlusion, float strength) noexcept
{
    const float term = 1.0f - strength * (1.0f - occlusion);
    return term > 1.0f ? 1.0f : (term < 0.0f ? 0.0f : term);
}

// Two visibility factors in the same 1 = unoccluded convention compose by multiplication, and the blend takes that product part of the way.
// 1.0 is the engine's occlusion and this module's, multiplied; 0.0 is the engine's own value; and a term of 1.0 - nothing found, or a strength
// of 0 - is the identity at every blend, so an empty or wrong search can never delete the engine's occlusion. A linear mix expressed none of
// those three: at 1.0 it threw the engine's value away, and at 0.5 it averaged two factors that multiply.
constexpr float Mix(float engine, float ours, float blend) noexcept
{
    const float mixed = engine * (1.0f + (ours - 1.0f) * blend);
    return mixed > 1.0f ? 1.0f : (mixed < 0.0f ? 0.0f : mixed);
}

// ---- what a reader draw is -------------------------------------------------------------------------------------------------
//
// The engine binds one B8G8R8A8_UNORM texture at the render size under the occlusion name, and a 4x4 placeholder of the same format on a few
// draws that have no occlusion of their own. Only the render-size binding is a reader this module serves; the placeholder is another draw of
// the frame, passed on without a refusal counted, exactly as the other knobs treat a buffer under their name that is not theirs.
inline constexpr uint32_t kOcclusionFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
inline constexpr uint32_t kMinOcclusionWidth = 16;
inline constexpr uint32_t kMinOcclusionHeight = 16;

constexpr bool IsReaderTarget(uint32_t format, uint32_t width, uint32_t height) noexcept
{
    return format == kOcclusionFormat && width >= kMinOcclusionWidth && height >= kMinOcclusionHeight;
}

// The camera table the pass copies: the three matrices it reads, in the engine's own layout (g_CameraParameter, read from the shader's
// reflection: the view's three rows at +0x00, the view-projection's at +0x60, the inverse view-projection's at +0xA0, the inverse
// projection's at +0xE0 and the projection's at +0x120). The same size the contact shadow pass reads, because it is the same table.
inline constexpr uint32_t kCameraBytes = 0x160;

// What one reader draw measured before the pass decided. `occlusion` is the texture at the occlusion slot; the depth and the normal are the
// two textures the pass reads, at whatever size the game allocated them (the display allocation in a session with the picture rendered
// smaller, which is why the mapping below is not an assumption about equal sizes).
struct ReaderFacts
{
    bool hasOcclusion = false;
    bool hasDepth = false;
    bool hasNormal = false;
    bool hasCamera = false;
    bool replay = false;           // the draw is another module's replay of a game draw
    uint32_t occlusionFormat = 0;
    uint32_t occlusionWidth = 0, occlusionHeight = 0, occlusionSamples = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthSamples = 0;
    uint32_t normalWidth = 0, normalHeight = 0, normalSamples = 0;
    uint32_t cameraBytes = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, width, height of the draw's render window, in depth texels
};

// The depth texel that stands behind one texel of the occlusion target. The engine's occlusion texture is a fraction of the display
// allocation, and every draw that reads it samples it with the SAME allocation-relative screen coordinate it samples the G-buffer and the
// lighting with (measured: the readers' shaders multiply the pixel position by the same `g_CommonParameter.m_RenderTarget` constant), not with
// a coordinate relative to the render window. So a texel of ours stands for the depth texels at the same fraction of the depth allocation:
// the ratio of the two allocations' sizes, with no origin of its own.
//
// With the picture rendered at the occlusion texture's own size the ratio is 1. In a session that renders 1920x1080 inside a 3840x2160
// allocation, the engine's occlusion texture is 1920x1080 and the ratio is 2: one texel of ours covers a 2x2 block of depth texels.
struct Mapping
{
    float scaleX = 1.0f, scaleY = 1.0f; // depth texels per occlusion texel: the two allocations' ratio

    // The depth texel a texel of ours covers, as a texel coordinate whose integer part is that texel's index (the first one of the block).
    constexpr float DepthX(float occlTexel) const noexcept { return occlTexel * scaleX; }
    constexpr float DepthY(float occlTexel) const noexcept { return occlTexel * scaleY; }
};

constexpr Mapping MappingOf(const ReaderFacts& facts) noexcept
{
    Mapping mapping;
    if (facts.occlusionWidth == 0 || facts.occlusionHeight == 0)
        return mapping;
    mapping.scaleX = float(facts.depthWidth) / float(facts.occlusionWidth);
    mapping.scaleY = float(facts.depthHeight) / float(facts.occlusionHeight);
    return mapping;
}

// The texels of the occlusion target this module computes: the ones whose whole depth block lies inside the draw's render window. The
// engine's own producer fills that part and no other (measured: a 960x540 viewport inside a 1920x1080 texture, whose readers read a 960x540
// quadrant of it through the allocation-relative coordinate above), so this is the part that lines up with the engine's own answer. Every
// other texel keeps the engine's value, copied there before the final draw writes this region.
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

constexpr Region RegionOf(const ReaderFacts& facts) noexcept
{
    Region region;
    if (facts.occlusionWidth == 0 || facts.occlusionHeight == 0)
        return region;
    const Mapping mapping = MappingOf(facts);
    if (!(mapping.scaleX > 0.0f) || !(mapping.scaleY > 0.0f))
        return region;
    // A texel only half inside the window does not stand for the picture (its depth belongs to whatever the window leaves out), so the
    // region takes the blocks that fit whole: the first one that starts at or after the window's left edge, up to the last one that ends at
    // or before its right edge.
    const uint32_t left = CeilToIndex(facts.window[0] / mapping.scaleX);
    const uint32_t right = FloorToIndex((facts.window[0] + facts.window[2]) / mapping.scaleX);
    const uint32_t top = CeilToIndex(facts.window[1] / mapping.scaleY);
    const uint32_t bottom = FloorToIndex((facts.window[1] + facts.window[3]) / mapping.scaleY);
    region.x = left < facts.occlusionWidth ? left : facts.occlusionWidth;
    region.y = top < facts.occlusionHeight ? top : facts.occlusionHeight;
    const uint32_t x1 = right < facts.occlusionWidth ? right : facts.occlusionWidth;
    const uint32_t y1 = bottom < facts.occlusionHeight ? bottom : facts.occlusionHeight;
    region.width = x1 > region.x ? x1 - region.x : 0;
    region.height = y1 > region.y ? y1 - region.y : 0;
    return region;
}

// ---- the frame's three inputs, as its readers supply them ----------------------------------------------------------------
//
// The first reader of a frame is not a draw that declares all three (measured in one session: a material draw declares the camera table only,
// the light draw the depth and the camera, the deferred composite the normal and the camera, and the first of the three alternates from one
// frame to the next). So every reader of the frame contributes what it declares, the first usable binding of each input is the one the frame
// keeps, and the pass runs at the first reader for which all three are in hand - the reader that supplied the last missing one. A reader that
// supplies nothing usable is not the frame's attempt: it costs one rate-limited line and the frame waits for the next reader. Fail-open is
// unchanged: a frame no reader completes keeps the engine's texture, and kDeclineLimit such frames in a row pause the module for a while.
//
// The frame is torn, and that is the price of running at a reader the frame has already gone past: the swap of the engine's texture starts
// only after the pass has run, so every reader of that frame inspected before the completing one keeps the engine's occlusion and every one
// after it gets this module's. In the captured draw order (the sun light draws 1370..1379, the deferred composite 1442, the materials 1444
// and later) a frame whose first reader is a light draw that declares no G-buffer normal is completed by the composite, so that frame's light
// draws keep the engine's mask while its composite and materials get ours; a frame whose first reader is the composite or a material draw has
// no later depth supplier at all, so it is never completed and keeps the engine's texture for the whole frame. Nothing re-serves a reader the
// frame has already gone past: the pass cannot run at a draw that has happened.

// One input as a reader bound it: whether it was there at all, the slot the shader declared it at (for the log), the shape the device measured
// and, for the camera table, the device object behind it - the view's identity (FrameInputs::Merge). The camera's size is its buffer's byte
// width, the textures' the allocation's.
struct BoundInput
{
    bool present = false;
    bool twoD = false;
    uint8_t slot = kSlotMissing;
    uint32_t width = 0, height = 0, samples = 0, bytes = 0;
    const void* resource = nullptr; // the camera table's own object: compared across the readers that supply the frame's inputs
};

// What the pass can actually read. The merge below keeps a binding only when this says so, and Classify refuses a frame whose inputs fail it,
// so the two never disagree about a binding: the depth and the normal are single-sample 2D textures of the frame's own allocation, and the
// camera table is big enough for the matrices the shader reads.
constexpr bool UsableTexture(uint32_t width, uint32_t height, uint32_t samples, bool twoD) noexcept
{
    return twoD && width > 0 && height > 0 && samples == 1;
}
constexpr bool UsableCamera(uint32_t bytes) noexcept { return bytes >= kCameraBytes; }

constexpr bool UsableTextureInput(const BoundInput& input) noexcept
{
    return input.present && UsableTexture(input.width, input.height, input.samples, input.twoD);
}
constexpr bool UsableCameraInput(const BoundInput& input) noexcept { return input.present && UsableCamera(input.bytes); }

// How many readers of one frame are inspected before the frame gives up and keeps the engine's texture. The readers of a measured frame
// completed the three inputs within one or two readers; the bound exists so a build no reader can supply costs a fixed number of state
// queries per frame rather than the whole frame's readers.
inline constexpr uint32_t kMaxFrameInputReaders = 64;

// The three inputs one frame has collected, and the view they belong to. Pure and device free: the caller owns the views themselves and keeps
// exactly the ones a merge took.
struct FrameInputs
{
    // What one reader contributed to the frame.
    struct Contribution
    {
        bool depth = false, normal = false, camera = false; // the inputs the frame took from this reader
        bool otherView = false;                            // this reader binds another view's camera table
        constexpr bool Kept() const noexcept { return depth || normal || camera; }
    };

    uint64_t frame = 0;
    bool hasFrame = false;
    bool noted = false;   // this frame was counted towards the pause once, whatever its first decline was (NoteFrame)
    uint32_t readers = 0; // readers of this frame inspected for its inputs
    BoundInput depth, normal, camera;
    // The camera table's device object, which is the identity of the view the frame's textures belong to. The depth, the normal and the
    // camera the pass reads are one view's only when the readers that supplied them bound the same table.
    bool hasView = false;
    const void* view = nullptr;

    void Reset() noexcept { *this = FrameInputs {}; }

    // A different frame token starts a frame: nothing the last one supplied crosses over, because a frame's inputs are its own buffers.
    // Returns true when the token is a new frame, so the caller can let go of the views it was holding.
    bool Begin(uint64_t token) noexcept
    {
        if (hasFrame && token == frame)
            return false;
        Reset();
        hasFrame = true;
        frame = token;
        return true;
    }

    // Whether this frame is counted towards the pause now. One frame counts once: its first decline (a reader that had to wait, or the
    // frame-level refusal, whichever comes first) takes the count and every later decline of the same frame takes nothing, so a frame that
    // waits and is then refused is one frame, not two.
    bool TakeCount() noexcept
    {
        if (noted)
            return false;
        noted = true;
        return true;
    }

    // One reader's bindings: the camera table it bound, and the depth and the normal. The first usable binding of each input is kept, so a
    // later reader cannot move the pass's inputs under the frame. The camera table is also the view's identity: measured in one frame's
    // capture, the sun light draws, the deferred composite and the material draws of one view all bind the same g_CameraParameter object (each
    // at its own slot), so a reader that binds another object is another view - a reflection, a mirror, a preview camera - whose depth has the
    // same allocation's size and would otherwise pass for this frame's. A texture that arrives without a usable camera table cannot be
    // attributed to a view and is not taken (the frame keeps waiting); a texture from another view refuses the frame rather than serving it
    // with a search computed from two views' buffers.
    Contribution Merge(const BoundInput& cameraBound, const BoundInput& depthBound, const BoundInput& normalBound) noexcept
    {
        Contribution taken;
        if (!UsableTextureInput(depthBound) && !UsableTextureInput(normalBound))
            return taken; // a reader that binds none of the pass's textures: a material draw binds the camera table and its own normal
        if (!UsableCameraInput(cameraBound))
            return taken; // no view to attribute the texture to
        if (hasView && cameraBound.resource != view)
        {
            taken.otherView = true;
            return taken;
        }
        if (!hasView)
        {
            hasView = true;
            view = cameraBound.resource;
            camera = cameraBound;
            taken.camera = true;
        }
        if (!depth.present && UsableTextureInput(depthBound))
        {
            depth = depthBound;
            taken.depth = true;
        }
        if (!normal.present && UsableTextureInput(normalBound))
        {
            normal = normalBound;
            taken.normal = true;
        }
        return taken;
    }

    constexpr bool Complete() const noexcept
    {
        return UsableTextureInput(depth) && UsableTextureInput(normal) && UsableCameraInput(camera);
    }

    // The slots for the log: each input at the slot the reader that supplied it declared it at.
    constexpr Inputs Slots() const noexcept { return Inputs { depth.slot, normal.slot, camera.slot }; }
};

// The facts the validation reads: the occlusion and the render window are the triggering reader's, the depth, the normal and the camera are the
// frame's, each from whichever reader of the frame supplied it.
constexpr ReaderFacts FactsOf(const ReaderFacts& reader, const FrameInputs& inputs) noexcept
{
    ReaderFacts facts = reader;
    facts.hasDepth = inputs.depth.present;
    facts.depthWidth = inputs.depth.width;
    facts.depthHeight = inputs.depth.height;
    facts.depthSamples = inputs.depth.samples;
    facts.hasNormal = inputs.normal.present;
    facts.normalWidth = inputs.normal.width;
    facts.normalHeight = inputs.normal.height;
    facts.normalSamples = inputs.normal.samples;
    facts.hasCamera = inputs.camera.present;
    facts.cameraBytes = inputs.camera.bytes;
    return facts;
}

// Why a reader was left alone. None of these touches the draw: the engine's texture stays bound and the draw runs unchanged.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the master switch is off, the strength or the blend is at zero, or the module paused
    NoOcclusion,    // nothing is bound at the occlusion slot
    OcclusionShape, // the texture at that slot is not the engine's render-size B8G8R8A8 occlusion
    NoDepth,        // the draw declares no depth, binds none, or binds a multisampled or empty one
    NoNormal,       // the same for the G-buffer normal
    NoCamera,       // the draw declares no camera table, binds none, or binds one too small for the matrices
    SizeMismatch,   // the depth and the normal are not the same allocation, or the render window does not fit in them
    ViewMismatch,   // a reader of the frame binds another view's camera table: the frame's inputs would mix two views
    Reentry,        // the draw came from this module's own work
    Replay,         // a character-preview replay of a game draw
    Failure,        // an owned texture, a shader or a draw could not run
    Exception,      // a call threw
};

inline constexpr uint32_t kDeclineReasonCount = 13;
static_assert(uint32_t(Decline::Exception) + 1 == kDeclineReasonCount, "kDeclineReasonCount must cover every ao::Decline value");

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the strength or blend is at zero";
    case Decline::NoOcclusion: return "nothing is bound at the occlusion slot";
    case Decline::OcclusionShape: return "the texture at that slot is not the engine's occlusion texture";
    case Decline::NoDepth: return "the reader declares or binds no usable depth";
    case Decline::NoNormal: return "the reader declares or binds no usable G-buffer normal";
    case Decline::NoCamera: return "the reader declares or binds no usable camera table";
    case Decline::SizeMismatch: return "the depth, the normal and the render window do not agree on a size";
    case Decline::ViewMismatch: return "a reader of the frame binds another view's camera table";
    case Decline::Reentry: return "the call came from this module's own work";
    case Decline::Replay: return "the draw is a character-preview replay";
    case Decline::Failure: return "the owned textures or the pass could not run";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// Whether the decline is about an input a later reader of the same frame can still supply (the frame waits, nothing is spent) rather than a
// decision about the frame itself (the occlusion's shape, a size no reader will change, the window).
constexpr bool IsWaitingDecline(Decline reason) noexcept
{
    return reason == Decline::NoDepth || reason == Decline::NoNormal || reason == Decline::NoCamera;
}

// The validation of one reader draw: the engine's own occlusion texture, the depth and the normal at one single-sample allocation each, a
// camera table big enough for the matrices the shader reads, and a render window that fits inside the depth. The window is what the pass
// measures with, so it also has to be a picture rather than a few texels: a render target this small is a preview, not the scene the
// occlusion is for.
inline constexpr uint32_t kMinWindowWidth = 16;
inline constexpr uint32_t kMinWindowHeight = 16;

constexpr Decline Classify(const ReaderFacts& facts) noexcept
{
    if (facts.replay)
        return Decline::Replay;
    if (!facts.hasOcclusion)
        return Decline::NoOcclusion;
    if (!IsReaderTarget(facts.occlusionFormat, facts.occlusionWidth, facts.occlusionHeight))
        return Decline::OcclusionShape;
    // The pass copies the engine's texels into its own single-sample target before it writes them, and that copy has no return code: a
    // multisampled source would silently leave the previous frame's texels there, so the shape test refuses it like any other non-reader.
    if (facts.occlusionSamples > 1)
        return Decline::OcclusionShape;
    if (!facts.hasDepth || !UsableTexture(facts.depthWidth, facts.depthHeight, facts.depthSamples, true))
        return Decline::NoDepth;
    if (!facts.hasNormal || !UsableTexture(facts.normalWidth, facts.normalHeight, facts.normalSamples, true))
        return Decline::NoNormal;
    if (!facts.hasCamera || !UsableCamera(facts.cameraBytes))
        return Decline::NoCamera;
    // The occlusion search reads the depth at a texel and the normal of the same texel, so the two allocations have to be the same one; a
    // G-buffer at another size is a build this module has not measured.
    if (facts.normalWidth != facts.depthWidth || facts.normalHeight != facts.depthHeight)
        return Decline::SizeMismatch;
    if (!(facts.window[2] >= float(kMinWindowWidth)) || !(facts.window[3] >= float(kMinWindowHeight)))
        return Decline::SizeMismatch;
    if (!(facts.window[0] >= 0.0f) || !(facts.window[1] >= 0.0f))
        return Decline::SizeMismatch;
    // Half a pixel of slack: a viewport origin of 0.5 is the engine's habit and lands here.
    if (facts.window[0] + facts.window[2] > float(facts.depthWidth) + 0.5f)
        return Decline::SizeMismatch;
    if (facts.window[1] + facts.window[3] > float(facts.depthHeight) + 0.5f)
        return Decline::SizeMismatch;
    // A window that does not cover one whole texel of the occlusion target leaves nothing this module can align to the engine's own answer.
    if (RegionOf(facts).Empty())
        return Decline::SizeMismatch;
    return Decline::None;
}

// ---- the session -----------------------------------------------------------------------------------------------------------

// After this many frames in a row are left alone, the module pauses instead of standing down for the session. One frame counts once, whether its
// first decline was a reader that had to wait or a frame-level refusal (NoteFrame). The frames that drive the streak are measured to be a menu's
// or a loading screen's (the stand-down of the session that exposed this happened with the menu opened and the window unfocused), and a frame whose
// first reader is the deferred composite has no later depth supplier in the captured order, so a terminal stand-down would cost an ordinary
// session the effect for good. While paused, the whole of one frame's readers is inspected every kPauseFrames frames - a wait inside it neither
// ends that inspection nor defers the next one - so the scene coming back re-arms the module within kPauseFrames frames, and a build whose frames
// can never be completed pays one bounded frame inspection every kPauseFrames frames. The per-frame bound (kMaxFrameInputReaders) and the served
// frame that clears the streak are unchanged.
inline constexpr uint32_t kDeclineLimit = 64;
inline constexpr uint64_t kPauseFrames = 120;      // about 2.7 seconds at the measured session's 45 frames a second
inline constexpr uint32_t kMaxPausedDraws = 65536; // a present counter that stands still: one look per this many reader draws, and the look
                                                   // starts its frame from scratch, so it inspects instead of reaching the spent attempt

// Whether the draw path may inspect this frame's readers, and which part of a look this draw is. Only `Skip` passes a draw on without a state
// query.
enum class LookStep : uint8_t
{
    Skip,       // a paused module, and this is not the frame it looks at: one load and out
    Frame,      // the module is not paused: every frame is inspected
    ProbeBegin, // the pause's look starts at this frame: the caller clears the frame's spent attempt and what it collected before it collects
    Probe,      // the rest of the frame the look was granted for: its readers are inspected up to the frame's own bound
};

struct Session
{
    uint32_t declines = 0;    // frames in a row that were left alone, counted once per frame
    bool off = false;         // the module paused after the limit
    uint64_t lookAt = 0;      // the frame token at which a paused module looks at a frame's readers again
    uint32_t pausedDraws = 0; // reader draws a paused module passed on since its last look
    bool haveLook = false;    // a look was granted and its frame is still being inspected
    uint64_t lookFrame = 0;   // the frame that look is on

    // One frame left alone, counted once per frame (`NoteFrame` is the gate every caller goes through). Returns true when this frame is the one
    // that paused the module: that frame is the pause's first look, so a later reader of it can still complete it and end the pause at once. A
    // frame-level refusal of a look's own frame defers the next look by a whole interval; a wait in that frame never does (NoteFrame).
    bool NoteDecline(uint64_t frame)
    {
        if (off)
        {
            lookAt = frame + kPauseFrames;
            pausedDraws = 0;
            haveLook = false;
            return false;
        }
        if (++declines < kDeclineLimit)
            return false;
        off = true;
        lookAt = frame + kPauseFrames;
        pausedDraws = 0;
        return true;
    }

    // The frame the pause inspects first: the one that reached the limit on a reader that had to wait, which a later reader of the same frame
    // can still complete. A pause that a frame-level refusal reached has nothing left to inspect in its own frame (its one attempt is spent), so
    // it starts with the next frame at or past lookAt instead.
    void BeginLook(uint64_t frame) noexcept
    {
        haveLook = true;
        lookFrame = frame;
        pausedDraws = 0;
    }

    // Whether this frame's readers may be inspected. Not paused: every frame is. Paused: the first draw of the frame the look is granted for
    // starts that look from scratch (ProbeBegin), every later draw of that same frame is part of it (Probe), and a frame after a look that ended
    // without serving it sets the next look a whole interval later. A present counter that stands still is looked at again for every
    // kMaxPausedDraws draws the module passes on, instead of never.
    LookStep Look(uint64_t frame)
    {
        if (!off)
            return LookStep::Frame;
        if (haveLook)
        {
            if (frame == lookFrame)
                return LookStep::Probe;
            lookAt = lookFrame + kPauseFrames; // the look's frame ended without serving it: the next look is one interval after it
            haveLook = false;
            pausedDraws = 0;
        }
        if (frame >= lookAt || ++pausedDraws >= kMaxPausedDraws)
        {
            haveLook = true;
            lookFrame = frame;
            pausedDraws = 0;
            return LookStep::ProbeBegin;
        }
        return LookStep::Skip;
    }

    // A frame this module served: the scene the pause was waiting for, so the streak and the pause are both over.
    void NoteRun()
    {
        declines = 0;
        off = false;
        pausedDraws = 0;
        haveLook = false;
    }

    bool Paused() const noexcept { return off; }
};

// One frame left alone, counted once per frame: the first decline of a frame - a reader that had to wait, or the frame-level refusal, whichever
// comes first - counts it, and every later decline of the same frame counts nothing (`counted` is the frame's own flag, FrameInputs::noted). A
// wait on the frame a paused module is looking at counts nothing at all: the look is doing its job, and one of its readers taking its time must
// not end it. A refusal of that frame does end it - the frame has nothing left to inspect, and a present counter that stands still has to be able
// to reach the next look - and sets the next one a whole interval out. Returns true when this frame is the one that paused the module.
inline bool NoteFrame(Session& session, bool& counted, uint64_t frame, bool waiting)
{
    if (waiting && session.Paused())
        return false; // the look's own frame, still being inspected
    if (session.Paused())
    {
        session.NoteDecline(frame); // a refusal of the look's frame: the look is over and the next one is an interval away
        return false;
    }
    if (counted)
        return false;
    counted = true;
    const bool paused = session.NoteDecline(frame);
    if (paused && waiting)
        session.BeginLook(frame); // a frame that paused the module on a wait can still be completed by its later readers
    return paused;
}

// ---- the per-frame memory of the engine's texture -----------------------------------------------------------------------------
//
// The engine alternates between two occlusion textures from one frame to the next (measured: 0x...4b2e0 in one frame, 0x...eb7e60 in the
// next), so nothing is remembered across a frame: the frame's first reader records which texture the engine bound, and every later draw of
// that frame has to bind the same object to be served. The token is the caller's (ext/present's output-frame counter, one relaxed load).
inline constexpr uint32_t kMaxServedDraws = 8192; // a token that never advances costs one pass per few thousand readers, not a stale texture

struct FrameCache
{
    uint64_t frame = 0;
    bool hasFrame = false;
    bool valid = false;  // `engine` was recorded this frame and `ours` may answer for it
    bool tried = false;  // this frame's one attempt is spent (a refusal, or the pass ran)
    uint32_t served = 0; // reader draws that bound our texture this frame
    const void* engine = nullptr;   // the resource the engine's occlusion view is on
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

// ---- the cost meter and the log -----------------------------------------------------------------------------------------------
//
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

inline std::string FormatDecline(Decline reason)
{
    return std::format("Ambient occlusion: the draw was left alone ({})", DeclineName(reason));
}

inline std::string FormatSlot(uint8_t slot)
{
    return slot < kTextureSlots ? std::format("{}", unsigned(slot)) : std::string("none");
}

// The line a refused frame leaves when no reader could serve it (or a reader's inputs disagreed). It carries that reader's own shape - what
// the frame has collected under each name and where - because a session cannot otherwise tell a scene this module cannot serve (a frame whose
// readers never supply a depth) from a shader whose slots were never read (a stamp that never landed): both refuse with the same reason.
inline std::string FormatRefusedReader(Decline reason, const ReaderFacts& facts, const Inputs& inputs)
{
    return std::format("Ambient occlusion: the frame could not be served ({}); occlusion {}x{} in format {}, depth {}x{} at slot "
                       "{}, normal {}x{} at slot {}, camera {} bytes at slot {}, window {:.0f}x{:.0f} at {:.0f},{:.0f}",
                       DeclineName(reason), facts.occlusionWidth, facts.occlusionHeight, facts.occlusionFormat, facts.depthWidth,
                       facts.depthHeight, FormatSlot(inputs.depth), facts.normalWidth, facts.normalHeight, FormatSlot(inputs.normal),
                       facts.cameraBytes, FormatSlot(inputs.camera), double(facts.window[2]), double(facts.window[3]),
                       double(facts.window[0]), double(facts.window[1]));
}

// The line one reader of a frame leaves when the frame is still missing an input: the frame waits for another reader of the same frame, so
// nothing of the draw is touched and no attempt is spent. It carries the same shape as the refusal line (what the frame has collected and at
// which slots), because a session cannot otherwise tell a scene this module cannot serve from a reader whose inputs another reader supplies.
inline std::string FormatWaiting(Decline reason, const ReaderFacts& facts, const Inputs& inputs)
{
    return std::format("Ambient occlusion: the frame waits for another reader ({}); occlusion {}x{} in format {}, depth {}x{} at slot {}, normal "
                       "{}x{} at slot {}, camera {} bytes at slot {}, window {:.0f}x{:.0f} at {:.0f},{:.0f}",
                       DeclineName(reason), facts.occlusionWidth, facts.occlusionHeight, facts.occlusionFormat, facts.depthWidth, facts.depthHeight,
                       FormatSlot(inputs.depth), facts.normalWidth, facts.normalHeight, FormatSlot(inputs.normal), facts.cameraBytes,
                       FormatSlot(inputs.camera), double(facts.window[2]), double(facts.window[3]), double(facts.window[0]),
                       double(facts.window[1]));
}

// The line of a run: what the pass computed, at which sizes, where its three inputs were bound, and what the GPU work cost once it has been
// measured. The slots are each input's own - the reader of the frame that supplied it - because the failure this line exists for (a shader
// whose sampler register was read as its texture's) looks exactly like a search that finds nothing, and only the numbers tell a wrong slot
// apart from a scene with no occlusion.
inline std::string FormatRun(const Params& params, const ReaderFacts& facts, const Inputs& inputs, bool haveGpuMilliseconds,
                             double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the pass {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Ambient occlusion: {}x{} at {} steps a direction, radius {:.2f} m, strength {:.2f}, blend {:.2f}, depth {}x{} at {}, "
                       "normal {}x{} at {}, camera {} at {}{}",
                       facts.occlusionWidth, facts.occlusionHeight, Steps(params.quality), double(params.radius),
                       double(params.strength), double(params.blend), facts.depthWidth, facts.depthHeight, FormatSlot(inputs.depth),
                       facts.normalWidth, facts.normalHeight, FormatSlot(inputs.normal), facts.cameraBytes, FormatSlot(inputs.camera), gpu);
}

// The line that says how many reader draws of one frame bound this module's own texture. A session compares it with the number of occlusion
// bindings the frame makes: a draw whose shader's reflection was stripped declares nothing, keeps the engine's texture and is not counted here,
// so the two numbers differ by exactly the readers this module cannot name.
inline std::string FormatServed(uint32_t served)
{
    return std::format("Ambient occlusion: {} reader draws of the last frame bound this module's texture", served);
}

inline std::string FormatPaused()
{
    // The interval is stated in frames, and the seconds that comes to at the measured session's frame rate: a session reads its own frame rate
    // in the line above it, so a time alone would be a guess.
    return std::format("Ambient occlusion: {} frames in a row were left alone, so the module pauses and looks at a frame's readers again {} frames "
                       "later (about 2.7 seconds at 45 frames a second)",
                       kDeclineLimit, kPauseFrames);
}

// ---- the shader's constant block ----------------------------------------------------------------------------------------------
//
// The pass hands its scalars in one buffer, and tests/ext/ffxiv/engine checks the layout against the shader's. A block of five float4s:
// the render window and the mapping, both texture sizes, the tuning, and the region of the occlusion target this module writes.
struct Constants
{
    float window[4] {};   // xy = the render window's origin in depth texels, zw = its size in depth texels
    float mapping[4] {};  // xy = depth texels per occlusion texel, zw = the occlusion target's size
    float sizes[4] {};    // xy = the depth texture's size, zw = the steps a direction, w = the radius in metres
    float tuning[4] {};   // x = strength, y = blend, z = the horizon's cosine bias, w = unused
    float region[4] {};   // xy = the occlusion texel this module computes from, zw = how many texels of it
};

inline constexpr uint32_t kHorizonBiasBits = 0; // documented in the shader; the host test reads the same value from the rules
inline constexpr float kHorizonBias = 0.02f;    // cosine units: a tap this close to the tangent plane is the pixel's own surface

constexpr Constants ConstantsOf(const ReaderFacts& facts, const Params& params) noexcept
{
    Constants constants;
    const Mapping mapping = MappingOf(facts);
    constants.window[0] = facts.window[0];
    constants.window[1] = facts.window[1];
    constants.window[2] = facts.window[2];
    constants.window[3] = facts.window[3];
    constants.mapping[0] = mapping.scaleX;
    constants.mapping[1] = mapping.scaleY;
    constants.mapping[2] = float(facts.occlusionWidth);
    constants.mapping[3] = float(facts.occlusionHeight);
    constants.sizes[0] = float(facts.depthWidth);
    constants.sizes[1] = float(facts.depthHeight);
    constants.sizes[2] = float(Steps(params.quality));
    constants.sizes[3] = params.radius;
    constants.tuning[0] = params.strength;
    constants.tuning[1] = params.blend;
    constants.tuning[2] = kHorizonBias;
    const Region region = RegionOf(facts);
    constants.region[0] = float(region.x);
    constants.region[1] = float(region.y);
    constants.region[2] = float(region.width);
    constants.region[3] = float(region.height);
    return constants;
}

static_assert(sizeof(Constants) == 80, "the pass's constant block is five float4s, the same as the shader's");
} // namespace ext::ffxiv::engine::ao
