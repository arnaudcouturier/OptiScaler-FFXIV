#include "pch.h"

#include "ext/ffxiv/relight/Inject.h"

#include "ext/core/ExtConfig.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>
#include <unordered_map>
#include <vector>

// Contact shadows before the sun light pass: the reflection index and the config side (ext/ffxiv/relight/Inject.h). The device side -
// reading the draw's state, running the pass, binding and restoring the mask - is ContactShadowsApply.cpp; the rules are in
// ContactShadowsRules.h; the shaders in shaders/ContactShadows.hlsl.
//
// When the index is filled: while the game creates its shaders, whatever the key says. A cheap byte test for the mask's name drops the
// shaders that cannot be a sun light draw before the reflection parse, so the index costs a scan of each pixel shader's bytecode and is
// there when a switch is turned on in a running session (the game creates its light shaders while it loads, long before the player can
// reach the card). Nothing is logged while the key is off, so a default-off install writes no line about this feature; while it is on, at
// most one line per session says which shader was indexed (and one more if a shader declares the mask but not the other three names).
//
// Cost with the key off: the seam's per-bind call in ext/ffxiv/lighting/FfxivLightingCapture.h (one out-of-line call and one relaxed load,
// about a third of the PSSetShader call it sits behind, which the game makes ~0.7 times per draw) and nothing on the draw path but the
// hook's own one relaxed load. Measured on an idle 3.3 GHz machine: 1.0 ns per bind, so 0.007 ms a frame at 6,300 binds. At shader creation:
// a linear byte scan of every pixel shader's bytecode, 0.43 ns per byte (1.8 us for a 4 KB shader; 33,000 shaders at that size are 58 ms
// over a whole session's loading), and, for the few that name the mask, one RDEF parse and a 4-byte stamp.
//
// With the key on: per draw one relaxed load, and only for a draw whose bound shader is indexed the refusal lookup (the clock is read for
// that lookup only when a shape refusal is pending) and the 24-byte layout copy; measured at 0.9 ns per draw for the common case, 0.008 ms
// at 9,250 draws a frame. Per sun light draw (two a frame) two full-screen draws, one at half and one at full resolution, both in this
// module's own device-context state.
//
// Threads: shader creation may run on any thread and only appends to the index under its mutex; the draw path runs on the render thread.
// No D3D call is made while the mutex is held, and the draw path works on a copy of the 24-byte layout, never on a pointer into the vector
// a shader creation could grow.
namespace ext::ffxiv::relight
{
namespace
{
using Microsoft::WRL::ComPtr;

// Stamped on a shader object that is in the index: the layout's index plus one (an unstamped object reads 0 = not in the index).
constexpr GUID kLayoutTag { 0x6b1e2d44, 0x51f7, 0x4bb0, { 0x9d, 0x2a, 0x70, 0xc4, 0x18, 0x3f, 0x60, 0x2b } };

constexpr uint32_t kMaxIndexedShaders = 20000; // the game creates about 33k pixel shaders a session

// The seam's pointer table: the indexed shaders' objects, so a bind costs a hash, a pointer compare and (on a hit) one stamp check -
// rather than a GetPrivateData on every bind, which would move the old per-draw cost to the (fewer, but still thousands a frame) binds.
// Only a sun light draw's shader is inserted, so the table stays tiny; a collision costs a missed classification, which fails open, and a
// recycled address is caught by the stamp check on the hit.
constexpr uint32_t kSeenSlots = 1024;
struct SeenShader
{
    std::atomic<ID3D11PixelShader*> shader { nullptr };
    std::atomic<uint32_t> id { 0 };
};
SeenShader g_seen[kSeenSlots];

uint32_t SeenSlotOf(const void* shader) { return (uint32_t((uintptr_t(shader) >> 4) * 2654435761u)) & (kSeenSlots - 1); }

void RememberIndexed(ID3D11PixelShader* shader, uint32_t id)
{
    if (shader == nullptr || id == 0)
        return;
    SeenShader& slot = g_seen[SeenSlotOf(shader)];
    slot.id.store(id, std::memory_order_relaxed);
    slot.shader.store(shader, std::memory_order_relaxed);
}

uint32_t TrackedIdOf(ID3D11PixelShader* shader)
{
    if (shader == nullptr)
        return 0;
    const SeenShader& slot = g_seen[SeenSlotOf(shader)];
    if (slot.shader.load(std::memory_order_relaxed) != shader)
        return 0; // not a shader the index knows: the common case, and no D3D call
    const uint32_t id = slot.id.load(std::memory_order_relaxed);
    UINT bytes = sizeof id;
    uint32_t stamped = 0;
    if (id == 0 || FAILED(shader->GetPrivateData(kLayoutTag, &bytes, &stamped)) || stamped != id)
        return 0; // a recycled address: the stamp is the object's own answer, and it disagrees
    return id;
}

// The on-cost meter: after kCostEveryDraws draws while the key is on, the next kCostSampleDraws draws have their pixel-shader lookup
// bracketed with QueryPerformanceCounter and one line reports the average (ContactShadowsRules.h has the meter and its text).
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
    std::vector<Layout> layouts;
    std::unordered_map<uint32_t, uint32_t> byCrc; // crc -> layouts index + 1
};

Index& I()
{
    static Index* index = new Index;
    return *index;
}

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The tuning keys of this session. Read on every config load and whenever the card changes a value, so the render thread never touches the
// config for them.
void PublishParams()
{
    auto& cfg = ext::Cfg().ffxiv;
    const int32_t dump = cfg.contactShadowsDump.value_or_default();
    S().dumpDelaySeconds = dump <= 0 ? 0 : (dump > kMaxDumpDelaySeconds ? kMaxDumpDelaySeconds : dump);
    S().params = BuildParams(cfg.contactShadowsLength.value_or_default(), cfg.contactShadowsStrength.value_or_default(),
                             cfg.contactShadowsSteps.value_or_default(), ParseSunSpace(cfg.contactShadowsSunSpace.value_or_default()),
                             ParseSunSign(cfg.contactShadowsSunSign.value_or_default()));
}
} // namespace

