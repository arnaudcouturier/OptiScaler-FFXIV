// Ambient occlusion: the pass (ext/ffxiv/engine/AoPass.cpp and its shaders) on the D3D11 WARP software adapter, on synthetic buffers this test
// builds itself, against a CPU reference of the same horizon search.
//
// The camera table is the engine's real layout (the view matrix's three rows at register 0, the view-projection's at 6, the inverse
// view-projection's at 10, the inverse projection's at 14 and the projection's at 18), and the scene is ray cast in view space from that same
// camera: a floor at y = -1 and, optionally, a wall at x = +0.5 standing on it. The depth is written the way the game's is (reversed-Z: 1 at
// the near plane, 0 at the far plane) and the normal the way the game's G-buffer writes it (world space, n * 0.5 + 0.5, in B8G8R8A8_UNORM).
//
// What is checked is what a wrong search, a wrong mapping or a wrong blend would break:
//   the corner where the wall meets the floor comes out darker than the open floor,
//   a flat plane gives 1.0 everywhere (it never shadows itself),
//   a strength of 0 leaves the engine's own texel at every blend, and so does a blend of 0,
//   the pass matches a CPU reference of the same search on the same buffers, texel by texel within a tolerance and in the mean,
//   the sky (a cleared depth) stays 1.0,
//   a depth allocation twice the occlusion texture's: the pass computes the quadrant the engine's own producer fills, at the depth texels that
//   quadrant stands for (the same scene rendered as its own allocation agrees with it, and a CPU reference of the same search agrees at that 2:1
//   scale, which is what pins a texel of ours to the first depth texel of its block), and every texel outside it keeps the engine's own,
//   two runs of the same scene are byte for byte the same picture, and a second run at the same size creates no new targets while a size
//   change creates one pair,
//   the timing pair is begun by every run that can time itself and ended only by the run that began it,
//   a run the pure rules refuse returns null, so the caller keeps the engine's own texture.
#include "check.h"

#include "ext/ffxiv/engine/AoPass.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::ao;

namespace
{
constexpr uint32_t kSize = 64;       // the occlusion target and, in most cases of this test, the depth and the normal too
constexpr uint32_t kValueChannel = 2; // the byte of a B8G8R8A8 texel this module's value is in (red)
constexpr uint32_t kCameraRows = 22; // 0x160 bytes, the same table the contact shadow pass reads
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kFovDegrees = 60.0f;
constexpr float kPi = 3.14159265358979f;
constexpr float kFloorY = -1.0f;
constexpr float kWallX = 0.5f;
// The projection's width over its height (2026-11-07): 1 everywhere except the case that needs the game's 16:9, where the horizontal and the
// vertical focal lengths differ and a radius cap read from the wrong one is 1.78 times off. Projection() and BuildScene() both honour it.
float g_aspect = 1.0f;

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

// The projection of a right-handed view space looking down -z, reversed the way the game's depth is: 1 at the near plane, 0 at the far plane.
Mat4 Projection()
{
    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    Mat4 p;
    p.m[0][0] = 1.0f / (tanY * g_aspect);
    p.m[1][1] = 1.0f / tanY;
    p.m[2][2] = kNear / (kFar - kNear);
    p.m[2][3] = kNear * kFar / (kFar - kNear);
    p.m[3][2] = -1.0f;
    return p;
}

// The depth the projection writes for a point `distance` in front of the camera.
float DepthOf(float distance)
{
    const Mat4 p = Projection();
    return (-p.m[2][2] * distance + p.m[2][3]) / distance;
}

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
    float v[4] {};
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
float Saturate(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

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

CameraTable BuildCamera()
{
    const Mat4 identity = Identity();
    const Mat4 projection = Projection();
    Mat4 inverseProjection;
    const bool inverted = Invert(projection, inverseProjection);
    (void) inverted;
    CameraTable table;
    table.Put(0, identity, 3); // the view matrix is three rows
    table.Put(3, identity, 3); // and so is its inverse
    table.Put(6, projection);  // the view-projection (an identity view)
    table.Put(10, inverseProjection);
    table.Put(14, inverseProjection);
    table.Put(18, projection);
    return table;
}

// One frame of the fixture: the buffers behind the occlusion target, and the tuning that produced them.
struct Frame
{
    std::vector<float> depth;  // depthWidth * depthHeight, reversed-Z, 0 = the far plane
    std::vector<float> normal; // depthWidth * depthHeight * 3, the decoded normal
    float engineValue = 1.0f;  // the value in the engine's own occlusion texel
    ReaderFacts facts;
    Params params;
};

uint8_t ByteOf(float value) { return uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, value)) * 255.0f)); }

// The engine's own occlusion texture's four bytes at one texel, in B8G8R8A8 memory order: red is the value every captured reader samples, and
// blue/green are a pattern of the texel's own position, so a copy that landed shifted is a test failure instead of a picture that looks right.
void EngineBytes(uint32_t x, uint32_t y, float value, uint8_t* out)
{
    out[0] = uint8_t(y & 0xFF);
    out[1] = uint8_t(x & 0xFF);
    out[2] = ByteOf(value);
    out[3] = 0xA5;
}

