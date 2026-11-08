// Screen-space light bounce: the pass and its shaders (ext/ffxiv/engine/GiPass.cpp, shaders/Gi.hlsl + shaders/GiMarch.hlsl) on the D3D11 WARP
// software adapter, on synthetic buffers this test builds itself.
//
// The frame is 128x96, the camera at its origin looking down -z with no rotation (the view matrix's rows the identity carry). The floor is the
// plane y = -1; a dim slab hangs above a stretch of it (its underside at y = 0.5, z in [-4, -2.5], x in [-0.5, 1.5], its own direct light 0.02).
// A floor texel's rays leave it through its own tangent hemisphere - every ray goes up - so an OPEN floor pixel's rays find nothing but the sky
// and carry nothing (the sky is no bounce source), and a pixel under the slab's footprint has some of its rays land on the slab's dark underside
// instead: its estimate is small but positive. The floor's scene texels are written as the albedo store times two (store * sqrt(4)) on a
// checker of two stores per texel (0.8 and 0.3), so the floor's own linear scene value is store^2 * 4: 2.56 on one texel and 0.36 on the next. The
// slab's scene texels are its store times sqrt(0.02).
//
// The reference in this file computes the expected irradiance per pass pixel from the fixture's own geometry with the estimator the rules carry.
// A hit's radiance toward the origin is the hit texel's OWN linear scene value (its stored texel squared, the surface's colour left in: the
// checker makes that 2.56 on one floor texel and 0.36 on its neighbour, which an albedo quotient - 4.0 at both - could not fake), and the
// reference reads it from the fixture's scene values, never from the albedo target. Its data marker is the number of rays marched. The pass's
// irradiance and alpha are compared against that reference per pixel. What a wrong march, a wrong projection, a wrong estimator, a radiance
// read that dropped the squaring or kept the old albedo quotient, or a data marker that lost the traced zeros would break, and what is checked:
//   the irradiance matches the CPU reference at every pass pixel (worst and mean over the counted ones), and on the slab's underside, which
//   sees the checkered floor, within a few percent of the reference's value;
//   the store's alpha is the reference's traced-ray count at EVERY pass texel: the open floor, whose rays all go to the sky, is traced with
//   every ray missed (a valid zero, alpha above 0), while the sky and a block of floor whose normal store is cleared (an origin the pass
//   cannot trace) carry alpha 0;
//   a lamp (a band of the slab's underside lit to a stored 2.5, linear 6.25) bounces its own light, bounded by the clamp, and a band over the
//   clamp (stored 3.2, linear 10.2) is refused: the floor under it reads what it read with no lamp, and its alpha does not change;
//   the floor pixels under the slab read light where the open floor's pixels beside them read nothing, and both classes exist;
//   the smooth is the dense binomial stencil's own answer (an independent CPU reference with the weights typed in this file, its data marker
//   the reference's traced count) at every pixel;
//   the apply adds the bounded light: no channel of any pixel past the frame's white, the sky's scene bytes exact, the open floor's texels
//   byte-exact copies of the chain's scene, the covered floor's adds under the white bound, and the strength key's ratio linear until the
//   bound descales it;
//   a pass that cannot read its inputs returns null with a reason, and the engine's bindings come back after every run.
#include "check.h"

#include "ext/ffxiv/engine/GiPass.h"
#include "ext/ffxiv/engine/GiRules.h"

#include <d3d11.h>

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::gi;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

namespace
{
struct float2 { float x = 0.0f, y = 0.0f; };
struct float3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct int2 { int x = 0, y = 0; };

constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 96;

constexpr float kTanFov = 1.0f; // a 90-degree frustum: the ndc spans the window's own directions
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kFloorY = -1.0f;
constexpr float kFloorLight = 4.0f; // the direct light the floor carries, in the scene's own units
constexpr float kSlabY = 0.5f;      // the occluder slab's underside plane
constexpr float kSlabLight = 0.02f; // its own direct light: the slab carries almost nothing back to the floor
constexpr float kSlabZ0 = -4.0f;    // the slab's span along z
constexpr float kSlabZ1 = -2.5f;
constexpr float kSlabX0 = -0.5f; // its span along x
constexpr float kSlabX1 = 1.5f;
constexpr float kWhite = 4.0f; // the frame's white point the pass is handed: room above the fixture's brightest scene value (the floor's store times 2, at most 2.0), so the bounded addition has a head to work in
constexpr float kBrightestRadiance = (204.0f / 255.0f) * (204.0f / 255.0f) * kFloorLight; // the brightest linear scene value of the main run: store 0.8, light 4 (2.56)
constexpr float kIrradianceEps = 0.05f;  // the mean grid's tolerance
constexpr float kIrradianceWorstEps = 1.0f; // the one-hit's own boundary at any given pixel group's worst-case // one pixel's worst ramp tolerance at 16 steps over the five-metre range

constexpr float ProjectionA() { return kNear / (kFar - kNear); }
constexpr float ProjectionB() { return kNear * kFar / (kFar - kNear); }

// The projection as the march projects with: mul(matrix, v), row-major, the engine's own layout.
void ProjectionRows(float out[16])
{
    out[0] = 1.0f / kTanFov; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = 1.0f / kTanFov; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = ProjectionA(); out[11] = ProjectionB();
    out[12] = 0.0f; out[13] = 0.0f; out[14] = -1.0f; out[15] = 0.0f;
}

// The view matrix's three rows, the identity (no rotation): the world normals are the view-space ones themselves.
void FillView(float out[12])
{
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = 1.0f; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = 0.0f; out[9] = 0.0f; out[10] = 1.0f; out[11] = 0.0f;
}

// ---- half floats ----------------------------------------------------------------------------------------------------------------

uint16_t HalfOf(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return uint16_t(sign);
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

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

// ---- the fixtures' little vector maths ------------------------------------------------------------------------------------------

float3 Scale(const float3& a, float s) { return float3 { a.x * s, a.y * s, a.z * s }; }
float3 Sub(const float3& a, const float3& b) { return float3 { a.x - b.x, a.y - b.y, a.z - b.z }; }
float3 Add(const float3& a, const float3& b) { return float3 { a.x + b.x, a.y + b.y, a.z + b.z }; }
float Dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const float3& a) { return std::sqrt(Dot(a, a)); }
float3 Normalize(const float3& a) { const float l = Length(a); return l > 0.0f ? Scale(a, 1.0f / l) : a; }

// ---- the fixture: what each of the five game-side textures carries --------------------------------------------------------------

// The surface a camera ray through one texel reaches, in view space: the slab's underside where the ray crosses the y = 0.5 plane inside the
// slab's own footprint, the floor below the horizon, the sky above the slab's span. One source of truth for the textures and the reference both.
bool SurfaceAt(uint32_t x, uint32_t y, float3& viewPos, uint32_t& surface)
{
    const float ndcX = (float(x) + 0.5f) / float(kWidth) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (float(y) + 0.5f) / float(kHeight) * 2.0f;
    const float3 dir { ndcX * kTanFov, ndcY * kTanFov, -1.0f };
    if (dir.y > 1.0e-6f)
    {
        // A ray above the horizon: the slab's underside, where it crosses the y plane inside the footprint, or the sky.
        const float t = kSlabY / dir.y;
        const float3 p = Scale(dir, t);
        if (p.x >= kSlabX0 && p.x <= kSlabX1 && p.z >= kSlabZ0 && p.z <= kSlabZ1)
        {
            viewPos = float3 { p.x, kSlabY, p.z };
            surface = 2;
            return true;
        }
        return false; // the sky
    }
    if (dir.y < -1.0e-6f)
    {
        // The floor below the horizon.
        const float t = kFloorY / dir.y;
        viewPos = Scale(dir, t);
        surface = 1;
        return true;
    }
    return false; // on the horizon: no surface of this fixture's
}

// The open floor the classes compare against: near-field floor three metres or more beside the slab's span. The far floor near the horizon
// is not a class of this fixture: a texel row there spans metres of depth at this resolution, so its rays read depth neighbours at random
// (the view fade, the cosine of the camera against the normal, is what keeps that out of a real picture).
bool NearOpenFloor(const float3& p)
{
    return p.z >= -6.0f && p.z <= kSlabZ1 + 1.0f && (p.x >= kSlabX1 + 3.0f || p.x <= kSlabX0 - 3.0f);
}

// The middle of a class's values: the class compares are about what most of its texels get, not about the few the fixture's coarse depth
// rows throw outliers at.
double Median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
    return values[values.size() / 2];
}

uint8_t AlbedoStore(uint32_t x, uint32_t y) { return uint8_t(((x + y) & 1u) != 0u ? 204 : 77); } // the stores 0.8 and 0.3

// The lamp: a band of the slab's underside (view-space x in [kLampX0, kLampX1]) whose scene value the lamp runs set to a stored g_lampStored (the
// main run leaves it 0: no lamp, the band is the slab's own dim value). A lamp is an emissive surface: it sends its own light back, not the albedo's
// share of somebody else's.
constexpr float kLampX0 = 0.2f;
constexpr float kLampX1 = 0.6f;
float g_lampStored = 0.0f;

// The engine's special material rows (the id byte of the normal target's alpha in 32..44, and 224) get no bounce as origins: a band of the slab's
// underside whose id byte a run sets to a special row (the main run leaves the generic 128). The apply reads that byte from the normal view.
constexpr float kSpecialX0 = 0.9f;
constexpr float kSpecialX1 = 1.3f;
uint8_t g_specialId = 0;

// The block of floor whose normal store is cleared (an origin the pass cannot trace: the decoded length is not a surface's): window pixels
// [kBadX0, kBadX0 + kBadSize) x [kBadY0, kBadY0 + kBadSize), on the open floor left of the slab. At half resolution that is the pass texels
// [8, 12) x [36, 40).
constexpr int kBadX0 = 16;
constexpr int kBadY0 = 72;
constexpr int kBadSize = 8;

