// [MIP-BIND] The bind-time mip bias through real Detours hooks. The install below is the one of MipBindHooks.cpp (the six
// SetSamplers and GetSamplers of the immediate context first, the device's CreateSamplerState last, thunks that call the
// engine), run on a WARP device, so the game's own calls (`context->PSSetSamplers(...)`, `device->CreateSamplerState(...)`)
// take the whole route: vtable -> patched code -> thunk -> engine -> trampoline -> native.
//
// Upstream's own hook on CreateSamplerState is stood in for by a counting layer that is attached BEFORE ours (upstream attaches
// when the first device is hooked, ours at the first upscaler create) and detached AFTER ours (D3D11Hooks::Unhook runs after
// ext::game::OnD3D11Unhook). Detours transactions on one target only restore correctly in the reverse order of the attaches.
#include "ext/upscaler/MipBindEngine.h"

#include <detours/detours.h>

#include <cmath>
#include <cstdio>

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

static Engine g_engine;
static std::atomic<bool> g_ready { false };
static CreateSamplerFn g_createTramp = nullptr; // trampoline of our create hook
static std::array<SetSamplersFn, kStages> g_set {};
static std::array<GetSamplersFn, kStages> g_get {};
static int g_layerCalls = 0;
static bool g_inVariant = false;

template <unsigned S> static void STDMETHODCALLTYPE HkSet(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11SamplerState* const* in)
{
    if (!g_ready.load())
        g_set[S](c, a, n, in);
    else
        g_engine.Set(S, c, a, n, in);
}
template <unsigned S> static void STDMETHODCALLTYPE HkGet(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11SamplerState** out)
{
    if (!g_ready.load())
        g_get[S](c, a, n, out);
    else
        g_engine.Get(S, c, a, n, out);
}

static HRESULT STDMETHODCALLTYPE HkCreate(ID3D11Device* d, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    const HRESULT hr = g_createTramp(d, desc, out);
    if (SUCCEEDED(hr) && desc && out && *out && !g_inVariant)
        g_engine.NoteCreated(*out, d, *desc, *desc, IsEligible(desc->MipLODBias, desc->MinLOD, desc->MaxLOD, desc->Filter));
    return hr;
}

static CreateSamplerFn g_variantCreate = nullptr; // what the engine makes variants with
static HRESULT STDMETHODCALLTYPE CreateVariant(ID3D11Device* d, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    g_inVariant = true; // stands for ext::core::BypassScope
    const HRESULT hr = g_variantCreate(d, desc, out);
    g_inVariant = false;
    return hr;
}

// Stands in for upstream's hkCreateSamplerState.
static CreateSamplerFn g_layerNext = nullptr;
static HRESULT STDMETHODCALLTYPE HkLayer(ID3D11Device* d, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    ++g_layerCalls;
    return g_layerNext(d, desc, out);
}

static D3D11_SAMPLER_DESC Desc(float bias)
{
    D3D11_SAMPLER_DESC d {};
    d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MipLODBias = bias;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    return d;
}

static float BiasOf(ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return d.MipLODBias;
}

