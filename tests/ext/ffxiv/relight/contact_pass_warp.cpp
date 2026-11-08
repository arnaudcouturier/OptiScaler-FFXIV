// Contact shadows: the pass (ext/ffxiv/relight/ContactShadowsPass.cpp and its shaders) on the D3D11 WARP software adapter, on synthetic
// buffers this test builds itself.
//
// The camera buffer is the engine's real layout (CameraRegisters.h: the view matrix's three rows at register 0, the view-projection at 6 and
// the inverse projection at 14), built by camera_rows.h, and the CPU reference at the bottom of this file computes the march and the
// combine from the same numbers: what the GPU writes must match it within one level of the mask's 8 bits. That is what pins the shader to
// the algorithm and to the layout - a matrix read at the wrong register changes every position and the reference no longer agrees.
//
// The scene is a flat surface two metres in front of the camera, optionally with a plate one or two centimetres in front of it (a contact
// occluder), and the sun 26.5 degrees above the horizon. What is checked is what a wrong march or a wrong mapping would break:
//   a ground point near the plate is darkened at more than one pixel, and a surface facing away from the sun never is,
//   the term lands on the side away from the sun, and the sign key turns the ray around (the other side, or nothing),
//   open ground, a NaN depth and a depth column of zero stay fully lit, and a flat plane with quantised depth does not shadow itself,
//   a plate further away than the march length darkens nothing, and full strength reaches zero,
//   a smaller render window at a half-pixel origin is passed through outside and shadowed inside,
//   a depth edge does not let a term from the other side bleed across it,
//   the combine keeps the mask's alpha and multiplies the colour rather than replacing it,
//   a dynamic-resolution change (another window in the same allocation) creates no D3D object,
//   a second allocation (a window resize) creates the term target again (its copy is not the scene's mask, so it is not judged per texel),
//   the world-space direction is turned by the view matrix,
//   the march covers the render window's half and no more (read back from the march's own target),
//   a neighbour across a depth edge is dropped instead of blended into a pixel,
//   a march shorter than the start distance finds nothing, although the same march without the rule finds the surface at the
//   receiver's own edge,
//   a pixel's normal ignores a neighbour that is not on its surface (a cleared texel beside it),
//   the pass refuses a second entry while a run holds it (its own draws re-enter the hook), and its GPU timing pair is begun by every run that
//   can time itself and ended only by the run that began it.
#include "check.h"

#include "camera_rows.h"

#include "ext/ffxiv/relight/ContactShadowsPass.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::relight;

namespace
{
using contact_test::CameraRows;
using contact_test::DepthOf;
using contact_test::Mat4;

// The smallWindow vector types the reference reads like the shader's own.
struct float2
{
    float x = 0.0f, y = 0.0f;
};
struct float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct float4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 288;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;
constexpr float kFovDegrees = 40.0f;
// The scene is built the way the camera sees it, not the other way round: the camera is at the origin looking down -z, the floor is the
// plane y = kFloorY, and the objects stand on it. The sun is 25 degrees above the horizon, 15 degrees from the left toward the camera, so
// the shadow of an object falls to its right and toward the viewer - the side the test looks for the term on.
constexpr float kFloorY = -1.6f;
constexpr float kBoxMinX = -1.20f, kBoxMaxX = -0.20f; // 1 m wide
constexpr float kBoxTopY = -0.10f;                     // 1.5 m tall, standing on the floor
constexpr float kBoxNearZ = -5.80f, kBoxFarZ = -6.40f;
constexpr float kSphereX = 1.50f, kSphereY = -1.10f, kSphereZ = -6.50f, kSphereRadius = 0.50f;
// A rail one texel thick at six metres, just above the floor and across the view: what a march with steps wider than a texel draws as dots.
constexpr float kTwigMinX = -1.6f, kTwigMaxX = 1.6f, kTwigLowY = kFloorY + 0.15f, kTwigHighY = kFloorY + 0.165f, kTwigNearZ = -6.0f, kTwigFarZ = -6.015f;
constexpr float kWallMinX = -7.0f, kWallMaxX = 7.0f; // a wall across the end of the floor, 8 m away
constexpr float kWallTopY = 2.5f;
constexpr float kWallNearZ = -8.0f, kWallFarZ = -8.6f;
constexpr float kSunLeftUp[3] { -0.875f, 0.423f, 0.234f };     // toward the sun: left, 25 degrees up, 15 degrees toward the camera
constexpr float kSunBehindWall[3] { 0.15f, 0.423f, -0.894f };  // 25 degrees up, beyond the wall: its base shadows the floor
constexpr float kSunBehind[3] { 0.5f, 0.0f, -0.8660f };        // the sun behind the surface: never a contact shadow

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                             &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }
};

// What a pixel has in front of it: the floor, the box, the sphere, or nothing (the sky).
enum class Surface : uint8_t
{
    Sky = 0,
    Floor = 1,
    Box = 2,
    Sphere = 3,
    Wall = 4,
};

// The scene: the floor alone, or the floor with the box and the sphere on it. Every field the cases break on purpose is here too.
struct Scene
{
    bool box = false;
    bool sphere = false;
    bool wall = false;
    bool twig = false;
    int nanAt = -1;
    int zeroColumn = -1;
    bool quantise = false;
};

// One rendered frame: the depth buffer the pass reads, and what the test needs to judge the term against the scene it came from.
struct Rendered
{
    std::vector<float> depths;
    std::vector<Surface> surface;
    std::vector<float> worldX, worldY, worldZ; // the hit point of the ray through each pixel (zeros for the sky)
};

// A ray through the centre of a pixel, normalized, in view space, for an allocation of any size.
float3 RayOf(const Mat4& projection, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const float ndcX = (float(x) + 0.5f) / float(width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (float(y) + 0.5f) / float(height) * 2.0f;
    const float3 direction { ndcX / projection.m[0][0], ndcY / projection.m[1][1], -1.0f };
    const float lengthSquared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
    const float scale = 1.0f / std::sqrt(lengthSquared);
    return float3 { direction.x * scale, direction.y * scale, direction.z * scale };
}

// The nearest of the floor, the box and the sphere along a ray, and where it is. A slab test for the box, the school formula for the sphere.
void Intersect(const float3& origin, const float3& direction, const Scene& scene, float& nearest, Surface& surface)
{
    nearest = std::numeric_limits<float>::infinity();
    surface = Surface::Sky;
    if (direction.y < -1e-6f)
    {
        const float t = (kFloorY - origin.y) / direction.y;
        if (t > 0.0f && t < nearest)
        {
            nearest = t;
            surface = Surface::Floor;
        }
    }
    const auto slab = [&](float low, float high, float o, float d, float& enter, float& exit) {
        if (std::fabs(d) < 1e-9f)
            return o >= low && o <= high;
        float first = (low - o) / d, second = (high - o) / d;
        if (first > second)
            std::swap(first, second);
        enter = std::max(enter, first);
        exit = std::min(exit, second);
        return enter <= exit;
    };
    if (scene.box)
    {
        float enter = 0.0f, exit = nearest;
        if (slab(kBoxMinX, kBoxMaxX, origin.x, direction.x, enter, exit) && slab(kFloorY, kBoxTopY, origin.y, direction.y, enter, exit) &&
            slab(kBoxFarZ, kBoxNearZ, origin.z, direction.z, enter, exit) && enter > 0.0f && enter < nearest)
        {
            nearest = enter;
            surface = Surface::Box;
        }
    }
    if (scene.twig)
    {
        float enter = 0.0f, exit = nearest;
        if (slab(kTwigMinX, kTwigMaxX, origin.x, direction.x, enter, exit) && slab(kTwigLowY, kTwigHighY, origin.y, direction.y, enter, exit) &&
            slab(kTwigFarZ, kTwigNearZ, origin.z, direction.z, enter, exit) && enter > 0.0f && enter < nearest)
        {
            nearest = enter;
            surface = Surface::Box;
        }
    }
    if (scene.wall)
    {
        float enter = 0.0f, exit = nearest;
        if (slab(kWallMinX, kWallMaxX, origin.x, direction.x, enter, exit) && slab(kFloorY, kWallTopY, origin.y, direction.y, enter, exit) &&
            slab(kWallFarZ, kWallNearZ, origin.z, direction.z, enter, exit) && enter > 0.0f && enter < nearest)
        {
            nearest = enter;
            surface = Surface::Wall;
        }
    }
    if (scene.sphere)
    {
        const float3 centre { kSphereX, kSphereY, kSphereZ };
        const float3 offset { origin.x - centre.x, origin.y - centre.y, origin.z - centre.z };
        const float b = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
        const float c = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - kSphereRadius * kSphereRadius;
        const float discriminant = b * b - c;
        if (discriminant > 0.0f)
        {
            const float t = -b - std::sqrt(discriminant);
            if (t > 0.0f && t < nearest)
            {
                nearest = t;
                surface = Surface::Sphere;
            }
        }
    }
}

// Renders the scene into the buffers the pass and the checks read.
Rendered Render(const Mat4& projection, const Scene& scene)
{
    Rendered out;
    out.depths.assign(size_t(kWidth) * kHeight, 0.0f);
    out.surface.assign(size_t(kWidth) * kHeight, Surface::Sky);
    out.worldX.assign(size_t(kWidth) * kHeight, 0.0f);
    out.worldY.assign(size_t(kWidth) * kHeight, 0.0f);
    out.worldZ.assign(size_t(kWidth) * kHeight, 0.0f);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const size_t index = size_t(y) * kWidth + x;
            const float3 direction = RayOf(projection, x, y, kWidth, kHeight);
            float nearest = 0.0f;
            Surface surface = Surface::Sky;
            Intersect(float3 { 0.0f, 0.0f, 0.0f }, direction, scene, nearest, surface);
            if (surface == Surface::Sky || !(nearest > 0.0f))
                continue;
            float3 hit { direction.x * nearest, direction.y * nearest, direction.z * nearest };
            const float metres = -hit.z;
            float depth = DepthOf(projection, metres);
            if (scene.quantise)
            {
                // The 24 bits a fixed-point depth buffer keeps: the noise a perfect float plane does not have.
                const float steps = std::ldexp(1.0f, 24);
                depth = std::nearbyint(depth * steps) / steps;
            }
            if (int(index) == scene.nanAt)
                depth = std::numeric_limits<float>::quiet_NaN();
            if (int(x) == scene.zeroColumn)
                depth = 0.0f; // the far plane of a cleared buffer
            out.depths[index] = depth;
            out.surface[index] = surface;
            out.worldX[index] = hit.x;
            out.worldY[index] = hit.y;
            out.worldZ[index] = hit.z;
        }
    return out;
}

