#include "pch.h"

#include "ext/ffxiv/shadows/ShadowsHooks.h"
#include "ext/ffxiv/shadows/ShadowsPass.h"
#include "ext/ffxiv/shadows/ShadowsRules.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/engine/EnginePass.h"   // the seam's arming-chain hook (this module's feature bits feed it)
#include "ext/ffxiv/engine/FogSun.h"      // the visible sun's elevation, published by the fog-post capture; this module asks for it with a flag
#include "ext/ffxiv/engine/ShaftsRules.h"  // the scene-HDR decline's watch, the shafts rule reused verbatim
#include "ext/hdr/scene/FfxivSceneHdr.h"   // the scene HDR path replaces the tone chain this module draws before
#include "ext/present/PresentSeams.h"      // the output-frame counter, the capture ring's frame key

#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame
#include "ext/ffxiv/relight/Inject.h" // AnyIndexedFakeSpecular: whether the session's index knows the main view's sun light shader
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <unordered_map>

// Shadow mapping (ext/ffxiv/shadows): the module's state, its sink hooks, the light-colour capture ring and the two
// entry paths. The rules are ShadowsRules.h, the own pass ShadowsPass.{h,cpp}, the card ShadowsMenu.cpp, the keys
// FfxivConfig.h.
//
// The house rules this module keeps, in the order the report names them:
//  - fail-open everywhere: a failed step is the game's own draw, and one log line the first time each reason appears
//    (the LogGate); sixty-four refused light draws in a row stand the mask route down for the session with one line
//    (ShadowsRules.h's Session; the engine seam's own sixty-four covers the ambient arm);
//  - the character-preview replay is refused uncounted, as relight::OnGameDraw's guard;
//  - the scene-HDR decline is the light shafts' rule (that path replaces the tone chain this module draws before)
//    with the same SceneHdrWatch, so the line is written once per session per re-appear;
//  - no game resource is ever written; the pass owns its textures, sized to the engine's, recreated on a size
//    change, released on a device change and on Release();
//  - one log line per state change, and the GPU timestamp pair is read a run or two later with
//    D3D11_ASYNC_GETDATA_DONOTFLUSH, never waited for (the fork's own pass idiom).
namespace ext::ffxiv::shadows
{
namespace
{
using Microsoft::WRL::ComPtr;

// ---- the light-colour capture ring (the ShaftsSun.cpp readback pattern, m_DiffuseColor's range only) ----------------

// g_LightParam's first three float4 rows: m_Position @0, m_Direction @16, m_DiffuseColor @32..0x2C. The colour is
// the frame's own day/night parameter (the report 2.2); the curve keys on its Rec.709 luminance.
constexpr uint32_t kLightColourBytes = 0x30;
constexpr uint32_t kColourRing = 4;   // the same depth the shafts' ring keeps: a GPU a frame or two behind reads later
constexpr uint32_t kMaxColourChangeLines = 24; // the colour-change diagnostic's own budget
constexpr uint32_t kMaxSourceLines = 16;       // the night curve's source-change lines' own budget
constexpr uint32_t kColourSlots = 16; // the texture slots a light draw is inspected at (ContactShadowsApply's count)
constexpr uint32_t kColourCbs = 8;    // and its constant-buffer slots

int64_t Qpc()
{
    LARGE_INTEGER value {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
int64_t QpcFrequency()
{
    LARGE_INTEGER value {};
    QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : 1;
}
double SecondsBetween(int64_t from, int64_t to)
{
    return from <= 0 ? 0.0 : double(to - from) / double(QpcFrequency());
}

// The log tag of a state change, for a module whose values are continuous: the curve in 1/64 steps and the lift in
// 1/128 steps, so a real change writes a line and a wander inside the same step does not.
constexpr uint32_t StateTag(uint32_t kind, float curve, float lift, float softness) noexcept
{
    const uint32_t curvePart = (uint32_t)(curve * 64.0f) & 0x3Fu;
    const uint32_t liftPart = (uint32_t)(lift * 128.0f) & 0x7Fu;
    const uint32_t softnessPart = (uint32_t)(softness * 4.0f) & 0x3Fu;
    return (kind << 22) | (curvePart << 15) | (liftPart << 7) | softnessPart;
}

using shafts::SceneHdrWatch; // the shafts' scene-HDR watch: `Sample` answers once, when the rule first applies

// One module's whole session state. The mask route and the ambient provider share it: the curve and the blend are
// computed at each sink call (a handful of loads and arithmetic), the keys are the config's.
struct State
{
    MaskLiftPass pass;
    Session session;    // the mask route's own 64-refusal stand-down (the engine seam's ambient arm has its own)
    LogGate log;

    // The master switch as the last config load published it, and the module's keys (read whenever the menu or a
    // config load changes them, so the render thread never touches the config).
    bool keyOn = false;
    float liftKey = kDefaultLift;
    float softnessKey = kDefaultSoftness;
    float ambientKey = kDefaultAmbientLift;
    bool nightCurveToggle = true;

    // The capture ring: the frame's own g_LightParam copied at each served light draw, read no-wait a frame or two
    // later. Until the first read lands, the curve runs the identity (fail-open), so the night keys never act
    // before the frame's own colour has been measured.
    ComPtr<ID3D11Buffer> staging[kColourRing];
    uint64_t staged[kColourRing] {};    // the capture count each slot holds (the ring's own key, see CaptureLightColour)
    bool haveStaged[kColourRing] {};
    uint64_t captureSeq = 0;            // distinct frames captured so far: the ring is keyed by this, never by the frame counter
    uint64_t lastCaptureKey = 0;        // the frame counter value of the last capture (two light draws in one frame share a capture)
    bool haveCaptureKey = false;
    bool haveRead = false;
    float lastColour[3] {};
    uint64_t lastReadSeq = 0;
    uint32_t stagedLayout[kColourRing] {}; // the route id of the light draw whose copy each slot holds (0 = the contact seam's, which carries none)
    uint32_t lastReadLayout = 0;           // the route id behind the colour the curve reads now
    uint32_t capturesThisKey = 0;          // light draws copied under the current frame key (the later copy overwrites the slot)
    uint32_t firstLayoutThisKey = 0;
    uint32_t loggedLayout = 0xFFFFFFFFu;   // what the last colour-change line said (a diagnostic only: nothing reads these to decide)
    float loggedLuminance = -1.0f;
    uint32_t colourChangeLines = 0;
    FogSunTracker fogTracker;                  // ages the fog sun's publishes on this module's own clock (captureSeq: rendered frames with a served light draw)
    NightSource loggedSource = NightSource::None; // the source the last source line named
    bool haveLoggedSource = false;
    uint32_t sourceLines = 0;
    bool loggedTwoLights = false;
    uint32_t colourReads = 0;
    uint32_t colourNotReady = 0;
    uint32_t colourCopies = 0;

    // The route's own shape-refusal memory: the relight module's rule, one look per light draw shader for the
    // cascade atlas and masks of another format, looked at again after relight::kShapeRefusalSeconds.
    std::unordered_map<uint32_t, int64_t> refusedShapes;

    std::atomic<bool> inPass { false }; // true while this module's own GPU work runs (never classified, never served twice)

    uint32_t runs = 0;           // light draws the mask lift served with the copy bound
    uint32_t declined = 0;       // light draws left alone after the classification refused
    uint32_t shapes = 0;         // of those, the ones about the draw's shape (never counted against the streak)
    uint64_t routeDrawsSeen = 0; // the route's own counter (the route runs only when contact shadows does not)
    uint32_t combos = 0;         // composite draws for which the ambient provider gave entries
    float lastCurve = 0.0f;      // the curve the last blend computed, for the card and the log line

    shafts::SceneHdrWatch sceneHdrWatch; // the scene-HDR decline's once-per-appear state
    bool sceneHdrInactive = false;

    bool loggedRoute = false;      // the route's first served draw line
    bool loggedCombine = false;    // the combine stage's first line
    bool loggedOff = false;        // the stand-down's one line
    bool loggedColour = false;     // the first light colour read's line
    uint32_t idleLogged = 0;       // one bit per Idle reason: its line was written in this state
    uint32_t layoutAsks = 0;       // route asks that came without a layout, counted until the first layout is seen
    bool sawLayout = false;        // the route has been handed a layout (the index knows a sun light draw)
    uint32_t noColourAsks = 0;     // ambient asks with the ticked curve and no light colour yet
    uint32_t ambientAsks = 0;      // every ask of the ambient provider in this state (the engine seam reached a composite)
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The module's keys, read whenever the menu or a config load changes them (the render thread never touches config).
void PublishParams()
{
    State& state = S();
    auto& cfg = ext::Cfg().ffxiv;
    state.liftKey = ClampLift(cfg.shadowLift.value_or_default());
    state.softnessKey = ClampSoftness(cfg.shadowEdgeSoftness.value_or_default());
    state.ambientKey = ClampAmbientLift(cfg.nightAmbientLift.value_or_default());
    state.nightCurveToggle = cfg.nightCurveToggle.value_or_default();
}

// The scene-HDR rule, from the shafts module (ShaftsRules.h SceneHdrInactive): that path replays the engine's own
// tone chain in its own branch and never hands the module's draws over, so the module is inert while it is
// requested. The watch's Sample is what keeps the line to one per session per re-appear.
bool SceneHdrDecline(State& state)
{
    const bool enabled = ext::Cfg().hdr.ffxivHdr.value_or_default();
    shafts::SceneHdrWatch& watcher = state.sceneHdrWatch;
    if (enabled && ext::hdr::FfxivSceneHdr::Requested())
    {
        if (watcher.Sample(enabled, true) && state.log.Take(0xA0000000u))
            LOG_INFO("{}", "Shadow mapping: inactive while scene HDR is on, because that path replaces the tone "
                           "chain this module draws before");
        state.sceneHdrInactive = true;
        return true;
    }
    watcher.Sample(enabled, false); // the path is off again: the watch re-arms for the next appear of it
    state.sceneHdrInactive = false;
    return false;
}

// The cheap gate of the mask route, asked per draw and per bind while contact shadows does not ask (one pair of
// loads; the curve's decision and the scene-HDR check live at the sinks that answer values, at the light-draw rate).
bool MaskWants(State& state)
{
    // The master key, the route's own stand-down (one state read instead of a route per draw), and then the lift key or, with
    // the lift at its identity, the night ambient key on the ticked curve: that arm needs the frame's light colour, and the
    // route is where the colour is captured when contact shadows does not serve the light draws. Both keys at identity run
    // nothing: no copies, no pins, no entries.
    return MaskWantsDecision(state.keyOn, state.session.Armed(), state.liftKey, state.ambientKey, state.nightCurveToggle);
}

// The draw hook's feature bit follows the route's gate, not the master key alone: with both keys at their idle value (the shipped
// default once the master is on) or after the route stood itself down, the hook stays at one relaxed load for every draw of the frame.
void PublishRouteBit(State& state)
{
    ext::SetActive(ext::F::ShadowMapping, MaskWants(state));
    // The night curve's input is the fog post's sun: ask the fog-post capture for it exactly when the curve can act (off with the master key off, the
    // curve unticked, both keys at their identity, or the route stood down). The capture arms its seam on the flag; it runs whatever the god rays key says.
    fogsun::SetWanted(fogsun::kShadowMapping, FogSunWanted(state.keyOn, state.session.Armed(), state.liftKey, state.ambientKey, state.nightCurveToggle));
}

// The first time a reason keeps the module idle in this state: the caller writes the reason's own line then (the formatter
// is only built for that first time).
bool TakeIdle(State& state, Idle reason)
{
    const uint32_t bit = 1u << uint32_t(reason);
    if ((state.idleLogged & bit) != 0)
        return false;
    state.idleLogged |= bit;
    return true;
}

// The reading dies with its feed. The colour ring is refreshed only by the route's own served draws, so once the route has stood
// down nothing could ever age a night colour out: it would keep lifting a later daytime scene for the rest of the session. Forgetting
// it (the colour, the ring's staged copies, the curve the card shows and the first-colour line's state) puts the ambient arm back
// on the engine's own table, the fail-open state a fresh session starts in, until the module is re-armed and a colour lands again.
void ForgetReading(State& state)
{
    state.haveRead = false;
    state.lastColour[0] = state.lastColour[1] = state.lastColour[2] = 0.0f;
    state.lastReadSeq = 0;
    for (uint32_t i = 0; i < kColourRing; ++i)
        state.haveStaged[i] = false;
    state.lastCurve = 0.0f;
    state.loggedColour = false;
    state.lastReadLayout = 0;
    state.loggedLayout = 0xFFFFFFFFu;
    state.loggedLuminance = -1.0f;
    state.fogTracker.Reset();
    state.haveLoggedSource = false;
    state.sourceLines = 0;
}

// The cheap gate of the ambient arm, asked by the engine's arming chain (one pair of loads). The curve's decision
// lives in the provider below (per composite draw, which is what the dispatch pays).
bool AmbientWantsFast(State& state)
{
    if (!state.keyOn)
        return false;
    if (IsAmbientIdentity(ClampAmbientLift(state.ambientKey)))
        return false; // the default state: no entries, no dispatch, nothing the seam pays for
    return true;
}

// The form's blend from the keys and the capture ring's newest landed colour. Asked at each served light draw and
// each composite draw that wants entries: a few loads and arithmetic, never a blocked GPU read. `context` is where
// the ring's no-wait read runs; the engine seam and the contact seam both pass the draw's own context.
Blend BlendNow(State& state, ID3D11DeviceContext* context)
{
    // The newest staged colour the GPU has finished: the previous captures (ages 1..ring by capture count, whatever the frame
    // counter stepped by), DO_NOT_WAIT, the last good colour keeps serving.
    for (uint32_t age = 1; age < kColourRing; ++age)
    {
        if (state.captureSeq <= age)
            break;
        const uint64_t candidate = state.captureSeq - age;
        const uint32_t slot = uint32_t(candidate % kColourRing);
        if (!state.haveStaged[slot] || state.staged[slot] != candidate || state.staging[slot] == nullptr)
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        // The read never waits for the GPU (a waited map on the game's own context is a frame stall the session
        // reads as a lag): a copy that has not landed is skipped this ask, and the last good colour keeps serving.
        if (context->Map(state.staging[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK ||
            mapped.pData == nullptr)
        {
            ++state.colourNotReady;
            continue;
        }
        std::memcpy(state.lastColour, static_cast<const unsigned char*>(mapped.pData) + 0x20, sizeof(float) * 3);
        context->Unmap(state.staging[slot].Get(), 0);
        state.haveRead = true;
        state.lastReadSeq = candidate;
        state.lastReadLayout = state.stagedLayout[slot];
        ++state.colourReads;
        break; // the newest read wins: an older one is never taken
    }
    const float luminance = LuminanceOf(state.lastColour);
    // The curve's input is the visible sun while a fog sun is fresh (aged on this module's own clock, not the present counter that frame generation
    // steps), the main view's light colour otherwise, nothing when neither has landed: ONE curve for the mask lift and the ambient table.
    const fogsun::Reading fog = fogsun::Read();
    state.fogTracker.Observe(fog.count, state.captureSeq);
    NightReading reading;
    reading.fogFresh = fog.have && state.fogTracker.Fresh(state.captureSeq);
    reading.fogSunY = fog.y;
    reading.haveColour = state.haveRead;
    reading.luminance = luminance;
    const NightCurveResult night = NightCurve(state.nightCurveToggle, reading);
    state.lastCurve = night.curve;
    const Blend blend = FrameBlendCurve(state.keyOn, true, state.liftKey, state.softnessKey, state.ambientKey, night.curve);
    if (state.nightCurveToggle && (!state.haveLoggedSource || night.source != state.loggedSource) && state.sourceLines < kMaxSourceLines &&
        !(night.source == NightSource::None && !state.haveLoggedSource))
    {
        state.haveLoggedSource = true;
        state.loggedSource = night.source;
        ++state.sourceLines;
        LOG_INFO("{}", FormatNightSource(night.source, fog.y, luminance, night.curve));
    }
    if (state.haveRead && !state.loggedColour)
    {
        // The first light colour that landed in this state, with the curve it gives: the line that tells a retest the capture
        // path works, whether or not the picture changes.
        state.loggedColour = true;
        LOG_INFO("{}", FormatColourRead(state.lastColour, blend.curve, blend.maskLift, blend.ambientScale));
    }
    // The colour the curve keys on changes with the time of day, the weather and which light draw the copy came from (a frame can carry two
    // light draws, and the later copy of the frame wins the slot): one line per change of the draw behind it or of the luminance by a factor
    // of two, at most kMaxColourChangeLines a session, so a session reads WHEN and FROM WHICH draw the night curve moved. A diagnostic only:
    // nothing here reads these to decide.
    if (state.haveRead && state.colourChangeLines < kMaxColourChangeLines)
    {
        const float safeNow = luminance > 1.0e-4f ? luminance : 1.0e-4f;
        const float safeThen = state.loggedLuminance > 1.0e-4f ? state.loggedLuminance : 1.0e-4f;
        const bool moved = state.loggedLuminance < 0.0f || std::fabs(std::log2(safeNow / safeThen)) >= 1.0f;
        if (moved || state.lastReadLayout != state.loggedLayout)
        {
            state.loggedLuminance = luminance;
            state.loggedLayout = state.lastReadLayout;
            ++state.colourChangeLines;
            LOG_INFO("{}", FormatColourChange(state.lastColour, state.lastCurve, state.lastReadLayout));
        }
    }
    return blend;
}

// ---- the capture ring -------------------------------------------------------------------------------------------------

// The frame's own light colour, copied here at the draw that binds it (either served path lands on such a draw),
// staged into the ring slot of this capture; the no-wait read of a later capture keeps the curve fresh. The ring is keyed
// by the capture count (a new capture whenever the frame counter value changed), not by the frame counter itself: that
// counter ticks once per output present and a frame generation mode steps it by two, three or four per game frame, which
// a key of its own would turn into a read window the previous capture never falls into. Nothing of the
// game is ever written, and the staging pair's owner is checked: a pair the previous device created cannot receive
// this context's copy (the ShaftsSun.cpp rule), so it is rebuilt on the spot.
void CaptureLightColour(ID3D11DeviceContext* context, ID3D11Buffer* lightParam, uint64_t frame, uint32_t layoutId)
{
    State& state = S();
    if (context == nullptr || lightParam == nullptr || !state.keyOn)
        return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return;
    ID3D11Device* rawDevice = device.Get();
    const auto madeOn = [&](const ComPtr<ID3D11Buffer>& target) {
        if (target == nullptr)
            return false;
        ComPtr<ID3D11Device> owner;
        target->GetDevice(&owner); // ID3D11DeviceChild::GetDevice answers with the reference alone, no HRESULT
        return owner && owner.Get() == rawDevice;
    };
    D3D11_BUFFER_DESC sourceDesc {};
    lightParam->GetDesc(&sourceDesc);
    if (sourceDesc.ByteWidth < kLightColourBytes)
    {
        // a build whose buffer is smaller than the members the curve keys on: nothing to read, no patch
        if (TakeIdle(state, Idle::ColourBufferSmall))
            LOG_INFO("{}", FormatIdleColourBuffer(sourceDesc.ByteWidth, kLightColourBytes));
        return;
    }
    if (!state.haveCaptureKey || state.lastCaptureKey != frame)
    {
        state.haveCaptureKey = true;
        state.lastCaptureKey = frame;
        ++state.captureSeq;
        state.capturesThisKey = 0;
        state.firstLayoutThisKey = layoutId;
    }
    ++state.capturesThisKey;
    if (state.capturesThisKey == 2 && !state.loggedTwoLights)
    {
        state.loggedTwoLights = true;
        LOG_INFO("{}", FormatTwoLightDraws(state.firstLayoutThisKey, layoutId));
    }
    const uint64_t seq = state.captureSeq;
    const uint32_t slot = uint32_t(seq % kColourRing);
    if (!state.staging[slot] || !madeOn(state.staging[slot]))
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = kLightColourBytes;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        state.staging[slot].Reset();
        ComPtr<ID3D11Buffer> created;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &created)) || !created)
        {
            // the capture is never a gate: the curve keeps its last read's colour
            if (TakeIdle(state, Idle::ColourStagingFailed))
                LOG_INFO("{}", FormatIdleColourStaging());
            return;
        }
        state.staging[slot] = created;
        state.haveStaged[slot] = false;
    }
    D3D11_BOX box {};
    box.right = kLightColourBytes;
    box.bottom = 1;
    box.back = 1;
    context->CopySubresourceRegion(state.staging[slot].Get(), 0, 0, 0, 0, lightParam, 0, &box);
    state.staged[slot] = seq;
    state.stagedLayout[slot] = layoutId;
    state.haveStaged[slot] = true;
    ++state.colourCopies;
}

// ---- the route's inspect (the light-draw's shape, the contact module's own reads trimmed to what a remap needs) -------

// The run's inspect, in the same field shapes ContactShadowsApply uses: what the classification and the pass need.
// The slot numbers come from the relight index's layout, handed in by the seam that resolved the shader.
// The route's Inputs: the mask view of the draw (released by the ComPtr when the inspect scope ends) and the two
// constant buffers the classification's shape rules read (a light draw's g_LightParam is the capture's source).
struct RouteInputs
{
    ComPtr<ID3D11ShaderResourceView> mask;
    ComPtr<ID3D11Buffer> lightParam;
    ComPtr<ID3D11Buffer> cameraParam;
    uint32_t maskFormat = 0, maskWidth = 0, maskHeight = 0;
    float window[4] {};
};

// One bound texture of the draw, in the field shapes the contact module's TextureAt fills; nothing else can tell a
// typeless texture's typed view from its other views.
struct RelightTexture
{
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t format = 0, textureFormat = 0, width = 0, height = 0, samples = 1;
    bool twoD = false;

    static RelightTexture At(ID3D11ShaderResourceView* at)
    {
        RelightTexture out;
        if (at == nullptr)
            return out;
        out.view = at;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        at->GetDesc(&desc);
        out.format = uint32_t(desc.Format);
        out.twoD = desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11Resource> resource;
        at->GetResource(&resource);
        if (!resource)
            return out;
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        resource->GetType(&dimension);
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            out.twoD = false;
            return out;
        }
        D3D11_TEXTURE2D_DESC texture {};
        static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
        out.textureFormat = uint32_t(texture.Format);
        out.width = texture.Width;
        out.height = texture.Height;
        out.samples = texture.SampleDesc.Count;
        out.twoD = out.twoD && texture.ArraySize == 1;
        return out;
    }
};

void InspectRoute(ID3D11DeviceContext* context, const RouteLayout& layout, relight::DrawFacts& facts, RouteInputs& inputs)
{
    ID3D11ShaderResourceView* views[kColourSlots] {};
    context->PSGetShaderResources(0, kColourSlots, views);
    const RelightTexture mask = RelightTexture::At(layout.maskSlot < kColourSlots ? views[layout.maskSlot] : nullptr);
    const RelightTexture depth = RelightTexture::At(layout.depthSlot < kColourSlots ? views[layout.depthSlot] : nullptr);
    for (auto*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    ID3D11Buffer* buffers[kColourCbs] {};
    context->PSGetConstantBuffers(0, kColourCbs, buffers);
    if (layout.lightParamSlot < kColourCbs && buffers[layout.lightParamSlot] != nullptr)
        inputs.lightParam.Attach(buffers[layout.lightParamSlot]);
    if (layout.cameraParamSlot < kColourCbs && buffers[layout.cameraParamSlot] != nullptr)
        inputs.cameraParam.Attach(buffers[layout.cameraParamSlot]);
    for (uint32_t i = 0; i < kColourCbs; ++i)
        if (buffers[i] != nullptr && buffers[i] != inputs.lightParam.Get() && buffers[i] != inputs.cameraParam.Get())
            buffers[i]->Release();

    facts.hasMask = mask.view != nullptr;
    facts.hasDepth = depth.view != nullptr;
    facts.hasLightParam = inputs.lightParam != nullptr;
    facts.hasCameraParam = inputs.cameraParam != nullptr;
    facts.maskFormat = mask.format;
    facts.maskTextureFormat = mask.textureFormat;
    facts.maskWidth = mask.width;
    facts.maskHeight = mask.height;
    facts.maskSamples = mask.samples;
    facts.mask2D = mask.twoD;
    facts.depthWidth = depth.width;
    facts.depthHeight = depth.height;
    // The display allocation's resolution is the draw's own full-resolution depth binding: the screen-space mask is
    // that size (the contact classification's own rule, reused verbatim).
    facts.displayWidth = depth.width;
    facts.displayHeight = depth.height;

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    // relight::DrawFacts.window is the contact module's own Window struct (x, y, width, height), not an array.
    facts.window.x = viewport.TopLeftX;
    facts.window.y = viewport.TopLeftY;
    facts.window.width = viewport.Width;
    facts.window.height = viewport.Height;

    inputs.mask = mask.view;
    inputs.maskFormat = mask.format;
    inputs.maskWidth = mask.width;
    inputs.maskHeight = mask.height;
    inputs.window[0] = facts.window.x;
    inputs.window[1] = facts.window.y;
    inputs.window[2] = facts.window.width;
    inputs.window[3] = facts.window.height;
}
} // anonymous namespace

// ---- the sinks (the hooks' bodies; the first-time log lines one per change, the same gate the other modules keep) --------

bool MaskWantsSink()
{
    State& state = S();
    return MaskWants(state);
}

// The contact combine's stage: the VALUES the pass writes into its own combine's ParamsBuffer (the remap lives in
// that shader, with the tent). A false answer is "the game's own mask bytes" and nothing else: the module copies
// nothing here - one copy per frame, zero extra copies, the brief's no-second-competing-copy contract - and the
// contact pass's own bind/restore contract carries the copy.
bool CombineLiftSink(ID3D11DeviceContext* context, float* outLift, float* outSoftness)
{
    State& state = S();
    if (context == nullptr || outLift == nullptr || outSoftness == nullptr)
        return false;
    if (!MaskWants(state))
        return false;
    if (SceneHdrDecline(state))
        return false; // inert while the engine's scene HDR path runs
    const Blend blend = BlendNow(state, context);
    if (!blend.maskRuns)
        return false; // the day curve (or the no-reading state) is the game's own mask
    *outLift = blend.maskLift;
    *outSoftness = blend.softness;
    // The stage's own state-change line: the values' step, in the tag. The contact module's own FormatRun names the
    // copy it binds; this line says what the combine carried in it.
    if (state.log.Take(StateTag(0x1u, blend.curve, blend.maskLift, blend.softness)))
        LOG_INFO("Shadow mapping: the sun mask lift rides the contact combine (lift {:.3f}, softness {:.2f} px, curve {:.3f}{});",
                 double(blend.maskLift), double(blend.softness), double(blend.curve),
                 state.haveRead ? "" : "at identity, no light colour read yet");
    return true;
}

// The module's own route (the brief's "when contact is off, do your own copy/remap/rebind"): the resolved layout
// comes from the seam that tracked the bound shader. Every path here either calls the game's draw exactly once and
// answers true (the route answered the draw), or answers false (the seam passes through and draws).
bool LightDrawRouteSink(ID3D11DeviceContext* context, void (*draw)(void*), void* user, const RouteLayout* layout)
{
    State& state = S();
    if (!MaskWants(state))
        return false; // the cheap gate: the identity state (the master, the stand-down or the lift key at 0)
    if (layout == nullptr || layout->id == 0)
    {
        // only what the seam resolved: the rest is never counted and never inspected - and this is almost every draw of the frame, so
        // it is answered before anything atomic. But a route that is asked for a long time and never handed a sun light draw has a
        // reason, and the log says it once.
        if (!state.sawLayout && ++state.layoutAsks == kNoLayoutAsks && TakeIdle(state, Idle::NoLayout))
            LOG_INFO("{}", FormatIdleNoLayout(kNoLayoutAsks));
        return false;
    }
    const bool previous = state.inPass.exchange(true, std::memory_order_relaxed);
    if (previous)
        return false; // this module's own GPU work (the lift's one draw): never classified, never a second run
    struct Owner
    {
        State& state;
        const bool previous;
        ~Owner() { state.inPass.store(previous, std::memory_order_relaxed); } // a nested guard restores, never clears
    };
    Owner owner { state, previous };
    state.sawLayout = true;
    if (ext::ffxiv::chara::t_replay)
        return false; // a replay of a game draw is exactly that replay: the seam draws it, never the copy
    ++state.routeDrawsSeen;
    // The ambient arm lives in the engine seam's composite draw: a retest that sees the route and the colour lines but no ambient line
    // learns here that the seam never asked (the composite was not classified), before it judges a picture.
    if (state.routeDrawsSeen == kNoCompositeDraws && state.ambientAsks == 0 && !IsAmbientIdentity(ClampAmbientLift(state.ambientKey)) &&
        TakeIdle(state, Idle::NoComposite))
        LOG_INFO("{}", FormatIdleNoComposite(kNoCompositeDraws, state.ambientKey));
    bool drew = false;
    // One decline path: the game's draw runs exactly once (also on a throw), a first-time line per reason, the
    // streak counter for the rules' own refusals only, the same accounting ContactShadowsApply.cpp keeps.
    const auto decline = [&](relight::Decline reason) -> bool {
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::ShadowLift, ext::ffxiv::diag::StageOutcome::Declined,
                                      relight::DeclineName(reason));
        state.declined++;
        const bool shape = relight::IsShapeDecline(reason);
        if (shape)
            state.shapes++; // a draw of another shape (the cascade atlas): the memory, not the stand-down counter
        if (state.log.Take(uint32_t(reason)))
            LOG_INFO("{}", ext::ffxiv::shadows::FormatDecline(reason)); // qualified: ADL also finds relight's own formatter for its Decline
        if (!shape && state.session.NoteDecline())
        {
            ForgetReading(state);   // the colour feed is gone for the session: the reading goes with it, the ambient arm stops
            PublishRouteBit(state); // the route's gate closed with the stand-down: the draw hook is one relaxed load again
            if (!state.loggedOff)
            {
                state.loggedOff = true;
                LOG_WARN("{}", FormatSwitchedOff());
            }
        }
        if (!drew)
            draw(user);
        return true; // answered: the game's draw has run with the game's own mask binding
    };