void OnGameDraw(ID3D11DeviceContext* context, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return;
    if (context == nullptr || !state.session.Armed())
    {
        draw(user);
        return;
    }
    if (state.inPass.load(std::memory_order_relaxed))
    {
        draw(user); // this module's own pass drawing its two full-screen triangles: never classified, never a decline
        return;
    }
    ++state.drawsSeen;
    try
    {
        // The lookup is the seam's tracked layout index: one relaxed load, no state query. The seam runs once per PSSetShader, which the
        // game makes far less often than it draws (measured on the benchmark). The cost meter brackets the lookup for 64 of every 262,144
        // draws; no clock is read on the others.
        const int64_t lookupStart = state.cost.Sampling() ? Qpc() : 0;
        const uint32_t id = state.boundLayoutId.load(std::memory_order_relaxed);
        Layout layout;
        bool known = false;
        bool refused = false;
        if (id != 0)
        {
            // The clock is read only while a shape refusal is remembered: that map is empty on the normal path, which then costs the load
            // above and one branch.
            if (!state.refusedShapes.empty())
            {
                const auto refusal = state.refusedShapes.find(id);
                refused = refusal != state.refusedShapes.end() && ShapeRefusalActive(refusal->second.refusedQpc, Qpc(), QpcFrequency());
            }
            if (!refused)
            {
                std::lock_guard lock(I().mutex);
                if (size_t(id) <= I().layouts.size())
                {
                    layout = I().layouts[id - 1]; // a copy: the vector grows on other threads
                    known = true;
                }
            }
        }

        if (lookupStart != 0)
            state.cost.Sample(Qpc() - lookupStart);
        else
            state.cost.Skip();
        if (state.cost.Done() && !state.cost.logged)
        {
            state.cost.logged = true;
            LOG_INFO("{}", FormatCost(state.cost, state.drawsSeen, state.bindCost, state.bindsSeen, QpcFrequency()));
        }

        if (refused || !known || !layout.ok)
        {
            draw(user); // not a sun light draw, or one of another shape: passed through untouched
            return;
        }
        const Decline outcome = ApplyDraw(context, state, &layout, draw, user);
        if (outcome == Decline::None)
            state.refusedShapes.erase(id); // a shader that has run is never remembered as refused
        else if (IsShapeDecline(outcome))
            state.refusedShapes[id] = ShapeRefusal { Qpc() }; // a shader of another shape: looked at again when the memory expires
        if (state.session.off)
            ext::SetActive(ext::F::ContactShadows, false); // the draw path is back to one relaxed load
    }
    catch (...)
    {
        draw(user);
    }
}

void OnPixelShaderSet(ID3D11PixelShader* shader, uint32_t instances)
{
    if (!ext::Active(ext::F::ContactShadows))
        return; // one relaxed load per bind while the key is off; nothing is tracked because nothing reads it
    State& state = S();
    if (state.inPass.load(std::memory_order_relaxed))
        return; // this module's own pass: the game's binding is put back by the context-state swap, not by another PSSetShader
    ++state.bindsSeen;
    const int64_t bindStart = state.bindCost.Sampling() ? Qpc() : 0;
    const uint32_t id = instances == 0 ? TrackedIdOf(shader) : 0; // a class-instance override is never a sun light draw
    state.boundLayoutId.store(id, std::memory_order_relaxed);
    if (bindStart != 0)
        state.bindCost.Sample(Qpc() - bindStart);
    else
        state.bindCost.Skip();
}

