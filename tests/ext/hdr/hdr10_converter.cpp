#include "ext/hdr/Hdr10.h"
#include "ext/core/ExtConfig.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("D3D12 operation failed");
}

double DecodePq(unsigned value)
{
    const double encoded = value / 1023.0;
    const double p = std::pow(encoded, 32.0 / 2523.0);
    return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
                                  (2413.0 / 128.0 - (2392.0 / 128.0) * p),
                              16384.0 / 2610.0);
}

void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    commands->ResourceBarrier(1, &barrier);
}
} // namespace

int main()
try
{
    ext::Cfg().hdr.expansion = 0.0f;

    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)));

    D3D12_RESOURCE_DESC sourceDesc {};
    sourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sourceDesc.Width = 2;
    sourceDesc.Height = 1;
    sourceDesc.DepthOrArraySize = sourceDesc.MipLevels = 1;
    sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sourceFootprint {};
    UINT sourceRows = 0;
    UINT64 sourceRowBytes = 0, sourceBytes = 0;
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &sourceFootprint, &sourceRows, &sourceRowBytes, &sourceBytes);

    D3D12_HEAP_PROPERTIES defaultHeap {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> source;
    Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &sourceDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source)));

    D3D12_HEAP_PROPERTIES uploadHeap {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sourceBytes;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    Check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped));
    auto* input = static_cast<BYTE*>(mapped) + sourceFootprint.Offset;
    std::memset(input, 0, sourceFootprint.Footprint.RowPitch);
    input[3] = 255;
    input[4] = input[5] = input[6] = input[7] = 255;
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = source.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION placed {};
    placed.pResource = upload.Get();
    placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    placed.PlacedFootprint = sourceFootprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &placed, nullptr);

    ID3D12Resource* output = ext::hdr::Convert(device.Get(), commands.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!output || output->GetDesc().Format != DXGI_FORMAT_R10G10B10A2_UNORM)
        throw std::runtime_error("HDR10 converter did not return a 10-bit PQ texture");

    Barrier(commands.Get(), output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outputFootprint {};
    UINT outputRows = 0;
    UINT64 outputRowBytes = 0, outputBytes = 0;
    const auto outputDesc = output->GetDesc();
    device->GetCopyableFootprints(&outputDesc, 0, 1, 0, &outputFootprint, &outputRows, &outputRowBytes, &outputBytes);
    bufferDesc.Width = outputBytes;
    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = outputFootprint;
    placed = {};
    placed.pResource = output;
    placed.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &placed, nullptr);

    Check(commands->Close());
    ID3D12CommandList* lists[] { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("Could not create GPU completion event");
    Check(queue->Signal(fence.Get(), 1));
    Check(fence->SetEventOnCompletion(1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
    {
        CloseHandle(event);
        throw std::runtime_error("D3D12 WARP timed out");
    }
    CloseHandle(event);

    Check(readback->Map(0, nullptr, &mapped));
    const auto* pixels = reinterpret_cast<const unsigned*>(static_cast<const BYTE*>(mapped) + outputFootprint.Offset);
    const unsigned black = pixels[0];
    const unsigned white = pixels[1];
    if ((black & 0x3fffffff) != 0 || ((black >> 30) & 3) != 3)
        throw std::runtime_error("black HDR pixel or alpha was incorrect");
    const unsigned white10 = white & 1023;
    if (std::abs(DecodePq(white10) - 203.0) > 3.0)
        throw std::runtime_error("paper-white SDR pixel was not mapped to 203 nits");
    readback->Unmap(0, nullptr);

    std::puts("PASS: production D3D12 HDR10 converter emits R10G10B10A2 PQ with black and 203-nit paper white on WARP");
    return 0;
}
catch (const std::exception& error)
{
    std::puts(error.what());
    return 1;
}
