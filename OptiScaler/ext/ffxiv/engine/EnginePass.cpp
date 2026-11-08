#include "pch.h"

#include "ext/ffxiv/engine/EnginePass.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/engine/AoApply.h" // the ambient occlusion pass, on the same seam as the knobs, the puddles, the shafts and the reflections
#include "ext/ffxiv/engine/CbPatch.h"
#include "ext/ffxiv/engine/Atmo.h" // the atmosphere stage, on the same seam (between the light bounce and depth of field)
#include "ext/ffxiv/engine/Dof.h" // the depth-of-field stage, on the same seam (the chain's last member)
#include "ext/ffxiv/engine/Flare.h"           // the lens flare's card, on the same seam (one call per draw)
#include "ext/ffxiv/engine/GlossPuddlesApply.h" // the puddles pass, on the same seam
#include "ext/ffxiv/engine/Gi.h"              // the screen-space light bounce, on the same seam (the chain's stage after the march)
#include "ext/ffxiv/engine/Godrays.h"          // the volumetric god rays march, on the same seam (the chain's last member)
#include "ext/ffxiv/enginelight/EngineLight.h" // the engine light controls (ENG2), on the same seam: the lamp arm, and the exposure hold in the night grade's old chain slot (between the light bounce and depth of field)
#include "ext/ffxiv/engine/PuddleTone.h"       // the puddles' tone-map stage, on the same seam
#include "ext/ffxiv/engine/PassIndex.h"
#include "ext/ffxiv/engine/SsrApply.h"     // the screen-space reflections pass, on the same seam
#include "ext/ffxiv/engine/SsrAb.h"        // the capture frame's frame-exact A/B of that pass (a diagnostic of the one-frame dump)
#include "ext/ffxiv/engine/WaterExecutionTrace.h"
#include "ext/ffxiv/enginelight/ActiveSwap.h" // the readers inside a swapped light draw see the game's buffer
#include "ext/ffxiv/enginelight/CbFamily.h"   // the engine light controls' constant-buffer family: its ask, its plans, its composite entries
#include "ext/ffxiv/engine/Shafts.h"       // the light shafts' draw path, on the same seam (one call per draw)
#include "ext/ffxiv/shadows/ShadowsHooks.h" // shadow mapping: its two ambient entries, appended into this module's ONE CbPatch dispatch (null-checked hooks)
#include "ext/ffxiv/shadows/ShadowsRules.h" // the shadows run line's formatter (pure rules, no device)
#include "ext/present/PresentSeams.h"       // the output-frame counter, the wetness patch's frame token

#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>
#include <unordered_map>

