#include "pch.h"

#include "ext/ffxiv/engine/Shafts.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/engine/ChainHooks.h"   // the tone-map chain's stage hooks: the march, the chain's last member
#include "ext/ffxiv/engine/EnginePass.h"    // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/PuddleTone.h"      // the puddles' tone-map stage, which runs on this pass's output before t0 is rebound
#include "ext/ffxiv/engine/ShaftsPass.h"
#include "ext/ffxiv/engine/ShaftsSun.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"    // the scene HDR path replaces the tone chain this pass draws before
#include "ext/present/PresentSeams.h"       // the output-frame counter, the same token the wetness patch's cache uses
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame

#include <d3d11.h>
#include <wrl/client.h>

#include <thread>

// Light shafts before the tone map: the draw path, the frame's sun and scene readings and the module's session (Shafts.h). The pure decisions
// are in ShaftsRules.h, the frame's light and camera bytes in ShaftsSun.cpp, the four draws in ShaftsPass.cpp, the shaders in
// shaders/LightShafts.hlsl, the card in ShaftsMenu.cpp.
//
// What it costs while it is off: the engine module's one call per draw into OnEngineDraw, which returns at one test of the stored key and
// params - no state query, no allocation and no log line. While it is on, per frame: the light draws up to the first one with the sun draw's
// shape are measured (a few state and resource reads), one pair of small copies is issued for it, and at the tone map the newest copy the ring
// still holds of the frames before it is mapped without ever waiting for the GPU; at the tone-map draw, one draw's state is measured, the pass
// runs its four full-screen draws and the game's draw runs with this module's copy of the scene at its t0.
//
// Nothing of the game is written: the scene is copied into an owned texture, and the tone map's own binding is put back after its one draw,
// also when that draw throws.
namespace ext::ffxiv::shafts
{
namespace
{
using Microsoft::WRL::ComPtr;
using ext::ffxiv::engine::kConsecutiveDeclineLimit;
using ext::ffxiv::engine::Knob;
using ext::ffxiv::engine::Session;

// A frame makes about fifty light draws, of which five are the wetness mask's volumes before the sun's own draw. The frame's light draws are
// measured until the first one with the sun draw's shape, bounded here so a build that never matches costs a bounded number of measurements a
// frame instead of one per light draw.
inline constexpr uint32_t kMaxCaptureAttempts = 24;

// The render targets a draw is measured at: D3D11's own maximum, so a draw with more than the light-buffer pair is seen as such.
inline constexpr uint32_t kMaxRenderTargets = 8;

// The tone map's own parameter buffer (`cCommonTexParam`) sits in its pixel shader's first constant slot: a captured tone-map shader's reflection
// lists it at 0 and the merge weight at 1. The module has no name table of its own for that buffer, so the slot is this constant and the buffer is
// judged by its content - an exposure multiplier in the range a frame's tone map can produce (ShaftsRules.h ExposureUsable).
inline constexpr uint32_t kToneMapSlot = 0;

// This frame's sun light draw, as the draw itself measured it: the depth the pass builds its mask from, and the rendered frame it belongs to.
// The light's parameters and the camera's matrices come from ShaftsSun.h, from the newest copy the ring still holds of the frames before this
// one.
struct SunDraw
{
    bool taken = false;
    uint64_t frames = 0;                    // the rendered frame the light draw belongs to (ShaftsRules.h AdvanceFrame)
    ComPtr<ID3D11ShaderResourceView> depth; // the game's depth, read only
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
};

struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    Colour colour = Colour::Auto;
    bool enabled = false;
    SunReader sun;  // the light's bytes and the projection, read a ring of frames late
    ToneReader tone; // the tone map's own exposure multiplier, read a frame late: the frame's white point
    SunUvReader sunUv;    // the sky's own sun parameters, read a frame late: the sun disk's screen UV
    float sunUvRow[4] {}; // the last landed row: (aspect, 1.0, sunUvX, sunUvY)
    bool haveSunUv = false;
    uint64_t sunUvFrame = 0; // the rendered frame that row was written at (ShaftsRules.h SunUvFresh)
    bool loggedSunUv = false;
    SunDraw sunDraw; // the frame's sun draw and its depth
    const void* rejectedLight = nullptr; // a light the read-back showed at a world position: a lamp, never the sun
    uint64_t rejectedFrame = 0;          // the rendered frame it was refused at: the buffer pool reuses the pointer
    uint32_t attempts = 0;
    uint64_t frames = 0;     // the module's own count of rendered frames: what the ring's ages and every "same frame" test are counted in
    bool frameClosed = false; // the frame in progress was closed at its tone map, so the next light draw or composite opens a new count
    // The last sun a read accepted, and the camera it was measured with: the frame's light when its own read has not landed yet (ShaftsRules.h
    // HoldFade). The camera of every read that lands is kept apart, so a cut is seen even on a frame whose light was refused - the frame a held sun
    // must never be used on is the one where the view moved.
    SunHold hold;                  // the last sun a read accepted, and the frame it was accepted at (ShaftsRules.h SunHold)
    const void* heldLight = nullptr; // the game's buffer that sun came from, for the run line
    float lastCamera[kProjectionRows] {}; // the newest camera a read landed: the view-projection CameraCut reads
    bool haveLastCamera = false;
    float lastProjection[kProjectionRows] {}; // the same camera's projection: what a held sun's view-space direction is projected through
    float lastView[kViewRows] {};             // and its view, which turns a held sun's world direction into that camera's view space
    bool loggedCameraCut = false;
    // The frame's white point (ShaftsRules.h WhitePointOf): the frame's own exposure when it has arrived, the captured frame's measurement until
    // then.
    float whitePoint = kFallbackWhitePoint;
    bool haveWhitePoint = false;
    uint64_t whiteFrame = 0; // the rendered frame the live white point was read at: past kMaxWhiteStaleFrames without a landing it is not this frame's
    bool loggedWhitePoint = false;
    bool loggedWhitePointFallback = false;
    // The frame's lit scene: the deferred composite's single render target, which the tone map then reads as its t0.
    void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;
    // The frame's first draw with the tone map's shape is the one the module looks at.
    uint64_t toneMapFrames = 0;
    bool haveToneMapFrame = false;
    // What the card shows and what the log says.
    uint32_t runs = 0;
    uint32_t heldRuns = 0; // of those, the frames drawn with the last accepted sun because this frame's own read had not landed
    uint32_t declined = 0;
    CardCounts skips;            // the frames left alone without the shafts, and which of the four reasons each was (ShaftsRules.h CountSkip)
    uint64_t belowHorizonFrame = 0; // the rendered frame whose light the read-back refused as the sun's mirror
    bool haveBelowHorizonFrame = false;
    uint32_t notSunRefusals = 0; // the read-back's refusals by the frame's own shadow: its own count, on drawn frames too
    uint64_t notSunFrame = 0; // the rendered frame whose light the read-back refused against the frame's directional shadow
    bool haveNotSunFrame = false;
    ShadowJudgement sunShadow; // what the frame's shadow said about the light the pass draws with (the last accepted read's)
    uint32_t declines[kDeclineReasonCount] {};
    LightFacts lastFacts; // the last sun the pass ran with, for the card
    SunProjection lastSun;
    Window lastWindow;
    uint64_t nextCountsLog = 0; // the rendered frame the card's numbers are written to the log on, kCountsLogFrames apart
    bool loggedOff = false;
    bool loggedCost = false;
    bool loggedRejected = false;
    bool loggedBelowHorizon = false;
    bool loggedNotTheSun = false;
    bool loggedShadowMissing = false;
    bool loggedShadowUnnegated = false;
    bool loggedProjection = false;
    bool loggedStale = false;
    bool loggedCamera = false;
    bool loggedStall = false;
    SceneHdrWatch hdr;           // the engine's scene HDR path as the frame's sample found it: the log line's own state
    uint64_t hdrSampleFrame = 0; // the output frame the state was last sampled in
    bool haveHdrSample = false;
    std::thread::id renderThread;
    bool haveRenderThread = false;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The rendered frame in progress (ShaftsRules.h AdvanceFrame): the frame's first light draw, or its composite, opens a new count once the frame
// before it has closed at its tone map. Nothing else moves the count, and in particular the output-present counter does not: frame generation
// presents a generated image while the game renders the next frame, so that counter steps inside one rendered frame and a "same frame" test on
// it drops the frames a present landed in.
void EnterFrame(State& state)
{
    if (!state.frameClosed)
        return;
    state.frameClosed = false;
    AdvanceFrame(state.frames);
    state.attempts = 0;
}

// The frame's tone map is its last draw here: the frame is over, so the next light draw or composite opens the next count. A frame that drew no
// classified tone map never closes: the count stays where it is, the next frame's light draw sees its own count and is not taken again, and that
// frame's tone map then composites with the light draw and depth this frame stored - a stale-frame path this count does not remove.
void CloseFrame(State& state) { state.frameClosed = true; }

// The deferred composite's single render target: the lit scene the tone map reads. Recorded here because the composite is the pass that writes
// it, which is what makes the scene recognisable without a shader CRC or an address.
void RecordScene(ID3D11DeviceContext* context, State& state)
{
    EnterFrame(state);
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
            if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && IsSceneFormat(uint32_t(viewDesc.Format)))
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

// The shape of a light draw: its render targets (the light-buffer pair), the blend state it writes them with, and the depth and camera bindings
// the pass reads. It is measured only for a light draw of the frame - the sun draw is the one light draw of the frame that does not blend into the
// pair - so a wrong shape costs one refusal-free pass, never a frame.
SunDrawFacts MeasureLightDraw(ID3D11DeviceContext* context, const State& state, bool depth, bool camera)
{
    SunDrawFacts shape;
    shape.depth = depth;
    shape.camera = camera;
    shape.sceneWidth = state.sceneWidth;
    shape.sceneHeight = state.sceneHeight;

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

    // The blend state the draw writes the pair with: the sun's pass writes the frame's directional light and the local lights accumulate on top
    // of it, so a light draw that blends is one of those. No state bound is D3D11's default, which does not blend. With independent blending off
    // only entry 0 applies to every target, which is also the entry a capture reports: a build that fills the unused entries with TRUE must not
    // make the sun's pass read as blended, or the module would skip every frame as "no depth" without counting.
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
    return shape;
}

// One light draw of the frame. The sun's draw is taken when its own shape says so (the light-buffer pair, written without blending, beside the
// frame's depth and the camera), and only the first such draw of a frame: the frame makes two light passes, the second into another pair, and
// the composite reads the first pass's buffers. The light's bytes and the camera's are copied GPU side here and read at a later frame's tone map,
// so the direction and the projection arrive late and the depth is this frame's.
void Capture(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    EnterFrame(state);
    if (state.sunDraw.taken && SameRenderedFrame(state.sunDraw.frames, state.frames))
        return; // this frame's sun draw is taken
    if (state.attempts >= kMaxCaptureAttempts)
        return;
    ++state.attempts;
    const uint32_t lightSlot = ext::ffxiv::engine::ShaftsSlotOf(anchors);
    if (lightSlot >= ext::ffxiv::engine::kConstantBufferSlots)
        return;

    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ID3D11ShaderResourceView* views[16] {};
    context->PSGetShaderResources(0, 16, views);
    // The seam's read add-refs every object it returns: taking them into ComPtrs owns exactly those references, so every early return below
    // gives them back.
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11ShaderResourceView> textures[16];
    for (uint32_t i = 0; i < 16; ++i)
        if (views[i] != nullptr)
            textures[i].Attach(views[i]);

    ComPtr<ID3D11Buffer> light = bound[lightSlot];
    if (light == nullptr)
        return;
    // A light whose parameters were read at a world position is a lamp: the frames after it look at the next light draw instead of this one, for
    // a few seconds. The engine's light parameter buffers are a small pool reused within the frame, so the pointer alone would block a buffer the
    // sun's own draw uses later in the session.
    if (static_cast<const void*>(light.Get()) == state.rejectedLight &&
        RejectedStill(state.rejectedLight != nullptr, state.rejectedFrame, state.frames))
        return;

    // The camera: the buffer that can hold the camera the module reads (the view, the view-projection and the projection), besides the light's
    // own buffer. The engine binds g_CameraParameter at 1,024 bytes on the light draws (the module's name table carries the sun's buffer only, so
    // the camera is recognised by the size it must have and by the matrices it then has to contain).
    ComPtr<ID3D11Buffer> camera;
    uint32_t cameraBytes = 0;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == light.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth < kCameraBytes || desc.ByteWidth > 0x400 || desc.ByteWidth <= cameraBytes)
            continue;
        cameraBytes = desc.ByteWidth;
        camera = buffer;
    }
    if (camera == nullptr)
        return;

