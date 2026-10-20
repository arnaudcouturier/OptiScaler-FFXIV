// [MIP-BIND] The bind path of the engine (ext/upscaler/MipBindEngine.h): what is bound on the context, what the game reads
// back, and which calls are left alone. The natives are the real SetSamplers / GetSamplers of a WARP immediate context
// (taken from its vtable, unhooked); the engine's Set / Get are what the detours call.
#include "ext/upscaler/MipBindEngine.h"

#include <d3d11_1.h>

#include <thread>

#include <cmath>
#include <cstdio>
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

static float BiasOf(ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return d.MipLODBias;
}

struct Rig
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<SetSamplersFn, kStages> set {};
    std::array<GetSamplersFn, kStages> get {};
    Engine engine;

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                     nullptr, &context)))
            return false;
        void** ctxVt = *reinterpret_cast<void***>(context.Get());
        void** devVt = *reinterpret_cast<void***>(device.Get());
        for (unsigned s = 0; s < kStages; ++s)
        {
            set[s] = reinterpret_cast<SetSamplersFn>(ctxVt[kContextSetSamplersSlots[s]]);
            get[s] = reinterpret_cast<GetSamplersFn>(ctxVt[kContextGetSamplersSlots[s]]);
        }
        engine.SetCreateFn(reinterpret_cast<CreateSamplerFn>(devVt[kDeviceCreateSamplerStateSlot]));
        engine.Attach(context.Get(), set, get);
        return true;
    }

    ComPtr<ID3D11SamplerState> Make(float bias, D3D11_FILTER f = D3D11_FILTER_MIN_MAG_MIP_LINEAR)
    {
        ComPtr<ID3D11SamplerState> s;
        const auto d = Desc(bias, f);
        CHECK(SUCCEEDED(device->CreateSamplerState(&d, &s)));
        D3D11_SAMPLER_DESC created {};
        s->GetDesc(&created);
        engine.NoteCreated(s.Get(), device.Get(), created, created,
                           IsEligible(created.MipLODBias, created.MinLOD, created.MaxLOD, created.Filter));
        engine.Service(); // the variants of new samplers are made off the bind path, here as the evaluate seam does
        return s;
    }

    // What is really bound on the context in `slot` of `stage` (native read, no translation).
    ComPtr<ID3D11SamplerState> Bound(unsigned stage, UINT slot)
    {
        ComPtr<ID3D11SamplerState> s;
        get[stage](context.Get(), slot, 1, s.GetAddressOf());
        return s;
    }

    // What the game reads back through the detour.
    ComPtr<ID3D11SamplerState> Read(unsigned stage, UINT slot)
    {
        ComPtr<ID3D11SamplerState> s;
        engine.Get(stage, context.Get(), slot, 1, s.GetAddressOf());
        return s;
    }

    void Bind(unsigned stage, UINT slot, ID3D11SamplerState* s) { engine.Set(stage, context.Get(), slot, 1, &s); }
};

static void SixStages()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-0.584963, 0.0, Mode::Add);
    r.engine.SetActive(true);

    auto art = r.Make(-1.0f);
    auto flat = r.Make(0.0f);
    for (unsigned stage = 0; stage < kStages; ++stage)
    {
        r.Bind(stage, 2, art.Get());
        r.Bind(stage, 3, flat.Get());
        auto bound = r.Bound(stage, 2);
        CHECK(bound && bound.Get() != art.Get() && Near(BiasOf(bound.Get()), -1.584963));
        CHECK(r.Bound(stage, 3).Get() == flat.Get()); // not eligible: bound as it is
        CHECK(r.Read(stage, 2).Get() == art.Get());   // the game reads back what it set
        CHECK(r.Read(stage, 3).Get() == flat.Get());
    }
    CHECK(r.engine.SubstitutedBinds() == kStages);

    // the same sampler again: no new variant, still substituted
    const auto made = r.engine.counters().variantsCreated.load();
    r.Bind(0, 2, art.Get());
    CHECK(r.engine.counters().variantsCreated.load() == made);
}