struct Fixture
{
    std::vector<float3> viewPos;
    std::vector<uint8_t> surface;
    std::vector<float> depth;
    std::vector<uint8_t> lamp; // the slab texels of the lamp band
    std::vector<uint8_t> bad;  // the floor texels whose normal store is cleared

    void Build()
    {
        viewPos.resize(size_t(kWidth) * kHeight);
        surface.assign(size_t(kWidth) * kHeight, 0);
        depth.assign(size_t(kWidth) * kHeight, 0.0f);
        lamp.assign(size_t(kWidth) * kHeight, 0);
        bad.assign(size_t(kWidth) * kHeight, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                float3 p;
                uint32_t which = 0;
                if (SurfaceAt(x, y, p, which))
                {
                    // The store's own rounding: the GPU reads the half floats, so the reference reads the same rounded ones.
                    viewPos[size_t(y) * kWidth + x] = float3 { FromHalf(HalfOf(p.x)), FromHalf(HalfOf(p.y)), FromHalf(HalfOf(p.z)) };
                    surface[size_t(y) * kWidth + x] = uint8_t(which);
                    depth[size_t(y) * kWidth + x] = FromHalf(HalfOf((ProjectionA() * p.z + ProjectionB()) / -p.z));
                    lamp[size_t(y) * kWidth + x] = uint8_t(which == 2 && p.x >= kLampX0 && p.x <= kLampX1 ? 1 : 0);
                    bad[size_t(y) * kWidth + x] = uint8_t(which == 1 && int(x) >= kBadX0 && int(x) < kBadX0 + kBadSize && int(y) >= kBadY0 &&
                                                          int(y) < kBadY0 + kBadSize ? 1 : 0);
                }
            }
    }

    float3 ViewPosAt(int2 at) const { return viewPos[size_t(at.y) * kWidth + at.x]; }
    float DepthAt(int2 at) const { return depth[size_t(at.y) * kWidth + at.x]; }
    // The surface's own normal, as the store carries it: the same 8-bit bytes the shaders read, decoded with the same n*2-1 (the identity view).
    // The 0.004 the store's own rounds in makes the basis one basis one way at the last bits, which is what the reference matches.
    float3 NormalAt(int2 at) const
    {
        const uint8_t which = surface[size_t(at.y) * kWidth + at.x];
        const float y = which == 2 ? -1.0f : 1.0f; // the store's 0 decodes to -1.0 (the underside's own), 255 to 1.0
        return float3 { (128.0f / 255.0f) * 2.0f - 1.0f, y, (128.0f / 255.0f) * 2.0f - 1.0f };
    }
    // Whether the surface at a texel is the slab underside.
    bool SlabAt(int2 at) const { return surface[size_t(at.y) * kWidth + at.x] == 2; }
    bool LampAt(int2 at) const { return lamp[size_t(at.y) * kWidth + at.x] != 0; }
    // The slab's special-row band (only when a run set an id): window pixels whose slab point lies in [kSpecialX0, kSpecialX1].
    bool SpecialAt(int2 at) const
    {
        float3 p;
        uint32_t which = 0;
        return g_specialId != 0 && SurfaceAt(uint32_t(at.x), uint32_t(at.y), p, which) && which == 2 && p.x >= kSpecialX0 && p.x <= kSpecialX1;
    }
    bool BadAt(int2 at) const { return bad[size_t(at.y) * kWidth + at.x] != 0; }

    // The scene's stored value at a texel, as the fixture writes it BEFORE the half float's rounding: the engine's stored form (the square root of
    // the composed sum) of store^2 * light - the floor's store times sqrt(4) on the checker, the slab's times sqrt(0.02), a lamp band's stored value
    // when a lamp run set one, the sky's constant 0.25.
    float SceneValue(int2 at) const
    {
        const uint8_t which = surface[size_t(at.y) * kWidth + at.x];
        if (which == 0)
            return 0.25f;
        if (which == 2 && g_lampStored > 0.0f && LampAt(at))
            return g_lampStored;
        const float store = which == 2 ? 36.0f / 255.0f : float(AlbedoStore(uint32_t(at.x), uint32_t(at.y))) / 255.0f;
        return store * std::sqrt(which == 2 ? kSlabLight : kFloorLight);
    }
    // What the GPU reads back from the scene texture there (the half float round trip included).
    float SceneStoredAt(int2 at) const { return FromHalf(HalfOf(SceneValue(at))); }
};

// The probe's per-ray debug: the fixture's one compare's own printing, while the march's own answers are being matched.
bool DebugProbe(int2 at) { return (at.x == 20 && at.y == 40) || (at.x == 30 && at.y == 33); }
inline uint32_t debugRaysValues = 0;
float2 ProjectWindowSimple(const float3 p, bool& inside)
{
    const float clipZ = ProjectionA() * p.z + ProjectionB();
    const float clipW = -p.z;
    inside = clipW > 0.0f;
    if (!inside)
        return float2 { -1.0f, -1.0f };
    const float2 ndc { (p.x / kTanFov) / clipW, (p.y / kTanFov) / clipW };
    const float2 result { (ndc.x * 0.5f + 0.5f) * float(kWidth), (0.5f - ndc.y * 0.5f) * float(kHeight) };
    inside = result.x >= 0.0f && result.y >= 0.0f && result.x < float(kWidth) && result.y < float(kHeight);
    return result;
}

// The radiance a hit sends back to its origin, typed here and not read from the rules header (gi_rules decides the rules' own function, gi_pins
// the equality of the clamp): the hit texel's stored value squared, refused when it is not a finite non-negative number or the square is over 8.
float ReferenceRadiance(float stored)
{
    if (!(stored >= 0.0f) || !std::isfinite(stored))
        return 0.0f;
    const float radiance = stored * stored;
    return radiance > 8.0f ? 0.0f : radiance;
}

// What the reference answers for one pass pixel: the irradiance and the store's data marker (the number of rays it marched, found or missed;
// 0 for an origin it cannot trace).
struct RefResult
{
    float3 rgb {};
    uint32_t traced = 0;
};

