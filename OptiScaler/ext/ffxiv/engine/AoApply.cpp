#include "pch.h"

#include "ext/ffxiv/engine/AoApply.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"   // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h"   // the DXBC RDEF reader the engine's own index uses
#include "ext/ffxiv/engine/AoPass.h"
#include "ext/ffxiv/engine/EnginePass.h"     // the module's own feature bit and publish
#include "ext/present/PresentSeams.h"        // the output-frame counter, the frame token

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>

// Ambient occlusion (ext/ffxiv/engine): the seam's entries and the draw path (AoApply.h). The pure decisions are in AoRules.h, the GPU work in
// AoPass.cpp.
//
// When a shader is stamped: while the game creates its pixel shaders, whatever the key says, because the game creates the shaders of its light
// passes and its materials while it loads and a switch turned on later has to find them. A cheap byte scan for the occlusion name drops every
// other shader before the reflection parse; nothing is logged while the key is off. The stamp is written on the shader object, so there is no
// table to fill and nothing to overflow.
//
// Cost with the key off: one relaxed load per indexed draw (the caller's) and nothing else - nothing here is reached for a draw whose shader the
// index did not claim as a reader. Cost with the key on: one relaxed load per reader draw, one PSGetShaderResources for its slot, one
// GetResource, and the slot swap and its restore; the frame's first reader that can serve the frame also runs the pass once.
namespace ext::ffxiv::engine::ao
{
namespace
{
using Microsoft::WRL::ComPtr;

// The slots a reader's shader declares are stamped on the shader object itself (AoSwap.cpp's detail::StampShader, the only place this module
// remembers anything). The game creates tens of thousands of pixel shaders a session, and any fixed table of them loses the ones a frame needs (a
// 1024-slot table took every material shader - they all declare the camera - and the light shader's entry was gone long before the first reader
// looked it up).

// The one name that makes a shader a reader of the engine's occlusion texture: the shared table's own row, the row the draw path classifies a
// reader with. A shader that does not declare it can never be served by this pass, so the creation hook reads a shader's reflection only when it
// does: every material shader declares the camera table, and stamping those would spend a second parse and a private-data write on each of the
// tens of thousands the game creates, for a word nothing ever reads.
constexpr std::string_view kOcclusionName = engine::kNames[5].name;
static_assert(kOcclusionName == "g_SamplerOcclusion",
              "the occlusion name is the shared table's row (the row the draw path's IsOcclusionDraw reads): a row that moved would leave "
              "every reader unstamped");

// Whether the bytecode contains `name` as a byte string: the cheap test that drops the shaders that cannot be a reader before the reflection
// parse. Deliberately a plain scan, like the shared index's.
bool Mentions(const void* code, size_t size, std::string_view name)
{
    if (code == nullptr || size < name.size())
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i + name.size() <= size; ++i)
        if (bytes[i] == static_cast<unsigned char>(name[0]) && std::memcmp(bytes + i, name.data(), name.size()) == 0)
            return true;
    return false;
}

// The three slots one shader declares, from the same reflection the shared index reads. The names arrive with the effect compiler's type
// suffix (`g_SamplerDepth.T`), which diag::ResourceBaseName strips; the selection itself is the pure rule (AoRules.h), so the draw path holds
// no second copy of it.
Inputs InputsOfShader(const void* code, size_t size)
{
    if (code == nullptr || size < 32)
        return Inputs {};
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    return InputsOfBindings(parsed, count);
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

// One bound view the caller owns once, and either releases or hands to a swap guard or to the frame's own memory.
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
    // Takes another holder's view, releasing what this one held: the frame keeps exactly the view it took.
    void Take(ViewRef& other)
    {
        Reset();
        view = other.Release();
    }
    ViewRef(const ViewRef&) = delete;
    ViewRef& operator=(const ViewRef&) = delete;
    ViewRef() = default;
};

struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    // The frame's memory of the engine's occlusion texture, its view and this module's result (EnginePassRules.h's FrameCache shape).
    FrameCache frame;
    // The frame's three inputs: the first usable binding each reader of the frame supplied (AoRules.h's FrameInputs) and the views it was on,
    // held until the frame changes. A session's first reader of a frame does not declare all three, so the frame collects them from whichever
    // reader does and the pass runs at the first one that completes them.
    FrameInputs inputs;
    ViewRef depthHeld, normalHeld;
    ComPtr<ID3D11Buffer> cameraHeld;
    std::atomic<bool> keyOn { false };
    std::atomic<bool> armed { false };  // the published feature bit's own condition (the shared arming asks for it)
    std::atomic<bool> inPass { false }; // true while this module's own GPU work runs
    uint32_t runs = 0;
    uint32_t readers = 0;
    uint32_t declined = 0;
    uint32_t servedLastFrame = 0; // the reader draws of the last finished frame
    uint64_t token = 0;           // the frame token the draw path last read, for the pause's own clock
    uint32_t declines[kDeclineReasonCount] {};
    ReaderFacts lastFacts;
    bool haveRun = false;
    bool loggedOff = false;
    bool loggedRun = false;  // the one line that says what the pass computes
    bool loggedServed = false; // the one line that says how many readers a frame served
    bool loggedFailure = false; // the first failure's reason, once
    const char* failure = nullptr;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// Marks this module's own GPU work: its two draws re-enter the draw seam (they are drawn on the game's own immediate context) and the shared
// module must never classify or count them. A nested guard restores, never clears.
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

// One bound view the caller owns once, and either releases or hands to a swap guard.

// One frame left alone. Every reason leaves the draw exactly as the game made it, and each is one line of the log the first time it happens.
// `line` replaces the short line when the caller has something more to say (a frame that no reader could complete reports what it collected, so
// a session can tell the shapes that refuse alike apart). The frame counts towards the pause once, whatever its first decline was.
Decline Refuse(Decline reason, std::string_view line = {})
{
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", line.empty() ? std::string(FormatDecline(reason)) : std::string(line));
    if (NoteFrame(state.session, state.inputs.noted, state.token, false) && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("{}", FormatPaused());
    }
    return reason;
}

// One reader as the draw path found it: the slots its shader declares (read from the stamp on the shader object), the bindings it made there,
// and how each binding measured. The views are owned here until the frame takes them.
struct ReaderInputs
{
    Inputs slots;
    ViewRef depth, normal;
    ComPtr<ID3D11Buffer> camera;
    BoundInput depthBound, normalBound, cameraBound;
};

BoundInput InputOf(const BoundTexture& texture)
{
    BoundInput input;
    input.present = texture.view != nullptr;
    input.twoD = texture.twoD;
    input.width = texture.width;
    input.height = texture.height;
    input.samples = texture.samples;
    return input;
}

// What the engine bound for this draw: the shader's own stamp says where its depth, its normal and its camera table are, so it does not matter
// how many shaders the game created after it; a shader this module never read (the key was on before the game created it) declares nothing.
void ReadReaderInputs(ID3D11DeviceContext* context, ReaderInputs& reader)
{
    Inputs inputs;
    {
        ComPtr<ID3D11PixelShader> pixelShader;
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        inputs = UnpackInputs(detail::ReadStamp(pixelShader.Get()));
    }
    if (inputs.HasDepth())
    {
        context->PSGetShaderResources(inputs.depth, 1, &reader.depth.view);
        reader.depthBound = InputOf(Describe(reader.depth.view));
    }
    if (inputs.HasNormal())
    {
        context->PSGetShaderResources(inputs.normal, 1, &reader.normal.view);
        reader.normalBound = InputOf(Describe(reader.normal.view));
    }
    if (inputs.HasCamera())
    {
        context->PSGetConstantBuffers(inputs.camera, 1, &reader.camera);
        if (reader.camera)
        {
            D3D11_BUFFER_DESC desc {};
            reader.camera->GetDesc(&desc);
            reader.cameraBound.present = true;
            reader.cameraBound.bytes = desc.ByteWidth;
            // The view's identity: the frame's depths and normals are one view's only when the readers that supplied them bound the same
            // camera table object (measured: the light draws, the composite and the materials of one view all bind the same one).
            reader.cameraBound.resource = reader.camera.Get();
        }
    }
    reader.depthBound.slot = inputs.depth;
    reader.normalBound.slot = inputs.normal;
    reader.cameraBound.slot = inputs.camera;
    reader.slots = inputs;
}

// The render window the mapping and the region are computed from, in depth texels. The engine renders the picture at a half-pixel viewport
// origin; the search works on depth texels, whose grid has no half pixel, so the origin is floored rather than carried as 0.5, which would put
// our texel 0 on the boundary of depth texel 0 and one texel 1 on depth texel 2.
void ReaderFactsOf(ID3D11DeviceContext* context, const BoundTexture& occlusion, ReaderFacts& facts)
{
    facts.hasOcclusion = true;
    facts.occlusionFormat = occlusion.format;
    facts.occlusionWidth = occlusion.width;
    facts.occlusionHeight = occlusion.height;
    facts.occlusionSamples = occlusion.samples;
    facts.replay = ext::ffxiv::chara::t_replay;

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window[0] = std::floor(viewport.TopLeftX);
    facts.window[1] = std::floor(viewport.TopLeftY);
    facts.window[2] = viewport.Width;
    facts.window[3] = viewport.Height;
}

// One reader of a frame that is still missing an input: the frame waits for another reader, so nothing of the draw is touched and the frame's
// one attempt is not spent. One rate-limited line says what the frame has and what it is waiting for. The per-reason table counts the wait with
// the total, so a capture's breakdown adds up to the number of draws it says were left alone; the pause's own count is the frame's, and the
// caller lets NoteFrame decide whether this frame is still uncounted.
void WaitForInputs(Decline reason, const ReaderFacts& facts, const Inputs& inputs)
{
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", FormatWaiting(reason, facts, inputs));
}

// The frame's run, once its inputs are in hand: fills the pass's inputs from the frame's held views and runs it. Returns this module's view for
// the frame, or null after a failure (which is counted here).
ID3D11ShaderResourceView* RunFrame(ID3D11DeviceContext* context, State& state, const BoundTexture& occlusion, const ReaderFacts& facts)
{
    PassInputs passInputs;
    passInputs.occlusion = occlusion.view;
    passInputs.occlusionTexture = occlusion.texture;
    passInputs.depth = state.depthHeld.view;
    passInputs.normal = state.normalHeld.view;
    passInputs.camera = state.cameraHeld.Get();
    passInputs.facts = facts;
    passInputs.params = state.params;

    ID3D11ShaderResourceView* ours = nullptr;
    {
        // The character preview's marker is raised around the work too, so its copy and clear hooks do not count this module's draws as the
        // game's.
        OwnWork own;
        ext::ffxiv::chara::ReplayScope replay;
        ours = state.pass.Run(context, passInputs);
    }
    if (ours == nullptr)
    {
        state.failure = state.pass.Failure();
        if (state.failure != nullptr && !state.loggedFailure)
        {
            state.loggedFailure = true;
            LOG_WARN("Ambient occlusion: the pass did not run ({})", state.failure);
        }
        Refuse(Decline::Failure);
        return nullptr;
    }

    state.frame.Record(occlusion.resource, occlusion.view, state.pass.OutputTexture());
    state.runs++;
    state.lastFacts = facts;
    state.haveRun = true;
    state.session.NoteRun();
    if (!state.loggedRun)
    {
        state.loggedRun = true;
        LOG_INFO("{}", FormatRun(state.params, facts, state.inputs.Slots(), state.pass.HasGpuMilliseconds(), state.pass.GpuMilliseconds()));
    }
    return ours;
}

// What one reader of a frame with no pass yet came to.
enum class FrameStep : uint8_t
{
    Waited,  // the frame is still missing an input this reader did not supply: the engine's texture stays and nothing is spent
    Refused, // the frame cannot be served: the frame's one attempt is taken and one line says why
    Ran,     // the pass ran for this frame, and this reader's draw is served
};

// One reader of a frame that has no pass yet. The frame collects the first usable binding of the pass's three inputs from whichever reader of
// the frame supplies it, and the pass runs at the first reader that completes them - so the frame's first reader's shape no longer decides
// whether the frame is served. A reader that supplies nothing usable does not spend the frame's attempt; the bound in AoRules.h keeps a frame
// no reader can complete from inspecting every draw of it.
FrameStep CollectFrame(ID3D11DeviceContext* context, State& state, const BoundTexture& occlusion)
{
    ReaderInputs reader;
    ReadReaderInputs(context, reader);

    // The frame keeps the first usable binding of each input together with the camera table that identifies its view, and the views it takes
    // are held until the frame changes. A reader whose camera table is another view's refuses the frame: its depth has the same allocation's
    // size, so only the view's identity tells the two apart, and a search run from two views is a wrong picture no fail-open catches.
    const FrameInputs::Contribution taken = state.inputs.Merge(reader.cameraBound, reader.depthBound, reader.normalBound);
    if (taken.depth)
        state.depthHeld.Take(reader.depth);
    if (taken.normal)
        state.normalHeld.Take(reader.normal);
    if (taken.camera)
        state.cameraHeld = reader.camera;

    ReaderFacts readerFacts;
    ReaderFactsOf(context, occlusion, readerFacts);
    const ReaderFacts facts = FactsOf(readerFacts, state.inputs);
    const Inputs slots = state.inputs.Slots();

    ++state.inputs.readers;
    if (taken.otherView)
    {
        // Two views in one frame's readers: that is a fact about the frame, not a shortage a later reader supplies.
        state.frame.TakeAttempt();
        Refuse(Decline::ViewMismatch, FormatRefusedReader(Decline::ViewMismatch, facts, slots));
        return FrameStep::Refused;
    }
    if (!state.inputs.Complete())
    {
        const Decline reason = Classify(facts);
        if (IsWaitingDecline(reason) && state.inputs.readers <= kMaxFrameInputReaders)
        {
            if (!state.inputs.noted)
            {
                WaitForInputs(reason, facts, slots);
                // A wait is not a refusal, and one frame counts once: this frame's first decline counts it, and a frame-level refusal of the
                // same frame later counts nothing. A wait on the frame a paused module is looking at counts nothing towards the pause (the look
                // is doing its job), but its line and its place in the per-reason table are one per frame like every other frame's.
                if (NoteFrame(state.session, state.inputs.noted, state.token, true) && !state.loggedOff)
                {
                    state.loggedOff = true;
                    LOG_WARN("{}", FormatPaused());
                }
                state.inputs.noted = true;
            }
            return FrameStep::Waited;
        }
        // A frame no reader supplied in time, or a shape no later reader can change (a size, the window): the frame's one attempt.
        state.frame.TakeAttempt();
        Refuse(reason, FormatRefusedReader(reason, facts, slots));
        return FrameStep::Refused;
    }

    state.frame.TakeAttempt();
    if (RunFrame(context, state, occlusion, facts) == nullptr)
        return FrameStep::Refused;
    return FrameStep::Ran;
}
} // namespace

