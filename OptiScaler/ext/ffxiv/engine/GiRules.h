#pragma once
// Screen-space light bounce (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the module does, in one line: at a tone-map draw the tone-map chain reaches it on (its chained role, behind the volumetric march, or
// its fallback-owner role when no earlier stage took the draw), it composes its own screen-space bounce light into a copy of the scene the
// chain already produced, bounded by the frame's white point, and no game texture is ever written. Per pixel, at a half (or quarter) of the
// render window: the origin is the composite's own view position, K rays leave the tangent hemisphere of the G-buffer normal each cosine out
// of it, each ray marches the scene's view positions up to the range key with thickness rejection, and a hit's radiance toward the origin is
// the hit texel's own linear scene value: the scene's stored texel squared. The engine's deferred composite sums in a squared-light domain,
// writes the square root of the sum, and its tone map squares the scene back on read, so the stored texel squared is the radiance the surface
// sends toward the camera, and equally toward any other surface that sees it. That radiance is the surface's albedo times the light that
// reached it (plus its specular and emissive terms, which are not separated), so its own colour is IN the value: a warm floor sends warm light
// and a dark one little. The origin's own (albedo store)^2 is multiplied at the end. The accumulated irradiance is smoothed by a dense 5x5
// binomial stencil (every position of the square, centre included) that averages every texel that was TRACED - a texel whose rays all missed is
// a valid zero and weighs in as one - and skips only the origins that could not be traced, and is added to the scene through the radial shafts'
// own white-bounded addition, so no key can push a channel past the white the pass reads.
//
// An earlier version divided the hit's squared scene value by the albedo carrier's squared store to get "the light that hit the surface
// without the surface's own colour". That dropped the albedo the surface applies on the way out, so it overstated every hit by 1/albedo (2x to
// 5x indoors), removed the colour bleeding and pushed the colour toward the neutral incident light: a warm room got a blue-grey veil. It also
// skipped the traced texels whose rays all missed as "no data", which spread a lone hit over its whole neighbourhood at full strength. Both
// are gone. The engine's composite whose bytecode this fork had disassembled for its GI study carries the facts used here: the albedo carrier
// enters SQUARED into the composition, the final sum is written SQUARE-ROOTED, and the tone map squares the scene back on read.
//
// The fail-open contract: every reason in Decline leaves the game's draw exactly as the chain left it - the chain's last good picture when
// the stage is chained (the hold stands), the game's own binding when the stage owns the draw - with one log line for the reason. A declined
// path never touches the chain's shared state: the tone-map chain's capture stamp is the puddle stage's own file-local state, unreachable
// from this module, and this stage's answered-tracking is its own rendered-frame counter (the stamping contract, the march's own).