// The scene: a floor at y = -1 and, when `wall` is set, a wall at x = +0.5 standing on it, which is the corner the test judges; `picket` puts a
// row of six such walls across the picture instead, so that a texel stands near a depth discontinuity wherever it is. `size` is the depth
// allocation's edge and `window` the render window's: the picture occupies a window at the allocation's origin, the way the session renders
// 1920x1080 inside a 3840x2160 allocation, and the occlusion texture is the window's size. The normal is quantised to the 8 bits the G-buffer
// gives it and decoded again, so the reference and the shader read the same numbers.
Frame BuildScene(bool wall, float engineValue, float strength, float blend, uint32_t size = kSize, uint32_t window = kSize,
                 uint32_t occlusion = 0, bool picket = false, float floorTiltDegrees = 0.0f, bool fp16Depth = false, float sceneScale = 1.0f, float radiusScale = -1.0f)
{
    if (occlusion == 0)
        occlusion = window;
    Frame frame;
    frame.depth.assign(size_t(size) * size, 0.0f);
    frame.normal.assign(size_t(size) * size * 3, 0.0f);
    frame.engineValue = engineValue;

    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    for (uint32_t y = 0; y < window; ++y)
        for (uint32_t x = 0; x < window; ++x)
        {
            const size_t index = size_t(y) * size + x;
            const float ndcX = (float(x) + 0.5f) / float(window) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (float(y) + 0.5f) / float(window) * 2.0f;
            const float3 ray = Make3(ndcX * tanY * g_aspect, ndcY * tanY, -1.0f);

            float distance = 0.0f;
            float3 normal = Make3(0.0f, 1.0f, 0.0f);
            // The floor: ray.y * distance = kFloorY.
            if (ray.y < -1e-6f)
                distance = kFloorY * sceneScale / ray.y;
            // A wall: ray.x * distance = its own x, valid only above the floor. The picket is six of them at different distances from the axis,
            // so the picture holds a depth and a normal discontinuity about every tenth of a texel row and column.
            static const float kWallXs[] = { kWallX, 0.35f, 0.2f, -0.05f, -0.25f, -0.45f };
            const int count = picket ? int(sizeof kWallXs / sizeof kWallXs[0]) : (wall ? 1 : 0);
            for (int w = 0; w < count; ++w)
            {
                const float wallX = kWallXs[w] * sceneScale;
                if (std::fabs(ray.x) <= 1e-6f)
                    continue;
                const float wallDistance = wallX / ray.x;
                const float wallHeight = ray.y * wallDistance;
                if (wallDistance > 0.0f && wallHeight >= kFloorY * sceneScale - 1e-4f * sceneScale && (distance <= 0.0f || wallDistance < distance))
                {
                    distance = wallDistance;
                    normal = Make3(wallX > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
                }
            }
            if (distance <= 0.0f)
                continue; // the sky above the floor's horizon: nothing there, and the pass must keep 1.0
            // A normal map on flat ground (2026-11-07): the floor's STORED normal tilted about the view's z axis, the surface itself flat.
            if (floorTiltDegrees != 0.0f && normal.y > 0.5f)
            {
                const float a = floorTiltDegrees * kPi / 180.0f;
                normal = Make3(std::sin(a), std::cos(a), 0.0f);
            }

            if (fp16Depth)
            {
                // the replay lab's depth: synthesised from FP16 view positions, so a surface steps by 0 and then by one quantum
                int exponent = 0;
                const float mantissa = std::frexp(distance, &exponent);
                distance = std::ldexp(std::round(mantissa * 2048.0f) / 2048.0f, exponent);
            }
            frame.depth[index] = DepthOf(distance);
            frame.normal[index * 3 + 0] = float(ByteOf(normal.x * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
            frame.normal[index * 3 + 1] = float(ByteOf(normal.y * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
            frame.normal[index * 3 + 2] = float(ByteOf(normal.z * 0.5f + 0.5f)) / 255.0f * 2.0f - 1.0f;
        }

    frame.facts.hasOcclusion = true;
    frame.facts.hasDepth = true;
    frame.facts.hasNormal = true;
    frame.facts.hasCamera = true;
    frame.facts.occlusionFormat = kOcclusionFormat;
    frame.facts.occlusionWidth = occlusion;
    frame.facts.occlusionHeight = occlusion;
    frame.facts.depthWidth = size;
    frame.facts.depthHeight = size;
    frame.facts.depthSamples = 1;
    frame.facts.normalWidth = size;
    frame.facts.normalHeight = size;
    frame.facts.normalSamples = 1;
    frame.facts.cameraBytes = kCameraBytes;
    frame.facts.window[0] = 0.0f;
    frame.facts.window[1] = 0.0f;
    frame.facts.window[2] = float(window);
    frame.facts.window[3] = float(window);
    frame.params.on = true;
    frame.params.strength = strength;
    frame.params.blend = blend;
    frame.params.radius = 1.0f * (radiusScale > 0.0f ? radiusScale : sceneScale);
    frame.params.quality = Quality::Medium;
    return frame;
}

// ---- the CPU reference: the same search, texel by texel ----------------------------------------------------------------

struct Reference
{
    const Frame& frame;
    const CameraTable& camera;
    bool firstNeighbour = false; // test-only: the plane from the first neighbour found instead of the smaller depth step (the rule's alternative)

    float3 ViewPositionOf(float centreX, float centreY, float depth) const
    {
        const float4 input { (centreX - frame.facts.window[0]) / frame.facts.window[2] * 2.0f - 1.0f,
                             1.0f - (centreY - frame.facts.window[1]) / frame.facts.window[3] * 2.0f, depth, 1.0f };
        float4 point {};
        for (int r = 0; r < 4; ++r)
            point.v[r] = camera.rows[14 + r][0] * input.v[0] + camera.rows[14 + r][1] * input.v[1] + camera.rows[14 + r][2] * input.v[2] +
                         camera.rows[14 + r][3] * input.v[3];
        if (point.v[3] == 0.0f)
            return Make3(0.0f, 0.0f, 0.0f);
        return Scale(Make3(point.v[0], point.v[1], point.v[2]), 1.0f / point.v[3]);
    }

    bool ProjectToTexel(const float3& viewPoint, float2& centre) const
    {
        const float4 input { viewPoint.x, viewPoint.y, viewPoint.z, 1.0f };
        float4 clip {};
        for (int r = 0; r < 4; ++r)
            clip.v[r] = camera.rows[18 + r][0] * input.v[0] + camera.rows[18 + r][1] * input.v[1] + camera.rows[18 + r][2] * input.v[2] +
                        camera.rows[18 + r][3] * input.v[3];
        centre = { -1.0f, -1.0f };
        if (!(clip.v[3] > 0.0f))
            return false;
        const float ndcX = clip.v[0] / clip.v[3];
        const float ndcY = clip.v[1] / clip.v[3];
        const float projectedX = frame.facts.window[0] + (ndcX * 0.5f + 0.5f) * frame.facts.window[2];
        const float projectedY = frame.facts.window[1] + (0.5f - ndcY * 0.5f) * frame.facts.window[3];
        if (!(projectedX >= frame.facts.window[0]) || !(projectedY >= frame.facts.window[1]) ||
            !(projectedX < frame.facts.window[0] + frame.facts.window[2]) || !(projectedY < frame.facts.window[1] + frame.facts.window[3]))
            return false;
        // The depth belongs to the texel, so the position is the texel's own centre (the shader snaps it the same way).
        centre.x = std::floor(projectedX) + 0.5f;
        centre.y = std::floor(projectedY) + 0.5f;
        return true;
    }

    float DepthAt(float centreX, float centreY) const
    {
        const int x = int(std::floor(centreX));
        const int y = int(std::floor(centreY));
        if (x < 0 || y < 0 || x >= int(frame.facts.depthWidth) || y >= int(frame.facts.depthHeight))
            return 0.0f;
        return frame.depth[size_t(y) * size_t(frame.facts.depthWidth) + size_t(x)];
    }

    float3 NormalAt(int x, int y) const
    {
        const size_t index = size_t(y) * size_t(frame.facts.depthWidth) + size_t(x);
        return Make3(frame.normal[index * 3 + 0], frame.normal[index * 3 + 1], frame.normal[index * 3 + 2]);
    }

    // The surface's own normal from the depth (Ao.hlsl GeometricNormal): on each axis the neighbour with the smaller depth step (the same
    // surface at a silhouette, the next row of a floor seen at a grazing angle); false when an axis has no neighbour with a depth.
    bool GeometricNormal(int x, int y, const float3& P, float3& N) const
    {
        float3 axes[2] {};
        const float floorStep = 0.002f * std::fabs(P.z);
        for (int axis = 0; axis < 2; ++axis)
        {
            float3 neighbour[2] {};
            bool has[2] { false, false };
            for (int s = 0; s < 2; ++s)
            {
                const int side = s == 0 ? 1 : -1;
                const int nx = x + (axis == 0 ? side : 0);
                const int ny = y + (axis == 1 ? side : 0);
                if (nx < 0 || ny < 0 || nx >= int(frame.facts.depthWidth) || ny >= int(frame.facts.depthHeight))
                    continue;
                const float d = DepthAt(float(nx) + 0.5f, float(ny) + 0.5f);
                if (!(d > 0.0f))
                    continue;
                neighbour[s] = ViewPositionOf(float(nx) + 0.5f, float(ny) + 0.5f, d);
                has[s] = true;
            }
            if (firstNeighbour && has[0])
                axes[axis] = Sub(neighbour[0], P); // the alternative the test measures against: always the first side found
            else if (has[0] && has[1])
            {
                const float stepPlus = std::fabs(neighbour[0].z - P.z);
                const float stepMinus = std::fabs(neighbour[1].z - P.z);
                if (std::fabs(stepPlus - stepMinus) <= std::fmax(0.5f * std::fmax(stepPlus, stepMinus), floorStep))
                    axes[axis] = Sub(neighbour[0], neighbour[1]);
                else
                    axes[axis] = stepPlus < stepMinus ? Sub(neighbour[0], P) : Sub(P, neighbour[1]);
            }
            else if (has[0])
                axes[axis] = Sub(neighbour[0], P);
            else if (has[1])
                axes[axis] = Sub(P, neighbour[1]);
            else
                return false;
        }
        N = Cross(axes[0], axes[1]);
        const float lengthN = Length(N);
        if (!(lengthN > 1e-3f * Length(axes[0]) * Length(axes[1])))
            return false;
        N = Scale(N, 1.0f / lengthN);
        return true;
    }

    // One slice of the search: the highest cosine any of its taps reached above the pixel's tangent plane.
    float Slice(const float3& P, const float3& N, const float3& direction, float radius, int steps) const
    {
        static const float kGrowth = 1.45f;
        const float growth = std::pow(kGrowth, float(steps)) - 1.0f;
        const float base = radius * (kGrowth - 1.0f) / std::fmax(growth, 1e-6f);
        float horizon = 0.0f;
        float t = 0.0f;
        for (int k = 0; k < steps; ++k)
        {
            t += base * std::pow(kGrowth, float(k));
            const float3 S = Add(P, Scale(direction, t));
            float2 centre {};
            if (!ProjectToTexel(S, centre))
                break;
            const float tapDepth = DepthAt(centre.x, centre.y);
            if (!(tapDepth > 0.0f))
                continue;
            const float3 Q = ViewPositionOf(centre.x, centre.y, tapDepth);
            const float3 D = Sub(Q, P);
            const float distance3d = Length(D);
            if (!(distance3d > 1e-5f) || !(distance3d <= radius))
                continue;
            const float inFront = (-S.z) - (-Q.z);
            if (!(inFront > 0.0f) || !(inFront <= radius))
                continue;
            horizon = std::fmax(horizon, Dot(Scale(D, 1.0f / distance3d), N));
        }
        return horizon;
    }

    // The raw occlusion the first draw writes, over the region the pass computes (AoRules.h's RegionOf): the depth texel a texel of ours stands
    // for is its block's first one, the same rule the shader follows.
    std::vector<float> Raw() const
    {
        const uint32_t width = frame.facts.occlusionWidth;
        const uint32_t height = frame.facts.occlusionHeight;
        const Mapping mapping = MappingOf(frame.facts);
        const Region region = RegionOf(frame.facts);
        std::vector<float> raw(size_t(width) * height, 1.0f);
        const int steps = int(Steps(frame.params.quality));
        const float radius = frame.params.radius;
        for (uint32_t y = region.y; y < region.Bottom(); ++y)
            for (uint32_t x = region.x; x < region.Right(); ++x)
            {
                const int atX = int(std::floor(float(x) * mapping.scaleX));
                const int atY = int(std::floor(float(y) * mapping.scaleY));
                const float centreX = float(atX) + 0.5f;
                const float centreY = float(atY) + 0.5f;
                const float depth = DepthAt(centreX, centreY);
                if (!(depth > 0.0f))
                    continue;
                const float3 P = ViewPositionOf(centreX, centreY, depth);
                float3 N = Make3(0.0f, 0.0f, 0.0f);
                if (!GeometricNormal(atX, atY, P, N))
                {
                    N = NormalAt(atX, atY);
                    if (!(Dot(N, N) > 1e-6f))
                        continue;
                }
                N = Normalize(N);
                if (Dot(N, P) > 0.0f)
                    N = Scale(N, -1.0f);

                const float3 helper = std::fabs(N.z) < 0.9f ? Make3(0.0f, 0.0f, 1.0f) : Make3(1.0f, 0.0f, 0.0f);
                const float3 B1 = Normalize(Cross(N, helper));
                const float3 B2 = Cross(N, B1);
                // The rotation is a function of the texel of this target, as the shader's is.
                const float phi = (float((x & 3) + (y & 3) * 4) + 0.5f) / 16.0f * (kPi * 0.5f);
                const float3 direction0 = Add(Scale(B1, std::cos(phi)), Scale(B2, std::sin(phi)));
                const float3 direction1 = Add(Scale(B1, -std::sin(phi)), Scale(B2, std::cos(phi)));

                // the radius capped on the screen at a quarter of the window's height (Ao.hlsl kMaxRadiusWindowFraction)
                const float p11 = camera.rows[19][1];
                const float texelMetres = p11 > 0.0f ? 2.0f * std::fabs(P.z) / (p11 * frame.facts.window[3]) : 0.0f;
                const float capped = texelMetres > 0.0f ? std::fmin(radius, 0.25f * frame.facts.window[3] * texelMetres) : radius;
                float occlusion = 0.0f;
                occlusion += Saturate(Slice(P, N, direction0, capped, steps) - kHorizonBias) * 0.5f;
                occlusion += Saturate(Slice(P, N, direction1, capped, steps) - kHorizonBias) * 0.5f;
                raw[size_t(y) * width + x] = Saturate(1.0f - occlusion);
            }
        return raw;
    }

    // The encoded texel the second draw writes: the 4x4 denoise over the region, the strength, and the product of the two visibility factors
    // with the engine's value, which the blend takes part of the way.
    std::vector<float> Final() const
    {
        const uint32_t width = frame.facts.occlusionWidth;
        const uint32_t height = frame.facts.occlusionHeight;
        const Region region = RegionOf(frame.facts);
        const std::vector<float> raw = Raw();
        std::vector<float> out(size_t(width) * height, frame.engineValue);
        for (uint32_t y = region.y; y < region.Bottom(); ++y)
            for (uint32_t x = region.x; x < region.Right(); ++x)
            {
                float sum = 0.0f;
                float taps = 0.0f;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx)
                    {
                        const int tx = int(x) + dx - 1;
                        const int ty = int(y) + dy - 1;
                        if (tx < int(region.x) || ty < int(region.y) || tx >= int(region.Right()) || ty >= int(region.Bottom()))
                            continue;
                        sum += raw[size_t(ty) * width + tx];
                        taps += 1.0f;
                    }
                const float blurred = taps > 0.0f ? sum / taps : 1.0f;
                const float ours = Saturate(1.0f - frame.params.strength * (1.0f - blurred));
                const float engine = frame.engineValue;
                out[size_t(y) * width + x] = Saturate(engine * (1.0f + (ours - 1.0f) * frame.params.blend));
            }
        return out;
    }
};

// ---- the WARP harness ------------------------------------------------------------------------------------------------

struct Harness
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> depthTexture, normalTexture, occlusionTexture;
    ComPtr<ID3D11ShaderResourceView> depthView, normalView, occlusionView;
    ComPtr<ID3D11Buffer> camera;
    ComPtr<ID3D11Texture2D> staging;
    uint32_t stagingWidth = 0, stagingHeight = 0;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                             &context);
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

    bool Upload(const Frame& frame)
    {
        if (!camera)
        {
            const CameraTable table = BuildCamera();
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kCameraBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data {};
            data.pSysMem = table.rows;
            if (FAILED(device->CreateBuffer(&desc, &data, &camera)))
                return false;
        }

        // The G-buffer's normal, the way the game writes it: B8G8R8A8_UNORM in memory, world space, n * 0.5 + 0.5. It is the depth
        // allocation's size, which is not necessarily the occlusion texture's.
        const uint32_t depthWidth = frame.facts.depthWidth;
        const uint32_t depthHeight = frame.facts.depthHeight;
        std::vector<uint8_t> normalBytes(size_t(depthWidth) * depthHeight * 4, 0);
        for (uint32_t y = 0; y < depthHeight; ++y)
            for (uint32_t x = 0; x < depthWidth; ++x)
            {
                const size_t index = size_t(y) * depthWidth + x;
                if (!(frame.depth[index] > 0.0f))
                    continue; // a cleared texel: the bytes stay 0, which decodes to something that is not a unit vector
                normalBytes[index * 4 + 0] = ByteOf(frame.normal[index * 3 + 2] * 0.5f + 0.5f); // blue
                normalBytes[index * 4 + 1] = ByteOf(frame.normal[index * 3 + 1] * 0.5f + 0.5f); // green
                normalBytes[index * 4 + 2] = ByteOf(frame.normal[index * 3 + 0] * 0.5f + 0.5f); // red
                normalBytes[index * 4 + 3] = 255;
            }

        // The engine's occlusion texture at the size the facts name, with the pattern EngineBytes writes.
        const uint32_t occlusionWidth = frame.facts.occlusionWidth;
        const uint32_t occlusionHeight = frame.facts.occlusionHeight;
        std::vector<uint8_t> occlusionBytes(size_t(occlusionWidth) * occlusionHeight * 4, 0);
        for (uint32_t y = 0; y < occlusionHeight; ++y)
            for (uint32_t x = 0; x < occlusionWidth; ++x)
                EngineBytes(x, y, frame.engineValue, &occlusionBytes[(size_t(y) * occlusionWidth + x) * 4]);

        depthTexture = Texture(uint32_t(DXGI_FORMAT_R32_FLOAT), depthWidth, depthHeight, 4, frame.depth.data());
        normalTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), depthWidth, depthHeight, 4, normalBytes.data());
        occlusionTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), occlusionWidth, occlusionHeight, 4, occlusionBytes.data());
        depthView.Reset();
        normalView.Reset();
        occlusionView.Reset();
        if (!depthTexture || !normalTexture || !occlusionTexture)
            return false;
        if (FAILED(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView)) ||
            FAILED(device->CreateShaderResourceView(normalTexture.Get(), nullptr, &normalView)) ||
            FAILED(device->CreateShaderResourceView(occlusionTexture.Get(), nullptr, &occlusionView)))
            return false;
        return true;
    }

    // One run of the pass, read back as the encoded texels (four bytes a texel, B8G8R8A8 order).
    bool Run(Pass& pass, const Frame& frame, std::vector<uint8_t>& out, uint32_t width, uint32_t height)
    {
        if (!Upload(frame))
            return false;
        PassInputs inputs;
        inputs.occlusion = occlusionView.Get();
        inputs.occlusionTexture = occlusionTexture.Get();
        inputs.depth = depthView.Get();
        inputs.normal = normalView.Get();
        inputs.camera = camera.Get();
        inputs.facts = frame.facts;
        inputs.params = frame.params;
        if (pass.Run(context.Get(), inputs) == nullptr)
            return false;

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
};

