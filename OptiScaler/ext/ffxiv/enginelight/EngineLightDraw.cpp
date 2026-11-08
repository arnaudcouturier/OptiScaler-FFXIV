#include "pch.h"

#include "ext/ffxiv/enginelight/EngineLight.h"

#include "ext/ExtFeatures.h"
#include "ext/ffxiv/chara/CharaDispatch.h"    // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h"    // the reflection's plain rows: what a bytecode parse produced (the composite's stamp)
#include "ext/ffxiv/diag/StageDump.h"         // the capture frame's per-stage pictures and line (stage-13); stage-side header only, no link dependency
#include "ext/ffxiv/engine/ChainHooks.h"      // the chain's stage hooks, which this module registers and calls
#include "ext/ffxiv/engine/EnginePass.h"      // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/RecogRules.h"      // FrameStep: one count per rendered frame; ClosesFrame
#include "ext/ffxiv/engine/ShaftsSun.h"       // the tone map parameter ring (ToneReader): the frame's white point, for the meter and the stage line
#include "ext/ffxiv/enginelight/EngineLightPass.h"
#include "ext/ffxiv/shadows/ShadowsHooks.h"   // the night reading (the shared night curve's input) and the feed request
#include "ext/hdr/scene/FfxivSceneHdr.h"      // the scene HDR path replaces the tone chain this stage draws before
#include "ext/present/PresentSeams.h"         // the output-frame counter, the scene-HDR sample's token

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>

