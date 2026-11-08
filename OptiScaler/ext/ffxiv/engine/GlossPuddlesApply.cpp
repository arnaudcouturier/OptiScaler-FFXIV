#include "pch.h"

#include "ext/ffxiv/engine/GlossPuddlesApply.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"  // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h"  // the DXBC RDEF reader the engine's own index uses
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame
#include "ext/ffxiv/engine/EnginePass.h"    // the module's own feature bit and publish
#include "ext/ffxiv/engine/GlossPuddlesPass.h"
#include "ext/ffxiv/engine/PuddleTone.h"
#include "ext/present/PresentSeams.h"       // the output-frame counter, the frame token

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <string_view>

// Puddles (ext/ffxiv/engine): the seam's entries and the draw path (GlossPuddlesApply.h). The pure decisions are in GlossPuddlesRules.h, the GPU
// work in GlossPuddlesPass.cpp.
//
// When a shader is stamped: while the game creates its pixel shaders, whatever the key says, because the game creates the shaders of its light
// passes, its composite and its forward draws while it loads and a switch turned on later has to find them. A cheap byte scan for the four
// G-buffer target names drops every other shader before the reflection parse; nothing is logged while the key is off. The stamp is written on the
// shader object, so there is no table to fill and nothing to overflow.
//
// Cost with the key off: one relaxed load per draw (the caller's) and nothing else - nothing here is reached for a draw whose shader the index
// did not claim as a reader. Cost with the key on: one relaxed load per reader draw, one PSGetShaderResources for its slot, one GetResource, and
// the slot swap and its restore; the frame's first complete reader also runs the pass once.
namespace ext::ffxiv::engine::gloss
{
namespace
{
using Microsoft::WRL::ComPtr;

// The four names that make a shader a reader of a G-buffer target: the shared table's own rows, the rows the draw path classifies a reader with.
// A shader that declares none of them can never be served by this pass, so the creation hook reads a shader's reflection only when it does.
bool MentionsTarget(const void* code, size_t size)
{
    if (code == nullptr || size < 8)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (const char* name : kTargetNames)
    {
        const size_t length = std::strlen(name);
        if (size < length)
            continue;
        for (size_t i = 0; i + length <= size; ++i)
            if (bytes[i] == static_cast<unsigned char>(name[0]) && std::memcmp(bytes + i, name, length) == 0)
                return true;
    }
    return false;
}

// The slots one shader declares, from the same reflection the shared index reads. The names arrive with the effect compiler's type suffix
// (`g_SamplerDepth.T` for the texture, `g_SamplerDepth.S` for its sampler), which diag::ResourceBaseName strips; the type is what keeps the two
// apart, and the selection itself lives in the pure rules so a test can feed it a real reflection table (GlossPuddlesRules.h's InputsFromBindings).
Inputs InputsOfShader(const void* code, size_t size)
{
    if (code == nullptr || size < 32)
        return Inputs {};
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    return InputsFromBindings(parsed, count);
}

// One bound texture as the classification needs it: its format and size, the resource behind it and its sample count. The caller keeps the
// view's reference; nothing here owns it.
struct BoundTexture
{
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Texture2D* texture = nullptr;
    const void* resource = nullptr;
    uint32_t format = 0, width = 0, height = 0, samples = 0;
    bool twoD = false;
};

BoundTexture Describe(ID3D11ShaderResourceView* view)
{
    BoundTexture out;
    if (view == nullptr)
        return out;
    out.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    view->GetDesc(&desc);
    out.format = uint32_t(desc.Format);
    out.samples = 1;
    out.twoD = desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource)
        return out;
    out.resource = resource.Get();
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        out.twoD = false;
        return out;
    }
    D3D11_TEXTURE2D_DESC texture {};
    static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
    out.texture = static_cast<ID3D11Texture2D*>(resource.Get());
    out.width = texture.Width;
    out.height = texture.Height;
    out.samples = texture.SampleDesc.Count;
    out.twoD = out.twoD && texture.ArraySize == 1;
    return out;
}

// One bound view the caller owns once, and either releases or hands to a swap guard.
struct ViewRef
{
    ID3D11ShaderResourceView* view = nullptr;
    ~ViewRef() { Reset(); }
    void Reset()
    {
        if (view != nullptr)
            view->Release();
        view = nullptr;
    }
    ID3D11ShaderResourceView* Release()
    {
        ID3D11ShaderResourceView* released = view;
        view = nullptr;
        return released;
    }
};

struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    // The frame's memory of the engine's target, its view and this module's result (GlossPuddlesRules.h's FrameCache shape).
    FrameCache frame;
    std::atomic<bool> keyOn { false };
    std::atomic<bool> armed { false };  // the published feature bit's own condition (the shared arming asks for it)
    std::atomic<bool> inPass { false }; // true while this module's own GPU work runs
    uint32_t runs = 0;
    uint32_t readers = 0;
    uint32_t declined = 0;
    uint32_t servedLastFrame = 0; // the reader draws of the last finished frame
    uint32_t declines[kDeclineReasonCount] {};
    // The engine's wetness amount is copied once per frame, from whichever draw that binds it comes first in the frame.
    uint64_t wetnessFrame = 0;
    bool hasWetnessFrame = false;
    bool wetnessKnown = false;
    bool wetnessForced = false;
    // The wetness gate's dwell is no longer a field of its own: it is the pass's shared latch, which both stages read
    // (GlossPuddlesRules.h's pairing), so neither stage can disagree with the other one inside a frame.
    ReaderFacts lastFacts;
    bool haveRun = false;
    bool loggedOff = false;
    bool loggedRun = false;       // the one line that says what the pass shapes
    LogGate gateLog;              // the gate's lines: one per change of state (open, mixed, under cover), with a budget of their own
    uint32_t loggedGateUnreadable = 0; // the target key whose "no engine gloss to read" line the log already carries (0: none)
    bool loggedServed = false;    // the one line that says how many readers a frame served
    bool loggedFailure = false;   // the first failure's reason, once
    bool loggedCamera = false;    // the camera layout refusal, once
    uint32_t loggedTarget = 0;    // the target key value whose refusal the log already carries (0: none)
    uint32_t loggedChannel = 0;   // and the target and channel pair, packed (+1), whose refusal it carries (0: none)
    const char* failure = nullptr;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// Marks this module's own GPU work: its draw re-enters the draw seam (it is drawn on the game's own immediate context) and the shared module must
// never classify or count it. A nested guard restores, never clears.
struct OwnWork
{
    const bool previous;
    OwnWork() : previous(S().inPass.exchange(true, std::memory_order_relaxed))
    {
        if (!previous)
            engine::SetOwnWork(true);
    }
    ~OwnWork()
    {
        if (!previous)
            engine::SetOwnWork(false);
        S().inPass.store(previous, std::memory_order_relaxed);
    }
    OwnWork(const OwnWork&) = delete;
    OwnWork& operator=(const OwnWork&) = delete;
};

// One refused reader. Every reason leaves the draw exactly as the game made it, and each is one line of the log the first time it happens.
// `line` replaces the short line when the caller has something more to say (the first reader of a frame reports what it bound, so a session can
// tell the two shapes that refuse alike apart). `counted` is false for the camera layout refusal: a loading screen's camera is not a build this
// module cannot serve, so it does not walk the module towards its switch-off.
Decline Refuse(Decline reason, std::string_view line = {}, bool counted = true)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Gloss, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", line.empty() ? std::string(FormatDecline(reason)) : std::string(line));
    if (counted && state.session.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("{}", FormatSwitchedOff());
        // The seam this module armed is closed from here on, so the shared latch and the shape machinery would freeze at
        // their last values while the tone stage's own session may still run: clear them together, the Publish(false)
        // precedent - both stages disarm together, and a later live scene cannot hold a frozen answer. The pass's Reset
        // drops the latch, the shape stamp and the hold's output flag with its own state.
        state.pass.Reset();
        state.hasWetnessFrame = false;
        state.wetnessKnown = false;
        state.wetnessForced = false;
        state.frame.Reset();
        state.pass.Reset();
    }
    return reason;
}

// The engine's wetness amount for this frame, from the first draw that binds it, and the wetness the puddles must gate on. The copy is one
// 32-byte GPU-side transfer, no map on this path; the amount the engine's OWN draws read is the wetness knob's forced value while that knob is
// armed (EnginePassRules.h's wetness entry patches the game's buffer for the material draws, and the copy this module takes in the same frame holds
// the unpatched bytes), so the pass's buffer is overwritten with that value. Below zero the knob is off and the copy above is the value. A frame
// whose material draws come after its light draws keeps the previous frame's value, which is the same value to within the weather's ramp. The
// frame is only marked as taken when a copy or the knob's value landed: a draw that binds nothing, or another buffer, under the engine's wetness
// name leaves the frame open, so a later draw of the same frame hands over the real value instead of leaving the pass a frame behind.
// The wetness knob's effective value: only while the engine pass overrides switch is on can its slider force the weather.
float WetnessKnob()
{
    const float knob = engine::ClampWetnessOverride(ext::Cfg().ffxiv.wetnessOverride.value_or_default());
    return WetnessKnobFor(ext::Cfg().ffxiv.enginePassOverrides.value_or_default(), knob);
}

