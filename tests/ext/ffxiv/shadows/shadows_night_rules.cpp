// Shadow mapping: the night curve follows the visible sun (ext/ffxiv/shadows/ShadowsRules.h), on the host.
//
// The input of the curve is the fog post's own sun: cFogParam[2].y, the visible sun's world elevation sine (0.183 on the day frame with the sun in view,
// -0.109 / -0.058 / -0.012 on the three frames of the evening). night = smoothstep(sin(-1 deg), sin(-7 deg), y): no lift while the sun is above -1
// degree, full by the end of civil twilight. The light colour (what the curve read before) stays as the fallback when no fog sun has landed recently.
//
// What a wrong rule would do: the colour of the main view's shading light on the evening frame of capture 4 is the dim blue moon-like light, which put
// the curve at 1 (lift on) on a frame the player saw as daylight; the fog sun there is -0.012, so the curve is 0. A curve that read a stale sun for ever
// would lift the shadows of a later daytime scene; a curve with no fallback would run the identity whenever the fog capture failed (the user's session had
// several such frames).
#include "check.h"

#include "ext/ffxiv/shadows/ShadowsRules.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace fs = ext::ffxiv::shadows;

namespace
{
bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
float QuietNan() { return std::numeric_limits<float>::quiet_NaN(); }

// The four rc6 frames: the fog sun's world y and the main view's light colour.
constexpr float kFogSunY[4] { 0.183012649f, -0.108604327f, -0.058195621f, -0.011878015f };
constexpr float kLightColour[4][3] { { 1.96f, 1.48981f, 1.24563f }, { 0.1764f, 0.263511f, 0.49f }, { 0.0779614f, 0.116937f, 0.16f }, { 0.0776352f, 0.116538f, 0.16f } };
} // namespace

