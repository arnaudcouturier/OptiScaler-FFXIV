#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "ext/gpu/GpuLifetime.h"
#include "ext/hdr/scene/NrSceneInput.h"

using Microsoft::WRL::ComPtr;
namespace NrSceneInput = ext::hdr::NrSceneInput;

namespace
{
void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("D3D12 call failed: " + std::to_string(static_cast<unsigned>(result)));
}

void Expect(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

float Half(unsigned short value)
{
    const unsigned exponent = (value >> 10) & 31u;
    const unsigned mantissa = value & 1023u;
    float result = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                 : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN()
                                                             : std::numeric_limits<float>::infinity())
                                                  : std::ldexp(1.0f + mantissa / 1024.0f,
                                                               static_cast<int>(exponent) - 15);
    return value & 0x8000u ? -result : result;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height,
                               D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                         IID_PPV_ARGS(&resource)));
    return resource;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 size,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                         IID_PPV_ARGS(&resource)));
    return resource;
}

void UploadIntoCopyDest(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination,
                        ID3D12Resource* upload, UINT64 offset, UINT width, UINT height)
{
    D3D12_TEXTURE_COPY_LOCATION to {}, from {};
    to.pResource = destination;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = upload;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = offset;
    from.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 256 };
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

void CopyToReadback(ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
                    ID3D12Resource* readback, UINT64 offset, UINT width, UINT height)
{
    NrSceneInput::Transition(commands, source, D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to {}, from {};
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = offset;
    to.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 256 };
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    NrSceneInput::Transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
}
} // namespace