void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    if (!Mentions(code, size, kOcclusionName))
        return; // most of the game's shaders, on a byte scan alone: only a reader of the occlusion texture is read and stamped
    try
    {
        detail::StampShader(shader, PackInputs(InputsOfShader(code, size)));
    }
    catch (...)
    {
    }
}

BoundReader SwapReader(ID3D11DeviceContext* context, Anchors anchors)
{
    BoundReader none;
    if (context == nullptr || !IsOcclusionAnchors(anchors))
        return none;
    State& state = S();
    if (!state.armed.load(std::memory_order_relaxed))
        return none;
    if (state.inPass.load(std::memory_order_relaxed))
        return none; // this module's own work: never classified, never swapped
    const uint32_t slot = OcclusionSlotOf(anchors);
    if (slot >= kTextureSlots)
        return none;

    // Everything below talks to the device and reads the game's own objects. A throw is then a counted refusal, not an exception through the
    // game's draw hook: the draw runs unchanged with the engine's texture, which is what failing open means here.
    try
    {
        // The frame token is the output-frame counter both present paths tick (ext/present/PresentSeams.h), one relaxed load; the engine
        // alternates its occlusion texture from one frame to the next, so a new frame forgets everything the last one recorded. It is also
        // the pause's clock (ao::Session): a paused module looks at one frame every kPauseFrames, so the draw path of every other frame is
        // the key-off path plus this one load.
        const uint64_t token = ext::present::FrameCount();
        state.token = token;
        const LookStep look = state.session.Look(token);
        if (look == LookStep::Skip)
            return none;
        if (state.frame.hasFrame && state.frame.frame != token)
            state.servedLastFrame = state.frame.served; // the frame that just ended, before Begin() or the look's reset clears it
        if (look == LookStep::ProbeBegin)
        {
            // A look starts its frame from scratch. The frame it is about to inspect has already spent its one attempt in the cases that
            // matter - a pause that a frame-level refusal reached, and a present counter that stands still, which never begins a new frame -
            // and a look that reached a spent attempt would inspect nothing at all. What the frame collected is let go with it, by the Begin
            // below.
            state.frame.Reset();
            state.inputs.Reset();
        }
        state.frame.Begin(token);
        if (state.inputs.Begin(token))
        {
            // A frame's inputs are its own buffers: the views the last frame supplied are let go here, not carried into this one.
            state.depthHeld.Reset();
            state.normalHeld.Reset();
            state.cameraHeld.Reset();
        }
        // One line a session: how many reader draws a frame served. A session compares it with the number of occlusion bindings it sees, because a
        // reader whose shader's reflection was stripped is never served and the two numbers then differ.
        if (!state.loggedServed && state.servedLastFrame > 0)
        {
            state.loggedServed = true;
            LOG_INFO("{}", FormatServed(state.servedLastFrame));
        }

        ViewRef bound;
        context->PSGetShaderResources(slot, 1, &bound.view);
        const BoundTexture occlusion = Describe(bound.view);
        if (occlusion.view == nullptr)
            return none; // nothing bound under that name: another draw of the frame, not a refusal
        if (!occlusion.twoD || !IsReaderTarget(occlusion.format, occlusion.width, occlusion.height))
            return none; // the engine's own 4x4 placeholder: another draw of the frame, not a refusal

        if (!state.frame.valid)
        {
            if (!state.frame.MayRun())
                return none; // this frame's pass was tried and could not run
            // The frame collects the pass's inputs from whichever reader supplies them, and the pass runs at the first reader that completes
            // them. A reader that supplies nothing usable leaves the frame waiting: the engine's texture stays and the frame's attempt is not
            // spent, so its first draw no longer decides whether the frame is served.
            if (CollectFrame(context, state, occlusion) != FrameStep::Ran)
                return none;
        }

        // The cheap test first: the engine binds one view object for the whole frame, so a different view is a different binding. A second view
        // of the same resource (a game that makes one per pass) is served too, which is why the resource is compared as well.
        const bool sameBinding = occlusion.view == state.frame.engineView || state.frame.Has(occlusion.resource);
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
    // The pause is deliberately not part of this answer: a paused module keeps the draw path armed, because a reader draw that returns at one
    // load is how its next look is reached. `Status.armed` says whether the pass can run now.
    return state.armed.load(std::memory_order_relaxed);
}

Params CurrentParams() { return S().params; }

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.key = state.keyOn.load(std::memory_order_relaxed);
    status.identity = IsIdentity(state.params);
    status.paused = state.session.Paused();
    status.armed = status.key && !status.identity && !status.paused;
    status.runs = state.runs;
    status.readers = state.readers;
    status.declined = state.declined;
    status.servedLastFrame = state.servedLastFrame;
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
    diagnostics.paused = status.paused;
    diagnostics.runs = status.runs;
    diagnostics.readers = status.readers;
    diagnostics.declined = status.declined;
    diagnostics.servedLastFrame = status.servedLastFrame;
    for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        diagnostics.declines[i] = state.declines[i];
    diagnostics.haveRun = state.haveRun;
    if (diagnostics.haveRun)
    {
        diagnostics.sourceWidth = state.lastFacts.depthWidth;
        diagnostics.sourceHeight = state.lastFacts.depthHeight;
        diagnostics.targetWidth = state.lastFacts.occlusionWidth;
        diagnostics.targetHeight = state.lastFacts.occlusionHeight;
        for (size_t i = 0; i < 4; ++i)
            diagnostics.window[i] = state.lastFacts.window[i];
    }
    diagnostics.haveGpuMilliseconds = state.pass.HasGpuMilliseconds();
    diagnostics.gpuMilliseconds = state.pass.GpuMilliseconds();
    return diagnostics;
}