static void NullsAndRanges()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);
    auto a = r.Make(-1.0f);
    auto b = r.Make(-0.5f);

    // several slots at once, a null in the middle
    ID3D11SamplerState* in[4] = { a.Get(), nullptr, b.Get(), nullptr };
    r.engine.Set(0, r.context.Get(), 4, 4, in);
    CHECK(r.Bound(0, 4) && Near(BiasOf(r.Bound(0, 4).Get()), -2.0));
    CHECK(!r.Bound(0, 5));
    CHECK(r.Bound(0, 6) && Near(BiasOf(r.Bound(0, 6).Get()), -1.5));
    CHECK(!r.Bound(0, 7));
    ComPtr<ID3D11SamplerState> back[4];
    r.engine.Get(0, r.context.Get(), 4, 4, reinterpret_cast<ID3D11SamplerState**>(&back));
    CHECK(back[0].Get() == a.Get() && !back[1] && back[2].Get() == b.Get() && !back[3]);

    // overwrite one slot with a null, and one with a sampler that is not eligible
    ID3D11SamplerState* nul = nullptr;
    r.engine.Set(0, r.context.Get(), 4, 1, &nul);
    CHECK(!r.Bound(0, 4) && !r.Read(0, 4));
    CHECK(r.Bound(0, 6) != nullptr && Near(BiasOf(r.Bound(0, 6).Get()), -1.5)); // slot 6 untouched

    // the last slot, a full range, and out-of-range calls go to the native unchanged (fake natives record them)
    r.Bind(0, kSlots - 1, a.Get());
    CHECK(Near(BiasOf(r.Bound(0, kSlots - 1).Get()), -2.0));
    ID3D11SamplerState* full[kSlots];
    for (auto& s : full)
        s = a.Get();
    r.engine.Set(0, r.context.Get(), 0, kSlots, full);
    for (UINT i = 0; i < kSlots; ++i)
        CHECK(Near(BiasOf(r.Bound(0, i).Get()), -2.0));
}

// Fake natives that record what they were called with, for the calls the engine must not touch.
struct Recorded
{
    int calls = 0;
    ID3D11DeviceContext* context = nullptr;
    UINT start = 0, count = 0;
    ID3D11SamplerState* const* input = nullptr;
};
static Recorded g_rec;
static void STDMETHODCALLTYPE FakeSet(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11SamplerState* const* input)
{
    ++g_rec.calls;
    g_rec.context = c;
    g_rec.start = start;
    g_rec.count = count;
    g_rec.input = input;
}
static void STDMETHODCALLTYPE FakeGet(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**) {}

static void PassThrough()
{
    Rig r;
    CHECK(r.Init());
    auto a = r.Make(-1.0f);
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);

    // foreign context pointers (a deferred context, a second device's context, a proxy object): same pointer in, same array
    // in, the native called once
    std::array<SetSamplersFn, kStages> fakeSet;
    std::array<GetSamplersFn, kStages> fakeGet;
    fakeSet.fill(&FakeSet);
    fakeGet.fill(&FakeGet);
    r.engine.Attach(r.context.Get(), fakeSet, fakeGet);
    r.engine.Refresh(); // the variants of the new revision (the evaluate seam does this)

    ID3D11SamplerState* in[2] = { a.Get(), a.Get() };
    auto* foreign = reinterpret_cast<ID3D11DeviceContext*>(0x1234560);
    g_rec = {};
    r.engine.Set(0, foreign, 1, 2, in);
    CHECK(g_rec.calls == 1 && g_rec.context == foreign && g_rec.start == 1 && g_rec.count == 2 && g_rec.input == in);

    // bad ranges and a null array on the latched context: untouched too
    g_rec = {};
    r.engine.Set(1, r.context.Get(), kSlots, 1, in);
    CHECK(g_rec.calls == 1 && g_rec.input == in);
    g_rec = {};
    r.engine.Set(1, r.context.Get(), 10, 7, in);
    CHECK(g_rec.calls == 1 && g_rec.input == in && g_rec.start == 10 && g_rec.count == 7);
    g_rec = {};
    r.engine.Set(1, r.context.Get(), 0, 0, in);
    CHECK(g_rec.calls == 1 && g_rec.input == in && g_rec.count == 0);
    g_rec = {};
    r.engine.Set(1, r.context.Get(), 0, 2, nullptr);
    CHECK(g_rec.calls == 1 && g_rec.input == nullptr);

    // on the latched context the array handed to the native is the engine's, with the variants in it
    g_rec = {};
    r.engine.Set(2, r.context.Get(), 0, 2, in);
    CHECK(g_rec.calls == 1 && g_rec.input != in && g_rec.input[0] != a.Get() && g_rec.input[0] == g_rec.input[1]);

    // inactive: everything goes through
    r.engine.SetActive(false);
    g_rec = {};
    r.engine.Set(2, r.context.Get(), 0, 2, in);
    CHECK(g_rec.calls == 1 && g_rec.input == in);
}