    // The shape-refusal memory: one look per light draw shader until it expires, exactly the relight module's rule.
    const auto refused = state.refusedShapes.find(layout->id);
    if (refused != state.refusedShapes.end() &&
        SecondsBetween(refused->second, Qpc()) <= relight::kShapeRefusalSeconds)
        return false; // remembered as a wrong shape: pass through, neither counted nor inspected

    try
    {
        relight::DrawFacts facts;
        RouteInputs inputs;
        InspectRoute(context, *layout, facts, inputs);
        relight::Decline reason = relight::Classify(facts);
        // The same paircheck the contact pass makes after the pure rules: only the family's typed member is a mask
        // view (an R8_TYPELESS texture sampled as R8_SNORM reads other numbers).
        if (reason == relight::Decline::None && !relight::IsMaskFormatPair(facts.maskTextureFormat, facts.maskFormat))
            reason = relight::Decline::MaskFormat;
        if (reason != relight::Decline::None)
            return decline(reason);

        // The first served draw of this state, once: which draw the route follows and what the keys ask of it.
        if (!state.loggedRoute)
        {
            state.loggedRoute = true;
            LOG_INFO("{}", FormatRouteFirst(facts.maskWidth, facts.maskHeight, facts.maskFormat, layout->maskSlot,
                                            layout->lightParamSlot, state.liftKey, state.ambientKey, state.nightCurveToggle));
        }

        // The frame's blend and the same-frame capture: one CPU-side chain, no GPU read back, never a wait. The
        // capture keys on the frame counter's value (a new capture per value), the blend reads the previous captures.
        // Only the main view's sun feeds the night curve's colour (2026-11-07, relight::FeedsNightColour): the character view's directional
        // light is served by this route too (its mask is lifted the same way), but its own light rig is not the frame's sun.
        if (relight::FeedsNightColour(layout->fakeSpecular, relight::AnyIndexedFakeSpecular()))
            CaptureLightColour(context, inputs.lightParam.Get(), ext::present::FrameCount(), layout->id);
        const Blend blend = BlendNow(state, context);
        if (!blend.maskRuns)
            ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::ShadowLift, ext::ffxiv::diag::StageOutcome::Declined,
                                          "the lift is identity or the day curve is serving: the game's own mask is used");
        if (!blend.maskRuns)
        {
            state.session.NoteRun(); // a served draw (the classification passed and the colour was captured) ends the decline streak
            draw(user);              // the day curve (or the no-reading state): the game's own mask, no copies, nothing else counted
            return true;
        }

