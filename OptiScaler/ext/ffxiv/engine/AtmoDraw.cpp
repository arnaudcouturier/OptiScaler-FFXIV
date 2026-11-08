#include "pch.h"

#include "ext/ffxiv/engine/Atmo.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h" // the reflection's plain rows: the fog post's profile slot, for the stamp
#include "ext/ffxiv/diag/StageDump.h"      // the per-stage image dump: this stage reports its run and its pictures on the capture frame
#include "ext/ffxiv/engine/AtmoCounts.h" // the timed counts line: the pure rules (INT2)
#include "ext/ffxiv/engine/AtmoPass.h"
#include "ext/ffxiv/engine/ChainHooks.h" // the chain's stage hooks, which this module registers
#include "ext/ffxiv/engine/EnginePass.h" // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/GodraysSun.h"  // the readers (the light and camera copy, the fog copy, the secured depth), reused as types: nothing of the march's is edited
#include "ext/ffxiv/engine/RecogRules.h"  // FrameStep: one count per rendered frame, the step line, the pixel-to-NDC mapping
#include "ext/hdr/scene/FfxivSceneHdr.h"  // the scene HDR path replaces the tone chain this stage draws before
#include "ext/present/PresentSeams.h"     // the output-frame counter, the scene-HDR sample's token

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <format>
#include <thread>

// Atmosphere: the draw path, the frame's captures and the module's session (Atmo.h). The pure decisions are in AtmoRules.h, the pass in AtmoPass.cpp, the shader
// in shaders/Atmo.hlsl, the card in AtmoMenu.cpp.
//
// What it costs while it is off: the engine module's one call per draw into OnSeamDraw, which returns at two bit tests - no state query, no allocation and
// no log line. While it is on, per frame: the sun's light draw and the camera are copied at the frame's first draw of the sun's shape (and, CAMFIX, the camera alone at
// the frame's first light draw of any shape, the source depth of field uses, for the frames and layouts with no sun-shaped light draw), the fog constants, the
// height profile and the depth at the fog post's draw, the scene is recorded at the composite, and at the tone map the stage reads the newest copies the rings
// still hold (never waiting for the GPU), mixes the chain's current output toward the air and the chain binds this module's copy of the scene at t0 for that
// one draw (the owner role) or hands it on (the chained role).
//
// Nothing of the game is written: the scene is copied into an owned texture, and the depth is read through a view the fog post itself bound. The tone map's
// own binding is put back after its one draw, also when that draw throws.
//
// The frame's count opens at the frame's first composite after the previous frame's tone map (the game draws the composite twice a frame), and every capture
// this stage makes stamps the count it was made in. The captures' rings are this module's own - never the march's, never the radial's, never the puddle
// stage's stamp (the stamping contract: this module's answered-tracking is its own rendered-frame counter, atmoFrame).
namespace ext::ffxiv::engine::atmo
{
namespace
{
using Microsoft::WRL::ComPtr;

inline constexpr uint32_t kMaxRenderTargets = 8;
inline constexpr uint32_t kToneMapSlot = 0;
inline constexpr uint32_t kMaxCaptureAttempts = 24;
inline constexpr uint32_t kMaxCameraBufferBytes = 0x400;
inline constexpr uint64_t kMaxReadbackAge = 90;
inline constexpr uint32_t kReadbackStallFrames = 64;
// (kMaxCameraAge, the age a camera or a sun draw's sun stands for, is in AtmoRules.h with the rule that uses it: FoldCameraSources / CameraStands / SunStands.)

// The slot this module's stamp carries the fog post's height profile at: its own tag (the march's is its own, file-local to its translation unit), written on
// the shader object at creation and read at the fog post's draw.
constexpr GUID kProfileStampTag { 0x6d2f8a14, 0x9e37, 0x4c51, { 0xb0, 0x8d, 0x25, 0xf1, 0x7c, 0xe3, 0x4a, 0x69 } };

// What the frame has told this module, held across gaps: the camera (the inverse projection the depth is reconstructed through and the view rows the up
// vector and the sun come from), the fog state (the engine's own colour and density), the last sun-class light (colour, and direction in world space).
struct Hold
{
    HoldStamps stamps; // CAMFIX: when the camera was last refreshed and when the sun draw's copy last landed (AtmoRules.h FoldCameraSources: the ages the hold stands for)
    float inverseProjection[16] {};
    float view[shafts::kViewRows] {};
    bool haveView = false;
    float cameraWorldY = 0.0f;

    bool haveFog = false;
    godrays::FogState fog;
    float engineDensity = 0.0f; // the engine's own density at the camera's height, per metre
    bool haveFogSun = false;
    float fogSunY = 0.0f; // the fog's own sun's elevation sine (the one component of cFogParam's sun row that is the sun's: see SunGain)

    // L2b: the sky's own sun sprite (cSunParam row 0: the disc's UV in the window), the last row that landed and the rendered frame it was written at. The lobe
    // follows it only while it is fresh and agrees with the light (AtmoRules.h ChooseSunDirection).
    bool haveSunUv = false;
    float sunUvRow[4] {};
    uint64_t sunUvFrame = 0;

    bool haveSun = false; // a sun-class light has been read
    float sunWorld[3] { 0.0f, 1.0f, 0.0f };
    bool haveLuminance = false; // ... and it reported a colour
    float diffuse[3] {};
    float luminance = 0.0f;
    float sunElevation = 0.0f; // the world elevation sine of that light
};

struct State
{
    Pass pass;
    Streak streak;
    LogGate log;
    LogGate fogLog;              // FOGFIX: the lines for the engine's fog going between none and fogged (its own budget, so the run and skip lines do not eat it)
    Params params;
    bool enabled = false;
    godrays::FrameReader frame;  // the sun draw's light and the camera, one copy
    godrays::FrameReader lightDraws; // CAMFIX: the camera copy taken at the frame's first light draw of ANY shape (the same reader, the same verification; its light is ignored)
    godrays::FogReader fog;      // the fog post's constants, the height profile and the viewport buffer
    shafts::ToneReader tone;     // the tone map's own exposure multiplier: the frame's white point
    godrays::StolenViews views;  // the scene depth the fog post binds (the atlas, sky and LUT views are not used by this stage)
    shafts::SunUvReader sunUv;   // L2b: the sky's sun sprite parameters, copied at its draw and read a frame late (never waited for)
    bool haveRenderThread = false;
    std::thread::id renderThread;

