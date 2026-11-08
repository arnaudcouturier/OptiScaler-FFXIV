#pragma once
// Depth of field, Route B (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is
// covered by tests/ext/ffxiv/engine on the host.
//
// What the feature does, in one line: at the tone-map draw, before the game's tone map reads the lit scene, the pass packs the
// scene's colour with an analytic circle of confusion (the game's own Weight2 + CoC-LUT pair's pack, with the CoC computed from
// the camera's own depth instead of read from the engine's LUT), blurs it on a disc at a quarter of the render window split
// near and far (the game's own DownScale4x4 + DiscBlurNear shapes with our own tap budget), and merges the two quarter targets
// back into an owned copy of the scene through the full-resolution CoC the pack carries. The tone map's `t0` then reads the
// blurred picture for that one draw - the blurred scene is what the tone map and everything behind it sees, the order the
// game's own DoF chain runs in on the benchmark (the pack, the downscale pair, the near blur, the merge, the tone map -
// ordinals 4583-4606 of the bench capture's draws.csv).
//
// Everything that can be decided without a device lives here: the keys and their ranges, the analytic CoC (a sharp focus band
// around the focus plane, a capped near side, the near/far split and the focus modes), the camera rows' acceptance, the disc pattern, the merge rule, the quarter extent, the outcomes the
// chain's hold semantics name, and the log/card lines. The fail-open contract is the same one the march's and the radial's
// are: every reason leaves the chain exactly as it was - the chained decline leaves the chain's last good picture standing
// at t0, the owner decline leaves the game's own binding, draws nothing and returns false, so the caller draws the game's call once
// (the chain's owner-return rule).
//
// The game's own facts this rests on (read off the benchmark's draw capture and the shipped shader files):
//  - the engine's own DoF family runs on the benchmark: Weight2 (the pack), DownScale4x4 (two quarter targets), an unnamed
//    far blur (the bench's ordinal 4585), DiscBlurNear (both ways) and Merge2, with the tone map immediately after. Weight2
//    saturates its colour to 10 (min(col,10)) and carries the CoC in the output alpha - this pack keeps the same two habits;
//  - the CoC LUT (fmt65:256x1) belongs to shaping the game's own chain (cWeightSamplingParam's 192-B layout would ride it in that
//    design): this stage computes the CoC analytically from the pixel's view depth against the focus distance;
//  - the camera rows are validated the contact pass's own way: ProjectionOk on the projection, and the derived near/far
//    planes inside the band a real camera's clip space can produce.