// Engine pass overrides (ext/ffxiv/engine): the index, the seam's entries and the draw path (EnginePass.h). The pure decisions are in
// EnginePassRules.h, the shader parse in PassIndex.cpp, the primitive in CbPatch.cpp.
//
// When the index is filled: while the game creates its shaders, whatever the key says, because the game creates its composite shader while it
// loads and a switch turned on later has to find it. A cheap byte scan for the names drops every shader that cannot be one of these draws
// before the reflection parse, and a layout with no anchor bit for any knob is not stored either, so the table holds the draws the knobs can
// patch and nothing else; nothing is logged while the key is off, except the one line a full table writes, so a default-off install writes no
// line about this module.
//
// Cost with the key off: the seam's two relaxed loads (one per draw, one per pixel-shader bind) and nothing else - the feature bit is set only
// while the master switch is on and the knob is not at 1.0, so a master left on with the slider at vanilla costs the same as off.
//
// Cost with the key on: per draw one relaxed load, and for a draw whose shader the seam classified as the composite - about two a frame - the
// state queries of the validation and three copies and one dispatch of the scale shader. A material draw whose shader declares the wetness buffer
// - about 1,500 a frame - pays one slot read and the slot swap, and its patch runs once per frame: the engine binds one wetness buffer for all of
// them, and the frame's later draws bind the cached result. Every other draw of the frame is passed on at that one load: no lock, no state query,
// and no refusal counted.
namespace ext::ffxiv::engine
{
namespace
{
using Microsoft::WRL::ComPtr;
// Stamped on a shader object that is in the index: the layout's index plus one (an unstamped object reads 0 = not in the index).
constexpr GUID kLayoutTag { 0x2f6c1a51, 0x8b30, 0x4d1e, { 0x9a, 0x41, 0x63, 0xd2, 0x57, 0x0e, 0x9c, 0x38 } };

// The index's table (EnginePassRules.h): open addressing over 65,536 slots, so a shader never evicts another one and every indexed shader
// answers on its bind. The table is 1.5 MiB and allocated on first use; the module keeps its pointer and never frees it, like the rest of its
// session state. Inserts happen at shader creation only, under the index's one lock; the bind path reads it with no lock.
// The table is created by the first insert (at shader creation, under the index's lock, inside its try) and published to the bind path
// through one pointer, so a bind never allocates and a session that never indexes a shader never pays the megabyte.
std::atomic<SeenTable<kSeenSlots>*> g_seenTable { nullptr };

SeenTable<kSeenSlots>* SeenIfAny() noexcept { return g_seenTable.load(std::memory_order_acquire); }

SeenTable<kSeenSlots>& SeenForInsert()
{
    SeenTable<kSeenSlots>* table = g_seenTable.load(std::memory_order_acquire);
    if (table == nullptr)
    {
        table = new SeenTable<kSeenSlots>; // never destroyed: no destructor may run under the loader lock
        g_seenTable.store(table, std::memory_order_release);
    }
    return *table;
}

// The id the shader object itself carries, 0 when it has none. Asked only on a pointer hit, so a bind of a shader the index does not hold
// costs no D3D call.
uint32_t StampedIdOf(ID3D11PixelShader* shader)
{
    UINT bytes = sizeof(uint32_t);
    uint32_t stamped = 0;
    if (FAILED(shader->GetPrivateData(kLayoutTag, &bytes, &stamped)))
        return 0;
    return stamped;
}

bool IsStampedWaterReader(ID3D11DeviceContext* context)
{
    if (context == nullptr) return false;
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    return shader && ssr::UnpackSlots(ssr::detail::ReadStamp(shader.Get())).water;
}

Anchors TrackedAnchorsOf(ID3D11PixelShader* shader)
{
    if (shader == nullptr)
        return 0;
    // The record's id must still be the id the object carries: a recycled address answers "not mine" instead of another layout's slots.
    SeenTable<kSeenSlots>* table = SeenIfAny();
    if (table == nullptr)
        return 0; // nothing indexed yet: the common case before the first candidate shader, and no allocation on the bind path
    return table->Find(reinterpret_cast<uintptr_t>(shader), [shader](uintptr_t) { return StampedIdOf(shader); });
}

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

struct Index
{
    std::mutex mutex;
    std::unordered_map<uint32_t, uint32_t> byCrc; // crc -> the id the shader object is stamped with
    uint32_t next = 0;
};

Index& I()
{
    static Index* index = new Index;
    return *index;
}

// One session's state, so nothing of it is a static that a test cannot build.
struct State
{
    CbPatch patch;
    Session session;
    LogGate log;
    Params params;
    // The packed anchors of the shader the game bound last (EnginePassRules.h): 0 when it is not a draw this module patches. The draw path
    // reads this one 64-bit value with one relaxed load, and every arm and disarm transition clears it.
    std::atomic<Anchors> boundAnchors { 0 };
    std::atomic<bool> inPatch { false }; // true while this module's own GPU work runs
    // The wetness patch's per-frame cache: the buffer the frame binds and the patched copy its draws bind (EnginePassRules.h's FrameCache).
    FrameCache wetnessCache;
    LogGate engineLightLog; // the composite line of the engine light controls' entries (a gate of its own: the knob's and the shadows arm's lines are theirs)
    // The cache's epoch: the menu and a config load publish a new one instead of writing the cache, whose entry belongs to the draw thread alone.
    std::atomic<uint32_t> cacheEpoch { 0 };
    std::atomic<bool> keyOn { false };   // the key as the last config load or menu change published it
    uint64_t drawsSeen = 0;
    uint64_t bindsSeen = 0;
    uint32_t runs = 0;         // composite draws the ambient knob patched
    uint32_t shadowsRuns = 0;  // composite dispatches that carried the shadow mapping module's entries
    LogGate shadowsLog;        // the shadow mapping arm's own line when the shipped knob shares the dispatch (a gate of its own, so the two do not alternate)
    uint32_t wetnessRuns = 0;  // wetness patches (dispatches), not binds
    uint32_t wetnessSwaps = 0; // material draws that bound the patched wetness buffer
    uint32_t declined = 0;
    uint32_t declines[kDeclineReasonCount] {}; // one count per reason, indexed by Decline; None stays 0, the runs are counted in `runs`
    // The index's size as a capture reads it: `indexed` is the bytecodes stored so far, `indexRefused` the new bytecodes the cap refused. Both
    // are written by the creation hook, which any loader thread can run, so they are atomics and the capture reads them without the index's lock.
    std::atomic<uint32_t> indexed { 0 };
    std::atomic<uint32_t> indexRefused { 0 };
    DrawFacts lastFacts;                       // the facts of the latest patched draw, for the capture's engine.txt
    bool haveRun = false;
    CostMeter cost;
    bool loggedOff = false;    // the one line that says the module switched itself off for the session
    bool indexedLogged = false; // the one line that says which shader was indexed
    bool compositeIndexedLogged = false; // the one line that says which slots the composite was indexed at (the ambient arm patches the first)
    bool indexFullLogged = false; // the one line that says the index ran out of probe room (fail open for the shader it refused)
    bool indexRefusedLogged = false; // the one line that says the index is at its cap (fail open for every bytecode created after it)
    bool wetnessLogged = false; // the one line that says what the wetness knob forces
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The tuning keys of this session, read on every config load and after every menu change, so the render thread never touches the config. A knob
// that moved publishes a new cache epoch: the cached patched bytes belong to the value the knob had.
void PublishParams()
{
    State& state = S();
    state.params = BuildParams(ext::Cfg().ffxiv.ambientSkyVisibilityScale.value_or_default(),
                               ext::Cfg().ffxiv.wetnessOverride.value_or_default());
    state.cacheEpoch.fetch_add(1, std::memory_order_relaxed);
}

// The shadow mapping module's ask, for the arming chain: one cheap hook read, false while the module is not linked
// or its night lift sits at the engine's own value. The curve's decision is the provider's (per composite draw).
bool ShadowsAmbientAsks()
{
    // The seam's "an ambient-table asker beyond the knob" word, in every arming list and in the composite's intent. The engine light controls'
    // constant-buffer family (ext/ffxiv/enginelight) asks through it as well: its master and its night weight, not the engine pass overrides' key,
    // decide it, and a composite that only the family asks for still goes through the composite's validation (intent.shadowsAmbient).
    return (ext::ffxiv::shadows::Hooks::ambientWants != nullptr && ext::ffxiv::shadows::Hooks::ambientWants()) ||
           ext::ffxiv::enginelight::cb::Asks();
}

// The one place the feature bit changes. A transition also forgets the seam's value for the bound shader: the game re-binds its pixel shaders
// many times a frame and the next bind fills it again, so a knob moved from 1.0 to 0.7, a master turned back on after the module stood down,
// or the switch-off itself never patches a draw with a value the seam stored under another state. The cached wetness copy belongs to that
// state too, so a new cache epoch goes with it.
void SetArmed(bool armed)
{
    if (ext::Active(ext::F::EnginePassOverrides) != armed)
    {
        S().boundAnchors.store(0, std::memory_order_relaxed);
        S().cacheEpoch.fetch_add(1, std::memory_order_relaxed);
    }
    ext::SetActive(ext::F::EnginePassOverrides, armed);
}

// The keys and the feature bit. Turning the master off, or leaving every knob at the engine's own value, puts the draw path back to one relaxed
// load - unless another of the seam's features wants a draw (the ambient occlusion pass, the puddles, the light shafts, the screen-space
// reflections and the shadow mapping module's ambient lift), which is why each of them has the last word here. The shadows module's ask is one
// cheap hook read (its keys' identity test) so the seam is entered when its night lift is armed, whatever the curve says today.
void ApplyFeatureBit(bool on)
{
    SetArmed(FeatureBitWanted(on, AnyKnobOn(S().params), ao::Armed(), gloss::Armed(), ext::ffxiv::shafts::Armed(), ssr::Armed(), ShadowsAmbientAsks(),
                              ext::ffxiv::godrays::Armed(), ext::ffxiv::engine::gi::Armed(), ext::ffxiv::enginelight::Armed(), ext::ffxiv::engine::atmo::Armed(), ext::ffxiv::dof::Armed(), flare::Armed()));
}

// The cache's epoch, for FrameCache::Begin: the knob and the master publish a number when they change, and the patch primitive counts its own
// re-initialisation (a device change, or a size cache eviction that frees the buffer a cached entry points at). Both numbers only grow, so a
// change of either is a new epoch - and folding them here keeps every write of the cache itself on the draw thread.
uint32_t CacheEpoch()
{
    State& state = S();
    return state.cacheEpoch.load(std::memory_order_relaxed) + state.patch.Generation();
}

// Reads the draw's bound buffers, its render target and its viewport, and stores what the pure validation needs. Changes none of the
// context's state. `ambientSlot` is the slot the seam's packed value carried; the reference this keeps is that slot's alone, so a buffer the
// game binds at two slots is released once for each slot this does not own.
void Inspect(ID3D11DeviceContext* context, uint32_t ambientSlot, DrawFacts& facts, ID3D11Buffer*& ambient)
{
    ambient = nullptr;

    ID3D11Buffer* buffers[kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, kConstantBufferSlots, buffers);
    if (ambientSlot < kConstantBufferSlots)
        ambient = buffers[ambientSlot];
    if (ambient != nullptr)
    {
        D3D11_BUFFER_DESC desc {};
        ambient->GetDesc(&desc);
        facts.boundBytes = desc.ByteWidth;
    }
    for (uint32_t i = 0; i < kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr && i != ambientSlot)
            buffers[i]->Release();

    // Exactly one render target at the size of the scene is the composite's shape; the second slot tells "one" from "more than one".
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.renderTargets = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    if (views[0] != nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        if (resource)
        {
            D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            {
                D3D11_TEXTURE2D_DESC texture {};
                static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
                facts.targetWidth = texture.Width;
                facts.targetHeight = texture.Height;
            }
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window[0] = viewport.TopLeftX;
    facts.window[1] = viewport.TopLeftY;
    facts.window[2] = viewport.Width;
    facts.window[3] = viewport.Height;

    facts.replay = ext::ffxiv::chara::t_replay;
}

// The module's own GPU work is marked, so its copies and dispatches never reach the draw classification: a copy of the patch is not a draw of
// the frame. Set around each patch and put back by the destructor, also on a throw.
struct PatchGuard
{
    State& state;
    const bool previous;
    explicit PatchGuard(State& state) : state(state), previous(state.inPatch.exchange(true, std::memory_order_relaxed)) {}
    ~PatchGuard() { state.inPatch.store(previous, std::memory_order_relaxed); }
};

// The wetness draw's facts: the buffer bound at the slot its shader declares. Only that one slot is read (the same read of all 14 slots on every
// material draw would cost more than the patch it saves), and no render target, size or viewport is looked at - the knob has no shape rule, because
// a material draw writes the five-target G-buffer and a shadow-atlas draw writes no colour, and both may carry the value.
void InspectWetness(ID3D11DeviceContext* context, uint32_t wetnessSlot, WetnessFacts& facts, ID3D11Buffer*& wetness)
{
    wetness = nullptr;
    if (wetnessSlot < kConstantBufferSlots)
        context->PSGetConstantBuffers(wetnessSlot, 1, &wetness);
    if (wetness != nullptr)
    {
        D3D11_BUFFER_DESC desc {};
        wetness->GetDesc(&desc);
        facts.boundBytes = desc.ByteWidth;
    }
    facts.replay = ext::ffxiv::chara::t_replay;
}

// The engine light controls' light and water draws (ext/ffxiv/enginelight): the moon at its light draw, the water pass's copy of it, the water surface's
// ambient block. Apart from the knobs on purpose: the family keeps its own accounting and its own stand-down (a refusal here is never charged to a knob's
// streak, whose limit switches the shipped knobs off), and a draw it did not ask for - the point-light volumes, the shadow-mask readers - costs the
// plan's stamp read and nothing else. The game's draw runs exactly once on every path. The patch is the one CbPatch of this module.
Decline ApplyEngineLightDraw(ID3D11DeviceContext* context, State& state, Anchors anchors, DrawFn draw, void* user)
{
    namespace el = ext::ffxiv::enginelight::cb;
    bool drew = false;
    try
    {
        el::DrawPlan plan;
        if (!el::Plan(context, anchors, plan))
        {
            draw(user);
            return Decline::None;
        }
        ID3D11Buffer* patched = nullptr;
        {
            ext::ffxiv::chara::ReplayScope own;
            PatchGuard guard { state };
            patched = state.patch.Scale(context, plan.game.Get(), plan.bytes, plan.entries, plan.count);
        }
        if (patched == nullptr)
        {
            el::NoteFailure(state.patch.Failure());
            draw(user);
            return Decline::PatchFailed;
        }
        el::RecordPatch(context, plan.kind, plan.slot, plan.game.Get(), patched, plan.bytes, plan.factor, plan.weight);
        {
            BoundConstantBuffer swap(context, plan.slot, patched);
            if (!swap.Bound())
            {
                el::NoteFailure("the patched buffer could not be bound");
                draw(user);
                return Decline::PatchFailed;
            }
            drew = true;
            if (plan.mapReaders)
            {
                // The readers of the light colour that run inside this draw (the chara ambient's night feed, the contact shadows and the shadow
                // mapping route's colour capture) read the game's buffer, not the patched one: otherwise the moon at x0.1 feeds the night curve.
                ext::ffxiv::enginelight::ActiveSwapScope active(patched, swap.Original());
                draw(user);
            }
            else
                draw(user);
        }
        el::NoteRun();
        return Decline::None;
    }
    catch (...)
    {
        el::NoteFailure("a call threw");
        if (!drew)
            draw(user);
        return Decline::Exception;
    }
}

// One of this module's patched draws. A shader that declares both names gets both patches, on the slots its reflection gave, and is drawn once; the
// first refusal leaves the draw exactly as the game made it. Which knobs ask for the draw, and what a refusal is, come from the pure rules
// (IntentOf, AmbientVerdict, WetnessVerdict in EnginePassRules.h), so the refusal streaks are testable without a device.
Decline ApplyDraw(ID3D11DeviceContext* context, State& state, Anchors anchors, DrawFn draw, void* user)
{
    // The swap of one patch, in place while the game's draw runs: what its slot comes back to is the game's own buffer.
    struct Swap
    {
        uint32_t slot = 0;
        ID3D11Buffer* buffer = nullptr; // owned by CbPatch, which keeps it for the session
    };
    Swap swaps[2] {};
    uint32_t swapCount = 0;
    bool ambientRun = false;
    bool wetnessRun = false;
    DrawFacts facts;
    bool drew = false;
    // The engine light controls' light and water draws take their own path (their own accounting, no knob's streak).
    if (!IsAmbientAnchors(anchors) && !IsWetnessAnchors(anchors) && ext::ffxiv::enginelight::cb::WantsDraw(anchors))
        return ApplyEngineLightDraw(context, state, anchors, draw, user);
    // A knob that stood the module down patches nothing, and neither does either knob while the master key is off; the ambient occlusion pass
    // and the light shafts keep their own arming (OnGameDraw decides whether the seam is worth entering at all).
    // The shadows module's ask rides this arm: its master key and its night lift key decide it (ShadowsWants), the
    // composite's anchors decide when it applies, and the entries join the ONE dispatch below.
    const KnobIntent intent = WithShadowsAmbient(IntentOfSession(state.params, anchors, state.session.Armed(), state.keyOn.load(std::memory_order_relaxed)),
                                                 anchors, ShadowsAmbientAsks());
    // A refusal belongs to the knob that asked for the draw, and moves that knob's streak alone.
    const auto decline = [&](Knob knob, Decline reason) {
        state.declined++;
        if (uint32_t(reason) < kDeclineReasonCount)
            state.declines[uint32_t(reason)]++; // the per-reason table a capture reads instead of the log
        if (state.log.Take(uint32_t(reason)))
            LOG_INFO("{}", FormatDecline(reason));
        if (state.session.NoteDecline(knob) && !state.loggedOff)
        {
            state.loggedOff = true;
            LOG_WARN("{}", FormatSwitchedOff());
        }
        if (!drew)
            draw(user);
        return reason;
    };

    try
    {
        // ---- the ambient arm: the composite's probe table, the knob's scale and - in the SAME dispatch - the
        // shadow mapping module's night lift (its entries append after the shipped knob's own: CbScale.hlsl
        // multiplies every matching entry in order, so the two levers compose on the field they share, and a lone
        // shadows arm is the same dispatch shape with only its own entries). One Inspect, one verdict, one bind.

        ScaleEntry entries[kMaxScaleEntries] {};
        uint32_t knobCount = 0;
        uint32_t shadowsCount = 0;
        float shadowsCurve = 0.0f;
        if (intent.ambient)
            knobCount = AmbientEntries(state.params, entries, kMaxScaleEntries);
        if (intent.shadowsAmbient && ext::ffxiv::shadows::Hooks::ambientEntries != nullptr)
            shadowsCount = ext::ffxiv::shadows::Hooks::ambientEntries(context, entries + knobCount,
                                                                      kMaxScaleEntries - knobCount, &shadowsCurve);
        // The engine light controls' composite entries (the SH rows per channel and the header's sky scale): appended AFTER the shadows arm's, so the
        // shadows arm's first entry stays at entries[knobCount]. The shadows module yields its night ambient lift while the engine light master is on.
        uint32_t engineLightCount = 0;
        if (IsAmbientAnchors(anchors))
            engineLightCount = ext::ffxiv::enginelight::cb::AmbientEntries(entries + knobCount + shadowsCount, kMaxScaleEntries - knobCount - shadowsCount);
        {
            // A count of zero is the engine's own picture: the knob at 1.0 and the night lift at its identity (a
            // day frame's curve, or a no-reading session's). The draw is then the game's own call, not a refusal,
            // and the wetness knob may still want it.
            const uint32_t count = knobCount + shadowsCount + engineLightCount;
            if (count != 0)
            {
                ID3D11Buffer* bound = nullptr;
                Inspect(context, AmbientSlotOf(anchors), facts, bound);
                // The reference the inspection took is owned here, so every path below - a decline, a failure or a throw - gives it back.
                ComPtr<ID3D11Buffer> ambient;
                ambient.Attach(bound);
                if (!ambient)
                    facts.boundBytes = 0; // the validation says "nothing bound" instead of "wrong size"
                const Verdict verdict = AmbientVerdict(intent, facts);
                if (verdict.kind == KnobVerdict::Refused)
                    return decline(Knob::Ambient, verdict.reason);

                // The patch: the game's buffer is read GPU side, the scaled copy is bound for the game's draw, and the game's binding comes back
                // before this returns. The character preview's marker is raised around the work so its copy and clear hooks do not count this
                // module's copies as the game's.
                ID3D11Buffer* patched = nullptr;
                {
                    ext::ffxiv::chara::ReplayScope own;
                    PatchGuard guard { state };
                    patched = state.patch.Scale(context, ambient.Get(), facts.boundBytes, entries, count);
                }
                if (patched == nullptr)
                    return decline(Knob::Ambient, Decline::PatchFailed);
                swaps[swapCount++] = Swap { AmbientSlotOf(anchors), patched };
                ambientRun = true;
                if (engineLightCount != 0)
                    ext::ffxiv::enginelight::cb::RecordComposite(context, AmbientSlotOf(anchors), ambient.Get(), patched, facts.boundBytes);
            }
        }

        // ---- the wetness knob: the buffer every material draw of the frame binds
        if (intent.wetness)
        {
            SetEntry setEntries[kMaxSetEntries] {};
            if (const uint32_t setCount = WetnessEntries(state.params, setEntries, kMaxSetEntries); setCount != 0)
            {
                const uint32_t wetnessSlot = WetnessSlotOf(anchors);
                WetnessFacts wetnessFacts;
                ID3D11Buffer* bound = nullptr;
                InspectWetness(context, wetnessSlot, wetnessFacts, bound);
                ComPtr<ID3D11Buffer> wetness;
                wetness.Attach(bound);

                // One patch per frame and buffer: the engine binds the same wetness buffer for every material draw of a frame, so the cache
                // answers every later draw and only the slot swap is left. Its frame token is the output-frame counter both present paths tick
                // (ext/present/PresentSeams.h), one relaxed load: a session with frame generation off and SDR output never runs the bridge, and
                // its present count would sit at one value for the whole session. The epoch carries the knob, the master and the patch
                // primitive's generation, so a changed knob or a re-created buffer clears the entry here.
                state.wetnessCache.Begin(ext::present::FrameCount(), CacheEpoch());
                const bool cached = wetness && state.wetnessCache.Has(wetness.Get());
                const Verdict verdict = WetnessVerdict(intent, wetnessFacts, cached, state.wetnessCache.MayPatch());
                if (verdict.kind == KnobVerdict::Refused)
                    return decline(Knob::Wetness, verdict.reason);
                if (verdict.kind == KnobVerdict::Serve)
                {
                    // The cached buffer is read before Serve(): the bind that reaches the expiry clears the entry, and that draw still binds the
                    // copy the cache was holding instead of a null.
                    ID3D11Buffer* cachedCopy = reinterpret_cast<ID3D11Buffer*>(const_cast<void*>(state.wetnessCache.Dst()));
                    state.wetnessCache.Serve();
                    state.wetnessSwaps++;
                    swaps[swapCount++] = Swap { wetnessSlot, cachedCopy };
                    wetnessRun = true;
                }
                else if (verdict.kind == KnobVerdict::Patch)
                {
                    ext::ffxiv::chara::ReplayScope own;
                    PatchGuard guard { state };
                    ID3D11Buffer* patched = state.patch.Set(context, wetness.Get(), wetnessFacts.boundBytes, setEntries, setCount);
                    if (patched == nullptr)
                        return decline(Knob::Wetness, Decline::PatchFailed);
                    state.wetnessCache.Record(wetness.Get(), patched);
                    state.wetnessRuns++;
                    if (!state.wetnessLogged)
                    {
                        state.wetnessLogged = true;
                        LOG_INFO("{}", FormatWetnessRun(state.params, wetnessSlot, wetnessFacts.boundBytes, state.patch.HasGpuMilliseconds(),
                                                        state.patch.GpuMilliseconds()));
                    }
                    state.wetnessSwaps++;
                    swaps[swapCount++] = Swap { wetnessSlot, patched };
                    wetnessRun = true;
                }
                // A draw whose buffer under that name is not the engine's own 32 bytes, or a replay of one, is not a refusal: it is another draw
                // of the frame, and the name sits on about 1,500 shaders.
            }
        }

        if (swapCount == 0)
        {
            draw(user); // neither knob asked for this draw: the game's own call, and nothing counted
            return Decline::None;
        }

        // Both swaps are in place for the one draw and the game's own buffers are back when the guards go out of scope, also when the draw throws.
        // A guard built with no replacement binds nothing, so a draw with one patch costs one guard's work.
        {
            BoundConstantBuffer first(context, swaps[0].slot, swaps[0].buffer);
            BoundConstantBuffer second(context, swapCount > 1 ? swaps[1].slot : swaps[0].slot, swapCount > 1 ? swaps[1].buffer : nullptr);
            if (!first.Bound())
                return decline(intent.ambient ? Knob::Ambient : Knob::Wetness, Decline::PatchFailed);
            drew = true;
            draw(user);
        }

        if (ambientRun)
        {
            state.runs++;
            state.lastFacts = facts; // the composite's facts, for the capture's engine.txt
            state.haveRun = true;
            if (shadowsCount != 0)
            {
                // Counted for the shadow mapping card whatever the shipped knob did. The line below is the shadows arm's alone; with the
                // knob in the same dispatch it is the knob's, so the shadows arm says its own beside it (once per composite size).
                ++state.shadowsRuns;
                if (knobCount != 0 && state.shadowsLog.Take(0x80000000u | ((facts.targetWidth & 0x7FFFu) << 11) | (facts.targetHeight & 0x7FFu)))
                    LOG_INFO("{}", ext::ffxiv::shadows::FormatAmbientRun(entries[knobCount].multiplier[1], shadowsCurve, facts.boundBytes,
                                                                         facts.targetWidth, facts.targetHeight, AmbientSlotOf(anchors)));
            }
            if (engineLightCount != 0 && state.engineLightLog.Take(0x80000000u | ((facts.targetWidth & 0x7FFFu) << 11) | (facts.targetHeight & 0x7FFu)))
                LOG_INFO("{}", ext::ffxiv::enginelight::cb::CompositeRunLine(facts.boundBytes));
            if (state.log.Take(0x80000000u | ((facts.targetWidth & 0x7FFFu) << 11) | (facts.targetHeight & 0x7FFu)))
            {
                if (knobCount != 0)
                    LOG_INFO("{}", FormatRun(state.params, facts.boundBytes, facts.targetWidth, facts.targetHeight,
                                             state.patch.HasGpuMilliseconds(), state.patch.GpuMilliseconds()));
                else if (shadowsCount != 0)
                    // The shadows arm alone: its own line, naming the dispatch and the curve the entries ran under.
                    LOG_INFO("{}", ext::ffxiv::shadows::FormatAmbientRun(entries[knobCount].multiplier[1], shadowsCurve, facts.boundBytes,
                                                                         facts.targetWidth, facts.targetHeight, AmbientSlotOf(anchors)));
            }
            state.session.NoteRun(Knob::Ambient);
        }
        if (wetnessRun)
            state.session.NoteRun(Knob::Wetness);
        return Decline::None;
    }
    catch (...)
    {
        return decline(intent.ambient ? Knob::Ambient : Knob::Wetness, Decline::Exception);
    }
}
} // namespace

void OnGameDraw(ID3D11DeviceContext* context, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return;
    ext::ffxiv::enginelight::LampDraw lampDraw; // lives as long as the call: the lamp arm's wrapper (below) points at it, also in the catch
    struct NativeDraw
    {
        DrawFn draw;
        void* user;
        water_trace::Scope* trace;
    } native { draw, user, nullptr };
    const auto forwardNative = +[](void* opaque) {
        auto& call = *static_cast<NativeDraw*>(opaque);
        if (call.trace != nullptr)
            call.trace->NativeCallback();
        ext::core::draw_dispatch::ForwardPhaseScope originalPhase(1);
        call.draw(call.user);
    };
    std::optional<water_trace::Scope> trace;
    ssr::Status traceBefore {};
    // The four features have their own switch-offs: this module's session covers the two knobs only, and a session that stood them down must not
    // take the ambient occlusion pass, the puddles, the light shafts or the screen-space reflections with it (each decides with its own key and
    // its own streak).
    if (context == nullptr ||
        !DrawPathWanted(state.session.Armed(), ao::Armed(), gloss::Armed(), ext::ffxiv::shafts::Armed(), ssr::Armed(), ShadowsAmbientAsks(),
                        ext::ffxiv::godrays::Armed(), ext::ffxiv::engine::gi::Armed(), ext::ffxiv::enginelight::Armed(), ext::ffxiv::engine::atmo::Armed(), ext::ffxiv::dof::Armed(), flare::Armed()))
    {
        // Nothing wants the seam any more: the last feature to stand down takes the feature bit with it, so its draws cost one relaxed load
        // again instead of entering here and finding that no feature wants them.
        if (!state.session.Armed())
            SetArmed(AnyFeatureArmed(ao::Armed(), gloss::Armed(), ext::ffxiv::shafts::Armed(), ssr::Armed(), ShadowsAmbientAsks(),
                                     ext::ffxiv::godrays::Armed(), ext::ffxiv::engine::gi::Armed(), ext::ffxiv::enginelight::Armed(), ext::ffxiv::engine::atmo::Armed(), ext::ffxiv::dof::Armed(), flare::Armed()));
        draw(user);
        return;
    }
    if (state.inPatch.load(std::memory_order_relaxed))
    {
        draw(user); // this module's own work: never classified, never a decline
        return;
    }
    ++state.drawsSeen;
    try
    {
        // The lookup is the seam's packed value for the bound shader: one relaxed load, no lock and no state query. A shader that declares
        // neither of the knobs' names is passed on here, before anything is measured, queried or counted: a frame makes far more draws of those
        // shaders than material and composite draws, and counting them would stand the module down before the first of its own draws.
        const int64_t lookupStart = state.cost.Sampling() ? Qpc() : 0;
        const Anchors anchors = state.boundAnchors.load(std::memory_order_relaxed);
        // The engine light lamp arm (ENG2): a point-light volume draw gets the blend-factor wrapper in place of the game's draw function BEFORE any module below sees it, so
        // every branch (the ambient occlusion / puddles / reflection reader swaps, the knobs' ApplyDraw, the chain stages and the plain pass-through) calls the wrapped draw
        // and the factor lands on the game's one real call. A factor of 1 builds nothing: a bit test of the anchors already read, on every draw.
        if (!ext::ffxiv::chara::t_replay && ext::ffxiv::enginelight::PrepareLampDraw(context, anchors, draw, user, lampDraw)) // a character-preview replay is not the world's light pass
        {
            native.draw = draw;
            native.user = user;
        }
        if (lookupStart != 0)
            state.cost.Sample(Qpc() - lookupStart);
        else
            state.cost.Skip();
        if (state.cost.Done() && !state.cost.logged)
        {
            state.cost.logged = true;
            LOG_INFO("{}", FormatCost(state.cost, state.drawsSeen, state.runs, state.wetnessRuns, state.wetnessSwaps, state.declined,
                                       QpcFrequency()));
        }

        // The light shafts' own draws (a light draw, the composite and the tone map) see this seam first: the module records the frame's sun and
        // scene or runs its pass, and only the tone-map draw is one it draws itself (true); every other draw carries on below. It is reached
        // whether or not this module's session is still armed: the four features' switch-offs are separate.
        if (ext::ffxiv::shafts::OnEngineDraw(context, anchors, draw, user))
            return;
        // The puddles' tone-map stage, next to the shafts' one: it records the frame's composite inputs on that draw and runs its own pass at the
        // tone map. The shafts' branch above owns that draw while its card is on (one module rebinds t0, and the shafts' air light is part of the
        // scene the pool reflects), so this one runs when the shafts left the tone map alone. One bit test for every other draw of the frame.
        if (gloss::OnToneStageDraw(context, anchors, draw, user))
            return;
        // The volumetric march, the chain's tone-map stage: its captures at the fog post and the assembly draws, and the tone map's fallback
        // owner when neither earlier stage took the draw. One bit test for every other draw of the frame.
        if (ext::ffxiv::godrays::OnSeamDraw(context, anchors, draw, user))
            return;
        // The screen-space light bounce, the chain's stage after the march: its captures at the composite and the fog post, and the tone
        // map's fallback owner when neither earlier stage nor the march took the draw. One bit test for every other draw of the frame.
        if (ext::ffxiv::engine::gi::OnSeamDraw(context, anchors, draw, user))
            return;
        // The engine light controls (ENG2), the chain's stage right behind the light bounce: the frame tick at the composite and the tone map's fallback owner
        // when the earlier stages did not take the draw (it hands the atmosphere, then depth of field, the turn on every exit it has). A false from any of
        // them falls through to the next one, and the seam's own pass-through draws the game's call once. One bit test for every other draw of the frame.
        if (ext::ffxiv::enginelight::OnSeamDraw(context, anchors, draw, user))
            return;
        // The atmosphere, the chain's stage between the light bounce and depth of field: its captures at the light draws, the fog post and the composite
        // ride the same anchors the earlier stages' do, and the tone map's fallback owner when none of them took the draw (it composes depth of field on
        // its own answer). A false from any of them falls through to the next one.
        if (ext::ffxiv::engine::atmo::OnSeamDraw(context, anchors, draw, user))
            return;
        // The depth-of-field stage, the chain's last member: its captures at the fog post and the light draws ride the same anchors the
        // earlier stages' do, and the tone map's fallback owner when none of them took the draw. A false from any of them falls through
        // to the next one, and the seam's own pass-through draws the game's call once.
        if (ext::ffxiv::dof::OnSeamDraw(context, anchors, draw, user))
            return;
        // The lens flare's card, next: its four draws carry no anchors bit of their own (the anchors word was full when this card came), so
        // the card answers its own stamps and is reached here for every draw the seam sees - one relaxed load of the card's bind value for
        // the frames whose draws do not belong to the family.
        if (flare::OnSeamDraw(context, draw, user))
            return;
        // The engine light controls' light draws that carry none of the five bits below: no reader of the occlusion, the puddles or the reflections
        // to swap, so the family's own path takes the draw (a light draw that does carry one goes on through the guards and ApplyDraw).
        if (ext::ffxiv::enginelight::cb::WantsDraw(anchors) && !IsAmbientAnchors(anchors) && !IsWetnessAnchors(anchors) && !IsOcclusionAnchors(anchors) &&
            !gloss::IsGlossAnchors(anchors) && !IsReflectionAnchors(anchors))
        {
            ApplyEngineLightDraw(context, state, anchors, draw, user);
            return;
        }
        if (!IsAmbientAnchors(anchors) && !IsWetnessAnchors(anchors) && !IsOcclusionAnchors(anchors) && !gloss::IsGlossAnchors(anchors) &&
            !IsReflectionAnchors(anchors))
        {
            draw(user); // none of the seam's draws: passed through untouched
            return;
        }
        // A reader of the engine's occlusion texture binds this module's occlusion for that one draw; the guard puts the game's binding back
        // when the draw is over, and an ordinary draw of the size this module does not serve binds nothing at all.
        ao::BoundReader occlusion = ao::SwapReader(context, anchors);
        // The puddles pass's own reader: the frame's shaped G-buffer target for this one draw, and the frame's copy of the engine's wetness
        // amount from whichever draw binds it first.
        gloss::BoundReader puddles = gloss::SwapReader(context, anchors);
        // A sampler of the engine's planar reflection map binds this module's reflection for that one draw, the same way (its own guard puts the
        // game's binding back).
        //
        // The three guards are constructed in this order and so destroyed in the reverse one: the reflection's binding, the puddles' and the
        // occlusion's go back before the draw returns, each on the same context the next one was swapped on.
        // Reflection readers while SSR is armed form the bounded diagnostic candidate
        // set. Start the outer query before SwapReader can run any private SSR work;
        // readerBound is recorded after the attempted swap as memory-only metadata.
        const bool traceCandidate = IsReflectionAnchors(anchors) && ssr::Armed();
        const ssr::Status candidateStatus = traceCandidate && ssr::Armed() ? ssr::CurrentStatus() : ssr::Status {};
        // The existing SSR shader-object stamp distinguishes water from other
        // planar-map readers. Read it before opening the scope/private pass; this
        // diagnostic-only getter never runs between the private work and draw.
        const bool traceWater = candidateStatus.diagnostic && IsStampedWaterReader(context);
        traceBefore = traceWater ? candidateStatus : ssr::Status {};
        if (traceCandidate)
        {
            trace.emplace(context, true, traceWater, ext::present::FrameCount(), traceBefore.diagnosticDraws,
                          traceBefore.diagnosticMode);
            if (traceWater && trace->Active())
                native.trace = &*trace;
        }
        ssr::BoundReader reflection = ssr::SwapReader(context, anchors);
        if (native.trace != nullptr)
            trace->SetReaderBound(reflection.Bound());
        // The capture frame's reflection A/B (SsrAb.h): at the first reader draw the reflections served, the call below issues the same draw once
        // more with the engine's own map into a private copy of the target, and then the real draw. One relaxed load on every other frame.
        DrawFn gameDraw = native.trace ? forwardNative : draw;
        void* gameUser = native.trace ? &native : user;
        ssr::ab::Call reflectionAb;
        if (reflection.Bound())
            ssr::ab::Begin(reflectionAb, context, reflection, gameDraw, gameUser);
        ApplyDraw(context, state, anchors, gameDraw, gameUser);
        ssr::ab::End(reflectionAb);
        if (traceWater)
        {
            const ssr::Status traceAfter = ssr::CurrentStatus();
            trace->SetPrivateDraws(traceAfter.diagnosticDraws - traceBefore.diagnosticDraws);
        }
        if (!state.session.Armed())
            SetArmed(AnyFeatureArmed(ao::Armed(), gloss::Armed(), ext::ffxiv::shafts::Armed(), ssr::Armed(), ShadowsAmbientAsks(),
                                     ext::ffxiv::godrays::Armed(), ext::ffxiv::engine::gi::Armed(), ext::ffxiv::enginelight::Armed(), ext::ffxiv::engine::atmo::Armed(), ext::ffxiv::dof::Armed(), flare::Armed())); // stays on while any feature wants a draw
    }
    catch (...)
    {
        if (native.trace != nullptr)
        {
            const ssr::Status traceAfter = ssr::CurrentStatus();
            trace->SetPrivateDraws(traceAfter.diagnosticDraws - traceBefore.diagnosticDraws);
            forwardNative(&native);
        }
        else
            draw(user);
    }
}

void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances)
{
    if (!ext::ActiveAny(ext::F::EnginePassOverrides, ext::F::LightShafts, ext::F::GodraysVolumetric, ext::F::Gi, ext::F::DepthOfField, ext::F::LensFlare,
                        ext::F::Atmosphere, ext::F::NightGrade))
        return; // one relaxed load per bind while the keys are off; nothing is tracked because nothing reads it
    State& state = S();
    if (state.inPatch.load(std::memory_order_relaxed))
        return;
    // The engine's scene HDR path takes every game draw before this seam hands the shafts one, so that path makes the module's per-frame sample
    // too: a session that switches HDR on or off is noticed within the frame, with its own key untouched (one relaxed load a bind while the
    // shafts key is on; the sample itself is once an output frame). The march and the light bounce sample the same state for the same reason.
    if (ext::Active(ext::F::LightShafts))
        ext::ffxiv::shafts::SampleSceneHdr();
    if (ext::Active(ext::F::GodraysVolumetric))
        ext::ffxiv::godrays::SampleSceneHdr();
    if (ext::Active(ext::F::Gi))
        ext::ffxiv::engine::gi::SampleSceneHdr();
    if (ext::Active(ext::F::Atmosphere))
        ext::ffxiv::engine::atmo::SampleSceneHdr();
    if (ext::Active(ext::F::DepthOfField))
        ext::ffxiv::dof::SampleSceneHdr();
    if (ext::Active(ext::F::NightGrade))
        ext::ffxiv::enginelight::SampleSceneHdr();
    // The depth-of-field stage's bind seam: whether the shader the game bound is the engine's own weight pass (its stamp, one table find).
    if (ext::Active(ext::F::DepthOfField))
        ext::ffxiv::dof::OnPixelShaderSet(shader, instances);
    // The lens flare's bind seam: the station stamp of the shader the game bound, one table find beside the anchors one. A class-instance
    // override never carries the value (the module answers 0 itself).
    if (ext::Active(ext::F::LensFlare))
        flare::OnPixelShaderSet(shader, instances);
    ++state.bindsSeen;
    state.boundAnchors.store(instances == 0 ? TrackedAnchorsOf(shader) : 0, std::memory_order_relaxed);
}

void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc)
{
    if (shader == nullptr || crc == 0 || code == nullptr || size < 32)
        return;
    if (!MentionsAnyName(code, size))
        return; // most of the game's shaders, on a byte scan alone: not a draw this module can patch
    try
    {
        // The ambient occlusion pass's stamp, before anything this module's index decides: a shader the index does not store (no anchor bit of
        // its own, a full index) still has to answer for the slots its own reflection declares.
        ao::NoteShader(shader, code, size);
        gloss::NoteShader(shader, code, size);
        // The screen-space reflections pass's stamp, on the same terms.
        ssr::NoteShader(shader, code, size);
        // The light bounce's composite stamp, on the same terms (five slots of the frame's own G-buffer family, one word on the shader).
        ext::ffxiv::engine::gi::NoteShader(shader, code, size);
        // The fog post's height-profile slot, on the same terms: the god rays pick the profile by the slot the shader names, and the stamp
        // is written at creation whatever the key says.
        ext::ffxiv::godrays::NoteShader(shader, code, size);
        // The lens flare's stamps, on the same terms: the family's four draws say their own reflected names, so the card wants them
        // stamped whatever the key says (a switch turned on later finds the family's shaders).
        flare::NoteShader(shader, code, size);
        // The engine's own depth of field's weight pass, on the same terms: stamped whatever the key says, so a switch turned on later finds it.
        ext::ffxiv::dof::NoteShader(shader, code, size);
        // The engine light controls' stamp (the main directional light, the water pass's copy of it, the water surface), on the same terms.
        ext::ffxiv::enginelight::cb::NoteShader(shader, code, size);
        // The atmosphere's own stamp of the fog post's height-profile slot (its own tag), written at creation whatever the key says.
        ext::ffxiv::engine::atmo::NoteShader(shader, code, size);
        // The engine light controls' stamp of the composite's g_CommonParameter slot, on the same terms (written at creation whatever the key says).
        ext::ffxiv::enginelight::NoteShader(shader, code, size);
        const bool logging = ext::Active(ext::F::EnginePassOverrides);
        const PassLayout layout = PassLayoutOfShader(code, size);
        if (!layout.ok)
            return;
        // A shader that names one of the buffers but whose layout carries no anchor bit for any knob (another pass that happens to bind the
        // name, or a shape the knobs cannot address) is not a draw this module can patch. It is not stored and takes no id, so the common
        // names of the game's material shaders cannot fill the table and leave a patchable shader unclassified.
        Anchors anchors = DrawAnchors(layout);
        if (anchors == 0)
            return;
        uint32_t id = 0;
        bool full = false;
        bool refused = false;
        {
            // One bytecode is parsed once, however often the game creates a shader object from it: the id is keyed on the CRC and only
            // bounds the index's size. The insert shares this lock, so two loader threads that create shaders at once cannot mix two records
            // in one slot.
            std::lock_guard lock(I().mutex);
            const auto found = I().byCrc.find(crc);
            if (found != I().byCrc.end())
                id = found->second;
            else if (IndexHasRoom(I().byCrc.size()))
            {
                id = ++I().next;
                I().byCrc.emplace(crc, id);
                S().indexed.store(static_cast<uint32_t>(I().byCrc.size()), std::memory_order_relaxed);
            }
            else
            {
                refused = true; // a NEW bytecode past the cap: the index keeps the ones it has and this one's draws fail open
            }
            if (id != 0)
            {
                // A shader that refused the stamp can never answer a bind, so it takes no slot and its draws fail open.
                if (SUCCEEDED(shader->SetPrivateData(kLayoutTag, sizeof id, &id)))
                    full = SeenForInsert().Insert(reinterpret_cast<uintptr_t>(shader), id, anchors) == SeenInsert::Full;
            }
        }
        if (refused)
        {
            // Counted and written once: a session reads why a shader created late is not patched instead of finding its draws passed on with
            // no line (the water shaders of a long session are created exactly this late).
            S().indexRefused.fetch_add(1, std::memory_order_relaxed);
            if (!S().indexRefusedLogged)
            {
                S().indexRefusedLogged = true;
                LOG_WARN("{}", FormatIndexRefused());
            }
            return;
        }
        if (id == 0)
            return;
        if (full && !S().indexFullLogged)
        {
            S().indexFullLogged = true;
            LOG_WARN("{}", FormatIndexFull());
        }
        if (logging && !S().indexedLogged)
        {
            S().indexedLogged = true;
            LOG_INFO("Engine pass overrides: shader indexed (g_AmbientParamArray slot {}, g_SamplerLightDiffuse slot {}, {} output(s))",
                     layout.Has(0) ? layout.SlotOf(0) : 0u, layout.Has(1) ? layout.SlotOf(1) : 0u, layout.outputs);
        }
        // The line above names whichever shader was indexed first, so it does not say which slots the composite was found at. The ambient
        // arm patches the slot of g_AmbientParamArray in THIS shader's own reflection; this line names it once, when the composite is indexed (it is
        // written only while the shadow mapping module asks for the ambient arm, so the shipped knob's path gains no line).
        if (logging && IsAmbientDraw(layout) && ShadowsAmbientAsks() && !S().compositeIndexedLogged)
        {
            S().compositeIndexedLogged = true;
            LOG_INFO("{}", ext::ffxiv::shadows::FormatCompositeIndexed(layout.SlotOf(0), layout.SlotOf(1)));
        }
    }
    catch (...)
    {
    }
}

bool Armed()
{
    State& state = S();
    return ext::Active(ext::F::EnginePassOverrides) && state.session.Armed();
}

Params CurrentParams() { return S().params; }

void SetOwnWork(bool on) { S().inPatch.store(on, std::memory_order_relaxed); }

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.identity = !AnyKnobOn(state.params);
    // The card's word is the master key's, not the feature bit's: the bit stays on while the ambient occlusion pass, the puddles, the light shafts
    // or the screen-space reflections want a draw, and the card describes the two knobs that share this key.
    status.armed = KnobsArmed(state.keyOn.load(std::memory_order_relaxed), state.params, state.session.Armed());
    status.runs = state.runs;
    status.shadowsRuns = state.shadowsRuns;
    status.declined = state.declined;
    status.wetnessRuns = state.wetnessRuns;
    status.wetnessSwaps = state.wetnessSwaps;
    status.indexed = state.indexed.load(std::memory_order_relaxed);
    status.indexRefused = state.indexRefused.load(std::memory_order_relaxed);
    return status;
}

// The counters are read from the render thread (the diag's Present path and the menu's own status read), and they are written there by
// ApplyDraw on the game's immediate context, so the plain fields below need no lock for that use; a deferred-context draw on a worker thread
// would make this read torn, so it stays a render-thread-only export.
Diagnostics CurrentDiagnostics()
{
    State& state = S();
    Diagnostics diagnostics;
    diagnostics.key = state.keyOn.load(std::memory_order_relaxed);
    const Status status = CurrentStatus();
    diagnostics.armed = status.armed;
    diagnostics.identity = status.identity;
    diagnostics.runs = status.runs;
    diagnostics.declined = status.declined;
    diagnostics.indexed = status.indexed;
    diagnostics.indexRefused = status.indexRefused;
    for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        diagnostics.declines[i] = state.declines[i];
    diagnostics.haveRun = state.haveRun;
    if (diagnostics.haveRun)
    {
        diagnostics.boundBytes = state.lastFacts.boundBytes;
        diagnostics.targetWidth = state.lastFacts.targetWidth;
        diagnostics.targetHeight = state.lastFacts.targetHeight;
        for (size_t i = 0; i < 4; ++i)
            diagnostics.window[i] = state.lastFacts.window[i];
    }
    return diagnostics;
}

void Publish(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    PublishParams();
    // Every arm and disarm transition forgets the seam's value for the bound shader, so the first draw after a menu change or a config load
    // is the game's own call until the game binds a shader the seam classified under the new state (it binds many a frame).
    state.boundAnchors.store(0, std::memory_order_relaxed);
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.shadowsLog = LogGate {};
        state.loggedOff = false;
        state.indexedLogged = false;
        state.compositeIndexedLogged = false;
        state.indexFullLogged = false;
        state.indexRefusedLogged = false;
        state.wetnessLogged = false;
        state.patch.Release();
        SetArmed(AnyFeatureArmed(ao::Armed(), gloss::Armed(), ext::ffxiv::shafts::Armed(), ssr::Armed(), ShadowsAmbientAsks(),
                                 ext::ffxiv::godrays::Armed(), ext::ffxiv::engine::gi::Armed(), ext::ffxiv::enginelight::Armed(), ext::ffxiv::engine::atmo::Armed(), ext::ffxiv::dof::Armed(), flare::Armed())); // stays on while any feature wants it
        return;
    }
    // Turning the switch back on after the module switched itself off is the player's re-arm: the counters start again and the draw path
    // measures the intent again. The index is not touched: it is filled whatever the key says.
    if (state.session.off)
        state.session = Session {};
    state.indexedLogged = false;
    state.compositeIndexedLogged = false;
    state.shadowsLog = LogGate {};
    state.indexFullLogged = false;
    state.indexRefusedLogged = false;
    state.wetnessLogged = false;
    ApplyFeatureBit(true);
}

