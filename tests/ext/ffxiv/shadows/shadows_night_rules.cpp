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
        // A fresh fog sun still wins where it can speak: capture 4's pair (the sun 0.7 degrees below the horizon, the dim blue shading light) is day.
        reading = { true, kFogSunY[3], true, fs::LuminanceOf(kLightColour[3]) };
        fs::NightCurveResult result = fs::NightCurve(true, reading);
        CHECK_EQ(result.curve, 0.0f);
        CHECK(result.source == fs::NightSource::FogSun);
        // LIVE1: a day sun well ABOVE the horizon (0.183) with the same dim blue light cannot be the sun: it is the moon, so night (this pairing
        // used to read day; it is the midnight case of the first rc7c session, the fog row up and the engine's light already the moon's).
        reading = { true, kFogSunY[0], true, fs::LuminanceOf(kLightColour[3]) };
        result = fs::NightCurve(true, reading);
        CHECK_EQ(result.curve, 1.0f);
        CHECK(result.source == fs::NightSource::FogSun && result.arm == fs::NightArm::Moon);
        reading = { true, kFogSunY[1], true, 0.26f };
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

    // ---- LIVE1 item 1: the moon arm and the bright-light veto (2026-11-07 first rc7c session) -----------------------------------------------------
    //
    // The session: the fog row said "sun up" (y +0.500 in the log, curve 0) while the engine's own shading light was already the moon (luminance
    // 0.103 at 11:33:07), so the fog-sun-only curve was 0 at midnight. Colour alone fails rc6 capture 4 (moon-coloured light, fog y -0.012, looked
    // like day); the fog row alone fails midnight. The rule needs both: night = max(sun arm, moon arm), moon arm = colour curve x a gate on the
    // fog row's height ABOVE the horizon (the dusk ramp's mirror: +1 deg opens it, +7 deg is fully open), and a bright light (luminance >= 0.9) is
    // day whatever a lagging fog row says.
    {
        // the gate is the sun arm's mirror image
        CHECK(Near(fs::kNightMoonRowUpY, -fs::kNightFogSunUpY, 1.0e-7f));
        CHECK(Near(fs::kNightMoonRowFullY, -fs::kNightFogSunDownY, 1.0e-7f));
        CHECK_EQ(fs::MoonRowGate(-0.5f), 0.0f);
        CHECK_EQ(fs::MoonRowGate(kFogSunY[3]), 0.0f);           // rc6 capture 4, sun 0.7 deg below the horizon
        CHECK_EQ(fs::MoonRowGate(0.0f), 0.0f);
        CHECK_EQ(fs::MoonRowGate(fs::kNightMoonRowUpY), 0.0f);
        CHECK_EQ(fs::MoonRowGate(fs::kNightMoonRowFullY), 1.0f);
        CHECK_EQ(fs::MoonRowGate(0.5f), 1.0f);                  // the log's 0.500
        CHECK_EQ(fs::MoonRowGate(QuietNan()), 0.0f);
        {
            float last = -1.0f;
            for (int i = 0; i <= 200; ++i)
            {
                const float value = fs::MoonRowGate(-0.1f + 0.002f * float(i));
                CHECK(value >= last && value >= 0.0f && value <= 1.0f);
                last = value;
            }
        }
        // the moon arm: the colour curve through the gate
        CHECK_EQ(fs::NightFromMoon(0.5f, 1.5f), 0.0f);          // a bright light: never the moon
        CHECK_EQ(fs::NightFromMoon(0.5f, 0.9f), 0.0f);
        CHECK_EQ(fs::NightFromMoon(0.5f, 0.103f), 1.0f);
        CHECK_EQ(fs::NightFromMoon(0.5f, 0.15f), 1.0f);
        CHECK_EQ(fs::NightFromMoon(kFogSunY[3], 0.111f), 0.0f); // the colour alone cannot make capture 4 night
        CHECK_EQ(fs::NightFromMoon(0.5f, QuietNan()), 0.0f);    // an unreadable colour is not a moon
    }
    {
        // every capture on disk (cbdump cFogParam row 2 + cb.csv g_LightParam at the same instant) and the session's readings, through the rule
        struct Frame
        {
            const char* name;
            float y;
            float colour[3];
            float expected;
            float tolerance;
        };
        const Frame frames[] {
            { "rc6 c1 day", 0.183012649f, { 1.96f, 1.48981f, 1.24563f }, 0.0f, 0.0f },
            { "rc6 c2 dusk blue", -0.108604327f, { 0.1764f, 0.263511f, 0.49f }, 0.958f, 0.005f },
            { "rc6 c3 twilight", -0.058195621f, { 0.0779614f, 0.116937f, 0.16f }, 0.335f, 0.005f },
            { "rc6 c4 looked like day", -0.011878015f, { 0.0776352f, 0.116538f, 0.16f }, 0.0f, 0.0f },
            { "11/05 22:11 night, sun far below", -0.683013f, { 0.0779614f, 0.116937f, 0.16f }, 1.0f, 0.0f },
            { "11/05 22:13 day, sun 20 deg up", 0.338030f, { 1.96f, 1.76403f, 1.54676f }, 0.0f, 0.0f },
            { "11/06 21:16 night, sun far below", -0.866025f, { 0.1764f, 0.263511f, 0.49f }, 1.0f, 0.0f },
            { "11/06 18:00 dusk blue, fog row +12 deg (no ground truth)", 0.213300f, { 0.238757f, 0.373067f, 0.49f }, 0.82f, 0.01f },
            { "11/07 midnight: fog 0.500 + moonlight (two different log moments)", 0.5f, { 0.0703f, 0.1067f, 0.16f }, 1.0f, 0.0f },
            { "11/07 11:26:29 fog 0.500 + the blue fill light", 0.5f, { 0.3986f, 0.7511f, 1.0f }, 0.185f, 0.005f },
        };
        for (const Frame& frame : frames)
        {
            fs::NightReading reading { true, frame.y, true, fs::LuminanceOf(frame.colour) };
            const fs::NightCurveResult result = fs::NightCurve(true, reading);
            if (!Near(result.curve, frame.expected, frame.tolerance))
                std::printf("    %s: curve %.4f, wanted %.4f\n", frame.name, double(result.curve), double(frame.expected));
            CHECK(Near(result.curve, frame.expected, frame.tolerance));
            CHECK(result.source == fs::NightSource::FogSun);
        }
        // the arm that decided
        fs::NightReading midnight { true, 0.5f, true, 0.1028f };
        CHECK(fs::NightCurve(true, midnight).arm == fs::NightArm::Moon);
        fs::NightReading dusk { true, kFogSunY[1], true, 0.2613f };
        CHECK(fs::NightCurve(true, dusk).arm == fs::NightArm::Sun);
        fs::NightReading noon { true, 0.9f, true, 1.9f };
        CHECK_EQ(fs::NightCurve(true, noon).curve, 0.0f);
        CHECK(fs::NightCurve(true, noon).arm == fs::NightArm::BrightLight);
        // a bright light is day whatever a lagging fog row says (the session logged curve 1.000 with luminance 1.57 / 1.81 / 1.37 / 1.15)
        for (const float luminance : { 0.9f, 1.155f, 1.370f, 1.572f, 1.814f })
        {
            fs::NightReading lagging { true, -0.5f, true, luminance };
            CHECK_EQ(fs::NightCurve(true, lagging).curve, 0.0f);
            CHECK(fs::NightCurve(true, lagging).arm == fs::NightArm::BrightLight);
        }
        fs::NightReading justBelow { true, -0.5f, true, 0.8999f };
        CHECK_EQ(fs::NightCurve(true, justBelow).curve, 1.0f); // below the veto the sun arm decides: the sun is far below, so night
        // no colour yet: the sun arm alone, exactly as before
        fs::NightReading noColour { true, 0.5f, false, 0.0f };
        CHECK_EQ(fs::NightCurve(true, noColour).curve, 0.0f);
        fs::NightReading noColourNight { true, -0.5f, false, 0.0f };
        CHECK_EQ(fs::NightCurve(true, noColourNight).curve, 1.0f);
        // a reading whose colour may not arm (the character ambient's: its capture's draw is not proven to be the main view's) keeps the sun arm alone
        // under a fresh fog row, the pre-LIVE1 rule: a bright character-view light cannot veto the world's night, a dim one cannot make a moon
        fs::NightReading characterBright { true, kFogSunY[1], true, 1.9f, false };
        CHECK(Near(fs::NightCurve(true, characterBright).curve, 0.958f, 0.005f));
        CHECK(fs::NightCurve(true, characterBright).arm == fs::NightArm::Sun);
        fs::NightReading characterDimByDay { true, 0.5f, true, 0.1028f, false };
        CHECK_EQ(fs::NightCurve(true, characterDimByDay).curve, 0.0f);
        fs::NightReading characterStale { false, 0.5f, true, 0.1028f, false };
        CHECK_EQ(fs::NightCurve(true, characterStale).curve, 1.0f); // the colour fallback is untouched
        // a stale fog row still hands the curve to the colour; the toggle off still pins 1
        fs::NightReading stale { false, 0.5f, true, 0.1028f };
        CHECK_EQ(fs::NightCurve(true, stale).curve, 1.0f);
        CHECK(fs::NightCurve(true, stale).source == fs::NightSource::LightColour);
        fs::NightReading pinned { true, 0.5f, true, 1.9f };
        CHECK_EQ(fs::NightCurve(false, pinned).curve, 1.0f);
        // monotone in the colour at a fixed height above the horizon: a dimmer light is never less night
        float lastCurve = -1.0f;
        for (int i = 0; i <= 60; ++i)
        {
            fs::NightReading sweep { true, 0.5f, true, 1.2f - 0.02f * float(i) };
            const float curve = fs::NightCurve(true, sweep).curve;
            CHECK(curve >= lastCurve);
            lastCurve = curve;
        }
    }
    {
        // the log lines name the arm and the fog height (the missing datum of the session: the log had no fog y at the moon-coloured moments)
        const float moon[3] { 0.0703f, 0.1067f, 0.16f };
        const std::string change = fs::FormatColourChange(moon, 1.0f, 0, true, 0.5f, fs::NightArm::Moon);
        CHECK(change.find("fog sun y +0.500") != std::string::npos && change.find("moon") != std::string::npos);
        CHECK(fs::FormatColourChange(moon, 1.0f, 0).find("fog sun y") == std::string::npos); // the old call is unchanged
        const std::string bright = fs::FormatColourChange(moon, 0.0f, 0, true, -0.5f, fs::NightArm::BrightLight);
        CHECK(bright.find("fog sun y -0.500") != std::string::npos && bright.find("bright light") != std::string::npos);
        const std::string sun = fs::FormatNightSource(fs::NightSource::FogSun, 0.5f, 0.103f, 1.0f, fs::NightArm::Moon);
        CHECK(sun.find("moon") != std::string::npos && sun.find("0.103") != std::string::npos);
        CHECK(fs::FormatNightSource(fs::NightSource::FogSun, -0.1086f, 0.109f, 0.958f).find("the night curve reads the fog sun") != std::string::npos);
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

    // ---- the night grade's feed (L1): another consumer keeps the colour and the fog sun fed whatever the keys say ---------------------------------------
    {
        // THE STARVED CELL: night grade on, Shadow lift 0, Night ambient lift 1 (both at their identity). Without the feed the route is not wanted, no colour
        // is ever captured, and at the user's midnight (the fog row above the horizon) the moon arm has no colour: curve 0. With the feed the route runs.
        CHECK(!fs::MaskWantsDecision(true, true, 0.0f, 1.0f, true));                  // the module's own keys alone: nothing runs
        CHECK(fs::MaskWantsDecision(true, true, 0.0f, 1.0f, true, true));             // + the night grade's request: the route feeds it
        CHECK(fs::MaskWantsDecision(true, true, 0.0f, 1.0f, false, true));            // the card's toggle unticked does not stop the feed (the grade ticks it itself)
        CHECK(fs::MaskWantsDecision(false, true, 0.0f, 1.0f, true, true));            // the master key off: the feed still runs
        CHECK(!fs::MaskWantsDecision(true, false, 0.0f, 1.0f, true, true));           // stood down: nothing, with or without the request
        CHECK(!fs::MaskWantsDecision(false, true, 0.5f, 2.0f, true, false));          // the master off without a request stays off (the shipped default)
        CHECK(fs::MaskWantsDecision(true, true, 0.5f, 1.0f, true, false));            // an existing arm is unchanged by the new parameter
        CHECK(fs::FogSunWanted(true, true, 0.0f, 1.0f, true, true));                  // the fog sun capture follows the request
        CHECK(fs::FogSunWanted(true, true, 0.0f, 1.0f, false, true));                 // ... also with the toggle unticked
        CHECK(fs::FogSunWanted(false, true, 0.0f, 1.0f, true, true));                 // ... also with the master key off
        CHECK(!fs::FogSunWanted(true, false, 0.0f, 1.0f, true, true));                // stood down
        CHECK(!fs::FogSunWanted(true, true, 0.0f, 1.0f, true, false));                // without the request: as before
        // the colour the starved cell needs: at midnight (fog row above the horizon, moonlight colour) the curve is 1 only WITH the colour
        fs::NightReading midnight;
        midnight.fogFresh = true;
        midnight.fogSunY = 0.5f;
        CHECK_EQ(fs::NightCurve(true, midnight).curve, 0.0f);       // no colour reached the ring: curve 0 (the bug the feed fixes)
        midnight.haveColour = true;
        midnight.luminance = 0.1028f;
        CHECK_EQ(fs::NightCurve(true, midnight).curve, 1.0f);       // with the moonlight colour: night
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
