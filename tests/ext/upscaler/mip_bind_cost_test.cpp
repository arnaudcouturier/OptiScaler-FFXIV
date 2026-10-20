// [MIP-BIND] What the bind path costs (ext/upscaler/MipBindEngine.h, Engine::Set). The native is a no-op, so the numbers are
// the engine's own work. Three cases for a call that binds 8 slots: the engine off (a compare and the native), the same
// samplers again (the per-slot record hits), and samplers that changed since the last call (a table lookup per slot, the
// variants already made). The test fails only when a call costs more than a generous bound, so a slow machine does not
// make it flaky; the printed numbers go into the log of the work.
#include "ext/upscaler/MipBindEngine.h"

#include <chrono>
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

static volatile int g_sink = 0;
static void STDMETHODCALLTYPE NoopSet(ID3D11DeviceContext*, UINT, UINT count, ID3D11SamplerState* const* in)
{
    g_sink = g_sink + static_cast<int>(count) + (in != nullptr ? 1 : 0);
}
static void STDMETHODCALLTYPE NoopGet(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**) {}

template <class F> static double NsPerCall(F&& f, int iterations)
{
    for (int i = 0; i < iterations / 10; ++i)
        f(i); // warm up
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i)
        f(i);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iterations;
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context)))
    {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }

    Engine engine;
    void** devVt = *reinterpret_cast<void***>(device.Get());
    engine.SetCreateFn(reinterpret_cast<CreateSamplerFn>(devVt[kDeviceCreateSamplerStateSlot]));
    std::array<SetSamplersFn, kStages> set;
    std::array<GetSamplersFn, kStages> get;
    set.fill(&NoopSet);
    get.fill(&NoopGet);
    engine.Attach(context.Get(), set, get);

    // 32 eligible samplers: four sets of eight to alternate between
    std::vector<ComPtr<ID3D11SamplerState>> samplers;
    for (int i = 0; i < 32; ++i)
    {
        D3D11_SAMPLER_DESC d {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        d.MipLODBias = -0.1f - 0.1f * static_cast<float>(i);
        d.MaxAnisotropy = 1;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> s;
        device->CreateSamplerState(&d, &s);
        engine.table().OnCreated(s.Get(), d, d.MipLODBias, true);
        samplers.push_back(s);
    }
    ID3D11SamplerState* sets[4][8];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 8; ++i)
            sets[k][i] = samplers[k * 8 + i].Get();

    engine.SetTarget(-0.584963, Mode::Add);
    // make every variant for this revision once
    engine.SetActive(true);
    for (int k = 0; k < 4; ++k)
        engine.Set(0, context.Get(), 0, 8, sets[k]);

    constexpr int kIter = 400000;
    ID3D11DeviceContext* ctx = context.Get();

    engine.SetActive(false);
    const double off = NsPerCall([&](int i) { engine.Set(0, ctx, 0, 8, sets[i & 3]); }, kIter);

    engine.SetActive(true);
    engine.Set(0, ctx, 0, 8, sets[0]);
    const double hits = NsPerCall([&](int) { engine.Set(0, ctx, 0, 8, sets[0]); }, kIter);

    const double misses = NsPerCall([&](int i) { engine.Set(0, ctx, 0, 8, sets[i & 3]); }, kIter);

    const double gets = NsPerCall([&](int) { ID3D11SamplerState* out[8] = {}; engine.Get(0, ctx, 0, 8, out); }, kIter);

    std::printf("mip_bind_cost: engine off %.1f ns/call; same samplers (record hits) %.1f ns/call = %.1f ns/slot; changed "
                "samplers (table lookups) %.1f ns/call = %.1f ns/slot; GetSamplers of 8 slots %.1f ns/call\n",
                off, hits, hits / 8, misses, misses / 8, gets);

    CHECK(off < 200.0);
    CHECK(hits < 500.0);
    CHECK(misses < 2000.0);
    CHECK(gets < 500.0);
    CHECK(engine.counters().variantsCreated.load() == 32); // no variant was made on the bind path after the warm-up

    if (g_failed == 0)
        std::printf("mip_bind_cost: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
