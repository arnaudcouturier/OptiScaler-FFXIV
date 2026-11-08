// Light shafts before the tone map: the pure rules (ext/ffxiv/engine/ShaftsRules.h) and the anchors it runs on (EnginePassRules.h).
//
// What a player would feel if one of these broke: the shafts appear when the sun is behind the camera, a wrong projection puts them in the wrong
// place, a window of another frame is composited, a frame with no sky or no sun is still patched, or the master switch left on at intensity 0
// still costs a pass a frame. Everything here is decided without a device, so the numbers are hand-computed.
#include "check.h"

#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace eng = ext::ffxiv::engine;
using namespace ext::ffxiv::shafts;

namespace
{
bool Near(float a, float b, float tolerance = 1.0e-4f) { return std::fabs(a - b) <= tolerance; }

// The projection the engine stores in g_CameraParameter: row-major, a column-vector multiply, the camera looking down -z (the layout
// tests/ext/ffxiv/relight/camera_rows.h builds from the same table: m[3][2] = -1 makes the clip w the view depth).
void Perspective(float rows[16], float fovDegrees, float aspect, float nearPlane, float farPlane)
{
    std::memset(rows, 0, sizeof(float) * 16);
    const float tanHalf = std::tan(fovDegrees * 3.14159265358979f / 360.0f);
    rows[0] = 1.0f / tanHalf / aspect;
    rows[5] = 1.0f / tanHalf;
    rows[10] = farPlane / (nearPlane - farPlane);
    rows[11] = nearPlane * farPlane / (nearPlane - farPlane);
    rows[14] = -1.0f;
}

// out = a * b, both row-major 4x4 as the engine stores them and as `mul(matrix, vector)` applies them.
void Multiply(float out[16], const float a[16], const float b[16])
{
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
        {
            float sum = 0.0f;
            for (uint32_t k = 0; k < 4; ++k)
                sum += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = sum;
        }
}

// A view matrix whose third row is `back` (the direction the camera looks away from, in world space) and whose up direction is `up`: the rows are
// the camera's own axes in world space, so `mul(view, world)` is the view-space position. The translation column is the captured frame's.
void ViewLookingAlong(float rows[16], const float back[3], const float up[3])
{
    std::memset(rows, 0, sizeof(float) * 16);
    const float* b = back;
    const float right[3] { up[1] * b[2] - up[2] * b[1], up[2] * b[0] - up[0] * b[2], up[0] * b[1] - up[1] * b[0] };
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    const float x[3] { right[0] / rightLength, right[1] / rightLength, right[2] / rightLength };
    const float y[3] { b[1] * x[2] - b[2] * x[1], b[2] * x[0] - b[0] * x[2], b[0] * x[1] - b[1] * x[0] };
    for (uint32_t i = 0; i < 3; ++i)
    {
        rows[0 * 4 + i] = x[i];
        rows[1 * 4 + i] = y[i];
        rows[2 * 4 + i] = b[i];
    }
    rows[3 * 4 + 3] = 1.0f;
}

Window Frame() { return Window { 0.5f, 0.5f, 1920.0f, 1080.0f }; }
} // namespace

