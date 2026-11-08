// Light shafts: the pass (ext/ffxiv/engine/ShaftsPass.cpp and its shaders) on the D3D11 WARP software adapter, on synthetic buffers this test
// builds itself.
//
// The frame is 128x96: the depth is the far plane (sky) on the right half and a wall at half depth on the left, the scene is a flat 0.5 on the
// wall and 0.25 under the sky, and the sun sits on the right edge of the window. What is checked is what a wrong mask, a wrong blur direction or
// a wrong composite would break:
//   the sky mask is 1 exactly where the depth is the far plane and 0 on the wall,
//   the wall next to the sky is brighter than it was, and never darker, while the far side of the frame is unchanged,
//   the increase falls off with the distance from the sun (a blur that did not move toward the sun, or one that summed the whole mask evenly,
//   would not),
//   the sky keeps its own value byte for byte (the shafts never glow on the sky they come from),
//   intensity 0 is a byte-exact copy of the scene,
//   a second run of the same sizes creates no texture, a smaller render window re-creates only the half-resolution pair, and a wider scene
//   re-creates the scene copy,
//   a binding the pass cannot read (an 8-bit scene, a depth of another format, an empty window) returns null with a reason and runs no draw.
#include "check.h"

#include "ext/ffxiv/engine/ShaftsPass.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <utility>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::shafts;

namespace
{
constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 96;

// The values the fixture uses, all exactly representable as half floats.
constexpr float kWall = 0.5f;
constexpr float kSky = 0.25f;

// The blur's per-pixel offset, as LightShafts.hlsl's Hash01 computes it: an integer hash of the pixel's own coordinate, so every pixel's march
// starts at a different fraction of its first step and the term is not one lattice shared by the whole frame. SV_Position.x is the pixel centre,
// so its integer part is the pixel's own index.
uint32_t HashBits(int x, int y)
{
    uint32_t h = uint32_t(x) * 1664525u + uint32_t(y) * 1013904223u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h & 0xFFFFFFu;
}

float Hash01(int x, int y) { return float(HashBits(x, y)) * (1.0f / 16777216.0f); }

// A half float from its bits: the fixture only needs the exact values above and its own outputs.
float FromHalf(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    if (exponent == 0)
        return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31)
        return mantissa == 0 ? sign * INFINITY : NAN;
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}

uint16_t ToHalf(float value)
{
    // Only the fixture's own values go through here: 0.5, 0.25 and whole halves of them.
    if (value == 0.0f)
        return 0;
    int exponent = 0;
    float mantissa = std::frexp(std::fabs(value), &exponent); // value = mantissa * 2^exponent, 0.5 <= mantissa < 1
    const uint16_t sign = value < 0.0f ? 0x8000u : 0u;
    const int biased = exponent - 1 + 15;
    const uint32_t bits = uint32_t((mantissa * 2.0f - 1.0f) * 1024.0f + 0.5f) & 0x3FFu;
    return uint16_t(sign | (uint16_t(biased & 0x1F) << 10) | bits);
}

struct Frame
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                           &context)) &&
               device && context;
    }

    // The frame's depth: the far plane (0, reversed-Z) on the right half, the wall at half depth on the left. It is bound as R32_FLOAT, one of
    // the two formats the pass reads.
    ComPtr<ID3D11ShaderResourceView> Depth(uint32_t width, uint32_t height)
    {
        std::vector<float> depth(size_t(width) * height, kWall);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = width / 2; x < width; ++x)
                depth[size_t(y) * width + x] = 0.0f;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { depth.data(), width * sizeof(float), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
            return {};
        return view;
    }

    // The lit scene: a flat wall colour on the left and a flat sky colour on the right, R16G16B16A16_FLOAT.
    ComPtr<ID3D11Texture2D> SceneTexture(uint32_t width, uint32_t height)
    {
        std::vector<uint16_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float value = x < width / 2 ? kWall : kSky;
                const size_t at = (size_t(y) * width + x) * 4;
                pixels[at + 0] = ToHalf(value);
                pixels[at + 1] = ToHalf(value);
                pixels[at + 2] = ToHalf(value);
                pixels[at + 3] = ToHalf(1.0f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), width * 4 * sizeof(uint16_t), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        return texture;
    }

    // The lit scene with a value per texel: four exact halves that repeat diagonally, so a copy that is one pixel off is visible in the data and
    // not only in the picture.
    ComPtr<ID3D11Texture2D> GradientTexture(uint32_t width, uint32_t height)
    {
        std::vector<uint16_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float value = 0.25f * float(1 + ((x + y * 2) % 4)); // 0.25, 0.5, 0.75, 1.0: exact as a half
                const size_t at = (size_t(y) * width + x) * 4;
                pixels[at + 0] = ToHalf(value);
                pixels[at + 1] = ToHalf(value);
                pixels[at + 2] = ToHalf(value);
                pixels[at + 3] = ToHalf(1.0f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), width * 4 * sizeof(uint16_t), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        return texture;
    }

    // A scene of one colour, with a different one at a single texel: the sun's own pixel, so a pass that reads it instead of the sky around it is
    // caught. Each of the two is a colour, because the sampler's job is the sky's hue as much as its level.
    ComPtr<ID3D11Texture2D> SkyColourTexture(uint32_t width, uint32_t height, const float sky[3], const float sun[3], uint32_t sunX, uint32_t sunY)
    {
        std::vector<uint16_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float* value = (x == sunX && y == sunY) ? sun : sky;
                const size_t at = (size_t(y) * width + x) * 4;
                pixels[at + 0] = ToHalf(value[0]);
                pixels[at + 1] = ToHalf(value[1]);
                pixels[at + 2] = ToHalf(value[2]);
                pixels[at + 3] = ToHalf(1.0f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), width * 4 * sizeof(uint16_t), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        return texture;
    }

    // A scene of one colour with a different one at a single texel: the "leaf in front of the sun" the colour pass has to look past.
    ComPtr<ID3D11Texture2D> LeafTexture(uint32_t width, uint32_t height, float background, float leaf, uint32_t leafX, uint32_t leafY)
    {
        std::vector<uint16_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float value = (x == leafX && y == leafY) ? leaf : background;
                const size_t at = (size_t(y) * width + x) * 4;
                pixels[at + 0] = ToHalf(value);
                pixels[at + 1] = ToHalf(value);
                pixels[at + 2] = ToHalf(value);
                pixels[at + 3] = ToHalf(1.0f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { pixels.data(), width * 4 * sizeof(uint16_t), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        return texture;
    }

    // The frame's depth with no sky at all: every pixel is a surface, so the mask is zero everywhere and nothing is added.
    ComPtr<ID3D11ShaderResourceView> DepthAllWall(uint32_t width, uint32_t height)
    {
        std::vector<float> depth(size_t(width) * height, kWall);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { depth.data(), width * sizeof(float), 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || !texture)
            return {};
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
            return {};
        return view;
    }

    // The frame's depth the live build binds: the R24G8 buffer every light draw carries, read through an R24_UNORM_X8_TYPELESS view (format
    // 46), with every texel at the far plane. The pass reads it through the same Load path as R32_FLOAT; a build that only accepted the
    // buffer's own typeless format (44) would refuse this depth on every frame.
    ComPtr<ID3D11ShaderResourceView> Depth24Sky(uint32_t width, uint32_t height)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) || !texture)
            return {};
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> dsv;
        if (FAILED(device->CreateDepthStencilView(texture.Get(), &dsvDesc, &dsv)))
            return {};
        // Reversed-Z: 0 is the far plane, so a cleared depth buffer is sky everywhere.
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &srvDesc, &view)))
            return {};
        return view;
    }

    ComPtr<ID3D11ShaderResourceView> View(ID3D11Resource* resource, DXGI_FORMAT format)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        desc.Format = format;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(resource, &desc, &view)))
            return {};
        return view;
    }

    // A resource's pixels, through a staging copy of the same format. The row pitch of the staging texture is not the packed width, so the
    // caller reads row by row.
    template <class T>
    bool Readback(ID3D11Resource* resource, uint32_t width, uint32_t height, DXGI_FORMAT format, std::vector<T>& out)
    {
        D3D11_TEXTURE2D_DESC desc {};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || !copy)
            return false;
        context->CopyResource(copy.Get(), resource);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            return false;
        out.assign(size_t(width) * height * (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4u : 1u), T {});
        const size_t perPixel = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4u : 1u;
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&out[size_t(y) * width * perPixel], static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(width) * perPixel * sizeof(T));
        context->Unmap(copy.Get(), 0);
        return true;
    }
};