    uint64_t frames = 0; // this module's own count of rendered frames, opened at the frame's composite (FrameStep)
    shafts::FrameStep step;
    shafts::ChangeLine stepLine;
    uint32_t frameAge = shafts::kRingNotLanded;
    uint32_t lightDrawAge = shafts::kRingNotLanded;
    uint32_t fogAge = shafts::kRingNotLanded;
    uint32_t attempts = 0;
    bool sunDrawTaken = false;
    bool cameraTaken = false; // CAMFIX: the frame's light draws' camera copy was taken (one copy a frame, at the first light draw that binds a camera table)
    LogGate sourceLog;        // CAMFIX: the line for the camera changing between the sun draw's copy and the light draws' (its own budget, like fogLog)
    const void* rejectedLight = nullptr; // a light the read-back showed at a world position: a lamp, never the sun
    uint64_t rejectedFrame = 0;

    // The frame's lit scene: the deferred composite's single render target, which the tone map then reads as its t0.
    void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;
    // The stage answers at most one tone map a frame, by its own counter: a second tone-shaped draw of the frame is the engine's own post-tone copy.
    uint64_t atmoFrame = 0;
    bool haveAtmoFrame = false;
    Hold hold;
    float whitePoint = shafts::kFallbackWhitePoint;
    bool haveWhitePoint = false;
    uint64_t whiteFrame = 0;

    // What the card shows and what the log says.
    uint32_t runs = 0;
    uint32_t declined = 0;
    SkipCounts skips;
    TimedCounts counts; // the timed counts line (AtmoCounts.h): the window, the total and the clock the next line is due on
    bool loggedOff = false;
    bool loggedCost = false;
    bool loggedStall = false;
    bool loggedStale = false;
    bool loggedDepth = false;
    bool loggedNoProfile = false;
    bool loggedRejected = false;
    bool loggedFogUnusable = false;
    shafts::SceneHdrWatch hdr;
    uint64_t hdrSampleFrame = 0;
    bool haveHdrSample = false;
    uint32_t lastWindowWidth = 0, lastWindowHeight = 0;
    // What the last run used, for the card and the stage dump.
    float lastEngineScale = 1.0f;
    float lastDayScale = 1.0f;
    float lastDensityPerMetre = 0.0f;
    float lastSunGain = 0.0f;
    bool lastHaveSun = false;
    bool lastHaveLight = false; // the light's colour reached the run (the sun draw's copy stands and it reported one)
    bool lastLobeOnDisc = false;
    bool loggedSunDisc = false;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// ---- the stamp --------------------------------------------------------------------------------------------------------------------

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

uint32_t ProfileSlotOfShader(const void* code, size_t size)
{
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    const uint32_t fogSlot = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "cFogParam");
    const uint32_t profile = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "cExpHeightFogParam");
    if (fogSlot >= ext::ffxiv::engine::kConstantBufferSlots || profile >= ext::ffxiv::engine::kConstantBufferSlots)
        return godrays::kNoProfileSlot;
    return profile;
}

uint32_t BoundProfileSlot(ID3D11DeviceContext* context)
{
    ID3D11PixelShader* bound = nullptr;
    context->PSGetShader(&bound, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> shader;
    if (bound != nullptr)
        shader.Attach(bound);
    if (shader == nullptr)
        return godrays::kNoProfileSlot;
    uint64_t packed = 0;
    UINT bytes = sizeof packed;
    if (FAILED(shader->GetPrivateData(kProfileStampTag, &bytes, &packed)) || bytes != sizeof packed)
        return godrays::kNoProfileSlot;
    return godrays::UnpackProfileSlot(packed);
}

// ---- the frame's captures ---------------------------------------------------------------------------------------------------------

// The frame's composite: the one draw a rendered frame reliably makes before its tone map, and the stage's frame boundary. The count opens here (once a
// frame), the sun draw's taken flag and the capture budget reset with it, and the scene the composite writes is recorded.
void RecordScene(ID3D11DeviceContext* context, State& state)
{
    if (state.step.OpenAtComposite(state.frames))
    {
        state.attempts = 0;
        state.sunDrawTaken = false;
        state.cameraTaken = false;
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

// The sun draw's own shape, measured at the draw: the radial's own facts, with this module's own scene sizes.
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
    shape.depth = true;
    shape.camera = true;
    return shape;
}

// The light draws: the frame's sun draw (the light the lobe is centred on and coloured by, and the camera the depth is reconstructed through). The capture is
// bounded: after the first draw with the sun draw's shape the frame's copy is taken. A light the read-back showed to be a lamp is remembered, so the frames
// after it look at the next light draw instead of the same one (the radial's own rule).
void CaptureLightDraw(ID3D11DeviceContext* context, Anchors anchors, State& state)
{
    // CAMFIX: the capture budget and the early exit stand for BOTH copies - the sun draw's (the hunt below, unchanged) and the light draws' camera copy (taken at the first light draw
    // that binds a camera table, whatever its shape, so a lighting layout with no sun-shaped light draw - the benchmark's - and a zone transition still have a camera).
    if ((state.sunDrawTaken && state.cameraTaken) || state.attempts >= kMaxCaptureAttempts)
        return;
    ++state.attempts;
    const uint32_t lightSlot = ShaftsSlotOf(anchors);
    if (lightSlot >= kConstantBufferSlots)
        return;
    ID3D11Buffer* buffers[kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, kConstantBufferSlots, buffers);
    ComPtr<ID3D11Buffer> bound[kConstantBufferSlots];
    for (uint32_t i = 0; i < kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11Buffer> light = bound[lightSlot];
    if (light == nullptr)
        return;
    ComPtr<ID3D11Buffer> camera;
    uint32_t cameraBytes = 0;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == light.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth < shafts::kCameraBytes || desc.ByteWidth > kMaxCameraBufferBytes || desc.ByteWidth <= cameraBytes)
            continue;
        cameraBytes = desc.ByteWidth;
        camera = buffer;
    }
    if (camera == nullptr)
        return;
    // The light draws' camera copy first: before the sun hunt's exits (a taken sun draw, a lamp the read-back rejected, a draw that is not the sun's shape), because the
    // table it copies is the same for a lamp's draw. The light buffer rides along as the reader's contract needs (its first kLightBytes), and is never read by this stage
    // from this ring.
    if (!state.cameraTaken)
    {
        D3D11_BUFFER_DESC lightDesc {};
        light->GetDesc(&lightDesc);
        if (lightDesc.ByteWidth >= shafts::kLightBytes && state.lightDraws.Copy(context, light.Get(), camera.Get(), state.frames))
            state.cameraTaken = true;
    }
    if (state.sunDrawTaken)
        return;
    if (static_cast<const void*>(light.Get()) == state.rejectedLight && shafts::RejectedStill(state.rejectedLight != nullptr, state.rejectedFrame, state.frames))
        return;
    if (!shafts::IsSunDraw(MeasureSunDraw(context, state)))
        return;
    if (!state.frame.Copy(context, light.Get(), camera.Get(), state.frames))
        return;
    state.sunDrawTaken = true;
}

// The engine's fog post: the fog constants, the height profile and the depth, all captured at the one draw a frame that binds cFogParam makes. The depth is
// the fog post's own t0 - the same scene depth the march bounds its rays with.
void CaptureFogPost(ID3D11DeviceContext* context, Anchors anchors, State& state)
{
    const uint32_t fogSlot = FogPostSlotOf(anchors);
    if (fogSlot >= kConstantBufferSlots)
        return;
    ID3D11Buffer* buffers[kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, kConstantBufferSlots, buffers);
    ID3D11ShaderResourceView* textureViews[8] {};
    context->PSGetShaderResources(0, 8, textureViews);
    ComPtr<ID3D11Buffer> bound[kConstantBufferSlots];
    for (uint32_t i = 0; i < kConstantBufferSlots; ++i)
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
    if (fogDesc.ByteWidth != godrays::kFogParamBytes)
        return; // a buffer of another size under that name is another draw's parameters

    // The height profile: the buffer bound at the slot this module's own stamp names (its reflection at shader creation), copied whole.
    uint32_t boundBytes[kConstantBufferSlots] {};
    for (uint32_t i = 0; i < kConstantBufferSlots; ++i)
    {
        if (!bound[i])
            continue;
        D3D11_BUFFER_DESC desc {};
        bound[i]->GetDesc(&desc);
        boundBytes[i] = desc.ByteWidth;
    }
    const uint32_t stamped = BoundProfileSlot(context);
    const uint32_t profileSlot = godrays::ProfileSlotFor(stamped, boundBytes);
    ID3D11Buffer* profile = profileSlot != godrays::kNoProfileSlot ? bound[profileSlot].Get() : nullptr;
    if (profile == nullptr && !state.loggedNoProfile)
    {
        state.loggedNoProfile = true;
        LOG_INFO("Atmosphere: the fog post's height profile is not bound where this module's stamp says (stamp {}), so there is no fog state to take the engine's colour and density from",
                 stamped == godrays::kNoProfileSlot ? -1 : int(stamped));
    }
    ID3D11Buffer* viewportParam = nullptr;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == fogParam.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth == godrays::kViewportParamBytes)
        {
            viewportParam = buffer.Get();
            break;
        }
    }
    state.fog.Copy(context, fogParam.Get(), profile, viewportParam, state.frames);