#include "ext/ffxiv/engine/ShaftsRules.h" // Window, WindowUsable, HalfExtent, ProjectionOk, IsSceneFormat, ClassifyToneMap
#include "ext/ffxiv/engine/EnginePassRules.h" // the session's knob and the log gate

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace ext::ffxiv::dof
{
// ---- the keys ------------------------------------------------------------------------------------------------------------

// The blur's reach: a fraction of the render window's height, so the same 0.05 reads as the same circle at 1440p and at
// 1080p. Below the floor the disc's radius is sub-pixel at every resolution and the key means nothing; above the band's
// top the quarter-res blur loses the taps' individual identity (12% of the height at a quarter res is a 43-quarter-pixel
// radius, and a quarter target is only 360 texels tall at the captured window).
inline constexpr float kMinAperture = 0.005f;
inline constexpr float kMaxAperture = 0.12f;
inline constexpr float kDefaultAperture = 0.05f;

// The mix amount: 0 is off (the feature bit goes off, the draw path costs one relaxed load - the radial's own rule for its
// intensity key), 1 is the blur at its own weight. A NaN reads as the default, so a broken value never changes the picture.
inline constexpr float kMinStrength = 0.0f;
inline constexpr float kMaxStrength = 1.0f;
inline constexpr float kDefaultStrength = 0.8f;
inline constexpr float kStrengthEpsilon = 0.001f;

// The disc's taps: 16 is the coarsest pattern that still lands as a disc, 48 the most the pass is worth at a quarter of the
// window. The count is the key, and every tap reads its own texel, so both ends stay deterministic.
inline constexpr int32_t kMinSteps = 16;
inline constexpr int32_t kMaxSteps = 48;
inline constexpr int32_t kDefaultSteps = 24;

// The manual focus distance, in metres (the engine's view space: the fog profile's own reference heights read 10 and 40).
// 0.5 m is the closest a real scene's near plane allows; 500 m is past every playable focus plane the game draws.
inline constexpr float kMinFocusDistance = 0.5f;
inline constexpr float kMaxFocusDistance = 500.0f;
inline constexpr float kDefaultFocusDistance = 3.0f;

constexpr float ClampStrength(float value) noexcept
{
    if (value != value)
        return kDefaultStrength;
    return value < kMinStrength ? kMinStrength : (value > kMaxStrength ? kMaxStrength : value);
}

constexpr float ClampAperture(float value) noexcept
{
    if (value != value)
        return kDefaultAperture;
    return value < kMinAperture ? kMinAperture : (value > kMaxAperture ? kMaxAperture : value);
}

constexpr float ClampFocusDistance(float value) noexcept
{
    if (value != value)
        return kDefaultFocusDistance;
    return value < kMinFocusDistance ? kMinFocusDistance : (value > kMaxFocusDistance ? kMaxFocusDistance : value);
}

constexpr uint32_t ClampSteps(int32_t value) noexcept
{
    const int32_t steps = value < kMinSteps ? kMinSteps : (value > kMaxSteps ? kMaxSteps : value);
    return uint32_t(steps);
}

// The focus mode: `auto` is the view depth under the screen centre (the depth texel at the window's own centre pixel,
// reconstructed through the engine's inverse projection inside the pack shader - no ring, no staleness, the focus rides
// the same depth the pack already reads), `manual` is the DofFocusDistance key. An unknown text reads as auto.
enum class FocusMode : uint8_t
{
    Auto = 0,
    Manual = 1,
};

constexpr FocusMode ParseFocusMode(std::string_view text) noexcept
{
    return text == "manual" || text == "Manual" || text == "MANUAL" ? FocusMode::Manual : FocusMode::Auto;
}

constexpr const char* FocusModeName(FocusMode mode) noexcept { return mode == FocusMode::Manual ? "manual" : "auto"; }

// The look: `distance` is the gameplay mode (the default, far side only, see "the distance mode" below), `cinematic` is the shaped CoC of the
// first release, kept exactly as it was. An unknown text reads as the default, so a broken ini value never changes the picture to the old look.
enum class Mode : uint8_t
{
    Distance = 0,
    Cinematic = 1,
};

constexpr Mode ParseMode(std::string_view text) noexcept
{
    return text == "cinematic" || text == "Cinematic" || text == "CINEMATIC" ? Mode::Cinematic : Mode::Distance;
}

constexpr const char* ModeName(Mode mode) noexcept { return mode == Mode::Cinematic ? "cinematic" : "distance"; }

// The parameters one session runs with, from the keys. The master switch is not part of them: it decides whether the
// module is armed at all, and whether it wants a draw is Wanted() below.
struct Params
{
    float aperture = kDefaultAperture;
    float strength = kDefaultStrength;
    uint32_t steps = uint32_t(kDefaultSteps);
    FocusMode focusMode = FocusMode::Auto;
    float focusDistance = kDefaultFocusDistance;
    Mode mode = Mode::Distance;
};

constexpr Params BuildParams(float strength, float aperture, int32_t steps, std::string_view focusMode,
                             float focusDistance) noexcept
{
    Params params;
    params.aperture = ClampAperture(aperture);
    params.strength = ClampStrength(strength);
    params.steps = ClampSteps(steps);
    params.focusMode = ParseFocusMode(focusMode);
    params.focusDistance = ClampFocusDistance(focusDistance);
    return params;
}

// The same with the look (the key `DofMode`). The five-argument form above stays for callers that predate the key: they get the default look.
constexpr Params BuildParams(float strength, float aperture, int32_t steps, std::string_view focusMode, float focusDistance,
                             std::string_view mode) noexcept
{
    Params params = BuildParams(strength, aperture, steps, focusMode, focusDistance);
    params.mode = ParseMode(mode);
    return params;
}

// Whether the stage wants a draw: the master switch on, a strength above zero and an aperture above zero. A strength of 0
// mixes nothing (the merge's own lerp degenerates to the scene), an aperture of 0 reaches nothing: both are today's
// picture at no cost, so the draw path costs one relaxed load per draw and nothing else - the same rule the intensity key
// has in the shafts and the march. This is also what makes "identity at Strength 0" byte-exact: the stage does not run,
// and the scene handed in stays the scene the game's tone map reads.
constexpr bool Wanted(bool enabled, const Params& params) noexcept
{
    return enabled && params.strength > kStrengthEpsilon && (params.mode == Mode::Distance || params.aperture > kMinAperture);
}

// ---- the analytic CoC, and the near/far split -----------------------------------------------------------------------------

// The pack's colour cap: the engine's own Weight2 saturates its RGB to 10 before the merge (min(col,10)) and carries the
// CoC in the output alpha; this pack keeps the same two habits, so the blur's taps cannot carry a runaway highlight
// across a neighbour's pixels. The merge reads the scene it was handed for the in-focus colour (not the pack), so a
// highlight above 10 on an in-focus pixel keeps its own value byte for byte.
inline constexpr float kPackColourCap = 10.0f;

// The focus band and the near cap. The raw CoC (focus - z) / (z + focus) is a fraction of the unit circle, and with a far
// focus (the camera looking across a plaza) everything nearer than about a third of the focus distance sits at a raw CoC of
// 0.5 or more: a strong near blur over the whole lower third of the frame, the character included. So the CoC the pack writes
// is shaped:
//  - kDeadZone: a pixel whose raw |CoC| is within this of the focus plane is sharp (0.12 is 15.7 m to 25.4 m around a 20 m
//    focus, 4.7 m to 7.6 m around a 6 m one);
//  - above the band the magnitude is remapped linearly, (|c| - kDeadZone) / (1 - kDeadZone), so it keeps the unit range and
//    the blur starts continuously at the band's edge (a hard cut would start a 0.12 CoC, a 6.5 px blur at the default reach,
//    at once). The far blur is therefore a little weaker in the middle of its range (-55 % at a raw |c| of 0.2, -32 % at 0.3,
//    -14 % at 0.5, none at 1) and unchanged at its end;
//  - kNearCap: the near side (toward the camera) never blurs by more than this share of the full CoC; the far side is not
//    capped.
// Dof.hlsl carries the same two numbers and the same operations (CoCSigned); neither file may drift (dof_pins reads them
// against each other).
inline constexpr float kDeadZone = 0.12f;
inline constexpr float kNearCap = 0.25f;

// The shaping of a raw CoC (see above). A NaN is no blur; anything past ±1 is clamped to the unit circle first, so the answer
// lies in [-1, kNearCap]. The sign survives (positive = near, negative = far), and it is monotone in the raw CoC.
constexpr float ShapeCoC(float raw) noexcept
{
    if (!(raw == raw))
        return 0.0f;
    const float clamped = raw < -1.0f ? -1.0f : (raw > 1.0f ? 1.0f : raw);
    const float excess = std::fabs(clamped) - kDeadZone;
    const float magnitude = (excess > 0.0f ? excess : 0.0f) / (1.0f - kDeadZone);
    return clamped > 0.0f ? (magnitude < kNearCap ? magnitude : kNearCap) : -magnitude;
}

// The pack's alpha: the signed CoC of a pixel whose view depth is `viewZ` metres against a focus plane at `focus` metres.
// The raw value c = (focus - z) / (z + focus) is 0 exactly on the focus plane, positive toward the camera (the near side),
// negative away from it (the far side), ±1 toward the camera's own plane and at infinity; the pack carries it shaped
// (ShapeCoC): 0 inside the focus band, at most kNearCap on the near side, at least -1 on the far side - the band both halves
// of the split read as weights without a second clamp. A broken read (a non-finite depth or focus) is no blur, not a
// full-frame one: the same fail-open as a depth the march's rays cannot reconstruct.
constexpr float CoCSigned(float viewZ, float focus) noexcept
{
    if (!(viewZ > 0.0f) || !(focus > 0.0f) || !(viewZ == viewZ) || !(focus == focus))
        return 0.0f;
    const float c = (focus - viewZ) / (viewZ + focus);
    if (!(c == c))
        return 0.0f;
    return ShapeCoC(c);
}

// The two halves of the split, as the disc blur and the merge read them: the pixel's own reach into its class's target,
// always in [0, 1]. A perfectly focused pixel answers 0 to both, so it blurs nothing and mixes nothing.
constexpr float NearWeight(float cocSigned) noexcept { return cocSigned > 0.0f ? cocSigned : 0.0f; }
constexpr float FarWeight(float cocSigned) noexcept { return cocSigned < 0.0f ? -cocSigned : 0.0f; }

// The view depth of a hardware depth texel, in metres, through the engine's own inverse projection: the march's own
// position reconstruction applied to the depth row factored out, so the CoC's units are the same metres the focus
// distance is named in and the march's rays travel in. `inverseProjection` is the 64 bytes at g_CameraParameter +0x0E0
// (the march's own decode), `ndc` is the texel's pixel through the window's pixel-to-NDC mapping (RecogRules.h PixelToNdcRow) - the same mapping
// the march's rays walk. A depth the matrix cannot reconstruct answers 0, and CoCSigned refuses a zero.
constexpr float ViewDistanceAtDepth(const float inverseProjection[16], float ndcX, float ndcY, float depth) noexcept
{
    if (inverseProjection == nullptr || !(depth >= 0.0f) || !(depth == depth))
        return 0.0f;
    const float x = inverseProjection[0] * ndcX + inverseProjection[3];
    const float y = inverseProjection[5] * ndcY + inverseProjection[7];
    const float z = inverseProjection[11];
    const float w = inverseProjection[14] * depth + inverseProjection[15];
    if (!(w > 1.0e-6f) || !(w == w))
        return 0.0f;
    const float viewZ = -(z / w);
    if (!(viewZ > 0.0f) || !(viewZ == viewZ))
        return 0.0f;
    return viewZ;
}

// The band the camera's own planes must sit in for the CoC to be trusted: near 0.1 mm..100 m, far 10 m..100 km, and the
// far at least ten times the near (the engine camera struct's NearPlane/FarPlane as format validation - a buffer whose derived planes are
// a game's own clip space is accepted, a buffer that is anything else is refused like the contact pass refuses a camera
// whose projection's shape is not one). The planes are derived from the inverse projection and checked against the
// forward projection's own row 2, which must be the z row alone (p[8..11] = 0, 0, -b, a...see below) for the pack's
// depth-alone reconstruction to be the camera's.
struct DepthBand
{
    bool usable = false;
    float nearPlane = 0.0f;
    float farPlane = 0.0f; // +infinity when the projection has no far plane (reversed-Z); only the log reads it
};

inline constexpr float kMinNearPlane = 1.0e-4f;
inline constexpr float kMaxNearPlane = 100.0f;
inline constexpr float kMinFarPlane = 10.0f;
inline constexpr float kMaxFarPlane = 100000.0f;
// The ratio of the inverse projection's `a` to its `c` below which the far plane is taken as infinite (reversed-Z, the live build's own
// projection stores an exact zero there).
inline constexpr float kInfiniteFarRatio = 1.0e-9f;

constexpr DepthBand CameraDepthBand(const float inverseProjection[16], const float projection[16]) noexcept
{
    DepthBand band;
    if (inverseProjection == nullptr || projection == nullptr)
        return band;
    if (!shafts::ProjectionOk(projection))
        return band;
    // The z rows of both matrices must be the depth engine's own flat pair: the capture's own camera (the march's
    // fixture's round trip) carries the projection's row 2 as (0, 0, a, b) and the inverse's row 2 as (0, 0, 0, -b),
    // with the inverse's row 3 (0, 0, 1, a) - so a plane pair already exists inside them. Anything else is another
    // buffer's layout, and a wrong plane pair would scale every CoC by a wrong metres-per-depth.
    if (inverseProjection[8] != 0.0f || inverseProjection[9] != 0.0f || !(inverseProjection[11] < 0.0f))
        return band;
    // The engine's projection (ShaftsRules.h ProjectionOk): row 3 is (0, 0, -1, 0) - clip.w = -z. The depth identity the
    // march's fixture verified: d = (a*z + b) / (-z) = inverseProjection's own pair reconstructed - so nearPlane and
    // farPlane come from the rebuild through the capture's own depth 1 and depth 0.
    //
    // The live build is reversed-Z with an infinite far plane: the inverse's row 3 is (0, 0, 10, 0), so a = 0 (a finite far plane
    // would give a > 0) and the metres of a depth d are 1 / (10 d). A zero a is therefore the far plane at infinity, not a broken
    // buffer; a negative a, a b or c that is not positive, and a NaN in any of the three still are.
    const float a = inverseProjection[15];
    const float b = -inverseProjection[11];
    const float c = inverseProjection[14];
    if (!(a >= 0.0f) || !(b > 0.0f) || !(c > 0.0f) || !(a == a) || !(b == b) || !(c == c))
        return band;
    const float nearPlane = b / (a + c);
    if (!(nearPlane == nearPlane))
        return band;
    if (!(nearPlane >= kMinNearPlane && nearPlane <= kMaxNearPlane))
        return band;
    // An `a` too small to move the far plane out of float range counts as zero (the far plane b/a would be past the band's ceiling
    // anyway, and a real infinite projection stores an exact zero, possibly a negative one).
    if (a <= kInfiniteFarRatio * c)
    {
        band.usable = true;
        band.nearPlane = nearPlane;
        band.farPlane = std::numeric_limits<float>::infinity();
        return band;
    }
    const float farPlane = b / a;
    if (!(farPlane == farPlane))
        return band;
    if (!(farPlane >= kMinFarPlane && farPlane <= kMaxFarPlane))
        return band;
    if (!(farPlane > nearPlane * 10.0f))
        return band;
    band.usable = true;
    band.nearPlane = nearPlane;
    band.farPlane = farPlane;
    return band;
}

// ---- the distance mode ---------------------------------------------------------------------------------------------------------
//
// The gameplay look (the default). The first release blurred everything past a few focus distances with a disc of up to 54 px at 1080p, so the castle
// at 160 m, the sea and every wall behind the character got the full blur at every angle (measured on the user's frames: 15 % of 64-px tiles kept
// under half their detail). The distance look blurs only the far side and only by a small, capped radius:
//  - nothing nearer than the START distance is touched: the larger of kStartFocusMultiple x the focus distance and kStartFloorMetres, so the
//    character (8 m) and the walls and trees around it stay sharp whatever the focus is;
//  - beyond the start the CoC follows the thin lens, |1/start - 1/z| over |1/start - 1/far|, so it is 0 at the start (continuous, no step),
//    grows quickly at first and flattens, and saturates at 1 at the far limit (kFarOverStart x the start) and beyond;
//  - the near side never blurs (the pack's alpha is never positive, so the near half of the split stays empty);
//  - the radius is kDistanceMaxRadius of the window height at the full CoC (not the reach key's 5 %), and the mix is a share of the blurred
//    picture, kDistanceMixAtLimit x Strength, because the blur runs at a quarter of the window and keeps almost no full-resolution detail:
//    a tile keeps about (1 - mix)^2 of its detail, so a mix near 0.2 leaves the far background visibly softer and not destroyed.
// Dof.hlsl carries the same numbers and the same operations (DistanceCoC); neither file may drift (dof_pins reads them against each other).
inline constexpr float kStartFocusMultiple = 2.0f;
inline constexpr float kStartFloorMetres = 20.0f;
inline constexpr float kFarOverStart = 8.0f;
inline constexpr float kDistanceMaxRadius = 0.0075f;
inline constexpr float kDistanceMixAtLimit = 0.25f;

constexpr bool IsPositiveFinite(float value) noexcept
{
    return value == value && value > 0.0f && value < std::numeric_limits<float>::infinity();
}

// The start distance in metres for a focus distance; a focus that is missing or broken gives the floor.
constexpr float DistanceStart(float focus) noexcept
{
    if (!IsPositiveFinite(focus))
        return kStartFloorMetres;
    const float scaled = focus * kStartFocusMultiple;
    return scaled > kStartFloorMetres ? scaled : kStartFloorMetres;
}

// Where the CoC reaches 1 (0 for a start that is not a distance).
constexpr float DistanceFarLimit(float start) noexcept { return IsPositiveFinite(start) ? start * kFarOverStart : 0.0f; }

// The CoC magnitude in [0, 1] of a pixel at view depth `viewZ` for a focus: 0 up to and at the start, then (1 - start/z) / (1 - start/far), the
// thin-lens difference anchored at the start. A broken depth is no blur. Infinity is the full CoC.
constexpr float DistanceCoCMagnitude(float viewZ, float focus) noexcept
{
    if (!(viewZ > 0.0f))
        return 0.0f; // zero, negative, NaN
    const float start = DistanceStart(focus);
    if (!(viewZ > start))
        return 0.0f;
    const float coc = (1.0f - start / viewZ) / (1.0f - 1.0f / kFarOverStart);
    return coc < 0.0f ? 0.0f : (coc > 1.0f ? 1.0f : coc);
}

// The pack's alpha in this look: the far side is negative, the near side is never set.
constexpr float DistanceCoCSigned(float viewZ, float focus) noexcept { return -DistanceCoCMagnitude(viewZ, focus); }

// The disc's reach in window pixels at the full CoC: a fixed fraction of the window height.
constexpr float DistanceReachPixels(float windowHeight) noexcept
{
    return IsPositiveFinite(windowHeight) ? kDistanceMaxRadius * windowHeight : 0.0f;
}

// The strength the merge mixes with: the key for the cinematic look, the key times the share at the limit for the distance look.
constexpr float EffectiveStrength(Mode mode, float strength) noexcept
{
    const float clamped = ClampStrength(strength);
    return mode == Mode::Distance ? clamped * kDistanceMixAtLimit : clamped;
}

// ---- the auto focus' time smoothing ---------------------------------------------------------------------------------------------
//
// The distance look keeps its auto focus in a one-texel state on the GPU (no readback): each run the pass takes the centre of the window, a cross
// of kFocusTaps depth taps kFocusTapOffsetFraction of the window height apart, drops the taps that read no depth (the sky) and takes the lower median
// of the rest (FocusSample), then moves the state toward it (FocusStep). The first sample is adopted whole; a run after a gap of kFocusStaleSeconds or
// more adopts the sample whole too (the state is stale). A sample more than kFocusFarRatio times past the focus is a far outlier (the camera looked
// through a gap past the character): it is approached with the slow constant, so a glance moves the focus a fraction of a percent per frame and a
// real, lasting change of subject still arrives within a few seconds. dt is the time since the pass last RAN, not the frame time.
inline constexpr float kFocusTimeConstant = 0.4f;
inline constexpr float kFocusFarTimeConstant = 1.6f;
inline constexpr float kFocusFarRatio = 3.0f;
inline constexpr float kFocusStaleSeconds = 1.0f;
inline constexpr float kFocusTapOffsetFraction = 0.015f;
inline constexpr uint32_t kFocusTaps = 5;

// The weight of one step, 1 - exp(-dt / tau): 0 for no time (or a broken time), 1 for a stale gap or no time constant.
inline float FocusAlpha(float dt, float tau) noexcept
{
    if (!(dt > 0.0f))
        return 0.0f;
    if (dt >= kFocusStaleSeconds || !(tau > 0.0f))
        return 1.0f;
    return 1.0f - std::exp(-dt / tau);
}

inline float FocusStep(float previous, float sample, float dt) noexcept
{
    const bool havePrevious = IsPositiveFinite(previous);
    if (!IsPositiveFinite(sample))
        return havePrevious ? previous : 0.0f; // the sky (no sample): the focus holds
    if (!havePrevious)
        return sample; // nothing before: adopt it
    const float tau = sample > previous * kFocusFarRatio ? kFocusFarTimeConstant : kFocusTimeConstant;
    return previous + (sample - previous) * FocusAlpha(dt, tau);
}

// The lower median of the taps that read a depth (0 when none did): the middle one of an odd count, the nearer of the two middle ones of an even count.
constexpr float FocusSample(const float* taps, uint32_t count) noexcept
{
    if (taps == nullptr || count == 0)
        return 0.0f;
    constexpr uint32_t kMaxTaps = 8;
    if (count > kMaxTaps)
        count = kMaxTaps;
    float valid[kMaxTaps] {};
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (IsPositiveFinite(taps[i]))
            valid[kept++] = taps[i];
    if (kept == 0)
        return 0.0f;
    for (uint32_t i = 1; i < kept; ++i)
    {
        const float key = valid[i];
        uint32_t j = i;
        while (j > 0 && valid[j - 1] > key)
        {
            valid[j] = valid[j - 1];
            --j;
        }
        valid[j] = key;
    }
    return valid[(kept - 1) / 2];
}

// ---- the engine's own depth of field ---------------------------------------------------------------------------------------------
//
// The game's classic depth of field (Weight2, DownScale4x4, the far blur, DiscBlurNear, Merge2) writes its result back into the scene texture the tone map
// reads, so this stage would blur an already blurred picture (on the benchmark's cutscenes the two together took the high frequencies to x0.32-0.70). The
// pass that makes the picture is the weight pass: a pixel shader that declares the family's parameter buffer (cWeightSamplingParam) AND reads a picture
// (sInput), and draws to a scene-sized target. The 256x1 CoC LUT update declares the buffer alone and writes no picture, so it never counts. The stage
// stands down for the frame the weight pass drew in (the whole engine chain is then the frame's depth of field).
constexpr bool IsEngineDofLayout(bool declaresWeightParam, bool declaresInput) noexcept { return declaresWeightParam && declaresInput; }

constexpr bool IsSceneSized(uint32_t targetWidth, uint32_t targetHeight, uint32_t sceneWidth, uint32_t sceneHeight) noexcept
{
    return targetWidth != 0 && targetHeight != 0 && targetWidth == sceneWidth && targetHeight == sceneHeight;
}

// The flag is the stage's own frame counter value at the draw, so it needs no reset: a flag from an earlier frame reads false.
constexpr bool EngineDofRanThisFrame(bool have, uint64_t engineFrame, uint64_t frame) noexcept { return have && engineFrame == frame; }

// ---- the disc pattern, the quarter res and the reach ----------------------------------------------------------------------

// The quarter-resolution extent of an extent in pixels: an odd extent keeps its last half pixel (the march's HalfExtent
// rule, the blur's own size), a zero extent stays zero instead of becoming a one-texel pass.
constexpr uint32_t QuarterExtent(float extent) noexcept
{
    if (!(extent >= 2.0f))
        return 0;
    return uint32_t((extent * 0.25f) + 0.9999f);
}

// The disc's reach, in window pixels: the blur's taps lay their discs in the pack's own (full-resolution) space, which
// is what a single tap's own coverage is compared against and what the shader's offset arithmetic uses. One key, one
// circle, in the render window's own pixels.
constexpr float ReachPixels(float windowHeight, float aperture) noexcept
{
    if (!(windowHeight > 0.0f) || !(aperture > 0.0f))
        return 0.0f;
    return ClampAperture(aperture) * windowHeight;
}

// The radius at the full CoC for a look: the distance look ignores the reach key.
constexpr float ReachPixelsFor(Mode mode, float windowHeight, float aperture) noexcept
{
    return mode == Mode::Distance ? DistanceReachPixels(windowHeight) : ReachPixels(windowHeight, aperture);
}

// What the capture frame measured, for the stage line (the dump writes it after the snapshot of the keys): the focus the frame used, the raw centre
// sample, the start distance and the far limit (distance look), the radius at the full CoC in pixels and whether the engine's own depth of field ran.
struct Measured
{
    Mode mode = Mode::Distance;
    bool autoFocus = true;
    float focus = 0.0f;  // the focus the frame used, metres (0 = not known)
    float sample = 0.0f; // the raw centre sample of the frame, metres (0 = none)
    float windowHeight = 0.0f;
    float aperture = kDefaultAperture;
    bool engineDof = false;
};

inline std::string FormatMeasured(const Measured& measured) noexcept
{
    const auto metres = [](float value, const char* unknown) {
        return IsPositiveFinite(value) ? std::format("{:.2f}", double(value)) : std::string(unknown);
    };
    const std::string focus = metres(measured.focus, "n/a");
    const std::string sample = measured.autoFocus ? metres(measured.sample, "n/a") : std::string("n/a");
    std::string start = "n/a", farText = "n/a";
    if (measured.mode == Mode::Distance)
    {
        const float startMetres = DistanceStart(measured.focus);
        start = std::format("{:.2f}", double(startMetres));
        farText = std::format("{:.1f}", double(DistanceFarLimit(startMetres)));
    }
    return std::format("focusM={} centreM={} start={} farM={} maxPx={:.1f} engineDof={}", focus, sample, start, farText,
                       double(ReachPixelsFor(measured.mode, measured.windowHeight, measured.aperture)), measured.engineDof ? 1 : 0);
}

// The disc: tap i's angle on a golden-angle sweep and its radius on a sqrt ramp, so the N taps cover the disc's area
// uniformly (Vogel's distribution, the standard disc pattern a disc blur draws its taps with). Pure, so the shader and
// the host's reference agree to the last float at every tap count, and a test can pin the exact tap table.
inline constexpr float kGoldenAngle = 2.39996333f; // (sqrt(5) - 1) * pi, kept to the float's own rounding

constexpr void DiscTap(uint32_t index, uint32_t steps, float reachQuarter, float& angle, float& radius) noexcept
{
    if (!(steps > 0) || !(reachQuarter > 0.0f) || index >= steps)
    {
        angle = 0.0f;
        radius = 0.0f;
        return;
    }
    angle = float(index) * kGoldenAngle;
    radius = std::sqrt((float(index) + 0.5f) / float(steps)) * reachQuarter;
}

// A tap's weight, from its own CoC: a perfectly focused texel blurs nothing, so in-focus surroundings never haze a sharp
// edge, and the strongest defocus weighs the most (the pack's tap-level alpha; the merge adds the centre's own).
constexpr float TapWeight(float tapCoc) noexcept
{
    if (!(tapCoc == tapCoc))
        return 0.0f;
    const float weight = std::fabs(tapCoc);
    return weight > 1.0f ? 1.0f : weight;
}

// Whether a tap belongs to this centre: its own defocus (|c_tap|, in the pack's unit CoC) scaled to the disc's reach in
// pixels covers the tap's distance on the disc pattern. The distance is the pre-snap radius the DiscTap layout gave the
// tap, the same float the shader and the host reference run.
constexpr bool TapReaches(float distance, float reachTap) noexcept
{
    if (!(distance >= 0.0f) || !(distance == distance) || !(reachTap >= 0.0f))
        return false;
    return distance <= reachTap;
}

// ---- the merge ------------------------------------------------------------------------------------------------------------

// The mix amounts at one pixel: strength times the full-resolution alpha's own half (the pack's CoC, read at full res -
// the game's own Merge2 reads the pack beside the quarter targets for exactly this reason: the amount map stays at full
// resolution even though the blur itself lives at a quarter). Exactly one of the two is ever non-zero, and Strength 0
// answers both at 0 - which is the key's off, and the pass then does not run at all.
constexpr void MergeAmounts(float cocSigned, float strength, float& nearAmount, float& farAmount) noexcept
{
    const float strengthClamped = ClampStrength(strength);
    nearAmount = NearWeight(cocSigned) * strengthClamped;
    farAmount = FarWeight(cocSigned) * strengthClamped;
}

// The merged colour, guarded: a quarter target's answer that cannot be read (a NaN texel, an unfilled neighbourhood)
// mixes nothing instead of burning the pixel with a garbage tap. The scene's own value (the chain's output, never the
// pack's capped copy) is what an in-focus pixel returns byte for byte, and the mix's own algebra is exact at the bounds.
constexpr void DofMix(const float scene[3], const float blurred[3], float amount, float out[3]) noexcept
{
    if (!(amount > 0.0f) || !(amount == amount))
    {
        out[0] = scene[0];
        out[1] = scene[1];
        out[2] = scene[2];
        return;
    }
    if (!(blurred[0] == blurred[0]) || !(blurred[1] == blurred[1]) || !(blurred[2] == blurred[2]))
    {
        out[0] = scene[0];
        out[1] = scene[1];
        out[2] = scene[2];
        return;
    }
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = scene[i] + (blurred[i] - scene[i]) * amount;
}

// ---- the render window ----------------------------------------------------------------------------------------------------

// Whether the window's quarter targets can be made at all: the extent must be at least two (QuarterExtent's own rule),
// and the window's own rules are the shafts' (the tone-map shape asserts WindowUsable first; this is an extra belt for
// the pass's own inputs).
constexpr bool QuarterTargetsPossible(const shafts::Window& window) noexcept
{
    return QuarterExtent(window.width) > 0 && QuarterExtent(window.height) > 0 && shafts::WindowUsable(window);
}

// ---- the outcomes, the declines, and the log lines ------------------------------------------------------------------------

// Why the stage left a draw alone. Every one of these leaves the chain exactly as it was, with one log line per state
// change (the march's own log gate and the march's own wordings).
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,   // the key is off, or the strength or the aperture is 0, or the stage stood down for the session
    NotToneMap,   // the draw is not the tone map's shape
    RenderTarget, // the tone map's render target is not one post-tone texture
    TargetFormat, // the tone map's target is not FP16
    SceneFormat,  // the scene is not FP16
    SizeMismatch, // the scene, the depth and the render window do not belong to one frame
    Window,       // the render window is empty, too small or does not fit the scene
    CharaReplay,  // the draw is a character-preview replay
    SceneHdr,     // the scene HDR path replaces the tone chain this stage draws before
    NoDepth,      // the fog post has not given this module a depth of the scene's own size yet
    NoCamera,     // the light draws' camera bytes did not hold a camera the band accepts (a build mismatch)
    PassFailed,   // the pass could not build its textures or run its draws
    Exception,    // a call threw
    EngineDof,    // the engine's own depth of field drew in this frame (its weight pass), so this stage stands down for the frame
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the strength or the aperture is 0";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene, the depth and the render window do not belong to one frame";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::SceneHdr: return "the engine's scene HDR path is on, which replaces this stage's tone chain";
    case Decline::NoDepth: return "no depth of the scene's own size was secured by the fog post";
    case Decline::NoCamera: return "the light draws' camera buffer did not hold a camera this build can read";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draws";
    case Decline::Exception: return "a call threw";
    case Decline::EngineDof: return "the engine's own depth of field drew in this frame";
    }
    return "unknown";
}

