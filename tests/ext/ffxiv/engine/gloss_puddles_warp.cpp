// Puddles: the pass (ext/ffxiv/engine/GlossPuddlesPass.cpp and its shader) on the D3D11 WARP software adapter, on synthetic buffers this test
// builds itself, against a CPU reference of the same formula (GlossPuddlesRules.h).
//
// The camera table is the engine's real layout (the view matrix's three rows at register 0, the inverse view's at 3, the view-projection's at 6,
// the inverse view-projection's at 10, the inverse projection's at 14 and the projection's at 18), and the scene is ray cast in world space from
// that same camera: a floor at y = -1 and a wall at x = +0.5 standing on it, with the camera yawed and offset so the inverse view's rotation and
// its translation are both on the path. The depth is written the way the game's is (reversed-Z: 1 at the near plane, 0 at the far plane), the
// normal the way the game's G-buffer writes it (world space, n * 0.5 + 0.5, in B8G8R8A8_UNORM), and the target's four channels each carry a
// pattern of the texel's own position, so a copy that landed shifted is a failure instead of a picture that looks right.
//
// What is checked is what a wrong formula, a wrong mapping or a wrong copy would break:
//   the floor's chosen channel follows the CPU reference within one 8-bit step, and the other three channels are the engine's own bits,
//   the wall (not up-facing) is byte for byte the engine's texel, and so is the sky above the floor's horizon,
//   a wetness of 0 is a pure copy of the whole target, and a coverage of 0 is one too,
//   the wetness the shader gates on is the amount the engine's own draws read: the engine's buffer at 0 with the wetness knob at 1.0 shapes the
//   full-rain picture (the module's copy of that buffer is unpatched, and gating on it is what shaped nothing in a forced-wet session), the knob
//   at 0.0 over a wet engine is a pure copy, and the engine's own amount is read back with a no-wait map for the run line,
//   a sky texel whose depth is cleared keeps the engine's texel even when its normal says "up", and a partial engine wetness scales the
//   threshold the way the reference does,
//   the region the pass draws (its viewport) and the shader's own region test are pinned as the pair that keeps the engine's texels outside the
//   render window, because the viewport makes the shader's test unobservable in a picture,
//   the channel key moves the change to the channel it names (the red channel is then the engine's own),
//   a target larger than the render window: every texel outside the window is the engine's, byte for byte,
//   the engine's half-pixel viewport origin leaves the first texel of the window alone (the region rule),
//   two runs of the same scene are byte for byte identical and reuse the one owned target while a size change creates another (a width-only
//   resize included),
//   the camera layout check passes on a real camera table and refuses a table that is not one (and the pass then returns null),
//   a run the pure rules refuse returns null, so the caller keeps the engine's own texture.
#include "check.h"

#include "ext/ffxiv/engine/GlossPuddlesPass.h"

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::gloss;

namespace
{
constexpr uint32_t kSize = 320;      // the target, the depth and the normal in most cases of this test (the rules' smallest scene)
constexpr uint32_t kCameraRows = 22; // 0x160 bytes, the same table the contact shadow pass reads
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kFovDegrees = 60.0f;
constexpr float kPi = 3.14159265358979f;
constexpr float kFloorY = -1.0f;
constexpr float kWallX = 0.5f;

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

// The inverse of a 4x4, by Gauss-Jordan with partial pivoting.
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
            {
                const double t = a[c][k];
                a[c][k] = a[pivot][k];
                a[pivot][k] = t;
            }
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

float DepthOf(float distance)
{
    const Mat4 p = Projection();
    return (-p.m[2][2] * distance + p.m[2][3]) / distance;
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
float3 Cross(const float3& a, const float3& b) { return Make3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float Length(const float3& a) { return std::sqrt(Dot(a, a)); }
float3 Normalize(const float3& a)
{
    const float length = Length(a);
    return length > 0.0f ? Scale(a, 1.0f / length) : a;
}

// The camera: a yaw about the world's up axis and a position, so the inverse view's rotation and its translation are both exercised. FFXIV is
// Y-up, so the floor is the plane y = kFloorY and a wall is a plane of constant x.
struct Camera
{
    float yaw = 0.35f;
    float3 position { -1.6f, 0.4f, 2.4f };

    float3 Right() const { return Make3(std::cos(yaw), 0.0f, std::sin(yaw)); }
    float3 Up() const { return Make3(0.0f, 1.0f, 0.0f); }
    float3 Forward() const { return Make3(std::sin(yaw), 0.0f, -std::cos(yaw)); }
    // The view space's +z: the camera looks down -z, so the basis (right, up, back) is the right-handed one the inverse view stores.
    float3 Back() const { return Make3(-std::sin(yaw), 0.0f, std::cos(yaw)); }
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

    // The view-projection and its inverse: only the layout check reads them, and it asks that they multiply to the identity.
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
    table.Put(0, view, 3); // the view matrix is three rows
    table.Put(3, inverseView, 3);
    table.Put(6, viewProjection);
    table.Put(10, inverseViewProjection);
    table.Put(14, inverseProjection);
    table.Put(18, projection);
    return table;
}

// One frame of the fixture: the buffers behind the target, and the tuning that produced them.
struct Frame
{
    std::vector<float> depth;  // size * height, reversed-Z, 0 = the far plane
    std::vector<float> normal; // size * height * 3, the decoded world-space normal
    float wetness = 1.0f;
    uint32_t size = kSize;   // the allocation's width
    uint32_t height = kSize; // and its height: a width-only resize is what the owned target's cache key has to notice
    uint32_t window = kSize; // the render window's edge, at the allocation's origin
    float windowOrigin = 0.0f;
    // A G-buffer's sky texel can carry a normal (the engine's clear, a stale write). The depth is what says there is no surface there, so this
    // writes a unit-length up-facing normal on the rows the scene left as sky.
    bool skyNormals = false;
    ReaderFacts facts;
    Params params;
};

uint8_t ByteOf(float value) { return uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, value)) * 255.0f)); }

