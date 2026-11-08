#pragma once
// Engine light controls (ENG2, 2026-11-07): the pure rules of the lamp / exposure / weight slice (no device, no config, no clock). The contract with the cb-patch
// family is EngineLightContract.h (Params, Effective, PublishCbFamily); this file adds what only this slice owns:
//   - the keys' ranges and clamps (the contract's ranges), the "does this configuration change anything" test (Wanted);
//   - the night weight: the shared NightCurve smoothed in WALL-CLOCK time (the retired night grade's rule, kept: the false-night guard, the 0.1 s step cap, the snap);
//   - the exposure hold: the engine scales the stored scene by its own exposure `s` (stored = sqrt(C) * s, g_CommonParameter byte 48), measures it AFTER that
//     (the luminance measure runs after the composite) and raises `s` when the lights are darkened, which would undo the darkening. The hold rescales the stored
//     scene at the tone map by s_hold / s_now, so the picture keeps the exposure the engine had before the controls acted;
//   - the Controller: one pure state machine that gates the controls until the engine's exposure has settled on its OWN light (the latch), then ramps them in.
//
// Why a gate, and not "freeze s at the first frame the controls act" (advisor, 2026-11-07): at night onset (a Weatherman jump to 00:00, a login at night, a dusk)
// the engine's `s` is still climbing towards its night value on its own time constant. Freezing it then would hold a DAY exposure and crush the night by 2-3x.
// So while the controls are wanted but no settled identity reading exists, the applied weight stays 0 (and is published as 0 to the cb family), `s` is watched until
// it has been stable (inside a 1.5 % band over kSettleSeconds), it is latched, and only then does the weight ramp. The same detector re-latches for free whenever the controls are at identity
// (master off, the engine / reference A/B, weight 0): releasing the A/B puts the hold on the engine's own settled exposure.

#include "ext/ffxiv/engine/EnginePassRules.h" // kConsecutiveDeclineLimit
#include "ext/ffxiv/engine/ShaftsRules.h"      // the tone map's exposure and the frame's white point (the meter's display terms)
#include "ext/ffxiv/enginelight/EngineLightContract.h"
#include "ext/ffxiv/shadows/ShadowsRules.h"   // NightCurve, NightReading: the ONE day/night rule

#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace ext::ffxiv::enginelight
{
// ---- the keys -------------------------------------------------------------------------------------------------------------------

inline constexpr float kMoonMin = 0.05f, kMoonMax = 1.0f;
inline constexpr float kTintMin = 0.5f, kTintMax = 1.5f;
inline constexpr float kAmbientMin = 0.1f, kAmbientMax = 1.0f;
inline constexpr float kWaterMoonMin = 0.05f, kWaterMoonMax = 1.0f;
inline constexpr float kWaterAmbientMin = 0.1f, kWaterAmbientMax = 1.0f;
inline constexpr float kLampMin = 1.0f, kLampMax = 5.0f;
inline constexpr float kManualMin = 0.25f, kManualMax = 4.0f;
inline constexpr int kExposureOff = 0, kExposureAuto = 1, kExposureManual = 2;

// A factor this close to 1 is the engine's own value (the arm does nothing and touches no state).
inline constexpr float kIdentityEpsilon = 1.0f / 1024.0f;

constexpr bool IsNan(float v) noexcept { return !(v == v); }
constexpr float Saturate(float x) noexcept { return IsNan(x) ? 0.0f : (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)); }
constexpr float Abs(float v) noexcept { return v < 0.0f ? -v : v; }

constexpr float ClampOr(float value, float lo, float hi, float fallback) noexcept
{
    if (IsNan(value))
        return fallback;
    return value < lo ? lo : (value > hi ? hi : value);
}

constexpr int ClampMode(int v) noexcept { return v < kExposureOff ? kExposureAuto : (v > kExposureManual ? kExposureAuto : v); }

