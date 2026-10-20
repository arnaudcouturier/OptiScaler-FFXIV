// [MIP-BIND] The sampler table and the sampler variants (ext/upscaler/MipBindEngine.h): the table with fake pointers
// (insert, refresh, invalidate, address reuse, full, threads), then the engine on a real D3D11 WARP device (variants in
// add and replace, unknown and ineligible samplers, a new revision, a failing create, the runtime's own deduplication).
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

static void TableOnly()
{
    SamplerTable t;
    const auto d = Desc(-1.0f);

    // unknown and not eligible: nothing stored
    CHECK(t.OnCreated(Fake(1), d, -1.0f, false) == SamplerTable::Result::Ignored);
    CHECK(t.Find(Fake(1)) == nullptr && t.Count() == 0);
    CHECK(t.OnCreated(nullptr, d, -1.0f, true) == SamplerTable::Result::Ignored);
    CHECK(t.Find(nullptr) == nullptr);

    // insert, find
    CHECK(t.OnCreated(Fake(1), d, -1.0f, true) == SamplerTable::Result::Inserted);
    auto* e = t.Find(Fake(1));
    CHECK(e != nullptr && e->eligible.load() && Near(e->original.load(), -1.0f) && e->key == Fake(1));
    CHECK(t.Count() == 1 && t.Find(Fake(2)) == nullptr);

    // the same object created again (the runtime deduplicates): refreshed, same entry, new original
    CHECK(t.OnCreated(Fake(1), d, -1.5f, true) == SamplerTable::Result::Refreshed);
    CHECK(t.Find(Fake(1)) == e && Near(e->original.load(), -1.5f) && t.Count() == 1);

    // the address is reused by a sampler that is not eligible: switched off
    CHECK(t.OnCreated(Fake(1), d, 0.0f, false) == SamplerTable::Result::Invalidated);
    CHECK(!e->eligible.load());
    // and later by an eligible one with the same description: switched on again
    CHECK(t.OnCreated(Fake(1), d, -0.5f, true) == SamplerTable::Result::Refreshed);
    CHECK(e->eligible.load() && Near(e->original.load(), -0.5f));

    // the address is reused by an eligible sampler with a DIFFERENT description: a new entry takes over
    auto other = Desc(-1.0f, D3D11_FILTER_ANISOTROPIC);
    other.MaxAnisotropy = 8;
    CHECK(t.OnCreated(Fake(1), other, -2.0f, true) == SamplerTable::Result::Replaced);
    auto* e2 = t.Find(Fake(1));
    CHECK(e2 != nullptr && e2 != e && !e->eligible.load() && e2->eligible.load() && e2->desc.MaxAnisotropy == 8);
    CHECK(Near(e2->original.load(), -2.0f) && t.Count() == 2);

    // full: 256 entries, then the native fallback
    SamplerTable full;
    for (size_t i = 0; i < kTableCapacity; ++i)
        CHECK(full.OnCreated(Fake(i), d, -1.0f, true) == SamplerTable::Result::Inserted);
    CHECK(full.Count() == kTableCapacity);
    CHECK(full.OnCreated(Fake(kTableCapacity), d, -1.0f, true) == SamplerTable::Result::Full);
    CHECK(full.OnCreated(Fake(kTableCapacity + 1), d, -1.0f, true) == SamplerTable::Result::Full);
    CHECK(full.Find(Fake(kTableCapacity)) == nullptr);
    CHECK(full.Count() == kTableCapacity);
    for (size_t i = 0; i < kTableCapacity; ++i)
        CHECK(full.Find(Fake(i)) != nullptr && full.Find(Fake(i))->key == Fake(i)); // all of them still reachable
    CHECK(full.OnCreated(Fake(3), d, -0.75f, true) == SamplerTable::Result::Refreshed); // known ones still refresh when full
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
                    t.OnCreated(Fake(id), d, -1.0f - static_cast<float>(id), true);
                    t.OnCreated(Fake(id), d, -1.0f - static_cast<float>(id), true); // and again: a refresh
                }
            });
    for (auto& th : pool)
        th.join();

    CHECK(t.Count() == kThreads * kPer);
    for (size_t id = 0; id < kThreads * kPer; ++id)
    {
        auto* e = t.Find(Fake(id));
        CHECK(e != nullptr && Near(e->original.load(), -1.0f - static_cast<float>(id)));
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

// What the glue does at a create: classify and hand the table the created object.
static SamplerTable::Result Classify(Engine& e, ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return e.table().OnCreated(s, d, d.MipLODBias, IsEligible(d.MipLODBias, d.MinLOD, d.MaxLOD, d.Filter));
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
        auto unknown = Make(device, Desc(-1.5f)); // created but never classified (made before the hook)

        CHECK(Classify(e, art1.Get()) == SamplerTable::Result::Inserted);
        CHECK(Classify(e, art2.Get()) == SamplerTable::Result::Inserted);
        CHECK(Classify(e, flat.Get()) == SamplerTable::Result::Ignored);
        CHECK(Classify(e, cmp.Get()) == SamplerTable::Result::Ignored);

        // no target yet: everything is bound as the game made it
        CHECK(e.Resolve(art1.Get()) == art1.Get());

        CHECK(e.SetTarget(-0.584963, Mode::Add));
        auto* v1 = e.Resolve(art1.Get());
        CHECK(v1 != nullptr && v1 != art1.Get() && Near(BiasOf(v1), -1.584963));
        auto* v2 = e.Resolve(art2.Get());
        CHECK(v2 != nullptr && v2 != art2.Get() && Near(BiasOf(v2), -2.584963));
        CHECK(e.Resolve(flat.Get()) == flat.Get());
        CHECK(e.Resolve(cmp.Get()) == cmp.Get());
        CHECK(e.Resolve(unknown.Get()) == unknown.Get());
        CHECK(e.Resolve(nullptr) == nullptr);
        CHECK(e.Resolve(art1.Get()) == v1); // cached for this revision, no second variant
        CHECK(e.counters().variantsCreated.load() == 2);

        // the variant keeps everything but the bias
        D3D11_SAMPLER_DESC a {}, b {};
        art1->GetDesc(&a);
        v1->GetDesc(&b);
        CHECK(a.Filter == b.Filter && a.AddressU == b.AddressU && a.MaxLOD == b.MaxLOD && a.MaxAnisotropy == b.MaxAnisotropy);

        // same target again: no new revision
        const auto rev = e.Revision();
        CHECK(!e.SetTarget(-0.584963, Mode::Add) && e.Revision() == rev);

        // a new target: a new revision, the next bind makes the new variant
        CHECK(e.SetTarget(-1.0, Mode::Add) && e.Revision() != rev);
        auto* v1b = e.Resolve(art1.Get());
        CHECK(v1b != nullptr && Near(BiasOf(v1b), -2.0));

        // DLAA with no offset: add leaves the sampler alone (no variant needed)
        CHECK(e.SetTarget(0.0, Mode::Add));
        CHECK(e.Resolve(art1.Get()) == art1.Get());
    }

    // replace mode: the target alone; two different art biases collapse onto the same variant object ---------------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        auto art1 = Make(device, Desc(-1.0f));
        auto art2 = Make(device, Desc(-2.0f));
        Classify(e, art1.Get());
        Classify(e, art2.Get());
        e.SetTarget(-0.584963, Mode::Replace);
        auto* v1 = e.Resolve(art1.Get());
        auto* v2 = e.Resolve(art2.Get());
        CHECK(v1 != nullptr && Near(BiasOf(v1), -0.584963) && Near(BiasOf(v2), -0.584963));
        CHECK(v1 == v2); // the runtime deduplicates identical descriptions: the reason Get cannot map variant -> source

        // a replace target equal to the game's own value: the runtime hands the game's sampler back, nothing to substitute
        auto same = Make(device, Desc(-0.5f));
        Classify(e, same.Get());
        e.SetTarget(-0.5, Mode::Replace);
        CHECK(e.Resolve(same.Get()) == same.Get()); // NeedsVariant is false

        // a variant that is the same object as another game sampler is still returned for the sampler that needs it
        auto target = Make(device, Desc(-0.75f));
        auto needs = Make(device, Desc(-1.0f));
        Classify(e, needs.Get());
        e.SetTarget(-0.75, Mode::Replace);
        CHECK(e.Resolve(needs.Get()) == target.Get());
    }

    // a manual-override style change of eligibility and a reused address ---------------------------------------------
    {
        Engine e;
        e.SetCreateFn(NativeCreate(device));
        auto art = Make(device, Desc(-1.0f));
        Classify(e, art.Get());
        e.SetTarget(-1.0, Mode::Add);
        CHECK(e.Resolve(art.Get()) != art.Get());
        // the address comes back as a sampler that is not eligible
        e.table().OnCreated(art.Get(), Desc(0.0f), 0.0f, false);
        CHECK(e.Resolve(art.Get()) == art.Get());
    }

    // a failing create falls back to the game's sampler, once per revision -------------------------------------------
    {
        Engine e;
        g_realCreate = NativeCreate(device);
        e.SetCreateFn(&FailingCreate);
        auto art = Make(device, Desc(-1.0f));
        Classify(e, art.Get());
        e.SetTarget(-1.0, Mode::Add);
        g_failNext = 1;
        CHECK(e.Resolve(art.Get()) == art.Get());
        CHECK(e.counters().variantFailures.load() == 1);
        CHECK(e.Resolve(art.Get()) == art.Get()); // no retry within the revision
        CHECK(e.counters().variantFailures.load() == 1 && g_failNext == 0);
        e.SetTarget(-1.5, Mode::Add); // the next revision tries again and succeeds
        auto* v = e.Resolve(art.Get());
        CHECK(v != art.Get() && Near(BiasOf(v), -2.5));
    }

    // no create function: the game's sampler -------------------------------------------------------------------------
    {
        Engine e;
        auto art = Make(device, Desc(-1.0f));
        Classify(e, art.Get());
        e.SetTarget(-1.0, Mode::Add);
        CHECK(e.Resolve(art.Get()) == art.Get());
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
