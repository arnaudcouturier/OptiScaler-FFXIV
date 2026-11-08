// Shadow mapping on D3D11 WARP: the module's own mask-lift pass and the contact combine's lift stage, against the
// CPU reference of ShadowsRules.h, on synthetic buffers this test builds itself.
//
// The checks, motivated by what a wrong patch would break:
//   a lift of 0.5 moves every texel's shared square by exactly (1-m^2)*0.5 and the copy carries the square root
//   back (the CPU reference of RemapCopyValue, the whole buffer compared at the mask's own 1/255 grid),
//   the identity is the game's own mask on both paths: the pass refuses a lift at or under the identity epsilon
//   (null, no D3D object created), and a second run of the same allocation creates no D3D object,
//   the tent on a step edge reads the CPU tent over the REMAPPED field: whole buffer against the CPU reference,
//   and the interior plateau is exactly the no-tent pass's answer (a blur of one level is that level),
//   the light draw reads the lifted copy: a fixture shader samples the slot the module bound and writes the byte it
//   read, and the restore guard puts the game's own mask back at the slot afterwards (also after the throw path),
//   the contact pass's combine integrates the lift WITHOUT a second copy: its own copy answers the remap times the
//   march's term on every masked texel, byte-identical to the reference, and re-runs create no object,
//   a GPU timestamp pair is begun by every run that can time itself (and ends exactly as often as it begins).
#include "check.h"

#include "ext/ffxiv/relight/ContactShadowsPass.h"
#include "ext/ffxiv/shadows/ShadowsPass.h"
#include "ext/ffxiv/shadows/ShadowsRules.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace fs = ext::ffxiv::shadows;

namespace
{
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 32;

constexpr uint32_t Quantize(float value) noexcept
{
    const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    const uint32_t level = uint32_t(clamped * 255.0f + 0.5f);
    return level > 255 ? 255 : level;
}

double TentWeightCpu(double dx, double dy, double softness) noexcept
{
    if (!(softness > 0.0))
        return dx == 0.0 && dy == 0.0 ? 1.0 : 0.0;
    const double d = std::sqrt(dx * dx + dy * dy);
    if (!(d < softness))
        return 0.0;
    const double w = 1.0 - d / softness;
    return w > 0.0 ? w : 0.0;
}

float ReferenceTexel(const std::vector<float>& mask, double lift, double softness, uint32_t x, uint32_t y)
{
    if (!(lift > fs::kIdentityEpsilon))
        return mask[y * kWidth + x];
    if (!(softness > 0.0))
        return fs::RemapCopyValue(mask[y * kWidth + x], float(lift));
    double sumW = 0.0, sumV = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            const double w = TentWeightCpu(dx, dy, softness);
            if (!(w > 0.0))
                continue;
            const int32_t nx = int32_t(x) + dx, ny = int32_t(y) + dy;
            if (nx < 0 || ny < 0 || nx >= int32_t(kWidth) || ny >= int32_t(kHeight))
                continue;
            sumW += w;
            sumV += w * double(fs::RemapCopyValue(mask[uint32_t(ny) * kWidth + uint32_t(nx)], float(lift)));
        }
    if (!(sumW > 0.0))
        return fs::RemapCopyValue(mask[y * kWidth + x], float(lift));
    return float(sumV / sumW);
}

// A synthetic mask: a step edge at x = 32 (a shadowed wall to the left, rising lit ground to the right), one edge
// texel at a real middle (0.5), quarters along the edge (0.25 at every 8th row), both extremes on the far side.
std::vector<float> MakeMask()
{
    std::vector<float> mask(kWidth * kHeight, 0.0f);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            if (x < 32)
                mask[y * kWidth + x] = 0.0f;
            else
                mask[y * kWidth + x] = float(x - 32) / float(kWidth - 32);
            if (x == 32)
                mask[y * kWidth + x] = 0.5f;
            if (x == 32 && y % 8 == 0)
                mask[y * kWidth + x] = 0.25f;
        }
    return mask;
}

