#include "pch.h"

#include "ext/ffxiv/lighting/CharaAmbientApply.h"

#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the replay marker: a replay of a game draw is never a draw of this module's own
#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/engine/CbPatch.h" // the shared patch primitive and the bind guard
#include "ext/ffxiv/engine/EnginePass.h" // the seen table's shape, the decline-count convention it shares
#include "ext/ffxiv/engine/EnginePassRules.h" // kConstantBufferSlots, the shared LogGate
#include "ext/ffxiv/engine/FogSun.h"         // the visible sun's elevation (the night curve's first input), asked for as this module's own consumer
#include "ext/ffxiv/enginelight/ActiveSwap.h" // the night feed reads the engine's light buffer, not the engine light controls' patched copy
#include "ext/ffxiv/relight/Inject.h" // the contact path's own sun-light-draw name test (LayoutOfShader)
#include "ext/present/PresentSeams.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>

// The device side of the chara ambient: the stamp, the light-colour ring, and the one draw-path patch. Nothing of the game is ever written and
// no game buffer is ever mapped: the patch reads the game's bytes only through CopyResource, and the ring's staging copy is this module's own
// buffer read back with a no-wait map. A failed step leaves the game's draw exactly as it was, with the game's own buffer still bound.
namespace ext::ffxiv::lighting::chara
{
using Microsoft::WRL::ComPtr;

namespace
{
// The light bytes the curve's capture copies: g_LightParam's first 0x30 bytes carry the position, the direction and the diffuse colour (bytes
// 32..43 are the colour the curve keys on), the same range the shadow mapping module's ring copies.
constexpr uint32_t kLightColourBytes = 0x30;
constexpr uint32_t kRingDepth = 4; // the same depth the other readback rings keep: a GPU a frame or two behind reads later

// The one private-data tag this module stamps a shader with: the id the seen table keyed it on. The game has its own tags on its own shaders,
// and a GUID this specific does not collide with them.
constexpr GUID kStampTag { 0x7c9e41f2, 0x63b8, 0x4d75, { 0x92, 0xaf, 0x0d, 0x51, 0xe6, 0x3b, 0x87, 0x44 } };

// One session's state, so nothing of it is a static a test cannot build. All of it is written by the draw path's single thread (the game's
// render thread); a config load publishes through the atomics and the flags below, and the patch primitive serialises its own entries.
struct State
{
    ext::ffxiv::engine::CbPatch patch;
    ext::ffxiv::engine::SeenTable<1024> seen;

    // The bound shader's pack (CharaAmbientRules.h's PackOf), from the seen table at each pixel-shader bind. It carries the two slots the
    // curve and the knob need:
    //   bits 0..3  the chara ambient block's slot + 1 (0 = not a character draw)
    //   bits 4..7  the sun light draw's g_LightParam slot + 1 (0 = not the sun light draw)
    std::atomic<uint32_t> boundPack { 0 };
    std::atomic<uint32_t> nextStampId { 0 };

    ext::ffxiv::engine::LogGate log;
    Session session; // the wave's stand-down, this knob's own streak

    // The keys as the last config load or menu change published them, so the render thread never touches the config. The lift rides a 1/1024
    // fixed-point atomic, so the draw path reads it without a float atomic.
    std::atomic<bool> keyOn { false };
    std::atomic<uint32_t> liftKey { 1024 }; // 1.000

    // The per-frame cache of the patched block, and the epoch that invalidates it (the knob, the master and the patch primitive's generation).
    CharaFrameCache cache;
    std::atomic<uint32_t> cacheEpoch { 0 };

    // The curve's capture ring: the frame's own light colour copied at the draw that binds g_LightParam, read no-wait a frame or two later.
    ComPtr<ID3D11Buffer> staging[kRingDepth];
    uint64_t staged[kRingDepth] {};
    float lastColour[3] { 0.0f, 0.0f, 0.0f };
    bool haveRead = false;      // a staging copy has landed at least once
    // The visible sun (2026-11-07 integration): the fog sun's publishes aged on this module's own clock, one tick per frame with character draws
    // (the ring's once-a-frame read), not the present counter that frame generation steps two to four times per game frame.
    shadows::FogSunTracker fogTracker;
    uint64_t nightClock = 0;
    bool waitingLogged = false; // the one line for an armed module whose ring has not landed yet
    bool budgetLogged = false;
    bool loggedOff = false;

