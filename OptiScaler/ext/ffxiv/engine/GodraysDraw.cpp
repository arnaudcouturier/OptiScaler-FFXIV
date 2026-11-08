#include "pch.h"

#include "ext/ffxiv/engine/Godrays.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h" // the reflection's plain rows: the fog post's profile slot, for the stamp
#include "ext/ffxiv/engine/ChainHooks.h"   // the chain's stage hooks, which this module registers
#include "ext/ffxiv/engine/EnginePass.h"   // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/FogSun.h" // the fog sun published for the night curve; the capture's arming flag
#include "ext/ffxiv/engine/GodraysPass.h"
#include "ext/ffxiv/engine/GodraysSun.h"
#include "ext/ffxiv/engine/RecogRules.h" // FrameStep: one count per rendered frame, the step line, the pixel-to-NDC mapping
#include "ext/ffxiv/engine/Gi.h" // the stage after this march: the chain's next hooks the march's own answers compose into
#include "ext/hdr/scene/FfxivSceneHdr.h" // the scene HDR path replaces the tone chain this march draws before
#include "ext/present/PresentSeams.h"    // the output-frame counter, the scene-HDR sample's token
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame

#include <d3d11.h>
#include <wrl/client.h>

#include <thread>

// God rays: the volumetric march before the tone map: the draw path, the frame's captures and the module's session (Godrays.h). The pure
// decisions are in GodraysRules.h, the frame's captures in GodraysSun.cpp, the three draws in GodraysPass.cpp, the shaders in
// shaders/Godrays.hlsl, the card in GodraysMenu.cpp.
//
// What it costs while it is off: the engine module's one call per draw into OnSeamDraw, which returns at two bit tests - no state query,
// no allocation and no log line. While it is on, per frame: the assembly draws are measured until the atlas and its four cascades are
// secured, the fog post's draw is measured once, and at the tone map the newest copy the rings still hold of the frames before it is mapped
// without ever waiting for the GPU; the march runs its three draws and the chain binds this module's copy of the scene at t0 for that one
// draw (the owner role) or takes this module's output as its own (the chained role).
//
// Nothing of the game is written: the scene is copied into an owned texture, the atlas and the sky and the LUT are read through views this
// module owns (or owned copies when the bind flag refuses), and the tone map's own binding is put back after its one draw, also when that
// draw throws.
//
// The frame's count opens at the frame's first composite after the previous frame closed at its tone map (RecogRules.h FrameStep; the
// game draws the composite twice a frame), which also resets the sun draw's taken flag and the capture budget: the composite is the one
// draw a rendered frame reliably makes before its tone map, and every capture the march makes stamps the count it was made in, so the
// tone map's reads take the copies of the frame before the one that closed - the radial's own ring ages, one boundary earlier because
// the march's tone map can be drawn by another stage of the chain. The fog post's own copies are stamped after the composite's advance, so
// the fog state the march reads is one rendered frame old - the fog moves over seconds, not frames, and the hold's fade covers the seam.
//
// The stamping contract: the chain's shared state - the composite's capture stamp the puddle stage keeps - is file-local to that stage's
// translation unit and unreachable from here. This module's own answered-tracking is its own rendered-frame counter (marchFrame), and a
// decline writes nothing but this module's own counters and log.
namespace ext::ffxiv::godrays
{
namespace
{
using Microsoft::WRL::ComPtr;
using ext::ffxiv::engine::Knob;
using ext::ffxiv::engine::Session;
using ext::ffxiv::engine::kConsecutiveDeclineLimit;

// The render targets a draw is measured at: D3D11's own maximum.
inline constexpr uint32_t kMaxRenderTargets = 8;

// The tone map's own parameter buffer (`cCommonTexParam`) sits in its pixel shader's first constant slot, the radial's own slot for the
// same buffer.
inline constexpr uint32_t kToneMapSlot = 0;

// The light draws the march measures per frame, bounded like the radial's own capture: the assembly draws and the sun's own draw among
// them, and the march's atlas capture is done after a handful.
inline constexpr uint32_t kMaxCaptureAttempts = 24;

// A read that has not landed for this long is not a GPU that is behind any more: one line says so, and the frames are skipped (the radial's
// own staleness rule, the march's own instance).
inline constexpr uint64_t kMaxReadbackAge = 90;
inline constexpr uint32_t kReadbackStallFrames = 64;

// One accepted frame state: the light, the camera, the fog constants and the cascades ride one accept, and the march draws with the last
// accepted state when this frame's own read has not landed (the radial's own hold, one unit up).
struct MarchHold
{
    bool have = false;
    shafts::LightFacts facts;
    float world[3] {};            // the accepted light's direction as the world sees it (the radial's WorldDirectionOf)
    float inverseProjection[16] {};
    float inverseViewRow1[4] {};
    float projection[16] {};
    float view[shafts::kViewRows] {};
    bool haveView = false;
    FogState fog;
    Cascade cascades[kCascadeCount] {};
    uint32_t cascadeCount = 0;
    float cameraWorldY = 0.0f;
    float invRhoCamera = 0.0f;
    float marchFar = 0.0f;
    float discScale = 0.0f;
    LightJudgement judgement; // the accepted light's class: its luminance gain scales the composite
    uint64_t frame = 0;

    // One accept, from one frame's landed reads: the light and the camera are one copy, the fog state and the cascades are the frame's
    // own. A partial landing updates the hold's own piece instead (Readback below), because a fog change on a frame whose light read
    // missed still reaches the march.
    void Accept(const FrameRead& frameRead, const FogRead& fogRead, const CascadeRead& cascadeRead, uint64_t at)
    {
        have = true;
        facts = frameRead.facts;
        std::memcpy(inverseProjection, frameRead.inverseProjection, sizeof inverseProjection);
        std::memcpy(inverseViewRow1, frameRead.inverseViewRow1, sizeof inverseViewRow1);
        std::memcpy(projection, frameRead.projection, sizeof projection);
        std::memcpy(view, frameRead.view, sizeof view);
        haveView = frameRead.haveView;
        fog = fogRead.fog;
        SetCascades(cascadeRead);
        Refresh(at);
    }

    void SetCascades(const CascadeRead& cascadeRead)
    {
        cascadeCount = cascadeRead.count;
        for (uint32_t i = 0; i < cascadeRead.count && i < kCascadeCount; ++i)
            cascades[i] = cascadeRead.cascades[i];
    }

    // The values the fog and the cascades decide: the camera's own world height, the term's normalization, the march's far bound and the
    // disc scale, all recomputed whenever a piece of the state moved.
    void Refresh(uint64_t at)
    {
        cameraWorldY = CameraWorldY(inverseViewRow1);
        const float rhoCamera = FogDensityAt(fog, cameraWorldY, 1.0f);
        invRhoCamera = rhoCamera > 1.0e-8f ? 1.0f / rhoCamera : 1.0f / 1.0e-8f;
        float fars[kCascadeCount] {};
        for (uint32_t i = 0; i < cascadeCount; ++i)
            fars[i] = cascades[i].distanceFar;
        marchFar = MarchFarOf(fars, cascadeCount);
        discScale = 0.0f;
        for (uint32_t i = 0; i < cascadeCount && !(discScale > 0.0f); ++i)
            discScale = DiscScale(cascades[i].mapRowX);
        frame = at;
    }

    void Drop()
    {
        have = false;
        facts = shafts::LightFacts {};
        cascadeCount = 0;
        frame = 0;
    }