        // The pass: one explicit state save and restore, one draw, the copy returned.
        MaskInputs in;
        in.mask = inputs.mask.Get();
        in.maskFormat = inputs.maskFormat;
        in.maskWidth = inputs.maskWidth;
        in.maskHeight = inputs.maskHeight;
        in.window[0] = inputs.window[0];
        in.window[1] = inputs.window[1];
        in.window[2] = inputs.window[2];
        in.window[3] = inputs.window[3];
        in.lift = blend.maskLift;
        in.softness = blend.softness;
        ID3D11ShaderResourceView* copy = nullptr;
        {
            // The pass draws its own triangle through the same hooks; its own explicit state save and restore keeps the
            // game's pipeline out of it, and the owner guard above keeps the route off it (a nested hook re-entry
            // sees the owner and passes through). A failure is the caller's decline, with the pass's own reason.
                    copy = state.pass.Run(context, in);
        }
        if (copy == nullptr)
        {
            if (state.log.Take(0xB0000000u))
                LOG_INFO("Shadow mapping: the mask copy could not run ({})", state.pass.Failure() ? state.pass.Failure() : "the pass failed");
            return decline(relight::Decline::PassFailed);
        }

        // Bind the copy for exactly one draw and put the game's own binding back afterwards, also when the draw
        // throws - the source lines are ContactShadowsApply.cpp's contract, and the pin test pins them.
        ID3D11ShaderResourceView* original = nullptr;
        context->PSGetShaderResources(layout->maskSlot, 1, &original);
        struct Restore
        {
            ID3D11DeviceContext* context;
            uint32_t slot;
            ID3D11ShaderResourceView* view;
            ~Restore()
            {
                context->PSSetShaderResources(slot, 1, &view);
                if (view != nullptr)
                    view->Release();
            }
        } restore { context, layout->maskSlot, original };
        context->PSSetShaderResources(layout->maskSlot, 1, &copy);
        drew = true;
        draw(user);