int main()
{
    // ---- the thresholds are sin(-1 deg) and sin(-7 deg) -------------------------------------------------------------------------
    CHECK(Near(fs::kNightFogSunUpY, std::sin(-1.0f * 3.14159265f / 180.0f), 1.0e-6f));
    CHECK(Near(fs::kNightFogSunDownY, std::sin(-7.0f * 3.14159265f / 180.0f), 1.0e-6f));
    CHECK(fs::kNightFogSunUpY > fs::kNightFogSunDownY); // the sun goes down: y falls from the "up" edge to the "down" edge

    // ---- the curve on the fog sun's y: the four frames, the endpoints, monotone ---------------------------------------------------
    CHECK_EQ(fs::NightFromFogSun(kFogSunY[0]), 0.0f);                       // capture 1: sun well above the horizon
    CHECK(Near(fs::NightFromFogSun(kFogSunY[1]), 0.958f, 0.005f));          // capture 2: -6.2 degrees, nearly full night
    CHECK(Near(fs::NightFromFogSun(kFogSunY[2]), 0.335f, 0.005f));          // capture 3: -3.3 degrees, twilight
    CHECK_EQ(fs::NightFromFogSun(kFogSunY[3]), 0.0f);                       // capture 4: -0.7 degrees, still daylight to the player
    CHECK_EQ(fs::NightFromFogSun(fs::kNightFogSunUpY), 0.0f);
    CHECK_EQ(fs::NightFromFogSun(fs::kNightFogSunDownY), 1.0f);
    CHECK_EQ(fs::NightFromFogSun(-0.5f), 1.0f);
    CHECK_EQ(fs::NightFromFogSun(QuietNan()), 0.0f); // an unreadable sun is not night
    {
        float last = -1.0f;
        for (int i = 0; i <= 200; ++i)
        {
            const float y = 0.2f - 0.002f * float(i); // 0.2 down to -0.2
            const float value = fs::NightFromFogSun(y);
            CHECK(value >= last && value >= 0.0f && value <= 1.0f);
            last = value;
        }
    }

    // ---- what the colour curve said on the same frames: capture 4 is the case the fog sun fixes ------------------------------------
    {
        const float colourCurve[4] { fs::CurveAt(fs::LuminanceOf(kLightColour[0])), fs::CurveAt(fs::LuminanceOf(kLightColour[1])),
                                     fs::CurveAt(fs::LuminanceOf(kLightColour[2])), fs::CurveAt(fs::LuminanceOf(kLightColour[3])) };
        CHECK_EQ(colourCurve[0], 0.0f);
        CHECK(colourCurve[1] > 0.9f);
        CHECK_EQ(colourCurve[2], 1.0f);
        CHECK_EQ(colourCurve[3], 1.0f); // capture 4: the moon-like light put the curve at 1 on a daylight frame
        CHECK(fs::NightFromFogSun(kFogSunY[3]) < colourCurve[3]);
    }

    // ---- the source: the fog sun when it is fresh, the light colour when it is not, nothing when there is neither ---------------------
    {
        fs::NightReading reading;
        // A fresh fog sun wins over the colour, both ways: a day sun with a dim blue shading light reads the sun, a night sun with a bright light too.
        reading = { true, kFogSunY[0], true, fs::LuminanceOf(kLightColour[3]) };
        fs::NightCurveResult result = fs::NightCurve(true, reading);
        CHECK_EQ(result.curve, 0.0f);
        CHECK(result.source == fs::NightSource::FogSun);
        reading = { true, kFogSunY[1], true, 1.5f };
        result = fs::NightCurve(true, reading);
        CHECK(Near(result.curve, 0.958f, 0.005f) && result.source == fs::NightSource::FogSun);
        // A stale or absent fog sun falls back to the colour: the scenarios of the first slice (no fog data) keep their curve.
        reading = { false, kFogSunY[0], true, 0.109f };
        result = fs::NightCurve(true, reading);
        CHECK_EQ(result.curve, 1.0f);
        CHECK(result.source == fs::NightSource::LightColour);
        reading = { false, 0.0f, true, 1.5f };
        CHECK_EQ(fs::NightCurve(true, reading).curve, 0.0f);
        // Neither: the identity, as before any reading landed.
        reading = { false, 0.0f, false, 0.0f };
        result = fs::NightCurve(true, reading);
        CHECK_EQ(result.curve, 0.0f);
        CHECK(result.source == fs::NightSource::None);
        // The toggle off pins the full effect whatever either source says (the debug override of the first slice).
        reading = { true, kFogSunY[0], true, 1.5f };
        CHECK_EQ(fs::NightCurve(false, reading).curve, 1.0f);
        // The colour curve of the first slice is unchanged where it is used.
        CHECK_EQ(fs::CurveStrength(true, true, 0.05f), 1.0f);
        CHECK_EQ(fs::CurveStrength(true, false, 0.05f), 0.0f);
    }

    // ---- freshness is on the consumer's own clock: rendered frames, not the present counter ------------------------------------------------
    {
        fs::FogSunTracker tracker;
        CHECK(!tracker.Fresh(0));                // nothing published yet
        tracker.Observe(0, 5);                   // a publish count of 0 is "nothing"
        CHECK(!tracker.Fresh(5));
        tracker.Observe(1, 10);
        CHECK(tracker.Fresh(10));
        CHECK(tracker.Fresh(10 + fs::kFogSunMaxAgeFrames));       // exactly at the bound
        CHECK(!tracker.Fresh(10 + fs::kFogSunMaxAgeFrames + 1));  // one past it
        tracker.Observe(1, 40);                  // the same publish seen again: the clock does NOT restart
        CHECK(!tracker.Fresh(10 + fs::kFogSunMaxAgeFrames + 1));
        tracker.Observe(2, 200);                 // a new publish restarts it
        CHECK(tracker.Fresh(200) && tracker.Fresh(200 + fs::kFogSunMaxAgeFrames) && !tracker.Fresh(200 + fs::kFogSunMaxAgeFrames + 1));
        CHECK(!tracker.Fresh(199));              // a clock that ran backwards is not fresh
        tracker.Reset();
        CHECK(!tracker.Fresh(200));
        tracker.Observe(2, 300);                 // a publish seen before the reset is not revived by it...
        CHECK(!tracker.Fresh(300));
        tracker.Observe(3, 310);                 // ...a new one is
        CHECK(tracker.Fresh(310));
        // The present counter steps once, or four times, per rendered frame under frame generation; the consumer's clock counts distinct frames
        // (its capture sequence), so the bound is the same number of RENDERED frames at 1x and at 4x.
        for (const uint64_t step : { uint64_t(1), uint64_t(4) })
        {
            fs::FogSunTracker t;
            uint64_t sequence = 0, lastKey = ~uint64_t(0), staleAt = 0;
            for (uint64_t frame = 0; frame < 400; ++frame)
            {
                const uint64_t key = frame * step; // the present counter at this rendered frame
                if (key != lastKey)
                {
                    lastKey = key;
                    ++sequence;                    // the consumer's capture sequence: one per distinct key
                }
                if (frame == 0)
                    t.Observe(1, sequence);        // one publish at the first frame, none after
                if (staleAt == 0 && !t.Fresh(sequence))
                    staleAt = frame;
            }
            CHECK_EQ(staleAt, uint64_t(fs::kFogSunMaxAgeFrames) + 1u);
        }
        CHECK(fs::kFogSunMaxAgeFrames >= 60 && fs::kFogSunMaxAgeFrames <= 600); // seconds, not minutes, and not a flicker
    }

    // ---- both consumers read the one curve: the mask lift AND the ambient table ------------------------------------------------------------
    {
        const fs::Blend off = fs::FrameBlendCurve(true, true, 0.5f, 0.25f, 2.5f, 0.0f);
        CHECK_EQ(off.maskLift, 0.0f);
        CHECK_EQ(off.ambientScale, 1.0f);
        CHECK(!off.maskRuns && !off.ambientRuns);
        const fs::Blend night = fs::FrameBlendCurve(true, true, 0.5f, 0.25f, 2.5f, 1.0f);
        CHECK(Near(night.maskLift, 0.5f, 1.0e-6f) && Near(night.ambientScale, 2.5f, 1.0e-6f) && night.maskRuns && night.ambientRuns);
        const float curve = fs::NightFromFogSun(kFogSunY[2]);
        const fs::Blend dusk = fs::FrameBlendCurve(true, true, 0.5f, 0.25f, 2.5f, curve);
        CHECK(Near(dusk.maskLift, 0.5f * curve, 1.0e-6f));
        CHECK(Near(dusk.ambientScale, 1.0f + 1.5f * curve, 1.0e-6f));
        CHECK(Near(dusk.curve, curve, 1.0e-6f));
        // The first slice's colour form is the same blend for the same curve (nothing else moved).
        const fs::Blend viaColour = fs::FrameBlend(true, true, 0.5f, 0.25f, 2.5f, true, true, 0.05f);
        const fs::Blend viaCurve = fs::FrameBlendCurve(true, true, 0.5f, 0.25f, 2.5f, fs::CurveStrength(true, true, 0.05f));
        CHECK(viaColour.maskLift == viaCurve.maskLift && viaColour.ambientScale == viaCurve.ambientScale && viaColour.curve == viaCurve.curve);
        // Master off: nothing runs.
        const fs::Blend masterOff = fs::FrameBlendCurve(false, true, 0.5f, 0.25f, 2.5f, 1.0f);
        CHECK(!masterOff.maskRuns && !masterOff.ambientRuns && masterOff.maskLift == 0.0f);
    }

    // ---- the gate of the route: the night curve needs the fog sun only when it can act ------------------------------------------------------
    {
        // Wanted: the route is wanted (master on, not stood down, lift off identity or the ambient arm on the ticked curve) and the curve is ticked.
        CHECK(fs::FogSunWanted(true, true, 0.5f, 1.0f, true));
        CHECK(fs::FogSunWanted(true, true, 0.0f, 2.0f, true));
        CHECK(!fs::FogSunWanted(true, true, 0.5f, 1.0f, false));  // the toggle pins the curve: no input needed
        CHECK(!fs::FogSunWanted(true, true, 0.0f, 1.0f, true));   // both keys at identity: nothing runs
        CHECK(!fs::FogSunWanted(false, true, 0.5f, 2.0f, true));  // master off (the shipped default): the capture stays off
        CHECK(!fs::FogSunWanted(true, false, 0.5f, 2.0f, true));  // stood down
    }

    // ---- the log lines: once per change of source ----------------------------------------------------------------------------------------------
    {
        const std::string fog = fs::FormatNightSource(fs::NightSource::FogSun, -0.1086f, 0.109f, 0.958f);
        CHECK(fog.find("fog sun") != std::string::npos && fog.find("-0.109") != std::string::npos && fog.find("0.958") != std::string::npos);
        const std::string colour = fs::FormatNightSource(fs::NightSource::LightColour, 0.0f, 0.109f, 1.0f);
        CHECK(colour.find("light colour") != std::string::npos && colour.find("0.109") != std::string::npos && colour.find("no fog sun") != std::string::npos);
        const std::string none = fs::FormatNightSource(fs::NightSource::None, 0.0f, 0.0f, 0.0f);
        CHECK(none.find("neither") != std::string::npos);
        CHECK(fs::NightSourceKey(fs::NightSource::FogSun) != fs::NightSourceKey(fs::NightSource::LightColour));
        CHECK(fs::NightSourceKey(fs::NightSource::None) != fs::NightSourceKey(fs::NightSource::LightColour));
    }

    return Finish("shadows_night_rules");
}