// Every field inside the contract's range, NaN -> the field's default. The flags pass through.
constexpr Params Sanitize(const Params& in) noexcept
{
    const Params d;
    Params p = in;
    p.moonIntensity = ClampOr(in.moonIntensity, kMoonMin, kMoonMax, d.moonIntensity);
    p.ambientIntensity = ClampOr(in.ambientIntensity, kAmbientMin, kAmbientMax, d.ambientIntensity);
    p.waterMoon = ClampOr(in.waterMoon, kWaterMoonMin, kWaterMoonMax, d.waterMoon);
    p.waterAmbient = ClampOr(in.waterAmbient, kWaterAmbientMin, kWaterAmbientMax, d.waterAmbient);
    p.lampIntensity = ClampOr(in.lampIntensity, kLampMin, kLampMax, d.lampIntensity);
    p.exposureManual = ClampOr(in.exposureManual, kManualMin, kManualMax, d.exposureManual);
    p.exposureMode = ClampMode(in.exposureMode);
    for (int i = 0; i < 3; ++i)
    {
        p.moonTint[i] = ClampOr(in.moonTint[i], kTintMin, kTintMax, 1.0f);
        p.ambientTint[i] = ClampOr(in.ambientTint[i], kTintMin, kTintMax, 1.0f);
        p.waterTint[i] = ClampOr(in.waterTint[i], kTintMin, kTintMax, 1.0f);
    }
    return p;
}

constexpr bool NearOne(float v) noexcept { return Abs(v - 1.0f) < kIdentityEpsilon; }
constexpr bool TintIdentity(const float t[3]) noexcept { return NearOne(t[0]) && NearOne(t[1]) && NearOne(t[2]); }

// Whether any LIGHT arm (everything but the exposure) would change the engine's picture at full weight.
constexpr bool AnyLightArm(const Params& p) noexcept
{
    return !NearOne(p.moonIntensity) || !TintIdentity(p.moonTint) || !NearOne(p.ambientIntensity) || !TintIdentity(p.ambientTint) || !NearOne(p.waterMoon) ||
           !NearOne(p.waterAmbient) || !TintIdentity(p.waterTint) || !NearOne(p.lampIntensity);
}

// The module is wanted (armed, its feed asked for, its seam entered) while the master is on and something would change: a light arm, or the manual exposure
// factor. The auto hold on its own does nothing without a darkened light, and the exposure mode off adds nothing: so lamps-only and cb-only configurations
// are armed too (the feed is not tied to the exposure stage).
constexpr bool Wanted(const Params& p) noexcept
{
    return p.master && (AnyLightArm(p) || (p.exposureMode == kExposureManual && !NearOne(p.exposureManual)));
}

// The tone-map stage has work only with the auto hold (needs a light arm to hold against) or the manual factor.
constexpr bool StageWanted(const Params& p) noexcept
{
    if (!p.master)
        return false;
    return (p.exposureMode == kExposureAuto && AnyLightArm(p)) || (p.exposureMode == kExposureManual && !NearOne(p.exposureManual));
}

constexpr bool ParamsEqual(const Params& a, const Params& b) noexcept
{
    if (a.master != b.master || a.referenceAB != b.referenceAB || a.exposureMode != b.exposureMode || a.forceNightWeight != b.forceNightWeight)
        return false;
    if (a.moonIntensity != b.moonIntensity || a.ambientIntensity != b.ambientIntensity || a.waterMoon != b.waterMoon || a.waterAmbient != b.waterAmbient ||
        a.lampIntensity != b.lampIntensity || a.exposureManual != b.exposureManual)
        return false;
    for (int i = 0; i < 3; ++i)
        if (a.moonTint[i] != b.moonTint[i] || a.ambientTint[i] != b.ambientTint[i] || a.waterTint[i] != b.waterTint[i])
            return false;
    return true;
}

// The lamp arm's factor at the applied weight: the contract's Effective (1 for master off and for the A/B). 1 exactly (not "close") means "touch nothing".
constexpr float LampFactor(const Params& p, float weight) noexcept { return Effective(p, p.lampIntensity, Saturate(weight)); }
constexpr bool LampActive(const Params& p, float weight) noexcept { return !NearOne(LampFactor(p, weight)); }

// ---- the weight -------------------------------------------------------------------------------------------------------------------