#include "ext/ffxiv/engine/EnginePassRules.h" // kConsecutiveDeclineLimit, this module's stand-down count
#include "ext/ffxiv/engine/ShaftsRules.h"     // ShaftAdded (the compose bound), Window, the tone map's facts and formats

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::engine::gi
{
// ---- the keys ------------------------------------------------------------------------------------------------------------

// The rays one pixel casts, clamped: fewer than 1 is no effect at all, more than 8 costs more than the difference is worth (the occlusion
// pass's quality band, this pass's own key).
inline constexpr int32_t kMinRays = 1;
inline constexpr int32_t kMaxRays = 8;
inline constexpr int32_t kDefaultRays = 4;

// The march's steps, clamped: fewer than 8 rings visibly at the default range, more than 32 costs more than the picture gains.
inline constexpr int32_t kMinSteps = 8;
inline constexpr int32_t kMaxSteps = 32;
inline constexpr int32_t kDefaultSteps = 16;

// The march's reach, in view metres, clamped: beyond it a ray ends without a hit and carries nothing. The march's own surface bias ("a ray
// that starts on the surface reads its own texel") is not a key: it is this pass's own constant, like the march's.
inline constexpr float kMinRange = 1.0f;
inline constexpr float kMaxRange = 20.0f;
inline constexpr float kDefaultRange = 5.0f;

// The strength: 0 to 1, the share of the pass's own gain that reaches the picture. 0 runs nothing (the frame costs one test per draw, the
// march's rule); 1 is the full diffuse bounce (kGain below). The default 0.55 (the 2026-11-07 integration's choice, on the user's four 2026-11-06
// frames in the replay lab, stage 09) is the lowest strength whose bounce stays visible on the bright room (capture 3: the lamp's wall +7.1 %, the
// fireplace's +4.6 % of their stored luminance; 8.9 % / 5.6 % at 0.70, 12.3 % / 7.5 % at 1.0) AND whose speckle excess over a constant-irradiance
// control stays under 0.01 % on the rain square (capture 2: 0.0041 % at 0.55; 0.70 gave 0.0115 %, a miss). Flat open areas stay under 1 %.
inline constexpr float kMinStrength = 0.0f;
inline constexpr float kMaxStrength = 1.0f;
inline constexpr float kDefaultStrength = 0.55f;

// The pass's own resolution: half of the render window (the default, the tab's choice) or a quarter of it.
enum class Resolution : uint8_t
{
    Half = 0,
    Quarter = 1,
};

constexpr Resolution ParseResolution(std::string_view text) noexcept
{
    return text == "quarter" || text == "Quarter" || text == "QUARTER" ? Resolution::Quarter : Resolution::Half;
}

constexpr const char* ResolutionName(Resolution resolution) noexcept
{
    return resolution == Resolution::Quarter ? "quarter" : "half";
}

constexpr uint32_t ClampRays(int32_t value) noexcept
{
    const int32_t rays = value < kMinRays ? kMinRays : (value > kMaxRays ? kMaxRays : value);
    return uint32_t(rays);
}

constexpr uint32_t ClampSteps(int32_t value) noexcept
{
    const int32_t steps = value < kMinSteps ? kMinSteps : (value > kMaxSteps ? kMaxSteps : value);
    return uint32_t(steps);
}

constexpr float ClampRange(float value) noexcept
{
    if (value != value)
        return kDefaultRange;
    return value < kMinRange ? kMinRange : (value > kMaxRange ? kMaxRange : value);
}

constexpr float ClampStrength(float value) noexcept
{
    if (value != value)
        return kDefaultStrength;
    return value < kMinStrength ? kMinStrength : (value > kMaxStrength ? kMaxStrength : value);
}

// The parameters one session runs with, from the keys. The master switch is not part of them: it decides whether the module is armed.
struct Params
{
    uint32_t rays = uint32_t(kDefaultRays);
    uint32_t steps = uint32_t(kDefaultSteps);
    float range = kDefaultRange;
    float strength = kDefaultStrength;
    Resolution resolution = Resolution::Half;
};

constexpr Params BuildParams(int32_t rays, int32_t steps, float range, float strength, std::string_view resolution) noexcept
{
    Params params;
    params.rays = ClampRays(rays);
    params.steps = ClampSteps(steps);
    params.range = ClampRange(range);
    params.strength = ClampStrength(strength);
    params.resolution = ParseResolution(resolution);
    return params;
}

// A strength of zero applies no light at all, so the pass does not run, nothing is created and no draw is composed; written so a NaN reads
// as the default, like every other knob of this family.
constexpr bool IsIdentity(const Params& params) noexcept { return !(params.strength > 0.0f); }

constexpr bool Wants(const Params& params) noexcept { return !IsIdentity(params); }

// The module is wanted when the master key is on and the strength asks for something (the occlusion and march modules' own shape).
constexpr bool Wanted(bool enabled, const Params& params) noexcept { return enabled && Wants(params); }

// The pass target's extent of a render-window extent: the march's own rule, an odd extent keeps its last half pixel (the shaders clamp their
// reads), and a zero extent stays zero instead of becoming a one-texel pass.
constexpr uint32_t GiExtent(float extent, Resolution resolution) noexcept
{
    const float divide = resolution == Resolution::Half ? 2.0f : 4.0f;
    if (!(extent >= divide))
        return 0;
    return uint32_t((extent / divide) + 0.9999f);
}

// ---- the pass's own constants (fixed, so the picture cannot drift with a number the ini does not name) --------------------

// The march's own surface bias, in metres: a ray point has crossed a surface only once it lies this far behind it along the camera's own
// line, and the ray's first tap sits this far along its direction, so a surface never crosses itself (the march's own constant, this pass's
// number).
inline constexpr float kSurfaceBias = 0.02f;

// The bias also grows with the surface's own distance from the camera, by this share of it: a far surface is sampled coarser (a texel row
// spans more depth), and without the share a flat floor would cross itself at the first steps of a grazing ray. PROVISIONAL: the in-game
// look settles it.
inline constexpr float kDepthBias = 0.02f;

// The march's thickness rejection, in metres: a ray point farther behind the surface than the bias plus this is another object's wall, not
// the surface the ray crossed (fixed here because the keys the card names do not list it).
inline constexpr float kThickness = 0.30f;

// The crossing interval's halvings: a step's interval, halved, lands the ray on the crossed texel (the march's refine, stopped one halving
// earlier than the reflections' because this pass has more rays per step to pay for).
inline constexpr uint32_t kMaxRefine = 4;

// ---- the camera the pass draws with ---------------------------------------------------------------------------------------

// The view matrix's three rows (g_CameraParameter +0x000), as the pass reads them: each row's first three floats are a rotation's row (the
// camera's own axes, about unit length), and the same test this family's other consumers put on a projection covers it. A buffer whose +0x000
// holds a translation or an inverse cannot be a view matrix this pass rotates its normals with, and is refused instead of lighting the frame
// wrong.
// METER sweep S1: what a frame does when its camera read ARRIVED and was refused (the bytes are not this build's camera matrices). With a good camera in hand (the
// last accepted state, young enough and not yet faded out) the frame stands on it, exactly as it does when the read has not landed: a refusal of one read is not a
// frame without the stage in it. With nothing to stand on the frame is declined and counted (a build mismatch). The depth of field already did this; the light
// bounce declined at once and counted the frame toward its stand-down streak.
constexpr bool RefusedCameraUsesHold(bool haveHold, uint32_t holdAge, float holdFade) noexcept
{
    return haveHold && holdAge <= shafts::kHoldFrames && holdFade > 0.0f;
}

constexpr bool ViewRowsOk(const float view[12]) noexcept
{
    if (view == nullptr)
        return false;
    for (uint32_t row = 0; row < 3; ++row)
    {
        const float x = view[row * 4 + 0];
        const float y = view[row * 4 + 1];
        const float z = view[row * 4 + 2];
        const float lengthSquared = x * x + y * y + z * z;
        if (!(lengthSquared > 0.9604f) || !(lengthSquared < 1.0404f)) // 0.98^2 .. 1.02^2, the light's own unit test
            return false;
    }
    return true;
}

// ---- the log's one-line constants -----------------------------------------------------------------------------------------

// The one line a session gets when the engine's scene HDR comes on while the stage's key is on: that path replays the engine's tone chain in
// its own branch, so this stage's draw path never runs and the pass cannot draw (the march's own line, this stage's own).
inline constexpr const char* kSceneHdrInactive =
    "Light bounce: inactive while scene HDR is on, because that path replaces the tone chain this stage draws before";

// The lines the read-back rings write once a session at their own stall or staleness ends, and the camera-cut line the hold writes.
inline constexpr const char* kReadbackStale =
    "Light bounce: the frame's camera bytes stopped arriving, so the pass is leaving the frames alone";
inline constexpr const char* kReadbackNotLanding =
    "Light bounce: the camera's bytes have never landed, so the frames are left as they were";
inline constexpr const char* kCameraRefused =
    "Light bounce: the camera's bytes arrived and are not this build's camera matrices";
inline constexpr const char* kCameraRefusedHeld =
    "Light bounce: the camera's bytes arrived and are not this build's camera matrices, so the last accepted camera stands while it is young enough (a refusal with a good camera in hand is not a frame without the stage)";
inline constexpr const char* kCameraCut =
    "Light bounce: the camera cut, so the held state was dropped";

// The stand-down's line, with the count in its own wording (the march's own line, this stage's own).
inline std::string FormatSwitchedOff(uint32_t limit)
{
    return std::format("Light bounce: {} counted declines in a row were left alone, so the stage is off for this session", limit);
}

// The pass's gain: the estimator below is the hemisphere integral of the hits' radiance, an IRRADIANCE (pi times the mean radiance), and a
// Lambertian surface sends back its reflectance times that irradiance over pi. The origin's reflectance here is its albedo store squared (the
// engine's own multiplier), so the pass multiplies by 1/pi: a surface with reflectance 1 under a uniform environment of radiance L reflects
// exactly L (the white furnace), and the strength key 1.0 is that full diffuse bounce. There is no other tuned scale between the estimator and
// the picture; the default strength is the one number a session moves. (The first version's gain was 1.0, which was pi too much on top of
// the albedo it left out of every hit.)
inline constexpr float kGain = 0.31830988618379067154f; // 1 / pi

// A hit this pass cannot bound: a radiance above it (a linear scene value over 8, a stored value over 2.83) is a specular glint or a lamp core
// the pass will not carry, so the sample carries nothing - it is refused, not scaled down. On the four 11/05 captures the share of surface
// pixels over it is 0.0013% (c1), 0 (c2, c3) and 0.07% (c4, the inn's lamps), and 99.9% of surface pixels sit under a linear 6.
inline constexpr float kMaxRadiance = 8.0f;

// The smooth's fixed shape: a dense 5x5 stencil in irradiance texels, every position of the square once, row by row, as exact integers. The
// weight of a tap is the 1-4-6-4-1 row at its |dx| times the row at its |dy| (a binomial: the discrete form of a gaussian), so the texel's own
// tap and its four direct and four diagonal neighbours are all in, nearer taps weigh more, the weights sum to 256, and exactly half of that
// (128) sits on the taps with an odd x+y. The balance is the point: the stencil's response at the checkerboard frequency, and at the
// one-texel stripes along each axis, is exactly zero. A stencil whose taps all have an even x+y (the first version's: its (+-1.5,+-1.5)
// offsets were truncated to (+-1,+-1)) never mixes a texel with a neighbour of the other parity, filters the two classes as two independent
// noise fields and prints them interleaved as a diamond lattice. The taps stay inside the irradiance target's own extent: an outside tap is
// skipped rather than clamped (the occlusion pass's own edge rule), a tap without data is skipped, and the sum is divided by the weights that
// were present. The shader carries the same two tables under the same names; the pins compare them element by element.
inline constexpr uint32_t kSmoothTaps = 25;
inline constexpr int32_t kSmoothRadius = 2; // the stencil reaches this many texels each way
inline constexpr int32_t kSmoothOffsets[kSmoothTaps][2] {
    { -2, -2 }, { -1, -2 }, { 0, -2 }, { 1, -2 }, { 2, -2 },
    { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 2, -1 },
    { -2, 0 },  { -1, 0 },  { 0, 0 },  { 1, 0 },  { 2, 0 },
    { -2, 1 },  { -1, 1 },  { 0, 1 },  { 1, 1 },  { 2, 1 },
    { -2, 2 },  { -1, 2 },  { 0, 2 },  { 1, 2 },  { 2, 2 },
};
inline constexpr float kSmoothWeights[kSmoothTaps] {
     1.0f,  4.0f,  6.0f,  4.0f,  1.0f,
     4.0f, 16.0f, 24.0f, 16.0f,  4.0f,
     6.0f, 24.0f, 36.0f, 24.0f,  6.0f,
     4.0f, 16.0f, 24.0f, 16.0f,  4.0f,
     1.0f,  4.0f,  6.0f,  4.0f,  1.0f,
};

// One tap's weight, from the table (a tap past the set weighs nothing, so a caller indexing the whole stencil gets one answer).
inline double SmoothWeight(uint32_t tap) noexcept { return tap >= kSmoothTaps ? 0.0 : double(kSmoothWeights[tap]); }

// ---- the denoise: the outlier control and the edge-aware filter --------------------------------------------------------------------
// The irradiance one texel gets from K rays is a very noisy estimate (four rays a texel: most are traced zeros, the rest are single hits that may
// be a lamp). Gi.hlsl cleans it in two steps, both at the pass's own resolution, both single-frame (no history): PS_Clamp holds a texel's luminance
// to a multiple of its neighbours' mean, then four iterations of the a-trous wavelet filter (PS_Atrous0..3) average it with the neighbours that
// belong to the same surface. The kernel is the table above, its taps `step` texels apart; the shader carries the same numbers under the same names.

// The clamp: a texel's luminance may not exceed kClampRatio times the mean luminance of its other traced neighbours (the 24 around it, traced
// zeros included) plus kClampFloor, and its colour scales with it (energy kept below the cap). The floor is what one ray hitting an ordinary lit
// surface contributes (0.07 to 0.28 in the scene's linear units): where nearly every ray misses, that one hit is the signal, not a firefly.
inline constexpr float kClampRatio = 16.0f;
inline constexpr float kClampFloor = 0.1f;

// The scale the clamp gives a texel's colour: 1 below the cap, cap / luminance above it. A luminance that is not a finite positive number, and
// a neighbourhood mean that is not a finite number, leave the texel alone (the shader has no neighbour to compare with: it keeps the texel).
constexpr float ClampScale(float luminance, float neighbourMean) noexcept
{
    if (!(luminance > 0.0f) || luminance != luminance || !(neighbourMean >= 0.0f) || neighbourMean != neighbourMean)
        return 1.0f;
    const float cap = kClampRatio * neighbourMean + kClampFloor;
    return luminance > cap ? cap / luminance : 1.0f;
}

// The filter: four iterations, the taps one, two, four and eight texels apart, so the footprint is 2 x (1 + 2 + 4 + 8) + 1 = 31 texels across.
inline constexpr uint32_t kFilterIterations = 4;
inline constexpr int32_t kFilterSteps[kFilterIterations] { 1, 2, 4, 8 };

// A tap is another surface when its inverse depth is off the centre's depth plane by more than this share of the centre's own inverse depth (3 %).
// The plane is the one through the centre with the screen-space gradient of the inverse depth measured around it (the inverse depth of a plane is
// linear in screen position, so a floor seen at a grazing angle is one surface to its far end, and a step to a wall behind it is not). The weight
// is exp(-|inverse depth of the tap - the plane's| / (kDepthSigma * the centre's inverse depth)).
// The G-buffer normals are not an edge stop: they are shading normals, and normal-mapped ground varies by tens of degrees from texel to texel on one
// flat surface. Every normal weight tried (power 32, 4, 1) left each texel with only the neighbours that shared its bump and the filtered light
// printed streaks (grain on the flat 16x16 tiles of the rain capture 0.77 with the normal against 0.12 without it).
inline constexpr float kDepthSigma = 0.03f;

// The weight of a tap, from its inverse depth, the inverse depth the centre's plane predicts there and the centre's inverse depth. 0 for a number
// it cannot read, never 1.
inline float DepthWeight(float tapInverseDepth, float predictedInverseDepth, float centreInverseDepth) noexcept
{
    if (!(centreInverseDepth > 0.0f) || !(tapInverseDepth > 0.0f) || predictedInverseDepth != predictedInverseDepth)
        return 0.0f;
    return std::exp(-std::fabs(tapInverseDepth - predictedInverseDepth) / (kDepthSigma * centreInverseDepth));
}

// The change of the inverse depth over one texel along an axis, from the two texels either side of the centre: where both exist (an inverse depth
// above 0) the one with the smaller step (the other is across an edge and says nothing about the plane the centre is on), where only one exists
// that one, else 0.
constexpr float InverseDepthStep(float centre, float plus, float minus) noexcept
{
    const bool havePlus = plus > 0.0f;
    const bool haveMinus = minus > 0.0f;
    const float stepPlus = plus - centre;
    const float stepMinus = centre - minus;
    if (havePlus && haveMinus)
    {
        const float absPlus = stepPlus < 0.0f ? -stepPlus : stepPlus;
        const float absMinus = stepMinus < 0.0f ? -stepMinus : stepMinus;
        return absPlus <= absMinus ? stepPlus : stepMinus;
    }
    if (havePlus)
        return stepPlus;
    if (haveMinus)
        return stepMinus;
    return 0.0f;
}

// ---- the origin's albedo multiplier --------------------------------------------------------------------------------------

// The material id the engine's own composite decodes from the normal target's alpha byte ("a byte": the store 0..1 mapped to the index row
// 0..255 exactly the way the composite's own bytecode reads it).
inline float MaterialIdOf(float store) noexcept { return float(int32_t(store * 255.0f + 0.5f)); }

// A material-id byte in the engine's own light-draw band (its own row test the study identified: the band where the per-material tables
// carry their own shading formulas) or the byte beside it, the engine's own exotic row: not a plain deferred surface, so this first slice
// answers it with nothing. The generic deferred surfaces are the byte 128 the kept frame's dump measured across a quarter of the frame; the
// cleared texels carry byte 255 with a zero normal, which the sky test refuses long before this gate is asked.
constexpr bool IsSpecialMaterialId(float id) noexcept
{
    if (!(id == id))
        return false; // a NaN byte is not one of the engine's own rows this pass can name
    const int32_t row = int32_t(id);
    if (row < 0 || row > 255)
        return false;
    return (row >= 32 && row <= 44) || row == 224;
}

// The origin pixel's own composition weight: the generic material's diffuse weight (one minus the material pack's specular-albedo share)
// and the pass's answer to the engine's special rows, 0. One zero is no bounce from this pixel: the gate is an AND of the three reasons, so
// a NaN store or a NaN weight answers zero - a cleared texel the sky test missed cannot become a light source.
//
// `materialPackX` is the material pack's red channel (the selector; at or above 1.0 the engine's own composite takes the surface's colour
// from the fourth G-buffer target's own class, which this slice does not compose), `materialPackZ` its blue channel (the
// (1 - z) weight the engine's own diffuse path carries).
constexpr float OriginGate(float materialIdByte, float materialPackX, float materialPackZ) noexcept
{
    if (!(materialPackX == materialPackX) || !(materialPackZ == materialPackZ) || !(materialIdByte == materialIdByte))
        return 0.0f;
    if (IsSpecialMaterialId(materialIdByte))
        return 0.0f;
    if (materialPackX >= 1.0f)
        return 0.0f; // the engine's own special-colour class: another model owns its colour, the plain form does not
    const float diffuseShare = 1.0f - (materialPackZ < 0.0f ? 0.0f : (materialPackZ > 1.0f ? 1.0f : materialPackZ));
    return diffuseShare > 0.0f ? diffuseShare : 0.0f;
}

// The store squared, on the byte scale: the pass multiplies the origin's bounce light by this, exactly the engine's own composite does.
constexpr float AlbedoSquared(float store) noexcept { return store * store; }

// ---- the hit's radiance (the reflected radiance) --------------------------------------------------------------------------

// One hit's radiance toward the origin, one channel: the hit texel's own linear scene value, the stored texel squared (the scene is stored as
// the square root of the composed sum). Nothing is divided out - the light a surface sends away is its albedo times the light that reached
// it, and the albedo is the colour the bounce should carry. A negative or non-finite value is not a light, a value over kMaxRadiance is refused
// (a glint or a lamp core, not scaled down), and the answer for a hit that carries nothing is 0, which adds nothing to the sum while the
// origin stays traced. An emissive surface (a lamp) sends its own light, which is physically right, bounded by that clamp.
constexpr float BounceRadiance(float sceneStored) noexcept
{
    if (!(sceneStored >= 0.0f) || sceneStored != sceneStored)
        return 0.0f;
    const float radiance = sceneStored * sceneStored;
    if (!(radiance == radiance) || radiance <= 0.0f)
        return 0.0f;
    return radiance > kMaxRadiance ? 0.0f : radiance;
}

// ---- the store's data marker ----------------------------------------------------------------------------------------------

// The irradiance store's alpha says whether an origin was TRACED: the number of rays it marched, found or missed. A texel whose rays all missed
// was traced and is a valid zero (rgb 0, alpha above 0); only an origin that could not be traced (the sky, a texel with no view position, a normal
// the store does not carry, a zero fade, no ray marched) carries alpha 0, "no data". The smooth averages the traced texels - zeros included, so a
// lone hit among misses is shared over its neighbourhood instead of being carried at full strength - and skips the untraceable ones.
constexpr float DataMarker(uint32_t raysTraced) noexcept { return raysTraced > 0 ? float(raysTraced) : 0.0f; }

constexpr bool HasData(float marker) noexcept { return marker > 0.0f && marker == marker; }

// ---- the accumulation ----------------------------------------------------------------------------------------------------

inline constexpr float kPi = 3.14159265358979323846f;

// A normal that decodes to something the pass can read as a surface: the occlusion pass's own limits, on the same length squared. A cleared
// or half-written texel's store keeps zero rays (and zero bounce) - the failure direction is "no light", never a glow at a garbage texel.
constexpr bool NormalUsable(const float normal[3]) noexcept
{
    const float lengthSquared = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
    return lengthSquared > 0.5f && lengthSquared < 1.5f;
}

// The tangent basis of a view-space normal, as three unit rows (B1, B2, N): the occlusion pass's own construction (the near-axis helper,
// the cross with the normal, their cross again, each normalized), so the two passes build one basis one way. Returns false when the normal
// is not a surface, leaving the basis zeroed: no rays from it.
inline bool TangentBasis(const float normal[3], float basis[3][3]) noexcept
{
    basis[0][0] = basis[0][1] = basis[0][2] = 0.0f;
    basis[1][0] = basis[1][1] = basis[1][2] = 0.0f;
    basis[2][0] = basis[2][1] = basis[2][2] = 0.0f;
    if (!(normal[0] == normal[0]) || !NormalUsable(normal))
        return false;
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(length > 0.0f))
        return false;
    const float n[3] = { normal[0] / length, normal[1] / length, normal[2] / length };
    // The helper axis the normal is least aligned with, so the cross is never degenerate (the occlusion pass's own answer).
    float helper[3] = { 0.0f, 0.0f, 1.0f };
    if (n[2] * n[2] >= 0.81f)
    {
        helper[0] = 1.0f;
        helper[1] = 0.0f;
        helper[2] = 0.0f;
    }
    float b1[3] = { n[1] * helper[2] - n[2] * helper[1], n[2] * helper[0] - n[0] * helper[2], n[0] * helper[1] - n[1] * helper[0] };
    const float b1Length = std::sqrt(b1[0] * b1[0] + b1[1] * b1[1] + b1[2] * b1[2]);
    if (!(b1Length > 0.0f))
        return false;
    b1[0] /= b1Length;
    b1[1] /= b1Length;
    b1[2] /= b1Length;
    // The second tangent, from the two the basis already has: B2 = N x B1.
    const float b2[3] = { n[1] * b1[2] - n[2] * b1[1], n[2] * b1[0] - n[0] * b1[2], n[0] * b1[1] - n[1] * b1[0] };
    const float b2Length = std::sqrt(b2[0] * b2[0] + b2[1] * b2[1] + b2[2] * b2[2]);
    if (!(b2Length > 0.0f))
        return false;
    basis[0][0] = b1[0];
    basis[0][1] = b1[1];
    basis[0][2] = b1[2];
    basis[1][0] = b2[0] / b2Length;
    basis[1][1] = b2[1] / b2Length;
    basis[1][2] = b2[2] / b2Length;
    basis[2][0] = n[0];
    basis[2][1] = n[1];
    basis[2][2] = n[2];
    return true;
}

