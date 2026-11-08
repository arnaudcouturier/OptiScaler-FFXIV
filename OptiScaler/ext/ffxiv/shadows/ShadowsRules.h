#pragma once
// Shadow mapping (ext/ffxiv/shadows): the pure rules. No pch, no Windows, no D3D call, so every decision here is
// covered by tests/ext/ffxiv/shadows on the host - the house style of EnginePassRules.h and ContactShadowsRules.h.
//
// What the module does, in one line: it retunes the game's own deferred light at the two seams the fork already
// reads - the R8 sun/shadow mask the sun light draw samples (the mask lift, patch A) and the deferred composite's
// ambient probe table (the night ambient lift, patch B) - keyed by a day/night curve the frame's own light colour
// gives. Zero upstream lines: the mask path rides the contact-shadows draw seam (ext/ffxiv/relight), the ambient
// path appends two entries into the engine pass overrides module's ONE CbPatch dispatch on the composite draw.
//
// The two levers, from the SHADOWS design report (section 2 is the evidence, section 4 is the design; the ledger's the design report file, never named in product comments):
//
//  - the sun light draw squares the mask it samples (asm 649 of 0x9DC59665: `mul r4.xyw, r12.xxxx, r4.xyxw` with
//    r12.x = shadow^2), so the remap is defined on the SQUARED mask: `m'^2 = m^2 + (1-(m^2))*LiftEff`, and the copy
//    the draw binds carries `sqrt(m'^2)` so the shader's own square lands back on `m'^2`. The knob then reads
//    linearly in the picture: a shadowed texel's term moves by about `2*(1-m^2)*LiftEff*f(N,L)*m_DiffuseColor`, and
//    LiftEff = 0 is byte-exact - the game's own mask byte goes through unchanged, the softness key alone does
//    nothing without a lift (it feathers the lift).
//  - the deferred composite reads the header's `skyShRecipScale` (the global sky-SH scale, the night's dominant
//    ambient term; buffer byte 4 of the 64-byte header) and every used probe's `shRecipScale` (the float4 at byte
//    112 + 192k, `.w`), and multiplies each with `.y` / `.w` of the float4 it sits in - so the two entries below
//    are multiplier vectors with exactly one non-1 component each, and every other byte of the table passes through
//    the CbScale dispatch bit for bit.
//
// Composition rules (the design report 4.5, resolved the evidence-based way; the landing report carries the how):
//  - with contact shadows armed, the mask lift runs as a stage of contact's own combine (one copy of the mask per
//    frame, per light draw already paid: zero extra copies, one extra bind... none - the values travel through the
//    sink hooks below and the combine applies the same remap math before its term multiply). With contact off, the
//    shadows module runs its own one-copy, one-draw remap and the identical bind/restore.
//  - with AmbientSkyVisibilityScale armed, the shadows' entries go into the SAME dispatch: CbScale.hlsl applies
//    every matching entry in order, multiplicatively (the loop multiplies a component by each entry that addresses
//    its float4), so the shipped knob's scale and the night lift compose on the `112+192k` `.w` field and the
//    header's `.y` field is untouched by the shipped knob.
//
// The day/night curve scales both patches' keys. Its thresholds are PROVISIONAL: the five-stop F9 capture protocol
// of the design report 5 (night exterior, night interior, the same exterior in day, one 3-minute dawn window,
// one screenshot per stop; U-c reads cb.csv's colour sweep) settles them in a session, and nothing final is
// invented here. The game's own `g_LightParam.m_DiffuseColor` is the frame's day/night parameter (cb.csv rows:
// night 0.078/0.117/0.16 and 0.176/0.264/0.49; day 1.96/1.49/1.25 - the sweep IS the curve).