        state.runs++;
        state.session.NoteRun();
        // The capture frame's pictures of this stage: the engine's mask and the lifted copy the draw sampled instead, copied after the pass returned
        // (its explicit-state guard is gone; the copy sets no state and draws nothing).
        ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::ShadowLift, context, in.mask, copy,
                                   ext::ffxiv::diag::StageWindow { in.window[0], in.window[1], in.window[2], in.window[3] });
        state.refusedShapes.erase(layout->id); // a shader that has run is never remembered as refused
        // The run's line: size, format and lift, once per state change (the tag carries the mask's size and the
        // values' step, so a wandering curve writes only its real moves).
        if (state.log.Take(StateTag(0x2u, blend.curve, blend.maskLift, blend.softness) |
                           ((facts.maskWidth & 0x7FFFu) << 24)))
            LOG_INFO("{}", FormatMaskRun(facts.maskWidth, facts.maskHeight, facts.maskFormat, blend.maskLift,
                                         blend.softness, blend.curve, state.haveRead));
        return true;
    }
    catch (...)
    {
        return decline(relight::Decline::Exception);
    }
}



bool AmbientWantsSink() { return AmbientWantsFast(S()); }

// The capture sink (the hooks' table the only seam surface): the sink reads the frame's own counter, so the relight
// caller carries no present dependency.
void CaptureLightColourSink(ID3D11DeviceContext* context, ID3D11Buffer* lightParam)
{
    CaptureLightColour(context, lightParam, ext::present::FrameCount(), 0u); // the contact seam carries no route id
}

