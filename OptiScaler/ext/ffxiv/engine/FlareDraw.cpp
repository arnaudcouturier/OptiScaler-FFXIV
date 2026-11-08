#include "pch.h"

#include "ext/ffxiv/engine/Flare.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's marker and replay guard
#include "ext/ffxiv/engine/CbPatch.h"
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame
#include "ext/ffxiv/engine/EnginePass.h" // OwnWork: the patch's own work is marked, so the seam never classifies it

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

// Lens flare (ext/ffxiv/engine/Flare.h): the index the family's shaders are stamped into at creation, the bind seam's one value, and the
// draw path's patches. The pure decisions are in engine/FlareRules.h; the constant-buffer patch is the seam's own primitive
// (engine/CbPatch.cpp), kept as this card's own instance so neither card's failure state or counters belong to the other.
//
// Cost with the key off: one relaxed load in the draw path and nothing else - the stamps are built while the game creates its shaders
// whatever the key says, so a switch turned on in a running session patches from the next flare draw on.
//
// Cost with the key on: per draw one relaxed load of the bind seam's stamp; for a flare draw, two copies of a buffer of at most 128 bytes
// and one dispatch of one group (or the veil's one target measure and its skip); for every other draw of the frame nothing.
namespace ext::ffxiv::flare
{
using Microsoft::WRL::ComPtr;

namespace
{
// The stamp this module leaves on a flare pixel shader object: this session's record index, so a find can refuse a recycled address by the
// object's own answer instead of trusting a record the address still sits in (the engine module's own index carries the ABI of this rule;
// the record comment there states the publication-order argument the same way this one needs).
constexpr GUID kFlareStampTag { 0x5bf1a0b4, 0x7d63, 0x4f0c, { 0x99, 0xab, 0x03, 0x3f, 0x4c, 0x90, 0x18, 0xd9 } };

// The module's own index of the family's shaders: the slot count a bind walks with the same probe length the engine's own table uses, and
// a cap far above the family's handful of objects (four names, a few blobs, however many sessions of object creation). A table that still
// ran out of probe room fails open with one line, exactly the engine's own rule.
inline constexpr uint32_t kFlareSeenSlots = 4096;
inline constexpr uint32_t kFlareMaxShaders = 512;
inline constexpr uint32_t kDeclineCount = 5; // None .. Exception (FlareRules.h's enum covers all five)

static_assert((kFlareSeenSlots & (kFlareSeenSlots - 1)) == 0, "the slot count is a power of two, so the hash's low bits are the slot");
static_assert(kFlareSeenSlots >= 2 * kFlareMaxShaders, "the stamp index leaves the table at most half full at the cap");

std::atomic<engine::SeenTable<kFlareSeenSlots>*> g_seenTable { nullptr };

engine::SeenTable<kFlareSeenSlots>* SeenIfAny() noexcept { return g_seenTable.load(std::memory_order_acquire); }

engine::SeenTable<kFlareSeenSlots>& SeenForInsert()
{
    engine::SeenTable<kFlareSeenSlots>* table = g_seenTable.load(std::memory_order_acquire);
    if (table == nullptr)
    {
        table = new engine::SeenTable<kFlareSeenSlots>; // never destroyed: no destructor may run under the loader lock
        g_seenTable.store(table, std::memory_order_release);
    }
    return *table;
}

struct StampRegister
{
    std::mutex mutex; // one stamp and one table insert per shader object, whatever loader thread the creation runs on
    uint32_t assigned = 0; // the stamps handed out (far above the family's own count, and the cap rule lives beside it)
};

StampRegister& I()
{
    static StampRegister* registry = new StampRegister; // never destroyed: no destructor may run under the loader lock
    return *registry;
}

// Whether a new shader object may take a stamp: the cap rule, the one rule the insert and a test read (the engine index's own shape).
constexpr bool StampHasRoom(uint32_t assigned) noexcept { return assigned < kFlareMaxShaders; }

// The stamp the shader object itself carries, 0 when it has none. A find asks it only on a pointer hit, so a bind of a shader the index
// does not hold costs no D3D call.
uint32_t StampedStampOf(const void* shader)
{
    if (shader == nullptr)
        return 0;
    UINT bytes = sizeof(uint32_t);
    uint32_t stamped = 0;
    if (FAILED(static_cast<ID3D11PixelShader*>(const_cast<void*>(shader))->GetPrivateData(kFlareStampTag, &bytes, &stamped)))
        return 0;
    return stamped;
}

// This session's state, so nothing of it is a static that a test cannot build. The counters are written by the draw path on the game's
// immediate context and read there by the card, so the plain fields need no lock; the atomics are the ones another thread publishes or
// the draw path arms.
struct State
{
    engine::CbPatch patch;     // this card's own instance, so its failure state and counters stay its own
    Session session;
    LogGate log;
    Params params;
    std::atomic<uint32_t> boundStamp { 0 }; // the bind seam's one value: the family and slot of the pixel shader the game bound last
    std::atomic<bool> keyOn { false };
    std::atomic<bool> armedFlag { false };
    bool tableFullLogged = false; // the one line the stamp index's bounded probe writes, once for the session
    bool loggedOff = false;       // the one line that says the card stood itself down
    bool keysLogged = false;      // the keys line, once per arming
    bool veilSkipLogged = false;  // the one line that says why the veil's add is skipped
    bool runLogged[3] { false, false, false }; // the run line of each patch part, once per session
    uint32_t ringRuns = 0;   // ring draws patched
    uint32_t sunRuns = 0;    // sun draws patched
    uint32_t ghostRuns = 0;  // radial-blur draws patched
    uint32_t veilSkips = 0;  // veil adds skipped
    uint32_t passes = 0;     // flare draws the card saw and left as the game drew them (no change asked)
    uint32_t declined = 0;   // draws the card asked for and was refused
    uint32_t declines[kDeclineCount] {}; // one count per Decline (None stays 0); a capture and the log read the same table
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// One counted refusal: the streak, the per-reason table, and the line the reason writes the first time. The game's draw always runs the
// way the caller decides - on the refusal paths it runs after this call, so the draw is never left out (one game draw per game draw,
// whatever fails around it, the seam's own rule).
void NoteDecline(State& state, Decline reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Flare, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    ++state.declined;
    if (uint32_t(reason) < kDeclineCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", FormatDecline(reason));
    if (state.session.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        state.armedFlag.store(false, std::memory_order_relaxed); // the card answered nothing more: the seam's costs are back to the two loads
        LOG_WARN("{}", FormatStoodDown());
    }
}

// The tuning keys of this session, read on every config load and after every menu change, so the render thread never touches the config.
// The clamps of FlareRules.h are the horse-doors: a NaN reads as the engine's own value, a range beyond its ends as the end.
Params ParamsFromCfg()
{
    auto& cfg = ext::Cfg().ffxiv;
    return flare::BuildParams(cfg.flareRingScale.value_or_default(), cfg.flareSunScale.value_or_default(),
                              cfg.flareGhostScale.value_or_default(), int32_t(cfg.flareParts.value_or_default()));
}

// Whether the draw path wants the seam: the key is on, a part is off the engine's own picture, and the session has not stood down.
bool Wanted(bool keyOn, const Params& params, const Session& session) noexcept
{
    return keyOn && AnyFlareChange(params) && session.Armed();
}

// The glare buffer and its size, for the veil's draw-time split from the exposure-meter readers that share its reflection. The view's
// reference is given back here, whether or not the target is a kind this reader reads.
VeilFacts VeilTarget(ID3D11DeviceContext* context)
{
    VeilFacts facts;
    ID3D11RenderTargetView* view = nullptr;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (view != nullptr)
    {
        ComPtr<ID3D11RenderTargetView> held;
        held.Attach(view); // the query's reference, owned here
        ComPtr<ID3D11Resource> resource;
        held->GetResource(&resource);
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
    return facts;
}

// One patch that ran: the family's own counter, the session, and the line its first run writes. `index` selects the family's run flag.
void NoteRun(State& state, Family family, uint32_t index, float scale, uint32_t slot, uint32_t byteWidth)
{
    // The card patches the engine's own flare constants and makes no picture of its own: the capture frame gets its status line only.
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Flare, ext::ffxiv::diag::StageOutcome::Ran);
    if (family == Family::Ring)
        ++state.ringRuns;
    else if (family == Family::Sun)
        ++state.sunRuns;
    else
        ++state.ghostRuns;
    state.session.NoteRun();
    if (index < 3 && !state.runLogged[index])
    {
        state.runLogged[index] = true;
        LOG_INFO("{}", FormatRun(family, scale, slot, byteWidth, state.patch.HasGpuMilliseconds(), state.patch.GpuMilliseconds()));
    }
}
} // namespace

void NoteShader(void* shaderVoid, const void* code, size_t size)
{
    auto* shader = static_cast<ID3D11PixelShader*>(shaderVoid);
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    if (!MentionsFlareName(code, size))
        return; // most of the game's shaders, on a byte scan alone: not a draw this card can patch
    try
    {
        const FlareLayout layout = LayoutOfShader(code, size);
        const uint32_t stamp = StampOfLayout(layout);
        if (stamp == 0)
            return; // a layout that names two of the three buffers, or an sToneMap beside one: not a draw this card answers
        bool full = false;
        {
            // One stamp and one table insert per shader object, under the register's lock. The id is this session's own count, so a
            // find can refuse a recycled address by the object's own answer instead of trusting a record the address still sits in.
            std::lock_guard lock(I().mutex);
            if (!StampHasRoom(I().assigned))
            {
                // The cap is far above the family's own count: the object's draws fail open exactly the way the engine's cap's does,
                // with one line for the session and the draws left as the game made them.
                if (!S().tableFullLogged)
                {
                    S().tableFullLogged = true;
                    LOG_WARN("Lens flare: the stamp index is at its cap, so a shader created from here takes no stamp and its "
                             "draws are left alone");
                }
                return;
            }
            const uint32_t id = ++I().assigned;
            if (SUCCEEDED(shader->SetPrivateData(kFlareStampTag, sizeof id, &id)))
                full = SeenForInsert().Insert(reinterpret_cast<uintptr_t>(shader), id, stamp) == engine::SeenInsert::Full;
        }
        if (full && !S().tableFullLogged)
        {
            S().tableFullLogged = true;
            LOG_WARN("Lens flare: the stamp index ran out of probe room, so one shader's draws are left alone");
        }
    }
    catch (...)
    {
    }
}

void OnPixelShaderSet(const void* shader, uint32_t instances)
{
    State& state = S();
    // A class-instance override is never one of these draws, and a null shader binds nothing: both answer 0 until the next plain bind.
    if (instances != 0 || shader == nullptr)
    {
        state.boundStamp.store(0, std::memory_order_relaxed);
        return;
    }
    engine::SeenTable<kFlareSeenSlots>* table = SeenIfAny();
    const engine::Anchors found =
        table != nullptr
            ? table->Find(reinterpret_cast<uintptr_t>(shader), [shader](uintptr_t) { return StampedStampOf(shader); })
            : 0;
    state.boundStamp.store(uint32_t(found), std::memory_order_relaxed);
}

bool OnSeamDraw(ID3D11DeviceContext* context, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr || context == nullptr)
        return false;
    const bool armed = state.armedFlag.load(std::memory_order_relaxed);
    const uint32_t stamp = state.boundStamp.load(std::memory_order_relaxed);
    if (!armed || stamp == 0)
        return false; // not the card's business: one relaxed load, and the seam's other branches answer the draw
    const Family family = FamilyOf(stamp);
    if (family == Family::None)
        return false;
    // A replay of a game draw keeps every byte of the game's own picture: the card never patches and never skips there.
    if (ext::ffxiv::chara::t_replay)
        return false;
    const Params params = state.params;

