#pragma once

#include "HdrMenuLayer_Text.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <utility>

#include <imgui/imgui.h>

namespace ext::hdr::menu
{
inline D3D12_RECT MenuBounds(const ImDrawData& data, UINT width, UINT height)
{
    const D3D12_RECT full { 0, 0, LONG(width), LONG(height) };
    float left = float(width), top = float(height), right = 0, bottom = 0;
    for (const auto* list : data.CmdLists)
    {
        // Callbacks can draw arbitrary geometry not described by VtxBuffer.
        for (const auto& command : list->CmdBuffer)
        {
            if (command.UserCallback)
                return full;
        }
        for (const auto& vertex : list->VtxBuffer)
        {
            const float x = (vertex.pos.x - data.DisplayPos.x) * data.FramebufferScale.x;
            const float y = (vertex.pos.y - data.DisplayPos.y) * data.FramebufferScale.y;
            if (!std::isfinite(x) || !std::isfinite(y))
                return full;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    // One pixel of padding covers rasterization and antialiasing at geometry boundaries.
    return { LONG(std::clamp(std::floor(left) - 1, 0.f, float(width))),
             LONG(std::clamp(std::floor(top) - 1, 0.f, float(height))),
             LONG(std::clamp(std::ceil(right) + 1, 0.f, float(width))),
             LONG(std::clamp(std::ceil(bottom) + 1, 0.f, float(height))) };
}

class MenuLayer
{
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> _device;
    Ptr<ID3D12Resource> _layer, _scene;
    Ptr<ID3D12DescriptorHeap> _rtv, _srv;
    Ptr<ID3D12RootSignature> _root;
    Ptr<ID3D12PipelineState> _pipeline;
    UINT _width = 0, _height = 0;

    static void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* image, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commands->ResourceBarrier(1, &barrier);
    }

    HRESULT Init(ID3D12Device* device, UINT width, UINT height)
    {
        _device = device;
        _width = width;
        _height = height;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtv));
        if (FAILED(hr))
            return hr;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_srv));
        if (FAILED(hr))
            return hr;

        D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = { 1, &range };
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 1, &parameter, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        Ptr<ID3DBlob> blob, error, vs, ps;
        hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr))
            return hr;
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root));
        if (FAILED(hr))
            return hr;
        hr = D3DCompile(HdrMenuLayer, sizeof(HdrMenuLayer), "OptiHDR menu layer", nullptr, nullptr, "VS", "vs_5_0",
                        0, 0, &vs, &error);
        if (FAILED(hr))
            return hr;
        hr = D3DCompile(HdrMenuLayer, sizeof(HdrMenuLayer), "OptiHDR menu layer", nullptr, nullptr, "PS", "ps_5_0",
                        0, 0, &ps, &error);
        if (FAILED(hr))
            return hr;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = _root.Get();
        pipelineDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipelineDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
        pipelineDesc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&_pipeline));
        if (FAILED(hr))
            return hr;

        D3D12_RESOURCE_DESC textureDesc {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heapProps {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear {};
        clear.Format = textureDesc.Format;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&_layer));
        if (FAILED(hr))
            return hr;
        device->CreateRenderTargetView(_layer.Get(), nullptr, _rtv->GetCPUDescriptorHandleForHeapStart());

        textureDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_scene));
        if (FAILED(hr))
            return hr;
        auto handle = _srv->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(_layer.Get(), nullptr, handle);
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(_scene.Get(), nullptr, handle);
        return S_OK;
    }

  public:
    UINT Width() const { return _width; }
    UINT Height() const { return _height; }
    bool Ready() const { return _layer.Get() != nullptr; }
    D3D12_CPU_DESCRIPTOR_HANDLE Target() const { return _rtv->GetCPUDescriptorHandleForHeapStart(); }

    HRESULT Ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
    {
        if (!device || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || !desc.Width || !desc.Height || desc.Width > 16384 || desc.Height > 16384)
            return E_INVALIDARG;
        if (_device.Get() == device && _width == desc.Width && _height == desc.Height && _layer)
            return S_OK;
        MenuLayer next;
        const auto hr = next.Init(device, static_cast<UINT>(desc.Width), desc.Height);
        if (SUCCEEDED(hr))
            *this = std::move(next);
        return hr;
    }

    void Begin(ID3D12GraphicsCommandList* commands, ID3D12Resource* output, const D3D12_RECT& bounds)
    {
        Barrier(commands, output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from {}, to {};
        from.pResource = output;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = _scene.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box { UINT(bounds.left), UINT(bounds.top), 0, UINT(bounds.right), UINT(bounds.bottom), 1 };
        commands->CopyTextureRegion(&to, box.left, box.top, 0, &from, &box);
        Barrier(commands, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float transparent[4] {};
        const auto target = Target();
        commands->ClearRenderTargetView(target, transparent, 1, &bounds);
        commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    }

    void End(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE output,
             const D3D12_RECT& bounds)
    {
        Barrier(commands, _layer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(commands, _scene.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->OMSetRenderTargets(1, &output, FALSE, nullptr);
        ID3D12DescriptorHeap* heap = _srv.Get();
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootSignature(_root.Get());
        commands->SetPipelineState(_pipeline.Get());
        commands->SetGraphicsRootDescriptorTable(0, _srv->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport { 0, 0, float(_width), float(_height), 0, 1 };
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &bounds);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        Barrier(commands, _layer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(commands, _scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
};
} // namespace ext::hdr::menu