inline constexpr float kWeightTauSeconds = 1.0f;     // 63 % of a step in one second of wall-clock
inline constexpr float kMaxStepSeconds = 0.1f;       // one update moves at most this much clock: a stall followed by one misread cannot flash the frame
inline constexpr float kWeightSnap = 1.0f / 1024.0f; // inside this of 0 (or 1) the weight IS 0 (or 1)

// The curve's answer as a target: NaN and out-of-range are clamped, and "no reading yet" is 0 (the identity), whatever the curve says.
constexpr float TargetWeight(bool haveReading, float curve) noexcept { return haveReading ? Saturate(curve) : 0.0f; }

// The target from the night curve's own reading, with the curve's toggle FORCED TRUE (the shadow card's "Key on the visible sun" box pins the curve at 1 for that
// module's own A/B; honouring it here would darken a whole daylight frame). No fog sun and no colour is "no reading": weight 0.
constexpr float NightTarget(const shadows::NightReading& reading) noexcept
{
    const shadows::NightCurveResult result = shadows::NightCurve(true, reading);
    return TargetWeight(result.source != shadows::NightSource::None, result.curve);
}

// One update of the smoothed weight. `dt` is the wall-clock seconds since the previous update (clamped to [0, kMaxStepSeconds], NaN = 0). The same total clock gives
// the same weight however it is cut into updates, which is what makes the smoothing a function of wall-clock time and not of the update count. A weight inside
// kWeightSnap of the target lands on it; the snap at 0 is what returns a day frame to the engine's own picture.
inline float NextWeight(float previous, float target, float dt) noexcept
{
    const float from = Saturate(previous);
    const float to = Saturate(target);
    const float step = !(dt == dt) || dt < 0.0f ? 0.0f : (dt > kMaxStepSeconds ? kMaxStepSeconds : dt);
    if (!(step > 0.0f))
        return from;
    const float next = to + (from - to) * std::exp(-step / kWeightTauSeconds);
    if (std::fabs(next - to) < kWeightSnap)
        return to;
    return next;
}

// The night is "engaged" (the controls may start) when the target reaches kEngageAbove and stays engaged until it falls under kEngageBelow. The hysteresis keeps
// the settle-and-latch from starting in the middle of a dusk, when the engine's own exposure is still moving with the sky; the arms then fade in over the weight's
// own second. The forced weight (benchmark) is always engaged.
inline constexpr float kEngageAbove = 0.8f;
inline constexpr float kEngageBelow = 0.3f;

// ---- the exposure ------------------------------------------------------------------------------------------------------------------

// The scene-pass g_CommonParameter (64 B): m_Misc2 at byte 48 = (s, y, FLT_MAX, 0). `s` is the engine's exposure, y = max(1, 2.04 s^2) a function of it: that pair is the
// fingerprint that the bytes read are the right buffer (nightlight / enginemap: 8 captures, s 0.6988 .. 1.4106). The GPU copy (ExposureHold.hlsl) applies the same test.
inline constexpr uint32_t kCommonParameterBytes = 64;
inline constexpr uint32_t kMisc2Offset = 48;
inline constexpr float kYFactor = 2.04f;
inline constexpr float kSMinValid = 0.25f, kSMaxValid = 6.0f;
inline constexpr float kYRelTolerance = 0.02f;
inline constexpr float kYAbsTolerance = 0.01f;

constexpr float YOf(float s) noexcept { const float y = kYFactor * s * s; return y < 1.0f ? 1.0f : y; }

constexpr bool ExposureValid(float s, float y) noexcept
{
    if (IsNan(s) || IsNan(y) || !(s >= kSMinValid) || !(s <= kSMaxValid))
        return false;
    const float expected = YOf(s);
    return Abs(y - expected) <= kYRelTolerance * expected + kYAbsTolerance;
}

// The compensation is bounded: it undoes what the controls made the engine's loop do, which the bracket puts at x1.05 .. x1.55 (enginemap section 4); a reading far
// outside is a scene cut or a wrong buffer, not a take-back.
inline constexpr float kGainMin = 0.5f, kGainMax = 2.0f;

