// Render-target-bindable shared plate texture (ext/hudless/SharedSlot.cpp): the nameplate engine draws the plates
// straight into a slot on the game's D3D11 device, and the output pass samples the same memory on the D3D12 device, so
// the texture must be a D3D11 render target, keep shader-resource access on the D3D12 side, and be writable through a
// render target view with the game-style depth and blend state bound. WARP D3D11 writes the slot with a clear and a
// real draw; the D3D12 WARP device reads it back through a copy and samples it through a shader into a private target.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ext/hudless/SharedSlot.h"

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// Straight (non-premultiplied) colour words, as the plate image and its blend expect.
constexpr uint32_t Colour(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return r | (g << 8) | (b << 16) | (a << 24); }
constexpr uint32_t kCleared = Colour(40, 80, 120, 255); // ClearRenderTargetView colour
constexpr uint32_t kDrawn = Colour(200, 30, 90, 255);   // the pass-through draw colour

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
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                            D3D11_SDK_VERSION, &d.d3d11, nullptr, &d.context),
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

void FlushD3D12(Devices& d)
{
    Check(d.list->Close(), "list close");
    ID3D12CommandList* lists[] = { d.list.Get() };
    d.queue->ExecuteCommandLists(1, lists);
    Check(d.queue->Signal(d.fence.Get(), ++d.fenceValue), "signal");
    Check(d.fence->SetEventOnCompletion(d.fenceValue, d.event), "completion event");
    Require(WaitForSingleObject(d.event, 20000) == WAIT_OBJECT_0, "D3D12 work completed");
}

// Reads the whole texture back through D3D12 (COMMON -> COPY_SOURCE -> COMMON).
std::vector<uint32_t> ReadBack(Devices& d, ID3D12Resource* texture)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
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
    barrier.Transition.pResource = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    d.list->ResourceBarrier(1, &barrier);
    FlushD3D12(d);

    const UINT width = static_cast<UINT>(desc.Width);
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * desc.Height);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(total) };
    Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map");
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(&pixels[static_cast<size_t>(y) * width], mapped + footprint.Footprint.RowPitch * y,
                    static_cast<size_t>(width) * sizeof(uint32_t));
    readback->Unmap(0, nullptr);
    return pixels;
}

ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "plate_shared_rt", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::printf("FAIL: D3DCompile %s/%s (%08lX) %s\n", entry, profile, static_cast<unsigned long>(hr),
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        std::exit(1);
    }
    return code;
}

// Fills the left half of the target with a flat colour: the left half keeps the cleared colour, the right half shows the
// draw, so one readback proves both paths wrote through the shared render target.
const char* kFillShader = R"(
struct VsOut
{
    float4 pos : SV_Position;
};

VsOut VSMain(uint id : SV_VertexID)
{
    // Two triangles over the right half of the target, in clip space.
    float2 corner = (id == 0) ? float2(0.0, -1.0) : (id == 1) ? float2(1.0, -1.0) : (id == 2) ? float2(0.0, 1.0)
                 : (id == 3) ? float2(1.0, -1.0) : (id == 4) ? float2(1.0, 1.0) : float2(0.0, 1.0);
    VsOut o;
    o.pos = float4(corner, 0.0, 1.0);
    return o;
}

float4 PSMain(VsOut i) : SV_Target
{
    return float4(200.0 / 255.0, 30.0 / 255.0, 90.0 / 255.0, 1.0);
}
)";

// Samples the slot into the private target: the same operation the plate output pass does.
const char* kSampleShader = R"(
Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VSMain(uint id : SV_VertexID)
{
    float2 corner = (id == 0) ? float2(-1.0, -1.0) : (id == 1) ? float2(3.0, -1.0) : float2(-1.0, 3.0);
    VsOut o;
    o.pos = float4(corner, 0.0, 1.0);
    o.uv = float2((corner.x + 1.0) * 0.5, (1.0 - corner.y) * 0.5);
    return o;
}

float4 PSMain(VsOut i) : SV_Target
{
    return Source.SampleLevel(Linear, i.uv, 0);
}
)";