    // The counters the card and a capture read.
    uint32_t runs = 0;
    uint32_t served = 0;
    uint32_t declined = 0;
    bool haveRun = false;
    float lastCurve = 0.0f;
    float lastScale = 1.0f;
    uint32_t lastSlot = 0;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// ---- the stamp (the seen table's answer, asked only when a record matches) -------------------------------------------------

// The shared seen-table contract, this module's own tag: SetPrivateData is the only thing this module remembers about a shader, so a recycled
// address is caught by the object's own answer and never trusted.
bool StampShader(ID3D11DeviceChild* shader, uint32_t id)
{
    if (shader == nullptr || id == 0)
        return false; // 0 is "never stamped": writing it would make an unstamped shader indistinguishable
    return SUCCEEDED(shader->SetPrivateData(kStampTag, sizeof id, &id));
}

uint32_t ReadStamp(ID3D11DeviceChild* shader)
{
    if (shader == nullptr)
        return 0;
    uint32_t id = 0;
    UINT bytes = sizeof id;
    if (FAILED(shader->GetPrivateData(kStampTag, &bytes, &id)) || bytes != sizeof id)
        return 0;
    return id;
}

// Whether a bytecode mentions a name this module stamps on: the cheap test that drops the shaders that can neither be a chara draw nor the
// sun light draw before the reflection parse. Deliberately a plain byte scan, like the shared index's.
bool Mentions(const void* code, size_t size, std::string_view name) noexcept
{
    if (code == nullptr || size < name.size())
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i + name.size() <= size; ++i)
        if (bytes[i] == static_cast<unsigned char>(name[0]) && std::memcmp(bytes + i, name.data(), name.size()) == 0)
            return true;
    return false;
}

// The two slots one shader declares, from the same reflection the diag reads for every other pass.
//  - the character family's ambient block: the shader declares `g_AmbientParam` AND `g_CustomizeParameter` (the world's materials declare the
//    ambient block alone). The names are compared by the reflected variable name and the binding kind asked for, so the composite's
//    g_AmbientParamArray can never answer for the chara block, and a sampler can never answer for a buffer;
//  - the sun light draw's `g_LightParam`: the contact path's own name test (the sun mask under either spelling, the depth, the light parameters
//    and the camera, plus the directional light's g_DirectionalShadowParameter - IsSunLightLayout). The frame's other `g_LightParam` draws (the rain-exposure volumes of the wetness mask, which are
//    the first four light draws of a frame, and the local lights) are not the colour the curve keys on.
struct Slots
{
    uint32_t charaBlock = kNoSlot;
    uint32_t lightParam = kNoSlot;
};

Slots SlotsOfShader(const void* code, size_t size, bool maybeChara, bool maybeSun)
{
    Slots slots;
    if (maybeChara)
    {
        ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
        const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
        if (count != 0)
        {
            const uint32_t ambient = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "g_AmbientParam");
            const uint32_t customize =
                ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kCharaCustomizeName);
            if (IsCharaShader(ambient != ext::ffxiv::diag::kNoBindPoint, customize != ext::ffxiv::diag::kNoBindPoint))
                slots.charaBlock = ambient;
        }
    }
    if (maybeSun)
    {
        const ext::ffxiv::relight::Layout sun = ext::ffxiv::relight::LayoutOfShader(code, size);
        if (ext::ffxiv::relight::IsSunLightLayout(sun)) // the four names AND the directional cascade parameters: not a point/spot light
            slots.lightParam = sun.lightParamSlot;
    }
    return slots;
}

// ---- the capture ring ------------------------------------------------------------------------------------------------------