constexpr float HoldGain(float sHold, float sNow) noexcept
{
    if (!(sHold > 0.0f) || !(sNow > 0.0f))
        return 1.0f;
    const float g = sHold / sNow;
    return g < kGainMin ? kGainMin : (g > kGainMax ? kGainMax : g);
}

// The stage's multiplier on the STORED scene at the tone map (stored' = stored * gain):
//   mode 0 (off), the master off, the engine / reference A/B, an applied weight of 0: exactly 1 (the stage runs no pass);
//   mode 1 (auto hold): stored = sqrt(C) * s_now, the held picture is sqrt(C) * s_hold, so gain = s_hold / s_now (bounded), 1 until a hold and a valid reading exist.
//     NOT weighted by the applied weight: the gain is already a measurement of what the engine did, weighting it again would under-compensate during the ramp;
//   mode 2 (manual): lerp(1, factor, weight), the user's factor eased with the night weight like every other arm.
constexpr float StoredGain(const Params& p, float weight, bool haveHold, float sHold, bool sValid, float sNow) noexcept
{
    if (!p.master || p.referenceAB || !(weight > 0.0f))
        return 1.0f;
    if (p.exposureMode == kExposureManual)
        return Effective(p, p.exposureManual, Saturate(weight));
    if (p.exposureMode == kExposureAuto)
        return (haveHold && sValid) ? HoldGain(sHold, sNow) : 1.0f;
    return 1.0f;
}

constexpr bool GainIsIdentity(float gain) noexcept { return !(gain < 1.0f - kIdentityEpsilon) && !(gain > 1.0f + kIdentityEpsilon); }

// ---- the controller: settle, latch, ramp -------------------------------------------------------------------------------------------

// "Calm" is a WINDOWED test, not a per-frame rate: over the last kSettleSeconds of observed reads the exposure stayed inside a band of kCalmBand (in ln s, 1.5 %). A per-frame
// rate (|d ln s / dt| under a limit) is defeated by any frame-to-frame wobble of a few 1e-4 at 60-144 fps, and by an exposure that is only rewritten every few frames; the
// window sees neither. The window is kept as kBuckets buckets of kBucketSeconds, each holding the min and max of ln s.
inline constexpr float kCalmBand = 0.015f;        // max(ln s) - min(ln s) over the window under this is calm
inline constexpr float kSettleSeconds = 3.0f;     // the window length: calm for this long is settled
inline constexpr float kBucketSeconds = 0.25f;
inline constexpr int kBuckets = 12;               // kSettleSeconds / kBucketSeconds
inline constexpr float kSettleMaxSeconds = 20.0f; // the longest the controls wait for a settle before latching what there is (or failing open)
inline constexpr float kGapSeconds = 2.0f;        // a step this long is a loading screen / a stall / an alt-tab: the exposure moved under us, settle again
inline constexpr float kReadStepCap = 0.25f;      // the longest one read counts for in the calm timer
inline constexpr float kHoldBreak = 0.6f;        // |ln(s_now / s_hold)| over this while the controls act (a factor of 1.8) is a scene cut, not a take-back (the bracket tops out at 1.55): the latch is stale, settle again

enum class Phase : uint8_t
{
    Idle = 0,     // the controls are not wanted now (day, master off, nothing to do): the weight is 0
    Settling = 1, // wanted, no settled identity reading yet: the applied weight stays 0 until the exposure is calm (or the wait runs out)
    Active = 2,   // the controls act at the smoothed weight
};

constexpr const char* PhaseName(Phase phase) noexcept
{
    switch (phase)
    {
    case Phase::Idle: return "idle";
    case Phase::Settling: return "settling";
    case Phase::Active: return "active";
    }
    return "unknown";
}

struct StepInput
{
    float dt = 0.0f;           // wall-clock seconds since the previous step, NOT clamped (NaN = 0)
    bool haveReading = false;  // the night curve has an input
    float curve = 0.0f;        // its answer (the shared NightCurve with the toggle forced)
    bool sFresh = false;       // a NEW landed read of the scene exposure came with this step
    bool sValid = false;       // ... and it passed ExposureValid
    float s = 0.0f;            // the read (only meaningful with sFresh && sValid)
};