void NoteWetnessFrame(ID3D11DeviceContext* context, State& state, uint64_t frameToken, uint32_t slot)
{
    if (state.hasWetnessFrame && state.wetnessFrame == frameToken)
        return;
    bool copied = false;
    if (slot < kConstantBufferSlots)
    {
        ComPtr<ID3D11Buffer> wetness;
        context->PSGetConstantBuffers(slot, 1, &wetness);
        copied = wetness != nullptr && state.pass.NoteWetness(context, wetness.Get());
    }
    if (copied)
        state.wetnessKnown = true; // the engine's own amount is copied (and the run line's figure read back on a later frame)
    // The wetness the engine's own draws read while the knob is armed is the knob's forced amount, and the copy above was taken before the knob
    // patched the game's buffer: the pass's own buffer is overwritten with it, so the shape follows what the material draws read.
    const float knob = WetnessKnob();
    bool forced = false;
    if (knob >= 0.0f)
        forced = state.pass.SetWetness(context, WetnessFor(state.pass.EngineWetness(), knob));
    else if (!copied && state.wetnessForced)
        state.pass.SetWetness(context, state.pass.EngineWetnessKnown() ? state.pass.EngineWetness() : 0.0f);
    state.wetnessForced = forced;
    // The wetness gate's one latch (GlossPuddlesRules.h's WetEnoughDwell), answered on the frame's own copy - the
    // read's own tick, wherever the buffer landed in the frame. Both stages read the pass's bit from here on, so the
    // two answers cannot disagree inside a frame: the tone map's stage cannot be armed a frame the gloss stage's
    // readers were dry or the other way round, which was one of the ramp's crossings where the two could flip apart.
    const Wetness wet = CurrentWetness();
    state.pass.NoteWetLatch(WetEnoughDwell(state.pass.WetLatched(), wet.known, wet.amount));
    if (!copied && !forced)
        return; // not the engine's buffer and no knob: a later draw of this frame may still hand over the real one
    state.hasWetnessFrame = true;
    state.wetnessFrame = frameToken;
}