// What the stage's decline means for the chain: the march's own four outcomes, the same names, the same semantics. A
// chained HoldStands leaves the caller's texture at t0 (the chain's own hold stands), an owner EngineOwn leaves the
// game's own binding and draws nothing (the owner returns false and the caller draws the game's call once), a Skipped frame is a frame without the stage in it (not
// counted against the stand-down).
enum class Outcome : uint8_t
{
    Answered = 0,
    HoldStands,
    EngineOwn,
    Skipped,
};

constexpr const char* OutcomeName(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::Answered: return "answered";
    case Outcome::HoldStands: return "the chain's hold stands";
    case Outcome::EngineOwn: return "the frame is the engine's own";
    case Outcome::Skipped: return "skipped";
    }
    return "unknown";
}

// Which declines count toward the stand-down: a shape the stage cannot serve (a camera this build cannot read, no depth,
// a tone-map draw whose target or scene the pack refuses) is a build mismatch or a capture that stopped landing, while
// "the draw is not the tone map's" and "the frame has no DoF in it" are not failures at all. The march's own counting
// rule, on this module's own knob.
constexpr bool CountsTowardsStandDown(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::RenderTarget:
    case Decline::TargetFormat:
    case Decline::SceneFormat:
    case Decline::SizeMismatch:
    case Decline::Window:
    case Decline::NoDepth:
    case Decline::NoCamera:
    case Decline::PassFailed:
        return true;
    case Decline::None:
    case Decline::FeatureOff:
    case Decline::NotToneMap:
    case Decline::CharaReplay:
    case Decline::SceneHdr:
    case Decline::Exception:
    case Decline::EngineDof:
        return false;
    }
    return false;
}

