#pragma once
// The game's D3D11 draw state for the plate game-side tests: a real WARP context with the states a nameplate draw has
// (one 8-bit target, the standard alpha blend, a GREATER_EQUAL depth test that writes nothing) and one wrong variant of
// each, so a test can put exactly one condition out of place and see the classifier refuse.
//
// A "plate pixel shader" is a real compiled shader stamped with the plate kind (the product stamps the kind from the
// shader's bytecode CRC at creation; the tag is what the classifier reads). A "nameplate input layout" is an input
// layout carrying the layout tag the game's own TagLayout writes for its three nameplate vertex shaders.
//
// Headers only, so both the game-side test and the end-to-end scenario test use the same rig.

#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ext/ffxiv/plates/PlateShaderKind.h"
#include "ext/ffxiv/ui/PlateLayoutTag.h"

namespace plate_game_test
{
using Microsoft::WRL::ComPtr;

inline constexpr UINT kWidth = 64;
inline constexpr UINT kHeight = 32;
inline constexpr unsigned kPlateCrc = 3865947726u; // the layout 0 nameplate pixel shader

inline void Require(bool ok, const char* what)
{
    if (!ok)
        throw std::runtime_error(what);
}

inline ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "plate_game", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &error);
    if (FAILED(hr))
    {
        std::string text = "D3DCompile failed: ";
        if (error)
            text += static_cast<const char*>(error->GetBufferPointer());
        throw std::runtime_error(text);
    }
    return code;
}

// One fullscreen triangle in clip space: the shader just has to be real so the states and the pipeline are real.
inline constexpr const char* kVertexShader = R"(
struct VsOut { float4 pos : SV_Position; };
VsOut VSMain(uint id : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";
inline constexpr const char* kPixelShader = R"(
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
)";

// Everything a draw needs, on one device. Construct with the device and context the test already has.
class DrawStateRig
{
  public:
    DrawStateRig(ID3D11Device* device, ID3D11DeviceContext* context, UINT width = kWidth, UINT height = kHeight)
        : device_(device), context_(context), width_(width), height_(height)
    {
        // The game's colour target and depth buffer.
        D3D11_TEXTURE2D_DESC colour {};
        colour.Width = width_;
        colour.Height = height_;
        colour.MipLevels = colour.ArraySize = 1;
        colour.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colour.SampleDesc.Count = 1;
        colour.Usage = D3D11_USAGE_DEFAULT;
        colour.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Require(SUCCEEDED(device_->CreateTexture2D(&colour, nullptr, &target_)), "colour target");
        Require(SUCCEEDED(device_->CreateRenderTargetView(target_.Get(), nullptr, &targetView_)), "colour view");
        D3D11_TEXTURE2D_DESC depth = colour;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Require(SUCCEEDED(device_->CreateTexture2D(&depth, nullptr, &depthTexture_)), "depth target");
        Require(SUCCEEDED(device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, &depthView_)), "depth view");
        D3D11_TEXTURE2D_DESC smallDepth = depth;
        smallDepth.Width = width_ / 2;
        smallDepth.Height = height_ / 2;
        Require(SUCCEEDED(device_->CreateTexture2D(&smallDepth, nullptr, &smallDepthTexture_)), "small depth target");
        Require(SUCCEEDED(device_->CreateDepthStencilView(smallDepthTexture_.Get(), nullptr, &smallDepthView_)),
                "small depth view");

        // A target of another format (the classifier takes only the 8-bit SDR formats).
        D3D11_TEXTURE2D_DESC fp = colour;
        fp.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Require(SUCCEEDED(device_->CreateTexture2D(&fp, nullptr, &floatTexture_)), "float colour target");
        Require(SUCCEEDED(device_->CreateRenderTargetView(floatTexture_.Get(), nullptr, &floatView_)), "float colour view");

        // The slot the engine would hand out: same size and format, its own view.
        D3D11_TEXTURE2D_DESC slot = colour;
        slot.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Require(SUCCEEDED(device_->CreateTexture2D(&slot, nullptr, &slotTexture_)), "slot texture");
        Require(SUCCEEDED(device_->CreateRenderTargetView(slotTexture_.Get(), nullptr, &slotView_)), "slot view");

        // Depth-stencil states: the plate rule (GREATER_EQUAL, no write, no stencil) and one wrong variant of each part.
        plateDepth_ = DepthState(true, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_GREATER_EQUAL, false);
        writingDepth_ = DepthState(true, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_GREATER_EQUAL, false);
        lessDepth_ = DepthState(true, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_LESS, false);
        stencilDepth_ = DepthState(true, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_GREATER_EQUAL, true);
        noDepth_ = DepthState(false, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_GREATER_EQUAL, false);