// The frame's one attempt: reads the reader's inputs, decides, and runs the pass. Returns this module's view for the frame, or null after a refusal
// (which is counted here).
ID3D11ShaderResourceView* RunFrame(ID3D11DeviceContext* context, State& state, const BoundTexture& target, const Inputs& inputs)
{
    const uint32_t targetIndex = TargetIndex(state.params);
    ReaderFacts facts;
    facts.hasTarget = true;
    facts.targetFormat = target.format;
    facts.targetWidth = target.width;
    facts.targetHeight = target.height;
    facts.targetSamples = target.samples;
    facts.replay = ext::ffxiv::chara::t_replay;

    ComPtr<ID3D11ShaderResourceView> depth, normal;
    if (inputs.HasDepth())
        context->PSGetShaderResources(inputs.depth, 1, &depth);
    if (inputs.HasTarget(3))
        context->PSGetShaderResources(inputs.targets[3], 1, &normal);
    ComPtr<ID3D11Buffer> camera;
    if (inputs.HasCamera())
        context->PSGetConstantBuffers(inputs.camera, 1, &camera);

    if (const BoundTexture described = Describe(depth.Get()); described.view != nullptr)
    {
        facts.depthFormat = described.format;
        facts.depthWidth = described.width;
        facts.depthHeight = described.height;
        facts.depthSamples = described.samples;
        facts.hasDepth = described.twoD && described.width > 0;
    }
    if (const BoundTexture described = Describe(normal.Get()); described.view != nullptr)
    {
        facts.normalWidth = described.width;
        facts.normalHeight = described.height;
        facts.normalSamples = described.samples;
        facts.hasNormal = described.twoD && described.width > 0;
    }
    if (camera)
    {
        D3D11_BUFFER_DESC desc {};
        camera->GetDesc(&desc);
        facts.cameraBytes = desc.ByteWidth;
        facts.hasCamera = desc.ByteWidth > 0;
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    // The engine renders the picture at a half-pixel viewport origin. The region works on target texels, whose grid has no half pixel, so the
    // window is the texels the picture covers: the origin is floored rather than carried as 0.5, which the region rule then rounds up.
    facts.window[0] = std::floor(viewport.TopLeftX);
    facts.window[1] = std::floor(viewport.TopLeftY);
    facts.window[2] = viewport.Width;
    facts.window[3] = viewport.Height;

    const Decline reason = Classify(facts);
    if (reason != Decline::None)
    {
        // Only a refusal that says this build cannot do the work walks the module towards its switch-off; a scene whose first reader binds
        // nothing usable (a forward pass, a preview, a second view) is skipped with its own line and leaves the streak where it stands.
        Refuse(reason, FormatRefusedReader(reason, facts, inputs, targetIndex), CountsTowardsSwitchOff(reason));
        return nullptr;
    }

    PassInputs passInputs;
    passInputs.target = target.view;
    passInputs.targetTexture = target.texture;
    passInputs.depth = depth.Get();
    passInputs.normal = normal.Get();
    passInputs.camera = camera.Get();
    passInputs.facts = facts;
    passInputs.params = state.params;
    // The gate's measurement rides on one run in kGateStatsEvery (the first one included); a target without an engine gloss has no gate to
    // measure, and the log says so once instead.
    passInputs.gateStats = GateReadable(state.params.target) && (state.runs % kGateStatsEvery) == 0;
    if (!GateReadable(state.params.target) && state.loggedGateUnreadable != state.params.target)
    {
        state.loggedGateUnreadable = state.params.target;
        LOG_INFO("{}", FormatGateUnreadable(state.params.target));
    }

    ID3D11ShaderResourceView* ours = nullptr;
    {
        // The character preview's marker is raised around the work too, so its copy and clear hooks do not count this module's draw as the game's.
        OwnWork own;
        ext::ffxiv::chara::ReplayScope replay;
        ours = state.pass.Run(context, passInputs);
    }
    if (ours == nullptr)
    {
        if (state.pass.CameraRejected())
        {
            // A camera table that is not the layout the shaders read: refused, looked at again when the retry time comes, and not counted towards
            // the switch-off (a loading screen's camera would otherwise stand the module down for the session).
            if (!state.loggedCamera)
            {
                state.loggedCamera = true;
                LOG_WARN("Puddles: the camera table is not the layout the shaders read, so the puddles are off until it is");
            }
            Refuse(Decline::CameraLayout, {}, false);
            return nullptr;
        }
        if (state.pass.CameraNotReady())
        {
            // The table's copy is still in the GPU's queue: the check reads it on a later frame instead of waiting here, so a session pays two
            // frames of the frame's first pass and never a pipeline drain inside a draw. Counted apart from a refusal, and not towards the
            // switch-off.
            Refuse(Decline::CameraNotReady, {}, false);
            return nullptr;
        }
        state.failure = state.pass.Failure();
        if (state.failure != nullptr && !state.loggedFailure)
        {
            state.loggedFailure = true;
            LOG_WARN("Puddles: the pass did not run ({})", state.failure);
        }
        Refuse(Decline::Failure);
        return nullptr;
    }

    state.frame.Record(target.resource, target.view, state.pass.OutputTexture());
    // This shape is the next frame's hold: the pass's own answer for a later frame whose attempt is refused, on the
    // same object the frame cache compares (GlossPuddlesRules.h's AnswersSameBinding).
    state.pass.NoteHold(target.resource, target.view);
    // The capture frame's pictures of this stage: the G-buffer target the readers sample and the copy the pass rewrote it into, copied after the
    // pass returned (its guards are gone).
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Gloss, context, target.view, ours,
                               ext::ffxiv::diag::StageWindow { facts.window[0], facts.window[1], facts.window[2], facts.window[3] });
    state.runs++;
    state.lastFacts = facts;
    state.haveRun = true;
    state.session.NoteRun();
    if (!state.loggedRun)
    {
        state.loggedRun = true;
        // The amount the shader gated on, with the two values it was chosen from: the engine's own (read back a frame behind, for this line) and
        // the wetness knob's. Same expression as the one the buffer is written from (NoteWetnessFrame), so the line cannot drift from the shape.
        const float knob = WetnessKnob();
        const float engineWetness = state.pass.EngineWetness();
        LOG_INFO("{}", FormatRun(state.params, facts.targetWidth, facts.targetHeight, WetnessFor(engineWetness, knob), engineWetness,
                                 state.pass.EngineWetnessKnown(), knob));
    }
    // What the gate removed, as the last measurement that landed: one line per change of state (under cover, open, mixed), never one per frame. A
    // measurement with almost no pool in view says nothing about cover and leaves the state where it stands.
    if (state.pass.GateStatsKnown() && state.pass.GateStatsLast().Measured())
    {
        const GateStats measured = state.pass.GateStatsLast();
        if (state.gateLog.Take(uint32_t(GateLevelOf(measured.RemovedShare())) + 1u))
            LOG_INFO("{}", FormatGateShare(measured));
    }
    return ours;
}
} // namespace