    // The frame's depth: the fog post's own t0. A depth of another size than the scene the composite wrote is another draw's binding, and the stage keeps the
    // last one it kept.
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
            LOG_INFO("{}", godrays::FormatSecured("scene depth (atmosphere)", false, desc.Width, desc.Height, uint32_t(viewDesc.Format)));
        }
        break;
    }
}

// The sky's own sun sprite (L2b): its parameter buffer's first row is the sun disc's screen UV, which is where a player sees the sun. The draw is not a light
// draw (its shader declares cSunParam and no g_LightParam), so nothing else is taken from it. The copy is issued here and read at a later tone map, because a copy
// is never mapped at the frame that made it. The same 16 bytes the light shafts read, copied by this stage's own reader.
void CaptureSunParam(ID3D11DeviceContext* context, Anchors anchors, State& state)
{
    const uint32_t slot = ShaftsSlotOf(anchors);
    ID3D11Buffer* buffer = nullptr;
    context->PSGetConstantBuffers(slot, 1, &buffer);
    ComPtr<ID3D11Buffer> params;
    if (buffer != nullptr)
        params.Attach(buffer);
    if (!params)
        return;
    D3D11_BUFFER_DESC desc {};
    params->GetDesc(&desc);
    // The engine binds it at 128 bytes and this stage reads its first 16: a buffer of another size is another draw's parameters.
    if (desc.ByteWidth < shafts::kSunParamBytes || desc.ByteWidth > 0x100)
        return;
    state.sunUv.Copy(context, params.Get(), state.frames);
}

// The frame's white point, from the tone map's own parameters at this draw (the radial's own read, this module's own instance).
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