// What the stage skipped the frame for, without counting anything against it (the march's own decomposition).
enum class SkipReason : uint8_t
{
    None = 0,   // not skipped
    NoDepth,    // the fog post has secured no depth or no mapping row of this scene's size yet
    NoCamera,   // the light draws never landed a camera the band accepts
};

constexpr SkipReason SkipOf(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::NoDepth: return SkipReason::NoDepth;
    case Decline::NoCamera: return SkipReason::NoCamera;
    default: return SkipReason::None;
    }
}

constexpr const char* SkipName(SkipReason reason) noexcept
{
    switch (reason)
    {
    case SkipReason::None: return "nothing to skip";
    case SkipReason::NoDepth: return "the fog post's depth or mapping row has not been secured yet";
    case SkipReason::NoCamera: return "the frame's camera rows never landed or have stopped landing";
    }
    return "unknown";
}

// The stage's own counts, moved once per skip so the card's numbers are a whole (the march's own rule).
struct SkipCounts
{
    uint32_t skipped = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
};

constexpr void CountSkip(SkipCounts& counts, SkipReason reason) noexcept
{
    switch (reason)
    {
    case SkipReason::NoDepth: ++counts.noDepth; break;
    case SkipReason::NoCamera: ++counts.noCamera; break;
    case SkipReason::None: break;
    }
    if (reason != SkipReason::None)
        ++counts.skipped;
}