// The per-pixel hash, salted: the radial shafts' own jitter (the march's copy carries the origin with it: the film-shafts' own hash, a
// function of the pixel alone), with one integer salt per purpose, so the ray azimuth and the elevation are two different functions of the
// same pixel. A still frame stays still: no frame number, no history, no sampler state - the upscaler's temporal accumulation is the
// denoiser this pass never carries.
inline constexpr uint32_t kHashSaltElevation = 0x9E3779B9u;

constexpr float Hash01(float pixelX, float pixelY, uint32_t salt) noexcept
{
    uint32_t h = uint32_t(pixelX) * 1664525u + uint32_t(pixelY) * 1013904223u;
    h ^= salt;              // the salt enters where no input the game's own feeds could reach it
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// One ray's unit direction over the tangent basis, cosine-distributed (the elevation taken from the hash is where the cosine pdf draws it),
// with the azimuth's rotational jitter per pixel. Returns the ray's own cosine with the basis's normal row (the last row of `basis`),
// clamped at zero; a zero means the direction is not a ray this estimator counts. The elevation of a cosine distribution over the
// hemisphere comes from the hash by sqrt(u): one half of the rays sit above the tangent plane's midpoint and the mean ray's cosine is the
// hemisphere's own 2/3.
inline float HemisphereDirection(int32_t index, int32_t count, float azimuthHash, float elevationHash, const float basis[3][3],
                                 float out[3]) noexcept
{
    out[0] = out[1] = out[2] = 0.0f;
    if (count <= 0 || index < 0 || index >= count)
        return 0.0f;
    if (!(azimuthHash == azimuthHash) || !(elevationHash == elevationHash))
        return 0.0f;
    for (uint32_t column = 0; column < 3; ++column)
        if (!(basis[0][column] == basis[0][column]) || !(basis[1][column] == basis[1][column]) || !(basis[2][column] == basis[2][column]))
            return 0.0f;
    const float u = elevationHash > 1.0f ? 1.0f : (elevationHash < 0.0f ? 0.0f : elevationHash);
    const float elevation = std::sqrt(u);
    if (!(elevation > 0.0f))
        return 0.0f; // along the tangent plane: its own cosine is zero and the ray carries nothing by construction
    const float horizontal = std::sqrt(1.0f - elevation * elevation);
    const float phi = (float(index) + azimuthHash) / float(count) * 2.0f * kPi;
    const float along1 = horizontal * std::cos(phi);
    const float along2 = horizontal * std::sin(phi);
    out[0] = basis[0][0] * along1 + basis[1][0] * along2 + basis[2][0] * elevation;
    out[1] = basis[0][1] * along1 + basis[1][1] * along2 + basis[2][1] * elevation;
    out[2] = basis[0][2] * along1 + basis[1][2] * along2 + basis[2][2] * elevation;
    const float lengthSquared = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    if (!(lengthSquared > 0.0f) || !(lengthSquared == lengthSquared))
        return 0.0f;
    const float length = std::sqrt(lengthSquared);
    out[0] /= length;
    out[1] /= length;
    out[2] /= length;
    const float cosine = out[0] * basis[2][0] + out[1] * basis[2][1] + out[2] * basis[2][2];
    return cosine > 0.0f ? cosine : 0.0f;
}

// The view fade of one pixel: how much of its own bounce still reaches the picture while the pixel's normal turns away from the camera. A
// grazing surface dominates its neighbourhood's geometry, which the recomposition cannot separate; its estimate fades with its own
// camera-facing cosine. The view direction is minus the origin, normalized (the camera sits at the view space's own origin).
inline float ViewFade(const float origin[3], const float normalView[3]) noexcept
{
    if (!(origin[0] == origin[0]) || !(origin[1] == origin[1]) || !(origin[2] == origin[2]))
        return 0.0f;
    const float lengthSquared = origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
    if (!(lengthSquared > 1.0e-6f))
        return 0.0f;
    const float length = std::sqrt(lengthSquared);
    const float facing = (-origin[0] / length) * normalView[0] + (-origin[1] / length) * normalView[1] + (-origin[2] / length) * normalView[2];
    if (!(facing == facing))
        return 0.0f;
    return facing < 0.0f ? 0.0f : (facing > 1.0f ? 1.0f : facing);
}

// The whole estimate of one pixel, as one number in the scene's own light units: the hemisphere integral, ONE factor over the rays the keys
// asked for. The rays are cosine-distributed, so their pdf is cos/pi, and the estimator's per-ray w * (N * L) product with a hit's own
// radiance is exactly pi per ray - the two factors cancel into the one pi below. The factor divides by the rays the keys asked for, NOT by
// the pairs that hit it (a corner's shade is the estimator's own answer, not a renormalization a miss washes out; a miss adds zero). `sum` is
// what the caller accumulated per ray (the hit's radiance, with the pixel's own view fade folded in once per pixel afterwards), `pairs` the
// rays that found a hit (a pixel with nothing hit carries nothing: this pass never adds light out of a numbers book), `rays` the rays the keys
// chose. A pixel whose rays all missed answers 0 here and is still DATA in the store (DataMarker): it was traced, and the smooth averages it.
constexpr float IrradianceEstimate(float sum, uint32_t pairs, uint32_t rays) noexcept
{
    if (rays == 0 || pairs == 0 || !(sum > 0.0f) || sum != sum)
        return 0.0f;
    return sum * (kPi / float(rays));
}

// ---- the compose, through the radial's own bound -------------------------------------------------------------------------

// The light this pixel adds, in fractions of the frame's white: the smoothed estimate times the pass's gain, the origin's own albedo
// multiplier (the store squared, the engine's own form) and the strength key. The white-room bound itself is the radial's ShaftAdded
// (ShaftsRules.h), which the pass's apply shader carries and whose own tests pin; this is the only number between the estimate and it.
constexpr float AddedLightShare(float estimate, float albedoSquaredSelf, const Params& params) noexcept
{
    if (!(estimate > 0.0f) || estimate != estimate || !(albedoSquaredSelf > 0.0f) || albedoSquaredSelf != albedoSquaredSelf)
        return 0.0f;
    return estimate * albedoSquaredSelf * kGain * ClampStrength(params.strength);
}

// The stored-domain increment of a linear light. The engine's composite sums in the squared-light domain and writes the square root of the
// sum (the scene's stored values are that root), so a light L added to a pixel whose stored value is s makes the stored value sqrt(s*s + L):
// the increment is that minus s. A plain addition of L to s would under-light a dark pixel, which is exactly where a bounce is seen.
inline float StoredIncrement(float sceneStored, float linearLight) noexcept
{
    if (!(linearLight > 0.0f) || linearLight != linearLight || sceneStored != sceneStored)
        return 0.0f;
    const float stored = sceneStored > 0.0f ? sceneStored : 0.0f;
    return std::sqrt(stored * stored + linearLight) - stored;
}

// What one channel gets, in the scene's stored units: the stored-domain increment of the linear light, through the radial shafts' white-bounded
// addition (head = white - scene, add = head * raw / (raw + head), raw = the increment), so the sum stays strictly below the white and a pixel
// already at white gets nothing. The slope at zero is the increment itself.
inline float BoundedAdd(float sceneStored, float linearLight, float whitePoint) noexcept
{
    if (!(whitePoint > 0.0f))
        return 0.0f;
    return shafts::ShaftAdded(sceneStored, StoredIncrement(sceneStored, linearLight) / whitePoint, whitePoint);
}

// ---- the capture frame's numbers (the stage line, the log) -------------------------------------------------------------------------
// On the frame an F9 dump captures, and on that frame only, the pass reads its own half-resolution targets back once (a staging copy, no per-frame
// readback, no GPU reduction) and these two numbers come from them: how many of the traced texels the outlier control held back, and the mean light the
// filtered estimate carries. Pure CPU, so a host test decides them.
struct CaptureStats
{
    uint32_t traced = 0;    // the texels that carry data
    uint32_t clamped = 0;   // of those, the texels the clamp held back (a scale below 1)
    double meanLight = 0.0; // over the traced texels: the filtered estimate's luminance times the strength key times the gain - the light the apply
                            // multiplies by the pixel's albedo, before the white bound (the scene's linear units)
    double ClampedShare() const noexcept { return traced > 0 ? double(clamped) / double(traced) : 0.0; }
};

inline double LuminanceOf(float red, float green, float blue) noexcept
{
    return 0.2126 * double(red) + 0.7152 * double(green) + 0.0722 * double(blue);
}

// `raw` is the irradiance as PS_Gi wrote it and `filtered` the final filtered one, both `width * height` RGBA float texels, row-major; `lightScale` is the
// strength key times kGain. A texel is traced when its raw marker (alpha) is above 0. The clamp is replayed on the raw texels with the shader's own
// rule: the other traced texels of the 5x5 neighbourhood (taps outside the extent skipped) and ClampScale; a texel with no traced neighbour is kept.
inline CaptureStats ComputeCaptureStats(const float* raw, const float* filtered, uint32_t width, uint32_t height, float lightScale) noexcept
{
    CaptureStats stats;
    if (raw == nullptr || filtered == nullptr || width == 0 || height == 0)
        return stats;
    double lightSum = 0.0;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const float* centre = raw + (size_t(y) * width + x) * 4;
            if (!HasData(centre[3]))
                continue;
            ++stats.traced;
            lightSum += LuminanceOf(filtered[(size_t(y) * width + x) * 4], filtered[(size_t(y) * width + x) * 4 + 1], filtered[(size_t(y) * width + x) * 4 + 2]);
            double neighbourSum = 0.0;
            uint32_t neighbours = 0;
            for (int32_t dy = -kSmoothRadius; dy <= kSmoothRadius; ++dy)
                for (int32_t dx = -kSmoothRadius; dx <= kSmoothRadius; ++dx)
                {
                    const int32_t tx = int32_t(x) + dx, ty = int32_t(y) + dy;
                    if ((dx == 0 && dy == 0) || tx < 0 || ty < 0 || tx >= int32_t(width) || ty >= int32_t(height))
                        continue;
                    const float* tap = raw + (size_t(ty) * width + size_t(tx)) * 4;
                    if (!HasData(tap[3]))
                        continue;
                    neighbourSum += LuminanceOf(tap[0], tap[1], tap[2]);
                    ++neighbours;
                }
            if (neighbours == 0)
                continue;
            if (ClampScale(float(LuminanceOf(centre[0], centre[1], centre[2])), float(neighbourSum / double(neighbours))) < 1.0f)
                ++stats.clamped;
        }
    stats.meanLight = stats.traced > 0 ? lightSum / double(stats.traced) * double(lightScale) : 0.0;
    return stats;
}