static void DeferredContext()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);
    auto art = r.Make(-1.0f);

    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(r.device->CreateDeferredContext(0, &deferred)));
    // the hook code is shared by every context of the class, so a deferred context reaches the engine: it is not latched
    ID3D11SamplerState* s = art.Get();
    r.engine.Set(0, deferred.Get(), 0, 1, &s);
    ComPtr<ID3D11SamplerState> onDeferred;
    r.get[0](deferred.Get(), 0, 1, onDeferred.GetAddressOf());
    CHECK(onDeferred.Get() == art.Get()); // recorded with the game's own sampler, no variant
    CHECK(!r.Bound(0, 0));                // nothing happened on the immediate context
    ComPtr<ID3D11SamplerState> viaEngine;
    r.engine.Get(0, deferred.Get(), 0, 1, viaEngine.GetAddressOf());
    CHECK(viaEngine.Get() == art.Get());
    CHECK(r.engine.SubstitutedBinds() == 0);
}

// M3/M4: a sampler the create hook never saw is classified at its first bind; its variant is made at the next Service (off the
// bind path) and the slot that holds it is rebound by that Service, with no new Set from the game.
static void LazyClassificationThroughSet()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);

    D3D11_SAMPLER_DESC d = Desc(-1.0f);
    ComPtr<ID3D11SamplerState> unseen;
    CHECK(SUCCEEDED(r.device->CreateSamplerState(&d, &unseen))); // not passed to NoteCreated
    d = Desc(0.0f);
    ComPtr<ID3D11SamplerState> flat;
    CHECK(SUCCEEDED(r.device->CreateSamplerState(&d, &flat)));

    r.Bind(2, 3, unseen.Get());
    r.Bind(2, 4, flat.Get());
    CHECK(r.Bound(2, 3).Get() == unseen.Get()); // not ready yet: the game''s own sampler, never a stale or half-made one
    CHECK(r.engine.counters().lazyClassified.load() == 1 && r.engine.counters().variantsCreated.load() == 0);
    CHECK(r.engine.table().Count() == 1);

    r.engine.Service();
    CHECK(r.Bound(2, 3).Get() != unseen.Get() && Near(BiasOf(r.Bound(2, 3).Get()), -2.0));
    CHECK(r.Bound(2, 4).Get() == flat.Get());
    CHECK(r.Read(2, 3).Get() == unseen.Get());
}