    // The depth: a single-sample 2D view of the engine's depth buffer, at a size the render window can live in. A light draw can bind more than
    // one view this pass reads the shape of (a shadow map or a reflection's depth in a lower slot is one), so a view at the scene's own size
    // wins over the first one: the mask is read in the scene's pixels, and a depth of another size would be refused by the tone map on every
    // frame of such a session.
    struct DepthView
    {
        ComPtr<ID3D11ShaderResourceView> view;
        uint32_t format = 0, width = 0, height = 0;
        bool matchesScene = false;
    } best, found;
    for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
    {
        if (!texture)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        texture->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !IsDepthFormat(uint32_t(viewDesc.Format)))
            continue;
        ComPtr<ID3D11Resource> resource;
        texture->GetResource(&resource);
        ComPtr<ID3D11Texture2D> asTexture;
        if (resource == nullptr || FAILED(resource.As(&asTexture)) || asTexture == nullptr)
            continue;
        D3D11_TEXTURE2D_DESC desc {};
        asTexture->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
            continue;
        found.view = texture;
        found.format = uint32_t(viewDesc.Format);
        found.width = desc.Width;
        found.height = desc.Height;
        found.matchesScene = state.sceneWidth != 0 && desc.Width == state.sceneWidth && desc.Height == state.sceneHeight;
        if (BetterDepthView(best.view != nullptr, best.matchesScene, found.matchesScene))
            best = found;
    }
    ComPtr<ID3D11ShaderResourceView> depth = best.view;
    const uint32_t depthFormat = best.format, depthWidth = best.width, depthHeight = best.height;

    if (!IsSunDraw(MeasureLightDraw(context, state, depth != nullptr, camera != nullptr)))
        return;
    if (depth == nullptr)
        return;

    // The light draw's 128-byte parameter buffers: the frame's `g_DirectionalShadowParameter` is one of them, and the rules pick the buffer whose
    // third row is this light's own axis (ShaftsRules.h JudgeShadow). A captured frame's sun draw binds two of that size (the shadow's and
    // g_FakeSpecularParam's), so the copy takes every candidate and the check is on the content, never on a slot number.
    ID3D11Buffer* shadows[kShadowCandidates] {};
    uint32_t shadowCount = 0;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == light.Get() || buffer.Get() == camera.Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth != kShadowBytes)
            continue;
        if (shadowCount < kShadowCandidates)
            shadows[shadowCount++] = buffer.Get();
    }

    if (!state.sun.Copy(context, light.Get(), camera.Get(), state.frames, shadows, shadowCount))
        return;
    state.sunDraw.depth = depth;
    state.sunDraw.depthFormat = depthFormat;
    state.sunDraw.depthWidth = depthWidth;
    state.sunDraw.depthHeight = depthHeight;
    state.sunDraw.frames = state.frames;
    state.sunDraw.taken = true;
}