// The engine pass overrides' ambient arm asks here, per composite draw: the two entries in the shadows' own slots
// appended after the shipped knob's entries (CbScale applies every matching entry in order, multiplicatively). The
// curve the dispatch ran under is returned for the run line; a 0-count answer is "no change", not a refusal.
uint32_t AmbientEntriesSink(ID3D11DeviceContext* context, engine::ScaleEntry* out, uint32_t capacity, float* outCurve)
{
    State& state = S();
    if (context == nullptr || out == nullptr)
        return 0;
    if (!AmbientWantsFast(state))
        return 0;
    ++state.ambientAsks;
    if (SceneHdrDecline(state))
        return 0; // inert while the engine's scene HDR path runs
    const Blend blend = BlendNow(state, context);
    if (!blend.ambientRuns)
    {
        // the day curve (or the no-reading state): the engine's own table, no dispatch. A ticked curve that keeps waiting for a
        // colour that never lands says why, once (the counters tell the capture path apart from the read window).
        if (!state.haveRead && state.nightCurveToggle && ++state.noColourAsks == kNoColourAsks && TakeIdle(state, Idle::NoColourYet))
            LOG_INFO("{}", FormatIdleNoColour(kNoColourAsks, state.routeDrawsSeen, state.colourCopies, state.colourReads,
                                              state.colourNotReady));
        return 0;
    }
    const uint32_t count = AmbientEntries(blend.ambientScale, out, capacity);
    if (count == 0)
    {
        // the entries did not fit: the arm's decline covers the shape, the provider answers only
        if (TakeIdle(state, Idle::EntriesDidNotFit))
            LOG_INFO("{}", FormatIdleEntries());
        return 0;
    }
    if (outCurve != nullptr)
        *outCurve = blend.curve;
    ++state.combos;
    return count;
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.keyOn = state.keyOn;
    status.maskAsk = MaskWants(state);
    status.liftKeyOff = IsLiftIdentity(ClampLift(state.liftKey));
    status.ambientKeyOff = IsAmbientIdentity(ClampAmbientLift(state.ambientKey));
    status.standDown = state.session.off;
    status.sceneHdr = state.sceneHdrInactive;
    status.haveReading = state.haveRead;
    status.curve = state.lastCurve;
    status.routeDrawsSeen = state.routeDrawsSeen;
    status.routeRuns = state.runs;
    status.routeDeclined = state.declined;
    status.colourCopies = state.colourCopies;
    status.colourReads = state.colourReads;
    status.colourNotReady = state.colourNotReady;
    status.ambientAsked = state.combos;
    return status;
}