Inputs MakeInputs(ID3D11ShaderResourceView* scene, uint32_t sceneFormat, ID3D11ShaderResourceView* depth, uint32_t depthFormat,
                  uint32_t width, uint32_t height, Window window)
{
    Inputs inputs;
    inputs.scene = scene;
    inputs.depth = depth;
    inputs.sceneFormat = sceneFormat;
    inputs.sceneWidth = width;
    inputs.sceneHeight = height;
    inputs.depthFormat = depthFormat;
    inputs.depthWidth = width;
    inputs.depthHeight = height;
    inputs.window = window;
    inputs.sun.unit = true;
    inputs.sun.screenX = float(width) - 1.0f;
    inputs.sun.screenY = float(height) * 0.5f;
    // The fixture's reach: with the default density the blur reaches most of the way to the sun from every pixel, which a real frame's small
    // sky patch never lets it do; 0.35 keeps the fixture's half-frame of sky a local source, so the far side of the frame can be checked for
    // being left alone.
    inputs.params = BuildParams(0.5f, 0.96f, 48, 0.35f);
    inputs.tint.fromSky = true; // the live build's directional light reports no colour: the sky's colour at the sun is used
    // The fixture's frame is the captured day frame's shape: its measured white point, not the fallback the pass starts at. The pass never invents a
    // white of its own; this is the one the rules measured from the pair, and the bright case below moves it down to 0.2.
    inputs.whitePoint = kVerifiedWhitePoint;
    return inputs;
}
} // namespace

