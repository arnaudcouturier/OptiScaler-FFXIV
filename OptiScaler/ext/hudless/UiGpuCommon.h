#pragma once
// S10 package C: helpers shared by UiPaste.cpp and UiImage.cpp (docs/HUDLESS.md sections 6 and 7). Header only so both
// translation units stay self-contained. Expects the including .cpp to have pulled in pch.h (LOG_* macros) and <State.h>
// (ScopedSkipHeapCapture) first; the standalone tests stub both.

#include <State.h> // ScopedSkipHeapCapture

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ext::hudless::detail
{
using Microsoft::WRL::ComPtr;

// Defaults of MaskParams (HudlessGpu.h): a NaN or out-of-range value from a hand-edited ini must not disable the mask.
inline float ClampThreshold(float threshold)
{
    return std::isfinite(threshold) ? std::clamp(threshold, 0.0f, 1.0f) : 0.008f;
}

inline uint32_t ClampDilation(uint32_t dilation)
{
    return std::min<uint32_t>(dilation, 4);
}

// The bridge copies can be typeless; a shader view needs a typed format.
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

// Formats a float4 SRV can read as colour. The mask compares encoded values, so both images are expected in the same
// encoding (the bridge copies the same swapchain format into both).
inline bool IsColorFormat(DXGI_FORMAT format)
{
    switch (TypedFormat(format))
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

// The format to read an input through: the caller's, or the resource's own when the caller passed UNKNOWN.
inline DXGI_FORMAT ViewFormat(DXGI_FORMAT requested, const D3D12_RESOURCE_DESC& desc)
{
    return TypedFormat(requested != DXGI_FORMAT_UNKNOWN ? requested : desc.Format);
}

// A 2D, single-sample, single-slice texture the shaders can sample.
inline bool IsSampleableTexture(const D3D12_RESOURCE_DESC& desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
           desc.DepthOrArraySize == 1 && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
}

inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &barrier);
}

inline ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                                       [[maybe_unused]] const char* what)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        LOG_ERROR("HudLess: {} root signature serialization failed: {:X} {}", what, (UINT) hr,
                  error ? (const char*) error->GetBufferPointer() : "");
        return nullptr;
    }

    ComPtr<ID3D12RootSignature> root;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr))
    {
        LOG_ERROR("HudLess: {} CreateRootSignature failed: {:X}", what, (UINT) hr);
        return nullptr;
    }
    return root;
}

inline ComPtr<ID3D12DescriptorHeap> CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                                               bool shaderVisible)
{
    // Our own heaps must not be captured by the descriptor tracking of the game's hooks.
    ScopedSkipHeapCapture skipHeapCapture {};

    D3D12_DESCRIPTOR_HEAP_DESC desc {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        return nullptr;
    return heap;
}

inline D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap, UINT index, UINT increment)
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * increment;
    return handle;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap, UINT index, UINT increment)
{
    auto handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * increment;
    return handle;
}

// A null resource gives a null descriptor (reads return zero): used for the inputs a pass does not need.
inline void WriteTextureSrv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
                            D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc {};
    desc.Format = TypedFormat(format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &desc, handle);
}

inline void WriteTextureUav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
                            D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc {};
    desc.Format = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

inline bool SupportsTypedUavStore(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

inline D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props {};
    props.Type = type;
    return props;
}

inline D3D12_RESOURCE_DESC Texture2DDesc(UINT64 width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}
} // namespace ext::hudless::detail