    uint32_t Age(uint64_t now) const noexcept { return have && now >= frame ? uint32_t(now - frame) : uint32_t(kHoldFrames + 1u); }
    float Weight(uint64_t now) const noexcept { return have ? HoldFade(Age(now)) : 0.0f; }
};

struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    bool enabled = false;
    FrameReader frame;       // the light's and the camera's bytes, read a ring of frames late
    FogReader fog;           // the fog post's constants, the height profile and the viewport buffer
    CascadeReader cascades;  // the four per-cascade shadow parameter instances, pointer-keyed per frame
    shafts::ToneReader tone; // the tone map's own exposure multiplier, read a frame late: the frame's white point
    StolenViews views;       // the secured atlas, sky, LUT and depth views
    StatsReader stats;       // the stats texture's read-back: the term's peak and the sky ceilings, a run or two late, never waited for
    StatsRead lastStats;     // the newest stats read that landed
    StageMeasure lastRun;    // the last COMPLETED run's numbers, for the stage dump's values string (read when the capture window opens: one run old)
    LightJudgement judgement; // the newest light draw's verdict (class, axis agreement, nearest pixel ray)
    bool haveJudgement = false;
    bool haveSunPublished = false; // the fog sun's newest published copy, so a copy the GPU returns twice is published once
    uint64_t sunPublishedFrame = 0;
    uint32_t sunOnlySequence = 0;  // the fog-sun-only capture's own clock: one count per fog post draw
    bool fogKeyedBySequence = false; // the fog reader's ring is keyed by that clock (march off) rather than by the rendered-frame count
    bool loggedSunOnly = false;
    bool loggedSlices = false;
    bool sunDrawTaken = false; // the frame's sun draw is taken (reset at the frame's composite, the march's frame boundary)
    const void* rejectedLight = nullptr; // a light the read-back showed at a world position: a lamp, never the sun
    uint64_t rejectedFrame = 0;
    uint32_t attempts = 0;
    uint64_t frames = 0; // the module's own count of rendered frames, opened at the frame's composite (FrameStep)
    shafts::FrameStep step;
    shafts::ChangeLine stepLine;    // the step line's own once-per-state memory
    shafts::ChangeLine missingLine; // the wait line's (which read-back the first accept lacks), its own memory
    // The ages the last read-back took at the tone map, in counts (kRingNotLanded when a ring landed nothing): what the step line names.
    uint32_t frameAge = shafts::kRingNotLanded;
    uint32_t fogAge = shafts::kRingNotLanded;
    uint32_t cascadeAge = shafts::kRingNotLanded;
    // Which read-backs have landed at least once since the module was switched on: the first accept needs all of them, and the wait line
    // names the first it lacks.
    bool seenLight = false, seenCascades = false, seenFog = false, seenFogUsable = false;
    // The frame's lit scene: the deferred composite's single render target, which the tone map then reads as its t0.
    void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;
    // The first draw of a frame with the tone map's shape is the one this module looks at, and the march answers at most one tone map a
    // frame: this module's own rendered-frame counter, never the chain's shared stamp (the stamping contract).
    uint64_t marchFrame = 0;
    bool haveMarchFrame = false;
    // What the card shows and what the log says.
    uint32_t runs = 0;
    uint32_t heldRuns = 0;
    uint32_t declined = 0;
    CardCounts skips;
    uint32_t refusals = 0; // the camera refusals (a build mismatch), counted toward the stand-down
    MarchHold hold;        // the last accepted frame state, held across gaps with the radial's own fade
    float lastCamera[shafts::kProjectionRows] {}; // the newest camera a read landed: the view-projection a camera cut reads
    bool haveLastCamera = false;
    bool lastReadCameraRefused = false; // the last read's camera bytes arrived and are not this build's camera (a build mismatch)
    float whitePoint = shafts::kFallbackWhitePoint;
    bool haveWhitePoint = false;
    uint64_t whiteFrame = 0;
    uint64_t nextCountsLog = 0;
    bool loggedOff = false;
    bool loggedCost = false;
    bool loggedRejected = false;
    bool loggedCameraRefused = false;
    bool loggedStale = false;
    bool loggedStall = false;
    bool loggedCameraCut = false;
    bool loggedAtlas = false;
    bool loggedSky = false;
    bool loggedLut = false;
    bool loggedDepth = false;
    bool loggedFogUnusable = false;
    bool loggedViewportRow = false;
    bool loggedNoProfile = false;
    bool loggedProfileRefused = false;
    bool loggedCascadesUnusable = false;
    SceneHdrWatch hdr;
    uint64_t hdrSampleFrame = 0;
    bool haveHdrSample = false;
    std::thread::id renderThread;
    bool haveRenderThread = false;
    uint32_t lastWindowWidth = 0, lastWindowHeight = 0;
    uint32_t lastMarchWidth = 0, lastMarchHeight = 0;
    uint32_t lastProfileSlot = kNoProfileSlot; // the slot the last fog post draw took the height profile from
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The frame's composite: the one draw a rendered frame reliably makes before its tone map, and the march's frame boundary. The count
// opens here (once a frame), the sun draw's taken flag and the capture budget reset with it, and the scene the composite writes is
// recorded - the lit scene the tone map reads, which is what makes the scene recognisable without a shader CRC or an address.
void RecordScene(ID3D11DeviceContext* context, State& state)
{
    // The count opens once a frame, at the first composite after the previous frame's tone map: the captured frame draws the composite
    // twice in a row, and a count that moved at both draws stepped by two a frame, which pushed the previous frame's light, camera and
    // cascade copies out of the ring's read window. The capture budget and the sun draw's taken flag are tied to the real advance.
    if (state.step.OpenAtComposite(state.frames))
    {
        state.attempts = 0;
        state.sunDrawTaken = false;
    }
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && shafts::IsSceneFormat(uint32_t(viewDesc.Format)))
            {
                state.sceneResource = texture.Get();
                state.sceneWidth = desc.Width;
                state.sceneHeight = desc.Height;
                state.sceneFormat = uint32_t(viewDesc.Format);
                state.sceneFrames = state.frames;
            }
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }
}

// The sun draw's own shape, measured at the draw: the radial's own facts, with the march's own scene sizes.
shafts::SunDrawFacts MeasureSunDraw(ID3D11DeviceContext* context, const State& state)
{
    shafts::SunDrawFacts shape;
    ID3D11RenderTargetView* targets[kMaxRenderTargets] {};
    context->OMGetRenderTargets(kMaxRenderTargets, targets, nullptr);
    uint32_t count = 0;
    bool same = true;
    for (ID3D11RenderTargetView* target : targets)
    {
        if (target == nullptr)
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
        target->GetDesc(&viewDesc);
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC desc {};
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
            texture->GetDesc(&desc);
        if (count == 0)
        {
            shape.targetFormat = uint32_t(viewDesc.Format);
            shape.targetWidth = desc.Width;
            shape.targetHeight = desc.Height;
        }
        else if (uint32_t(viewDesc.Format) != shape.targetFormat || desc.Width != shape.targetWidth || desc.Height != shape.targetHeight)
        {
            same = false;
        }
        ++count;
    }
    for (ID3D11RenderTargetView*& target : targets)
        if (target != nullptr)
        {
            target->Release();
            target = nullptr;
        }
    shape.targets = count;
    shape.sameTargets = count != 0 && same;
    ID3D11BlendState* blend = nullptr;
    context->OMGetBlendState(&blend, nullptr, nullptr);
    shape.blended = false;
    if (blend != nullptr)
    {
        D3D11_BLEND_DESC desc {};
        blend->GetDesc(&desc);
        if (desc.IndependentBlendEnable)
        {
            for (const D3D11_RENDER_TARGET_BLEND_DESC& target : desc.RenderTarget)
                shape.blended = shape.blended || target.BlendEnable;
        }
        else
        {
            shape.blended = desc.RenderTarget[0].BlendEnable;
        }
        blend->Release();
    }
    shape.sceneWidth = state.sceneWidth;
    shape.sceneHeight = state.sceneHeight;
    // The depth and camera facts are answered by the caller's own reads: the shape rule needs them true only after this function said the
    // draw is the sun's, and the caller refuses a draw with neither.
    shape.depth = true;
    shape.camera = true;
    return shape;
}

// The light draws: the frame's sun draw (the light and the camera the march phases and projects with) and the assembly draws (the four
// per-cascade shadow parameter instances and the atlas itself). Both captures are bounded: the atlas's is done once it is secured, and the
// sun's after the first draw with the sun draw's shape.
void CaptureLightDraw(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    const bool sunNeeded = !state.sunDrawTaken;
    const bool atlasNeeded = state.views.AtlasView() == nullptr;
    if ((!sunNeeded && !atlasNeeded) || state.attempts >= kMaxCaptureAttempts)
        return;
    ++state.attempts;
    const uint32_t lightSlot = ext::ffxiv::engine::ShaftsSlotOf(anchors);
    if (lightSlot >= ext::ffxiv::engine::kConstantBufferSlots)
        return;

    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ID3D11ShaderResourceView* textureViews[16] {};
    context->PSGetShaderResources(0, 16, textureViews);
    // The seam's read add-refs every object it returns: taking them into ComPtrs owns exactly those references, so every early return
    // below gives them back.
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11ShaderResourceView> textures[16];
    for (uint32_t i = 0; i < 16; ++i)
        if (textureViews[i] != nullptr)
            textures[i].Attach(textureViews[i]);

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    // The atlas: the SRV whose resource is a single-sample 2D plane the engine itself samples as R16_UNORM. The assembly draws bind it,
    // and the march's own view of it is secured once (an own SRV after the bind-flag check, else an owned copy).
    if (atlasNeeded && device)
    {
        for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
        {
            if (!texture)
                continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            texture->GetDesc(&viewDesc);
            if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || uint32_t(viewDesc.Format) != kAtlasSampledFormat)
                continue;
            ComPtr<ID3D11Resource> resource;
            texture->GetResource(&resource);
            ComPtr<ID3D11Texture2D> atlasTexture;
            if (resource == nullptr || FAILED(resource.As(&atlasTexture)) || atlasTexture == nullptr)
                continue;
            D3D11_TEXTURE2D_DESC desc {};
            atlasTexture->GetDesc(&desc);
            if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Width < 256 || desc.Height < 1024)
                continue;
            bool ownView = false;
            uint32_t width = 0, height = 0, format = 0;
            if (state.views.SecureAtlas(context, texture.Get(), ownView, width, height, format) != nullptr)
            {
                if (!state.loggedAtlas)
                {
                    state.loggedAtlas = true;
                    LOG_INFO("{}", FormatSecured("cascade shadow atlas", ownView, width, height, format));
                }
                break;
            }
        }
    }

    // The four per-cascade shadow parameter instances: the draw's 128-byte parameter buffers, pointer-keyed per frame. A draw binds one of
    // the four; across the frame's assembly draws all four land here, and a buffer already copied this frame costs nothing.
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer)
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth != kCascadeBytes)
            continue;
        state.cascades.Copy(context, buffer.Get(), state.frames);
    }

    if (!sunNeeded)
        return;

    // The sun draw's own shape, the radial's own rule: the light-buffer pair, written without blending, beside the frame's depth and the
    // camera. The march phases and projects with the light it reads here; the visible disc's UV is the radial's business.
    ComPtr<ID3D11Buffer> light = bound[lightSlot];
    if (light == nullptr)
        return;
    if (static_cast<const void*>(light.Get()) == state.rejectedLight &&
        shafts::RejectedStill(state.rejectedLight != nullptr, state.rejectedFrame, state.frames))
        return;
    ComPtr<ID3D11Buffer> camera;
    uint32_t cameraBytes = 0;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == light.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth < shafts::kCameraBytes || desc.ByteWidth > 0x400 || desc.ByteWidth <= cameraBytes)
            continue;
        cameraBytes = desc.ByteWidth;
        camera = buffer;
    }
    if (camera == nullptr)
        return;
    if (!shafts::IsSunDraw(MeasureSunDraw(context, state)))
        return;
    if (!state.frame.Copy(context, light.Get(), camera.Get(), state.frames))
        return;
    state.sunDrawTaken = true;
}