// What one frame's reads came to: the newest copy each ring still holds, folded into the hold. Nothing here declines: the caller judges what the hold has.
void Readback(ID3D11DeviceContext* context, State& state)
{
    const godrays::FrameRead frameRead = state.frame.Read(context, state.frames);
    const godrays::FrameRead lightDrawRead = state.lightDraws.Read(context, state.frames); // CAMFIX: the camera of the frame's first light draw, whatever its shape
    const godrays::FogRead fogRead = state.fog.Read(context, state.frames);
    state.frameAge = frameRead.landed ? shafts::AgeOf(state.frames, frameRead.writtenFrame) : shafts::kRingNotLanded;
    state.lightDrawAge = lightDrawRead.landed ? shafts::AgeOf(state.frames, lightDrawRead.writtenFrame) : shafts::kRingNotLanded;
    state.fogAge = fogRead.landed ? shafts::AgeOf(state.frames, fogRead.writtenFrame) : shafts::kRingNotLanded;
    Hold& hold = state.hold;
    // The sky's sun sprite row (L2b): kept only when it is this build's layout, held with the frame it was written at (the shafts' own freshness rule judges it).
    const shafts::SunUvRead uv = state.sunUv.Read(context, state.frames);
    if (uv.landed && shafts::SunParamUvUsable(uv.uv))
    {
        std::memcpy(hold.sunUvRow, uv.uv, sizeof hold.sunUvRow);
        hold.sunUvFrame = uv.writtenFrame;
        hold.haveSunUv = true;
    }

    // CAMFIX: which copy the frame's camera comes from - the sun draw's when it landed with a camera this build reads, else the light draws' one (AtmoRules.h
    // FoldCameraSources decides, and refreshes the two stamps the hold stands on). Only the sun draw's copy feeds the sun: a lamp's light never becomes the sun.
    const CameraFold fold = FoldCameraSources(hold.stamps, CameraCopy { frameRead.landed, frameRead.landed && frameRead.camera },
                                              CameraCopy { lightDrawRead.landed, lightDrawRead.landed && lightDrawRead.camera }, state.frames);
    hold.stamps = fold.stamps;
    if (fold.source != CameraSource::None && state.sourceLog.Take(uint32_t(fold.source)))
        LOG_INFO("{}", FormatCameraSource(fold.source));
    if (fold.source == CameraSource::LightDraw)
    {
        std::memcpy(hold.inverseProjection, lightDrawRead.inverseProjection, sizeof hold.inverseProjection);
        std::memcpy(hold.view, lightDrawRead.view, sizeof hold.view);
        hold.haveView = lightDrawRead.haveView;
        hold.cameraWorldY = godrays::CameraWorldY(lightDrawRead.inverseViewRow1);
    }
    if (fold.source == CameraSource::SunDraw)
    {
        std::memcpy(hold.inverseProjection, frameRead.inverseProjection, sizeof hold.inverseProjection);
        std::memcpy(hold.view, frameRead.view, sizeof hold.view);
        hold.haveView = frameRead.haveView;
        hold.cameraWorldY = godrays::CameraWorldY(frameRead.inverseViewRow1);
        // The light of the same copy: a sun-class light (a directional light above the horizon: the sun, or the moon at night) gives the lobe its direction
        // and colour; a lamp is remembered so the frames after it look at the next light draw. Neither is a reason to decline the frame.
        const float elevation = frameRead.haveView ? shafts::WorldElevationOf(frameRead.view, frameRead.facts.direction) : frameRead.facts.direction[1];
        const shafts::LightVerdict verdict = shafts::ClassifyLight(frameRead.facts, elevation);
        if (verdict == shafts::LightVerdict::Sun)
        {
            float world[3] { 0.0f, 1.0f, 0.0f };
            if (frameRead.haveView && shafts::WorldDirectionOf(frameRead.view, frameRead.facts.direction, world))
            {
                std::memcpy(hold.sunWorld, world, sizeof world);
                hold.haveSun = true;
                hold.sunElevation = elevation;
                const float luminance = shafts::Luminance(frameRead.facts.diffuse);
                hold.haveLuminance = luminance == luminance && luminance > 0.0f;
                if (hold.haveLuminance)
                {
                    std::memcpy(hold.diffuse, frameRead.facts.diffuse, sizeof hold.diffuse);
                    hold.luminance = luminance;
                }
            }
        }
        else if (verdict == shafts::LightVerdict::Lamp)
        {
            state.rejectedLight = state.frame.LastLight();
            state.rejectedFrame = state.frames;
            if (!state.loggedRejected)
            {
                state.loggedRejected = true;
                LOG_INFO("{}", shafts::FormatNotTheSun(frameRead.facts));
            }
        }
    }
    if (fogRead.landed)
    {
        if (fogRead.sunLanded)
        {
            hold.fogSunY = fogRead.sun[1];
            hold.haveFogSun = true;
        }
        if (fogRead.usable)
        {
            hold.fog = fogRead.fog;
            hold.haveFog = true;
        }
        else if (!state.loggedFogUnusable)
        {
            state.loggedFogUnusable = true;
            LOG_INFO("Atmosphere: the fog state this frame's fog post bound is not one this stage can evaluate, so the last good one is held");
        }
    }
    if (hold.haveFog)
    {
        hold.engineDensity = godrays::FogDensityAt(hold.fog, hold.cameraWorldY, 1.0f);
        // FOGFIX: the engine's height fog going between none (clear weather binds a profile with every density zero: engine density 0, engine fog scale 1) and fogged
        // is written once per change, so a session shows which of the two it was in and when it changed.
        const bool none = godrays::EngineHasNoHeightFog(hold.fog);
        if (state.fogLog.Take(none ? 1u : 2u))
            LOG_INFO("{}", FormatEngineFog(none, hold.engineDensity, EngineFogScale(hold.engineDensity), hold.fog.fogColor));
    }
    // The staleness lines, the march's own two states on this module's own rings - and, since the camera can come from either copy (CAMFIX), on the camera as the hold
    // stands on it: "never started" is neither ring having read a thing while a ring has copies waiting, "stopped" is the hold's camera not refreshed by EITHER copy for
    // kMaxReadbackAge frames (a stage running happily on the light draws' copy is not "stopped").
    if (state.frame.Reads() == 0 && state.lightDraws.Reads() == 0 && (state.frame.NotReady() >= kReadbackStallFrames || state.lightDraws.NotReady() >= kReadbackStallFrames) &&
        !state.loggedStall)
    {
        state.loggedStall = true;
        LOG_INFO("Atmosphere: the camera read-back has not started landing while this stage is armed");
    }
    else if (hold.stamps.haveCamera && state.frames >= hold.stamps.cameraFrame && state.frames - hold.stamps.cameraFrame > kMaxReadbackAge && !state.loggedStale)
    {
        state.loggedStale = true;
        LOG_INFO("Atmosphere: the camera read-back has stopped landing, so the last accepted camera stands for a while");
    }
}

// ---- one declined or skipped frame ------------------------------------------------------------------------------------------------

// One declined tone-map draw: the per-reason count, one line per change of reason with the outcome the chain's hold semantics name, this module's own refusal
// streak, and the game's own draw left exactly as the chain left it. A decline NEVER draws: the owner role returns false and the caller draws the game's call
// once on the binding it already has (the chain's owner-return rule); the chained role returns null and the chain's hold stands.
void NoteDecline(State& state, Decline reason, Outcome outcome)
{
    if (reason != Decline::NotToneMap)
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Atmosphere, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    state.declined++;
    state.counts.NoteDeclined(reason);
    if (CountsTowardsStandDown(reason) && state.streak.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("Atmosphere: sixty-four draws in a row were left alone, so the stage is off for the session (reason of the last: {})", DeclineName(reason));
        ext::SetActive(ext::F::Atmosphere, false); // the draw path is back to two bit tests
    }
    if (state.log.Take(uint32_t(reason) | (uint32_t(outcome) << 8)))
        LOG_INFO("{}", FormatDecline(reason, outcome));
}

// One skipped frame: nothing is counted against the module (a frame without the stage in it is not a failure), and the game's draw is left exactly as the
// chain left it.
void NoteSkip(State& state, SkipReason reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Atmosphere, ext::ffxiv::diag::StageOutcome::Declined, SkipName(reason));
    CountSkip(state.skips, reason);
    state.counts.NoteSkipped(reason);
    if (state.log.Take(0x80000010u | uint32_t(reason)))
        LOG_INFO("{}", FormatSkip(reason));
}

