// Recognition and frame-step rules on the host, built from rows copied out of four live captures of the game.
//
// What a wrong rule would break, and what is checked here:
//   R1  depth of field's camera band: the real inverse projection has an infinite far plane (reversed-Z: the pair is (0, 0, 10, 0) in row 3),
//       the band must accept it with near = 0.1 m and answer 10 m at depth 0.01 and 0.1 m at depth 1.0; a finite far plane, and every
//       negative or degenerate row, keep their old answers; the light bounce's own camera checks keep accepting the four jittered rows.
//   R2  one frame step per rendered frame: the game draws the composite twice a frame (adjacent draws, identical bindings), the counter must
//       move once for that pair, a frame that declines still closes, a replayed tone map never closes the frame.
//   R3  god rays' fog height profile: chosen by the slot the fog shader names it at, never by the shape of its 16 floats; the declared
//       48 bytes decide whether a copy is a profile; the clip-to-world matrix beside it never passes.
//   R4  the pixel-to-NDC mapping both stages need, derived from the render window: it reproduces the sun pixel the game itself reported.
//
// The camera and fog rows are the constant-buffer bytes of c1 (day exterior, sun in view), c2 (night exterior), c3 (night exterior, the
// extra press) and c4 (interior hall). The clip-to-world matrix the fog shader multiplies by was not in the capture: the stand-in is the
// camera buffer's inverse view-projection (+0x0A0), which is what the shader's own arithmetic makes it (inferred, not read).
#include "check.h"
#include "live_fog_fixtures.h"
#include "recog_fixtures.h"

#include "ext/ffxiv/engine/DofRules.h"
#include "ext/ffxiv/engine/GiRules.h"
#include "ext/ffxiv/engine/GodraysRules.h"
#include "ext/ffxiv/engine/RecogRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

using namespace recog_fixture;
namespace shafts = ext::ffxiv::shafts;
namespace dof = ext::ffxiv::dof;
namespace godrays = ext::ffxiv::godrays;
namespace gi = ext::ffxiv::engine::gi;

namespace
{
struct Camera
{
    float view[12] {};   // +0x000, the three rows the elevation and world tests read
    float c2w[16] {};    // +0x0A0, the stand-in for cC2W
    float inverse[16] {}; // +0x0E0
    float projection[16] {}; // +0x120
};

Camera CameraOf(const uint32_t (&words)[88])
{
    Camera camera;
    for (uint32_t i = 0; i < 12; ++i)
        camera.view[i] = F(words[i]);
    for (uint32_t i = 0; i < 16; ++i)
    {
        camera.c2w[i] = F(words[40 + i]);
        camera.inverse[i] = F(words[56 + i]);
        camera.projection[i] = F(words[72 + i]);
    }
    return camera;
}

// The twelve bytes' worth of floats of a row as the unsigned bytes the ring hands the decoder.
template <size_t N> void BytesOf(const uint32_t (&words)[N], unsigned char* out) { std::memcpy(out, words, N * 4); }

struct Capture
{
    const char* name;
    const uint32_t (&camera)[88];
    const uint32_t (&fog)[32];
    const uint32_t (&profile)[16];
    float density1;
};

const Capture kCaptures[] {
    { "c1", kC1Camera, kC1FogParam, kC1ExpHeightFog, 0.0017325f },
    { "c2", kC2Camera, kC2FogParam, kC2ExpHeightFog, 0.0017325f },
    { "c3", kC3Camera, kC3FogParam, kC3ExpHeightFog, 0.0017325f },
    { "c4", kC4Camera, kC4FogParam, kC4ExpHeightFog, 0.003465f },
};

// The synthetic finite-far inverse projection (near 0.5 m, far 500 m): a = near/(far-near), b = near*far/(far-near), c = 1, so
// near = b/(a+c) = 0.5 and far = b/a = 500. It keeps the finite branch honest: with a = 0 the near plane is b/c, with a != 0 it is not.
void FiniteInverse(float out[16], float near_, float far_)
{
    std::memset(out, 0, 16 * sizeof(float));
    const float a = near_ / (far_ - near_);
    const float b = near_ * far_ / (far_ - near_);
    out[0] = b;
    out[5] = b;
    out[11] = -b;
    out[14] = 1.0f;
    out[15] = a;
}

// What the tree did before this change picked as the height profile: the one of two 64-byte candidates that is not a rigid transform.
// Kept here to show that on the real bytes it picks nothing (neither candidate is rigid), which is why the profile is chosen by slot.
bool Rigid(const float bytes[16])
{
    for (uint32_t i = 0; i < 16; ++i)
        if (!(bytes[i] == bytes[i]))
            return false;
    if (!(bytes[12] > -1.0e-3f && bytes[12] < 1.0e-3f) || !(bytes[13] > -1.0e-3f && bytes[13] < 1.0e-3f) ||
        !(bytes[14] > -1.0e-3f && bytes[14] < 1.0e-3f) || !(bytes[15] > 1.0f - 1.0e-3f && bytes[15] < 1.0f + 1.0e-3f))
        return false;
    for (uint32_t row = 0; row < 3; ++row)
    {
        const float* a = bytes + row * 4;
        const float aSquared = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(aSquared > 0.98f * 0.98f) || !(aSquared < 1.02f * 1.02f))
            return false;
        for (uint32_t other = row + 1; other < 3; ++other)
        {
            const float* b = bytes + other * 4;
            const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (!(dot > -1.0e-3f && dot < 1.0e-3f))
                return false;
        }
    }
    return true;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

shafts::ToneMapFacts GoodToneMap()
{
    shafts::ToneMapFacts facts;
    facts.renderTargets = 1;
    facts.targetWidth = facts.sceneWidth = 3840;
    facts.targetHeight = facts.sceneHeight = 2160;
    facts.targetFormat = facts.sceneFormat = 10; // R16G16B16A16_FLOAT
    facts.targetIsScene = false;
    facts.sceneSampled = true;
    facts.sceneIsScene = true;
    facts.window = { 0.5f, 0.5f, 1920.0f, 1080.0f };
    return facts;
}
} // namespace

