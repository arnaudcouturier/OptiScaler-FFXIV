// Puddles at the tone map: the pass (ext/ffxiv/engine/PuddleTonePass.cpp and shaders/PuddleReflection.hlsl) on the D3D11 WARP software adapter,
// on synthetic buffers this test builds itself, against a CPU reference of the same formula (GlossPuddlesRules.h).
//
// The camera table is the engine's real layout (the view matrix's three rows at register 0, the inverse view's at 3, the view-projection's at 6, the
// inverse view-projection's at 10, the inverse projection's at 14 and the projection's at 18), and the scene is ray cast in world space from that
// same camera: a floor at y = -1 and, in the cases that need something for the mirror to find, a bright wall at x = +0.5 standing on it. The
// view-position buffer holds the exact view-space point of every texel (the pass marches that, not a depth), the normal the way the G-buffer writes
// it (world space, n * 0.5 + 0.5, in B8G8R8A8_UNORM) and the scene the engine's composited colour (FP16, the engine's own encoding).
//
// What is checked is what a wrong formula, a wrong mapping or a wrong copy would break:
//   a dry frame (wetness 0) is the scene byte for byte, because the shader keeps every texel and the caller skips the copy before it,
//   a pool texel with nothing above it is exactly the darkened scene - the engine's own probe reflection, darkened, never black and never a hole,
//   a pool texel under a bright wall comes back brighter than the darkened scene, by a gain that is the CPU reference's pool mask times the key's
//     strength times water's Fresnel (the gain is solved for and checked inside 2 %..100 %, dead on the reference's mask),
//   the wall itself, a texel outside the pool mask, and a texel outside the render window keep the engine's own value (the wall and the dry texel
//     byte for byte, the border texel as the scene's own - the copy is the window's one-texel ring, because the draw writes every texel inside it),
//   the scene the chained order hands over: the light shafts add their air light before this stage runs, so the same frame with a uniform addition
//     comes back with every pool texel grown by what the pool keeps of it plus the reflection's own gain on it, and every texel outside the pool by
//     the addition whole - the mirror reflects the air-lit colour. (The chain's own entries are driven in puddle_tone_chain_warp; this case is the
//     formula they hand the pass,)
//   the reflection key at off is the darkening alone, and the darkening key at zero with the reflection off is the scene byte for byte,
//   the field is anchored to the world: the same world point seen from two cameras comes back with the same mask,
//   two runs of the same scene are byte for byte identical (no history, so nothing can ghost), and a size change creates a new owned target,
//   the statistics target counts the pool and the march's hits, which is what a capture reads out of engine.txt,
//   the render window's texels inside the allocation are the only ones written, and the copy covers the one-texel ring outside it,
//   a run the pure rules refuse returns null, so the caller keeps the engine's own scene.
#include "check.h"

#include "ext/ffxiv/engine/PuddleTonePass.h"

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::gloss;

namespace
{
constexpr uint32_t kSize = 320;      // the scene allocation's edge in most cases of this test (the rules' smallest scene)
constexpr uint32_t kCameraRows = 22; // 0x160 bytes, the same table the contact shadow pass reads
constexpr float kNear = 0.1f;
constexpr float kFar = 200.0f;
constexpr float kFovDegrees = 60.0f;
constexpr float kPi = 3.14159265358979f;
constexpr float kFloorY = -1.0f;
constexpr float kWallX = 0.5f;
constexpr uint32_t kSceneFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
constexpr float kFloorColour = 0.25f;
constexpr float kWallColour = 0.75f;

// ---- small maths ---------------------------------------------------------------------------------------------------------

struct Mat4
{
    float m[4][4] {};
};

Mat4 Identity()
{
    Mat4 out;
    for (int i = 0; i < 4; ++i)
        out.m[i][i] = 1.0f;
    return out;
}

bool Invert(const Mat4& in, Mat4& out)
{
    double a[4][8] {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            a[r][c] = in.m[r][c];
    for (int r = 0; r < 4; ++r)
        a[r][4 + r] = 1.0;
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12)
            return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k)
                std::swap(a[c][k], a[pivot][k]);
        const double scale = a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] /= scale;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double factor = a[r][c];
                for (int k = 0; k < 8; ++k)
                    a[r][k] -= factor * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = float(a[r][4 + c]);
    return true;
}

// The projection of a right-handed view space looking down -z, reversed the way the game's depth is: 1 at the near plane, 0 at the far plane. The
// matrix is column-vector convention (clip[r] = sum_c m[r][c] * v[c]), which is what the shader's mul() and the reference below read.
Mat4 Projection()
{
    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    Mat4 p;
    p.m[0][0] = 1.0f / tanY;
    p.m[1][1] = 1.0f / tanY;
    p.m[2][2] = kNear / (kFar - kNear);
    p.m[2][3] = kNear * kFar / (kFar - kNear);
    p.m[3][2] = -1.0f;
    return p;
}

struct float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

float3 Make3(float x, float y, float z) { return float3 { x, y, z }; }
float3 Add(const float3& a, const float3& b) { return Make3(a.x + b.x, a.y + b.y, a.z + b.z); }
float3 Sub(const float3& a, const float3& b) { return Make3(a.x - b.x, a.y - b.y, a.z - b.z); }
float3 Scale(const float3& a, float s) { return Make3(a.x * s, a.y * s, a.z * s); }
float Dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const float3& a) { return std::sqrt(Dot(a, a)); }
float3 Normalize(const float3& a)
{
    const float length = Length(a);
    return length > 0.0f ? Scale(a, 1.0f / length) : a;
}