// What one frame's read of the light came to: a sun the frame can draw with, a light the rules refused as the sun, or nothing that landed (a GPU
// that is behind, or a frame whose light draw was not the sun's shape).
enum class Freshness : uint8_t
{
    None = 0,
    Sun = 1,
    Refused = 2,
};

// What the copies said, read without waiting for the GPU: the light's bytes and the camera's come from the newest copy the ring still holds of
// the frames before this one, and the last good values stay until a newer read lands (a map that is not ready is a GPU that is behind, not a
// refusal). An accepted sun is also what the frames after it draw with when their own read has not landed (ShaftsRules.h HoldFade), and the camera
// of every read that lands is compared with the one before it: a cut drops the held sun, because a sun from before a cut would light the new
// picture from the old one's sky.
Freshness Readback(ID3D11DeviceContext* context, State& state)
{
    const SunRead read = state.sun.Read(context, state.frames);
    if (!read.light)
        return Freshness::None;
    // The camera of any read that landed, before the light's verdict: the camera buffer is the frame's camera whatever the light turned out to be,
    // and a held sun must not survive a cut even on a frame whose light was refused. The rows of the copy are the held sun's own if this frame's
    // read missed (a missed light read misses the camera too - the documented limit: a cut during a missed read is seen when a read lands).
    if (state.sun.HaveProjection())
    {
        if (state.haveLastCamera && CameraCut(state.lastCamera, state.sun.ViewProjection()))
        {
            state.hold.Drop();
            // The sky's own sun was drawn with the camera before the cut: placing the new picture's shafts with it would put them where the old
            // view had the sun, so it is dropped with the held sun and the frames after the cut project the light until a copy lands.
            state.haveSunUv = false;
            if (!state.loggedCameraCut)
            {
                state.loggedCameraCut = true;
                LOG_INFO("{}", kCameraCut);
            }
        }
        for (uint32_t i = 0; i < kProjectionRows; ++i)
        {
            state.lastCamera[i] = state.sun.ViewProjection()[i];
            state.lastProjection[i] = state.sun.Projection()[i];
        }
        for (uint32_t i = 0; i < kViewRows; ++i)
            state.lastView[i] = state.sun.View()[i];
        state.haveLastCamera = true;
    }
    // The light's direction is in view space, so "above the horizon" is its WORLD elevation: the view's second row dotted with it (ShaftsRules.h
    // WorldElevationOf). A host test that supplies no view keeps the direction's own y, which is the same answer while the camera is level.
    const float elevation = state.sun.HaveView() ? WorldElevationOf(state.sun.View(), state.sun.Facts().direction)
                                                 : state.sun.Facts().direction[1];
    const LightVerdict verdict = ClassifyLight(state.sun.Facts(), elevation);
    // The frame's own directional shadow is the identity the light has to pass once its own parameters say "a directional light above the
    // horizon": the shape rule cannot tell the sun from another directional light, and a light that is a yaw of the sun draws shafts where the
    // sun is not (ShaftsRules.h JudgeShadow). Its row is the light's axis in the same view space the light's own direction is in (a captured
    // frame carries the very same three floats), so the two are compared as they are; the rows were copied with the light, so they are the same
    // frame's.
    ShadowJudgement shadow;
    if (verdict == LightVerdict::Sun)
    {
        shadow = JudgeShadow(state.sun.Shadows(), state.sun.ShadowCount(), state.sun.Facts().direction);
        if (shadow.result == ShadowResult::Missing && !state.loggedShadowMissing)
        {
            state.loggedShadowMissing = true;
            LOG_INFO("{}", kShadowAnchorMissing);
        }
        if (shadow.result == ShadowResult::AgreedUnnegated && !state.loggedShadowUnnegated)
        {
            state.loggedShadowUnnegated = true;
            LOG_INFO("{}", kShadowUnnegated);
        }
    }
    if (verdict != LightVerdict::Sun || shadow.result == ShadowResult::Refused)
    {
        // A light at a world position is a lamp, a direction that is not unit is a light that is off or a layout this build no longer has, a
        // directional light pointing below the horizon is the sun's mirror - the value another directional light draw of the frame carries, and
        // the one that put the shafts behind the camera while the sun was on screen - and a directional light whose direction is not the one the
        // frame's own shadow is built from is a light that is not this frame's sun. None of them is this frame's sun, so the read is dropped and
        // the frame is left as it was. A below-horizon light does not send the frames after it looking at another light: the first sun-shaped draw
        // of a frame is the frame's directional light, so a direction below the horizon means this frame has no sun above it, not that the frame's
        // next light draw is the one to take. Only a lamp is remembered by its buffer - the engine's light parameter buffers are a pool, so the
        // pointer is remembered for a few seconds and then looked at again.
        if (verdict == LightVerdict::Lamp && !state.loggedRejected)
        {
            state.loggedRejected = true;
            LOG_INFO("{}", FormatNotTheSun(state.sun.Facts()));
        }
        if (verdict == LightVerdict::BelowHorizon)
        {
            // The refusal is remembered for this frame, not counted here: the frame is counted once, below, as a frame left alone for the light
            // being the sun's mirror (CountSkip), so the card's numbers are the whole of its `skipped`.
            state.belowHorizonFrame = state.frames;
            state.haveBelowHorizonFrame = true;
            if (!state.loggedBelowHorizon)
            {
                state.loggedBelowHorizon = true;
                LOG_INFO("{}", FormatBelowHorizon(state.sun.Facts()));
            }
        }
        if (shadow.result == ShadowResult::Refused)
        {
            // The light's own direction against the frame's shadow row, once a session: it is the measurement that says which light the frame
            // drew, and the count of frames this refused is the card's own "not the frame's sun" number.
            state.notSunFrame = state.frames;
            state.haveNotSunFrame = true;
            ++state.notSunRefusals;
            if (!state.loggedNotTheSun)
            {
                state.loggedNotTheSun = true;
                LOG_INFO("{}", FormatShadowRefused(state.sun.Facts().direction, shadow.degrees));
            }
        }
        if (verdict == LightVerdict::Lamp)
        {
            state.rejectedLight = state.sun.LastLight();
            state.rejectedFrame = state.frames;
        }
        state.sun.Forget();
        state.sunDraw.taken = false;
    }
    if (verdict != LightVerdict::Sun || shadow.result == ShadowResult::Refused)
        return Freshness::Refused;
    if (read.projection && !state.loggedProjection)
    {
        state.loggedProjection = true;
        LOG_INFO("{}", FormatProjection(state.sun.Projection()));
        LOG_INFO("{}", FormatViewProjection(state.sun.ViewProjection()));
    }
    // The shadow's answer for the light the pass will draw with: the run line names it, so a session's own frame says whether the light it read
    // was the sun by the frame's own matrix (an angle of a few degrees) or by its direction alone (no row).
    state.sunShadow = shadow;
    // The frame's sun, kept for the frames whose own read has not landed. Its direction and the camera it was measured with come from one copy, so
    // a missed light read misses the camera too; the hold projects the direction with the newest camera that did land (see RunToneMap), and a cut
    // seen by any landed camera drops the whole hold.
    if (state.sun.HaveProjection())
    {
        // The held copy is the light as the WORLD sees it: the frame that uses it may have turned, and a view-space vector would then point at a
        // screen position that belongs to the view it was measured in (the hold is what a missed read draws with, so it is used across a turn).
        float world[3] { 0.0f, 0.0f, 0.0f };
        if (state.sun.HaveView() && WorldDirectionOf(state.sun.View(), state.sun.Facts().direction, world))
            state.hold.Accept(state.sun.Facts(), world, state.frames);
        state.heldLight = state.sun.LastLight();
    }
    return Freshness::Sun;
}