// The engine's target's four bytes at one texel, in B8G8R8A8 memory order: a pattern of the texel's own position, so a copy that landed shifted
// or a channel that was written when it should not have been is a failure instead of a picture that looks right.
void EngineBytes(uint32_t x, uint32_t y, uint8_t* out)
{
    out[0] = uint8_t((x * 7 + y * 13) & 0xFF); // blue
    out[1] = uint8_t((x * 3 + y * 5 + 40) & 0xFF); // green
    out[2] = uint8_t((x * 11 + y * 17 + 90) & 0xFF); // red
    out[3] = uint8_t((x * 5 + y * 19 + 200) & 0xFF); // alpha
}

float EngineChannel(uint32_t x, uint32_t y, uint32_t channel)
{
    uint8_t bytes[4] {};
    EngineBytes(x, y, bytes);
    return float(bytes[channel]) / 255.0f;
}

// The scene: a floor at y = -1 and a wall at x = +0.5 standing on it, ray cast in world space from the camera above. `window` is the render
// window's edge, at `windowOrigin` inside the allocation, the way the session renders 1920x1080 inside a 3840x2160 allocation. The normal is
// quantised to the 8 bits the G-buffer gives it and decoded again, so the reference and the shader read the same numbers.
Frame BuildScene(const Camera& camera, bool wall, float wetness, uint32_t size = kSize, uint32_t window = kSize, float windowOrigin = 0.0f,
                 Params params = Params { true, 0.5f, 1.0f, 0.1f, 1, Channel::R, 1.0f }, uint32_t height = 0)
{
    Frame frame;
    frame.size = size;
    frame.height = height != 0 ? height : size;
    frame.window = window;
    frame.windowOrigin = windowOrigin;
    frame.wetness = wetness;
    frame.depth.assign(size_t(size) * frame.height, 0.0f);
    frame.normal.assign(size_t(size) * frame.height * 3, 0.0f);
    frame.params = params;

    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    const float3 right = camera.Right();
    const float3 up = camera.Up();
    const float3 forward = camera.Forward();
    for (uint32_t y = 0; y < window; ++y)
        for (uint32_t x = 0; x < window; ++x)
        {
            const size_t index = size_t(y) * size + x;
            const float ndcX = (float(x) + 0.5f) / float(window) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (float(y) + 0.5f) / float(window) * 2.0f;
            const float3 direction = Normalize(Add(Add(Scale(right, ndcX * tanY), Scale(up, ndcY * tanY)), forward));

            float distance = 0.0f;
            float3 normal = Make3(0.0f, 1.0f, 0.0f);
            // The floor: position.y + direction.y * distance = kFloorY.
            if (direction.y < -1e-6f)
                distance = (kFloorY - camera.position.y) / direction.y;
            // The wall: position.x + direction.x * distance = kWallX, above the floor and in front of whatever the floor found.
            if (wall && std::fabs(direction.x) > 1e-6f)
            {
                const float wallDistance = (kWallX - camera.position.x) / direction.x;
                const float height = camera.position.y + direction.y * wallDistance;
                if (wallDistance > 0.0f && height >= kFloorY - 1e-4f && (distance <= 0.0f || wallDistance < distance))
                {
                    distance = wallDistance;
                    normal = Make3(-1.0f, 0.0f, 0.0f); // the face turned towards the camera, which stands at x < kWallX
                }
            }
            if (!(distance > 0.0f))
                continue; // the sky above the floor's horizon: nothing there, and the pass must keep the engine's texel

            const float3 world = Add(camera.position, Scale(direction, distance));
            // The view-space point the depth belongs to: the camera's own basis, dotted with the offset from its position (the camera looks down
            // -z, so the third component is along -forward).
            const float3 offset = Sub(world, camera.position);
            const float view[3] { Dot(right, offset), Dot(up, offset), Dot(camera.Back(), offset) };
            const Mat4 projection = Projection();
            const float clipZ = projection.m[2][0] * view[0] + projection.m[2][1] * view[1] + projection.m[2][2] * view[2] + projection.m[2][3];
            const float clipW = projection.m[3][0] * view[0] + projection.m[3][1] * view[1] + projection.m[3][2] * view[2] + projection.m[3][3];
            if (!(clipW > 0.0f))
                continue; // behind the camera
            frame.depth[index] = clipZ / clipW;
            frame.normal[index * 3 + 0] = float(ByteOf(normal.x * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
            frame.normal[index * 3 + 1] = float(ByteOf(normal.y * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
            frame.normal[index * 3 + 2] = float(ByteOf(normal.z * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
        }

    frame.facts.hasTarget = true;
    frame.facts.hasDepth = true;
    frame.facts.hasNormal = true;
    frame.facts.hasCamera = true;
    frame.facts.targetFormat = kTargetFormat;
    frame.facts.targetWidth = size;
    frame.facts.targetHeight = frame.height;
    frame.facts.targetSamples = 1;
    frame.facts.depthFormat = 41; // DXGI_FORMAT_R32_FLOAT, which the fixture's depth really is
    frame.facts.depthWidth = size;
    frame.facts.depthHeight = frame.height;
    frame.facts.depthSamples = 1;
    frame.facts.normalWidth = size;
    frame.facts.normalHeight = frame.height;
    frame.facts.normalSamples = 1;
    frame.facts.cameraBytes = kCameraBytes;
    frame.facts.window[0] = windowOrigin;
    frame.facts.window[1] = windowOrigin;
    frame.facts.window[2] = float(window);
    frame.facts.window[3] = float(window);
    return frame;
}

// ---- the CPU reference: the same formula, texel by texel ---------------------------------------------------------------

struct Reference
{
    const Frame& frame;
    const CameraTable& camera;

    float3 ViewPositionOf(float centreX, float centreY, float depth) const
    {
        const float input[4] { (centreX - frame.facts.window[0]) / frame.facts.window[2] * 2.0f - 1.0f,
                               1.0f - (centreY - frame.facts.window[1]) / frame.facts.window[3] * 2.0f, depth, 1.0f };
        float point[4] {};
        for (int r = 0; r < 4; ++r)
            point[r] = camera.rows[14 + r][0] * input[0] + camera.rows[14 + r][1] * input[1] + camera.rows[14 + r][2] * input[2] +
                       camera.rows[14 + r][3] * input[3];
        if (point[3] == 0.0f)
            return Make3(0.0f, 0.0f, 0.0f);
        return Make3(point[0] / point[3], point[1] / point[3], point[2] / point[3]);
    }

    float3 WorldPositionOf(const float3& view) const
    {
        return Make3(camera.rows[3][0] * view.x + camera.rows[3][1] * view.y + camera.rows[3][2] * view.z + camera.rows[3][3],
                     camera.rows[4][0] * view.x + camera.rows[4][1] * view.y + camera.rows[4][2] * view.z + camera.rows[4][3],
                     camera.rows[5][0] * view.x + camera.rows[5][1] * view.y + camera.rows[5][2] * view.z + camera.rows[5][3]);
    }

    // The shaped value of one texel of the target, as the shader computes it: the region, the wetness floor, the surface and up-facing tests, the
    // world position, the noise, the threshold and the blend.
    float Shaped(uint32_t x, uint32_t y, uint32_t channel) const
    {
        const float engine = EngineChannel(x, y, channel);
        const Region region = RegionOf(frame.facts);
        if (x < region.x || y < region.y || x >= region.Right() || y >= region.Bottom())
            return engine;
        if (!(frame.wetness > kWetnessFloor))
            return engine;
        const size_t index = size_t(y) * frame.size + x;
        const float depth = frame.depth[index];
        if (!(depth > 0.0f))
            return engine;
        const float3 normal = Make3(frame.normal[index * 3 + 0], frame.normal[index * 3 + 1], frame.normal[index * 3 + 2]);
        const float lengthSquared = Dot(normal, normal);
        if (!(lengthSquared > kNormalLengthLow) || !(lengthSquared < kNormalLengthHigh))
            return engine;
        if (!FacesUp(normal.y))
            return engine;
        const float3 world = WorldPositionOf(ViewPositionOf(float(x) + 0.5f, float(y) + 0.5f, depth));
        const float field = Noise(world.x, world.z, frame.params.scale);
        return GlossValue(engine, PuddleFactor(field, frame.params.coverage, frame.wetness, frame.params.softness), frame.wetness,
                          frame.params.wet);
    }
};

// ---- the WARP harness ------------------------------------------------------------------------------------------------

struct Harness
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> depthTexture, normalTexture, targetTexture;
    ComPtr<ID3D11ShaderResourceView> depthView, normalView, targetView;
    ComPtr<ID3D11Buffer> camera, wetness;
    ComPtr<ID3D11Texture2D> staging;
    uint32_t stagingWidth = 0, stagingHeight = 0;
    Camera pose;
    bool garbageCamera = false;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }

    ComPtr<ID3D11Texture2D> Texture(uint32_t format, uint32_t width, uint32_t height, uint32_t bytesPerTexel, const void* pixels)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = pixels;
        data.SysMemPitch = width * bytesPerTexel;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)))
            return nullptr;
        return texture;
    }

    bool Upload(const Frame& frame, bool withWall)
    {
        (void) withWall;
        if (!camera)
        {
            const CameraTable table = BuildCamera(pose);
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kCameraBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data {};
            if (garbageCamera)
            {
                // A table that is not a camera: every row a pattern, so the one-time layout check has to refuse it.
                float rows[kCameraBytes / 4] {};
                for (uint32_t i = 0; i < kCameraBytes / 4; ++i)
                    rows[i] = float(i) * 0.125f;
                data.pSysMem = rows;
            }
            else
                data.pSysMem = table.rows;
            if (FAILED(device->CreateBuffer(&desc, &data, &camera)))
                return false;
        }
        if (!wetness)
        {
            const float values[kWetnessBytes / 4] { frame.wetness, 1.0f - frame.wetness * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kWetnessBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data { values, 0, 0 };
            if (FAILED(device->CreateBuffer(&desc, &data, &wetness)))
                return false;
        }

        const uint32_t size = frame.size;
        const uint32_t rows = frame.height;
        // The G-buffer's normal, the way the game writes it: B8G8R8A8_UNORM in memory, world space, n * 0.5 + 0.5.
        std::vector<uint8_t> normalBytes(size_t(size) * rows * 4, 0);
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < size; ++x)
            {
                const size_t index = size_t(y) * size + x;
                if (!(frame.depth[index] > 0.0f))
                {
                    if (frame.skyNormals)
                    {
                        // A sky texel whose normal is a unit vector pointing up: only the depth says there is no surface under it.
                        normalBytes[index * 4 + 0] = ByteOf(0.5f);
                        normalBytes[index * 4 + 1] = ByteOf(1.0f);
                        normalBytes[index * 4 + 2] = ByteOf(0.5f);
                        normalBytes[index * 4 + 3] = 255;
                    }
                    continue; // a cleared texel: the bytes stay 0, which decodes to something that is not a unit vector
                }
                normalBytes[index * 4 + 0] = ByteOf(frame.normal[index * 3 + 2] * 0.5f + 0.5f); // blue
                normalBytes[index * 4 + 1] = ByteOf(frame.normal[index * 3 + 1] * 0.5f + 0.5f); // green
                normalBytes[index * 4 + 2] = ByteOf(frame.normal[index * 3 + 0] * 0.5f + 0.5f); // red
                normalBytes[index * 4 + 3] = 255;
            }
        std::vector<uint8_t> targetBytes(size_t(size) * rows * 4, 0);
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < size; ++x)
                EngineBytes(x, y, &targetBytes[(size_t(y) * size + x) * 4]);

        depthTexture = Texture(uint32_t(DXGI_FORMAT_R32_FLOAT), size, rows, 4, frame.depth.data());
        normalTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), size, rows, 4, normalBytes.data());
        targetTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), size, rows, 4, targetBytes.data());
        depthView.Reset();
        normalView.Reset();
        targetView.Reset();
        if (!depthTexture || !normalTexture || !targetTexture)
            return false;
        if (FAILED(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView)) ||
            FAILED(device->CreateShaderResourceView(normalTexture.Get(), nullptr, &normalView)) ||
            FAILED(device->CreateShaderResourceView(targetTexture.Get(), nullptr, &targetView)))
            return false;
        return true;
    }

    // The test's own wait for the GPU work it queued. The pass never waits: it reads a camera copy an earlier frame made with a no-wait map, and a
    // test has no frame boundary to submit the queue for it. This is an event query the pass does not know about, ended and flushed, then polled to
    // completion - a wait the test pays so that the check's next frame is deterministic on a software adapter.
    bool WaitForGpu()
    {
        D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> done;
        if (FAILED(device->CreateQuery(&desc, &done)) || !done)
            return false;
        context->End(done.Get());
        context->Flush();
        // GetData with a null buffer only tests the query, so the wait is a bounded poll of this test's own.
        for (unsigned spin = 0; spin < 4000; ++spin)
        {
            if (context->GetData(done.Get(), nullptr, 0, 0) == S_OK)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    // One run of the pass, read back as the shaped target (four bytes a texel, B8G8R8A8 order). The camera layout check reads a copy taken on an
    // earlier frame without waiting, so the first runs of a session are the frames it waits for: the harness asks again, exactly as the draw path
    // does on the next frame, until the check has an answer. `knob` is the wetness knob's value the draw path applies after the copy (-1.0: off).
    bool Run(Pass& pass, const Frame& frame, std::vector<uint8_t>& out, bool withWall, bool quiet = false, float knob = -1.0f)
    {
        if (!Upload(frame, withWall))
            return false;
        // The engine's wetness amount for the frame, the way the draw path hands it over: one GPU-side copy, then the pass reads .x.
        if (!pass.NoteWetness(context.Get(), wetness.Get()))
            return false;
        // The wetness knob, at the point the draw path applies it: the amount the engine's own draws read is the knob's forced value while the
        // knob is armed, so the puddles gate on that and not on the unpatched copy above.
        if (knob >= 0.0f && !pass.SetWetness(context.Get(), WetnessFor(pass.EngineWetness(), knob)))
            return false;
        PassInputs inputs;
        inputs.target = targetView.Get();
        inputs.targetTexture = targetTexture.Get();
        inputs.depth = depthView.Get();
        inputs.normal = normalView.Get();
        inputs.camera = camera.Get();
        inputs.facts = frame.facts;
        inputs.params = frame.params;
        ID3D11ShaderResourceView* result = nullptr;
        for (unsigned attempt = 0; attempt < 8; ++attempt)
        {
            result = pass.Run(context.Get(), inputs);
            if (result != nullptr || !pass.CameraNotReady())
                break; // the pass ran, or the camera check has an answer (a refusal or a real failure)
            WaitForGpu();
        }
        if (result == nullptr)
        {
            if (!quiet)
            {
                std::printf("gloss_puddles_warp: the pass refused: %s\n", pass.Failure() == nullptr ? "no reason" : pass.Failure());
                std::fflush(stdout);
            }
            return false;
        }

        const uint32_t width = frame.size;
        const uint32_t height = frame.height;
        if (!staging || stagingWidth != width || stagingHeight != height)
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
                return false;
            stagingWidth = width;
            stagingHeight = height;
        }
        context->CopyResource(staging.Get(), pass.OutputTexture());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            return false;
        out.assign(size_t(width) * height * 4, 0);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(out.data() + size_t(y) * width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(width) * 4);
        context->Unmap(staging.Get(), 0);
        return true;
    }

    // A wetness of another value for a second run: the buffer is re-created, which is what a weather change looks like to the pass.
    bool SetWetness(float value)
    {
        wetness.Reset();
        const float values[kWetnessBytes / 4] { value, 1.0f - value * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = kWetnessBytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data { values, 0, 0 };
        return SUCCEEDED(device->CreateBuffer(&desc, &data, &wetness));
    }
};

// The largest and the smallest byte one channel takes over a rectangle of the shaped target.
void RegionRange(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t channel,
                 int& low, int& high)
{
    low = 255;
    high = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const int value = pixels[(size_t(y) * width + x) * 4 + channel];
            low = std::min(low, value);
            high = std::max(high, value);
        }
}