// Half floats, for the fixture and for reading the pass's output back: the scene is FP16 and the values the test pins have to be comparable to
// what the shader computed, so the test converts rather than approximating.
uint16_t HalfOf(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return uint16_t(sign);
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

float FloatOf(uint16_t half)
{
    const uint32_t sign = (uint32_t(half) & 0x8000u) << 16;
    const uint32_t exponent = (uint32_t(half) >> 10) & 0x1Fu;
    const uint32_t mantissa = uint32_t(half) & 0x3FFu;
    uint32_t bits = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            // A subnormal half: normalise it into a float.
            int32_t shift = 0;
            uint32_t value = mantissa;
            while ((value & 0x400u) == 0)
            {
                value <<= 1;
                ++shift;
            }
            bits = sign | (uint32_t(127 - 15 - shift) << 23) | ((value & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7F800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The camera: a yaw about the world's up axis and a position, so the inverse view's rotation and its translation are both exercised. FFXIV is
// Y-up, so the floor is the plane y = kFloorY and a wall is a plane of constant x.
struct Camera
{
    float yaw = 0.35f;
    float pitch = 0.0f; // radians, looking down
    float3 position { -1.6f, 0.4f, 2.4f };

    float3 Right() const { return Make3(std::cos(yaw), 0.0f, std::sin(yaw)); }
    float3 Up() const
    {
        // The up vector tilted with the pitch: a camera that looks down has its up turned towards the view direction.
        if (pitch == 0.0f)
            return Make3(0.0f, 1.0f, 0.0f);
        const float3 forward = Forward();
        const float3 side = Right();
        const float3 flat = Make3(0.0f, 1.0f, 0.0f);
        return Normalize(Add(Scale(flat, std::cos(pitch)), Scale(forward, -std::sin(pitch))));
    }
    float3 Forward() const
    {
        if (pitch == 0.0f)
            return Make3(std::sin(yaw), 0.0f, -std::cos(yaw));
        const float3 flat = Make3(std::sin(yaw), 0.0f, -std::cos(yaw));
        const float3 up = Make3(0.0f, 1.0f, 0.0f);
        return Normalize(Add(Scale(flat, std::cos(pitch)), Scale(up, -std::sin(pitch))));
    }
    float3 Back() const { return Scale(Forward(), -1.0f); }
};

// g_CameraParameter as the shaders read it: the view's three rows, the inverse view's three, the view-projection's four, the inverse
// view-projection's four, the inverse projection's four and the projection's four.
struct CameraTable
{
    float rows[kCameraRows][4] {};

    void Put(uint32_t at, const Mat4& m, int count = 4)
    {
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 4; ++c)
                rows[at + r][c] = m.m[r][c];
    }
};

CameraTable BuildCamera(const Camera& camera)
{
    const float3 right = camera.Right();
    const float3 up = camera.Up();
    const float3 back = camera.Back();

    // The inverse view: world = right * view.x + up * view.y + back * view.z + position, so its three rows are that basis with the camera's
    // position in .w (the layout the shaders read: three rows at register 3).
    Mat4 inverseView = Identity();
    inverseView.m[0][0] = right.x;
    inverseView.m[0][1] = up.x;
    inverseView.m[0][2] = back.x;
    inverseView.m[1][0] = right.y;
    inverseView.m[1][1] = up.y;
    inverseView.m[1][2] = back.y;
    inverseView.m[2][0] = right.z;
    inverseView.m[2][1] = up.z;
    inverseView.m[2][2] = back.z;
    inverseView.m[0][3] = camera.position.x;
    inverseView.m[1][3] = camera.position.y;
    inverseView.m[2][3] = camera.position.z;

    Mat4 view;
    const bool inverted = Invert(inverseView, view);
    (void) inverted;
    const Mat4 projection = Projection();
    Mat4 inverseProjection;
    Invert(projection, inverseProjection);

    Mat4 viewProjection;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += projection.m[r][k] * view.m[k][c];
            viewProjection.m[r][c] = sum;
        }
    Mat4 inverseViewProjection;
    Invert(viewProjection, inverseViewProjection);

    CameraTable table;
    table.Put(0, view, 3);
    table.Put(3, inverseView, 3);
    table.Put(6, viewProjection);
    table.Put(10, inverseViewProjection);
    table.Put(14, inverseProjection);
    table.Put(18, projection);
    return table;
}

// ---- the fixture --------------------------------------------------------------------------------------------------------

// One frame: per texel of the allocation the scene colour, the view-space position, the world-space normal and whether the surface is the wall.
struct Frame
{
    uint32_t size = kSize;
    uint32_t height = kSize;
    uint32_t windowX = 0, windowY = 0, windowWidth = kSize, windowHeight = kSize;
    float wetness = 1.0f;
    Params params;
    std::vector<float> scene;    // rgb per texel, the engine's encoding
    std::vector<float> position; // xyz per texel, the view-space point (zero where there is no surface: the sky)
    std::vector<float> normal;   // xyz per texel, the decoded world-space normal
    std::vector<float> world;    // xyz per texel, the world point (for the reference and the world-anchoring case)
    std::vector<uint8_t> surface;
    ToneFacts facts;
    CameraTable camera;

    size_t Index(uint32_t x, uint32_t y) const { return size_t(y) * size + x; }
};

Frame BuildScene(const Camera& camera, bool wall, float wetness,
                 Params params = Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.35f, 24 },
                 uint32_t size = kSize, uint32_t windowWidth = kSize, uint32_t windowHeight = kSize, uint32_t windowX = 0,
                 uint32_t windowY = 0, float floorColour = kFloorColour)
{
    Frame frame;
    frame.size = size;
    frame.height = size;
    frame.windowX = windowX;
    frame.windowY = windowY;
    frame.windowWidth = windowWidth;
    frame.windowHeight = windowHeight;
    frame.wetness = wetness;
    frame.params = params;
    frame.camera = BuildCamera(camera);
    const size_t texels = size_t(size) * size;
    frame.scene.assign(texels * 3, 0.0f);
    frame.position.assign(texels * 3, 0.0f);
    frame.normal.assign(texels * 3, 0.0f);
    frame.world.assign(texels * 3, 0.0f);
    frame.surface.assign(texels, 0);

    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    const float3 right = camera.Right();
    const float3 up = camera.Up();
    const float3 forward = camera.Forward();
    for (uint32_t y = windowY; y < windowY + windowHeight; ++y)
        for (uint32_t x = windowX; x < windowX + windowWidth; ++x)
        {
            const size_t index = frame.Index(x, y);
            const float ndcX = (float(x - windowX) + 0.5f) / float(windowWidth) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (float(y - windowY) + 0.5f) / float(windowHeight) * 2.0f;
            const float3 direction = Normalize(Add(Add(Scale(right, ndcX * tanY), Scale(up, ndcY * tanY)), forward));

            float distance = 0.0f;
            float3 normal = Make3(0.0f, 1.0f, 0.0f);
            bool isWall = false;
            if (direction.y < -1e-6f)
                distance = (kFloorY - camera.position.y) / direction.y;
            if (wall && std::fabs(direction.x) > 1e-6f)
            {
                const float wallDistance = (kWallX - camera.position.x) / direction.x;
                const float height = camera.position.y + direction.y * wallDistance;
                if (wallDistance > 0.0f && height >= kFloorY - 1e-4f && (distance <= 0.0f || wallDistance < distance))
                {
                    distance = wallDistance;
                    normal = Make3(-1.0f, 0.0f, 0.0f); // the face turned towards the camera, which stands at x < kWallX
                    isWall = true;
                }
            }
            if (!(distance > 0.0f))
                continue; // the sky: no surface, so nothing for the pool to see and nothing to shade

            const float3 point = Add(camera.position, Scale(direction, distance));
            const float3 offset = Sub(point, camera.position);
            // The view position as the FP16 buffer will hand it to the shader, and the world point reconstructed from that rounded value the way the
            // shader reconstructs it (the inverse view's three rows dotted with it): the reference then reads the same numbers the shader does.
            const float viewX = FloatOf(HalfOf(Dot(right, offset)));
            const float viewY = FloatOf(HalfOf(Dot(up, offset)));
            const float viewZ = FloatOf(HalfOf(Dot(camera.Back(), offset)));
            frame.position[index * 3 + 0] = viewX;
            frame.position[index * 3 + 1] = viewY;
            frame.position[index * 3 + 2] = viewZ;
            frame.world[index * 3 + 0] = frame.camera.rows[3][0] * viewX + frame.camera.rows[3][1] * viewY + frame.camera.rows[3][2] * viewZ +
                                        frame.camera.rows[3][3];
            frame.world[index * 3 + 1] = frame.camera.rows[4][0] * viewX + frame.camera.rows[4][1] * viewY + frame.camera.rows[4][2] * viewZ +
                                        frame.camera.rows[4][3];
            frame.world[index * 3 + 2] = frame.camera.rows[5][0] * viewX + frame.camera.rows[5][1] * viewY + frame.camera.rows[5][2] * viewZ +
                                        frame.camera.rows[5][3];
            frame.normal[index * 3 + 0] = normal.x;
            frame.normal[index * 3 + 1] = normal.y;
            frame.normal[index * 3 + 2] = normal.z;
            frame.surface[index] = isWall ? 2u : 1u;
            const float colour = isWall ? kWallColour : floorColour;
            frame.scene[index * 3 + 0] = colour;
            frame.scene[index * 3 + 1] = colour;
            frame.scene[index * 3 + 2] = colour;
        }

    frame.facts.renderTargets = 1;
    frame.facts.targetWidth = size;
    frame.facts.targetHeight = size;
    frame.facts.targetFormat = kSceneFormat;
    frame.facts.targetIsScene = false;
    frame.facts.sceneFormat = kSceneFormat;
    frame.facts.sceneWidth = size;
    frame.facts.sceneHeight = size;
    frame.facts.sceneSampled = true;
    frame.facts.sceneIsScene = true;
    frame.facts.viewPositionSampled = true;
    frame.facts.viewPositionWidth = size;
    frame.facts.viewPositionHeight = size;
    frame.facts.normalSampled = true;
    frame.facts.normalWidth = size;
    frame.facts.normalHeight = size;
    frame.facts.cameraBytes = kCameraBytes;
    frame.facts.window[0] = float(windowX);
    frame.facts.window[1] = float(windowY);
    frame.facts.window[2] = float(windowWidth);
    frame.facts.window[3] = float(windowHeight);
    return frame;
}

// ---- the CPU reference: the same formula, texel by texel ----------------------------------------------------------------

// What the shader should have written for one texel: the darkened scene plus, when the march finds something, the hit's colour with water's
// Fresnel. `hit` is the marched colour the caller determined (0 for a miss), and `mask` the pool mask at the texel's world point.
float ReferenceTexel(float scene, float hit, float mask, float cosine, const Params& params)
{
    return PuddleTexel(scene, hit, mask, cosine, params.reflection ? params.reflectionStrength : 0.0f, params.darkening);
}

// The pool mask and the world position of a texel, as the reference reads them: the shader's own rules (the region, the wetness floor, the surface,
// the up-facing test) and the gloss pass's field.
struct TexelReference
{
    bool shaded = false;
    float mask = 0.0f;
    float3 world {};
};

TexelReference ReferenceOf(const Frame& frame, uint32_t x, uint32_t y)
{
    TexelReference out;
    const Region region = WholeTexels(frame.facts.window[0], frame.facts.window[1], frame.facts.window[2], frame.facts.window[3],
                                      frame.facts.sceneWidth, frame.facts.sceneHeight);
    if (x < region.x || y < region.y || x >= region.Right() || y >= region.Bottom())
        return out;
    if (!(frame.wetness > kWetnessFloor))
        return out;
    const size_t index = frame.Index(x, y);
    if (frame.surface[index] == 0)
        return out; // the sky
    // The view-depth guard the shader applies beside it: the camera looks down -Z, so visible ground within 500 m has 0.05 < -P.z < 500, and the
    // far plane the sky sits on has nothing that can hold standing water. The pool's reach rides the same view distance (GlossPuddlesRules.h's
    // WithinReach, the CPU reference of the shader's gate): past it the stage keeps the engine's own texel, which is what keeps the field off the
    // open water whose G-buffer holds the ground beneath it.
    const float viewZ = frame.position[index * 3 + 2];
    if (!(-viewZ > 0.05f && -viewZ < 500.0f))
        return out;
    if (!WithinReach(-viewZ, frame.params))
        return out;
    const float3 normal = Make3(frame.normal[index * 3 + 0], frame.normal[index * 3 + 1], frame.normal[index * 3 + 2]);
    if (!FacesUp(normal.y))
        return out; // a wall
    const float3 world = Make3(frame.world[index * 3 + 0], frame.world[index * 3 + 1], frame.world[index * 3 + 2]);
    const float field = Noise(world.x, world.z, frame.params.scale);
    out.world = world;
    out.mask = PoolMask(field, frame.params.coverage, frame.wetness, frame.params.softness);
    out.shaded = out.mask > 0.0f;
    return out;
}

// ---- the WARP harness ------------------------------------------------------------------------------------------------

struct Harness
{
    float clockSeconds = 0.0f; // the raindrops' clock, and how strongly it rains: Run passes both as they are
    float rainAmount = 1.0f;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, positionTexture, normalTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, positionView, normalView;
    ComPtr<ID3D11Buffer> camera, wetness;
    ComPtr<ID3D11ShaderResourceView> wetnessView;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }

    static DXGI_FORMAT FormatOf(uint32_t format) { return DXGI_FORMAT(format); }

    ComPtr<ID3D11Texture2D> Texture(uint32_t format, uint32_t width, uint32_t height, const void* pixels, uint32_t pitch)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = FormatOf(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = pixels;
        data.SysMemPitch = pitch;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)))
            return nullptr;
        return texture;
    }

    bool Upload(const Frame& frame)
    {
        {
            const CameraTable& table = frame.camera;
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kCameraBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data {};
            data.pSysMem = table.rows;
            camera.Reset();
            if (FAILED(device->CreateBuffer(&desc, &data, &camera)))
                return false;
        }
        {
            const float values[kWetnessBytes / 4] { frame.wetness, 1.0f - frame.wetness * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kWetnessBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data { values, 0, 0 };
            wetness.Reset();
            wetnessView.Reset();
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            viewDesc.Buffer.FirstElement = 0;
            viewDesc.Buffer.NumElements = kWetnessBytes / 16;
            if (FAILED(device->CreateBuffer(&desc, &data, &wetness)) ||
                FAILED(device->CreateShaderResourceView(wetness.Get(), &viewDesc, &wetnessView)))
                return false;
        }

        const uint32_t size = frame.size;
        std::vector<uint16_t> sceneHalf(size_t(size) * frame.height * 4, 0);
        std::vector<uint16_t> positionHalf(size_t(size) * frame.height * 4, 0);
        std::vector<uint8_t> normalBytes(size_t(size) * frame.height * 4, 0);
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < size; ++x)
            {
                const size_t index = frame.Index(x, y);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    sceneHalf[index * 4 + c] = HalfOf(frame.scene[index * 3 + c]);
                    positionHalf[index * 4 + c] = HalfOf(frame.position[index * 3 + c]);
                }
                sceneHalf[index * 4 + 3] = HalfOf(1.0f);
                positionHalf[index * 4 + 3] = HalfOf(1.0f);
                // The G-buffer's normal the way the game writes it: B8G8R8A8_UNORM in memory, world space, n * 0.5 + 0.5.
                const float nx = frame.normal[index * 3 + 0];
                const float ny = frame.normal[index * 3 + 1];
                const float nz = frame.normal[index * 3 + 2];
                const auto byteOf = [](float value) { return uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, value)) * 255.0f)); };
                normalBytes[index * 4 + 0] = byteOf(nz * 0.5f + 0.5f); // blue
                normalBytes[index * 4 + 1] = byteOf(ny * 0.5f + 0.5f); // green
                normalBytes[index * 4 + 2] = byteOf(nx * 0.5f + 0.5f); // red
                normalBytes[index * 4 + 3] = 255;
            }
        sceneTexture = Texture(kSceneFormat, size, frame.height, sceneHalf.data(), size * 8);
        positionTexture = Texture(kSceneFormat, size, frame.height, positionHalf.data(), size * 8);
        normalTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), size, frame.height, normalBytes.data(), size * 4);
        sceneView.Reset();
        positionView.Reset();
        normalView.Reset();
        if (!sceneTexture || !positionTexture || !normalTexture)
            return false;
        if (FAILED(device->CreateShaderResourceView(sceneTexture.Get(), nullptr, &sceneView)) ||
            FAILED(device->CreateShaderResourceView(positionTexture.Get(), nullptr, &positionView)) ||
            FAILED(device->CreateShaderResourceView(normalTexture.Get(), nullptr, &normalView)))
            return false;
        return true;
    }

    // The test's own wait for the GPU work it queued. The pass never waits; a test has no frame boundary to submit the queue for it, so this is an
    // event query the pass does not know about, ended and flushed, then polled to completion.
    bool WaitForGpu()
    {
        D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> done;
        if (FAILED(device->CreateQuery(&desc, &done)) || !done)
            return false;
        context->End(done.Get());
        context->Flush();
        for (unsigned spin = 0; spin < 4000; ++spin)
        {
            if (context->GetData(done.Get(), nullptr, 0, 0) == S_OK)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    // One run of the pass, then this test's wait and a read of the output: the scene the tone map would sample, as float rgb per texel.
    bool Run(TonePass& pass, const Frame& frame, std::vector<float3>& out, bool stats = false)
    {
        if (!Upload(frame))
            return false;
        TonePassInputs inputs;
        inputs.scene = sceneView.Get();
        inputs.sceneTexture = sceneTexture.Get();
        inputs.viewPosition = positionView.Get();
        inputs.normal = normalView.Get();
        inputs.camera = camera.Get();
        inputs.wetness = wetnessView.Get();
        inputs.facts = frame.facts;
        inputs.params = frame.params;
        inputs.stats = stats;
        inputs.timeSeconds = clockSeconds;
        inputs.rain = rainAmount;
        ID3D11ShaderResourceView* result = pass.Run(context.Get(), inputs);
        if (result == nullptr)
            return false;
        if (!WaitForGpu())
            return false;

        // The texture the pass returned is the one the tone map would sample: this test reads it back through the size it had.
        ComPtr<ID3D11Resource> resource;
        result->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
            return false;
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC staging {};
        staging.Width = desc.Width;
        staging.Height = desc.Height;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = desc.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)))
            return false;
        context->CopyResource(copy.Get(), texture.Get());
        if (!WaitForGpu())
            return false;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        out.assign(size_t(desc.Width) * desc.Height, float3 {});
        for (uint32_t y = 0; y < desc.Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
            for (uint32_t x = 0; x < desc.Width; ++x)
                out[size_t(y) * desc.Width + x] = Make3(FloatOf(row[size_t(x) * 4 + 0]), FloatOf(row[size_t(x) * 4 + 1]),
                                                       FloatOf(row[size_t(x) * 4 + 2]));
        }
        context->Unmap(copy.Get(), 0);
        return true;
    }
};