// The frame's own light colour, copied here at the draw that binds g_LightParam (staged once per frame per slot, the newest-lit rule the other
// rings keep). Nothing of the game is ever written, and the staging pair's owner is checked: a pair the previous device created cannot receive
// this context's copy (the other readback rings' rule), so it is rebuilt on the spot.
void CaptureLightColour(ID3D11DeviceContext* context, ID3D11Buffer* lightParam, uint64_t frame, State& state)
{
    if (context == nullptr || lightParam == nullptr)
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
        return; // a build whose buffer is smaller than the members the curve keys on: nothing to read, no patch
    const uint32_t slot = uint32_t(frame % kRingDepth);
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
            return; // the capture is never a gate: the curve keeps its last read's colour
        state.staging[slot] = created;
    }
    D3D11_BOX box {};
    box.right = kLightColourBytes;
    box.bottom = 1;
    box.back = 1;
    context->CopySubresourceRegion(state.staging[slot].Get(), 0, 0, 0, 0, lightParam, 0, &box);
    state.staged[slot] = frame;
}

// The curve's newest landed colour: the ring's slot for `frame - age` is mapped no-wait, the newest one that lands is taken, and the last good
// colour keeps serving. A map that has not landed is skipped this ask (a waited map on the game's own context is a frame stall the session
// reads as a lag).
void ReadRing(ID3D11DeviceContext* context, State& state, uint64_t frame)
{
    if (context == nullptr || frame == 0)
        return;
    for (uint32_t age = 1; age < kRingDepth; ++age)
    {
        if (frame < age)
            break;
        const uint64_t candidate = frame - age;
        const uint32_t slot = uint32_t(candidate % kRingDepth);
        if (!state.staging[slot] || state.staged[slot] != candidate)
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (context->Map(state.staging[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK ||
            mapped.pData == nullptr)
            continue; // the copy has not landed: skip it, the last good colour keeps serving
        std::memcpy(state.lastColour, static_cast<const unsigned char*>(mapped.pData) + 0x20, sizeof(float) * 3);
        context->Unmap(state.staging[slot].Get(), 0);
        state.haveRead = true;
        break;
    }
    if (!state.haveRead && !state.waitingLogged)
    {
        state.waitingLogged = true;
        LOG_INFO("{}", FormatWaitingForColour());
    }
}

// The cache's epoch, the wetness pattern: the knob and the master publish a number when they change, and the patch primitive counts its own
// re-initialisation (a device change, or a size cache eviction that frees the buffer a cached entry points at). Both numbers only grow, so a
// change of either is a new epoch - and folding them here keeps every write of the cache itself on the draw thread.
uint32_t CacheEpoch(State& state)
{
    return state.cacheEpoch.load(std::memory_order_relaxed) + state.patch.Generation();
}

// One declined draw: counted, logged once per reason, and moved into the stand-down streak. Returns true when this decline is the one that
// switched the module off, so the caller writes the one line that says so.
bool DeclineDraw(State& state, Decline reason)
{
    state.declined++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", FormatDecline(reason));
    const bool wasOff = state.session.off;
    const bool switched = wasOff ? false : state.session.NoteDecline();
    if (switched && !state.loggedOff)
    {
        state.loggedOff = true;
        return true;
    }
    return false;
}

float LiftKeyOf(State& state)
{
    return float(state.liftKey.load(std::memory_order_relaxed)) / 1024.0f;
}

// The log tag of a run, so a real change writes a line and a wander inside the same step does not: the curve in 1/64 steps, the scale in 1/64
// steps, and the slot the block sat at.
uint32_t RunTag(uint32_t slot, float scale, float curve) noexcept
{
    const uint32_t scalePart = (uint32_t)(scale * 64.0f) & 0x3Fu;
    const uint32_t curvePart = (uint32_t)(curve * 64.0f) & 0x3Fu;
    return (scalePart << 12) | (curvePart << 6) | (slot & 0xFu);
}
} // namespace

void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc)
{
    (void)crc;
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    // The composite's g_AmbientParamArray carries the block's name as a byte string, so the scan asks for the shorter name and the reflection's
    // own comparison keeps the two apart: the scan costs a pass over the bytecode per name, the parse only for a shader that mentions them all.
    const bool maybeChara = Mentions(code, size, "g_AmbientParam") && Mentions(code, size, kCharaCustomizeName);
    const bool maybeSun = ext::ffxiv::relight::MentionsMaskName(code, size) && Mentions(code, size, "g_LightParam");
    if (!maybeChara && !maybeSun)
        return;
    try
    {
        const Slots slots = SlotsOfShader(code, size, maybeChara, maybeSun);
        const uint32_t pack = PackOf(slots.charaBlock, slots.lightParam);
        if (pack == 0)
            return; // a shader that names the buffers but is neither a character draw nor the sun light draw at a boundable slot
        State& state = S();
        const uint32_t id = state.nextStampId.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!StampShader(shader, id))
            return; // a shader that refuses the stamp can never answer a lookup, so its draws fail open quietly
        // The shared seen-table contract: a shader whose record cannot fit fails open (its draws are passed on unfound), and one address
        // replaces its own record in place, so one address never takes two slots and a find keeps its stop-at-empty rule.
        (void)state.seen.Insert(reinterpret_cast<uintptr_t>(shader), id, pack);
    }
    catch (...)
    {
    }
}