// Engine light controls: the module's state, the frame tick, the exposure hold's chain entries and the seam entry (EngineLight.h). The pure rules are EngineLightRules.h, the
// lamp arm EngineLightLamps.cpp, the pass EngineLightPass.cpp, the shader shaders/ExposureHold.hlsl, the card EngineLightMenu.cpp, the config reads EngineLightConfig.cpp.
//
// What it costs while it is off: the engine module's one call per draw into the seam, which returns at the armed test. While it is on, per frame: the composite's draw
// records the scene's identity, copies the 64-byte exposure buffer on the GPU (two tiny copies), reads one landed copy of an earlier frame without waiting, advances the
// controller and publishes the applied weight when it changed; at the tone map, when the hold has work, one draw of the pass (0.01 ms class) hands the chain its answer.
//
// Nothing of the game is written: the scene is read through the view the tone map binds, and scaled into an owned texture. The tone map's own binding is put back after its
// one draw, also when that draw throws.
namespace ext::ffxiv::enginelight
{
namespace
{
using Microsoft::WRL::ComPtr;

// The tone map's own parameter buffer (`cCommonTexParam`) sits in its pixel shader's first constant slot, the stages' own slot for the same buffer.
inline constexpr uint32_t kToneMapSlot = 0;

// The tone map's table of the second exposure system is the 1024x1 texture bound at t1 of the tone map draw.
inline constexpr uint32_t kToneMapTableSlot = 1;

// The once-a-second measurement line and its cap (a long session at night writes about 3,600 lines an hour; the series is what the next session reads).
inline constexpr double kSecondLineSeconds = 1.0;
inline constexpr uint32_t kMaxSecondLines = 7200;

inline constexpr uint32_t kNoFeedFrames = 600;
inline constexpr uint32_t kCountsLogFrames = 3600;

// Host tests only: a clock in ticks and its frequency in place of the wall clock (SetClockForTest). Null in the product.
int64_t (*g_testClock)() = nullptr;
int64_t g_testFrequency = 1;

int64_t Qpc()
{
    if (g_testClock != nullptr)
        return g_testClock();
    LARGE_INTEGER value {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t QpcFrequency()
{
    if (g_testClock != nullptr)
        return g_testFrequency > 0 ? g_testFrequency : 1;
    LARGE_INTEGER value {};
    QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : 1;
}

// The stamp the composite's shader carries: the slot its g_CommonParameter sits at, plus one (0 = not the composite).
constexpr GUID kCommonStampTag { 0x5b2f9e41, 0x1c77, 0x4a30, { 0x9d, 0x62, 0x84, 0x0e, 0x31, 0xa7, 0xc5, 0x19 } };
constexpr uint32_t kConstantBufferSlots = 14;

struct State
{
    Pass pass;
    ExposureReader reader;
    TableSampler table;
    shafts::ToneReader tone; // the tone map's own exposure, read a frame late: the frame's white point (the meter's display terms)
    float whitePoint = shafts::kFallbackWhitePoint;
    bool haveWhitePoint = false;
    uint64_t whiteFrame = 0;
    Session session;
    engine::LogGate log;
    Controller controller;
    bool configured = false;
    uint32_t paramsVersion = 0;
    shafts::FrameStep step; // one count per rendered frame: opened at the composite, closed at the tone map
    shafts::SceneHdrWatch hdr;
    uint64_t frames = 0;
    // The scene identity the composite recorded (pointer compared only, never dereferenced), valid for the frame it was recorded in.
    const void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;
    // The stage's own answered-tracking, never the chain's shared stamp (the stamping contract).
    uint64_t stageFrame = 0;
    bool haveStageFrame = false;
    // The night weight's inputs.
    const char* arm = "none";
    bool haveClock = false;
    int64_t lastQpc = 0;
    bool haveReading = false;
    bool feedMasterOff = false;
    uint64_t answeredWithoutReading = 0;
    // The exposure the engine used: the newest valid CPU read.
    bool haveS = false;
    float s = 0.0f, y = 0.0f;
    uint64_t exposureSampleFrame = 0; // the frame the newest valid read was taken at
    // What was last published.
    bool publishedOnce = false;
    float publishedWeight = 0.0f;
    uint32_t publishedVersion = 0;
    bool wasWanted = false;
    // The table sample and the once-a-second line.
    bool haveTable = false;
    float tableLow = 0.0f, tableMid = 0.0f, tableHigh = 0.0f;
    int64_t lastTableQpc = 0;
    int64_t lastSecondQpc = 0;
    uint32_t secondLines = 0;
    // The card and the log.
    uint32_t runs = 0, declined = 0;
    uint32_t lastWindowWidth = 0, lastWindowHeight = 0;
    uint64_t nextCountsLog = 0;
    uint32_t latchesLogged = 0;
    Phase lastLoggedPhase = Phase::Idle;
    bool loggedOff = false, loggedNoFeed = false, loggedMasterOff = false, loggedCost = false, loggedNoStamp = false;
    uint64_t hdrSampleFrame = 0;
    bool haveHdrSample = false;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

bool SceneHdrRequested() { return ext::hdr::FfxivSceneHdr::Requested(); }

// The module is wanted while the master is on and something would change. The exposure stage's own stand-down (State::session) does NOT disarm it: the lamps, the weight and
// the cb family go on without the tone-map stage.
bool WantedNow(const State& state) { return state.configured && Wanted(state.controller.params); }

bool ArmedInternal(State& state)
{
    if (!WantedNow(state))
        return false;
    return !shafts::SceneHdrInactive(true, SceneHdrRequested());
}

// The night curve's feed (the light colour and the fog sun) is wanted exactly while the module is armed - its own request, whatever any other key says. The forced weight
// (benchmark) never reads a light, so it asks for nothing.
void PublishFeed(State& state)
{
    if (ext::ffxiv::shadows::Hooks::nightFeed != nullptr)
        ext::ffxiv::shadows::Hooks::nightFeed(WantedNow(state) && !state.controller.params.forceNightWeight);
}

// The applied weight and the keys, handed to the lamp arm as one factor and to writer 1's cb-patch family through the contract's hook (null-checked), whenever either
// changed. The factor is exactly 1 while the master is off, the A/B shows the engine's values or the weight is 0.
void ApplyOutputs(State& state, bool force)
{
    const Params& params = state.controller.params;
    const float weight = state.controller.weight;
    SetLampFactor(LampFactor(params, weight));
    if (publishCbFamily != nullptr && (force || !state.publishedOnce || weight != state.publishedWeight || state.paramsVersion != state.publishedVersion))
    {
        publishCbFamily(params, weight);
        state.publishedOnce = true;
        state.publishedWeight = weight;
        state.publishedVersion = state.paramsVersion;
    }
}

// ---- the counters and the lines -----------------------------------------------------------------------------------------------

void NoteDecline(State& state, Decline reason, Outcome outcome)
{
    // A later tone-shaped draw of the frame (the engine's own post-tone copy) is not a decision about the capture frame: it never replaces the real reason.
    if (reason != Decline::NotToneMap)
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::NightGrade, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    state.declined++;
    if (CountsTowardsStandDown(reason) && state.session.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("{}", FormatSwitchedOff(engine::kConsecutiveDeclineLimit));
        // Only the exposure stage stands down: the lamps, the weight and the cb family go on. The seam bit stays while the module is wanted.
    }
    if (state.log.Take(uint32_t(reason) | (uint32_t(outcome) << 8)))
        LOG_INFO("{}", FormatDecline(reason, outcome));
}

float GainNowCpu(const State& state)
{
    return state.controller.Gain(state.haveS, state.s);
}

RunFacts RunFactsOf(const State& state)
{
    RunFacts run;
    run.weight = state.controller.weight;
    run.rawTarget = state.controller.rawTarget;
    run.arm = state.arm;
    run.phase = state.controller.phase;
    run.mode = state.controller.params.exposureMode;
    run.s = state.haveS ? state.s : 0.0f;
    run.sHold = state.controller.sHold;
    run.haveHold = state.controller.haveHold;
    run.gain = GainNowCpu(state);
    run.lamp = LampFactorNow();
    run.haveGpuMs = state.pass.HasGpuMilliseconds();
    run.gpuMs = state.pass.GpuMilliseconds();
    return run;
}

// The stage's numbers for the capture frame's line (the weight, the phase, the exposure and the hold). The capture frame only: the open bit is tested first.
StageFacts StageFactsOf(const State& state)
{
    StageFacts facts;
    facts.phase = state.controller.phase;
    facts.weight = state.controller.weight;
    facts.rawTarget = state.controller.rawTarget;
    facts.arm = state.arm;
    facts.mode = state.controller.params.exposureMode;
    facts.haveS = state.haveS;
    facts.s = state.s;
    facts.sHold = state.controller.sHold;
    facts.gain = GainNowCpu(state);
    facts.lamp = LampFactorNow();
    return facts;
}

void NoteStageNumbers(const State& state)
{
    if (!ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::NightGrade))
        return;
    ext::ffxiv::diag::StageFrameValues values;
    values.haveWhite = true;
    values.white = state.whitePoint;
    values.fade = 1.0f;
    ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::NightGrade, values);
    ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::NightGrade, FormatStageWords(StageFactsOf(state)));
}

// The frame's white point, from the tone map's own parameters at this draw (the stages' own read, this module's own instance).
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
    const WhiteReading reading = ReadWhite(read.landed, read.exposure);
    if (reading.usable)
    {
        state.whitePoint = reading.white;
        state.whiteFrame = state.frames;
        state.haveWhitePoint = true;
        return;
    }
    if (shafts::WhitePointStale(state.haveWhitePoint, state.whiteFrame, state.frames))
    {
        state.haveWhitePoint = false;
        state.whitePoint = shafts::kFallbackWhitePoint;
    }
}

// ---- the frame's reads --------------------------------------------------------------------------------------------------------

// The slot the composite's g_CommonParameter sits at, from the shader's stamp; -1 when the bound pixel shader carries none.
int CommonSlotOfBoundShader(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    if (!shader)
        return -1;
    uint32_t stamped = 0;
    UINT bytes = sizeof stamped;
    if (FAILED(shader->GetPrivateData(kCommonStampTag, &bytes, &stamped)) || bytes != sizeof stamped || stamped == 0 || stamped > kConstantBufferSlots)
        return -1;
    return int(stamped) - 1;
}

// The frame's exposure, copied on the GPU at the composite: the 64-byte buffer the composite binds at the slot its shader names (reflection, never a guess at slot 0).
void CaptureExposure(ID3D11DeviceContext* context, State& state)
{
    const int slot = CommonSlotOfBoundShader(context);
    if (slot < 0)
    {
        if (!state.loggedNoStamp && state.frames > 120)
        {
            state.loggedNoStamp = true;
            LOG_INFO("Engine light: the composite's shader carries no g_CommonParameter stamp, so the engine's exposure cannot be read; the exposure hold stays at gain 1 (the lamps and the light arms go on)");
        }
        return;
    }
    ComPtr<ID3D11Buffer> bound;
    UINT first = 0, count = 0;
    ComPtr<ID3D11DeviceContext1> context1;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))) && context1)
        context1->PSGetConstantBuffers1(UINT(slot), 1, &bound, &first, &count);
    else
        context->PSGetConstantBuffers(UINT(slot), 1, &bound);
    if (!bound)
        return;
    state.reader.Capture(context, bound.Get(), first * 16u, state.frames);
}

