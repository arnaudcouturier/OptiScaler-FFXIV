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
#include <source_location>
#include <stdexcept>
#include <unordered_map>

#include "ext/hdr/scene/DlssSceneInput.h"

using Microsoft::WRL::ComPtr;
namespace DlssSceneInput = ext::hdr::DlssSceneInput;

namespace
{
struct Recording
{
    bool submitted = false;
    bool retired = false;
};
std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<Recording>> recordings;

std::shared_ptr<Recording> Current(ID3D12GraphicsCommandList* commands)
{
    auto& current = recordings[commands];
    if (!current || current->retired)
        current = std::make_shared<Recording>();
    return current;
}

void Check(HRESULT result, const std::source_location where = std::source_location::current())
{
    if (FAILED(result))
        throw std::runtime_error("D3D12 call failed at line " + std::to_string(where.line()) + ": " +
                                 std::to_string(static_cast<unsigned>(result)));
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
    const float magnitude = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                          : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN()
                                                                       : std::numeric_limits<float>::infinity())
                                                           : std::ldexp(1.0f + mantissa / 1024.0f,
                                                                        static_cast<int>(exponent) - 15);
    return value & 0x8000u ? -magnitude : magnitude;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                               D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
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

void Upload(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture, ID3D12Resource* upload,
            UINT64 offset, DXGI_FORMAT format, UINT width, UINT height)
{
    DlssSceneInput::Barrier(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to {}, from {};
    to.pResource = texture;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = upload;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = offset;
    from.PlacedFootprint.Footprint = { format, width, height, 1, 256 };
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    DlssSceneInput::Barrier(commands, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}

void CopyToReadback(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture,
                    ID3D12Resource* readback, UINT64 offset)
{
    DlssSceneInput::Barrier(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to {}, from {};
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = offset;
    const auto desc = texture->GetDesc();
    to.PlacedFootprint.Footprint = { desc.Format, static_cast<UINT>(desc.Width), desc.Height, 1, 256 };
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    DlssSceneInput::Barrier(commands, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

void SimulateIdentityDlss(ID3D12GraphicsCommandList* commands, DlssSceneInput::Packet& packet)
{
    DlssSceneInput::Barrier(commands, packet.input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
    DlssSceneInput::Barrier(commands, packet.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(packet.output.Get(), packet.input.Get());
    DlssSceneInput::Barrier(commands, packet.input.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssSceneInput::Barrier(commands, packet.output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace

namespace ext::hdr
{
// Keep this packet test focused on the production scene adapter. GPU queue and recording lifetime
// semantics are exercised against ext::gpu::GpuLifetime by the separate GPU lifetime WARP test.
std::function<bool()> TrackCommands(ID3D12GraphicsCommandList* commands)
{
    const auto recording = Current(commands);
    return [recording] { return recording->submitted && recording->retired; };
}
} // namespace ext::hdr

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

    auto colour = Texture(device.Get(), 4, 1, DXGI_FORMAT_R16G16B16A16_TYPELESS, D3D12_RESOURCE_STATE_COMMON);
    auto sceneHdr = Texture(device.Get(), 4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    auto sceneReference = Texture(device.Get(), 4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    auto edited = Texture(device.Get(), 4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    auto output = Texture(device.Get(), 4, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    auto unsupported = Texture(device.Get(), 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    auto upload = Buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2048, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = Buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2048, D3D12_RESOURCE_STATE_COPY_DEST);

    void* mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped));
    std::memset(mapped, 0, 2048);
    auto* data = static_cast<unsigned short*>(mapped);
    for (UINT x = 0; x < 4; ++x)
    {
        const unsigned short value = x == 1 ? 0x0000 : 0x3800; // SDR 0.5, with a black pixel.
        for (UINT channel = 0; channel < 3; ++channel)
            data[x * 4 + channel] = value;
        data[x * 4 + 3] = 0x3c00;
        const unsigned short hdr = x == 0 ? 0x4000 : x == 1 ? 0x4400 : 0x4200;
        for (UINT channel = 0; channel < 3; ++channel)
        {
            data[128 + x * 4 + channel] = hdr;       // Upload footprint offset 256 bytes.
            data[256 + x * 4 + channel] = 0x3800;    // Upload footprint offset 512 bytes.
            data[384 + x * 4 + channel] = 0x3a00;    // Upload footprint offset 768 bytes.
        }
        data[128 + x * 4 + 3] = data[256 + x * 4 + 3] = data[384 + x * 4 + 3] = 0x3c00;
    }
    upload->Unmap(0, nullptr);
    Upload(commands.Get(), colour.Get(), upload.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1);
    Upload(commands.Get(), sceneHdr.Get(), upload.Get(), 256, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1);
    Upload(commands.Get(), sceneReference.Get(), upload.Get(), 512, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1);
    Upload(commands.Get(), edited.Get(), upload.Get(), 768, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1);

    const char* failure = nullptr;
    Expect(!DlssSceneInput::Prepare(device.Get(), commands.Get(), unsupported.Get(), output.Get(), 4, 1, {}, &failure) &&
               failure && std::strstr(failure, "colour SRV"),
           "unsupported DLSS colour format did not return its explicit fallback reason");

    ext::hdr::SceneInput scene;
    scene.hdr = sceneHdr;
    scene.reference = sceneReference;
    scene.rect = { 0, 0, 1, 1 };
    scene.owner = std::make_shared<int>(7);
    std::function<bool()> sceneRetired;
    scene.retire = [&](std::function<bool()> probe) { sceneRetired = std::move(probe); };
    DlssSceneInput::bridgeColour = colour.Get();
    DlssSceneInput::bridgeColourState = D3D12_RESOURCE_STATE_COMMON;
    DlssSceneInput::bridgeOutput = output.Get();
    auto packet = DlssSceneInput::Prepare(device.Get(), commands.Get(), colour.Get(), output.Get(), 4, 1, scene);
    Expect(packet != nullptr, "valid typed SRV over typeless FP16 colour was rejected");
    SimulateIdentityDlss(commands.Get(), *packet);
    DlssSceneInput::Finish(packet, commands.Get(), true);
    auto image = DlssSceneInput::Image(packet);
    Expect(image.owner != nullptr && image.referenceSrgb && image.referenceHdrShoulder,
           "reconstructed scene metadata or packet ownership was lost");
    Expect(!packet->done() && sceneRetired && !sceneRetired(), "DLSS work retired before queue submission");
    Expect(DlssSceneInput::Reconcile(image, commands.Get(), edited.Get(), D3D12_RESOURCE_STATE_COMMON),
           "valid PreSR edit was not reconciled into the HDR carrier");
    Expect(!DlssSceneInput::Reconcile(image, commands.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "reconciliation allowed recorded descriptors to be overwritten twice");

    CopyToReadback(commands.Get(), packet->input.Get(), readback.Get(), 0);
    CopyToReadback(commands.Get(), image.hdr.Get(), readback.Get(), 512);
    CopyToReadback(commands.Get(), image.reference.Get(), readback.Get(), 1024);
    CopyToReadback(commands.Get(), output.Get(), readback.Get(), 1536);
    Check(commands->Close());
    ID3D12CommandList* lists[] { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), 1));
    Check(fence->SetEventOnCompletion(1, event));
    Expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "DLSS adapter WARP work timed out");
    Current(commands.Get())->submitted = true;
    Expect(!packet->done() && !sceneRetired(), "GPU completion alone retired an open command recording");
    Check(allocator->Reset());
    Check(commands->Reset(allocator.Get(), nullptr));
    Current(commands.Get())->retired = true;
    Expect(packet->done() && sceneRetired(), "completed and reset DLSS recording did not retire");

    Check(readback->Map(0, nullptr, &mapped));
    auto* pixels = static_cast<const unsigned short*>(mapped);
    Expect(Half(pixels[0]) > 4.0f, "same-frame HDR scene light was not carried into the DLSS input");
    Expect(Half(pixels[4]) == 0.0f, "black UI pixel acquired scene HDR light");
    Expect(std::abs(Half(pixels[256]) - 0.75f) < 0.01f, "PreSR SDR edit was not transported into HDR");
    Expect(std::abs(Half(pixels[512]) - 0.75f) < 0.01f, "PreSR edit was not copied to the updated SDR reference");
    Expect(Half(pixels[768]) > 0.9f, "HDR compatibility reference did not produce a shoulder-mapped SDR output");
    readback->Unmap(0, nullptr);

    image = {};
    packet.reset();
    Check(commands->Close());
    Check(allocator->Reset());
    Check(commands->Reset(allocator.Get(), nullptr));
    auto fallback = DlssSceneInput::Prepare(device.Get(), commands.Get(), colour.Get(), output.Get(), 4, 1, {});
    Expect(fallback != nullptr, "missing-scene SDR fallback did not prepare");
    SimulateIdentityDlss(commands.Get(), *fallback);
    DlssSceneInput::Finish(fallback, commands.Get(), true);
    auto fallbackImage = DlssSceneInput::Image(fallback);
    Expect(!fallbackImage.referenceHdrShoulder, "missing scene incorrectly retained HDR shoulder metadata");
    CopyToReadback(commands.Get(), fallbackImage.hdr.Get(), readback.Get(), 0);
    CopyToReadback(commands.Get(), output.Get(), readback.Get(), 512);
    Check(commands->Close());
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), 2));
    Check(fence->SetEventOnCompletion(2, event));
    Expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "SDR fallback WARP work timed out");
    Current(commands.Get())->submitted = true;
    Check(allocator->Reset());
    Check(commands->Reset(allocator.Get(), nullptr));
    Current(commands.Get())->retired = true;
    Check(readback->Map(0, nullptr, &mapped));
    pixels = static_cast<const unsigned short*>(mapped);
    Expect(std::abs(Half(pixels[0]) - 0.5f) < 0.01f, "missing-scene HDR carrier did not fall back to SDR");
    Expect(std::abs(Half(pixels[256]) - 0.5f) < 0.01f, "missing-scene display output did not preserve SDR identity");
    readback->Unmap(0, nullptr);

    CloseHandle(event);
    std::puts("PASS: DLSS scene input HDR transport, black preservation, PreSR reconciliation, explicit unsupported-format fallback, SDR fallback and recording retirement");
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