// The reference march for one pass pixel: the ramp, the crossing test, the thickness rejection and the estimator, all the rules' own shapes, on
// the fixture's own geometry. The radiance it reads at a hit is the hit texel's OWN linear scene value - the fixture's stored value squared,
// through the rules' BounceRadiance - never a store quotient and never the albedo target: a pass whose shader misread the albedo target, kept
// the quotient, or forgot the squaring is caught by this compare.
RefResult ReferenceIrradiance(const Fixture& fixture, int2 at, uint32_t rays, uint32_t steps, float range, float scale)
{
    float2 windowPixel { (float(at.x) + 0.5f) * scale, (float(at.y) + 0.5f) * scale };
    windowPixel.x = windowPixel.x >= float(kWidth) ? float(kWidth) - 1.0f : windowPixel.x;
    windowPixel.y = windowPixel.y >= float(kHeight) ? float(kHeight) - 1.0f : windowPixel.y;
    const int2 surfaceTexel { int(windowPixel.x), int(windowPixel.y) };
    if (fixture.DepthAt(surfaceTexel) <= 0.0f)
        return RefResult {}; // the sky keeps nothing, and is not traced
    const float3 origin = fixture.ViewPosAt(surfaceTexel);
    if (Dot(origin, origin) <= 1.0e-6f)
        return RefResult {};
    if (fixture.BadAt(surfaceTexel))
        return RefResult {}; // a cleared normal store: not a surface the pass can trace
    const float3 world = fixture.NormalAt(surfaceTexel);
    float normalView[3] { world.x, world.y, world.z };
    float basis[3][3] {};
    CHECK(TangentBasis(normalView, basis));
    const float originArr[3] { origin.x, origin.y, origin.z };
    const float fade = ViewFade(originArr, normalView);
    CHECK(fade > 0.0f); // the fixture's surfaces all face the camera where the floor is drawn
    if (!(fade > 0.0f))
        return RefResult {};

    const bool debugRays = DebugProbe(at);
    auto hash = [](float px, float py, uint32_t salt) {
        uint32_t h = uint32_t(px) * 1664525u + uint32_t(py) * 1013904223u;
        h ^= salt;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        return float(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
    };
    const float azimuthHash = hash(float(surfaceTexel.x), float(surfaceTexel.y), 0);
    const float elevationHash = hash(float(surfaceTexel.x), float(surfaceTexel.y), kHashSaltElevation);

    auto project = [&](const float3& p, bool& inside) {
        const float clipZ = ProjectionA() * p.z + ProjectionB();
        const float clipW = -p.z;
        inside = clipW > 0.0f;
        if (!inside)
            return float2 { -1.0f, -1.0f };
        const float2 ndc { (p.x / kTanFov) / clipW, (p.y / kTanFov) / clipW };
        const float2 result { (ndc.x * 0.5f + 0.5f) * float(kWidth), (0.5f - ndc.y * 0.5f) * float(kHeight) };
        inside = result.x >= 0.0f && result.y >= 0.0f && result.x < float(kWidth) && result.y < float(kHeight);
        return result;
    };

    float sum = 0.0f;
    uint32_t traced = 0;
    for (uint32_t ray = 0; ray < rays; ++ray)
    {
        const float u = elevationHash < 0.0f ? 0.0f : (elevationHash > 1.0f ? 1.0f : elevationHash);
        const float elevation = std::sqrt(u);
        if (!(elevation > 0.0f))
            continue;
        const float horizontal = std::sqrt(1.0f - elevation * elevation);
        const float angle = (float(ray) + azimuthHash) / float(rays) * 6.28318530717958647692f;
        const float along1 = horizontal * std::cos(angle);
        const float along2 = horizontal * std::sin(angle);
        float3 direction { basis[0][0] * along1 + basis[1][0] * along2 + basis[2][0] * elevation,
                           basis[0][1] * along1 + basis[1][1] * along2 + basis[2][1] * elevation,
                           basis[0][2] * along1 + basis[1][2] * along2 + basis[2][2] * elevation };
        direction = Normalize(direction);
        ++traced; // this ray is marched from here on, whether or not it finds a hit

        // The march: the ramp, the depth-surface crossing and the thickness, on the fixture's own view positions.
        float previous = 0.0f;
        bool found = false;
        int2 hit { -1, -1 };
        // How far a ray point lies behind the visible surface at its texel along the camera's line, less the bias (the shader's GiBehind).
        const auto behindOf = [](const float3& rayPoint, const float3& surface) {
            if (!(Dot(surface, surface) > 1.0e-6f))
                return -1.0f;
            const float surfaceDistance = std::sqrt(Dot(surface, surface));
            return std::sqrt(Dot(rayPoint, rayPoint)) - surfaceDistance - (kSurfaceBias + kDepthBias * surfaceDistance);
        };
        for (uint32_t k = 0; k < steps; ++k)
        {
            const float u2 = float(k + 1) / float(steps);
            const float next = kSurfaceBias + (range - kSurfaceBias) * u2 * u2;
            if (next > range)
                break;
            bool inside = false;
            const float2 window = project(Add(origin, Scale(direction, next)), inside);
            const int2 texel { inside ? int(window.x) : -1, inside ? int(window.y) : -1 };
            if (texel.x < 0 || texel.y < 0 || texel.x >= int(kWidth) || texel.y >= int(kHeight))
                break;
            if (!(behindOf(Add(origin, Scale(direction, next)), fixture.ViewPosAt(texel)) > 0.0f))
            {
                previous = next;
                continue;
            }
            // A crossing: the interval halved, then the thickness decides.
            float loEdge = previous;
            float hiEdge = next;
            int2 refined = texel;
            for (uint32_t r = 0; r < kMaxRefine; ++r)
            {
                const float mid = 0.5f * (loEdge + hiEdge);
                bool midInside = false;
                const float2 midWindow = project(Add(origin, Scale(direction, mid)), midInside);
                const int2 midTexel { midInside ? int(midWindow.x) : -1, midInside ? int(midWindow.y) : -1 };
                if (midTexel.x < 0)
                    break;
                if (behindOf(Add(origin, Scale(direction, mid)), fixture.ViewPosAt(midTexel)) > 0.0f)
                {
                    hiEdge = mid;
                    refined = midTexel;
                }
                else
                {
                    loEdge = mid;
                }
            }
            const float behind = behindOf(Add(origin, Scale(direction, hiEdge)), fixture.ViewPosAt(refined));
            if (behind > 0.0f && behind <= kThickness)
            {
                hit = refined;
                found = hit.x >= 0;
            }
            break;
        }
        if (!found)
            continue;
        const int2 hitTexel = hit;
        // The radiance leaving the hit toward the origin: its own linear scene value (refused above the clamp, like the shader).
        const float radiance = ReferenceRadiance(fixture.SceneStoredAt(hitTexel));
        sum += radiance;
        if (debugRays)
            std::printf("gi_warp: probe after ray %u: traced %u sum %.4f radiance %.4f\n", ray, traced, double(sum), double(radiance));
    }
    RefResult result;
    result.traced = traced;
    if (!(sum > 0.0f))
        return result; // every ray missed (or was refused): a valid zero, traced
    const float estimate = sum * (kPi / float(rays)) * fade;
    result.rgb = float3 { estimate, estimate, estimate };
    return result;
}

// What the clamp and the four a-trous iterations answer for the whole pass grid, as an independent CPU reference with its numbers typed HERE (the
// rules header's constants are pinned against the shader by gi_pins, and gi_filter_warp decides the shaders' behaviour in isolation; this reference
// decides that the pass wires them up: the guide, the clamp, the four steps in order, the ping-pong, the final target).
//   guide   z of a traced texel = the depth distance of the window pixel its rays start at (the fixture's view position, through the half float),
//           0 for an origin that is not traced (so "has data" and "has a guide" are one fact);
//   clamp   luminance held to 16 x the mean luminance of the other traced texels of the 5x5 square + 0.1, the colour scaled as a whole;
//   filter  four iterations, the binomial row 1-4-6-4-1 at 1, 2, 4 and 8 texels apart, taps outside the extent and taps without data skipped, each tap
//           weighted by exp(-|inverse depth - the centre plane's| / (0.03 x the centre's inverse depth)), the plane measured around the centre
//           (per axis the smaller one-sided step).
// Every stage's result goes through the half float, as the pass's RGBA16F targets do. The irradiance is the shader's own (a ray that grazes a hit
// boundary may count differently on the CPU and the GPU, which is the march's tolerance above, not the filter's), the data marker the REFERENCE's
// traced count, which a grazing ray cannot change.
struct FilterResult
{
    std::vector<float3> rgb;     // the filtered irradiance (0 where the texel has no data)
    std::vector<uint8_t> has;    // the texel carries data after the filter
    std::vector<float> guideZ;   // the reference guide
    uint32_t clampedTexels = 0;  // the texels the clamp held back
};

float RoundedHalf(float value) { return FromHalf(HalfOf(value)); }

FilterResult ReferenceFilter(const Fixture& fixture, const std::vector<float3>& irradiance, const std::vector<float>& traced, uint32_t width, uint32_t height)
{
    constexpr float row[5] { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };
    constexpr float kRatio = 16.0f, kFloor = 0.1f, kSigma = 0.03f;
    constexpr int steps[4] { 1, 2, 4, 8 };
    FilterResult result;
    const size_t count = size_t(width) * height;
    result.guideZ.assign(count, 0.0f);
    result.has.assign(count, 0);
    std::vector<float3> current = irradiance;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = size_t(y) * width + x;
            if (!(traced[i] > 0.0f))
                continue;
            const float px = std::min((float(x) + 0.5f) * 2.0f, float(kWidth) - 1.0f), py = std::min((float(y) + 0.5f) * 2.0f, float(kHeight) - 1.0f);
            result.guideZ[i] = RoundedHalf(-fixture.ViewPosAt(int2 { int(px), int(py) }).z);
            result.has[i] = 1;
        }
    const auto lum = [](const float3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
    // The clamp (reads the raw irradiance, writes the first filter target).
    {
        std::vector<float3> clamped = current;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = size_t(y) * width + x;
                if (!result.has[i])
                    continue;
                double sum = 0.0;
                uint32_t neighbours = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        const int tx = int(x) + dx, ty = int(y) + dy;
                        if ((dx == 0 && dy == 0) || tx < 0 || ty < 0 || tx >= int(width) || ty >= int(height) || !result.has[size_t(ty) * width + size_t(tx)])
                            continue;
                        sum += double(lum(current[size_t(ty) * width + size_t(tx)]));
                        ++neighbours;
                    }
                if (neighbours == 0)
                    continue;
                const float cap = kRatio * float(sum / double(neighbours)) + kFloor;
                const float l = lum(current[i]);
                if (l > cap)
                {
                    const float scale = cap / l;
                    clamped[i] = float3 { current[i].x * scale, current[i].y * scale, current[i].z * scale };
                    ++result.clampedTexels;
                }
            }
        for (float3& c : clamped)
            c = float3 { RoundedHalf(c.x), RoundedHalf(c.y), RoundedHalf(c.z) };
        current = clamped;
    }
    // The four iterations.
    const auto inverseDepth = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= int(width) || y >= int(height))
            return 0.0f;
        const float z = result.guideZ[size_t(y) * width + size_t(x)];
        return z > 0.0f ? 1.0f / z : 0.0f;
    };
    const auto slope = [](float centre, float plus, float minus) {
        const bool havePlus = plus > 0.0f, haveMinus = minus > 0.0f;
        const float stepPlus = plus - centre, stepMinus = centre - minus;
        if (havePlus && haveMinus)
            return std::fabs(stepPlus) <= std::fabs(stepMinus) ? stepPlus : stepMinus;
        return havePlus ? stepPlus : (haveMinus ? stepMinus : 0.0f);
    };
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        std::vector<float3> next(count);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = size_t(y) * width + x;
                if (!result.has[i])
                    continue;
                const float u0 = inverseDepth(int(x), int(y));
                const float sx = slope(u0, inverseDepth(int(x) + 1, int(y)), inverseDepth(int(x) - 1, int(y)));
                const float sy = slope(u0, inverseDepth(int(x), int(y) + 1), inverseDepth(int(x), int(y) - 1));
                double sum[3] {};
                double weight = 0.0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        const int ox = dx * steps[iteration], oy = dy * steps[iteration];
                        const int tx = int(x) + ox, ty = int(y) + oy;
                        if (tx < 0 || ty < 0 || tx >= int(width) || ty >= int(height) || !result.has[size_t(ty) * width + size_t(tx)])
                            continue;
                        const float uq = inverseDepth(tx, ty);
                        if (!(uq > 0.0f))
                            continue;
                        const float predicted = u0 + sx * float(ox) + sy * float(oy);
                        const double w = double(row[dx + 2] * row[dy + 2]) * std::exp(-std::fabs(double(uq - predicted)) / (double(kSigma) * double(u0)));
                        const float3& c = current[size_t(ty) * width + size_t(tx)];
                        sum[0] += double(c.x) * w;
                        sum[1] += double(c.y) * w;
                        sum[2] += double(c.z) * w;
                        weight += w;
                    }
                if (weight > 0.0)
                    next[i] = float3 { RoundedHalf(float(sum[0] / weight)), RoundedHalf(float(sum[1] / weight)), RoundedHalf(float(sum[2] / weight)) };
            }
        current = next;
    }
    result.rgb = current;
    return result;
}