int main() try
{
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
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("failed to create fence event");

    auto native = Texture(device.Get(), 8, 2, D3D12_RESOURCE_STATE_COPY_DEST);
    auto hdr = Texture(device.Get(), 8, 1, D3D12_RESOURCE_STATE_COPY_DEST);
    auto reference = Texture(device.Get(), 8, 1, D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = Buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2048, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = Buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2048, D3D12_RESOURCE_STATE_COPY_DEST);

    void* mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped));
    std::memset(mapped, 0, 2048);
    auto* data = static_cast<unsigned short*>(mapped);
    // The native target is padded around a four-pixel viewport; alpha and padding carry sentinels.
    for (UINT y = 0; y < 2; ++y)
        for (UINT x = 0; x < 8; ++x)
        {
            auto* pixel = data + y * 128 + x * 4;
            pixel[0] = pixel[1] = pixel[2] = 0x3400; // 0.25 in untouched padding.
            pixel[3] = 0x3800;                       // 0.5 alpha.
        }
    for (UINT x = 0; x < 4; ++x)
        for (UINT channel = 0; channel < 3; ++channel)
            data[x * 4 + channel] = x == 2 ? 0 : 0x3c00; // Active SDR scene with one black pixel.
    for (UINT x = 0; x < 8; ++x)
        for (UINT channel = 0; channel < 4; ++channel)
        {
            data[256 + x * 4 + channel] = x >= 2 && x < 6 ? 0x4000 : 0x4c00; // HDR scene: 2.0 in crop.
            data[384 + x * 4 + channel] = 0x3c00;                            // SDR reference: 1.0.
        }
    for (UINT x = 0; x < 4; ++x)
    {
        for (UINT channel = 0; channel < 3; ++channel)
            data[512 + x * 4 + channel] = x == 2 ? 0 : 0x4098; // Half-light NR edit on even frames.
        data[512 + x * 4 + 3] = 0x3800;
    }
    upload->Unmap(0, nullptr);
    UploadIntoCopyDest(commands.Get(), hdr.Get(), upload.Get(), 512, 8, 1);
    UploadIntoCopyDest(commands.Get(), reference.Get(), upload.Get(), 768, 8, 1);
    NrSceneInput::Transition(commands.Get(), hdr.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COMMON);
    NrSceneInput::Transition(commands.Get(), reference.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COMMON);

    ext::gpu::GpuLifetime lifetime;
    ext::hdr::SceneInput scene;
    scene.hdr = hdr;
    scene.reference = reference;
    scene.owner = std::make_shared<int>(11);
    scene.rect = { 0.25f, 0, 0.5f, 1 };
    std::function<bool()> retired;
    scene.retire = [&](std::function<bool()> probe) { retired = std::move(probe); };

    Expect(!ext::hdr::ReusableRecordedWork(true, {}), "missing GPU retirement probe was accepted");
    Expect(!ext::hdr::ReusableRecordedWork(true, [] { return false; }), "incomplete GPU probe was accepted");
    Expect(ext::hdr::ReusableRecordedWork(false, {}), "never-recorded allocation was not reusable");

    UINT64 fenceValue = 0;
    for (UINT frame = 1; frame <= 8; ++frame)
    {
        if (frame > 1)
        {
            Check(allocator->Reset());
            Check(commands->Reset(allocator.Get(), nullptr));
        }
        UploadIntoCopyDest(commands.Get(), native.Get(), upload.Get(), 0, 8, 2);

        ext::nr::SceneState failure = ext::nr::SceneState::Off;
        Expect(!NrSceneInput::Prepare(device.Get(), commands.Get(), native.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      9, 1, scene, lifetime, &failure) &&
                   failure == ext::nr::SceneState::UnsupportedFormat,
               "oversize NR viewport was not rejected as unsupported");
        failure = ext::nr::SceneState::Off;
        Expect(!NrSceneInput::Prepare(device.Get(), commands.Get(), native.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      4, 1, {}, lifetime, &failure) &&
                   failure == ext::nr::SceneState::WaitingScene,
               "missing NR scene was not rejected as waiting for scene input");

        auto* packet = NrSceneInput::Prepare(device.Get(), commands.Get(), native.Get(),
                                             D3D12_RESOURCE_STATE_COPY_DEST, 4, 1, scene, lifetime, &failure);
        Expect(packet != nullptr, "valid NR scene adapter preparation failed");
        Expect(retired && !retired(), "NR command recording retired before submission");
        NrSceneInput::Collect(false);
        Expect(NrSceneInput::Packets().size() == 1 && packet->native && packet->scene.owner == scene.owner,
               "unsubmitted NR resources were collected");

        NrSceneInput::Transition(commands.Get(), packet->work.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to {}, from {};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = 0;
        to.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1, 1, 256 };
        from.pResource = packet->work.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        const bool edit = frame % 2 == 0;
        NrSceneInput::Transition(commands.Get(), packet->work.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                 edit ? D3D12_RESOURCE_STATE_COPY_DEST
                                      : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (edit)
        {
            UploadIntoCopyDest(commands.Get(), packet->work.Get(), upload.Get(), 1024, 4, 1);
            NrSceneInput::Transition(commands.Get(), packet->work.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        NrSceneInput::Finish(*packet, commands.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        CopyToReadback(commands.Get(), native.Get(), readback.Get(), 512, 8, 2);

        Check(commands->Close());
        ID3D12CommandList* lists[] { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        lifetime.Submitted(queue.Get(), 1, lists);
        Check(queue->Signal(fence.Get(), ++fenceValue));
        Check(fence->SetEventOnCompletion(fenceValue, event));
        Expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "NR adapter WARP work timed out");
        Expect(!retired(), "GPU completion alone retired an open NR recording");
        NrSceneInput::Collect(false);
        Expect(NrSceneInput::Packets().size() == 1 && packet->native,
               "completed but unreset NR recording was collected");
        lifetime.ResetRecording(commands.Get());
        Expect(retired(), "completed and reset NR recording did not retire");

        Check(readback->Map(0, nullptr, &mapped));
        const auto* result = static_cast<const unsigned short*>(mapped);
        Expect(std::abs(Half(result[0]) - std::pow(2.0f, 2.2f)) < 0.02f,
               "cropped HDR scene light was not reconstructed");
        Expect(result[8] == 0, "black viewport pixel changed in HDR input");
        const auto* restored = result + 256;
        Expect(restored[3] == 0x3800 && restored[11] == 0x3800, "native alpha was not preserved");
        Expect(restored[8] == 0, "black viewport pixel changed in SDR restoration");
        for (UINT y = 0; y < 2; ++y)
            for (UINT x = 0; x < 8; ++x)
                if (y || x >= 4)
                {
                    const auto* pixel = restored + y * 128 + x * 4;
                    Expect(pixel[0] == 0x3400 && pixel[3] == 0x3800,
                           "NR viewport copy changed native padding or alpha");
                }
        if (edit)
            Expect(Half(restored[0]) > 0.73f && Half(restored[0]) < 0.74f,
                   "NR light edit was not restored to the SDR scene");
        else
            Expect(restored[0] == 0x3c00, "identity NR pass altered the native SDR scene");
        readback->Unmap(0, nullptr);
    }

    Expect(NrSceneInput::Packets().size() == 1, "NR work packet pool grew despite completed recordings");
    NrSceneInput::Collect(false);
    Expect(NrSceneInput::Packets().empty(), "completed NR work packet was not reclaimed");
    CloseHandle(event);
    std::puts("PASS: NR HDR scene conversion, viewport crop, identity/edit restoration, black/alpha/padding preservation, packet reuse and production GPU retirement");
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