// ---- the run -----------------------------------------------------------------------------------------------------------------------

// The pass on the scene it was handed, with the frame's own numbers: the camera's reconstruction, the up vector and the sun in view space, the density after the
// engine-fog and night scales, the colours. Null when the pass did not run.
ID3D11ShaderResourceView* RunStage(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    const Hold& hold = state.hold;
    Inputs inputs;
    inputs.scene = scene;
    inputs.depth = state.views.Depth();
    inputs.sceneFormat = facts.sceneFormat;
    inputs.sceneWidth = facts.sceneWidth;
    inputs.sceneHeight = facts.sceneHeight;
    inputs.depthFormat = state.views.DepthFormat();
    inputs.depthWidth = state.views.DepthWidth();
    inputs.depthHeight = state.views.DepthHeight();
    inputs.window = facts.window;
    std::memcpy(inputs.inverseProjection, hold.inverseProjection, sizeof inputs.inverseProjection);
    // The pixel-to-NDC mapping, derived from the render window (the fog post's bytecode hard-codes it; see RecogRules.h).
    const shafts::NdcRow ndc = shafts::PixelToNdcRow(facts.window);
    inputs.ndcScale[0] = ndc.scale[0];
    inputs.ndcScale[1] = ndc.scale[1];
    inputs.ndcOffset[0] = ndc.offset[0];
    inputs.ndcOffset[1] = ndc.offset[1];
    inputs.params = state.params;
    // The world's up in view space (the reference height is the camera's, so this is all the geometry the medium needs), and the sun when there is one.
    const float up[3] { 0.0f, 1.0f, 0.0f };
    if (!hold.haveView || !shafts::ViewDirectionOf(hold.view, up, inputs.upView))
    {
        inputs.upView[0] = 0.0f;
        inputs.upView[1] = 1.0f;
        inputs.upView[2] = 0.0f;
    }
    // CAMFIX: the sun the sun draw's copy carried stands for kMaxCameraAge frames after that copy (SunStands) and no longer: a camera that keeps landing from the light
    // draws must not keep drawing the lobe direction and the tint of a sun the player has left behind (those belong to the zone). Past that age the frame has no sun
    // lobe: the ambient term alone. The day/night weight of the density is NOT aged with it: it follows the sun's and the moon's luminance, which is the time of day
    // (global), so the last luminance read keeps standing - dropping it would step a night scene to ten times its haze at the age boundary.
    const bool sunStands = SunStands(hold.stamps, state.frames);
    const bool haveLight = sunStands && hold.haveLuminance;
    bool haveSun = false;
    if (sunStands && hold.haveSun && hold.haveView && shafts::ViewDirectionOf(hold.view, hold.sunWorld, inputs.sunView))
        haveSun = true;
    // The lobe's direction (L2b): the visible disc when the sky's sun row is fresh and agrees with the light, else the light's own direction (the L2 behaviour).
    float discView[3] {};
    const bool discFresh = shafts::SunUvFresh(hold.haveSunUv, hold.sunUvFrame, state.frames) &&
                           SunDirectionFromUv(hold.inverseProjection, hold.sunUvRow[2], hold.sunUvRow[3], discView);
    const SunChoice choice = ChooseSunDirection(inputs.sunView, haveSun, discView, discFresh, inputs.upView);
    if (choice.disc)
    {
        const float between = inputs.sunView[0] * choice.dir[0] + inputs.sunView[1] * choice.dir[1] + inputs.sunView[2] * choice.dir[2];
        if (!state.loggedSunDisc)
        {
            state.loggedSunDisc = true;
            const float clamped = between > 1.0f ? 1.0f : (between < -1.0f ? -1.0f : between);
            LOG_INFO("{}", FormatSunDisc(hold.sunUvRow[2], hold.sunUvRow[3], std::acos(clamped) * 57.2957795f));
        }
        for (uint32_t i = 0; i < 3; ++i)
            inputs.sunView[i] = choice.dir[i];
    }
    state.lastLobeOnDisc = choice.disc;
    const float elevation = hold.haveFogSun ? hold.fogSunY : hold.sunElevation;
    inputs.sunGain = SunGain(haveSun, haveLight, hold.luminance, elevation);
    const shafts::SunTint tint = shafts::ResolveSunTint(haveLight ? hold.diffuse : nullptr);
    for (uint32_t i = 0; i < 3; ++i)
    {
        inputs.sunTint[i] = tint.rgb[i];
        inputs.fogColour[i] = hold.fog.fogColor[i];
    }
    inputs.whiteLinear = WhiteLinearOf(state.whitePoint);
    // The medium: the key per metre, scaled down where the engine's own fog already fogs the air and where the light is the moon's.
    const float engineScale = EngineFogScale(hold.engineDensity);
    const float dayScale = NightDensityScale(state.params.nightDensity, hold.luminance, hold.haveLuminance);
    inputs.densityPerMetre = EffectiveDensityPerMetre(state.params, hold.engineDensity, hold.luminance, hold.haveLuminance);
    state.lastEngineScale = engineScale;
    state.lastDayScale = dayScale;
    state.lastDensityPerMetre = inputs.densityPerMetre;
    state.lastSunGain = inputs.sunGain;
    state.lastHaveSun = haveSun;
    state.lastHaveLight = haveLight;

    ID3D11ShaderResourceView* ours = nullptr;
    {
        ext::ffxiv::chara::ReplayScope own;
        ext::ffxiv::engine::OwnWork ownWork;
        ours = state.pass.Run(context, inputs);
    }
    if (ours == nullptr)
    {
        const char* failure = state.pass.Failure();
        if (failure != nullptr && state.log.Take(0x80000002u))
            LOG_INFO("Atmosphere: the stage's pass did not run ({})", failure);
    }
    return ours;
}

// The counts line (AtmoCounts.h): about every ten seconds of wall clock, from every entry that reaches the stage (StageAtToneMap's first statement) and from the
// once-an-output-frame heartbeat (SampleSceneHdr), so a stage that is armed and never reached still writes a line - one that says "reached 0 times".
void LogCountsIfDue(State& state)
{
    const uint64_t now = GetTickCount64();
    if (!state.counts.Due(now, state.frames))
        return;
    LOG_INFO("{}", state.counts.TakeLine(now, state.frames, state.params));
}