// ---- the GPU side: the fixture's textures and one stage run ----------------------------------------------------------------------

struct GpuFixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> scene, viewPos, normal, albedo, material, depth;
    ComPtr<ID3D11ShaderResourceView> sceneSrv, viewPosSrv, normalSrv, albedoSrv, materialSrv, depthSrv;

    void Make()
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &scene));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(scene.Get(), nullptr, &sceneSrv));
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &viewPos));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(viewPos.Get(), nullptr, &viewPosSrv));

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &normal));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(normal.Get(), nullptr, &normalSrv));
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &albedo));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(albedo.Get(), nullptr, &albedoSrv));
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &material));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(material.Get(), nullptr, &materialSrv));

        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &depth));
        CHECK_SUCCEEDED(device->CreateShaderResourceView(depth.Get(), nullptr, &depthSrv));
    }

    static uint8_t SaturateByte(float value) { return uint8_t(value <= 0.0f ? 0.0f : (value >= 1.0f ? 255.0f : value * 255.0f + 0.5f)); }

    void Fill(const Fixture& fixture)
    {
        std::vector<uint16_t> sceneTexels(size_t(kWidth) * kHeight * 4);
        std::vector<uint16_t> viewTexels(size_t(kWidth) * kHeight * 4);
        std::vector<uint8_t> normalTexels(size_t(kWidth) * kHeight * 4);
        std::vector<uint8_t> albedoTexels(size_t(kWidth) * kHeight * 4);
        std::vector<uint8_t> materialTexels(size_t(kWidth) * kHeight * 4);
        std::vector<float> depthTexels(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t at = size_t(y) * kWidth + x;
                float3 p;
                uint32_t which = 0;
                if (!SurfaceAt(x, y, p, which))
                {
                    // The sky's own scene value: a known constant, read back exact after the apply.
                    sceneTexels[at * 4] = HalfOf(0.25f);
                    sceneTexels[at * 4 + 1] = HalfOf(0.25f);
                    sceneTexels[at * 4 + 2] = HalfOf(0.25f);
                    sceneTexels[at * 4 + 3] = 0;
                    viewTexels[at * 4] = viewTexels[at * 4 + 1] = viewTexels[at * 4 + 2] = HalfOf(0.0f);
                    viewTexels[at * 4 + 3] = 0;
                    normalTexels[at * 4] = normalTexels[at * 4 + 1] = normalTexels[at * 4 + 2] = normalTexels[at * 4 + 3] = 0;
                    albedoTexels[at * 4] = albedoTexels[at * 4 + 1] = albedoTexels[at * 4 + 2] = albedoTexels[at * 4 + 3] = 0;
                    materialTexels[at * 4] = materialTexels[at * 4 + 1] = materialTexels[at * 4 + 2] = materialTexels[at * 4 + 3] = 0;
                    depthTexels[at] = 0.0f;
                    continue;
                }
                const float store = which == 2 ? 36.0f / 255.0f : float(AlbedoStore(x, y)) / 255.0f;
                const float value = fixture.SceneValue(int2 { int(x), int(y) });
                // The scene: the stored sqrt of the composed sum, the engine's own domain: store*mult here, so the texel's own linear value is
                // store^2 * light (2.56 or 0.36 on the checker of the floor). A lamp run sets the lamp band's stored value instead.
                sceneTexels[at * 4] = HalfOf(value);
                sceneTexels[at * 4 + 1] = HalfOf(value);
                sceneTexels[at * 4 + 2] = HalfOf(value);
                sceneTexels[at * 4 + 3] = 0;
                // The albedo carrier: still bound for the apply (the origin's own albedo multiplier); the march no longer reads it.
                albedoTexels[at * 4] = SaturateByte(store);
                albedoTexels[at * 4 + 1] = SaturateByte(store);
                albedoTexels[at * 4 + 2] = SaturateByte(store);
                albedoTexels[at * 4 + 3] = 128; // the generic deferred surface's own id byte
                // The normal (the floor's up or the slab underside's down, as n*0.5+0.5; the byte order is the engine's own B8G8R8 read).
                const uint8_t up = which == 2 ? 0 : 255;
                normalTexels[at * 4] = 128;      // the x axis carries 0.0
                normalTexels[at * 4 + 1] = up;   // the y axis carries 1.0 (the floor) or -1.0 (the underside)
                normalTexels[at * 4 + 2] = 128;  // the z axis carries 0.0
                normalTexels[at * 4 + 3] = fixture.SpecialAt(int2 { int(x), int(y) }) ? g_specialId : 128; // the generic deferred surface's own id byte, or a special row
                if (fixture.BadAt(int2 { int(x), int(y) }))
                    normalTexels[at * 4] = normalTexels[at * 4 + 1] = normalTexels[at * 4 + 2] = 0; // a cleared normal store: decoded length^2 = 3, not a surface
                // The material pack: the selector off (0), the diffuse weight 0 (nothing taken off the generic path).
                materialTexels[at * 4] = materialTexels[at * 4 + 1] = materialTexels[at * 4 + 2] = materialTexels[at * 4 + 3] = 0;
                // The view positions: rgb = the view-space position, fp16.
                viewTexels[at * 4] = HalfOf(p.x);
                viewTexels[at * 4 + 1] = HalfOf(p.y);
                viewTexels[at * 4 + 2] = HalfOf(p.z);
                viewTexels[at * 4 + 3] = 0;
                depthTexels[at] = fixture.DepthAt(int2 { int(x), int(y) });
            }
        Upload(scene.Get(), sceneTexels);
        Upload(viewPos.Get(), viewTexels);
        Upload(normal.Get(), normalTexels);
        Upload(albedo.Get(), albedoTexels);
        Upload(material.Get(), materialTexels);
        UploadDepth(depth.Get(), depthTexels);
    }

    void Upload(ID3D11Texture2D* texture, const std::vector<uint16_t>& texels16)
    {
        std::vector<uint8_t> bytes(texels16.size() * sizeof(uint16_t));
        std::memcpy(bytes.data(), texels16.data(), bytes.size());
        UploadBytes(texture, bytes, uint32_t(kWidth) * 4u * sizeof(uint16_t));
    }

    void Upload(ID3D11Texture2D* texture, const std::vector<uint8_t>& bytes8)
    {
        UploadBytes(texture, bytes8, uint32_t(kWidth) * 4u);
    }

    void UploadDepth(ID3D11Texture2D* texture, const std::vector<float>& values)
    {
        std::vector<uint8_t> bytes(values.size() * sizeof(float));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        UploadBytes(texture, bytes, uint32_t(kWidth) * sizeof(float));
    }

    void UploadBytes(ID3D11Texture2D* texture, const std::vector<uint8_t>& bytes, uint32_t rowBytes)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        stagingDesc.MipLevels = 1;
        ComPtr<ID3D11Texture2D> staging;
        CHECK_SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped));
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(static_cast<unsigned char*>(mapped.pData) + size_t(mapped.RowPitch) * y, bytes.data() + size_t(rowBytes) * y,
                        rowBytes);
        context->Unmap(staging.Get(), 0);
        context->CopyResource(texture, staging.Get());
    }

    std::vector<uint16_t> ReadBack(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MipLevels = 1;
        ComPtr<ID3D11Texture2D> staging;
        CHECK_SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint16_t> out(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(out.data() + size_t(y) * desc.Width * 4,
                        static_cast<const unsigned char*>(mapped.pData) + size_t(mapped.RowPitch) * y,
                        size_t(desc.Width) * 4 * sizeof(uint16_t));
        context->Unmap(staging.Get(), 0);
        return out;
    }

    ComPtr<ID3D11Texture2D> TextureOf(ID3D11ShaderResourceView* view) const
    {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        CHECK(SUCCEEDED(resource.As(&texture)));
        return texture;
    }
};