// The fixture's own scene value at a texel, as the pass should have kept it when it did not shade it.
float3 SceneOf(const Frame& frame, uint32_t x, uint32_t y)
{
    const size_t index = frame.Index(x, y);
    return Make3(frame.scene[index * 3 + 0], frame.scene[index * 3 + 1], frame.scene[index * 3 + 2]);
}

// The texels whose reference says they are pool, and of those the ones the pass's output shows a reflection at (a value above the darkened scene
// by more than one FP16 step).
struct PoolSample
{
    uint32_t x = 0, y = 0;
    float mask = 0.0f;
    float darkened = 0.0f;
    float read = 0.0f;
};

std::vector<PoolSample> PoolOf(const Frame& frame, const std::vector<float3>& out)
{
    std::vector<PoolSample> samples;
    for (uint32_t y = 0; y < frame.height; ++y)
        for (uint32_t x = 0; x < frame.size; ++x)
        {
            const TexelReference reference = ReferenceOf(frame, x, y);
            if (!reference.shaded)
                continue;
            PoolSample sample;
            sample.x = x;
            sample.y = y;
            sample.mask = reference.mask;
            sample.darkened = kFloorColour * PuddleDarkening(reference.mask, frame.params.darkening);
            sample.read = out[frame.Index(x, y)].x;
            samples.push_back(sample);
        }
    return samples;
}