// The stage line's words ("name=value" words, the values string's own shape) and the log line.
inline std::string FormatCaptureStats(const CaptureStats& stats)
{
    return std::format("clamped={:.2f}% bounce={:.5f}", stats.ClampedShare() * 100.0, stats.meanLight);
}

inline std::string FormatCaptureLine(const CaptureStats& stats)
{
    return std::format("Light bounce: capture frame, the outlier control held back {:.2f} % of the {} traced texels, the filtered bounce carries a mean light of {:.5f}",
                       stats.ClampedShare() * 100.0, stats.traced, stats.meanLight);
}

// ---- the outcomes, the declines, the skips and the log lines --------------------------------------------------------------

// What one tone-map draw came to for this stage. The chain's hold semantics name two of these, the same way they name the march's:
//   Answered   - the stage ran and its own texture is what t0 reads for that one draw;
//   HoldStands - the stage declined as a chain member: the texture it was handed (the chain's last good picture) stays at t0. The stage does
//                not fall through to the engine's texel and does not bind an unlit scene; the hold stands.
//   EngineOwn  - the stage declined as the fallback owner: the game's own binding stands (the brief's fail-open; the hold applies to the
//                chained case).
//   Skipped    - the frame has nothing to compose (no G-buffer views, no depth, no camera bytes yet): not a refusal, not counted against the
//                module's streak, and the same bind semantics as the outcome it would have had.
enum class Outcome : uint8_t
{
    Answered = 0,
    HoldStands = 1,
    EngineOwn = 2,
    Skipped = 3,
};