// The sky's own sun draw: its parameter buffer's first row is the sun disk's own screen position, which is where a player sees the sun. The draw is
// not a light draw (its shader declares cSunParam and no g_LightParam), so nothing else is taken from it. The copy is issued here and read at a
// later frame's tone map, because a copy is never mapped at the frame that made it.
void CaptureSunParam(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    const uint32_t slot = ext::ffxiv::engine::ShaftsSlotOf(anchors);
    ID3D11Buffer* buffer = nullptr;
    context->PSGetConstantBuffers(slot, 1, &buffer);
    ComPtr<ID3D11Buffer> params;
    if (buffer != nullptr)
        params.Attach(buffer); // the reference the read took
    if (!params)
        return;
    D3D11_BUFFER_DESC desc {};
    params->GetDesc(&desc);
    // The engine binds it at 128 bytes and this module reads its first 16: a buffer of another size is another draw's parameters, and the frame
    // keeps the sun it placed the shafts with last rather than reading a row that is not one.
    if (desc.ByteWidth < kSunParamBytes || desc.ByteWidth > 0x100)
        return;
    state.sunUv.Copy(context, params.Get(), state.frames);
}

// The frame's own tone map parameters, copied at the tone map draw and read a frame or more late: `cCommonTexParam`'s exposure multiplier is
// what the frame's white point is the reciprocal of, inside the band the captured measurement supports (ShaftsRules.h WhitePointFromExposure).
// The copy is never waited for; the last good white point stays while it is this frame's, and after kMaxWhiteStaleFrames without a landed copy -
// or before the first one - the bound is the captured frame's own measurement, with one line saying so. A drawn frame is a frame whose sun is in
// view, and the session's own reads of those frames were all a white of about 0.84, so the fallback is that measurement: the band's dark end left
// `head = white - scene` at zero on every lit surface of a daylight frame, which is a screenshot with the shafts drawn and nothing added.
void ReadToneMapParams(ID3D11DeviceContext* context, State& state)
{
    ID3D11Buffer* buffer = nullptr;
    context->PSGetConstantBuffers(kToneMapSlot, 1, &buffer);
    ComPtr<ID3D11Buffer> params;
    if (buffer != nullptr)
        params.Attach(buffer); // the reference the read took
    if (params)
        state.tone.Copy(context, params.Get(), state.frames);
    const ToneRead read = state.tone.Read(context, state.frames);
    if (read.landed && ExposureUsable(read.exposure))
    {
        const float whitePoint = WhitePointFromExposure(read.exposure);
        if (WhitePointUsable(whitePoint))
        {
            state.whitePoint = whitePoint;
            state.whiteFrame = state.frames;
            state.haveWhitePoint = true;
            if (!state.loggedWhitePoint)
            {
                state.loggedWhitePoint = true;
                LOG_INFO("{}", FormatWhitePoint(whitePoint, read.exposure));
            }
            state.loggedWhitePointFallback = false;
            return;
        }
    }
    if (WhitePointStale(state.haveWhitePoint, state.whiteFrame, state.frames))
    {
        state.haveWhitePoint = false;
        state.whitePoint = kFallbackWhitePoint;
        if (!state.loggedWhitePointFallback)
        {
            state.loggedWhitePointFallback = true;
            LOG_INFO("{}", kWhitePointFallback);
        }
    }
}