// The one line a declined tone-map draw writes. The two outcomes carry the two wordings the chain's hold semantics name
// (the march's own pins pin them; this stage's pins do the same).
inline std::string FormatDecline(Decline reason, Outcome outcome) noexcept
{
    if (outcome == Outcome::HoldStands)
        return std::format("Depth of field: the draw was left alone, so the chain's hold stands ({})", DeclineName(reason));
    return std::format("Depth of field: the draw was left alone, so the frame is the engine's own ({})", DeclineName(reason));
}

// The line of a skipped frame.
inline std::string FormatSkip(SkipReason reason) noexcept
{
    return std::format("Depth of field: the frame has no depth of field in it ({})", SkipName(reason));
}

// What one run measured, for the run line and the card.
struct RunFacts
{
    Mode mode = Mode::Distance;
    const char* focusMode = FocusModeName(FocusMode::Auto);
    float focus = kDefaultFocusDistance;
    float strength = kDefaultStrength;
    float aperture = kDefaultAperture;
    uint32_t steps = uint32_t(kDefaultSteps);
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t quarterWidth = 0, quarterHeight = 0;
};

inline std::string FormatRun(const RunFacts& facts) noexcept
{
    return std::format("Depth of field: mode {}, packed and blurred the window's {}x{} at a quarter ({}x{}), {} taps at "
                       "aperture {:.3f}, strength {:.2f}, focus mode {}{}",
                       ModeName(facts.mode), facts.windowWidth, facts.windowHeight, facts.quarterWidth, facts.quarterHeight, facts.steps,
                       facts.aperture, facts.strength, facts.focusMode,
                       facts.focusMode == FocusModeName(FocusMode::Manual)
                           ? std::format(" at {:.1f} m", facts.focus)
                           : std::string(" (the view depth under the screen centre)"));
}