constexpr const char* OutcomeName(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::Answered: return "answered";
    case Outcome::HoldStands: return "declined, the chain's hold stands";
    case Outcome::EngineOwn: return "declined, the frame is the engine's own";
    case Outcome::Skipped: return "skipped";
    }
    return "unknown";
}

// Why a tone-map draw was left alone. Every one of these leaves the game's draw exactly as the chain left it, and each is one line of the
// log the first time it happens.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the key is off, the strength is 0, or the module stood down for the session
    NoGBuffer,      // the composite's views are not secured (the frame had none of the pass's textures to keep yet)
    NoDepth,        // no scene depth for this frame (the fog post that binds it never drew)
    NoCamera,       // no camera read landed, or the bytes arrived and are not this build's camera (a build mismatch, counted)
    NotToneMap,     // the draw's shape is another post pass (passed on, nothing counted)
    RenderTarget,   // not exactly one render target, or it is the scene itself
    TargetFormat,   // the tone map's target is not FP16
    SceneFormat,    // the scene is not FP16
    SizeMismatch,   // the scene, the G-buffer views and the depth do not belong to one frame
    Window,         // the render window is empty or does not fit the scene
    SceneHdr,       // the engine's scene HDR path owns the tone chain (a configuration, not a build this module cannot serve)
    CharaReplay,    // a character-preview replay of a game draw
    PassFailed,     // the pass could not build its textures, shaders or draws
    Exception,      // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the strength is 0";
    case Decline::NoGBuffer: return "no G-buffer view this pass binds landed for this frame";
    case Decline::NoDepth: return "no scene depth for this frame (no fog post drew)";
    case Decline::NoCamera: return "the camera bytes did not land, or are not this build's camera matrices";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene, the G-buffer views and the depth do not belong to one frame";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::SceneHdr: return "the engine's scene HDR path replaces the tone chain this stage draws before";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draws";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// Which declines count toward the module's own stand-down: the ones that say this build cannot serve the stage. A frame without the pass's