// The tone map's own shape, measured at the draw: one FP16 render target at the scene's size, and a t0 that is the scene the composite wrote.
ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, const State& state, ComPtr<ID3D11ShaderResourceView>& scene)
{
    ToneMapFacts facts;
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
            // The scene the composite wrote has to be this frame's: a frame with no composite of its own (a menu, a cut) would otherwise match
            // a scene resource left over from an earlier frame and let the pass draw over a picture that is not the one it measured.
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

// The game's tone-map draw with the puddles' stage chained in front of it, and the volumetric march behind that: the chain's order is the
// shafts, the puddles' pools, the march, with the tone map's t0 bound exactly once by whoever is last. A frame this pass leaves as it was - the
// sun behind the camera or outside the margin, no sun draw of the frame, a light that is the sun's mirror, a camera this build cannot read -
// still has a scene the pools must be drawn into: the stage's own fallback steps aside while the light shafts own the tone map, so the chain has
// to run here or turning Light Shafts on would kill the puddles whenever the shafts had nothing to add. The stage's own concerns decide: it
// returns null when its card is off, the frame is dry, or it cannot serve the frame's shape, and the game's draw then runs on the shafts' own (or
// the engine's) scene. When the stage is not armed and the march is, the march is the chain's fallback owner: it makes its own scene copy of
// the game's scene, runs its pass, and binds its output at t0 for the one draw. The binding is put back afterwards, also when the draw throws,
// so one module owns t0 for exactly one draw.
bool DrawToneMapWithChain(ID3D11DeviceContext* context, const ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user,
                          bool& drew)
{
    if (drew)
        return true;
    drew = true;
    if (context != nullptr && scene != nullptr && ext::ffxiv::engine::gloss::ToneArmedForChain())
    {
        // The stage runs on the shafts' own copy when it has anything to do, and the game's draw reads this module's texture for that one draw;
        // when it has nothing to do the draw reads the scene it was handed. Either way the game's binding is back afterwards.
        ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context, facts, scene, draw, user);
        return true;
    }
    if (context != nullptr && scene != nullptr && ext::ffxiv::engine::toneMapMarchOwner != nullptr)
    {
        // The march, the chain's fallback owner: neither earlier stage answered, so it makes its own scene copy and binds t0 for the one draw.
        // The hook answers true only when it drew the game's call; false (the march is off, it already answered this frame's tone map, or it
        // declined) means nothing was drawn, and the game's call below runs on the binding it already has - exactly once either way.
        if (ext::ffxiv::engine::toneMapMarchOwner(context, facts, scene, draw, user))
            return true;
    }
    draw(user);
    return true;
}

// One refused tone-map draw: the per-reason count, one line per change of reason, the module's own refusal streak, and the game's own draw.
bool DeclineAndDraw(ID3D11DeviceContext* context, const ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user, bool& drew,
                     Decline reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Shafts, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", FormatDecline(reason));
    if (state.session.NoteDecline(Knob::LightShafts) && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("{}", FormatSwitchedOff(kConsecutiveDeclineLimit));
        ext::SetActive(ext::F::LightShafts, false); // the draw path is back to one relaxed load
    }
    return DrawToneMapWithChain(context, facts, scene, draw, user, drew);
}

// One frame left as it was without counting anything against the module: no light draw of this frame had the sun draw's shape, its bytes have
// not arrived yet, or they are not a sun. The frame has no shafts in it, which is not a failure, so the streak is untouched and the line says why.
bool SkipAndDraw(ID3D11DeviceContext* context, const ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user, bool& drew,
                  Decline reason, uint32_t tag)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Shafts, ext::ffxiv::diag::StageOutcome::Declined,
                                  reason == Decline::None ? "the sun or camera read has not landed yet" : DeclineName(reason));
    State& state = S();
    // The three reasons a frame has no shafts in it are counted through the one function that keeps `skipped` and the reason in step, so the card
    // can say which one a session hit and its four numbers stay the whole of the count.
    if (reason == Decline::NoDepth)
        CountSkip(state.skips, SkipReason::NoDepth);
    else if (reason == Decline::NoSun)
        CountSkip(state.skips, SkipReason::NoSun);
    if (state.log.Take(tag))
        LOG_INFO("{}", FormatSkip(reason));
    return DrawToneMapWithChain(context, facts, scene, draw, user, drew);
}

// One frame left as it was because the read-back refused its light as the sun's mirror: one refusal, counted once as that reason, and its own
// line (the read-back's line named the light once a session; this one says why the frame has no shafts, once per run of such frames).
bool SkipRefusedAndDraw(ID3D11DeviceContext* context, const ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user,
                        bool& drew)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Shafts, ext::ffxiv::diag::StageOutcome::Declined,
                                  "the read-back refused the light as the sun's mirror (below the horizon)");
    State& state = S();
    CountSkip(state.skips, SkipReason::BelowHorizon);
    if (state.log.Take(0x40000008u))
        LOG_INFO("{}", kBelowHorizonSkip);
    return DrawToneMapWithChain(context, facts, scene, draw, user, drew);
}

// One frame left as it was because the frame's own directional shadow refused its light: the same shape as the mirror's refusal, its own reason
// and its own line, so the card says how many frames a light that is not the sun cost a session.
bool SkipNotTheSunAndDraw(ID3D11DeviceContext* context, const ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user,
                          bool& drew)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Shafts, ext::ffxiv::diag::StageOutcome::Declined,
                                  "the frame's directional shadow refused the light (it is not the sun)");
    State& state = S();
    CountSkip(state.skips, SkipReason::NotTheSun);
    if (state.log.Take(0x40000009u))
        LOG_INFO("{}", kNotTheSunSkip);
    return DrawToneMapWithChain(context, facts, scene, draw, user, drew);
}