// The one line a live session needs to see the mechanism: how often the count moved for the frame that just closed, and the ages of the copies its read-backs took.
void NoteFrameStep(State& state)
{
    if (!state.step.HaveSample())
        return;
    const shafts::RingAge rings[] { { "light and camera copy", state.frameAge }, { "light draws' camera copy", state.lightDrawAge }, { "fog copy", state.fogAge } };
    const uint32_t advances = state.step.AdvancesPerFrame();
    const uint32_t composites = state.step.CompositesPerFrame();
    if (state.stepLine.Take(shafts::StepKey(advances, composites, rings, 3)))
        LOG_INFO("{}", shafts::FormatFrameStep("Atmosphere", advances, composites, rings, 3));
}

// The frame's tone map closes the frame (RecogRules.h ClosesFrame): the next frame's first composite then opens the next count. It runs at the top of every
// entry that can see the tone map, before the stage's one-answer gate and whatever the stage then does with the draw, so a frame the stage declines or skips
// still moves the count on.
void CloseFrameAtToneMap(State& state, const shafts::ToneMapFacts& facts, bool chained)
{
    if (shafts::ClosesFrame(facts, chained))
        state.step.CloseAtToneMap();
}

// The capture frame's numbers for the stage line: what the medium used.
void NoteCaptureNumbers(State& state, const shafts::ToneMapFacts& facts)
{
    RunFacts run;
    run.params = state.params;
    run.engineDensity = state.hold.engineDensity;
    run.engineScale = state.lastEngineScale;
    run.dayScale = state.lastDayScale;
    run.densityPerMetre = state.lastDensityPerMetre;
    run.haveSun = state.lastHaveSun;
    run.haveLight = state.lastHaveLight;
    run.sunGain = state.lastSunGain;
    run.lobeOnDisc = state.lastLobeOnDisc;
    run.elevationSine = state.hold.haveFogSun ? state.hold.fogSunY : state.hold.sunElevation;
    run.whiteStored = state.whitePoint;
    run.windowWidth = uint32_t(facts.window.width);
    run.windowHeight = uint32_t(facts.window.height);
    ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Atmosphere,
                                    std::format("engineDensity={:.5f} engineScale={:.3f} dayScale={:.3f} densityPerMetre={:.5f} haveSun={} sunGain={:.3f} lobeOnDisc={} neutral={:.2f} fogColour={:.3f},{:.3f},{:.3f}",
                                                double(run.engineDensity), double(run.engineScale), double(run.dayScale), double(run.densityPerMetre),
                                                run.haveSun ? 1 : 0, double(run.sunGain), run.lobeOnDisc ? 1 : 0, double(state.params.neutral), double(state.hold.fog.fogColor[0]), double(state.hold.fog.fogColor[1]),
                                                double(state.hold.fog.fogColor[2])));
    ext::ffxiv::diag::StageFrameValues values;
    values.haveWhite = true;
    values.white = state.whitePoint;
    values.fade = 1.0f;
    ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Atmosphere, values);
}

// The stage at one tone-map draw. Returns this module's own texture when the pass ran (the caller binds it - or composes depth of field on it first), and
// null on every decline and skip. The game's draw is never made here.
ID3D11ShaderResourceView* StageAtToneMap(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene,
                                         Outcome declineOutcome, Entry entry)
{
    // Every entry that reaches the stage with its one answer available comes through here: the counts line is ticked and the entry is counted first.
    LogCountsIfDue(state);
    state.counts.NoteReached(entry);
    // The scene HDR path replays the engine's own tone chain in its own branch, so the scene this stage would mix is not the picture that path presents.
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
    // The frame's depth has to be this frame's scene's own: a depth of another size is another draw's binding, and the reconstruction would read the wrong pixels.
    if (state.views.Depth() == nullptr || state.views.DepthWidth() != facts.sceneWidth || state.views.DepthHeight() != facts.sceneHeight)
    {
        NoteSkip(state, SkipReason::NoDepth);
        return nullptr;
    }
    Readback(context, state);
    ReadToneMapParams(context, state);
    NoteFrameStep(state);
    // A camera that has never landed, or that stopped landing for a while, is a frame without the stage in it (not a failure); so is a frame without a fog
    // state (the engine's colour and density are what the air is made of). A missing SUN is none of these: the stage runs on the ambient term alone.
    if (!CameraStands(state.hold.stamps, state.frames))
    {
        NoteSkip(state, SkipReason::NoCamera);
        return nullptr;
    }
    if (!state.hold.haveFog)
    {
        NoteSkip(state, SkipReason::NoFog);
        return nullptr;
    }
    ID3D11ShaderResourceView* ours = RunStage(state, context, facts, scene);
    if (ours == nullptr)
    {
        NoteDecline(state, Decline::PassFailed, declineOutcome);
        return nullptr;
    }
    // The capture frame's pictures of this stage: the scene it was handed and the answer it made, copied now (its pass is over and its guards are gone).
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Atmosphere, context, scene, ours,
                               ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height, state.whitePoint });
    if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Atmosphere))
        NoteCaptureNumbers(state, facts);

    // Answered: the counters and the one line a session gets.
    state.runs++;
    state.counts.NoteRan(entry);
    state.streak.NoteRun();
    state.lastWindowWidth = uint32_t(facts.window.width);
    state.lastWindowHeight = uint32_t(facts.window.height);
    if (state.log.Take(0x80000003u | ((facts.sceneWidth & 0x7FFFu) << 11)))
    {
        RunFacts run;
        run.params = state.params;
        run.engineDensity = state.hold.engineDensity;
        run.engineScale = state.lastEngineScale;
        run.dayScale = state.lastDayScale;
        run.densityPerMetre = state.lastDensityPerMetre;
        run.haveSun = state.lastHaveSun;
        run.haveLight = state.lastHaveLight;
        run.sunGain = state.lastSunGain;
        run.lobeOnDisc = state.lastLobeOnDisc;
        run.elevationSine = state.hold.haveFogSun ? state.hold.fogSunY : state.hold.sunElevation;
        run.whiteStored = state.whitePoint;
        run.windowWidth = state.lastWindowWidth;
        run.windowHeight = state.lastWindowHeight;
        LOG_INFO("{}", FormatRun(run));
    }
    if (!state.loggedCost && state.pass.HasGpuMilliseconds())
    {
        state.loggedCost = true;
        LOG_INFO("{}", FormatCost(true, state.pass.GpuMilliseconds(), state.frames, state.runs, state.declined));
    }
    return ours;
}