void Publish(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    Params params;
    params.on = on;
    params.radius = ClampRadius(ext::Cfg().ffxiv.ambientOcclusionRadius.value_or_default());
    params.strength = ClampStrength(ext::Cfg().ffxiv.ambientOcclusionStrength.value_or_default());
    params.blend = ClampBlend(ext::Cfg().ffxiv.ambientOcclusionBlend.value_or_default());
    params.quality = ParseQuality(ext::Cfg().ffxiv.ambientOcclusionQuality.value_or_default());
    state.params = params;
    // The frame's texture belongs to the tuning it was computed with, and an off key (or an identity one) gives the device objects back.
    state.frame.Reset();
    state.inputs.Reset();
    state.depthHeld.Reset();
    state.normalHeld.Reset();
    state.cameraHeld.Reset();
    state.armed.store(Wants(params), std::memory_order_relaxed);
    state.loggedRun = false;
    state.loggedFailure = false;
    state.loggedServed = false;
    state.servedLastFrame = 0;
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.pass.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // a change on the card after the pause is the player's own re-arm, before the pause's next look
    }
    // The draw path's own arming is the shared module's: it asks this module whether it wants a draw, so the shared switch and this one arm
    // the same seam whichever of them moved.
    engine::Publish(ext::Cfg().ffxiv.enginePassOverrides.value_or_default());
}

void Release()
{
    State& state = S();
    state.frame.Reset();
    state.inputs.Reset();
    state.depthHeld.Reset();
    state.normalHeld.Reset();
    state.cameraHeld.Reset();
    state.servedLastFrame = 0;
    state.pass.Reset();
}
} // namespace ext::ffxiv::engine::ao

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run.
struct RegisterAmbientOcclusion
{
    RegisterAmbientOcclusion() { ext::FfxivConfig::ambientOcclusionOnLoaded = &ext::ffxiv::engine::ao::Publish; }
} g_registerAmbientOcclusion;
} // namespace