// The card's numbers into the log about once a minute of rendered frames. It is called from the frame's close, so a session is counted whatever
// its frames came to - the version that wrote the line only when a frame drew wrote nothing at all in the 2-of-1272 session it exists for. The
// module's own frame count is printed with the counts, so the share is the sum over it rather than a number a session has to derive.
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
    numbers.noDepth = state.skips.noDepth;
    numbers.noSun = state.skips.noSun;
    numbers.offScreen = state.skips.offScreen;
    numbers.belowHorizon = state.skips.belowHorizon;
    numbers.notTheSun = state.skips.notTheSun;
    numbers.reads = state.sun.Reads();
    numbers.notReady = state.sun.NotReady();
    numbers.refused = state.notSunRefusals;
    numbers.sunX = state.lastSun.screenX;
    numbers.sunY = state.lastSun.screenY;
    numbers.windowWidth = uint32_t(state.lastWindow.width);
    numbers.windowHeight = uint32_t(state.lastWindow.height);
    LOG_INFO("{}", FormatCard(numbers));
}

bool RunToneMap(ID3D11DeviceContext* context, DrawFn draw, void* user)
{
    State& state = S();
    bool drew = false;

    // The game's draw runs exactly once, whatever happens below: a throw after it must not let the seam run it a second time.
    try
    {
        ComPtr<ID3D11ShaderResourceView> scene;
        const ToneMapFacts facts = MeasureToneMap(context, state, scene);
        const Decline shape = ClassifyToneMap(facts);
        if (shape == Decline::NotToneMap)
            return false; // another draw that declares the tone map's parameter buffer, or one that writes the scene back: the game's own
                          // call, and nothing counted against the module - a build that shares that name with a later post pass must not stand
                          // the pass down in the first frames
        // The first draw of a frame with the tone map's shape is the one this module looks at. The engine reuses the scene texture for its own
        // post-tone copy, and a later draw of the same shape would read that copy; the frames after the first are the game's own call.
        if (state.haveToneMapFrame && state.toneMapFrames == state.frames)
            return false;
        state.haveToneMapFrame = true;
        state.toneMapFrames = state.frames;
        // The frame is over whatever this draw comes to, so the frame count moves on at the next frame's first light draw or composite. The
        // close is a scope guard so a throw below closes the frame as well: a frame left open would make the next frame's light draw look like
        // this one's.
        struct Close
        {
            State& state;
            ~Close()
            {
                CloseFrame(state);
                LogCountsIfDue(state);
            }
        } close { state };

        if (shape != Decline::None)
            return DeclineAndDraw(context, facts, scene.Get(), draw, user, drew, shape);

        // The frame's white point, from the tone map's own parameters at this draw (copied here, read at the next frame's tone map).
        ReadToneMapParams(context, state);
        // The sky's own sun parameters, the same way: when the row lands it is this frame's visual sun for kHoldFrames (ShaftsRules.h SunUvFresh),
        // and it is what places the shafts when the light's clamped direction does not point where the sun is drawn.
        const SunUvRead uv = state.sunUv.Read(context, state.frames);
        if (uv.landed && SunParamUvUsable(uv.uv))
        {
            for (uint32_t i = 0; i < 4; ++i)
                state.sunUvRow[i] = uv.uv[i];
            state.sunUvFrame = uv.writtenFrame;
            state.haveSunUv = true;
        }
        // The light this frame read: a sun it can draw with, a light the rules refused, or nothing that landed. The reasons a frame ends up with no
        // shafts are counted apart, and a refusal is counted once, as the reason it was.
        const Freshness freshness = Readback(context, state);

        // A frame without the sun draw of its own has no depth for this frame's mask: nothing is counted, exactly like a sun behind the camera. A
        // frame whose light the read-back refused is counted once, as that reason, and not again as a frame without a light draw.
        if (!state.sunDraw.taken || !SameRenderedFrame(state.sunDraw.frames, state.frames))
        {
            if (state.haveBelowHorizonFrame && state.belowHorizonFrame == state.frames)
                return SkipRefusedAndDraw(context, facts, scene.Get(), draw, user, drew);
            if (state.haveNotSunFrame && state.notSunFrame == state.frames)
                return SkipNotTheSunAndDraw(context, facts, scene.Get(), draw, user, drew);
            return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoDepth, 0x40000003u);
        }
        // The light this frame draws with. Its own read when that landed a sun; otherwise the last sun the read-back accepted, held for at most
        // kHoldFrames rendered frames with a decaying weight (ShaftsRules.h HoldFade), so a copy that missed a frame costs a little light instead of a
        // whole frame of the effect - the on/off overlay inside the upscaler's input is what ghosts. The held sun is dropped by a camera cut and by
        // its age; it is never held across a cut.
        const bool sunLanded = freshness == Freshness::Sun;
        // The light is a sun and its direction is a unit vector, but the buffer that had to hold the camera's projection did not: that is a build
        // mismatch, and the reason a session needs to read instead of "no sun". A camera map that has not landed yet is a GPU that is behind (or a
        // light the read-back has just dropped), which is not a refusal: the frame is skipped, one line says which it was.
        if (sunLanded && !state.sun.HaveProjection())
        {
            if (!state.sun.ProjectionRefused())
            {
                if (!state.loggedCamera)
                {
                    state.loggedCamera = true;
                    LOG_INFO("{}", kCameraPending);
                }
                return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::None, 0x40000007u);
            }
            return DeclineAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoCamera);
        }
        const float fade = sunLanded ? 1.0f : state.hold.Weight(state.frames);
        if (!sunLanded && !(fade > 0.0f))
        {
            // A session whose maps have been answered "still drawing" and that has never read anything is a stalled read-back: the "no sun"
            // line below would name the wrong cause, so it gets its own, once.
            if (state.sun.Reads() == 0 && state.sun.NotReady() >= kReadbackStallFrames && !state.loggedStall)
            {
                state.loggedStall = true;
                LOG_INFO("{}", kReadbackNotLanding);
            }
            if (ReadStale(state.sun.HaveRead(), state.sun.LastReadFrame(), state.frames))
            {
                if (!state.loggedStale)
                {
                    state.loggedStale = true;
                    LOG_INFO("{}", kReadbackStale);
                }
                return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::None, 0x40000005u);
            }
            return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoSun, 0x40000004u);
        }
        const LightFacts& sunFacts = sunLanded ? state.sun.Facts() : state.hold.facts;
        // The direction that is projected: this frame's own light (already in view space), or the held sun turned into the newest camera's view -
        // the held copy is a world vector, so a camera that turned since its read keeps the shafts on the sun instead of sliding them with an old
        // view. The facts and the camera come from the same ring copy, so a missed light read also missed the camera - the newest camera is then
        // the held read's own, and a cut inside a stretch of missed reads is seen when the next read lands (at most kHoldFrames of them).
        const float* projection = sunLanded ? state.sun.Projection() : (state.haveLastCamera ? state.lastProjection : nullptr);
        if (projection == nullptr)
            return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoSun, 0x4000000Au);
        float sunDirection[3] { sunFacts.direction[0], sunFacts.direction[1], sunFacts.direction[2] };
        if (!sunLanded && (!state.haveLastCamera || !ViewDirectionOf(state.lastView, state.hold.world, sunDirection)))
            return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoSun, 0x4000000Au);
        const void* sunLight = sunLanded ? state.sun.LastLight() : state.heldLight;

        // The sun's screen position: the sky's own sun UV when it landed (the disk the player sees, drawn at its true elevation), otherwise the
        // light draw's direction, which is already in the space the engine's projection maps to the window - so it goes through the projection
        // alone. A sun behind the camera or further outside the window than the margin is a frame with no shafts in it: the frame is left as it
        // was, nothing counted against the module, and the card keeps the sun and the window this frame measured so a session sees where it
        // pointed.
        const bool sunFromUv = SunUvFresh(state.haveSunUv, state.sunUvFrame, state.frames);
        const SunProjection projected = sunFromUv ? ProjectSunFromUv(state.sunUvRow[2], state.sunUvRow[3], facts.window)
                                                  : ProjectSun(projection, sunDirection, facts.window);
        if (!projected.unit)
            return SkipAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::NoSun, 0x40000006u);
        if (!SunVisible(projected))
        {
            ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Shafts, ext::ffxiv::diag::StageOutcome::Declined,
                                          "the sun is behind the camera or outside the margin");
            CountSkip(state.skips, SkipReason::OffScreen);
            state.lastSun = projected;
            state.lastWindow = facts.window;
            if (state.log.Take(0x40000001u))
                LOG_INFO("{}", FormatSunSkipped(projected, facts.window));
            return DrawToneMapWithChain(context, facts, scene.Get(), draw, user, drew);
        }
        if (sunFromUv && !state.loggedSunUv)
        {
            state.loggedSunUv = true;
            LOG_INFO("{}", FormatSunUv(state.sunUvRow[2], state.sunUvRow[3], projected));
        }
        if (state.sunDraw.depthWidth != facts.sceneWidth || state.sunDraw.depthHeight != facts.sceneHeight)
            return DeclineAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::SizeMismatch);

        Inputs inputs;
        inputs.scene = scene.Get();
        inputs.depth = state.sunDraw.depth.Get();
        inputs.sceneFormat = facts.sceneFormat;
        inputs.sceneWidth = facts.sceneWidth;
        inputs.sceneHeight = facts.sceneHeight;
        inputs.depthFormat = state.sunDraw.depthFormat;
        inputs.depthWidth = state.sunDraw.depthWidth;
        inputs.depthHeight = state.sunDraw.depthHeight;
        inputs.window = facts.window;
        inputs.sun = projected;
        inputs.params = state.params;
        inputs.tint = ResolveSunTint(sunFacts.diffuse, state.colour);
        inputs.whitePoint = state.whitePoint;
        inputs.fade = fade;

        ID3D11ShaderResourceView* copy = nullptr;
        {
            // The four draws this runs are this module's own work: the draw seam must not classify them as the game's draws (the pixel-shader
            // tracker would otherwise read this pass's own shaders as the game's), and the character preview's marker keeps its copies out of
            // its replay accounting.
            ext::ffxiv::chara::ReplayScope own;
            ext::ffxiv::engine::OwnWork ownWork;
            copy = state.pass.Run(context, inputs);
        }
        if (copy == nullptr)
            return DeclineAndDraw(context, facts, scene.Get(), draw, user, drew, Decline::PassFailed);
        // The capture frame's pictures of this stage, the first of the tone-map chain: the scene the game bound (the chain's input) and the
        // air-lit copy this pass made, copied now (its pass is over and its guards are gone) and before the puddles' stage runs on the copy.
        ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Shafts, context, scene.Get(), copy,
                                   ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height });
        // The numbers this frame's run used and the dump's inputs do not hold (the replay lab needs them): the white point and fade, the sun in window
        // pixels, the tint (the light's colour, or "sky" when the pass took the sky's colour at the sun). Capture frame only.
        if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Shafts))
        {
            ext::ffxiv::diag::StageFrameValues values;
            values.haveWhite = true;
            values.white = state.whitePoint;
            values.fade = fade;
            values.haveSun = true;
            values.sunX = projected.screenX;
            values.sunY = projected.screenY;
            values.haveTint = !inputs.tint.fromSky;
            for (uint32_t i = 0; i < 3; ++i)
                values.tint[i] = inputs.tint.rgb[i];
            ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Shafts, values);
            if (inputs.tint.fromSky)
                ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Shafts, "tint=sky");
        }

        // The puddles' tone-map stage, on this pass's own air-lit output and before t0 is rebound: one module owns the binding, and the pool's
        // mirror then reflects the air light as well as the scene. OnShaftsSceneDraw runs the stage when it has anything to do and binds the
        // resulting texture (or this pass's copy when the stage is off, dry, or refused) at t0 for the one draw, restoring the game's own binding
        // afterwards.
        ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context, facts, copy, draw, user);
        drew = true;

        state.runs++;
        if (!sunLanded)
            state.heldRuns++;
        state.lastFacts = sunFacts;
        state.lastSun = projected;
        state.lastWindow = facts.window;
        if (state.log.Take(0x40000002u | ((facts.sceneWidth & 0x7FFFu) << 11)))
            LOG_INFO("{}", FormatRun(projected, facts.window, state.params, facts.sceneWidth, facts.sceneHeight, sunDirection, fade, sunLight,
                                     state.sunShadow.degrees, state.whitePoint));
        state.session.NoteRun(Knob::LightShafts);
        if (!state.loggedCost && state.pass.HasGpuMilliseconds())
        {
            state.loggedCost = true;
            LOG_INFO("{}", FormatCost(true, state.pass.GpuMilliseconds(), state.frames, state.runs, state.declined));
        }
        return true;
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception));
        if (!drew)
            draw(user); // the game's draw, once, and not twice
        return true;
    }
}