// Depth of field behind this stage (ChainHooks.h): the chained role, asked with this stage's answer or with the texture this stage was given. Null when it
// is not linked or has nothing for the frame.
ID3D11ShaderResourceView* AskDof(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    if (scene == nullptr || ext::ffxiv::engine::chainDofScene == nullptr)
        return nullptr;
    return ext::ffxiv::engine::chainDofScene(context, facts, scene);
}

// The owner turn of depth of field, on exactly the contract this entry carries: true only when it drew; false leaves the game's draw to the caller.
bool AskDofOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    if (ext::ffxiv::engine::toneMapDofOwner != nullptr)
        return ext::ffxiv::engine::toneMapDofOwner(context, facts, scene, draw, user);
    return false;
}

// The owner's one bind: this module's copy of the scene (with depth of field composed on it) at t0 for the one draw, and the game's own view back afterwards,
// also when that draw throws.
void DrawWithBound(ID3D11DeviceContext* context, ID3D11ShaderResourceView* bind, DrawFn draw, void* user)
{
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &bind);
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
}

// The stage as the chain's fallback owner: makes the pass's answer, lets depth of field compose on it, binds the result for the game's one draw. True only if
// this stage drew; a decline or a skip draws nothing and asks depth of field's owner turn instead, outside the try (that stage catches everything itself, and
// a throw after it drew must not reach this stage's catch, which would draw the game's call a second time).
bool RunToneMapAsOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    bool drew = false;
    ID3D11ShaderResourceView* ours = nullptr;
    try
    {
        ours = StageAtToneMap(state, context, facts, scene, Outcome::EngineOwn, Entry::Owner);
        if (ours != nullptr)
        {
            ID3D11ShaderResourceView* composed = AskDof(context, facts, ours);
            DrawWithBound(context, composed != nullptr ? composed : ours, draw, user);
            drew = true;
            return true;
        }
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
        if (!drew)
            draw(user); // the game's draw, once, and not twice (the march's own catch)
        return true;
    }
    return AskDofOwner(context, facts, scene, draw, user);
}

// The stage as a chain member: the entry the light bounce reaches with the chain's current output. A texture comes back for the caller to bind at t0; null
// means nothing has anything for this frame, and the caller's own texture - the chain's last good picture - stays at t0 (the hold stands).
ID3D11ShaderResourceView* ChainSceneInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr || !Armed())
        return AskDof(context, facts, scene); // off: the chain is exactly the chain without this stage
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    // The chained role answers at most one tone map a frame, by this module's own counter: a second tone-shaped draw of the frame is the engine's own
    // post-tone copy, and the game's own call.
    if (state.haveAtmoFrame && state.atmoFrame == state.frames)
        return AskDof(context, facts, scene);
    state.haveAtmoFrame = true;
    state.atmoFrame = state.frames;
    ID3D11ShaderResourceView* ours = nullptr;
    try
    {
        ours = StageAtToneMap(state, context, facts, scene, Outcome::HoldStands, Entry::Chain);
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::HoldStands));
        ours = nullptr; // the chain's hold stands
    }
    if (ours != nullptr)
    {
        // Depth of field composes on this stage's answer, here, before any bind (the light bounce's own pattern one link further down).
        ID3D11ShaderResourceView* composed = AskDof(context, facts, ours);
        return composed != nullptr ? composed : ours;
    }
    return AskDof(context, facts, scene); // this stage has no answer: depth of field gets the texture this stage was handed
}

bool ToneMapOwnerInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    if (context == nullptr || scene == nullptr || draw == nullptr || !Armed())
        return AskDofOwner(context, facts, scene, draw, user);
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    if (state.haveAtmoFrame && state.atmoFrame == state.frames)
        return AskDofOwner(context, facts, scene, draw, user); // this frame's tone map was already answered, by this stage
    state.haveAtmoFrame = true;
    state.atmoFrame = state.frames;
    return RunToneMapAsOwner(context, facts, scene, draw, user);
}

// The tone map's own shape, measured at the draw (the march's own measurement, this stage's own recorded scene).
shafts::ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, State& state, ComPtr<ID3D11ShaderResourceView>& scene)
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
            // The scene the composite wrote has to be this frame's: the test is this module's own recorded frame, never the chain's shared stamp.
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