struct Controller
{
    Params params;           // the sanitized keys of this step
    float weight = 0.0f;     // the applied weight: smoothed, gated; what the lamps, the compensation and the cb family use
    float rawTarget = 0.0f;  // the night curve's target before the gate
    float target = 0.0f;     // the gated target the weight heads for
    bool engaged = false;
    bool prevEngaged = false;
    Phase phase = Phase::Idle;
    bool haveHold = false;
    float sHold = 0.0f;
    bool haveLastS = false;
    float lastS = 0.0f;
    float obsTime = 0.0f;    // observed seconds since the window was reset
    bool windowIdentity = true;      // whether the reads in the window were taken with the controls at identity (a window never mixes the two)
    int32_t bucketId[kBuckets] {};   // which absolute bucket each slot holds (-1 = none)
    float bucketMin[kBuckets] {};
    float bucketMax[kBuckets] {};
    float waited = 0.0f;     // seconds in Settling
    bool forcedLatch = false;   // the last latch was the max-wait one, not a settled one
    bool holdUnavailable = false; // the wait ran out with no valid reading: the controls run without a hold (gain 1)
    bool prevNeedHold = false;
    uint32_t latches = 0;

    bool NeedHold() const noexcept { return params.exposureMode == kExposureAuto && AnyLightArm(params); }
    // True while the arms have no effect on the picture the engine measures.
    bool IdentityNow() const noexcept { return !params.master || params.referenceAB || !Wanted(params) || !(weight > 0.0f); }

    void SetParams(const Params& p) noexcept { params = Sanitize(p); }

    void ResetWindow() noexcept
    {
        obsTime = 0.0f;
        for (int i = 0; i < kBuckets; ++i)
        {
            bucketId[i] = -1;
            bucketMin[i] = bucketMax[i] = 0.0f;
        }
    }

    // One observed read: the ln of the exposure goes into the bucket the observed time falls in.
    void Observe(float lnS, float dtObs) noexcept
    {
        obsTime += dtObs;
        const int32_t id = int32_t(obsTime / kBucketSeconds);
        const int slot = id % kBuckets;
        if (bucketId[slot] != id)
        {
            bucketId[slot] = id;
            bucketMin[slot] = bucketMax[slot] = lnS;
        }
        else
        {
            bucketMin[slot] = lnS < bucketMin[slot] ? lnS : bucketMin[slot];
            bucketMax[slot] = lnS > bucketMax[slot] ? lnS : bucketMax[slot];
        }
    }

    // Settled: the window has been observed for its whole length and the band over it is inside kCalmBand.
    bool Calm() const noexcept
    {
        if (obsTime < kSettleSeconds)
            return false;
        const int32_t current = int32_t(obsTime / kBucketSeconds);
        float lo = 0.0f, hi = 0.0f;
        bool any = false;
        for (int32_t id = current - kBuckets + 1; id <= current; ++id)
        {
            if (id < 0)
                continue;
            const int slot = id % kBuckets;
            if (bucketId[slot] != id)
                continue;
            if (!any)
            {
                lo = bucketMin[slot];
                hi = bucketMax[slot];
                any = true;
            }
            else
            {
                lo = bucketMin[slot] < lo ? bucketMin[slot] : lo;
                hi = bucketMax[slot] > hi ? bucketMax[slot] : hi;
            }
        }
        return any && (hi - lo) <= kCalmBand;
    }

    Controller() noexcept { ResetWindow(); }

    void ForgetHold() noexcept
    {
        haveHold = false;
        sHold = 0.0f;
        haveLastS = false;
        ResetWindow();
        waited = 0.0f;
        holdUnavailable = false;
    }