// The darkest and the brightest texel of a region of the encoded target. `width` is the target's own width in texels, `channel` the byte of a
// texel this module's value is in.
void RegionRange(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t channel,
                 float& low, float& high)
{
    low = 1.0f;
    high = 0.0f;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const float value = float(pixels[(size_t(y) * width + x) * 4 + channel]) / 255.0f;
            low = std::fmin(low, value);
            high = std::fmax(high, value);
        }
}

// How far the GPU's texels are from the reference's: the worst texel, and the mean of the absolute differences.
void Compare(const std::vector<uint8_t>& pixels, const std::vector<float>& reference, uint32_t channel, float& worst, float& mean)
{
    worst = 0.0f;
    double total = 0.0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        const float got = float(pixels[i * 4 + channel]) / 255.0f;
        const float difference = std::fabs(got - reference[i]);
        worst = std::fmax(worst, difference);
        total += double(difference);
    }
    mean = reference.empty() ? 1.0f : float(total / double(reference.size()));
}

// The same comparison over one region of the target, for the case whose texels outside the region are the engine's own and not this module's
// answer.
void CompareRegion(const std::vector<uint8_t>& pixels, const std::vector<float>& reference, uint32_t width, const Region& region,
                   uint32_t channel, float& worst, float& mean)
{
    worst = 0.0f;
    double total = 0.0;
    uint32_t count = 0;
    for (uint32_t y = region.y; y < region.Bottom(); ++y)
        for (uint32_t x = region.x; x < region.Right(); ++x)
        {
            const size_t index = size_t(y) * width + x;
            const float got = float(pixels[index * 4 + channel]) / 255.0f;
            const float difference = std::fabs(got - reference[index]);
            worst = std::fmax(worst, difference);
            total += double(difference);
            ++count;
        }
    mean = count == 0 ? 1.0f : float(total / double(count));
}
} // namespace

