// The probe's reference to a derived texture (CharaProbeHold.h) on D3D11 WARP: an ordinary texture is held (one reference, released with the
// holder), a buffer of a swapchain is never held (a reference we hold would make the game's ResizeBuffers fail), and ResizeBuffers really
// succeeds while the probe holds an ordinary texture but would fail while it held a buffer.
#include "check.h"
#include "pch.h"

#include "ext/ffxiv/chara/CharaProbeHold.h"

#include <dxgi1_2.h>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara::probe;

namespace
{
ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

int Run()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
    {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }

    // an ordinary texture
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 32;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> plain;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &plain)));
    CHECK(!IsSwapchainBuffer(plain.Get()));
    const ULONG before = RefCount(plain.Get());
    {
        const std::shared_ptr<void> hold = HoldOf(plain.Get(), false);
        CHECK(hold != nullptr && RefCount(plain.Get()) == before + 1); // exactly one reference of its own
        const std::shared_ptr<void> copy = hold;                         // copies of the holder share it
        CHECK(RefCount(plain.Get()) == before + 1);
    }
    CHECK(RefCount(plain.Get()) == before); // released with the last holder
    CHECK(HoldOf(plain.Get(), true) == nullptr && HoldOf(nullptr, false) == nullptr); // the reported backbuffer, or nothing: not held

    // a swapchain (composition: no window needed)
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CHECK(SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))));
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = sd.Height = 64;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> chain;
    const HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &chain);
    CHECK(SUCCEEDED(hr)); // the case must exist: without a swapchain the flag below is unverified
    if (chain == nullptr)
        return g_failures == 0 ? 0 : 1;

    ComPtr<ID3D11Texture2D> buffer0;
    CHECK(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer0))));
    CHECK(buffer0 != nullptr && IsSwapchainBuffer(buffer0.Get())); // DXGI marks it as a back buffer
    CHECK(HoldOf(buffer0.Get(), false) == nullptr);                 // so it is never held, even when the caller did not recognise it

    // the point of it all: while a reference to a swapchain buffer is held the chain cannot be resized, without one it can
    {
        const ULONG refs = RefCount(buffer0.Get());
        buffer0.Reset();
        (void) refs;
        const HRESULT resized = chain->ResizeBuffers(2, 96, 96, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        CHECK(SUCCEEDED(resized)); // nothing of ours holds a buffer: the resize works
    }
    ComPtr<ID3D11Texture2D> buffer1;
    CHECK(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer1))));
    CHECK(IsSwapchainBuffer(buffer1.Get()));
    const HRESULT blocked = chain->ResizeBuffers(2, 128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0); // buffer1 is still referenced here
    CHECK(FAILED(blocked)); // what a held reference would cause: DXGI_ERROR_INVALID_CALL
    buffer1.Reset();
    CHECK(SUCCEEDED(chain->ResizeBuffers(2, 128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0)));
    return g_failures == 0 ? 0 : 1;
}
} // namespace

int main()
{
    Run();
    return TEST_RESULT("chara_probe_hold");
}