    void Step(const StepInput& in) noexcept
    {
        const float rawDt = (IsNan(in.dt) || in.dt < 0.0f) ? 0.0f : in.dt;
        if (rawDt > kGapSeconds)
        {
            // The exposure moved while nobody watched: the hold and the calm timer are stale. The controls wait for the next settled identity reading.
            ForgetHold();
        }
        const bool force = params.forceNightWeight;
        rawTarget = force ? 1.0f : TargetWeight(in.haveReading, in.curve);
        if (force)
            engaged = true;
        else if (!engaged && rawTarget >= kEngageAbove)
            engaged = true;
        else if (engaged && rawTarget < kEngageBelow)
            engaged = false;

        // The night engaging (the rising edge) means the sky changed under the engine: any latch taken before it was taken in other light, whatever the exposure
        // reads at this very frame (it lags the sky). It is forgotten, and the controls wait for a settled reading in the new light.
        if (engaged && !prevEngaged)
            ForgetHold();
        prevEngaged = engaged;

        const bool needHold = NeedHold();
        if (needHold && !prevNeedHold && weight > 0.0f)
            ForgetHold(); // the auto hold switched on while the controls already act: the engine's exposure carries their take-back, there is nothing valid to hold
        prevNeedHold = needHold;

        // The calm timer and the latch watch every fresh read.
        if (in.sFresh && in.sValid)
        {
            const float dtObs = rawDt > kReadStepCap ? kReadStepCap : rawDt;
            // A latch must be the engine's exposure on its OWN light: the window holds identity reads only. When the controls start or stop acting (an A/B press or release, the weight
            // reaching 0, the master) the engine's exposure is about to move, and the steady reads from before say nothing about where it settles: the window starts again.
            const bool identityForWindow = IdentityNow();
            if (identityForWindow != windowIdentity)
            {
                ResetWindow();
                windowIdentity = identityForWindow;
            }
            if (in.s > 0.0f)
                Observe(std::log(in.s), dtObs);
            lastS = in.s;
            haveLastS = true;
            // A reading far from the latch while the controls act is a scene cut (a zone change that did not stall the frame clock): forget it, settle again.
            if (haveHold && !IdentityNow() && sHold > 0.0f && std::fabs(std::log(in.s / sHold)) > kHoldBreak)
            {
                haveHold = false;
                sHold = 0.0f;
                ResetWindow();
                waited = 0.0f;
                holdUnavailable = false;
            }
            if (needHold && IdentityNow() && Calm())
            {
                sHold = in.s; // a settled identity reading: the engine's own exposure for this light
                haveHold = true;
                forcedLatch = false;
                holdUnavailable = false;
                ++latches;
            }
        }

        const bool wantArms = Wanted(params) && engaged;
        if (!wantArms)
        {
            phase = Phase::Idle;
            waited = 0.0f;
        }
        else if (needHold && !haveHold && !holdUnavailable && !force)
        {
            phase = Phase::Settling;
            waited += rawDt;
            if (waited >= kSettleMaxSeconds)
            {
                if (haveLastS)
                {
                    sHold = lastS; // the controls waited long enough: hold what the engine has (documented, and logged)
                    haveHold = true;
                    forcedLatch = true;
                    ++latches;
                }
                else
                    holdUnavailable = true; // no usable reading at all: fail open, the controls run with gain 1
                phase = Phase::Active;
                waited = 0.0f;
            }
        }
        else
        {
            if (force && needHold && !haveHold && haveLastS)
            {
                sHold = lastS; // the forced weight (benchmark) never waits: it bootstraps from the first reading
                haveHold = true;
                forcedLatch = true;
                ++latches;
            }
            phase = Phase::Active;
            waited = 0.0f;
        }

        target = (wantArms && phase == Phase::Active) ? rawTarget : 0.0f;
        weight = NextWeight(weight, target, rawDt);
    }

    float Gain(bool sValid, float sNow) const noexcept { return StoredGain(params, weight, haveHold, sHold, sValid, sNow); }
};

// ---- the frame's white point -------------------------------------------------------------------------------------------------------
// The hold itself needs no white point (it scales the stored scene by a gain). The effect meter measures a stage's change in display terms, and the F9 stage line names the
// frame's white, so the stage reads the tone map's own exposure the way the other stages do (ShaftsSun.h ToneReader on its parameter buffer, a frame late).
struct WhiteReading
{
    bool usable = false;
    float white = shafts::kFallbackWhitePoint;
};