// The bind path while samplers are being created on other threads: no lock is taken anywhere (the test finishes), no bind ever
// sees a half-made entry, and once the creators are done and the evaluate seam has run, every eligible sampler is substituted.
static void BindsWhileOthersCreate()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);

    constexpr int kThreads = 4, kPer = 20;
    std::vector<std::vector<ComPtr<ID3D11SamplerState>>> made(kThreads);
    for (int k = 0; k < kThreads; ++k)
        for (int i = 0; i < kPer; ++i)
        {
            auto& v = made[k];
            D3D11_SAMPLER_DESC d = Desc(-0.1f - 0.01f * static_cast<float>(k * kPer + i));
            d.MaxAnisotropy = 1;
            ComPtr<ID3D11SamplerState> s;
            r.device->CreateSamplerState(&d, &s);
            v.push_back(s);
        }

    std::atomic<bool> go { false };
    std::vector<std::thread> pool;
    for (int k = 0; k < kThreads; ++k)
        pool.emplace_back(
            [&, k]
            {
                while (!go.load())
                    std::this_thread::yield();
                for (auto& s : made[k])
                {
                    D3D11_SAMPLER_DESC d {};
                    s->GetDesc(&d);
                    r.engine.NoteCreated(s.Get(), r.device.Get(), d, d, IsEligible(d.MipLODBias, d.MinLOD, d.MaxLOD, d.Filter));
                }
            });

    go = true;
    for (int round = 0; round < 200; ++round) // the game binds the same samplers while they are being created
        for (int k = 0; k < kThreads; ++k)
            r.Bind(0, static_cast<UINT>(k), made[k][round % kPer].Get());
    for (auto& th : pool)
        th.join();

    r.engine.Service();
    for (int k = 0; k < kThreads; ++k)
        for (int i = 0; i < kPer; ++i)
        {
            auto* v = r.engine.VariantFor(made[k][i].Get());
            CHECK(v != nullptr && v != made[k][i].Get());
        }
    CHECK(r.engine.table().Count() <= kTableCapacity);
}
static void SharedVariantsUnderReplace()
{
    // REPLACE applies only the change since the sampler was made. A sampler made at DLAA with -1.0 and the game's own sampler
    // made at Quality with exactly the value REPLACE gives it (-1.0 + the Quality base) are the same description: the runtime
    // hands back one object for both.
    Rig r;
    CHECK(r.Init());
    constexpr double kQ = -0.584963;
    auto art1 = r.Make(-1.0f);  // made at DLAA (no target yet: native resolution)
    auto art2 = r.Make(-2.0f);
    r.engine.SetTarget(kQ, 0.0, Mode::Replace);
    r.engine.SetActive(true);
    auto game = r.Make(static_cast<float>(-1.0 + kQ)); // the game''s own value at Quality, made at Quality
    r.engine.Refresh();
    r.Bind(0, 0, art1.Get());
    r.Bind(0, 1, art2.Get());
    r.Bind(0, 2, game.Get());

    // art1''s variant is the game''s own Quality sampler; art2 gets its own; the game''s sampler is bound as it is
    CHECK(r.Bound(0, 0).Get() == game.Get());
    CHECK(r.Bound(0, 1).Get() != art2.Get() && Near(BiasOf(r.Bound(0, 1).Get()), -2.584963));
    CHECK(r.Bound(0, 2).Get() == game.Get());
    // each slot still reads back its own sampler (a variant-to-source map could not do this)
    CHECK(r.Read(0, 0).Get() == art1.Get());
    CHECK(r.Read(0, 1).Get() == art2.Get());
    CHECK(r.Read(0, 2).Get() == game.Get()); // bound directly by the game: not translated
    CHECK(r.engine.SubstitutedBinds() == 2);
}
static void NewTargetAndRefresh()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-0.5, 0.0, Mode::Add);
    r.engine.SetActive(true);
    auto art = r.Make(-1.0f);
    auto flat = r.Make(0.0f);
    r.Bind(0, 0, art.Get());
    r.Bind(0, 1, flat.Get());
    r.Bind(4, 7, art.Get()); // another stage
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -1.5));

    // the target moves: the old variant stays bound until Refresh, then the new one replaces it in every stage
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -1.5));
    // a bind of the same sampler after the move, before any Refresh, gets the game's sampler: variants are made off the bind path,
    // and a stale one (-1.5) is never bound again
    r.Bind(0, 0, art.Get());
    CHECK(r.Bound(0, 0).Get() == art.Get());
    r.engine.Refresh();
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -2.0));
    CHECK(Near(BiasOf(r.Bound(4, 7).Get()), -2.0));
    CHECK(r.Bound(0, 1).Get() == flat.Get());
    CHECK(r.Read(0, 0).Get() == art.Get() && r.Read(4, 7).Get() == art.Get());

    // a mode change is a new revision too: the sampler was made at base -0.5, so REPLACE applies the change since (-1.0 - -0.5)
    r.engine.SetTarget(-1.0, 0.0, Mode::Replace);
    r.engine.Refresh();
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -1.5));
    CHECK(r.Read(0, 0).Get() == art.Get());

    // DLAA under add: the variant would equal the game's bias, so the game's sampler is bound again
    r.engine.SetTarget(0.0, 0.0, Mode::Add);
    r.engine.Refresh();
    CHECK(r.Bound(0, 0).Get() == art.Get() && r.Bound(4, 7).Get() == art.Get());
    CHECK(r.Read(0, 0).Get() == art.Get());

    // a sampler bound before substitution started is taken over by Refresh
    Rig q;
    CHECK(q.Init());
    auto early = q.Make(-1.0f);
    ID3D11SamplerState* e = early.Get();
    q.set[1](q.context.Get(), 5, 1, &e); // bound natively, before the engine is active
    q.engine.SetTarget(-1.0, 0.0, Mode::Add);
    q.engine.SetActive(true);
    q.engine.Refresh();
    CHECK(q.Bound(1, 5).Get() != early.Get() && Near(BiasOf(q.Bound(1, 5).Get()), -2.0));
    CHECK(q.Read(1, 5).Get() == early.Get());
}

