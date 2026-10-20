#pragma once
// Bind-time mipmap bias: the sampler table and the sampler variants (docs/FEATURES.md "Automatic mipmap bias"). Header
// only, D3D11 and the standard library only (no Config, no logging, no Detours), so the tests drive it with a real WARP
// device. MipBindHooks.cpp is the glue that attaches it to the game's device and context.
//
// Threads. Samplers are created on any thread; everything that touches a variant or a bound slot runs on the immediate
// context's timeline (D3D11 already requires the game to serialise that context, and the evaluate seam and the menu run on
// it too). So:
//   - the table is insert-only and lock-free: a creator fills a free entry and publishes its index with one
//     compare-and-swap; readers only follow published indices;
//   - the fields a creator may change after publishing are atomics (the sampler's original bias, whether it is eligible);
//   - variants, the variant revision and the per-slot records belong to the immediate-context timeline and need no lock.
// The bind path takes no lock and allocates nothing.

#include "ext/upscaler/MipBind.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ext::upscaler::mipbind
{
using Microsoft::WRL::ComPtr;

// ID3D11Device::CreateSamplerState, and the context's SetSamplers / GetSamplers for the six shader stages. The natives are
// the originals (the detour trampolines in the product, the vtable entries in the tests).
using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
using SetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using GetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**);

inline constexpr unsigned kStages = 6; // PS, VS, GS, HS, DS, CS
inline constexpr unsigned kSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
inline constexpr size_t kTableCapacity = 256; // samplers that can take part; the rest are bound as the game made them

enum class Stage : unsigned
{
    PS,
    VS,
    GS,
    HS,
    DS,
    CS
};

inline const char* StageName(unsigned stage)
{
    static constexpr const char* names[kStages] = { "PS", "VS", "GS", "HS", "DS", "CS" };
    return stage < kStages ? names[stage] : "?";
}

// ---- the sampler table ---------------------------------------------------------------------------------------------
class SamplerTable
{
  public:
    struct Entry
    {
        // Fixed when the entry is published.
        ID3D11SamplerState* key = nullptr; // identity only, no reference is held
        D3D11_SAMPLER_DESC desc {};        // as the device created it (keeps upstream's anisotropy changes)

        // Refreshed by every create of the same object (D3D11 returns an existing object for an identical description).
        std::atomic<float> original { 0.0f };
        std::atomic<bool> eligible { false };

        // Immediate-context timeline only.
        ComPtr<ID3D11SamplerState> variant;
        std::uint32_t variantRevision = 0; // 0 = none made for any revision yet
    };

    enum class Result
    {
        Ignored,     // not eligible and not known: nothing stored
        Inserted,    // a new entry
        Refreshed,   // the object was known with the same description: bias and eligibility updated
        Invalidated, // known, and now not eligible (or its address was reused by a different sampler): entry switched off
        Replaced,    // the address was reused by a different sampler that is eligible: a new entry took over
        Full,        // eligible, but the table has no room: the sampler is bound as the game made it
    };

    Result OnCreated(ID3D11SamplerState* sampler, const D3D11_SAMPLER_DESC& created, float originalBias,
                     bool eligible)
    {
        if (sampler == nullptr)
            return Result::Ignored;

        Entry* existing = Find(sampler);
        if (existing != nullptr)
        {
            if (!eligible)
            {
                existing->eligible.store(false, std::memory_order_release);
                return Result::Invalidated;
            }
            if (SameDesc(existing->desc, created))
            {
                existing->original.store(originalBias, std::memory_order_relaxed);
                existing->eligible.store(true, std::memory_order_release);
                return Result::Refreshed;
            }
            // The address of a released sampler came back as a different sampler: a new entry takes the index slot over.
            existing->eligible.store(false, std::memory_order_release);
            return Insert(sampler, created, originalBias, true) ? Result::Replaced : Result::Full;
        }

        if (!eligible)
            return Result::Ignored;

        return Insert(sampler, created, originalBias, false) ? Result::Inserted : Result::Full;
    }

    // Lock-free; follows only published entries.
    Entry* Find(ID3D11SamplerState* sampler)
    {
        if (sampler == nullptr)
            return nullptr;

        const size_t start = Hash(sampler);
        for (size_t probe = 0; probe < kIndexSize; ++probe)
        {
            const std::uint32_t idx = _index[(start + probe) & (kIndexSize - 1)].load(std::memory_order_acquire);
            if (idx == 0)
                return nullptr;
            Entry& e = _entries[idx - 1];
            if (e.key == sampler)
                return &e;
        }
        return nullptr;
    }

    size_t Count() const { return std::min<size_t>(_next.load(std::memory_order_relaxed), kTableCapacity); }
    Entry& EntryAt(size_t i) { return _entries[i]; }

  private:
    static constexpr size_t kIndexSize = 1024; // power of two, four times the capacity