int main()
{
    // ---- the keys: every one defaults to today's picture, and a broken value can never turn the pass on
    {
        CHECK(!Wanted(false, BuildParams(kDefaultIntensity, kDefaultDecay, kDefaultSamples, kDefaultDensity)));
        CHECK(Wanted(true, BuildParams(kDefaultIntensity, kDefaultDecay, kDefaultSamples, kDefaultDensity)));
        CHECK(!Wanted(true, BuildParams(0.0f, kDefaultDecay, kDefaultSamples, kDefaultDensity)));
        CHECK(!Wanted(true, BuildParams(1.0e-6f, kDefaultDecay, kDefaultSamples, kDefaultDensity)));
        CHECK(Wanted(true, BuildParams(kIntensityEpsilon * 2.0f, kDefaultDecay, kDefaultSamples, kDefaultDensity)));
        CHECK(Near(ClampIntensity(-1.0f), kMinIntensity));
        CHECK(Near(ClampIntensity(9.0f), kMaxIntensity));
        CHECK(Near(ClampIntensity(std::nanf("")), kDefaultIntensity));
        CHECK(Near(ClampDecay(0.1f), kMinDecay));
        CHECK(Near(ClampDecay(2.0f), kMaxDecay));
        CHECK(Near(ClampDecay(std::nanf("")), kDefaultDecay));
        CHECK(Near(ClampDensity(0.0f), kMinDensity));
        CHECK(Near(ClampDensity(9.0f), kMaxDensity));
        CHECK_EQ(ClampSamples(0), uint32_t(kMinSamples));
        CHECK_EQ(ClampSamples(1000), uint32_t(kMaxSamples));
        const Params defaults = BuildParams(kDefaultIntensity, kDefaultDecay, kDefaultSamples, kDefaultDensity);
        CHECK_EQ(defaults.samples, 48u);
        CHECK(Near(defaults.decay, 0.96f) && Near(defaults.density, 0.9f) && Near(defaults.intensity, 1.0f));
    }

    // ---- the render window, and the half-resolution extent the two first draws run at
    {
        CHECK(WindowUsable(Frame()));
        CHECK(!WindowUsable(Window { 0.0f, 0.0f, 0.0f, 1080.0f }));
        CHECK(!WindowUsable(Window { 0.0f, 0.0f, -1920.0f, 1080.0f }));
        CHECK(!WindowUsable(Window { -1.0f, 0.0f, 1920.0f, 1080.0f }));
        CHECK(!WindowUsable(Window { 0.0f, 0.0f, 32.0f, 18.0f })); // a preview, not the frame
        CHECK(!WindowUsable(Window { std::nanf(""), 0.0f, 1920.0f, 1080.0f }));
        CHECK_EQ(HalfExtent(1920.0f), 960u);
        CHECK_EQ(HalfExtent(1080.0f), 540u);
        CHECK_EQ(HalfExtent(1081.0f), 541u); // an odd extent keeps its last half pixel
        CHECK_EQ(HalfExtent(2.0f), 1u);
        CHECK_EQ(HalfExtent(1.0f), 0u);
        CHECK_EQ(HalfExtent(0.0f), 0u);
        CHECK_EQ(HalfExtent(-4.0f), 0u);
        CHECK_EQ(HalfExtent(std::nanf("")), 0u);
    }

    // ---- the sky test: the engine's depth is reversed-Z, so the far plane is 0 and a NaN is a surface
    {
        CHECK(Near(SkyOf(0.0f), 1.0f));
        CHECK(Near(SkyOf(kSkyDepth * 0.5f), 1.0f));
        CHECK(Near(SkyOf(kSkyDepth * 10.0f), 0.0f));
        CHECK(Near(SkyOf(0.5f), 0.0f));
        CHECK(Near(SkyOf(1.0f), 0.0f));
        CHECK(Near(SkyOf(std::nanf("")), 0.0f));
        CHECK(Near(SkyOf(-1.0f), 0.0f));
    }

    // ---- the sun's projection: the window's own centre for a sun straight ahead, the solstices at the window's edges, and the three ways a
    //      frame is skipped (behind the camera, at its plane, further out than the margin). The camera here has no rotation, so a view-space
    //      direction and its world direction are the same vector and this projection places both the way ProjectSun places the light's own.
    {
        float viewProjection[16] {};
        Perspective(viewProjection, 60.0f, 1920.0f / 1080.0f, 0.1f, 10000.0f);
        CHECK(ProjectionOk(viewProjection) && ViewProjectionOk(viewProjection));
        const Window window = Frame();

        const float ahead[3] { 0.0f, 0.0f, -1.0f };
        const SunProjection centre = ProjectSun(viewProjection, ahead, window);
        CHECK(centre.unit && !centre.behind);
        CHECK(Near(centre.ndcX, 0.0f) && Near(centre.ndcY, 0.0f));
        CHECK(Near(centre.screenX, 960.5f) && Near(centre.screenY, 540.5f));
        CHECK(Near(centre.offscreen, 0.0f));
        CHECK(SunVisible(centre));

        // 30 degrees up with a 60-degree vertical field: the top edge of the window, which is still inside it
        const float up[3] { 0.0f, 0.5f, -0.8660254f };
        const SunProjection top = ProjectSun(viewProjection, up, window);
        CHECK(top.unit && !top.behind);
        CHECK(Near(top.ndcY, 1.0f, 1.0e-3f));
        CHECK(Near(top.screenY, window.y, 1.0f));
        CHECK(Near(top.offscreen, 0.0f, 1.0e-3f));
        CHECK(SunVisible(top));

        // to the right: 0.8 of the tangent plane puts it 15 % of a window outside, which the margin still accepts
        const float right[3] { 0.8f, 0.0f, -0.6f };
        const SunProjection outside = ProjectSun(viewProjection, right, window);
        CHECK(outside.unit && !outside.behind);
        CHECK(outside.offscreen > 0.1f && outside.offscreen < kMaxOffscreen);
        CHECK(SunVisible(outside));

        // a third of a window outside the right edge: the margin is half a window, so this sun still lights the frame - the morning and evening
        // sun the engine's shadow-casting light has moved away from is exactly this far out (the frame the shafts failed on)
        const float thirdOut[3] { 0.867583f, 0.0f, -0.497234f };
        const SunProjection thirdOutside = ProjectSun(viewProjection, thirdOut, window);
        CHECK(thirdOutside.unit && !thirdOutside.behind);
        CHECK(thirdOutside.offscreen > 0.30f && thirdOutside.offscreen < 0.40f);
        CHECK(SunVisible(thirdOutside));
        CHECK(!SunVisible(thirdOutside, 0.25f)); // the quarter-window margin this bound used to be refused it

        // eight tenths of a window outside, on the same edge: past the margin, so this sun lights nothing. The value is between the two sides
        // a wider margin would leave open, so it separates the bound from "the sun is behind the camera" and from "the sun is far outside".
        const float farOut[3] { 0.936418f, 0.0f, -0.350935f };
        const SunProjection farOutside = ProjectSun(viewProjection, farOut, window);
        CHECK(farOutside.unit && !farOutside.behind);
        CHECK(farOutside.offscreen > 0.7f && farOutside.offscreen < 0.9f);
        CHECK(!SunVisible(farOutside));
        CHECK_EQ(kMaxOffscreen, 0.50f); // the margin the design names, written out

        // the sun behind the camera, and the sun exactly on the camera's plane (a zenith sun with a level camera: no screen position exists)
        const float behind[3] { 0.0f, 0.0f, 1.0f };
        const SunProjection back = ProjectSun(viewProjection, behind, window);
        CHECK(back.unit && back.behind);
        CHECK(!SunVisible(back));
        const float horizon[3] { 0.0f, 1.0f, 0.0f };
        const SunProjection edge = ProjectSun(viewProjection, horizon, window);
        CHECK(edge.unit && edge.behind);
        CHECK(!SunVisible(edge));

        // far outside the window: a direction almost along the camera's axis
        const float far[3] { 0.9999f, 0.0f, -0.01f };
        const SunProjection wide = ProjectSun(viewProjection, far, window);
        CHECK(wide.unit && !wide.behind);
        CHECK(wide.offscreen > kMaxOffscreen);
        CHECK(!SunVisible(wide));
        // and the margin itself is the boundary: a sun exactly on it still lights the frame
        CHECK(SunVisible(outside, outside.offscreen));
        CHECK(!SunVisible(outside, outside.offscreen * 0.5f));

        // a direction that is not a unit vector is not a sun: a light that is off, or a layout this build no longer has
        const float shortDirection[3] { 0.0f, 0.5f, -0.5f };
        CHECK(!ProjectSun(viewProjection, shortDirection, window).unit);
        const float zero[3] { 0.0f, 0.0f, 0.0f };
        CHECK(!ProjectSun(viewProjection, zero, window).unit);
        const float nan[3] { std::nanf(""), 0.0f, -1.0f };
        CHECK(!ProjectSun(viewProjection, nan, window).unit);
        // 3 % off a unit is not a sun either
        const float longDirection[3] { 0.0f, 0.618f, -0.824f };
        CHECK(!ProjectSun(viewProjection, longDirection, window).unit);

        // a projection of another shape is refused before anything is projected with it
        // The table the pass reads it from: g_CameraParameter +0x120 is the projection, and +0x60 beside it is the view-projection. A read one
        // offset early lands on the view-projection of the camera's own position, which is not what a direction at infinity is projected with.
        CHECK_EQ(kProjectionOffset, 0x120u);
        CHECK_EQ(kProjectionRows, 16u);
        CHECK_EQ(kProjectionBytes, 0x40u);
        CHECK_EQ(kViewProjectionAt, 0x060u); // the camera copy's view-projection, beside the projection in the same 0x160 bytes
        CHECK_EQ(kCameraBytes, 0x160u);
        float wrong[16] {};
        Perspective(wrong, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        wrong[14] = 0.0f; // a w row that is not the D3D perspective's
        CHECK(!ProjectionOk(wrong));
        // a live buffer whose w row is not bit-exactly (0, 0, -1, 0) is still the projection: the old exact test refused it, which is what made
        // the "no camera" reason unreachable and the frame a "no sun" one
        float almost[16] {};
        Perspective(almost, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        almost[12] = 3.0e-4f;
        almost[13] = -2.0e-4f;
        almost[14] = -0.9997f;
        almost[15] = 1.0e-4f;
        CHECK(ProjectionOk(almost));
        float shifted[16] {};
        Perspective(shifted, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        shifted[12] = 0.01f; // a w row that is not a perspective's
        CHECK(!ProjectionOk(shifted));
        float zeroScale[16] {};
        Perspective(zeroScale, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        zeroScale[0] = 0.0f;
        CHECK(!ProjectionOk(zeroScale));
        float nanRow[16] {};
        Perspective(nanRow, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        nanRow[9] = std::nanf("");
        CHECK(!ProjectionOk(nanRow));
        CHECK(!ProjectionOk(nullptr));
    }

    // ---- the light's direction is VIEW space: the frame's own numbers from a captured session, where the light draw's direction and the
    //      directional shadow matrix's axis are the same three floats, the camera's view rows are the ones cb.csv recorded, and the sun's position
    //      the frame's sky draws it at is (1319.4, 82.5) of its 1920x1080 window. Projecting that vector with the projection alone is what the
    //      module does; projecting it with the view-projection applies the view a second time.
    {
        const float direction[3] { 0.250254f, 0.318263f, -0.914375f }; // g_LightParam.m_Direction, and the shadow row's axis
        CHECK_EQ(kCameraBytes, 0x160u);
        CHECK_EQ(kViewProjectionAt, 0x060u);
        CHECK_EQ(kProjectionOffset, 0x120u);
        CHECK_EQ(kViewAt, 0x000u);
        CHECK_EQ(kViewRows, 12u);
        CHECK_EQ(kViewBytes, 0x30u);

        // The captured frame's projection (cbdump.csv, g_CameraParameter +0x120): a 16:9 perspective whose w row is (0, 0, -1, 0), so the clip w
        // is -direction.z and this sun, in front of the camera, has a positive one.
        float projection[16] {};
        projection[0] = 1.368430f;
        projection[2] = 0.000108f;
        projection[5] = 2.432765f;
        projection[6] = -0.000469f;
        projection[11] = 0.1f;
        projection[14] = -1.0f;
        CHECK(ProjectionOk(projection));

        // The same copy's view matrix (cb.csv: view_00 .. view_22 are the first three rows).
        const float view[16] { -0.923477f, -0.000000f, 0.383653f, -168.078476f, -0.205196f, 0.844949f, -0.493919f, 378.584137f,
                               -0.324167f, -0.534847f, -0.780291f, 641.551575f, 0.0f, 0.0f, 0.0f, 1.0f };

        // The frame's sun, where the frame's sky draws it: the projection alone, on the light's own view-space vector. The window's origin is zero
        // so the numbers are the capture's own, not offset by the half texel this test's other cases use.
        const Window window { 0.0f, 0.0f, 1920.0f, 1080.0f };
        const SunProjection placed = ProjectSun(projection, direction, window);
        CHECK(placed.unit && !placed.behind);
        CHECK(Near(placed.screenX, 1319.4f, 0.1f) && Near(placed.screenY, 82.5f, 0.1f));
        CHECK(Near(placed.offscreen, 0.0f, 1.0e-6f));
        CHECK(SunVisible(placed));

        // The view-projection the same copy carries, built from the two: projecting the view-space vector through it applies the view a second
        // time, and the frame's sun ends up behind the camera instead of at the top of the window. That is the defect.
        float captured[16] {};
        Multiply(captured, projection, view);
        CHECK(ViewProjectionOk(captured));
        const SunProjection doubled = ProjectSun(captured, direction, window);
        CHECK(doubled.behind);
        CHECK(!SunVisible(doubled));

        // The same sun as the WORLD sees it: elevation 0.758 (49 degrees up), and that vector through the same view-projection lands on the very
        // position the projection gives the view-space vector - the identity a capture's own columns state.
        float world[3] {};
        CHECK(WorldDirectionOf(view, direction, world));
        CHECK(Near(world[1], 0.757967f, 1.0e-4f));
        CHECK(Near(world[2], 0.652293f, 1.0e-4f));
        CHECK(Near(WorldElevationOf(view, direction), 0.757967f, 1.0e-4f)); // not the direction's own y, 0.318
        const SunProjection byWorld = ProjectSun(captured, world, window);
        CHECK(byWorld.unit && !byWorld.behind);
        CHECK(Near(byWorld.screenX, placed.screenX, 0.01f) && Near(byWorld.screenY, placed.screenY, 0.01f));

        // And back: the view's rows turn the world vector into the light's own direction, which is how a sun held across a camera that turned
        // is still projected where the sun is.
        float back[3] {};
        CHECK(ViewDirectionOf(view, world, back));
        CHECK(Near(back[0], direction[0]) && Near(back[1], direction[1]) && Near(back[2], direction[2]));

        // The horizon test reads the WORLD elevation, not the direction's own y: a level camera rolled upside down (its second row is -y) makes a
        // sun 0.6 above the horizon point down the view by 0.6, which the old rule refused as the sun's mirror.
        // Row-major, four floats a row as the buffer stores it: rows (-1, 0, 0), (0, -1, 0), (0, 0, 1).
        const float rolled[12] { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        LightFacts upsideDown;
        upsideDown.direction[0] = -0.8f;
        upsideDown.direction[1] = -0.6f;
        upsideDown.direction[2] = 0.0f;
        CHECK_EQ(uint32_t(ClassifyLight(upsideDown)), uint32_t(LightVerdict::BelowHorizon)); // what the direction's own y says
        CHECK(Near(WorldElevationOf(rolled, upsideDown.direction), 0.6f));
        CHECK_EQ(uint32_t(ClassifyLight(upsideDown, WorldElevationOf(rolled, upsideDown.direction))), uint32_t(LightVerdict::Sun));
        CHECK(IsSunLight(upsideDown, WorldElevationOf(rolled, upsideDown.direction)));

        // A view-projection that is not one is refused before anything is projected with it: a buffer whose +0x060 holds a translation, or an
        // all-zero matrix, is NoCamera rather than a sun in the wrong place.
        float translated[16] {};
        std::memcpy(translated, captured, sizeof translated);
        translated[12] = 3.0f;
        CHECK(!ViewProjectionOk(translated));
        float scaled[16] {};
        std::memcpy(scaled, captured, sizeof scaled);
        scaled[14] = -4.0f;
        CHECK(!ViewProjectionOk(scaled));
        float nanMatrix[16] {};
        std::memcpy(nanMatrix, captured, sizeof nanMatrix);
        nanMatrix[3] = std::nanf("");
        CHECK(!ViewProjectionOk(nanMatrix));
        float empty[16] {};
        CHECK(!ViewProjectionOk(empty));
        CHECK(!ViewProjectionOk(nullptr));
    }

    // ---- the read-back ring: a copy is read at a later frame's tone map, never at the frame that made it, and an older copy never replaces a
    //      newer one. The old code copied and mapped in the same frame, which asks a GPU that is a frame behind for bytes it has not written;
    //      the shape it was replaced with (one exact age, one map attempt) read nothing at all in a session where that one attempt never landed.
    {
        CHECK(kStagingRing >= 4); // a copy survives being retried while the GPU catches up: it is a candidate at ages 1 .. ring - 1
        CHECK_EQ(kNewestReadAge, 1u);
        CHECK(kStagingRing > kNewestReadAge);
        CHECK_EQ(RingIndex(0), 0u);
        CHECK_EQ(RingIndex(1), 1u);
        CHECK_EQ(RingIndex(2), 2u);
        CHECK_EQ(RingIndex(3), 3u);
        CHECK_EQ(RingIndex(4), 0u);
        CHECK_EQ(RingIndex(7), 3u);

        // Every age the ring still holds is a slot of its own, and this frame's own copy (age 0) is never one of them.
        for (uint64_t frame = uint64_t(kStagingRing); frame < 64; ++frame)
        {
            for (uint32_t age = kNewestReadAge; age < kStagingRing; ++age)
            {
                CHECK_EQ(SlotForAge(frame, age), RingIndex(frame - age));
                CHECK(SlotForAge(frame, age) != RingIndex(frame)); // never this frame's own slot
            }
            CHECK(!ReadCandidate(true, frame, frame, false, 0));                     // the copy this frame issued: age 0
            CHECK(ReadCandidate(true, frame - kNewestReadAge, frame, false, 0));      // the newest copy a read may take
            CHECK(ReadCandidate(true, frame - (kStagingRing - 1), frame, false, 0));  // the oldest the ring still holds
            CHECK(!ReadCandidate(true, frame - kStagingRing, frame, false, 0));       // past the ring's depth: already written over
            CHECK(!ReadCandidate(false, frame - kNewestReadAge, frame, false, 0));    // nothing was ever written there
            CHECK(!ReadCandidate(true, frame + 1, frame, false, 0));                  // a copy of a later frame is not this frame's
        }

        // A newer copy is never replaced by an older one: after frame 12's copy was read, the ring's older slots are no candidates at all.
        CHECK(!ReadCandidate(true, 12, 15, true, 12));
        CHECK(!ReadCandidate(true, 11, 15, true, 12));
        CHECK(ReadCandidate(true, 13, 15, true, 12));
        CHECK(ReadCandidate(true, 12, 15, true, 11));

        // The ring and the "same frame" tests run on the module's own count of rendered frames, not on the output-present token: that counter steps
        // about 1.8 times inside one rendered frame with frame generation on, so a light draw and the tone map it feeds would answer "different
        // frames" whenever a present landed between them - which is what dropped half of the session's frames. One rendered frame is the light
        // pass and the tone map it feeds; the count moves when the frame closes, and only then.
        {
            uint64_t frames = 0;
            uint64_t sunDrawFrames = 0;
            for (uint64_t frame = 0; frame < 6; ++frame)
            {
                sunDrawFrames = frames;                              // the frame's light draw stores the count it saw
                const uint64_t tokenAtLightDraw = 100 + frame * 2;    // and the output token it saw, which is not an identity
                const uint64_t tokenAtToneMap = tokenAtLightDraw + 1; // one generated output was presented in between
                CHECK(tokenAtLightDraw != tokenAtToneMap);
                CHECK(SameRenderedFrame(sunDrawFrames, frames));       // the tone map belongs to that light draw's frame
                CHECK_EQ(frames, frame);                              // nothing moved the count inside the frame
                AdvanceFrame(frames);                                  // the frame's tone map closes it
                CHECK_EQ(frames, frame + 1);
                CHECK(!SameRenderedFrame(sunDrawFrames, frames));      // and a later tone map of the same token is not that frame's
            }
            CHECK_EQ(frames, 6u);
            // A frame whose tone map never came leaves the count where it is: the next frame's light draw sees the same count and is not taken
            // again, so that frame's tone map composites with this frame's light draw and depth - a stale-frame path the identity does not remove.
            CHECK(!SameRenderedFrame(sunDrawFrames, frames));
        }

        // A not-ready map is not a refusal: it is a read that has not landed, and the values it would have replaced stay.
        CHECK(!ReadStale(false, 0, 1'000'000)); // never read: never stale, the frames are skipped as "no sun draw's bytes yet"
        CHECK(!ReadStale(true, 10, 10 + kMaxReadbackAge));
        CHECK(ReadStale(true, 10, 10 + kMaxReadbackAge + 1));
        CHECK(!ReadStale(true, 10, 10)); // the frame of the read itself
        // The bound in literal frames: a read ninety frames old still frames the picture and one ninety-one frames old does not.
        CHECK(!ReadStale(true, 10, 100));
        CHECK(ReadStale(true, 10, 101));
        CHECK_EQ(kMaxReadbackAge, 90u);
    }

    // ---- the buffer a refused light came from: the engine's light parameter buffers are a small pool reused within the frame, so the pointer is
    //      only trusted for a while and the sun is looked at again instead of being blocked for the rest of the session.
    {
        CHECK(RejectedStill(true, 100, 100));
        CHECK(RejectedStill(true, 100, 100 + kRejectedLightFrames - 1));
        CHECK(!RejectedStill(true, 100, 100 + kRejectedLightFrames));
        CHECK(!RejectedStill(false, 100, 100)); // nothing was ever refused
        CHECK(!RejectedStill(true, 100, 99));   // a counter that went backwards is not a hold
    }

    // ---- which of a light draw's depth views to read: one at the scene's own size wins over the first one (a shadow map or a reflection's
    //      depth in a lower slot), because the tone map refuses a depth of another size and that refusal would count on every frame.
    {
        CHECK(BetterDepthView(false, false, false)); // nothing found yet
        CHECK(BetterDepthView(false, false, true));
        CHECK(BetterDepthView(true, false, true));   // a view of the scene's size beats the first one
        CHECK(!BetterDepthView(true, true, true));   // the first of the scene's size wins
        CHECK(!BetterDepthView(true, true, false));  // and a smaller one never replaces it
        CHECK(!BetterDepthView(true, false, false)); // the first of another size stays
    }

    // ---- the sun's own draw: the light-buffer pair, written without blending, beside the frame's depth and camera. The rows are a captured
    //      frame's own (draws.csv, format 3): the wetness mask's four R8 draws come first, then the sun (two R11G11B10 targets, blend off),
    //      then every local light, which writes the same pair with blending on. The old rule took the first g_LightParam draw that had a depth
    //      and a camera, which is a wetness draw whose light says "straight up": the feature could never draw, and it counted nothing.
    {
        // The wetness mask's volume draw, as the capture has it.
        SunDrawFacts wetness;
        wetness.targets = 1;
        wetness.sameTargets = true;
        wetness.targetFormat = 28; // R8_UNORM
        wetness.targetWidth = 3840;
        wetness.targetHeight = 2160;
        wetness.blended = true;
        wetness.sceneWidth = 3840;
        wetness.sceneHeight = 2160;
        wetness.depth = true;  // it binds the depth and the camera too, which is why the old rule accepted it
        wetness.camera = true;
        CHECK(!IsSunDraw(wetness));

        // The sun's own light draw of the first light pass.
        SunDrawFacts sunDraw = wetness;
        sunDraw.targets = 2;
        sunDraw.targetFormat = kLightTargetFormat;
        sunDraw.blended = false;
        CHECK(IsSunDraw(sunDraw));
        CHECK_EQ(kLightTargetFormat, 26u); // R11G11B10_FLOAT

        // A local light writes the same pair, with blending on.
        SunDrawFacts local = sunDraw;
        local.blended = true;
        CHECK(!IsSunDraw(local));

        // The second light pass writes another pair of the same shape: the first match of a frame is the one the composite reads, so it wins
        // by being earlier, not by being brighter.
        CHECK(IsSunDraw(sunDraw));

        // Anything the pass cannot use is not the sun's draw: no pair, a pair of another format or size, a pair that is not the scene's size,
        // a draw without the depth, one without the camera.
        SunDrawFacts other = sunDraw;
        other.targets = 3;
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.targetFormat = 10; // R16G16B16A16_FLOAT
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.sameTargets = false; // two targets of different sizes
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.targetWidth = 1920;
        other.targetHeight = 1080; // a pair at the render size, not the allocation the scene lives in
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.depth = false;
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.camera = false;
        CHECK(!IsSunDraw(other));
        other = sunDraw;
        other.sceneWidth = other.sceneHeight = 0; // the session's first frames, before a composite was seen
        CHECK(IsSunDraw(other));

        // ---- the light's own parameters, from the capture's two rows
        LightFacts sun;
        sun.position[0] = sun.position[1] = sun.position[2] = 0.0f;
        sun.direction[0] = -0.0146525f;
        sun.direction[1] = 0.999893f;
        sun.direction[2] = 5.95828e-08f;
        CHECK(IsUnitVector(sun.direction));
        CHECK(IsSunLight(sun)); // the live build's directional light: no position, a unit direction, a zero colour

        // The wetness draw's own light: no position and straight up, which is a unit vector - the shape rule is what refuses it, and this is the
        // row the old candidate table filled first.
        LightFacts wetnessLight;
        wetnessLight.direction[1] = 1.0f;
        CHECK(IsSunLight(wetnessLight)); // the parameters alone cannot tell it from a zenith sun
        CHECK(!IsSunDraw(wetness));      // the draw's shape can

        // The lamp of the interior capture: a world position, a unit direction and its own colour.
        LightFacts lamp;
        lamp.position[0] = 43.1969f;
        lamp.position[1] = 15.1779f;
        lamp.position[2] = -66.3372f;
        lamp.direction[0] = -0.440384f;
        lamp.direction[1] = -0.179535f;
        lamp.direction[2] = 0.879675f;
        lamp.diffuse[0] = 2.25f;
        lamp.diffuse[1] = 1.9272f;
        lamp.diffuse[2] = 1.08405f;
        CHECK(IsUnitVector(lamp.direction));
        CHECK(!IsSunLight(lamp)); // a lamp's direction is a unit vector too: the position is what refuses it
        // A local light's draw is already refused by the blend rule (it accumulates into the pair); the position rule is the second half of the
        // identification, and the one that catches a build whose sun pass blends, or a light that writes the pair on its own.
        CHECK(!IsSunDraw(local));

        // A spot light with a huge colour is a lamp, not a sun.
        LightFacts spot;
        spot.position[0] = -30.7452f;
        spot.position[1] = 4.53519f;
        spot.position[2] = -13.0176f;
        spot.direction[1] = -0.999969f;
        spot.direction[2] = 0.00784561f;
        spot.diffuse[0] = 100.0f;
        CHECK(!IsSunLight(spot));

        // ---- the sun's direction points at the light, so it is above the horizon: the captured afternoon sun and its mirror, which is the value
        //      another directional light of the same frame carries and the one that produced the "0.46 .. 0.75 windows outside" family.
        {
            LightFacts afternoon;
            afternoon.direction[0] = 0.445f;
            afternoon.direction[1] = 0.511f;
            afternoon.direction[2] = 0.736f;
            CHECK(IsUnitVector(afternoon.direction));
            CHECK(IsSunLight(afternoon));
            CHECK(ClassifyLight(afternoon) == LightVerdict::Sun);
            LightFacts mirror = afternoon;
            mirror.direction[0] = -0.445f;
            mirror.direction[1] = -0.511f;
            mirror.direction[2] = -0.736f;
            CHECK(IsUnitVector(mirror.direction)); // the parameters alone cannot tell them apart: the sign is what does
            CHECK(!IsSunLight(mirror));
            CHECK(ClassifyLight(mirror) == LightVerdict::BelowHorizon);
            const std::string named = FormatBelowHorizon(mirror);
            CHECK(named.find("Light shafts") == 0);
            CHECK(named.find("below the horizon") != std::string::npos);
            CHECK(named.find("mirror") != std::string::npos);
            CHECK(named.find("-0.5110") != std::string::npos);
            // The bound itself is a tolerance about a degree below the horizon line, so a sun 1 to 3 degrees up at dawn or dusk is still the sun,
            // and every light it refuses is a degree or more below the line: a sun exactly on the line, one a degree up, one half a degree below
            // it, one three degrees below it, and the afternoon sun's own mirror (above).
            LightFacts level;
            level.direction[2] = -1.0f;
            CHECK(ClassifyLight(level) == LightVerdict::Sun); // exactly on the horizon line: still the sun
            LightFacts lowSun;
            lowSun.direction[1] = 0.02f;
            lowSun.direction[2] = -std::sqrt(1.0f - lowSun.direction[1] * lowSun.direction[1]);
            CHECK(ClassifyLight(lowSun) == LightVerdict::Sun); // about a degree above the line
            LightFacts justBelow;
            justBelow.direction[1] = -0.01f;
            justBelow.direction[2] = -std::sqrt(1.0f - justBelow.direction[1] * justBelow.direction[1]);
            CHECK(ClassifyLight(justBelow) == LightVerdict::Sun); // half a degree below it: inside the tolerance
            LightFacts sinking;
            sinking.direction[1] = -0.05f;
            sinking.direction[2] = -std::sqrt(1.0f - sinking.direction[1] * sinking.direction[1]);
            CHECK(ClassifyLight(sinking) == LightVerdict::BelowHorizon); // three degrees below it: not a sun
            CHECK_EQ(kMinSunElevation, -0.02f); // the tolerance the design names, written out
            // a lamp stays a lamp whatever its direction says, and a direction that is not unit is not a sun at all
            LightFacts lampBelow = lamp;
            lampBelow.direction[1] = -1.0f;
            lampBelow.direction[2] = 0.0f;
            CHECK(ClassifyLight(lampBelow) == LightVerdict::Lamp);
            LightFacts off = sun;
            off.direction[1] = 0.5f;
            CHECK(ClassifyLight(off) == LightVerdict::NoDirection);
        }

        // A direction that is not a unit vector is not a sun at infinity, and a NaN or a zero direction is no sun at all.
        LightFacts off;
        off.direction[1] = 0.5f;
        CHECK(!IsUnitVector(off.direction) && !IsSunLight(off));
        LightFacts shortDirection;
        shortDirection.direction[2] = 0.9f;
        CHECK(!IsUnitVector(shortDirection.direction));
        LightFacts zero;
        CHECK(!IsUnitVector(zero.direction));
        LightFacts nan;
        nan.direction[0] = std::nanf("");
        nan.direction[1] = 1.0f;
        CHECK(!IsUnitVector(nan.direction) && !IsSunLight(nan));
        LightFacts tooLong;
        tooLong.direction[0] = 0.618f;
        tooLong.direction[1] = -0.824f; // 1.03 of a unit: past the 2 % the rule allows
        CHECK(!IsUnitVector(tooLong.direction));

        // ---- the frame's directional shadow: the identity the light has to pass before its own direction places the sun. The bytes are the
        //      128 a captured frame's `g_DirectionalShadowParameter` holds (its hex, decoded), the sun is that frame's own sun draw, and the yawed
        //      light is the one the module drew from 4.6 s later - which the session's log implies and the review measured.
        {
            // The whole 128-byte buffer, as the capture printed it: a light-space matrix, then the distance and the map parameters.
            const float bytes[kShadowFloats] {
                0.0484935865f, -0.00731439004f, -4.53086315e-08f, 0.500575721f,
                0.00248205499f, 0.0164556783f, 0.0106650954f, 0.250255108f,
                -0.00554635376f, -0.0367720611f, 0.0580281243f, 1.00341737f,
                0.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 0.00769290933f, 190.0f, 0.0f,
                0.00048828125f, 0.000122070312f, 0.0526315682f, 1.0f,
                -2.9000001f, 12.9989834f, 0.666666687f, 0.0f,
                -0.000553447695f, -14.5092039f, -16.2876949f, 1.0f,
            };
            CHECK(ShadowMatrixShape(bytes));
            // Its third row (at +0x20), negated and normalised, is the frame's sun to the buffer's precision (the capture's own two rows agree
            // to 1e-4 degrees).
            const float* row = bytes + kShadowRowAt / 4;
            const float sun[3] { 0.0804743f, 0.533533f, -0.841942f }; // the captured `g_LightParam` of the sun's own draw
            const float yawed[3] { 0.7624f, 0.5300f, -0.3713f };      // the light the 17:53:08 run line drew from
            CHECK(std::fabs(ShadowDegrees(row, sun)) < 0.001f);
            CHECK(ShadowDegrees(row, yawed) > 48.0f && ShadowDegrees(row, yawed) < 50.0f);
            // The yaw keeps the sun's elevation (32.0 against 32.2 degrees), which is why the elevation rule could not tell them apart and the
            // frame's shadow is the identity the pass needs.
            const float pitch = std::asinf(yawed[1]) * (180.0f / 3.14159265358979f);
            CHECK(pitch > 31.5f && pitch < 32.5f);
            // The mirror light is 180 degrees from the row's axis, not a wrapped small value.
            const float mirror[3] { -sun[0], -sun[1], -sun[2] };
            CHECK(ShadowDegrees(row, mirror) > 179.0f);
            // A row that is not a vector is not a light that disagrees (the buffer was not the shadow's): negative, never "far".
            const float zeroRow[3] { 0.0f, 0.0f, 0.0f };
            const float nanRow[3] { std::nanf(""), 0.0f, 1.0f };
            CHECK(ShadowDegrees(zeroRow, sun) < 0.0f && ShadowDegrees(nanRow, sun) < 0.0f);
            CHECK(ShadowDegrees(nullptr, sun) < 0.0f && ShadowDegrees(row, nullptr) < 0.0f);
            // The tolerance is a few degrees: a build that smooths one of the two still agrees, a light that is another direction does not.
            const float nudged[3] { 0.0808f, 0.5336f, -0.8419f }; // about a fifth of a degree off
            CHECK(ShadowDegrees(row, nudged) < kShadowAgreementDegrees);
            CHECK(kShadowAgreementDegrees >= 2.0f && kShadowAgreementDegrees <= 5.0f);
            // The shape rule is what keeps a neighbour out: the captured sun draw binds `g_FakeSpecularParam` at the same size, and a buffer of
            // another shape is not judged - a frame of nothing but such buffers is Missing (the direction alone decides), never refused.
            const float neighbour[kShadowFloats] {
                0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
                0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
            };
            CHECK(!ShadowMatrixShape(neighbour));
            CHECK(!ShadowMatrixShape(nullptr));
            // A matrix whose last row is not (0,0,0,1) is not a light-space transform, and a NaN anywhere refuses the shape.
            float notAShadow[kShadowFloats] {};
            std::memcpy(notAShadow, bytes, sizeof notAShadow);
            notAShadow[15] = 0.5f;
            CHECK(!ShadowMatrixShape(notAShadow));
            float naned[kShadowFloats] {};
            std::memcpy(naned, bytes, sizeof naned);
            naned[7] = std::nanf("");
            CHECK(!ShadowMatrixShape(naned));
            float unnormalised[kShadowFloats] {};
            std::memcpy(unnormalised, bytes, sizeof unnormalised);
            unnormalised[12] = 0.25f; // a fourth row that translates: not the light-space matrix this rule reads
            CHECK(!ShadowMatrixShape(unnormalised));
            // The judgement over the candidates one light draw bound, once with the shadow's buffer beside the neighbour's and once the other way
            // round: a slot order is never what decides it.
            ShadowCandidate shadow;
            shadow.shaped = ShadowMatrixShape(bytes);
            std::memcpy(shadow.row, row, sizeof shadow.row);
            ShadowCandidate fake;
            fake.shaped = ShadowMatrixShape(neighbour);
            std::memcpy(fake.row, neighbour + kShadowRowAt / 4, sizeof fake.row);
            const ShadowCandidate candidates[2] { shadow, fake };
            const ShadowJudgement agreed = JudgeShadow(candidates, 2, sun);
            CHECK(agreed.result == ShadowResult::Agreed);
            CHECK(agreed.degrees >= 0.0f && agreed.degrees < 0.001f);
            const ShadowCandidate reversed[2] { fake, shadow };
            CHECK(JudgeShadow(reversed, 2, sun).result == ShadowResult::Agreed);
            // A light the frame's own shadow says no to: the 17:53:08 draw, refused with its own angle and line.
            const ShadowJudgement refused = JudgeShadow(candidates, 2, yawed);
            CHECK(refused.result == ShadowResult::Refused);
            CHECK(refused.degrees > 48.0f && refused.degrees < 50.0f);
            const std::string named = FormatShadowRefused(yawed, refused.degrees);
            CHECK(named.find("48.9 degrees") != std::string::npos);
            CHECK(named.find("not this frame's sun") != std::string::npos);
            CHECK(named.find("(0.7624, 0.5300, -0.3713)") != std::string::npos);
            // No candidate, no shaped candidate, or a missing list: the light's own direction alone decides, and the line says so - the effect is
            // never silently refused because the anchor is absent.
            CHECK(JudgeShadow(nullptr, 0, sun).result == ShadowResult::Missing);
            CHECK(JudgeShadow(candidates, 0, sun).result == ShadowResult::Missing);
            CHECK(JudgeShadow(nullptr, 2, sun).result == ShadowResult::Missing);
            const ShadowCandidate onlyFake[1] { fake };
            CHECK(JudgeShadow(onlyFake, 1, sun).result == ShadowResult::Missing);
            CHECK(std::string(kShadowAnchorMissing).find("own direction") != std::string::npos);
            // A build whose matrix stores the light's axis unnegated is accepted only with the other sign, and a light that is another direction
            // still fails: the fallback is a sign, not a tolerance.
            float flippedBytes[kShadowFloats] {};
            std::memcpy(flippedBytes, bytes, sizeof flippedBytes);
            flippedBytes[8] = -bytes[8];
            flippedBytes[9] = -bytes[9];
            flippedBytes[10] = -bytes[10];
            CHECK(ShadowMatrixShape(flippedBytes));
            ShadowCandidate flipped;
            flipped.shaped = ShadowMatrixShape(flippedBytes);
            std::memcpy(flipped.row, flippedBytes + kShadowRowAt / 4, sizeof flipped.row);
            const ShadowCandidate flippedCandidates[1] { flipped };
            const ShadowJudgement unnegated = JudgeShadow(flippedCandidates, 1, sun);
            CHECK(unnegated.result == ShadowResult::AgreedUnnegated);
            CHECK(unnegated.degrees >= 0.0f && unnegated.degrees < 0.001f);
            CHECK(JudgeShadow(flippedCandidates, 1, yawed).result == ShadowResult::Refused);
            CHECK(std::string(kShadowUnnegated).find("unnegated") != std::string::npos);
            CHECK(std::string(kNotTheSunSkip).find("directional shadow") != std::string::npos);
            CHECK_EQ(kShadowBytes, 0x80u);
            CHECK_EQ(kShadowRowAt, 0x20u);
            CHECK_EQ(kShadowFloats, 32u);
            CHECK(kShadowCandidates >= 2u); // the captured sun draw binds two 128-byte buffers
        }

        // the zero-colour directional light the live build reports: the colour rule sends the pass to the sky instead of drawing nothing
        const float zeroColour[3] { 0.0f, 0.0f, 0.0f };
        CHECK(ResolveSunTint(zeroColour).fromSky);
        CHECK(Near(ResolveSunTint(zeroColour).rgb[0], 1.0f));
        // The live build reports no colour for the sun, so the colour section above needs a build that does: the same light with a colour.
        LightFacts colouredSun = sun;
        colouredSun.diffuse[0] = 1.0f;
        colouredSun.diffuse[1] = 0.9f;
        colouredSun.diffuse[2] = 0.8f;
        const SunTint warm = ResolveSunTint(colouredSun.diffuse);
        CHECK(!warm.fromSky);
        CHECK(Near(warm.rgb[0], 1.0f) && Near(warm.rgb[1], 0.9f) && Near(warm.rgb[2], 0.8f));
        // The colour is a colour, not a magnitude: the 16 clamp first, then the peak of the clamped triple - so a light that blows past the clamp
        // still comes back as its own hue.
        const float negative[3] { -2.0f, 0.5f, 100.0f };
        const SunTint bounded = ResolveSunTint(negative);
        CHECK(!bounded.fromSky);
        CHECK(Near(bounded.rgb[0], 0.0f) && Near(bounded.rgb[1], 0.03125f) && Near(bounded.rgb[2], 1.0f));
        const float nanColour[3] { std::nanf(""), 1.0f, 1.0f };
        CHECK(ResolveSunTint(nanColour).fromSky);
        // The captured frame's own directional light: 1.96 is the magnitude the engine reports for that sun, and the key is what says how much
        // light there is - so the tint is that light's colour at peak 1 ({1.96, 1.79, 1.58} -> {1.0, 0.916, 0.807}), not 1.96 times the key.
        const float captured[3] { 1.96f, 1.79455f, 1.58069f };
        const SunTint capturedTint = ResolveSunTint(captured);
        CHECK(!capturedTint.fromSky);
        CHECK(Near(capturedTint.rgb[0], 1.0f) && Near(capturedTint.rgb[1], 0.915586f) && Near(capturedTint.rgb[2], 0.806474f));
        CHECK(Near(capturedTint.rgb[0], 1.0f) && capturedTint.rgb[0] >= capturedTint.rgb[1] && capturedTint.rgb[1] >= capturedTint.rgb[2]);
        // A session's other light, 0.048/0.112/0.16: the same key gives the same amount of light in that scene, only bluer.
        const float dim[3] { 0.0482276f, 0.111635f, 0.16f };
        const SunTint dimTint = ResolveSunTint(dim);
        CHECK(Near(dimTint.rgb[2], 1.0f) && Near(dimTint.rgb[0], 0.301423f));
    }

    // ---- the added light is measured against the white the pass reads, and it can never push a channel past that white
    {
        // A frame reaches display-white where its tone map's exposure multiplier's reciprocal is. The captured frame measured the crossing at a
        // scene 0.838 and the local gain at 1.21, and the two agree: 1/1.21 = 0.826, within 1.5 %. The measured 0.838 is the ceiling, so the live
        // value is used inside the band that measurement supports and clamped at its edges.
        CHECK(Near(WhitePointFromExposure(1.21f), 0.826446f));
        CHECK(Near(kVerifiedWhitePoint, 0.838f) && Near(kMaxWhitePoint, kVerifiedWhitePoint));
        CHECK(Near(WhitePointFromExposure(2.0f), 0.5f)); // a dusk frame exposes more: its white is nearer the scene's values
        // An exposure that claims a white above the measured one is not a value this build has verified, and a white that is too high is the one
        // direction in which the bound can be pushed past the frame's real white (a wrong-but-in-range .y = 0.5 claims 2.0).
        CHECK(Near(WhitePointFromExposure(0.5f), kVerifiedWhitePoint));
        CHECK(Near(WhitePointFromExposure(0.0f), kMaxWhitePoint)); // a zero or a nonsense exposure is clamped, not divided into an infinity
        CHECK(Near(WhitePointFromExposure(-2.0f), kMaxWhitePoint));
        CHECK(Near(WhitePointFromExposure(1.0e6f), kMinWhitePoint));
        CHECK(Near(WhitePointFromExposure(std::nanf("")), kFallbackWhitePoint));
        CHECK(!ExposureUsable(std::nanf("")) && !ExposureUsable(0.0f) && !ExposureUsable(-1.0f));
        CHECK(ExposureUsable(1.21f) && ExposureUsable(kMinExposure) && ExposureUsable(kMaxExposure));
        // A buffer whose second float is the multiplier's reciprocal (0.8895, the value the captured session's drawn frames carried) is accepted and
        // lands on the measured white, not refused: refusing it is what left those frames drawing against the fallback.
        CHECK(ExposureUsable(0.8895f) && Near(WhitePointFromExposure(0.8895f), kVerifiedWhitePoint));
        CHECK(!ExposureUsable(kMinUsableExposure - 0.01f) && kMinUsableExposure < kMinExposure); // below the floor is no tone map's parameters
        CHECK(WhitePointUsable(kFallbackWhitePoint) && WhitePointUsable(kMinWhitePoint) && WhitePointUsable(kMaxWhitePoint));
        CHECK(!WhitePointUsable(0.0f) && !WhitePointUsable(std::nanf("")) && !WhitePointUsable(kMaxWhitePoint + 1.0f));
        // The fallback is the captured frame's measurement, not the band's dark end: a frame the pass draws shafts on is a daylight frame, and the
        // session's own reads of those frames were a white of about 0.84 whenever the copy landed. A dark-end fallback left `head = white - scene`
        // at zero on every lit surface of such a frame, so a screenshot with thousands of drawn frames showed the scene's pixels plus nothing.
        CHECK(Near(kFallbackWhitePoint, kVerifiedWhitePoint));
        CHECK(kFallbackWhitePoint > kMinWhitePoint && kFallbackWhitePoint <= kVerifiedWhitePoint);
        // The band's dark end is still reachable from a live exposure, so the two ends are pinned apart.
        CHECK(Near(WhitePointFromExposure(kMaxExposure), kMinWhitePoint));
        // The staleness rule: a white point is this frame's only while a copy has landed and it is no more than kMaxWhiteStaleFrames old.
        CHECK(WhitePointStale(false, 0, 0) && WhitePointStale(false, 10, 11));
        CHECK(!WhitePointStale(true, 10, 10) && !WhitePointStale(true, 10, 10 + kMaxWhiteStaleFrames));
        CHECK(WhitePointStale(true, 10, 11 + kMaxWhiteStaleFrames));
        CHECK(kMaxWhiteStaleFrames >= 30 && kMaxWhiteStaleFrames <= 600); // long enough for a GPU behind, short for a stopped copy
        // The two live-value cases, as numbers: a wrong-but-in-range .y = 0.5 on a frame whose real white is 0.838, and the fallback on a computed
        // 0.2-white dusk frame at the top key. Both are bounded by the white the pass reads, and neither can push a channel past it - which is the
        // whole contract: the pass never decides how bright the frame is, it only never adds past the white it read.
        const float dayWhite = 0.838f, duskWhite = 0.2f;
        const float wrongLive = WhitePointFromExposure(0.5f); // what a wrong .y of 0.5 gives: the ceiling, not 2.0
        CHECK(Near(wrongLive, dayWhite));
        const float wallAdd = ShaftAdded(0.5f, kMaxIntensity * kShaftGain, wrongLive);
        CHECK(0.5f + wallAdd <= dayWhite); // no channel past the white the pass reads ...
        CHECK(wallAdd > 0.19f && wallAdd < 0.22f);
        CHECK(ShaftAdded(0.5f, kMaxIntensity * kShaftGain, 2.0f) > 0.36f); // ... where the unguarded 2.0 added 0.667 and read 1.17
        const float duskAdd = ShaftAdded(0.02f, kMaxIntensity * kShaftGain, kFallbackWhitePoint);
        CHECK(duskAdd > 0.0f && 0.02f + duskAdd <= kFallbackWhitePoint); // the bound holds, whatever the frame's own white would have been
        CHECK(duskAdd / duskWhite > 0.75f); // a 0.2-white dusk frame is where the fallback's measurement is the wrong one; the live value is its own
        CHECK(Near(ShaftAdded(0.02f, kMaxIntensity * kShaftGain, kVerifiedWhitePoint), duskAdd));
        // The residual hole, as a pin so the wording cannot come back: the bound is the white the pass reads, not a white it has measured for the
        // frame. The 0.838 ceiling - or, while the exposure has not landed, the 0.838 fallback - on a computed 0.2-white frame takes the top key's
        // 0.1 surface to 0.399, twice that frame's white, and its 0.02 surface to 0.331 (1.66x it).
        const float darkFrameAdd = ShaftAdded(0.1f, kMaxIntensity * kShaftGain, kVerifiedWhitePoint);
        CHECK(Near(0.1f + darkFrameAdd, 0.399f, 2.0e-3f));
        CHECK(0.1f + darkFrameAdd > duskWhite); // past the frame's real white: the band cannot tell a wrong-high value from a bright frame
        CHECK(Near(ShaftAdded(0.1f, kMaxIntensity * kShaftGain, duskWhite), 0.0545f, 1.0e-3f));
        // And a night frame, whose own white is 0.12: the fallback's 0.838 takes its 0.1 surface to 0.399 where the frame's own white would have
        // added 0.0157. It is the widest the hole gets, it is bounded by the same room-to-white rule, and a night frame with a live exposure lands
        // on its own white and is not affected.
        const float nightAdd = ShaftAdded(0.1f, kMaxIntensity * kShaftGain, kFallbackWhitePoint);
        CHECK(Near(0.1f + nightAdd, 0.399f, 2.0e-3f));
        CHECK(Near(ShaftAdded(0.1f, kMaxIntensity * kShaftGain, 0.12f), 0.0157f, 1.0e-3f));
        CHECK(PeakWhiteShare(kDefaultIntensity) > 0.22f && PeakWhiteShare(kDefaultIntensity) < 0.24f);
        CHECK(PeakWhiteShare(kMaxIntensity) > 0.37f && PeakWhiteShare(kMaxIntensity) < 0.38f);

        // The share of the white a pixel with no light of its own gets is the same share in a day frame and a dusk one, because the light is a
        // fraction of the frame's white, and it is below 1 at every key.
        CHECK(PeakWhiteShare(0.0f) == 0.0f && PeakWhiteShare(kMaxIntensity * 4.0f) == PeakWhiteShare(kMaxIntensity)); // clamped like the key
        for (float intensity : {0.1f, 0.5f, 1.0f, 2.0f})
            CHECK(PeakWhiteShare(intensity) < 1.0f && PeakWhiteShare(intensity) > 0.0f);
        const float day = 0.838f, dusk = 0.2f;
        CHECK(Near(ShaftAdded(0.0f, kDefaultIntensity * kShaftGain, day) / day, PeakWhiteShare(kDefaultIntensity)));   // a black pixel
        CHECK(Near(ShaftAdded(0.0f, kDefaultIntensity * kShaftGain, dusk) / dusk, PeakWhiteShare(kDefaultIntensity))); // day and dusk agree
        // No key, mask or white point puts a channel past the white it was given: every addition is bounded by the pixel's own room to it.
        for (float intensity : {0.0f, 0.1f, 0.5f, 1.0f, 2.0f})
            for (float white : {kMinWhitePoint, 0.5f, kFallbackWhitePoint, kVerifiedWhitePoint})
                for (float scene : {0.0f, 0.05f, 0.1f, 0.5f, 0.83f, 0.838f, 1.0f, 3.0f})
                {
                    const float add = ShaftAdded(scene, intensity * kShaftGain, white);
                    CHECK(add >= 0.0f);
                    if (scene < white)
                    {
                        CHECK(add <= white - scene + 1.0e-5f); // the sum never reaches the white ...
                        CHECK(scene + add <= white + 1.0e-5f);
                    }
                    else
                    {
                        CHECK(add == 0.0f); // ... and a pixel at or past the white gets nothing
                    }
                }
        // The measured case: the 0.5 wall under a full-mask column at the top of the slider reads 0.5 + 0.20 on the measured frame's white, never
        // the 0.5 + 3.92 a single session's keys added, and a pixel already at white gets nothing.
        const float wall = ShaftAdded(0.5f, kMaxIntensity * kShaftGain, kVerifiedWhitePoint);
        CHECK(wall > 0.19f && wall < 0.22f);
        CHECK(0.5f + wall < kVerifiedWhitePoint); // strictly below the white, so the tone map's own clip is never reached
        CHECK(ShaftAdded(kVerifiedWhitePoint, kMaxIntensity * kShaftGain, kVerifiedWhitePoint) == 0.0f);
        CHECK(ShaftAdded(0.5f + kVerifiedWhitePoint, kMaxIntensity * kShaftGain, kVerifiedWhitePoint) == 0.0f);
        CHECK(ShaftAdded(0.5f, 0.0f, kVerifiedWhitePoint) == 0.0f); // intensity 0 is today's picture
        CHECK(ShaftAdded(0.5f, std::nanf(""), kVerifiedWhitePoint) == 0.0f);
        CHECK(ShaftAdded(std::nanf(""), 0.06f, kVerifiedWhitePoint) == 0.0f);
        // The white point's own line, and the fallback's, so a session reads which one the bound used.
        CHECK(FormatWhitePoint(0.8264f, 1.21f).find("0.826") != std::string::npos);
        CHECK(FormatWhitePoint(0.8264f, 1.21f).find("1.210") != std::string::npos);
        CHECK(std::string(kWhitePointFallback).find("0.838") != std::string::npos);
        CHECK_EQ(kToneMapExposureAt, 0x04u); // the tone map's second float is the exposure multiplier (EnginePassRules.h names it)
        CHECK(kStagingRing >= 4u);           // the tone map's ring is as deep as the light's: an older copy answers a GPU that is behind
    }

    // ---- the sun a frame draws with when its own read has not landed: held for a few frames, faded, and dropped by a camera cut
    {
        // The fade is 1 for the frame the read belongs to and the one after it, then 0.85, 0.65, 0.45, 0.25 and 0.05 - the largest step between two
        // frames is 20 % of the light, where the first version fell from 1 to 0.5625 (44 %) in one frame at the render boundary, which frame
        // generation interpolates into a visible slide. A frame older than the last is skipped.
        CHECK(Near(HoldFade(0), 1.0f) && Near(HoldFade(1), 1.0f) && Near(HoldFade(2), 0.85f));
        CHECK(Near(HoldFade(3), 0.65f) && Near(HoldFade(4), 0.45f) && Near(HoldFade(5), 0.25f) && Near(HoldFade(6), 0.05f));
        CHECK(HoldFade(kHoldFrames + 1) == 0.0f && HoldFade(1000) == 0.0f);
        CHECK_EQ(kHoldFrames, 6u);
        // The step property, which is what "smooth" means here: no two consecutive frames lose more than a fifth of the light, and the fade falls
        // the whole way.
        for (uint32_t age = 0; age < kHoldFrames; ++age)
            CHECK(HoldFade(age) - HoldFade(age + 1) <= 0.2f + 1.0e-6f);
        for (uint32_t age = 0; age < kHoldFrames; ++age)
            CHECK(HoldFade(age) >= HoldFade(age + 1));
        CHECK(HoldFade(0) - HoldFade(2) < 0.2f && HoldFade(kHoldFrames) < 0.1f);

        // The held sun, driven through a frame sequence the way the draw path does: a read accepted at frame 10 is drawn at 1, 1, 0.85, ... and
        // is gone after the last weight; a camera cut seen by a landed read drops it at once (the module drops it before the next frame's weight is
        // asked for); and a frame whose own read missed does not lose it.
        {
            SunHold hold;
            CHECK(!hold.have && hold.Weight(10) == 0.0f); // nothing accepted yet
            LightFacts sun;
            sun.direction[1] = 0.5f;
            sun.direction[2] = -0.8660254f;
            const float world[3] { 0.0f, 0.5f, -0.8660254f }; // an identity view, so the world copy of that direction is the same vector
            hold.Accept(sun, world, 10);
            CHECK(hold.have && Near(hold.Weight(10), 1.0f) && Near(hold.Weight(11), 1.0f));
            // frame 12's read missed: the weight is the fade, not 0 (the on/off step the hold exists to remove)
            CHECK(Near(hold.Weight(12), 0.85f) && Near(hold.Weight(13), 0.65f));
            CHECK(Near(hold.Weight(16), 0.05f) && hold.Weight(17) == 0.0f && hold.Weight(100) == 0.0f);
            // a read that landed and saw a cut: the hold is dropped, and the frame draws only when a new sun lands
            hold.Drop();
            CHECK(!hold.have && hold.Weight(13) == 0.0f);
            hold.Accept(sun, world, 20);
            CHECK(Near(hold.Weight(20), 1.0f));
            // a frame number that went backwards (a session reset) is not an age: the weight is 0, never a huge fade
            CHECK(hold.Weight(19) == 0.0f);
            // and the accepted facts are the ones the frame draws with, beside the world copy a frame that turned is projected from
            CHECK(hold.facts.direction[1] == 0.5f && Near(hold.facts.direction[2], -0.8660254f));
            CHECK(Near(hold.world[1], 0.5f) && Near(hold.world[2], -0.8660254f));
        }
        // A camera cut: the view-projection's fourth row is the camera's own forward, so a turn of twenty degrees or more is a cut and a small turn
        // is not. `Perspective` writes the w row as (0, 0, -1, 0), which is a unit forward.
        float held[kProjectionRows] {};
        Perspective(held, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        CHECK(!CameraCut(held, held));
        float turned[kProjectionRows] {};
        std::memcpy(turned, held, sizeof turned);
        turned[12] = -0.5f;
        turned[13] = 0.0f;
        turned[14] = -0.866f; // 30 degrees off the held forward (0, 0, -1)
        CHECK(CameraCut(held, turned));
        turned[12] = -0.139f;
        turned[13] = 0.0f;
        turned[14] = -0.990f; // 8 degrees
        CHECK(!CameraCut(held, turned));
        turned[12] = 0.0f;
        turned[13] = 0.0f;
        turned[14] = 1.0f; // a half turn: the view behind the camera it held
        CHECK(CameraCut(held, turned));
        float zero[kProjectionRows] {};
        CHECK(!CameraCut(held, zero) && !CameraCut(zero, held)); // a matrix that is not a camera is not a cut
        CHECK(!CameraCut(nullptr, held) && !CameraCut(held, nullptr));
        // The ring's age rule after a refusal: the copy a refused read took is not an accepted one, so the next frame's read may take an older copy
        // than the last accepted one - which is what stops a refused read costing every frame until a two-frame-old copy lands (the alternation a
        // session showed for 85 frames).
        CHECK(!ReadCandidate(true, 3, 5, true, 4));  // while the refused read's own frame is the floor
        CHECK(ReadCandidate(true, 3, 5, false, 4));  // after Forget() cleared the floor
        CHECK(ReadCandidate(true, 5, 6, true, 4));   // ... and a newer copy is still taken first
        CHECK(std::string(kCameraCut).find("Light shafts") == 0 && std::string(kCameraCut).find("cut") != std::string::npos);
    }

    // ---- the colour key and the brightness floor: white is a neutral shaft, and a sky too dark to be a sun adds nothing
    {
        CHECK(ParseColour("white") == Colour::White);
        CHECK(ParseColour("White") == Colour::White);
        CHECK(ParseColour("WHITE") == Colour::White);
        CHECK(ParseColour("auto") == Colour::Auto);
        CHECK(ParseColour("") == Colour::Auto);
        CHECK(ParseColour("blue") == Colour::Auto);

        const float night[3] { 0.0f, 0.0f, 0.0f };
        const float deep[3] { 0.005f, 0.005f, 0.005f };
        const float sky[3] { 0.25f, 0.25f, 0.25f };
        const float floorRgb[3] { 0.02f, 0.02f, 0.02f };
        const float nanRgb[3] { std::nanf(""), 1.0f, 1.0f };
        CHECK(!TintBrightEnough(night) && !TintBrightEnough(deep));
        CHECK(TintBrightEnough(sky));
        CHECK(TintBrightEnough(floorRgb)); // the floor itself is a sun
        CHECK(!TintBrightEnough(nanRgb));
        CHECK(std::fabs(Luminance(sky) - 0.25f) < 1.0e-5f); // equal components weigh to themselves
        CHECK(Luminance(nullptr) == 0.0f);
        CHECK(!TintBrightEnough(nullptr));

        // white ignores the light's own colour entirely; auto still reads it
        const float warm[3] { 1.0f, 0.9f, 0.8f };
        const SunTint white = ResolveSunTint(warm, Colour::White);
        CHECK(!white.fromSky);
        CHECK(Near(white.rgb[0], 1.0f) && Near(white.rgb[1], 1.0f) && Near(white.rgb[2], 1.0f));
        const SunTint coloured = ResolveSunTint(warm, Colour::Auto);
        CHECK(!coloured.fromSky && Near(coloured.rgb[1], 0.9f));
        CHECK(ResolveSunTint(night, Colour::Auto).fromSky);
    }

    // ---- the tone-map draw: one FP16 render target at the scene's size, and a t0 that is the scene the frame's composite wrote
    {
        ToneMapFacts facts;
        facts.renderTargets = 1;
        facts.targetWidth = 3840;
        facts.targetHeight = 2160;
        facts.targetFormat = kFp16Format;
        facts.sceneFormat = kFp16Format;
        facts.sceneWidth = 3840;
        facts.sceneHeight = 2160;
        facts.sceneSampled = true;
        facts.sceneIsScene = true;
        facts.window = Window { 0.5f, 0.5f, 1920.0f, 1080.0f };
        CHECK(ClassifyToneMap(facts) == Decline::None);

        ToneMapFacts replay = facts;
        replay.replay = true;
        CHECK(ClassifyToneMap(replay) == Decline::CharaReplay);

        ToneMapFacts twoTargets = facts;
        twoTargets.renderTargets = 2;
        CHECK(ClassifyToneMap(twoTargets) == Decline::RenderTarget);

        // The engine reuses the scene texture for its own post-tone copy: a draw that writes the scene is not the tone map at all, and it is
        // passed on without counting (the old order counted it as a render-target refusal on every frame, which stands the module down).
        ToneMapFacts inPlace = facts;
        inPlace.targetIsScene = true;
        CHECK(ClassifyToneMap(inPlace) == Decline::NotToneMap);
        // A draw of the same family that reads another texture is passed on too, whatever its target looks like.
        ToneMapFacts anotherTexture = facts;
        anotherTexture.sceneIsScene = false;
        anotherTexture.targetIsScene = true;
        CHECK(ClassifyToneMap(anotherTexture) == Decline::NotToneMap);

        ToneMapFacts target8Bit = facts;
        target8Bit.targetFormat = 87; // B8G8R8A8_UNORM: another post pass
        CHECK(ClassifyToneMap(target8Bit) == Decline::TargetFormat);

        ToneMapFacts notTheScene = facts;
        notTheScene.sceneIsScene = false; // t0 is the previous post stage's target, not the lit scene
        CHECK(ClassifyToneMap(notTheScene) == Decline::NotToneMap);

        ToneMapFacts not2D = facts;
        not2D.sceneSampled = false;
        CHECK(ClassifyToneMap(not2D) == Decline::NotToneMap);

        ToneMapFacts scene8Bit = facts;
        scene8Bit.sceneFormat = 87;
        CHECK(ClassifyToneMap(scene8Bit) == Decline::SceneFormat);

        ToneMapFacts mismatched = facts;
        mismatched.sceneHeight = 1440;
        CHECK(ClassifyToneMap(mismatched) == Decline::SizeMismatch);

        ToneMapFacts emptyWindow = facts;
        emptyWindow.window.width = 0.0f;
        CHECK(ClassifyToneMap(emptyWindow) == Decline::Window);

        ToneMapFacts tooWide = facts;
        tooWide.window = Window { 0.0f, 0.0f, 4096.0f, 1080.0f };
        CHECK(ClassifyToneMap(tooWide) == Decline::Window);

        ToneMapFacts tinyHalf = facts;
        tinyHalf.window = Window { 0.0f, 0.0f, 100.0f, 60.0f }; // below kMinWindowWidth and kMinWindowHeight
        CHECK(ClassifyToneMap(tinyHalf) == Decline::Window);

        ToneMapFacts oddWindow = facts;
        oddWindow.window = Window { 0.0f, 0.0f, 1921.0f, 1081.0f }; // odd extents: the half is rounded up
        CHECK(ClassifyToneMap(oddWindow) == Decline::None);
        CHECK_EQ(HalfExtent(oddWindow.window.width), 961u);
        CHECK_EQ(HalfExtent(oddWindow.window.height), 541u);
    }

    // ---- the sky's own sun: the screen UV the engine draws the disk at, placed in the window without a camera, and the row that says whether a
    //      read is this build's sun parameter buffer at all
    {
        const Window window = Frame();
        // The session 5 capture's own row: cSunParam[0] = (1.7778, 1.0, 0.6995, 0.1751) with the sun disk on screen at 07:00
        const float captured[4] { 1.7778f, 1.0f, 0.6995f, 0.1751f };
        CHECK(SunParamUvUsable(captured));
        const SunProjection sky = ProjectSunFromUv(captured[2], captured[3], window);
        CHECK(sky.unit && !sky.behind);
        CHECK(Near(sky.screenX, 1343.54f, 0.05f) && Near(sky.screenY, 189.61f, 0.05f));
        CHECK(Near(sky.screenX, window.x + captured[2] * window.width) && Near(sky.screenY, window.y + captured[3] * window.height));
        CHECK(Near(sky.ndcX, captured[2] * 2.0f - 1.0f) && Near(sky.ndcY, 1.0f - captured[3] * 2.0f)); // y runs down the window
        CHECK(Near(sky.offscreen, 0.0f));
        CHECK(SunVisible(sky));

        // the centre of the window is UV (0.5, 0.5), the same place ProjectSun puts a sun straight ahead
        const SunProjection centre = ProjectSunFromUv(0.5f, 0.5f, window);
        CHECK(Near(centre.screenX, 960.5f) && Near(centre.screenY, 540.5f));
        CHECK(Near(centre.ndcX, 0.0f) && Near(centre.ndcY, 0.0f));

        // a sun 0.4 of a window above the top edge: the sky mounted it there and the frame draws it there, so the pass draws the shafts into the
        // frame - where the light's clamped direction put it a quarter of a window out and the frame was skipped
        const SunProjection above = ProjectSunFromUv(0.5f, -0.4f, window);
        CHECK(above.unit && !above.behind);
        CHECK(Near(above.screenY, -431.5f, 0.05f));
        CHECK(Near(above.offscreen, 0.4f, 1.0e-4f));
        CHECK(SunVisible(above));
        CHECK(!SunVisible(above, 0.25f)); // the quarter-window margin refused it

        // 0.6 of a window to the right is further out than the margin: the frame is left alone, and that is not a refusal of the feature
        const SunProjection right = ProjectSunFromUv(1.6f, 0.5f, window);
        CHECK(Near(right.offscreen, 0.6f, 1.0e-4f));
        CHECK(!SunVisible(right));

        // a UV outside 0..1 but inside the margin is a real sun: the disk's centre can sit past the edge while its light still reaches the frame
        const SunProjection corner = ProjectSunFromUv(-0.1f, 0.5f, window);
        CHECK(corner.unit && Near(corner.offscreen, 0.1f));
        CHECK(SunVisible(corner));

        // an unusable window and a NaN UV are not a sun: the caller falls back to the light's own projection
        CHECK(!ProjectSunFromUv(0.5f, 0.5f, Window {}).unit);
        CHECK(!ProjectSunFromUv(std::nanf(""), 0.5f, window).unit);
        CHECK(!ProjectSunFromUv(0.5f, std::nanf(""), window).unit);

        // the row itself: the aspect and the 1.0 are the engine's own shape, the two UVs are a screen position
        float row[4] { 1.7778f, 1.0f, 0.5f, 0.5f };
        CHECK(SunParamUvUsable(row));
        CHECK(!SunParamUvUsable(nullptr));
        row[2] = -1.0f;
        row[3] = 2.0f;
        CHECK(SunParamUvUsable(row)); // the UV range's closed ends
        row[0] = 0.4f; // a portrait display is the narrowest aspect there is
        row[2] = 0.5f;
        row[3] = 0.5f;
        CHECK(!SunParamUvUsable(row));
        row[0] = 6.0f;
        CHECK(!SunParamUvUsable(row));
        row[0] = 1.7778f;
        row[1] = 0.9f; // not the 1.0 the engine pairs the aspect with
        CHECK(!SunParamUvUsable(row));
        row[1] = 1.1f;
        CHECK(!SunParamUvUsable(row));
        row[1] = 1.0f;
        row[2] = 2.5f;
        CHECK(!SunParamUvUsable(row));
        row[2] = 0.5f;
        row[3] = -1.5f;
        CHECK(!SunParamUvUsable(row));
        row[0] = std::nanf("");
        row[3] = 0.5f;
        CHECK(!SunParamUvUsable(row));
        for (float& value : row)
            value = 0.0f; // a buffer that was never written
        CHECK(!SunParamUvUsable(row));

        // and the age rule: the copy is read a frame late, covers the ordinary GPU delay, and a frame older than the hold is not this frame's sun
        CHECK(SunUvFresh(true, 10, 10));
        CHECK(SunUvFresh(true, 10, 10 + kHoldFrames));
        CHECK(!SunUvFresh(true, 10, 11 + kHoldFrames));
        CHECK(!SunUvFresh(true, 20, 10)); // a copy of a later frame is not this frame's
        CHECK(!SunUvFresh(false, 10, 10));
    }

    // ---- the anchors the module runs on: one name row, one bit each in the shared map, its slot field, and its own refusal streak
    {
        CHECK_EQ(eng::kNameCount, 13u); // the light parameter sits before the puddles' four G-buffer readers and the reflections' map, after the occlusion texture, and the sky's sun parameters are last
        CHECK_EQ(eng::IndexOfName("g_LightParam"), 6);
        CHECK_EQ(eng::IndexOfName("cSunParam"), 12);
        CHECK(eng::kNames[6].kind == eng::NameKind::ConstantBuffer);
        CHECK(eng::kNames[12].kind == eng::NameKind::ConstantBuffer);

        // The bits are the map's (EnginePassRules.h): 41 the light pass, 42 the composite, 43 the tone map, 62 the sky's visual sun, and the
        // light parameter's slot at 44..51. The map refuses at compile time any two fields that overlap, so these values are the contract a
        // renumber is checked against.
        CHECK_EQ(eng::kShaftsLightBit, eng::Anchors { 1 } << 41);
        CHECK_EQ(eng::kShaftsCompositeBit, eng::Anchors { 1 } << 42);
        CHECK_EQ(eng::kShaftsToneMapBit, eng::Anchors { 1 } << 43);
        CHECK_EQ(eng::kShaftsSunParamBit, eng::Anchors { 1 } << 62);
        CHECK_EQ(eng::kShaftsSlotShift, 44u);
        CHECK_EQ(eng::kShaftsSlotMask, eng::Anchors { 0xFF } << 44);
        CHECK(eng::AnchorsMapIsSound());

        // every field of the map is listed there, the four bits and the slot included: a knob that keeps a private constant is not covered
        {
            bool listedLight = false, listedComposite = false, listedToneMap = false, listedSunParam = false, listedSlot = false;
            for (const eng::Anchors field : eng::kAnchorsFeatureMasks)
            {
                listedLight = listedLight || field == eng::kShaftsLightBit;
                listedComposite = listedComposite || field == eng::kShaftsCompositeBit;
                listedToneMap = listedToneMap || field == eng::kShaftsToneMapBit;
                listedSunParam = listedSunParam || field == eng::kShaftsSunParamBit;
                listedSlot = listedSlot || field == eng::kShaftsSlotMask;
            }
            CHECK(listedLight && listedComposite && listedToneMap && listedSunParam && listedSlot);
        }

        // the map's reserved low range covers the bits and the slot field this module used to keep privately, so a leftover private constant
        // fails the map's compile-time check instead of colliding with the shipped knobs at run time
        CHECK((eng::kAnchorsReservedLowBits & ((eng::Anchors { 1 } << 29) | (eng::Anchors { 1 } << 28) | (eng::Anchors { 0xFF } << 16))) ==
              ((eng::Anchors { 1 } << 29) | (eng::Anchors { 1 } << 28) | (eng::Anchors { 0xFF } << 16)));
        CHECK_EQ(eng::kShaftsLightBit & eng::kAnchorsReservedLowBits, 0u);

        eng::PassLayout lightDraw;
        lightDraw.ok = true;
        lightDraw.found = 1u << 6;
        lightDraw.slots[6] = 2;
        const eng::Anchors lightAnchors = eng::DrawAnchors(lightDraw);
        CHECK(eng::IsShaftsLightAnchors(lightAnchors));
        CHECK(!eng::IsShaftsToneMapAnchors(lightAnchors) && !eng::IsShaftsCompositeAnchors(lightAnchors));
        CHECK(!eng::IsAmbientAnchors(lightAnchors) && !eng::IsWetnessAnchors(lightAnchors));
        CHECK_EQ(eng::ShaftsSlotOf(lightAnchors), 2u);
        CHECK_EQ(eng::IntentOf(eng::BuildParams(0.7f, 0.5f), lightAnchors).ambient, false); // a light draw is never a patch target of the two knobs
        CHECK_EQ(eng::IntentOf(eng::BuildParams(0.7f, 0.5f), lightAnchors).wetness, false);

        eng::PassLayout toneMap;
        toneMap.ok = true;
        toneMap.found = 1u << 3; // cCommonTexParam alone
        toneMap.slots[3] = 0;
        const eng::Anchors toneMapAnchors = eng::DrawAnchors(toneMap);
        CHECK(eng::IsShaftsToneMapAnchors(toneMapAnchors));
        CHECK(!eng::IsShaftsLightAnchors(toneMapAnchors) && !eng::IsShaftsCompositeAnchors(toneMapAnchors));
        CHECK(!eng::IsAmbientAnchors(toneMapAnchors) && !eng::IsWetnessAnchors(toneMapAnchors));
        CHECK_EQ(eng::DrawAnchors(eng::PassLayout {}), 0u);

        // the three bits and their slot field do not overlap each other or the knobs' own fields
        CHECK_EQ(eng::kShaftsLightBit & eng::kShaftsSlotMask, 0u);
        CHECK_EQ(eng::kShaftsLightBit & eng::kShaftsCompositeBit, 0u);
        CHECK_EQ(eng::kShaftsLightBit & eng::kShaftsToneMapBit, 0u);
        CHECK_EQ(eng::kShaftsCompositeBit & eng::kShaftsToneMapBit, 0u);
        CHECK_EQ(eng::kShaftsCompositeBit & eng::kShaftsSlotMask, 0u);
        CHECK_EQ(eng::kShaftsToneMapBit & eng::kShaftsSlotMask, 0u);
        CHECK_EQ(eng::kShaftsSlotMask & eng::kAmbientSlotMask, 0u);
        CHECK_EQ(eng::kShaftsSlotMask & eng::kWetnessSlotMask, 0u);
        CHECK_EQ(eng::kShaftsSunParamBit & eng::kShaftsSlotMask, 0u); // the sky's bit is a flag, not a slot field
        CHECK_EQ(eng::kShaftsSunParamBit & eng::kShaftsLightBit, 0u);
        CHECK(eng::kShaftsSlotMask > (eng::kAmbientDrawBit | eng::kWetnessDrawBit)); // the slot field sits above the shipped knobs' bits

        // Every one of the three draws this module hangs on carries its own bit in the packed word, which is what a shared shader index keyed on
        // that word needs to keep the shader: a recognition that only filled a slot field would be dropped at shader creation. A tone-map-shaped
        // layout is the case to check: cCommonTexParam alone, no slot of its own, and its word must still be non-zero.
        {
            eng::PassLayout compositeLayout;
            compositeLayout.ok = true;
            compositeLayout.found = (1u << 0) | (1u << 1); // g_AmbientParamArray and g_SamplerLightDiffuse
            compositeLayout.slots[0] = 0;
            compositeLayout.slots[1] = 1;
            const eng::Anchors compositeAnchors = eng::DrawAnchors(compositeLayout);
            CHECK(compositeAnchors != 0u);
            CHECK(eng::IsShaftsCompositeAnchors(compositeAnchors));
            CHECK(eng::IsAmbientAnchors(compositeAnchors)); // the same draw is still the ambient knob's, on its own bit

            eng::PassLayout lightLayout;
            lightLayout.ok = true;
            lightLayout.found = 1u << 6;
            lightLayout.slots[6] = 2;
            const eng::Anchors lightDrawAnchors = eng::DrawAnchors(lightLayout);
            CHECK(lightDrawAnchors != 0u && (lightDrawAnchors & eng::kShaftsLightBit) != 0u);

            eng::PassLayout toneMapLayout;
            toneMapLayout.ok = true;
            toneMapLayout.found = 1u << 3;
            toneMapLayout.slots[3] = 1;
            const eng::Anchors toneMapAnchors = eng::DrawAnchors(toneMapLayout);
            CHECK(toneMapAnchors != 0u && (toneMapAnchors & eng::kShaftsToneMapBit) != 0u);
            CHECK_EQ(toneMapAnchors, eng::kShaftsToneMapBit); // nothing else is set for a shader that declares cCommonTexParam alone

            // and none of the three guesses another's bit: the bit is the recognition, not a slot that happens to be filled
            CHECK(!eng::IsShaftsLightAnchors(compositeAnchors) && !eng::IsShaftsToneMapAnchors(compositeAnchors));
            CHECK(!eng::IsShaftsCompositeAnchors(lightDrawAnchors) && !eng::IsShaftsToneMapAnchors(lightDrawAnchors));
            CHECK(!eng::IsShaftsCompositeAnchors(toneMapAnchors) && !eng::IsShaftsLightAnchors(toneMapAnchors));

            // the sky's own sun draw: its own bit, its buffer slot in the light pass's field, and no light pass's bit beside it
            eng::PassLayout sunParamLayout;
            sunParamLayout.found = 1u << 12;
            sunParamLayout.slots[12] = 1;
            const eng::Anchors sunParamAnchors = eng::DrawAnchors(sunParamLayout);
            CHECK(eng::IsShaftsSunParamAnchors(sunParamAnchors));
            CHECK(!eng::IsShaftsLightAnchors(sunParamAnchors) && !eng::IsShaftsCompositeAnchors(sunParamAnchors) &&
                  !eng::IsShaftsToneMapAnchors(sunParamAnchors));
            CHECK_EQ(eng::ShaftsSlotOf(sunParamAnchors), 1u);
            CHECK_EQ(sunParamAnchors, eng::kShaftsSunParamBit | (eng::Anchors { 1 } << eng::kShaftsSlotShift));
        }

        // a light draw is only a light draw at a slot D3D11 exposes
        eng::PassLayout pastTheLastSlot;
        pastTheLastSlot.ok = true;
        pastTheLastSlot.found = 1u << 6;
        pastTheLastSlot.slots[6] = eng::kConstantBufferSlots;
        CHECK_EQ(eng::DrawAnchors(pastTheLastSlot), 0u);
        eng::PassLayout toneMapPastTheLastSlot;
        toneMapPastTheLastSlot.ok = true;
        toneMapPastTheLastSlot.found = 1u << 3;
        toneMapPastTheLastSlot.slots[3] = eng::kConstantBufferSlots;
        CHECK_EQ(eng::DrawAnchors(toneMapPastTheLastSlot), 0u);

        // the module's own streak: a refusal of the shafts never stands the two knobs down, and theirs never stands the shafts down
        eng::Session session;
        for (uint32_t i = 0; i < eng::kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline(eng::Knob::LightShafts));
        CHECK(!session.off);
        CHECK_EQ(session.ambientDeclines, 0u);
        CHECK_EQ(session.wetnessDeclines, 0u);
        session.NoteRun(eng::Knob::LightShafts);
        for (uint32_t i = 0; i < eng::kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline(eng::Knob::LightShafts));
        CHECK(session.NoteDecline(eng::Knob::LightShafts)); // the sixty-fourth in a row switches the module off
        CHECK(session.off);

        // The engine module's streak covers its two knobs only: sixty-four ambient refusals stand that session down while the shafts keep their
        // own, and the draw path keeps handing the shafts their draws. This is the combination a player creates by leaving both experimental
        // cards on (the engine card with one knob off its vanilla value, the shafts card at its default).
        {
            eng::Session knobs;
            eng::Session shafts;
            bool switchedOff = false;
            for (uint32_t i = 0; i < eng::kConsecutiveDeclineLimit; ++i)
                switchedOff = knobs.NoteDecline(eng::Knob::Ambient) || switchedOff;
            CHECK(switchedOff);
            CHECK(!knobs.Armed());
            CHECK(shafts.Armed()); // the shafts' session never saw those refusals
            CHECK(eng::DrawPathWanted(knobs.Armed(), /*aoArmed*/ false, /*glossArmed*/ false, shafts.Armed()));
            CHECK(!eng::DrawPathWanted(false, false, false, false));
            CHECK(eng::DrawPathWanted(true, false, false, false));
            CHECK(eng::DrawPathWanted(false, true, false, false)); // the occlusion pass alone keeps the seam entered too
            CHECK(eng::DrawPathWanted(true, false, false, true));

            // and a draw the engine's session no longer wants is not patched: the two knobs' intent is empty, whatever the anchors say
            const eng::Anchors composite = eng::kAmbientDrawBit | eng::kShaftsCompositeBit | eng::Anchors { 1 }; // the slot byte is the low 8 bits
            const eng::Params offVanilla = eng::BuildParams(0.7f, 0.5f);
            CHECK(eng::IntentOf(offVanilla, composite).ambient);
            CHECK(!eng::IntentOfSession(offVanilla, composite, knobs.Armed(), /*masterOn*/ true).ambient);
            CHECK(eng::IntentOfSession(offVanilla, composite, /*sessionArmed*/ true, /*masterOn*/ true).ambient);
            CHECK(!eng::IntentOfSession(offVanilla, composite, /*sessionArmed*/ true, /*masterOn*/ false).ambient);
        }

        // The scene HDR key is sampled where the module decides, and the line is the sample's own answer: switching HDR on while the shafts key
        // does not move writes one line, a second sample writes nothing, and switching it off and on again gets its own line.
        {
            SceneHdrWatch watch;
            CHECK(!watch.Sample(false, false) && !watch.inactive);
            CHECK(!watch.Sample(true, false) && !watch.inactive);
            CHECK(watch.Sample(true, true) && watch.inactive); // the transition: one line
            CHECK(!watch.Sample(true, true) && watch.inactive); // a frame later: nothing new to say
            CHECK(!watch.Sample(false, true) && !watch.inactive); // the key is off: the HDR path is not this module's business
            CHECK(!watch.Sample(true, false) && !watch.inactive); // HDR off: the pass runs again, and no line is owed
            CHECK(watch.Sample(true, true) && watch.inactive); // a second transition gets its own line
            CHECK_EQ(SceneHdrInactive(false, true), false);
            CHECK_EQ(SceneHdrInactive(true, true), true);
            CHECK_EQ(SceneHdrInactive(true, false), false);
        }
    }

    // ---- the lines a session reads: one per change of reason, one for a frame without a sun in view, and the cost line
    {
        CHECK(FormatDecline(Decline::NoSun).find("Light shafts") == 0);
        CHECK(FormatDecline(Decline::NoSun).find(DeclineName(Decline::NoSun)) != std::string::npos);
        CHECK(FormatDecline(Decline::None).find(DeclineName(Decline::None)) != std::string::npos);
        CHECK(FormatSkip(Decline::NoSun).find("frame was left as it was") != std::string::npos);
        CHECK(FormatSkip(Decline::NoDepth).find(DeclineName(Decline::NoDepth)) != std::string::npos);
        CHECK(FormatSkip(Decline::None).find("run") == std::string::npos);
        CHECK(FormatSkip(Decline::None).find("the line above says why") != std::string::npos); // the skip that logged its own reason first

        // the projection's own facts, once a session, and the lamp the read-back found
        float lineProjection[16] {};
        Perspective(lineProjection, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        const std::string projectionLine = FormatProjection(lineProjection);
        CHECK(projectionLine.find("Light shafts") == 0);
        CHECK(projectionLine.find("-1.0000") != std::string::npos);
        CHECK(FormatProjection(nullptr).find("not read") != std::string::npos);
        // the view-projection the sun's direction is projected through, all four rows at four decimals, so a session tells a wrong direction from
        // a wrong matrix without a capture
        float lineViewProjection[16] {};
        for (int i = 0; i < 16; ++i)
            lineViewProjection[i] = float(i) * 0.5f;
        const std::string viewProjectionLine = FormatViewProjection(lineViewProjection);
        CHECK(viewProjectionLine.find("Light shafts: the camera's view-projection is (") == 0);
        CHECK(viewProjectionLine.find("(0.0000, 0.5000, 1.0000, 1.5000)") != std::string::npos); // the first row
        CHECK(viewProjectionLine.find("(6.0000, 6.5000, 7.0000, 7.5000)") != std::string::npos);  // the last row
        CHECK(FormatViewProjection(nullptr).find("not read") != std::string::npos);
        LightFacts lamp;
        lamp.position[0] = 43.2f;
        CHECK(FormatNotTheSun(lamp).find("lamp") != std::string::npos);
        CHECK(std::string(kReadbackStale).find("Light shafts") == 0);
        CHECK(std::string(kCameraPending).find("Light shafts: the camera") == 0);
        CHECK(std::string(kReadbackNotLanding).find("Light shafts") == 0 && std::string(kReadbackNotLanding).find("never landed") != std::string::npos);
        CHECK(kReadbackStallFrames >= 16); // long enough that a GPU a frame or two behind never reaches it
        CHECK(std::string(kSceneHdrInactive).find("scene HDR") != std::string::npos);
        CHECK(FormatSwitchedOff(64).find("64") != std::string::npos);
        for (uint32_t i = 0; i <= uint32_t(Decline::Exception); ++i)
            CHECK(std::string(DeclineName(Decline(i))) != "unknown");
        CHECK_EQ(kDeclineReasonCount, uint32_t(Decline::Exception) + 1);

        float viewProjection[16] {};
        Perspective(viewProjection, 60.0f, 16.0f / 9.0f, 0.1f, 10000.0f);
        const float ahead[3] { 0.0f, 0.0f, -1.0f };
        const SunProjection sun = ProjectSun(viewProjection, ahead, Frame());
        const Params params = BuildParams(kDefaultIntensity, kDefaultDecay, kDefaultSamples, kDefaultDensity);
        const std::string run =
            FormatRun(sun, Frame(), params, 3840, 2160, ahead, 0.5f, reinterpret_cast<const void*>(uintptr_t(0x1e6b8a5d6e0ull)), 0.4f);
        CHECK(run.find("3840x2160") != std::string::npos);
        CHECK(run.find("48 samples") != std::string::npos);
        CHECK(run.find("Light shafts:") == 0);
        // The direction the light reported, the buffer it came from and the held sun's fade: they are what settles a light that is not the sun (the
        // position follows from the direction and the view-projection the log prints, and the address ties the line to a capture's own row).
        CHECK(run.find("direction (0.0000, 0.0000, -1.0000)") != std::string::npos);
        CHECK(run.find("of the light at 1e6b8a5d6e0") != std::string::npos);
        CHECK(run.find("fade 0.50") != std::string::npos);
        // The frame's own shadow said the light was the sun by a tenth of a degree: the line a session reads says which light placed the shafts.
        CHECK(run.find("shadow 0.4 deg") != std::string::npos);
        const std::string runWithoutShadow = FormatRun(sun, Frame(), params, 3840, 2160, ahead, 1.0f, nullptr, -1.0f);
        CHECK(runWithoutShadow.find("shadow no row") != std::string::npos);
        CHECK(run.find("windows outside") != std::string::npos);
        CHECK_EQ(kCountsLogFrames, 3600u); // the card's numbers reach the log about once a minute

        const float behind[3] { 0.0f, 0.0f, 1.0f };
        const std::string skipped = FormatSunSkipped(ProjectSun(viewProjection, behind, Frame()), Frame());
        CHECK(skipped.find("behind the camera") != std::string::npos);
        const float far[3] { 0.9999f, 0.0f, -0.01f };
        CHECK(FormatSunSkipped(ProjectSun(viewProjection, far, Frame()), Frame()).find("outside") != std::string::npos);

        // ---- the card's one line: each of the four reasons a frame had no shafts in it prints on its own, so a session reads which one it hit
        //      instead of one number for three, and the sun and the window are the last ones the module measured.
        {
            CardNumbers numbers;
            numbers.intensity = 0.5f;
            numbers.runs = 1234;
            numbers.declined = 7;
            numbers.skipped = 40;
            numbers.noDepth = 12;
            numbers.noSun = 20;
            numbers.offScreen = 5;
            numbers.belowHorizon = 3;
            numbers.reads = 1200;
            numbers.notReady = 3;
            numbers.sunX = 1720.0f;
            numbers.sunY = 640.0f;
            numbers.windowWidth = 1920;
            numbers.windowHeight = 1080;
            const std::string card = FormatCard(numbers);
            CHECK_EQ(numbers.noDepth + numbers.noSun + numbers.offScreen + numbers.belowHorizon + numbers.notTheSun, numbers.skipped); // the five are the whole of it
            CHECK(card.find("on at 0.50") == 0);
            CHECK(card.find("1234 with the shafts") != std::string::npos);
            CHECK(card.find("7 left alone") != std::string::npos);
            CHECK(card.find("40 with no sun to draw") != std::string::npos);
            CHECK(card.find("no sun among the frame's light draws: 12") != std::string::npos); // no draw of the frame had the sun's shape
            CHECK(card.find("the light's bytes not arrived: 20") != std::string::npos);
            CHECK(card.find("the sun out of view: 5") != std::string::npos);
            CHECK(card.find("the light below the horizon: 3") != std::string::npos);
            CHECK(card.find("1200 reads, 3 not ready") != std::string::npos);
            CHECK(card.find("the last sun at 1720,640 of a 1920x1080 window") != std::string::npos);
            // one reason at a time: each moves only its own number, so a card that swapped two of them fails here
            const auto cardWith = [](uint32_t CardNumbers::*field) {
                CardNumbers one;
                one.skipped = 1;
                one.*field = 1;
                return FormatCard(one);
            };
            const std::string onlyNoDepth = cardWith(&CardNumbers::noDepth);
            CHECK(onlyNoDepth.find("no sun among the frame's light draws: 1,") != std::string::npos);
            CHECK(onlyNoDepth.find("the light's bytes not arrived: 0,") != std::string::npos);
            CHECK(onlyNoDepth.find("the sun out of view: 0,") != std::string::npos);
            CHECK(onlyNoDepth.find("the light below the horizon: 0,") != std::string::npos);
            CHECK(onlyNoDepth.find("the light not the frame's sun: 0)") != std::string::npos);
            const std::string onlyNoSun = cardWith(&CardNumbers::noSun);
            CHECK(onlyNoSun.find("no sun among the frame's light draws: 0,") != std::string::npos);
            CHECK(onlyNoSun.find("the light's bytes not arrived: 1,") != std::string::npos);
            const std::string onlyOffScreen = cardWith(&CardNumbers::offScreen);
            CHECK(onlyOffScreen.find("the sun out of view: 1,") != std::string::npos);
            const std::string onlyBelowHorizon = cardWith(&CardNumbers::belowHorizon);
            CHECK(onlyBelowHorizon.find("the light below the horizon: 1,") != std::string::npos);
            const std::string onlyNotTheSun = cardWith(&CardNumbers::notTheSun);
            CHECK(onlyNotTheSun.find("the light not the frame's sun: 1)") != std::string::npos);
            // a session that measured nothing yet: the card says so instead of inventing a window
            const std::string empty = FormatCard(CardNumbers {});
            CHECK(empty.find("0 with no sun to draw") != std::string::npos);
            CHECK(empty.find("the last sun at 0,0 of a 0x0 window") != std::string::npos);
            // The frames the share is over, and the held ones: the counts line is written whatever a session's frames came to, so the share is
            // the module's own frame count and the held runs are a part of the drawn ones.
            CHECK(empty.find("0 rendered frames") != std::string::npos);
            CHECK(empty.find("(0 of them drawn with a held sun)") != std::string::npos);
            CardNumbers drawn;
            drawn.frames = 13136;
            drawn.runs = 291;
            drawn.held = 12;
            const std::string share = FormatCard(drawn);
            CHECK(share.find("13136 rendered frames, 291 with the shafts (12 of them drawn with a held sun)") != std::string::npos);
        }

        // ---- the four reasons are the whole of `skipped`, counted through the one function the module counts a frame with: the session's own
        //      numbers (33 frames left alone without the shafts) add up under the count they are printed under, and a below-horizon refusal is
        //      one skip and one reason - not also a frame without a light draw, the double count that made the card's four numbers add up to more
        //      than the number they sat under.
        {
            const auto counted = [](uint32_t noDepth, uint32_t noSun, uint32_t offScreen, uint32_t belowHorizon, uint32_t notTheSun) {
                CardCounts counts;
                for (uint32_t i = 0; i < noDepth; ++i)
                    CountSkip(counts, SkipReason::NoDepth);
                for (uint32_t i = 0; i < noSun; ++i)
                    CountSkip(counts, SkipReason::NoSun);
                for (uint32_t i = 0; i < offScreen; ++i)
                    CountSkip(counts, SkipReason::OffScreen);
                for (uint32_t i = 0; i < belowHorizon; ++i)
                    CountSkip(counts, SkipReason::BelowHorizon);
                for (uint32_t i = 0; i < notTheSun; ++i)
                    CountSkip(counts, SkipReason::NotTheSun);
                return counts;
            };
            const CardCounts counts = counted(3, 17, 11, 2, 4);
            CHECK_EQ(counts.skipped, 37u);
            CHECK_EQ(counts.noDepth + counts.noSun + counts.offScreen + counts.belowHorizon + counts.notTheSun, counts.skipped);
            CHECK_EQ(counts.noDepth, 3u);
            CHECK_EQ(counts.noSun, 17u);
            CHECK_EQ(counts.offScreen, 11u);
            CHECK_EQ(counts.belowHorizon, 2u);
            CHECK_EQ(counts.notTheSun, 4u);
            // one skip is one reason: a fresh count of any one reason moves `skipped` and its own number, and nothing else
            const SkipReason reasons[] { SkipReason::NoDepth, SkipReason::NoSun, SkipReason::OffScreen, SkipReason::BelowHorizon,
                                         SkipReason::NotTheSun };
            for (SkipReason reason : reasons)
            {
                CardCounts one;
                CountSkip(one, reason);
                CHECK_EQ(one.skipped, 1u);
                CHECK_EQ(one.noDepth + one.noSun + one.offScreen + one.belowHorizon + one.notTheSun, 1u);
            }
            // and the card prints exactly those numbers under their count
            CardNumbers numbers;
            numbers.skipped = counts.skipped;
            numbers.noDepth = counts.noDepth;
            numbers.noSun = counts.noSun;
            numbers.offScreen = counts.offScreen;
            numbers.belowHorizon = counts.belowHorizon;
            numbers.notTheSun = counts.notTheSun;
            const std::string card = FormatCard(numbers);
            CHECK(card.find("37 with no sun to draw (no sun among the frame's light draws: 3, the light's bytes not arrived: 17, the sun out of view: 11, "
                            "the light below the horizon: 2, the light not the frame's sun: 4)") != std::string::npos);
        }

        const std::string measured = FormatCost(true, 0.42, 300, 290, 10);
        CHECK(measured.find("0.4200 ms") != std::string::npos);
        CHECK(measured.find("300 frames") != std::string::npos);
        CHECK(FormatCost(false, 0.0, 1, 1, 0).find("not measured yet") != std::string::npos);
    }

    // ---- the engine's own formats: the scene is FP16 and the depth is the two views the light draws bind
    {
        CHECK(IsSceneFormat(10));
        CHECK(!IsSceneFormat(11)); // the typeless twin is the resource's format, never the view's
        CHECK(!IsSceneFormat(87));
        CHECK(IsDepthFormat(44)); // R24G8_TYPELESS, the buffer's own format
        CHECK(IsDepthFormat(46)); // R24_UNORM_X8_TYPELESS, the view a captured frame binds at the light draws
        CHECK(IsDepthFormat(41)); // R32_FLOAT
        CHECK(!IsDepthFormat(45)); // D24_UNORM_S8_UINT: a depth-stencil view's format, never a shader resource's
        CHECK(!IsDepthFormat(10));
        CHECK(!IsDepthFormat(0));
    }
    return Finish("shafts_rules");
}