// The game's draw state at a nameplate draw: a depth-stencil view bound with GREATER_EQUAL and no depth write, and the
// standard alpha blend. None of it may stop the slot from being written.
void DrawIntoSlot(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv,
                  ID3D11DepthStencilView* dsv)
{
    D3D11_BLEND_DESC blend {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blendState;
    Check(device->CreateBlendState(&blend, &blendState), "blend state");
    D3D11_DEPTH_STENCIL_DESC depth {};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    depth.StencilEnable = FALSE;
    ComPtr<ID3D11DepthStencilState> depthState;
    Check(device->CreateDepthStencilState(&depth, &depthState), "depth state");
    D3D11_RASTERIZER_DESC raster {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;
    Check(device->CreateRasterizerState(&raster, &rasterState), "rasterizer state");

    auto vsCode = Compile(kFillShader, "VSMain", "vs_5_0");
    auto psCode = Compile(kFillShader, "PSMain", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    Check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "vertex shader");
    Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "pixel shader");

    const float clear[4] = { 40.0f / 255.0f, 80.0f / 255.0f, 120.0f / 255.0f, 1.0f };
    context->ClearRenderTargetView(rtv, clear);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    context->OMSetRenderTargets(1, &rtv, dsv);
    context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->RSSetState(rasterState.Get());
    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->Draw(6, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

// Samples the slot through a real D3D12 draw into a private target and returns that target's pixels.
std::vector<uint32_t> SampleThroughD3D12(Devices& d, ID3D12Resource* slot)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC targetDesc {};
    targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    targetDesc.Width = kWidth;
    targetDesc.Height = kHeight;
    targetDesc.DepthOrArraySize = 1;
    targetDesc.MipLevels = 1;
    targetDesc.Format = kFormat;
    targetDesc.SampleDesc.Count = 1;
    targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target;
    Check(d.d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)),
          "sample target");

    D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = { 1, &range };
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc { 1, &param, 1, &sampler,
                                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error), "root signature");
    ComPtr<ID3D12RootSignature> root;
    Check(d.d3d12->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                       IID_PPV_ARGS(&root)),
          "CreateRootSignature");

    auto vsCode = Compile(kSampleShader, "VSMain", "vs_5_0");
    auto psCode = Compile(kSampleShader, "PSMain", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = root.Get();
    psoDesc.VS = { vsCode->GetBufferPointer(), vsCode->GetBufferSize() };
    psoDesc.PS = { psCode->GetBufferPointer(), psCode->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = kFormat;
    psoDesc.SampleDesc = { 1, 0 };
    ComPtr<ID3D12PipelineState> pso;
    Check(d.d3d12->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "pipeline state");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    Check(d.d3d12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)), "srv heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    d.d3d12->CreateShaderResourceView(slot, &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Check(d.d3d12->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "rtv heap");
    D3D12_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = kFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d.d3d12->CreateRenderTargetView(target.Get(), &rtv, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    Check(d.allocator->Reset(), "allocator reset");
    Check(d.list->Reset(d.allocator.Get(), nullptr), "list reset");
    d.list->SetGraphicsRootSignature(root.Get());
    d.list->SetPipelineState(pso.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor { 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
    d.list->RSSetViewports(1, &viewport);
    d.list->RSSetScissorRects(1, &scissor);
    const auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    d.list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);
    FlushD3D12(d);
    return ReadBack(d, target.Get());
}

// The game-style depth texture and its view (the plate draw always has a depth-stencil view bound).
ComPtr<ID3D11DepthStencilView> MakeDepth(Devices& d)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    Check(d.d3d11->CreateTexture2D(&desc, nullptr, &depth), "depth texture");
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(d.d3d11->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "depth-stencil view");
    return dsv;
}
} // namespace