// The seam's own entry: the captures at the light draws, the fog post and the composite, and the tone map's fallback owner when none of the earlier stages
// took the draw (and, when this stage is armed first, the owner of the whole tail of the chain: it composes depth of field on its own answer).
bool SeamDraw(ID3D11DeviceContext* context, Anchors anchors, DrawFn draw, void* user)
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
        if (IsFogPostAnchors(anchors))
        {
            CaptureFogPost(context, anchors, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (IsShaftsLightAnchors(anchors))
        {
            CaptureLightDraw(context, anchors, state);
            return false;
        }
        if (IsShaftsSunParamAnchors(anchors))
        {
            CaptureSunParam(context, anchors, state);
            return false; // the sky's own sun draw is copied and passed on untouched (L2b)
        }
        if (IsShaftsCompositeAnchors(anchors))
        {
            RecordScene(context, state);
            return false;
        }
        if (IsShaftsToneMapAnchors(anchors))
        {
            // The shafts, the puddles, the march and the light bounce had their chances above this entry in the dispatch; reaching here with a tone map
            // means none took it, and this stage is the fallback owner. The shape is measured here, and a draw that is not the tone map is the game's call.
            ComPtr<ID3D11ShaderResourceView> scene;
            const shafts::ToneMapFacts facts = MeasureToneMap(context, state, scene);
            if (shafts::ClassifyToneMap(facts) == shafts::Decline::NotToneMap)
                return false;
            CloseFrameAtToneMap(state, facts, false);
            if (state.haveAtmoFrame && state.atmoFrame == state.frames)
                return false;
            state.haveAtmoFrame = true;
            state.atmoFrame = state.frames;
            // A false here lets the dispatch reach depth of field's own seam entry, which is the owner then (this stage's decline asked nothing of it).
            ID3D11ShaderResourceView* ours = nullptr;
            bool drew = false;
            try
            {
                ours = StageAtToneMap(state, context, facts, scene.Get(), Outcome::EngineOwn, Entry::Seam);
                if (ours != nullptr)
                {
                    ID3D11ShaderResourceView* composed = AskDof(context, facts, ours);
                    DrawWithBound(context, composed != nullptr ? composed : ours, draw, user);
                    drew = true;
                    return true;
                }
            }
            catch (...)
            {
                LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
                if (!drew)
                    draw(user);
                return true;
            }
            return false;
        }
    }
    catch (...)
    {
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
    state.params = BuildParams(cfg.atmosphereDensity.value_or_default(), cfg.atmosphereHeightFalloff.value_or_default(), cfg.atmosphereStart.value_or_default(),
                               cfg.atmospherePhaseG.value_or_default(), cfg.atmosphereSunStrength.value_or_default(),
                               cfg.atmosphereSkyStrength.value_or_default(), cfg.atmosphereNightDensity.value_or_default(), cfg.atmosphereNeutral.value_or_default());
    ObserveSceneHdr(state);
    if (!on)
    {
        state.streak = Streak {};
        state.log = LogGate {};
        state.fogLog = LogGate {};
        state.sourceLog = LogGate {};
        state.loggedOff = false;
        state.loggedCost = false;
        state.loggedStall = false;
        state.loggedStale = false;
        state.loggedDepth = false;
        state.loggedNoProfile = false;
        state.loggedRejected = false;
        state.loggedFogUnusable = false;
        state.step.Reset();
        state.stepLine = shafts::ChangeLine {};
        state.frameAge = state.lightDrawAge = state.fogAge = shafts::kRingNotLanded;
        state.sceneResource = nullptr;
        state.sunDrawTaken = false;
        state.cameraTaken = false;
        state.sunUv.Reset();
        state.lastLobeOnDisc = false;
        state.loggedSunDisc = false;
        state.haveAtmoFrame = false;
        state.atmoFrame = 0;
        state.hold = Hold {};
        state.counts.Reset();
        state.runs = 0;
        state.declined = 0;
        state.skips = SkipCounts {};
        state.rejectedLight = nullptr;
        state.rejectedFrame = 0;
        state.views.Reset();
    }
    else if (state.streak.off)
    {
        state.streak = Streak {}; // the player turned the switch back on: the counters and the streaks start again
        state.counts.Reset();
    }
    const bool wanted = Wanted(state.enabled, state.params) && state.streak.Armed();
    ext::SetActive(ext::F::Atmosphere, wanted);
    if (!wanted || state.hdr.inactive)
    {
        state.pass.Reset();
        state.frame.Reset();
        state.lightDraws.Reset();
        state.fog.Reset();
        state.tone.Reset();
        state.sunUv.Reset();
        state.views.Reset();
        state.hold = Hold {};
    }
}
} // namespace

void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    try
    {
        if (!MentionsProfile(code, size))
            return;
        const uint32_t slot = ProfileSlotOfShader(code, size);
        const uint64_t packed = godrays::PackProfileSlot(slot);
        if (slot == godrays::kNoProfileSlot || packed == 0)
            return; // 0 is "nothing stamped": writing it would only make an unstamped shader look stamped
        shader->SetPrivateData(kProfileStampTag, sizeof packed, &packed);
    }
    catch (...)
    {
    }
}

bool Armed()
{
    State& state = S();
    if (!Wanted(state.enabled, state.params) || !state.streak.Armed())
        return false;
    return !shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
}

void SampleSceneHdr()
{
    State& state = S();
    const uint64_t token = ext::present::FrameCount();
    if (state.haveHdrSample && state.hdrSampleFrame == token)
        return;
    state.haveHdrSample = true;
    state.hdrSampleFrame = token;
    ObserveSceneHdr(state);
    LogCountsIfDue(state); // the heartbeat: once an output frame while the stage is armed, so a stage no entry reaches still writes its line
}

bool OnSeamDraw(ID3D11DeviceContext* context, Anchors anchors, DrawFn draw, void* user)
{
    return SeamDraw(context, anchors, draw, user);
}

// The chained role, behind the light bounce's stage (ChainHooks.h's own hook pointer, this module's registration at start-up).
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    try
    {
        return ChainSceneInternal(context, facts, scene);
    }
    catch (...)
    {
        return nullptr; // nothing escapes into the light bounce's entries: the chain's hold stands
    }
}

// The fallback-owner role, behind the light bounce's stage. The owner contract: true only when the game's draw was drawn. The internals catch around
// everything that can run after the draw; an exception that gets out of them happened before it, so false (the caller draws once) is the honest answer.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    try
    {
        return ToneMapOwnerInternal(context, facts, scene, draw, user);
    }
    catch (...)
    {
        return false;
    }
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.armed = Armed();
    status.hdrInactive = shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
    status.params = state.params;
    status.frames = state.frames;
    status.runs = state.runs;
    status.declined = state.declined;
    status.skips = state.skips;
    status.engineDensity = state.hold.engineDensity;
    status.engineScale = state.lastEngineScale;
    status.densityPerMetre = state.lastDensityPerMetre;
    status.haveSun = state.lastHaveSun;
    status.windowWidth = state.lastWindowWidth;
    status.windowHeight = state.lastWindowHeight;
    return status;
}

void Publish() { Apply(ext::Cfg().ffxiv.atmosphere.value_or_default()); }

void OnConfigLoaded(bool on) { Apply(on); }

void Release()
{
    State& state = S();
    state.pass.Reset();
    state.frame.Reset();
    state.lightDraws.Reset();
    state.fog.Reset();
    state.tone.Reset();
    state.sunUv.Reset();
    state.views.Reset();
    state.sceneResource = nullptr;
    state.hold = Hold {};
}

// The stage dump's two questions about this stage, asked at the capture frame: is it armed (Armed()), and what are its values.
std::string StageValues() { return FormatStageValues(S().params); }
} // namespace ext::ffxiv::engine::atmo

namespace
{
// The registration of the config hook and the chain's stage hooks, set when this DLL's static initialisers run: the [FFXIV] Atmosphere key arms the seam's
// feature bit at every config load, and the light bounce's entries reach the stage through the pointers the chain's own header declares - null in every build
// that does not link this file, which is the stage being off.
struct RegisterAtmo
{
    RegisterAtmo()
    {
        ext::FfxivConfig::atmosphereOnLoaded = &ext::ffxiv::engine::atmo::OnConfigLoaded;
        ext::ffxiv::engine::chainAtmoScene = &ext::ffxiv::engine::atmo::ChainScene;
        ext::ffxiv::engine::toneMapAtmoOwner = &ext::ffxiv::engine::atmo::ToneMapOwner;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Atmosphere, &ext::ffxiv::engine::atmo::Armed, &ext::ffxiv::engine::atmo::StageValues);
    }
} g_registerAtmo;
} // namespace