void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances)
{
    if (!ext::Active(ext::F::CharaAmbient))
        return; // one relaxed load per bind while the key is off; nothing is tracked because nothing reads it
    State& state = S();
    if (instances == 0 || shader == nullptr)
    {
        state.boundPack.store(0, std::memory_order_relaxed);
        return;
    }
    // The bound shader's pack, answered from the seen table: one table lookup per bind, and the object's own stamp read only when a record
    // matches, so a recycled address is caught by the object's own answer and never trusted (and the game's ~6,300 other binds a frame cost
    // one table probe, no COM call).
    const auto pack = static_cast<uint32_t>(state.seen.Find(reinterpret_cast<uintptr_t>(shader),
                                                            [shader](unsigned long long) -> uint32_t { return ReadStamp(shader); }));
    state.boundPack.store(pack, std::memory_order_relaxed);
}

void OnGameDraw(ID3D11DeviceContext* context, void (*draw)(void*), void* user)
{
    State& state = S();
    if (draw == nullptr)
        return;
    if (context == nullptr || ext::ffxiv::chara::t_replay)
    {
        // A draw on another context, or a replay of a game draw (the scene HDR path, the character preview): the game's own call, counted as
        // nothing at all - the seam's replay rule every other draw-path module keeps.
        draw(user);
        return;
    }
    const uint32_t pack = state.boundPack.load(std::memory_order_relaxed);
    const uint32_t charaSlot = CharaSlotOf(pack);
    const uint32_t lightSlot = LightSlotOf(pack);
    try
    {
        // The curve's capture: the sun light draw stages the frame's own light colour into the ring (the copy never touches the binds; the
        // map, a frame later, is what reads it, and it is no-wait, so nothing here waits on the GPU). The fetched buffer arrives with a
        // reference, which the ComPtr gives back.
        if (lightSlot != kNoSlot && lightSlot < ext::ffxiv::engine::kConstantBufferSlots)
        {
            const uint64_t frame = ext::present::FrameCount();
            const uint32_t ringSlot = uint32_t(frame % kRingDepth);
            if (state.staged[ringSlot] != frame)
            {
                ID3D11Buffer* fetched = nullptr;
                context->PSGetConstantBuffers(lightSlot, 1, &fetched);
                fetched = ext::ffxiv::enginelight::OwnedGameBuffer(fetched); // the engine's colour, not an engine light controls' patched copy
                ComPtr<ID3D11Buffer> lightParam;
                lightParam.Attach(fetched);
                if (lightParam)
                {
                    D3D11_BUFFER_DESC desc {};
                    lightParam->GetDesc(&desc);
                    if (desc.ByteWidth >= kLightColourBytes)
                        CaptureLightColour(context, lightParam.Get(), frame, state);
                }
            }
        }

        // The chara draws' own patch, keyed on the shared curve: one scale entry on the block's reciprocal scale field, one copy of the
        // game's bytes into an owned constant buffer, one dispatch, the patched copy bound for the draw and the game's buffer back afterwards.
        if (charaSlot != kNoSlot && charaSlot < ext::ffxiv::engine::kConstantBufferSlots)
        {
            ID3D11Buffer* bound = nullptr;
            context->PSGetConstantBuffers(charaSlot, 1, &bound);
            ComPtr<ID3D11Buffer> block;
            block.Attach(bound);
            if (block)
            {
                D3D11_BUFFER_DESC desc {};
                block->GetDesc(&desc);
                if (IsCharaBufferSize(desc.ByteWidth))
                {
                    // The ring read, once a frame at the frame change (the cache's own Begin answers it), never per draw.
                    const uint64_t frame = ext::present::FrameCount();
                    if (state.cache.Begin(frame, CacheEpoch(state)))
                    {
                        ReadRing(context, state, frame);
                        ++state.nightClock;
                    }

                    // The night reading: shadow mapping's own curve input (the fog sun while fresh, the light colour otherwise).
                    const fogsun::Reading fog = fogsun::Read();
                    state.fogTracker.Observe(fog.count, state.nightClock);
                    shadows::NightReading reading;
                    reading.fogFresh = fog.have && state.fogTracker.Fresh(state.nightClock);
                    reading.fogSunY = fog.y;
                    reading.haveColour = state.haveRead;
                    reading.luminance = shadows::LuminanceOf(state.lastColour);
                    reading.colourArms = false; // the sun arm alone under a fresh fog sun: this ring's draw is not proven to be the main view's (NightReading)
                    const Blend blend = FrameBlendNight(state.keyOn.load(std::memory_order_relaxed), LiftKeyOf(state), reading);
                    state.lastCurve = blend.curve;
                    state.lastScale = blend.scale;
                    state.lastSlot = charaSlot;

                    if (!blend.runs)
                    {
                        // The identity in the chain (the master off, no reading yet, or a day frame's curve): the game's own call, counted as
                        // nothing at all. The equality gate swaps nothing at identity, and the frame cache's budget stays untouched for it.
                        draw(user);
                        return;
                    }

                    ext::ffxiv::engine::ScaleEntry entries[1] { CharaRecipEntry(blend.scale) };
                    if (!EntryFitsChara(entries[0]))
                    {
                        if (DeclineDraw(state, Decline::PatchFailed))
                            LOG_WARN("{}", FormatSwitchedOff());
                        draw(user); // the game's own call, the same fail-open every other patch keeps
                        return;
                    }

                    const bool cached = state.cache.Has(block.Get());
                    if (cached)
                    {
                        // The cached copy is read before Serve(): a bind that reaches the expiry must bind the copy the cache was holding
                        // instead of a null.
                        ID3D11Buffer* cachedCopy = reinterpret_cast<ID3D11Buffer*>(const_cast<void*>(state.cache.Dst()));
                        state.cache.Serve();
                        state.served++;
                        state.session.NoteRun();
                        ext::ffxiv::engine::BoundConstantBuffer swap(context, charaSlot, cachedCopy);
                        if (!swap.Bound())
                        {
                            if (DeclineDraw(state, Decline::PatchFailed))
                                LOG_WARN("{}", FormatSwitchedOff());
                            draw(user);
                            return;
                        }
                        draw(user);
                        return;
                    }
                    if (state.cache.MayPatch())
                    {
                        ID3D11Buffer* patched = nullptr;
                        {
                            // The character preview's marker is raised around the work, so the preview layer's copy hooks do not count this
                            // module's copy of the game's block as the game's.
                            const ext::ffxiv::chara::ReplayScope own;
                            patched = state.patch.Scale(context, block.Get(), desc.ByteWidth, entries, 1);
                        }
                        if (patched == nullptr)
                        {
                            if (DeclineDraw(state, Decline::PatchFailed))
                                LOG_WARN("{}", FormatSwitchedOff());
                            draw(user);
                            return;
                        }
                        state.cache.Record(block.Get(), patched);
                        state.runs++;
                        state.haveRun = true;
                        state.session.NoteRun();
                        if (state.log.Take(uint32_t(RunTag(charaSlot, blend.scale, blend.curve)) | 0x80000000u))
                            LOG_INFO("{}", FormatRun(blend.scale, blend.curve, charaSlot, desc.ByteWidth,
                                                     state.patch.HasGpuMilliseconds(), state.patch.GpuMilliseconds()));
                        ext::ffxiv::engine::BoundConstantBuffer swap(context, charaSlot, patched);
                        if (!swap.Bound())
                        {
                            if (DeclineDraw(state, Decline::PatchFailed))
                                LOG_WARN("{}", FormatSwitchedOff());
                            draw(user);
                            return;
                        }
                        draw(user);
                        return;
                    }

                    // A later distinct pointer of the frame, past the budget: passed through, and the one line that says so. The budget is a
                    // real-world state a frame of many characters reaches, not a refusal the streak should ever be driven by, so it counts no
                    // stand-down.
                    if (!state.budgetLogged)
                    {
                        state.budgetLogged = true;
                        LOG_INFO("{}", FormatBudgetSpent(kMaxPatchesPerFrame));
                    }
                    draw(user);
                    return;
                }
                // A buffer of another width under the name is another draw's table: passed through without a refusal, like the wetness knob's
                // rule. The name carries no shape rule of its own.
            }
        }
    }
    catch (...)
    {
        if (DeclineDraw(state, Decline::Exception))
            LOG_WARN("{}", FormatSwitchedOff());
    }
    draw(user);
}