// How far the GPU's texels are from the reference's, in 8-bit steps: the worst texel and the mean.
void Compare(const std::vector<uint8_t>& pixels, const Reference& reference, uint32_t width, uint32_t channel, int& worst, double& mean)
{
    worst = 0;
    double total = 0.0;
    for (uint32_t y = 0; y < width; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const int got = pixels[(size_t(y) * width + x) * 4 + channel];
            const int want = int(ByteOf(reference.Shaped(x, y, channel)));
            worst = std::max(worst, std::abs(got - want));
            total += double(std::abs(got - want));
        }
    mean = double(width) * width > 0.0 ? total / (double(width) * double(width)) : 0.0;
}

// How many bytes of a channel differ between the shaped target and the engine's own pattern.
uint32_t Differences(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t channel, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t count = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            uint8_t engine[4] {};
            EngineBytes(x, y, engine);
            if (pixels[(size_t(y) * width + x) * 4 + channel] != engine[channel])
                ++count;
        }
    return count;
}
} // namespace

// The pass's source and its shader, for the pin below: comments and whitespace removed, so a pin is about the code and not about its formatting.
static std::string CodeOf(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        const char c = text[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    }
    return out;
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 && argv[1] != nullptr ? std::string(argv[1]) : std::string();
    if (repo.empty())
    {
        std::printf("gloss_puddles_warp: no repository path\n");
        return 2;
    }


    // ---- the region: the pass draws the region with a viewport, and the shader's own test is the belt to it
    {
        // A picture cannot separate the shader's region test from its absence: the pass clips the draw to the region, so no fragment outside it is
        // ever shaded. The two statements are pinned together, because either one alone leaves a future viewport (or a future region rule) free to
        // shape the engine's texels outside the render window - which is the whole fail-open shape of this pass.
        const std::string pass = CodeOf(repo, "ffxiv/engine/GlossPuddlesPass.cpp");
        const std::string shader = CodeOf(repo, "ffxiv/engine/shaders/GlossPuddles.hlsl");
        CHECK(pass.find("constD3D11_VIEWPORTviewport{float(region.x),float(region.y),float(region.width),float(region.height),0.0f,1.0f};") !=
              std::string::npos);
        CHECK(shader.find("if(at.x<regionLow.x||at.y<regionLow.y||at.x>=regionHigh.x||at.y>=regionHigh.y)returnengine;") !=
              std::string::npos);
    }

    Harness harness;
    if (!harness.Create())
    {
        std::printf("gloss_puddles_warp: no D3D11 WARP device\n");
        return 1;
    }
    const CameraTable camera = BuildCamera(harness.pose);
    CHECK(std::fabs(DepthOf(kNear) - 1.0f) < 1e-4f); // the fixture's depth really is reversed-Z
    CHECK(std::fabs(DepthOf(kFar) - 0.0f) < 1e-3f);

    Pass pass;
    std::vector<uint8_t> pixels;

    // ---- the floor and the wall: the chosen channel follows the reference, and nothing else moves
    {
        const Frame frame = BuildScene(harness.pose, true, 1.0f);
        CHECK(harness.Run(pass, frame, pixels, true));
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean); // red, in B8G8R8A8 memory order
        CHECK(worst <= 1);
        CHECK(mean < 0.25);
        std::printf("gloss_puddles_warp: floor and wall, red worst %d, mean %.4f\n", worst, mean);

        // The wall (the right half of the picture, where the wall stands) and the sky are the engine's own bits, in all four channels.
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            CHECK_EQ(Differences(pixels, kSize, channel, kSize / 2, 0, kSize, kSize / 2), 0u);
            // The floor really did change in the chosen channel: the pattern is not the engine's any more.
            if (channel == 2)
                CHECK(Differences(pixels, kSize, channel, 0, kSize / 2, kSize / 2, kSize) > 0);
        }
        // Only the chosen channel is this module's: the engine's other three come back untouched at every texel.
        for (uint32_t channel : { 0u, 1u, 3u })
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, kSize), 0u);
    }

    // ---- a wetness of 0 is a pure copy of the whole target
    {
        CHECK(harness.SetWetness(0.0f));
        const Frame frame = BuildScene(harness.pose, true, 0.0f);
        CHECK(harness.Run(pass, frame, pixels, true));
        for (uint32_t channel = 0; channel < 4; ++channel)
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, kSize), 0u);
    }

    // ---- the engine's own buffer at 0 with the wetness knob at 1.0: the pass gates on the amount the engine's own draws read
    {
        // A dry frame by the engine's own buffer, and the wetness knob forcing 1.0 for the material draws. This module's copy of that buffer is
        // taken before the knob patches it, so gating on the copy would shape nothing (the bug a session saw: a forced-wet plaza with no puddles).
        // The reference is the full-rain picture, which is what the knob's amount has to produce.
        CHECK(harness.SetWetness(0.0f));
        const Frame frame = BuildScene(harness.pose, true, 1.0f);
        CHECK(harness.Run(pass, frame, pixels, true, false, 1.0f));
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
        CHECK(mean < 0.25);
        CHECK(Differences(pixels, kSize, 2, 0, kSize / 2, kSize / 2, kSize) > 0);
        std::printf("gloss_puddles_warp: engine 0 with the knob at 1.0, red worst %d, mean %.4f\n", worst, mean);
    }

    // ---- the engine's own buffer wet with the knob at 0.0: the same rule the other way round, so the ground stays the engine's
    {
        // The engine amount the knob's rule is given is the CPU read, which lags a frame by design, so it is settled first: two frames with the
        // engine's buffer at 1.0 and the test's own wait make the read 1.0.
        CHECK(harness.SetWetness(1.0f));
        const Frame wet = BuildScene(harness.pose, true, 1.0f);
        CHECK(harness.Run(pass, wet, pixels, true));
        CHECK(harness.WaitForGpu());
        CHECK(harness.Run(pass, wet, pixels, true));
        CHECK(pass.EngineWetnessKnown());
        CHECK_EQ(pass.EngineWetness(), 1.0f);
        // A knob at 0.0 forces the ground dry even though the engine's buffer says 1.0: the amount the engine's own draws read is the knob's.
        CHECK(harness.Run(pass, wet, pixels, true, false, 0.0f));
        for (uint32_t channel = 0; channel < 4; ++channel)
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, kSize), 0u);
    }

    // ---- the engine's own amount reaches the CPU for the run line, and the read never waits: the staging copy of an earlier frame is mapped
    {
        CHECK(harness.SetWetness(0.5f));
        const Frame frame = BuildScene(harness.pose, true, 0.5f);
        CHECK(harness.Run(pass, frame, pixels, true)); // queues this frame's staging copy
        CHECK(harness.WaitForGpu());                   // the test's own wait, which the pass never makes
        const Frame again = BuildScene(harness.pose, true, 0.5f);
        CHECK(harness.Run(pass, again, pixels, true)); // maps the copy the wait above made safe
        CHECK(pass.EngineWetnessKnown());
        CHECK_EQ(pass.EngineWetness(), 0.5f);
    }

    // ---- a coverage of 1 at full rain: the whole up-facing ground takes the wet value, and the wall is still the engine's own texel
    {
        CHECK(harness.SetWetness(1.0f));
        // A wet value of 0 with a hard edge: every puddle texel on the floor is 0 in the red channel, which the engine's own pattern never is.
        const Frame frame =
            BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.0f, Params { true, 1.0f, 1.0f, 0.0f, 1, Channel::R, 0.0f });
        CHECK(harness.Run(pass, frame, pixels, true));
        for (uint32_t channel = 0; channel < 4; ++channel)
            CHECK_EQ(Differences(pixels, kSize, channel, kSize / 2, 0, kSize, kSize / 2), 0u); // the wall and the sky
        uint32_t zeroed = 0;
        for (uint32_t y = kSize / 2; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize / 2; ++x)
                if (pixels[(size_t(y) * kSize + x) * 4 + 2] == 0)
                    ++zeroed;
        CHECK(zeroed > (kSize / 2) * (kSize / 2) / 2); // most of the floor took the wet value
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
    }

    // ---- the channel key: the change moves to the channel it names
    {
        const Frame frame =
            BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.0f, Params { true, 0.5f, 1.0f, 0.1f, 1, Channel::G, 0.25f });
        CHECK(harness.Run(pass, frame, pixels, true));
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 1, worst, mean); // green
        CHECK(worst <= 1);
        CHECK(mean < 0.25);
        for (uint32_t channel : { 0u, 2u, 3u })
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, kSize), 0u);
    }

    // ---- the channel the card ships with: alpha, the material pack's gloss. The green case above exercises the middle of the if-chain; this one
    //      is the last branch, which is the pair a session runs with and the one a missed ray's probe fallback is sharpened through.
    {
        const Frame frame = BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.0f, Params { true, 0.5f, 1.0f, 0.1f, 1, Channel::A, 0.25f });
        CHECK(harness.Run(pass, frame, pixels, true));
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 3, worst, mean); // alpha
        CHECK(worst <= 1);
        CHECK(mean < 0.25);
        for (uint32_t channel : { 0u, 1u, 2u })
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, kSize), 0u);
    }

    // ---- a target larger than the render window: everything outside it is the engine's, byte for byte
    {
        // A 320x320 allocation with a 200x200 window in it: the engine's own geometry pass wrote only that window.
        const Frame frame = BuildScene(harness.pose, true, 1.0f, kSize, 200);
        CHECK(harness.Run(pass, frame, pixels, true));
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            CHECK_EQ(Differences(pixels, kSize, channel, 200, 0, kSize, kSize), 0u); // the columns to the right of the window
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 200, 200, kSize), 0u);   // and the rows below it
        }
        // Inside the window the formula still holds.
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
    }

    // ---- the engine's half-pixel viewport origin: the first texel of the window is not this pass's
    {
        // The window is the whole allocation but starts at 0.5, which is how the engine sets its viewport: the region rule leaves the first texel
        // row and column alone, exactly as the ambient occlusion pass does.
        const Frame frame = BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.5f);
        CHECK(harness.Run(pass, frame, pixels, true));
        const Region region = RegionOf(frame.facts);
        CHECK_EQ(region.x, 1u);
        CHECK_EQ(region.y, 1u);
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, 1, kSize), 0u); // the first column
            CHECK_EQ(Differences(pixels, kSize, channel, 0, 0, kSize, 1), 0u); // the first row
        }
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
    }

    // ---- a cleared depth is not a surface: a sky texel whose normal points up keeps the engine's texel
    {
        // The G-buffer's sky rows can carry a normal (the engine's clear, a stale write from a pass that filled the channel). The depth is what
        // says there is no surface there: a rule that only asked the normal would shape the sky, and the picture would gain puddles above the
        // horizon. The reference carries the same depth test, so the two agree only when the shader has it.
        CHECK(harness.SetWetness(1.0f));
        Frame frame = BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.0f, Params { true, 1.0f, 1.0f, 0.0f, 1, Channel::R, 0.0f });
        frame.skyNormals = true;
        CHECK(harness.Run(pass, frame, pixels, true));
        uint32_t skyTexels = 0;
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
                if (!(frame.depth[size_t(y) * kSize + x] > 0.0f))
                    ++skyTexels;
        CHECK(skyTexels > 0); // the fixture has sky to guard, or the case proves nothing
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
        std::printf("gloss_puddles_warp: %u sky texels carry an up-facing normal and keep the engine's byte\n", skyTexels);
    }

    // ---- a partial engine wetness: the threshold is the coverage scaled by the rain, not the coverage alone
    {
        // At half rain the level is `1 - coverage * wetness`, so a coverage of 0.5 wets a quarter of the field's range rather than a half. The
        // reference computes the same level from the same two numbers, so a shader that dropped the wetness term shapes a band the reference
        // leaves alone - and the picture is dry where it should be half wet.
        CHECK(harness.SetWetness(0.5f));
        const Frame frame =
            BuildScene(harness.pose, true, 0.5f, kSize, kSize, 0.0f, Params { true, 0.5f, 1.0f, 0.1f, 1, Channel::R, 0.0f });
        CHECK(harness.Run(pass, frame, pixels, true));
        const Reference reference { frame, camera };
        int worst = 255;
        double mean = 1.0;
        Compare(pixels, reference, kSize, 2, worst, mean);
        CHECK(worst <= 1);
        // And the half-rain picture is not the full-rain one: the wetness really reaches the shader.
        uint32_t shaped = 0;
        for (uint32_t y = kSize / 2; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize / 2; ++x)
                if (pixels[(size_t(y) * kSize + x) * 4 + 2] != 0)
                    ++shaped;
        CHECK(shaped > 0);
        std::printf("gloss_puddles_warp: half rain and half coverage, red worst %d, mean %.4f\n", worst, mean);
    }

    // ---- the owned target: two runs of the same scene agree, and a size change creates one more
    {
        const Frame frame = BuildScene(harness.pose, true, 1.0f);
        CHECK(harness.Run(pass, frame, pixels, true));
        const std::vector<uint8_t> first = pixels;
        const uint32_t created = pass.TargetsCreated();
        CHECK(harness.Run(pass, frame, pixels, true));
        CHECK(pixels == first); // a still scene is the same picture every frame
        CHECK_EQ(pass.TargetsCreated(), created); // and no D3D object was created for it

        // A different allocation: one new target, and the picture is still the reference's.
        const Frame bigger = BuildScene(harness.pose, true, 1.0f, 384);
        CHECK(harness.Run(pass, bigger, pixels, true));
        CHECK_EQ(pass.TargetsCreated(), created + 1);
        CHECK_EQ(pixels.size(), size_t(384) * 384 * 4);

        // A width-only resize is a new allocation too: the owned target's cache key is the pair of extents, and one that dropped the width
        // would write a taller picture into the previous frame's texture (and hand a reader a texture of the wrong shape).
        const uint32_t wide = pass.TargetsCreated();
        const Frame taller = BuildScene(harness.pose, true, 1.0f, kSize, kSize, 0.0f, Params { true, 0.5f, 1.0f, 0.1f, 1, Channel::R, 1.0f },
                                        kSize + 64);
        CHECK(harness.Run(pass, taller, pixels, true));
        CHECK_EQ(pass.TargetsCreated(), wide + 1);
        CHECK_EQ(pixels.size(), size_t(kSize) * (kSize + 64) * 4);
    }

    // ---- the camera layout check: a real camera table passes, and a table that is not one is refused (the pass then keeps the engine's target).
    // The check copies the table on one frame and reads it on a later one with a no-wait map, so the first run of a session is a frame of delay
    // (CameraNotReady, not a refusal) and a table that is not a camera is refused once the copy has landed.
    {
        CHECK(pass.CameraChecked());
        Harness broken;
        CHECK(broken.Create());
        broken.garbageCamera = true;
        Pass refused;
        std::vector<uint8_t> out;
        const Frame frame = BuildScene(broken.pose, true, 1.0f);
        CHECK(!broken.Run(refused, frame, out, true, true)); // the check has not passed, so the run returned null
        CHECK(refused.CameraRejected());
        CHECK(!refused.CameraChecked());
        CHECK(!refused.CameraNotReady()); // an answer, not a copy still in the queue
    }

    // ---- the check's first frame is a delay and not a stall: a fresh pass reports not-ready, copies its table, and answers on a later frame
    {
        Harness fresh;
        CHECK(fresh.Create());
        Pass waiting;
        const Frame frame = BuildScene(fresh.pose, true, 1.0f);
        CHECK(fresh.Upload(frame, true));
        PassInputs inputs;
        inputs.target = fresh.targetView.Get();
        inputs.targetTexture = fresh.targetTexture.Get();
        inputs.depth = fresh.depthView.Get();
        inputs.normal = fresh.normalView.Get();
        inputs.camera = fresh.camera.Get();
        inputs.facts = frame.facts;
        inputs.params = frame.params;
        CHECK(waiting.Run(fresh.context.Get(), inputs) != nullptr || waiting.CameraNotReady());
        // Either the copy landed at once (a software adapter executes it before the map) or the check waits for a later frame: both are answers
        // the caller distinguishes, and neither is a refused table.
        CHECK(!waiting.CameraRejected());
        bool checked = waiting.CameraChecked();
        for (unsigned attempt = 0; attempt < 8 && !checked; ++attempt)
        {
            fresh.WaitForGpu(); // the test's own frame boundary, which the pass never takes
            checked = waiting.Run(fresh.context.Get(), inputs) != nullptr;
        }
        CHECK(checked); // the copy landed within a few frames and the real table passed
        CHECK(!waiting.CameraRejected());
    }

    // ---- a run the pure rules refuse returns null, so the caller keeps the engine's own texture
    {
        Frame frame = BuildScene(harness.pose, true, 1.0f);
        frame.facts.targetFormat = 10; // not one of the engine's 8-bit targets
        CHECK(!harness.Run(pass, frame, pixels, true));
        frame = BuildScene(harness.pose, true, 1.0f);
        frame.facts.window[2] = 8.0f; // a preview, not the picture
        CHECK(!harness.Run(pass, frame, pixels, true));
    }

    return Finish("gloss_puddles_warp");
}