// A depth buffer of a scene at an allocation of any size, without the per-pixel surface and world arrays: the ray is cast once per
// `step` x `step` block and its depth written to the whole block, so a 3840x2160 buffer costs a quarter of the rays of a 1920x1080 one. The pass
// and the CPU reference read this one buffer, so a blocky buffer changes nothing about what is compared; the smaller cases use Render(), which
// keeps the surface and the hit point for the judgements against the scene.
std::vector<float> RenderDepth(const Mat4& projection, const Scene& scene, uint32_t width, uint32_t height, uint32_t step)
{
    std::vector<float> depths(size_t(width) * height, 0.0f);
    if (step == 0)
        step = 1;
    for (uint32_t y = 0; y < height; y += step)
        for (uint32_t x = 0; x < width; x += step)
        {
            const float3 direction = RayOf(projection, x, y, width, height);
            float nearest = 0.0f;
            Surface surface = Surface::Sky;
            Intersect(float3 { 0.0f, 0.0f, 0.0f }, direction, scene, nearest, surface);
            if (surface == Surface::Sky || !(nearest > 0.0f))
                continue;
            const float3 hit { direction.x * nearest, direction.y * nearest, direction.z * nearest };
            const float depth = DepthOf(projection, -hit.z);
            const uint32_t lastX = std::min(x + step, width), lastY = std::min(y + step, height);
            for (uint32_t by = y; by < lastY; ++by)
                for (uint32_t bx = x; bx < lastX; ++bx)
                    depths[size_t(by) * width + bx] = depth;
        }
    return depths;
}

struct Textures
{
    ComPtr<ID3D11Texture2D> depth, mask;
    ComPtr<ID3D11ShaderResourceView> depthView, maskView;
};

ComPtr<ID3D11Texture2D> MakeTexture2D(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data,
                                      uint32_t rowPitch)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial { data, rowPitch, 0 };
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, data != nullptr ? &initial : nullptr, &texture)))
        return {};
    return texture;
}

ComPtr<ID3D11ShaderResourceView> MakeView(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture, &srv, &view)))
        return {};
    return view;
}

// The mask the pass copies is the game's own screen-space sun mask. This game build's is R8_UNORM (one visibility factor per texel) on an
// R8_TYPELESS texture; the benchmark build the feature was designed on had B8G8R8A8_UNORM with the same role, so both formats are exercised.
// `maskPixel` stays the B8G8R8A8 word; its low byte is the R8 level, so a case reads the same value in either format. `textureFormat` is the
// format the texture itself is created with, which differs from the view's when the engine's typeless texture is sampled through a typed view.
Textures MakeInputs(ID3D11Device* device, const std::vector<float>& depths, uint32_t maskPixel, DXGI_FORMAT maskFormat,
                     uint32_t width = kWidth, uint32_t height = kHeight, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN)
{
    Textures textures;
    const bool bgra = maskFormat == DXGI_FORMAT_B8G8R8A8_UNORM || maskFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    std::vector<uint32_t> pixels(size_t(width) * height, maskPixel);
    std::vector<unsigned char> levels(size_t(width) * height, static_cast<unsigned char>(maskPixel & 0xFFu));
    textures.mask = MakeTexture2D(device, width, height, maskFormat, bgra ? static_cast<const void*>(pixels.data())
                                                                         : static_cast<const void*>(levels.data()),
                                  bgra ? width * 4 : width);
    if (viewFormat == DXGI_FORMAT_UNKNOWN)
        viewFormat = maskFormat;
    textures.depth = MakeTexture2D(device, width, height, DXGI_FORMAT_R32_FLOAT, depths.data(), width * 4);
    if (textures.mask)
        textures.maskView = MakeView(device, textures.mask.Get(), viewFormat);
    if (textures.depth)
        textures.depthView = MakeView(device, textures.depth.Get(), DXGI_FORMAT_R32_FLOAT);
    return textures;
}

struct Buffers
{
    ComPtr<ID3D11Buffer> light, camera;
};

Buffers MakeBuffers(ID3D11Device* device, const float sun[3], const CameraRows& camera)
{
    Buffers out;
    float lightBytes[8] { 0.0f, 0.0f, 0.0f, 0.0f, sun[0], sun[1], sun[2], 0.0f };
    D3D11_BUFFER_DESC desc {};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.ByteWidth = sizeof lightBytes;
    D3D11_SUBRESOURCE_DATA lightData { lightBytes, 0, 0 };
    device->CreateBuffer(&desc, &lightData, &out.light);
    desc.ByteWidth = sizeof camera.v;
    D3D11_SUBRESOURCE_DATA cameraData { camera.v, 0, 0 };
    device->CreateBuffer(&desc, &cameraData, &out.camera);
    return out;
}

struct Pixels
{
    std::vector<uint8_t> red, alpha;
    // The copy's own size: a case that runs the pass on a smaller allocation than the scene's mask gets a smaller copy back, and the
    // per-texel judgement (which is written against the scene's kWidth x kHeight mask) does not apply to it.
    uint32_t width = 0, height = 0;
};

Pixels ReadCopy(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* view)
{
    Pixels pixels;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);
    if (!texture)
        return pixels;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || !copy)
        return pixels;
    context->CopyResource(copy.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
        return pixels;
    const auto* bytes = static_cast<const unsigned char*>(map.pData);
    pixels.width = desc.Width;
    pixels.height = desc.Height;
    pixels.red.resize(size_t(desc.Width) * desc.Height);
    pixels.alpha.resize(size_t(desc.Width) * desc.Height);
    // The red byte of the copy's own format: the second byte of a B8G8R8A8 word, the only byte of an R8_UNORM texel.
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const uint32_t stride = bgra ? 4u : 1u;
    const uint32_t redOffset = bgra ? 2u : 0u;
    for (uint32_t y = 0; y < desc.Height; ++y)
        for (uint32_t x = 0; x < desc.Width; ++x)
        {
            pixels.red[size_t(y) * desc.Width + x] = bytes[size_t(y) * map.RowPitch + size_t(x) * stride + redOffset];
            pixels.alpha[size_t(y) * desc.Width + x] =
                bgra ? bytes[size_t(y) * map.RowPitch + size_t(x) * 4 + 3] : 255; // an R8 target has no alpha channel
        }
    context->Unmap(copy.Get(), 0);
    return pixels;
}

// The march's own output (an R8_UNORM target), read back: what the pass wrote, and where it stopped writing.
struct TermPixels
{
    std::vector<uint8_t> values;
    uint32_t width = 0, height = 0;

    int at(uint32_t x, uint32_t y) const { return int(values[size_t(y) * width + x]); }
    int darkened() const
    {
        int count = 0;
        for (const uint8_t value : values)
            count += value < 250 ? 1 : 0;
        return count;
    }
};

TermPixels ReadTerm(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* view)
{
    TermPixels out;
    if (view == nullptr)
        return out;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);
    if (!texture)
        return out;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || !copy)
        return out;
    context->CopyResource(copy.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
        return out;
    const auto* bytes = static_cast<const unsigned char*>(map.pData);
    out.width = desc.Width;
    out.height = desc.Height;
    out.values.resize(size_t(desc.Width) * desc.Height);
    for (uint32_t y = 0; y < desc.Height; ++y)
        for (uint32_t x = 0; x < desc.Width; ++x)
            out.values[size_t(y) * desc.Width + x] = bytes[size_t(y) * map.RowPitch + x];
    context->Unmap(copy.Get(), 0);
    return out;
}

struct Stats
{
    int min = 255;
    int below250 = 0;
    int minLeft = 255, minRight = 255;
};

Stats Measure(const std::vector<uint8_t>& values)
{
    Stats stats;
    for (uint32_t y = 8; y < kHeight - 8; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const int value = values[size_t(y) * kWidth + x];
            if (value < 250)
                ++stats.below250;
            stats.min = value < stats.min ? value : stats.min;
            if (x + 16 < kWidth / 2)
                stats.minLeft = value < stats.minLeft ? value : stats.minLeft;
            if (x > kWidth / 2 + 16)
                stats.minRight = value < stats.minRight ? value : stats.minRight;
        }
    return stats;
}

int valueAt(const std::vector<uint8_t>& values, int x, int y) { return int(values[size_t(y) * kWidth + size_t(x)]); }

// ---- the CPU reference of the two shaders -------------------------------------------------------------------------------------
//
// The same arithmetic as shaders/ContactShadows.hlsl, in the same order, with the camera buffer of camera_rows.h: the per-pixel shader runs
// in C++ so the GPU's answer can be compared with it. Derivatives come from the half-resolution quad (the four texels a pixel shader's 2x2
// block sees), which is what ddx/ddy are on the GPU.
struct Reference
{
    const std::vector<float>* depths = nullptr;
    Mat4 projection;
    Mat4 inverseProjection;
    CameraRows camera;
    float sun[3] {};
    SunSpace space = SunSpace::View;
    SunSign sign = SunSign::Toward;
    Params params;
    Window window;
    // The two switches that drop one of the march's own rules, so a case can compute what the shader would answer without it. A fixture
    // asserts that the answers differ, which is what proves the fixture exercises that rule.
    bool ignoreStart = false;      // the start distance: a hit closer than this to the receiver is the receiver's own surface
    bool ignoreNormalDepth = false; // the normal's depth tolerance: a neighbour across a silhouette does not belong to the pixel

    float ndcX(uint32_t windowX, float origin) const { return ((float(windowX) + 0.5f) - origin) / window.width * 2.0f - 1.0f; }
    float ndcY(uint32_t windowY, float origin) const { return 1.0f - ((float(windowY) + 0.5f) - origin) / window.height * 2.0f; }

    // The depth allocation the window and the mask live in. The small cases use the mask's own size; the game's shape is a 1920x1080 window
    // inside a 3840x2160 allocation, and the shader reads the depth anywhere inside that allocation, not only inside the window.
    uint32_t depthWidth = kWidth, depthHeight = kHeight;

    float3 Position(int x, int y) const;   // view-space position of a mask pixel, (0,0,0) where the shader's reconstruction fails
    float4 Multiply(const Mat4& m, float4 v) const;
    bool Inside(int x, int y) const { return x >= 0 && y >= 0 && x < int(depthWidth) && y < int(depthHeight); }

    // The pixel a term texel stands for (shaders: int2(position.xy), a viewport at the window's whole-texel origin).
    void At(uint32_t texelX, uint32_t texelY, int& x, int& y) const
    {
        x = int(window.x) + int(texelX);
        y = int(window.y) + int(texelY);
    }

    float Term(uint32_t texelX, uint32_t texelY) const;
    float TermQuantised(uint32_t texelX, uint32_t texelY) const;
    float Combine(uint32_t x, uint32_t y, uint32_t maskValue) const;
};

float3 Normalized(const float3& v)
{
    const float lengthSquared = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!(lengthSquared > 1e-12f))
        return float3 { 0.0f, 0.0f, 0.0f };
    const float scale = 1.0f / std::sqrt(lengthSquared);
    return float3 { v.x * scale, v.y * scale, v.z * scale };
}

