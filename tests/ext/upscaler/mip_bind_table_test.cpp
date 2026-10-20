// [MIP-BIND] The sampler table and the sampler variants (ext/upscaler/MipBindEngine.h): the table with fake pointers (insert,
// refresh, generation, invalidate, address reuse, full, threads), then the engine on a real D3D11 WARP device: variants in add
// and replace with the real numbers of the game's tracking family, variants made off the bind path, lazy classification of
// samplers the create hook never saw, stale variants after a reused address, dead entries, a failing create.
#include "ext/upscaler/MipBindEngine.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ext::upscaler::mipbind;

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance; }

static constexpr double kQ = -0.584963; // Quality 1.5x

static D3D11_SAMPLER_DESC Desc(float bias, D3D11_FILTER filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR)
{
    D3D11_SAMPLER_DESC d {};
    d.Filter = filter;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MipLODBias = bias;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MinLOD = 0.0f;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    return d;
}

static ID3D11SamplerState* Fake(size_t n) { return reinterpret_cast<ID3D11SamplerState*>(0x10000 + n * 0x40); }

static CreateInfo Info(const D3D11_SAMPLER_DESC& created, float original, float baseCreate = 0.0f, bool eligible = true)
{
    CreateInfo i;
    i.created = created;
    i.original = original;
    i.baseCreate = baseCreate;
    i.family = FamilyOf(created);
    i.eligible = eligible;
    return i;
}

static void TableOnly()
{
    SamplerTable t;
    const auto d = Desc(-1.0f);

    // unknown and not eligible: nothing stored
    CHECK(t.OnCreated(Fake(1), Info(d, -1.0f, 0, false)) == SamplerTable::Result::Ignored);
    CHECK(t.Find(Fake(1)) == nullptr && t.Count() == 0);
    CHECK(t.OnCreated(nullptr, Info(d, -1.0f)) == SamplerTable::Result::Ignored);
    CHECK(t.Find(nullptr) == nullptr);

    // insert, find
    CHECK(t.OnCreated(Fake(1), Info(d, -1.0f, -0.5f)) == SamplerTable::Result::Inserted);
    auto* e = t.Find(Fake(1));
    CHECK(e != nullptr && e->eligible.load() && Near(e->original.load(), -1.0f) && Near(e->baseCreate.load(), -0.5f) &&
          e->key == Fake(1) && e->ready.load() && e->generation.load() == 0);
    CHECK(t.Count() == 1 && t.Find(Fake(2)) == nullptr);

    // the same object created again with the same values (the runtime deduplicates): refreshed, same generation
    CHECK(t.OnCreated(Fake(1), Info(d, -1.0f, -0.5f)) == SamplerTable::Result::Refreshed);
    CHECK(t.Find(Fake(1)) == e && e->generation.load() == 0 && t.Count() == 1);

    // M6: the same object with another bias (a reused address): the entry keeps its place, the generation moves
    CHECK(t.OnCreated(Fake(1), Info(d, -1.5f, -0.5f)) == SamplerTable::Result::Refreshed);
    CHECK(Near(e->original.load(), -1.5f) && e->generation.load() == 1);
    CHECK(t.OnCreated(Fake(1), Info(d, -1.5f, 0.0f)) == SamplerTable::Result::Refreshed); // another creation target
    CHECK(e->generation.load() == 2 && Near(e->baseCreate.load(), 0.0f));

    // the address is reused by a sampler that is not eligible: switched off, and later on again
    CHECK(t.OnCreated(Fake(1), Info(d, 0.0f, 0, false)) == SamplerTable::Result::Invalidated);
    CHECK(!e->eligible.load());
    CHECK(t.OnCreated(Fake(1), Info(d, -0.5f)) == SamplerTable::Result::Refreshed);
    CHECK(e->eligible.load() && Near(e->original.load(), -0.5f));

    // the address is reused by an eligible sampler with a DIFFERENT description: a new entry takes over
    auto other = Desc(-1.0f, D3D11_FILTER_ANISOTROPIC);
    other.MaxAnisotropy = 8;
    CHECK(t.OnCreated(Fake(1), Info(other, -2.0f)) == SamplerTable::Result::Replaced);
    auto* e2 = t.Find(Fake(1));
    CHECK(e2 != nullptr && e2 != e && !e->eligible.load() && e2->eligible.load() && e2->desc.MaxAnisotropy == 8);
    CHECK(Near(e2->original.load(), -2.0f) && t.Count() == 2 && e2->family != e->family);

    // full: 256 entries, then the native fallback
    SamplerTable full;
    for (size_t i = 0; i < kTableCapacity; ++i)
        CHECK(full.OnCreated(Fake(i), Info(d, -1.0f)) == SamplerTable::Result::Inserted);
    CHECK(full.Count() == kTableCapacity);
    CHECK(full.OnCreated(Fake(kTableCapacity), Info(d, -1.0f)) == SamplerTable::Result::Full);
    CHECK(full.OnCreated(Fake(kTableCapacity + 1), Info(d, -1.0f)) == SamplerTable::Result::Full);
    CHECK(full.Find(Fake(kTableCapacity)) == nullptr);
    CHECK(full.Count() == kTableCapacity);
    for (size_t i = 0; i < kTableCapacity; ++i)
        CHECK(full.Find(Fake(i)) != nullptr && full.Find(Fake(i))->key == Fake(i)); // all of them still reachable
    CHECK(full.OnCreated(Fake(3), Info(d, -0.75f)) == SamplerTable::Result::Refreshed); // known ones still refresh when full
}