// ---- the config side and the registry --------------------------------------------------------------------------------

void OnConfigLoaded(bool on)
{
    State& state = S();
    state.keyOn = on;
    PublishParams();
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.pass.Reset();
        state.refusedShapes.clear();
        state.sceneHdrWatch = shafts::SceneHdrWatch {};
        state.loggedOff = false;
        state.loggedRoute = false;
        state.loggedCombine = false;
        // A module switched off forgets what it measured: a colour read before must not serve a later session (the time of
        // day has moved), and the card's counters start again with the next one.
        ForgetReading(state);
        state.captureSeq = 0;
        state.haveCaptureKey = false;
        state.colourReads = state.colourNotReady = state.colourCopies = 0;
        state.colourChangeLines = 0;
        state.loggedTwoLights = false;
        state.capturesThisKey = 0;
        state.firstLayoutThisKey = 0;
        state.runs = state.declined = state.shapes = state.combos = 0;
        state.routeDrawsSeen = 0;
        state.idleLogged = 0;
        state.layoutAsks = state.noColourAsks = state.ambientAsks = 0;
        state.sawLayout = false;
        PublishRouteBit(state);
        ext::ffxiv::engine::NotifyShadowArmingChanged();
        return;
    }
    // The player's re-arm after a stand-down counts its own session again; the index is not touched (the relight
    // module fills it whatever any key says).
    if (state.session.off)
        state.session = Session {};
    // A slider move reaches this point on every change (the card publishes after each one): the once-per-state lines of the route, the
    // colour read and the idle reasons are NOT re-armed here, only by a switch-off above, so a drag does not write them again.
    state.loggedOff = false;
    state.loggedCombine = false;
    state.sceneHdrWatch = shafts::SceneHdrWatch {};
    PublishRouteBit(state);
    ext::ffxiv::engine::NotifyShadowArmingChanged();
}