// The GPU's smoothstep, including its clamp of the fraction.
float Smoothstep(float edge0, float edge1, float value)
{
    if (!(edge1 > edge0))
        return value < edge0 ? 0.0f : 1.0f;
    const float x = std::fmin(std::fmax((value - edge0) / (edge1 - edge0), 0.0f), 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float3 Cross(const float3& a, const float3& b)
{
    return float3 { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float4 Reference::Multiply(const Mat4& m, float4 v) const
{
    float4 out {};
    out.x = m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3] * v.w;
    out.y = m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3] * v.w;
    out.z = m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3] * v.w;
    out.w = m.m[3][0] * v.x + m.m[3][1] * v.y + m.m[3][2] * v.z + m.m[3][3] * v.w;
    return out;
}

float3 Reference::Position(int x, int y) const
{
    const float depth = (*depths)[size_t(y) * depthWidth + size_t(x)];
    const float4 viewPoint = Multiply(inverseProjection, float4 { ndcX(uint32_t(x), window.x), ndcY(uint32_t(y), window.y), depth, 1.0f });
    if (viewPoint.w == 0.0f)
        return float3 { 0.0f, 0.0f, 0.0f };
    return float3 { viewPoint.x / viewPoint.w, viewPoint.y / viewPoint.w, viewPoint.z / viewPoint.w };
}

float Reference::Term(uint32_t texelX, uint32_t texelY) const
{
    int ax = 0, ay = 0;
    At(texelX, texelY, ax, ay);
    if (!Inside(ax, ay))
        return 1.0f;
    const float depth = (*depths)[size_t(ay) * depthWidth + size_t(ax)];
    if (!(depth > 0.0f))
        return 1.0f;
    const float3 P = Position(ax, ay);
    if (std::isnan(P.x) || std::isnan(P.y) || std::isnan(P.z))
        return 1.0f;

    // The geometric normal from the pixel's own depth neighbourhood, exactly as NormalOf() in the shader: a neighbour across a
    // silhouette is replaced by the pixel itself, so the normal belongs to the pixel's own surface.
    const float normalTolerance = 0.02f * depth;
    float3 neighbour[4] {};
    const int dx[4] { -1, 1, 0, 0 };
    const int dy[4] { 0, 0, -1, 1 };
    for (int i = 0; i < 4; ++i)
    {
        const int nx = ax + dx[i], ny = ay + dy[i];
        neighbour[i] = P;
        if (!Inside(nx, ny))
            continue;
        const float nd = (*depths)[size_t(ny) * depthWidth + size_t(nx)];
        if (ignoreNormalDepth || (nd > 0.0f && std::fabs(nd - depth) <= normalTolerance))
            neighbour[i] = Position(nx, ny);
    }
    // One side per axis, the one whose depth is closest (the shader's NormalOf): the neighbour's own depth decides, also when the neighbour was
    // replaced by the pixel, and also outside the picture, where the shader's clamped tap reads the pixel's own depth.
    float sideDepth[4] {};
    for (int i = 0; i < 4; ++i)
    {
        const int nx = ax + dx[i], ny = ay + dy[i];
        sideDepth[i] = Inside(nx, ny) ? (*depths)[size_t(ny) * depthWidth + size_t(nx)] : depth;
    }
    const bool useRight = std::fabs(sideDepth[1] - depth) <= std::fabs(sideDepth[0] - depth);
    const bool useDown = std::fabs(sideDepth[3] - depth) <= std::fabs(sideDepth[2] - depth);
    const float3 acrossX = useRight ? float3 { neighbour[1].x - P.x, neighbour[1].y - P.y, neighbour[1].z - P.z }
                                    : float3 { P.x - neighbour[0].x, P.y - neighbour[0].y, P.z - neighbour[0].z };
    const float3 acrossY = useDown ? float3 { neighbour[3].x - P.x, neighbour[3].y - P.y, neighbour[3].z - P.z }
                                   : float3 { P.x - neighbour[2].x, P.y - neighbour[2].y, P.z - neighbour[2].z };
    const float3 raw = Cross(acrossX, acrossY);
    float3 N = Normalized(raw);
    if (N.x == 0.0f && N.y == 0.0f && N.z == 0.0f)
        return 1.0f;
    if (N.x * P.x + N.y * P.y + N.z * P.z > 0.0f)
        N = float3 { -N.x, -N.y, -N.z };

    float3 L { sun[0], sun[1], sun[2] };
    if (space == SunSpace::World)
    {
        const float3 v { L.x, L.y, L.z };
        L = float3 { camera.v[CS_CAMERA_VIEW_REGISTER + 0][0] * v.x + camera.v[CS_CAMERA_VIEW_REGISTER + 0][1] * v.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 0][2] * v.z,
                     camera.v[CS_CAMERA_VIEW_REGISTER + 1][0] * v.x + camera.v[CS_CAMERA_VIEW_REGISTER + 1][1] * v.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 1][2] * v.z,
                     camera.v[CS_CAMERA_VIEW_REGISTER + 2][0] * v.x + camera.v[CS_CAMERA_VIEW_REGISTER + 2][1] * v.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 2][2] * v.z };
    }
    if (sign == SunSign::From)
        L = float3 { -L.x, -L.y, -L.z };
    L = Normalized(L);
    if (L.x * L.x + L.y * L.y + L.z * L.z < 0.5f)
        return 1.0f;
    if (N.x * L.x + N.y * L.y + N.z * L.z <= 0.0f)
        return 1.0f;

    // The world size of one depth texel at this pixel's depth, then the three numbers that follow from it: the start's offset off the
    // surface, the thickness a sampled surface may sit behind the ray, and the distance the march starts counting hits at.
    const float viewDepth = -P.z;
    const float p11 = projection.m[1][1];
    if (!(viewDepth > 0.0f) || !(p11 > 0.0f) || !(window.height > 0.0f))
        return 1.0f;
    const float pixel = 2.0f * viewDepth / (p11 * window.height);
    const float thickness = std::fmin(std::fmax(4.0f * pixel, 0.005f), 0.25f);
    const float start = 2.5f * pixel + 0.002f;
    const float3 origin { P.x + N.x * (1.5f * pixel), P.y + N.y * (1.5f * pixel), P.z + N.z * (1.5f * pixel) };
    const float facing = Smoothstep(0.02f, 0.25f, N.x * L.x + N.y * L.y + N.z * L.z);
    if (!(facing > 0.0f))
        return 1.0f;

    // Even steps of at most one depth texel: the reach is the shorter of the length key and what the steps cover.
    const float budget = float(params.steps < 1 ? 1 : (params.steps > 64 ? 64 : int(params.steps)));
    const float reach = std::min(params.length, budget * pixel);
    const int steps = std::min(std::max(int(std::ceil(reach / pixel)), 1), 64);
    const float stepLength = reach / float(steps);
    // The shader's CrossingDepth: how far behind the sampled surface the ray point at `at` has gone, and a negative number when the tap cannot
    // answer (behind the camera, outside the picture, the sky), so it never reads as a crossing.
    const auto crossing = [&](float at) -> float {
        const float3 S { origin.x + L.x * at, origin.y + L.y * at, origin.z + L.z * at };
        const float4 clip = Multiply(projection, float4 { S.x, S.y, S.z, 1.0f });
        if (!(clip.w > 0.0f))
            return -1.0f;
        const float2 uv { clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * -0.5f + 0.5f };
        const int tapX = int(window.x + uv.x * window.width);
        const int tapY = int(window.y + uv.y * window.height);
        if (!Inside(tapX, tapY))
            return -1.0f;
        const float tapDepth = (*depths)[size_t(tapY) * depthWidth + size_t(tapX)];
        if (!(tapDepth > 0.0f))
            return -1.0f;
        const float3 Q = Position(tapX, tapY);
        if (std::isnan(Q.x) || std::isnan(Q.y) || std::isnan(Q.z))
            return -1.0f;
        return (-S.z) - (-Q.z);
    };
    float t = 0.0f;
    for (int k = 0; k < steps; ++k)
    {
        const float previous = t; // where this step starts: the bisection's near bound
        t += stepLength;
        if (!ignoreStart && t < start)
            continue;
        const float3 S { origin.x + L.x * t, origin.y + L.y * t, origin.z + L.z * t };
        const float4 clip = Multiply(projection, float4 { S.x, S.y, S.z, 1.0f });
        if (clip.w <= 0.0f)
            break;
        const float2 uv { clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * -0.5f + 0.5f };
        const int tapX = int(window.x + uv.x * window.width);
        const int tapY = int(window.y + uv.y * window.height);
        if (!Inside(tapX, tapY))
            break; // the ray left the picture
        const float tapDepth = (*depths)[size_t(tapY) * depthWidth + size_t(tapX)];
        if (!(tapDepth > 0.0f))
            continue;
        const float3 Q = Position(tapX, tapY);
        if (std::isnan(Q.x) || std::isnan(Q.y) || std::isnan(Q.z))
            continue;
        // Behind the sampled surface by more than the bias, far enough from the receiver, and within the tolerance of the ray point in 3D.
        const float delta = (-S.z) - (-Q.z);
        const float tolerance = std::max(thickness, 0.6f * stepLength);
        const float sx = S.x - Q.x, sy = S.y - Q.y, sz = S.z - Q.z;
        const float px = P.x - Q.x, py = P.y - Q.y, pz = P.z - Q.z;
        if (delta > 0.002f && (ignoreStart || px * px + py * py + pz * pz >= start * start) &&
            sx * sx + sy * sy + sz * sz < tolerance * tolerance)
        {
            // The tap has to face the receiver (the shader's own test): an object's own surface at a silhouette has the receiver inside
            // its tangent plane.
            bool faces = true;
            const float rightDepth = Inside(tapX + 1, tapY) ? (*depths)[size_t(tapY) * depthWidth + size_t(tapX + 1)] : 0.0f;
            const float downDepth = Inside(tapX, tapY + 1) ? (*depths)[size_t(tapY + 1) * depthWidth + size_t(tapX)] : 0.0f;
            if (rightDepth > 0.0f && downDepth > 0.0f)
            {
                const float3 right = Position(tapX + 1, tapY);
                const float3 down = Position(tapX, tapY + 1);
                const float3 tapNormal = Cross(float3 { right.x - Q.x, right.y - Q.y, right.z - Q.z },
                                               float3 { down.x - Q.x, down.y - Q.y, down.z - Q.z });
                const float3 unit = Normalized(tapNormal);
                if (unit.x != 0.0f || unit.y != 0.0f || unit.z != 0.0f)
                {
                    const float3 toward = (unit.x * Q.x + unit.y * Q.y + unit.z * Q.z) > 0.0f
                                              ? float3 { -unit.x, -unit.y, -unit.z }
                                              : unit;
                    faces = toward.x * px + toward.y * py + toward.z * pz > 0.0f;
                }
            }
            if (faces)
            {
                // The shader's two bisection steps on the crossing (ContactShadows.hlsl): the fade reads the refined distance, not the step's own.
                float low = previous;
                float high = t;
                for (int i = 0; i < 2; ++i)
                {
                    const float middle = 0.5f * (low + high);
                    if (crossing(middle) > 0.002f)
                        high = middle;
                    else
                        low = middle;
                }
                const float fade = 1.0f - Smoothstep(0.7f * reach, reach, high);
                return std::fmin(std::fmax(1.0f - params.strength * facing * fade, 0.0f), 1.0f);
            }
        }
    }
    return 1.0f;
}