// One step of the controller, once per rendered frame at the composite: the night curve's reading from the shadow module (ticked: no pin at 1), the newest landed exposure
// read, the wall-clock step, then the outputs.
void TickFrame(ID3D11DeviceContext* context, State& state)
{
    ext::ffxiv::shadows::NightNow now;
    if (ext::ffxiv::shadows::Hooks::nightNow != nullptr)
        now = ext::ffxiv::shadows::Hooks::nightNow(context);
    state.haveReading = now.have;
    state.arm = ext::ffxiv::shadows::NightArmName(ext::ffxiv::shadows::NightArm(now.arm));
    if (now.have && !now.masterOn)
    {
        state.feedMasterOff = true;
        if (!state.loggedMasterOff)
        {
            state.loggedMasterOff = true;
            LOG_INFO("Engine light: Shadow mapping's master key is off; the night curve is read from the light-colour feed this module asks for, but the card's Key on the visible sun checkbox does not apply here");
        }
    }
    else if (now.have)
        state.feedMasterOff = false;
    if (!now.have && ++state.answeredWithoutReading == kNoFeedFrames && !state.loggedNoFeed && !state.controller.params.forceNightWeight)
    {
        state.loggedNoFeed = true;
        LOG_INFO("{}", FormatNoFeed());
    }
    if (now.have)
        state.answeredWithoutReading = 0;

    const ExposureRead read = state.reader.Read(context, state.frames);
    if (read.landed && read.valid)
    {
        state.haveS = true;
        state.s = read.s;
        state.y = read.y;
        state.exposureSampleFrame = read.writtenFrame;
    }

    const int64_t at = Qpc();
    const float dt = state.haveClock ? float(double(at - state.lastQpc) / double(QpcFrequency())) : 0.0f;
    state.haveClock = true;
    state.lastQpc = at;

    StepInput in;
    in.dt = dt;
    in.haveReading = now.have;
    in.curve = TargetWeight(now.have, now.curve);
    in.sFresh = read.landed;
    in.sValid = read.valid;
    in.s = read.s;
    const uint32_t latchesBefore = state.controller.latches;
    state.controller.Step(in);
    if (state.controller.latches != latchesBefore && state.latchesLogged < 64)
    {
        ++state.latchesLogged;
        LOG_INFO("{}", FormatLatch(state.controller.sHold, state.controller.forcedLatch));
    }
    ApplyOutputs(state, false);

    // The once-a-second measurement line, while the controls are doing anything (settling or acting): s, the table, the hold and the compensation.
    if (state.controller.phase != Phase::Idle && state.secondLines < kMaxSecondLines &&
        (state.lastSecondQpc == 0 || double(at - state.lastSecondQpc) / double(QpcFrequency()) >= kSecondLineSeconds))
    {
        state.lastSecondQpc = at;
        ++state.secondLines;
        SecondFacts facts;
        facts.haveS = state.haveS;
        facts.s = state.s;
        facts.y = state.y;
        facts.haveTable = state.haveTable;
        facts.tableLow = state.tableLow;
        facts.tableMid = state.tableMid;
        facts.tableHigh = state.tableHigh;
        facts.haveHold = state.controller.haveHold;
        facts.sHold = state.controller.sHold;
        facts.gain = GainNowCpu(state);
        facts.weight = state.controller.weight;
        facts.phase = state.controller.phase;
        LOG_INFO("{}", FormatSecond(facts));
    }
    if (state.controller.phase != state.lastLoggedPhase)
    {
        state.lastLoggedPhase = state.controller.phase;
        if (state.log.Take(0x40000100u | uint32_t(state.controller.phase)))
            LOG_INFO("{}", FormatRun(RunFactsOf(state)));
    }
}