// Gives the owned textures back, but only from the render thread: a config load can run on the DLL's start-up thread while a draw is in flight,
// and releasing device objects under it would be a use after free.
void FreeOwnedTextures(State& state)
{
    if (!state.haveRenderThread || std::this_thread::get_id() != state.renderThread)
        return;
    state.pass.Reset();
    state.sun.Reset();
    state.tone.Reset();
    state.sunUv.Reset();
    state.sunDraw = SunDraw {};
    state.hold = SunHold {};
    state.haveLastCamera = false;
    state.haveSunUv = false;
    for (float& value : state.lastProjection)
        value = 0.0f;
    for (float& value : state.lastView)
        value = 0.0f;
}

// The scene HDR path replays the engine's own tone chain in its own branch (the seam hands the draw path the plain call when it is requested),
// so while it is on this module's draws never arrive. The key is read live wherever the module decides, and this is where the one log line is
// written: on the sample that first finds the module inactive, so a session that switches HDR on or off with the shafts key untouched gets one
// line per transition instead of one a frame (or none at all, which is what a value stored at the config load came to).
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
    state.params = BuildParams(cfg.lightShaftsIntensity.value_or_default(), cfg.lightShaftsDecay.value_or_default(),
                               cfg.lightShaftsSamples.value_or_default(), cfg.lightShaftsDensity.value_or_default());
    state.colour = ParseColour(cfg.lightShaftsColor.value_or_default());
    ObserveSceneHdr(state);
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.loggedRejected = false;
        state.loggedBelowHorizon = false;
        state.loggedNotTheSun = false;
        state.loggedShadowMissing = false;
        state.loggedShadowUnnegated = false;
        state.loggedProjection = false;
        state.loggedStale = false;
        state.loggedCamera = false;
        state.loggedStall = false;
        state.loggedCameraCut = false;
        state.loggedSunUv = false;
        state.sceneResource = nullptr;
        state.sunDraw = SunDraw {};
        state.rejectedLight = nullptr;
        state.rejectedFrame = 0;
        state.haveBelowHorizonFrame = false;
        state.haveNotSunFrame = false;
        state.sunShadow = ShadowJudgement {};
        state.haveToneMapFrame = false;
        state.haveWhitePoint = false;
        state.whiteFrame = 0;
        state.loggedWhitePoint = false;
        state.loggedWhitePointFallback = false;
        state.whitePoint = kFallbackWhitePoint;
        state.hold = SunHold {};
        state.heldLight = nullptr;
        state.haveLastCamera = false;
        state.haveSunUv = false;
        state.sunUvFrame = 0;
        state.nextCountsLog = 0;
    }
    else if (state.session.off)
    {
        state.session = Session {}; // the player turned the switch back on: the counters and the streaks start again
        state.nextCountsLog = 0;
    }
    // The feature bit stays on while the engine's scene HDR path is requested: the seam then hands the pass no draw, but the pixel-shader seam
    // keeps sampling that key, which is how switching HDR off again starts the pass without the shafts key moving.
    const bool wanted = Wanted(state.enabled, state.params) && state.session.Armed();
    ext::SetActive(ext::F::LightShafts, wanted);
    if (!wanted || state.hdr.inactive)
        FreeOwnedTextures(state);
}
} // namespace

