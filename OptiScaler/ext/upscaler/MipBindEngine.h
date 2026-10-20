#pragma once
// Bind-time mipmap bias: the sampler table and the sampler variants (docs/FEATURES.md "Automatic mipmap bias"). Header
// only, D3D11 and the standard library only (no Config, no logging, no Detours), so the tests drive it with a real WARP
// device. MipBindHooks.cpp is the glue that attaches it to the game's device and context.
//
// Threads. Samplers are created on any thread; everything that creates or replaces a variant, or touches a bound slot, runs
// on the immediate context's timeline (D3D11 already requires the game to serialise that context, and the evaluate seam and
// the menu run on it too). So:
//   - the table is insert-only and lock-free: a creator fills a free entry and publishes its index with one
//     compare-and-swap; readers only follow published indices;
//   - the fields a creator may change after publishing are atomics (original bias, creation target, eligibility, generation);
//   - variants, their revision and the per-slot records belong to the immediate-context timeline and need no lock.
//
// What the bind path (Set / Get) does: a pointer compare, a table lookup per changed slot, the native call and the COM
// reference counting of the slot records. It takes no lock, allocates nothing and makes NO variant: variants are made for the
// whole table when the target changes and when new samplers appeared (Refresh / Service, called by the evaluate seam), and a
// sampler whose variant is not ready is bound as the game made it until then. The one D3D call that can still happen on the bind
// path is ID3D11SamplerState::GetDesc, once per sampler the first time it is seen unclassified (lazy classification).
//
// References. The table holds no reference on game samplers (an address can be reused: every create refreshes its entry and a
// changed bias bumps the entry's generation). The slot records hold references on the sampler the game asked for and on the
// variant bound for it, up to 96 slots, so a sampler bound by the game outlives its release by the game until its slot is
// overwritten.

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

// Vtable slots of the entry points the hooks wrap (pinned against d3d11.h by tests/ext/upscaler/mip_bind_vtable_test.cpp).
inline constexpr unsigned kDeviceCreateSamplerStateSlot = 23;
inline constexpr std::array<unsigned, kStages> kContextSetSamplersSlots = { 10, 26, 32, 61, 65, 70 };
inline constexpr std::array<unsigned, kStages> kContextGetSamplersSlots = { 75, 85, 88, 99, 103, 108 };

inline const char* StageName(unsigned stage)
{
    static constexpr const char* names[kStages] = { "PS", "VS", "GS", "HS", "DS", "CS" };
    return stage < kStages ? names[stage] : "?";
}

// The description fields that identify a family of samplers (everything but the bias), for the add/replace detector.
inline std::uint32_t FamilyOf(const D3D11_SAMPLER_DESC& d)
{
    return FamilyHash(static_cast<unsigned>(d.Filter), static_cast<unsigned>(d.AddressU), static_cast<unsigned>(d.AddressV),
                      static_cast<unsigned>(d.AddressW), d.MaxAnisotropy, static_cast<unsigned>(d.ComparisonFunc), d.BorderColor,
                      d.MinLOD, d.MaxLOD);
}

// What a creator hands the table.
struct CreateInfo
{
    ID3D11Device* device = nullptr; // identity only: variants are made through it (it outlives the samplers)
    D3D11_SAMPLER_DESC created {};  // as the device created it (keeps upstream's anisotropy changes)
    float original = 0.0f;          // the bias the game asked for
    float baseCreate = 0.0f;        // the base target (log2 ratio) in force when the sampler was made
    std::uint32_t family = 0;
    bool eligible = false;
};

// ---- the sampler table ---------------------------------------------------------------------------------------------
class SamplerTable
{
  public:
    struct Entry
    {
        // Fixed when the entry is published.
        ID3D11SamplerState* key = nullptr; // identity only, no reference is held
        ID3D11Device* device = nullptr;
        D3D11_SAMPLER_DESC desc {};
        std::uint32_t family = 0;
        std::atomic<bool> ready { false }; // all of the above is written

        // Refreshed by every create of the same object (D3D11 returns an existing object for an identical description).
        std::atomic<float> original { 0.0f };
        std::atomic<float> baseCreate { 0.0f };
        std::atomic<bool> eligible { false };
        std::atomic<std::uint32_t> generation { 0 }; // bumped when original or baseCreate change under an existing entry