constexpr WhiteReading ReadWhite(bool landed, float exposure) noexcept
{
    WhiteReading out;
    if (!landed || !shafts::ExposureUsable(exposure))
        return out;
    const float white = shafts::WhitePointFromExposure(exposure);
    if (!shafts::WhitePointUsable(white))
        return out;
    out.usable = true;
    out.white = white;
    return out;
}

// ---- the stage's declines, the stand-down streak and the log lines -----------------------------------------------------------------

enum class Decline : uint8_t
{
    None = 0,
    NotToneMap,   // the draw is not the tone map's shape
    RenderTarget, // not one post-tone texture
    TargetFormat, // the tone map's target is not FP16
    SceneFormat,  // the scene is not FP16
    SizeMismatch, // the scene and the target are not one size
    Window,       // the render window is empty, too small or does not fit the scene
    CharaReplay,  // the draw is a character-preview replay
    SceneHdr,     // the scene HDR path replaces the tone chain this stage draws before
    PassFailed,   // the pass could not build its textures or run its draw
    Exception,    // a call threw
};
inline constexpr uint32_t kDeclineReasonCount = 10;

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene and the target are not one size";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::SceneHdr: return "the engine's scene HDR path is on, which replaces this stage's tone chain";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draw";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

enum class Outcome : uint8_t
{
    HoldStands = 0, // the chain's hold stands (chained role)
    EngineOwn,      // the frame is the engine's own (owner role)
};

constexpr const char* OutcomeName(Outcome outcome) noexcept
{
    return outcome == Outcome::HoldStands ? "the chain's hold stands" : "the frame is the engine's own";
}

// The decline that counts toward the stand-down: a shape the stage cannot serve, never "not the tone map" and never the frame's own skip reasons.
constexpr bool CountsTowardsStandDown(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::RenderTarget:
    case Decline::TargetFormat:
    case Decline::SceneFormat:
    case Decline::SizeMismatch:
    case Decline::Window:
    case Decline::PassFailed:
        return true;
    default:
        return false;
    }
}

// This stage's own refusal streak: kConsecutiveDeclineLimit counted declines in a row stand the STAGE down for the session (the lamps and the cb family go on),
// one served frame starts the count again.
struct Session
{
    uint32_t declines = 0;
    bool off = false;