// ---- the frame's scene record and the tone map's own shape -------------------------------------------------------------------

// The frame's composite: the one draw a rendered frame reliably makes before its tone map, and the module's frame boundary. The count opens here (once a frame, FrameStep),
// the composite's render target is the scene identity the tone map's t0 test compares, the exposure is copied and the controller steps.
void RecordScene(ID3D11DeviceContext* context, State& state)
{
    const bool opened = state.step.OpenAtComposite(state.frames);
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
    if (opened && !ext::ffxiv::chara::t_replay)
    {
        CaptureExposure(context, state);
        TickFrame(context, state);
    }
}

// The tone map's own shape, measured at the draw (the stages' own measurement, this module's own recorded scene).
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

// The frame's tone map closes the frame (RecogRules.h ClosesFrame): the next frame's first composite then opens the next count.
void CloseFrameAtToneMap(State& state, const shafts::ToneMapFacts& facts, bool chained)
{
    if (shafts::ClosesFrame(facts, chained))
        state.step.CloseAtToneMap();
}

// The stages after this one (the atmosphere, then depth of field, the chain's last member): composed on this stage's own answer through the chain's own dispatcher.
void ComposeChainNext(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView*& ours)
{
    if (ours == nullptr)
        return;
    if (ID3D11ShaderResourceView* composed = ext::ffxiv::engine::chainAfterNightScene(context, facts, ours))
        ours = composed;
}