void Release()
{
    State& state = S();
    state.pass.Reset();
    for (ComPtr<ID3D11Buffer>& buffer : state.staging)
        buffer.Reset();
    state.haveStaged[0] = state.haveStaged[1] = state.haveStaged[2] = state.haveStaged[3] = false;
    state.refusedShapes.clear();
}

// The stage dump's question about this stage's values, asked at the capture frame.
std::string StageValues()
{
    const State& state = S();
    return std::format("lift={:.2f} softness={:.2f} ambientLift={:.2f} nightCurve={}", double(state.liftKey), double(state.softnessKey),
                       double(state.ambientKey), state.nightCurveToggle ? 1 : 0);
}
} // namespace ext::ffxiv::shadows

// The registration: the config hook and the six sink hooks, set when this DLL's static initialisers run.
namespace
{
struct RegisterShadowMapping
{
    RegisterShadowMapping()
    {
        ext::FfxivConfig::shadowMappingOnLoaded = &ext::ffxiv::shadows::OnConfigLoaded;
        ext::ffxiv::diag::StageRegister(
            ext::ffxiv::diag::StageId::ShadowLift, +[]() { return ext::Active(ext::F::ShadowMapping); }, &ext::ffxiv::shadows::StageValues);
        using namespace ext::ffxiv::shadows;
        Hooks::maskWants = &MaskWantsSink;
        Hooks::combineLift = &CombineLiftSink;
        Hooks::lightDrawRoute = &LightDrawRouteSink;
        Hooks::captureLightColour = &CaptureLightColourSink;
        Hooks::ambientWants = &AmbientWantsSink;
        Hooks::ambientEntries = &AmbientEntriesSink;
    }
} g_registerShadowMapping;
} // namespace