// views or its depth is a frame without the stage in it, not a failure; the scene HDR path and the replay are configurations.
constexpr bool CountsTowardsStandDown(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::NoCamera:
    case Decline::RenderTarget:
    case Decline::TargetFormat:
    case Decline::SceneFormat:
    case Decline::SizeMismatch:
    case Decline::Window:
    case Decline::PassFailed:
    case Decline::Exception:
        return true;
    case Decline::NoGBuffer:
    case Decline::NoDepth:
    case Decline::NotToneMap:
    case Decline::SceneHdr:
    case Decline::CharaReplay:
    case Decline::FeatureOff:
    case Decline::None:
        return false;
    }
    return false;
}

// Why a frame had no bounce in it, the three reasons the card counts apart (the march's own decomposition, this stage's own: the camera's
// bytes are one frame late once the GPU is behind, and a ring that has never landed is a stall, not a refusal - a frame without a camera is a
// frame without the stage in it).
enum class SkipReason : uint8_t
{
    NoGBuffer = 0, // the frame's composite did not yield the pass's own views (or they belong to another frame)
    NoDepth = 1,   // the frame's fog post did not yield the scene depth (or it does not fit the scene)
    NoCamera = 2,  // the camera read did not land (its bytes late or never)
};

struct CardCounts
{
    uint32_t skipped = 0;
    uint32_t noGBuffer = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
};