// ---- the fog post shader's profile-slot stamp ---------------------------------------------------------------------------------

// The one private-data tag this module writes on a game object: the constant-buffer slot the fog post's own reflection names
// cExpHeightFogParam at, stored as PackProfileSlot's word. The other pass families' stamps carry their own GUIDs.
constexpr GUID kProfileStampTag { 0x3b9a41d7, 0x5c20, 0x4e6b, { 0x9d, 0x1f, 0x72, 0xa8, 0x06, 0xc3, 0x4b, 0xe9 } };

// A byte scan for the profile's name before any parse: the creation hook sees every shader that names one of the engine pass's buffers, and
// only the fog post names this one.
bool MentionsProfile(const void* code, size_t size)
{
    constexpr char kName[] = "cExpHeightFogParam";
    constexpr size_t kLength = sizeof kName - 1;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i + kLength <= size; ++i)
        if (bytes[i] == static_cast<unsigned char>(kName[0]) && std::memcmp(bytes + i, kName, kLength) == 0)
            return true;
    return false;
}

// The slot the shader declares cExpHeightFogParam at, from the binding table a bytecode parse produced. Only the fog post declares both
// the fog constants and the profile, so a shader without both takes no stamp.
uint32_t ProfileSlotOfShader(const void* code, size_t size)
{
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    const uint32_t fog = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "cFogParam");
    const uint32_t profile = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "cExpHeightFogParam");
    if (fog >= ext::ffxiv::engine::kConstantBufferSlots || profile >= ext::ffxiv::engine::kConstantBufferSlots)
        return kNoProfileSlot;
    return profile;
}

bool StampShader(ID3D11DeviceChild* shader, uint64_t packed)
{
    if (shader == nullptr || packed == 0)
        return false; // 0 is "nothing stamped": writing it would only make an unstamped shader look stamped
    return SUCCEEDED(shader->SetPrivateData(kProfileStampTag, sizeof packed, &packed));
}

uint64_t ReadStampRaw(ID3D11DeviceChild* shader)
{
    if (shader == nullptr)
        return 0;
    uint64_t packed = 0;
    UINT bytes = sizeof packed;
    if (FAILED(shader->GetPrivateData(kProfileStampTag, &bytes, &packed)) || bytes != sizeof packed)
        return 0;
    return packed;
}

// The stamp of the pixel shader bound right now (the fog post, at its draw): the profile's slot, or kNoProfileSlot.
uint32_t BoundProfileSlot(ID3D11DeviceContext* context)
{
    ID3D11PixelShader* bound = nullptr;
    context->PSGetShader(&bound, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> shader;
    if (bound != nullptr)
        shader.Attach(bound);
    return UnpackProfileSlot(ReadStampRaw(shader.Get()));
}

// The engine's fog post: the fog constants, the height profile, the viewport buffer, the frame's depth and the sky/LUT views, all captured
// at the one draw a frame that binds cFogParam makes. The depth is the fog post's own t0 - the same scene depth the march bounds its rays
// with - and the sky and LUT views are secured for the enginefog colour.
void CaptureFogPost(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    const uint32_t fogSlot = ext::ffxiv::engine::FogPostSlotOf(anchors);
    if (fogSlot >= ext::ffxiv::engine::kConstantBufferSlots)
        return;

    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ID3D11ShaderResourceView* textureViews[8] {};
    context->PSGetShaderResources(0, 8, textureViews);
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11ShaderResourceView> textures[8];
    for (uint32_t i = 0; i < 8; ++i)
        if (textureViews[i] != nullptr)
            textures[i].Attach(textureViews[i]);

    ComPtr<ID3D11Buffer> fogParam = bound[fogSlot];
    if (fogParam == nullptr)
        return;
    D3D11_BUFFER_DESC fogDesc {};
    fogParam->GetDesc(&fogDesc);
    if (fogDesc.ByteWidth != kFogParamBytes)
        return; // a buffer of another size under that name is another draw's parameters

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    // The height profile: the buffer bound at the slot the fog shader names cExpHeightFogParam at (the stamp its reflection gave the shader
    // object at creation), copied whole. The other 64-byte buffer the post binds is a clip-to-world matrix; no shape rule tells the two
    // apart, so the slot decides and a draw without it has no profile.
    uint32_t boundBytes[ext::ffxiv::engine::kConstantBufferSlots] {};
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
    {
        if (!bound[i])
            continue;
        D3D11_BUFFER_DESC desc {};
        bound[i]->GetDesc(&desc);
        boundBytes[i] = desc.ByteWidth;
    }
    const uint32_t stamped = BoundProfileSlot(context);
    const uint32_t profileSlot = ProfileSlotFor(stamped, boundBytes);
    state.lastProfileSlot = profileSlot;
    ID3D11Buffer* profile = profileSlot != kNoProfileSlot ? bound[profileSlot].Get() : nullptr;
    if (profile == nullptr && !state.loggedNoProfile)
    {
        state.loggedNoProfile = true;
        if (stamped == kNoProfileSlot)
            LOG_INFO("{}", kNoProfileStamp);
        else
            LOG_INFO("{}", FormatProfileSlotEmpty(stamped, boundBytes[stamped]));
    }
    // The viewport buffer: the 32-byte buffer the fog post binds beside the fog constants (cDynamicViewportResolutionParam). Its second row
    // is copied for one log line only; the march maps pixels to NDC from the render window.
    ID3D11Buffer* viewportParam = nullptr;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == fogParam.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth == kViewportParamBytes)
        {
            viewportParam = buffer.Get();
            break;
        }
    }
    state.fog.Copy(context, fogParam.Get(), profile, viewportParam, state.frames);

    // The frame's depth: the fog post's own t0, the same scene depth the march bounds its rays with. A depth of another size than the
    // scene the composite wrote is another draw's binding, and the march keeps the last one it kept.
    for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
    {
        if (!texture)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        texture->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !shafts::IsDepthFormat(uint32_t(viewDesc.Format)))
            continue;
        ComPtr<ID3D11Resource> resource;
        texture->GetResource(&resource);
        ComPtr<ID3D11Texture2D> depthTexture;
        if (resource == nullptr || FAILED(resource.As(&depthTexture)) || depthTexture == nullptr)
            continue;
        D3D11_TEXTURE2D_DESC desc {};
        depthTexture->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
            continue;
        if (state.sceneWidth != 0 && (desc.Width != state.sceneWidth || desc.Height != state.sceneHeight))
            continue;
        state.views.KeepDepth(texture.Get(), uint32_t(viewDesc.Format), desc.Width, desc.Height);
        if (!state.loggedDepth)
        {
            state.loggedDepth = true;
            LOG_INFO("{}", FormatSecured("scene depth", false, desc.Width, desc.Height, uint32_t(viewDesc.Format)));
        }
        break;
    }

    // The sky (FP16, half the render window) and the weight LUT (R16G16, 256x1): the enginefog colour's two views, secured once each.
    if (device)
    {
        for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
        {
            if (!texture)
                continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            texture->GetDesc(&viewDesc);
            if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || uint32_t(viewDesc.Format) != shafts::kFp16Format)
                continue;
            ComPtr<ID3D11Resource> resource;
            texture->GetResource(&resource);
            ComPtr<ID3D11Texture2D> skyTexture;
            if (resource == nullptr || FAILED(resource.As(&skyTexture)) || skyTexture == nullptr)
                continue;
            D3D11_TEXTURE2D_DESC desc {};
            skyTexture->GetDesc(&desc);
            if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.Width < 64 || desc.Height < 64)
                continue;
            if (state.sceneWidth != 0 && desc.Width >= state.sceneWidth && desc.Height >= state.sceneHeight)
                continue; // the scene itself is not the sky
            bool ownView = false;
            uint32_t width = 0, height = 0, format = 0;
            if (state.views.SecureSky(context, texture.Get(), ownView, width, height, format) != nullptr && !state.loggedSky)
            {
                state.loggedSky = true;
                LOG_INFO("{}", FormatSecured("engine sky", ownView, width, height, format));
            }
            break;
        }
        for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
        {
            if (!texture)
                continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            texture->GetDesc(&viewDesc);
            if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || uint32_t(viewDesc.Format) != kR16G16Format)
                continue;
            ComPtr<ID3D11Resource> resource;
            texture->GetResource(&resource);
            ComPtr<ID3D11Texture2D> lutTexture;
            if (resource == nullptr || FAILED(resource.As(&lutTexture)) || lutTexture == nullptr)
                continue;
            D3D11_TEXTURE2D_DESC desc {};
            lutTexture->GetDesc(&desc);
            if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.Height != 1 || desc.Width < 64)
                continue;
            bool ownView = false;
            uint32_t width = 0, height = 0, format = 0;
            if (state.views.SecureLut(context, texture.Get(), ownView, width, height, format) != nullptr && !state.loggedLut)
            {
                state.loggedLut = true;
                LOG_INFO("{}", FormatSecured("fog weight LUT", ownView, width, height, format));
            }
            break;
        }
    }
}