// The pass copy's own rgb, read back.
std::vector<float3> ReadBackCopyRgb(GpuFixture& gpu, ID3D11ShaderResourceView* copySrv)
{
    const ComPtr<ID3D11Texture2D> texture = gpu.TextureOf(copySrv);
    const std::vector<uint16_t> texels = gpu.ReadBack(texture.Get());
    std::vector<float3> out(size_t(kWidth) * kHeight);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const size_t at = size_t(y) * kWidth + x;
            out[at] = float3 { FromHalf(texels[at * 4]), FromHalf(texels[at * 4 + 1]), FromHalf(texels[at * 4 + 2]) };
        }
    return out;
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // the crash-friendly unbuffered stdout: every printf lands whatever comes
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context)) ||
        device == nullptr)
    {
        std::printf("gi_warp: no D3D11 WARP device\n");
        return 1;
    }

    Fixture fixture;
    fixture.Build();
    GpuFixture gpu;
    gpu.device = device;
    gpu.context = context;
    gpu.Make();
    std::printf("gi_warp: fixture textures made\n");
    gpu.Fill(fixture);
    std::printf("gi_warp: fixture filled\n");

    const ext::ffxiv::shafts::Window window { 0.0f, 0.0f, float(kWidth), float(kHeight) };

    // The run: half res, four rays, sixteen steps, the five-metre range, the default strength.
    Inputs inputs;
    inputs.scene = gpu.sceneSrv.Get();
    inputs.depth = gpu.depthSrv.Get();
    inputs.viewPos = gpu.viewPosSrv.Get();
    inputs.normal = gpu.normalSrv.Get();
    inputs.albedo = gpu.albedoSrv.Get();
    inputs.material = gpu.materialSrv.Get();
    inputs.sceneFormat = 10;
    inputs.sceneWidth = kWidth;
    inputs.sceneHeight = kHeight;
    inputs.depthFormat = 41;
    inputs.depthWidth = kWidth;
    inputs.depthHeight = kHeight;
    inputs.window = window;
    FillView(inputs.view);
    ProjectionRows(inputs.projection);
    inputs.params = BuildParams(4, 16, 5.0f, 0.5f, "half");
    inputs.whitePoint = kWhite;
    inputs.fade = 1.0f;

    std::printf("gi_warp: running the pass...\n");
    Pass pass;
    CHECK(pass.Init(device.Get()));
    std::printf("gi_warp: pass inited\n");
    ID3D11ShaderResourceView* firstCopy = pass.Run(context.Get(), inputs);
    std::printf("gi_warp: pass ran\n");
    if (firstCopy == nullptr)
    {
        std::printf("gi_warp: the pass did not run (%s)\n", pass.Failure() != nullptr ? pass.Failure() : "no reason");
        return 1;
    }

    // ---- the irradiance against the CPU reference ----------------------------------------------------------------------------------
    const std::vector<uint16_t> irradianceRaw = gpu.ReadBack(gpu.TextureOf(pass.IrradianceView()).Get());
    const std::vector<uint16_t> smoothRaw = gpu.ReadBack(gpu.TextureOf(pass.SmoothView()).Get());
    const uint32_t giWidth = pass.IrradianceWidth();
    const uint32_t giHeight = pass.IrradianceHeight();
    CHECK_EQ(giWidth, uint32_t(kWidth / 2));
    CHECK_EQ(giHeight, uint32_t(kHeight / 2));

    std::vector<float3> referenceIrradiance(size_t(giWidth) * giHeight);
    std::vector<float> referenceData(size_t(giWidth) * giHeight, 0.0f);
    std::vector<float> referenceTraced(size_t(giWidth) * giHeight, 0.0f); // the reference's data marker: the rays it marched
    std::vector<float3> referenceSmoothed;                                // the independent smooth of the shader's own irradiance (filled below)
    uint32_t alphaMismatches = 0, skyTexels = 0, untraceableSurface = 0, openTracedZeros = 0, slabRelativeCount = 0;
    double slabRelativeSum = 0.0;
    float worstIrradiance = 0.0f;
    double meanIrradiance = 0.0;
    uint32_t irradianceWorstTexel = 0;
    uint32_t coveredCount = 0, openCount = 0;
    double coveredMean = 0.0, openMean = 0.0;
    std::vector<double> coveredValues, openValues;
    for (uint32_t y = 0; y < giHeight; ++y)
        for (uint32_t x = 0; x < giWidth; ++x)
        {
            const int2 at { int(x), int(y) };
            const RefResult ref = ReferenceIrradiance(fixture, at, inputs.params.rays, inputs.params.steps, inputs.params.range, 2.0f);
            const float3 expected = ref.rgb;
            referenceIrradiance[size_t(y) * giWidth + x] = expected;
            referenceTraced[size_t(y) * giWidth + x] = float(ref.traced);
            referenceData[size_t(y) * giWidth + x] = expected.x > 0.0f || expected.y > 0.0f || expected.z > 0.0f ? 1.0f : 0.0f;
            const uint16_t* texel = &irradianceRaw[(size_t(y) * giWidth + x) * 4];
            const float3 got { FromHalf(texel[0]), FromHalf(texel[1]), FromHalf(texel[2]) };
            // The store's alpha is the rays the origin traced, exactly (a ray marched is marched whether or not it hit): not the rays that hit.
            const float gotAlpha = FromHalf(texel[3]);
            if (gotAlpha != float(ref.traced))
            {
                if (alphaMismatches < 6)
                    std::printf("gi_warp: alpha mismatch at pass texel %u,%u: got %.1f, the reference traced %u rays (got rgb %.4f, reference rgb %.4f)\n", x, y,
                                double(gotAlpha), ref.traced, double(got.x), double(expected.x));
                ++alphaMismatches;
            }
            {
                const float2 originPixel { (float(x) + 0.5f) * 2.0f, (float(y) + 0.5f) * 2.0f };
                const int2 originTexel { originPixel.x < float(kWidth) ? int(originPixel.x) : int(kWidth - 1), originPixel.y < float(kHeight) ? int(originPixel.y) : int(kHeight - 1) };
                if (ref.traced == 0 && fixture.DepthAt(originTexel) <= 0.0f)
                    ++skyTexels;
                else if (ref.traced == 0)
                    ++untraceableSurface;
            }
            const float worst = std::max(std::fabs(got.x - expected.x), std::max(std::fabs(got.y - expected.y), std::fabs(got.z - expected.z)));
            if (worst > worstIrradiance)
            {
                worstIrradiance = worst;
                irradianceWorstTexel = uint32_t(x) + uint32_t(y) * giWidth;
            }
            meanIrradiance += double(worst);
            // The fixture's two classes: a covered floor pixel has some rays on the dim slab, an open one has only the sky above it.
            int2 underTexel { -1, -1 };
            {
                const float2 under { (float(x) + 0.5f) * 2.0f, (float(y) + 0.5f) * 2.0f };
                underTexel.x = under.x < float(kWidth) ? int(under.x) : int(kWidth - 1);
                underTexel.y = under.y < float(kHeight) ? int(under.y) : int(kHeight - 1);
            }
            if (fixture.DepthAt(underTexel) > 0.0f)
            {
                float3 p;
                uint32_t which = 0;
                SurfaceAt(uint32_t(underTexel.x), uint32_t(underTexel.y), p, which);
                if (which == 1 && p.x >= kSlabX0 - 0.3f && p.x <= kSlabX1 + 0.3f && p.z >= kSlabZ0 && p.z <= kSlabZ1)
                {
                    coveredMean += double(got.x);
                    coveredValues.push_back(double(got.x));
                    ++coveredCount;
                }
                else if (which == 1 && NearOpenFloor(p))
                {
                    openMean += double(got.x);
                    openValues.push_back(double(got.x));
                    ++openCount;
                    // A traced origin whose rays all went to the sky: rgb 0 and alpha above 0 (a valid zero), not "no data".
                    if (gotAlpha > 0.0f && got.x == 0.0f && got.y == 0.0f && got.z == 0.0f)
                        ++openTracedZeros;
                }
                else if (which == 2 && expected.x > 0.05f)
                {
                    // The slab's underside sees the checkered floor: its irradiance is the floor's own linear values (2.56 and 0.36), not 4.0.
                    slabRelativeSum += double(std::fabs(got.x - expected.x) / expected.x);
                    ++slabRelativeCount;
                }
            }
        }
    meanIrradiance /= double(size_t(giWidth) * giHeight);
    std::printf("gi_warp: irradiance against the reference, worst %.4f at %u, mean %.5f\n", double(worstIrradiance), irradianceWorstTexel,
                meanIrradiance);
    for (const int2 probe : { int2 { 10, 10 }, int2 { 20, 20 }, int2 { 40, 20 }, int2 { 20, 40 }, int2 { 50, 30 }, int2 { 2142 % 64, 2142 / 64 } })
    {
        const uint16_t* probeRaw = &irradianceRaw[(size_t(probe.y) * giWidth + probe.x) * 4];
        const float3 gotProbe { FromHalf(probeRaw[0]), FromHalf(probeRaw[1]), FromHalf(probeRaw[2]) };
        const float3 expectProbe = referenceIrradiance[size_t(probe.y) * giWidth + probe.x];
        std::printf("gi_warp: probe %d,%d expected %.4f got %.4f, pairs %.0f, depth %.4f\n", probe.x, probe.y, double(expectProbe.x),
                    double(gotProbe.x), double(FromHalf(probeRaw[3])),
                    double(fixture.DepthAt(int2 { probe.x * 2 + 1, probe.y * 2 + 1 })));
    }
    CHECK(meanIrradiance < kIrradianceEps);
    CHECK(worstIrradiance < kIrradianceWorstEps);
    const double slabRelativeMean = slabRelativeCount > 0 ? slabRelativeSum / double(slabRelativeCount) : 1.0e9;
    std::printf("gi_warp: the data marker: %u alpha mismatches against the reference's traced counts; %u sky texels, %u untraceable surface texels, %u open-floor "
                "texels traced with every ray missed (alpha above 0, rgb 0); the slab underside's mean relative error %.4f over %u texels\n",
                alphaMismatches, skyTexels, untraceableSurface, openTracedZeros, slabRelativeMean, slabRelativeCount);
    // The store's alpha is the traced count at every pass texel; the sky and the cleared-normal block (16 pass texels... 4x4) are the only origins
    // with none, and the open floor's all-miss texels are traced zeros.
    CHECK_EQ(alphaMismatches, 0u);
    CHECK(skyTexels > 100);
    CHECK_EQ(untraceableSurface, 16u);
    CHECK(openTracedZeros >= 15); // 18 of the 19 near-open-floor texels (the 19th grazes a bright floor texel and has a hit)
    // A hit is read as the surface's own linear value: the slab sees the checker (2.56 / 0.36), an albedo quotient would see 4.0 at both.
    CHECK(slabRelativeCount > 30);
    CHECK(slabRelativeMean < 0.05);
    // The estimator's own ceiling: no texel's estimate exceeds pi times the brightest linear scene value of the fixture (a hemisphere full of
    // that radiance is its whole answer), whatever the rays drew.
    for (size_t boundAt = 0; boundAt < referenceData.size(); ++boundAt)
    {
        CHECK(irradianceRaw[boundAt * 4] <= HalfOf(kBrightestRadiance * kPi * 1.01f + 1.0e-2f));
        CHECK(irradianceRaw[boundAt * 4 + 1] <= HalfOf(kBrightestRadiance * kPi * 1.01f + 1.0e-2f));
        CHECK(irradianceRaw[boundAt * 4 + 2] <= HalfOf(kBrightestRadiance * kPi * 1.01f + 1.0e-2f));
    }
    // The covered floor's estimate carries the slab's dim underside in some of its rays; the open floor's beside it gets one fewer of them.
    CHECK(coveredCount > 0 && openCount > 0);
    std::printf("gi_warp: the covered floor's irradiance mean %.5f over %u, the open floor's %.5f over %u\n", coveredMean, coveredCount,
                openMean, openCount);
    {
        // The open class is not empty of light in raw irradiance: the fixture's coarse rows near the horizon let a ray graze a bright floor texel
        // (a depth neighbour at random), and such an isolated texel is what the smooth then spreads (below).
        uint32_t openWithHit = 0, coveredWithHit = 0;
        double openLargest = 0.0;
        for (double value : openValues)
        {
            openWithHit += value > 0.0 ? 1u : 0u;
            openLargest = std::max(openLargest, value);
        }
        for (double value : coveredValues)
            coveredWithHit += value > 0.0 ? 1u : 0u;
        std::printf("gi_warp: raw irradiance texels with a hit: covered floor %u of %u, open floor %u of %u (largest %.4f)\n", coveredWithHit,
                    coveredCount, openWithHit, openCount, openLargest);
    }
    CHECK(Median(coveredValues) >= Median(openValues));

    // ---- the guide, the clamp and the filter against the reference ---------------------------------------------------------------------
    FilterResult filtered;
    {
        std::vector<float3> shaderIrradiance(size_t(giWidth) * giHeight);
        for (size_t i = 0; i < shaderIrradiance.size(); ++i)
            shaderIrradiance[i] = float3 { FromHalf(irradianceRaw[i * 4]), FromHalf(irradianceRaw[i * 4 + 1]), FromHalf(irradianceRaw[i * 4 + 2]) };
        filtered = ReferenceFilter(fixture, shaderIrradiance, referenceTraced, giWidth, giHeight);
        // The guide: a traced texel carries its depth distance in the alpha, every other texel carries nothing, and the two facts are one (the pass's
        // trace and its guide decode the origin through one function). A pass whose guide disagreed with its trace would filter a texel against nothing.
        const std::vector<uint16_t> guideRaw = gpu.ReadBack(gpu.TextureOf(pass.GuideView()).Get());
        uint32_t guideMismatches = 0, guideTraced = 0;
        double worstGuideZ = 0.0;
        for (size_t i = 0; i < filtered.guideZ.size(); ++i)
        {
            const float guideZ = FromHalf(guideRaw[i * 4 + 3]);
            const bool traced = referenceTraced[i] > 0.0f;
            guideTraced += traced ? 1u : 0u;
            if ((guideZ > 0.0f) != traced || guideRaw[i * 4] != 0 || guideRaw[i * 4 + 1] != 0 || guideRaw[i * 4 + 2] != 0)
                ++guideMismatches;
            else if (traced)
                worstGuideZ = std::max(worstGuideZ, double(std::fabs(guideZ - filtered.guideZ[i])) / double(filtered.guideZ[i]));
        }
        std::printf("gi_warp: the guide against the trace: %u mismatches over %zu texels (%u traced), worst relative distance of z from the fixture's %.5f\n", guideMismatches,
                    filtered.guideZ.size(), guideTraced, worstGuideZ);
        CHECK_EQ(guideMismatches, 0u);
        CHECK(guideTraced > 100);
        CHECK(worstGuideZ < 0.002);
        // The filter's final target against the reference, at every texel: the colour where the texel carries data, nothing (0,0,0,0) where it does not.
        double worstFilter = 0.0;
        uint32_t alphaWrong = 0;
        for (uint32_t y = 0; y < giHeight; ++y)
            for (uint32_t x = 0; x < giWidth; ++x)
            {
                const size_t i = size_t(y) * giWidth + x;
                const uint16_t* texel = &smoothRaw[i * 4];
                const float3 got { FromHalf(texel[0]), FromHalf(texel[1]), FromHalf(texel[2]) };
                const float expectedAlpha = filtered.has[i] ? 1.0f : 0.0f;
                alphaWrong += FromHalf(texel[3]) != expectedAlpha ? 1u : 0u;
                const float3 expected = filtered.rgb[i];
                worstFilter = std::max(worstFilter, double(std::max(std::fabs(got.x - expected.x), std::max(std::fabs(got.y - expected.y), std::fabs(got.z - expected.z)))));
            }
        std::printf("gi_warp: the clamp and the filter against the reference, worst %.5f, %u alpha mismatches, the reference clamped %u texels\n", worstFilter, alphaWrong,
                    filtered.clampedTexels);
        CHECK(worstFilter < 0.01);
        CHECK_EQ(alphaWrong, 0u);
        referenceSmoothed = filtered.rgb;
        // An ordinary frame computes no statistics (no staging copy, no readback: they are for the capture frame only).
        CHECK(!pass.HasCaptureStats());
    }

    // ---- the capture frame's two numbers --------------------------------------------------------------------------------------------------
    {
        // The same run with the capture flag set: the pass reads its own half-resolution targets back once and reports how many traced texels the
        // outlier control held back and the mean light the filtered estimate carries. The numbers are the reference's: its count of clamped texels
        // and the mean luminance of its filtered irradiance over the traced texels times the strength key times the gain.
        Inputs capture = inputs;
        capture.captureStats = true;
        ID3D11ShaderResourceView* copy = pass.Run(context.Get(), capture);
        CHECK(copy != nullptr);
        CHECK(pass.HasCaptureStats());
        if (pass.HasCaptureStats())
        {
            const CaptureStats stats = pass.LastCaptureStats();
            double sum = 0.0;
            uint32_t traced = 0;
            for (size_t i = 0; i < filtered.has.size(); ++i)
                if (filtered.has[i])
                {
                    sum += double(filtered.rgb[i].x) * 0.2126 + double(filtered.rgb[i].y) * 0.7152 + double(filtered.rgb[i].z) * 0.0722;
                    ++traced;
                }
            const double expectedLight = sum / double(traced) * double(ClampStrength(inputs.params.strength) * kGain);
            std::printf("gi_warp: capture frame statistics: %u traced texels (the reference %u), %u clamped (the reference %u), mean light %.6f (the reference %.6f)\n", stats.traced,
                        traced, stats.clamped, filtered.clampedTexels, stats.meanLight, expectedLight);
            CHECK_EQ(stats.traced, traced);
            CHECK_EQ(stats.clamped, filtered.clampedTexels);
            CHECK(stats.clamped > 0 && stats.clamped < stats.traced / 10);
            CHECK(std::fabs(stats.meanLight - expectedLight) < 0.02 * expectedLight + 1.0e-6);
        }
        // The next ordinary run clears them again.
        CHECK(pass.Run(context.Get(), inputs) != nullptr);
        CHECK(!pass.HasCaptureStats());
    }

    // ---- the apply: the bounded addition ------------------------------------------------------------------------------------------
    {
        const std::vector<float3> applied = ReadBackCopyRgb(gpu, firstCopy);
        // The scene the pass was handed at a texel, as the fixture wrote it (the half-float round trip included).
        const auto sceneAt = [](uint32_t x, uint32_t y) {
            float3 p;
            uint32_t surface = 0;
            if (!SurfaceAt(x, y, p, surface))
                return 0.25f;
            const float store = surface == 2 ? 36.0f / 255.0f : float(AlbedoStore(x, y)) / 255.0f;
            return FromHalf(HalfOf(store * std::sqrt(surface == 2 ? kSlabLight : kFloorLight)));
        };
        // The sky keeps its own value byte for byte (this fixture's texel 0,0 is the sky's own).
        CHECK_EQ(applied[0].x, 0.25f);
        CHECK_EQ(applied[0].y, 0.25f);
        CHECK_EQ(applied[0].z, 0.25f);
        // No pixel gets darker, and no channel is pushed past the frame's white, whatever the estimate and the strength.
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const float3& texel = applied[size_t(y) * kWidth + x];
                const float scene = sceneAt(x, y);
                CHECK(texel.x >= scene - 1.0e-3f && texel.y >= scene - 1.0e-3f && texel.z >= scene - 1.0e-3f);
                CHECK(texel.x <= kWhite + 1.0e-4f && texel.y <= kWhite + 1.0e-4f && texel.z <= kWhite + 1.0e-4f);
            }
        // What was added, by class: the slab's underside (carried the bright floor's light onto it), the floor beneath the slab (carried
        // the dim slab's), and the open floor beside it.
        float3 px;
        uint32_t which = 0;
        double slabAdd = 0.0, coveredAdd = 0.0, openAdd = 0.0;
        uint32_t slabAdds = 0, coveredAdds = 0, openAdds = 0;
        std::vector<double> slabValues, coveredValues2, openValues2;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                if (!SurfaceAt(x, y, px, which))
                    continue;
                const size_t at = size_t(y) * kWidth + x;
                const double added = double(applied[at].x) - double(sceneAt(x, y));
                if (which == 2)
                {
                    slabAdd += added;
                    slabValues.push_back(added);
                    ++slabAdds;
                    continue;
                }
                const bool covered = px.x >= kSlabX0 - 0.3f && px.x <= kSlabX1 + 0.3f && px.z >= kSlabZ0 && px.z <= kSlabZ1;
                const bool open = NearOpenFloor(px);
                if (covered)
                {
                    coveredAdd += added;
                    coveredValues2.push_back(added);
                    ++coveredAdds;
                }
                else if (open)
                {
                    openAdd += added;
                    openValues2.push_back(added);
                    ++openAdds;
                }
            }
        CHECK(slabAdds > 0 && coveredAdds > 0 && openAdds > 0);
        const double slabMean = slabAdd / double(slabAdds), coveredMean = coveredAdd / double(coveredAdds), openMean = openAdd / double(openAdds);
        std::printf("gi_warp: the apply's mean add: slab underside %.5f over %u, floor beneath it %.5f over %u, open floor %.5f over %u\n", slabMean,
                    slabAdds, coveredMean, coveredAdds, openMean, openAdds);
        // The slab's underside is lit by the bright floor below it: light carried onto a surface no direct light reaches, the effect's point.
        const double slabMedian = Median(slabValues), coveredMedian = Median(coveredValues2), openMedian = Median(openValues2);
        std::printf("gi_warp: the apply's median add: slab underside %.5f, floor beneath it %.5f, open floor %.5f\n", slabMedian, coveredMedian, openMedian);
        // The slab's add against a CPU model of the apply, built from the reference's own filtered estimate: the read between the four surrounding
        // texels (each weighted by its bilinear share and by whether it carries data: a texel the pass could not trace has no light to give), times the
        // slab's albedo store squared (the origin's own multiplier, gate 1 on this generic material), times the strength key (0.5 in this run) and the
        // rules' gain, through the white-bounded stored-domain addition (the rules' BoundedAdd). The bound is the model's own value, not a number picked
        // to pass: a wrong gain, strength, albedo multiplier or read moves the mean off it. The slab's rim, the pixels whose four texels include the
        // sky (nothing traced there), is judged on its own: the plain bilinear read (every texel counts, a texel without data is a zero) would darken
        // it toward the sky, and the rim's model value differs from it by more than the tolerance, so the check below cannot pass for lack of a rim.
        {
            double modelSum = 0.0, gotSum = 0.0, rimModel = 0.0, rimPlain = 0.0, rimGot = 0.0;
            uint32_t modelCount = 0, rimCount = 0;
            const float slabAlbedoSquared = AlbedoSquared(36.0f / 255.0f);
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    if (!SurfaceAt(x, y, px, which) || which != 2)
                        continue;
                    const float cx = (float(x) + 0.5f) / 2.0f - 0.5f, cy = (float(y) + 0.5f) / 2.0f - 0.5f;
                    const int bx = int(std::floor(cx)), by = int(std::floor(cy));
                    const float fx = std::min(std::max(cx - float(bx), 0.0f), 1.0f), fy = std::min(std::max(cy - float(by), 0.0f), 1.0f);
                    const auto texelAt = [&](int ix, int iy) {
                        ix = std::min(std::max(ix, 0), int(giWidth) - 1);
                        iy = std::min(std::max(iy, 0), int(giHeight) - 1);
                        return size_t(iy) * giWidth + size_t(ix);
                    };
                    const size_t taps[4] { texelAt(bx, by), texelAt(bx + 1, by), texelAt(bx, by + 1), texelAt(bx + 1, by + 1) };
                    const float shares[4] { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy };
                    float weighted = 0.0f, weights = 0.0f, plain = 0.0f;
                    bool rim = false;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float w = filtered.has[taps[k]] ? shares[k] : 0.0f;
                        weighted += referenceSmoothed[taps[k]].x * w;
                        weights += w;
                        plain += referenceSmoothed[taps[k]].x * shares[k];
                        rim = rim || !filtered.has[taps[k]];
                    }
                    const float estimate = weights > 0.0f ? std::max(weighted / weights, 0.0f) : 0.0f;
                    const float light = estimate * slabAlbedoSquared * 0.5f * kGain;
                    const double modelAdd = double(BoundedAdd(sceneAt(x, y), light, kWhite));
                    const double gotAdd = double(applied[size_t(y) * kWidth + x].x) - double(sceneAt(x, y));
                    modelSum += modelAdd;
                    gotSum += gotAdd;
                    ++modelCount;
                    if (rim)
                    {
                        rimModel += modelAdd;
                        rimGot += gotAdd;
                        rimPlain += double(BoundedAdd(sceneAt(x, y), std::max(plain, 0.0f) * slabAlbedoSquared * 0.5f * kGain, kWhite));
                        ++rimCount;
                    }
                }
            const double modelMean = modelSum / double(modelCount), gotMean = gotSum / double(modelCount);
            std::printf("gi_warp: the apply on the slab against the CPU model: mean add %.5f, model %.5f over %u texels\n", gotMean, modelMean, modelCount);
            CHECK(modelCount > 100);
            CHECK(modelMean > 0.02); // the slab does get a real add from the checkered floor below it
            CHECK(std::fabs(gotMean - modelMean) < 0.05 * modelMean);
            std::printf("gi_warp: the slab's rim (a texel of the four without data) over %u pixels: mean add %.5f, data-weighted model %.5f, plain bilinear model %.5f\n",
                        rimCount, rimGot / double(std::max(rimCount, 1u)), rimModel / double(std::max(rimCount, 1u)), rimPlain / double(std::max(rimCount, 1u)));
            CHECK(rimCount > 10);
            if (rimCount > 10)
            {
                CHECK(std::fabs(rimGot - rimModel) < 0.05 * rimModel);
                CHECK(std::fabs(rimModel - rimPlain) > 0.08 * rimModel); // the two reads differ here: the rim tells them apart
            }
        }
        CHECK(slabMean > 0.02 && slabMedian > 0.02);
        // The floor beneath the dim slab gets less than the underside gets from the bright floor.
        CHECK(coveredMedian < slabMedian);
        // The open floor has nothing above it to carry light down: most of its texels add next to nothing (the bound is the old one, unchanged).
        // The raw irradiance of this class is
        // not empty (the fixture's coarse rows near the horizon let a ray graze a bright floor texel: 1 texel of 19, printed above), and a tap
        // without data is skipped, so the smooth carries such an isolated texel's value over its whole stencil: the 25 taps of the dense binomial
        // reach farther and in both checkerboard parities than the 9 same-parity taps of the old ring did, which lifts the open floor's median add
        // from 0.0000 to about 0.004. (The old assertion that the covered floor's median is at least the open floor's held only because both medians were exactly 0:
        // the covered floor's own add, from a 0.02 light, is below 1e-4, and its MEAN was already under the open floor's on the old stencil.)
        CHECK(openMedian < 0.005);

        // The strength key's own linearity until the head bound descales it: the default keys' run, then a quarter of the strength.
        inputs.params = BuildParams(4, 16, 5.0f, 0.25f, "half");
        ID3D11ShaderResourceView* quarterCopy = pass.Run(context.Get(), inputs);
        CHECK(quarterCopy != nullptr);
        if (quarterCopy != nullptr)
        {
            const std::vector<float3> quarter = ReadBackCopyRgb(gpu, quarterCopy);
            uint32_t counted = 0;
            double sumAtHalf = 0.0, sumAtQuarter = 0.0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    if (!SurfaceAt(x, y, px, which) || which != 2)
                        continue;
                    const size_t at = size_t(y) * kWidth + x;
                    sumAtHalf += double(applied[at].x) - double(sceneAt(x, y));
                    sumAtQuarter += double(quarter[at].x) - double(sceneAt(x, y));
                    ++counted;
                }
            std::printf("gi_warp: the strength key's adds on the slab, %.5f at 0.5 over %u texels, %.5f at 0.25\n", sumAtHalf, counted, sumAtQuarter);
            CHECK(counted > 0);
            // The key scales the LINEAR light and the stored increment is a square-root law of it for a dark pixel (linear for a bright one),
            // so a quarter of the key lands between 0.4 and 0.9 of the half's increment: more than a linear law's half, less than all of it.
            CHECK(sumAtHalf > 1.0e-3);
            if (sumAtHalf > 1.0e-3)
            {
                const double ratio = sumAtQuarter / sumAtHalf;
                CHECK(ratio > 0.4 && ratio < 0.9);
            }
        }
    }

    // ---- the lamp: an emissive surface bounces its own light, bounded by the clamp -----------------------------------------------------
    {
        // Three runs of the same pass on the same geometry with the lamp band (a strip of the slab's underside) at a stored 0 (no lamp: the slab's
        // own dim value), 2.5 (linear 6.25, under the clamp) and 3.2 (linear 10.24, over it). The floor under the band is the observer.
        struct LampRun
        {
            std::vector<float3> rgb;
            std::vector<float> alpha;
            double meanError = 0.0;   // mean over the pass texels of the worst channel's distance from the reference
            double badShare = 0.0;    // the share of texels more than 0.05 from the reference (a grazing ray's flip is a few texels)
            uint32_t alphaMismatches = 0;
            double observerMax = 0.0; // the largest red under the lamp band
        };
        const auto runWith = [&](float lampStored) {
            g_lampStored = lampStored;
            gpu.Fill(fixture);
            LampRun run;
            ID3D11ShaderResourceView* copy = pass.Run(context.Get(), inputs);
            CHECK(copy != nullptr);
            const std::vector<uint16_t> raw = gpu.ReadBack(gpu.TextureOf(pass.IrradianceView()).Get());
            run.rgb.resize(size_t(giWidth) * giHeight);
            run.alpha.resize(size_t(giWidth) * giHeight);
            uint32_t bad = 0;
            for (uint32_t y = 0; y < giHeight; ++y)
                for (uint32_t x = 0; x < giWidth; ++x)
                {
                    const size_t i = size_t(y) * giWidth + x;
                    const RefResult ref = ReferenceIrradiance(fixture, int2 { int(x), int(y) }, inputs.params.rays, inputs.params.steps, inputs.params.range, 2.0f);
                    const uint16_t* t = &raw[i * 4];
                    const float3 got { FromHalf(t[0]), FromHalf(t[1]), FromHalf(t[2]) };
                    run.rgb[i] = got;
                    run.alpha[i] = FromHalf(t[3]);
                    const float worst = std::max(std::fabs(got.x - ref.rgb.x), std::max(std::fabs(got.y - ref.rgb.y), std::fabs(got.z - ref.rgb.z)));
                    run.meanError += double(worst);
                    bad += worst > 0.05f ? 1u : 0u;
                    run.alphaMismatches += run.alpha[i] != float(ref.traced) ? 1u : 0u;
                    const float2 under { (float(x) + 0.5f) * 2.0f, (float(y) + 0.5f) * 2.0f };
                    const uint32_t ux = under.x < float(kWidth) ? uint32_t(under.x) : kWidth - 1, uy = under.y < float(kHeight) ? uint32_t(under.y) : kHeight - 1;
                    float3 p;
                    uint32_t which = 0;
                    if (SurfaceAt(ux, uy, p, which) && which == 1 && p.x >= kLampX0 - 0.3f && p.x <= kLampX1 + 0.3f && p.z >= kSlabZ0 && p.z <= kSlabZ1)
                        run.observerMax = std::max(run.observerMax, double(got.x));
                }
            run.meanError /= double(size_t(giWidth) * giHeight);
            run.badShare = double(bad) / double(size_t(giWidth) * giHeight);
            return run;
        };
        const LampRun dark = runWith(0.0f);
        const LampRun lit = runWith(2.5f);
        const LampRun over = runWith(3.2f);
        g_lampStored = 0.0f;
        gpu.Fill(fixture); // the fixture as the main run had it
        double overVsDark = 0.0;
        uint32_t overAlphaChanges = 0;
        for (size_t i = 0; i < dark.rgb.size(); ++i)
        {
            overVsDark = std::max(overVsDark, double(std::fabs(over.rgb[i].x - dark.rgb[i].x)));
            overAlphaChanges += over.alpha[i] != dark.alpha[i] ? 1u : 0u;
        }
        std::printf("gi_warp: lamp runs, floor under the band: largest irradiance %.4f with no lamp, %.4f with a lamp at stored 2.5 (linear 6.25), %.4f with one at 3.2 "
                    "(linear 10.24, over the clamp); against the reference mean %.5f / %.5f / %.5f, texels off by more than 0.05: %.4f / %.4f / %.4f; the refused lamp differs "
                    "from no lamp by at most %.5f, alpha changes %u\n",
                    dark.observerMax, lit.observerMax, over.observerMax, dark.meanError, lit.meanError, over.meanError, dark.badShare, lit.badShare, over.badShare,
                    overVsDark, overAlphaChanges);
        CHECK(dark.meanError < kIrradianceEps && lit.meanError < kIrradianceEps && over.meanError < kIrradianceEps);
        CHECK(dark.badShare < 0.01 && lit.badShare < 0.01 && over.badShare < 0.01);
        CHECK_EQ(dark.alphaMismatches, 0u);
        CHECK_EQ(lit.alphaMismatches, 0u);
        CHECK_EQ(over.alphaMismatches, 0u);
        // A lamp sends its own light back: the floor under it now reads far more than the dim slab gave it, and no more than a hemisphere full of
        // the lamp's own radiance (pi * 6.25): the clamp and the estimator keep it bounded.
        CHECK(dark.observerMax < 0.05);
        CHECK(lit.observerMax > 1.5);
        CHECK(lit.observerMax <= double(kPi) * 6.25 * 1.01 + 0.01);
        // A lamp over the clamp is refused, not scaled down: the floor under it reads what it read with no lamp, and the origins stay traced.
        CHECK(overVsDark < 0.01);
        CHECK_EQ(overAlphaChanges, 0u);
        CHECK(over.observerMax < 0.05);
    }

    // ---- the engine's special material rows get no bounce (the apply reads the id byte from the normal view) ------------------------------
    {
        // The same pass on the same geometry with a band of the slab's underside marked as one of the engine's special rows (id 40, in 32..44): its
        // pixels keep the scene's own value exactly, while the generic pixels beside them get the bounce as before. The first version never bound the
        // normal view for the apply, so it read id 0 everywhere and the exclusion never applied.
        Inputs special = inputs;
        special.params = BuildParams(4, 16, 5.0f, 0.5f, "half");
        g_specialId = 40;
        gpu.Fill(fixture);
        ID3D11ShaderResourceView* copy = pass.Run(context.Get(), special);
        CHECK(copy != nullptr);
        double bandAdd = 0.0, besideAdd = 0.0;
        uint32_t bandCount = 0, besideCount = 0;
        if (copy != nullptr)
        {
            const std::vector<float3> out = ReadBackCopyRgb(gpu, copy);
            float3 p;
            uint32_t surface = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    if (!SurfaceAt(x, y, p, surface) || surface != 2)
                        continue;
                    const float scene = FromHalf(HalfOf((36.0f / 255.0f) * std::sqrt(kSlabLight)));
                    const double added = double(out[size_t(y) * kWidth + x].x) - double(scene);
                    if (fixture.SpecialAt(int2 { int(x), int(y) }))
                    {
                        bandAdd += added;
                        ++bandCount;
                    }
                    else if (p.x >= kSpecialX1 + 0.05f || p.x <= kSpecialX0 - 0.05f)
                    {
                        besideAdd += added;
                        ++besideCount;
                    }
                }
        }
        g_specialId = 0;
        gpu.Fill(fixture);
        std::printf("gi_warp: special rows: the band of %u slab pixels adds %.6f in all, the generic pixels beside it (%u) add %.5f on average\n", bandCount, bandAdd, besideCount,
                    besideAdd / double(std::max(besideCount, 1u)));
        CHECK(bandCount > 20 && besideCount > 40);
        CHECK(bandAdd == 0.0);
        CHECK(besideAdd / double(std::max(besideCount, 1u)) > 0.02);
    }

    // ---- the calibrated bounce: a lamp-lit surface bleeds visibly onto its neighbour at the default strength ---------------------------------
    {
        // A lamp (a band of the slab's underside lit to a stored 2.5) over the checkered floor, the pass at the DEFAULT keys. The floor under the lamp
        // is the neighbour that gets the bounce, the open floor beside the slab, which sees only the sky, rises by nothing (a flat open area is not
        // lifted). The fixture's lamp is a band of a few texels on a 64x48 grid, so the filter's 31-texel footprint dilutes it: the lift under it is a
        // fraction of a percent here (the user's bright room lifts its lamp wall 7.1 % at the same default, see the report). What is decided: the
        // default lifts the neighbour, a flat open area stays under 1 %, and the strength is the knob (a stronger setting, the writer's 0.70, lifts
        // the same neighbour more; 0.70 / 0.55 = 1.27 in a linear law). The default is 0.55 since the 2026-11-07 integration (the rain square's
        // speckle bar); "older" below is the stronger comparison setting.
        Inputs lit = inputs;
        lit.params = BuildParams(4, 16, 5.0f, kDefaultStrength, "half");
        Inputs older = inputs;
        older.params = BuildParams(4, 16, 5.0f, 0.70f, "half");
        g_lampStored = 2.5f;
        gpu.Fill(fixture);
        const auto liftOf = [&](const Inputs& run, double& underLamp, double& openFloor) {
            ID3D11ShaderResourceView* copy = pass.Run(context.Get(), run);
            CHECK(copy != nullptr);
            underLamp = openFloor = 0.0;
            if (copy == nullptr)
                return;
            const std::vector<float3> out = ReadBackCopyRgb(gpu, copy);
            double underSum = 0.0, underScene = 0.0, openSum = 0.0, openScene = 0.0;
            float3 p;
            uint32_t surface = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    if (!SurfaceAt(x, y, p, surface) || surface != 1)
                        continue;
                    const float scene = fixture.SceneStoredAt(int2 { int(x), int(y) });
                    const double added = double(out[size_t(y) * kWidth + x].x) - double(scene);
                    if (p.x >= kLampX0 - 0.3f && p.x <= kLampX1 + 0.3f && p.z >= kSlabZ0 && p.z <= kSlabZ1)
                    {
                        underSum += added;
                        underScene += double(scene);
                    }
                    else if (NearOpenFloor(p))
                    {
                        openSum += added;
                        openScene += double(scene);
                    }
                }
            underLamp = underSum / std::max(underScene, 1.0e-9);
            openFloor = openSum / std::max(openScene, 1.0e-9);
        };
        double underDefault = 0.0, openDefault = 0.0, underOlder = 0.0, openOlder = 0.0;
        liftOf(lit, underDefault, openDefault);
        liftOf(older, underOlder, openOlder);
        g_lampStored = 0.0f;
        gpu.Fill(fixture);
        std::printf("gi_warp: the calibrated bounce, floor under the lamp: stored luminance +%.3f %% at the default %.2f (+%.3f %% at 0.70); the open floor +%.4f %% (%.4f %%)\n",
                    underDefault * 100.0, double(kDefaultStrength), underOlder * 100.0, openDefault * 100.0, openOlder * 100.0);
        CHECK(underDefault > 0.001);                   // the lamp bleeds onto the floor under it at the default (diluted by the filter's footprint here)
        CHECK(underOlder > underDefault * 1.2);        // the strength is the knob: 0.70 lifts the same neighbour more than the default 0.55
        CHECK(openDefault < 0.01 && openOlder < 0.01); // and a flat open area stays under 1 %
    }

    // ---- a pass that cannot read its inputs returns null with a reason ------------------------------------------------------------
    {
        Inputs wrong = inputs;
        wrong.depth = nullptr;
        CHECK_EQ(pass.Run(context.Get(), wrong), nullptr);
        CHECK(pass.Failure() != nullptr);
    }

    // ---- the pass's own targets, for the test that pinned the march (their sizes are the pass's own resolution) --------------------
    CHECK(pass.IrradiancesCreated() >= 1);
    CHECK(pass.HasGpuMilliseconds() == false || pass.GpuMilliseconds() > 0.0);
    CHECK_EQ(pass.GpuTimingBegins(), pass.GpuTimingEnds()); // a run ends the pair it began, on the test's device

    return Finish("gi_warp");
}
