#include "pch.h"

#include "ext/reshade/ReShadeBoundaryCore.h"

#include <cctype>
#include <format>

namespace ext::reshade
{
const char* ApplyResultName(ApplyResult result)
{
    switch (result)
    {
    case ApplyResult::Applied:
        return "applied";
    case ApplyResult::NotEnabled:
        return "off by config";
    case ApplyResult::NoReShade:
        return "ReShade add-on not registered";
    case ApplyResult::NoRuntime:
        return "no ReShade runtime";
    case ApplyResult::NothingToApply:
        return "nothing to apply";
    case ApplyResult::Failed:
        return "failed";
    }
    return "?";
}

PresetsMode ParsePresetsMode(std::string_view text)
{
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "on" || t == "true" || t == "1")
        return PresetsMode::On;
    if (t == "off" || t == "false" || t == "0")
        return PresetsMode::Off;
    return PresetsMode::Auto;
}

const char* PresetsModeName(PresetsMode mode)
{
    return mode == PresetsMode::On ? "on" : (mode == PresetsMode::Off ? "off" : "auto");
}
} // namespace ext::reshade

namespace ext::reshade::boundary
{
namespace
{
size_t Idx(ApplyResult r) { return static_cast<size_t>(r); }

// Drops everything the backend kept from Inspect(), on every exit of Apply() (a retained back buffer reference makes the game's
// ResizeBuffers fail).
struct BoundaryRelease
{
    probe::IBackend& backend;
    ~BoundaryRelease()
    {
        try
        {
            backend.EndBoundary();
        }
        catch (...)
        {
        }
    }
};
} // namespace

Applier::Applier(probe::IBackend& backend) : _backend(backend) {}

Applier::Slot* Applier::FindSlot(uint64_t runtime)
{
    for (auto& s : _slots)
        if (s.id.load(std::memory_order_relaxed) == runtime)
            return &s;
    return nullptr;
}

std::vector<uint64_t> Applier::AliveIds() const
{
    std::vector<uint64_t> ids;
    for (const auto& s : _slots)
    {
        const uint64_t id = s.id.load(std::memory_order_relaxed);
        if (id != 0)
            ids.push_back(id);
    }
    return ids;
}

uint32_t Applier::AliveRuntimes() const
{
    uint32_t n = 0;
    for (const auto& s : _slots)
        n += s.id.load(std::memory_order_relaxed) != 0 ? 1 : 0;
    return n;
}

void Applier::OnInitRuntime(uint64_t runtime)
{
    try
    {
        std::lock_guard lock(_mutex);
        ++_runtimeEvents;
        Slot* slot = FindSlot(runtime);
        if (slot == nullptr)
        {
            for (auto& s : _slots)
                if (s.id.load(std::memory_order_relaxed) == 0)
                {
                    slot = &s;
                    break;
                }
        }
        if (slot == nullptr)
            slot = &_slots.back(); // more runtimes than slots: recycle the last
        slot->reloaded.store(false, std::memory_order_relaxed);
        slot->id.store(runtime, std::memory_order_relaxed);
        ++_generation;
        _techDirty.store(true, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void Applier::OnDestroyRuntime(uint64_t runtime)
{
    try
    {
        std::lock_guard lock(_mutex);
        ++_runtimeEvents;
        if (Slot* slot = FindSlot(runtime))
        {
            slot->id.store(0, std::memory_order_relaxed);
            slot->reloaded.store(false, std::memory_order_relaxed);
        }
        ++_generation;
        _techDirty.store(true, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void Applier::OnReloadedEffects(uint64_t runtime)
{
    if (Slot* slot = FindSlot(runtime))
        slot->reloaded.store(true, std::memory_order_relaxed);
    _techDirty.store(true, std::memory_order_relaxed);
}

void Applier::OnTechniquesChanged() { _techDirty.store(true, std::memory_order_relaxed); }

ApplyResult Applier::Done(ApplyResult result, const char* detail)
{
    _results[Idx(result)].fetch_add(1, std::memory_order_relaxed);
    if (result != ApplyResult::Applied && detail != nullptr && detail[0] != '\0')
    {
        std::lock_guard lock(_detailMutex);
        if (_lastDetail != detail)
            _lastDetail = detail;
    }
    return result;
}

ApplyStats Applier::GetStats() const
{
    ApplyStats s;
    s.calls = _calls.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kApplyResults; ++i)
        s.results[i] = _results[i].load(std::memory_order_relaxed);
    s.callUsMax = _callUsMax.load(std::memory_order_relaxed);
    s.callUsTotal = _callUsTotal.load(std::memory_order_relaxed);
    s.runtimeEvents = _runtimeEvents.load(std::memory_order_relaxed);
    s.inert = _inert.load(std::memory_order_relaxed);
    std::lock_guard lock(_detailMutex);
    s.lastDetail = _lastDetail;
    return s;
}

ApplyResult Applier::Apply(PresetsMode mode, bool registered, void* context, void* target)
{
    try
    {
        _calls.fetch_add(1, std::memory_order_relaxed);
        if (mode == PresetsMode::Off)
            return Done(ApplyResult::NotEnabled, "[HudLess] ReShadePresets = off");
        if (!registered)
            return Done(ApplyResult::NoReShade, "OptiScaler is not registered as a ReShade add-on");

        std::lock_guard lock(_mutex);
        if (_inert.load(std::memory_order_relaxed))
            return Done(ApplyResult::Failed, "latched inert after an earlier failure (restart the game)");
        if (_appliedThisFrame.load(std::memory_order_relaxed))
            return Done(ApplyResult::NothingToApply, "already applied this frame");
        if (context == nullptr)
            return Done(ApplyResult::Failed, "no D3D11 context");

        // ---- which runtime: chosen once per (context, runtime generation) --------------------------------------------
        if (!_pickValid || _pickContext != context || _pickGeneration != _generation)
        {
            _pickValid = true;
            _pickContext = context;
            _pickGeneration = _generation;
            _pickRuntime = 0;
            _pickWhy.clear();
            const std::vector<uint64_t> alive = AliveIds();
            if (alive.empty())
                _pickWhy = "no live ReShade effect runtime";
            else
                _pickRuntime = _backend.PickRuntime(context, alive, _pickWhy);
            if (_pickRuntime == 0 && _pickWhy.empty())
                _pickWhy = "no ReShade runtime matches this D3D11 context";
        }
        if (_pickRuntime == 0)
            return Done(ApplyResult::NoRuntime, _pickWhy);
        const uint64_t runtime = _pickRuntime;

        // ---- is there anything to apply -------------------------------------------------------------------------------
        bool effects = false;
        if (!_backend.GetEffectsState(runtime, effects))
            return Done(ApplyResult::Failed, "could not read the ReShade effects state");
        if (!effects)
            return Done(ApplyResult::NothingToApply, "ReShade effects are disabled");

        Slot* slot = FindSlot(runtime);
        if (slot == nullptr || !slot->reloaded.load(std::memory_order_relaxed))
            return Done(ApplyResult::NothingToApply, "ReShade effects are not loaded yet");

        if (_techDirty.exchange(false, std::memory_order_relaxed) || _techRuntime != runtime || _techGeneration != _generation ||
            ++_techAge >= kRecountEvery)
        {
            _techAge = 0;
            _techRuntime = runtime;
            _techGeneration = _generation;
            _techKnown = _backend.CountTechniques(runtime, _techTotal, _techEnabled);
            if (!_techKnown)
                _techAge = kRecountEvery - 60; // unknown: retry in 60 calls, meanwhile do not block the presets
        }
        if (_techKnown && _techEnabled == 0 && mode != PresetsMode::On)
            return Done(ApplyResult::NothingToApply, std::format("no technique is enabled ({} loaded)", _techTotal));

        // ---- the call ---------------------------------------------------------------------------------------------------
        BoundaryRelease release { _backend };
        const probe::BoundaryInfo b = _backend.Inspect(context, runtime);
        if (!b.ok)
            return Done(ApplyResult::Failed, "boundary target refused: " + b.refusal);
        if (target != nullptr && b.resource != reinterpret_cast<uint64_t>(target))
            return Done(ApplyResult::Failed, "the bound render target is not the one passed to ApplyPresetsInPlace");

        const int64_t t0 = _backend.Qpc();
        const probe::CallOutcome out = _backend.RenderEffects(context, runtime, b);
        const int64_t t1 = _backend.Qpc();
        const int64_t freq = _backend.QpcFrequency();
        if (freq > 0)
        {
            const uint64_t us = static_cast<uint64_t>((t1 - t0) * 1000000 / freq);
            _callUsTotal.fetch_add(us, std::memory_order_relaxed);
            if (us > _callUsMax.load(std::memory_order_relaxed))
                _callUsMax.store(us, std::memory_order_relaxed);
        }

        if (out.exception)
        {
            _inert.store(true, std::memory_order_relaxed);
            return Done(ApplyResult::Failed, std::format("render_effects raised {:#x} ({}); the live path is latched inert", out.code, out.what));
        }
        if (!out.ok)
            return Done(ApplyResult::Failed, "render_effects was not executed: " + out.what);

        _appliedThisFrame.store(true, std::memory_order_relaxed);
        _results[Idx(ApplyResult::Applied)].fetch_add(1, std::memory_order_relaxed);
        if (!out.stateRestored)
        {
            // The effects ran, but the game's pipeline state may be wrong now: never call ReShade again in this process.
            _inert.store(true, std::memory_order_relaxed);
            std::lock_guard dl(_detailMutex);
            _lastDetail = "render_effects ran but the D3D11 state could not be restored (" + out.stateNote + "); the live path is latched inert";
        }
        return ApplyResult::Applied;
    }
    catch (...)
    {
        _results[Idx(ApplyResult::Failed)].fetch_add(1, std::memory_order_relaxed);
        return ApplyResult::Failed;
    }
}
} // namespace ext::reshade::boundary