void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    if (!MentionsTarget(code, size))
        return; // most of the game's shaders, on a byte scan alone: only a reader of a G-buffer target is read and stamped
    try
    {
        detail::StampShader(shader, PackInputs(InputsOfShader(code, size)));
    }
    catch (...)
    {
    }
}

BoundReader SwapReader(ID3D11DeviceContext* context, engine::Anchors anchors)
{
    BoundReader none;
    // The frame's wetness amount is both stages' input: the tone-map stage reads the same 32-byte buffer, so a frame whose gloss target or channel
    // key is one this pass refuses still hands the weather over here - otherwise the stage would run with an all-zero buffer, shape nothing, and the
    // card and engine.txt would say it was armed. The reader path below is the gloss stage's own and stays gated on its keys.
    const bool weather = engine::IsWetnessAnchors(anchors);
    const bool reader = IsGlossAnchors(anchors);
    if (context == nullptr || (!reader && !weather))
        return none;
    State& state = S();
    if (!Armed())
        return none; // neither stage wants a draw, or the module stood itself down
    if (state.inPass.load(std::memory_order_relaxed))
        return none; // this module's own work: never classified, never swapped
    // Another module's replay of a game draw (the character preview, a diagnostic replay) is not a draw of the frame: it keeps the game's own
    // texels and never spends the frame's one attempt, so a preview opened beside the puddles shows the engine's picture and the frame's own
    // reader still runs the pass. The gate below asks the same question again, so the decision is also written where a test can pin it.
    if (ext::ffxiv::chara::t_replay)
        return none;
    // The gloss stage's own reader path: only while its target and channel keys give it a texture to shape. A refused pair still gets the weather
    // above, and never the swap.
    const bool gloss = state.armed.load(std::memory_order_relaxed) && reader;

    // Everything below talks to the device and reads the game's own objects. A throw is then a counted refusal, not an exception through the
    // game's draw hook: the draw runs unchanged with the engine's texture, which is what failing open means here.
    try
    {
        // The frame token is the output-frame counter both present paths tick (ext/present/PresentSeams.h), one relaxed load; the engine's target
        // can be a different object from one frame to the next, so a new frame forgets everything the last one recorded.
        const uint64_t token = ext::present::FrameCount();
        if (gloss)
        {
            if (state.frame.hasFrame && state.frame.frame != token)
                state.servedLastFrame = state.frame.served; // the frame that just ended, before Begin() clears it
            state.frame.Begin(token);
            // One line a session: how many reader draws a frame served. A session compares it with the number of bindings of the target it sees,
            // because a reader whose shader's reflection was stripped is never served and the two numbers then differ.
            if (!state.loggedServed && state.servedLastFrame > 0)
            {
                state.loggedServed = true;
                LOG_INFO("{}", FormatServed(state.servedLastFrame));
            }
        }

        // The engine's wetness amount for this frame: one 32-byte copy, from whichever draw that binds it comes first in the frame. The material
        // draws come before the light draws, so both stages read this frame's value.
        if (weather)
            NoteWetnessFrame(context, state, token, engine::WetnessSlotOf(anchors));
        if (!gloss)
            return none;
        // The dwell is the pass's own latch (NoteWetnessFrame answers it on the frame's copy, and both stages read it):
        // once a reading lands above the floor the stages stay armed until an exact 0.0, so a stale-low read-back at the
        // ramp's crossing cannot flip them off for one frame and back on the next.
        if (!state.pass.WetLatched())
            return none; // dry: the engine's own target is the picture, so nothing is copied and no reader is swapped

        // The slots this draw's shader declares, from its own stamp: it does not matter how many shaders the game created after it, and a shader
        // this module never read is passed on rather than served with guessed slots.
        Inputs inputs;
        {
            ComPtr<ID3D11PixelShader> pixelShader;
            context->PSGetShader(&pixelShader, nullptr, nullptr);
            inputs = UnpackInputs(detail::ReadStamp(pixelShader.Get()));
        }
        const uint32_t targetIndex = TargetIndex(state.params);
        // The gate of this draw (GlossPuddlesRules.h's ServeAnswer): a draw whose shader declares the key's target is a reader, and it is served
        // from the frame's own texture as soon as that exists - the composite and the forward draws of a frame declare the material pack and
        // nothing else. The depth, the normal and the camera table are what the PASS needs, so only the frame's one attempt asks for them. The
        // replay marker is part of the answer: a replay is passed on before either branch, so it is never served and never spends the attempt.
        const Serve answer = ServeAnswer(inputs, targetIndex, state.frame.valid, state.frame.MayRun(), ext::ffxiv::chara::t_replay);
        if (answer == Serve::PassOn)
            return none;
        const uint32_t slot = inputs.targets[targetIndex];

        ViewRef bound;
        context->PSGetShaderResources(slot, 1, &bound.view);
        const BoundTexture target = Describe(bound.view);
        if (target.view == nullptr)
            return none; // nothing bound under that name: another draw of the frame, not a failure
        // A texture of another shape under the target's name is not the engine's G-buffer target (another set of targets in the same frame, an
        // effect's own copy): passed on, not counted.
        if (!target.twoD || target.format != kTargetFormat || target.width < kMinTargetWidth || target.height < kMinTargetHeight)
            return none;

        if (answer == Serve::Attempt)
        {
            state.frame.TakeAttempt();
            if (RunFrame(context, state, target, inputs) == nullptr)
            {
                // The frame's attempt is spent and its pass never ran. Left to the engine's target, this frame's
                // composite would read an UNSHAPED gloss channel while the tone-map stage - which gates on the
                // wetness, not on the channel - darkened and mirrored the pool's positions anyway: the darkened patch
                // with nothing on it, the pool's black frame at the ramp's edges (the live log's 'the camera table's
                // copy has not landed yet' refusals, one per pass re-arming). The hold answers instead: the pass's last
                // good shape, on the object it was computed for, keeps the frame's answer on the ramp (GlossPuddlesRules.h's
                // HoldAnswersShape and AnswersSameBinding); the frame cache is then recorded from it, so the same hold
                // serves the rest of the frame's readers.
                ComPtr<ID3D11Device> holdDevice;
                context->GetDevice(&holdDevice); // the hold must never answer with a dead device's texture
                if (!(HoldAnswersShape(state.pass.WetLatched(), state.pass.OutputReady(target.width, target.height),
                                      !state.pass.CameraRejected()) &&
                      AnswersSameBinding(state.pass.HoldEngineResource(), state.pass.HoldEngineView(), target.resource, target.view) &&
                      state.pass.OnDevice(holdDevice.Get())))
                {
                    state.pass.NoteShapeFrame(token, false);
                    return none;
                }
                state.frame.Record(target.resource, target.view, state.pass.OutputView());
                state.pass.NoteHold(target.resource, target.view);
            }
            // The composite of this frame reads the shape (its own, or the hold's - the gloss channel is the shaped
            // one either way), which is exactly the frame the tone-map stage runs its answer on.
            state.pass.NoteShapeFrame(token, state.frame.valid);
        }

        // The cheap test first: the engine binds one view object for the whole frame, so a different view is a different binding. A second view of
        // the same resource (a game that makes one per pass) is served too, which is why the resource is compared as well.
        const bool sameBinding = target.view == state.frame.engineView || state.frame.Has(target.resource);
        if (!sameBinding)
            return none;

        ID3D11ShaderResourceView* ours = state.pass.OutputView();
        if (ours == nullptr)
            return none;
        // The swap asks the slot again and hands back the game's view with the reference the guard needs to restore it; the description's own
        // reference goes back here, so exactly one is held for the length of the draw.
        ID3D11ShaderResourceView* replaced = detail::SwapSlot(context, slot, ours);
        if (replaced == nullptr)
            return none;
        bound.Reset();
        state.frame.Serve();
        state.readers++;
        return BoundReader(context, slot, replaced);
    }
    catch (...)
    {
        Refuse(Decline::Exception);
        return none;
    }
}