// The one place a frame is counted as skipped: `skipped` and exactly one of the three reasons move together (the march's own contract).
constexpr void CountSkip(CardCounts& counts, SkipReason reason) noexcept
{
    ++counts.skipped;
    switch (reason)
    {
    case SkipReason::NoGBuffer: ++counts.noGBuffer; return;
    case SkipReason::NoDepth: ++counts.noDepth; return;
    case SkipReason::NoCamera: ++counts.noCamera; return;
    }
}

// ---- the log and the card ----------------------------------------------------------------------------------------------------------------

inline constexpr unsigned kMaxLogLines = 32;

// One count per Decline, indexed by the enum's value.
inline constexpr uint32_t kDeclineReasonCount = uint32_t(Decline::Exception) + 1;

struct LogGate
{
    uint32_t last = 0;
    bool have = false;
    unsigned lines = 0;

    bool Take(uint32_t tag) noexcept
    {
        if (lines >= kMaxLogLines || (have && tag == last))
            return false;
        have = true;
        last = tag;
        lines++;
        return true;
    }
};

// A declined tone-map draw, with the outcome the chain's hold semantics name - the two wordings distinct on purpose, the march's contract.
inline std::string FormatDecline(Decline reason, Outcome outcome)
{
    if (outcome == Outcome::HoldStands)
        return std::format("Light bounce: the stage declined ({}), so the chain's hold stands", DeclineName(reason));
    if (outcome == Outcome::EngineOwn)
        return std::format("Light bounce: the stage declined ({}), so the frame is the engine's own", DeclineName(reason));
    return std::format("Light bounce: the stage declined ({})", DeclineName(reason));
}

