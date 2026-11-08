// Engine light controls (ENG2): the pure rules on the host (no device).
//
// What a wrong rule would break, and what is checked here:
//   the keys: every default is the contract's, every clamp (NaN -> the default), Wanted (a lamps-only or cb-only configuration is wanted: the feed is NOT tied to
//   the exposure stage), the effective factor (the A/B and the master off are exactly 1, weight 0 is exactly 1, the weight lerps);
//   the weight: WALL-CLOCK smoothing (63 % in a second at any update rate, four updates of dt/4 equal one of dt, a misread moves it ~5 %, a stalled step cannot flash
//   it, the snap lands a day frame on exactly 0), and the shared night curve on every capture on disk with the toggle forced (day = 0, night = full);
//   the exposure: the fingerprint of the real g_CommonParameter bytes of the captures, the compensation formula (s_hold / s_now, NOT s_now / s_hold), its bounds,
//   the identities (mode 0, master off, A/B, weight 0), the manual factor;
//   the controller on a synthetic first-order-lag engine: a day -> night step latches the engine's SETTLED identity exposure and never the day one (the advisor's
//   blocker: freezing at the first frame the controls act would hold a day exposure and crush the night), the hold does not follow `s` while the controls act, the A/B
//   release is a free re-latch and a short A/B leaves the hold alone, a gap forgets the hold, the wait runs out into a documented latch, a session with no reading
//   fails open, the forced weight never waits, a cb-only configuration needs no reading at all.
#include "check.h"

#include "ext/ffxiv/enginelight/EngineLightRules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace el = ext::ffxiv::enginelight;
namespace fs = ext::ffxiv::shadows;

namespace
{
bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
float QuietNan() { return std::numeric_limits<float>::quiet_NaN(); }

// Every capture on disk (analysis\rc6-session\fixes\l1\FALSE-NIGHT-table.md): the fog row's y, the sun-draw light luminance (< 0: no colour), the weight the shared
// night curve must give and whether the frame is daylight. Copied from the retired night grade's rules test: the rule did not change, only its consumer.
struct Capture
{
    const char* name;
    float fogY;
    float lightL;
    float expected;
    bool day;
};
constexpr Capture kCaptures[] = {
    { "c1 11/05 22:09 (day, sun in view)", 0.683013f, 1.880749f, 0.0f, true },
    { "c2 11/05 22:09b (night plaza, no sun-draw row)", -0.683013f, -1.0f, 1.0f, false },
    { "c3 11/05 22:11 (night plaza)", -0.683013f, 0.111760f, 1.0f, false },
    { "c4 11/05 22:13 (day, interior)", 0.338035f, 1.790006f, 0.0f, true },
    { "11/06 18:00 (dusk courtyard: lamps lit)", 0.213297f, 0.352955f, 0.819948f, false },
    { "11/06 21:16 (night courtyard)", -0.866025f, 0.261344f, 1.0f, false },
    { "r1 11/06 23:44 (low-sun coast)", 0.183013f, 1.572143f, 0.0f, true },
    { "r2 11/06 23:46 (rain, sun 6 degrees down)", -0.108604f, 0.261344f, 0.955684f, false },
    { "r3 11/06 23:46 (lamp-lit hall, sun 3 degrees down)", -0.058196f, 0.111760f, 0.337943f, false },
    { "r4 11/06 23:47 (evening, sun 0.7 degrees down, looked like day)", -0.011878f, 0.111405f, 0.0f, true },
    { "b1 (benchmark, no colour feed)", 0.295f, -1.0f, 0.0f, true },
    { "b5 (benchmark, fog sun 3 degrees up)", 0.057f, -1.0f, 0.0f, true },
    { "benchmark: the highest fog sun", 0.612f, -1.0f, 0.0f, true },
};

fs::NightReading ReadingOf(const Capture& c)
{
    fs::NightReading reading;
    reading.fogFresh = true;
    reading.fogSunY = c.fogY;
    reading.haveColour = c.lightL >= 0.0f;
    reading.luminance = c.lightL >= 0.0f ? c.lightL : 0.0f;
    return reading;
}

float Run(float start, float target, float seconds, float rate)
{
    float w = start;
    const int n = int(seconds * rate + 0.5f);
    for (int i = 0; i < n; ++i)
        w = el::NextWeight(w, target, 1.0f / rate);
    return w;
}

// ---- a synthetic engine: its exposure `s` follows a target with a first-order lag; the controls raise the target by `takeBack` x the weight that is really acting ----
struct Sim
{
    el::Controller c;
    float s = 0.7f;           // the engine's exposure now
    float sIdentity = 0.7f;   // the exposure the engine settles to with the controls at identity (day: 0.7, night plaza: 1.4)
    float takeBack = 0.35f;   // the controls' darkening raises the engine's target by this share at full weight
    float tau = 4.0f;         // seconds
    float curve = 0.0f;       // the night curve's answer fed in
    bool haveReading = true;
    bool sReadings = true;    // whether reads of `s` land at all
    float t = 0.0f;
    float dtStep = 1.0f / 60.0f;
    float noise = 0.0f;       // relative per-frame wobble of the value the CPU reads (the real exposure is not a clean curve)
    int holdFrames = 1;       // the value is only rewritten every this many frames (a stepped update)
    int frameNo = 0;
    float observed = 0.7f;