// The fog post's sun row, copied for the night curve alone (the march is off): the 128-byte cFogParam through the same reader as the march's capture
// (no profile, no viewport buffer), read at the NEXT fog post before the new copy goes in, keyed by this capture's own clock (one count per fog post
// draw: the rendered-frame counter only moves while the march is armed). The read publishes the sun's y whether or not a profile exists.
void CaptureFogSunOnly(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    const uint32_t fogSlot = ext::ffxiv::engine::FogPostSlotOf(anchors);
    if (fogSlot >= ext::ffxiv::engine::kConstantBufferSlots)
        return;
    if (!state.fogKeyedBySequence)
    {
        state.fog.Reset(); // the ring was keyed by the rendered-frame count: its slots mean nothing under this clock
        state.fogKeyedBySequence = true;
        state.haveSunPublished = false;
    }
    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11Buffer> fogParam = bound[fogSlot];
    if (fogParam == nullptr)
        return;
    D3D11_BUFFER_DESC fogDesc {};
    fogParam->GetDesc(&fogDesc);
    if (fogDesc.ByteWidth != kFogParamBytes)
        return; // a buffer of another size under that name is another draw's parameters
    ++state.sunOnlySequence;
    const FogRead read = state.fog.Read(context, state.sunOnlySequence);
    if (read.landed && read.sunLanded && (!state.haveSunPublished || read.writtenFrame != state.sunPublishedFrame))
    {
        fogsun::Publish(read.sun[1]);
        state.haveSunPublished = true;
        state.sunPublishedFrame = read.writtenFrame;
    }
    state.fog.Copy(context, fogParam.Get(), nullptr, nullptr, state.sunOnlySequence);
    if (!state.loggedSunOnly)
    {
        state.loggedSunOnly = true;
        LOG_INFO("{}", kFogSunOnlyLine);
    }
}

// The frame's white point, from the tone map's own parameters at this draw (the radial's own read, the march's own instance).
void ReadToneMapParams(ID3D11DeviceContext* context, State& state)
{
    ID3D11Buffer* buffer = nullptr;
    context->PSGetConstantBuffers(kToneMapSlot, 1, &buffer);
    ComPtr<ID3D11Buffer> params;
    if (buffer != nullptr)
        params.Attach(buffer);
    if (params)
        state.tone.Copy(context, params.Get(), state.frames);
    const shafts::ToneRead read = state.tone.Read(context, state.frames);
    if (read.landed && shafts::ExposureUsable(read.exposure))
    {
        const float whitePoint = shafts::WhitePointFromExposure(read.exposure);
        if (shafts::WhitePointUsable(whitePoint))
        {
            state.whitePoint = whitePoint;
            state.whiteFrame = state.frames;
            state.haveWhitePoint = true;
            return;
        }
    }
    if (shafts::WhitePointStale(state.haveWhitePoint, state.whiteFrame, state.frames))
    {
        state.haveWhitePoint = false;
        state.whitePoint = shafts::kFallbackWhitePoint;
    }
}

// The slot the last fog post draw's profile was taken from, for the refusal line.
uint32_t BoundProfileSlotMemo(const State& state) { return state.lastProfileSlot; }

// The first accept has not happened: the line names the first read-back the march lacks, and a march whose read-backs have each landed
// but never together says that. One line a state.
void NoteMissingInput(State& state)
{
    const MarchInput missing = FirstMissingInput(state.seenLight, state.seenCascades, state.seenFog, state.seenFogUsable);
    if (missing != MarchInput::None)
    {
        if (state.missingLine.Take(uint32_t(missing)))
            LOG_INFO("{}", MissingInputLine(missing));
    }
    else if (state.missingLine.Take(0xF0u))
    {
        LOG_INFO("{}", kReadbacksNotTogether);
    }
}

// The one line a live session needs to see the mechanism: how often the count moved for the frame that just closed, and the ages of the
// copies its read-backs took.
void NoteFrameStep(State& state)
{
    if (!state.step.HaveSample())
        return;
    const shafts::RingAge rings[] { { "light and camera copy", state.frameAge }, { "fog copy", state.fogAge }, { "cascade copies", state.cascadeAge } };
    const uint32_t advances = state.step.AdvancesPerFrame();
    const uint32_t composites = state.step.CompositesPerFrame();
    if (state.stepLine.Take(shafts::StepKey(advances, composites, rings, 3)))
        LOG_INFO("{}", shafts::FormatFrameStep("God rays", advances, composites, rings, 3));
}

// The frame's tone map closes the frame (RecogRules.h ClosesFrame): the next frame's first composite then opens the next count. It runs
// at the top of every entry that can see the tone map, before the march's one-answer gate and whatever the march then does with the
// draw, so a frame the march declines or skips still moves the count on.
void CloseFrameAtToneMap(State& state, const shafts::ToneMapFacts& facts, bool chained)
{
    if (shafts::ClosesFrame(facts, chained))
        state.step.CloseAtToneMap();
}