        // Blend states: the standard alpha blend, and one wrong variant.
        plateBlend_ = BlendState(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE,
                                 D3D11_BLEND_INV_SRC_ALPHA);
        wrongBlend_ = BlendState(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE);
        additiveBlend_ = BlendState(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA);
        alphaBlend_ = BlendState(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ONE);

        // Shaders and layouts.
        auto vsCode = Compile(kVertexShader, "VSMain", "vs_5_0");
        auto psCode = Compile(kPixelShader, "PSMain", "ps_5_0");
        Require(SUCCEEDED(device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)),
                "vertex shader");
        Require(SUCCEEDED(device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_)),
                "pixel shader");
        ext::ffxiv::plates::OnPixelShaderCreated(ps_.Get(), kPlateCrc); // the stamp the product's creation hook writes
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        Require(SUCCEEDED(device_->CreateInputLayout(elements, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                                     &plateLayout_)),
                "plate input layout");
        // The tag the game's TagLayout writes for a nameplate vertex shader (it derives it from the bytecode CRC; the
        // classifier only reads the tag).
        ext::ffxiv::ui::plate::Layout layout {};
        layout.count = 1;
        strcpy_s(layout.elements[0].semantic, "POSITION");
        layout.elements[0].format = DXGI_FORMAT_R32G32B32_FLOAT;
        Require(SUCCEEDED(plateLayout_->SetPrivateData(ext::ffxiv::ui::plate::layoutTag, sizeof(layout), &layout)),
                "layout tag");
        // A layout that is not a nameplate layout: same data, no tag.
        Require(SUCCEEDED(device_->CreateInputLayout(elements, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                                     &otherLayout_)),
                "other input layout");

        // A UAV target for the "no unordered access view" condition.
        D3D11_TEXTURE2D_DESC uavDesc = colour;
        uavDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        uavDesc.MipLevels = 1;
        Require(SUCCEEDED(device_->CreateTexture2D(&uavDesc, nullptr, &uavTexture_)), "uav texture");
        Require(SUCCEEDED(device_->CreateUnorderedAccessView(uavTexture_.Get(), nullptr, &uav_)), "uav");
    }

    // The state a plate draw has, with one field optionally out of place. `variant` names the condition to break.
    void BindPlateDraw(const char* broken = nullptr)
    {
        auto is = [broken](const char* name) { return broken != nullptr && std::strcmp(broken, name) == 0; };
        ID3D11Buffer* nothing = nullptr;
        UINT stride = 0, offset = 0;
        context_->IASetInputLayout(is("layout") ? otherLayout_.Get() : plateLayout_.Get());
        context_->IASetVertexBuffers(0, 1, &nothing, &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(is("shader") ? otherShader() : ps_.Get(), nullptr, 0);
        context_->OMSetBlendState(is("blend")        ? wrongBlend_.Get()
                                      : is("alpha blend") ? alphaBlend_.Get()
                                                          : plateBlend_.Get(),
                                  nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(is("depth write")   ? writingDepth_.Get()
                                         : is("depth func")  ? lessDepth_.Get()
                                         : is("stencil")     ? stencilDepth_.Get()
                                         : is("depth test")  ? noDepth_.Get()
                                                             : plateDepth_.Get(),
                                         0);
        ID3D11RenderTargetView* targets[2] = { is("sdr") ? floatView_.Get() : targetView_.Get(),
                                               is("two targets") ? slotView_.Get() : nullptr };
        const UINT targetCount = is("two targets") ? 2 : 1;
        ID3D11DepthStencilView* depthView =
            is("no depth") ? nullptr : (is("depth size") ? smallDepthView_.Get() : depthView_.Get());
        // One call binds the colour targets and clears the UAV range above them (a null view in it means "no UAV"); the
        // ranges must not overlap, so the UAV range starts at the number of colour targets.
        ID3D11UnorderedAccessView* uavs[1] = { is("uav") ? uav_.Get() : nullptr };
        context_->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, targets, depthView, targetCount, 1, uavs,
                                                           nullptr);
        const D3D11_VIEWPORT viewport { 0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_), 0.f, 1.f };
        context_->RSSetViewports(1, &viewport);
    }

    // Every state a plate draw needs, so a test can start from a correct draw and break one thing.
    // Every condition the classifier reads from the context. The context identity itself is the hook's test (the engine is
    // only reached for the game's own immediate context), so it cannot be broken from inside a draw.
    static constexpr const char* kBroken[] = { "two targets", "sdr",     "uav",        "no depth", "depth size",
                                               "depth test",  "depth write", "depth func", "stencil", "blend",
                                               "alpha blend", "shader",  "layout" };
    static constexpr size_t kBrokenCount = sizeof(kBroken) / sizeof(kBroken[0]);

    // Reads one pixel back through a staging copy.
    uint32_t PixelOf(ID3D11Texture2D* texture, UINT x, UINT y)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        Require(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging)), "staging texture");
        context_->CopyResource(staging.Get(), texture);
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped {};
        Require(SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map staging");
        const uint32_t pixel = *reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(mapped.pData) +
                                                                  static_cast<size_t>(y) * mapped.RowPitch + x * 4);
        context_->Unmap(staging.Get(), 0);
        return pixel;
    }

    ID3D11Device* Device() { return device_; }
    ID3D11DeviceContext* Context() { return context_; }
    ID3D11Texture2D* Target() { return target_.Get(); }
    ID3D11RenderTargetView* TargetView() { return targetView_.Get(); }
    ID3D11DepthStencilView* DepthView() { return depthView_.Get(); }
    ID3D11Texture2D* SlotTexture() { return slotTexture_.Get(); }
    ID3D11RenderTargetView* SlotView() { return slotView_.Get(); }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

  private:
    ID3D11PixelShader* otherShader()
    {
        if (!otherPixel_)
        {
            // The same bytecode without the plate stamp: a real shader that is not a nameplate variant.
            auto code = Compile(kPixelShader, "PSMain", "ps_5_0");
            Require(SUCCEEDED(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                         &otherPixel_)),
                    "other pixel shader");
        }
        return otherPixel_.Get();
    }

    ComPtr<ID3D11DepthStencilState> DepthState(bool enable, D3D11_DEPTH_WRITE_MASK write, D3D11_COMPARISON_FUNC func,
                                               bool stencil)
    {
        D3D11_DEPTH_STENCIL_DESC desc {};
        desc.DepthEnable = enable ? TRUE : FALSE;
        desc.DepthWriteMask = write;
        desc.DepthFunc = func;
        desc.StencilEnable = stencil ? TRUE : FALSE;
        desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        // A stencil-enabled state must carry valid operations (0 is not a valid D3D11_STENCIL_OP).
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        desc.BackFace = desc.FrontFace;
        ComPtr<ID3D11DepthStencilState> state;
        Require(SUCCEEDED(device_->CreateDepthStencilState(&desc, &state)), "depth-stencil state");
        return state;
    }

    ComPtr<ID3D11BlendState> BlendState(D3D11_BLEND src, D3D11_BLEND dest, D3D11_BLEND srcAlpha,
                                       D3D11_BLEND destAlpha)
    {
        D3D11_BLEND_DESC desc {};
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = src;
        desc.RenderTarget[0].DestBlend = dest;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = srcAlpha;
        desc.RenderTarget[0].DestBlendAlpha = destAlpha;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> state;
        Require(SUCCEEDED(device_->CreateBlendState(&desc, &state)), "blend state");
        return state;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11RenderTargetView> targetView_;
    ComPtr<ID3D11Texture2D> depthTexture_;
    ComPtr<ID3D11DepthStencilView> depthView_;
    ComPtr<ID3D11Texture2D> smallDepthTexture_;
    ComPtr<ID3D11DepthStencilView> smallDepthView_;
    ComPtr<ID3D11Texture2D> floatTexture_;
    ComPtr<ID3D11RenderTargetView> floatView_;
    ComPtr<ID3D11Texture2D> slotTexture_;
    ComPtr<ID3D11RenderTargetView> slotView_;
    ComPtr<ID3D11DepthStencilState> plateDepth_;
    ComPtr<ID3D11DepthStencilState> writingDepth_;
    ComPtr<ID3D11DepthStencilState> lessDepth_;
    ComPtr<ID3D11DepthStencilState> stencilDepth_;
    ComPtr<ID3D11DepthStencilState> noDepth_;
    ComPtr<ID3D11BlendState> plateBlend_;
    ComPtr<ID3D11BlendState> wrongBlend_;
    ComPtr<ID3D11BlendState> additiveBlend_;
    ComPtr<ID3D11BlendState> alphaBlend_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11PixelShader> otherPixel_;
    ComPtr<ID3D11InputLayout> plateLayout_;
    ComPtr<ID3D11InputLayout> otherLayout_;
    ComPtr<ID3D11Texture2D> uavTexture_;
    ComPtr<ID3D11UnorderedAccessView> uav_;
};
} // namespace plate_game_test