    float Acting() const { return (!c.params.master || c.params.referenceAB || !el::Wanted(c.params)) ? 0.0f : c.weight; }
    void Advance(float seconds, float dtOverride = -1.0f)
    {
        const float dt = dtOverride > 0.0f ? dtOverride : dtStep;
        const int n = int(seconds / dt + 0.5f);
        for (int i = 0; i < n; ++i)
        {
            const float target = sIdentity * (1.0f + takeBack * Acting());
            s += (target - s) * (1.0f - std::exp(-dt / tau));
            if (frameNo++ % holdFrames == 0)
                observed = s * (1.0f + noise * std::sin(float(frameNo) * 1.9f));
            el::StepInput in;
            in.dt = dt;
            in.haveReading = haveReading;
            in.curve = curve;
            in.sFresh = sReadings;
            in.sValid = sReadings;
            in.s = observed;
            c.Step(in);
            t += dt;
        }
    }
};
} // namespace

int main()
{
    // ---- the keys: the contract's defaults and the ranges ---------------------------------------------------------------------------
    {
        const el::Params d;
        CHECK(d.master && !d.referenceAB && !d.forceNightWeight);
        CHECK_EQ(d.moonIntensity, 0.1f);
        CHECK_EQ(d.ambientIntensity, 0.5f);
        CHECK_EQ(d.waterMoon, 0.2f);
        CHECK_EQ(d.waterAmbient, 0.5f);
        CHECK_EQ(d.lampIntensity, 3.0f);
        CHECK_EQ(d.exposureMode, 1);
        CHECK_EQ(d.exposureManual, 1.0f);
        for (int i = 0; i < 3; ++i)
            CHECK(d.moonTint[i] == 1.0f && d.ambientTint[i] == 1.0f && d.waterTint[i] == 1.0f);
        // the defaults are inside the ranges
        CHECK(d.moonIntensity >= el::kMoonMin && d.moonIntensity <= el::kMoonMax);
        CHECK(d.ambientIntensity >= el::kAmbientMin && d.ambientIntensity <= el::kAmbientMax);
        CHECK(d.waterMoon >= el::kWaterMoonMin && d.waterAmbient >= el::kWaterAmbientMin);
        CHECK(d.lampIntensity >= el::kLampMin && d.lampIntensity <= el::kLampMax);

        el::Params bad;
        bad.moonIntensity = QuietNan();
        bad.ambientIntensity = -4.0f;
        bad.waterMoon = 9.0f;
        bad.waterAmbient = 0.0f;
        bad.lampIntensity = 99.0f;
        bad.exposureManual = 0.0f;
        bad.exposureMode = 9;
        bad.moonTint[0] = 0.0f;
        bad.ambientTint[1] = 7.0f;
        bad.waterTint[2] = QuietNan();
        const el::Params s = el::Sanitize(bad);
        CHECK_EQ(s.moonIntensity, d.moonIntensity);               // NaN -> the default
        CHECK_EQ(s.ambientIntensity, el::kAmbientMin);
        CHECK_EQ(s.waterMoon, el::kWaterMoonMax);
        CHECK_EQ(s.waterAmbient, el::kWaterAmbientMin);
        CHECK_EQ(s.lampIntensity, el::kLampMax);
        CHECK_EQ(s.exposureManual, el::kManualMin);
        CHECK_EQ(s.exposureMode, el::kExposureAuto);
        CHECK_EQ(s.moonTint[0], el::kTintMin);
        CHECK_EQ(s.ambientTint[1], el::kTintMax);
        CHECK_EQ(s.waterTint[2], 1.0f);
        CHECK(el::ParamsEqual(el::Sanitize(d), d));
        el::Params other = d;
        other.waterTint[1] = 1.2f;
        CHECK(!el::ParamsEqual(other, d));
        other = d;
        other.referenceAB = true;
        CHECK(!el::ParamsEqual(other, d));
    }

    // ---- Wanted: the feed and the seam follow what would change, NOT the exposure stage --------------------------------------------
    {
        el::Params p;
        CHECK(el::Wanted(p));                       // the defaults
        p.master = false;
        CHECK(!el::Wanted(p));                      // master off: nothing
        p = el::Params {};
        p.referenceAB = true;
        CHECK(el::Wanted(p));                       // the A/B keeps the module armed (the latch and the weight keep running)
        // lamps only, exposure off: wanted (a configuration the old grade-only feed would have starved)
        el::Params lamps;
        lamps.moonIntensity = lamps.ambientIntensity = lamps.waterMoon = lamps.waterAmbient = 1.0f;
        lamps.exposureMode = el::kExposureOff;
        CHECK(el::Wanted(lamps) && !el::StageWanted(lamps));
        // cb only (writer 1's arms), lamps 1, exposure off
        el::Params cb = lamps;
        cb.lampIntensity = 1.0f;
        cb.moonIntensity = 0.1f;
        CHECK(el::Wanted(cb) && !el::StageWanted(cb));
        // everything at the engine's value
        el::Params none = cb;
        none.moonIntensity = 1.0f;
        CHECK(!el::Wanted(none));
        // a tint alone is an arm
        none.moonTint[0] = 0.9f;
        CHECK(el::Wanted(none));
        // the auto hold needs a light arm to hold against; the manual factor stands alone
        el::Params holdOnly = none;
        holdOnly.moonTint[0] = 1.0f;
        holdOnly.exposureMode = el::kExposureAuto;
        CHECK(!el::Wanted(holdOnly) && !el::StageWanted(holdOnly));
        holdOnly.exposureMode = el::kExposureManual;
        holdOnly.exposureManual = 1.3f;
        CHECK(el::Wanted(holdOnly) && el::StageWanted(holdOnly));
    }

    // ---- the effective factor: master off, the A/B and weight 0 are exactly 1 (mutants M5, M9) -----------------------------------------
    {
        el::Params p;
        p.lampIntensity = 3.0f;
        CHECK_EQ(el::LampFactor(p, 1.0f), 3.0f);
        CHECK_EQ(el::LampFactor(p, 0.5f), 2.0f);
        CHECK_EQ(el::LampFactor(p, 0.0f), 1.0f);        // a day frame: exactly 1, no state touched
        CHECK_EQ(el::LampFactor(p, QuietNan()), 1.0f);
        CHECK(!el::LampActive(p, 0.0f) && el::LampActive(p, 1.0f));
        p.referenceAB = true;
        CHECK_EQ(el::LampFactor(p, 1.0f), 1.0f);        // the A/B shows the engine's own value
        CHECK(!el::LampActive(p, 1.0f));
        p.referenceAB = false;
        p.master = false;
        CHECK_EQ(el::LampFactor(p, 1.0f), 1.0f);
        CHECK_EQ(el::Effective(p, 0.2f, 1.0f), 1.0f);
    }

    // ---- the weight: wall-clock smoothing ----------------------------------------------------------------------------------------------
    {
        CHECK_EQ(el::TargetWeight(false, 0.9f), 0.0f);  // no reading: the identity whatever the curve says
        CHECK_EQ(el::TargetWeight(true, QuietNan()), 0.0f);
        CHECK_EQ(el::TargetWeight(true, 7.0f), 1.0f);
        // 63 % of a step in one second, at 60 and at 6 updates a second (the wall clock, not the update count)
        const float at60 = Run(0.0f, 1.0f, 1.0f, 60.0f);
        const float at10 = Run(0.0f, 1.0f, 1.0f, 10.0f);
        CHECK(Near(at60, 0.632f, 0.01f));
        CHECK(Near(at10, 0.632f, 0.01f));
        CHECK(Near(at60, at10, 0.002f));
        // four updates of dt/4 equal one of dt
        const float one = el::NextWeight(0.0f, 1.0f, 0.08f);
        float four = 0.0f;
        for (int i = 0; i < 4; ++i)
            four = el::NextWeight(four, 1.0f, 0.02f);
        CHECK(Near(one, four, 1e-5f));
        // a stalled step cannot flash the frame: dt 5 s moves the weight by one capped step (0.1 s), not to the target
        CHECK(el::NextWeight(0.0f, 1.0f, 5.0f) < 0.11f);
        CHECK_EQ(el::NextWeight(0.4f, 1.0f, 0.0f), 0.4f);
        CHECK_EQ(el::NextWeight(0.4f, 1.0f, QuietNan()), 0.4f);
        // the snap: a ramp out lands on exactly 0
        float w = 1.0f;
        for (int i = 0; i < 2000; ++i)
            w = el::NextWeight(w, 0.0f, 1.0f / 60.0f);
        CHECK_EQ(w, 0.0f);
        // a 3-frame misread of the target moves the weight by about 5 % at most
        float steady = 1.0f;
        for (int i = 0; i < 3; ++i)
            steady = el::NextWeight(steady, 0.0f, 1.0f / 60.0f);
        CHECK(steady > 0.94f);
    }

    // ---- the shared night curve on every capture on disk (toggle forced): day 0 exactly, night full -------------------------------------
    {
        for (const Capture& c : kCaptures)
        {
            const float target = el::NightTarget(ReadingOf(c));
            if (c.day)
                CHECK_EQ(target, 0.0f);
            CHECK(Near(target, c.expected, 0.01f));
        }
        CHECK_EQ(el::NightTarget(fs::NightReading {}), 0.0f);   // no reading
    }

    // ---- the exposure fingerprint: the real bytes of the captures -----------------------------------------------------------------------
    {
        // (s, y) of the 8 captures on disk (enginemap section 4)
        const float pairs[][2] = { { 1.4106f, 4.061f }, { 1.2090f, 2.983f }, { 0.9531f, 1.854f }, { 0.8116f, 1.344f }, { 0.6988f, 1.0f } };
        for (const auto& pair : pairs)
            CHECK(el::ExposureValid(pair[0], pair[1]));
        CHECK(!el::ExposureValid(1.4106f, 1.0f));          // the right s with another buffer's y
        CHECK(!el::ExposureValid(1.4106f, 5.0f));
        CHECK(!el::ExposureValid(QuietNan(), 1.0f));
        CHECK(!el::ExposureValid(1.0f, QuietNan()));
        CHECK(!el::ExposureValid(0.1f, 1.0f));             // out of the band
        CHECK(!el::ExposureValid(9.0f, el::YOf(9.0f)));
        CHECK(!el::ExposureValid(0.0f, 1.0f));
    }

    // ---- the compensation formula (M7) and its identities ------------------------------------------------------------------------------
    {
        // stored = sqrt(C) * s_now; the held picture is sqrt(C) * s_hold: gain = s_hold / s_now
        CHECK(Near(el::HoldGain(1.2f, 1.5f), 0.8f, 1e-6f));
        CHECK(Near(el::HoldGain(1.5f, 1.2f), 1.25f, 1e-6f));    // the engine lowered its exposure (lamps brighter): the hold brings it back up
        CHECK_EQ(el::HoldGain(1.4f, 1.4f), 1.0f);
        CHECK_EQ(el::HoldGain(0.1f, 1.4f), el::kGainMin);       // bounded: a scene cut is not a take-back
        CHECK_EQ(el::HoldGain(9.0f, 1.0f), el::kGainMax);
        CHECK_EQ(el::HoldGain(0.0f, 1.4f), 1.0f);
        CHECK_EQ(el::HoldGain(1.4f, 0.0f), 1.0f);

        el::Params p;                                           // defaults: mode auto
        // a full-weight night frame with a hold
        CHECK(Near(el::StoredGain(p, 1.0f, true, 1.2f, true, 1.5f), 0.8f, 1e-6f));
        // NOT weighted by the applied weight (the advisor): half weight, same gain
        CHECK(Near(el::StoredGain(p, 0.5f, true, 1.2f, true, 1.5f), 0.8f, 1e-6f));
        // identities
        CHECK_EQ(el::StoredGain(p, 0.0f, true, 1.2f, true, 1.5f), 1.0f);    // weight 0: a day frame
        CHECK_EQ(el::StoredGain(p, 1.0f, false, 0.0f, true, 1.5f), 1.0f);   // no hold yet
        CHECK_EQ(el::StoredGain(p, 1.0f, true, 1.2f, false, 1.5f), 1.0f);   // no valid reading this frame
        el::Params ab = p;
        ab.referenceAB = true;
        CHECK_EQ(el::StoredGain(ab, 1.0f, true, 1.2f, true, 1.5f), 1.0f);
        el::Params off = p;
        off.master = false;
        CHECK_EQ(el::StoredGain(off, 1.0f, true, 1.2f, true, 1.5f), 1.0f);
        el::Params mode0 = p;
        mode0.exposureMode = el::kExposureOff;
        CHECK_EQ(el::StoredGain(mode0, 1.0f, true, 1.2f, true, 1.5f), 1.0f);
        // manual: lerp with the weight, independent of s and of the hold
        el::Params manual = p;
        manual.exposureMode = el::kExposureManual;
        manual.exposureManual = 2.0f;
        CHECK(Near(el::StoredGain(manual, 1.0f, false, 0.0f, false, 0.0f), 2.0f, 1e-6f));
        CHECK(Near(el::StoredGain(manual, 0.5f, false, 0.0f, false, 0.0f), 1.5f, 1e-6f));
        CHECK_EQ(el::StoredGain(manual, 0.0f, false, 0.0f, false, 0.0f), 1.0f);
        CHECK(el::GainIsIdentity(1.0f) && !el::GainIsIdentity(0.99f));
    }

    // ---- the controller on a synthetic engine -------------------------------------------------------------------------------------------
    {
        // (1) day -> night step. The engine's identity exposure goes 0.7 -> 1.4 with a 4 s lag; the controls take back 35 %. The latch must be the SETTLED identity
        // exposure (about 1.4), never the day 0.7 (the advisor's blocker; mutant "freeze at the first weight > 0").
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sIdentity = 0.7f;
        sim.s = 0.7f;
        sim.curve = 0.0f;
        sim.Advance(20.0f);
        CHECK_EQ(sim.c.weight, 0.0f);
        CHECK(sim.c.phase == el::Phase::Idle);
        sim.sIdentity = 1.4f;
        sim.curve = 1.0f;
        sim.Advance(0.5f);
        CHECK(sim.c.phase == el::Phase::Settling);      // engaged, no settled identity reading: wait
        CHECK_EQ(sim.c.weight, 0.0f);                   // the applied (and published) weight stays 0 while the engine settles
        sim.Advance(40.0f);
        CHECK(sim.c.phase == el::Phase::Active);
        CHECK(sim.c.haveHold);
        CHECK(Near(sim.c.sHold, 1.4f, 0.05f));          // the settled identity exposure
        CHECK(sim.c.sHold > 1.2f);                      // never the day one (0.7)
        CHECK(!sim.c.forcedLatch);
        CHECK(Near(sim.c.weight, 1.0f, 0.01f));
        // the controls act: the engine raises its exposure by the take-back; the hold did NOT follow it (mutant M3), and the compensation brings the picture back
        CHECK(Near(sim.s, 1.4f * 1.35f, 0.03f));
        CHECK(Near(sim.c.sHold, 1.4f, 0.05f));
        const float gain = sim.c.Gain(true, sim.s);
        CHECK(Near(gain * sim.s, sim.c.sHold, 0.02f));  // stored * gain = the held exposure
        CHECK(gain < 0.8f && gain > 0.7f);
        // a 20 % brighter view: the engine lowers its exposure, the hold stays, the gain goes above 1
        sim.sIdentity = 1.4f / 1.6f;
        sim.Advance(20.0f);
        CHECK(Near(sim.c.sHold, 1.4f, 0.05f));
        CHECK(sim.c.Gain(true, sim.s) > 1.0f);
    }
    {
        // (2) the A/B release is a free re-latch, a short A/B leaves the hold alone
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sIdentity = 1.4f;
        sim.s = 1.4f;
        sim.curve = 1.0f;
        sim.Advance(40.0f);
        CHECK(sim.c.phase == el::Phase::Active && sim.c.haveHold);
        const float held = sim.c.sHold;
        // a 1 s A/B: the engine has barely started to relax, the hold must not take that value
        el::Params ab;
        ab.referenceAB = true;
        sim.c.SetParams(ab);
        sim.Advance(1.0f);
        CHECK(sim.c.sHold == held);
        CHECK(Near(el::StoredGain(sim.c.params, sim.c.weight, sim.c.haveHold, sim.c.sHold, true, sim.s), 1.0f, 0.0f));   // identity while the A/B shows the engine's values
        sim.c.SetParams(el::Params {});
        sim.Advance(0.5f);
        CHECK(sim.c.sHold == held);
        // the engine's identity exposure drifted (a cloud): a long A/B re-latches the new settled value
        sim.sIdentity = 1.1f;
        sim.c.SetParams(ab);
        sim.Advance(30.0f);
        CHECK(Near(sim.c.sHold, 1.1f, 0.04f));
        CHECK(sim.c.weight > 0.99f);                    // the weight stayed where it was: the release puts the look back at once
        sim.c.SetParams(el::Params {});
        CHECK(Near(sim.c.Gain(true, 1.1f), 1.0f, 0.04f));
    }
    {
        // (3) a gap (a loading screen, an alt-tab) forgets the hold: the controls fade out, settle again and return
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sIdentity = 1.4f;
        sim.s = 1.4f;
        sim.curve = 1.0f;
        sim.Advance(40.0f);
        CHECK(sim.c.haveHold);
        el::StepInput gap;
        gap.dt = 5.0f;
        gap.haveReading = true;
        gap.curve = 1.0f;
        gap.sFresh = true;
        gap.sValid = true;
        gap.s = 0.9f;                                    // the new zone is darker for the engine
        sim.c.Step(gap);
        CHECK(!sim.c.haveHold);
        CHECK(sim.c.phase == el::Phase::Settling);
        sim.sIdentity = 0.9f;
        sim.s = 0.9f * 1.35f;
        sim.Advance(60.0f);
        CHECK(sim.c.phase == el::Phase::Active && sim.c.haveHold);
        CHECK(Near(sim.c.sHold, 0.9f, 0.04f));
    }
    {
        // (4) the wait runs out: an engine that never calms (an oscillating scene) still gets its controls, with a logged "forced" latch
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.curve = 1.0f;
        sim.s = 1.0f;
        for (int i = 0; i < 60 * 30; ++i)
        {
            el::StepInput in;
            in.dt = 1.0f / 60.0f;
            in.haveReading = true;
            in.curve = 1.0f;
            in.sFresh = in.sValid = true;
            in.s = 1.0f + 0.3f * std::sin(float(i) * 0.05f);   // never calm
            sim.c.Step(in);
        }
        CHECK(sim.c.phase == el::Phase::Active && sim.c.haveHold && sim.c.forcedLatch);
        CHECK(sim.c.weight > 0.99f);
    }
    {
        // (5) no reading ever lands: fail open - the controls run, the hold is unavailable, the gain is exactly 1
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sReadings = false;
        sim.curve = 1.0f;
        sim.Advance(30.0f);
        CHECK(sim.c.phase == el::Phase::Active);
        CHECK(!sim.c.haveHold && sim.c.holdUnavailable);
        CHECK(sim.c.weight > 0.99f);
        CHECK_EQ(sim.c.Gain(false, 0.0f), 1.0f);
    }
    {
        // (6) the forced weight never waits
        Sim sim;
        el::Params p;
        p.forceNightWeight = true;
        sim.c.SetParams(p);
        sim.curve = 0.0f;                                // the curve says day; the pin says night
        sim.haveReading = false;
        sim.sIdentity = 1.4f;
        sim.s = 1.4f;
        sim.Advance(5.0f);
        CHECK(sim.c.phase == el::Phase::Active);
        CHECK(sim.c.weight > 0.98f);
    }
    {
        // (7) cb-only / lamps-only (exposure off): the weight reaches 1 at night with NO exposure reading at all (the "starved cell": the weight must not depend on the grade or the hold)
        Sim sim;
        el::Params p;
        p.exposureMode = el::kExposureOff;
        sim.c.SetParams(p);
        sim.sReadings = false;
        sim.curve = 1.0f;
        sim.Advance(6.0f);
        CHECK(sim.c.phase == el::Phase::Active);
        CHECK(Near(sim.c.weight, 1.0f, 0.01f));
        CHECK(el::Wanted(sim.c.params));
        // ... and back to exactly 0 by day
        sim.curve = 0.0f;
        sim.Advance(15.0f);
        CHECK_EQ(sim.c.weight, 0.0f);
        CHECK(sim.c.phase == el::Phase::Idle);
    }
    {
        // (8) the master off: weight 0 everywhere, the hold keeps tracking the engine (identity), and the master back on starts from a fresh latch
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sIdentity = 1.4f;
        sim.s = 1.4f;
        sim.curve = 1.0f;
        sim.Advance(40.0f);
        el::Params off;
        off.master = false;
        sim.c.SetParams(off);
        sim.Advance(15.0f);
        CHECK_EQ(sim.c.weight, 0.0f);
        CHECK(Near(sim.c.sHold, 1.4f, 0.04f));
        sim.c.SetParams(el::Params {});
        sim.Advance(0.2f);
        CHECK(sim.c.phase == el::Phase::Active);        // the latch matched the engine's identity exposure: no wait
    }
    {
        // (9) a dusk: the curve rises over three minutes while the engine's exposure rises with the sky. The controls engage only at kEngageAbove, and a day latch is
        // never used for the night (stale check): the hold is within 25 % of the final identity exposure and the gain stays inside its bounds.
        Sim sim;
        sim.c.SetParams(el::Params {});
        sim.sIdentity = 0.7f;
        sim.s = 0.7f;
        sim.Advance(10.0f);
        CHECK(sim.c.haveHold && Near(sim.c.sHold, 0.7f, 0.02f));   // a day latch exists
        float maxWeightBeforeEngage = 0.0f;
        for (int i = 0; i < 180 * 60; ++i)
        {
            const float f = float(i) / (180.0f * 60.0f);
            sim.curve = f;
            sim.sIdentity = 0.7f + 0.7f * f;
            if (f < el::kEngageAbove)
                maxWeightBeforeEngage = std::max(maxWeightBeforeEngage, sim.c.weight);
            sim.Advance(1.0f / 60.0f);
        }
        CHECK_EQ(maxWeightBeforeEngage, 0.0f);          // nothing acts in the early dusk
        sim.curve = 1.0f;
        sim.sIdentity = 1.4f;
        sim.Advance(30.0f);
        CHECK(sim.c.phase == el::Phase::Active);
        CHECK(sim.c.sHold > 1.05f);                      // never the day 0.7
        const float gain = sim.c.Gain(true, sim.s);
        CHECK(gain >= el::kGainMin && gain <= el::kGainMax);
    }
    {
        // (10) the auto hold switched on while the controls already act has nothing valid to hold: it settles again
        Sim sim;
        el::Params p;
        p.exposureMode = el::kExposureOff;
        sim.c.SetParams(p);
        sim.sIdentity = 1.4f;
        sim.s = 1.4f;
        sim.curve = 1.0f;
        sim.Advance(30.0f);
        CHECK(sim.c.weight > 0.99f && !sim.c.haveHold);
        p.exposureMode = el::kExposureAuto;
        sim.c.SetParams(p);
        sim.Advance(0.2f);
        CHECK(sim.c.phase == el::Phase::Settling);
        sim.Advance(60.0f);
        CHECK(sim.c.phase == el::Phase::Active && sim.c.haveHold);
        CHECK(Near(sim.c.sHold, 1.4f, 0.05f));
    }

    {
        // (11) a NOISY exposure (a few 1e-4 of wobble per frame at 144 Hz) and a STEPPED one (rewritten every 6th frame at 60 Hz): the windowed calm test sees both settled; a per-frame
        // rate test would reset on every wobble and run the whole wait into a forced latch. Same for the A/B re-latch.
        for (int variant = 0; variant < 2; ++variant)
        {
            Sim sim;
            sim.c.SetParams(el::Params {});
            sim.sIdentity = 0.7f;
            sim.s = 0.7f;
            sim.observed = 0.7f;
            sim.curve = 0.0f;
            if (variant == 0)
            {
                sim.dtStep = 1.0f / 144.0f;
                sim.noise = 0.0004f;
            }
            else
            {
                sim.dtStep = 1.0f / 60.0f;
                sim.holdFrames = 6;
            }
            sim.Advance(10.0f);
            sim.sIdentity = 1.4f;
            sim.curve = 1.0f;
            sim.Advance(40.0f);
            CHECK(sim.c.phase == el::Phase::Active && sim.c.haveHold);
            CHECK(!sim.c.forcedLatch);                     // settled, not timed out
            CHECK(Near(sim.c.sHold, 1.4f, 0.06f));
            // the A/B release is a free re-latch with the same wobble
            sim.sIdentity = 1.1f;
            el::Params ab;
            ab.referenceAB = true;
            sim.c.SetParams(ab);
            sim.Advance(40.0f);
            CHECK(!sim.c.forcedLatch);
            CHECK(Near(sim.c.sHold, 1.1f, 0.06f));
        }
    }

    // ---- the lines ---------------------------------------------------------------------------------------------------------------------
    {
        el::RunFacts f;
        f.weight = 0.5f;
        f.phase = el::Phase::Active;
        f.s = 1.41f;
        f.sHold = 1.2f;
        f.haveHold = true;
        f.gain = 0.85f;
        f.lamp = 2.0f;
        const std::string line = el::FormatRun(f);
        CHECK(line.find("active") != std::string::npos && line.find("lamps x2.00") != std::string::npos && line.find("compensation x0.8500") != std::string::npos);
        el::SecondFacts sec;
        sec.haveS = true;
        sec.s = 1.41f;
        sec.y = 4.06f;
        CHECK(el::FormatSecond(sec).find("s 1.4100 y 4.0600") != std::string::npos);
        CHECK(el::FormatLatch(1.4f, true).find("ran out") != std::string::npos);
        CHECK(el::RunLineTag(0.0f, el::Phase::Idle) != el::RunLineTag(1.0f, el::Phase::Active));
        el::Session session;
        for (uint32_t i = 0; i < ext::ffxiv::engine::kConsecutiveDeclineLimit - 1; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.NoteDecline() && !session.Armed());
    }

    return Finish("engine_light_rules");
}