bool Armed()
{
    State& state = S();
    // Either stage is enough to arm the shared draw seam: the tone-map stage has its own identity (the darkening at zero with the reflection
    // off), so a session whose gloss target key is one the pass must not shape still gets the pool's colour at the tone map.
    return (state.armed.load(std::memory_order_relaxed) || ToneWants(state.params)) && state.session.Armed();
}

Wetness CurrentWetness()
{
    State& state = S();
    const float knob = WetnessKnob();
    Wetness wetness;
    wetness.known = knob >= 0.0f || state.pass.EngineWetnessKnown();
    wetness.amount = WetnessFor(state.pass.EngineWetness(), knob);
    wetness.engine = state.pass.EngineWetness();
    wetness.engineKnown = state.pass.EngineWetnessKnown();
    return wetness;
}

ID3D11ShaderResourceView* WetnessResource() { return S().pass.WetnessView(); }

// The wetness gate's one latch and the frame the gloss shape is on, the same pass the wetness buffer lives in: the
// pairing that makes the tone-map stage's answer follow the frame whose composite the gloss stage shaped (or the hold
// served), instead of running against the engine's own unshaped picture.
bool WetLatched() { return S().pass.WetLatched(); }
bool ShapeOnFrame(uint64_t token) { return S().pass.ShapePresent(token); }