// What one frame's reads came to, and what the march draws with: the newest copy each ring still holds, the last accepted state held
// across gaps with the radial's own fade, and a camera cut dropping the hold (a state from before a cut does not belong to the new
// picture). False when the frame has no march state in it.
bool Readback(ID3D11DeviceContext* context, State& state, FrameState& marchState, float& fade, const shafts::ToneMapFacts& facts)
{
    const FrameRead frameRead = state.frame.Read(context, state.frames);
    const FogRead fogRead = state.fog.Read(context, state.frames);
    const CascadeRead cascadeRead = state.cascades.Read(context, state.frames);
    state.frameAge = frameRead.landed ? shafts::AgeOf(state.frames, frameRead.writtenFrame) : shafts::kRingNotLanded;
    state.fogAge = fogRead.landed ? shafts::AgeOf(state.frames, fogRead.writtenFrame) : shafts::kRingNotLanded;
    state.cascadeAge = cascadeRead.landed ? shafts::AgeOf(state.frames, cascadeRead.writtenFrame) : shafts::kRingNotLanded;
    // The fog sun goes to the night curve whether or not the march can use this fog state (the profile may be refused): a copy returned twice counts once.
    if (state.fogKeyedBySequence)
    {
        state.fog.Reset(); // the ring was keyed by the fog-sun-only capture's clock
        state.fogKeyedBySequence = false;
        state.haveSunPublished = false;
    }
    if (fogRead.landed && fogRead.sunLanded && (!state.haveSunPublished || fogRead.writtenFrame != state.sunPublishedFrame))
    {
        fogsun::Publish(fogRead.sun[1]);
        state.haveSunPublished = true;
        state.sunPublishedFrame = fogRead.writtenFrame;
    }
    state.seenLight = state.seenLight || (frameRead.landed && frameRead.camera);
    state.seenCascades = state.seenCascades || cascadeRead.landed;
    state.seenFog = state.seenFog || fogRead.landed;
    state.seenFogUsable = state.seenFogUsable || fogRead.usable;
    // The fog post's viewport buffer is not what the march's pixel-to-NDC mapping comes from (the fog post's bytecode reads only its first
    // row and hard-codes the mapping): its second row is logged once next to the mapping the march derives from the window, so a session
    // settles what the row holds.
    const shafts::NdcRow derived = shafts::PixelToNdcRow(facts.window);
    if (fogRead.viewportLanded && derived.ok && !state.loggedViewportRow)
    {
        state.loggedViewportRow = true;
        const float second[4] { fogRead.ndcScale[0], fogRead.ndcScale[1], fogRead.ndcOffset[0], fogRead.ndcOffset[1] };
        LOG_INFO("{}", shafts::FormatViewportRow("God rays", second, derived));
    }
    if (fogRead.landed && fogRead.profileRefused && !state.loggedProfileRefused)
    {
        state.loggedProfileRefused = true;
        LOG_INFO("{}", FormatProfileRefused(BoundProfileSlotMemo(state)));
    }
    if (cascadeRead.landed && cascadeRead.count == 0 && !state.loggedCascadesUnusable)
    {
        state.loggedCascadesUnusable = true;
        LOG_INFO("{}", kCascadesUnusable);
    }
    // The camera of any read that landed, before the light's verdict: a cut is seen even on a frame whose light was refused.
    state.lastReadCameraRefused = frameRead.landed && !frameRead.camera;
    if (frameRead.landed && frameRead.camera)
    {
        if (state.haveLastCamera && shafts::CameraCut(state.lastCamera, frameRead.projection))
        {
            state.hold.Drop();
            if (!state.loggedCameraCut)
            {
                state.loggedCameraCut = true;
                LOG_INFO("{}", kCameraCut);
            }
        }
        for (uint32_t i = 0; i < shafts::kProjectionRows; ++i)
            state.lastCamera[i] = frameRead.projection[i];
        state.haveLastCamera = true;
    }
    // The light's judgement: the draw's direction against the finest cascade's axis (3 degrees: it is the shadow light or it is not), its luminance (the
    // gain), the nearest pixel ray (a light behind or beside the camera casts no beams). One verdict function, one line per change.
    LightJudgement judged;
    bool haveJudged = false;
    if (frameRead.landed && frameRead.camera)
    {
        const Cascade* source = cascadeRead.landed ? cascadeRead.cascades : state.hold.cascades;
        uint32_t finestOrder[kCascadeCount] {};
        const uint32_t usable = CascadeOrder(source, finestOrder, kCascadeCount);
        const float elevationSine =
            frameRead.haveView ? shafts::WorldElevationOf(frameRead.view, frameRead.facts.direction) : frameRead.facts.direction[1];
        judged = JudgeLight(frameRead.facts.direction, frameRead.facts.diffuse, elevationSine, frameRead.inverseProjection,
                            usable > 0 ? source[finestOrder[0]].matrix : nullptr);
        haveJudged = true;
    }
    // The light's verdict, when the frame's own read landed: a lamp or a mirror is remembered the radial's way, and the hold is what the
    // frames after it draw with.
    bool lightIsSun = false;
    if (frameRead.landed)
    {
        const float elevation =
            frameRead.haveView ? shafts::WorldElevationOf(frameRead.view, frameRead.facts.direction) : frameRead.facts.direction[1];
        const shafts::LightVerdict verdict = shafts::ClassifyLight(frameRead.facts, elevation);
        if (verdict != shafts::LightVerdict::Sun)
        {
            if (verdict == shafts::LightVerdict::Lamp && !state.loggedRejected)
            {
                state.loggedRejected = true;
                LOG_INFO("{}", shafts::FormatNotTheSun(frameRead.facts));
            }
            if (verdict == shafts::LightVerdict::Lamp)
                state.rejectedLight = state.frame.LastLight();
            // A refused light does not empty the hold: the frame's light being a lamp says nothing about the state the last read accepted.
        }
        else
        {
            lightIsSun = true;
            // Only a sun is judged: a lamp the read-back refused above says nothing about the shadow light, and must not gate the march.
            if (haveJudged)
            {
                state.judgement = judged;
                state.haveJudgement = true;
                if (state.log.Take(0x30000000u | LightGateKey(judged)))
                    LOG_INFO("{}", FormatLightJudgement(judged));
            }
        }
        if (verdict == shafts::LightVerdict::Sun && state.hold.have)
        {
            float world[3] { 0.0f, 0.0f, 0.0f };
            if (frameRead.haveView && shafts::WorldDirectionOf(frameRead.view, frameRead.facts.direction, world))
            {
                state.hold.facts = frameRead.facts;
                for (uint32_t i = 0; i < 3; ++i)
                    state.hold.world[i] = world[i];
            }
        }
    }
    // One accept, or the pieces that landed update the hold's own pieces: a fog change on a frame whose light read missed still reaches
    // the march, and the hold's age moves with the newest piece.
    if (frameRead.landed && frameRead.camera && fogRead.landed && fogRead.usable && cascadeRead.landed && lightIsSun &&
        (!haveJudged || judged.gate == LightGate::Run))
    {
        state.hold.Accept(frameRead, fogRead, cascadeRead, state.frames);
        state.hold.judgement = judged;
    }
    else
    {
        bool moved = false;
        if (fogRead.landed && fogRead.usable)
        {
            state.hold.fog = fogRead.fog;
            moved = true;
        }
        if (cascadeRead.landed)
        {
            state.hold.SetCascades(cascadeRead);
            moved = true;
        }
        if (moved && state.hold.have)
            state.hold.Refresh(state.frames);
    }
    if (fogRead.landed && !fogRead.usable && !state.loggedFogUnusable)
    {
        state.loggedFogUnusable = true;
        LOG_INFO("God rays: the fog state this frame's fog post bound is not one the march can evaluate, so the last good one is held");
    }
    // The march's state: the hold when it is fresh enough, with the fade the radial's own weights give it.
    const uint32_t age = state.hold.Age(state.frames);
    fade = HoldFade(age);
    if (!state.hold.have || age > kHoldFrames || !(fade > 0.0f))
    {
        if (state.hold.have && age > kHoldFrames)
            state.hold.Drop();
        if (!state.hold.have)
            NoteMissingInput(state);
        return false;
    }
    std::memcpy(marchState.inverseProjection, state.hold.inverseProjection, sizeof marchState.inverseProjection);
    std::memcpy(marchState.inverseViewRow1, state.hold.inverseViewRow1, sizeof marchState.inverseViewRow1);
    std::memcpy(marchState.projection, state.hold.projection, sizeof marchState.projection);
    marchState.fog = state.hold.fog;
    marchState.cameraWorldY = state.hold.cameraWorldY;
    marchState.invRhoCamera = state.hold.invRhoCamera;
    marchState.marchFar = state.hold.marchFar;
    marchState.discScale = state.hold.discScale;
    // The pixel-to-NDC mapping, derived from the render window (the fog post's bytecode hard-codes it; see RecogRules.h).
    marchState.ndcScale[0] = derived.scale[0];
    marchState.ndcScale[1] = derived.scale[1];
    marchState.ndcOffset[0] = derived.offset[0];
    marchState.ndcOffset[1] = derived.offset[1];
    // The march's own order: the usable cascades, finest first (the rules' own order, applied to the hold's array).
    Cascade ordered[kCascadeCount] {};
    uint32_t order[kCascadeCount] {};
    const uint32_t count = CascadeOrder(state.hold.cascades, order, kCascadeCount);
    for (uint32_t i = 0; i < count; ++i)
        ordered[i] = state.hold.cascades[order[i]];
    // Only cascades whose own matrix and fade row name an atlas slice march: one that names none would read another cascade's slice.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (CascadeSlice(ordered[i].matrix, ordered[i].fade) != kNoSlice)
            marchState.cascades[kept++] = ordered[i];
    marchState.cascadeCount = kept;
    if (count > 0 && kept == 0 && !state.loggedSlices)
    {
        state.loggedSlices = true;
        LOG_INFO("God rays: none of the landed shadow cascades names an atlas slice (their matrix and fade rows put the middle of their band outside the atlas), so there is no shadow to march through");
    }
    // The march's light is the finest cascade's axis (-normalize of its matrix's third row): the shadows' own direction, which is the light draw's to
    // the cascade-agreement bound. The phase centres on it.
    float axis[3] { state.hold.facts.direction[0], state.hold.facts.direction[1], state.hold.facts.direction[2] };
    if (kept > 0)
        CascadeAxisOf(marchState.cascades[0].matrix, axis);
    marchState.sunDirection[0] = axis[0];
    marchState.sunDirection[1] = axis[1];
    marchState.sunDirection[2] = axis[2];
    return kept > 0;
}

// The tone map's own shape, measured at the draw (the radial's own measurement, the march's own instance).
shafts::ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, const State& state, ComPtr<ID3D11ShaderResourceView>& scene)
{
    shafts::ToneMapFacts facts;
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.renderTargets = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            facts.targetWidth = desc.Width;
            facts.targetHeight = desc.Height;
            facts.targetFormat = uint32_t(viewDesc.Format);
            facts.targetIsScene = texture.Get() == state.sceneResource;
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    ID3D11ShaderResourceView* first = nullptr;
    context->PSGetShaderResources(0, 1, &first);
    scene.Attach(first);
    if (scene != nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        scene->GetDesc(&viewDesc);
        facts.sceneSampled = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        facts.sceneFormat = uint32_t(viewDesc.Format);
        ComPtr<ID3D11Resource> resource;
        scene->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            facts.sceneWidth = desc.Width;
            facts.sceneHeight = desc.Height;
            facts.sceneIsScene = texture.Get() == state.sceneResource && state.sceneFrames == state.frames;
        }
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
    facts.replay = ext::ffxiv::chara::t_replay;
    return facts;
}

// The tint the march draws with, from the keys and the frame's own light (the radial's own rule, the march's own instance). The enginefog
// key's colour is the engine's own rule, which the composite computes per pixel from the secured sky and LUT, so the CPU hands it a
// neutral tint and the mode flag.
void ResolveTint(State& state, const FrameState& marchState, const shafts::ToneMapFacts& facts, Inputs& inputs)
{
    const shafts::Colour radialColour = state.params.colour == Colour::White ? shafts::Colour::White : shafts::Colour::Auto;
    const shafts::SunTint tint = shafts::ResolveSunTint(state.hold.facts.diffuse, radialColour);
    inputs.tintRgb[0] = tint.rgb[0];
    inputs.tintRgb[1] = tint.rgb[1];
    inputs.tintRgb[2] = tint.rgb[2];
    inputs.tintFromSky = tint.fromSky && state.params.colour == Colour::Auto;
    // The sun's screen position: the light's direction projected through the frame's own projection (the radial's own fallback rule; the
    // sky's own sun UV is the radial's business, and the march's colour search only needs a place where the sky is).
    const shafts::SunProjection projected = shafts::ProjectSun(marchState.projection, marchState.sunDirection, facts.window);
    if (projected.unit && !projected.behind)
    {
        inputs.sunScreenX = projected.screenX;
        inputs.sunScreenY = projected.screenY;
    }
}

