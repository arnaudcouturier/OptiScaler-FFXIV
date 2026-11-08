// Screen-space reflections: the pass (ext/ffxiv/engine/SsrPass.cpp and its shaders) on the D3D11 WARP software adapter, on synthetic buffers this
// test builds itself, against a CPU reference of the same march.
//
// The camera table is the engine's real layout (the projection's four rows at register 18), and the scene is geometry this test places in view
// space, the way the engine's own view-position buffer holds it: a floor at y = -1, a wall at z = -8 standing on it, and a bright square on the
// wall. The scene colour is built from the same geometry (the square's colour where the wall has it, a floor and wall tone elsewhere, black in
// the sky), so a reflection of the square is a colour only that square has.
//
// What is checked is what a wrong march, a wrong mapping or a wrong blend would break:
//   the floor below the bright square shows the square's colour, with a confidence above one half,
//   a texel whose ray leaves the picture keeps the engine's own texel byte for byte, also in a window placed away from the allocation's origin
//   whose texels outside it hold a surface the picture does not show (a march that kept going could reflect it),
//   a camera under the water plane: the scene above the camera matches its reference,
//   and the two rules a picture cannot separate from their absence - the plane's turn to the camera's side and the march's stop where the ray leaves
//   the window - are pinned at their statements.
//   a blend of 0 is the engine's map, byte for byte, and a floor with nothing above it is too,
//   the pass matches a CPU reference of the same march on the same buffers, texel by texel within a tolerance and in the mean,
//   a scene allocation twice the planar map's: the pass computes the quadrant the engine's own producer fills and every texel outside it keeps the
//   engine's own, all four channels,
//   two runs of the same scene are byte for byte the same picture, and a second run at the same size creates no new targets while a size
//   change creates a new pair,
//   the timing pair is begun by every run that can time itself and ended only by the run that began it,
//   a run the pure rules refuse returns null, so the caller keeps the engine's own texture,
//   the slots stamped on a shader object are read back unchanged after thousands of other shaders are created, and a shader that was never
//   stamped reads the empty word.
#include "check.h"

#include "ext/ffxiv/engine/EnginePass.h" // the two seams the reflections write through, and the feature bit's own module
#include "ext/ffxiv/engine/SsrPass.h"
#include "ext/ffxiv/engine/SsrApply.h"
#include "ext/ffxiv/engine/SsrNativeContext.h"
#include "ext/present/PresentSeams.h" // the output-frame counter the module's frame memory is ticked by
#include "ext/core/ExtConfig.h"
#include "ext/core/InjectedWorkDiagnostic.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <filesystem>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <process.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::ssr;
namespace ao = ext::ffxiv::engine::ao; // the mapper next door (SsrRules.h maps with the ambient occlusion pass's own rule)

// ---- the module's seams, which this device test drives ----
//
// The reflections' draw path (SsrApply.cpp) writes through two seams of the engine pass overrides module and reads the output-frame counter. That
// module is a text-pinned wiring with its own tests, so the three seams are defined here: they record what the reflections asked for and let the
// draw path run for real on this test's device.
namespace ext::ffxiv::engine
{
int g_ssrTestPublishCalls = 0;
bool g_ssrTestOwnWork = false;

void Publish(bool) { ++g_ssrTestPublishCalls; }
void SetOwnWork(bool on) { g_ssrTestOwnWork = on; }
} // namespace ext::ffxiv::engine

namespace ext::present
{
uint64_t g_ssrTestFrame = 1; // the frame token the tests advance between draws
uint64_t FrameCount() noexcept { return g_ssrTestFrame; }
} // namespace ext::present

namespace ext::ffxiv::chara::probe::dump
{
std::filesystem::path OutputRoot() { return std::filesystem::temp_directory_path() / (L"ssr-pair-test-" + std::to_wstring(_getpid())); }
} // namespace ext::ffxiv::chara::probe::dump

namespace
{
constexpr uint32_t kWindow = 64;      // the render window's edge, and the planar map's, in texels
constexpr uint32_t kScene = 128;      // the scene allocation's edge: twice the window, the session's own shape at a smaller size
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kFovDegrees = 60.0f;
constexpr float kPi = 3.14159265358979f;
constexpr float kFloorY = -1.0f;
constexpr float kWallZ = -8.0f;
// The bright square on the wall, in view-space metres: it sits above the floor's horizon, so the picture has it in its upper half.
constexpr float kSquareLowX = -2.0f;
constexpr float kSquareHighX = 2.0f;
constexpr float kSquareLowY = 0.0f;
constexpr float kSquareHighY = 2.0f;
// The three colours the scene is built from. Only the square's is bright, so a colour read back at a floor texel can only come from a reflection
// of the square and not from the floor's own tone.
constexpr float kSquareColour[3] { 0.95f, 0.85f, 0.10f };
constexpr float kWallColour[3] { 0.20f, 0.22f, 0.25f };
constexpr float kFloorColour[3] { 0.10f, 0.12f, 0.10f };
constexpr uint32_t kEngineAlpha = 0xA5;

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

// The projection of a right-handed view space looking down -z, reversed the way the game's depth is: 1 at the near plane, 0 at the far plane. The
// same matrix tests/ext/ffxiv/engine's ambient occlusion reference uses, so the two fixtures agree on the camera.
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
float3 Mul(const float3& a, float s) { return Make3(a.x * s, a.y * s, a.z * s); }
float Dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 Cross(const float3& a, const float3& b) { return Make3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float Length(const float3& a) { return std::sqrt(Dot(a, a)); }
float3 Normalized(const float3& a)
{
    const float length = Length(a);
    return length > 0.0f ? Mul(a, 1.0f / length) : a;
}
float Saturate(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }
float3 Saturate3(const float3& v) { return Make3(Saturate(v.x), Saturate(v.y), Saturate(v.z)); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// One frame of the fixture: the buffers behind the planar map, and the tuning that produced them.
struct Frame
{
    // The scene allocation's texels, in texel order: the view-space position and the colour of every pixel. A texel the picture does not cover is
    // the sky: a zero position and a black colour, which is what the engine's own buffers hold there.
    std::vector<float> position; // kScene * kScene * 3
    std::vector<float> colour;   // kScene * kScene * 3
    float engineValue = 0.5f;    // the value in the engine's own planar texel
    float tiltDegrees = 0.0f;    // a roll of the camera: the water plane is the world's up seen in view space, and a rolled camera tilts it
    ReaderFacts facts;
    Params params;
};

uint8_t ByteOf(float value) { return uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, value)) * 255.0f)); }

// float -> half -> float, so a value the fixture writes into a half-float texture is the value its CPU reference reads back: without this the two
// sides of the comparison would differ by the storage's rounding, which at a knife-edge crossing is a texel away.
float HalfRound(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    uint16_t packed = uint16_t(sign);
    if (exponent <= 0)
        packed = uint16_t(sign); // this fixture's values are ordinary: a denormal reads as zero
    else if (exponent >= 31)
        packed = uint16_t(sign | 0x7C00u);
    else
        packed = uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
    // and back again
    const uint32_t backSign = (uint32_t(packed) & 0x8000u) << 16;
    const uint32_t backExponent = (uint32_t(packed) >> 10) & 0x1Fu;
    const uint32_t backMantissa = uint32_t(packed) & 0x3FFu;
    uint32_t back = backSign;
    if (backExponent != 0 && backExponent != 31)
        back |= ((backExponent + 127 - 15) << 23) | (backMantissa << 13);
    float answer = 0.0f;
    std::memcpy(&answer, &back, sizeof answer);
    return answer;
}

// The engine's own planar map's four bytes at one texel, in B8G8R8A8 memory order: red is the value the readers sample, and blue and green are a
// pattern of the texel's own position, so a copy that landed shifted is a test failure instead of a picture that looks right.
void EngineBytes(uint32_t x, uint32_t y, float value, uint8_t* out)
{
    out[0] = uint8_t(y & 0xFF);
    out[1] = uint8_t(x & 0xFF);
    out[2] = ByteOf(value);
    out[3] = uint8_t(kEngineAlpha);
}

// The scene: a floor at y = -1 and, when `wall` is set, a wall at z = -8 standing on it - and, when `square` is set, the bright square on that wall.
// The picture is the window, which sits at `windowX`,`windowY` in the allocation the way the session renders 1920x1080 inside a 3840x2160
// allocation. `outsideDepth` fills every texel the window does not cover with a screen-parallel plane that many metres from the camera: a surface
// the picture does not show, which is what makes "the ray left the picture" a fixture with something to find outside it.
Frame BuildScene(bool wall, bool square, uint32_t windowX = 0, uint32_t windowY = 0, float outsideDepth = 0.0f)
{
    Frame frame;
    frame.position.assign(size_t(kScene) * kScene * 3, 0.0f);
    frame.colour.assign(size_t(kScene) * kScene * 3, 0.0f);

    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    for (uint32_t y = 0; y < kWindow; ++y)
        for (uint32_t x = 0; x < kWindow; ++x)
        {
            const float ndcX = (float(x) + 0.5f) / float(kWindow) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (float(y) + 0.5f) / float(kWindow) * 2.0f;
            const float3 ray = Make3(ndcX * tanY, ndcY * tanY, -1.0f);

            float distance = 0.0f;
            float3 normal = Make3(0.0f, 1.0f, 0.0f);
            const float* colour = kFloorColour;
            // The floor: ray.y * distance = kFloorY, for the rays that point down.
            if (ray.y < -1e-6f)
                distance = kFloorY / ray.y;
            // The wall: ray.z * distance = kWallZ, valid only above the floor.
            if (wall && std::fabs(ray.z) > 1e-6f)
            {
                const float wallDistance = kWallZ / ray.z;
                if (wallDistance > 0.0f && (distance <= 0.0f || wallDistance < distance))
                {
                    const float height = ray.y * wallDistance;
                    if (height >= kFloorY)
                    {
                        distance = wallDistance;
                        normal = Make3(0.0f, 0.0f, 1.0f); // the wall's visible face points back at the camera
                        colour = kWallColour;
                        const float worldX = ray.x * wallDistance;
                        if (square && worldX >= kSquareLowX && worldX <= kSquareHighX && height >= kSquareLowY && height <= kSquareHighY)
                            colour = kSquareColour;
                    }
                }
            }
            if (distance <= 0.0f)
                continue; // the sky: no position and no colour, and the pass has nothing to reflect from there

            const size_t index = (size_t(y + windowY) * kScene + x + windowX) * 3;
            frame.position[index + 0] = HalfRound(ray.x * distance);
            frame.position[index + 1] = HalfRound(ray.y * distance);
            frame.position[index + 2] = HalfRound(ray.z * distance);
            for (size_t c = 0; c < 3; ++c)
                frame.colour[index + c] = HalfRound(colour[c]);
        }

    if (outsideDepth > 0.0f)
    {
        // A screen-parallel plane in every texel outside the window: the surface a march that kept going past the window's edge would sample, whose
        // stored point is exactly where the camera ray through that texel meets the plane. The picture's own texels stay the picture.
        for (uint32_t y = 0; y < kScene; ++y)
            for (uint32_t x = 0; x < kScene; ++x)
            {
                if (x >= windowX && x < windowX + kWindow && y >= windowY && y < windowY + kWindow)
                    continue;
                const float ndcX = (float(x) - float(windowX) + 0.5f) / float(kWindow) * 2.0f - 1.0f;
                const float ndcY = 1.0f - (float(y) - float(windowY) + 0.5f) / float(kWindow) * 2.0f;
                const float3 ray = Make3(ndcX * tanY, ndcY * tanY, -1.0f);
                const size_t index = (size_t(y) * kScene + x) * 3;
                frame.position[index + 0] = HalfRound(ray.x * outsideDepth);
                frame.position[index + 1] = HalfRound(ray.y * outsideDepth);
                frame.position[index + 2] = HalfRound(ray.z * outsideDepth);
                for (size_t c = 0; c < 3; ++c)
                    frame.colour[index + c] = HalfRound(kWallColour[c]);
            }
    }

    frame.facts.hasPlanar = true;
    frame.facts.hasScene = true;
    frame.facts.hasVPosition = true;
    frame.facts.hasCamera = true;
    frame.facts.planarFormat = kPlanarFormat;
    frame.facts.planarWidth = kWindow;
    frame.facts.planarHeight = kWindow;
    frame.facts.planarSamples = 1;
    frame.facts.sceneFormat = kSceneFormat;
    frame.facts.sceneWidth = kScene;
    frame.facts.sceneHeight = kScene;
    frame.facts.sceneSamples = 1;
    frame.facts.vpositionFormat = kVPositionFormat;
    frame.facts.vpositionWidth = kScene;
    frame.facts.vpositionHeight = kScene;
    frame.facts.vpositionSamples = 1;
    frame.facts.cameraBytes = kCameraBytes;
    frame.facts.window[0] = float(windowX);
    frame.facts.window[1] = float(windowY);
    frame.facts.window[2] = float(kWindow);
    frame.facts.window[3] = float(kWindow);
    frame.params.on = true;
    frame.params.steps = 48;
    frame.params.thickness = 0.3f;
    frame.params.blend = 1.0f;
    frame.params.fade = 0.1f;
    return frame;
}

