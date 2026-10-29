// Direct3D 12 helpers shared by the recorder in the game and the replay engine: how a texture is copied out to (and back
// in from) a buffer. Header only, with nothing from the rest of the product, so tests can build it alone.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "ext/nr/diag/NrReplayFormat.h"

namespace ext::nr::replay
{
using Microsoft::WRL::ComPtr;

// Where and how the first subresource of a texture sits in a buffer.
struct Footprint
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {};
    UINT rows = 0;
    UINT64 rowBytes = 0; // bytes of one row that carry pixels (the buffer's row pitch is larger, padded)
    UINT64 total = 0;
    uint32_t copyFormat = 0;
};

// The footprint of subresource 0 in the copy format of the resource. False when the device reports nothing to copy.
inline bool QueryFootprint(ID3D12Device* device, const D3D12_RESOURCE_DESC& resource, Footprint& out)
{
    if (device == nullptr || resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return false;

    D3D12_RESOURCE_DESC typed = resource;
    typed.Format = static_cast<DXGI_FORMAT>(CopyFormatFor(resource.Format));
    out.copyFormat = typed.Format;
    out.layout = {};
    device->GetCopyableFootprints(&typed, 0, 1, 0, &out.layout, &out.rows, &out.rowBytes, &out.total);
    return out.total != 0 && out.rows != 0 && out.rowBytes != 0;
}

inline ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buffer;
    const D3D12_RESOURCE_STATES state =
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

inline void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

// Copies subresource 0 of a texture that is in COPY_SOURCE into a readback buffer.
inline void CopyOut(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture, ID3D12Resource* readback, const Footprint& fp)
{
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp.layout;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// Copies a buffer filled by the caller into subresource 0 of a texture that is in COPY_DEST.
inline void CopyIn(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture, ID3D12Resource* upload, const Footprint& fp)
{
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp.layout;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}
} // namespace ext::nr::replay