// The stage after this march: this march's own answer composes on the GI stage's chained hook when it is installed, and the stage's answer is
// what t0 binds. A null from the hook is this march's own answer unchanged (the stage being off is today's picture).
void ComposeChainNext(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView*& ours)
{
    if (ext::ffxiv::engine::chainGiScene == nullptr || ours == nullptr)
        return; // the stage after this march is not linked, or this march has nothing to hand it: today's picture
    if (ID3D11ShaderResourceView* composed = ext::ffxiv::engine::chainGiScene(context, facts, ours))
        ours = composed;
}

// One run of the march, whichever role reached it: the pass on the scene it was handed, the counters and the one line a session gets.
ID3D11ShaderResourceView* RunMarch(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                   const FrameState& marchState, float fade, ID3D11ShaderResourceView* scene)
{
    Inputs inputs;
    inputs.scene = scene;
    inputs.depth = state.views.Depth();
    inputs.atlas = state.views.AtlasView();
    inputs.sky = state.params.colour == Colour::EngineFog ? state.views.SkyView() : nullptr;
    inputs.lut = state.params.colour == Colour::EngineFog ? state.views.LutView() : nullptr;
    inputs.sceneFormat = facts.sceneFormat;
    inputs.sceneWidth = facts.sceneWidth;
    inputs.sceneHeight = facts.sceneHeight;
    inputs.depthFormat = state.views.DepthFormat();
    inputs.depthWidth = state.views.DepthWidth();
    inputs.depthHeight = state.views.DepthHeight();
    inputs.window = facts.window;
    inputs.state = marchState;
    inputs.params = state.params;
    inputs.whitePoint = state.whitePoint;
    inputs.fade = fade;
    inputs.lightGain = state.hold.judgement.gain;
    ResolveTint(state, marchState, facts, inputs);

    ID3D11ShaderResourceView* ours = nullptr;
    {
        ext::ffxiv::chara::ReplayScope own;
        ext::ffxiv::engine::OwnWork ownWork;
        ours = state.pass.Run(context, inputs);
    }
    if (ours == nullptr)
    {
        const char* failure = state.pass.Failure();
        if (failure != nullptr && state.log.Take(0x20000001u))
            LOG_INFO("God rays: the march's pass did not run ({})", failure);
        return ours;
    }
    // The stats texture goes to its staging ring and the newest copy a frame or two old is read back without waiting (the term's peak, the sky ceilings).
    const StatsRead read = state.stats.Read(context, state.frames);
    if (read.landed)
        state.lastStats = read;
    state.stats.Copy(context, state.pass.StatsTexture(), state.frames);
    if (state.lastStats.landed && state.log.Take(0x40000000u | uint32_t(std::lround(std::log2(state.lastStats.termPeak + 1.0e-4f)) + 64) & 0xFFu))
        LOG_INFO("{}", FormatStatsLine(state.lastStats));
    // What this run measured, for the stage dump's values string (the string is read at the next capture window, so it is one run old).
    StageMeasure& measured = state.lastRun;
    measured.have = true;
    measured.termPeak = state.lastStats.termPeak;
    measured.whitePoint = inputs.whitePoint;
    measured.fade = inputs.fade;
    measured.sunScreen[0] = inputs.sunScreenX;
    measured.sunScreen[1] = inputs.sunScreenY;
    for (uint32_t i = 0; i < 3; ++i)
        measured.tint[i] = inputs.tintRgb[i];
    measured.luminance = state.hold.judgement.luminance;
    measured.elevationDegrees = state.hold.judgement.elevationDegrees;
    measured.nearestDegrees = state.hold.judgement.nearestDegrees;
    measured.gate = state.hold.judgement.gate;
    return ours;
}

// The card's numbers into the log about once a minute of rendered frames, from the frame's close (the radial's own cadence).
void LogCountsIfDue(State& state)
{
    if (state.frames < state.nextCountsLog)
        return;
    state.nextCountsLog = state.frames + kCountsLogFrames;
    CardNumbers numbers;
    numbers.intensity = state.params.intensity;
    numbers.frames = state.frames;
    numbers.runs = state.runs;
    numbers.held = state.heldRuns;
    numbers.declined = state.declined;
    numbers.skipped = state.skips.skipped;
    numbers.noAtlas = state.skips.noAtlas;
    numbers.noFog = state.skips.noFog;
    numbers.noLight = state.skips.noLight;
    numbers.refused = state.refusals;
    numbers.reads = state.frame.Reads();
    numbers.notReady = state.frame.NotReady();
    numbers.windowWidth = state.lastWindowWidth;
    numbers.windowHeight = state.lastWindowHeight;
    numbers.marchWidth = state.lastMarchWidth;
    numbers.marchHeight = state.lastMarchHeight;
    LOG_INFO("{}", FormatCard(numbers));
}

// One declined tone-map draw: the per-reason count, one line per change of reason with the outcome the chain's hold semantics name, the
// module's own refusal streak, and the game's own draw left exactly as the chain left it. Nothing here touches the chain's shared state -
// the composite's capture stamp is the puddle stage's own file-local state, and this module's counters are its own.
void NoteDecline(State& state, Decline reason, Outcome outcome)
{
    // A later tone-shaped draw of the frame (the engine's own post-tone copy) is not a decision about the capture frame: it never replaces the real reason.
    if (reason != Decline::NotToneMap)
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Godrays, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    state.declined++;
    if (CountsTowardsStandDown(reason))
    {
        if (reason == Decline::NoCamera)
            ++state.refusals;
        if (state.session.NoteDecline(Knob::Godrays) && !state.loggedOff)
        {
            state.loggedOff = true;
            LOG_WARN("{}", FormatSwitchedOff(kConsecutiveDeclineLimit));
            ext::SetActive(ext::F::GodraysVolumetric, false); // the draw path is back to two bit tests
        }
    }
    if (state.log.Take(uint32_t(reason) | (uint32_t(outcome) << 8)))
        LOG_INFO("{}", FormatDecline(reason, outcome));
}

// One frame left as it was without counting anything against the module: no atlas, no fog state, or no shadow light. The frame has no
// march in it, which is not a failure, so the streak is untouched and the line says why.
void NoteSkip(State& state, SkipReason reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Godrays, ext::ffxiv::diag::StageOutcome::Declined,
                                  DeclineName(reason == SkipReason::NoAtlas ? Decline::NoAtlas
                                              : reason == SkipReason::NoFogState ? Decline::NoFogState
                                                                                 : Decline::NoLight));
    CountSkip(state.skips, reason);
    if (state.log.Take(0x20000010u | uint32_t(reason)))
        LOG_INFO("{}", FormatSkip(reason));
}