// ---- the CPU reference: the same march, texel by texel ----------------------------------------------------------------

struct Reference
{
    const Frame& frame;

    // One row of g_CameraParameter's view matrix (registers 0..2): the roll about the view axis that the frame asks for.
    float3 CameraRow(int row) const
    {
        const float roll = frame.tiltDegrees * kPi / 180.0f;
        const float c = std::cos(roll);
        const float s = std::sin(roll);
        if (row == 0)
            return Make3(c, -s, 0.0f);
        if (row == 1)
            return Make3(s, c, 0.0f);
        return Make3(0.0f, 0.0f, 1.0f);
    }

    // A world-space point put through the view matrix the frame uploads, row by row: the same multiplication the shader's projection uses.
    float3 TransformPoint(const float3& point) const
    {
        return Make3(Dot(CameraRow(0), point), Dot(CameraRow(1), point), Dot(CameraRow(2), point));
    }

    // The water plane's normal, derived without the shader's column extraction: three points of the world's ground plane (y = 0) are put through
    // the same view matrix and the plane's normal is the cross product of two of its edges. A camera roll turns the ground plane in view space, so
    // this is the one place the roll can move the picture - and an extraction that read another row or another component of the camera table would
    // disagree with this, which is what makes the assertion about the transform and not about a number repeated twice.
    float3 PlaneNormal() const
    {
        const float3 origin = TransformPoint(Make3(0.0f, 0.0f, 0.0f));
        const float3 alongZ = TransformPoint(Make3(0.0f, 0.0f, 1.0f));
        const float3 alongX = TransformPoint(Make3(1.0f, 0.0f, 0.0f));
        return Normalized(Cross(Sub(alongZ, origin), Sub(alongX, origin)));
    }

    float3 At(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= int(kScene) || y >= int(kScene))
            return Make3(0.0f, 0.0f, 0.0f);
        const size_t index = (size_t(y) * kScene + size_t(x)) * 3;
        return Make3(frame.position[index + 0], frame.position[index + 1], frame.position[index + 2]);
    }

    float3 ColourAt(int x, int y) const
    {
        const size_t index = (size_t(y) * kScene + size_t(x)) * 3;
        return Make3(frame.colour[index + 0], frame.colour[index + 1], frame.colour[index + 2]);
    }

    // The scene's colour at the crossing's own fraction: the four texels the point sits between, weighted by the fractional part — the same manual
    // bilinear SceneMarchColour draws, with the taps clamped into the render window the way the shader's are (the copy holds the window's texels
    // only). The reference reads its own float colour buffer, so no half-float round trip stands between the two sides here.
    float3 ColourAtFraction(float projectedX, float projectedY) const
    {
        const float ax = projectedX - 0.5f;
        const float ay = projectedY - 0.5f;
        const float baseX = std::floor(ax);
        const float baseY = std::floor(ay);
        const float weightX = ax - baseX;
        const float weightY = ay - baseY;
        const int lowX = int(frame.facts.window[0]);
        const int lowY = int(frame.facts.window[1]);
        const int highX = lowX + int(frame.facts.window[2]) - 1;
        const int highY = lowY + int(frame.facts.window[3]) - 1;
        const int i0x = std::min(std::max(int(baseX), lowX), highX);
        const int i0y = std::min(std::max(int(baseY), lowY), highY);
        const int i1x = std::min(i0x + 1, highX);
        const int i1y = std::min(i0y + 1, highY);
        const float w00 = (1.0f - weightX) * (1.0f - weightY);
        const float w10 = weightX * (1.0f - weightY);
        const float w01 = (1.0f - weightX) * weightY;
        const float w11 = weightX * weightY;
        const float3 c00 = ColourAt(i0x, i0y);
        const float3 c10 = ColourAt(i1x, i0y);
        const float3 c01 = ColourAt(i0x, i1y);
        const float3 c11 = ColourAt(i1x, i1y);
        return Make3(w00 * c00.x + w10 * c10.x + w01 * c01.x + w11 * c11.x, w00 * c00.y + w10 * c10.y + w01 * c01.y + w11 * c11.y,
                     w00 * c00.z + w10 * c10.z + w01 * c01.z + w11 * c11.z);
    }

    // The crossing's own window coordinate: the same value the march hands the shader for the colour read. `inside` says whether the point projects
    // into the render window at all (behind the camera and outside the picture both answer false).
    bool ProjectFraction(const float3& point, float& projectedX, float& projectedY) const
    {
        const Mat4 projection = Projection();
        const float clipX = projection.m[0][0] * point.x + projection.m[0][1] * point.y + projection.m[0][2] * point.z + projection.m[0][3];
        const float clipY = projection.m[1][0] * point.x + projection.m[1][1] * point.y + projection.m[1][2] * point.z + projection.m[1][3];
        const float clipW = projection.m[3][0] * point.x + projection.m[3][1] * point.y + projection.m[3][2] * point.z + projection.m[3][3];
        if (!(clipW > 0.0f))
            return false;
        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float* w = frame.facts.window; // x, y, width, height
        projectedX = w[0] + (ndcX * 0.5f + 0.5f) * w[2];
        projectedY = w[1] + (0.5f - ndcY * 0.5f) * w[3];
        return projectedX >= w[0] && projectedY >= w[1] && projectedX < w[0] + w[2] && projectedY < w[1] + w[3];
    }

    // The scene texel a view-space point would be drawn at, or `inside = false` when it is outside the render window (the shader's SceneTexel).
    bool Project(const float3& point, int& texelX, int& texelY) const
    {
        const Mat4 projection = Projection();
        const float clipX = projection.m[0][0] * point.x + projection.m[0][1] * point.y + projection.m[0][2] * point.z + projection.m[0][3];
        const float clipY = projection.m[1][0] * point.x + projection.m[1][1] * point.y + projection.m[1][2] * point.z + projection.m[1][3];
        const float clipW = projection.m[3][0] * point.x + projection.m[3][1] * point.y + projection.m[3][2] * point.z + projection.m[3][3];
        if (!(clipW > 0.0f))
            return false;
        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float* w = frame.facts.window; // x, y, width, height
        const float projectedX = w[0] + (ndcX * 0.5f + 0.5f) * w[2];
        const float projectedY = w[1] + (0.5f - ndcY * 0.5f) * w[3];
        if (!(projectedX >= w[0]) || !(projectedY >= w[1]) || !(projectedX < w[0] + w[2]) || !(projectedY < w[1] + w[3]))
            return false;
        texelX = int(std::floor(projectedX));
        texelY = int(std::floor(projectedY));
        if (texelX < 0 || texelY < 0 || texelX >= int(frame.facts.sceneWidth) || texelY >= int(frame.facts.sceneHeight))
            return false;
        return true;
    }

    // Why the march stopped, for the texel the test picks: kHit, kLeftPicture (the ray ran out of the window, which is what keeps the engine's
    // texel) or kNothing (the ray never crossed anything before its steps ran out).
    enum Stop
    {
        kHit = 0,
        kLeftPicture = 1,
        kNothing = 2,
    };

    // What the shader writes at one planar texel, in the four channels' order (red, green, blue, alpha) as floats.
    void Texel(uint32_t ownX, uint32_t ownY, float out[4], int* stop = nullptr) const
    {
        if (stop != nullptr)
            *stop = kNothing;
        uint8_t engine[4] {};
        EngineBytes(ownX, ownY, frame.engineValue, engine);
        out[0] = float(engine[2]) / 255.0f; // red
        out[1] = float(engine[1]) / 255.0f; // green
        out[2] = float(engine[0]) / 255.0f; // blue
        out[3] = float(engine[3]) / 255.0f; // alpha

        const ao::Region region = RegionOf(frame.facts);
        if (int(ownX) < int(region.x) || int(ownY) < int(region.y) || int(ownX) >= int(region.Right()) || int(ownY) >= int(region.Bottom()))
            return;

        const ao::Mapping mapping = MappingOf(frame.facts);
        const int atX = int(std::floor(float(ownX) * mapping.scaleX));
        const int atY = int(std::floor(float(ownY) * mapping.scaleY));
        if (atX >= int(frame.facts.sceneWidth) || atY >= int(frame.facts.sceneHeight))
            return;
        const float3 P = At(atX, atY);
        if (!(Dot(P, P) > 1e-6f))
            return; // the sky

        // The water plane's normal, as the shader reads it: the world's up in view space. The reference takes it from the ground plane put through
        // the same view matrix (PlaneNormal), not from the column the shader extracts, so the two sides of the comparison are independent.
        float3 N = PlaneNormal();
        if (!(Dot(N, N) > 1e-12f))
            return;
        N = Normalized(N);
        if (Dot(N, P) > 0.0f)
            N = Mul(N, -1.0f);

        const float3 V = Normalized(P);
        float3 R = Sub(V, Mul(N, 2.0f * Dot(V, N)));
        if (std::isnan(R.x) || std::isnan(R.y) || std::isnan(R.z))
            return;

        const int steps = int(frame.params.steps);
        const float thickness = frame.params.thickness;
        const float surfaceBias = 0.02f;
        const float maxDistance = 100.0f;
        const float range = maxDistance - surfaceBias;

        float t = surfaceBias;
        float previous = 0.0f;
        bool haveFront = false;
        int hitX = -1, hitY = -1;
        float hitFX = 0.0f, hitFY = 0.0f;
        bool found = false;
        for (int k = 0; k < steps; ++k)
        {
            // The shader's quadratic ramp: t = bias + range * u * u with u = (k + 1) / steps.
            const float u = float(k + 1) / float(steps);
            const float next = surfaceBias + range * u * u;
            if (next > maxDistance)
                break;
            int texelX = 0, texelY = 0;
            if (!Project(Add(P, Mul(R, next)), texelX, texelY))
            {
                if (stop != nullptr)
                    *stop = kLeftPicture;
                break; // the ray left the picture
            }
            const float3 Q = At(texelX, texelY);
            if (!(Dot(Q, Q) > 1e-6f))
            {
                previous = next;
                haveFront = false;
                t = next;
                continue; // the sky along the ray
            }
            const float crossed = Dot(Sub(Q, P), R);
            if (crossed <= 0.0f)
            {
                previous = next;
                haveFront = false;
                t = next;
                continue;
            }
            if (crossed >= next)
            {
                previous = next;
                haveFront = true;
                t = next;
                continue; // in front of the surface: keep going
            }
            // The crossing sits between the last step that had not crossed anything and this one.
            float loEdge = haveFront && previous > 0.0f ? previous : std::fmax(t, 0.0f);
            float hiEdge = next;
            int refinedX = texelX, refinedY = texelY;
            for (int r = 0; r < 6; ++r) // the shader's kMaxRefine
            {
                const float mid = 0.5f * (loEdge + hiEdge);
                int midX = 0, midY = 0;
                if (!Project(Add(P, Mul(R, mid)), midX, midY))
                    break;
                const float midCrossed = Dot(Sub(At(midX, midY), P), R);
                if (midCrossed > 0.0f && midCrossed <= mid)
                {
                    hiEdge = mid;
                    refinedX = midX;
                    refinedY = midY;
                }
                else
                {
                    loEdge = mid;
                }
            }
            // The thickness heuristic at the refined point: a surface farther behind the crossing than this is another object. The crossing's own
            // window coordinate goes with the texel, for the colour read at the fraction (the shader's SceneMarchColour).
            float hitProjectedX = float(refinedX) + 0.5f, hitProjectedY = float(refinedY) + 0.5f;
            {
                float px = 0.0f, py = 0.0f;
                if (ProjectFraction(Add(P, Mul(R, hiEdge)), px, py))
                {
                    hitProjectedX = px;
                    hitProjectedY = py;
                }
            }
            const float gap = hiEdge - Dot(Sub(At(refinedX, refinedY), P), R);
            if (gap <= thickness)
            {
                // The self-graze rejection, the shader's own: a hit this close that lands a texel or two from its own origin is the ray grazing the
                // surface it left (the view-position buffer holds the ground under the water, so the ray starts inside it). The engine's texel
                // answers instead, which is the reference's `found = false` answer here.
                const bool selfGraze = hiEdge < kSelfGrazeMetres &&
                                       std::fmax(std::fabs(float(refinedX) - float(atX)), std::fabs(float(refinedY) - float(atY))) <= float(kSelfGrazeTexels);
                if (!selfGraze)
                {
                    hitX = refinedX;
                    hitY = refinedY;
                    hitFX = hitProjectedX;
                    hitFY = hitProjectedY;
                    found = true;
                }
            }
            break;
        }

        if (!found)
            return;
        if (stop != nullptr)
            *stop = kHit;

        const float inX = (float(hitX) + 0.5f - frame.facts.window[0]) / frame.facts.window[2];
        const float inY = (float(hitY) + 0.5f - frame.facts.window[1]) / frame.facts.window[3];
        const float edge = std::fmin(std::fmin(inX, 1.0f - inX), std::fmin(inY, 1.0f - inY));
        const float confidence = Saturate(edge / std::fmax(frame.params.fade, 1e-4f));
        if (!(confidence > kConfidenceThreshold))
            return;

        const float3 hit = Saturate3(ColourAtFraction(hitFX, hitFY));
        const float factor = Saturate(frame.params.blend * confidence);
        out[0] = Lerp(out[0], hit.x, factor);
        out[1] = Lerp(out[1], hit.y, factor);
        out[2] = Lerp(out[2], hit.z, factor);
        // Alpha is the engine's own, byte for byte: the confidence is internal.
    }

    // Whether the texel's march found anything (used to pick the texel whose ray leaves the picture).
    bool Found(uint32_t ownX, uint32_t ownY) const
    {
        float before[4] {};
        Texel(ownX, ownY, before);
        uint8_t engine[4] {};
        EngineBytes(ownX, ownY, frame.engineValue, engine);
        return std::abs(before[0] - float(engine[2]) / 255.0f) > 1e-4f || std::abs(before[1] - float(engine[1]) / 255.0f) > 1e-4f;
    }
};