static void Threads()
{
    // Creators race on the same table: every sampler ends up findable exactly once.
    SamplerTable t;
    const auto d = Desc(-1.0f);
    constexpr size_t kThreads = 8, kPer = 24;
    std::vector<std::thread> pool;
    for (size_t k = 0; k < kThreads; ++k)
        pool.emplace_back(
            [&, k]
            {
                for (size_t i = 0; i < kPer; ++i)
                {
                    const size_t id = k * kPer + i;
                    t.OnCreated(Fake(id), Info(d, -1.0f - static_cast<float>(id)));
                    t.OnCreated(Fake(id), Info(d, -1.0f - static_cast<float>(id))); // and again: a refresh
                }
            });
    for (auto& th : pool)
        th.join();

    CHECK(t.Count() == kThreads * kPer);
    for (size_t id = 0; id < kThreads * kPer; ++id)
    {
        auto* e = t.Find(Fake(id));
        CHECK(e != nullptr && Near(e->original.load(), -1.0f - static_cast<float>(id)) && e->ready.load());
    }
}

static CreateSamplerFn NativeCreate(ID3D11Device* device)
{
    return reinterpret_cast<CreateSamplerFn>((*reinterpret_cast<void***>(device))[23]);
}

static int g_failNext = 0;
static CreateSamplerFn g_realCreate = nullptr;
static HRESULT STDMETHODCALLTYPE FailingCreate(ID3D11Device* d, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    if (g_failNext > 0)
    {
        --g_failNext;
        return E_FAIL;
    }
    return g_realCreate(d, desc, out);
}

static ComPtr<ID3D11SamplerState> Make(ID3D11Device* device, const D3D11_SAMPLER_DESC& d)
{
    ComPtr<ID3D11SamplerState> s;
    CHECK(SUCCEEDED(device->CreateSamplerState(&d, &s)));
    return s;
}

static float BiasOf(ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return d.MipLODBias;
}

// What the create hook does for a sampler the game made.
static SamplerTable::Result Created(Engine& e, ID3D11Device* device, ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return e.NoteCreated(s, device, d, d, IsEligible(d.MipLODBias, d.MinLOD, d.MaxLOD, d.Filter));
}