// The march at one tone-map draw, in either role. The game's draw runs exactly once, whoever owns it: the chained role returns this
// module's texture (or null, which leaves the chain's hold standing), and the owner role binds its own output at t0 for the one draw and
// puts the game's own binding back, also when that draw throws.
ID3D11ShaderResourceView* MarchAtToneMap(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                         ID3D11ShaderResourceView* scene, bool owner, DrawFn draw, void* user, bool& drew)
{
    const Outcome declineOutcome = owner ? Outcome::EngineOwn : Outcome::HoldStands;
    // The scene HDR path replays the engine's own tone chain in its own branch, so the scene this march would shade is not the picture
    // that path presents. The key is read live, so a session that switches HDR off gets the march back without arming anything again.
    if (shafts::SceneHdrInactive(state.enabled, ext::hdr::FfxivSceneHdr::Requested()))
    {
        NoteDecline(state, Decline::SceneHdr, declineOutcome);
        return nullptr;
    }
    if (facts.replay)
    {
        NoteDecline(state, Decline::CharaReplay, declineOutcome);
        return nullptr;
    }
    const shafts::Decline shape = shafts::ClassifyToneMap(facts);
    if (shape != shafts::Decline::None)
    {
        Decline reason = Decline::NotToneMap;
        if (shape == shafts::Decline::RenderTarget)
            reason = Decline::RenderTarget;
        else if (shape == shafts::Decline::TargetFormat)
            reason = Decline::TargetFormat;
        else if (shape == shafts::Decline::SceneFormat)
            reason = Decline::SceneFormat;
        else if (shape == shafts::Decline::SizeMismatch)
            reason = Decline::SizeMismatch;
        else if (shape == shafts::Decline::Window)
            reason = Decline::Window;
        NoteDecline(state, reason, declineOutcome);
        return nullptr;
    }
    // The frame's state: the newest copy each ring still holds, the last accepted state held across gaps.
    FrameState marchState;
    float fade = 1.0f;
    const bool ready = Readback(context, state, marchState, fade, facts);
    NoteFrameStep(state);
    // The three things a frame needs before the march has anything to add to it: the atlas, the fog state and the shadow light. A frame
    // without one is a frame without the march in it, not a refusal.
    if (state.views.AtlasView() == nullptr || state.views.Depth() == nullptr)
    {
        NoteSkip(state, SkipReason::NoAtlas);
        return nullptr;
    }
    // The newest light draw's verdict comes before anything held: a light behind or beside the camera, or one that is not the shadow light, is a frame
    // without beams, whatever the last accepted state would still draw (its line was written when the verdict changed).
    if (state.haveJudgement && state.judgement.gate != LightGate::Run)
    {
        NoteSkip(state, SkipReason::NoLight);
        return nullptr;
    }
    if (!ready || !FogStateUsable(marchState.fog))
    {
        // A camera whose bytes arrived and are not this build's camera is a build mismatch, counted toward the stand-down; a read that has
        // stopped landing (or never landed) is a GPU that is behind or a build without the draws, and its own line says which.
        if (state.lastReadCameraRefused)
        {
            if (!state.loggedCameraRefused)
            {
                state.loggedCameraRefused = true;
                LOG_INFO("{}", kCameraRefused);
            }
            NoteDecline(state, Decline::NoCamera, declineOutcome);
            return nullptr;
        }
        if (state.frame.Reads() == 0 && state.frame.NotReady() >= kReadbackStallFrames && !state.loggedStall)
        {
            state.loggedStall = true;
            LOG_INFO("{}", kReadbackNotLanding);
        }
        else if (state.frame.HaveRead() && state.frames >= state.frame.LastReadFrame() &&
                 state.frames - state.frame.LastReadFrame() > kMaxReadbackAge && !state.loggedStale)
        {
            state.loggedStale = true;
            LOG_INFO("{}", kReadbackStale);
        }
        NoteSkip(state, SkipReason::NoFogState);
        return nullptr;
    }
    if (!(fade > 0.0f))
    {
        NoteSkip(state, SkipReason::NoLight);
        return nullptr;
    }
    // The depth has to be this frame's scene's own: a depth of another size is another draw's binding, and the march's rays would end at
    // the wrong surfaces.
    if (state.views.DepthWidth() != facts.sceneWidth || state.views.DepthHeight() != facts.sceneHeight)
    {
        NoteDecline(state, Decline::SizeMismatch, declineOutcome);
        return nullptr;
    }

    ID3D11ShaderResourceView* ours = RunMarch(state, context, facts, marchState, fade, scene);
    if (ours == nullptr)
    {
        NoteDecline(state, Decline::PassFailed, declineOutcome);
        return nullptr;
    }
    // The capture frame's pictures of this stage: the scene it was handed and the answer it made, copied now (its pass is over and its guards
    // are gone) and before the stage after it composes on the answer.
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Godrays, context, scene, ours,
                               ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height });
    // The numbers this run used that the dump's inputs do not hold (the replay lab needs them): the white point and fade, the sun in window pixels and
    // the tint, this frame's own (RunMarch filled state.lastRun for this run). Capture frame only; the values snapshot carries none of them.
    if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Godrays))
    {
        ext::ffxiv::diag::StageFrameValues values;
        values.haveWhite = true;
        values.white = state.lastRun.whitePoint;
        values.fade = state.lastRun.fade;
        values.haveSun = true;
        values.sunX = state.lastRun.sunScreen[0];
        values.sunY = state.lastRun.sunScreen[1];
        values.haveTint = true;
        for (uint32_t i = 0; i < 3; ++i)
            values.tint[i] = state.lastRun.tint[i];
        ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Godrays, values);
    }

    // Answered: the counters and the one line a session gets. The chained role hands its texture back for the caller to bind; the owner
    // role binds it here, for exactly one draw, and puts the game's own binding back.
    state.runs++;
    if (fade < 1.0f)
        state.heldRuns++;
    state.lastWindowWidth = uint32_t(facts.window.width);
    state.lastWindowHeight = uint32_t(facts.window.height);
    state.lastMarchWidth = MarchExtent(facts.window.width, state.params.resolution);
    state.lastMarchHeight = MarchExtent(facts.window.height, state.params.resolution);
    state.session.NoteRun(Knob::Godrays);
    if (state.log.Take(0x20000002u | ((facts.sceneWidth & 0x7FFFu) << 11)))
        LOG_INFO("{}", FormatRun(state.params, state.lastWindowWidth, state.lastWindowHeight, state.lastMarchWidth,
                                 state.lastMarchHeight, marchState.sunDirection, fade, state.whitePoint,
                                 state.lastStats.landed ? state.lastStats.termPeak : -1.0f));
    // The march's own answer is counted above; the stage after this march (the screen-space light bounce) composes on it now when its chained
    // hook is installed, and a null from it is this march's own answer unchanged (the stage being off is today's picture). The composition runs
    // in both roles, so the owner's bind and the chained return both carry the stage after this march's own answer when it has one.
    ComposeChainNext(context, facts, ours);
    if (!state.loggedCost && state.pass.HasGpuMilliseconds())
    {
        state.loggedCost = true;
        LOG_INFO("{}", FormatCost(true, state.pass.GpuMilliseconds(), state.frames, state.runs, state.declined));
    }
    if (!owner)
        return ours;

    // The owner's one bind: this module's copy of the scene at t0 for the one draw, and the game's own view back afterwards, also when
    // that draw throws. The chained role's caller does the same for its own texture.
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &ours);
    struct Restore
    {
        ID3D11DeviceContext* context = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        ~Restore()
        {
            context->PSSetShaderResources(0, 1, &view);
            if (view != nullptr)
                view->Release(); // the reference the read above took
        }
    } restore { context, original };
    draw(user);
    drew = true;
    return ours;
}

// The march as the chain's fallback owner: the entry the shafts' chain and the seam's own call reach when neither earlier stage answered.
bool RunToneMapAsOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw,
                       void* user)
{
    State& state = S();
    bool drew = false;
    try
    {
        LogCountsIfDue(state);
        MarchAtToneMap(state, context, facts, scene, true, draw, user, drew);
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
        if (!drew)
            draw(user); // the game's draw, once, and not twice
        return true;
    }
    if (drew)
        return true;
    // The march's own decline is the chain's next stage's owner turn: the stage after this march owns the game's draw on exactly the contract
    // this entry carries (true only when it drew; its own declines stay its own, and false leaves the game's draw to the caller). It is asked
    // outside the march's try on purpose: that stage catches everything itself, and a throw after it drew must not reach the march's catch,
    // which would draw the game's call a second time.
    if (ext::ffxiv::engine::toneMapGiOwner != nullptr)
        return ext::ffxiv::engine::toneMapGiOwner(context, facts, scene, draw, user);
    return false;
}

// The march as a chain member: the entry the earlier stages' hooks call with the chain's current output. A texture comes back for the
// caller to bind at t0; null means the march has nothing for this frame, and the caller's own texture - the chain's last good picture -
// stays at t0 (the hold stands). The stage after this march is called on either member's own hold: the march composes onto it where the
// march's own answer is, and the stage after composes onto the handed-in texture where the march's hold stands - the chain's own order is
// the two members' two answers, one after the other, and a null from either is the one that came before it.
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr)
        return nullptr;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    ID3D11ShaderResourceView* ours = nullptr;
    // The close needs no armed test: a march that is off never opens a count, so its frame is closed already and the close is a no-op.
    CloseFrameAtToneMap(state, facts, true);
    if (MarchArmed() && !(state.haveMarchFrame && state.marchFrame == state.frames))
    {
        // The chained role answers at most one tone map a frame, by this module's own counter: a second tone-shaped draw of the frame is the
        // engine's own post-tone copy, and the game's own call.
        state.haveMarchFrame = true;
        state.marchFrame = state.frames;
        try
        {
            bool drew = false; // the chained role never draws: the caller binds and draws
            ours = MarchAtToneMap(state, context, facts, scene, false, nullptr, nullptr, drew);
        }
        catch (...)
        {
            LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::HoldStands));
            ours = nullptr; // the chain's hold stands
        }
    }
    // The march answered: the stage after it was already handed that answer inside the march (ComposeChainNext), so its result is final.
    if (ours != nullptr)
        return ours;
    // The march has no answer - its key is off (the default), it answered this frame already, or it declined - and its hold stands. Every one
    // of those exits hands the stage after it (ChainHooks.h's chainGiScene) the texture this call was given, the earlier stages' picture, so
    // that stage is reachable with the march off; the stage applies its own armed test and its own one-answer gate. A null from it is the
    // march's null: the caller's texture stays at t0.
    if (ext::ffxiv::engine::chainGiScene != nullptr)
        return ext::ffxiv::engine::chainGiScene(context, facts, scene);
    return nullptr;
}

// The march as the chain's fallback owner, through the chain's own hook: the shafts' DrawToneMapWithChain reaches here when the puddles'
// stage is not armed and the march is.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    // Both early exits below hand the stage after this march (ChainHooks.h's toneMapGiOwner) the game's scene: the march's key is off by
    // default, and the shafts' fallback-owner branch reaches this entry whether the march is armed or not, so a stage placed after these
    // returns would never run. The stage owns the game's draw on exactly the contract this entry carries (true only when it drew; false obliges
    // the caller to draw the game's call once) and applies its own armed test and its own one-answer gate, so the second exit - this frame's
    // tone map already answered - is a no-op for it unless it has not had its turn. With the stage after not linked both exits are today's:
    // false, and the game's own draw runs unwrapped.
    if (context == nullptr || scene == nullptr || draw == nullptr || !MarchArmed())
    {
        if (ext::ffxiv::engine::toneMapGiOwner != nullptr)
            return ext::ffxiv::engine::toneMapGiOwner(context, facts, scene, draw, user);
        return false;
    }
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    if (state.haveMarchFrame && state.marchFrame == state.frames)
    {
        // this frame's tone map was already answered, by this module or by the chain
        if (ext::ffxiv::engine::toneMapGiOwner != nullptr)
            return ext::ffxiv::engine::toneMapGiOwner(context, facts, scene, draw, user);
        return false;
    }
    state.haveMarchFrame = true;
    state.marchFrame = state.frames;
    return RunToneMapAsOwner(context, facts, scene, draw, user);
}