// ---- the WARP harness ------------------------------------------------------------------------------------------------

struct Harness
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> positionTexture, sceneTexture, planarTexture;
    ComPtr<ID3D11ShaderResourceView> positionView, sceneView, planarView;
    ComPtr<ID3D11RenderTargetView> sceneRtv; // the scene bound as a render target while the pass copies it, which is the session's own shape
    ComPtr<ID3D11Buffer> camera;
    ComPtr<ID3D11Texture2D> staging;
    uint32_t stagingWidth = 0, stagingHeight = 0;
    bool boundRestored = false; // the game's render target came back after the copy

    bool Create()
    {
        const HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }

    ComPtr<ID3D11Texture2D> Texture(uint32_t format, uint32_t width, uint32_t height, uint32_t bytesPerTexel, const void* pixels,
                                    uint32_t bindFlags = D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
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
        // g_CameraParameter's layout: the view matrix's three rows at register 0 (the fixture's view is a roll about the view axis, so its second
        // column is the world's up in view space) and the projection's four at register 18; the table is the engine's own size. The buffer is
        // created once and rewritten on every upload, because a case that rolls the camera has to reach the GPU with that roll: a camera built on
        // the first frame of the run would leave every later case marching with the first case's view matrix.
        {
            float rows[kCameraBytes / 16][4] {};
            const Mat4 projection = Projection();
            const float roll = frame.tiltDegrees * kPi / 180.0f;
            rows[0][0] = std::cos(roll);
            rows[0][1] = -std::sin(roll);
            rows[0][2] = 0.0f;
            rows[1][0] = std::sin(roll);
            rows[1][1] = std::cos(roll);
            rows[1][2] = 0.0f;
            rows[2][2] = 1.0f;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    rows[18 + r][c] = projection.m[r][c];
            if (!camera)
            {
                D3D11_BUFFER_DESC desc {};
                desc.ByteWidth = kCameraBytes;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                if (FAILED(device->CreateBuffer(&desc, nullptr, &camera)))
                    return false;
            }
            context->UpdateSubresource(camera.Get(), 0, nullptr, rows, 0, 0);
        }

        // The two half-float buffers, at the allocation's size, with the picture in the window.
        std::vector<uint16_t> positionBits(size_t(kScene) * kScene * 4, 0);
        std::vector<uint16_t> colourBits(size_t(kScene) * kScene * 4, 0);
        // float -> IEEE half, the one conversion the fixture needs and the one every D3D11 implementation agrees on.
        const auto half = [](float value) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            const uint32_t sign = (bits >> 16) & 0x8000u;
            int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
            uint32_t mantissa = bits & 0x7FFFFFu;
            if (exponent <= 0)
                return uint16_t(sign); // this fixture's values are ordinary: a denormal reads as zero
            if (exponent >= 31)
                return uint16_t(sign | 0x7C00u);
            return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
        };
        for (uint32_t y = 0; y < kScene; ++y)
            for (uint32_t x = 0; x < kScene; ++x)
            {
                const size_t index = (size_t(y) * kScene + x) * 4;
                const size_t source = (size_t(y) * kScene + x) * 3;
                positionBits[index + 0] = half(frame.position[source + 0]);
                positionBits[index + 1] = half(frame.position[source + 1]);
                positionBits[index + 2] = half(frame.position[source + 2]);
                positionBits[index + 3] = half(1.0f);
                colourBits[index + 0] = half(frame.colour[source + 0]);
                colourBits[index + 1] = half(frame.colour[source + 1]);
                colourBits[index + 2] = half(frame.colour[source + 2]);
                colourBits[index + 3] = half(1.0f);
            }

        // The engine's planar map at the size the facts name, with the pattern EngineBytes writes.
        std::vector<uint8_t> planarBytes(size_t(kWindow) * kWindow * 4, 0);
        for (uint32_t y = 0; y < kWindow; ++y)
            for (uint32_t x = 0; x < kWindow; ++x)
                EngineBytes(x, y, frame.engineValue, &planarBytes[(size_t(y) * kWindow + x) * 4]);

        positionTexture = Texture(uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT), kScene, kScene, 8, positionBits.data());
        sceneTexture = Texture(uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT), kScene, kScene, 8, colourBits.data(),
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        planarTexture = Texture(uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM), kWindow, kWindow, 4, planarBytes.data());
        positionView.Reset();
        sceneView.Reset();
        planarView.Reset();
        sceneRtv.Reset();
        if (!positionTexture || !sceneTexture || !planarTexture)
            return false;
        if (FAILED(device->CreateShaderResourceView(positionTexture.Get(), nullptr, &positionView)) ||
            FAILED(device->CreateShaderResourceView(sceneTexture.Get(), nullptr, &sceneView)) ||
            FAILED(device->CreateRenderTargetView(sceneTexture.Get(), nullptr, &sceneRtv)) ||
            FAILED(device->CreateShaderResourceView(planarTexture.Get(), nullptr, &planarView)))
            return false;
        return true;
    }

    // One run of the pass, read back as the encoded texels (four bytes a texel, B8G8R8A8 order).
    bool Run(Pass& pass, const Frame& frame, std::vector<uint8_t>& out)
    {
        if (!Upload(frame))
            return false;
        PassInputs inputs;
        inputs.planar = planarView.Get();
        inputs.planarTexture = planarTexture.Get();
        inputs.vposition = positionView.Get();
        inputs.scene = sceneTexture.Get();
        inputs.camera = camera.Get();
        inputs.facts = frame.facts;
        inputs.params = frame.params;
        // The scene is bound as a render target while the pass copies it: that is the shape a session has, because the water draw's own target is
        // still bound when the seam enters the draw wrapper. The pass unbinds it for the copy and puts the game's views back; the picture
        // assertions below are what says the copy landed under that shape.
        ID3D11RenderTargetView* boundScene = sceneRtv.Get();
        context->OMSetRenderTargets(1, &boundScene, nullptr);
        if (pass.Run(context.Get(), inputs) == nullptr)
            return false;
        {
            ID3D11RenderTargetView* restored = nullptr;
            context->OMGetRenderTargets(1, &restored, nullptr);
            boundRestored = restored == sceneRtv.Get();
            if (restored != nullptr)
                restored->Release();
        }

        // The readback is the output's own size: a case with another planar size creates its own staging texture, so a copy whose shapes differ
        // is not attempted at all (the result of that copy is undefined, and a later assertion would read garbage).
        const uint32_t outWidth = pass.OutputWidth();
        const uint32_t outHeight = pass.OutputHeight();
        if (outWidth == 0 || outHeight == 0)
            return false;
        if (!staging || stagingWidth != outWidth || stagingHeight != outHeight)
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = outWidth;
            desc.Height = outHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
                return false;
            stagingWidth = outWidth;
            stagingHeight = outHeight;
        }
        context->CopyResource(staging.Get(), pass.OutputTexture());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            return false;
        out.assign(size_t(outWidth) * outHeight * 4, 0);
        for (uint32_t y = 0; y < outHeight; ++y)
            std::memcpy(out.data() + size_t(y) * outWidth * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(outWidth) * 4);
        context->Unmap(staging.Get(), 0);
        return true;
    }
};

// The byte of a B8G8R8A8 texel that carries a colour channel: red is the third byte in memory, green the second, blue the first, alpha the fourth.
uint32_t ChannelByte(uint32_t channel) { return channel == 0 ? 2u : (channel == 1 ? 1u : (channel == 2 ? 0u : 3u)); }

// How far the GPU's texels are from the reference's: the worst texel and the mean of the absolute differences, over the region the pass writes.
void CompareRegion(const std::vector<uint8_t>& pixels, const Reference& reference, const ao::Region& region, float& worst, float& mean)
{
    worst = 0.0f;
    double total = 0.0;
    uint32_t count = 0;
    for (uint32_t y = region.y; y < region.Bottom(); ++y)
        for (uint32_t x = region.x; x < region.Right(); ++x)
        {
            float expected[4] {};
            reference.Texel(x, y, expected);
            for (uint32_t c = 0; c < 4; ++c)
            {
                const float got = float(pixels[(size_t(y) * kWindow + x) * 4 + ChannelByte(c)]) / 255.0f;
                const float difference = std::fabs(got - expected[c]);
                worst = std::fmax(worst, difference);
                total += double(difference);
                ++count;
            }
        }
    mean = count == 0 ? 1.0f : float(total / double(count));
}

// The four channels at one texel, in the reference's order.
void ReadTexel(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y, float out[4])
{
    for (uint32_t c = 0; c < 4; ++c)
        out[c] = float(pixels[(size_t(y) * kWindow + x) * 4 + ChannelByte(c)]) / 255.0f;
}

// Whether every channel of a texel is the engine's own byte.
bool IsEngineTexel(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y, float engineValue)
{
    uint8_t engine[4] {};
    EngineBytes(x, y, engineValue, engine);
    for (uint32_t c = 0; c < 4; ++c)
        if (pixels[(size_t(y) * kWindow + x) * 4 + c] != engine[c])
            return false;
    return true;
}
} // namespace

// The pass's source, for the pins below: comments and whitespace removed, so a pin is about the code and not about its formatting.
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

// How many times a text occurs, so a pin can say "one decision" instead of "at least one".
static size_t Count(const std::string& text, const std::string& needle)
{
    size_t found = 0;
    if (needle.empty())
        return found;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++found;
    return found;
}