// ---- the arming, the card's numbers and the release ------------------------------------------------------------------------

void Publish(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    const float lift = ClampCharaLift(ext::Cfg().ffxiv.charaAmbientLift.value_or_default());
    state.liftKey.store(uint32_t(lift * 1024.0f + 0.5f), std::memory_order_relaxed);
    // The feature bit: the draw seam is entered only while the master is on and the lift is off the engine's own value, so a master left on
    // with the lift at 1.0 costs one relaxed load per draw (and the bind hook nothing at all). The curve's identity (a day frame, or no
    // reading yet) is decided per draw: putting it in the feature bit would leave every capture the ring depends on behind.
    const bool armed = on && !IsCharaLiftIdentity(lift);
    const bool wasArmed = ext::Active(ext::F::CharaAmbient);
    // The fog-post capture is asked for while this module would patch: its curve reads the visible sun as shadow mapping's does, whether or not
    // shadow mapping is on (each consumer owns its bit of the flag).
    fogsun::SetWanted(fogsun::kCharaAmbient, armed);
    ext::SetActive(ext::F::CharaAmbient, armed);
    state.cacheEpoch.fetch_add(1, std::memory_order_relaxed); // every publish: a moved lift must not be served the previous scale's copy
    if (armed != wasArmed)
        state.boundPack.store(0, std::memory_order_relaxed); // the game re-binds its shaders many times a frame; the next bind fills the pack
    if (!on)
    {
        state.session = Session {};
        state.log = ext::ffxiv::engine::LogGate {};
        state.cache.Reset();
        state.runs = 0;
        state.served = 0;
        state.declined = 0;
        state.haveRun = false;
        state.waitingLogged = false;
        state.budgetLogged = false;
        state.loggedOff = false;
    }
}