    bool NoteDecline() noexcept
    {
        if (off)
            return false;
        if (++declines >= engine::kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }
    void NoteRun() noexcept { declines = 0; }
    bool Armed() const noexcept { return !off; }
};

inline std::string FormatDecline(Decline reason, Outcome outcome)
{
    return std::format("Engine light: the exposure stage left the draw alone ({}; {})", DeclineName(reason), OutcomeName(outcome));
}

inline std::string FormatSwitchedOff(uint32_t limit)
{
    return std::format("Engine light: the exposure stage is switched off for this session after {} declines in a row (the lamps and the light arms go on)", limit);
}

// What the F9 stage line carries besides the frame's white point and fade (those are the shared frame words, one source per word).
struct StageFacts
{
    Phase phase = Phase::Idle;
    float weight = 0.0f;
    float rawTarget = 0.0f;
    const char* arm = "none";
    int mode = kExposureAuto;
    bool haveS = false;
    float s = 0.0f;
    float sHold = 0.0f;
    float gain = 1.0f;
    float lamp = 1.0f;
};

inline std::string FormatStageWords(const StageFacts& f)
{
    std::string arm = f.arm != nullptr ? f.arm : "none";
    for (char& c : arm)
        if (c == ' ')
            c = '-'; // the line is name=value words: no spaces inside a value
    return std::format("exposure-hold phase={} weight={:.3f} curve={:.3f} arm={} mode={} s={:.4f} hold={:.4f} gain={:.4f} lamps={:.3f}", PhaseName(f.phase), double(f.weight),
                       double(f.rawTarget), arm, f.mode, double(f.haveS ? f.s : 0.0f), double(f.sHold), double(f.gain), double(f.lamp));
}

// The stage line's decline reason at a capture frame where the hold had no work: a night F9 that misreads says so (and in which phase) instead of "not reached this frame".
inline std::string FormatNoWorkReason(Phase phase, float weight, int mode)
{
    return std::format("exposure hold: no work ({}, weight {:.3f}, mode {}): the engine's own picture, no pass runs", PhaseName(phase), double(Saturate(weight)), mode);
}

// A weight in 1/16 steps: the log gate's tag, so a wander inside a step writes nothing.
constexpr uint32_t WeightStep(float weight) noexcept { return uint32_t(Saturate(weight) * 16.0f + 0.5f); }
constexpr uint32_t RunLineTag(float weight, Phase phase) noexcept { return 0x40000020u | (WeightStep(weight) << 8) | (uint32_t(phase) << 16); }

struct RunFacts
{
    float weight = 0.0f;
    float rawTarget = 0.0f;
    const char* arm = "none";
    Phase phase = Phase::Idle;
    int mode = kExposureAuto;
    float s = 0.0f;       // the newest valid read of the scene exposure (0 = none yet)
    float sHold = 0.0f;
    bool haveHold = false;
    float gain = 1.0f;
    float lamp = 1.0f;
    bool haveGpuMs = false;
    double gpuMs = 0.0;
};

// The line a session gets per step of the weight / change of phase: where the controls are and what the hold does.
inline std::string FormatRun(const RunFacts& f)
{
    std::string cost = f.haveGpuMs ? std::format("{:.3f} ms GPU", f.gpuMs) : std::string("GPU time not read yet");
    const std::string hold = f.haveHold ? std::format("{:.4f}", double(f.sHold)) : std::string("none yet");
    return std::format("Engine light: {} - weight {:.3f} (night curve {:.3f}, {}), lamps x{:.2f}, exposure mode {}, s {:.4f}, hold {}, compensation x{:.4f}, {}", PhaseName(f.phase),
                       double(f.weight), double(f.rawTarget), f.arm, double(f.lamp), f.mode, double(f.s), hold, double(f.gain), cost);
}

// The once-a-second measurement line: the exposure the engine used (s, y), the GPU 1x1 table sample, what the hold latched and the compensation. This is the
// series that measures the engine's real loop response in the next session.
struct SecondFacts
{
    bool haveS = false;
    float s = 0.0f;
    float y = 0.0f;
    bool haveTable = false;
    float tableLow = 0.0f, tableMid = 0.0f, tableHigh = 0.0f; // the 1024x1 table at 1/8, 1/2 and 7/8 (luminance rebalance, a function of the GPU 1x1)
    bool haveHold = false;
    float sHold = 0.0f;
    float gain = 1.0f;
    float weight = 0.0f;
    Phase phase = Phase::Idle;
};

inline std::string FormatSecond(const SecondFacts& f)
{
    const std::string s = f.haveS ? std::format("s {:.4f} y {:.4f}", double(f.s), double(f.y)) : std::string("s not read yet");
    const std::string table = f.haveTable ? std::format("table[128,512,896] {:.4f} {:.4f} {:.4f}", double(f.tableLow), double(f.tableMid), double(f.tableHigh))
                                          : std::string("table not sampled yet");
    const std::string hold = f.haveHold ? std::format("{:.4f}", double(f.sHold)) : std::string("none");
    return std::format("Engine light [1 s]: {}, {}, hold {}, compensation x{:.4f}, weight {:.3f} ({})", s, table, hold, double(f.gain), double(f.weight), PhaseName(f.phase));
}

inline std::string FormatNoFeed()
{
    return "Engine light: the night curve has no reading yet (no fog sun and no light colour), so the controls stay at weight 0; it needs the Shadow mapping module's light-colour feed";
}

inline std::string FormatLatch(float sHold, bool forced)
{
    return std::format("Engine light: exposure latched at s {:.4f} ({})", double(sHold),
                       forced ? "the settle wait ran out: the engine's exposure was still moving, or the controls were already acting" : "the engine's own exposure had settled");
}

inline constexpr const char* kSceneHdrInactive = "Engine light: inactive while scene HDR is on, because that path replaces the tone chain and the light draws this module patches";
} // namespace ext::ffxiv::enginelight