void Release()
{
    State& state = S();
    state.boundAnchors.store(0, std::memory_order_relaxed);
    // No cache write here: releasing the patch primitive bumps its generation, which is part of the epoch the next draw's Begin() compares, so
    // the entry is dropped by the draw thread that owns it.
    state.patch.Release();
    ao::Release();
    gloss::Release();
    ext::ffxiv::shafts::Release(); // the shafts' textures and read-back ring go back while the device is still valid, so no old-device staging
                                   // buffer survives a device change or the unload
    flare::Release();              // the lens flare's patch buffers, the same way
    ssr::Release();
    // The shadow mapping module owns its own copy and staging buffers; their release rides the same chain while the
    // device is still valid (no module leaves a buffer on a lost device behind).
    ext::ffxiv::shadows::Release();
    ext::ffxiv::enginelight::cb::Release(); // the family's dump staging
    ext::ffxiv::godrays::Release(); // the march's textures, rings and stolen views, the same way
    ext::ffxiv::engine::gi::Release(); // the light bounce's kept views and camera ring, behind the march's release
    ext::ffxiv::dof::Release();         // the depth-of-field stage's textures and rings, behind the light bounce's
    ext::ffxiv::engine::atmo::Release(); // the atmosphere's textures and rings, between the light bounce's and depth of field's
    ext::ffxiv::enginelight::Release();  // the engine light module's hold pass, exposure ring and lamp-arm blend states
}