// The seam's own entry: the captures at the fog post and the light draws, and the tone map's fallback owner when neither earlier stage
// took the draw.
bool SeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return false;
    if (!Armed() || context == nullptr)
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    try
    {
        if (ext::ffxiv::engine::IsFogPostAnchors(anchors))
        {
            // The fog post's copy serves the march (everything it reads) or, with the march off, the night curve alone (the sun row).
            if (MarchArmed())
                CaptureFogPost(context, anchors, state);
            else
                CaptureFogSunOnly(context, anchors, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (ext::ffxiv::engine::IsShaftsLightAnchors(anchors) && MarchArmed())
        {
            CaptureLightDraw(context, anchors, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors) && MarchArmed())
        {
            RecordScene(context, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors) && MarchArmed())
        {
            // The shafts and the puddles' stage had their chances above this entry in the dispatch; reaching here with a tone map means
            // neither took it, and the march is the fallback owner. The shape is measured here, and a draw that is not the tone map is
            // the game's own call.
            ComPtr<ID3D11ShaderResourceView> scene;
            const shafts::ToneMapFacts facts = MeasureToneMap(context, state, scene);
            if (shafts::ClassifyToneMap(facts) == shafts::Decline::NotToneMap)
                return false;
            CloseFrameAtToneMap(state, facts, false);
            if (state.haveMarchFrame && state.marchFrame == state.frames)
                return false;
            state.haveMarchFrame = true;
            state.marchFrame = state.frames;
            return RunToneMapAsOwner(context, facts, scene.Get(), draw, user);
        }
    }
    catch (...)
    {
        // Fail open: the engine module's draw path runs the game's draw, unchanged.
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
    }
    return false;
}

bool SceneHdrRequested() { return ext::hdr::FfxivSceneHdr::Requested(); }

void ObserveSceneHdr(State& state)
{
    if (state.hdr.Sample(state.enabled, SceneHdrRequested()))
        LOG_INFO("{}", kSceneHdrInactive);
}

void Apply(bool on)
{
    State& state = S();
    auto& cfg = ext::Cfg().ffxiv;
    state.enabled = on;
    state.params = BuildParams(cfg.godraysSteps.value_or_default(), cfg.godraysResolution.value_or_default(),
                               cfg.godraysFogBoost.value_or_default(), cfg.godraysHeightLift.value_or_default(),
                               cfg.godraysIntensity.value_or_default(), cfg.godraysColor.value_or_default(),
                               cfg.godraysShadowSoftness.value_or_default(), cfg.godraysJitter.value_or_default(),
                               cfg.godraysPhaseBoost.value_or_default(), cfg.godraysDensity.value_or_default(),
                               cfg.godraysAnisotropy.value_or_default());
    ObserveSceneHdr(state);
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.loggedCost = false;
        state.loggedRejected = false;
        state.loggedCameraRefused = false;
        state.loggedStale = false;
        state.loggedCameraCut = false;
        state.loggedAtlas = false;
        state.loggedSky = false;
        state.loggedLut = false;
        state.loggedDepth = false;
        state.loggedFogUnusable = false;
        state.loggedStall = false;
        state.loggedViewportRow = false;
        state.loggedNoProfile = false;
        state.loggedProfileRefused = false;
        state.loggedCascadesUnusable = false;
        state.loggedSlices = false;
        state.stats.Reset();
        state.lastStats = StatsRead {};
        state.lastRun = StageMeasure {};
        state.judgement = LightJudgement {};
        state.haveJudgement = false;
        state.step.Reset();
        state.stepLine = shafts::ChangeLine {};
        state.missingLine = shafts::ChangeLine {};
        state.frameAge = state.fogAge = state.cascadeAge = shafts::kRingNotLanded;
        state.seenLight = state.seenCascades = state.seenFog = state.seenFogUsable = false;
        state.sceneResource = nullptr;
        state.sunDrawTaken = false;
        state.rejectedLight = nullptr;
        state.rejectedFrame = 0;
        state.haveMarchFrame = false;
        state.marchFrame = 0;
        state.hold = MarchHold {};
        state.haveLastCamera = false;
        state.haveWhitePoint = false;
        state.whiteFrame = 0;
        state.whitePoint = shafts::kFallbackWhitePoint;
        state.nextCountsLog = 0;
        state.runs = 0;
        state.heldRuns = 0;
        state.declined = 0;
        state.refusals = 0;
        state.skips = CardCounts {};
        state.views.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // the player turned the switch back on: the counters and the streaks start again
        state.nextCountsLog = 0;
    }
    const bool wanted = Wanted(state.enabled, state.params) && state.session.Armed();
    // The feature bit is the march's key OR the night curve's request for the fog sun: with shadow mapping off (the default) the second term is false.
    ext::SetActive(ext::F::GodraysVolumetric, wanted || fogsun::Wanted());
    if (!wanted || state.hdr.inactive)
    {
        state.pass.Reset();
        state.frame.Reset();
        state.cascades.Reset();
        state.tone.Reset();
        state.views.Reset();
        state.stats.Reset();
        if (!fogsun::Wanted())
            state.fog.Reset(); // the sun-only capture keeps its ring across a config load
    }
}
} // namespace

bool MarchArmed()
{
    State& state = S();
    if (!Wanted(state.enabled, state.params) || !state.session.Armed())
        return false;
    return !shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
}

// The seam wants to see draws when the march is armed or the night curve asked for the fog sun (the fog post's capture then runs alone).
bool Armed() { return MarchArmed() || fogsun::Wanted(); }

// The fog sun flag changed (shadow mapping's config load, in either order with this module's): the feature bit follows it.
void OnFogSunWantedChanged()
{
    State& state = S();
    ext::SetActive(ext::F::GodraysVolumetric, (Wanted(state.enabled, state.params) && state.session.Armed()) || fogsun::Wanted());
}

// The stage dump's two questions about this stage, asked at the capture frame: is it armed, and what are its values. The window opens before the chain
// runs, so the measured numbers are the last completed run's.
std::string StageValues() { return FormatStageValues(S().params, S().lastRun); }

void SampleSceneHdr()
{
    State& state = S();
    const uint64_t token = ext::present::FrameCount();
    if (state.haveHdrSample && state.hdrSampleFrame == token)
        return;
    state.haveHdrSample = true;
    state.hdrSampleFrame = token;
    ObserveSceneHdr(state);
}

bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    return SeamDraw(context, anchors, draw, user);
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.enabled = state.enabled;
    status.armed = MarchArmed();
    status.hdrInactive = shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
    status.intensity = state.params.intensity;
    status.frames = state.frames;
    status.runs = state.runs;
    status.held = state.heldRuns;
    status.declined = state.declined;
    status.skipped = state.skips.skipped;
    status.noAtlas = state.skips.noAtlas;
    status.noFog = state.skips.noFog;
    status.noLight = state.skips.noLight;
    status.refused = state.refusals;
    status.reads = state.frame.Reads();
    status.notReady = state.frame.NotReady();
    status.windowWidth = state.lastWindowWidth;
    status.windowHeight = state.lastWindowHeight;
    status.marchWidth = state.lastMarchWidth;
    status.marchHeight = state.lastMarchHeight;
    return status;
}

// The fog post's profile-slot stamp, written once at shader creation (the engine pass's creation hook calls it for every shader that names
// one of the module's buffers, whatever any key says: a switch turned on later finds the shader already stamped). A shader that does not
// declare both the fog constants and the profile takes no stamp.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32 || !MentionsProfile(code, size))
        return;
    try
    {
        StampShader(shader, PackProfileSlot(ProfileSlotOfShader(code, size)));
    }
    catch (...)
    {
    }
}

void Publish() { Apply(ext::Cfg().ffxiv.godraysVolumetric.value_or_default()); }

void OnConfigLoaded(bool on) { Apply(on); }

void Release()
{
    State& state = S();
    state.pass.Reset();
    state.frame.Reset();
    state.fog.Reset();
    state.cascades.Reset();
    state.tone.Reset();
    state.views.Reset();
    state.stats.Reset();
    state.sceneResource = nullptr;
    state.hold = MarchHold {};
}
} // namespace ext::ffxiv::godrays

namespace
{
// The registration of the config hook and the chain's stage hooks, set when this DLL's static initialisers run: the [FFXIV]
// GodraysVolumetric key arms the seam's feature bit at every config load, and the chain's two earlier stages reach the march through the
// pointers the chain's own header declares (null in every build that does not link this file, which is the march being off).
struct RegisterGodrays
{
    RegisterGodrays()
    {
        ext::FfxivConfig::godraysOnLoaded = &ext::ffxiv::godrays::OnConfigLoaded;
        ext::ffxiv::engine::chainMarchScene = &ext::ffxiv::godrays::ChainScene;
        ext::ffxiv::engine::toneMapMarchOwner = &ext::ffxiv::godrays::ToneMapOwner;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Godrays, &ext::ffxiv::godrays::MarchArmed, &ext::ffxiv::godrays::StageValues);
        ext::ffxiv::fogsun::SetWantedChangedHook(&ext::ffxiv::godrays::OnFogSunWantedChanged);
    }
} g_registerGodrays;
} // namespace