inline std::string FormatSkip(SkipReason reason)
{
    switch (reason)
    {
    case SkipReason::NoGBuffer: return std::format("Light bounce: the frame skipped ({})", DeclineName(Decline::NoGBuffer));
    case SkipReason::NoDepth: return std::format("Light bounce: the frame skipped ({})", DeclineName(Decline::NoDepth));
    case SkipReason::NoCamera: return std::format("Light bounce: the frame skipped ({})", DeclineName(Decline::NoCamera));
    }
    return std::format("Light bounce: the frame skipped ({})", DeclineName(Decline::Exception));
}

// The one line the pass's kept views write, once per shape they first land.
inline std::string FormatSecured(const char* what, bool ownView, uint32_t width, uint32_t height, uint32_t format)
{
    return std::format("Light bounce: the {} kept as {} ({}x{}, format {})", what,
                       ownView ? "an own view on the game's resource" : "this pass's own reference to the drawn resource", width, height,
                       format);
}

// The numbers the card shows, in one struct so the menu and the log share the reading.
struct CardNumbers
{
    float strength = kDefaultStrength;
    uint64_t frames = 0;
    uint32_t runs = 0;
    uint32_t held = 0;
    uint32_t declined = 0;
    uint32_t skipped = 0;
    uint32_t noGBuffer = 0;
    uint32_t noDepth = 0;
    uint32_t noCamera = 0;
    uint32_t refused = 0; // the camera refusals (a build mismatch), counted toward the stand-down
    uint32_t windowWidth = 0, windowHeight = 0;
    uint32_t giWidth = 0, giHeight = 0;
};

// The card's one state line, in the numbers a session reads first when the bounce is missing.
inline std::string FormatCard(const CardNumbers& numbers)
{
    return std::format("Light bounce: {} frames, {} runs ({} held), {} declined, {} skipped ({} without the G-buffer views, {} without a "
                       "depth, {} without a camera), {} camera refusals, last window {}x{}, last pass {}x{}",
                       numbers.frames, numbers.runs, numbers.held, numbers.declined, numbers.skipped, numbers.noGBuffer, numbers.noDepth,
                       numbers.noCamera, numbers.refused, numbers.windowWidth, numbers.windowHeight, numbers.giWidth, numbers.giHeight);
}

// The once-per-run line: which keys the pass ran with, at which extent, and the fade the held state drew with.
inline std::string FormatRun(const Params& params, uint32_t windowWidth, uint32_t windowHeight, uint32_t giWidth, uint32_t giHeight, float fade,
                             float whitePoint)
{
    return std::format("Light bounce: the pass ran at {}x{} of the {}x{} window, {} rays, {} steps, range {:.1f} m, strength {:.2f}, "
                       "fade {:.2f}, white {:.3f}",
                       giWidth, giHeight, windowWidth, windowHeight, params.rays, params.steps, double(params.range),
                       double(params.strength), double(fade), double(whitePoint));
}

// The cost line, once the timestamp pair has landed (the march's own form, this pass's numbers).
inline std::string FormatCost(bool chained, double gpuMilliseconds, uint64_t frames, uint32_t runs, uint32_t declined)
{
    return std::format("Light bounce: {}{:.4f} GPU ms measured over {} frames ({} runs, {} declined)", chained ? "chained, " : "",
                       gpuMilliseconds, frames, runs, declined);
}

// A session that stood down after the limit: the march's own number, this module's own streak (one knob of this family, one Session here).
} // namespace ext::ffxiv::engine::gi