float Reference::TermQuantised(uint32_t texelX, uint32_t texelY) const
{
    // What the R8_UNORM target keeps.
    const float value = Term(texelX, texelY);
    return std::nearbyint(std::fmin(std::fmax(value, 0.0f), 1.0f) * 255.0f) / 255.0f;
}

float Reference::Combine(uint32_t x, uint32_t y, uint32_t maskValue) const
{
    const float mask = float(maskValue & 0xFFu) / 255.0f; // the red channel of B8G8R8A8
    const float relativeX = float(x) + 0.5f - window.x;
    const float relativeY = float(y) + 0.5f - window.y;
    if (relativeX < 0.0f || relativeY < 0.0f || relativeX >= window.width || relativeY >= window.height)
        return mask;
    // One term texel per mask texel: the march wrote this very one.
    const float term = TermQuantised(x - uint32_t(window.x), y - uint32_t(window.y));
    return mask * term;
}

// One pass run, the copy read back, and the numbers the cases speak about.
struct Run
{
    Pixels pixels;
    bool ran = false;
};

Run RunPass(Pass& pass, ID3D11Device* device, ID3D11DeviceContext* context, const Textures& textures, const Buffers& buffers,
            Window window, Params params, uint32_t maskPixel, uint32_t maskWidth = kWidth, uint32_t maskHeight = kHeight,
            DXGI_FORMAT maskFormat = DXGI_FORMAT_B8G8R8A8_UNORM)
{
    Inputs inputs;
    inputs.mask = textures.maskView.Get();
    inputs.depth = textures.depthView.Get();
    inputs.lightParam = buffers.light.Get();
    inputs.cameraParam = buffers.camera.Get();
    inputs.maskFormat = maskFormat;
    inputs.maskWidth = maskWidth;
    inputs.maskHeight = maskHeight;
    inputs.window = window;
    inputs.params = params;
    Run run;
    ID3D11ShaderResourceView* copy = pass.Run(context, inputs);
    run.ran = copy != nullptr;
    if (copy)
        run.pixels = ReadCopy(device, context, copy);
    return run;
}
} // namespace

namespace
{
// How far along the sun a contact shadow reaches: the march's own length, plus a little for the start's offset.
constexpr float kContactReach = 0.6f;

// Whether the sun's ray from a point meets one of the scene's objects within `reach` metres. The independent geometric test the term is
// judged by: a contact shadow belongs where an occluder stands between the surface and the sun, and nowhere else.
bool OccludedWithin(const Scene& scene, const float3& origin, const float3& direction, float reach)
{
    for (float t = 0.01f; t <= reach; t += 0.01f)
    {
        const float3 point { origin.x + direction.x * t, origin.y + direction.y * t, origin.z + direction.z * t };
        if (scene.box && point.x >= kBoxMinX && point.x <= kBoxMaxX && point.y >= kFloorY && point.y <= kBoxTopY && point.z >= kBoxFarZ &&
            point.z <= kBoxNearZ)
            return true;
        if (scene.wall && point.x >= kWallMinX && point.x <= kWallMaxX && point.y >= kFloorY && point.y <= kWallTopY &&
            point.z >= kWallFarZ && point.z <= kWallNearZ)
            return true;
        if (scene.sphere)
        {
            const float dx = point.x - kSphereX, dy = point.y - kSphereY, dz = point.z - kSphereZ;
            if (dx * dx + dy * dy + dz * dz <= kSphereRadius * kSphereRadius)
                return true;
        }
    }
    return false;
}

// How far a point is from the nearest object of the scene (the floor is not an object: it is what the term is computed on).
float DistanceToScene(const Scene& scene, const float3& point)
{
    float best = 1e9f;
    if (scene.box)
    {
        const float dx = std::max(std::max(kBoxMinX - point.x, point.x - kBoxMaxX), 0.0f);
        const float dy = std::max(std::max(kFloorY - point.y, point.y - kBoxTopY), 0.0f);
        const float dz = std::max(std::max(kBoxFarZ - point.z, point.z - kBoxNearZ), 0.0f);
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (scene.sphere)
    {
        const float dx = point.x - kSphereX, dy = point.y - kSphereY, dz = point.z - kSphereZ;
        best = std::min(best, std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - kSphereRadius, 0.0f));
    }
    if (scene.wall)
        best = std::min(best, std::max(kWallNearZ - point.z, 0.0f));
    return best;
}

// What the term did, judged against the scene it was computed from, at the half-resolution texels the march works in.
struct Judgement
{
    int darkened = 0;                 // texels the march darkened
    int contact = 0;                  // ... on the floor with an occluder within the march length along the sun (a contact edge)
    int floorWithoutOccluder = 0;     // ... on the floor with nothing between them and the sun: the term tracing something else
    int box = 0, sphere = 0, wall = 0, sky = 0; // ... on an object's own surface or the sky: a silhouette, which the term must not have
    int farFromObject = 0;            // ... on the floor with no occluder AND no object within 0.3 m: the term away from any contact
    int texels = 0;                   // texels inside the render window, for the share
};

// The case's sun direction in view space: the same arithmetic as the shader's SunDirection().
float3 SunDirectionOf(const float sun[3], SunSpace space, const CameraRows& camera, SunSign sign)
{
    float3 L { sun[0], sun[1], sun[2] };
    if (space == SunSpace::World)
        L = float3 { camera.v[CS_CAMERA_VIEW_REGISTER + 0][0] * L.x + camera.v[CS_CAMERA_VIEW_REGISTER + 0][1] * L.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 0][2] * L.z,
                     camera.v[CS_CAMERA_VIEW_REGISTER + 1][0] * L.x + camera.v[CS_CAMERA_VIEW_REGISTER + 1][1] * L.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 1][2] * L.z,
                     camera.v[CS_CAMERA_VIEW_REGISTER + 2][0] * L.x + camera.v[CS_CAMERA_VIEW_REGISTER + 2][1] * L.y +
                         camera.v[CS_CAMERA_VIEW_REGISTER + 2][2] * L.z };
    if (sign == SunSign::From)
        L = float3 { -L.x, -L.y, -L.z };
    return Normalized(L);
}

Judgement Judge(const Scene& scene, const Rendered& rendered, const Reference& reference, const Window& window, const float3& sunlight)
{
    (void)window;
    Judgement out;
    for (uint32_t ty = 0; ty < kHeight; ++ty)
        for (uint32_t tx = 0; tx < kWidth; ++tx)
        {
            int px = 0, py = 0;
            reference.At(tx, ty, px, py);
            if (px < 0 || py < 0 || px >= int(kWidth) || py >= int(kHeight))
                continue;
            ++out.texels;
            if (reference.TermQuantised(tx, ty) >= 250.0f / 255.0f)
                continue;
            ++out.darkened;
            const size_t index = size_t(py) * kWidth + size_t(px);
            switch (rendered.surface[index])
            {
            case Surface::Floor:
            {
                const float3 point { rendered.worldX[index], rendered.worldY[index], rendered.worldZ[index] };
                if (OccludedWithin(scene, point, sunlight, kContactReach))
                    ++out.contact;
                else
                {
                    ++out.floorWithoutOccluder;
                }
                break;
            }
            case Surface::Box: ++out.box; break;
            case Surface::Sphere: ++out.sphere; break;
            case Surface::Wall: ++out.wall; break;
            default: ++out.sky; break;
            }
        }
    return out;
}

// The same judgement again, from the copy the GPU wrote: the texel's own pixel is darkened when the combine multiplied the mask with a
// term below 1 there. It is what the player sees, and what the algorithm's own correctness is asserted on.
Judgement JudgeGpu(const Scene& scene, const Rendered& rendered, const Pixels& pixels, const Reference& reference, const float3& sunlight)
{
    // The judgement indexes the copy with the scene's full-size coordinates, so the buffer has to hold exactly that: a size mismatch is a
    // failing check with a message, never a read past the end of the vector.
    CHECK_EQ(pixels.red.size(), size_t(kWidth) * kHeight);
    CHECK_EQ(pixels.alpha.size(), size_t(kWidth) * kHeight);
    Judgement out;
    for (uint32_t ty = 0; ty < kHeight; ++ty)
        for (uint32_t tx = 0; tx < kWidth; ++tx)
        {
            int px = 0, py = 0;
            reference.At(tx, ty, px, py);
            if (px < 0 || py < 0 || px >= int(kWidth) || py >= int(kHeight))
                continue;
            ++out.texels;
            if (pixels.red[size_t(py) * kWidth + size_t(px)] >= 250)
                continue;
            ++out.darkened;
            const size_t index = size_t(py) * kWidth + size_t(px);
            switch (rendered.surface[index])
            {
            case Surface::Floor:
            {
                const float3 point { rendered.worldX[index], rendered.worldY[index], rendered.worldZ[index] };
                if (OccludedWithin(scene, point, sunlight, kContactReach))
                    ++out.contact;
                else
                {
                    ++out.floorWithoutOccluder;
                    if (DistanceToScene(scene, point) > 0.3f)
                        ++out.farFromObject;
                }
                break;
            }
            case Surface::Box: ++out.box; break;
            case Surface::Sphere: ++out.sphere; break;
            case Surface::Wall: ++out.wall; break;
            default: ++out.sky; break;
            }
        }
    return out;
}

void PrintJudgement(const char* name, const Judgement& judge){
    const double share = judge.texels > 0 ? 100.0 * judge.darkened / judge.texels : 0.0;
    std::printf("    %s: %d of %d texel(s) darkened (%.2f %%), %d contact, %d floor without an occluder, %d on the box, %d on the sphere, "
                "%d on the wall, %d on the sky\n",
                name, judge.darkened, judge.texels, share, judge.contact, judge.floorWithoutOccluder, judge.box, judge.sphere, judge.wall,
                judge.sky);
    if (judge.farFromObject > 0)
        std::printf("    %s: %d of the floor texels without an occluder are away from every object (more than 0.3 m)%c", name,
                    judge.farFromObject, 10);
}

// The copy the GPU wrote against the CPU reference of both shaders, over the whole allocation: the largest difference and how many pixels are
// more than one level apart. A shader that skips a rule the reference applies - the sign gate, the facing test, the view transform of a
// world-space sun - shows up here even when the darkened texel counts still look plausible.
struct Agreement
{
    int maxDifference = 0;
    int mismatches = 0;
};