int main(int, char** argv)
{
    const std::string repo = argv[1] != nullptr ? argv[1] : "";

    // ---- the scene copy: a target that is not bound, and a copy of the scene that starts cleared
    if (!repo.empty())
    {
        // The copy reads the water draw's own render target, which the game still has bound. The pass unbinds the game's render targets and
        // depth-stencil for that one copy and puts them back before anything else, so the read is unambiguously legal; and a fresh copy of the scene
        // starts cleared, so a copy that does not land reads as sky and every texel answers with the engine's own (fail open) instead of marching
        // whatever the allocation held.
        const std::string pass = CodeOf(repo, "ffxiv/engine/SsrPass.cpp");
        const size_t unbindAt = pass.find("context->OMSetRenderTargets(0,nullptr,nullptr);");
        CHECK(unbindAt != std::string::npos);
        // Operation counters may sit between calls; preserve the required ordering rather than textual adjacency.
        const size_t sceneCopyAt = pass.find("context->CopySubresourceRegion(_scene.Get()", unbindAt);
        const size_t restoreAt = pass.find("context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,gameViews,gameDepth);", sceneCopyAt);
        CHECK(sceneCopyAt != std::string::npos && restoreAt != std::string::npos && unbindAt < sceneCopyAt && sceneCopyAt < restoreAt);
        CHECK(pass.find("OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,gameViews,&gameDepth);") != std::string::npos);
        CHECK(pass.find("OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,gameViews,gameDepth);") != std::string::npos);
        CHECK(pass.find("ClearRenderTargetView(_sceneRtv.Get(),nothing);") != std::string::npos);
        CHECK(pass.find("D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;D3D11_RENDER_TARGET_VIEW_DESCsceneRtvDesc") != std::string::npos);

        // The copy's box is the pure rule's, clamped to the allocation: the window may hang half a texel past the scene buffer (the rules' slack for
        // the engine's +0.5 viewport origin) and a copy box that reaches past the destination is out of bounds. And a reset gives the scene copy's
        // view back with the copy itself, so the reset really releases everything the pass owns.
        CHECK(pass.find("constSceneCopycopy=SceneCopyOf(inputs.facts);") != std::string::npos);
        const size_t resetAt = pass.find("voidPass::Reset(){");
        const size_t initAt = pass.find("boolPass::Init(", resetAt == std::string::npos ? 0 : resetAt);
        CHECK(resetAt != std::string::npos);
        CHECK(initAt != std::string::npos);
        // Only Reset's own body: the same two releases and the same clear-flag write also sit in EnsureTargets, which recreates the pair.
        const std::string resetBody =
            resetAt == std::string::npos || initAt == std::string::npos ? std::string() : pass.substr(resetAt, initAt - resetAt);
        CHECK(resetBody.find("_sceneSrv.Reset();_sceneRtv.Reset();_scene.Reset();") != std::string::npos);
        CHECK(resetBody.find("_sceneWidth=_sceneHeight=0;_sceneClearPending=true;_targetsCreated=0;") != std::string::npos);

        // The module reads the two published rules where it writes the line and shows the card, so the rules the host test covers are the ones a
        // session gets: a line that waited for its measurement and reports the run it measured, and a card armed by this module's own key.
        const std::string apply = CodeOf(repo, "ffxiv/engine/SsrApply.cpp");
        CHECK(apply.find("if(RunLineDue(state.loggedRun,state.pass.HasGpuMilliseconds(),state.pass.CanMeasureGpuTime()))") != std::string::npos);
        CHECK(apply.find("status.armed=CardArmed(status.key,state.params,state.session.Armed());") != std::string::npos);
        CHECK(apply.find("FormatRun(state.lastParams,facts.planarWidth,facts.planarHeight,state.pass.HasGpuMilliseconds()") != std::string::npos);

        // A character-preview replay is asked before the frame's one attempt and before the slot swap, so a preview opened beside the reflections
        // keeps the engine's texel, is not counted as a refusal, and the frame's own water draws still get their pass.
        const size_t replayAt = apply.find("if(ext::ffxiv::chara::t_replay)");
        const size_t gateAt = apply.find("ServeAnswer(state.frame,");
        const size_t attemptAt = apply.find("state.frame.TakeAttempt();");
        const size_t swapAt = apply.find("detail::SwapSlot(");
        CHECK(replayAt != std::string::npos);
        CHECK(gateAt != std::string::npos);
        CHECK(attemptAt != std::string::npos);
        CHECK(swapAt != std::string::npos);
        CHECK(Count(apply, "ServeAnswer(") == 1); // one decision, above every branch that could spend the attempt
        CHECK(Count(apply, "state.frame.TakeAttempt();") == 1);
        if (replayAt != std::string::npos && gateAt != std::string::npos && attemptAt != std::string::npos && swapAt != std::string::npos)
        {
            CHECK(replayAt < gateAt);
            CHECK(replayAt < attemptAt); // the attempt the frame has one of is not spent by a replay
            CHECK(replayAt < swapAt);    // and a replay is never handed this module's texture
            CHECK(gateAt < attemptAt);   // and the gate is what takes it
        }
        // The rule carries the replay test as its first branch: before the frame's recorded texture and before the attempt.
        const std::string rules = CodeOf(repo, "ffxiv/engine/SsrRules.h");
        const size_t ruleAt = rules.find("constexprServeServeAnswer(");
        const size_t ruleReplay = rules.find("if(replay)returnServe::PassOn;");
        CHECK(ruleAt != std::string::npos);
        CHECK(ruleReplay != std::string::npos);
        if (ruleAt != std::string::npos && ruleReplay != std::string::npos)
        {
            CHECK(ruleReplay > ruleAt);
            CHECK(ruleReplay < rules.find("if(frame.valid)"));
            CHECK(ruleReplay < rules.find("returnServe::Attempt;"));
        }
        // The gate is told which draw it is looking at: the marker the draw path read is what the rule needs to pass a replay on before the frame's
        // recorded texture and before the one attempt.
        CHECK(Count(apply, "ServeAnswer(state.frame,ext::ffxiv::chara::t_replay)") == 1);

        // Every silent stop before the frame's one attempt is counted and written once a session, and no passed-on reason is a refusal: the
        // twelve returns of the seam each record their own reason, the gate gives each reason one line, and none of them touches the session's
        // refusal streak (which is what would stand the module down for a draw it does not answer for).
        CHECK(Count(apply, "NotePassedOn(PassedOn::") == kPassedOnCount);
        CHECK(apply.find("state.passedOn[uint32_t(reason)]++;") != std::string::npos);
        CHECK(apply.find("state.passedOnGate.Take(reason,state.log)") != std::string::npos);
        CHECK(apply.find("voidNotePassedOn(PassedOnreason,ID3D11PixelShader*shader=nullptr,constReadBinding*read=nullptr)") !=
              std::string::npos);
        CHECK(apply.find("FormatPassedOn(reason,ext::ffxiv::diag::ReadShaderCrc(shader),read)") != std::string::npos);
        {
            const size_t noteAt = apply.find("voidNotePassedOn(");
            const size_t runFrameAt = apply.find("ID3D11ShaderResourceView*RunFrame(");
            CHECK(noteAt != std::string::npos && runFrameAt != std::string::npos && noteAt < runFrameAt);
            const std::string passedOnBody = noteAt != std::string::npos && runFrameAt != std::string::npos ? apply.substr(noteAt, runFrameAt - noteAt)
                                                                                                           : std::string();
            CHECK(passedOnBody.find("NoteDecline") == std::string::npos); // a passed-on draw never counts as a refusal
        }
        // The card's own arming is written when it changes, and the card shows the reasons its draws were passed on for.
        CHECK(apply.find("if(!state.haveArmed||state.armed.load(std::memory_order_relaxed)!=armedNow)") != std::string::npos);
        CHECK(apply.find("FormatPublish(armedNow,params.blend)") != std::string::npos);
        CHECK(Count(apply, "status.passedOn[i]=state.passedOn[i];") == 1);
        CHECK(Count(apply, "diagnostics.passedOn[i]=state.passedOn[i];") == 1);
        // The slot and the view of the last water draw the gate passed on are exported with the counters, so a capture can print them.
        CHECK(Count(apply, "diagnostics.read=state.read;") == 1);

        // The creation hook's own call is pinned here as well as driven below: the stamp is written inside the byte scan of every created
        // pixel shader, before the index decides whether it stores the shader, so a water shader the index drops still answers for its slots.
        const std::string enginePass = CodeOf(repo, "ffxiv/engine/EnginePass.cpp");
        const size_t stampAt = enginePass.find("ssr::NoteShader(shader,code,size);");
        CHECK(stampAt != std::string::npos);
        CHECK(stampAt < enginePass.find("SeenForInsert().Insert("));

        // The region: the pass draws the region with a viewport, and the shader's own test is the belt to it. A picture cannot separate the shader's
        // test from its absence, because the viewport clips every fragment outside the region - so the two statements are pinned together, and either
        // one alone would leave a future viewport (or a future region rule) free to overwrite the engine's texels outside the render window.
        CHECK(pass.find("constD3D11_VIEWPORTviewport{float(region.x),float(region.y),float(region.width),float(region.height),0.0f,1.0f};") !=
              std::string::npos);
        const std::string shader = CodeOf(repo, "ffxiv/engine/shaders/Ssr.hlsl");
        CHECK(shader.find("if(own.x<regionLow.x||own.y<regionLow.y||own.x>=regionHigh.x||own.y>=regionHigh.y)SSR_ENGINE(1);") !=
              std::string::npos);

        // The water plane's normal is turned to the side that faces the camera. The turn cannot change a picture (a reflection about N and about -N
        // is the same reflection), so this pin is the rule's only witness: the comment the shader writes next to it is the contract a reader relies on
        // when the march's plane and the camera's side change.
        CHECK(shader.find("if(dot(N,P)>0.0)N=-N;") != std::string::npos);

        // The march ends when the ray leaves the render window: a step whose projected texel is outside it is no hit at all. A picture cannot
        // separate this from its absence either - a hit outside the window has a confidence of zero, which the threshold refuses before the colour is
        // written - so the statement is pinned, and the fixture above (a surface outside the window that nothing may reflect) is what shows the
        // geometry out there is unreachable.
        // The statement itself now lives in the march the puddles' mirror shares (shaders/SceneMarch.hlsl), so it is pinned there: one
        // implementation means one place for a future edit to widen the window.
        const std::string march = CodeOf(repo, "ffxiv/engine/shaders/SceneMarch.hlsl");
        CHECK(march.find("if(!(projected.x>=window.x)||!(projected.y>=window.y)||!(projected.x<window.x+window.z)||"
                         "!(projected.y<window.y+window.w))returnprojected;") != std::string::npos); // the exit lives in SceneMarchProject, which answers the coordinate
        CHECK(march.find("if(!inside)returnint2(-1,-1);") != std::string::npos); // and SceneMarchTexel turns it into the march's no-texel answer
    }

    Harness harness;
    if (!harness.Create())
    {
        std::printf("ssr_warp: no D3D11 WARP device\n");
        return 1;
    }

    Pass pass;
    std::vector<uint8_t> pixels;

    // ---- the mirrored square: the floor below it shows its colour, with a confidence above one half
    {
        const Frame scene = BuildScene(true, true);
        CHECK(harness.Run(pass, scene, pixels));
        const Reference reference { scene };

        // The floor texel under the square, and the wall texel the mirror image lands on (the CPU reference finds the pair, so the assertion is
        // about the picture and not about a number this test guessed twice).
        const uint32_t ownX = 16, ownY = 27; // the planar texel that stands for scene texel (32, 54), the floor below the square
        float got[4] {};
        ReadTexel(pixels, ownX, ownY, got);
        uint8_t engine[4] {};
        EngineBytes(ownX, ownY, scene.engineValue, engine);
        const float engineRed = float(engine[2]) / 255.0f;
        // The confidence is internal (the map's alpha is the engine's own), so it is read back out of the colour: at the full blend the red
        // channel is lerp(engine, hit, confidence) and the reference knows the hit's red.
        float expected[4] {};
        reference.Texel(ownX, ownY, expected);
        const float hitRed = kSquareColour[0];
        const float confidence = hitRed > engineRed ? (got[0] - engineRed) / (hitRed - engineRed) : 0.0f;
        CHECK(got[0] > 0.5f);     // the square's red
        CHECK(got[2] < 0.35f);    // and not the floor's own blue-heavy tone
        CHECK(confidence > 0.5f); // the confidence of that hit
        CHECK(std::fabs(got[0] - expected[0]) < 0.03f);
        CHECK_EQ(pixels[(size_t(ownY) * kWindow + ownX) * 4 + 3], uint8_t(kEngineAlpha)); // alpha stays the engine's
        std::printf("ssr_warp: mirrored square at the planar texel (%u, %u): rgb %.3f %.3f %.3f, confidence %.3f\n", ownX, ownY, double(got[0]),
                    double(got[1]), double(got[2]), double(confidence));

        // The whole region against the reference: the mapping, the march, the refinement, the fade and the blend, together.
        const ao::Region region = RegionOf(scene.facts);
        CHECK_EQ(region.width, kWindow / 2);
        CHECK_EQ(region.height, kWindow / 2);
        float worst = 1.0f, mean = 1.0f;
        CompareRegion(pixels, reference, region, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ssr_warp: whole region against the reference, worst %.4f, mean %.5f\n", double(worst), double(mean));

        // Alpha is the engine's in every texel this pass writes: the confidence drives the colour's blend only, so no session can see a channel
        // whose meaning was never measured move.
        uint32_t alphaDiffering = 0;
        for (uint32_t y = 0; y < kWindow; ++y)
            for (uint32_t x = 0; x < kWindow; ++x)
                if (pixels[(size_t(y) * kWindow + x) * 4 + 3] != uint8_t(kEngineAlpha))
                    ++alphaDiffering;
        CHECK_EQ(alphaDiffering, 0u);

        // Outside the region the engine's own texels come back untouched - all four channels, at every texel - which is the copy the pass takes
        // before it writes its quadrant. An uninitialised target or a shifted copy fails here.
        uint32_t outsideWorst = 0;
        for (uint32_t y = 0; y < kWindow; ++y)
            for (uint32_t x = 0; x < kWindow; ++x)
            {
                if (x >= region.x && y >= region.y && x < region.Right() && y < region.Bottom())
                    continue;
                uint8_t engine[4] {};
                EngineBytes(x, y, scene.engineValue, engine);
                for (uint32_t c = 0; c < 4; ++c)
                    outsideWorst = std::max(outsideWorst, uint32_t(std::abs(int(pixels[(size_t(y) * kWindow + x) * 4 + c]) - int(engine[c]))));
            }
        CHECK_EQ(outsideWorst, 0u);

        // The schema of the copy: the scene was bound as a render target while the pass read it (the session's own shape), and the game's render
        // target is back once the pass returns.
        CHECK(harness.boundRestored);
        std::printf("ssr_warp: the scene was copied while bound as a render target, and the game's view came back\n");
    }

    // ---- a texel whose ray leaves the picture keeps the engine's own texel, byte for byte
    {
        // A floor with nothing above it, seen by a level camera: every reflected ray rises into the sky and then runs out of the window, so a
        // picture with nothing to reflect is the engine's own eight-bit answer everywhere.
        Frame flat = BuildScene(false, false);
        const ao::Region region = RegionOf(flat.facts);
        CHECK(harness.Run(pass, flat, pixels));
        uint32_t differing = 0;
        for (uint32_t y = region.y; y < region.Bottom(); ++y)
            for (uint32_t x = region.x; x < region.Right(); ++x)
                if (!IsEngineTexel(pixels, x, y, flat.engineValue))
                    ++differing;
        CHECK_EQ(differing, 0u);
        std::printf("ssr_warp: a floor with nothing above it keeps %u of %u region texels\n", region.width * region.height,
                    region.width * region.height);

        // And the same floor through a rolled camera: the plane's mirror image runs out of the picture instead of up the screen, so the march
        // ends by leaving it - which is the case this assertion is about. The texel is searched rather than guessed, so the assertion is about the
        // picture, and the roll is the frame's own (the camera table is rewritten for every case).
        flat.tiltDegrees = 20.0f;
        CHECK(harness.Run(pass, flat, pixels));
        const Reference reference { flat };
        bool foundMiss = false;
        uint32_t missX = 0, missY = 0;
        for (uint32_t y = region.y; y < region.Bottom() && !foundMiss; ++y)
            for (uint32_t x = region.x; x < region.Right() && !foundMiss; ++x)
            {
                const size_t at = (size_t(y) * 2 * kScene + x * 2) * 3;
                const bool geometry = flat.position[at] != 0.0f || flat.position[at + 1] != 0.0f || flat.position[at + 2] != 0.0f;
                float ignored[4] {};
                int stop = Reference::kNothing;
                reference.Texel(x, y, ignored, &stop);
                if (geometry && stop == Reference::kLeftPicture)
                {
                    foundMiss = true;
                    missX = x;
                    missY = y;
                }
            }
        CHECK(foundMiss); // the fixture has a texel whose reflected ray leaves the picture
        if (foundMiss)
        {
            CHECK(IsEngineTexel(pixels, missX, missY, flat.engineValue));
            std::printf("ssr_warp: the texel (%u, %u) whose ray leaves the picture keeps the engine's texel\n", missX, missY);
        }
    }

    // ---- a march that leaves the render window stops there: a surface outside the window is never reflected
    {
        // The picture is a floor with nothing above it, in the middle of the allocation, and every texel outside the window holds a plane the picture
        // does not show: the surface a march that kept going past the window's edge would find. Every texel whose ray leaves the window must come
        // back with the engine's own byte whatever lies out there. The rule that ends the march at the window's edge is pinned at its statement in the
        // pin block above: a hit outside the window has a confidence of zero, so a picture cannot separate the rule from its absence - this fixture
        // is what shows the geometry outside the window really is unreachable, and that pin is what holds the rule.
        Frame padded = BuildScene(false, false, 16, 16, 24.0f);
        padded.tiltDegrees = 20.0f; // a level camera's reflection runs out of the frame's own steps; a rolled one leaves the picture
        const ao::Region region = RegionOf(padded.facts);
        CHECK_EQ(region.x, 8u);
        CHECK_EQ(region.width, kWindow / 2);
        CHECK(harness.Run(pass, padded, pixels));
        const Reference reference { padded };
        uint32_t misses = 0; // the texels whose march ended by leaving the picture
        uint32_t missX = 0, missY = 0;
        for (uint32_t y = region.y; y < region.Bottom(); ++y)
            for (uint32_t x = region.x; x < region.Right(); ++x)
            {
                const size_t at = (size_t(y * 2) * kScene + x * 2) * 3; // the planar texel's block of scene texels (the mapping is 2:1)
                const bool geometry = padded.position[at] != 0.0f || padded.position[at + 1] != 0.0f || padded.position[at + 2] != 0.0f;
                if (!geometry)
                    continue;
                float ignored[4] {};
                int stop = Reference::kNothing;
                reference.Texel(x, y, ignored, &stop);
                if (stop == Reference::kLeftPicture)
                {
                    ++misses;
                    missX = x;
                    missY = y;
                }
            }
        CHECK(misses > 8); // the fixture has more than one texel whose march ends by leaving the picture
        if (misses > 0)
            CHECK(IsEngineTexel(pixels, missX, missY, padded.engineValue)); // and none of them reflects the surface outside the window
        // The picture itself is still the reference's, so the window's own texels are unaffected by the geometry outside it.
        float worst = 1.0f, mean = 1.0f;
        CompareRegion(pixels, reference, region, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ssr_warp: %u texels whose march leaves a window placed away from the allocation's origin keep the engine's texel\n", misses);
    }

    // ---- a camera below the water plane: the plane is the same mirror from the other side
    {
        // The visible side of the water faces the camera, so a point of the scene on the camera's side of the plane is what the normal has to be
        // turned towards. This fixture puts the whole scene above the view-space origin (a camera under the water looking up), so every texel's
        // `dot(N, P)` is positive and the turn is asked for at every texel. The reflection of a plane is the same about either of its normals, so the
        // picture cannot separate the turn from its absence - the statement pin above is what holds the rule - but the under-water picture is a case
        // this pass has to match its reference in, and it had none.
        Frame scene = BuildScene(true, true);
        for (size_t i = 0; i + 2 < scene.position.size(); i += 3)
            scene.position[i + 1] = -scene.position[i + 1];
        CHECK(harness.Run(pass, scene, pixels));
        const Reference reference { scene };
        const ao::Region region = RegionOf(scene.facts);
        uint32_t above = 0; // the texels whose view-space point is on the camera's side of the plane, where the turn is asked for
        for (uint32_t y = region.y; y < region.Bottom(); ++y)
            for (uint32_t x = region.x; x < region.Right(); ++x)
                if (scene.position[(size_t(y * 2) * kScene + x * 2) * 3 + 1] > 0.0f)
                    ++above;
        CHECK(above > 16);
        float worst = 1.0f, mean = 1.0f;
        CompareRegion(pixels, reference, region, worst, mean);
        CHECK(worst < 0.08f);
        CHECK(mean < 0.01f);
        std::printf("ssr_warp: %u texels of the scene above the camera match the reference, worst %.4f, mean %.5f\n", above, double(worst),
                    double(mean));
    }

    // ---- the camera table is the frame's: a rolled camera reaches the GPU, and the water plane's normal comes from the view matrix
    {
        // The same scene at two rolls, each against a CPU reference that derives the plane's normal independently: three ground-plane points put
        // through the view matrix and crossed, not the column the shader extracts. A camera buffer built once on the run's first frame would leave
        // the GPU marching with the first case's view matrix, so the rolled picture would disagree with the rolled reference - and the last count
        // (two rolls giving the same texels) would be zero.
        Frame scene = BuildScene(true, true);
        CHECK(harness.Run(pass, scene, pixels));
        const std::vector<uint8_t> levelled = pixels;
        const Reference levelReference { scene };
        const ao::Region region = RegionOf(scene.facts);
        float levelWorst = 1.0f, levelMean = 1.0f;
        CompareRegion(pixels, levelReference, region, levelWorst, levelMean);
        CHECK(levelWorst < 0.08f);
        CHECK(levelMean < 0.01f);

        scene.tiltDegrees = 20.0f;
        CHECK(harness.Run(pass, scene, pixels));
        const Reference reference { scene };
        float worst = 1.0f, mean = 1.0f;
        CompareRegion(pixels, reference, region, worst, mean);
        CHECK(worst < 0.08f); // the rolled picture is the one the rolled camera gives, texel for texel
        CHECK(mean < 0.01f);

        // The roll reached the GPU: two scenes that differ only in the camera's roll are not the same picture.
        uint32_t rolledDiffering = 0;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            if (pixels[i] != levelled[i] || pixels[i + 1] != levelled[i + 1] || pixels[i + 2] != levelled[i + 2])
                ++rolledDiffering;
        CHECK(rolledDiffering > 0u);
        std::printf("ssr_warp: a 20 degree camera roll moves %u texels; the rolled region matches the back-projected reference, worst %.4f\n",
                    rolledDiffering, double(worst));
    }

    // ---- the blend: 0 is the engine's map, byte for byte
    {
        Frame scene = BuildScene(true, true);
        scene.params.blend = 0.0f;
        CHECK(harness.Run(pass, scene, pixels));
        uint32_t differing = 0;
        for (uint32_t y = 0; y < kWindow; ++y)
            for (uint32_t x = 0; x < kWindow; ++x)
                if (!IsEngineTexel(pixels, x, y, scene.engineValue))
                    ++differing;
        CHECK_EQ(differing, 0u);

        // Half the blend is a true mix of the two, and alpha stays the engine's whatever the blend is.
        scene.params.blend = 0.5f;
        CHECK(harness.Run(pass, scene, pixels));
        const Reference reference { scene };
        float half[4] {};
        ReadTexel(pixels, 16, 27, half);
        float expected[4] {};
        reference.Texel(16, 27, expected);
        for (uint32_t c = 0; c < 4; ++c)
            CHECK(std::fabs(half[c] - expected[c]) < 0.03f);
        CHECK_EQ(pixels[(size_t(27) * kWindow + 16) * 4 + 3], uint8_t(kEngineAlpha)); // alpha is the engine's, whatever the blend is
    }

    // ---- the steps and the thickness reach the picture, and the picture stays a function of the buffers
    {
        const Frame scene = BuildScene(true, true);
        CHECK(harness.Run(pass, scene, pixels));
        const std::vector<uint8_t> first = pixels;
        const uint32_t created = pass.TargetsCreated();
        const uint32_t begins = pass.GpuTimingBegins();
        CHECK(harness.Run(pass, scene, pixels));
        CHECK(pixels == first);                   // a still scene gives the same picture every run: no frame number, no history
        CHECK_EQ(pass.TargetsCreated(), created);  // and the same size reuses the owned targets
        CHECK(created >= 1u);
        CHECK(pass.GpuTimingBegins() > begins);
        CHECK_EQ(pass.GpuTimingEnds(), pass.GpuTimingBegins());

        // The pair lands: the measurement is read a run or two later without ever waiting for the GPU, so the run line has a figure to print and
        // the card has one to show. A pass that could not time itself at all would report CanMeasureGpuTime false - the caller's line then says
        // "not measured yet" instead of waiting forever.
        CHECK(pass.CanMeasureGpuTime());
        for (int attempt = 0; attempt < 32 && !pass.HasGpuMilliseconds(); ++attempt)
            CHECK(harness.Run(pass, scene, pixels));
        CHECK(pass.HasGpuMilliseconds());
        std::printf("ssr_warp: the timestamp pair reports %.4f ms on the GPU\n", pass.GpuMilliseconds());

        // A different planar size is a new pair of owned textures (a window resize), and the readback is that size's own.
        Frame bigger = BuildScene(true, true);
        bigger.facts.planarWidth = 96;
        bigger.facts.planarHeight = 96;
        const uint32_t before = pass.TargetsCreated();
        CHECK(harness.Run(pass, bigger, pixels));
        CHECK_EQ(pass.TargetsCreated(), before + 1);
        CHECK(pixels.size() == size_t(96) * 96 * 4); // the harness reads back the planar target's own size, not the last case's

        // Fewer steps find less: the march is the thing the key moves, so a run at the lowest count is not the reference's picture at 48.
        Frame coarse = BuildScene(true, true);
        coarse.params.steps = 16;
        pass.Reset();
        CHECK(harness.Run(pass, coarse, pixels));
        const Reference reference { coarse };
        float worst = 1.0f, mean = 1.0f;
        CompareRegion(pixels, reference, RegionOf(coarse.facts), worst, mean);
        CHECK(worst < 0.08f); // the reference marches the same number of steps, so the two still agree
        CHECK(mean < 0.01f);
    }

    // ---- a reset gives every device object back, the scene copy's render-target view included
    {
        // The view is one of the objects the copy owns (D3D11 has a view hold a reference on its resource), so a reset that left it behind would keep
        // the scene allocation alive (63 MB at 4K) until a later size change - and for the rest of the process on the unload path, where no later pass
        // runs. The reference count is not asserted here: this adapter does not add a reference for a view, so the accessors below are what a reset
        // that missed one shows up in.
        const Frame scene = BuildScene(true, true);
        CHECK(harness.Run(pass, scene, pixels));
        CHECK(pass.SceneTexture() != nullptr);
        CHECK(pass.SceneTargetView() != nullptr); // only used to clear a fresh copy, but owned all the same
        pass.Reset();
        CHECK(pass.SceneTexture() == nullptr);
        CHECK(pass.SceneTargetView() == nullptr);
        CHECK(pass.OutputView() == nullptr);
        CHECK_EQ(pass.TargetsCreated(), 0u);
        // A run after the reset builds its own objects again, so the reset left a pass that still works.
        CHECK(harness.Run(pass, scene, pixels));
        CHECK(pass.SceneTexture() != nullptr);
        CHECK(pass.SceneTargetView() != nullptr);
        CHECK_EQ(pass.TargetsCreated(), 1u);
    }

    // H still performs the real scene copy with the game target left bound. Its nonzero upload makes a missing copy observable.
    {
        Pass hPass;
        Frame scene = BuildScene(true, true);
        CHECK(harness.Upload(scene));
        PassInputs inputs;
        inputs.planar = harness.planarView.Get(); inputs.planarTexture = harness.planarTexture.Get();
        inputs.vposition = harness.positionView.Get(); inputs.scene = harness.sceneTexture.Get();
        inputs.camera = harness.camera.Get(); inputs.facts = scene.facts; inputs.params = scene.params;
        ID3D11RenderTargetView* entryRtv = harness.sceneRtv.Get();
        D3D11_BUFFER_DESC uavBufferDesc{}; uavBufferDesc.ByteWidth = 16; uavBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        uavBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS; uavBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        uavBufferDesc.StructureByteStride = 4;
        ComPtr<ID3D11Buffer> uavBuffer; CHECK(SUCCEEDED(harness.device->CreateBuffer(&uavBufferDesc, nullptr, &uavBuffer)));
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_UNKNOWN; uavDesc.Buffer.NumElements = 4;
        ComPtr<ID3D11UnorderedAccessView> entryUav; CHECK(SUCCEEDED(harness.device->CreateUnorderedAccessView(uavBuffer.Get(), &uavDesc, &entryUav)));
        ComPtr<ID3D11DeviceContext1> context1; CHECK(SUCCEEDED(harness.context.As(&context1)));
        UINT initialCount = UINT(-1);
        if (context1 && entryUav) context1->OMSetRenderTargetsAndUnorderedAccessViews(1, &entryRtv, nullptr, 1, 1, entryUav.GetAddressOf(), &initialCount);
        CHECK(hPass.Run(harness.context.Get(), inputs, true, true, false, true) != nullptr);
        ID3D11RenderTargetView* exitRtv = nullptr;
        harness.context->OMGetRenderTargets(1, &exitRtv, nullptr);
        CHECK(exitRtv == entryRtv);
        if (exitRtv) exitRtv->Release();
        ID3D11UnorderedAccessView* exitUav = nullptr;
        if (context1) context1->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &exitUav);
        CHECK(exitUav == entryUav.Get());
        if (exitUav) exitUav->Release();
        auto Read = [&](ID3D11Texture2D* texture, std::vector<uint8_t>& bytes)
        {
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            CHECK(SUCCEEDED(harness.device->CreateTexture2D(&desc, nullptr, &staging)));
            if (!staging) return;
            harness.context->CopyResource(staging.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(SUCCEEDED(harness.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
            if (mapped.pData)
            {
                bytes.resize(size_t(desc.Width) * desc.Height * 8);
                for (uint32_t y = 0; y < desc.Height; ++y)
                    std::memcpy(bytes.data() + size_t(y) * desc.Width * 8, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 8);
                harness.context->Unmap(staging.Get(), 0);
            }
        };
        std::vector<uint8_t> sourceBytes, copyBytes;
        Read(harness.sceneTexture.Get(), sourceBytes); Read(hPass.SceneTexture(), copyBytes);
        const SceneCopy box = SceneCopyOf(scene.facts);
        for (uint32_t y = box.y; y < box.y + box.height; ++y)
            for (uint32_t x = box.x; x < box.x + box.width; ++x)
                CHECK(std::memcmp(sourceBytes.data() + (size_t(y) * kScene + x) * 8, copyBytes.data() + (size_t(y) * kScene + x) * 8, 8) == 0);
    }

    // ---- a run the rules refuse hands the engine's texture back
    {
        Frame broken = BuildScene(true, true);
        broken.facts.vpositionWidth = kWindow; // the view position is not the scene's allocation
        PassInputs inputs;
        CHECK(harness.Upload(broken));
        inputs.planar = harness.planarView.Get();
        inputs.planarTexture = harness.planarTexture.Get();
        inputs.vposition = harness.positionView.Get();
        inputs.scene = harness.sceneTexture.Get();
        inputs.camera = harness.camera.Get();
        inputs.facts = broken.facts;
        inputs.params = broken.params;
        CHECK(pass.Run(harness.context.Get(), inputs) == nullptr);

        broken.facts = BuildScene(true, true).facts;
        broken.facts.planarWidth = 4; // under the reflection name, a texture that is not the engine's map
        broken.facts.planarHeight = 4;
        inputs.facts = broken.facts;
        CHECK(pass.Run(harness.context.Get(), inputs) == nullptr);

        pass.Reset();
        CHECK(pass.OutputView() == nullptr); // a release leaves nothing of this module behind
    }

    // ---- the stamp: the slots survive every other shader the game creates, and an unstamped object reads nothing
    {
        // The recognition's other half, on the device: a shader compiled with the names the water material declares is parsed by the product's own
        // parse, stamped on a real shader object and read back. A mistyped name, a wrong slot column or a suffix the parser does not strip lands
        // here instead of in a session where the pass serves nothing and says nothing.
        // The game's own shape: the effect compiler writes the samplers before the textures, at their own registers. The two numbers differ
        // here on purpose, so a parse that answered the sampler's slot lands somewhere else than this test's expectations.
        const char* waterSource = "SamplerState g_SamplerReflectionMapState : register(s1);\n"
                                  "SamplerState g_SamplerVPositionState : register(s3);\n"
                                  "SamplerState g_SamplerWaveMapState : register(s5);\n"
                                  "Texture2D<float4> g_SamplerReflectionMap : register(t8);\n"
                                  "Texture2D<float4> g_SamplerVPosition : register(t0);\n"
                                  "Texture2D<float4> g_SamplerWaveMap : register(t3);\n"
                                  "cbuffer g_CameraParameter : register(b2) { float4 g_CameraRows[16]; };\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target0 { const int2 t = int2(p.xy); const float2 uv = p.xy * 0.001;\n"
                                  "  return g_SamplerReflectionMap.Load(int3(t, 0)) + g_SamplerVPosition.Load(int3(t, 0)) + "
                                  "g_SamplerWaveMap.Load(int3(t, 0)) + g_SamplerReflectionMap.SampleLevel(g_SamplerReflectionMapState, uv, 0) + "
                                  "g_SamplerVPosition.SampleLevel(g_SamplerVPositionState, uv, 0) + "
                                  "g_SamplerWaveMap.SampleLevel(g_SamplerWaveMapState, uv, 0) + g_CameraRows[0]; }\n";
        ComPtr<ID3DBlob> waterBlob;
        ComPtr<ID3DBlob> waterErrors;
        CHECK(SUCCEEDED(D3DCompile(waterSource, std::strlen(waterSource), "ssr_warp_water", nullptr, nullptr, "main", "ps_5_0", 0, 0, &waterBlob,
                                  &waterErrors)));
        if (waterBlob)
        {
            const ReaderSlots parsed = SlotsOfShader(waterBlob->GetBufferPointer(), waterBlob->GetBufferSize());
            CHECK_EQ(parsed.reflection, 8u);
            CHECK_EQ(parsed.vposition, 0u);
            CHECK_EQ(parsed.camera, 2u);
            CHECK(parsed.water);
            CHECK(IsWaterReader(parsed));

            ComPtr<ID3D11PixelShader> reader;
            CHECK(SUCCEEDED(harness.device->CreatePixelShader(waterBlob->GetBufferPointer(), waterBlob->GetBufferSize(), nullptr, &reader)));
            CHECK(detail::StampShader(reader.Get(), PackSlots(parsed)));
            const ReaderSlots back = UnpackSlots(detail::ReadStamp(reader.Get()));
            CHECK_EQ(back.reflection, 8u);
            CHECK_EQ(back.vposition, 0u);
            CHECK_EQ(back.camera, 2u);
            CHECK(back.water);
            std::printf("ssr_warp: a compiled water shader stamps %u (reflection %u, view position %u, camera %u, water %d)\n",
                        detail::ReadStamp(reader.Get()), unsigned(back.reflection), unsigned(back.vposition), unsigned(back.camera),
                        back.water ? 1 : 0);
        }

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const char* source = "float4 main(float4 p : SV_Position) : SV_Target0 { return p; }";
        CHECK(SUCCEEDED(D3DCompile(source, std::strlen(source), "ssr_warp_stamp", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors)));
        if (blob)
        {
            ComPtr<ID3D11PixelShader> first;
            CHECK(SUCCEEDED(harness.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &first)));
            ReaderSlots slots;
            slots.reflection = 8;
            slots.vposition = 6;
            slots.camera = 2;
            slots.water = true;
            CHECK(detail::StampShader(first.Get(), PackSlots(slots)));
            CHECK_EQ(detail::ReadStamp(first.Get()), PackSlots(slots));
            // Thousands of other shaders, created and released after it: the stamp lives on the object and no table can lose it.
            for (int i = 0; i < 2000; ++i)
            {
                ComPtr<ID3D11PixelShader> other;
                if (FAILED(harness.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &other)))
                    break;
            }
            const ReaderSlots back = UnpackSlots(detail::ReadStamp(first.Get()));
            CHECK_EQ(back.reflection, 8u);
            CHECK_EQ(back.vposition, 6u);
            CHECK_EQ(back.camera, 2u);
            CHECK(back.water);

            // A shader that was never stamped reads the empty word, and so does a null object.
            ComPtr<ID3D11PixelShader> unstamped;
            CHECK(SUCCEEDED(harness.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &unstamped)));
            CHECK_EQ(detail::ReadStamp(unstamped.Get()), 0u);
            CHECK_EQ(detail::ReadStamp(nullptr), 0u);
            // A word of 0 is not written, so an object that answers "nothing read" is never made to look stamped.
            CHECK(!detail::StampShader(unstamped.Get(), 0));
            CHECK_EQ(detail::ReadStamp(unstamped.Get()), 0u);
            CHECK(!detail::StampShader(nullptr, PackSlots(slots)));
        }
    }

    // ---- the draw path: a stamped water shader is served, an unstamped one is passed on, an unarmed card is passed on
    {
        // The chain that had no test: the creation hook's own call stamps a compiled water shader at the slots the capture shows (map t8, view
        // position t0, camera b2, wave map t4, caustics t6), the draw path reads that stamp off the bound shader, decides the frame's first water
        // draw, runs the pass and swaps this module's texture in for that one draw. The three stops a session could not tell apart are the three
        // counters: a served reader, a shader whose stamp never landed, and a card that was not armed.
        // The same shape as the water fixture, with the capture's own slots: the samplers come first at their own registers (s1, s3, s5, s7)
        // and the textures at t8, t0, t4 and t6, so every slot the parse answers is the texture's and never the sampler's.
        const char* captureSource = "SamplerState g_SamplerReflectionMapState : register(s1);\n"
                                   "SamplerState g_SamplerVPositionState : register(s3);\n"
                                   "SamplerState g_SamplerWaveMapState : register(s5);\n"
                                   "SamplerState g_SamplerCausticsState : register(s7);\n"
                                   "Texture2D<float4> g_SamplerReflectionMap : register(t8);\n"
                                   "Texture2D<float4> g_SamplerVPosition : register(t0);\n"
                                   "Texture2D<float4> g_SamplerWaveMap : register(t4);\n"
                                   "Texture2D<float4> g_SamplerCaustics : register(t6);\n"
                                   "cbuffer g_CameraParameter : register(b2) { float4 g_CameraRows[16]; };\n"
                                   "float4 main(float4 p : SV_Position) : SV_Target0 { const int2 t = int2(p.xy); const float2 uv = p.xy * 0.001;\n"
                                   "  return g_SamplerReflectionMap.Load(int3(t, 0)) + g_SamplerVPosition.Load(int3(t, 0)) + "
                                   "g_SamplerWaveMap.Load(int3(t, 0)) + g_SamplerCaustics.Load(int3(t, 0)) + "
                                   "g_SamplerReflectionMap.SampleLevel(g_SamplerReflectionMapState, uv, 0) + "
                                   "g_SamplerVPosition.SampleLevel(g_SamplerVPositionState, uv, 0) + "
                                   "g_SamplerWaveMap.SampleLevel(g_SamplerWaveMapState, uv, 0) + "
                                   "g_SamplerCaustics.SampleLevel(g_SamplerCausticsState, uv, 0) + g_CameraRows[0]; }\n";
        ComPtr<ID3DBlob> captureBlob;
        ComPtr<ID3DBlob> captureErrors;
        CHECK(SUCCEEDED(D3DCompile(captureSource, std::strlen(captureSource), "ssr_warp_capture", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                  &captureBlob, &captureErrors)));
        if (captureBlob)
        {
            const ReaderSlots parsed = SlotsOfShader(captureBlob->GetBufferPointer(), captureBlob->GetBufferSize());
            CHECK_EQ(parsed.reflection, 8u);
            CHECK_EQ(parsed.vposition, 0u);
            CHECK_EQ(parsed.camera, 2u);
            CHECK(parsed.water);
            CHECK(IsWaterReader(parsed));

            const Frame frame = BuildScene(true, true);
            CHECK(harness.Upload(frame));
            ext::present::g_ssrTestFrame = 100;
            Publish(true);              // the card on: the module's own arming, blend 1.00 from the config stub
            CHECK(Armed());

            ComPtr<ID3D11PixelShader> reader;
            CHECK(SUCCEEDED(harness.device->CreatePixelShader(captureBlob->GetBufferPointer(), captureBlob->GetBufferSize(), nullptr, &reader)));
            // The call the creation hook makes (pinned above): the shader is stamped with the slots it declares.
            NoteShader(reader.Get(), captureBlob->GetBufferPointer(), captureBlob->GetBufferSize());
            CHECK_EQ(detail::ReadStamp(reader.Get()), PackSlots(parsed));

            // The draw the game submits: its shader, its map at t8, its view position at t0, its camera at b2, its scene bound as the target.
            harness.context->PSSetShader(reader.Get(), nullptr, 0);
            ID3D11ShaderResourceView* map = harness.planarView.Get();
            harness.context->PSSetShaderResources(8, 1, &map);
            ID3D11ShaderResourceView* position = harness.positionView.Get();
            harness.context->PSSetShaderResources(0, 1, &position);
            ID3D11Buffer* camera = harness.camera.Get();
            harness.context->PSSetConstantBuffers(2, 1, &camera);
            ID3D11RenderTargetView* scene = harness.sceneRtv.Get();
            harness.context->OMSetRenderTargets(1, &scene, nullptr);
            const D3D11_VIEWPORT window { 0.0f, 0.0f, float(kWindow), float(kWindow), 0.0f, 1.0f };
            harness.context->RSSetViewports(1, &window);

            // (i) a stamped water reader at the capture's slots: the frame's first water draw runs the pass and binds this module's texture
            {
                BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit);
                CHECK(served.Bound());
                CHECK_EQ(CurrentStatus().runs, 1u);
                CHECK_EQ(CurrentStatus().readers, 1u);
                CHECK_EQ(CurrentDiagnostics().passedOn[uint32_t(PassedOn::NoStamp)], 0u);
                // The slot holds this module's texture while the guard is alive, and the game's own view is back afterwards.
                ID3D11ShaderResourceView* bound = nullptr;
                harness.context->PSGetShaderResources(8, 1, &bound);
                CHECK(bound != nullptr && bound != harness.planarView.Get());
                if (bound != nullptr)
                    bound->Release();
            }
            ID3D11ShaderResourceView* restored = nullptr;
            harness.context->PSGetShaderResources(8, 1, &restored);
            CHECK(restored == harness.planarView.Get());
            if (restored != nullptr)
                restored->Release();

            // (ii) the same water shader with its stamp never written: the pass is not served and the reason is counted
            ComPtr<ID3D11PixelShader> unstamped;
            CHECK(SUCCEEDED(harness.device->CreatePixelShader(captureBlob->GetBufferPointer(), captureBlob->GetBufferSize(), nullptr, &unstamped)));
            CHECK_EQ(detail::ReadStamp(unstamped.Get()), 0u);
            harness.context->PSSetShader(unstamped.Get(), nullptr, 0);
            {
                BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit);
                CHECK(!served.Bound());
                CHECK_EQ(CurrentStatus().readers, 1u); // nothing more was served
                CHECK_EQ(CurrentDiagnostics().passedOn[uint32_t(PassedOn::NoStamp)], 1u);
            }

            // (ii-b) a stamped water draw whose map slot holds another texture: passed on as `OtherMap`, and the slot and the view it read are
            //        reported - the fact no earlier capture could read, and the one the next capture prints.
            {
                // A water shader is stamped, and its map slot holds another texture: passed on as `OtherMap`, with the slot and the view
                // it read reported - the fact no earlier capture could read, and the one the next capture prints.
                harness.context->PSSetShader(reader.Get(), nullptr, 0);
                ID3D11ShaderResourceView* other = harness.positionView.Get(); // a half-float view, which is not the engine's planar map
                harness.context->PSSetShaderResources(8, 1, &other);
                {
                    BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit);
                    CHECK(!served.Bound());
                }
                const Diagnostics diagnostics = CurrentDiagnostics();
                CHECK_EQ(diagnostics.passedOn[uint32_t(PassedOn::OtherMap)], 1u);
                CHECK(diagnostics.declined == 0u); // another map under the name is not a refusal
                CHECK(diagnostics.read.have);
                CHECK_EQ(diagnostics.read.stamp, PackSlots(parsed));
                CHECK_EQ(diagnostics.read.slot, 8u);
                CHECK_EQ(diagnostics.read.view, uint32_t(D3D11_SRV_DIMENSION_TEXTURE2D));
                CHECK_EQ(diagnostics.read.array, 1u);
                CHECK_EQ(diagnostics.read.format, uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT));
                CHECK_EQ(diagnostics.read.width, kScene);
                CHECK_EQ(diagnostics.read.height, kScene);
                CHECK(diagnostics.read.texture != 0);
                const std::string line = FormatReadBinding(diagnostics.read);
                CHECK(line.find("map slot 8") != std::string::npos);
                CHECK(line.find("view TEXTURE2D") != std::string::npos);
                CHECK(line.find("format R16G16B16A16_FLOAT") != std::string::npos);
                // The game's planar map goes back for the cases below.
                ID3D11ShaderResourceView* map = harness.planarView.Get();
                harness.context->PSSetShaderResources(8, 1, &map);
            }

            // (iii) a card that is not armed: the draw is passed on before the stamp is even read, and it is not a refusal
            Publish(false);
            harness.context->PSSetShader(reader.Get(), nullptr, 0);
            {
                BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit);
                CHECK(!served.Bound());
                CHECK_EQ(CurrentDiagnostics().passedOn[uint32_t(PassedOn::NotArmed)], 1u);
                CHECK_EQ(CurrentDiagnostics().declined, 0u); // a passed-on draw never counts as a refusal
                CHECK_EQ(CurrentDiagnostics().passedOn[uint32_t(PassedOn::NoStamp)], 1u);
            }
            // The counters a capture reads are the module's own, one per reason.
            const Diagnostics diagnostics = CurrentDiagnostics();
            CHECK_EQ(diagnostics.runs, 1u);
            CHECK_EQ(diagnostics.readers, 1u);
            CHECK(diagnostics.passedOn[uint32_t(PassedOn::NoStamp)] >= 1u);
            CHECK(diagnostics.passedOn[uint32_t(PassedOn::NotArmed)] >= 1u);
            CHECK(diagnostics.passedOn[uint32_t(PassedOn::NotReflection)] == 0u);
            std::printf("ssr_warp: a stamped water draw was served (%u run, %u reader), an unstamped one passed on (%u), an unarmed card passed "
                        "on (%u)\n",
                        diagnostics.runs, diagnostics.readers, diagnostics.passedOn[uint32_t(PassedOn::NoStamp)],
                        diagnostics.passedOn[uint32_t(PassedOn::NotArmed)]);

            // Controlled live-isolation policy through the production SwapReader and Pass::Run. Each mode gets a fresh frame token;
            // inspect the actual t8 resource and GPU bytes while the guard is alive, then verify the game's view is restored.
            Publish(true);
            ext::ffxiv::engine::ssr::PublishDiagnostic(true);
            ComPtr<ID3D11DeviceContext> identityImmediate;
            harness.device->GetImmediateContext(&identityImmediate);
            CHECK(ext::ffxiv::engine::ssr::native_context::Same(harness.context.Get(), identityImmediate.Get()));
            ComPtr<ID3D11Device> identityTextureDevice;
            harness.planarTexture->GetDevice(&identityTextureDevice);
            CHECK(ext::ffxiv::engine::ssr::native_context::Same(harness.device.Get(), identityTextureDevice.Get()));
            CHECK(ext::ffxiv::engine::ssr::native_context::VerifiedRoute(harness.context.Get(), identityImmediate.Get(), harness.device.Get(),
                                                                          harness.sceneTexture.Get(), harness.planarTexture.Get(),
                                                                          harness.positionTexture.Get(), harness.camera.Get()));
            ComPtr<ID3D11DeviceContext> deferred;
            CHECK(SUCCEEDED(harness.device->CreateDeferredContext(0, &deferred)));
            CHECK(!ext::ffxiv::engine::ssr::native_context::VerifiedRoute(deferred.Get(), identityImmediate.Get(), harness.device.Get(),
                                                                           harness.sceneTexture.Get(), harness.planarTexture.Get(),
                                                                           harness.positionTexture.Get(), harness.camera.Get()));
            ComPtr<ID3D11Device> otherDevice;
            ComPtr<ID3D11DeviceContext> otherImmediate;
            CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &otherDevice, nullptr, &otherImmediate)));
            CHECK(!ext::ffxiv::engine::ssr::native_context::VerifiedRoute(harness.context.Get(), otherImmediate.Get(), harness.device.Get(),
                                                                           harness.sceneTexture.Get(), harness.planarTexture.Get(),
                                                                           harness.positionTexture.Get(), harness.camera.Get()));
            std::vector<uint8_t> diagnosticBytes[10];
            const uint32_t beforeDiagnosticRuns = CurrentStatus().runs;
            const uint32_t beforeDiagnosticDraws = CurrentStatus().diagnosticDraws;
            const uint32_t beforeDiagnosticTimingBegins = CurrentStatus().diagnosticTimingBegins;
            const uint32_t beforeDiagnosticTimingEnds = CurrentStatus().diagnosticTimingEnds;
            const uint32_t beforeDiagnosticGetData = CurrentStatus().diagnosticTimingGetDataCalls;
            const uint32_t beforeDiagnosticBypasses = CurrentStatus().diagnosticBypasses;
            for (uint32_t mode = 0; mode < 10; ++mode)
            {
                const uint32_t timingBeginsBeforeMode = CurrentStatus().diagnosticTimingBegins;
                const uint32_t timingEndsBeforeMode = CurrentStatus().diagnosticTimingEnds;
                const uint32_t getDataBeforeMode = CurrentStatus().diagnosticTimingGetDataCalls;
                Frame diagnosticFrame = BuildScene(true, true);
                diagnosticFrame.engineValue = 0.37f;
                CHECK(harness.Upload(diagnosticFrame));
                ext::present::g_ssrTestFrame = 200 + mode;
                ext::ffxiv::engine::ssr::SetDiagnosticModeForTest(mode);
                harness.context->PSSetShader(reader.Get(), nullptr, 0);
                ID3D11ShaderResourceView* diagnosticMap = harness.planarView.Get();
                harness.context->PSSetShaderResources(8, 1, &diagnosticMap);
                ID3D11ShaderResourceView* diagnosticPosition = harness.positionView.Get();
                harness.context->PSSetShaderResources(0, 1, &diagnosticPosition);
                ID3D11Buffer* diagnosticCamera = harness.camera.Get();
                harness.context->PSSetConstantBuffers(2, 1, &diagnosticCamera);
                ID3D11RenderTargetView* diagnosticScene = harness.sceneRtv.Get();
                harness.context->OMSetRenderTargets(1, &diagnosticScene, nullptr);
                harness.context->RSSetViewports(1, &window);
                const uint32_t drawsBeforeMode = CurrentStatus().diagnosticDraws;
                {
                    BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit);
                    CHECK_EQ(served.Bound(), mode < 3 || mode == 9);
                    ID3D11ShaderResourceView* bound = nullptr;
                    harness.context->PSGetShaderResources(8, 1, &bound);
                    ComPtr<ID3D11Resource> resource;
                    if (bound) bound->GetResource(&resource);
                    CHECK(resource != nullptr);
                    CHECK_EQ(bound == harness.planarView.Get(), (mode >= 3 && mode <= 8) || mode == 1); // D-H keep the exact native view; B binds it via setter
                    CHECK_EQ(resource.Get() == harness.planarTexture.Get(), (mode >= 3 && mode <= 8) || mode == 1); // A/C/J are distinct SSR/private views
                    if (resource)
                    {
                        ComPtr<ID3D11Texture2D> texture;
                        resource.As(&texture);
                        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                        D3D11_TEXTURE2D_DESC stagingDesc = desc; stagingDesc.Usage = D3D11_USAGE_STAGING;
                        stagingDesc.BindFlags = 0; stagingDesc.MiscFlags = 0; stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> staging;
                        CHECK(SUCCEEDED(harness.device->CreateTexture2D(&stagingDesc, nullptr, &staging)));
                        if (staging)
                        {
                            harness.context->CopyResource(staging.Get(), texture.Get());
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            CHECK(SUCCEEDED(harness.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
                            if (mapped.pData)
                            {
                                diagnosticBytes[mode].resize(size_t(desc.Width) * desc.Height * 4);
                                for (uint32_t y = 0; y < desc.Height; ++y)
                                    std::memcpy(diagnosticBytes[mode].data() + size_t(y) * desc.Width * 4,
                                                static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4);
                                harness.context->Unmap(staging.Get(), 0);
                            }
                        }
                    }
                    if (bound) bound->Release();
                }
                ID3D11ShaderResourceView* restoredMode = nullptr;
                harness.context->PSGetShaderResources(8, 1, &restoredMode);
                CHECK(restoredMode == harness.planarView.Get());
                if (restoredMode) restoredMode->Release();
                CHECK_EQ(CurrentStatus().runs, beforeDiagnosticRuns + (mode >= 8 ? mode : mode + 1));
                CHECK(CurrentStatus().diagnosticCopyAvailable);
                CHECK_EQ(CurrentStatus().diagnosticDraws, drawsBeforeMode + ((mode >= 5 && mode <= 8) ? 0u : 1u));
                if (mode == 6 || mode == 8)
                {
                    CHECK_EQ(CurrentStatus().diagnosticTimingBegins, timingBeginsBeforeMode);
                    CHECK_EQ(CurrentStatus().diagnosticTimingEnds, timingEndsBeforeMode);
                    CHECK_EQ(CurrentStatus().diagnosticTimingGetDataCalls, getDataBeforeMode);
                }
                CHECK_EQ(CurrentStatus().diagnosticBypasses, beforeDiagnosticBypasses + (mode >= 8 ? 1u : 0u));
                CHECK_EQ(CurrentStatus().diagnosticSuppressionActiveDuringPass, mode == 4);
                CHECK(!ext::core::diagnostic::ObserversSuppressed()); // the E scope is gone before returning to the water draw
                CHECK_EQ(CurrentStatus().diagnosticSuppressedCallbacks, 0ull); // WARP invokes no runtime observers directly
            }
            for (uint32_t mode = 0; mode < 10; ++mode) CHECK(!diagnosticBytes[mode].empty());
            CHECK(diagnosticBytes[0] != diagnosticBytes[1]); // A runs the pass and presents its actual SSR output
            CHECK(diagnosticBytes[9] == diagnosticBytes[0]); // J runs the full pass on the verified native immediate context
            CHECK(diagnosticBytes[1] == diagnosticBytes[2]); // B original and C private copy have byte-identical engine map contents
            CHECK(diagnosticBytes[3] == diagnosticBytes[1] && diagnosticBytes[4] == diagnosticBytes[1] && diagnosticBytes[5] == diagnosticBytes[1]);
            CHECK_EQ(CurrentStatus().diagnosticCopies, 9u);
            CHECK_EQ(CurrentStatus().diagnosticBypasses, beforeDiagnosticBypasses + 1u);
            CHECK_EQ(CurrentStatus().diagnosticDraws, beforeDiagnosticDraws + 6u);
            CHECK_EQ(CurrentStatus().diagnosticTimingBegins, beforeDiagnosticTimingBegins + 8u);
            CHECK_EQ(CurrentStatus().diagnosticTimingEnds, beforeDiagnosticTimingEnds + 8u);
            ext::ffxiv::engine::ssr::PublishDiagnostic(false);

            // Exercise the real diagnostic capture hook and staging/readback path on WARP. The output root is unique to this process so a
            // previous run can never satisfy these file checks.
            ext::Cfg().ffxiv.drawDiagnosticsDump.value = "ssr-pair";
            Publish(true);
            const auto checkPair = [&](uint64_t firstToken, float firstValue, float secondValue) {
                ext::ffxiv::engine::ssr::OnDiagnosticCaptureStart();
                for (uint64_t token : { firstToken, firstToken + 1 })
                {
                    Frame capture = BuildScene(true, true);
                    capture.engineValue = token == firstToken ? firstValue : secondValue;
                    ext::present::g_ssrTestFrame = token;
                    CHECK(harness.Upload(capture));
                    harness.context->PSSetShader(reader.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* captureMap = harness.planarView.Get();
                    harness.context->PSSetShaderResources(8, 1, &captureMap);
                    ID3D11ShaderResourceView* capturePosition = harness.positionView.Get();
                    harness.context->PSSetShaderResources(0, 1, &capturePosition);
                    ID3D11Buffer* captureCamera = harness.camera.Get();
                    harness.context->PSSetConstantBuffers(2, 1, &captureCamera);
                    ID3D11RenderTargetView* captureScene = harness.sceneRtv.Get();
                    harness.context->OMSetRenderTargets(1, &captureScene, nullptr);
                    harness.context->RSSetViewports(1, &window);
                    { BoundReader served = SwapReader(harness.context.Get(), ext::ffxiv::engine::kReflectionReaderBit); CHECK(served.Bound()); }
                }
                std::filesystem::path pairDir;
                for (int attempt = 0; attempt < 5000; ++attempt)
                {
                    ext::ffxiv::engine::ssr::OnRenderCallback(harness.context.Get());
                    harness.context->Flush();
                    pairDir = ext::ffxiv::chara::probe::dump::OutputRoot() / L"SsrFramePair" /
                              (std::to_wstring(firstToken) + L"-" + std::to_wstring(CurrentStatus().runs - 1));
                    // The first ready resource creates the manifest; the rest may still be on the GPU.
                    // Wait for all fourteen records before inspecting any of their files.
                    std::ifstream progress(pairDir / L"manifest.csv");
                    const std::string records((std::istreambuf_iterator<char>(progress)), {});
                    if (std::count(records.begin(), records.end(), '\n') == 15) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const auto manifestPath = pairDir / L"manifest.csv";
                CHECK(std::filesystem::exists(manifestPath));
                if (std::filesystem::exists(manifestPath))
                {
                    std::ifstream manifest(manifestPath);
                    const std::string contents((std::istreambuf_iterator<char>(manifest)), {});
                    size_t lines = size_t(std::count(contents.begin(), contents.end(), '\n'));
                    CHECK_EQ(lines, 15u); // header and seven resources for each frame
                    if (lines != 15u) return; // a timeout must fail cleanly, not throw on missing files
                    for (const char* name : { "frame1_view-position.raw", "frame1_planar-source.raw", "frame1_camera-source.raw",
                                              "frame1_output.raw", "frame1_scene.raw", "frame1_camera.raw", "frame1_constants.raw",
                                              "frame2_view-position.raw", "frame2_planar-source.raw", "frame2_output.raw" })
                        CHECK(std::filesystem::file_size(pairDir / name) > 0);
                    std::vector<uint8_t> planar(size_t(kWindow) * kWindow * 4);
                    std::ifstream input(pairDir / "frame1_planar-source.raw", std::ios::binary);
                    input.read(reinterpret_cast<char*>(planar.data()), std::streamsize(planar.size()));
                    CHECK_EQ(size_t(input.gcount()), planar.size());
                    uint8_t expected[4] {};
                    EngineBytes(0, 0, firstValue, expected);
                    CHECK(std::equal(expected, expected + 4, planar.begin()));
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_view-position.raw"), size_t(kScene) * kScene * 8);
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_scene.raw"), size_t(kScene) * kScene * 8);
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_output.raw"), size_t(kWindow) * kWindow * 4);
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_camera-source.raw"), size_t(kCameraBytes));
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_camera.raw"), size_t(kCameraBytes));
                    CHECK_EQ(std::filesystem::file_size(pairDir / "frame1_constants.raw"), sizeof(Constants));
                    const auto readRaw = [&](const std::string& name) {
                        std::ifstream file(pairDir / name, std::ios::binary);
                        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
                    };
                    for (int index = 0; index < 2; ++index)
                    {
                        const std::string prefix = "frame" + std::to_string(index + 1) + "_";
                        Frame expectedFrame = BuildScene(true, true);
                        expectedFrame.engineValue = index == 0 ? firstValue : secondValue;
                        const auto originalCamera = readRaw(prefix + "camera-source.raw");
                        CHECK(readRaw(prefix + "camera.raw") == originalCamera);
                        const auto capturedConstants = readRaw(prefix + "constants.raw");
                        const Constants expectedConstants = ConstantsOf(expectedFrame.facts, expectedFrame.params);
                        CHECK_EQ(capturedConstants.size(), sizeof expectedConstants);
                        if (capturedConstants.size() == sizeof expectedConstants)
                            CHECK(std::memcmp(capturedConstants.data(), &expectedConstants, sizeof expectedConstants) == 0);
                        const auto capturedPlanar = readRaw(prefix + "planar-source.raw");
                        CHECK_EQ(capturedPlanar.size(), size_t(kWindow) * kWindow * 4);
                        for (uint32_t y = 0; y < kWindow && capturedPlanar.size() == size_t(kWindow) * kWindow * 4; ++y)
                            for (uint32_t x = 0; x < kWindow; ++x)
                            {
                                uint8_t texel[4] {};
                                EngineBytes(x, y, expectedFrame.engineValue, texel);
                                CHECK(std::equal(texel, texel + 4, capturedPlanar.begin() + (size_t(y) * kWindow + x) * 4));
                            }
                        Pass independentPass;
                        std::vector<uint8_t> expectedOutput;
                        CHECK(harness.Run(independentPass, expectedFrame, expectedOutput));
                        CHECK(readRaw(prefix + "output.raw") == expectedOutput);
                    }
                }
            };
            checkPair(200, 0.25f, 0.5f);
            checkPair(300, 0.5f, 0.25f); // a second capture start must rearm and produce its own directory
        }
    }

    return Finish("ssr_warp");
}