void LogCountsIfDue(State& state)
{
    if (state.frames < state.nextCountsLog)
        return;
    state.nextCountsLog = state.frames + kCountsLogFrames;
    LOG_INFO("Engine light: {} frames seen, {} exposure-hold passes, {} declined; {} (weight {:.3f}, night curve {:.3f}, {}), lamps x{:.2f}", state.frames, state.runs, state.declined,
             PhaseName(state.controller.phase), double(state.controller.weight), double(state.controller.rawTarget), state.arm, double(LampFactorNow()));
}

// Samples the engine's luminance-rebalance table once a second, at the tone map where it is bound (t1), for the log line.
void SampleTable(ID3D11DeviceContext* context, State& state)
{
    const TableRead read = state.table.Read(context);
    if (read.landed)
    {
        state.haveTable = true;
        state.tableLow = read.low;
        state.tableMid = read.mid;
        state.tableHigh = read.high;
    }
    const int64_t now = Qpc();
    if (state.lastTableQpc != 0 && double(now - state.lastTableQpc) / double(QpcFrequency()) < kSecondLineSeconds)
        return;
    ID3D11ShaderResourceView* view = nullptr;
    context->PSGetShaderResources(kToneMapTableSlot, 1, &view);
    ComPtr<ID3D11ShaderResourceView> table;
    table.Attach(view);
    if (table && state.table.Sample(context, table.Get()))
        state.lastTableQpc = now;
}

// One at-one-tone-map run, either role. The game's draw runs exactly once, whoever owns it: the chained role returns this stage's texture (or null, which leaves the chain's
// hold standing), and the owner role binds its output at t0 for the one draw, and puts the game's own binding back afterwards, also when that draw throws.
ID3D11ShaderResourceView* StageAtToneMap(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, bool owner, DrawFn draw,
                                         void* user, bool& drew)
{
    const Outcome declineOutcome = owner ? Outcome::EngineOwn : Outcome::HoldStands;
    if (shafts::SceneHdrInactive(true, SceneHdrRequested()))
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

    ReadToneMapParams(context, state);
    state.lastWindowWidth = uint32_t(facts.window.width);
    state.lastWindowHeight = uint32_t(facts.window.height);
    if (state.controller.phase != Phase::Idle)
        SampleTable(context, state);

    // The pass's inputs: the hold has work only with a stage-wanted configuration, an applied weight above 0, and (auto) a latched hold plus THIS frame's exposure copy.
    const Controller& c = state.controller;
    ID3D11ShaderResourceView* ours = nullptr;
    Inputs inputs;
    inputs.scene = scene;
    inputs.sceneFormat = facts.sceneFormat;
    inputs.sceneWidth = facts.sceneWidth;
    inputs.sceneHeight = facts.sceneHeight;
    inputs.window = facts.window;
    inputs.mode = c.params.exposureMode;
    bool hasWork = StageWanted(c.params) && !c.params.referenceAB && c.weight > 0.0f && state.session.Armed();
    if (hasWork && c.params.exposureMode == kExposureAuto)
    {
        inputs.sHold = c.sHold;
        inputs.exposureCopy = (c.haveHold && state.reader.LiveFrame() == state.frames) ? state.reader.Live() : nullptr;
        hasWork = inputs.exposureCopy != nullptr;
    }
    else if (hasWork)
        inputs.manualGain = StoredGain(c.params, c.weight, false, 0.0f, false, 0.0f);

    if (!hasWork)
    {
        // The engine's own picture: no pass, nothing bound. Not a failure: the streak starts over.
        state.session.NoteRun();
        ext::ffxiv::diag::MeterSkipped(ext::ffxiv::diag::StageId::NightGrade, "the exposure hold has no work (day, A/B, hold not latched yet, or the mode is off): the frame is the engine's own");
        if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::NightGrade))
        {
            ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::NightGrade, ext::ffxiv::diag::StageOutcome::Declined, FormatNoWorkReason(c.phase, c.weight, c.params.exposureMode).c_str());
            NoteStageNumbers(state);
        }
    }
    else
    {
        {
            ext::ffxiv::chara::ReplayScope own;
            ext::ffxiv::engine::OwnWork ownWork;
            ours = state.pass.Run(context, inputs);
        }
        if (ours == nullptr)
        {
            const char* failure = state.pass.Failure();
            if (failure != nullptr)
            {
                if (state.log.Take(0x40000001u))
                    LOG_INFO("Engine light: the exposure hold's pass did not run ({})", failure);
                NoteDecline(state, Decline::PassFailed, declineOutcome);
            }
            return nullptr;
        }
        // The capture frame's pictures of this stage: the scene it was handed and the answer it made, copied now (its pass is over and its guards are gone) and before the
        // stage after it composes on the answer; then its numbers (stage-13's line).
        ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::NightGrade, context, scene, ours,
                                   ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height, state.whitePoint });
        NoteStageNumbers(state);
        state.runs++;
        state.session.NoteRun();
        if (!state.loggedCost && state.pass.HasGpuMilliseconds())
        {
            state.loggedCost = true;
            LOG_INFO("Engine light: the exposure hold's pass costs {:.3f} ms of GPU time at {}x{} (one draw over the render window)", state.pass.GpuMilliseconds(),
                     uint32_t(facts.window.width), uint32_t(facts.window.height));
        }
    }

    // The stage after this one composes on this stage's answer, or - with no answer - hands the stage after it nothing from here: the chain's own texture stays.
    ComposeChainNext(context, facts, ours);
    if (ours == nullptr)
        return nullptr;
    if (!owner)
        return ours;

    // The owner's one bind: this stage's copy of the scene at t0 for the one draw, and the game's own view back afterwards, also when that draw throws.
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
    drew = true; // set BEFORE the draw: the game's call is attempted exactly once, and a throw out of it must never be retried by the owner's catch
    draw(user);
    return ours;
}

