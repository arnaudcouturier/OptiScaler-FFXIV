// [PACE] DXGI contract the hidden-present switch relies on, on a real swapchain (WARP device, hidden window): a present with
// DXGI_PRESENT_DO_NOT_WAIT is ACCEPTED by flip-discard, flip-sequential and the older discard / sequential effects, with or without
// the other flags the bridge forwards. The only results allowed are a success code or DXGI_ERROR_WAS_STILL_DRAWING; an
// invalid-argument or invalid-call answer would mean the flag must not be sent (the policy latches it off then).
// This is NOT a proof for the game's own chain (its desc comes from the game); it only shows the flag is legal on these chains.
// Informational output: how many presents answered WAS_STILL_DRAWING and the slowest present, per chain.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace
{
int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

struct Case
{
    const char* name;
    DXGI_SWAP_EFFECT effect;
    UINT bufferCount;
    UINT chainFlags;
    UINT presentFlags;
    UINT syncInterval;
};

// Returns false when the host could not create this swapchain (nothing was asserted); main requires at least one case to have run.
bool RunCase(IDXGIFactory2* factory, ID3D11Device* device, HWND hwnd, const Case& c)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 640;
    desc.Height = 360;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = c.bufferCount;
    desc.SwapEffect = c.effect;
    desc.Flags = c.chainFlags;

    ComPtr<IDXGISwapChain1> chain;
    const HRESULT created = factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &chain);
    if (FAILED(created))
    {
        std::printf("  [%s] swapchain not created on this host (0x%08X): case skipped\n", c.name, static_cast<unsigned>(created));
        return false;
    }

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    int accepted = 0, stillDrawing = 0, rejected = 0;
    double slowestMs = 0;
    for (int i = 0; i < 300; ++i)
    {
        // draw something so the frame is not a no-op for the driver
        ComPtr<ID3D11Texture2D> back;
        if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))))
        {
            ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv)))
            {
                const float color[4] = { (i % 7) / 7.0f, 0.2f, 0.4f, 1.0f };
                context->ClearRenderTargetView(rtv.Get(), color);
            }
        }

        const auto t0 = std::chrono::steady_clock::now();
        const HRESULT hr = chain->Present(c.syncInterval, c.presentFlags | DXGI_PRESENT_DO_NOT_WAIT);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > slowestMs)
            slowestMs = ms;

        if (hr == E_INVALIDARG || hr == DXGI_ERROR_INVALID_CALL)
            ++rejected;
        else if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            ++stillDrawing;
        else if (SUCCEEDED(hr))
            ++accepted;
        else
        {
            std::printf("  [%s] unexpected present result 0x%08X at %d\n", c.name, static_cast<unsigned>(hr), i);
            ++g_failed;
        }
    }
    std::printf("  [%s] 300 presents: accepted %d, still drawing %d, rejected %d, slowest %.3f ms\n", c.name, accepted, stillDrawing,
                rejected, slowestMs);
    CHECK(rejected == 0);              // DO_NOT_WAIT is legal on this chain
    CHECK(accepted + stillDrawing == 300); // nothing else came back
    CHECK(accepted > 0);               // the chain is usable (not every present refused)
    return true;
}
} // namespace

int main()
{
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"OptiScaler hidden present test", WS_POPUP, 0, 0, 640, 360, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    CHECK(hwnd != nullptr); // needs a desktop session; mark the recipe manual on a host without one
    if (hwnd == nullptr)
        return 1;
    ShowWindow(hwnd, SW_HIDE);

    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    if (!device)
        return 1;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CHECK(SUCCEEDED(device.As(&dxgiDevice)));
    CHECK(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    CHECK(SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))));
    if (!factory)
        return 1;

    BOOL tearing = FALSE;
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    }
    std::printf("  tearing support on this host: %s\n", tearing ? "yes" : "no");

    const Case cases[] = {
        { "flip discard, interval 0", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 0, 0, 0 },
        { "flip discard, interval 1", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 0, 0, 1 },
        { "flip sequential, interval 0", DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, 0, 0, 0 },
        { "discard, interval 0", DXGI_SWAP_EFFECT_DISCARD, 1, 0, 0, 0 },
        { "sequential, interval 0", DXGI_SWAP_EFFECT_SEQUENTIAL, 1, 0, 0, 0 },
        { "flip discard + RESTART, interval 0", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 0, DXGI_PRESENT_RESTART, 0 },
    };
    int attempted = 0, executed = 0;
    for (const Case& c : cases)
    {
        ++attempted;
        executed += RunCase(factory.Get(), device.Get(), hwnd, c) ? 1 : 0;
    }
    if (tearing)
    {
        const Case tear = { "flip discard + ALLOW_TEARING, interval 0", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2,
                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, DXGI_PRESENT_ALLOW_TEARING, 0 };
        ++attempted;
        executed += RunCase(factory.Get(), device.Get(), hwnd, tear) ? 1 : 0;
    }
    std::printf("  cases executed: %d of %d\n", executed, attempted);
    // A host that cannot create any of the swapchains would otherwise pass with zero assertions about DO_NOT_WAIT. Fail instead: the
    // recipe then needs a host that can (or to be marked manual by a person who read this).
    CHECK(executed > 0);

    DestroyWindow(hwnd);
    if (g_failed != 0)
    {
        std::printf("hidden_present_dxgi: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("hidden_present_dxgi: all passed\n");
    return 0;
}