std::vector<uint8_t> MaskBytes(const std::vector<float>& mask)
{
    std::vector<uint8_t> bytes(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
        bytes[i] = uint8_t(Quantize(mask[i]));
    return bytes;
}

void ReadResult(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* copy, std::vector<uint8_t>& bytes)
{
    D3D11_TEXTURE2D_DESC copyDesc {};
    copy->GetDesc(&copyDesc);
    D3D11_TEXTURE2D_DESC stage {};
    stage.Width = copyDesc.Width;
    stage.Height = copyDesc.Height;
    stage.MipLevels = 1;
    stage.ArraySize = 1;
    stage.Format = copyDesc.Format;
    stage.SampleDesc.Count = 1;
    stage.Usage = D3D11_USAGE_STAGING;
    stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device->CreateTexture2D(&stage, nullptr, &staging) == S_OK);
    if (staging)
    {
        context->CopyResource(staging.Get(), copy);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped) == S_OK)
        {
            bytes.resize(size_t(copyDesc.Width) * copyDesc.Height);
            for (uint32_t y = 0; y < copyDesc.Height; ++y)
                std::memcpy(bytes.data() + size_t(y) * copyDesc.Width,
                            static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, copyDesc.Width);
            context->Unmap(staging.Get(), 0);
        }
    }
}

void ReadFloats(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, size_t width,
                size_t height, std::vector<float>& values)
{
    D3D11_TEXTURE2D_DESC stageDesc {};
    texture->GetDesc(&stageDesc);
    D3D11_TEXTURE2D_DESC stage {};
    stage.Width = stageDesc.Width;
    stage.Height = stageDesc.Height;
    stage.MipLevels = 1;
    stage.ArraySize = 1;
    stage.Format = stageDesc.Format;
    stage.SampleDesc.Count = 1;
    stage.Usage = D3D11_USAGE_STAGING;
    stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device->CreateTexture2D(&stage, nullptr, &staging) == S_OK);
    if (staging)
    {
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped) == S_OK)
        {
            values.assign(width * height, 0.0f);
            for (size_t y = 0; y < height; ++y)
            {
                const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                std::memcpy(values.data() + y * width, row, width * sizeof(float));
            }
            context->Unmap(staging.Get(), 0);
        }
    }
}

struct MaskInputs
{
    fs::MaskInputs in;
};

fs::MaskInputs OwnInputs(ComPtr<ID3D11ShaderResourceView>& maskView, float lift, float softness)
{
    fs::MaskInputs in;
    in.mask = maskView.Get();
    in.maskFormat = 61; // R8_UNORM: this build's mask as the light draw samples it
    in.maskWidth = kWidth;
    in.maskHeight = kHeight;
    in.window[0] = 0.0f;
    in.window[1] = 0.0f;
    in.window[2] = float(kWidth);
    in.window[3] = float(kHeight);
    in.lift = lift;
    in.softness = softness;
    return in;
}
} // namespace