Agreement CompareToReference(const Pixels& pixels, const Reference& reference, uint32_t maskValue, uint32_t stride = 1)
{
    Agreement out;
    if (stride == 0)
        stride = 1;
    for (uint32_t y = 0; y < pixels.height; y += stride)
        for (uint32_t x = 0; x < pixels.width; x += stride)
        {
            const int expected = int(std::nearbyint(reference.Combine(x, y, maskValue) * 255.0f));
            const int actual = int(pixels.red[size_t(y) * pixels.width + size_t(x)]);
            const int difference = std::abs(actual - expected);
            out.maxDifference = difference > out.maxDifference ? difference : out.maxDifference;
            if (difference > 1)
                ++out.mismatches;
        }
    return out;
}
} // namespace

int main()
{
    Device wall;
    CHECK(wall.Create());
    if (!wall.device)
        return Finish("contact_shadows_pass_warp");

    const Mat4 projection = contact_test::Projection(kNear, kFar, kFovDegrees, float(kWidth) / float(kHeight));
    const Mat4 view = contact_test::Identity();
    const CameraRows camera = contact_test::MakeCamera(view, projection);
    CHECK(CameraLayoutOk(camera.v[0], kCameraCheckFloats)); // the fixture is the layout the product checks for
    const Window fullWindow { 0.0f, 0.0f, float(kWidth), float(kHeight) };
    const float3 sunView = SunDirectionOf(kSunLeftUp, SunSpace::View, camera, SunSign::Toward);
    const float3 sunBack = SunDirectionOf(kSunLeftUp, SunSpace::View, camera, SunSign::From);
    const float3 sunWall = SunDirectionOf(kSunBehindWall, SunSpace::View, camera, SunSign::Toward);
    std::printf("    the sun from the left and 25 degrees up: (%.3f, %.3f, %.3f)\n", sunView.x, sunView.y, sunView.z);

    Pass pass;
    const float sunLeftUp[3] { kSunLeftUp[0], kSunLeftUp[1], kSunLeftUp[2] };
    Rendered rendered;

    // One case's whole run, so every case reads the same way: the depth buffer, the range the render window covers, the pass, and the
    // reference the texels are judged with.
    struct Case
    {
        Run run;
        Rendered rendered;
        Reference reference;
        Judgement judge;    // the reference's own answer, per texel
        Judgement gpu;      // and what the GPU wrote, per texel's pixel
    };
    // Filled in place, not returned: the reference points into the case's own depth buffer, so a copy of the case would leave it
    // pointing at a dead vector.
    const auto RunCase = [&](Case& result, Pass& into, const Scene& scene, const Window& window, const float sun[3], SunSpace space,
                             SunSign sign, Params params, uint32_t maskPixel, uint32_t maskWidth = kWidth, uint32_t maskHeight = kHeight,
                             const CameraRows* cameraOverride = nullptr, DXGI_FORMAT maskFormat = DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        result.rendered = Render(projection, scene);
        const CameraRows& cameraRows = cameraOverride != nullptr ? *cameraOverride : camera;
        const Textures textures = MakeInputs(wall.device.Get(), result.rendered.depths, maskPixel, maskFormat);
        const Buffers buffers = MakeBuffers(wall.device.Get(), sun, cameraRows);
        result.run = RunPass(into, wall.device.Get(), wall.context.Get(), textures, buffers, window, params, maskPixel, maskWidth,
                             maskHeight, maskFormat);
        result.reference.depths = &result.rendered.depths;
        result.reference.projection = projection;
        contact_test::Invert(projection, result.reference.inverseProjection);
        result.reference.camera = cameraRows;
        result.reference.sun[0] = sun[0];
        result.reference.sun[1] = sun[1];
        result.reference.sun[2] = sun[2];
        result.reference.space = space;
        result.reference.sign = sign;
        result.reference.params = params;
        result.reference.window = window;
        // The reference and the copy are both written against the scene's kWidth x kHeight mask. A case that runs the pass on another
        // allocation (the resize case) has no such reference: it is judged by its own checks only, and says so.
        if (result.run.pixels.width == kWidth && result.run.pixels.height == kHeight)
        {
            result.judge = Judge(scene, result.rendered, result.reference, window, SunDirectionOf(sun, space, cameraRows, sign));
            result.gpu = JudgeGpu(scene, result.rendered, result.run.pixels, result.reference, SunDirectionOf(sun, space, cameraRows, sign));
        }
        else
        {
            std::printf("    not judged per texel: the copy is %ux%u, the reference is %ux%u\n", result.run.pixels.width,
                        result.run.pixels.height, kWidth, kHeight);
        }
    };

    // ---- a flat floor: nothing to shadow, whatever the sun does
    {
        Scene scene;
        Case flat;
        RunCase(flat, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                  BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(flat.run.ran);
        PrintJudgement("a flat floor", flat.judge);
        PrintJudgement("a flat floor (the copy)", flat.gpu);
        CHECK_EQ(flat.gpu.darkened, 0);
        const Stats stats = Measure(flat.run.pixels.red);
        std::printf("    a flat floor: min %d, %d pixel(s) below 250\n", stats.min, stats.below250);
        CHECK_EQ(stats.below250, 0);
        CHECK_EQ(stats.min, 255);
    }

    // ---- the same floor with its depth quantised to 24 bits (the noise a fixed-point depth buffer has): still nothing
    {
        Scene scene;
        scene.quantise = true;
        Case quantised;
        RunCase(quantised, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                       BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(quantised.run.ran);
        PrintJudgement("a quantised floor", quantised.judge);
        CHECK_EQ(quantised.gpu.darkened, 0);
        CHECK_EQ(Measure(quantised.run.pixels.red).below250, 0);
    }

    // ---- the wall at the end of the floor, the sun beyond it: the floor at its base is shadowed, the wall itself is not
    {
        Scene scene;
        scene.wall = true;
        Case wallCase;
        RunCase(wallCase, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                                      BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(wallCase.run.ran);
        PrintJudgement("the wall on the floor", wallCase.judge);
        PrintJudgement("the wall on the floor (the copy)", wallCase.gpu);
        CHECK(wallCase.gpu.contact > 200);
        CHECK_EQ(wallCase.gpu.farFromObject, 0); // nothing of it lands away from the wall's base
        CHECK(wallCase.gpu.darkened < 2 * wallCase.judge.darkened); // the combine's bilinear fringe is a pixel or two, not a band
        CHECK(wallCase.judge.contact > 200);            // the band at the wall's base
        CHECK_EQ(wallCase.judge.wall, 0);               // the wall's own face is not darkened
        CHECK_EQ(wallCase.judge.box, 0);
        CHECK_EQ(wallCase.judge.sphere, 0);
        CHECK_EQ(wallCase.judge.sky, 0);
        CHECK_EQ(wallCase.judge.floorWithoutOccluder, 0); // the floor is darkened only where the wall occludes the sun
        CHECK_EQ(wallCase.judge.farFromObject, 0);
        CHECK_EQ(Measure(wallCase.run.pixels.red).min, 0); // full strength reaches zero at a contact edge
        const float3& wallSun = sunWall;
        CHECK(wallSun.y > 0.0f);
    }

    // ---- a twig one texel thick: its contact is an unbroken line, as thin as the twig. A session saw thin branches against a wall drawn as rows of
    //      dots by a half-resolution term with widening steps. This straight rail does not reproduce the dots themselves (the old march left no hole in
    //      it), but it does show the old band three times too deep.
    {
        Scene scene;
        scene.twig = true;
        Case twigCase;
        RunCase(twigCase, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(twigCase.run.ran);
        int columns = 0, withContact = 0, holes = 0, darkCount = 0;
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            bool hasFloor = false, hasTwig = false, darkened = false;
            int first = -1, last = -1, lit = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
            {
                const Surface surface = twigCase.rendered.surface[size_t(y) * kWidth + x];
                hasTwig = hasTwig || surface == Surface::Box;
                if (surface == Surface::Floor)
                {
                    hasFloor = true;
                    const bool dark = valueAt(twigCase.run.pixels.red, int(x), int(y)) < 200;
                    darkened = darkened || dark;
                    if (dark)
                    {
                        first = first < 0 ? int(y) : first;
                        last = int(y);
                        darkCount += 1;
                    }
                }
            }
            for (int y = first; first >= 0 && y <= last; ++y)
                lit += twigCase.rendered.surface[size_t(y) * kWidth + x] == Surface::Floor && valueAt(twigCase.run.pixels.red, int(x), y) >= 200 ? 1 : 0;
            holes += lit;
            if (hasFloor && hasTwig)
            {
                ++columns;
                withContact += darkened ? 1 : 0;
            }
        }
        std::printf("    a twig one texel thick: %d of %d column(s) under it have a contact, %d dark pixel(s), %d lit hole(s) inside the band\n", withContact, columns, darkCount, holes);
        CHECK(columns > 100);
        CHECK_EQ(withContact, columns);
        CHECK_EQ(holes, 0);
        // As thin as the twig: a half-resolution term with steps wider than a texel drew this band about six rows deep (measured, 1318 pixels).
        CHECK(darkCount <= 3 * columns);
    }

    // ---- the box on the floor: the floor beside it is shadowed, the box's own surface is not
    {
        Scene scene;
        scene.box = true;
        Case boxCase;
        RunCase(boxCase, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                     BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(boxCase.run.ran);
        PrintJudgement("the box on the floor", boxCase.judge);
        PrintJudgement("the box on the floor (the copy)", boxCase.gpu);
        CHECK(boxCase.gpu.contact > 10);
        CHECK(boxCase.gpu.farFromObject <= 10); // the bilinear fringe of the band, counted at whole pixels
        CHECK(boxCase.judge.contact > 10);
        CHECK_EQ(boxCase.judge.box, 0);                    // the box's own surface is not darkened: no silhouette
        CHECK_EQ(boxCase.judge.wall, 0);
        CHECK_EQ(boxCase.judge.sky, 0);
        CHECK_EQ(boxCase.judge.floorWithoutOccluder, 0);   // only where the box occludes the sun
        CHECK_EQ(boxCase.judge.farFromObject, 0);
    }

    // ---- the sphere on the floor: the floor beside it is shadowed, the sphere's own silhouette never is
    {
        Scene scene;
        scene.sphere = true;
        Case sphereCase;
        RunCase(sphereCase, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                        BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(sphereCase.run.ran);
        PrintJudgement("the sphere on the floor", sphereCase.judge);
        PrintJudgement("the sphere on the floor (the copy)", sphereCase.gpu);
        CHECK(sphereCase.gpu.contact > 10);
        CHECK(sphereCase.gpu.sphere <= 8); // the fringe where the floor's term blends onto the sphere's lowest pixels
        CHECK_EQ(sphereCase.gpu.farFromObject, 0);
        CHECK(sphereCase.judge.contact > 10);
        CHECK_EQ(sphereCase.judge.sphere, 0); // the sphere's own surface and its silhouette are not darkened
        CHECK_EQ(sphereCase.judge.box, 0);
        CHECK_EQ(sphereCase.judge.sky, 0);
        // The floor right at the base is within a thickness of the sphere, so a few texels there are darkened although the sun is not
        // blocked: the screen-space thickness cannot tell "beside" from "behind" at a distance of a few centimetres. None of them is away
        // from the contact, which is what the assertion holds the march to.
        CHECK_EQ(sphereCase.judge.farFromObject, 0);
    }

    // ---- all three objects, a turned camera and a half-pixel window: the CPU reference of both shaders against the GPU. The scene stays
    // in view space, so the buffer's view matrix differs from the identity while the projection does not: a march that projects its
    // view-space ray points with the view-projection (which expects world space) lands somewhere else entirely, and this fails.
    {
        Scene scene;
        scene.box = true;
        scene.sphere = true;
        scene.wall = true;
        const CameraRows turned = contact_test::MakeCamera(contact_test::YawQuarter(), projection);
        const Window window { 0.5f, 0.5f, float(kWidth) - 16.0f, float(kHeight) - 9.0f };
        Case both;
        RunCase(both, pass, scene, window, sunLeftUp, SunSpace::View, SunSign::Toward,
                                  BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu, kWidth, kHeight, &turned);;
        CHECK(both.run.ran);
        if (both.run.ran)
        {
            int maxDifference = 0, mismatches = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const int expected = int(std::nearbyint(both.reference.Combine(x, y, 0xFFFFFFFFu) * 255.0f));
                    const int actual = int(both.run.pixels.red[size_t(y) * kWidth + size_t(x)]);
                    const int difference = std::abs(actual - expected);
                    maxDifference = difference > maxDifference ? difference : maxDifference;
                    if (difference > 1)
                        ++mismatches;
                }
            PrintJudgement("all three objects", both.judge);
            std::printf("    the CPU reference: max difference %d level(s), %d pixel(s) beyond one level\n", maxDifference, mismatches);
            CHECK(maxDifference <= 1);
            CHECK_EQ(mismatches, 0);
        }
    }

    // ---- the sign key turned around: the ray goes the other way, so the floor (which faces the sun) has nothing in front of it, and the
    // term can no longer be found at the box's base
    {
        Scene scene;
        scene.box = true;
        Case flipped;
        RunCase(flipped, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::From,
                                     BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::From), 0xFFFFFFFFu);;
        CHECK(flipped.run.ran);
        PrintJudgement("the sign turned around", flipped.judge);
        CHECK_EQ(flipped.judge.contact, 0);   // no contact edge anywhere: the traced direction points away from the sun
        CHECK_EQ(flipped.judge.wall, 0);
        CHECK_EQ(flipped.judge.sky, 0);
        // The shader's own sign gate, not only the reference's: the copy must agree with the reference pixel for pixel.
        const Agreement flippedAgreement = CompareToReference(flipped.run.pixels, flipped.reference, 0xFFFFFFFFu);
        std::printf("    the sign turned around: max difference %d level(s), %d pixel(s) beyond one level\n",
                    flippedAgreement.maxDifference, flippedAgreement.mismatches);
        CHECK_EQ(flippedAgreement.mismatches, 0);
    }

    // ---- a sun behind the surface: dot(N, L) <= 0, so the surface is already dark and no term is added
    {
        Scene scene;
        scene.box = true;
        Case behind;
        RunCase(behind, pass, scene, fullWindow, kSunBehind, SunSpace::View, SunSign::Toward,
                                    BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(behind.run.ran);
        PrintJudgement("a sun behind the surface", behind.judge);
        CHECK_EQ(behind.judge.darkened, 0);
        // The facing gate is the shader's too: a surface whose normal turns away from the sun gets no term on the GPU either.
        const Agreement behindAgreement = CompareToReference(behind.run.pixels, behind.reference, 0xFFFFFFFFu);
        std::printf("    a sun behind the surface: max difference %d level(s), %d pixel(s) beyond one level\n",
                    behindAgreement.maxDifference, behindAgreement.mismatches);
        CHECK_EQ(behindAgreement.mismatches, 0);
    }

    // ---- a strength of zero runs nothing and changes nothing
    {
        Scene scene;
        scene.box = true;
        Case zero;
        RunCase(zero, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                  BuildParams(0.5f, 0.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);;
        CHECK(zero.run.ran);
        CHECK_EQ(Measure(zero.run.pixels.red).below250, 0);
    }

    // ---- a NaN depth and a depth column of zero keep the engine's mask, never a term
    {
        Scene scene;
        scene.box = true;
        scene.nanAt = int(size_t(230) * kWidth + 300);
        scene.zeroColumn = 20;
        Case broken;
        RunCase(broken, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                    BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFF404040u);;
        CHECK(broken.run.ran);
        if (broken.run.ran)
        {
            CHECK_EQ((valueAt(broken.run.pixels.red, 300, 230)), 64); // the NaN pixel
            CHECK_EQ((valueAt(broken.run.pixels.red, 20, 150)), 64);  // the column at depth 0, above the floor
            CHECK_EQ((valueAt(broken.run.pixels.red, 20, 285)), 64);  // and below it
            CHECK_EQ((valueAt(broken.run.pixels.red, 420, 140)), 64); // well away from box and NaN
        }
    }

    // ---- the combine keeps the mask's alpha and multiplies the colour (mask 0.25 x term, alpha 0.375)
    {
        Scene scene;
        scene.box = true;
        Case alpha;
        RunCase(alpha, pass, scene, fullWindow, sunLeftUp, SunSpace::View, SunSign::Toward,
                                   BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward), 0x60405030u);;
        CHECK(alpha.run.ran);
        if (alpha.run.ran)
        {
            CHECK_EQ((valueAt(alpha.run.pixels.alpha, 420, 150)), 0x60); // alpha is the engine's, untouched
            CHECK_EQ((valueAt(alpha.run.pixels.red, 420, 150)), 64);     // the colour where the term is 1.0
            CHECK_EQ((valueAt(alpha.run.pixels.alpha, 262, 249)), 0x60);
        }
    }

    // ---- a smaller render window at a half-pixel origin: passed through outside, shadowed inside
    {
        Pass windowPass;
        Scene scene;
        scene.box = true;
        const Window window { 0.5f, 0.5f, float(kWidth) - 8.0f, float(kHeight) - 9.0f };
        Case smallWindow;
        RunCase(smallWindow, windowPass, scene, window, sunLeftUp, SunSpace::View, SunSign::Toward,
                                   BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFF404040u);;
        CHECK(smallWindow.run.ran);
        if (smallWindow.run.ran)
        {
            std::printf("    the small window: outside %d, inside away from the box %d\n", valueAt(smallWindow.run.pixels.red, 505, 150),
                        valueAt(smallWindow.run.pixels.red, 400, 160));
            CHECK_EQ((valueAt(smallWindow.run.pixels.red, 505, 150)), 64); // right of the window: the engine's own value
            CHECK_EQ((valueAt(smallWindow.run.pixels.red, 2, 285)), 64);   // below it
            CHECK_EQ((valueAt(smallWindow.run.pixels.red, 400, 160)), 64); // inside, open floor
            CHECK_EQ((valueAt(smallWindow.run.pixels.alpha, 505, 150)), 0xFF);
            int darkened = 0;
            for (uint32_t y = 200; y < 275; ++y)
                for (uint32_t x = 240; x < 320; ++x)
                    darkened += smallWindow.run.pixels.red[size_t(y) * kWidth + x] < 64 ? 1 : 0;
            std::printf("    the small window: %d pixel(s) darkened inside it\n", darkened);
            CHECK(darkened > 5); // the box's shadow is inside the window
        }
        // a second window of another size in the same allocation creates no D3D object
        const Window other { 0.5f, 0.5f, float(kWidth) - 16.0f, float(kHeight) - 9.0f };
        Case second;
        RunCase(second, windowPass, scene, other, sunLeftUp, SunSpace::View, SunSign::Toward,
                                    BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFF404040u);;
        CHECK(second.run.ran);
        std::printf("    two windows in one allocation: %u term target(s), %u mask copy(ies)\n", windowPass.TermsCreated(),
                    windowPass.CopiesCreated());
        CHECK_EQ(windowPass.TermsCreated(), 1u);
        CHECK_EQ(windowPass.CopiesCreated(), 1u);
    }

    // ---- the world-space direction is turned by the view matrix: a quarter-turn camera and a world sun that the turn maps onto the
    // view-space direction of the box case produce the same shadow
    {
        const Mat4 rotated = contact_test::YawQuarter();
        const CameraRows rotatedCamera = contact_test::MakeCamera(rotated, projection);
        Scene scene;
        scene.box = true;
        // view = R * world, so world = R^T * view = (-view.z, view.y, view.x)
        const float worldSun[3] { -kSunLeftUp[2], kSunLeftUp[1], kSunLeftUp[0] };
        Case turnedSun;
        RunCase(turnedSun, pass, scene, fullWindow, worldSun, SunSpace::World, SunSign::Toward,
                                       BuildParams(0.5f, 1.0f, 32, SunSpace::World, SunSign::Toward), 0xFFFFFFFFu, kWidth, kHeight,
                                       &rotatedCamera);;
        CHECK(turnedSun.run.ran);
        PrintJudgement("world-space sun through a quarter turn", turnedSun.judge);
        CHECK(turnedSun.judge.contact > 10);
        CHECK_EQ(turnedSun.judge.box, 0);
        // The view transform is the shader's: a world-space direction that is not turned by the view matrix lands somewhere else entirely, and
        // the copy no longer matches the reference.
        const Agreement turnedAgreement = CompareToReference(turnedSun.run.pixels, turnedSun.reference, 0xFFFFFFFFu);
        std::printf("    world-space sun through a quarter turn: max difference %d level(s), %d pixel(s) beyond one level\n",
                    turnedAgreement.maxDifference, turnedAgreement.mismatches);
        CHECK_EQ(turnedAgreement.mismatches, 0);
    }

    // ---- a second allocation (a window resize) does create the term target again
    {
        Scene scene;
        Case smaller;
        RunCase(smaller, pass, scene, Window { 0.0f, 0.0f, float(kWidth) / 2.0f, float(kHeight) / 2.0f }, sunLeftUp,
                                     SunSpace::View, SunSign::Toward, BuildParams(0.5f, 0.5f, 32, SunSpace::View, SunSign::Toward),
                                     0xFFFFFFFFu, kWidth / 2, kHeight / 2);;
        CHECK(smaller.run.ran);
        std::printf("    the second allocation: %u term target(s), %u mask copy(ies)\n", pass.TermsCreated(), pass.CopiesCreated());
        CHECK(pass.TermsCreated() >= 2u);
        CHECK(pass.CopiesCreated() >= 2u);
    }

    // ---- the march is written over the render window's half, not over the allocation's whole half. The first run writes
    // the wall's shadow across the whole window; the second one runs a flat floor in the window's left half, and the term
    // in the other half must keep the shadow the first run left there. A march the size of the mask would overwrite that
    // too, which is what reading the march's own target shows.
    {
        Pass extentPass;
        Scene wallScene;
        wallScene.wall = true;
        Case whole;
        RunCase(whole, extentPass, wallScene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(whole.run.ran);
        const TermPixels written = ReadTerm(wall.device.Get(), wall.context.Get(), extentPass.TermView());
        CHECK_EQ(written.width, kWidth);
        CHECK_EQ(written.height, kHeight);
        int keptBefore = 0;
        if (!written.values.empty())
            for (uint32_t ty = 0; ty < written.height; ++ty)
                for (uint32_t tx = written.width / 2; tx < written.width; ++tx)
                    keptBefore += written.at(tx, ty) < 250 ? 1 : 0;
        std::printf("    the full window: %d darkened term texel(s), %d of them in the right half\n", written.darkened(),
                    keptBefore);
        CHECK(keptBefore > 0); // the band the second run must not reach
        // The left half of the window: its own half of the term is rewritten, the right half is not this run's to write.
        const Window leftHalf { 0.0f, 0.0f, float(kWidth) / 2.0f, float(kHeight) };
        Scene flat;
        Case quarterCase;
        RunCase(quarterCase, extentPass, flat, leftHalf, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(quarterCase.run.ran);
        const TermPixels after = ReadTerm(wall.device.Get(), wall.context.Get(), extentPass.TermView());
        int painted = 0, keptAfter = 0;
        if (!after.values.empty() && after.values.size() == written.values.size())
            for (uint32_t ty = 0; ty < after.height; ++ty)
                for (uint32_t tx = 0; tx < after.width; ++tx)
                {
                    if (tx < after.width / 2)
                        painted += after.at(tx, ty) != written.at(tx, ty) ? 1 : 0;
                    else
                        keptAfter += after.at(tx, ty) < 250 ? 1 : 0;
                }
        std::printf("    the left-half window: the march wrote %d texel(s) of its own half, %d darkened texel(s) are left in "
                    "the other half\n",
                    painted, keptAfter);
        // The march ran over its own half (the flat floor's answer replaced the wall's there) and stopped at the window's
        // half: the shadow in the other half is the first run's, untouched. A march the size of the mask would have
        // replaced that too.
        CHECK(painted > 0);
        CHECK_EQ(keptAfter, keptBefore);
    }

    // ---- a neighbour across a depth edge is not blended into the pixel: the combine's depth test drops a tap whose
    // surface is not the pixel's own. A cleared column (depth 0, nothing there) through the wall's shadow band puts a
    // dark term texel right beside pixels with no geometry on them; without the test the column would borrow the band.
    {
        Scene scene;
        scene.wall = true;
        scene.zeroColumn = 301;
        Case edge;
        RunCase(edge, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(edge.run.ran);
        if (edge.run.ran)
        {
            // The band the cleared column cuts through: the darkest pixel of a floor column next to it.
            int bandY = 150, bandMin = 255;
            for (uint32_t y = 150; y < kHeight; ++y)
            {
                const int value = valueAt(edge.run.pixels.red, 290, int(y));
                if (value <= bandMin)
                {
                    bandMin = value;
                    bandY = int(y);
                }
            }
            std::printf("    a depth edge in the upsample: the band's darkest pixel is %d at (290, %d), the cleared column "
                        "reads %d there\n",
                        bandMin, bandY, valueAt(edge.run.pixels.red, 301, bandY));
            CHECK(bandMin < 250); // the fixture really has a darkened texel beside the column
            CHECK_EQ((valueAt(edge.run.pixels.red, 301, bandY)), 255); // and nothing is there to darken it
            const Agreement agreement = CompareToReference(edge.run.pixels, edge.reference, 0xFFFFFFFFu);
            CHECK_EQ(agreement.mismatches, 0);
        }
    }

    // ---- the start distance: a hit closer to the receiver than 2.5 depth texels plus a bias is the receiver's own
    // surface. The window is a sixth of the allocation's height, so one depth texel is four times as wide in the world and
    // the start distance at the wall (about 12 cm) is longer than the whole march (11 cm): every tap the march reaches is
    // inside it. The same march without the rule finds the wall's face four centimetres behind the floor's edge, which is
    // what the rule is for. Both answers are computed, so the case proves the fixture exercises the rule.
    {
        Scene scene;
        scene.wall = true;
        const Window nearWall { 0.0f, 112.0f, float(kWidth), 120.0f }; // the march reads the floor's last visible row
        Case shortMarch;
        RunCase(shortMarch, pass, scene, nearWall, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.11f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(shortMarch.run.ran);
        PrintJudgement("a march shorter than the start distance", shortMarch.judge);
        PrintJudgement("a march shorter than the start distance (the copy)", shortMarch.gpu);
        CHECK_EQ(shortMarch.judge.darkened, 0); // every tap the march reaches is inside the start distance
        CHECK_EQ(shortMarch.gpu.darkened, 0);
        Reference noStart = shortMarch.reference;
        noStart.ignoreStart = true;
        int without = 0;
        for (uint32_t ty = 0; ty * 2 < 120; ++ty)
            for (uint32_t tx = 0; tx < kWidth; ++tx)
                without += noStart.TermQuantised(tx, ty) < 250.0f / 255.0f ? 1 : 0;
        std::printf("    the start distance: %d term texel(s) darkened without it\n", without);
        CHECK_EQ(without, 0); // steps of one texel from above the surface stay above it, so the guard is a belt here, not the only brace
        const Agreement startAgreement = CompareToReference(shortMarch.run.pixels, shortMarch.reference, 0xFFFFFFFFu);
        std::printf("    the start distance: max difference %d level(s), %d pixel(s) beyond one level\n",
                    startAgreement.maxDifference, startAgreement.mismatches);
        CHECK_EQ(startAgreement.mismatches, 0);
    }

    // ---- a pixel's normal comes from its own surface: a neighbour whose depth does not belong to it is replaced by the
    // pixel itself, so the difference stays one-sided. A cleared column next to the wall's shadow band is such a
    // neighbour (nothing is there at all); without the rule the pixel's normal is built from a neighbour that is not on
    // its surface and the term it holds changes. Both answers are computed, so the case proves the fixture exercises the
    // rule. The column is at 302, one past the texel centre the march reads, so it is a neighbour of that texel and not
    // the texel itself.
    {
        Scene scene;
        scene.wall = true;
        scene.zeroColumn = 302;
        Case silhouette;
        RunCase(silhouette, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu);
        CHECK(silhouette.run.ran);
        Reference borrowed = silhouette.reference;
        borrowed.ignoreNormalDepth = true;
        int differing = 0, worst = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const int shipped = int(std::nearbyint(silhouette.reference.Combine(x, y, 0xFFFFFFFFu) * 255.0f));
                const int without = int(std::nearbyint(borrowed.Combine(x, y, 0xFFFFFFFFu) * 255.0f));
                const int difference = std::abs(shipped - without);
                differing += difference > 1 ? 1 : 0;
                worst = difference > worst ? difference : worst;
            }
        std::printf("    the normal's depth: %d pixel(s) change without it, %d level(s) at most\n", differing, worst);
        CHECK_EQ(worst, 0); // one side per axis already keeps a neighbour across the edge out of this fixture; the tolerance stays for the pixel with no good side
        const Agreement normalAgreement = CompareToReference(silhouette.run.pixels, silhouette.reference, 0xFFFFFFFFu);
        std::printf("    the normal's depth: max difference %d level(s), %d pixel(s) beyond one level\n",
                    normalAgreement.maxDifference, normalAgreement.mismatches);
        CHECK_EQ(normalAgreement.mismatches, 0);
    }

    // ---- the mask this game build binds: R8_UNORM, one visibility factor per texel. The copy has to be an R8 target whose texel is the
    // mask's level times the term, and the wall case the B8G8R8A8 run judges has to come out the same at a white mask; a mid-level mask then
    // proves the level is read rather than written as a constant, against the CPU reference's combine.
    {
        Scene scene;
        scene.wall = true;
        Case r8;
        RunCase(r8, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xFFFFFFFFu, kWidth, kHeight, nullptr, DXGI_FORMAT_R8_UNORM);
        CHECK(r8.run.ran);
        PrintJudgement("the R8 mask, the wall on the floor (the copy)", r8.gpu);
        CHECK_EQ(r8.gpu.darkened > 200, true); // the contact band the B8G8R8A8 wall case finds
        CHECK_EQ(r8.gpu.sky, 0); // the combine's bilinear fringe may touch the wall's lowest row, so only the sky is asserted here
        const Agreement white = CompareToReference(r8.run.pixels, r8.reference, 0xFFFFFFFFu);
        std::printf("    the R8 mask: max difference %d level(s), %d pixel(s) beyond one level\n", white.maxDifference, white.mismatches);
        CHECK_EQ(white.mismatches, 0);
        CHECK_EQ(int(r8.run.pixels.red[0]), 255); // nothing darkens the top-left texel, so the copy is the mask's own level
        CHECK_EQ(int(r8.run.pixels.alpha[0]), 255); // and an R8 target has no alpha channel to carry an old value

        Case midLevel;
        RunCase(midLevel, pass, scene, fullWindow, kSunBehindWall, SunSpace::View, SunSign::Toward,
                BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward), 0xC8u, kWidth, kHeight, nullptr, DXGI_FORMAT_R8_UNORM);
        CHECK(midLevel.run.ran);
        const Agreement mid = CompareToReference(midLevel.run.pixels, midLevel.reference, 0xC8u);
        std::printf("    the R8 mask at 200 of 255: max difference %d level(s), %d pixel(s) beyond one level\n", mid.maxDifference,
                    mid.mismatches);
        CHECK_EQ(mid.mismatches, 0);
        CHECK_EQ(int(midLevel.run.pixels.red[0]), 200); // the mask's own level where no occluder is
        CHECK_EQ(midLevel.run.pixels.width, kWidth);
    }

    // ---- the mask this game build binds, at the shape it binds it in: an R8_UNORM mask at 3840x2160 with the picture rendered at 1920x1080
    // inside it. The format and the quadrant are judged together against the CPU reference of both shaders, because that is the combination
    // the game runs and neither the 512x288 full-window R8 case nor the classified shape test executes. Every texel of the window is compared
    // with the reference, and every texel outside the window is checked against the mask's own level over the whole allocation.
    {
        constexpr uint32_t allocationWidth = 3840, allocationHeight = 2160;
        constexpr uint32_t windowWidth = 1920, windowHeight = 1080;
        constexpr uint32_t maskLevel = 200;
        Pass gamePass;
        Scene scene;
        scene.box = true;
        // The picture is the window: the scene's depth is rendered into the top-left 1920x1080 of the 3840x2160 allocation and every texel
        // outside it is left at the cleared value (0), which is what the game's own depth buffer holds outside its render window. The march may
        // step out of the window (the shader's bounds test is the allocation's), and both it and the reference then read that same zero.
        const std::vector<float> picture = RenderDepth(projection, scene, windowWidth, windowHeight, 1);
        std::vector<float> depth(size_t(allocationWidth) * allocationHeight, 0.0f);
        for (uint32_t y = 0; y < windowHeight; ++y)
            for (uint32_t x = 0; x < windowWidth; ++x)
                depth[size_t(y) * allocationWidth + x] = picture[size_t(y) * windowWidth + x];
        const Textures textures = MakeInputs(wall.device.Get(), depth, maskLevel, DXGI_FORMAT_R8_UNORM, allocationWidth, allocationHeight);
        CHECK(textures.mask && textures.depth && textures.maskView && textures.depthView);
        const Buffers buffers = MakeBuffers(wall.device.Get(), sunLeftUp, camera);
        const Window window { 0.0f, 0.0f, float(windowWidth), float(windowHeight) };
        const Params params = BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward);
        const Run run = RunPass(gamePass, wall.device.Get(), wall.context.Get(), textures, buffers, window, params, maskLevel,
                                allocationWidth, allocationHeight, DXGI_FORMAT_R8_UNORM);
        CHECK(run.ran);
        if (run.ran)
        {
            CHECK_EQ(run.pixels.width, allocationWidth);
            CHECK_EQ(run.pixels.height, allocationHeight);
            // The window's quadrant is the picture: the contact band the box casts has to be there.
            size_t darkened = 0;
            for (uint32_t y = 0; y < windowHeight; ++y)
                for (uint32_t x = 0; x < windowWidth; ++x)
                    darkened += run.pixels.red[size_t(y) * allocationWidth + x] < maskLevel ? 1 : 0;
            std::printf("    the game's shape (3840x2160 mask, 1920x1080 window): %zu of %u window texel(s) darkened\n", darkened,
                        windowWidth * windowHeight);
            CHECK(darkened > 100);
            // Everything outside the window is the engine's own mask, over the whole allocation: the combine passes it through.
            size_t outside = 0, changedOutside = 0;
            for (uint32_t y = 0; y < allocationHeight; ++y)
                for (uint32_t x = 0; x < allocationWidth; ++x)
                {
                    if (x < windowWidth && y < windowHeight)
                        continue;
                    ++outside;
                    changedOutside += run.pixels.red[size_t(y) * allocationWidth + x] != maskLevel ? 1 : 0;
                }
            std::printf("    the game's shape: %zu texel(s) outside the window, %zu of them not the mask's own level\n", outside,
                        changedOutside);
            CHECK_EQ(changedOutside, 0);
            // The CPU reference of the march and the combine, on the same 3840x2160 depth buffer and the same window.
            Reference reference;
            reference.depths = &depth;
            reference.depthWidth = allocationWidth;
            reference.depthHeight = allocationHeight;
            reference.projection = projection;
            contact_test::Invert(projection, reference.inverseProjection);
            reference.camera = camera;
            reference.sun[0] = sunLeftUp[0];
            reference.sun[1] = sunLeftUp[1];
            reference.sun[2] = sunLeftUp[2];
            reference.space = SunSpace::View;
            reference.sign = SunSign::Toward;
            reference.params = params;
            reference.window = window;
            const Agreement agreement = CompareToReference(run.pixels, reference, maskLevel);
            std::printf("    the game's shape: max difference %d level(s) over every texel of the window, %d beyond one level\n",
                        agreement.maxDifference, agreement.mismatches);
            CHECK_EQ(agreement.mismatches, 0);
            // An R8 copy has no alpha channel to carry an old value, and the texel outside the window is the mask's own level.
            CHECK_EQ(int(run.pixels.alpha[0]), 255);
            CHECK_EQ(int(run.pixels.red[size_t(allocationHeight - 1) * allocationWidth + allocationWidth - 1]), int(maskLevel));
        }
    }

    // ---- the mask as this game build creates it: an R8_TYPELESS texture sampled through an R8_UNORM view. The typeless half is the resource the
    // pass copies and the typed half is what it renders into and binds, so the copy has to come out as the typed member of the family and the
    // picture has to be the one a plain R8_UNORM mask of the same size produces, texel for texel.
    {
        constexpr uint32_t allocationWidth = 1024, allocationHeight = 576;
        constexpr uint32_t windowWidth = 512, windowHeight = 288;
        constexpr uint32_t maskLevel = 200;
        Scene scene;
        scene.box = true;
        const std::vector<float> picture = RenderDepth(projection, scene, windowWidth, windowHeight, 1);
        std::vector<float> depth(size_t(allocationWidth) * allocationHeight, 0.0f);
        for (uint32_t y = 0; y < windowHeight; ++y)
            for (uint32_t x = 0; x < windowWidth; ++x)
                depth[size_t(y) * allocationWidth + x] = picture[size_t(y) * windowWidth + x];
        const Buffers buffers = MakeBuffers(wall.device.Get(), sunLeftUp, camera);
        const Window window { 0.0f, 0.0f, float(windowWidth), float(windowHeight) };
        const Params params = BuildParams(0.5f, 1.0f, 32, SunSpace::View, SunSign::Toward);

        // The engine's own pair: the texture is typeless, the view the draw samples is R8_UNORM.
        const Textures typeless = MakeInputs(wall.device.Get(), depth, maskLevel, DXGI_FORMAT_R8_TYPELESS, allocationWidth, allocationHeight,
                                             DXGI_FORMAT_R8_UNORM);
        CHECK(typeless.mask && typeless.depth && typeless.maskView && typeless.depthView);
        if (typeless.mask && typeless.maskView)
        {
            D3D11_TEXTURE2D_DESC textureDesc {};
            typeless.mask->GetDesc(&textureDesc);
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            typeless.maskView->GetDesc(&viewDesc);
            std::printf("    the game's mask pair: texture format %u, view format %u\n", uint32_t(textureDesc.Format),
                        uint32_t(viewDesc.Format));
            CHECK_EQ(uint32_t(textureDesc.Format), DXGI_FORMAT_R8_TYPELESS);
            CHECK_EQ(uint32_t(viewDesc.Format), DXGI_FORMAT_R8_UNORM);
        }
        Pass typelessPass;
        const Run typed = RunPass(typelessPass, wall.device.Get(), wall.context.Get(), typeless, buffers, window, params, maskLevel,
                                  allocationWidth, allocationHeight, DXGI_FORMAT_R8_UNORM);
        CHECK(typed.ran);

        // The same scene and mask through a plain R8_UNORM texture: the typeless texture may change no texel of the result.
        const Textures plain = MakeInputs(wall.device.Get(), depth, maskLevel, DXGI_FORMAT_R8_UNORM, allocationWidth, allocationHeight);
        CHECK(plain.mask && plain.maskView);
        Pass plainPass;
        const Run reference8 = RunPass(plainPass, wall.device.Get(), wall.context.Get(), plain, buffers, window, params, maskLevel,
                                       allocationWidth, allocationHeight, DXGI_FORMAT_R8_UNORM);
        CHECK(reference8.ran);

        if (typed.ran && reference8.ran)
        {
            CHECK_EQ(typed.pixels.width, allocationWidth);
            CHECK_EQ(typed.pixels.height, allocationHeight);
            CHECK_EQ(typed.pixels.red.size(), reference8.pixels.red.size());
            size_t darkened = 0, differ = 0;
            const size_t count = std::min(typed.pixels.red.size(), reference8.pixels.red.size());
            for (size_t i = 0; i < count; ++i)
            {
                darkened += typed.pixels.red[i] < maskLevel ? 1 : 0;
                differ += typed.pixels.red[i] != reference8.pixels.red[i] ? 1 : 0;
            }
            std::printf("    the R8_TYPELESS mask: %zu texel(s) darkened, %zu of %zu texel(s) differ from the plain R8_UNORM picture\n",
                        darkened, differ, count);
            CHECK(darkened > 50); // the box's contact band is in the window
            CHECK_EQ(differ, 0);

            // And against the CPU reference of both shaders, over the copy's whole allocation: the same agreement the plain R8 case asks for.
            Reference reference;
            reference.depths = &depth;
            reference.depthWidth = allocationWidth;
            reference.depthHeight = allocationHeight;
            reference.projection = projection;
            contact_test::Invert(projection, reference.inverseProjection);
            reference.camera = camera;
            reference.sun[0] = sunLeftUp[0];
            reference.sun[1] = sunLeftUp[1];
            reference.sun[2] = sunLeftUp[2];
            reference.space = SunSpace::View;
            reference.sign = SunSign::Toward;
            reference.params = params;
            reference.window = window;
            const Agreement agreement = CompareToReference(typed.pixels, reference, maskLevel);
            std::printf("    the R8_TYPELESS mask: max difference %d level(s), %d texel(s) beyond one level\n", agreement.maxDifference,
                        agreement.mismatches);
            CHECK_EQ(agreement.mismatches, 0);
            // Everything outside the window is the engine's own mask level, so the combine passed it through through a typed view of the copy.
            CHECK_EQ(int(typed.pixels.red[size_t(allocationHeight - 1) * allocationWidth + allocationWidth - 1]), int(maskLevel));
        }
    }

    // ---- the pass may not be entered twice. Its own two draws run through the game's draw hooks, so a nested entry there would swap the
    // device-context state under the outer run and double every draw - the round-5 stack overflow. Run() takes the same guard.
    {
        CHECK(pass.TryEnter());
        CHECK(!pass.TryEnter()); // a second entry while a run holds it is refused
        pass.Leave();
        CHECK(pass.TryEnter()); // and the run after it is allowed again
        pass.Leave();
        CHECK(pass.TryEnter());
        pass.Leave();
    }

    // ---- the pass's GPU time: every run that can time itself begins the timestamp pair and only that run ends it, so the number resolves
    // instead of being restarted by a run that never began it.
    {
        std::printf("    the GPU timing pair: %u begin(s), %u end(s), %s\n", pass.GpuTimingBegins(), pass.GpuTimingEnds(),
                    pass.HasGpuMilliseconds() ? "a time resolved" : "no time resolved on this adapter");
        if (pass.HasGpuMilliseconds())
            std::printf("    the pass's two draws: %.4f ms on the GPU\n", pass.GpuMilliseconds());
        CHECK(pass.GpuTimingBegins() >= 2u); // a begin on run after run, not only on the first
        CHECK(pass.GpuTimingEnds() <= pass.GpuTimingBegins()); // never an End without its Begin
        CHECK(pass.HasGpuMilliseconds()); // the fix's point: the pass's own cost lands
    }

    return Finish("contact_shadows_pass_warp");
}
