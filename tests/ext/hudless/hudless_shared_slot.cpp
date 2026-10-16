// S10 package B (warp): the shared D3D11 -> D3D12 texture of the hud-less slot ring (ext/hudless/SharedSlot.cpp). A D3D11
// WARP device writes a known pattern into the slot with CopyResource (typeless source into the typed slot, as the engine
// does with the game's capture), a D3D12 WARP device opens the same memory, and the pattern is read back through a D3D12
// copy. Also checks what the mask / paste shaders need (shader-resource access) and that a retired slot is independent
// of its successor.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ext/hudless/SharedSlot.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        std::printf("FAIL: %s (%08lX)\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

void Require(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 32;

uint32_t Pixel(UINT x, UINT y, uint32_t salt) { return 0xFF000000u | ((x * 3 + salt) & 0xFF) | (((y * 5 + salt) & 0xFF) << 8) | (((x ^ y) & 0xFF) << 16); }

struct Devices
{
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
};

Devices Create()
{
    Devices d;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.d3d11, nullptr,
                            &d.context),
          "D3D11 WARP device");
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.d3d12)), "D3D12 WARP device");
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(d.d3d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue)), "queue");
    Check(d.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.allocator)), "allocator");
    Check(d.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocator.Get(), nullptr, IID_PPV_ARGS(&d.list)),
          "list");
    Check(d.list->Close(), "close");
    Check(d.d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)), "fence");
    d.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return d;
}

void WaitForD3D11(Devices& d)
{
    D3D11_QUERY_DESC query {};
    query.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> done;
    Check(d.d3d11->CreateQuery(&query, &done), "event query");
    d.context->End(done.Get());
    d.context->Flush();
    BOOL signalled = FALSE;
    for (int i = 0; i < 10000 && !signalled; ++i)
    {
        if (d.context->GetData(done.Get(), &signalled, sizeof(signalled), 0) == S_OK)
            break;
        Sleep(1);
    }
    Require(signalled, "D3D11 work completed");
}

// Reads the whole slot back through D3D12 (explicit COMMON -> COPY_SOURCE, like the bridge's copy list)
std::vector<uint32_t> ReadBack(Devices& d, ID3D12Resource* slot)
{
    const D3D12_RESOURCE_DESC desc = slot->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    d.d3d12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(d.d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, IID_PPV_ARGS(&readback)),
          "readback buffer");

    Check(d.allocator->Reset(), "allocator reset");
    Check(d.list->Reset(d.allocator.Get(), nullptr), "list reset");
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = slot;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    src.pResource = slot;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    d.list->ResourceBarrier(1, &barrier);
    Check(d.list->Close(), "list close");

    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    Check(d.queue->Signal(d.fence.Get(), ++d.fenceValue), "signal");
    Check(d.fence->SetEventOnCompletion(d.fenceValue, d.event), "completion event");
    Require(WaitForSingleObject(d.event, 10000) == WAIT_OBJECT_0, "D3D12 copy completed");

    std::vector<uint32_t> pixels(kWidth * kHeight);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(total) };
    Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map");
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(&pixels[y * kWidth], mapped + footprint.Footprint.RowPitch * y, kWidth * sizeof(uint32_t));
    readback->Unmap(0, nullptr);
    return pixels;
}

ComPtr<ID3D11Texture2D> MakeSource(Devices& d, DXGI_FORMAT format, uint32_t salt)
{
    std::vector<uint32_t> pixels(kWidth * kHeight);
    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x)
            pixels[y * kWidth + x] = Pixel(x, y, salt);

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA init { pixels.data(), kWidth * 4, 0 };
    ComPtr<ID3D11Texture2D> texture;
    Check(d.d3d11->CreateTexture2D(&desc, &init, &texture), "source texture");
    return texture;
}
} // namespace

int main()
{
    Devices d = Create();
    using ext::hudless::CreateSharedTexture;
    using ext::hudless::SharedTexture;

    // 1. The slot: typed format, shader-resource access on the D3D12 side, same size
    SharedTexture slot;
    Check(CreateSharedTexture(d.d3d11.Get(), d.d3d12.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, slot),
          "CreateSharedTexture");
    const D3D12_RESOURCE_DESC opened = slot.resource->GetDesc();
    Require(opened.Width == kWidth && opened.Height == kHeight && opened.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
            "opened resource has the slot shape");
    Require((opened.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0, "slot is sampleable from D3D12");

    // 2. A typeless capture copies into the typed slot and the pattern arrives through D3D12
    {
        auto source = MakeSource(d, DXGI_FORMAT_R8G8B8A8_TYPELESS, 7);
        d.context->CopyResource(slot.texture.Get(), source.Get());
        WaitForD3D11(d);
        const auto pixels = ReadBack(d, slot.resource.Get());
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x)
                Require(pixels[y * kWidth + x] == Pixel(x, y, 7), "slot pixel equals the typeless capture");
    }

    // 3. Rewriting the slot with the next frame replaces every pixel
    {
        auto source = MakeSource(d, DXGI_FORMAT_R8G8B8A8_UNORM, 91);
        d.context->CopyResource(slot.texture.Get(), source.Get());
        WaitForD3D11(d);
        const auto pixels = ReadBack(d, slot.resource.Get());
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x)
                Require(pixels[y * kWidth + x] == Pixel(x, y, 91), "slot pixel equals the next capture");
    }

    // 4. A second slot is independent memory, and the first one survives Reset of the other
    {
        SharedTexture other;
        Check(CreateSharedTexture(d.d3d11.Get(), d.d3d12.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, other),
              "second slot");
        Require(other.resource.Get() != slot.resource.Get(), "distinct resources");
        auto source = MakeSource(d, DXGI_FORMAT_R8G8B8A8_UNORM, 3);
        d.context->CopyResource(other.texture.Get(), source.Get());
        WaitForD3D11(d);
        other.Reset();
        Require(other.handle == nullptr && other.resource == nullptr && other.texture == nullptr, "Reset empties the slot");
        const auto pixels = ReadBack(d, slot.resource.Get());
        Require(pixels[5 * kWidth + 9] == Pixel(9, 5, 91), "first slot untouched");
    }

    // 5. Bad input is refused and leaves the output empty
    {
        SharedTexture bad;
        Require(FAILED(CreateSharedTexture(nullptr, d.d3d12.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, bad)),
                "null device refused");
        Require(FAILED(CreateSharedTexture(d.d3d11.Get(), d.d3d12.Get(), 0, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, bad)),
                "zero size refused");
        Require(bad.texture == nullptr && bad.resource == nullptr && bad.handle == nullptr, "failed create leaves nothing");
    }

    slot.Reset();
    CloseHandle(d.event);
    std::puts("PASS: shared slot texture: typeless capture -> typed slot -> D3D12 readback, shader-resource access, independent slots");
    return 0;
}