void OnShaderCreated(ID3D11PixelShader* shader, const void* code, size_t size, uint32_t crc)
{
    if (shader == nullptr || crc == 0 || code == nullptr || size < 32)
        return;
    if (!MentionsMaskName(code, size))
        return; // most of the game's shaders, on a byte scan alone: not a sun light draw
    try
    {
        // The index is filled whatever the key says (a switch turned on in a running session must find the game's light shader), but
        // nothing is logged while the key is off: a default-off install writes no line about a feature the player never enabled.
        const bool logging = ext::Active(ext::F::ContactShadows);
        const Layout layout = LayoutOfShader(code, size);
        if (!layout.ok)
        {
            // Not a sun light draw: no stamp at all, so the draw path never looks it up. A shader that declares the mask but not all four
            // names is worth one line: it is the shape of the draw the feature is looking for, and a build whose light pass lost one of the
            // bindings would otherwise fail silently.
            if (logging && (layout.found & 1u) != 0 && !S().partialLogged)
            {
                S().partialLogged = true;
                LOG_WARN("Contact shadows: a shader declares g_SamplerShadow but not all of g_SamplerDepth, g_LightParam and g_CameraParameter "
                         "(found mask {}, depth {}, light {}, camera {}); the pass was not indexed",
                         (layout.found & 1u) != 0, (layout.found & 2u) != 0, (layout.found & 4u) != 0, (layout.found & 8u) != 0);
            }
            return;
        }
        uint32_t id = 0;
        {
            std::lock_guard lock(I().mutex);
            const auto found = I().byCrc.find(crc);
            if (found != I().byCrc.end())
                id = found->second;
            else if (I().layouts.size() < kMaxIndexedShaders)
            {
                I().layouts.push_back(layout);
                id = uint32_t(I().layouts.size());
                I().byCrc.emplace(crc, id);
            }
        }
        if (id != 0)
        {
            shader->SetPrivateData(kLayoutTag, sizeof id, &id);
            RememberIndexed(shader, id);
            if (logging && !S().indexedLogged)
            {
                S().indexedLogged = true;
                LOG_INFO("Contact shadows: sun light draw shader indexed (mask slot {}, depth slot {}, g_LightParam slot {}, g_CameraParameter "
                         "slot {})",
                         layout.maskSlot, layout.depthSlot, layout.lightParamSlot, layout.cameraParamSlot);
            }
        }
    }
    catch (...)
    {
    }
}

bool Armed()
{
    State& state = S();
    return ext::Active(ext::F::ContactShadows) && state.session.Armed();
}

Params CurrentParams() { return S().params; }

void Publish(bool on)
{
    State& state = S();
    PublishParams();
    if (!on)
    {
        state.session = Session {};
        state.haveSun = false;
        state.lastCameraCheckQpc = 0;
        state.loggedOff = false;
        state.refusedShapes.clear();
        state.pass.Reset();
        state.boundLayoutId.store(0, std::memory_order_relaxed);
        ext::SetActive(ext::F::ContactShadows, false);
        return;
    }
    // Turning the switch back on after the module switched itself off is the player's re-arm: the counters start again and the draw path
    // measures the intent again. The index is not touched: it is filled whatever the key says, so the shaders the game created while the
    // key was off are already in it.
    if (state.session.off)
        state.session = Session {};
    if (state.dumpStartQpc == 0)
        state.dumpStartQpc = Qpc(); // the delay of the dump counts from the config load
    state.indexedLogged = false; // a fresh session says once which shader it indexed, and once if one had the wrong shape
    state.partialLogged = false;
    ext::SetActive(ext::F::ContactShadows, true);
}

void Reset()
{
    State& state = S();
    state.pass.Reset();
    state.session = Session {};
    state.log = LogGate {};
    state.haveSun = false;
    state.lastCameraCheckQpc = 0;
    state.runs = 0;
    state.declined = 0;
    state.refusedShapes.clear();
    state.loggedOff = false;
    state.indexedLogged = false;
    state.partialLogged = false;
    state.readback.Reset();
    state.cameraReadback.Reset();
    state.cost = CostMeter {};
    state.bindCost = BindCostMeter {};
    state.drawsSeen = 0;
    state.bindsSeen = 0;
    state.boundLayoutId.store(0, std::memory_order_relaxed);
    state.dumpStartQpc = 0;
    state.dumpWritten = false;
    {
        std::lock_guard lock(I().mutex);
        I().layouts.clear();
        I().byCrc.clear();
    }
    PublishParams();
}

// The key reaches the module through the config's own hook pointer (ext::FfxivConfig::contactShadowsOnLoaded), so ExtConfig.cpp keeps no
// link dependency on this file. A config load may run inside DllMain: nothing here logs, creates a device object or touches the GPU.
void OnConfigLoaded(bool on) { Publish(on); }
} // namespace ext::ffxiv::relight

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run.
struct RegisterContactShadows
{
    RegisterContactShadows() { ext::FfxivConfig::contactShadowsOnLoaded = &ext::ffxiv::relight::OnConfigLoaded; }
} g_registerContactShadows;
} // namespace