int main(int argc, char** argv)
{
    Harness harness;
    if (!harness.Create())
    {
        std::printf("ao_warp: no D3D11 WARP device\n");
        return 1;
    }
    const CameraTable camera = BuildCamera();
    CHECK(std::fabs(DepthOf(kNear) - 1.0f) < 1e-4f); // the fixture's depth really is reversed-Z
    CHECK(std::fabs(DepthOf(kFar) - 0.0f) < 1e-3f);

    Pass pass;
    std::vector<uint8_t> pixels;

    // ---- a flat plane: nothing shadows itself, and the pass's own answer is 1.0 everywhere
    {
        const Frame flat = BuildScene(false, 1.0f, 1.0f, 1.0f);
        CHECK(harness.Run(pass, flat, pixels, kSize, kSize));
        float low = 1.0f, high = 0.0f;
        RegionRange(pixels, kSize, 0, kSize / 2, kSize, kSize, kValueChannel, low, high);
        CHECK(low > 0.98f); // a floor alone sees open sky in every direction it searches
        CHECK(high <= 1.0f);

        const Reference reference { flat, camera };
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, reference.Final(), kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: flat plane, worst %.4f, mean %.5f\n", double(worst), double(mean));
    }

    // ---- a flat floor whose STORED normal is tilted 15 degrees (2026-11-07): the materials' normal maps tilt the G-buffer normal a median 5
    // (a room) to 13 (paving) degrees from the real surface on flat ground, and a search plane taken from it let the real surface rise above
    // the plane: 11-19 % of occlusion on flat ground in the user's frames. The plane is the depth's own surface, so flat ground stays open.
    {
        const Frame tilted = BuildScene(false, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 15.0f);
        CHECK(harness.Run(pass, tilted, pixels, kSize, kSize));
        float low = 1.0f, high = 0.0f;
        RegionRange(pixels, kSize, 0, kSize / 2, kSize, kSize, kValueChannel, low, high);
        CHECK(low > 0.98f); // rc6 (the G-buffer normal's plane) gives about 0.87 here
        const Reference reference { tilted, camera };
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, reference.Final(), kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: flat floor with a 15-degree normal map, lowest %.4f, worst %.4f, mean %.5f\n", double(low), double(worst), double(mean));
    }

    // ---- a quantised depth (FP16 steps, the replay lab's own; 2026-11-07): one surface's neighbours step by 0 and then by a quantum, and a rule
    // that took the zero step tipped the plane toward the camera (no occlusion anywhere). Both neighbours agree there, so the plane is the
    // central difference: the tilted floor stays open and the corner still darkens.
    {
        const Frame tiltedQ = BuildScene(false, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 15.0f, true);
        CHECK(harness.Run(pass, tiltedQ, pixels, kSize, kSize));
        float low = 1.0f, high = 0.0f;
        RegionRange(pixels, kSize, 0, kSize / 2, kSize, kSize, kValueChannel, low, high);
        CHECK(low > 0.98f);
        const Frame cornerQ = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 0.0f, true);
        CHECK(harness.Run(pass, cornerQ, pixels, kSize, kSize));
        float darkest = 1.0f, brightest = 0.0f, openLow = 1.0f, openHigh = 0.0f;
        RegionRange(pixels, kSize, 0, kSize / 2, kSize, kSize, kValueChannel, darkest, brightest);
        RegionRange(pixels, kSize, 0, kSize * 3 / 4, kSize / 8, kSize, kValueChannel, openLow, openHigh);
        CHECK(openLow > 0.95f);
        CHECK(darkest < openLow - 0.05f);
        const Reference reference { cornerQ, camera };
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, reference.Final(), kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: FP16-quantised depth, tilted floor lowest %.4f, corner darkest %.4f against open %.4f, worst %.4f\n", double(low),
                    double(darkest), double(openLow), double(worst));
    }

    // ---- the same corner a tenth the size, with a tenth the radius (2026-11-07): the picture must not change. The plane's normal is the cross
    // product of two one-texel differences, whose length is the square of a texel's size; a length test written for unit normals refused every
    // texel nearer than tens of metres in the game (no occlusion near the camera at all). The scaled scene's texels are millimetres wide.
    {
        const Frame corner = BuildScene(true, 1.0f, 1.0f, 1.0f);
        std::vector<uint8_t> unscaled;
        CHECK(harness.Run(pass, corner, unscaled, kSize, kSize));
        const Frame tenth = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 0.0f, false, 0.1f);
        CHECK(harness.Run(pass, tenth, pixels, kSize, kSize));
        float darkUnscaled = 1.0f, darkSmall = 1.0f, sum = 0.0f;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
        {
            const float u = float(unscaled[i * 4 + kValueChannel]) / 255.0f;
            const float v = float(pixels[i * 4 + kValueChannel]) / 255.0f;
            darkUnscaled = std::fmin(darkUnscaled, u);
            darkSmall = std::fmin(darkSmall, v);
            sum += std::fabs(u - v);
        }
        CHECK(darkUnscaled < 0.9f);
        CHECK(std::fabs(darkSmall - darkUnscaled) < 0.02f);
        CHECK(sum / float(kSize * kSize) < 0.01f);
        std::printf("ao_warp: a tenth the size, darkest %.4f against %.4f, mean difference %.5f\n", double(darkSmall), double(darkUnscaled),
                    double(sum / float(kSize * kSize)));
    }

    // ---- a close-up: the tenth-size corner with the FULL 1 m radius (2026-11-07). On the screen the radius is capped at a quarter of the window,
    // so the picture is the same as with the tenth radius; without the cap the search spans the screen and darkens everything behind the nearest
    // object (a cutscene close-up in the benchmark).
    {
        const Frame tenthRadius = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 0.0f, false, 0.1f, 0.1f);
        std::vector<uint8_t> expected;
        CHECK(harness.Run(pass, tenthRadius, expected, kSize, kSize));
        const Frame fullRadius = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 0.0f, false, 0.1f, 1.0f);
        CHECK(harness.Run(pass, fullRadius, pixels, kSize, kSize));
        float sum = 0.0f, darkest = 1.0f;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
        {
            const float u = float(expected[i * 4 + kValueChannel]) / 255.0f;
            const float v = float(pixels[i * 4 + kValueChannel]) / 255.0f;
            sum += std::fabs(u - v);
            darkest = std::fmin(darkest, v);
        }
        CHECK(sum / float(kSize * kSize) < 0.01f);
        const Reference reference { fullRadius, camera };
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, reference.Final(), kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: close-up with the full radius, darkest %.4f, mean difference to the tenth radius %.5f%s", double(darkest),
                    double(sum / float(kSize * kSize)), "\n");
    }

    // ---- the same close-up through a 16:9 projection (2026-11-07): the cap is a share of the window's HEIGHT, so a texel's height in metres
    // comes from the vertical focal length (projection[1][1]); the horizontal one is 1.78 times smaller in the game and would make the cap
    // 1.78 times larger. The pass must match the reference, which reads [1][1].
    {
        g_aspect = 16.0f / 9.0f;
        harness.camera.Reset(); // the camera buffer is built once per projection
        const CameraTable wide = BuildCamera();
        CHECK(std::fabs(wide.rows[18][0] * g_aspect - wide.rows[19][1]) < 1e-4f); // the fixture really is anisotropic
        const Frame fullRadius = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, false, 0.0f, false, 0.1f, 1.0f);
        CHECK(harness.Run(pass, fullRadius, pixels, kSize, kSize));
        const Reference reference { fullRadius, wide };
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, reference.Final(), kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: close-up through a 16:9 projection, worst %.4f, mean %.5f\n", double(worst), double(mean));
        g_aspect = 1.0f;
        harness.camera.Reset();
    }

    // ---- the plane's neighbour is the one with the smaller depth step (2026-11-07): at a wall's edge the floor texel beside it has the wall on
    // one side and the floor on the other, and a plane through the wall tilts the search. Where the two rules give different answers, the pass
    // follows the smaller step.
    {
        const Frame picket = BuildScene(true, 1.0f, 1.0f, 1.0f, kSize, kSize, 0, true);
        CHECK(harness.Run(pass, picket, pixels, kSize, kSize));
        const std::vector<float> smaller = Reference { picket, camera }.Final();
        const std::vector<float> first = Reference { picket, camera, true }.Final();
        int differing = 0;
        float errSmaller = 0.0f, errFirst = 0.0f;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
        {
            if (std::fabs(smaller[i] - first[i]) <= 0.03f)
                continue;
            ++differing;
            const float got = float(pixels[i * 4 + kValueChannel]) / 255.0f;
            errSmaller += std::fabs(got - smaller[i]);
            errFirst += std::fabs(got - first[i]);
        }
        CHECK(differing >= 8); // the scene really has texels where the choice matters
        CHECK(differing > 0 && errSmaller / float(differing) < 0.015f);
        CHECK(errSmaller < errFirst);
        std::printf("ao_warp: neighbour rule, %d texels where it matters, mean error %.4f (smaller step) against %.4f (first neighbour)\n", differing,
                    double(differing ? errSmaller / float(differing) : 0.0f), double(differing ? errFirst / float(differing) : 0.0f));
    }

    // ---- the corner: the wall darkens what is beside it
    {
        const Frame corner = BuildScene(true, 1.0f, 1.0f, 1.0f);
        CHECK(harness.Run(pass, corner, pixels, kSize, kSize));
        float darkest = 1.0f, brightest = 0.0f;
        RegionRange(pixels, kSize, 0, kSize / 2, kSize, kSize, kValueChannel, darkest, brightest);
        float openLow = 1.0f, openHigh = 0.0f;
        RegionRange(pixels, kSize, 0, kSize * 3 / 4, kSize / 8, kSize, kValueChannel, openLow, openHigh);
        CHECK(openLow > 0.95f);            // the far left of the floor is open
        CHECK(darkest < openLow - 0.05f);  // and something the wall is next to is darker than it
        CHECK(brightest <= 1.0f);

        const Reference reference { corner, camera };
        const std::vector<float> expected = reference.Final();
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, expected, kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ao_warp: corner, darkest %.4f against open %.4f, worst %.4f, mean %.5f\n", double(darkest), double(openLow),
                    double(worst), double(mean));

        // The tangent plane's bias, at the texel the search darkens most: the reference subtracts it from every tap's horizon (rule 5), so a
        // shader that dropped it comes out above the reference by the bias's own weight (0.02 of a level) - several 8-bit steps, where the
        // two agree to the encoding's own rounding. The whole-picture bound above is too wide to see a term that small.
        uint32_t at = 0;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
            if (float(pixels[i * 4 + kValueChannel]) / 255.0f < float(pixels[at * 4 + kValueChannel]) / 255.0f)
                at = i;
        const float biased = float(pixels[at * 4 + kValueChannel]) / 255.0f;
        const float unbiased = expected[at];
        CHECK(std::fabs(biased - unbiased) < 0.006f);
        std::printf("ao_warp: the darkest texel %u,%u at %.4f against the reference's %.4f\n", at % kSize, at / kSize, double(biased),
                    double(unbiased));

        // A stored normal that faces away from the viewer is turned back before the search (rule 2), and the whole corner comes out the same: a
        // normal pointing away from P would otherwise search the half-space on the far side of the surface, where every tap the search accepts
        // is below the plane that normal names - the picture loses its occlusion rather than gaining a wrong one. The G-buffer really holds
        // such normals (a back face, or the wrong sign of a quantised one).
        Frame turned = BuildScene(true, 1.0f, 1.0f, 1.0f);
        for (float& component : turned.normal)
            component = -component;
        const std::vector<uint8_t> upright = pixels;
        CHECK(harness.Run(pass, turned, pixels, kSize, kSize));
        CHECK(pixels == upright); // the search negates each one back, exactly: the same bytes, not merely a close picture
        const Reference turnedReference { turned, camera };
        float turnedWorst = 1.0f, turnedMean = 1.0f;
        Compare(pixels, turnedReference.Final(), kValueChannel, turnedWorst, turnedMean);
        CHECK(turnedWorst < 0.08f);
        CHECK(turnedMean < 0.01f);
        const float turnedDarkest = [&pixels]() {
            float low = 1.0f, high = 0.0f;
            RegionRange(pixels, kSize, 0, 0, kSize, kSize, kValueChannel, low, high);
            return low;
        }();
        CHECK(turnedDarkest < 0.95f); // and the corner's darkening is still there, not a frame of open floor
        std::printf("ao_warp: every stored normal facing away, darkest %.4f against %.4f, worst %.4f, mean %.5f\n", double(turnedDarkest),
                    double(darkest), double(turnedWorst), double(turnedMean));
        pixels = upright;
    }

    // ---- the strength and the blend: a strength of 0 leaves the engine's texel at every blend, and so does a blend of 0
    {
        // A term of 1.0 is the identity under the product: it has to leave the engine's own texel, not brighten it towards white. The mix that
        // replaced the engine's value at blend 1 turned this frame into 1.0 everywhere, which on the real capture deleted the engine's occlusion.
        const Frame none = BuildScene(true, 0.25f, 0.0f, 1.0f);
        CHECK(harness.Run(pass, none, pixels, kSize, kSize));
        float low = 1.0f, high = 0.0f;
        RegionRange(pixels, kSize, 0, 0, kSize, kSize, kValueChannel, low, high);
        CHECK(std::fabs(low - 0.25f) < 0.01f && std::fabs(high - 0.25f) < 0.01f); // the engine's texel back, whatever the search found

        // And at every blend: a factor of 1.0 taken half of the way to the engine's own is still the engine's own.
        const Frame noneHalf = BuildScene(true, 0.25f, 0.0f, 0.5f);
        CHECK(harness.Run(pass, noneHalf, pixels, kSize, kSize));
        RegionRange(pixels, kSize, 0, 0, kSize, kSize, kValueChannel, low, high);
        CHECK(std::fabs(low - 0.25f) < 0.01f && std::fabs(high - 0.25f) < 0.01f);

        const Frame engineOnly = BuildScene(true, 0.25f, 1.0f, 0.0f);
        CHECK(harness.Run(pass, engineOnly, pixels, kSize, kSize));
        RegionRange(pixels, kSize, 0, 0, kSize, kSize, kValueChannel, low, high);
        CHECK(std::fabs(low - 0.25f) < 0.01f && std::fabs(high - 0.25f) < 0.01f); // the engine's own texel, untouched

        const Frame half = BuildScene(true, 0.5f, 1.0f, 0.5f);
        CHECK(harness.Run(pass, half, pixels, kSize, kSize));
        const Reference reference { half, camera };
        const std::vector<float> expected = reference.Final();
        float worst = 1.0f, mean = 1.0f;
        Compare(pixels, expected, kValueChannel, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);

        // Only red is this module's: the engine's green, blue and alpha come back untouched, at every texel, whatever the strength and the
        // blend are. That is what keeps a channel whose meaning has not been measured out of the picture.
        uint32_t otherWorst = 0;
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                uint8_t engine[4] {};
                EngineBytes(x, y, half.engineValue, engine);
                for (uint32_t c : { 0u, 1u, 3u })
                {
                    const int difference = std::abs(int(pixels[(size_t(y) * kSize + x) * 4 + c]) - int(engine[c]));
                    otherWorst = std::max(otherWorst, uint32_t(difference));
                }
            }
        CHECK_EQ(otherWorst, 0u);
    }

    // ---- the quadrant: a depth allocation twice the occlusion texture's, and the render window inside it
    //
    // This is the session's own shape: a 1920x1080 occlusion texture over a 1920x1080 window inside a 3840x2160 allocation. The engine's own
    // producer fills the top-left 960x540 of that texture and every reader samples it through the allocation-relative screen coordinate, so this
    // module has to answer for the same quadrant, at the same depth texels, and leave the rest of the texture as the engine left it.
    {
        const Frame big = BuildScene(true, 0.25f, 1.0f, 1.0f, /*size*/ 2 * kSize, /*window*/ kSize);
        CHECK(harness.Run(pass, big, pixels, kSize, kSize));
        // The same scene with the window as its own allocation: inside the region the two runs must agree to the level, because a texel of ours
        // stands for the same depth block and the same window either way. A mapping by the window's size instead of the allocation's would read
        // a different depth texel here and disagree.
        const Frame crop = BuildScene(true, 0.25f, 1.0f, 1.0f, /*size*/ kSize, /*window*/ kSize, /*occlusion*/ kSize / 2);
        std::vector<uint8_t> cropped;
        CHECK(harness.Run(pass, crop, cropped, kSize / 2, kSize / 2));

        const Region region = RegionOf(big.facts);
        CHECK_EQ(region.width, kSize / 2);
        CHECK_EQ(region.height, kSize / 2);

        uint32_t insideWorst = 0, outsideWorst = 0;
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const size_t at = (size_t(y) * kSize + x) * 4;
                const bool inside = x >= region.x && y >= region.y && x < region.Right() && y < region.Bottom();
                if (!inside)
                {
                    // Outside the quadrant the engine's own texel has to come back untouched - all four channels, at this very texel - which
                    // is the copy the pass takes before it writes its region. An uninitialised target or a shifted copy fails here.
                    uint8_t engine[4] {};
                    EngineBytes(x, y, big.engineValue, engine);
                    for (uint32_t c = 0; c < 4; ++c)
                        outsideWorst = std::max(outsideWorst, uint32_t(std::abs(int(pixels[at + c]) - int(engine[c]))));
                }
                else if (x >= int(region.x) + 1 && y >= int(region.y) + 1 && x + 1 < region.Right() && y + 1 < region.Bottom())
                {
                    // Inside it (one texel in from the border, where the denoise box is the region's own and not the neighbouring one) the two
                    // runs agree.
                    const size_t cropAt = (size_t(y) * (kSize / 2) + x) * 4;
                    insideWorst = std::max(insideWorst, uint32_t(std::abs(int(pixels[at + kValueChannel]) - int(cropped[cropAt + kValueChannel]))));
                }
            }
        CHECK_EQ(outsideWorst, 0u); // the engine's texels outside the quadrant, byte for byte
        CHECK(insideWorst <= 1u);   // the quadrant's own texels, against the same scene rendered as its own allocation

        // The same scale-2 answer against the CPU reference, and not only against another run of the same shader: a texel of ours has to stand
        // for the depth texel the rules name (the first texel of its block) and be rotated by its own index. A shader that read the depth a texel
        // past the block's start - the pixel centre's +0.5 reaching the ratio - moves the whole picture by one texel, and the shader-against-shader
        // comparison above cannot see it because both runs would move the same way. The scene here is a row of walls, so a texel sits near a depth
        // and normal discontinuity wherever it is: the moved picture disagrees with the reference by more than the level-to-level differences.
        // The engine's own value is 1.0 in both picket frames, so the product leaves the search's term at full scale: with a smaller engine value
        // every difference is multiplied down with it, and an 8-bit tolerance wide enough for the search's own rounding would no longer see a
        // one-texel shift.
        {
            const Frame picket = BuildScene(true, 1.0f, 1.0f, 1.0f, /*size*/ 2 * kSize, /*window*/ kSize, /*occlusion*/ 0, /*picket*/ true);
            std::vector<uint8_t> picketPixels;
            CHECK(harness.Run(pass, picket, picketPixels, kSize, kSize));
            const Region picketRegion = RegionOf(picket.facts);
            const Reference reference { picket, camera };
            float worst = 1.0f, mean = 1.0f;
            CompareRegion(picketPixels, reference.Final(), kSize, picketRegion, kValueChannel, worst, mean);
            CHECK(worst < 0.08f);
            CHECK(mean < 0.01f);

            // The same scene rendered as its own allocation, whole target, is the same answer at scale 2.
            const Frame picketCrop = BuildScene(true, 1.0f, 1.0f, 1.0f, /*size*/ kSize, /*window*/ kSize, /*occlusion*/ kSize / 2,
                                                /*picket*/ true);
            std::vector<uint8_t> picketCropped;
            CHECK(harness.Run(pass, picketCrop, picketCropped, kSize / 2, kSize / 2));
            const Reference cropReference { picketCrop, camera };
            float cropWorst = 1.0f, cropMean = 1.0f;
            Compare(picketCropped, cropReference.Final(), kValueChannel, cropWorst, cropMean);
            CHECK(cropWorst < 0.08f);
            CHECK(cropMean < 0.01f);
            std::printf("ao_warp: quadrant against the reference, worst %.4f, mean %.5f (as its own allocation: worst %.4f, mean %.5f)\n",
                        double(worst), double(mean), double(cropWorst), double(cropMean));
        }

        // The wall's foot: depth texel (40, 40) is the base of the wall, and the occlusion texel that stands for it is (20, 20). The corner has
        // to reach that very texel: in the same scene with no wall, the open floor there is not darkened at all. Both texels here are the
        // product of the search's term with the engine's own 0.25, so the gap is a quarter of the raw term's: the bound is proportional.
        const Frame flat = BuildScene(false, 0.25f, 1.0f, 1.0f, /*size*/ 2 * kSize, /*window*/ kSize);
        std::vector<uint8_t> flatPixels;
        CHECK(harness.Run(pass, flat, flatPixels, kSize, kSize));
        const float foot = float(pixels[(size_t(20) * kSize + 20) * 4 + kValueChannel]) / 255.0f;
        const float open = float(flatPixels[(size_t(20) * kSize + 20) * 4 + kValueChannel]) / 255.0f;
        CHECK(foot < open * 0.98f);
        std::printf("ao_warp: quadrant %ux%u at %u,%u, foot %.3f against the open floor's %.3f, outside worst %u, inside worst %u\n", region.width, region.height,
                    region.x, region.y, double(foot), double(open), outsideWorst, insideWorst);
    }

    // ---- a cleared depth stays open
    {
        Frame sky = BuildScene(false, 1.0f, 1.0f, 1.0f);
        for (uint32_t y = 0; y < kSize / 2; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
                sky.depth[size_t(y) * kSize + x] = 0.0f;
        CHECK(harness.Run(pass, sky, pixels, kSize, kSize));
        float low = 0.0f, high = 0.0f;
        RegionRange(pixels, kSize, 0, 0, kSize, kSize / 2, kValueChannel, low, high);
        CHECK(low > 0.99f && high < 1.01f);
    }

    // ---- determinism and the owned targets
    {
        const Frame scene = BuildScene(true, 0.5f, 0.75f, 0.5f);
        CHECK(harness.Run(pass, scene, pixels, kSize, kSize));
        const std::vector<uint8_t> first = pixels;
        const uint32_t created = pass.TargetsCreated();
        const uint32_t begins = pass.GpuTimingBegins();
        CHECK(harness.Run(pass, scene, pixels, kSize, kSize));
        CHECK(pixels == first);                  // a still scene gives the same picture every run: no frame number, no history
        CHECK_EQ(pass.TargetsCreated(), created); // and the same size reuses the two owned targets
        CHECK(created >= 1u);
        CHECK(pass.GpuTimingBegins() > begins);
        CHECK_EQ(pass.GpuTimingEnds(), pass.GpuTimingBegins());

        // A different target size is a different pair of owned textures (a window resize).
        Frame bigger = BuildScene(true, 0.5f, 1.0f, 1.0f);
        bigger.facts.occlusionWidth = 96;
        bigger.facts.occlusionHeight = 96;
        const uint32_t before = pass.TargetsCreated();
        CHECK(harness.Run(pass, bigger, pixels, 96, 96));
        CHECK_EQ(pass.TargetsCreated(), before + 1);
        CHECK(pixels.size() == size_t(96) * 96 * 4);
    }

    // ---- a run the rules refuse hands the engine's texture back
    {
        Frame broken = BuildScene(true, 0.5f, 1.0f, 1.0f);
        broken.facts.normalWidth = 32; // the normal is not the depth's allocation
        PassInputs inputs;
        CHECK(harness.Upload(broken));
        inputs.occlusion = harness.occlusionView.Get();
        // The resource behind that view is an argument of the run too, and the null-argument guard is checked before the rules: a case that
        // leaves it unset never reaches Classify, so it would pass whatever the rules (or a caller that skipped them) decided.
        inputs.occlusionTexture = harness.occlusionTexture.Get();
        inputs.depth = harness.depthView.Get();
        inputs.normal = harness.normalView.Get();
        inputs.camera = harness.camera.Get();
        inputs.facts = broken.facts;
        inputs.params = broken.params;
        const uint32_t targets = pass.TargetsCreated();
        CHECK(pass.Run(harness.context.Get(), inputs) == nullptr);
        CHECK_EQ(pass.TargetsCreated(), targets); // the shape was refused before any owned target or device state was touched

        broken.facts = BuildScene(true, 0.5f, 1.0f, 1.0f).facts;
        broken.facts.occlusionWidth = 4; // the engine's 4x4 placeholder
        broken.facts.occlusionHeight = 4;
        inputs.facts = broken.facts;
        CHECK(pass.Run(harness.context.Get(), inputs) == nullptr);
        CHECK_EQ(pass.TargetsCreated(), targets); // the placeholder is refused as a shape, not passed on to the draws

        pass.Reset();
        CHECK(pass.OutputView() == nullptr); // a release leaves nothing of this module behind
    }

    // ---- the two bounds of the search that this fixture cannot separate from the rest of the picture
    //
    // Each is a rule of the shader's own rule list, and each is unreachable in a scene this test can build while the rest of the picture stays
    // the same:
    //   * a pixel without a usable depth (the far plane, a cleared depth, a NaN) keeps 1.0. With that guard gone the pixel is projected as if it
    //     were at the far plane, a hundred metres from the frame in this fixture and in a session, so every tap is either that far away or is the
    //     sky itself: the radius, the thickness bound and the sky test each refuse them and the texel comes out 1.0 either way.
    //   * the thickness bound (a tap farther in front than the search radius is another object). A tap is only accepted when it is nearer than
    //     the ray point, and every such tap is at or above the pixel's tangent plane, so its distance in front of the ray point is bounded by the
    //     same search radius the tap already has to be inside of.
    // Both are pinned at the statements that carry them, so a shader that drops or reorders one fails here instead of a picture that differs by
    // less than a level.
    {
        if (argc < 2)
        {
            std::printf("ao_warp: no repository path\n");
            return 2;
        }
        std::ifstream file(std::string(argv[1]) + "/OptiScaler/ext/ffxiv/engine/shaders/Ao.hlsl", std::ios::binary);
        const std::string shader((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(!shader.empty());
        CHECK(shader.find("if (!(depth > 0.0))") != std::string::npos);
        CHECK(shader.find("if (!(inFront > 0.0) || !(inFront <= radius))") != std::string::npos);
        CHECK(shader.find("saturate(horizon - bias) * 0.5") != std::string::npos); // the tangent plane's bias, rule 5
        CHECK(shader.find("if (!GeometricNormal(at, P, size, N))") != std::string::npos); // the search plane is the depth's own surface (2026-11-07)
    }

    return Finish("ao_warp");
}