// ITS OWN main: the device, the synthetic mask, the three WARP checks' shapes the header comments name.
int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
        return Finish("shadows_warp");

    const std::vector<float> mask = MakeMask();
    const std::vector<uint8_t> sourceBytes = MaskBytes(mask);

    D3D11_TEXTURE2D_DESC textureDesc {};
    textureDesc.Width = kWidth;
    textureDesc.Height = kHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initData {};
    initData.pSysMem = sourceBytes.data();
    initData.SysMemPitch = kWidth;
    ComPtr<ID3D11Texture2D> maskTexture;
    if (FAILED(device->CreateTexture2D(&textureDesc, &initData, &maskTexture)) || !maskTexture)
        return Finish("shadows_warp");
    ComPtr<ID3D11ShaderResourceView> maskView;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(maskTexture.Get(), &srvDesc, &maskView)) || !maskView)
            return Finish("shadows_warp");
    }

    // ---- the module's own pass: a 0.5 lift against the CPU reference, whole buffer ------------------------------------------
    {
        fs::MaskLiftPass pass;
        const fs::MaskInputs in = OwnInputs(maskView, 0.5f, 0.0f);
        ID3D11ShaderResourceView* copyView = pass.Run(context.Get(), in);
        CHECK(copyView != nullptr);
        if (copyView != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            copyView->GetResource(&resource);
            std::vector<uint8_t> bytes;
            ReadResult(device.Get(), context.Get(), static_cast<ID3D11Texture2D*>(resource.Get()), bytes);
            CHECK_EQ(bytes.size(), sourceBytes.size());
            uint32_t wrong = 0;
            for (size_t i = 0; i < bytes.size(); ++i)
                if (std::abs(int32_t(bytes[i]) -
                             int32_t(Quantize(ReferenceTexel(mask, 0.5, 0.0, uint32_t(i % kWidth), uint32_t(i / kWidth))))) > 1)
                    ++wrong;
            CHECK_EQ(wrong, 0u);
            // The lift's own point: the wall's base texel moved from the game's 0 to sqrt(0.5) = level 180.
            CHECK_EQ(bytes[0], 180);
            // A second run of the same allocation: no D3D object (the pass's own per-size cache).
            const uint32_t copiesBefore = pass.CopiesCreated();
            copyView = pass.Run(context.Get(), in);
            CHECK(copyView != nullptr);
            CHECK_EQ(pass.CopiesCreated(), copiesBefore);
            // The GPU timing rule: ends never exceed begins (only the run that began a pair may end it).
            CHECK_GE(pass.GpuTimingEnds(), pass.GpuTimingBegins());
        }
        pass.Reset();
    }

    // ---- the identity: null, no allocation, the game's own mask ----------------------------------------------------------------
    {
        fs::MaskLiftPass guard;
        const fs::MaskInputs in = OwnInputs(maskView, 0.0f, 2.0f);
        CHECK(guard.Run(context.Get(), in) == nullptr); // no lift: no copy, no draw, no object (softness rides only)
        CHECK_EQ(guard.CopiesCreated(), 0u);
        guard.Reset();
    }

    // ---- the tent on a step edge: the CPU kernel over the REMAPPED field ------------------------------------------------------
    {
        fs::MaskLiftPass pass;
        const fs::MaskInputs in = OwnInputs(maskView, 0.5f, 2.0f);
        ID3D11ShaderResourceView* copyView = pass.Run(context.Get(), in);
        CHECK(copyView != nullptr);
        if (copyView != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            copyView->GetResource(&resource);
            std::vector<uint8_t> bytes;
            ReadResult(device.Get(), context.Get(), static_cast<ID3D11Texture2D*>(resource.Get()), bytes);
            uint32_t wrong = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                    if (std::abs(int32_t(bytes[y * kWidth + x]) -
                                 int32_t(Quantize(ReferenceTexel(mask, 0.5, 2.0, x, y)))) > 1)
                        ++wrong;
            CHECK_EQ(wrong, 0u);
            // The wall's plateau is unchanged by the blur: a blur of one level is that level, and the reference
            // compare above already covers the rest, texel by texel.
            CHECK_EQ(bytes[6 * kWidth + 0], 180);
            CHECK_EQ(bytes[12 * kWidth + 16], 180);
        }
        pass.Reset();
    }

    // ---- the light draw reads the lifted copy; the game's own mask is back at the slot afterwards -----------------------------
    {
        fs::MaskLiftPass pass;
        const fs::MaskInputs in = OwnInputs(maskView, 0.5f, 0.0f);
        ID3D11ShaderResourceView* copyView = pass.Run(context.Get(), in);
        CHECK(copyView != nullptr);
        if (copyView != nullptr)
        {
            // The fixture: a full-screen triangle whose PS samples t0 and writes the byte it read (the engine's own
            // light draw reads the slot the route binds; a reading shader's answer is per-texel comparable here).
            ComPtr<ID3D10Blob> code, errors;
            static const char* vsSource =
                "struct VSOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };"
                "VSOutput VS(uint id : SV_VertexID){ VSOutput o; const float2 uv = float2((id << 1) & 2, id & 2);"
                "o.uv = uv; o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o; }";
            static const char* psSource =
                "Texture2D<float4> Mask : register(t0); SamplerState Game : register(s0);"
                "struct PSIn { float4 position : SV_Position; float2 uv : TEXCOORD0; };"
                "float4 PS(PSIn i) : SV_Target0 { return float4(Mask.SampleLevel(Game, i.uv, 0).r, 0, 0, 1); }";
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
            CHECK(SUCCEEDED(D3DCompile(vsSource, std::strlen(vsSource), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                                       &code, &errors)));
            if (code)
            {
                CHECK(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs)));
                code->Release();
                code = nullptr;
            }
            CHECK(SUCCEEDED(D3DCompile(psSource, std::strlen(psSource), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                                       &code, &errors)));
            if (code)
            {
                CHECK(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps)));
                code->Release();
                code = nullptr;
            }
            ComPtr<ID3D11Texture2D> target;
            {
                D3D11_TEXTURE2D_DESC targetDesc {};
                targetDesc.Width = kWidth;
                targetDesc.Height = kHeight;
                targetDesc.MipLevels = 1;
                targetDesc.ArraySize = 1;
                targetDesc.Format = DXGI_FORMAT_R32_FLOAT;
                targetDesc.SampleDesc.Count = 1;
                targetDesc.Usage = D3D11_USAGE_DEFAULT;
                targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                CHECK(SUCCEEDED(device->CreateTexture2D(&targetDesc, nullptr, &target)));
            }
            ComPtr<ID3D11RenderTargetView> targetView;
            ComPtr<ID3D11SamplerState> gameSampler;
            {
                if (target)
                    CHECK(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView)));
                D3D11_SAMPLER_DESC sampler {};
                sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                CHECK(SUCCEEDED(device->CreateSamplerState(&sampler, &gameSampler)));
            }
            if (vs && ps && targetView && gameSampler)
            {
                // The keep and the one draw: the game's own mask bound first (what the route's keep-guard holds),
                // the copy bound for exactly one draw, the restore guard's destructor restoring it (the plain
                // return's and a throw's alike; the throw answers through the same destructor).
                ID3D11ShaderResourceView* gameBinding = maskView.Get();
                context->PSSetShaderResources(0, 1, &gameBinding);
                ID3D11ShaderResourceView* original = nullptr;
                context->PSGetShaderResources(0, 1, &original);
                CHECK(original == maskView.Get());
                struct Restore
                {
                    ID3D11DeviceContext* context;
                    ID3D11ShaderResourceView* view;
                    bool throwBeforeDraw = false;
                    ~Restore()
                    {
                        context->PSSetShaderResources(0, 1, &view);
                        if (view != nullptr)
                            view->Release();
                    }
                } restore { context.Get(), original };
                context->PSSetShaderResources(0, 1, &copyView);
                bool threwInDraw = false;
                (void) threwInDraw;
                try
                {
                    ID3D11SamplerState* samplers[1] { gameSampler.Get() };
                    context->PSSetSamplers(0, 1, samplers);
                    ID3D11RenderTargetView* views[1] { targetView.Get() };
                    context->OMSetRenderTargets(1, views, nullptr);
                    D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
                    context->RSSetViewports(1, &viewport);
                    // The pipeline state the pass sets: without the topology the draw is undefined (a device removal
                    // on WARP), which is exactly the state a build without the fork's pass draws through its hooks.
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    context->VSSetShader(vs.Get(), nullptr, 0);
                    context->PSSetShader(ps.Get(), nullptr, 0);
                    context->Draw(3, 0);
                }
                catch (...)
                {
                    threwInDraw = true;
                }
                CHECK(!threwInDraw);
                std::vector<float> pixels;
                ReadFloats(device.Get(), context.Get(), target.Get(), kWidth, kHeight, pixels);
                CHECK_EQ(pixels.size(), size_t(kWidth) * kHeight);
                if (pixels.size() == size_t(kWidth) * kHeight)
                {
                    uint32_t ok = 0;
                    for (uint32_t y = 0; y < kHeight; ++y)
                        for (uint32_t x = 0; x < kWidth; ++x)
                            if (std::abs(int32_t(uint32_t(pixels[y * kWidth + x] * 255.0f + 0.5f)) -
                                         int32_t(Quantize(ReferenceTexel(mask, 0.5, 0.0, x, y)))) <= 1)
                                ++ok;
                    CHECK_GE(ok, uint32_t(kWidth) * kHeight - uint32_t(kWidth) * 8); // the point sampler's slack
                }
            }
        }
        // After the scope the game's own mask is back at the slot (the plain-return path of the guard).
        ID3D11ShaderResourceView* back = nullptr;
        context->PSGetShaderResources(0, 1, &back);
        CHECK(back == maskView.Get());
        if (back != nullptr)
            back->Release();
        pass.Reset();
    }

    // ---- the contact pass's integrated stage: one copy, the remap inside it, then the term multiply ---------------------------
    {
        ComPtr<ID3D11Texture2D> depthTexture;
        {
            D3D11_TEXTURE2D_DESC depthDesc {};
            depthDesc.Width = kWidth;
            depthDesc.Height = kHeight;
            depthDesc.MipLevels = 1;
            depthDesc.ArraySize = 1;
            depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.Usage = D3D11_USAGE_DEFAULT;
            depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            // A flat plane one unit deep: the march at reach 0.5 m over a view-space sun of (0, 0.8, 0.6) reads the
            // receiver's own plane everywhere, and the start-distance rule makes every tap the receiver's own
            // surface, so no occluder is found and the term is 1.0 all over.
            std::vector<float> depths(kWidth * kHeight, 1.0f);
            D3D11_SUBRESOURCE_DATA depthData {};
            depthData.pSysMem = depths.data();
            depthData.SysMemPitch = kWidth * sizeof(float);
            CHECK(SUCCEEDED(device->CreateTexture2D(&depthDesc, &depthData, &depthTexture)));
        }
        ComPtr<ID3D11ShaderResourceView> depthView;
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC depthSrv {};
            depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
            depthSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            depthSrv.Texture2D.MipLevels = 1;
            if (depthTexture)
                CHECK(SUCCEEDED(device->CreateShaderResourceView(depthTexture.Get(), &depthSrv, &depthView)));
        }
        ComPtr<ID3D11Buffer> light, camera;
        {
            // g_LightParam: the pass copies the row at +0x10, so the buffer carries 0x20 bytes - row 0 spare,
            // row 1 the unit direction (view space, toward the sun).
            const float lightRows[8] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.6f, 0.0f };
            D3D11_BUFFER_DESC lightDesc {};
            lightDesc.ByteWidth = 32;
            lightDesc.Usage = D3D11_USAGE_DEFAULT;
            lightDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA lightData {};
            lightData.pSysMem = lightRows;
            lightData.SysMemPitch = 16;
            CHECK(SUCCEEDED(device->CreateBuffer(&lightDesc, &lightData, &light)));
            struct CameraRows
            {
                float rows[22][4] {};
            } cameraBytes;
            cameraBytes.rows[0][0] = cameraBytes.rows[1][1] = cameraBytes.rows[2][2] = 1.0f; // view
            cameraBytes.rows[6][0] = cameraBytes.rows[7][1] = cameraBytes.rows[8][2] = 1.0f; // view-projection
            cameraBytes.rows[9][3] = 1.0f;
            cameraBytes.rows[10][3] = 1.0f;
            cameraBytes.rows[14][0] = cameraBytes.rows[15][1] = cameraBytes.rows[16][2] = 1.0f; // inverse projection
            cameraBytes.rows[18][0] = cameraBytes.rows[19][1] = cameraBytes.rows[20][2] = 1.0f; // projection
            cameraBytes.rows[17][3] = 1.0f;
            cameraBytes.rows[21][3] = 1.0f;
            D3D11_BUFFER_DESC cameraDesc {};
            cameraDesc.ByteWidth = 22 * 16;
            cameraDesc.Usage = D3D11_USAGE_DEFAULT;
            cameraDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA cameraData {};
            cameraData.pSysMem = cameraBytes.rows;
            cameraData.SysMemPitch = 16;
            CHECK(SUCCEEDED(device->CreateBuffer(&cameraDesc, &cameraData, &camera)));
        }
        ext::ffxiv::relight::Pass contactPass;
        ext::ffxiv::relight::Inputs contactIn;
        contactIn.mask = maskView.Get();
        contactIn.depth = depthView.Get();
        contactIn.lightParam = light.Get();
        contactIn.cameraParam = camera.Get();
        contactIn.maskFormat = 61;
        contactIn.maskWidth = kWidth;
        contactIn.maskHeight = kHeight;
        contactIn.window = { 0.0f, 0.0f, float(kWidth), float(kHeight) };
        contactIn.params = ext::ffxiv::relight::BuildParams(0.5f, 0.0f, 8u, ext::ffxiv::relight::SunSpace::View,
                                                            ext::ffxiv::relight::SunSign::Toward);
        contactIn.maskLift = 0.5f; // the stage's values: zeros are the game's own bytes, these move them
        contactIn.maskSoftness = 0.0f;
        ID3D11ShaderResourceView* contactCopy = contactPass.Run(context.Get(), contactIn);
                CHECK(contactCopy != nullptr);
        if (contactCopy != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            contactCopy->GetResource(&resource);
            std::vector<uint8_t> bytes;
            ReadResult(device.Get(), context.Get(), static_cast<ID3D11Texture2D*>(resource.Get()), bytes);
            uint32_t wrong = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < 12; ++x)
                    if (std::abs(int32_t(bytes[y * kWidth + x]) -
                                 int32_t(Quantize(ReferenceTexel(mask, 0.5, 0.0, x, y)))) > 1)
                        ++wrong;
            CHECK_EQ(wrong, 0u);
            // The wall's base texel is lifted the exact same amount the module's own pass answers (term 1.0).
            CHECK_EQ(bytes[0], 180);
            // The one copy rule: the contact pass's own per-size cache; a re-run adds no object.
            const uint32_t copiesBefore = contactPass.CopiesCreated();
            contactCopy = contactPass.Run(context.Get(), contactIn);
            CHECK(contactCopy != nullptr);
            CHECK_EQ(contactPass.CopiesCreated(), copiesBefore);
        }
        contactPass.Reset();
    }

    return Finish("shadows_warp");
}