// The stage as the chain's fallback owner: the entry the earlier stages' owner entries and the seam's own call reach when none of them answered.
bool RunToneMapAsOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    bool drew = false;
    try
    {
        LogCountsIfDue(state);
        StageAtToneMap(state, context, facts, scene, true, draw, user, drew);
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
    // This stage's own decline is the chain's next stage's owner turn (ChainHooks.h's toneMapAfterNightOwner: the atmosphere, else depth of field): it owns the
    // game's draw on exactly the contract this entry carries (true only when it drew; false leaves the game's draw to the caller). It is asked outside the try on
    // purpose: that stage catches everything itself, and a throw after it drew must not reach this stage's catch, which would draw the game's call a second time.
    return ext::ffxiv::engine::toneMapAfterNightOwner(context, facts, scene, draw, user);
}

// The stage as a chain member: the entry the light bounce's stage calls with the chain's current output. A texture comes back for the caller to bind at t0; null
// means the stage has nothing for this frame, and the caller's own texture - the chain's last good picture - stays at t0 (the hold stands).
ID3D11ShaderResourceView* ChainSceneInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr)
        return nullptr;
    ID3D11ShaderResourceView* ours = nullptr;
    // The close needs no armed test: a stage that is off never opens a count, so its frame is closed already and the close is a no-op.
    CloseFrameAtToneMap(state, facts, true);
    // This stage's own armed test and its own one-answer gate guard only this stage's attempt: the stage after it (depth of field) is reachable whether this
    // stage is off, has answered the frame already, declines or throws.
    if (ArmedInternal(state) && state.session.Armed() && !(state.haveStageFrame && state.stageFrame == state.frames))
    {
        // The chained role answers at most one tone map a frame, by this stage's own counter: a second tone-shaped draw of the frame is the engine's own
        // post-tone copy, and the game's own call.
        state.haveStageFrame = true;
        state.stageFrame = state.frames;
        try
        {
            bool drew = false; // the chained role never draws: the caller binds and draws
            ours = StageAtToneMap(state, context, facts, scene, false, nullptr, nullptr, drew);
        }
        catch (...)
        {
            LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::HoldStands));
            ours = nullptr; // the chain's hold stands
        }
    }
    // This stage answered: the stage after it was already handed that answer inside the stage (ComposeChainNext), so its result is final.
    if (ours != nullptr)
        return ours;
    // This stage has no answer and the chain's hold stands. Every one of those exits hands the stage after it (ChainHooks.h's chainAfterNightScene: the atmosphere, else
    // depth of field) the texture this call was given, so that stage is reachable with this stage off; it applies its own armed test and its own one-answer gate.
    return ext::ffxiv::engine::chainAfterNightScene(context, facts, scene);
}