    // ---- the veil: the add is skipped (its colour and bias live in its four vertices' data, which no patch can reach), or the game's
    // own draw is left to the seam below.
    if (family == Family::Veil)
    {
        const Verdict verdict = VeilVerdict(params, VeilTarget(context));
        if (verdict.kind != VerdictKind::Skip)
        {
            ++state.passes;
            return false; // the veil is asked on, or the target is an exposure-meter reader's, not a veil at all
        }
        ++state.veilSkips;
        if (!state.veilSkipLogged)
        {
            state.veilSkipLogged = true;
            LOG_INFO("{}", FormatVeilSkip());
        }
        return true; // the draw is not made: its add never reaches the glare buffer
    }

    // ---- the three patch families: the ring, the sun sprite, one ghost pass
    const Verdict verdict = family == Family::Ring ? RingVerdict(params)
                          : family == Family::Sun ? SunVerdict(params)
                                                  : GhostVerdict(params);
    if (verdict.kind != VerdictKind::Patch)
    {
        ++state.passes;
        return false; // the game's own call, without a refusal: the scale is the engine's own value, and the orbs ride the sun's patch
    }
    const uint32_t slot = StampSlotOf(stamp);
    if (slot >= engine::kConstantBufferSlots)
        return false; // a slot the draw could never have one of the game's buffers bound at (the stamp's own bits)