// The key reaches the module through the config's own hook pointer (ext::FfxivConfig::enginePassOverridesOnLoaded), so ExtConfig.cpp keeps
// no link dependency on this file. A config load may run inside DllMain, so Publish can run on another thread than the render thread: the
// patch primitive serialises its own entries (CbPatch's mutex), and the draw path only reads atomics.
void OnConfigLoaded(bool on) { Publish(on); }

// The shadows module's arming changed (its config load or its card): the feature bit re-asks, with the four
// features' own arming and the module's own knobs unchanged.
void NotifyShadowArmingChanged() { ApplyFeatureBit(S().keyOn.load(std::memory_order_relaxed)); }

// One of this module's own passes, or another module's: the same guard the patch uses, so the seam and the pixel-shader tracker leave it alone.
OwnWork::OwnWork() : _previous(S().inPatch.exchange(true, std::memory_order_relaxed)) {}
OwnWork::~OwnWork() { S().inPatch.store(_previous, std::memory_order_relaxed); } // a nested guard restores, never clears
} // namespace ext::ffxiv::engine

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run.
struct RegisterEnginePassOverrides
{
    RegisterEnginePassOverrides() { ext::FfxivConfig::enginePassOverridesOnLoaded = &ext::ffxiv::engine::OnConfigLoaded; }
} g_registerEnginePassOverrides;
} // namespace