bool OnEngineDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return false;
    // The scene HDR key is read live here, not remembered from the config load: while it is on the seam hands this module no draw at all, so a
    // session that switches it off has to start the pass again without the shafts key moving. `Armed` is the one place that answers it.
    if (!Armed() || context == nullptr)
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    try
    {
        if (ext::ffxiv::engine::IsShaftsLightAnchors(anchors))
        {
            Capture(context, anchors, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (ext::ffxiv::engine::IsShaftsSunParamAnchors(anchors))
        {
            CaptureSunParam(context, anchors, state);
            return false; // and the sky's own sun draw is read and passed on untouched
        }
        if (ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors))
        {
            RecordScene(context, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors))
            return RunToneMap(context, draw, user);
    }
    catch (...)
    {
        // Fail open: the engine module's draw path runs the game's draw, unchanged.
        LOG_INFO("{}", FormatDecline(Decline::Exception));
    }
    return false;
}

bool Armed()
{
    State& state = S();
    if (!Wanted(state.enabled, state.params) || !state.session.Armed())
        return false;
    return !SceneHdrInactive(state.enabled, SceneHdrRequested());
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
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.enabled = state.enabled;
    status.armed = Armed();
    status.hdrInactive = SceneHdrInactive(state.enabled, SceneHdrRequested());
    status.intensity = state.params.intensity;
    status.frames = state.frames;
    status.runs = state.runs;
    status.held = state.heldRuns;
    status.declined = state.declined;
    status.skipped = state.skips.skipped;
    status.noDepth = state.skips.noDepth;
    status.noSun = state.skips.noSun;
    status.offScreen = state.skips.offScreen;
    status.belowHorizon = state.skips.belowHorizon;
    status.notTheSun = state.skips.notTheSun;
    status.reads = state.sun.Reads();
    status.notReady = state.sun.NotReady();
    status.refused = state.notSunRefusals;
    status.haveSun = state.runs > 0 || state.skips.skipped > 0;
    for (uint32_t i = 0; i < 3; ++i)
    {
        status.sunDirection[i] = state.lastFacts.direction[i];
        status.sunDiffuse[i] = state.lastFacts.diffuse[i];
    }
    status.sunScreen[0] = state.lastSun.screenX;
    status.sunScreen[1] = state.lastSun.screenY;
    status.windowWidth = uint32_t(state.lastWindow.width);
    status.windowHeight = uint32_t(state.lastWindow.height);
    return status;
}

// The stage dump's question about this stage's values, asked at the capture frame (the armed question is Armed()).
std::string StageValues() { return FormatStageValues(S().params); }

void Publish() { Apply(ext::Cfg().ffxiv.lightShafts.value_or_default()); }

void OnConfigLoaded(bool on) { Apply(on); }

void Release()
{
    State& state = S();
    state.pass.Reset();
    state.sun.Reset();
    state.tone.Reset();
    state.sunUv.Reset();
    state.sunDraw = SunDraw {};
    state.sceneResource = nullptr;
    state.hold = SunHold {};
    state.heldLight = nullptr;
    state.haveLastCamera = false;
    state.haveSunUv = false;
}
} // namespace ext::ffxiv::shafts

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run: the [FFXIV] LightShafts key arms the seam's feature bit at
// every config load, and the tuning keys are read then too.
struct RegisterLightShafts
{
    RegisterLightShafts()
    {
        ext::FfxivConfig::lightShaftsOnLoaded = &ext::ffxiv::shafts::OnConfigLoaded;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Shafts, &ext::ffxiv::shafts::Armed, &ext::ffxiv::shafts::StageValues);
    }
} g_registerLightShafts;
} // namespace