int main()
{
    Frame frame;
    CHECK(frame.Create());
    if (!frame.context)
        return Finish("shafts_warp");

    ComPtr<ID3D11ShaderResourceView> depth = frame.Depth(kWidth, kHeight);
    ComPtr<ID3D11Texture2D> sceneTexture = frame.SceneTexture(kWidth, kHeight);
    CHECK(depth != nullptr && sceneTexture != nullptr);
    if (!depth || !sceneTexture)
        return Finish("shafts_warp");
    ComPtr<ID3D11ShaderResourceView> scene = frame.View(sceneTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(scene != nullptr);

    const Window window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
    const Inputs inputs = MakeInputs(scene.Get(), 10, depth.Get(), 41, kWidth, kHeight, window);

    Pass pass;
    ID3D11ShaderResourceView* copy = pass.Run(frame.context.Get(), inputs);
    CHECK(copy != nullptr);
    CHECK(pass.Failure() == nullptr);
    if (copy == nullptr)
        return Finish("shafts_warp");

    // ---- the sky mask: 1 exactly where the depth is the far plane (the right half), 0 on the wall. A mask that ignored the depth would be 1
    //      everywhere (a full-frame glow), and one that used the other convention would be 1 on the wall.
    {
        ComPtr<ID3D11Resource> resource;
        pass.MaskView()->GetResource(&resource);
        std::vector<unsigned char> mask;
        CHECK(frame.Readback(resource.Get(), HalfExtent(float(kWidth)), HalfExtent(float(kHeight)), DXGI_FORMAT_R8_UNORM, mask));
        CHECK_EQ(mask.size(), size_t(kWidth / 2) * (kHeight / 2));
        uint32_t skyPixels = 0, wallPixels = 0, wrong = 0;
        for (uint32_t y = 0; y < kHeight / 2; ++y)
            for (uint32_t x = 0; x < kWidth / 2; ++x)
            {
                const unsigned char value = mask[size_t(y) * (kWidth / 2) + x];
                if (x >= kWidth / 4)
                {
                    ++skyPixels;
                    if (value < 200)
                        ++wrong;
                }
                else
                {
                    ++wallPixels;
                    if (value > 55)
                        ++wrong;
                }
            }
        CHECK(skyPixels > 0 && wallPixels > 0);
        CHECK_EQ(wrong, 0u);
    }

    // ---- the composite: the wall next to the sky is brighter and the increase falls off away from the sun, nothing is darker, the far side of
    //      the frame is unchanged, and the sky keeps its own value byte for byte.
    {
        ComPtr<ID3D11Resource> resource;
        copy->GetResource(&resource);
        std::vector<uint16_t> out;
        std::vector<uint16_t> original;
        CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
        CHECK(frame.Readback(sceneTexture.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
        if (out.size() == original.size() && !out.empty())
        {
            double nearSum = 0.0, middleSum = 0.0, farSum = 0.0;
            uint32_t nearCount = 0, middleCount = 0, farCount = 0;
            uint32_t darker = 0, changedSky = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const size_t at = (size_t(y) * kWidth + x) * 4;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const float before = FromHalf(original[at + c]);
                        const float after = FromHalf(out[at + c]);
                        if (after < before)
                            ++darker;
                        if (x >= kWidth / 2 && out[at + c] != original[at + c])
                            ++changedSky; // the sky is never touched, not even by a rounding
                        if (x >= kWidth / 2)
                            continue;
                        const double increase = double(after) - double(before);
                        // The columns next to the sky line are the shaft's strongest on the wall; the far left is a frame-width away.
                        if (x >= kWidth / 2 - 4)
                        {
                            nearSum += increase;
                            ++nearCount;
                        }
                        else if (x >= kWidth / 4)
                        {
                            middleSum += increase;
                            ++middleCount;
                        }
                        else if (x < 4)
                        {
                            farSum += increase;
                            ++farCount;
                        }
                    }
                }
            CHECK(nearCount > 0 && middleCount > 0 && farCount > 0);
            const double nearAverage = nearSum / double(nearCount);
            const double middleAverage = middleSum / double(middleCount);
            const double farAverage = farSum / double(farCount);
            ComPtr<ID3D11Resource> termResource;
            pass.TermView()->GetResource(&termResource);
            std::vector<uint16_t> term;
            frame.Readback(termResource.Get(), HalfExtent(float(kWidth)), HalfExtent(float(kHeight)), DXGI_FORMAT_R16_FLOAT, term);
            const char nl = char(10);
            for (uint32_t x : {1u, 8u, 20u, 28u, 30u, 31u, 40u, 63u})
            {
                double sum = 0.0;
                for (uint32_t y = 0; y < kHeight / 2; ++y)
                    sum += double(FromHalf(term[size_t(y) * (kWidth / 2) + x]));
                std::printf("TERM x=%u mean=%d%c", x, int(sum * 10000.0 / double(kHeight / 2)), nl);
            }
            std::printf("AVG near=%d middle=%d far=%d%c", int(nearAverage * 10000.0), int(middleAverage * 10000.0), int(farAverage * 10000.0), nl);
            std::printf("DARK %u SKY %u%c", darker, changedSky, nl);
            CHECK(nearAverage > 0.005);                 // the wall beside the sky catches the light
            CHECK(nearAverage > middleAverage);         // and the shafts fall off away from the sun
            CHECK(middleAverage >= farAverage);
            CHECK(farAverage < 0.01);                   // the far side of the frame is left alone
            CHECK_EQ(darker, 0u);                       // nothing is ever darker than the scene
            CHECK_EQ(changedSky, 0u);                   // and the sky is never brighter than itself
        }
    }

    // ---- the density key: a longer reach carries the shafts further from the sun, which is what the key is for
    {
        Inputs wide = inputs;
        wide.params.density = 1.0f;
        ID3D11ShaderResourceView* wideCopy = pass.Run(frame.context.Get(), wide);
        CHECK(wideCopy != nullptr);
        if (wideCopy != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            wideCopy->GetResource(&resource);
            std::vector<uint16_t> out;
            std::vector<uint16_t> original;
            CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
            CHECK(frame.Readback(sceneTexture.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
            double farSum = 0.0;
            uint32_t count = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                {
                    const size_t at = (size_t(y) * kWidth + x) * 4;
                    farSum += double(FromHalf(out[at])) - double(FromHalf(original[at]));
                    ++count;
                }
            // The same far side the default reach leaves alone, now lit: the addition is a fraction of the frame's white, so the bar is small but
            // the key has to do something measurable there.
            CHECK(count > 0 && farSum / double(count) > 0.0015);
        }
    }

    // ---- the decay key: the blur's own fall-off, against the formula it states, at a texel whose ray crosses from the wall onto the sky
    //
    // Sample i is weighed decay^i, so the samples nearest the sun count less as the decay falls. A flat weight (no fall-off at all) still gives a
    // smooth term that brightens toward the sun, which is why the shape checks above cannot see it: this case reads the mask back, recomputes the
    // shader's own weighted mean for two decays, and requires the two runs to differ.
    {
        const uint32_t halfWidth = HalfExtent(float(kWidth));
        const uint32_t halfHeight = HalfExtent(float(kHeight));
        ComPtr<ID3D11Resource> maskResource;
        pass.MaskView()->GetResource(&maskResource);
        std::vector<unsigned char> mask;
        CHECK(frame.Readback(maskResource.Get(), halfWidth, halfHeight, DXGI_FORMAT_R8_UNORM, mask));
        CHECK_EQ(mask.size(), size_t(halfWidth) * halfHeight);

        // The shader's formula: the mask along the ray to the sun, weighed by decay^i, normalized by the weights. SV_Position is the pixel's
        // centre, so the ray starts half a texel into the blur's own grid.
        const auto reference = [&](float decay, int x, int y) {
            const float sunX = (inputs.sun.screenX - inputs.window.x) * 0.5f;
            const float sunY = (inputs.sun.screenY - inputs.window.y) * 0.5f;
            const float startX = float(x) + 0.5f;
            const float startY = float(y) + 0.5f;
            const float count = float(inputs.params.samples);
            const float stepX = (sunX - startX) * inputs.params.density / count;
            const float stepY = (sunY - startY) * inputs.params.density / count;
            const float jitter = Hash01(x, y);
            float weight = 1.0f, sum = 0.0f, total = 0.0f;
            for (int i = 0; i < int(count); ++i)
            {
                const int atX = int(startX + stepX * (float(i) + jitter));
                const int atY = int(startY + stepY * (float(i) + jitter));
                const bool inside = atX >= 0 && atY >= 0 && atX < int(halfWidth) && atY < int(halfHeight);
                const float value = inside ? float(mask[size_t(atY) * halfWidth + size_t(atX)]) / 255.0f : 0.0f;
                total += value * weight;
                sum += weight;
                weight *= decay;
            }
            return sum > 0.0f ? total / sum : 0.0f;
        };
        const auto termAt = [&](float decay, int x, int y) {
            Inputs tuned = inputs;
            tuned.params.decay = decay;
            if (pass.Run(frame.context.Get(), tuned) == nullptr)
                return -1.0f;
            ComPtr<ID3D11Resource> termResource;
            pass.TermView()->GetResource(&termResource);
            std::vector<uint16_t> term;
            if (!frame.Readback(termResource.Get(), halfWidth, halfHeight, DXGI_FORMAT_R16_FLOAT, term) ||
                term.size() != size_t(halfWidth) * halfHeight)
                return -1.0f;
            return FromHalf(term[size_t(y) * halfWidth + size_t(x)]);
        };

        const int atX = 28, atY = 24; // the ray from this texel leaves the wall and reaches the sky before the sun
        const float slow = termAt(0.80f, atX, atY);
        const float flat = termAt(1.00f, atX, atY);
        CHECK(slow >= 0.0f && flat >= 0.0f);
        CHECK(std::fabs(slow - reference(0.80f, atX, atY)) < 1.0e-3f); // the read-back is the formula's own number
        CHECK(std::fabs(flat - reference(1.00f, atX, atY)) < 1.0e-3f);
        // The march's per-pixel offset is part of that formula, at more than one texel: a fixed offset (one lattice for the whole frame) matches
        // this texel by luck and not the next ones.
        for (const auto& spot : { std::pair<int, int> { 28, 36 }, std::pair<int, int> { 40, 24 }, std::pair<int, int> { 13, 17 } })
            CHECK(std::fabs(termAt(0.96f, spot.first, spot.second) - reference(0.96f, spot.first, spot.second)) < 1.0e-3f);
        CHECK(flat - slow > 0.05f); // and the fall-off is what separates the two keys: with no fall-off they are the same term
        std::printf("DECAY slow=%d flat=%d reference slow=%d flat=%d\n", int(slow * 10000.0f), int(flat * 10000.0f),
                    int(reference(0.80f, atX, atY) * 10000.0f), int(reference(1.00f, atX, atY) * 10000.0f));
    }

    // ---- intensity 0: the pass still copies the scene, and the copy is byte for byte the engine's own pixels
    {
        Inputs quiet = inputs;
        quiet.params.intensity = 0.0f;
        ID3D11ShaderResourceView* flat = pass.Run(frame.context.Get(), quiet);
        CHECK(flat != nullptr);
        if (flat != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            flat->GetResource(&resource);
            std::vector<uint16_t> out;
            std::vector<uint16_t> original;
            CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
            CHECK(frame.Readback(sceneTexture.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
            CHECK(out.size() == original.size() && std::memcmp(out.data(), original.data(), out.size() * sizeof(uint16_t)) == 0);
        }
    }

    // ---- the top of the slider cannot burn the frame: on a full-mask column the addition is a bounded share of the frame's own white point, not the
    //      3.92 a session's keys added to a frame whose white was 0.838. Nothing the pass writes is past the white point, and a frame whose white is
    //      nearer (a bright exposure) gets a smaller addition - the bound follows the frame, not a constant in the pass.
    {
        Inputs hot = inputs;
        hot.params.intensity = kMaxIntensity;
        hot.tint.fromSky = false; // a coloured sun, as the check the diagnosis names uses
        hot.tint.rgb[0] = 1.0f;
        hot.tint.rgb[1] = 0.915586f;
        hot.tint.rgb[2] = 0.806474f;
        const auto runWall = [&](const Inputs& run, double& worst, uint32_t& pastWhite, uint32_t& darker, std::vector<uint16_t>& out,
                                 std::vector<uint16_t>& original) {
            worst = 0.0;
            pastWhite = 0;
            darker = 0;
            ID3D11ShaderResourceView* result = pass.Run(frame.context.Get(), run);
            CHECK(result != nullptr);
            if (result == nullptr)
                return;
            ComPtr<ID3D11Resource> resource;
            result->GetResource(&resource);
            CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
            CHECK(frame.Readback(sceneTexture.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
            if (out.size() != original.size() || out.empty())
                return;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth / 2; ++x)
                {
                    const size_t at = (size_t(y) * kWidth + x) * 4;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const float before = FromHalf(original[at + c]);
                        const float after = FromHalf(out[at + c]);
                        if (after < before)
                            ++darker;
                        if (before < run.whitePoint && after > run.whitePoint + 1.0e-3f)
                            ++pastWhite; // a pixel that was below the white was pushed past it: what must never happen
                        worst = std::max(worst, double(after) - double(before));
                    }
                }
        };
        double worst = 0.0;
        uint32_t pastWhite = 0, darker = 0;
        std::vector<uint16_t> out, original;
        runWall(hot, worst, pastWhite, darker, out, original);
        std::printf("BOUND worst=%d pastWhite=%u darker=%u%c", int(worst * 10000.0), pastWhite, darker, char(10));
        CHECK(worst > 0.02); // the shafts do add something on the wall
        // ... and never more than the rule's own share of the white: not the 3.92 a session's keys added to a white of 0.838.
        CHECK(worst <= double(PeakWhiteShare(kMaxIntensity)) + 1.0e-3);
        CHECK_EQ(pastWhite, 0u);                               // no channel of the frame is pushed past its white point
        CHECK_EQ(darker, 0u);
        CHECK(0.5 + worst < kVerifiedWhitePoint + 1.0e-3);     // the wall stays below white under a full-mask column
        // The bound is the frame's: a frame exposed so that white sits at 0.2 gets a smaller addition than the same frame at 0.838.
        Inputs bright = hot;
        bright.whitePoint = 0.2f;
        double brightWorst = 0.0;
        uint32_t brightPast = 0, brightDarker = 0;
        std::vector<uint16_t> brightOut, brightOriginal;
        runWall(bright, brightWorst, brightPast, brightDarker, brightOut, brightOriginal);
        CHECK(brightWorst < worst);
        CHECK_EQ(brightPast, 0u);
        // The wall is 0.5 and this frame's white is 0.2: the surface is already brighter than the frame reaches white, so the shafts add nothing
        // there at all. An addition that ignored the pixel's own room to the white would light it (and would be the 4.7x burn in miniature).
        CHECK(brightWorst == 0.0);

        // ---- the fade: a lost sun decays the light instead of switching it off. Zero is a byte-exact copy, half is less than the whole, and the
        //      half-resolution term still lights the frame (so the fade multiplies the light, not the pass).
        Inputs half = hot;
        half.fade = 0.5f;
        double halfWorst = 0.0;
        uint32_t halfPast = 0, halfDarker = 0;
        std::vector<uint16_t> halfOut, halfOriginal;
        runWall(half, halfWorst, halfPast, halfDarker, halfOut, halfOriginal);
        CHECK(halfWorst < worst * 0.85 && halfWorst > worst * 0.2);
        Inputs gone = hot;
        gone.fade = 0.0f;
        double goneWorst = 0.0;
        uint32_t gonePast = 0, goneDarker = 0;
        std::vector<uint16_t> goneOut, goneOriginal;
        runWall(gone, goneWorst, gonePast, goneDarker, goneOut, goneOriginal);
        CHECK(goneWorst == 0.0 && goneOut.size() == goneOriginal.size());
        if (goneOut.size() == goneOriginal.size() && !goneOut.empty())
            CHECK(std::memcmp(goneOut.data(), goneOriginal.data(), goneOut.size() * sizeof(uint16_t)) == 0);

        // ---- the term is read bilinearly. Two pixels whose half-resolution coordinates fall inside the *same* term texel must differ wherever the
        //      term changes between that texel and its neighbour: a nearest read paints one value over both, which is the 2x2 block a session saw as
        //      a mosaic. (Adjacent pixels that straddle a texel boundary differ under both reads, so they are not part of this check.)
        {
            const uint32_t halfWidth = HalfExtent(float(kWidth));
            const uint32_t halfHeight = HalfExtent(float(kHeight));
            ComPtr<ID3D11Resource> termResource;
            pass.TermView()->GetResource(&termResource);
            std::vector<uint16_t> term;
            CHECK(frame.Readback(termResource.Get(), halfWidth, halfHeight, DXGI_FORMAT_R16_FLOAT, term));
            CHECK_EQ(term.size(), size_t(halfWidth) * halfHeight);
            const auto termAt = [&term, halfWidth, halfHeight](int x, int y) {
                const int cx = x < 0 ? 0 : (x > int(halfWidth) - 1 ? int(halfWidth) - 1 : x);
                const int cy = y < 0 ? 0 : (y > int(halfHeight) - 1 ? int(halfHeight) - 1 : y);
                return FromHalf(term[size_t(cy) * halfWidth + size_t(cx)]);
            };
            uint32_t witnesses = 0, differ = 0;
            for (uint32_t y = 1; y + 1 < kHeight; ++y)
                for (uint32_t x = 1; x + 2 < kWidth / 2; ++x)
                {
                    // Position.xy is the pixel centre (x + 0.5), so the window's half-resolution coordinate is (x + 0.5) * 0.5 - 0.5. This pair
                    // starts at an odd x, which puts both pixels' floors on the same term texel.
                    if (((x + 1u) % 2u) != 0u || x >= (kWidth / 2) - 2)
                        continue;
                    const int texel = int((float(x) + 0.5f) * 0.5f - 0.5f);
                    if (std::fabs(termAt(texel, int(y)) - termAt(texel + 1, int(y))) < 0.05f)
                        continue; // the term does not change across this pair: nothing to see, either way
                    ++witnesses;
                    const size_t left = (size_t(y) * kWidth + x) * 4;
                    const size_t right = left + 4;
                    if (out[left] != out[right])
                        ++differ; // the red channel is the strongest of the fixture's shafts
                }
            std::printf("BILINEAR witnesses=%u differ=%u%c", witnesses, differ, char(10));
            CHECK(witnesses > 50);            // enough of the frame where the term changes inside one texel
            CHECK(differ > witnesses - witnesses / 10); // a nearest read gives zero here
        }
    }

    // ---- the owned textures: one scene copy and one half-resolution pair for the session's sizes; a smaller window re-creates only the half
    //      pair (a dynamic-resolution change), and a wider scene re-creates the scene copy.
    {
        CHECK_EQ(pass.ScenesCreated(), 1u); // the same frame ran the pass twice above
        CHECK_EQ(pass.HalfTargetsCreated(), 1u);
        CHECK_EQ(pass.SunColoursCreated(), 1u); // the one-texel colour target is created once for the session
        CHECK(pass.GpuTimingEnds() <= pass.GpuTimingBegins());

        Inputs smaller = inputs;
        smaller.window = Window { 0.0f, 0.0f, 96.0f, 72.0f };
        CHECK(pass.Run(frame.context.Get(), smaller) != nullptr);
        CHECK_EQ(pass.ScenesCreated(), 1u);
        CHECK_EQ(pass.HalfTargetsCreated(), 2u);

        ComPtr<ID3D11Texture2D> wide = frame.SceneTexture(160, kHeight);
        ComPtr<ID3D11ShaderResourceView> wideView = frame.View(wide.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        CHECK(wide != nullptr && wideView != nullptr);
        if (wide && wideView)
        {
            Inputs resized = MakeInputs(wideView.Get(), 10, depth.Get(), 41, 160, kHeight, Window { 0.0f, 0.0f, 160.0f, float(kHeight) });
            CHECK(pass.Run(frame.context.Get(), resized) != nullptr);
            CHECK_EQ(pass.ScenesCreated(), 2u);
        }
    }

    // ---- the alignment: the engine starts the render window at a half-pixel origin (a captured frame's viewport is 1920x1080+0.5+0.5), and
    //      SV_Position is already the target pixel. The old composite added the window's origin to every read, so the whole copy was the picture
    //      one pixel down and right of itself. With a value per texel, a scene that varies and no sky at all (so nothing is added), the copy has
    //      to be byte for byte the engine's own texels.
    {
        ComPtr<ID3D11Texture2D> varied = frame.GradientTexture(kWidth, kHeight);
        ComPtr<ID3D11ShaderResourceView> variedView = frame.View(varied.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        ComPtr<ID3D11ShaderResourceView> wall = frame.DepthAllWall(kWidth, kHeight);
        CHECK(varied != nullptr && variedView != nullptr && wall != nullptr);
        if (varied && variedView && wall)
        {
            const Window halfPixel { 0.5f, 0.5f, float(kWidth) - 1.0f, float(kHeight) - 1.0f };
            Inputs offset = MakeInputs(variedView.Get(), 10, wall.Get(), 41, kWidth, kHeight, halfPixel);
            offset.params.intensity = 1.0f;
            ID3D11ShaderResourceView* aligned = pass.Run(frame.context.Get(), offset);
            CHECK(aligned != nullptr);
            if (aligned != nullptr)
            {
                ComPtr<ID3D11Resource> resource;
                aligned->GetResource(&resource);
                std::vector<uint16_t> out;
                std::vector<uint16_t> original;
                CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
                CHECK(frame.Readback(varied.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
                CHECK(out.size() == original.size() && !out.empty());
                if (out.size() == original.size())
                    CHECK(std::memcmp(out.data(), original.data(), out.size() * sizeof(uint16_t)) == 0);
            }
            // And an integer origin reads the same texels: the origin is taken out of the half-resolution term, not out of the full-resolution read.
            Inputs integerWindow = MakeInputs(variedView.Get(), 10, wall.Get(), 41, kWidth, kHeight, Window { 0.0f, 0.0f, float(kWidth), float(kHeight) });
            integerWindow.params.intensity = 1.0f;
            ID3D11ShaderResourceView* plain = pass.Run(frame.context.Get(), integerWindow);
            CHECK(plain != nullptr);
            if (plain != nullptr)
            {
                ComPtr<ID3D11Resource> resource;
                plain->GetResource(&resource);
                std::vector<uint16_t> out;
                std::vector<uint16_t> original;
                CHECK(frame.Readback(resource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
                CHECK(frame.Readback(varied.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
                if (out.size() == original.size() && !out.empty())
                    CHECK(std::memcmp(out.data(), original.data(), out.size() * sizeof(uint16_t)) == 0);
            }
        }
    }

    // ---- the half-resolution term follows the window's own grid, not the target's: a scene pixel's term is the half texel that covers its window
    //      pixels 2k and 2k+1, with the window's origin taken out. With the window at an integer origin (2, 2) and the sky on the right half, the
    //      term changes from one texel to the next beside the sky line, so a composite that read `position.xy * 0.5` (the origin kept) would add
    //      the neighbouring texel's light: this case tells the two apart, which the 0.5-origin case above cannot (there the origin cancels out).
    {
        const Window offsetWindow { 2.0f, 2.0f, float(kWidth) - 4.0f, float(kHeight) - 4.0f };
        Inputs offset = MakeInputs(scene.Get(), 10, depth.Get(), 41, kWidth, kHeight, offsetWindow);
        offset.params.intensity = 1.0f;
        offset.tint.fromSky = true;
        ID3D11ShaderResourceView* offsetCopy = pass.Run(frame.context.Get(), offset);
        CHECK(offsetCopy != nullptr);
        if (offsetCopy != nullptr)
        {
            ComPtr<ID3D11Resource> copyResource;
            offsetCopy->GetResource(&copyResource);
            std::vector<uint16_t> out;
            std::vector<uint16_t> original;
            std::vector<uint16_t> term;
            ComPtr<ID3D11Resource> termResource;
            pass.TermView()->GetResource(&termResource);
            const uint32_t halfWidth = HalfExtent(offsetWindow.width);
            const uint32_t halfHeight = HalfExtent(offsetWindow.height);
            CHECK(frame.Readback(copyResource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
            CHECK(frame.Readback(sceneTexture.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
            CHECK(frame.Readback(termResource.Get(), halfWidth, halfHeight, DXGI_FORMAT_R16_FLOAT, term));
            CHECK(out.size() == original.size() && !out.empty());
            CHECK_EQ(term.size(), size_t(halfWidth) * halfHeight);
            if (out.size() == original.size() && term.size() == size_t(halfWidth) * halfHeight)
            {
                // The colour is the sky's own, normalised to its own peak (the sky here is a neutral kSky, so the tint is 1), the intensity is 1,
                // so a pixel's addition is term * tint.
                const float tint = 1.0f;
                const auto termAt = [&term, halfWidth, halfHeight](int x, int y) {
                    const int cx = x < 0 ? 0 : (x > int(halfWidth) - 1 ? int(halfWidth) - 1 : x);
                    const int cy = y < 0 ? 0 : (y > int(halfHeight) - 1 ? int(halfHeight) - 1 : y);
                    return FromHalf(term[size_t(cy) * halfWidth + size_t(cx)]);
                };
                uint32_t compared = 0, separating = 0, wrong = 0, darker = 0;
                for (uint32_t y = 3; y + 3 < kHeight; ++y)
                    for (uint32_t x = 3; x + 3 < kWidth; ++x)
                    {
                        if (x >= kWidth / 2)
                            continue; // the sky keeps its own value
                        const int hx = int(x - 2) / 2; // the window's own grid: pixel x is window pixel x - 2, and 2k and 2k+1 share texel k
                        const int hy = int(y - 2) / 2;
                        // The composite's own rule: the term (bilinear between the four texels around the pixel), the intensity, the sky's colour as
                        // the tint, the gain and the fade are the light in fractions of the frame's white, and the addition is bounded by the pixel's
                        // room to that white (ShaftsRules.h ShaftAdded). SV_Position is the pixel's centre, so the window-relative coordinate the
                        // composite reads is (x + 0.5 - window.x) * 0.5 - 0.5.
                        const auto bilinear = [&termAt](float px, float py) {
                            const float u = px * 0.5f - 0.5f;
                            const float v = py * 0.5f - 0.5f;
                            const int bx = int(std::floor(u));
                            const int by = int(std::floor(v));
                            const float fx = u - float(bx);
                            const float fy = v - float(by);
                            const float top = termAt(bx, by) + (termAt(bx + 1, by) - termAt(bx, by)) * fx;
                            const float bottom = termAt(bx, by + 1) + (termAt(bx + 1, by + 1) - termAt(bx, by + 1)) * fx;
                            return top + (bottom - top) * fy;
                        };
                        const float light = bilinear(float(x) + 0.5f - offsetWindow.x, float(y) + 0.5f - offsetWindow.y) * tint *
                                            offset.params.intensity * kShaftGain * offset.fade;
                        const float shifted = (termAt(hx + 1, hy + 1) - termAt(hx, hy)) * tint; // what `position.xy * 0.5` would add instead
                        const size_t at = (size_t(y) * kWidth + x) * 4;
                        for (uint32_t c = 0; c < 3; ++c)
                        {
                            const float before = FromHalf(original[at + c]);
                            const float after = FromHalf(out[at + c]);
                            if (after < before)
                                ++darker;
                            if (std::fabs(after - (before + ShaftAdded(before, light, offset.whitePoint))) > 3.0e-3f)
                                ++wrong;
                            if (std::fabs(shifted) > 0.02f)
                                ++separating;
                            ++compared;
                        }
                    }
                const char nl = char(10);
                std::printf("OFFSET compared=%u separating=%u wrong=%u darker=%u%c", compared, separating, wrong, darker, nl);
                CHECK(compared > 0);
                CHECK_EQ(darker, 0u);     // the composite only adds
                CHECK(separating > 100);  // enough of the frame where the two mappings disagree, or the case would prove nothing
                CHECK_EQ(wrong, 0u);      // and every one of those pixels is the window's own texel, not its neighbour
            }
        }
    }

    // ---- the sun's colour: only sky texels are the sun's colour. The sun's own pixel holds a dark leaf (a surface, so the composite would not
    //      paint it anyway), and the sky around it is bright: the one-texel pass must take the sky's colour, not the leaf's. With no sky anywhere
    //      near the sun there is no sun to take a colour from, so the pass adds nothing and the copy is byte for byte the scene.
    {
        const uint32_t sunX = kWidth - 1;
        const uint32_t sunY = kHeight / 2;
        ComPtr<ID3D11Texture2D> leafy = frame.LeafTexture(kWidth, kHeight, 0.25f, 0.0625f, sunX, sunY); // sky 0.25, a dark leaf at the sun
        ComPtr<ID3D11ShaderResourceView> leafyView = frame.View(leafy.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        ComPtr<ID3D11ShaderResourceView> wall = frame.DepthAllWall(kWidth, kHeight);
        CHECK(leafy != nullptr && leafyView != nullptr && wall != nullptr);
        if (leafy && leafyView && wall && depth != nullptr)
        {
            // The same frame's depth with the sun's own pixel a surface (a leaf) and the rest of the right half sky.
            std::vector<float> depthValues(size_t(kWidth) * kHeight, kWall);
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = kWidth / 2; x < kWidth; ++x)
                    depthValues[size_t(y) * kWidth + x] = 0.0f;
            depthValues[size_t(sunY) * kWidth + sunX] = kWall;
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = kWidth;
            desc.Height = kHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial { depthValues.data(), kWidth * sizeof(float), 0 };
            ComPtr<ID3D11Texture2D> leafDepthTexture;
            CHECK(SUCCEEDED(frame.device->CreateTexture2D(&desc, &initial, &leafDepthTexture)) && leafDepthTexture != nullptr);
            ComPtr<ID3D11ShaderResourceView> leafDepth = frame.View(leafDepthTexture.Get(), DXGI_FORMAT_R32_FLOAT);
            CHECK(leafDepth != nullptr);
            if (leafDepth != nullptr)
            {
                Inputs colour = MakeInputs(leafyView.Get(), 10, leafDepth.Get(), 41, kWidth, kHeight,
                                           Window { 0.0f, 0.0f, float(kWidth), float(kHeight) });
                colour.params.intensity = 1.0f;
                colour.tint.fromSky = true; // the live build's directional light reports no colour: the sky's is taken
                CHECK(pass.Run(frame.context.Get(), colour) != nullptr);
                ComPtr<ID3D11Resource> resource;
                pass.SunColourView()->GetResource(&resource);
                std::vector<uint16_t> texel;
                CHECK(frame.Readback(resource.Get(), 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, texel));
                CHECK_EQ(texel.size(), 4u);
                if (texel.size() == 4)
                {
                    const float red = FromHalf(texel[0]);
                    const float alpha = FromHalf(texel[3]);
                    CHECK(alpha > 0.5f); // there was sky to sample
                    // The sampled sky is a neutral 0.25, and the tint is normalised to its own peak: the shaft's brightness is then the key's and
                    // the mask's alone, where the sample's own level used to scale the whole effect. The leaf's 0.0625 could not produce a peak of
                    // 1 either, so a pass that read the sun's own pixel is still caught.
                    CHECK(std::fabs(red - 1.0f) < 1.0e-3f);
                }

                // No sky at all: the whole frame is a surface, so the search finds nothing and the pass must add nothing. The copy is the scene.
                Inputs noSky = MakeInputs(leafyView.Get(), 10, wall.Get(), 41, kWidth, kHeight, Window { 0.0f, 0.0f, float(kWidth), float(kHeight) });
                noSky.params.intensity = 1.0f;
                noSky.tint.fromSky = true;
                ID3D11ShaderResourceView* flat = pass.Run(frame.context.Get(), noSky);
                CHECK(flat != nullptr);
                if (flat != nullptr)
                {
                    ComPtr<ID3D11Resource> flatResource;
                    flat->GetResource(&flatResource);
                    std::vector<uint16_t> out;
                    std::vector<uint16_t> original;
                    CHECK(frame.Readback(flatResource.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, out));
                    CHECK(frame.Readback(leafy.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, original));
                    if (out.size() == original.size() && !out.empty())
                        CHECK(std::memcmp(out.data(), original.data(), out.size() * sizeof(uint16_t)) == 0);
                    pass.SunColourView()->GetResource(&flatResource);
                    std::vector<uint16_t> texel2;
                    CHECK(frame.Readback(flatResource.Get(), 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, texel2));
                    if (texel2.size() == 4)
                        CHECK(FromHalf(texel2[3]) < 0.5f); // no sky around the sun: the shafts add nothing
                }

                // The hue survives the normalisation and the level does not scale the effect. A sun that is a quarter of a window above the frame
                // clamps its search to the top border row, and that row of a daylight sky reads about (0.18, 0.28, 0.38): at the sample's own level
                // the shafts were dimmed by 0.38, and this is the frame the session's screenshot was. The tint must come back as that hue at peak 1
                // (0.474, 0.737, 1.0), not as a darker copy of it.
                {
                    const float blueSky[3] { 0.18f, 0.28f, 0.38f };
                    const float leaf[3] { 0.9f, 0.8f, 0.6f }; // a sunlit leaf where the sun's own pixel is, so the search has to look past it
                    ComPtr<ID3D11Texture2D> colourTexture = frame.SkyColourTexture(kWidth, kHeight, blueSky, leaf, sunX, sunY);
                    ComPtr<ID3D11ShaderResourceView> colourView =
                        colourTexture != nullptr ? frame.View(colourTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT) : ComPtr<ID3D11ShaderResourceView> {};
                    CHECK(colourView != nullptr);
                    if (colourView != nullptr)
                    {
                        Inputs skyTint = MakeInputs(colourView.Get(), 10, leafDepth.Get(), 41, kWidth, kHeight,
                                                    Window { 0.0f, 0.0f, float(kWidth), float(kHeight) });
                        skyTint.params.intensity = 1.0f;
                        skyTint.tint.fromSky = true;
                        CHECK(pass.Run(frame.context.Get(), skyTint) != nullptr);
                        ComPtr<ID3D11Resource> resource;
                        pass.SunColourView()->GetResource(&resource);
                        std::vector<uint16_t> tinted;
                        CHECK(frame.Readback(resource.Get(), 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, tinted));
                        if (tinted.size() == 4)
                        {
                            const float red = FromHalf(tinted[0]), green = FromHalf(tinted[1]), blue = FromHalf(tinted[2]);
                            CHECK(std::fabs(red - 0.474f) < 2.0e-2f && std::fabs(green - 0.737f) < 2.0e-2f &&
                                  std::fabs(blue - 1.0f) < 1.0e-2f);
                            CHECK(red < green && green < blue); // the sky's own order, not the leaf's
                        }
                    }
                }
            }
        }
    }

    // ---- the live depth format: R24_UNORM_X8_TYPELESS (46) through the same Load path. A cleared reversed-Z depth is sky everywhere, so the mask
    //      is 1 on every texel and the pass still runs (a build that only accepted the buffer's typeless format would refuse this depth).
    {
        ComPtr<ID3D11ShaderResourceView> depth24 = frame.Depth24Sky(kWidth, kHeight);
        CHECK(depth24 != nullptr);
        if (depth24 != nullptr)
        {
            Inputs live = MakeInputs(scene.Get(), 10, depth24.Get(), 46, kWidth, kHeight, Window { 0.0f, 0.0f, float(kWidth), float(kHeight) });
            live.params.intensity = 1.0f;
            ID3D11ShaderResourceView* copy24 = pass.Run(frame.context.Get(), live);
            CHECK(copy24 != nullptr);
            CHECK(pass.Failure() == nullptr);
            if (copy24 != nullptr)
            {
                ComPtr<ID3D11Resource> resource;
                pass.MaskView()->GetResource(&resource);
                std::vector<unsigned char> mask;
                CHECK(frame.Readback(resource.Get(), HalfExtent(float(kWidth)), HalfExtent(float(kHeight)), DXGI_FORMAT_R8_UNORM, mask));
                uint32_t wallPixels = 0;
                for (unsigned char value : mask)
                    if (value < 200)
                        ++wallPixels;
                CHECK_EQ(wallPixels, 0u);
            }
        }
    }

    // ---- what the pass refuses: every one hands back null with a reason, and the game's own draw would run unchanged
    {
        Pass refusing;
        Inputs bad = inputs;
        bad.sceneFormat = 87; // B8G8R8A8_UNORM: another pass's binding
        CHECK(refusing.Run(frame.context.Get(), bad) == nullptr);
        CHECK(refusing.Failure() != nullptr);

        bad = inputs;
        bad.depthFormat = 10; // the scene's own format is not a depth
        CHECK(refusing.Run(frame.context.Get(), bad) == nullptr);
        CHECK(refusing.Failure() != nullptr);

        bad = inputs;
        bad.window = Window { 0.0f, 0.0f, 0.0f, 0.0f };
        CHECK(refusing.Run(frame.context.Get(), bad) == nullptr);

        bad = inputs;
        bad.depth = nullptr;
        CHECK(refusing.Run(frame.context.Get(), bad) == nullptr);

        // a scene whose own texture is not the size the draw measured: a stale binding from another frame
        bad = inputs;
        bad.sceneWidth = 1920;
        CHECK(refusing.Run(frame.context.Get(), bad) == nullptr);

        // nothing was drawn for any of them: no scene copy was ever created
        CHECK_EQ(refusing.ScenesCreated(), 0u);
        CHECK_EQ(refusing.HalfTargetsCreated(), 0u);
    }
    return Finish("shafts_warp");
}