int main()
{
    // ==== R1: depth of field's camera band ==============================================================================================
    {
        for (const Capture& capture : kCaptures)
        {
            const Camera camera = CameraOf(capture.camera);
            // The real pair: row 3 of the inverse is (0, 0, 10, 0), so a = inverse[15] is zero. The band accepts it: near 0.1 m, no far plane.
            CHECK(camera.inverse[15] == 0.0f);
            CHECK_EQ(camera.inverse[14], 10.0f);
            CHECK_EQ(camera.inverse[11], -1.0f);
            const dof::DepthBand band = dof::CameraDepthBand(camera.inverse, camera.projection);
            if (!band.usable)
                std::printf("    %s: the real inverse projection is refused by the band\n", capture.name);
            CHECK(band.usable);
            CHECK(Near(band.nearPlane, 0.1f, 1.0e-4f));
            CHECK(std::isinf(band.farPlane) && band.farPlane > 0.0f);
            // Depth 0.01 is 10 m away, depth 1.0 is the near plane (0.1 m): the identity the fog post's own arithmetic computes.
            CHECK(Near(dof::ViewDistanceAtDepth(camera.inverse, 0.0f, 0.0f, 0.01f), 10.0f, 1.0e-3f));
            CHECK(Near(dof::ViewDistanceAtDepth(camera.inverse, 0.3f, -0.2f, 1.0f), 0.1f, 1.0e-4f));
            // The light bounce's own camera checks keep accepting the jittered projection and the view rows of the same buffer.
            CHECK(shafts::ProjectionOk(camera.projection));
            CHECK(gi::ViewRowsOk(camera.view));
        }
    }
    {
        // A finite far plane keeps its old answer, including a near plane that is not b/c.
        float inverse[16], projection[16];
        FiniteInverse(inverse, 0.5f, 500.0f);
        std::memset(projection, 0, sizeof projection);
        projection[0] = 1.0f;
        projection[5] = 1.0f;
        projection[14] = -1.0f;
        const dof::DepthBand band = dof::CameraDepthBand(inverse, projection);
        CHECK(band.usable);
        CHECK(Near(band.nearPlane, 0.5f, 1.0e-5f));
        CHECK(Near(band.farPlane, 500.0f, 0.05f));
        CHECK(std::isfinite(band.farPlane));

        const Camera real = CameraOf(kC1Camera);
        // a < 0, b <= 0, c <= 0 and a NaN in any of the three are refused, whatever the rest says.
        float row[16];
        std::memcpy(row, real.inverse, sizeof row);
        row[15] = -0.01f;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        std::memcpy(row, real.inverse, sizeof row);
        row[11] = 0.0f; // b = 0
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        row[11] = 1.0f; // b < 0
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        std::memcpy(row, real.inverse, sizeof row);
        row[14] = 0.0f; // c = 0
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        row[14] = -10.0f; // c < 0
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(row, real.inverse, sizeof row);
        row[15] = nan;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        std::memcpy(row, real.inverse, sizeof row);
        row[11] = nan;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        std::memcpy(row, real.inverse, sizeof row);
        row[14] = nan;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        // Negative zero is zero: the infinite branch, not a refusal; a vanishing positive a is the same far plane.
        std::memcpy(row, real.inverse, sizeof row);
        row[15] = -0.0f;
        CHECK(dof::CameraDepthBand(row, real.projection).usable);
        row[15] = 1.0e-12f;
        const dof::DepthBand tiny = dof::CameraDepthBand(row, real.projection);
        CHECK(tiny.usable && std::isinf(tiny.farPlane));
        // The infinite branch still keeps the near test: a near plane of 1e-6 m (c = 1e6) is another buffer's numbers.
        std::memcpy(row, real.inverse, sizeof row);
        row[14] = 1.0e6f;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
        // And a sideways push in the z row is still another layout.
        std::memcpy(row, real.inverse, sizeof row);
        row[9] = 0.25f;
        CHECK(!dof::CameraDepthBand(row, real.projection).usable);
    }

    // ==== R2: one frame step per rendered frame ========================================================================================
    {
        // A captured frame: two adjacent composite draws (identical bindings), the fog post, the tone map.
        shafts::FrameStep step;
        uint64_t frames = 0;
        CHECK(step.Closed()); // the first composite opens a count
        for (uint32_t frame = 1; frame <= 5; ++frame)
        {
            const bool first = step.OpenAtComposite(frames);
            const bool second = step.OpenAtComposite(frames);
            CHECK(first);
            CHECK(!second);
            CHECK_EQ(frames, uint64_t(frame));
            CHECK(!step.Closed());
            step.CloseAtToneMap();
            CHECK(step.Closed());
        }
        CHECK(step.HaveSample());
        CHECK_EQ(step.AdvancesPerFrame(), 1u);
        CHECK_EQ(step.CompositesPerFrame(), 2u);
        // A second tone-shaped draw of the frame (the engine's post-tone copy) changes nothing.
        step.CloseAtToneMap();
        CHECK_EQ(step.AdvancesPerFrame(), 1u);
        CHECK_EQ(step.CompositesPerFrame(), 2u);

        // Three composites with no tone map in between: one advance, and the count moves again only after a close.
        shafts::FrameStep open;
        uint64_t count = 7;
        CHECK(open.OpenAtComposite(count));
        CHECK(!open.OpenAtComposite(count));
        CHECK(!open.OpenAtComposite(count));
        CHECK_EQ(count, uint64_t(8));
        open.CloseAtToneMap();
        CHECK(open.OpenAtComposite(count));
        CHECK_EQ(count, uint64_t(9));

        // Reset puts the step back to "the next composite opens a count".
        open.Reset();
        CHECK(open.Closed());
        CHECK(open.OpenAtComposite(count));
        CHECK_EQ(count, uint64_t(10));
    }
    {
        // What closes a frame. A tone map that classifies, with or without a decline of the module's own, closes it; another draw of the
        // same shader family (it writes the scene back) does not; a replay never does; a chained call closes on any draw that is not a replay.
        shafts::ToneMapFacts good = GoodToneMap();
        CHECK(shafts::ClassifyToneMap(good) == shafts::Decline::None);
        CHECK(shafts::ClosesFrame(good, false));
        CHECK(shafts::ClosesFrame(good, true));

        shafts::ToneMapFacts declined = good; // the window does not fit: the module declines, the frame is still over
        declined.window = { 0.5f, 0.5f, 0.0f, 0.0f };
        CHECK(shafts::ClassifyToneMap(declined) == shafts::Decline::Window);
        CHECK(shafts::ClosesFrame(declined, false));

        shafts::ToneMapFacts postTone = good; // writes the scene back: not the tone map
        postTone.targetIsScene = true;
        CHECK(shafts::ClassifyToneMap(postTone) == shafts::Decline::NotToneMap);
        CHECK(!shafts::ClosesFrame(postTone, false));
        shafts::ToneMapFacts other = good; // reads another texture
        other.sceneIsScene = false;
        CHECK(!shafts::ClosesFrame(other, false));

        shafts::ToneMapFacts replay = good;
        replay.replay = true;
        CHECK(!shafts::ClosesFrame(replay, false));
        CHECK(!shafts::ClosesFrame(replay, true));

        // The chain's caller measured the facts against its own scene record and has classified the draw already.
        shafts::ToneMapFacts callers {};
        CHECK(shafts::ClosesFrame(callers, true));

        // A declining frame still moves the count on: close on the declined draw, open at the next composite.
        shafts::FrameStep step;
        uint64_t frames = 0;
        for (uint32_t frame = 1; frame <= 3; ++frame)
        {
            step.OpenAtComposite(frames);
            step.OpenAtComposite(frames);
            if (shafts::ClosesFrame(declined, false))
                step.CloseAtToneMap();
        }
        CHECK_EQ(frames, uint64_t(3));
        // A replayed tone map in the middle does not.
        step.OpenAtComposite(frames);
        if (shafts::ClosesFrame(replay, false))
            step.CloseAtToneMap();
        step.OpenAtComposite(frames);
        CHECK_EQ(frames, uint64_t(4));
    }
    {
        // The ring ages the log names: the count at the tone map minus the count a copy was written at.
        CHECK_EQ(shafts::AgeOf(10, 9), 1u);
        CHECK_EQ(shafts::AgeOf(10, 10), 0u);
        CHECK_EQ(shafts::AgeOf(10, 7), 3u);
        CHECK_EQ(shafts::AgeOf(5, 9), shafts::kRingNotLanded); // a copy from the future is nothing
        const shafts::RingAge ages[] { { "camera copy", 1 }, { "mapping row", shafts::kRingNotLanded } };
        const std::string line = shafts::FormatFrameStep("Depth of field", 1, 2, ages, 2);
        CHECK(line.find("Depth of field") == 0);
        CHECK(line.find("stepped 1 per tone map") != std::string::npos);
        CHECK(line.find("2 composite draws") != std::string::npos);
        CHECK(line.find("camera copy age 1") != std::string::npos);
        CHECK(line.find("mapping row not landed") != std::string::npos);
        const std::string doubled = shafts::FormatFrameStep("God rays", 2, 2, ages, 1);
        CHECK(doubled.find("stepped 2 per tone map") != std::string::npos);
        CHECK(shafts::FormatRingAge(ages[0]) != shafts::FormatRingAge(ages[1]));

        // The line is written once a state: the same key twice is one line, a changed step or age is another, and the session's cap holds.
        shafts::ChangeLine line1;
        const uint32_t key = shafts::StepKey(1, 2, ages, 2);
        CHECK(line1.Take(key));
        CHECK(!line1.Take(key));
        CHECK(shafts::StepKey(2, 2, ages, 2) != key);
        CHECK(shafts::StepKey(1, 2, ages, 1) != key);
        const shafts::RingAge older[] { { "camera copy", 2 }, { "mapping row", shafts::kRingNotLanded } };
        CHECK(shafts::StepKey(1, 2, older, 2) != key);
        CHECK(line1.Take(shafts::StepKey(2, 2, ages, 2)));
        shafts::ChangeLine capped;
        uint32_t taken = 0;
        for (uint32_t i = 0; i < 40; ++i)
            taken += capped.Take(i) ? 1u : 0u;
        CHECK_EQ(taken, shafts::ChangeLine::kMax);
    }

    // ==== R3: the fog height profile ====================================================================================================
    {
        // The slot stamp: a shader's cExpHeightFogParam slot, stored plus one so that 0 means "nothing stamped".
        for (uint32_t slot = 0; slot < ext::ffxiv::engine::kConstantBufferSlots; ++slot)
        {
            const uint64_t packed = godrays::PackProfileSlot(slot);
            CHECK(packed != 0);
            CHECK_EQ(godrays::UnpackProfileSlot(packed), slot);
        }
        CHECK_EQ(godrays::PackProfileSlot(ext::ffxiv::engine::kConstantBufferSlots), uint64_t(0)); // not a slot D3D11 has
        CHECK_EQ(godrays::PackProfileSlot(godrays::kNoProfileSlot), uint64_t(0));
        CHECK_EQ(godrays::UnpackProfileSlot(0), godrays::kNoProfileSlot);

        // The fog post's bound buffers (c1..c4): viewport 32 B at cb0, cFogParam 128 B at cb1, cExpHeightFogParam 64 B at cb2, cC2W 64 B at cb3.
        uint32_t bound[ext::ffxiv::engine::kConstantBufferSlots] {};
        bound[0] = 32;
        bound[1] = 128;
        bound[2] = 64;
        bound[3] = 64;
        CHECK_EQ(godrays::ProfileSlotFor(2, bound), 2u);
        CHECK_EQ(godrays::ProfileSlotFor(godrays::kNoProfileSlot, bound), godrays::kNoProfileSlot); // no stamp: no guess
        CHECK_EQ(godrays::ProfileSlotFor(ext::ffxiv::engine::kConstantBufferSlots, bound), godrays::kNoProfileSlot);
        CHECK_EQ(godrays::ProfileSlotFor(5, bound), godrays::kNoProfileSlot); // nothing bound there
        CHECK_EQ(godrays::ProfileSlotFor(1, bound), godrays::kNoProfileSlot); // the stamped slot holds a 128-byte buffer
        // The slot is the stamp's, not a hunt for a 64-byte buffer: with the stamp at 3 the matrix's slot is what would be copied.
        CHECK_EQ(godrays::ProfileSlotFor(3, bound), 3u);
        CHECK_EQ(godrays::ProfileSlotFor(2, nullptr), godrays::kNoProfileSlot);
    }
    for (const Capture& capture : kCaptures)
    {
        unsigned char fog[128], profile[64], c2w[64];
        BytesOf(capture.fog, fog);
        BytesOf(capture.profile, profile);
        const Camera camera = CameraOf(capture.camera);
        std::memcpy(c2w, camera.c2w, sizeof c2w);

        // The rule the tree had picks nothing on the real pair: neither candidate is a rigid transform.
        float first[16], second[16];
        std::memcpy(first, profile, sizeof first);
        std::memcpy(second, c2w, sizeof second);
        CHECK(!Rigid(first));
        CHECK(!Rigid(second));

        // The profile row passes the 48-byte check, and decoded with the fog constants it is a state the march can evaluate.
        if (!godrays::ProfileRowUsable(profile))
            std::printf("    %s: the real cExpHeightFogParam row is refused as a profile\n", capture.name);
        CHECK(godrays::ProfileRowUsable(profile));
        const godrays::FogState state = godrays::DecodeFog(fog, profile);
        CHECK(godrays::FogStateUsable(state));
        CHECK(Near(state.distB, 10.0f, 1.0e-6f));
        CHECK(Near(state.density1, capture.density1, 1.0e-7f));
        CHECK(Near(state.volumeFlag, 1.0f, 1.0e-6f));

        // The 16 bytes past the declared 48 vary per capture: whatever they hold, the decode and the check do not change.
        const float paddings[4][4] { { 0.0f, 0.0f, 0.0f, 1.0f }, { -0.00020445f, 0.00022641f, -1.0f, 0.0f }, { 0.76675f, 0.0f, 0.0f, 0.0f }, { 5.0f, -3.0f, 9.0f, -7.0f } };
        for (const auto& padding : paddings)
        {
            unsigned char padded[64];
            std::memcpy(padded, profile, sizeof padded);
            std::memcpy(padded + 0x30, padding, sizeof padding);
            CHECK(godrays::ProfileRowUsable(padded));
            const godrays::FogState other = godrays::DecodeFog(fog, padded);
            CHECK(std::memcmp(&other, &state, sizeof state) == 0);
        }

        // The clip-to-world matrix in the profile slot is refused. FogStateUsable alone would pass it (its densities are large and
        // positive), which is why the profile has a check of its own.
        CHECK(!godrays::ProfileRowUsable(c2w));
        CHECK(godrays::FogStateUsable(godrays::DecodeFog(fog, c2w)));
    }
    // ==== FOGFIX: the user's own captures (2026-11-07 16:41-16:44, clear weather) bind a correctly laid out profile with every density zero ============
    {
        struct Live
        {
            const char* name;
            const uint32_t (&camera)[88];
            const uint32_t (&fog)[32];
            const uint32_t (&profile)[16];
            bool zero;
        };
        const Live lives[] {
            { "day 164414-3", live_fog::kLiveDayCamera, live_fog::kLiveDayFogParam, live_fog::kLiveDayExpHeightFog, true },
            { "dawn 164337-2", live_fog::kLiveDawnCamera, live_fog::kLiveDawnFogParam, live_fog::kLiveDawnExpHeightFog, true },
            { "night 164135", live_fog::kLiveNightCamera, live_fog::kLiveNightFogParam, live_fog::kLiveNightExpHeightFog, true },
            { "earlier zero 004421-4", live_fog::kZeroEarlierCamera, live_fog::kZeroEarlierFogParam, live_fog::kZeroEarlierExpHeightFog, true },
            { "fogged r2 234604-2", live_fog::kFoggedR2Camera, live_fog::kFoggedR2FogParam, live_fog::kFoggedR2ExpHeightFog, false },
        };
        for (const Live& live : lives)
        {
            unsigned char fog[128], profile[64], c2w[64];
            BytesOf(live.fog, fog);
            BytesOf(live.profile, profile);
            const Camera camera = CameraOf(live.camera);
            std::memcpy(c2w, camera.c2w, sizeof c2w);
            float fields[godrays::kProfileFieldCount];
            godrays::ProfileFieldsOf(profile, fields);
            if (!godrays::ProfileRowUsable(profile))
                std::printf("    %s: the real cExpHeightFogParam row is refused as a profile (field %s)\n", live.name, godrays::ProfileFailedField(fields));
            CHECK(godrays::ProfileRowUsable(profile));
            const godrays::FogState state = godrays::DecodeFog(fog, profile);
            CHECK(godrays::FogStateUsable(state));
            CHECK(Near(state.distB, 10.0f, 1.0e-6f));
            // The engine's colour is live and non-black in every one of them (the atmosphere's ambient term is made of it): FOGFIX item 2.
            CHECK(state.fogColor[0] > 0.01f && state.fogColor[1] > 0.02f && state.fogColor[2] > 0.02f);
            if (live.zero)
            {
                // "The engine has no height fog": both layers' densities are zero, so the density at any height is exactly zero, the mix scale is zero.
                CHECK(state.density1 == 0.0f && state.density2 == 0.0f && state.mixScale == 0.0f);
                for (const float y : { -500.0f, -1.0f, 0.0f, 10.0f, 150.0f, 5000.0f })
                    CHECK(godrays::FogDensityAt(state, y, 1.0f) == 0.0f);
                CHECK(state.refHeight1 < 1.0f && state.refHeight2 < 1.0f && state.refHeight2 < state.refHeight1); // a layout, not a buffer of zeros
            }
            else
            {
                CHECK(state.density1 > 0.004f && state.mixScale == 1.0f);
            }
            // The clip-to-world stand-in beside it is still not a profile, whatever the live camera's numbers are.
            float matrixFields[godrays::kProfileFieldCount];
            godrays::ProfileFieldsOf(c2w, matrixFields);
            CHECK(!godrays::ProfileRowUsable(c2w));
            CHECK(!std::string(godrays::ProfileFailedField(matrixFields)).empty());
        }
        // Night's colour is the dim one and day's the bright one (the colour follows the sky, it is not a stale or fixed value).
        unsigned char dayFog[128], nightFog[128], profile[64];
        BytesOf(live_fog::kLiveDayFogParam, dayFog);
        BytesOf(live_fog::kLiveNightFogParam, nightFog);
        BytesOf(live_fog::kLiveDayExpHeightFog, profile);
        const godrays::FogState day = godrays::DecodeFog(dayFog, profile), night = godrays::DecodeFog(nightFog, profile);
        CHECK(day.fogColor[1] > 10.0f * night.fogColor[1]);
    }
    {
        // A profile with a field outside the ranges the post's own arithmetic clamps to is not a profile.
        unsigned char profile[64];
        BytesOf(kC1ExpHeightFog, profile);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        auto with = [&](uint32_t offset, float value) {
            unsigned char copy[64];
            std::memcpy(copy, profile, sizeof copy);
            std::memcpy(copy + offset, &value, sizeof value);
            return godrays::ProfileRowUsable(copy);
        };
        CHECK(!with(godrays::kExpStartAt, -1.0f));
        CHECK(!with(godrays::kExpFallOff1At, -0.5f));
        CHECK(!with(godrays::kExpDensity1At, -0.001f));
        CHECK(!with(godrays::kExpRefHeight1At, nan));
        CHECK(!with(godrays::kExpMinOpacityAt, 1.5f));
        CHECK(!with(godrays::kExpMinOpacityAt, -0.1f));
        CHECK(!with(godrays::kExpFallOff2At, -0.5f));
        CHECK(!with(godrays::kExpDensity2At, -0.001f));
        CHECK(!with(godrays::kExpRefHeight2At, nan));
        // FOGFIX: a mix scale of 0 is a clear-weather zone (the post's mix weight is sat((1 - t') * mixScale): no height fog drawn), not a wrong buffer;
        // a negative one, a NaN and one past the post's clamp still are.
        CHECK(with(godrays::kExpMixScaleAt, 0.0f));
        CHECK(!with(godrays::kExpMixScaleAt, -0.5f));
        CHECK(!with(godrays::kExpMixScaleAt, nan));
        CHECK(!with(godrays::kExpMixScaleAt, 5000.0f));
        // ... and the densities carry a ceiling, which is what separates the clip-to-world matrix (|density| >= 11.5 in 72 of 72 camera rows) from a profile
        // now that the mix scale no longer does: the ceiling itself passes, anything past it does not, on both layers.
        CHECK(godrays::kMaxProfileDensity == 1.0f);
        CHECK(with(godrays::kExpDensity1At, godrays::kMaxProfileDensity));
        CHECK(with(godrays::kExpDensity2At, godrays::kMaxProfileDensity));
        CHECK(!with(godrays::kExpDensity1At, 1.01f));
        CHECK(!with(godrays::kExpDensity2At, 1.01f));
        CHECK(!with(godrays::kExpDensity1At, 11.49f)); // the smallest density the matrix stand-in shows in 72 camera rows
        CHECK(!with(godrays::kExpDensity2At, 16.08f));
        CHECK(!with(godrays::kExpDensity1At, std::numeric_limits<float>::infinity()));
        CHECK(with(godrays::kExpDensity1At, 0.0f) && with(godrays::kExpDensity2At, 0.0f)); // zero densities are a profile
        CHECK(!with(godrays::kExpDiExponentAt, -2.0f));
        CHECK(with(godrays::kExpDiExponentAt, 4.0f));
        CHECK(!godrays::ProfileRowUsable(nullptr));
    }
    {
        // Which input a march is missing, in the order the first accept needs them; each one has a line of its own.
        using godrays::FirstMissingInput;
        using godrays::MarchInput;
        CHECK(FirstMissingInput(false, false, false, false) == MarchInput::Light);
        CHECK(FirstMissingInput(true, false, true, true) == MarchInput::Cascades);
        CHECK(FirstMissingInput(true, true, false, false) == MarchInput::Fog);
        CHECK(FirstMissingInput(true, true, true, false) == MarchInput::Fog); // landed but not usable
        CHECK(FirstMissingInput(true, true, true, true) == MarchInput::None);
        const std::string light = godrays::MissingInputLine(MarchInput::Light);
        const std::string cascades = godrays::MissingInputLine(MarchInput::Cascades);
        const std::string fog = godrays::MissingInputLine(MarchInput::Fog);
        CHECK(!light.empty() && !cascades.empty() && !fog.empty());
        CHECK(light != cascades && cascades != fog && light != fog);
        CHECK(light.find("light") != std::string::npos);
        CHECK(cascades.find("cascade") != std::string::npos);
        CHECK(fog.find("fog") != std::string::npos);
        CHECK(std::string(godrays::MissingInputLine(MarchInput::None)).empty());
        // The other waits and refusals each have a line of their own.
        const std::string together = godrays::kReadbacksNotTogether;
        const std::string unusable = godrays::kCascadesUnusable;
        const std::string stamp = godrays::kNoProfileStamp;
        const std::string empty = godrays::FormatProfileSlotEmpty(2, 128);
        const std::string refused = godrays::FormatProfileRefused(3);
        const std::string noEngineFog = godrays::kNoEngineHeightFog;
        CHECK(noEngineFog.find("no height fog") != std::string::npos && noEngineFog.find("every density zero") != std::string::npos && noEngineFog.find("leaves the frames alone") != std::string::npos);
        const std::string lines[] { light, cascades, fog, together, unusable, stamp, empty, refused, noEngineFog };
        for (size_t i = 0; i < std::size(lines); ++i)
        {
            CHECK(lines[i].find("God rays: ") == 0);
            for (size_t j = i + 1; j < std::size(lines); ++j)
                CHECK(lines[i] != lines[j]);
        }
        CHECK(empty.find("cb2") != std::string::npos && empty.find("128 bytes") != std::string::npos);
        CHECK(refused.find("cb3") != std::string::npos);
        CHECK(stamp.find("cExpHeightFogParam") != std::string::npos);
        // LIVE1 item 4: the refusal names the fields it judged (the first rc7c session logged "is not a height profile" at 11:26:41 and 11:37:18 and
        // nothing said which field failed, so the daylight "no fog state" skips could not be told from a wrong buffer)
        const float fields[godrays::kProfileFieldCount] { 400.0f, 0.005f, 188.0f, -200.0f, 0.0f, 0.05f, 0.0f, -300.0f, 1.0f, 1.0f, 16.0f };
        const std::string withFields = godrays::FormatProfileRefused(2, fields);
        CHECK(withFields.find("cb2") != std::string::npos && withFields.find("density1 188") != std::string::npos);
        CHECK(withFields.find("start 400") != std::string::npos && withFields.find("diExponent 16") != std::string::npos);
        CHECK(withFields.find("refHeight2 -300") != std::string::npos);
        CHECK(withFields.find("the field that fails is density1") != std::string::npos); // the failing field is named first, ahead of the list
        CHECK(godrays::FormatProfileRefused(2, nullptr) == godrays::FormatProfileRefused(2));
        // which field fails the check: the density above, the others by name
        CHECK(std::string(godrays::ProfileFailedField(fields)) == "density1");
        // the zero profile of the user's 2026-11-07 16:39 log line (mixScale 0, which the check used to refuse) fails nothing now
        const float zeroProfile[godrays::kProfileFieldCount] { 10.0f, 0.0f, 0.0f, -0.25f, 0.0f, 0.0f, 0.0f, -0.5f, 0.0f, 1.0f, 16.0f };
        CHECK(std::string(godrays::ProfileFailedField(zeroProfile)).empty());
        const float good[godrays::kProfileFieldCount] { 400.0f, 0.005f, 0.00173f, -200.0f, 0.0f, 0.05f, 0.0f, -300.0f, 1.0f, 1.0f, 16.0f };
        CHECK(std::string(godrays::ProfileFailedField(good)).empty());
        float negative[godrays::kProfileFieldCount];
        std::memcpy(negative, good, sizeof negative);
        negative[2] = -0.5f;
        CHECK(std::string(godrays::ProfileFailedField(negative)) == "density1");
        negative[2] = good[2];
        negative[0] = -1.0f;
        CHECK(std::string(godrays::ProfileFailedField(negative)) == "start");
    }

    // ==== R4: the pixel-to-NDC mapping, derived from the render window ====================================================================
    {
        const shafts::Window window { 0.5f, 0.5f, 1920.0f, 1080.0f };
        const shafts::NdcRow row = shafts::PixelToNdcRow(window);
        CHECK(row.ok);
        CHECK(Near(row.scale[0], 2.0f / 1920.0f, 1.0e-9f));
        CHECK(Near(row.scale[1], -2.0f / 1080.0f, 1.0e-9f));
        // The window's corners and centre, in the game's framebuffer coordinates (the viewport sits at +0.5).
        auto ndcX = [&](float pixel) { return pixel * row.scale[0] + row.offset[0]; };
        auto ndcY = [&](float pixel) { return pixel * row.scale[1] + row.offset[1]; };
        CHECK(Near(ndcX(0.5f), -1.0f, 1.0e-5f));
        CHECK(Near(ndcY(0.5f), 1.0f, 1.0e-5f));
        CHECK(Near(ndcX(0.5f + 1920.0f), 1.0f, 1.0e-5f));
        CHECK(Near(ndcY(0.5f + 1080.0f), -1.0f, 1.0e-5f));
        CHECK(Near(ndcX(0.5f + 960.0f), 0.0f, 1.0e-5f));
        CHECK(Near(ndcY(0.5f + 540.0f), 0.0f, 1.0e-5f));

        // It reproduces the sun pixel the game reported for c1 (885.752, 21.5481): the light's view-space direction through the captured
        // projection lands on that pixel, and the mapping takes the pixel back to the same NDC the projection produced.
        const Camera camera = CameraOf(kC1Camera);
        const float sun[3] { -0.052945f, 0.366994f, -0.928715f }; // g_LightParam, the same frame as the camera row
        const shafts::SunProjection projected = shafts::ProjectSun(camera.projection, sun, window);
        CHECK(projected.unit && !projected.behind);
        CHECK(Near(projected.screenX, 885.752f, 0.01f));
        CHECK(Near(projected.screenY, 21.5481f, 0.01f));
        CHECK(Near(ndcX(projected.screenX), projected.ndcX, 1.0e-5f));
        CHECK(Near(ndcY(projected.screenY), projected.ndcY, 1.0e-5f));

        // A scene point round trip, the way the stages reconstruct a ray: project a view-space point, take its pixel back through the mapping
        // and the inverse projection with the depth the engine would store, and the point comes back.
        const float point[3] { 1.5f, -0.7f, -6.0f };
        const float clipX = camera.projection[0] * point[0] + camera.projection[1] * point[1] + camera.projection[2] * point[2];
        const float clipY = camera.projection[4] * point[0] + camera.projection[5] * point[1] + camera.projection[6] * point[2];
        const float clipZ = camera.projection[8] * point[0] + camera.projection[9] * point[1] + camera.projection[10] * point[2] + camera.projection[11];
        const float clipW = -point[2];
        const float pixelX = window.x + (clipX / clipW * 0.5f + 0.5f) * window.width;
        const float pixelY = window.y + (0.5f - clipY / clipW * 0.5f) * window.height;
        const float depth = clipZ / clipW;
        const float x = ndcX(pixelX), y = ndcY(pixelY);
        float view[4];
        for (uint32_t i = 0; i < 4; ++i)
            view[i] = camera.inverse[i * 4 + 0] * x + camera.inverse[i * 4 + 1] * y + camera.inverse[i * 4 + 2] * depth + camera.inverse[i * 4 + 3];
        CHECK(Near(view[0] / view[3], point[0], 2.0e-3f));
        CHECK(Near(view[1] / view[3], point[1], 2.0e-3f));
        CHECK(Near(view[2] / view[3], point[2], 2.0e-3f));

        // Another window origin, another size: the mapping follows the window it is given.
        const shafts::NdcRow shifted = shafts::PixelToNdcRow({ 100.0f, 50.0f, 800.0f, 600.0f });
        CHECK(shifted.ok);
        CHECK(Near(100.0f * shifted.scale[0] + shifted.offset[0], -1.0f, 1.0e-5f));
        CHECK(Near(50.0f * shifted.scale[1] + shifted.offset[1], 1.0f, 1.0e-5f));
        CHECK(Near(900.0f * shifted.scale[0] + shifted.offset[0], 1.0f, 1.0e-5f));
        CHECK(Near(650.0f * shifted.scale[1] + shifted.offset[1], -1.0f, 1.0e-5f));

        // An empty, negative or non-finite window has no mapping.
        CHECK(!shafts::PixelToNdcRow({ 0.0f, 0.0f, 0.0f, 0.0f }).ok);
        CHECK(!shafts::PixelToNdcRow({ 0.0f, 0.0f, -10.0f, 10.0f }).ok);
        CHECK(!shafts::PixelToNdcRow({ std::numeric_limits<float>::quiet_NaN(), 0.0f, 10.0f, 10.0f }).ok);

        // The line a session reads to settle the layout: the buffer's bytes next to the derived mapping.
        const float bufferRow[4] { 0.5f, 0.5f, 0.0f, 0.0f };
        const std::string line = shafts::FormatViewportRow("Depth of field", bufferRow, row);
        CHECK(line.find("Depth of field") == 0);
        CHECK(line.find("0.5") != std::string::npos);
        CHECK(line.find("derive") != std::string::npos);
    }

    // ==== R5: the light bounce's per-pixel hash leaves no lattice on the grid the pass samples ======================================================
    {
        // The pass computes irradiance texel (k, l) at window pixel (2k+1, 2l+1), both odd, and draws its rays' azimuth and elevation from two
        // salts of one hash. A lattice in the finished picture would be a hash that correlates with itself at some small offset of that grid.
        // It does not: every offset up to four texels correlates at the noise floor, so the diamond pattern is not the hash. (The remaining
        // candidates are the 9-tap smoothing kernel's own diamond shape and the bilinear read of white noise at half resolution; see the report.)
        constexpr int kSide = 96;
        static float grid[2][kSide][kSide];
        const uint32_t salts[2] { 0u, gi::kHashSaltElevation };
        for (int which = 0; which < 2; ++which)
            for (int l = 0; l < kSide; ++l)
                for (int k = 0; k < kSide; ++k)
                    grid[which][l][k] = gi::Hash01(float(2 * k + 1), float(2 * l + 1), salts[which]);
        for (int which = 0; which < 2; ++which)
        {
            double mean = 0.0;
            for (int l = 0; l < kSide; ++l)
                for (int k = 0; k < kSide; ++k)
                    mean += double(grid[which][l][k]);
            mean /= double(kSide * kSide);
            double variance = 0.0;
            for (int l = 0; l < kSide; ++l)
                for (int k = 0; k < kSide; ++k)
                    variance += (double(grid[which][l][k]) - mean) * (double(grid[which][l][k]) - mean);
            variance /= double(kSide * kSide);
            CHECK(Near(float(mean), 0.5f, 0.02f));
            CHECK(Near(float(variance), 1.0f / 12.0f, 0.01f));
            for (int dy = 0; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    if (dy == 0 && dx <= 0)
                        continue;
                    double sum = 0.0;
                    int n = 0;
                    for (int l = 0; l + dy < kSide; ++l)
                        for (int k = 4; k < kSide - 4; ++k)
                        {
                            sum += (double(grid[which][l][k]) - mean) * (double(grid[which][l + dy][k + dx]) - mean);
                            ++n;
                        }
                    const double rho = sum / double(n) / variance;
                    if (std::fabs(rho) >= 0.06)
                        std::printf("    hash %d correlates %.3f at offset (%d, %d)\n", which, rho, dx, dy);
                    CHECK(std::fabs(rho) < 0.06);
                }
        }
    }

    return Finish("recog_rules");
}