// The card's one line: this stage's own counts (the march's own decomposition, on this stage's own facts).
struct CardCounts
{
    uint64_t frames = 0;
    uint32_t runs = 0;
    uint32_t declined = 0;
    uint32_t skipped = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
    uint32_t refusals = 0; // the camera refusals (a build mismatch), counted toward the stand-down
    bool armed = false;
    float strength = kDefaultStrength;
    float aperture = kDefaultAperture;
    uint32_t steps = uint32_t(kDefaultSteps);
    FocusMode focusMode = FocusMode::Auto;
    Mode mode = Mode::Distance;
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t quarterWidth = 0, quarterHeight = 0;
};

inline std::string FormatCard(const CardCounts& counts) noexcept
{
    return std::format("Depth of field: {}{} frames, {} runs, {} declined, {} skipped (depth {}, camera {}), {} refused; "
                       "window {}x{}, quarter {}x{}, strength {:.2f}, aperture {:.3f}, {} taps, focus mode {}, mode {}",
                       counts.armed ? "" : "off for this session: ", counts.frames, counts.runs, counts.declined,
                       counts.skipped, counts.noDepth, counts.noCamera, counts.refusals, counts.windowWidth,
                       counts.windowHeight, counts.quarterWidth, counts.quarterHeight, counts.strength, counts.aperture,
                       counts.steps, FocusModeName(counts.focusMode), ModeName(counts.mode));
}