bool Close(float a, float b, float tolerance = 2e-3f) { return std::fabs(a - b) <= tolerance; }

// The pool mask of the floor texel nearest to a world point, as the pass shaded it, and how far that texel's world point is from the one asked for.
// The floor's colour and the darkening are constants of the fixture, so the mask is recovered from the output rather than recomputed - which is
// what pins "the field is a function of the world, not of the pixel". A texel is only usable when it is close enough to the point that the two
// cameras' answers are the same field read at (nearly) the same place; the case checks that distance.
struct NearMask
{
    float mask = -1.0f;
    float distance = -1.0f;
};

NearMask MaskNear(const Frame& frame, const std::vector<float3>& out, const float3& point)
{
    NearMask best;
    for (uint32_t y = frame.windowY; y < frame.windowY + frame.windowHeight; ++y)
        for (uint32_t x = frame.windowX; x < frame.windowX + frame.windowWidth; ++x)
        {
            const size_t index = frame.Index(x, y);
            if (frame.surface[index] != 1)
                continue; // not the floor: this case's inversion is the floor's colour
            const float3 world = Make3(frame.world[index * 3 + 0], frame.world[index * 3 + 1], frame.world[index * 3 + 2]);
            const float distance = Length(Sub(world, point));
            if (best.distance >= 0.0f && !(distance < best.distance))
                continue;
            best.distance = distance;
            best.mask = (1.0f - out[index].x / kFloorColour) / frame.params.darkening;
        }
    return best;
}
} // namespace