// Whether the gloss pass's camera layout check REJECTED the scene (a different scene than the one the hold was
// computed for): the tone stage's hold answers a transient copy window, never a rejected scene - the pools of the old
// scene would freeze over the new one for the whole rejection. The pure gate is HoldAnswersShape's own third term.
bool GlossCameraRejected() { return S().pass.CameraRejected(); }

Params CurrentParams() { return S().params; }

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.key = state.keyOn.load(std::memory_order_relaxed);
    status.identity = IsIdentity(state.params);
    status.armed = status.key && !status.identity && state.session.Armed();
    status.runs = state.runs;
    status.readers = state.readers;
    status.declined = state.declined;
    status.servedLastFrame = state.servedLastFrame;
    status.wetnessKnown = state.wetnessKnown;
    return status;
}

Diagnostics CurrentDiagnostics()
{
    State& state = S();
    const Status status = CurrentStatus();
    Diagnostics diagnostics;
    diagnostics.key = status.key;
    diagnostics.identity = status.identity;
    diagnostics.armed = status.armed;
    diagnostics.runs = status.runs;
    diagnostics.readers = status.readers;
    diagnostics.declined = status.declined;
    diagnostics.servedLastFrame = status.servedLastFrame;
    for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        diagnostics.declines[i] = state.declines[i];
    diagnostics.haveRun = state.haveRun;
    diagnostics.targetIndex = TargetIndex(state.params);
    diagnostics.channel = uint32_t(state.params.channel);
    if (diagnostics.haveRun)
    {
        diagnostics.targetWidth = state.lastFacts.targetWidth;
        diagnostics.targetHeight = state.lastFacts.targetHeight;
        for (size_t i = 0; i < 4; ++i)
            diagnostics.window[i] = state.lastFacts.window[i];
    }
    diagnostics.wetnessKnown = state.wetnessKnown;
    return diagnostics;
}

FrameObservation CurrentFrameObservation()
{
    State& state = S();
    const Wetness wet = CurrentWetness();
    FrameObservation out;
    out.hasToken = state.frame.hasFrame;
    out.token = state.frame.frame;
    out.tried = state.frame.tried;
    out.valid = state.frame.valid;
    out.served = state.frame.served;
    out.runs = state.runs;
    out.declined = state.declined;
    out.wetnessToken = state.hasWetnessFrame ? state.wetnessFrame : 0;
    out.wetnessKnown = wet.known;
    out.wetness = wet.amount;
    out.wetLatched = state.pass.WetLatched();
    out.shapeToken = state.frame.hasFrame ? state.frame.frame : 0;
    out.shapePresent = state.pass.ShapePresent(out.shapeToken);
    return out;
}