static void ClearStateAndForeignBinds()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);
    auto art = r.Make(-1.0f);
    auto other = r.Make(-3.0f);
    r.Bind(0, 0, art.Get());
    CHECK(r.Bound(0, 0).Get() != art.Get());

    // ClearState empties the context behind the hooks' back: the game reads back null, and binding again works
    r.context->ClearState();
    CHECK(!r.Read(0, 0));
    r.Bind(0, 0, art.Get());
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -2.0));
    CHECK(r.Read(0, 0).Get() == art.Get());

    // something else binds a sampler natively (a restored command list): it is read back as it is, and Refresh follows it
    ID3D11SamplerState* o = other.Get();
    r.set[0](r.context.Get(), 0, 1, &o);
    CHECK(r.Read(0, 0).Get() == other.Get());
    r.engine.SetTarget(-1.5, 0.0, Mode::Add);
    r.engine.Refresh();
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -4.5)); // the foreign bind is now treated as the game's request
    CHECK(r.Read(0, 0).Get() == other.Get());
}

static void SwitchingOff()
{
    Rig r;
    CHECK(r.Init());
    r.engine.SetTarget(-1.0, 0.0, Mode::Add);
    r.engine.SetActive(true);
    auto art = r.Make(-1.0f);
    auto flat = r.Make(0.0f);
    r.Bind(0, 0, art.Get());
    r.Bind(3, 9, art.Get());
    r.Bind(0, 1, flat.Get());
    CHECK(r.engine.AnySubstituted());

    r.engine.SetActive(false);
    CHECK(r.Bound(0, 0).Get() == art.Get() && r.Bound(3, 9).Get() == art.Get() && r.Bound(0, 1).Get() == flat.Get());
    CHECK(!r.engine.AnySubstituted());
    CHECK(r.Read(0, 0).Get() == art.Get());

    // and on again
    r.engine.SetActive(true);
    r.engine.Refresh();
    CHECK(Near(BiasOf(r.Bound(0, 0).Get()), -2.0));
}

// The marker that tells the evaluate seam whether a context is the hooked one: a stamped immediate context matches itself,
// not another device's immediate context, a deferred context cannot be stamped, and a wrapper that forwards private data to
// the native context (what ReShade's context does) is the same as asking the native context.
static void ContextIdentity()
{
    Rig a, b;
    CHECK(a.Init() && b.Init());

    const unsigned long long token = ContextMarker::Stamp(a.context.Get());
    CHECK(token != 0);
    CHECK(ContextMarker::Matches(a.context.Get(), token));
    CHECK(!ContextMarker::Matches(b.context.Get(), token)); // another device's immediate context
    CHECK(!ContextMarker::Matches(nullptr, token));
    CHECK(!ContextMarker::Matches(a.context.Get(), 0));
    CHECK(!ContextMarker::Matches(a.context.Get(), token + 1)); // a stale token

    // the same object seen through another interface pointer (QueryInterface returns the same marker store)
    ComPtr<ID3D11DeviceContext1> viaInterface;
    if (SUCCEEDED(a.context.As(&viaInterface)))
        CHECK(ContextMarker::Matches(viaInterface.Get(), token));

    // a second stamp (another install) replaces the first one
    const unsigned long long second = ContextMarker::Stamp(a.context.Get());
    CHECK(second != 0 && second != token);
    CHECK(ContextMarker::Matches(a.context.Get(), second) && !ContextMarker::Matches(a.context.Get(), token));

    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(SUCCEEDED(a.device->CreateDeferredContext(0, &deferred)));
    CHECK(ContextMarker::Stamp(deferred.Get()) == 0);
    CHECK(!ContextMarker::Matches(deferred.Get(), second));
}

int main()
{
    ContextIdentity();
    SixStages();
    NullsAndRanges();
    PassThrough();
    DeferredContext();
    SharedVariantsUnderReplace();
    LazyClassificationThroughSet();
    BindsWhileOthersCreate();
    NewTargetAndRefresh();
    ClearStateAndForeignBinds();
    SwitchingOff();

    if (g_failed == 0)
        std::printf("mip_bind_bind: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
