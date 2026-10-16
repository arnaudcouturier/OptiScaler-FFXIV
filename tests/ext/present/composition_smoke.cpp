// E5 (manual): composition swapchain contract the CreateSwapChainForComposition hook and the wrapper rely on.
// Ported from the oracle's tests/dxgi_window_size_smoke.cpp composition half (its 0x0-to-window checks are not carried,
// SEAMS.md section 5). Needs a desktop session (hidden popup windows, DirectComposition) and a WARP-capable machine; it
// injects nothing and shows no UI. It does NOT exercise the hook itself (that needs OptiScaler in a running game); it
// checks the helpers in ext/present/CompositionWindow.h and the DXGI behaviour the wrapper assumes:
//   - IsCompositionWindow accepts a hidden 640x360 popup of this process, rejects a 32x32 one, null and (invalid) HWNDs;
//   - FindCompositionWindow returns the unique eligible window, and nullptr when two are eligible (never guesses);
//   - a composition swapchain has no native HWND (IDXGISwapChain::GetHwnd fails), attaches to a DirectComposition
//     visual and keeps FLIP_SEQUENTIAL / Flags == 0 across ResizeBuffers (why the wrapper skips the OverrideVsync rewrite).
// Build line (from a vcvars64 prompt, repo root):
//   cl /nologo /std:c++20 /EHsc /W4 /I OptiScaler tests\ext\present\composition_smoke.cpp d3d11.lib dxgi.lib dcomp.lib user32.lib
#include "ext/present/CompositionWindow.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
void expect(bool ok, const char* reason)
{
    if (!ok)
        throw std::runtime_error(reason);
}

void check(HRESULT hr) { expect(SUCCEEDED(hr), "DXGI/D3D11/DComp call failed"); }

struct Window
{
    HWND hwnd;
    Window(int width, int height)
        : hwnd(CreateWindowExW(0, L"STATIC", L"OptiScaler composition test", WS_POPUP, 0, 0, width, height, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr))
    {
        expect(hwnd != nullptr, "hidden test window creation failed");
    }
    ~Window() { DestroyWindow(hwnd); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};
} // namespace

int main()
try
{
    Window game(640, 360), tiny(32, 32);

    expect(ext::present::IsCompositionWindow(game.hwnd), "own hidden composition window rejected");
    expect(!ext::present::IsCompositionWindow(tiny.hwnd) && !ext::present::IsCompositionWindow(nullptr),
           "invalid composition window accepted");
    expect(!ext::present::IsCompositionWindow(reinterpret_cast<HWND>(static_cast<INT_PTR>(-1))), "invalid HWND accepted");

    {
        // One eligible window: found (unless the foreground window qualifies first, which can only be ours).
        const HWND found = ext::present::FindCompositionWindow();
        expect(found == game.hwnd, "unique eligible window not found");

        // Two eligible windows: ambiguous, must not guess.
        Window second(800, 450);
        expect(ext::present::FindCompositionWindow() == nullptr, "ambiguous windows resolved");
    }

    // Verify the actual composition contract with WARP, without injecting into a game or showing UI.
    ComPtr<ID3D11Device> device;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &device, nullptr, nullptr));
    ComPtr<IDXGIDevice> dxgiDevice;
    check(device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    check(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIFactory> factory;
    check(adapter->GetParent(IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIFactory2> factory2;
    check(factory.As(&factory2));
    DXGI_SWAP_CHAIN_DESC1 composition {};
    composition.Width = 640;
    composition.Height = 360;
    composition.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    composition.SampleDesc.Count = 1;
    composition.BufferCount = 2;
    composition.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    composition.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    composition.Scaling = DXGI_SCALING_STRETCH;
    composition.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> chain;
    check(factory2->CreateSwapChainForComposition(device.Get(), &composition, nullptr, &chain));

    ComPtr<IDCompositionDevice> compositor;
    check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&compositor)));
    ComPtr<IDCompositionTarget> target;
    check(compositor->CreateTargetForHwnd(game.hwnd, TRUE, &target));
    ComPtr<IDCompositionVisual> visual;
    check(compositor->CreateVisual(&visual));
    check(visual->SetContent(chain.Get()));
    check(target->SetRoot(visual.Get()));
    check(compositor->Commit());

    HWND nativeWindow = nullptr;
    expect(FAILED(chain->GetHwnd(&nativeWindow)), "composition unexpectedly has native HWND");

    check(chain->ResizeBuffers(2, 800, 450, DXGI_FORMAT_UNKNOWN, 0));
    DXGI_SWAP_CHAIN_DESC1 resized {};
    check(chain->GetDesc1(&resized));
    expect(resized.Width == 800 && resized.Height == 450 && resized.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL &&
               resized.Flags == 0,
           "composition resize changed contract");

    std::puts("PASS: composition window helpers, no native HWND, DirectComposition attach, resize keeps the descriptor (WARP)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
