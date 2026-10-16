#pragma once
// WARP D3D12 rig shared by the S10 package C tests: device + queue + fence, texture upload / readback, half floats.
// Header only, included by exactly one translation unit per test executable.
//
// The debug layer is enabled when the machine has it (Graphics Tools) and every D3D12 ERROR breaks into the debugger,
// i.e. kills the test: state / barrier mistakes in the modules under test fail loudly instead of passing by luck.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ext/gpu/Gpu.h"

namespace ext::gpu
{
// The detour lives in QueueHook.cpp (needs the product); the standalone tests have none.
bool QueueHookInstalled()
{
    return false;
}
} // namespace ext::gpu

namespace ui_test
{
using Microsoft::WRL::ComPtr;

inline int g_failures = 0;

#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++ui_test::g_failures;                                                                                     \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

inline void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}

// IEEE half from float (round to nearest even is not needed: the tests only use exactly representable values).
inline uint16_t Half(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (value == 0.0f)
        return static_cast<uint16_t>(sign);
    if (exponent <= 0 || exponent >= 31)
        throw std::runtime_error("Half: out of the supported range");
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

inline float FromHalf(uint16_t h)
{
    const int exponent = (h >> 10) & 0x1F;
    const float mantissa = static_cast<float>(h & 0x3FF);
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0)
        return sign * std::ldexp(mantissa, -24);
    return sign * std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15);
}

struct Rig
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    bool debugLayer = false;

    Rig()
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            debugLayer = true;
        }

        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter> adapter;
        Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

        if (debugLayer)
        {
            ComPtr<ID3D12InfoQueue> info;
            if (SUCCEEDED(device.As(&info)))
            {
                info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
              "CreateCommandList");
        list->Close();
    }

    ~Rig()
    {
        if (event != nullptr)
            CloseHandle(event);
    }

    void WaitIdle()
    {
        Check(queue->Signal(fence.Get(), ++value), "Signal");
        if (fence->GetCompletedValue() < value)
        {
            Check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
            if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU wait timed out");
        }
        Check(device->GetDeviceRemovedReason(), "device removed");
    }

    // Runs `record` on the scratch list and waits for the GPU.
    template <typename Fn> void Run(Fn&& record)
    {
        Check(allocator->Reset(), "allocator Reset");
        Check(list->Reset(allocator.Get(), nullptr), "list Reset");
        record(list.Get());
        Check(list->Close(), "list Close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        WaitIdle();
    }

    ComPtr<ID3D12Resource> Texture(UINT64 width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture)),
              "CreateCommittedResource(texture)");
        return texture;
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)),
              "CreateCommittedResource(buffer)");
        return buffer;
    }

    // A texture holding `pixels` (tightly packed rows of `bytesPerPixel`), left in COMMON like the bridge's copies.
    ComPtr<ID3D12Resource> Upload(UINT width, UINT height, DXGI_FORMAT format, const void* pixels, UINT bytesPerPixel,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        auto texture = Texture(width, height, format, flags, D3D12_RESOURCE_STATE_COPY_DEST);
        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        Check(upload->Map(0, nullptr, &mapped), "Map(upload)");
        for (UINT y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Footprint.RowPitch * y,
                        static_cast<const uint8_t*>(pixels) + static_cast<size_t>(width) * bytesPerPixel * y,
                        static_cast<size_t>(width) * bytesPerPixel);
        upload->Unmap(0, nullptr);
        Run(
            [&](ID3D12GraphicsCommandList* c)
            {
                D3D12_TEXTURE_COPY_LOCATION dst {};
                dst.pResource = texture.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION src {};
                src.pResource = upload.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprint;
                c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Barrier(c, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            });
        return texture;
    }

    // Tightly packed bytes of a texture that is in `state` (it is returned to that state).
    std::vector<uint8_t> Download(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, UINT bytesPerPixel)
    {
        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        Run(
            [&](ID3D12GraphicsCommandList* c)
            {
                Barrier(c, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst {};
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION src {};
                src.pResource = texture;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Barrier(c, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            });
        void* mapped = nullptr;
        Check(readback->Map(0, nullptr, &mapped), "Map(readback)");
        std::vector<uint8_t> bytes(static_cast<size_t>(desc.Width) * desc.Height * bytesPerPixel);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(bytes.data() + static_cast<size_t>(desc.Width) * bytesPerPixel * y,
                        static_cast<const uint8_t*>(mapped) + footprint.Footprint.RowPitch * y,
                        static_cast<size_t>(desc.Width) * bytesPerPixel);
        D3D12_RANGE none { 0, 0 };
        readback->Unmap(0, &none);
        return bytes;
    }

    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        c->ResourceBarrier(1, &barrier);
    }
};

// RGBA8 image helper.
struct Image8
{
    UINT width, height;
    std::vector<uint8_t> px;
    Image8(UINT w, UINT h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : width(w), height(h), px(size_t(w) * h * 4)
    {
        for (size_t i = 0; i < px.size(); i += 4)
        {
            px[i] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = a;
        }
    }
    uint8_t* at(UINT x, UINT y) { return &px[(size_t(y) * width + x) * 4]; }
    const uint8_t* at(UINT x, UINT y) const { return &px[(size_t(y) * width + x) * 4]; }
};
} // namespace ui_test