void Publish(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    state.params = BuildParams(on, ext::Cfg().ffxiv.puddlesCoverage.value_or_default(),
                               ext::Cfg().ffxiv.puddlesScale.value_or_default(), ext::Cfg().ffxiv.puddlesSoftness.value_or_default(),
                               ext::Cfg().ffxiv.puddlesGlossTarget.value_or_default(),
                               ParseChannel(ext::Cfg().ffxiv.puddlesGlossChannel.value_or_default()),
                               ext::Cfg().ffxiv.puddlesGlossWet.value_or_default(), ext::Cfg().ffxiv.puddlesReflection.value_or_default(),
                               ext::Cfg().ffxiv.puddlesReflectionStrength.value_or_default(),
                               ext::Cfg().ffxiv.puddlesDarkening.value_or_default(), ext::Cfg().ffxiv.puddlesRipple.value_or_default(),
                               ext::Cfg().ffxiv.puddlesReflectionSteps.value_or_default(),
                               ext::Cfg().ffxiv.puddlesReach.value_or_default());
    // The tone-map stage reads the same keys: one card, one publish, so the two stages cannot be tuned against different values.
    PublishTone(state.params);
    // The frame's texture belongs to the tuning it was computed with, and an off key (or an identity one) gives the device objects back.
    state.frame.Reset();
    state.armed.store(Wants(state.params), std::memory_order_relaxed);
    state.loggedRun = false;
    state.gateLog = LogGate {};
    state.loggedGateUnreadable = 0;
    state.loggedFailure = false;
    state.loggedServed = false;
    state.servedLastFrame = 0;
    // A target or a channel this pass cannot shape is today's picture and nothing more (IsIdentity): each is refused here, once per distinct
    // value, before any device call. A configuration the ini still carries is not a build this module cannot serve, so this never walks it
    // towards a switch-off - and a session that corrects the key sees the puddles without arming anything again. A refused channel is named with
    // its target, because the same channel is a different thing on the other target.
    if (on && !TargetServable(state.params.target))
    {
        if (state.loggedTarget != state.params.target)
        {
            state.loggedTarget = state.params.target;
            LOG_WARN("{}", FormatTargetRefused(state.params.target));
        }
        state.loggedChannel = 0;
    }
    else if (on && !ChannelServable(state.params.target, state.params.channel))
    {
        state.loggedTarget = 0;
        const uint32_t channelKey = state.params.target * 4u + uint32_t(state.params.channel) + 1u;
        if (state.loggedChannel != channelKey)
        {
            state.loggedChannel = channelKey;
            LOG_WARN("{}", FormatChannelRefused(state.params.target, state.params.channel));
        }
    }
    else
    {
        state.loggedTarget = 0;
        state.loggedChannel = 0;
    }
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.loggedCamera = false;
        state.hasWetnessFrame = false;
        state.wetnessKnown = false;
        state.wetnessForced = false;
        // the dwell is the card's own arming: a publish-off is a fresh dry as far as it is concerned; the pass's own
        // reset below clears the shared latch and the hold with it, so the two stages arm and disarm together.
        state.pass.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // turning the switch back on after a switch-off is the player's re-arm
    }
    // The draw path's own arming is the shared module's: it asks this module whether it wants a draw, so the shared switch and this one arm the
    // same seam whichever of them moved.
    engine::Publish(ext::Cfg().ffxiv.enginePassOverrides.value_or_default());
}

void Release()
{
    State& state = S();
    ReleaseTone();
    state.frame.Reset();
    state.servedLastFrame = 0;
    state.hasWetnessFrame = false;
    state.wetnessKnown = false;
    state.wetnessForced = false;
    state.pass.Reset();
}
} // namespace ext::ffxiv::engine::gloss

namespace ext::ffxiv::engine::gloss
{
// The stage dump's two questions about this stage (its gloss card, not the tone stage that shares the key), asked at the capture frame.
bool GlossStageArmed()
{
    State& state = S();
    return state.armed.load(std::memory_order_relaxed) && state.session.Armed();
}

std::string GlossStageValues()
{
    const Params& params = S().params;
    return std::format("coverage={:.2f} scale={:.2f} softness={:.3f} target={} wet={:.2f}", double(params.coverage), double(params.scale),
                       double(params.softness), params.target, double(params.wet));
}
} // namespace ext::ffxiv::engine::gloss

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run.
struct RegisterPuddles
{
    RegisterPuddles()
    {
        ext::FfxivConfig::puddlesOnLoaded = &ext::ffxiv::engine::gloss::Publish;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Gloss, &ext::ffxiv::engine::gloss::GlossStageArmed,
                                        &ext::ffxiv::engine::gloss::GlossStageValues);
    }
} g_registerPuddles;
} // namespace