// The stage as the chain's fallback owner, through the chain's own hook: the light bounce's owner entry reaches here at both of its early exits and after its
// own owner role declined.
bool ToneMapOwnerInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    // Both early exits hand the stage after this one (ChainHooks.h's toneMapAfterNightOwner) the game's scene: the chain's fallback-owner branch reaches this entry whether
    // it is armed or not, so a stage placed after these returns would never run. The stage after owns the game's draw on exactly the contract this entry carries.
    if (context == nullptr || scene == nullptr || draw == nullptr || !ArmedInternal(state) || !state.session.Armed())
        return ext::ffxiv::engine::toneMapAfterNightOwner(context, facts, scene, draw, user);
    CloseFrameAtToneMap(state, facts, true);
    if (state.haveStageFrame && state.stageFrame == state.frames)
    {
        // this frame's tone map was already answered, by this stage
        return ext::ffxiv::engine::toneMapAfterNightOwner(context, facts, scene, draw, user);
    }
    state.haveStageFrame = true;
    state.stageFrame = state.frames;
    return RunToneMapAsOwner(context, facts, scene, draw, user);
}

// The seam's own entry: the frame tick at the composite, and the tone map's fallback owner when neither earlier stage nor the march nor the light bounce took the draw.
bool OnSeamDrawInternal(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return false;
    if (!ArmedInternal(state) || context == nullptr)
        return false;
    try
    {
        if (ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors))
        {
            RecordScene(context, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors))
        {
            // The earlier stages had their chances above this entry in the dispatch; reaching here with a tone map means none took it, and this stage is the chain's
            // fallback owner. The shape is measured here, and a draw that is not the tone map is the game's own call.
            ComPtr<ID3D11ShaderResourceView> scene;
            const shafts::ToneMapFacts facts = MeasureToneMap(context, state, scene);
            if (shafts::ClassifyToneMap(facts) == shafts::Decline::NotToneMap)
                return false;
            CloseFrameAtToneMap(state, facts, false);
            if (!state.session.Armed())
                return false; // the exposure stage stood itself down: the next stage's turn comes from the dispatch
            if (state.haveStageFrame && state.stageFrame == state.frames)
                return false;
            state.haveStageFrame = true;
            state.stageFrame = state.frames;
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

void ObserveSceneHdr(State& state)
{
    if (state.hdr.Sample(state.configured && state.controller.params.master, SceneHdrRequested()))
        LOG_INFO("{}", kSceneHdrInactive);
}
} // namespace

// ---- the module's exports (EngineLight.h) ---------------------------------------------------------------------------------------

bool Armed()
{
    State& state = S();
    return ArmedInternal(state);
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

bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user) { return OnSeamDrawInternal(context, anchors, draw, user); }

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

// The fallback-owner role. The owner contract: true only when this stage drew the game's call. The internals catch around everything that can run after the draw
// (RunToneMapAsOwner); an exception that gets out of them happened before it, so false (the caller draws once) is the honest answer.
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

void Configure(const Params& params)
{
    State& state = S();
    const bool wasWanted = state.wasWanted;
    state.configured = true;
    state.controller.SetParams(params);
    ++state.paramsVersion;
    ObserveSceneHdr(state);
    const bool wanted = WantedNow(state);
    if (wanted && !wasWanted)
    {
        // Armed again (or for the first time): nothing watched the engine's exposure while the module was off, so there is no latch to trust.
        state.controller.ForgetHold();
        state.session = state.session.off ? Session {} : state.session;
        state.log = engine::LogGate {};
        state.loggedOff = state.loggedNoFeed = state.loggedMasterOff = state.loggedCost = state.loggedNoStamp = false;
        state.haveClock = false;
        state.nextCountsLog = 0;
        state.answeredWithoutReading = 0;
        state.lastLoggedPhase = Phase::Idle;
    }
    if (!wanted)
    {
        state.pass.Reset();
        state.reader.Reset();
        state.table.Reset();
        state.tone.Reset();
        state.haveWhitePoint = false;
        state.whitePoint = shafts::kFallbackWhitePoint;
        state.step.Reset();
        state.sceneResource = nullptr;
        state.sceneFrames = 0;
        state.haveStageFrame = false;
        state.haveS = false;
        state.haveTable = false;
        state.controller.phase = Phase::Idle;
    }
    state.wasWanted = wanted;
    ext::SetActive(ext::F::NightGrade, wanted);
    PublishFeed(state);
    ApplyOutputs(state, true);
}

void Release()
{
    State& state = S();
    state.pass.Reset();
    state.reader.Reset();
    state.table.Reset();
    state.tone.Reset();
    state.sceneResource = nullptr;
    ReleaseLamps();
}

void SetClockForTest(int64_t (*clock)(), int64_t frequency)
{
    g_testClock = clock;
    g_testFrequency = frequency;
    State& state = S();
    state.haveClock = false;
}

Status CurrentStatus()
{
    State& state = S();
    const Controller& c = state.controller;
    Status status;
    status.configured = state.configured;
    status.armed = ArmedInternal(state);
    status.hdrInactive = shafts::SceneHdrInactive(state.configured && c.params.master, SceneHdrRequested());
    status.standDown = state.session.off;
    status.haveReading = state.haveReading;
    status.feedMasterOff = state.feedMasterOff;
    status.params = c.params;
    status.phase = c.phase;
    status.weight = c.weight;
    status.rawTarget = c.rawTarget;
    status.arm = state.arm;
    status.haveS = state.haveS;
    status.s = state.s;
    status.y = state.y;
    status.haveHold = c.haveHold;
    status.sHold = c.sHold;
    status.forcedLatch = c.forcedLatch;
    status.gain = GainNowCpu(state);
    status.lampFactor = LampFactorNow();
    status.haveTable = state.haveTable;
    status.tableLow = state.tableLow;
    status.tableMid = state.tableMid;
    status.tableHigh = state.tableHigh;
    status.frames = state.frames;
    status.runs = state.runs;
    status.declined = state.declined;
    status.haveGpuMs = state.pass.HasGpuMilliseconds();
    status.gpuMs = state.pass.GpuMilliseconds();
    status.lampsScaled = CurrentLampStatus().scaled;
    return status;
}

std::string StageValues()
{
    const Params& p = S().controller.params;
    return std::format("master={} ab={} moon={:.2f} ambient={:.2f} waterMoon={:.2f} waterAmbient={:.2f} lamps={:.2f} exposure={} manual={:.2f} forceWeight={}", p.master ? 1 : 0, p.referenceAB ? 1 : 0,
                       double(p.moonIntensity), double(p.ambientIntensity), double(p.waterMoon), double(p.waterAmbient), double(p.lampIntensity), p.exposureMode, double(p.exposureManual),
                       p.forceNightWeight ? 1 : 0);
}

void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    try
    {
        ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
        const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
        // Only the composite takes the stamp: it declares the ambient probe table AND the scene-pass g_CommonParameter (the material shaders declare the second only).
        const uint32_t ambient = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "g_AmbientParamArray");
        const uint32_t common = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "g_CommonParameter");
        if (ambient >= kConstantBufferSlots || common >= kConstantBufferSlots)
            return;
        const uint32_t stamp = common + 1;
        shader->SetPrivateData(kCommonStampTag, sizeof stamp, &stamp);
    }
    catch (...)
    {
    }
}

namespace
{
// The registration of the chain's stage hooks, set when this DLL's static initialisers run: the light bounce's entries reach the stage through the pointers the chain's own
// header declares (null in every build that does not link this file, which is this stage being off - the chain runs unchanged without it).
struct RegisterEngineLight
{
    RegisterEngineLight()
    {
        ext::ffxiv::engine::chainNightScene = &ext::ffxiv::enginelight::ChainScene;
        ext::ffxiv::engine::toneMapNightOwner = &ext::ffxiv::enginelight::ToneMapOwner;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::NightGrade, &ext::ffxiv::enginelight::Armed, &ext::ffxiv::enginelight::StageValues);
    }
} g_registerEngineLight;
} // namespace
} // namespace ext::ffxiv::enginelight