    static size_t Hash(const void* p)
    {
        std::uint64_t x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)) >> 4;
        x *= 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(x >> 40);
    }

    static bool SameDesc(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b)
    {
        return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV &&
               a.AddressW == b.AddressW && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
               a.MinLOD == b.MinLOD && a.MaxLOD == b.MaxLOD && std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0;
    }

    // `replacing`: the pointer is already in the index (an older entry); the new entry takes over its index slot.
    bool Insert(ID3D11SamplerState* sampler, const D3D11_SAMPLER_DESC& created, float originalBias, bool replacing)
    {
        const std::uint32_t n = _next.fetch_add(1, std::memory_order_relaxed);
        if (n >= kTableCapacity)
        {
            _next.store(static_cast<std::uint32_t>(kTableCapacity), std::memory_order_relaxed);
            return false;
        }

        Entry& e = _entries[n];
        e.key = sampler;
        e.desc = created;
        e.original.store(originalBias, std::memory_order_relaxed);
        e.eligible.store(true, std::memory_order_relaxed);
        e.variant.Reset();
        e.variantRevision = 0;

        const std::uint32_t mine = n + 1;
        const size_t start = Hash(sampler);
        for (size_t probe = 0; probe < kIndexSize; ++probe)
        {
            std::atomic<std::uint32_t>& slot = _index[(start + probe) & (kIndexSize - 1)];
            std::uint32_t seen = slot.load(std::memory_order_acquire);
            for (;;)
            {
                if (seen == 0)
                {
                    if (slot.compare_exchange_weak(seen, mine, std::memory_order_release, std::memory_order_acquire))
                        return true;
                    continue; // `seen` was reloaded
                }
                if (replacing && _entries[seen - 1].key == sampler)
                {
                    if (slot.compare_exchange_weak(seen, mine, std::memory_order_release, std::memory_order_acquire))
                        return true;
                    continue;
                }
                break; // someone else's entry: next probe
            }
        }
        return false; // cannot happen (the index is four times the capacity)
    }

    std::array<Entry, kTableCapacity> _entries {};
    std::array<std::atomic<std::uint32_t>, kIndexSize> _index {}; // entry number + 1, 0 = free
    std::atomic<std::uint32_t> _next { 0 };
};

// ---- the engine ----------------------------------------------------------------------------------------------------
struct Counters
{
    std::atomic<std::uint64_t> eligibleCreated { 0 };
    std::atomic<std::uint64_t> tableFull { 0 };
    std::atomic<std::uint64_t> variantsCreated { 0 };
    std::atomic<std::uint64_t> variantFailures { 0 };
};

class Engine
{
  public:
    SamplerTable& table() { return _table; }
    Counters& counters() { return _counters; }

    void SetCreateFn(CreateSamplerFn create) { _create = create; }

    // ---- target (immediate-context timeline) ----
    // A new target and mode. True when they differ from the current ones (a new revision starts: the variants made for the
    // earlier one are dropped lazily, the next bind makes the new ones).
    bool SetTarget(double target, Mode mode)
    {
        if (_hasTarget && static_cast<Mode>(_mode) == mode && std::fabs(target - _target) < 1e-6)
            return false;

        _hasTarget = true;
        _target = target;
        _mode = static_cast<unsigned>(mode);
        ++_revision;
        return true;
    }

    bool HasTarget() const { return _hasTarget; }
    double Target() const { return _target; }
    Mode CurrentMode() const { return static_cast<Mode>(_mode); }
    std::uint32_t Revision() const { return _revision; }

    // The sampler that should be bound where the game asked for `ask` (immediate-context timeline). `ask` itself when the
    // sampler is unknown, not eligible, needs no change, or a variant cannot be made.
    ID3D11SamplerState* Resolve(ID3D11SamplerState* ask)
    {
        if (ask == nullptr || !_hasTarget)
            return ask;

        SamplerTable::Entry* e = _table.Find(ask);
        if (e == nullptr || !e->eligible.load(std::memory_order_acquire))
            return ask;

        if (e->variantRevision == _revision)
            return e->variant ? e->variant.Get() : ask;

        e->variantRevision = _revision;
        e->variant.Reset();

        const float original = e->original.load(std::memory_order_relaxed);
        const float bias = VariantBias(CurrentMode(), original, _target);
        if (!NeedsVariant(original, bias) || _create == nullptr)
            return ask;

        ComPtr<ID3D11Device> device;
        ask->GetDevice(&device);
        if (!device)
            return ask;

        D3D11_SAMPLER_DESC desc = e->desc;
        desc.MipLODBias = bias;
        ComPtr<ID3D11SamplerState> made;
        if (FAILED(_create(device.Get(), &desc, &made)) || !made)
        {
            ++_counters.variantFailures; // remembered for this revision (variantRevision is set), so no retry storm
            return ask;
        }

        ++_counters.variantsCreated;
        if (made.Get() == ask)
            return ask; // the runtime handed back the same object: nothing to substitute
        e->variant = std::move(made);
        return e->variant.Get();
    }

  private:
    SamplerTable _table;
    Counters _counters;
    CreateSamplerFn _create = nullptr;

    bool _hasTarget = false;
    double _target = 0.0;
    unsigned _mode = static_cast<unsigned>(Mode::Add);
    std::uint32_t _revision = 1;
};
} // namespace ext::upscaler::mipbind