static void OnDevice(ID3D11Device* device)
{
    // add mode ---------------------------------------------------------------------------------------------------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));

        auto art1 = Make(device, Desc(-1.0f));
        auto art2 = Make(device, Desc(-2.0f));
        auto flat = Make(device, Desc(0.0f)); // bias 0: not eligible
        auto cmp = Make(device, Desc(-1.0f, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR));

        CHECK(Created(e, device, art1.Get()) == SamplerTable::Result::Inserted);
        CHECK(Created(e, device, art2.Get()) == SamplerTable::Result::Inserted);
        CHECK(Created(e, device, flat.Get()) == SamplerTable::Result::Ignored);
        CHECK(Created(e, device, cmp.Get()) == SamplerTable::Result::Ignored);

        // no target yet: everything is bound as the game made it
        CHECK(e.VariantFor(art1.Get()) == art1.Get());
        CHECK(e.CreateBase() == 0.0); // and a sampler made now is a native-resolution one

        // M4: a new target makes NO variant by itself; the bind path would get the game's sampler until Refresh
        CHECK(e.SetTarget(kQ, 0.0, Mode::Add));
        CHECK(e.VariantFor(art1.Get()) == art1.Get());
        CHECK(e.counters().variantsCreated.load() == 0 && e.counters().notReadyBinds.load() >= 1);
        e.Refresh();
        CHECK(e.counters().variantsCreated.load() == 2);
        auto* v1 = e.VariantFor(art1.Get());
        CHECK(v1 != nullptr && v1 != art1.Get() && Near(BiasOf(v1), -1.584963));
        auto* v2 = e.VariantFor(art2.Get());
        CHECK(v2 != nullptr && v2 != art2.Get() && Near(BiasOf(v2), -2.584963));
        CHECK(e.VariantFor(flat.Get()) == flat.Get());
        CHECK(e.VariantFor(cmp.Get()) == cmp.Get());
        CHECK(e.VariantFor(nullptr) == nullptr);
        CHECK(e.VariantFor(art1.Get()) == v1); // cached for this revision, no second variant
        CHECK(e.counters().variantsCreated.load() == 2);

        // the variant keeps everything but the bias
        D3D11_SAMPLER_DESC a {}, b {};
        art1->GetDesc(&a);
        v1->GetDesc(&b);
        CHECK(a.Filter == b.Filter && a.AddressU == b.AddressU && a.MaxLOD == b.MaxLOD && a.MaxAnisotropy == b.MaxAnisotropy);

        // same target again: no new revision
        const auto rev = e.Revision();
        CHECK(!e.SetTarget(kQ, 0.0, Mode::Add) && e.Revision() == rev);

        // the offset is part of the add: the old module's number (base + offset)
        CHECK(e.SetTarget(kQ, -0.18, Mode::Add));
        e.Refresh();
        CHECK(Near(BiasOf(e.VariantFor(art1.Get())), -1.764963));

        // a new target: a new revision
        CHECK(e.SetTarget(-1.0, 0.0, Mode::Add) && e.Revision() != rev);
        e.Refresh();
        CHECK(Near(BiasOf(e.VariantFor(art1.Get())), -2.0));

        // DLAA with no offset: add leaves the sampler alone (no variant needed)
        CHECK(e.SetTarget(0.0, 0.0, Mode::Add));
        e.Refresh();
        CHECK(e.VariantFor(art1.Get()) == art1.Get());
    }

    // replace mode, the real numbers of the game's tracking family (H1) ---------------------------------------------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        e.SetTarget(0.0, 0.0, Mode::Replace); // DLAA at the login screen
        auto stale = Make(device, Desc(-1.0f));    // made at DLAA: the game gave it -1
        auto art = Make(device, Desc(-0.5f));      // an art value made at DLAA
        Created(e, device, stale.Get());
        Created(e, device, art.Get());
        e.Refresh();
        CHECK(e.VariantFor(stale.Get()) == stale.Get()); // at DLAA the game's -1 stays -1, not 0
        CHECK(e.VariantFor(art.Get()) == art.Get());

        // the mode moves to Quality
        e.SetTarget(kQ, 0.0, Mode::Replace);
        auto fresh = Make(device, Desc(static_cast<float>(-1.0 + kQ))); // made at Quality: the game's own value there
        CHECK(Near(e.CreateBase(), kQ));
        Created(e, device, fresh.Get());
        e.Refresh();
        auto* vs = e.VariantFor(stale.Get());
        CHECK(vs != stale.Get() && Near(BiasOf(vs), -1.584963)); // the stale -1 becomes -1.585
        CHECK(e.VariantFor(fresh.Get()) == fresh.Get());         // one made at Quality is bit-identical to the game
        CHECK(Near(BiasOf(e.VariantFor(art.Get())), -1.084963));  // the art value shifts like the manual add: -0.5 + (-0.585)
        // the runtime handed back the game's own sampler for the stale one (same description): nothing is lost
        CHECK(vs == fresh.Get());

        // with the user's offset on top, in both
        e.SetTarget(kQ, -0.18, Mode::Replace);
        e.Refresh();
        CHECK(Near(BiasOf(e.VariantFor(stale.Get())), -1.764963));
        CHECK(Near(BiasOf(e.VariantFor(fresh.Get())), -1.764963));

        // back to DLAA: the stale one returns to -1, the one made at Quality loses the sharpening again
        e.SetTarget(0.0, 0.0, Mode::Replace);
        e.Refresh();
        CHECK(e.VariantFor(stale.Get()) == stale.Get());
        auto* vf = e.VariantFor(fresh.Get());
        CHECK(vf != fresh.Get() && Near(BiasOf(vf), -1.0, 1e-4)); // -1.585 + 0.585 = -1.0 (the game's own DLAA value)
    }

    // a manual-override style change of eligibility and a reused address ---------------------------------------------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        auto art = Make(device, Desc(-1.0f));
        Created(e, device, art.Get());
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh();
        CHECK(e.VariantFor(art.Get()) != art.Get());
        // the address comes back as a sampler that is not eligible
        e.NoteCreated(art.Get(), device, Desc(0.0f), Desc(0.0f), false);
        CHECK(e.VariantFor(art.Get()) == art.Get());
    }

    // M6: the address of a released sampler comes back with another bias: the variant of the old one must not be used --------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        auto art = Make(device, Desc(-1.0f));
        Created(e, device, art.Get());
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh();
        CHECK(Near(BiasOf(e.VariantFor(art.Get())), -2.0));
        // the same object (same description but for the bias) is created again with -3 at the same address
        D3D11_SAMPLER_DESC reused {};
        art->GetDesc(&reused); // the runtime normalises some fields: take its own description, change only the bias
        reused.MipLODBias = -3.0f;
        CHECK(e.NoteCreated(art.Get(), device, reused, reused, true) == SamplerTable::Result::Refreshed);
        // until the next service the bind path gets the game's sampler, never the stale variant
        CHECK(e.VariantFor(art.Get()) == art.Get());
        e.Service();
        CHECK(Near(BiasOf(e.VariantFor(art.Get())), -4.0)); // -3 + (-1), not the stale -2
    }

    // M4: samplers created after a Refresh get their variants at the next Service, not on the bind path; M3: lazy classification --
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh();

        auto late = Make(device, Desc(-1.0f));
        Created(e, device, late.Get());
        CHECK(e.VariantFor(late.Get()) == late.Get()); // not ready yet
        const auto notReady = e.counters().notReadyBinds.load();
        CHECK(notReady >= 1 && e.counters().variantsCreated.load() == 0);
        e.Service();
        CHECK(e.counters().variantsCreated.load() == 1);
        CHECK(Near(BiasOf(e.VariantFor(late.Get())), -2.0));
        e.Service(); // nothing pending: nothing happens
        CHECK(e.counters().variantsCreated.load() == 1);

        // a sampler the create hook never saw: classified the first time it is bound, its variant comes at the next Service
        auto early = Make(device, Desc(-0.5f));
        auto early0 = Make(device, Desc(0.0f)); // not eligible: remembered as such, no entry
        CHECK(e.VariantFor(early.Get()) == early.Get());
        CHECK(e.counters().lazyClassified.load() == 1 && e.table().Count() == 2);
        CHECK(e.VariantFor(early0.Get()) == early0.Get());
        CHECK(e.VariantFor(early0.Get()) == early0.Get());
        CHECK(e.counters().lazyClassified.load() == 1 && e.table().Count() == 2); // the negative answer is cached
        e.Service();
        CHECK(Near(BiasOf(e.VariantFor(early.Get())), -1.5));
        // the lazily classified one counts as made at native resolution (the creation target was not known)
        CHECK(Near(e.table().Find(early.Get())->baseCreate.load(), 0.0f));

        // the negative cache is forgotten when the create hook sees the address again
        e.NoteCreated(early0.Get(), device, Desc(-2.0f), Desc(-2.0f), true);
        CHECK(e.table().Find(early0.Get()) != nullptr);
    }

    // a released game sampler leaves a dead entry: making variants for the whole table must not touch the dead pointer ------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        auto live = Make(device, Desc(-1.0f));
        Created(e, device, live.Get());
        {
            auto dying = Make(device, Desc(-0.5f));
            Created(e, device, dying.Get());
        } // released here (the table holds no reference)
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh(); // would crash on a dereference of the key
        CHECK(Near(BiasOf(e.VariantFor(live.Get())), -2.0));
        CHECK(e.table().Count() == 2);
    }

    // M-E: entries of another device (a probe device, or the game's device before a graphics reset) are never touched: the
    // variant is not made, the device pointer is not dereferenced (a fake pointer would crash), and the entry stays as it was.
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        std::array<SetSamplersFn, kStages> fs;
        std::array<GetSamplersFn, kStages> fg;
        void** ctxVt = *reinterpret_cast<void***>(context.Get());
        for (unsigned s = 0; s < kStages; ++s)
        {
            fs[s] = reinterpret_cast<SetSamplersFn>(ctxVt[kContextSetSamplersSlots[s]]);
            fg[s] = reinterpret_cast<GetSamplersFn>(ctxVt[kContextGetSamplersSlots[s]]);
        }
        e.Attach(context.Get(), fs, fg);

        auto* deadDevice = reinterpret_cast<ID3D11Device*>(static_cast<std::uintptr_t>(0xDEAD0000));
        const auto d = Desc(-1.0f);
        CHECK(e.NoteCreated(Fake(500), deadDevice, d, d, true) == SamplerTable::Result::Inserted);
        auto live = Make(device, Desc(-0.5f));
        Created(e, device, live.Get());
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh(); // MakeVariants must skip the dead device's entry
        CHECK(e.counters().variantsCreated.load() == 1); // only the live one
        CHECK(Near(BiasOf(e.VariantFor(live.Get())), -1.5));
        CHECK(e.VariantFor(Fake(500)) == Fake(500)); // stays as the game made it
        const auto notReady = e.counters().notReadyBinds.load();
        e.Service();
        e.Service();
        CHECK(e.counters().notReadyBinds.load() == notReady); // and does not keep the engine busy
    }

    // a failing create falls back to the game's sampler, once per revision -------------------------------------------
    {
        Engine e;
        g_realCreate = NativeCreate(device);
        e.SetCreateFn(&FailingCreate);
        auto art = Make(device, Desc(-1.0f));
        Created(e, device, art.Get());
        e.SetTarget(-1.0, 0.0, Mode::Add);
        g_failNext = 1;
        e.Refresh();
        CHECK(e.VariantFor(art.Get()) == art.Get());
        CHECK(e.counters().variantFailures.load() == 1);
        e.Service();
        CHECK(e.VariantFor(art.Get()) == art.Get()); // no retry within the revision
        CHECK(e.counters().variantFailures.load() == 1 && g_failNext == 0);
        e.SetTarget(-1.5, 0.0, Mode::Add); // the next revision tries again and succeeds
        e.Refresh();
        auto* v = e.VariantFor(art.Get());
        CHECK(v != art.Get() && Near(BiasOf(v), -2.5));
    }

    // no create function: the game's sampler -------------------------------------------------------------------------
    {
        Engine e;
        auto art = Make(device, Desc(-1.0f));
        Created(e, device, art.Get());
        e.SetTarget(-1.0, 0.0, Mode::Add);
        e.Refresh();
        CHECK(e.VariantFor(art.Get()) == art.Get());
    }

    // the new entries are drained once, in creation order (the detector reads them) ----------------------------------------
    {
        Engine e;
        auto a = Make(device, Desc(-1.0f));
        auto b = Make(device, Desc(-0.5f));
        Created(e, device, a.Get());
        Created(e, device, a.Get()); // a refresh is not a new entry
        Created(e, device, b.Get());
        std::vector<float> seen;
        e.DrainNewEntries([&](const SamplerTable::Entry& en) { seen.push_back(en.original.load()); });
        CHECK(seen.size() == 2 && Near(seen[0], -1.0) && Near(seen[1], -0.5));
        e.DrainNewEntries([&](const SamplerTable::Entry&) { seen.push_back(0.0f); });
        CHECK(seen.size() == 2);
        auto c = Make(device, Desc(-0.25f));
        Created(e, device, c.Get());
        e.DrainNewEntries([&](const SamplerTable::Entry& en) { seen.push_back(en.original.load()); });
        CHECK(seen.size() == 3 && Near(seen[2], -0.25));
    }
}

int main()
{
    TableOnly();
    Threads();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                         nullptr, &context);
    if (FAILED(hr))
    {
        std::printf("FAIL: no WARP device (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }
    OnDevice(device.Get());

    if (g_failed == 0)
        std::printf("mip_bind_table: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