#include "ext/ffxiv/engine/EnginePassRules.h"     // ScaleEntry, the ambient entry shapes, the table sizes
#include "ext/ffxiv/relight/ContactShadowsRules.h" // the light-draw classification and its drop list, reused verbatim

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::shadows
{
// ---- the tuning keys ------------------------------------------------------------------------------------------------------

// [FFXIV] ShadowLift: the shadow floor of the sun/shadow mask. 0 = the engine's own mask (byte-exact); the 0.9
// ceiling covers the demo's ~2-4x shadow brightness without ever flattening the mask entirely.
inline constexpr float kMinLift = 0.0f;
inline constexpr float kMaxLift = 0.9f;
inline constexpr float kDefaultLift = 0.0f;

// [FFXIV] ShadowEdgeSoftness: the tent's reach over the lifted mask, in mask pixels (a reach of at most one pixel
// reads a 3x3 kernel, two a 5x5 one; the weight falls to zero at the reach). 0 = the mask's own edges.
inline constexpr float kMinSoftness = 0.0f;
inline constexpr float kMaxSoftness = 2.0f;
inline constexpr float kDefaultSoftness = 0.0f;

// [FFXIV] NightAmbientLift: the multiplier on the two ambient scale fields, before the curve. 1.0 = the engine's
// own picture; 4 covers the demo's night floors.
inline constexpr float kMinAmbientLift = 1.0f;
inline constexpr float kMaxAmbientLift = 4.0f;
inline constexpr float kDefaultAmbientLift = 1.0f;

constexpr float ClampLift(float value) noexcept
{
    if (value != value)
        return kDefaultLift; // NaN reads as off, like the engine knobs' identity tests
    if (value < kMinLift)
        return kMinLift;
    return value > kMaxLift ? kMaxLift : value;
}

constexpr float ClampSoftness(float value) noexcept
{
    if (value != value)
        return kDefaultSoftness;
    if (value < kMinSoftness)
        return kMinSoftness;
    return value > kMaxSoftness ? kMaxSoftness : value;
}

constexpr float ClampAmbientLift(float value) noexcept
{
    if (value != value)
        return kDefaultAmbientLift;
    if (value < kMinAmbientLift)
        return kMinAmbientLift;
    return value > kMaxAmbientLift ? kMaxAmbientLift : value;
}

// The identity threshold carries the engine knob's epsilon: a thousandth away from the idle value is not a look
// anyone can see, and running the pass for it would cost a copy and a draw to change nothing.
inline constexpr float kIdentityEpsilon = 0.001f;

// Lift 0 (or below the epsilon) is the game's own mask, always - the softness feathers the lift, so a lift of 0 is
// the whole passthrough no matter what the softness key says.
constexpr bool IsLiftIdentity(float liftEff) noexcept
{
    return !(liftEff > kIdentityEpsilon);
}

constexpr bool IsAmbientIdentity(float multiplier) noexcept
{
    return !(multiplier > 1.0f + kIdentityEpsilon) && !(multiplier < 1.0f - kIdentityEpsilon);
}

// The gate of the mask route (the relight seam's tracker and its draw route ask it per bind and per draw): the master key on, the route
// not stood down, and then either the shadow lift key off its identity, or - with the lift at its identity - the night ambient key off
// its identity on the ticked curve. The second case is the light colour feed: the ambient arm keys on the frame's own light colour
// (CurveStrength returns 0 until a reading has landed), and the route is the only place that colour is captured when contact shadows
// is not serving the light draws; without the feed that arm would wait for a colour nothing ever copies. With the curve unticked the
// colour is not needed, and both keys at their identity run nothing at all: no tracking, no copy, no pin, no entry.
constexpr bool MaskWantsDecision(bool keyOn, bool sessionArmed, float liftKey, float ambientKey, bool nightCurveToggle) noexcept
{
    if (!keyOn || !sessionArmed)
        return false;
    if (!IsLiftIdentity(ClampLift(liftKey)))
        return true;
    return nightCurveToggle && !IsAmbientIdentity(ClampAmbientLift(ambientKey));
}

// ---- the day/night curve --------------------------------------------------------------------------------------------------

// PROVISIONAL thresholds, on the light colour's Rec.709 luminance: full effect at or below the night value, zero at
// or above the day value, a smoothstep between them. DO NOT tune either number from the screen: the five-stop F9
// capture of the design report 5 (U-c: one 3-minute window across a dawn or dusk, cb.csv's sweep of
// g_LightParam.m_DiffuseColor read against the five screenshots) settles both, and the ini defaults stay provisional
// until that session runs. Where the measured rows land: the night row (0.078, 0.117, 0.16) computes to luminance
// about 0.109 - comfortably below 0.15; the brighter moon row (0.176, 0.264, 0.49) about 0.26 - a quarter up; the
// day row (1.96, 1.49, 1.25) about 1.54 - above 0.9.
inline constexpr float kCurveNightLuminance = 0.15f; // PROVISIONAL: the full-effect side; the five-stop capture settles it
inline constexpr float kCurveDayLuminance = 0.90f;   // PROVISIONAL: the zero-effect side; the five-stop capture settles it

// Rec.709 luminance of a linear light colour: the single number the curve keys on. A NaN channel makes the colour
// unusable (a broken value never turns the patch on), not night.
constexpr float LuminanceOf(const float diffuse[3]) noexcept
{
    const float r = diffuse[0] == diffuse[0] ? diffuse[0] : 0.0f;
    const float g = diffuse[1] == diffuse[1] ? diffuse[1] : 0.0f;
    const float b = diffuse[2] == diffuse[2] ? diffuse[2] : 0.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// The curve's shape on the two provisional thresholds: full effect the whole way below the night value, zero from
// the day value up.
constexpr float CurveAt(float luminance) noexcept
{
    if (!(luminance > kCurveNightLuminance))
        return 1.0f;
    if (luminance >= kCurveDayLuminance)
        return 0.0f;
    const float t = (luminance - kCurveNightLuminance) / (kCurveDayLuminance - kCurveNightLuminance);
    return 1.0f - t * t * (3.0f - 2.0f * t); // 1 - smoothstep: full effect at night, zero from the day value up
}

// What the curve reads for a frame. A toggle that is off pins the full effect (an explicit debug override, day
// included); a session that has not read a light colour yet runs the identity, so neither the default-off state nor
// a session whose ring has not landed yet patches anything. A GPU a frame or two behind is the ring's own business
// (the staging ring below), not a refusal: the last good colour keeps serving.
constexpr float CurveStrength(bool nightCurveToggle, bool haveReading, float luminance) noexcept
{
    if (!nightCurveToggle)
        return 1.0f;
    if (!haveReading)
        return 0.0f;
    return CurveAt(luminance);
}

// ---- the effective values of one frame ------------------------------------------------------------------------------------

// What one frame's patches move, from the keys and the captured light colour alone. The curve scales each key (the
// mask lift to its 0 anchor, the ambient lift anchored at the engine's own 1.0), and identity anywhere in the chain
// means that patch runs nothing: the default state (every key at its own idle) is byte-exact today's picture, as is
// a day frame's night-curve-guarded branch.
struct Blend
{
    float curve = 0.0f;          // what the light colour gave, 0..1 (1.0 with the curve toggle off)
    float maskLift = 0.0f;       // the mask path's effective remap lift (the key x the curve)
    float softness = 0.0f;       // the tent's reach, as keyed (it feathers the lift only)
    float ambientScale = 1.0f;   // the ambient entries' multiplier (the key x the curve; 1.0 = the engine's own)
    bool maskRuns = false;       // the mask path wants to copy, remap and bind (armed or refused on its own streak)
    bool ambientRuns = false;    // the ambient arm wants entries on the composite draw
};

// The exact form, written out so no hidden clamp sits between the values: the curve scales the lift key and the
// ambient lift key, and a patch arms only when its effective value has left the identity range and the master is
// on. `maskArmed` is the mask arm's half of the master, asked by whichever bind path serves this frame (the
// contact combine's stage when contact shadows is armed, the module's own copy when it is not); the ambient arm
// ignores it.
constexpr Blend FrameBlendCurve(bool masterOn, bool maskArmed, float liftKey, float softnessKey, float ambientKey, float curve) noexcept
{
    Blend blend;
    if (!masterOn)
        return blend;
    blend.curve = curve;
    blend.maskLift = ClampLift(liftKey) * curve;        // 0 at curve 0: the mask's own byte, whatever the key says
    blend.softness = ClampSoftness(softnessKey);
    // The ambient multiplier is anchored at 1.0 (the engine's own picture), not at 0: the curve walks the lift DOWN
    // to identity for the day, never through "no multiplier" (a multiplier of 0 would black the whole table out).
    blend.ambientScale = 1.0f + (ClampAmbientLift(ambientKey) - 1.0f) * curve;
    blend.maskRuns = maskArmed && !IsLiftIdentity(blend.maskLift);
    blend.ambientRuns = !IsAmbientIdentity(blend.ambientScale);
    return blend;
}

// The first slice's form: the curve from the light colour's luminance (kept for the callers and tests that name it).
constexpr Blend FrameBlend(bool masterOn, bool maskArmed, float liftKey, float softnessKey, float ambientKey,
                           bool nightCurveToggle, bool haveReading, float luminance) noexcept
{
    return FrameBlendCurve(masterOn, maskArmed, liftKey, softnessKey, ambientKey, CurveStrength(nightCurveToggle, haveReading, luminance));
}

// ---- the night curve on the visible sun (2026-11-07) ---------------------------------------------------------------------------------------
//
// The curve's input is the fog post's own sun: cFogParam[2].y, the visible sun's world elevation sine (engine/FogSun.h). night = smoothstep(sin(-1
// degree), sin(-7 degree), y): no lift while the sun is above -1 degree, full by the end of civil twilight. On the rc6 frames: 0.183 -> 0, -0.109 ->
// 0.958, -0.058 -> 0.335, -0.012 -> 0 (the evening frame of capture 4 looked like daylight; the shading light's colour there is the dim moon-like
// light, which put the colour curve at 1). The light colour stays as the fallback when no fog sun has landed recently.
inline constexpr float kNightFogSunUpY = -0.017452406f;   // sin(-1 deg)
inline constexpr float kNightFogSunDownY = -0.121869343f; // sin(-7 deg)

constexpr float NightFromFogSun(float fogSunY) noexcept
{
    if (!(fogSunY == fogSunY))
        return 0.0f; // an unreadable sun is not night
    float t = (fogSunY - kNightFogSunUpY) / (kNightFogSunDownY - kNightFogSunUpY);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// How long a published fog sun counts as current, in the consumer's own clock: rendered frames with a served light draw (not the present counter, which
// frame generation steps two to four times per game frame). About two seconds at 60 frames a second: the fog moves over seconds, and a capture that has
// stopped (a cutscene, a zone without the fog post) must hand the curve back to the colour within a couple of them.
inline constexpr uint32_t kFogSunMaxAgeFrames = 120;

// Watches the accessor's publish count against the consumer's clock: a publish seen for the first time restarts the age, the same count seen again does
// not. A reset forgets the age but remembers the count, so a publish from before the reset is not revived by it.
struct FogSunTracker
{
    bool have = false;
    uint32_t lastCount = 0;
    uint64_t changedAt = 0;

    constexpr void Observe(uint32_t publishCount, uint64_t now) noexcept
    {
        if (publishCount == 0 || publishCount == lastCount)
            return;
        lastCount = publishCount;
        changedAt = now;
        have = true;
    }

    constexpr bool Fresh(uint64_t now) const noexcept { return have && now >= changedAt && now - changedAt <= kFogSunMaxAgeFrames; }

    constexpr void Reset() noexcept { have = false; }
};

enum class NightSource : uint8_t
{
    None = 0,        // neither a fresh fog sun nor a light colour: the identity
    FogSun = 1,
    LightColour = 2,
};

struct NightReading
{
    bool fogFresh = false;
    float fogSunY = 0.0f;
    bool haveColour = false;
    float luminance = 0.0f;
};

struct NightCurveResult
{
    float curve = 0.0f;
    NightSource source = NightSource::None;
};

// The frame's curve: the toggle off pins the full effect (the debug override), a fresh fog sun is the input, else the light colour's luminance curve,
// else the identity.
constexpr NightCurveResult NightCurve(bool nightCurveToggle, const NightReading& reading) noexcept
{
    NightCurveResult result;
    if (!nightCurveToggle)
    {
        result.curve = 1.0f;
        return result;
    }
    if (reading.fogFresh)
    {
        result.curve = NightFromFogSun(reading.fogSunY);
        result.source = NightSource::FogSun;
        return result;
    }
    if (reading.haveColour)
    {
        result.curve = CurveAt(reading.luminance);
        result.source = NightSource::LightColour;
    }
    return result;
}

// Whether the fog-post capture is wanted for the curve: the route is wanted (the master on, not stood down, the lift off its identity or the ambient arm
// on the ticked curve) and the curve is ticked. Off by default: the master key is off, so nothing is asked of the capture.
constexpr bool FogSunWanted(bool keyOn, bool sessionArmed, float liftKey, float ambientKey, bool nightCurveToggle) noexcept
{
    return nightCurveToggle && MaskWantsDecision(keyOn, sessionArmed, liftKey, ambientKey, nightCurveToggle);
}

constexpr uint32_t NightSourceKey(NightSource source) noexcept { return 0xC0000000u | uint32_t(source); }

inline std::string FormatNightSource(NightSource source, float fogSunY, float luminance, float curve)
{
    switch (source)
    {
    case NightSource::FogSun:
        return std::format("Shadow mapping: the night curve reads the fog sun (its world elevation sine is {:.3f}), curve {:.3f}", double(fogSunY), double(curve));
    case NightSource::LightColour:
        return std::format("Shadow mapping: no fog sun has landed recently, so the night curve reads the main view's light colour (luminance {:.3f}), curve {:.3f}",
                           double(luminance), double(curve));
    case NightSource::None:
        break;
    }
    return "Shadow mapping: the night curve has neither a fog sun nor a light colour yet, so it runs the identity";
}

// ---- the mask-path cost meter (the own-copy route, used when contact shadows is not armed) --------------------------------

// A scaled mask's copy costs one allocation per size and one dispatch per light draw; the classification they must
// all answer is the contact shadows rules' `Classify` (the struct below is the route's own accounting). The streak,
// the log gate and the decline list are the relight module's own rules, used verbatim: the light draw a shadows
// patch refuses and the light draw a contact patch refuses are the same draw.
using relight::CostMeter;
using relight::Decline;
using relight::LogGate;
using relight::DrawFacts;

// After this many of the module's own declines in a row the shadows module stands itself down for the session with
// one log line - the sixty-four-refusal budget of the engine seam, not the contact seam's sixteen, because the
// module sees about two light draws a frame and a shape mismatch would otherwise cost a session (EnginePassRules.h's
// session budget, applied to the light-draw seam). A shape refusal (relight::IsShapeDecline - the cascade atlas, a
// mask of another format) never counts, exactly as in ContactShadowsApply.cpp.
inline constexpr uint32_t kConsecutiveDeclineLimit = 64;

struct Session
{
    uint32_t consecutiveDeclines = 0;
    bool off = false;

    bool NoteDecline()
    {
        if (off)
            return false;
        if (++consecutiveDeclines >= kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    void NoteRun() { consecutiveDeclines = 0; }

    bool Armed() const noexcept { return !off; }
};

// ---- the remap (patch A's arithmetic, mirrored 1:1 by the shaders and the CPU reference) -----------------------------------

// The remap on the SQUARED mask: what the light draw multiplies after it squares the copy it samples. At an identity
// lift this answers the square as it came in, so the copy can skip the remap straight to the game's own byte. A mask
// value the R8 grid cannot carry (a NaN, in the CPU reference) passes through the same arithmetic unchanged: the
// grid itself never holds one, so the shader never sees it, and the caller's guard owns the passthrough.
constexpr float RemappedSquare(float mask, float liftEff) noexcept
{
    const float squared = mask * mask;
    if (!(liftEff > 0.0f))
        return squared; // identity, however a build or a sampler made it
    const float lifted = squared + (1.0f - squared) * liftEff;
    if (lifted != lifted)
        return lifted; // a broken mask keeps the answer the arithmetic gave, it never becomes a clean colour
    if (!(lifted > 0.0f))
        return 0.0f;
    return lifted > 1.0f ? 1.0f : lifted;
}

// The byte the copy carries: the square root of the remapped square, so the light draw's own square reproduces it.
// At an identity lift this answers the mask's own byte exactly - the passthrough guard sits in the caller's shader
// and in the CPU reference alike, so the R8 grid never sees a sqrt round of an unlifted value.
constexpr float RemapCopyValue(float mask, float liftEff) noexcept
{
    if (!(liftEff > 0.0f))
        return mask;
    const float squared = RemappedSquare(mask, liftEff);
    return squared <= 0.0f ? 0.0f : squared >= 1.0f ? 1.0f : sqrt(squared);
}

// The tent weight of a neighbour at (dx, dy) mask pixels with the softness S: max(0, 1 - d/S) with d the pixel
// distance. A reach of at most one pixel touches the 3x3 kernel, two the 5x5 one; S <= 0 is the texel itself with
// weight 1 (the softness feathers the lift, so it never acts on an identity pass). NOT constexpr: the sqrt keeps it
// a runtime rule (C++ constexpr sqrt is not portable), and the shader mirrors it as written.
inline float TentWeight(float dx, float dy, float softness) noexcept
{
    if (!(softness > 0.0f))
        return dx == 0.0f && dy == 0.0f ? 1.0f : 0.0f;
    const float d = sqrt(dx * dx + dy * dy);
    if (!(d < softness))
        return 0.0f;
    return 1.0f - d / softness < 0.0f ? 0.0f : 1.0f - d / softness;
}

// The kernel's layout is static (five to each side, zero weights outside the reach), so 3x3 and 5x5 are the same
// code and the same test expectation: the blurred value of a texel is the weights of its 5x5 neighbourhood over
// their sum, with the centre's own weight first. The blur lives in the two shaders (the module's own and the
// contact combine's, where the remap rides) and in the WARP tests' CPU reference; the tent WEIGHT above is the one
// rule they share, so a shader that weights a wrong offset is refused by the same test as a wrong distance.

// ---- the ambient entries (patch B, the corrected shapes) -------------------------------------------------------------------

// The two scale fields the composite multiplies, as CbScale.hlsl addresses them (byte offsets of float4 rows):
inline constexpr uint32_t kHeaderSkyScaleEntryOffset = 0; // the header's first float4; `skyShRecipScale` is its .y at byte 4
inline constexpr uint32_t kHeaderSkyScaleEntryStride = 16;
inline constexpr uint32_t kHeaderSkyScaleEntryCount = 1;

// The per-probe field reuses the engine module's own constants: `AmbientSkyRecipEntry` addresses 112 + 192k, `.w`,
// and its multiplier slot is the fourth component of the multiplier vector.
inline constexpr uint32_t kShadowEntryCount = 2;

// Entry 1: the header's skyShRecipScale. The design doc's original shape (offset 4, stride 0) is not addressable -
// AddressFits refuses byteOffset % 16 != 0 and stride == 0, and CbScale.hlsl's Addresses answers false for a stride
// of 0 - so the entry covers the buffer's first float4 and scales only its .y, the float4 row the header field
// lives in. The multiplier vector carries 1 everywhere but that field.
constexpr engine::ScaleEntry HeaderSkyScaleEntry(float multiplier) noexcept
{
    engine::ScaleEntry entry;
    entry.byteOffset = kHeaderSkyScaleEntryOffset;
    entry.stride = kHeaderSkyScaleEntryStride;
    entry.count = kHeaderSkyScaleEntryCount;
    entry.multiplier[1] = multiplier;
    return entry;
}

// Entry 2: the per-probe shRecipScale, the engine module's own AmbientSkyRecipEntry shape with the shadows'
// multiplier written in (.w of the multiplier vector; every other component stays exactly 1, so the probe's
// colour bytes beside it come out of the dispatch untouched).
constexpr engine::ScaleEntry ProbeSkyScaleEntry(float multiplier) noexcept
{
    return engine::AmbientSkyRecipEntry(multiplier);
}

// Whether the two entries stand inside a buffer one of the two ambient sizes: the composite binds either the
// engine's declared 12,352 bytes or a 16,384-byte buffer, both the same table (EnginePassRules.h).
constexpr bool AmbientEntriesFit(const engine::ScaleEntry* entries, uint32_t count, uint32_t byteWidth) noexcept
{
    for (uint32_t i = 0; i < count; ++i)
        if (!engine::EntryFits(entries[i], byteWidth))
            return false;
    return true;
}

// The shadows module's ambient table. Returns how many entries were written: 0 is "no change" (the multiplier is at
// 1.0, which is the engine's own picture; or the caller has no room, or the entries do not fit either size).
constexpr uint32_t AmbientEntries(float ambientMultiplier, engine::ScaleEntry* out, uint32_t capacity) noexcept
{
    if (out == nullptr || capacity < kShadowEntryCount || IsAmbientIdentity(ambientMultiplier))
        return 0;
    out[0] = HeaderSkyScaleEntry(ambientMultiplier);
    out[1] = ProbeSkyScaleEntry(ambientMultiplier);
    if (!AmbientEntriesFit(out, kShadowEntryCount, engine::kAmbientDeclaredBytes) ||
        !AmbientEntriesFit(out, kShadowEntryCount, engine::kAmbientBoundBytes))
        return 0;
    return kShadowEntryCount;
}

// How the one merged dispatch's entry table is built, and the fixed ORDER the composition relies on: the shipped
// knob's entries first (their `112+192k .w` scale reached earlier in the loop), then the shadows' two. CbScale
// multiplies every component by each matching entry in that order, so a probe's shRecipScale leaves the dispatch as
// `value * knobScale * nightLift` - the two knobs compose multiplicatively, and either one alone is the picture it
// alone would make.
struct MergedAmbientTable
{
    uint32_t knobEntries = 0;   // the shipped ambient knob's count (0 when it is at 1.0)
    uint32_t shadowEntries = 0; // the shadows module's count (0 when the multiplier is at 1.0)
    uint32_t total = 0;         // both together, never past kMaxScaleEntries
};

constexpr MergedAmbientTable MergedAmbientEntries(const engine::Params& knobParams, float ambientMultiplier,
                                                  engine::ScaleEntry* entries, uint32_t capacity) noexcept
{
    MergedAmbientTable table;
    if (entries == nullptr || capacity < kShadowEntryCount)
        return table; // fail open: no room for the pair means no patch at all, never a half table
    table.knobEntries = engine::AmbientEntries(knobParams, entries, capacity);
    if (engine::kMaxScaleEntries - table.knobEntries < kShadowEntryCount)
        return table; // the knob's entries keep the low slots; the night lift waits for a bigger table, never shifts them
    const uint32_t shadows = AmbientEntries(ambientMultiplier, entries + table.knobEntries, capacity - table.knobEntries);
    table.shadowEntries = shadows;
    table.total = table.knobEntries + shadows;
    return table;
}

// Whether anything at all wants the composite draw (the knob's scale off 1.0, or the shadows' multiplier): the
// engine seam asks this before it inspects anything, so a day frame with the night keys at their idle costs one
// relaxed load - the handle's answer is what patches nothing.
constexpr bool MergedAmbientRuns(const MergedAmbientTable& table) noexcept { return table.total != 0; }

// ---- the log lines --------------------------------------------------------------------------------------------------------

inline std::string FormatDecline(Decline reason)
{
    return std::format("Shadow mapping: the light draw was left alone ({})", relight::DeclineName(reason));
}

inline std::string FormatSwitchedOff()
{
    return std::format("Shadow mapping: {} light draws in a row were left alone, so the shadow mapping module is "
                       "off for this session (no more light colour is captured, so the night ambient lift forgets the colour "
                       "it had read and the engine's own ambient shows until the module is re-armed)",
                       kConsecutiveDeclineLimit);
}

// The mask route's first served light draw: which draw the route is following and what the keys ask of it. It is the
// line a retest looks for first, because it proves the draw hook reached the module with the contact pass off.
inline std::string FormatRouteFirst(uint32_t maskWidth, uint32_t maskHeight, uint32_t maskFormat, uint32_t maskSlot,
                                    uint32_t lightParamSlot, float liftKey, float ambientKey, bool nightCurveToggle)
{
    return std::format("Shadow mapping: the mask route is serving the sun light draw (mask {}x{} format {} at t{}, "
                       "g_LightParam at cb{}); shadow lift {:.2f}, night ambient lift {:.2f}, light colour capture on{}",
                       maskWidth, maskHeight, maskFormat, maskSlot, lightParamSlot, double(liftKey), double(ambientKey),
                       nightCurveToggle ? "" : " (the curve is pinned by the toggle, the colour is read for the log only)");
}

// The first light colour that landed from the capture ring, with the curve it gives and what that makes of the keys.
inline std::string FormatColourRead(const float diffuse[3], float curve, float maskLift, float ambientScale)
{
    return std::format("Shadow mapping: the first light colour landed ({:.4f}, {:.4f}, {:.4f}), luminance {:.4f}, "
                       "curve {:.3f}: mask lift {:.3f}, ambient table x{:.3f}",
                       double(diffuse[0]), double(diffuse[1]), double(diffuse[2]), double(LuminanceOf(diffuse)),
                       double(curve), double(maskLift), double(ambientScale));
}

// The light colour the curve keys on moved (by a factor of two in luminance, or to another light draw): when, what, the curve it gives and
// the route id of the draw whose copy it came from (0 = the contact seam's copy). The session reads from these lines when and from which
// draw the night curve changed, which the first-colour line alone cannot say.
inline std::string FormatColourChange(const float diffuse[3], float curve, uint32_t routeId)
{
    return std::format("Shadow mapping: the light colour read changed to ({:.4f}, {:.4f}, {:.4f}), luminance {:.4f}, curve {:.3f}, "
                       "from the light draw with route id {}",
                       double(diffuse[0]), double(diffuse[1]), double(diffuse[2]), double(LuminanceOf(diffuse)), double(curve),
                       routeId);
}

// A frame can carry two light draws (two different light shaders). The capture keeps ONE copy per frame: the later draw's copy overwrites the
// earlier one's slot, so the curve reads the later draw. Written once per state, with the two route ids, so a session knows it happened.
inline std::string FormatTwoLightDraws(uint32_t firstRouteId, uint32_t laterRouteId)
{
    return std::format("Shadow mapping: two light draws were served in one frame (route ids {} then {}); the capture keeps one copy a frame, "
                       "so the later draw's colour is the one the night curve reads",
                       firstRouteId, laterRouteId);
}

// Every reason the module stays idle although its keys ask for work has its own line, written once per state.
enum class Idle : uint8_t
{
    NoLayout = 0,         // the draw hook never handed the route a sun light draw the light-draw index knows
    ColourBufferSmall,    // the bound g_LightParam is smaller than the members the curve reads
    ColourStagingFailed,  // the capture ring's staging buffer could not be created
    NoColourYet,          // the curve waits for a light colour that has not landed
    EntriesDidNotFit,     // the ambient entries do not fit either table size
    NoComposite,          // the engine seam never asked for entries: it has not classified the deferred composite
    Count
};

// How many route asks without a layout, and how many ambient asks without a reading, before the module says why it idles.
inline constexpr uint32_t kNoLayoutAsks = 4096;
inline constexpr uint32_t kNoColourAsks = 240;
inline constexpr uint32_t kNoCompositeDraws = 600; // served sun light draws (two a frame) without one ask of the ambient provider

inline std::string FormatIdleNoLayout(uint32_t asks)
{
    return std::format("Shadow mapping: the mask route was asked {} times and none of the draws was a sun light draw "
                       "the light-draw index knows (no pixel shader declares the sun mask, the depth, g_LightParam and "
                       "g_CameraParameter together), so the mask lift and the light colour capture stay idle",
                       asks);
}

inline std::string FormatIdleColourBuffer(uint32_t bytes, uint32_t needed)
{
    return std::format("Shadow mapping: the light colour is not captured: the bound g_LightParam is {} bytes and the "
                       "curve reads the first {}",
                       bytes, needed);
}

inline std::string FormatIdleColourStaging()
{
    return "Shadow mapping: the light colour is not captured: the capture ring's staging buffer could not be created";
}

inline std::string FormatIdleNoColour(uint32_t asks, uint64_t routeDraws, uint32_t copies, uint32_t reads, uint32_t notReady)
{
    return std::format("Shadow mapping: the night curve has no light colour after {} asks ({} route draws, {} copies "
                       "staged, {} reads landed, {} not ready), so the lifts wait for it; with \"Key on the frame's own "
                       "light colour\" ticked this is the identity until the first read lands",
                       asks, routeDraws, copies, reads, notReady);
}

inline std::string FormatIdleNoComposite(uint32_t lightDraws, float ambientKey)
{
    return std::format("Shadow mapping: {} sun light draws were served and the night ambient lift (x{:.2f}) was never asked "
                       "for entries: the engine seam has not reached the deferred composite (no draw that declares "
                       "g_AmbientParamArray and g_SamplerLightDiffuse was classified), so the ambient table stays the "
                       "engine's own",
                       lightDraws, double(ambientKey));
}

inline std::string FormatIdleEntries()
{
    return "Shadow mapping: the ambient entries do not fit the composite's table sizes, so the night ambient lift is "
           "left alone";
}

// What the card's state line is made of: the module's own counters, never the engine seam's "asks".
struct StateFacts
{
    bool keyOn = false;
    bool sceneHdr = false;       // the engine's scene HDR path is on (the module is inert)
    bool liftKeyOff = true;      // the shadow lift key sits at its identity
    bool ambientKeyOff = true;   // the night ambient key sits at its identity
    bool standDown = false;      // the route stood itself down for the session
    uint64_t routeDraws = 0;     // light draws the mask route served (classified)
    uint32_t routeRuns = 0;      // of those, the ones that bound a lifted mask
    uint32_t colourCopies = 0;   // light colour copies staged
    uint32_t colourReads = 0;    // reads that landed
    uint32_t colourNotReady = 0; // reads that were not ready yet
    uint32_t ambientPatched = 0; // composite dispatches that carried the module's entries
    float curve = 0.0f;          // the curve of the last blend
    bool haveReading = false;
};

inline std::string FormatStateLine(const StateFacts& facts)
{
    if (!facts.keyOn)
        return "off";
    if (facts.sceneHdr)
        return "inactive while scene HDR is on: that path replaces the tone chain this module draws before";
    if (facts.liftKeyOff && facts.ambientKeyOff)
        return "on at the engine's own picture: both keys at their idle today, nothing runs";
    return std::format("night keys live: {} route draw(s) seen, {} lifted; light colour: {} copy(ies) staged, {} read(s) "
                       "landed, {} not ready, {}; ambient table: {} dispatch(es) patched{} (see the log for the "
                       "per-state lines)",
                       facts.routeDraws, facts.routeRuns, facts.colourCopies, facts.colourReads, facts.colourNotReady,
                       facts.haveReading ? std::format("curve {:.3f}", double(facts.curve)) : std::string("no reading yet"),
                       facts.ambientPatched,
                       facts.standDown ? "; the mask route stood itself down for the session" : "");
}

// The mask run's line: what the copy carries, the curve that produced it and the lift that moved it.
inline std::string FormatMaskRun(uint32_t maskWidth, uint32_t maskHeight, uint32_t maskFormat, float liftEff,
                                 float softness, float curve, bool haveReading)
{
    const std::string curvePart = haveReading ? std::format("curve {:.3f}", double(curve))
                                              : std::string("curve at identity (no light colour read yet)");
    return std::format("Shadow mapping: lifting the sun mask ({}x{} format {}), lift {:.3f}, softness {:.2f} px, {}",
                       maskWidth, maskHeight, maskFormat, double(liftEff), double(softness), curvePart);
}

// The ambient run's line: what the ONE dispatch multiplied, under which curve, on which buffer size.
inline std::string FormatAmbientRun(float ambientMultiplier, float curve, uint32_t bytes, uint32_t targetWidth,
                                    uint32_t targetHeight, uint32_t slot)
{
    return std::format("Shadow mapping: lifting the ambient table (g_AmbientParamArray at cb{}, {} bytes, {}x{} "
                       "composite) by {:.3f}, curve {:.3f} (header skyShRecipScale and 64 probe shRecipScale fields, "
                       "one dispatch)",
                       slot, bytes, targetWidth, targetHeight, double(ambientMultiplier), double(curve));
}

// The composite the ambient arm patches, named once when the engine seam indexes it: the slots come from its own
// reflection, so a retest reads here which constant buffer and which texture the arm and the engine's index mean.
inline std::string FormatCompositeIndexed(uint32_t ambientTableSlot, uint32_t lightDiffuseSlot)
{
    return std::format("Engine pass overrides: composite indexed (g_AmbientParamArray at cb{}, g_SamplerLightDiffuse at "
                       "t{}); the night ambient lift patches that constant buffer",
                       ambientTableSlot, lightDiffuseSlot);
}

// The on-cost line the seam keeps for the module's own route (the mask path), sampled like the contact module's.
inline std::string FormatCost(uint64_t drawsSeen, uint32_t sampled, double perDrawMilliseconds, double budgetMilliseconds)
{
    return std::format("Shadow mapping: on cost: {} light draws seen, {} sampled, the draw's lookup {:.5f} ms per "
                       "draw (the route's budget is {:.2f} ms)",
                       drawsSeen, sampled, perDrawMilliseconds, budgetMilliseconds);
}
} // namespace ext::ffxiv::shadows