// The session's own GPU-cost line, once a session when the measurement lands.
inline std::string FormatCost(bool haveGpuMilliseconds, double gpuMilliseconds, uint64_t frames, uint32_t runs,
                              uint32_t declined) noexcept
{
    const std::string gpu = haveGpuMilliseconds
                                ? std::format(", the pass {:.4f} ms on the GPU", gpuMilliseconds)
                                : std::string(", its GPU time not measured yet");
    return std::format("Depth of field: started on the tone-map chain ({} runs, {} declined over {} frames{})", runs,
                       declined, frames, gpu);
}

// The stage's own scene-HDR wording (the march's own rule, this stage's log namespace).
inline constexpr const char* kSceneHdrInactive =
    "Depth of field: inactive while the engine's scene HDR is on, because that path replaces the tone map this stage draws before";

// The camera mismatch's wording, once a session.
inline constexpr const char* kCameraRefused =
    "Depth of field: the light draws' camera bytes arrived and are not a camera this build reads (a build mismatch), "
    "so the stage keeps the last accepted camera for the hold's frames and refuses otherwise";

// The scene-HDR and camera refusal lines, as strings for the one std::format call the log takes.
inline std::string FormatSceneHdrInactive() noexcept { return kSceneHdrInactive; }
inline std::string FormatCameraRefused() noexcept { return kCameraRefused; }
} // namespace ext::ffxiv::dof