Status CurrentStatus()
{
    State& state = S();
    const bool on = state.keyOn.load(std::memory_order_relaxed);
    const float lift = LiftKeyOf(state);
    Status status;
    status.key = on;
    status.identity = IsCharaLiftIdentity(lift); // at 1.0: today's picture
    status.off = on && !state.session.Armed();
    status.armed = on && !IsCharaLiftIdentity(lift) && state.session.Armed();
    status.lift = lift;
    status.haveReading = state.haveRead;
    status.curve = state.lastCurve;
    status.lastScale = state.lastScale;
    status.runs = state.runs;
    status.served = state.served;
    status.declined = state.declined;
    return status;
}

bool HaveColourReading() { return S().haveRead; }

float LastCurve() { return S().lastCurve; }

void Release()
{
    State& state = S();
    state.boundPack.store(0, std::memory_order_relaxed);
    state.patch.Release(); // the buffer the cache points at goes with it; the epoch the next draw reads includes the primitive's generation
    for (ComPtr<ID3D11Buffer>& buffer : state.staging)
        buffer.Reset();
    for (uint64_t& at : state.staged)
        at = 0;
    state.cache.Reset();
}
} // namespace ext::ffxiv::lighting::chara

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run (the engine pass overrides' pattern).
struct RegisterCharaAmbient
{
    RegisterCharaAmbient() { ext::FfxivConfig::charaAmbientOnLoaded = &ext::ffxiv::lighting::chara::Publish; }
} g_registerCharaAmbient;
} // namespace