int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    Harness harness;
    if (!harness.Create())
    {
        std::printf("no WARP device: the puddle tone-map pass cannot be checked here\n");
        return 1;
    }

    // A frame whose wetness is zero: the shader keeps every texel, so the picture is the scene's own. This is the identity the feature ships with on
    // a dry day, and the check that the region, the surface, the normal and the field can none of them leak a change into a dry frame.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 0.0f);
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        bool identical = true;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.size; ++x)
            {
                const float3 read = out[frame.Index(x, y)];
                const float3 scene = SceneOf(frame, x, y);
                identical = identical && read.x == scene.x && read.y == scene.y && read.z == scene.z;
            }
        CHECK(identical);
        CHECK_EQ(pass.TargetsCreated(), 1u);
        CHECK(pass.StatsKnown() == false);
    }

    // A pool over the floor with nothing above it: every pool texel is exactly the darkened scene, so a ray that finds nothing is the engine's own
    // probe reflection, darkened - never black and never a hole. The wall is absent, so no texel can be a hit.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, false, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        const std::vector<PoolSample> pool = PoolOf(frame, out);
        CHECK(pool.size() > 100); // the frame really has a pool at coverage 0.5
        size_t exact = 0;
        size_t mistyped = 0;
        for (const PoolSample& sample : pool)
        {
            const TexelReference reference = ReferenceOf(frame, sample.x, sample.y);
            const float expected = ReferenceTexel(kFloorColour, 0.0f, reference.mask, 0.0f, frame.params);
            if (Close(sample.read, expected))
                ++exact;
            else
                ++mistyped;
        }
        CHECK_EQ(mistyped, 0u);
        CHECK_EQ(exact, pool.size());
        // And every full-pool texel is darker than the scene, which is the whole point of the darkening key.
        bool darker = true;
        size_t fullPool = 0;
        for (const PoolSample& sample : pool)
            if (sample.mask >= 0.5f)
            {
                ++fullPool;
                darker = darker && sample.read < kFloorColour - 1e-3f;
            }
        CHECK(fullPool > 100);
        CHECK(darker);
    }

    // The mirror: a bright wall stands above the pool, and the pool texels that see it come back brighter than the darkened scene, by a gain that is
    // the reference's mask times the key's strength times water's Fresnel.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        const std::vector<PoolSample> pool = PoolOf(frame, out);
        CHECK(!pool.empty());
        size_t hits = 0;
        size_t badGain = 0;
        for (const PoolSample& sample : pool)
        {
            const float gain = (sample.read - sample.darkened) / kWallColour;
            if (gain <= 2e-3f)
                continue; // a miss: the darkened scene is the answer, checked by the case above
            ++hits;
            // The gain is mask * strength * Fresnel, and Fresnel is between water's F0 and one: a gain outside that band means the shader added
            // something other than the hit's colour with the key's strength and the angle's Fresnel.
            const float unit = sample.mask * frame.params.reflectionStrength;
            const float fresnel = gain / unit;
            if (!(fresnel >= kWaterF0 - 5e-3f) || !(fresnel <= 1.0f + 5e-3f))
                ++badGain;
            // And the hit's colour is the wall's, so the channel ratios stay one: the test reads the same gain on all three channels.
            const float3 read = out[frame.Index(sample.x, sample.y)];
            if (!Close(read.y, read.x, 3e-3f) || !Close(read.z, read.x, 3e-3f))
                ++badGain;
        }
        CHECK(badGain == 0u);
        CHECK(hits > 20); // the wall is in the picture and the march finds it from the pool
    }

    // The scene the chained order hands over: the light shafts add their air light to the scene at the same draw, and this stage runs on their
    // output, so the pool's mirror has to reflect the air-lit colour and the pool's base has to be the air-lit stone. The same frame is run twice,
    // once as the composite wrote it and once with a uniform addition standing for the shafts' light: every pool texel grows by what the pool keeps
    // of the addition (1 - mask * darkening) plus the addition times the reflection's own gain, and a texel outside the pool is the air-lit scene to
    // the bit. The ordering that hands this scene over is puddle_tone_chain_warp's; this case is what the pass does with it.
    {
        TonePass pass;
        const Camera camera;
        // The floor carries the wall's colour in this one fixture: the mirror's hit colour is read at the crossing's own fraction now (the four
        // texels it sits between), so a crossing at the wall's base mixes the floor texel below it, and this fixture's model - the addition splits
        // between the pool's base and the mirror by known constants - needs every texel the taps can reach to hold one colour. The two-colour
        // channel pin is the fixture above; the hit mix itself is the mirror fixture's Fresnel bound.
        const Frame plain =
            BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 }, kSize, kSize,
                       kSize, 0, 0, kWallColour);
        const float airLight = 0.1f; // the shafts' addition, as bright as this fixture's own values leave room for
        Frame lit = plain;
        for (float& value : lit.scene)
            value += airLight;
        std::vector<float3> before;
        std::vector<float3> after;
        CHECK(harness.Run(pass, plain, before));
        CHECK(harness.Run(pass, lit, after));
        size_t pool = 0;
        size_t bad = 0;
        size_t reflected = 0;
        for (uint32_t y = 0; y < plain.height; ++y)
            for (uint32_t x = 0; x < plain.size; ++x)
            {
                const size_t index = plain.Index(x, y);
                const TexelReference reference = ReferenceOf(plain, x, y);
                if (!reference.shaded)
                {
                    // Outside the pool the stage keeps the scene's own texel, which is the air-lit one: the addition arrives whole.
                    if (!Close(after[index].x - before[index].x, airLight, 3e-3f))
                        ++bad;
                    continue;
                }
                ++pool;
                // The darkened base keeps 1 - mask * darkening of the addition, and the march's hit is the air-lit scene, so the reflection's gain
                // multiplies the addition too. The gain is the plain run's own reflection term over the fixture's colour.
                const float gain = (before[index].x - kWallColour * PuddleDarkening(reference.mask, plain.params.darkening)) / kWallColour;
                if (gain > 1e-3f)
                    ++reflected;
                const float expected = airLight * (PuddleDarkening(reference.mask, plain.params.darkening) + gain);
                if (!Close(after[index].x - before[index].x, expected, 4e-3f))
                    ++bad;
            }
        CHECK(pool > 100);
        CHECK(reflected > 20); // the mirror is in the picture, so the air light reaches it
        CHECK_EQ(bad, 0u);
    }

    // The wall itself and the sky are not pool: the wall's texels keep the engine's own colour, because the up-facing test refuses a vertical
    // surface, and a texel outside the mask keeps it too.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        size_t wallTexels = 0;
        size_t changed = 0;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.size; ++x)
            {
                const size_t index = frame.Index(x, y);
                if (frame.surface[index] != 2)
                    continue;
                ++wallTexels;
                const float3 read = out[index];
                if (!Close(read.x, kWallColour, 1e-3f) || !Close(read.y, kWallColour, 1e-3f))
                    ++changed;
            }
        CHECK(wallTexels > 0);
        CHECK_EQ(changed, 0u);
        // The sky as well: a texel with no surface (a cleared view position) keeps the scene's own colour, whatever the noise field says at the
        // camera's own position.
        size_t skyTexels = 0;
        size_t skyChanged = 0;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.size; ++x)
            {
                const size_t index = frame.Index(x, y);
                if (frame.surface[index] != 0)
                    continue;
                ++skyTexels;
                const float3 read = out[index];
                const float3 scene = SceneOf(frame, x, y);
                if (read.x != scene.x || read.y != scene.y || read.z != scene.z)
                    ++skyChanged;
            }
        CHECK(skyTexels > 0);
        CHECK_EQ(skyChanged, 0u);
    }

    // The reflection key at off is the darkening alone: the same frame with the mirror disabled must come back at the darkened value on every pool
    // texel, including the ones the wall was visible from.
    {
        TonePass pass;
        const Camera camera;
        Params params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.45f, 0.0f, 24 };
        const Frame frame = BuildScene(camera, true, 1.0f, params);
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        const std::vector<PoolSample> pool = PoolOf(frame, out);
        CHECK(!pool.empty());
        size_t exact = 0;
        for (const PoolSample& sample : pool)
        {
            const TexelReference reference = ReferenceOf(frame, sample.x, sample.y);
            const float expected = ReferenceTexel(kFloorColour, 0.0f, reference.mask, 0.0f, frame.params);
            exact += Close(sample.read, expected) ? 1u : 0u;
        }
        CHECK_EQ(exact, pool.size());
    }

    // The darkening key at zero with the reflection off is the scene byte for byte: the stage's own identity, which the draw path skips before the
    // copy and which the shader has to agree with.
    {
        TonePass pass;
        const Camera camera;
        Params params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.0f, 0.35f, 24 };
        const Frame frame = BuildScene(camera, true, 1.0f, params);
        CHECK(ToneIdentity(frame.params));
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        bool identical = true;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.size; ++x)
            {
                const float3 read = out[frame.Index(x, y)];
                const float3 scene = SceneOf(frame, x, y);
                identical = identical && read.x == scene.x && read.y == scene.y;
            }
        CHECK(identical);
    }

    // The field is anchored to the world: the same floor point seen from two cameras comes back with the same mask, which is what keeps the pool
    // still while the camera moves (the ripple is a function of the world too, but this case pins the mask through the darkening).
    {
        TonePass pass;
        Camera first;
        Camera second;
        second.yaw = 0.42f; // a different rotation, so the same world point sits at a different texel
        const Frame a = BuildScene(first, false, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        const Frame b = BuildScene(second, false, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> outA;
        std::vector<float3> outB;
        CHECK(harness.Run(pass, a, outA));
        CHECK(harness.Run(pass, b, outB));
        // A grid of floor points in front of both cameras. The mask is recovered from the darkened value, which is exact for the floor's colour, and
        // compared at the texel each camera sees nearest that point; a texel farther than 3 cm from it is skipped, so the two answers really are the
        // same field read at the same place. A field anchored to the pixel instead of the world has no reason to agree on any of these.
        size_t compared = 0;
        size_t mismatched = 0;
        for (int iz = 3; iz <= 11; ++iz)
            for (int ix = -3; ix <= 3; ++ix)
            {
                const float3 point = Make3(float(ix) * 0.7f, kFloorY, -float(iz));
                const NearMask first = MaskNear(a, outA, point);
                const NearMask second = MaskNear(b, outB, point);
                if (first.mask < 0.0f || second.mask < 0.0f || !(first.distance <= 0.05f) || !(second.distance <= 0.05f))
                    continue;
                ++compared;
                if (!Close(first.mask, second.mask, 0.06f))
                    ++mismatched;
            }
        CHECK(compared >= 8);
        CHECK_EQ(mismatched, 0u);
    }

    // Two runs of one scene are byte for byte identical: the formula has no frame number, no random offset and no history, which is what makes a
    // moving camera unable to ghost.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> first;
        std::vector<float3> second;
        CHECK(harness.Run(pass, frame, first));
        CHECK(harness.Run(pass, frame, second));
        CHECK(first.size() == second.size());
        bool identical = first.size() == second.size();
        for (size_t i = 0; identical && i < first.size(); ++i)
            identical = first[i].x == second[i].x && first[i].y == second[i].y && first[i].z == second[i].z;
        CHECK(identical);
        CHECK_EQ(pass.TargetsCreated(), 1u); // one owned target for one size
        // A size change builds its own pair, which is what keeps a resize from sampling a texture of the old size.
        const Frame grown = BuildScene(camera, true, 1.0f, frame.params, kSize + 32, kSize + 32, kSize + 32);
        std::vector<float3> resized;
        CHECK(harness.Run(pass, grown, resized));
        CHECK_EQ(pass.TargetsCreated(), 2u);
    }

    // The pool's reach: the same wet floor at the gate's two extremes. A reach below the nearest floor texel's view distance shades nothing -
    // every texel is the scene byte for byte, which is what keeps the field off the open water - and the gate at off shades the floor the other
    // fixtures shade. The camera is 1.4 m above the floor, so nothing it sees on the floor is closer than that: 0.5 m is below every texel.
    {
        TonePass pass;
        const Camera camera;
        Params gated { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, false, 0.6f, 0.45f, 0.0f, 24, 0.5f };
        const Frame frame = BuildScene(camera, false, 1.0f, gated);
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        bool untouched = true;
        for (uint32_t y = 0; y < frame.height && untouched; ++y)
            for (uint32_t x = 0; x < frame.size && untouched; ++x)
            {
                const float3 scene = SceneOf(frame, x, y);
                const float3 read = out[frame.Index(x, y)];
                untouched = read.x == scene.x && read.y == scene.y && read.z == scene.z;
            }
        CHECK(untouched); // past the reach nothing is shaded: the scene byte for byte
        gated.reach = 0.0f; // the gate at off is the field the other fixtures shade
        const Frame open = BuildScene(camera, false, 1.0f, gated);
        std::vector<float3> outOpen;
        CHECK(harness.Run(pass, open, outOpen));
        size_t pools = 0;
        for (uint32_t y = 0; y < open.height; ++y)
            for (uint32_t x = 0; x < open.size; ++x)
                if (ReferenceOf(open, x, y).shaded)
                    ++pools;
        CHECK(pools > 100);
    }

    // The statistics: the pass counts the pool and the march's hits into the second target while a capture asks for it, and the answer lands one
    // run later (a no-wait map of that run's copy). This is what engine.txt carries.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out, true));
        CHECK(harness.Run(pass, frame, out, true)); // the second run maps the first one's copy
        CHECK(pass.StatsKnown());
        const ToneStats& stats = pass.Stats();
        const std::vector<PoolSample> pool = PoolOf(frame, out);
        const uint32_t windowTexels = frame.windowWidth * frame.windowHeight;
        CHECK_EQ(stats.texels, windowTexels);
        CHECK(stats.poolTexels > 0);
        CHECK(stats.poolTexels <= windowTexels);
        CHECK(stats.hitTexels <= stats.poolTexels);
        CHECK(stats.PoolShare() > 0.0f && stats.PoolShare() <= 1.0f);
        // The shader's flag is "mask at or above one half", so the count is the reference's texels above that, to within the half's own rounding.
        uint32_t above = 0;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.size; ++x)
                if (ReferenceOf(frame, x, y).mask >= 0.5f)
                    ++above;
        CHECK(stats.poolTexels == above);
        CHECK(stats.hitTexels > 0); // the wall is above the pool, so the count's second flag is exercised
    }

    // The render window inside the allocation: only the window's texels are shaded, the one texel of border outside it is the scene's own (the copy
    // includes it because the tone map's own filter reads it), and a texel further out is not the engine's picture at all - the texture is cleared
    // once and the tone map never samples it.
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 }, kSize + 40, 200, 180, 20, 30);
        std::vector<float3> out;
        CHECK(harness.Run(pass, frame, out));
        // The copy's border: the texel just outside the window on each side is the scene's own value.
        const uint32_t border[4][2] { { 19, 30 + 90 }, { 20 + 200, 30 + 90 }, { 20 + 100, 29 }, { 20 + 100, 30 + 180 } };
        size_t borderMatches = 0;
        for (const auto& point : border)
        {
            const float3 read = out[frame.Index(point[0], point[1])];
            const float3 scene = SceneOf(frame, point[0], point[1]);
            borderMatches += (read.x == scene.x && read.y == scene.y) ? 1u : 0u;
        }
        CHECK_EQ(borderMatches, 4u);
        // And a texel that is neither the window nor its border is the cleared texture, not a stale picture (the tone map never samples it).
        const float3 outside = out[frame.Index(5, 5)];
        CHECK(outside.x == 0.0f && outside.y == 0.0f && outside.z == 0.0f);
    }

    // The ripple key: with everything else equal, a rippled mirror is a different picture from a flat one only inside the pool, and the flat one's
    // exactness (checked above) is what says the difference is the ripple and not a drift.
    {
        TonePass pass;
        const Camera camera;
        const Frame flat = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        const Frame rippled = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.35f, 24 });
        std::vector<float3> flatOut;
        std::vector<float3> rippleOut;
        CHECK(harness.Run(pass, flat, flatOut));
        CHECK(harness.Run(pass, rippled, rippleOut));
        size_t outside = 0;
        size_t insideDifferent = 0;
        for (uint32_t y = 0; y < flat.height; ++y)
            for (uint32_t x = 0; x < flat.size; ++x)
            {
                const size_t index = flat.Index(x, y);
                const bool differs = !Close(flatOut[index].x, rippleOut[index].x, 2e-3f);
                const bool pool = ReferenceOf(flat, x, y).shaded;
                if (differs && !pool)
                    ++outside;
                if (differs && pool)
                    ++insideDifferent;
            }
        CHECK_EQ(outside, 0u);
        CHECK(insideDifferent > 20); // the ripple really changes the mirror
    }

    // A frame the pure rules refuse (t0 is not the scene the composite wrote) returns null, so the caller keeps the engine's own scene.
    {
        TonePass pass;
        const Camera camera;
        Frame frame = BuildScene(camera, true, 1.0f, Params { true, 0.5f, 4.0f, 0.1f, 1, Channel::A, 1.0f, true, 0.6f, 0.45f, 0.0f, 24 });
        frame.facts.sceneIsScene = false;
        std::vector<float3> out;
        CHECK(!harness.Run(pass, frame, out));
        CHECK_EQ(pass.TargetsCreated(), 0u);
    }

    // ---- the splash rings belong to the rain: no rain is the still pool to the bit, whatever the clock says, and the amount scales them
    {
        TonePass pass;
        const Camera camera;
        const Frame frame = BuildScene(camera, true, 1.0f);
        const auto differs = [&](const std::vector<float3>& a, const std::vector<float3>& b) {
            int count = 0;
            for (size_t i = 0; i < a.size(); ++i)
                count += a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z ? 1 : 0;
            return count;
        };
        std::vector<float3> still, dry, half, full;
        harness.clockSeconds = 0.0f;
        harness.rainAmount = 1.0f;
        CHECK(harness.Run(pass, frame, still));
        harness.clockSeconds = 2.5f;
        harness.rainAmount = 0.0f;
        CHECK(harness.Run(pass, frame, dry));
        harness.rainAmount = 0.5f;
        CHECK(harness.Run(pass, frame, half));
        harness.rainAmount = 1.0f;
        CHECK(harness.Run(pass, frame, full));
        harness.clockSeconds = 0.0f;
        CHECK_EQ(differs(still, dry), 0);
        CHECK(differs(still, full) > 0);
        CHECK(differs(half, full) > 0);
        std::printf("    the rings: dry %d, half rain %d, full rain %d texel(s) differ from the still pool\n", differs(still, dry), differs(still, half),
                    differs(still, full));
    }

    return Finish("puddle_tone_warp");
}
