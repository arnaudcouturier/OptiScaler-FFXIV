#include "ext/hdr/Hdr10.h"
#include "ext/core/ExtConfig.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

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

// A command-list wrapper that counts the transitions recorded on every resource, then forwards everything.
// It catches barrier regressions (a dead barrier condition) that never change a pixel: on WARP the promotion
// rules make a missing transition benign, so only the recorded command stream tells.
class BarrierCounter final : public ID3D12GraphicsCommandList
{
  public:
    explicit BarrierCounter(ID3D12GraphicsCommandList* inner) : inner_(inner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(ID3D12Object)) ||
            IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(riid, __uuidof(ID3D12CommandList)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList)))
        {
            *out = static_cast<ID3D12GraphicsCommandList*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --refs_;
        if (!remaining)
            delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID guid, _Inout_ UINT *pDataSize, _Out_writes_bytes_opt_( *pDataSize ) void *pData) override { return inner_->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID guid, _In_ UINT DataSize, _In_reads_bytes_opt_( DataSize ) const void *pData) override { return inner_->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID guid, _In_opt_ const IUnknown *pData) override { return inner_->SetPrivateDataInterface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(_In_z_ LPCWSTR Name) override { return inner_->SetName(Name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, _COM_Outptr_opt_ void **ppvDevice) override { return inner_->GetDevice(riid, ppvDevice); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType(void) override { return inner_->GetType(); }
    HRESULT STDMETHODCALLTYPE Close(void) override { return inner_->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(_In_ ID3D12CommandAllocator *pAllocator, _In_opt_ ID3D12PipelineState *pInitialState) override { return inner_->Reset(pAllocator, pInitialState); }
    void STDMETHODCALLTYPE ClearState(_In_opt_ ID3D12PipelineState *pPipelineState) override { inner_->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(_In_ UINT VertexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartVertexLocation, _In_ UINT StartInstanceLocation) override { inner_->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(_In_ UINT IndexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartIndexLocation, _In_ INT BaseVertexLocation, _In_ UINT StartInstanceLocation) override { inner_->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE Dispatch(_In_ UINT ThreadGroupCountX, _In_ UINT ThreadGroupCountY, _In_ UINT ThreadGroupCountZ) override { inner_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    void STDMETHODCALLTYPE CopyBufferRegion(_In_ ID3D12Resource *pDstBuffer, UINT64 DstOffset, _In_ ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override { inner_->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes); }
    void STDMETHODCALLTYPE CopyTextureRegion(_In_ const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, _In_ const D3D12_TEXTURE_COPY_LOCATION *pSrc, _In_opt_ const D3D12_BOX *pSrcBox) override { inner_->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox); }
    void STDMETHODCALLTYPE CopyResource(_In_ ID3D12Resource *pDstResource, _In_ ID3D12Resource *pSrcResource) override { inner_->CopyResource(pDstResource, pSrcResource); }
    void STDMETHODCALLTYPE CopyTiles(_In_ ID3D12Resource *pTiledResource, _In_ const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, _In_ const D3D12_TILE_REGION_SIZE *pTileRegionSize, _In_ ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override { inner_->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE ResolveSubresource(_In_ ID3D12Resource *pDstResource, _In_ UINT DstSubresource, _In_ ID3D12Resource *pSrcResource, _In_ UINT SrcSubresource, _In_ DXGI_FORMAT Format) override { inner_->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(_In_ D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override { inner_->IASetPrimitiveTopology(PrimitiveTopology); }
    void STDMETHODCALLTYPE RSSetViewports(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumViewports, _In_reads_( NumViewports) const D3D12_VIEWPORT *pViewports) override { inner_->RSSetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSSetScissorRects(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumRects, _In_reads_( NumRects) const D3D12_RECT *pRects) override { inner_->RSSetScissorRects(NumRects, pRects); }
    void STDMETHODCALLTYPE OMSetBlendFactor(_In_reads_opt_(4) const FLOAT BlendFactor[ 4 ]) override { inner_->OMSetBlendFactor(BlendFactor); }
    void STDMETHODCALLTYPE OMSetStencilRef(_In_ UINT StencilRef) override { inner_->OMSetStencilRef(StencilRef); }
    void STDMETHODCALLTYPE SetPipelineState(_In_ ID3D12PipelineState *pPipelineState) override { inner_->SetPipelineState(pPipelineState); }
    // ResourceBarrier is defined by hand below (counts mask transitions).
    void STDMETHODCALLTYPE ExecuteBundle(_In_ ID3D12GraphicsCommandList *pCommandList) override { inner_->ExecuteBundle(pCommandList); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(_In_ UINT NumDescriptorHeaps, _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap *const *ppDescriptorHeaps) override { inner_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps); }
    void STDMETHODCALLTYPE SetComputeRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { inner_->SetComputeRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { inner_->SetGraphicsRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { inner_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { inner_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { inner_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { inner_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { inner_->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { inner_->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { inner_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE IASetIndexBuffer(_In_opt_ const D3D12_INDEX_BUFFER_VIEW *pView) override { inner_->IASetIndexBuffer(pView); }
    void STDMETHODCALLTYPE IASetVertexBuffers(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_VERTEX_BUFFER_VIEW *pViews) override { inner_->IASetVertexBuffers(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE SOSetTargets(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override { inner_->SOSetTargets(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE OMSetRenderTargets(_In_ UINT NumRenderTargetDescriptors, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, _In_ BOOL RTsSingleHandleToDescriptorRange, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override { inner_->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); }
    void STDMETHODCALLTYPE ClearDepthStencilView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, _In_ D3D12_CLEAR_FLAGS ClearFlags, _In_ FLOAT Depth, _In_ UINT8 Stencil, _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { inner_->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearRenderTargetView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, _In_ const FLOAT ColorRGBA[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { inner_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const UINT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { inner_->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const FLOAT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { inner_->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE DiscardResource(_In_ ID3D12Resource *pResource, _In_opt_ const D3D12_DISCARD_REGION *pRegion) override { inner_->DiscardResource(pResource, pRegion); }
    void STDMETHODCALLTYPE BeginQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { inner_->BeginQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE EndQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { inner_->EndQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE ResolveQueryData(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT StartIndex, _In_ UINT NumQueries, _In_ ID3D12Resource *pDestinationBuffer, _In_ UINT64 AlignedDestinationBufferOffset) override { inner_->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset); }
    void STDMETHODCALLTYPE SetPredication(_In_opt_ ID3D12Resource *pBuffer, _In_ UINT64 AlignedBufferOffset, _In_ D3D12_PREDICATION_OP Operation) override { inner_->SetPredication(pBuffer, AlignedBufferOffset, Operation); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { inner_->SetMarker(Metadata, pData, Size); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { inner_->BeginEvent(Metadata, pData, Size); }
    void STDMETHODCALLTYPE EndEvent(void) override { inner_->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(_In_ ID3D12CommandSignature *pCommandSignature, _In_ UINT MaxCommandCount, _In_ ID3D12Resource *pArgumentBuffer, _In_ UINT64 ArgumentBufferOffset, _In_opt_ ID3D12Resource *pCountBuffer, _In_ UINT64 CountBufferOffset) override { inner_->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset); }
    void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) override
    {
        for (UINT i = 0; i < count; ++i)
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                ++transitions_[barriers[i].Transition.pResource];
        inner_->ResourceBarrier(count, barriers);
    }
    std::map<ID3D12Resource*, size_t> transitions_;
  private:
    ID3D12GraphicsCommandList* inner_;
    std::atomic<ULONG> refs_ { 1 };
};

ComPtr<ID3D12Resource> MakeSceneTexture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&texture)));
    return texture;
}

// Fills a texture with one half-float value on every lane; the upload buffer outlives the list in `keepalive`.
void UploadUniform(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* texture,
                   uint16_t half, std::vector<ComPtr<ID3D12Resource>>& keepalive)
{
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bytes;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped));
    for (UINT y = 0; y < rows; ++y)
    {
        auto* pixel = reinterpret_cast<uint16_t*>(static_cast<BYTE*>(mapped) + footprint.Offset +
                                                 y * footprint.Footprint.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x)
            for (int c = 0; c < 4; ++c)
                pixel[x * 4 + c] = half;
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    keepalive.push_back(upload);
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

    // A scene and a mask through the production Convert: the constant wiring (legacy flag, UI expansion) and the
    // preview-mask barriers. Goldens from the CPU model (tests/ext/hdr/hdr_halo_model.h): a 0.75 gray final over a
    // 0.5 reference with scene == reference (gain 1) and a half-open mask. The mask is float here so t = 0.5 is
    // exact; production masks are RGBA8, which the barrier decision does not depend on.
    ext::Cfg().hdr.mode = 1;
    ext::Cfg().hdr.peak = 1100.0f;
    ext::Cfg().hdr.paper = 203.0f;
    ext::Cfg().hdr.expansion = 0.5f;
    ext::Cfg().hdr.contrast = 1.0f;
    ext::Cfg().hdr.saturation = 1.0f;
    ext::Cfg().hdr.vibrance = 0.0f;
    ext::Cfg().hdr.reshadeHighlights = false;

    std::vector<ComPtr<ID3D12Resource>> keepalive;
    auto sceneSource = MakeSceneTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 1);
    auto sceneHdr = MakeSceneTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 1);
    auto sceneRef = MakeSceneTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 1);
    auto sceneMask = MakeSceneTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 1);
    Check(allocator->Reset());
    Check(commands->Reset(allocator.Get(), nullptr));
    UploadUniform(device.Get(), commands.Get(), sceneSource.Get(), 0x3A00, keepalive); // 0.75
    UploadUniform(device.Get(), commands.Get(), sceneHdr.Get(), 0x3800, keepalive); // 0.5
    UploadUniform(device.Get(), commands.Get(), sceneRef.Get(), 0x3800, keepalive); // 0.5
    UploadUniform(device.Get(), commands.Get(), sceneMask.Get(), 0x3800, keepalive); // t = 0.5
    for (auto* settled : { sceneHdr.Get(), sceneRef.Get(), sceneMask.Get() })
        Barrier(commands.Get(), settled, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Check(commands->Close());
    ID3D12CommandList* setupLists[] { commands.Get() };
    queue->ExecuteCommandLists(1, setupLists);
    ComPtr<ID3D12Fence> sceneFence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sceneFence)));
    HANDLE sceneEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!sceneEvent)
        throw std::runtime_error("Could not create GPU completion event");
    UINT64 sceneValue = 0;
    auto waitForGpu = [&]()
    {
        ID3D12CommandList* lists[] { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(sceneFence.Get(), ++sceneValue));
        Check(sceneFence->SetEventOnCompletion(sceneValue, sceneEvent));
        if (WaitForSingleObject(sceneEvent, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("D3D12 WARP timed out");
    };
    waitForGpu();

    ext::hdr::SceneInput sceneInput;
    sceneInput.hdr = sceneHdr;
    sceneInput.reference = sceneRef;
    sceneInput.previewMask = sceneMask;

    ComPtr<ID3D12Resource> sceneReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sceneFootprint {};
    auto convertScene = [&](bool legacy, std::optional<float> ui)
    {
        ext::Cfg().hdr.legacyComposition = legacy;
        if (ui)
            ext::Cfg().hdr.uiExpansion = *ui;
        else
            ext::Cfg().hdr.uiExpansion = std::nullopt;
        Check(allocator->Reset());
        Check(commands->Reset(allocator.Get(), nullptr));
        auto* counter = new BarrierCounter(commands.Get());
        ID3D12Resource* output =
            ext::hdr::Convert(device.Get(), counter, sceneSource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, sceneInput);
        if (!output)
        {
            counter->Release();
            throw std::runtime_error("HDR10 converter with a scene returned null");
        }
        if (!sceneReadback)
        {
            const auto sceneOutputDesc = output->GetDesc();
            UINT sceneRows = 0;
            UINT64 sceneRowBytes = 0, sceneBytes = 0;
            device->GetCopyableFootprints(&sceneOutputDesc, 0, 1, 0, &sceneFootprint, &sceneRows, &sceneRowBytes,
                                          &sceneBytes);
            D3D12_RESOURCE_DESC sceneBufferDesc {};
            sceneBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            sceneBufferDesc.Width = sceneBytes;
            sceneBufferDesc.Height = sceneBufferDesc.DepthOrArraySize = sceneBufferDesc.MipLevels = 1;
            sceneBufferDesc.SampleDesc.Count = 1;
            sceneBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES sceneReadbackHeap {};
            sceneReadbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
            Check(device->CreateCommittedResource(&sceneReadbackHeap, D3D12_HEAP_FLAG_NONE, &sceneBufferDesc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sceneReadback)));
        }
        Barrier(counter, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION readbackDst {};
        readbackDst.pResource = sceneReadback.Get();
        readbackDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readbackDst.PlacedFootprint = sceneFootprint;
        D3D12_TEXTURE_COPY_LOCATION convertedSrc {};
        convertedSrc.pResource = output;
        convertedSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        counter->CopyTextureRegion(&readbackDst, 0, 0, 0, &convertedSrc, nullptr);
        Check(counter->Close());
        const auto transitions = counter->transitions_;
        counter->Release();
        waitForGpu();
        void* sceneBits = nullptr;
        Check(sceneReadback->Map(0, nullptr, &sceneBits));
        const auto* sceneWords =
            reinterpret_cast<const unsigned*>(static_cast<const BYTE*>(sceneBits) + sceneFootprint.Offset);
        const std::array<unsigned, 3> codes { sceneWords[0] & 1023, (sceneWords[0] >> 10) & 1023,
                                              (sceneWords[0] >> 20) & 1023 };
        sceneReadback->Unmap(0, nullptr);
        return std::pair<std::map<ID3D12Resource*, size_t>, std::array<unsigned, 3>> { transitions, codes };
    };
    auto transitionCount = [&](const std::map<ID3D12Resource*, size_t>& transitions, ID3D12Resource* resource)
    {
        const auto at = transitions.find(resource);
        return at == transitions.end() ? 0 : at->second;
    };
    auto expectStream = [&](const char* what, const std::map<ID3D12Resource*, size_t>& transitions)
    {
        if (transitionCount(transitions, sceneMask.Get()) != 2 ||
            transitionCount(transitions, sceneHdr.Get()) != 2 ||
            transitionCount(transitions, sceneRef.Get()) != 2 ||
            transitionCount(transitions, sceneSource.Get()) != 2)
            throw std::runtime_error(std::string("preview/scene barrier count wrong in ") + what);
    };
    auto nits = [&](const std::array<unsigned, 3>& codes)
    {
        return std::array<double, 3> { DecodePq(codes[0]), DecodePq(codes[1]), DecodePq(codes[2]) };
    };
    auto closeTo = [&](const char* what, double got, double want)
    {
        if (std::abs(got - want) > 0.02 * want + 0.5)
            throw std::runtime_error(std::string(what) + ": converted pixel is not the golden value");
    };
    const auto [legacyTransitions, legacyCodes] = convertScene(true, std::nullopt);
    expectStream("legacy with scene and mask", legacyTransitions);
    const auto legacyNits = nits(legacyCodes);
    for (int k = 0; k < 3; ++k)
        closeTo("legacy scene pixel", legacyNits[k], 194.581);
    const auto [lawTransitions, lawCodes] = convertScene(false, std::nullopt);
    expectStream("continuous with scene and mask", lawTransitions);
    const auto lawNits = nits(lawCodes);
    for (int k = 0; k < 3; ++k)
        closeTo("continuous scene pixel", lawNits[k], 176.418);
    if (std::abs(lawNits[0] - legacyNits[0]) < 5.0)
        throw std::runtime_error("continuous composition left the known pixel unchanged");
    const auto [uiTransitions, uiCodes] = convertScene(false, 0.9f);
    expectStream("continuous with interface expansion", uiTransitions);
    const auto uiNits = nits(uiCodes);
    for (int k = 0; k < 3; ++k)
        closeTo("interface expansion pixel", uiNits[k], 231.743);
    if (std::abs(uiNits[0] - lawNits[0]) < 5.0)
        throw std::runtime_error("interface expansion left the HUD pixel unchanged");
    const auto [legacyUiTransitions, legacyUiCodes] = convertScene(true, 0.9f);
    expectStream("legacy with interface expansion", legacyUiTransitions);
    if (legacyUiCodes != legacyCodes)
        throw std::runtime_error("legacy composition reads the interface expansion");
    CloseHandle(sceneEvent);

    std::puts("PASS: production D3D12 HDR10 converter emits R10G10B10A2 PQ with black and 203-nit paper white on WARP");
    std::puts("PASS: scene and mask through the production converter: legacy and continuous goldens, interface-only expansion, both mask barriers");
    return 0;
}
catch (const std::exception& error)
{
    std::puts(error.what());
    return 1;
}