static constexpr std::array<SetSamplersFn, kStages> kSetHooks = { &HkSet<0>, &HkSet<1>, &HkSet<2>, &HkSet<3>, &HkSet<4>, &HkSet<5> };
static constexpr std::array<GetSamplersFn, kStages> kGetHooks = { &HkGet<0>, &HkGet<1>, &HkGet<2>, &HkGet<3>, &HkGet<4>, &HkGet<5> };

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

    void** const deviceTable = *reinterpret_cast<void***>(device.Get());
    void** const contextTable = *reinterpret_cast<void***>(context.Get());
    const CreateSamplerFn deviceCreate = reinterpret_cast<CreateSamplerFn>(deviceTable[kDeviceCreateSamplerStateSlot]);
    g_variantCreate = deviceCreate;
    for (unsigned s = 0; s < kStages; ++s)
    {
        g_set[s] = reinterpret_cast<SetSamplersFn>(contextTable[kContextSetSamplersSlots[s]]);
        g_get[s] = reinterpret_cast<GetSamplersFn>(contextTable[kContextGetSamplersSlots[s]]);
    }

    // ---- 1. upstream's layer on CreateSamplerState (first device hooked) ----------------------------------------------
    g_layerNext = deviceCreate;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    DetourUpdateThread(GetCurrentThread());
    CHECK(DetourAttach(reinterpret_cast<PVOID*>(&g_layerNext), HkLayer) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    // ---- 2. the bind hooks (device hooked seam) ------------------------------------------------------------------------
    CHECK(DetourTransactionBegin() == NO_ERROR);
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    for (unsigned s = 0; s < kStages; ++s)
    {
        ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_set[s]), kSetHooks[s]) == NO_ERROR;
        ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_get[s]), kGetHooks[s]) == NO_ERROR;
    }
    CHECK(ok);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    g_engine.SetCreateFn(&CreateVariant);
    g_engine.Attach(context.Get(), g_set, g_get);
    g_ready = true;

    // a sampler made before our create hook exists is not classified (and goes through upstream's layer)
    const D3D11_SAMPLER_DESC art = Desc(-1.0f), flat = Desc(0.0f);
    ComPtr<ID3D11SamplerState> before;
    CHECK(SUCCEEDED(device->CreateSamplerState(&art, &before)));
    CHECK(g_layerCalls == 1 && g_engine.table().Count() == 0);

    // ---- 3. our create hook, attached last (first upscaler create) ---------------------------------------------------
    g_createTramp = deviceCreate;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    DetourUpdateThread(GetCurrentThread());
    CHECK(DetourAttach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreate) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    g_variantCreate = g_createTramp; // below both hooks

    // ---- the game's own calls ----------------------------------------------------------------------------------------
    g_layerCalls = 0;
    ComPtr<ID3D11SamplerState> artSampler, flatSampler;
    CHECK(SUCCEEDED(device->CreateSamplerState(&art, &artSampler)));
    CHECK(SUCCEEDED(device->CreateSamplerState(&flat, &flatSampler)));
    CHECK(g_layerCalls == 2);             // upstream's layer is in the path of both creates
    CHECK(g_engine.table().Count() == 1); // only the negative-bias one is classified
    CHECK(g_engine.table().Find(artSampler.Get()) != nullptr);
    CHECK(g_engine.table().Find(flatSampler.Get()) == nullptr);

    g_engine.SetTarget(-0.584963, 0.0, Mode::Add);
    g_engine.SetActive(true);
    g_engine.Refresh();

    // each of the six stages, through the ordinary D3D11 entry points
    ID3D11SamplerState* in[2] = { artSampler.Get(), flatSampler.Get() };
    context->PSSetSamplers(1, 2, in);
    context->VSSetSamplers(1, 2, in);
    context->GSSetSamplers(1, 2, in);
    context->HSSetSamplers(1, 2, in);
    context->DSSetSamplers(1, 2, in);
    context->CSSetSamplers(1, 2, in);

    using NativeGet = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**);
    const NativeGet nativeGet[kStages] = { g_get[0], g_get[1], g_get[2], g_get[3], g_get[4], g_get[5] }; // trampolines: no translation
    ComPtr<ID3D11SamplerState> viaGame[kStages][2], viaNative[kStages][2];
    context->PSGetSamplers(1, 2, viaGame[0][0].GetAddressOf());
    context->VSGetSamplers(1, 2, viaGame[1][0].GetAddressOf());
    context->GSGetSamplers(1, 2, viaGame[2][0].GetAddressOf());
    context->HSGetSamplers(1, 2, viaGame[3][0].GetAddressOf());
    context->DSGetSamplers(1, 2, viaGame[4][0].GetAddressOf());
    context->CSGetSamplers(1, 2, viaGame[5][0].GetAddressOf());
    for (unsigned s = 0; s < kStages; ++s)
    {
        nativeGet[s](context.Get(), 1, 2, viaNative[s][0].GetAddressOf());
        CHECK(viaGame[s][0].Get() == artSampler.Get()); // the game reads back its own sampler
        CHECK(viaGame[s][1].Get() == flatSampler.Get());
        CHECK(viaNative[s][0] && viaNative[s][0].Get() != artSampler.Get() && Near(BiasOf(viaNative[s][0].Get()), -1.584963));
        CHECK(viaNative[s][1].Get() == flatSampler.Get()); // what is really bound
    }
    CHECK(g_engine.SubstitutedBinds() == kStages);

    // The variant was made through our trampoline. Our hook was attached last, so it sits above upstream's layer and the
    // trampoline leads into the layer: the variant passes through upstream's hook (the product makes it inside a bypass scope,
    // where that hook passes the call on) but is NOT classified as a game sampler.
    CHECK(g_layerCalls >= 3);
    CHECK(g_engine.table().Count() == 1);

    // a new target reaches the bound samplers of every stage at once
    g_engine.SetTarget(-2.0, 0.0, Mode::Replace);
    g_engine.Refresh();
    ComPtr<ID3D11SamplerState> after;
    nativeGet[5](context.Get(), 1, 1, after.GetAddressOf());
    CHECK(after && Near(BiasOf(after.Get()), -3.0)); // replace: -1.0 + (-2.0 - 0), the sampler was made at base 0
    ComPtr<ID3D11SamplerState> game;
    context->CSGetSamplers(1, 1, game.GetAddressOf());
    CHECK(game.Get() == artSampler.Get());

    // switching off puts the game's samplers back on the context
    g_engine.SetActive(false);
    ComPtr<ID3D11SamplerState> back;
    nativeGet[0](context.Get(), 1, 1, back.GetAddressOf());
    CHECK(back.Get() == artSampler.Get());

    // ---- detach in the reverse order of the attaches: our create hook, the bind hooks, upstream's layer ------------------
    g_ready = false;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    DetourUpdateThread(GetCurrentThread());
    CHECK(DetourDetach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreate) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    g_variantCreate = deviceCreate;

    // upstream's layer is still in place and still works
    g_layerCalls = 0;
    ComPtr<ID3D11SamplerState> viaLayer;
    CHECK(SUCCEEDED(device->CreateSamplerState(&art, &viaLayer)));
    CHECK(g_layerCalls == 1);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    for (unsigned s = 0; s < kStages; ++s)
    {
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_set[s]), kSetHooks[s]) == NO_ERROR;
        ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_get[s]), kGetHooks[s]) == NO_ERROR;
    }
    CHECK(ok);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    DetourUpdateThread(GetCurrentThread());
    CHECK(DetourDetach(reinterpret_cast<PVOID*>(&g_layerNext), HkLayer) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    // plain D3D11 again: no patch left on any of the functions
    g_layerCalls = 0;
    ComPtr<ID3D11SamplerState> plain;
    CHECK(SUCCEEDED(device->CreateSamplerState(&art, &plain)));
    CHECK(g_layerCalls == 0);
    context->PSSetSamplers(0, 1, plain.GetAddressOf());
    ComPtr<ID3D11SamplerState> readBack;
    context->PSGetSamplers(0, 1, readBack.GetAddressOf());
    CHECK(readBack.Get() == plain.Get());
    CHECK(g_engine.SubstitutedBinds() == kStages); // the hooks no longer reach the engine

    // the first bytes of the device function are not a jump any more (a leftover patch would show here)
    const unsigned char* t = reinterpret_cast<const unsigned char*>(deviceCreate);
    CHECK(t[0] != 0xE9);

    if (g_failed == 0)
        std::printf("mip_bind_detours: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