        // Immediate-context timeline only.
        ComPtr<ID3D11SamplerState> variant;
        std::uint32_t variantRevision = 0;   // 0 = none made for any revision yet
        std::uint32_t variantGeneration = 0; // the generation the variant was made for
    };

    enum class Result
    {
        Ignored,     // not eligible and not known: nothing stored
        Inserted,    // a new entry
        Refreshed,   // the object was known with the same description: bias and eligibility updated
        Invalidated, // known, and now not eligible: entry switched off
        Replaced,    // the address was reused by a different sampler that is eligible: a new entry took over
        Full,        // eligible, but the table has no room: the sampler is bound as the game made it
    };

    Result OnCreated(ID3D11SamplerState* sampler, const CreateInfo& info)
    {
        if (sampler == nullptr)
            return Result::Ignored;

        Entry* existing = Find(sampler);
        if (existing != nullptr)
        {
            if (!info.eligible)
            {
                existing->eligible.store(false, std::memory_order_release);
                return Result::Invalidated;
            }
            if (SameDesc(existing->desc, info.created))
            {
                // The same object again, perhaps with another bias (a reused address): a variant made for the old values is stale.
                const bool changed = existing->original.load(std::memory_order_relaxed) != info.original ||
                                     existing->baseCreate.load(std::memory_order_relaxed) != info.baseCreate;
                existing->original.store(info.original, std::memory_order_relaxed);
                existing->baseCreate.store(info.baseCreate, std::memory_order_relaxed);
                if (changed)
                    existing->generation.fetch_add(1, std::memory_order_relaxed);
                existing->eligible.store(true, std::memory_order_release);
                return Result::Refreshed;
            }
            // The address of a released sampler came back as a different sampler: a new entry takes the index slot over.
            existing->eligible.store(false, std::memory_order_release);
            return Insert(sampler, info, true) ? Result::Replaced : Result::Full;
        }

        if (!info.eligible)
            return Result::Ignored;

        return Insert(sampler, info, false) ? Result::Inserted : Result::Full;
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
    bool Insert(ID3D11SamplerState* sampler, const CreateInfo& info, bool replacing)
    {
        const std::uint32_t n = _next.fetch_add(1, std::memory_order_relaxed);
        if (n >= kTableCapacity)
        {
            _next.store(static_cast<std::uint32_t>(kTableCapacity), std::memory_order_relaxed);
            return false;
        }

        Entry& e = _entries[n];
        e.key = sampler;
        e.device = info.device;
        e.desc = info.created;
        e.family = info.family;
        e.original.store(info.original, std::memory_order_relaxed);
        e.baseCreate.store(info.baseCreate, std::memory_order_relaxed);
        e.eligible.store(true, std::memory_order_relaxed);
        e.generation.store(0, std::memory_order_relaxed);
        e.variant.Reset();
        e.variantRevision = 0;
        e.variantGeneration = 0;
        e.ready.store(true, std::memory_order_release);

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

// ---- context identity ----------------------------------------------------------------------------------------------------
// A ReShade-style wrapper may hand NGX a different COM pointer for the same immediate context. Private data set through such a
// wrapper reaches the native context, so a marker put on the hooked context is found again through the wrapper. A deferred
// context has no marker and never gets one. Used only to LOG whether the evaluate context is the hooked one: binds are served
// by pointer compare on the hooked context, nothing here is on the bind path.
struct ContextMarker
{
    static constexpr GUID kTag = { 0x9d0d5c3e, 0x7c1b, 0x4a62, { 0x8f, 0x37, 0x41, 0xb5, 0x0e, 0x6a, 0x2c, 0x90 } };

    // The token that was set, 0 when `context` is not an immediate context or the data could not be set.
    static unsigned long long Stamp(ID3D11DeviceContext* context)
    {
        static std::atomic<unsigned long long> sequence { 0 };
        if (context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
            return 0;

        const unsigned long long token = ++sequence;
        return SUCCEEDED(context->SetPrivateData(kTag, sizeof(token), &token)) ? token : 0;
    }

    static bool Matches(ID3D11DeviceContext* context, unsigned long long token)
    {
        if (context == nullptr || token == 0)
            return false;

        unsigned long long actual = 0;
        UINT size = sizeof(actual);
        return SUCCEEDED(context->GetPrivateData(kTag, &size, &actual)) && size == sizeof(actual) && actual == token;
    }
};

// ---- the engine ----------------------------------------------------------------------------------------------------
struct Counters
{
    std::atomic<std::uint64_t> eligibleCreated { 0 };
    std::atomic<std::uint64_t> tableFull { 0 };
    std::atomic<std::uint64_t> variantsCreated { 0 };
    std::atomic<std::uint64_t> variantFailures { 0 };
    std::atomic<std::uint64_t> lazyClassified { 0 }; // samplers first seen at a bind (made before the create hook)
    std::atomic<std::uint64_t> notReadyBinds { 0 };  // binds of an eligible sampler whose variant was not ready yet
};

class Engine
{
  public:
    SamplerTable& table() { return _table; }
    Counters& counters() { return _counters; }

    void SetCreateFn(CreateSamplerFn create) { _create = create; }

    // ---- target (immediate-context timeline) ----
    // The base target (log2 ratio), the user's offset and the mode. True when any of them changed (a new revision starts: the
    // variants made for the earlier one are replaced by the next Refresh).
    bool SetTarget(double base, double offset, Mode mode)
    {
        if (_hasTarget && static_cast<Mode>(_mode) == mode && base == _base && offset == _offset)
            return false;

        _hasTarget = true;
        _base = base;
        _offset = offset;
        _mode = static_cast<unsigned>(mode);
        _baseForCreate.store(base, std::memory_order_relaxed);
        ++_revision;
        return true;
    }

    bool HasTarget() const { return _hasTarget; }
    double Base() const { return _base; }
    double Offset() const { return _offset; }
    // The number the variants of the add mode add (the base plus the offset).
    double Target() const { return mipbind::Target(_base, _offset); }
    Mode CurrentMode() const { return static_cast<Mode>(_mode); }
    std::uint32_t Revision() const { return _revision; }

    // The base target in force now, for a sampler being created (any thread). 0 (native resolution) before the first evaluate.
    double CreateBase() const { return _baseForCreate.load(std::memory_order_relaxed); }

    // ---- classification (any thread) ----
    // A sampler was created: the game asked for `gameDesc`, the device made `created`. Refreshes the table and forgets that the
    // address was ever known as a sampler that needs no change.
    SamplerTable::Result NoteCreated(ID3D11SamplerState* sampler, ID3D11Device* device, const D3D11_SAMPLER_DESC& gameDesc,
                                     const D3D11_SAMPLER_DESC& created, bool eligible)
    {
        ForgetNegative(sampler);
        CreateInfo info;
        info.device = device;
        info.created = created;
        info.original = gameDesc.MipLODBias;
        info.baseCreate = static_cast<float>(CreateBase());
        info.family = FamilyOf(gameDesc);
        info.eligible = eligible;
        const auto result = _table.OnCreated(sampler, info);
        if (result == SamplerTable::Result::Inserted || result == SamplerTable::Result::Replaced ||
            result == SamplerTable::Result::Refreshed)
            _pending.store(true, std::memory_order_release);
        return result;
    }

    // ---- binding (immediate-context timeline) ----------------------------------------------------------------------
    // The context the hooks serve, and the original SetSamplers / GetSamplers of its six stages. Any other context (a
    // deferred one, a second device's, a ReShade proxy the game does not bind through) is passed straight to the natives.
    void Attach(ID3D11DeviceContext* latched, const std::array<SetSamplersFn, kStages>& set,
                const std::array<GetSamplersFn, kStages>& get)
    {
        _set = set;
        _get = get;
        _latched.store(latched, std::memory_order_release);
    }

    ID3D11DeviceContext* Latched() const { return _latched.load(std::memory_order_acquire); }

    // The hooks sit on the wrong context: serve another one of the same class. The game's own samplers go back on the old one.
    void Relatch(ID3D11DeviceContext* context)
    {
        const bool wasActive = _active.load(std::memory_order_relaxed);
        SetActive(false);
        _latched.store(context, std::memory_order_release);
        if (wasActive)
        {
            _active.store(true, std::memory_order_release);
            Rebind(false);
        }
    }

    // Substitution on or off. Turning it off puts the game's own samplers back on the latched context at once.
    void SetActive(bool active)
    {
        if (_active.load(std::memory_order_relaxed) == active)
            return;

        _active.store(active, std::memory_order_release);
        if (!active)
            Rebind(true);
    }

    bool Active() const { return _active.load(std::memory_order_relaxed); }

    // Detour body of the six SetSamplers. No lock, no allocation, no variant creation: a pointer compare, a table lookup per
    // changed slot and the native call. A slot whose sampler and revision are unchanged since the last bind costs two compares.
    void Set(unsigned stage, ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState* const* input)
    {
        const SetSamplersFn native = _set[stage];
        if (!_active.load(std::memory_order_relaxed) || c != _latched.load(std::memory_order_relaxed) ||
            input == nullptr || start >= kSlots || count == 0 || count > kSlots - start)
        {
            native(c, start, count, input);
            return;
        }

        ID3D11SamplerState* out[kSlots];
        bool any = false;
        for (UINT i = 0; i < count; ++i)
        {
            ID3D11SamplerState* const ask = input[i];
            Slot& s = _slots[stage][start + i];
            if (s.revision != _revision || s.asked.Get() != ask)
            {
                ID3D11SamplerState* const v = Ready(ask);
                s.asked = ask;
                if (v != ask)
                    s.variant = v;
                else
                    s.variant.Reset();
                s.revision = _revision;
            }

            if (s.variant)
            {
                out[i] = s.variant.Get();
                any = true;
                ++_substitutedBinds;
            }
            else
            {
                out[i] = ask;
            }
        }

        if (any)
            _anyRecord.store(true, std::memory_order_relaxed);

        native(c, start, count, out);
    }

    // Detour body of the six GetSamplers: the game sees the samplers it asked for. A slot is translated only when the native
    // call returns exactly the variant this slot's record bound, so a sampler the game bound by another route (ClearState,
    // a restored command list, the very object a variant happens to be) comes back as it is.
    void Get(unsigned stage, ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState** output)
    {
        _get[stage](c, start, count, output);
        if (!_anyRecord.load(std::memory_order_relaxed) || c != _latched.load(std::memory_order_relaxed) ||
            output == nullptr || start >= kSlots || count == 0 || count > kSlots - start)
            return;

        for (UINT i = 0; i < count; ++i)
        {
            const Slot& s = _slots[stage][start + i];
            if (s.variant && output[i] == s.variant.Get() && s.asked)
            {
                output[i]->Release();
                output[i] = s.asked.Get();
                output[i]->AddRef();
            }
        }
    }

    // After SetTarget: make the variants of the new revision for the whole table, then bind them to what is bound now. Reads
    // the real state from the context, so it also follows ClearState, restored command lists and state swaps.
    void Refresh()
    {
        MakeVariants();
        Rebind(false);
        if (_pending.load(std::memory_order_relaxed)) // samplers classified while rebinding
        {
            MakeVariants();
            Rebind(false);
        }
    }

    // Every evaluate: when samplers were created or first seen since the last time, make their variants and rebind. Cheap
    // (one relaxed load) otherwise.
    void Service()
    {
        if (!_pending.exchange(false, std::memory_order_acq_rel))
            return;
        MakeVariants();
        Rebind(false);
    }

    // Calls `f(entry)` once for every entry published since the last call, in creation order (the add/replace detector and
    // the observe log read the originals through it).
    template <class F> void DrainNewEntries(F&& f)
    {
        for (; _drained < _table.Count(); ++_drained)
        {
            SamplerTable::Entry& e = _table.EntryAt(_drained);
            if (!e.ready.load(std::memory_order_acquire))
                break;
            f(static_cast<const SamplerTable::Entry&>(e));
        }
    }

    // The sampler that would be bound where the game asked for `ask` right now (its variant when one is ready, else `ask`).
    ID3D11SamplerState* VariantFor(ID3D11SamplerState* ask) { return Ready(ask); }

    // Shutdown: let go of every reference the engine holds (slot records and variants). The hooks and the table stay.
    void ReleaseAll()
    {
        for (auto& stage : _slots)
            for (Slot& s : stage)
            {
                s.asked.Reset();
                s.variant.Reset();
                s.revision = 0;
            }
        for (size_t i = 0; i < _table.Count(); ++i)
        {
            SamplerTable::Entry& e = _table.EntryAt(i);
            e.variant.Reset();
            e.variantRevision = 0;
        }
        _anyRecord.store(false, std::memory_order_relaxed);
    }

    std::uint64_t SubstitutedBinds() const { return _substitutedBinds; }
    bool AnySubstituted() const { return _anyRecord.load(std::memory_order_relaxed); }

  private:
    // What one slot of the latched context holds: what the game asked for and the variant bound instead (null = the game's
    // own sampler was bound). Immediate-context timeline only.
    struct Slot
    {
        ComPtr<ID3D11SamplerState> asked;
        ComPtr<ID3D11SamplerState> variant;
        std::uint32_t revision = 0;
    };

    static constexpr size_t kNegativeSize = 512;

    static size_t NegativeIndex(const void* p)
    {
        std::uint64_t x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)) >> 4;
        x *= 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(x >> 40) & (kNegativeSize - 1);
    }

    void ForgetNegative(ID3D11SamplerState* sampler)
    {
        std::atomic<std::uintptr_t>& slot = _negative[NegativeIndex(sampler)];
        std::uintptr_t expected = reinterpret_cast<std::uintptr_t>(sampler);
        slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    }

    // The entry of a sampler the game handed to a bind, classifying it now when no create was seen (it was made before the
    // create hook existed). Bounded: ineligible samplers go into a small direct-mapped cache so GetDesc runs once for them.
    SamplerTable::Entry* EntryFor(ID3D11SamplerState* ask)
    {
        if (SamplerTable::Entry* e = _table.Find(ask))
            return e;

        std::atomic<std::uintptr_t>& neg = _negative[NegativeIndex(ask)];
        if (neg.load(std::memory_order_relaxed) == reinterpret_cast<std::uintptr_t>(ask))
            return nullptr;

        D3D11_SAMPLER_DESC d {};
        ask->GetDesc(&d);
        if (!IsEligible(d.MipLODBias, d.MinLOD, d.MaxLOD, static_cast<unsigned>(d.Filter)))
        {
            neg.store(reinterpret_cast<std::uintptr_t>(ask), std::memory_order_relaxed);
            return nullptr;
        }

        ComPtr<ID3D11Device> device;
        ask->GetDevice(&device);
        CreateInfo info;
        info.device = device.Get(); // identity only: the device outlives its samplers
        info.created = d;
        info.original = d.MipLODBias;
        info.baseCreate = 0.0f; // not known: native resolution
        info.family = FamilyOf(d);
        info.eligible = true;
        const auto result = _table.OnCreated(ask, info);
        if (result == SamplerTable::Result::Inserted || result == SamplerTable::Result::Replaced)
        {
            ++_counters.lazyClassified;
            _pending.store(true, std::memory_order_release);
        }
        else if (result == SamplerTable::Result::Full)
        {
            ++_counters.tableFull;
        }
        return _table.Find(ask);
    }

    // The sampler to bind where the game asked for `ask`: its variant when one is ready for the current revision, else `ask`.
    ID3D11SamplerState* Ready(ID3D11SamplerState* ask)
    {
        if (ask == nullptr || !_hasTarget)
            return ask;

        SamplerTable::Entry* e = EntryFor(ask);
        if (e == nullptr || !e->eligible.load(std::memory_order_acquire))
            return ask;

        if (e->variantRevision == _revision && e->variantGeneration == e->generation.load(std::memory_order_relaxed))
            return e->variant ? e->variant.Get() : ask;

        ++_counters.notReadyBinds;
        _pending.store(true, std::memory_order_release);
        return ask;
    }

    // Makes the variant of one entry for the current revision (or records that none is needed).
    void MakeVariant(SamplerTable::Entry& e)
    {
        e.variantRevision = _revision;
        e.variantGeneration = e.generation.load(std::memory_order_relaxed);
        e.variant.Reset();

        const float original = e.original.load(std::memory_order_relaxed);
        const float bias = VariantBias(CurrentMode(), original, _base, e.baseCreate.load(std::memory_order_relaxed), _offset);
        if (!NeedsVariant(original, bias) || _create == nullptr || e.device == nullptr)
            return;

        D3D11_SAMPLER_DESC desc = e.desc;
        desc.MipLODBias = bias;
        ComPtr<ID3D11SamplerState> made;
        if (FAILED(_create(e.device, &desc, &made)) || !made)
        {
            ++_counters.variantFailures; // remembered for this revision (variantRevision is set), so no retry storm
            return;
        }

        ++_counters.variantsCreated;
        if (made.Get() == e.key)
            return; // the runtime handed back the same object: nothing to substitute
        e.variant = std::move(made);
    }

    void MakeVariants()
    {
        if (!_hasTarget)
            return;

        _pending.store(false, std::memory_order_relaxed);
        const size_t n = _table.Count();
        for (size_t i = 0; i < n; ++i)
        {
            SamplerTable::Entry& e = _table.EntryAt(i);
            if (!e.ready.load(std::memory_order_acquire) || !e.eligible.load(std::memory_order_acquire))
                continue;
            if (e.variantRevision == _revision && e.variantGeneration == e.generation.load(std::memory_order_relaxed))
                continue;
            MakeVariant(e);
        }
    }

    // Every slot of every stage: read what is bound, work out what should be bound, set it when different. `restore`
    // binds the game's samplers (substitution going off) and forgets the records.
    void Rebind(bool restore)
    {
        ID3D11DeviceContext* const c = _latched.load(std::memory_order_acquire);
        if (c == nullptr || _set[0] == nullptr)
            return;

        for (unsigned stage = 0; stage < kStages; ++stage)
        {
            ID3D11SamplerState* actual[kSlots] = {};
            ID3D11SamplerState* desired[kSlots] = {};
            _get[stage](c, 0, kSlots, actual);

            bool changed = false;
            for (UINT i = 0; i < kSlots; ++i)
            {
                Slot& s = _slots[stage][i];
                ID3D11SamplerState* const boundByUs = s.variant ? s.variant.Get() : s.asked.Get();
                // Still what this slot's record bound: the game's request is the record's. Anything else was bound behind
                // the hooks' back: what is bound is the request.
                ID3D11SamplerState* const ask = (boundByUs == actual[i] && s.asked) ? s.asked.Get() : actual[i];

                ID3D11SamplerState* want = ask;
                if (restore)
                {
                    s.asked.Reset();
                    s.variant.Reset();
                    s.revision = 0;
                }
                else
                {
                    want = Ready(ask);
                    s.asked = ask;
                    if (want != ask)
                    {
                        s.variant = want;
                        _anyRecord.store(true, std::memory_order_relaxed);
                    }
                    else
                    {
                        s.variant.Reset();
                    }
                    s.revision = _revision;
                }

                desired[i] = want;
                if (want != actual[i])
                    changed = true;
            }

            if (changed)
                _set[stage](c, 0, kSlots, desired);

            for (UINT i = 0; i < kSlots; ++i)
                if (actual[i] != nullptr)
                    actual[i]->Release();
        }

        if (restore)
            _anyRecord.store(false, std::memory_order_relaxed);
    }

    SamplerTable _table;
    Counters _counters;
    CreateSamplerFn _create = nullptr;

    std::array<SetSamplersFn, kStages> _set {};
    std::array<GetSamplersFn, kStages> _get {};
    std::atomic<ID3D11DeviceContext*> _latched { nullptr };
    std::atomic<bool> _active { false };
    std::atomic<bool> _anyRecord { false };
    std::atomic<bool> _pending { false }; // samplers are waiting for their variants
    std::array<std::atomic<std::uintptr_t>, kNegativeSize> _negative {};
    std::array<std::array<Slot, kSlots>, kStages> _slots {};
    std::uint64_t _substitutedBinds = 0; // timeline only: a plain counter keeps the bind path free of atomic increments
    size_t _drained = 0;

    bool _hasTarget = false;
    double _base = 0.0;
    double _offset = 0.0;
    unsigned _mode = static_cast<unsigned>(Mode::Add);
    std::uint32_t _revision = 1;
    std::atomic<double> _baseForCreate { 0.0 };
};
} // namespace ext::upscaler::mipbind