int main()
{
    Devices d = Create();
    using ext::hudless::CreateSharedTexture;
    using ext::hudless::SharedTexture;

    // 1. A render-target-bindable slot is created, and D3D12 still sees shader-resource access on it
    SharedTexture slot;
    Check(CreateSharedTexture(d.d3d11.Get(), d.d3d12.Get(), kWidth, kHeight, kFormat, slot, true),
          "CreateSharedTexture(renderTarget)");
    D3D11_TEXTURE2D_DESC created {};
    slot.texture->GetDesc(&created);
    Require((created.BindFlags & D3D11_BIND_RENDER_TARGET) != 0, "the D3D11 texture is a render target");
    Require((created.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0, "the D3D11 texture is a shader resource");
    const D3D12_RESOURCE_DESC opened = slot.resource->GetDesc();
    Require((opened.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0, "D3D12 keeps render target access");
    Require((opened.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0, "D3D12 keeps shader-resource access");
    Require(opened.Width == kWidth && opened.Height == kHeight && opened.Format == kFormat,
            "the opened resource has the slot shape");

    // 2. The game draws into the slot: cleared and drawn through its render target view, with the game's depth state
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(d.d3d11->CreateRenderTargetView(slot.texture.Get(), nullptr, &rtv), "slot render target view");
        auto dsv = MakeDepth(d);
        DrawIntoSlot(d.d3d11.Get(), d.context.Get(), rtv.Get(), dsv.Get());
        WaitForD3D11(d);

        // 3. The pixels arrive on the D3D12 side, through the shared NT handle
        const auto pixels = ReadBack(d, slot.resource.Get());
        Require(pixels[0] == kCleared, "the clear reached the shared slot (left half)");
        Require(pixels[kWidth / 2 + 3] == kDrawn, "the draw reached the shared slot (right half)");
        Require(pixels[static_cast<size_t>(kHeight - 1) * kWidth + kWidth - 1] == kDrawn,
                "the draw covers the last pixel");
    }

    // 4. The D3D12 side samples the slot with a real draw, as the plate output pass does
    {
        const auto sampled = SampleThroughD3D12(d, slot.resource.Get());
        Require(sampled[2 * kWidth + 2] == kCleared, "the sampled target shows the cleared half");
        Require(sampled[2 * kWidth + kWidth - 3] == kDrawn, "the sampled target shows the drawn half");
    }

    // 5. The default (copy target) slot still creates, and a rewritten render-target slot replaces its content
    {
        SharedTexture plain;
        Check(CreateSharedTexture(d.d3d11.Get(), d.d3d12.Get(), kWidth, kHeight, kFormat, plain), "copy-target slot");
        Require((plain.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
                "the copy-target slot is sampleable");
        plain.Reset();

        ComPtr<ID3D11RenderTargetView> rtv;
        Check(d.d3d11->CreateRenderTargetView(slot.texture.Get(), nullptr, &rtv), "slot render target view (rewrite)");
        const float other[4] = { 10.0f / 255.0f, 220.0f / 255.0f, 30.0f / 255.0f, 1.0f };
        d.context->ClearRenderTargetView(rtv.Get(), other);
        WaitForD3D11(d);
        const auto pixels = ReadBack(d, slot.resource.Get());
        Require(pixels[5] == Colour(10, 220, 30, 255), "a rewritten slot replaces every pixel");
    }

    // 6. Bad input is still refused with the render target flag on
    {
        SharedTexture bad;
        Require(FAILED(CreateSharedTexture(nullptr, d.d3d12.Get(), kWidth, kHeight, kFormat, bad, true)),
                "null device refused");
        Require(FAILED(CreateSharedTexture(d.d3d11.Get(), nullptr, kWidth, kHeight, kFormat, bad, true)),
                "null D3D12 device refused");
        Require(bad.texture == nullptr && bad.resource == nullptr && bad.handle == nullptr, "failed create leaves nothing");
    }

    slot.Reset();
    CloseHandle(d.event);
    std::puts("PASS: render-target plate slot: D3D11 clear + draw through an RTV with the game depth state, D3D12 "
              "readback, D3D12 sampling, copy-target slot unchanged, bad input refused");
    return 0;
}