    bool drew = false;
    try
    {
        ComPtr<ID3D11Buffer> bound;
        {
            ID3D11Buffer* buffers[1] = { nullptr };
            context->PSGetConstantBuffers(slot, 1, buffers);
            if (buffers[0] != nullptr)
                bound.Attach(buffers[0]); // the query's reference, owned here so every path below gives it back
        }
        if (!bound)
        {
            NoteDecline(state, Decline::NoBuffer);
            draw(user);
            return true;
        }
        D3D11_BUFFER_DESC desc {};
        bound->GetDesc(&desc);
        if (!IsBufferSize(family, desc.ByteWidth))
        {
            // A different size under the name is the engine's pool saying the draw is not this one any more - counted, one line, and
            // the card stands itself down if the pool keeps saying it (the fail-open this card wants when the pool changes).
            NoteDecline(state, Decline::BufferSize);
            draw(user);
            return true;
        }
        const ext::ffxiv::engine::ScaleEntry entry = EntryOf(family, verdict.scale);
        // The entry fits the bound table the draw is given, and the shader's own declared table (the proposal's pin) - the run is
        // refused with a line otherwise, the way the primitive hands a wrong request back.
        if (!ext::ffxiv::engine::EntryFits(entry, desc.ByteWidth) || !ext::ffxiv::engine::EntryFits(entry, DeclaredBytesOf(family)))
        {
            NoteDecline(state, Decline::PatchFailed);
            draw(user);
            return true;
        }
        ID3D11Buffer* patched = nullptr;
        {
            ext::ffxiv::chara::ReplayScope own; // the preview's hooks must not count the patch's copies as the game's
            engine::OwnWork work;               // the seam's own GPU work: never classified, never a bind the pixel-shader seam stores
            patched = state.patch.Scale(context, bound.Get(), desc.ByteWidth, &entry, 1);
        }
        if (patched == nullptr)
        {
            NoteDecline(state, Decline::PatchFailed);
            draw(user);
            return true;
        }
        {
            // The patched buffer is bound for this one draw and the game's own buffer put back for the next, also when the draw
            // throws: a swap of one slot, exactly the wetness arm's shape.
            engine::BoundConstantBuffer swap(context, slot, patched);
            if (!swap.Bound())
            {
                NoteDecline(state, Decline::PatchFailed);
                draw(user);
                return true;
            }
            drew = true;
            draw(user);
        }
        NoteRun(state, family, family == Family::Ring ? 0u : family == Family::Sun ? 1u : 2u, verdict.scale, slot, desc.ByteWidth);
        return true;
    }
    catch (...)
    {
        NoteDecline(state, Decline::Exception);
        if (!drew)
            draw(user); // a draw that already ran is never run again, whatever failed around it
        return true;
    }
}

bool Armed()
{
    State& state = S();
    return state.armedFlag.load(std::memory_order_relaxed);
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    const bool on = state.keyOn.load(std::memory_order_relaxed);
    status.enabled = on;
    status.armed = state.armedFlag.load(std::memory_order_relaxed);
    status.ring = state.params.ring;
    status.sun = state.params.sun;
    status.ghost = state.params.ghost;
    status.parts = state.params.parts;
    status.ringRuns = state.ringRuns;
    status.sunRuns = state.sunRuns;
    status.ghostRuns = state.ghostRuns;
    status.veilSkips = state.veilSkips;
    status.passes = state.passes;
    status.declined = state.declined;
    return status;
}

// Publishing the card's values to the render thread (the menu and the config load both reach this): the parameters, the arming, the
// feature bit, and the state the re-arm after the card stood itself down needs. Every publish forgets the bind seam's stamp, so the first
// draw after it is the game's own call until the game binds a flare shader again.
void Apply(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    state.params = ParamsFromCfg();
    state.boundStamp.store(0, std::memory_order_relaxed);
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.tableFullLogged = false;
        state.keysLogged = false;
        state.veilSkipLogged = false;
        state.runLogged[0] = state.runLogged[1] = state.runLogged[2] = false;
        state.patch.Release();
    }
    else if (state.session.off)
    {
        // The player turned the switch back on after a stand-down: the counters and the streaks start again.
        state.session = Session {};
        state.loggedOff = false;
        state.keysLogged = false;
    }
    const bool armed = Wanted(on, state.params, state.session);
    state.armedFlag.store(armed, std::memory_order_relaxed);
    ext::SetActive(ext::F::LensFlare, armed);
    if (armed && !state.keysLogged)
    {
        state.keysLogged = true;
        LOG_INFO("{}", FormatKeys(state.params));
    }
}

void Publish() { Apply(ext::Cfg().ffxiv.flareCard.value_or_default()); }

void OnConfigLoaded(bool on) { Apply(on); }

void Release()
{
    State& state = S();
    state.boundStamp.store(0, std::memory_order_relaxed);
    state.patch.Release(); // the card's own buffers go back while the device is still alive
}

// The key reaches the module through the config's own hook pointer (ext::FfxivConfig::lensFlareOnLoaded), so ExtConfig.cpp keeps no link
// dependency on this file. A config load may run inside DllMain, so a publish can run on another thread than the render thread: the patch
// primitive serialises its own entries, the draw path only reads atomics, and the card's log is its own.
struct RegisterFlareKey
{
    RegisterFlareKey()
    {
        ext::FfxivConfig::lensFlareOnLoaded = &ext::ffxiv::flare::OnConfigLoaded;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Flare, &ext::ffxiv::flare::Armed, nullptr);
    }
} g_registerFlareKey;
} // namespace ext::ffxiv::flare
