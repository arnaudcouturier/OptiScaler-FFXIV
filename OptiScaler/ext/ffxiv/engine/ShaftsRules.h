#pragma once
// Light shafts before the tone map (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is
// covered by tests/ext/ffxiv/engine on the host.
//
// What the feature does, in one line: before the engine's tone map reads the lit scene, the pass builds a half-resolution sky mask from the
// frame's own depth, blurs it toward the sun's screen position and adds the result to a copy of the scene, which is bound at the tone map's
// `t0` for that one draw; the engine's own scene is only ever read.
//
// Everything that can be decided without a device lives here: the keys and their ranges, the feature bit, the render window, the sun's
// projection and the off-screen rule, the sky test on the engine's reversed-Z depth, the shape rules that recognise the tone-map draw and the
// sun light draw, the drop list, and the log and cost lines. The fail-open contract is the drop list: every reason leaves the game's draw
// exactly as it was, with the game's own scene still bound.

#include "ext/ffxiv/engine/EnginePassRules.h" // the packed anchor word this module claims two bits of

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

namespace ext::ffxiv::shafts
{
// ---- the keys ------------------------------------------------------------------------------------------------------------

// How much of the sun's light the shafts add, 0 to 2. 0 is off: the feature bit is off and the draw path costs one relaxed load, so the
// intensity key alone can turn the effect away without touching the master switch. The default is a full key: at 0.5 the captured frame's own
// 0.05 tree got +15/255 and its 0.8 sky-side surface +6/255, which is a shaft a player has to look for; at 1.0 they are +33 and +8, and the
// default is still 11 % of the frame's white against the top key's 19 %.
inline constexpr float kMinIntensity = 0.0f;
inline constexpr float kMaxIntensity = 2.0f;
inline constexpr float kDefaultIntensity = 1.0f;
inline constexpr float kIntensityEpsilon = 0.001f;

// The per-sample falloff of the radial blur: 1.0 keeps every sample as bright as the last, 0.8 fades the far ones fast. Below 0.8 the shafts
// collapse onto the sun's own pixels instead of reaching across the frame.
inline constexpr float kMinDecay = 0.80f;
inline constexpr float kMaxDecay = 1.0f;
inline constexpr float kDefaultDecay = 0.96f;

// The march's sample count, clamped: fewer than 16 bands visibly, more than 96 costs more than the effect is worth at half the render size.
inline constexpr int32_t kMinSamples = 16;
inline constexpr int32_t kMaxSamples = 96;
inline constexpr int32_t kDefaultSamples = 48;

// How far a sample reaches toward the sun: 1.5 lets the blur sample past the sun, 0.1 keeps it a halo around the sun's own pixels.
inline constexpr float kMinDensity = 0.1f;
inline constexpr float kMaxDensity = 1.5f;
inline constexpr float kDefaultDensity = 0.9f;

constexpr float ClampIntensity(float value) noexcept
{
    if (value != value)
        return kDefaultIntensity;
    return value < kMinIntensity ? kMinIntensity : (value > kMaxIntensity ? kMaxIntensity : value);
}

constexpr float ClampDecay(float value) noexcept
{
    if (value != value)
        return kDefaultDecay;
    return value < kMinDecay ? kMinDecay : (value > kMaxDecay ? kMaxDecay : value);
}

constexpr float ClampDensity(float value) noexcept
{
    if (value != value)
        return kDefaultDensity;
    return value < kMinDensity ? kMinDensity : (value > kMaxDensity ? kMaxDensity : value);
}

constexpr uint32_t ClampSamples(int32_t value) noexcept
{
    const int32_t samples = value < kMinSamples ? kMinSamples : (value > kMaxSamples ? kMaxSamples : value);
    return uint32_t(samples);
}

// The parameters one session runs with, from the keys. The master switch is not part of them: it decides whether the module is armed at all.
struct Params
{
    float intensity = kDefaultIntensity;
    float decay = kDefaultDecay;
    float density = kDefaultDensity;
    uint32_t samples = uint32_t(kDefaultSamples);
};

constexpr Params BuildParams(float intensity, float decay, int32_t samples, float density) noexcept
{
    Params params;
    params.intensity = ClampIntensity(intensity);
    params.decay = ClampDecay(decay);
    params.samples = ClampSamples(samples);
    params.density = ClampDensity(density);
    return params;
}

// The feature bit: the master switch and an intensity above zero. A master left on at intensity 0 is today's picture and runs nothing, so
// the draw path costs one relaxed load per draw and nothing else.
constexpr bool Wanted(bool enabled, const Params& params) noexcept
{
    return enabled && params.intensity > kIntensityEpsilon;
}

// ---- the added light, in the white the pass reads -----------------------------------------------------------------------------

// The shafts' light is not a number in the render target's own units - those are arbitrary, and the same key meant a sixth of a bright day's
// white and four times a dark one's, which is how a default came to burn the picture. It is a share of the frame's white point instead (see
// WhitePointOf): this is the light the shafts add at intensity 1, so `LightShaftsIntensity` is a brightness in white units whatever the scene's
// exposure. The captured frame measured a white at 0.838 of the scene's units with the tone map's local gain at 1.21, and the default key then
// adds 23 % of it to a surface with no light of its own (PeakWhiteShare), the top key 37 %.
//
// A drawn frame of a daytime session is a bright one: the sun is in view, and every lit surface of the picture is already between 0.2 and 0.8 of
// the white, so a gain that reads as a beam on a dark night frame is invisible here. At 0.12 a dark 0.05 surface got 0.089 - about 23 of 255
// levels under the sun's own bloom, which a screenshot of the session shows as no change at all - and at 0.30 it gets 0.190, roughly 48 levels,
// which reads as a beam. The room-to-white bound below is what keeps that from burning a bright surface: 0.5 gets 0.144 and 0.8 gets 0.033.
inline constexpr float kShaftGain = 0.30f;

// The frame's white point: the scene value whose tone-mapped output is 1.0, i.e. where the picture turns display-white. A captured frame settles
// how it is measured - from that frame's own tone map input and output pair (the lit scene and the tone map's target, the same pixels): the
// target crosses 1.0 at a scene 0.838 - median of the pixels where it crosses - with a local gain of 1.18 to 1.22 over 0.2..0.8, and it is not
// clamped (the frame's brightest red leaves a scene 1.82 as 2.09). The gain is the tone map's own exposure multiplier, so the crossing is its
// reciprocal: 1/1.21 = 0.826, within 1.5 % of the pair's own measurement.
//
// The pass does not read that pair back - a 4K pair a frame is a copy this effect is not worth - it reads the exposure multiplier itself, which
// the tone map's parameter buffer carries (`cCommonTexParam`, the same draw the shafts are composited into, its second float).
//
// The measured 0.838 is the *only* white point this build has verified, so it is the ceiling: a live exposure that claims a white above it is a
// buffer this build has not measured (with exposure .y = 0.5, a white of 2.0 let the top key add 0.364 to a 0.5 wall, 0.026 past the measured
// white, where the ceiling's 0.838 adds 0.126 and leaves it 0.21 below). Below it the live exposure is what makes the same key the same share of
// the white in a day frame and a dusk one; the floor is the exposure range's dark end, the darkest white the band's own measurement supports.
//
// What the band does not do is tell a wrong-high live value from a legitimately bright frame: the bound is the white the pass reads, not a white
// it has measured for this frame. A live 0.838 on a computed 0.2-white frame - a worked example, not a dump - still over-lights it: the top key
// takes a 0.1 surface to 0.258 (29 % past that frame's white) and its 0.02 surface to 0.182 (81 % of it). Those are the residual hole; what the
// pass can do without measuring the frame's own pair is to keep the hole small and make it visible in its lines. The fallback below is where that
// hole is widest, because the pass then has no exposure at all: it reads a frame it is drawing shafts on as the daylight frame that measurement
// came from, which is right for every frame a live sun is in - a night or dusk frame the sun is visible in is a bright sun with a dark scene, and
// the band's ceiling is still the bound on it.
inline constexpr float kVerifiedWhitePoint = 0.838f; // the captured frame's measurement: the ceiling, until a second measurement exists
inline constexpr float kMinWhitePoint = 0.15f;       // a very dark frame exposed up (exposure 6.7): the floor of the band
inline constexpr float kMaxWhitePoint = kVerifiedWhitePoint;
inline constexpr float kMinExposure = 1.0f / kMaxWhitePoint;
inline constexpr float kMaxExposure = 1.0f / kMinWhitePoint;

// The value the bound falls back to while the frame's own exposure has not arrived (a session's first frames, or a build whose tone map's
// parameters are not where this build's are), and after it has stopped arriving (WhitePointStale). It is the captured frame's own measurement.
//
// A frame the pass draws shafts on is a frame whose sun is in view, which is a daylight frame the engine is exposing for, and the captured session
// reads a white of about 0.84 whenever its exposure lands - 0.8895 in one frame's parameters, 1.196 in another's, both a white of about 0.84. The
// band's dark end (0.15) therefore described no drawn frame at all: with `head = white - scene` at zero for every surface above 0.15 - which is
// every lit surface of a daylight frame - a screenshot with 1,808 drawn frames in it showed the scene's pixels plus nothing, and a black pixel got
// 0.018. A white point the pass has not measured yet is this frame's daytime exposure by construction, and the band already caps that at 0.838,
// so the fallback is the same measurement rather than a dark frame's bound that the measurement contradicts. One line says when it is in use.
inline constexpr float kFallbackWhitePoint = kVerifiedWhitePoint;

// How long a live white point stays this frame's exposure without a landed copy: the exposure moves over seconds, so a read that has not landed
// for a second and a half is not a GPU that is behind any more (a stalled copy, or a tone map that stopped binding the buffer). The frames after
// it draw against the fallback, one line says so.
inline constexpr uint64_t kMaxWhiteStaleFrames = 90;

// What one light draw's `cCommonTexParam` holds, and how much of it this module reads: the exposure multiplier is its second float.
inline constexpr uint32_t kToneMapBytes = 0x10;
inline constexpr uint32_t kToneMapExposureAt = 0x04;

// The exposure values this pass accepts as a tone map's own. kMaxExposure is the band's dark end (a white of 0.15); the floor is not the band's
// bright end but a buffer that is no tone map's parameters at all. The captured session's parameter buffer carried 0.8895 in one frame and 1.196 in
// another, and 0.8895 is below the band's bright end (1.0/0.838 = 1.1933) only because that float is the multiplier's reciprocal there: rejecting
// it cost the frame its live white point and left the shafts drawn against the fallback. Everything above the floor is clamped into the band by
// WhitePointFromExposure, so a value that is the reciprocal still lands on the measured white instead of being refused.
inline constexpr float kMinUsableExposure = 0.25f;

constexpr bool ExposureUsable(float exposure) noexcept
{
    return exposure == exposure && exposure >= kMinUsableExposure && exposure <= kMaxExposure;
}

// The scene value at which a frame with this exposure multiplier reaches white, inside the band the measurement supports. A value outside the
// range a frame's exposure can produce is clamped to the band's edge, and a value that is not a number at all - a buffer this build does not
// recognise - falls back to the captured measurement rather than dividing by a NaN or to a dark frame's bound the pass has no reason to expect.
constexpr float WhitePointFromExposure(float exposure) noexcept
{
    if (!(exposure == exposure))
        return kFallbackWhitePoint;
    const float clamped = exposure < kMinExposure ? kMinExposure : (exposure > kMaxExposure ? kMaxExposure : exposure);
    return 1.0f / clamped;
}

// Whether the last live white point is still this frame's: a copy has landed, and this frame is no more than kMaxWhiteStaleFrames past it.
constexpr bool WhitePointStale(bool haveWhite, uint64_t lastFrame, uint64_t frame) noexcept
{
    return !haveWhite || (frame >= lastFrame && frame - lastFrame > kMaxWhiteStaleFrames);
}

constexpr bool WhitePointUsable(float whitePoint) noexcept
{
    return whitePoint == whitePoint && whitePoint >= kMinWhitePoint && whitePoint <= kMaxWhitePoint;
}

// The share of the white the pass reads that the shafts add to a pixel with no light of its own (a black surface, where the shafts are
// brightest): light/(1 + light) at light = intensity * kShaftGain. It is the same share in a day frame and a dusk one while the live white is the
// frame's, and it is below 1 at every key, so a key can never push a channel past the white the pass reads - not past the frame's real white when
// the live value is wrong-high (see the band's comment).
constexpr float PeakWhiteShare(float intensity) noexcept
{
    const float light = ClampIntensity(intensity) * kShaftGain;
    return light / (1.0f + light);
}

// One channel's addition. `light` is the shafts' light at this pixel in fractions of the frame's white (the blurred mask times the key, the
// tint's channel and the held sun's fade), `scene` is the pixel's own value in the scene's units. The addition is bounded by the pixel's own room
// to the white - head = white - scene, and add = head * raw / (raw + head) with raw = light * white - so the sum is strictly below the white point
// whatever the key, the mask or the white point itself are. The slope at zero is 1: a pixel far from white gets the key's share of the white
// added to it, and a pixel already at white gets nothing, which is what a light that cannot be seen beyond white should do. The approach is smooth
// (a bilinear roll-off, no band at a ceiling), so nothing about it reads as a clip.
constexpr float ShaftAdded(float scene, float light, float whitePoint) noexcept
{
    if (!(whitePoint > 0.0f) || !(light > 0.0f) || !(light == light) || !(scene == scene))
        return 0.0f;
    const float head = whitePoint - scene;
    if (!(head > 0.0f))
        return 0.0f;
    const float raw = light * whitePoint;
    return head * raw / (raw + head);
}

// ---- the render window ---------------------------------------------------------------------------------------------------

// The render window of the draw, in the scene's pixels: the engine allocates the frame's targets at the display size and draws a smaller
// window inside them (a dynamic resolution or a render-scale setup), so the window - not the allocation - is what the pass works in.
struct Window
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// A window smaller than this is a preview or an effect's scratch surface, never the frame the sun shines into.
inline constexpr uint32_t kMinWindowWidth = 64;
inline constexpr uint32_t kMinWindowHeight = 64;

constexpr bool WindowUsable(const Window& window) noexcept
{
    if (!(window.width > 0.0f) || !(window.height > 0.0f))
        return false;
    if (!(window.x >= 0.0f) || !(window.y >= 0.0f))
        return false;
    if (window.width < float(kMinWindowWidth) || window.height < float(kMinWindowHeight))
        return false;
    // A NaN window is a draw whose viewport was never set: the comparisons above are false for it, and this one keeps the rule total.
    return window.x == window.x && window.y == window.y;
}

// The half-resolution extent of an extent in pixels: an odd extent keeps its last half pixel (the shaders clamp their reads), and a zero
// extent stays zero instead of becoming a one-texel pass.
constexpr uint32_t HalfExtent(float extent) noexcept
{
    if (!(extent >= 2.0f))
        return 0;
    return uint32_t((extent * 0.5f) + 0.9999f);
}

// ---- the engine's depth --------------------------------------------------------------------------------------------------

// The engine's depth buffer is reversed-Z: 0 is the far plane and 1 is the near plane (the contact shadow pass measured the same buffer).
// A pixel at the far plane is sky: it is where the sun's light comes from, and the shafts' mask is 1 there and 0 on everything that has a
// surface. A cannot-see value (a NaN, a missing read) is 0, i.e. "a surface": a broken depth adds no light rather than a full-frame glow.
inline constexpr float kSkyDepth = 1.0e-6f;

constexpr float SkyOf(float depth) noexcept
{
    if (!(depth == depth) || !(depth >= 0.0f))
        return 0.0f; // a NaN or a negative value is not a depth this pass can read: never light from it
    return depth <= kSkyDepth ? 1.0f : 0.0f;
}

// ---- the sun's projection ------------------------------------------------------------------------------------------------

// The camera `g_CameraParameter` holds, as `mul(matrix, vector)` applies it (CameraRegisters.h lays the same buffer out for the contact shadow
// shader). The light draw's `g_LightParam.m_Direction` is in VIEW space: on a captured frame it turns with the camera while the frame's
// directional-shadow axis stays put, so a sun projected from it goes through the PROJECTION alone (`m_ProjectionMatrix`, +0x120). Projecting it
// through the view-projection would apply the view twice - the capture measured a 29-degree-off sun at (2614, 2442) that way, where the
// projection puts it at the frame's own (1319.4, 82.5). The view matrix (+0x00) is read from the same copy: its second row is the world's up in
// view space, which is the only way to ask whether the sun is above the horizon once the direction is view space. The projection's shape stays
// the guard: a buffer that is not this build's camera is refused before anything is projected.
inline constexpr uint32_t kCameraBytes = 0x160;        // view, view-projection and projection: what the shaders read
inline constexpr uint32_t kViewProjectionAt = 0x060;   // g_CameraParameter +0x060: m_ViewProjectionMatrix, four rows of four floats
inline constexpr uint32_t kProjectionOffset = 0x120;   // g_CameraParameter +0x120: m_ProjectionMatrix, four rows of four floats
inline constexpr uint32_t kProjectionRows = 16;
inline constexpr uint32_t kProjectionBytes = 0x40; // the projection's four rows of four floats
inline constexpr uint32_t kViewAt = 0x000;         // g_CameraParameter +0x000: m_ViewMatrix's three rows (row 3 is the inverse view's row 0)
inline constexpr uint32_t kViewRows = 12;          // the three rows the elevation and world-space tests read, in floats
inline constexpr uint32_t kViewBytes = 0x30;

// What one light draw's `g_LightParam` holds, and how much of it this module reads: the position, the direction and the diffuse colour. The
// ranges are the ones the contact shadow pass measured on the same buffer (CameraRegisters.h).
inline constexpr uint32_t kLightBytes = 0x30;
inline constexpr uint32_t kLightDirectionAt = 0x10;
inline constexpr uint32_t kLightDiffuseAt = 0x20;

// The two reads of a light draw are taken from a ring of staging pairs, one slot per rendered frame, at the tone map of a later frame. A copy
// is never mapped at the frame that made it: the light draw is the frame's middle and the tone map its end, so a map asked not to wait would
// ask for bytes the GPU has not written. An exact age is not enough either - a GPU-bound session can be two or three frames behind, so a copy
// tried at one age only gets one map attempt per frame it still exists, and a session where that attempt always failed would read nothing for
// its whole length. So each read tries the newest copy the ring still holds first and takes an older one when that has not landed.
inline constexpr uint32_t kStagingRing = 4;
// The newest age a read may take: the copy the frame before this one made. Age 0 is this frame's own copy, which the GPU has almost certainly
// not run yet, so it is never a candidate.
inline constexpr uint32_t kNewestReadAge = 1;
// A read that has not landed for this long is not a GPU that is behind any more (a stalled device, a session that stopped drawing the light
// draws): the frames are then skipped without a refusal, with one line, instead of compositing a sun that no longer belongs to the frame.
inline constexpr uint64_t kMaxReadbackAge = 90;

constexpr uint32_t RingIndex(uint64_t frame) noexcept { return uint32_t(frame % kStagingRing); }

// The ring slot holding the copy that is `age` frames old at `frame`. Only an age ReadCandidate accepted is asked for, so `frame >= age`.
constexpr uint32_t SlotForAge(uint64_t frame, uint32_t age) noexcept { return RingIndex(frame - uint64_t(age)); }

// Whether the slot written at `writtenFrame` is one this read may try: it was written, its copy is at least kNewestReadAge old and still in the
// ring (its age is below the ring's depth), and it is newer than the last copy a read took (`haveAccepted`, `lastAcceptedFrame`), so an older
// copy never replaces a newer one. Every candidate is mapped without waiting, newest first, and the first map that lands is the read.
constexpr bool ReadCandidate(bool written, uint64_t writtenFrame, uint64_t frame, bool haveAccepted, uint64_t lastAcceptedFrame) noexcept
{
    if (!written || frame < writtenFrame)
        return false;
    const uint64_t age = frame - writtenFrame;
    if (age < uint64_t(kNewestReadAge) || age >= uint64_t(kStagingRing))
        return false;
    return !haveAccepted || writtenFrame > lastAcceptedFrame;
}

// The module's own count of rendered frames. One rendered frame is the game's light pass, the composite that writes the lit scene and the tone
// map that reads it; the game presents that frame's outputs once or twice (frame generation presents a generated image between two game
// frames), so the output-present counter steps about 1.8 times inside one rendered frame and cannot be the frame's identity: a light draw and
// the tone map it feeds would answer "different frames" whenever a present landed between them. The count therefore moves when a frame is
// closed, and only then: at the frame's tone map, where the next frame's first light draw or composite opens the next count. A frame whose tone
// map is never classified never closes, so it leaves the count where it is and the frame after it composites with the light draw and depth it
// stored - a stale-frame path this identity does not remove, and a frame the module has nothing to say about. The ring's ages and every "same
// frame" test are counted in this number.
constexpr void AdvanceFrame(uint64_t& frames) noexcept { ++frames; }

// Whether the frame's sun draw is this frame's: the light pass and the tone map it feeds are one rendered frame, however many presents the
// frame's outputs take. The present token is not an identity here - see AdvanceFrame.
constexpr bool SameRenderedFrame(uint64_t written, uint64_t now) noexcept { return written == now; }

// How long a light the read-back found at a world position is skipped. The engine's light parameter buffers are a small pool reused within a
// frame, so the pointer alone would block that buffer for the rest of the session - including the frames where the sun's own draw uses it. A
// few seconds of frames is enough to move past the lamp, and the light is then looked at again instead of never.
inline constexpr uint64_t kRejectedLightFrames = 240;

constexpr bool RejectedStill(bool haveRejected, uint64_t rejectedFrame, uint64_t frame) noexcept
{
    return haveRejected && frame >= rejectedFrame && frame - rejectedFrame < kRejectedLightFrames;
}

// The read being too old to use this frame: the last successful one is more than kMaxReadbackAge frames back.
constexpr bool ReadStale(bool haveRead, uint64_t lastReadFrame, uint64_t frame) noexcept
{
    return haveRead && frame >= lastReadFrame && frame - lastReadFrame > kMaxReadbackAge;
}

// How far outside the render window the sun may be and still light the frame: half a window beyond each edge. A sun further out than that is
// behind the player's view, and the shafts it would cast leave the frame. The bound is half a window rather than a quarter because the frame's
// shadow-casting directional light, which is what the light draws report, is clamped by the engine to a minimum elevation (about 30 degrees) so
// its cascade shadows do not stretch to infinity: at 07:00 and 17:00 that light points well above the sun the sky draws, and the quarter-window
// margin left a morning sun that was still on screen counted as out of view on most of a session's frames. The sky's own sun position replaces
// this projection whenever it is available (ProjectSunFromUv), so the margin is the fallback's bound.
inline constexpr float kMaxOffscreen = 0.50f;

struct SunProjection
{
    bool unit = false;     // the direction was a usable unit vector
    bool behind = false;   // the sun is at or behind the camera's plane (clip w not positive)
    float ndcX = 0.0f, ndcY = 0.0f;
    float screenX = 0.0f, screenY = 0.0f; // in the scene's pixels, the window's origin included
    float offscreen = 0.0f;               // how far outside the window, in window widths/heights (0 = inside)
};

// Projects a unit VIEW-space direction at infinity through `projection` into the window: `clip = projection * (direction, 0)`, whose w is
// `-direction.z` for the engine's projection, so a sun in front of the camera has a positive clip w. A direction that is not a unit vector
// within 2 % is refused (`unit` false): the buffer carries a light that is off or a layout this build no longer has, and a wrong sun position
// would put the shafts in the wrong place instead of not drawing them.
constexpr SunProjection ProjectSun(const float projection[kProjectionRows], const float direction[3], const Window& window) noexcept
{
    SunProjection sun;
    if (projection == nullptr || direction == nullptr)
        return sun;
    const float lengthSquared = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
    if (!(lengthSquared > 1.0e-6f) || !(lengthSquared == lengthSquared))
        return sun;
    if (lengthSquared < 0.9604f || lengthSquared > 1.0404f) // 0.98^2 .. 1.02^2
        return sun;
    sun.unit = true;

    // mul(matrix, float4(direction, 0)): the rows are the four float4s the buffer stores, and a direction has no translation, so only the
    // first three columns of each row take part. The direction is already in the projection's own space, so this is the whole transform.
    const float clipX = projection[0] * direction[0] + projection[1] * direction[1] + projection[2] * direction[2];
    const float clipY = projection[4] * direction[0] + projection[5] * direction[1] + projection[6] * direction[2];
    const float clipW = projection[12] * direction[0] + projection[13] * direction[1] + projection[14] * direction[2];
    if (!(clipW > 0.0f))
    {
        sun.behind = true; // at infinity or behind the camera: no screen position exists
        return sun;
    }
    sun.ndcX = clipX / clipW;
    sun.ndcY = clipY / clipW;
    if (!(sun.ndcX == sun.ndcX) || !(sun.ndcY == sun.ndcY))
    {
        sun.unit = false;
        return sun;
    }
    sun.screenX = window.x + (sun.ndcX * 0.5f + 0.5f) * window.width;
    sun.screenY = window.y + (0.5f - sun.ndcY * 0.5f) * window.height;

    // How far outside the window, in window widths and heights: the largest of the four edges' overshoot.
    const float left = window.x - sun.screenX;
    const float right = sun.screenX - (window.x + window.width);
    const float top = window.y - sun.screenY;
    const float bottom = sun.screenY - (window.y + window.height);
    float outside = 0.0f;
    if (left > 0.0f)
        outside = left / window.width;
    if (right > 0.0f && right / window.width > outside)
        outside = right / window.width;
    if (top > 0.0f && top / window.height > outside)
        outside = top / window.height;
    if (bottom > 0.0f && bottom / window.height > outside)
        outside = bottom / window.height;
    sun.offscreen = outside;
    return sun;
}

// Whether the frame is worth the pass: the sun has a screen position and is not further out than `maxOffscreen`. A sun behind the camera or
// far outside it is not a refusal of the feature - it is a frame that has no shafts in it, and the caller counts nothing.
constexpr bool SunVisible(const SunProjection& sun, float maxOffscreen = kMaxOffscreen) noexcept
{
    return sun.unit && !sun.behind && sun.offscreen <= maxOffscreen;
}

// What the sky's sun parameter buffer (`cSunParam`) holds in the bytes this module reads: its first row is (aspectRatio, 1.0, sunUvX, sunUvY), the
// sun disk's own position in the tone map's UV space. The light draws report a direction whose elevation the engine clamps, so at dawn and dusk
// this is the only place the frame says where the sun a player sees was drawn.
inline constexpr uint32_t kSunParamBytes = 0x10;

// Whether those four floats are this build's sun parameters: a frame with no sun in it (an interior, a cut), a buffer of another layout and a NaN
// all fail here, and the pass then projects the light's direction the way it always did. The first float is the window's own aspect, so it stays
// inside the range a display's aspect can take, and the second is the 1.0 the engine pairs it with - a buffer that carries neither is not this
// layout, whatever its UVs look like.
constexpr bool SunParamUvUsable(const float row0[4]) noexcept
{
    if (row0 == nullptr)
        return false;
    for (uint32_t i = 0; i < 4; ++i)
        if (!(row0[i] == row0[i]))
            return false;
    return row0[0] >= 0.5f && row0[0] <= 5.0f && row0[1] >= 0.95f && row0[1] <= 1.05f && row0[2] >= -1.0f && row0[2] <= 2.0f &&
           row0[3] >= -1.0f && row0[3] <= 2.0f;
}

// Projects the sky's own sun UV into the window: the drawing already placed the disk, so this copies the two numbers into the window's pixels and
// measures how far outside it the position is, with no camera matrix involved. The UV is the tone map's (0 at the window's top-left), so y runs
// down; the NDC is kept for the log and for a caller that needs the frame's own space. Nothing here is a refusal: an unusable value returns the
// default projection, whose `unit` bit is false.
constexpr SunProjection ProjectSunFromUv(float uvX, float uvY, const Window& window) noexcept
{
    SunProjection sun;
    if (!(uvX == uvX) || !(uvY == uvY) || !WindowUsable(window))
        return sun;
    sun.unit = true;
    sun.behind = false;
    sun.ndcX = uvX * 2.0f - 1.0f;
    sun.ndcY = 1.0f - uvY * 2.0f;
    sun.screenX = window.x + uvX * window.width;
    sun.screenY = window.y + uvY * window.height;
    const float left = window.x - sun.screenX;
    const float right = sun.screenX - (window.x + window.width);
    const float top = window.y - sun.screenY;
    const float bottom = sun.screenY - (window.y + window.height);
    float outside = 0.0f;
    if (left > 0.0f)
        outside = left / window.width;
    if (right > 0.0f && right / window.width > outside)
        outside = right / window.width;
    if (top > 0.0f && top / window.height > outside)
        outside = top / window.height;
    if (bottom > 0.0f && bottom / window.height > outside)
        outside = bottom / window.height;
    sun.offscreen = outside;
    return sun;
}

// ---- the sun a frame draws with: the last accepted one, held across a gap ------------------------------------------------

// A frame whose own light bytes have not landed yet draws with the sun the last read accepted, for at most kHoldFrames rendered frames: the
// read-back misses a frame now and then (a GPU a frame behind, a copy the driver has not finished), and a frame dropped inside the upscaler's
// input is a bright overlay that appears and disappears - the on/off ghosting a session showed. The age is bounded so a stale light cannot outlive
// the frame it belongs to by long, and the held light fades over the frames it covers a fifth at a time, so no frame of it is a step: the last
// values are close enough to zero that the frame after them can be skipped without the picture moving.
inline constexpr uint32_t kHoldFrames = 6;

// The held sun's weight on the frame it is used on, one entry per age from 0 to kHoldFrames: 1 for the frame the last read belongs to and the one
// after it (a read at an age of one is the normal one), then 0.85, 0.65, 0.45, 0.25 and 0.05 - the largest step between two frames is 20 % of the
// light, where the first version's second frame fell from 1 to 0.5625 (44 %) in one frame at the render boundary, which frame generation
// interpolates into a visible slide. A frame older than kHoldFrames is skipped instead (a weight of 0).
inline constexpr float kHoldWeights[kHoldFrames + 1u] { 1.0f, 1.0f, 0.85f, 0.65f, 0.45f, 0.25f, 0.05f };
constexpr float HoldFade(uint32_t age) noexcept { return age < uint32_t(kHoldFrames + 1u) ? kHoldWeights[age] : 0.0f; }

// Whether the last landed visual sun UV still belongs to this frame: the copy landed, and the rendered frame it was written at is no more than
// kHoldFrames back. The ring hands a copy over one frame late (age 0 is a copy the GPU has almost certainly not run), so this covers the ordinary
// delay and the frames a stale map costs; after that the pass projects the light's direction again instead of placing the shafts with a sun the
// sky may have stopped drawing (a cutscene, a menu, an interior).
constexpr bool SunUvFresh(bool haveUv, uint64_t uvFrame, uint64_t frame) noexcept
{
    return haveUv && frame >= uvFrame && frame - uvFrame <= uint64_t(kHoldFrames);
}

// A camera cut is the one thing a held sun must never survive: the frame is a different place, and a sun held from the picture before it would
// light the new picture from the old one's sky. The view-projection's fourth row is the view's own third row negated - the camera's forward, as
// the projection passes it through - so the angle between two frames' rows is how far the camera turned between them. A frame at 60 Hz can turn
// a few degrees; twenty is a cut, a look-behind, a menu change or a teleport. The comparison is squared, so no square root is needed here.
inline constexpr float kCameraCutCosine = 0.94f; // cos 20 degrees
constexpr bool CameraCut(const float held[kProjectionRows], const float now[kProjectionRows]) noexcept
{
    if (held == nullptr || now == nullptr)
        return false;
    const float aSquared = held[12] * held[12] + held[13] * held[13] + held[14] * held[14];
    const float bSquared = now[12] * now[12] + now[13] * now[13] + now[14] * now[14];
    if (!(aSquared > 1.0e-6f) || !(bSquared > 1.0e-6f) || !(aSquared == aSquared) || !(bSquared == bSquared))
        return false; // a matrix this pass cannot read is not a cut: the age bound still limits the hold
    const float dot = held[12] * now[12] + held[13] * now[13] + held[14] * now[14];
    if (!(dot > 0.0f))
        return true; // a quarter turn or more: no pair of frames, and no menu, moves that far without a cut
    return dot * dot < kCameraCutCosine * kCameraCutCosine * aSquared * bSquared;
}

// ---- the sun's light draw ------------------------------------------------------------------------------------------------

// What one g_LightParam binding said about its light, read back from the buffer the draw bound. `position` is the light's world position; a
// directional light reports zero. `diffuse` is the engine's own colour for it, which the live build reports as zero on the directional light
// (the shadow and rain volumes read only the direction), so a zero colour falls back to the sky's colour around the sun's screen position.
struct LightFacts
{
    float position[3] { 0.0f, 0.0f, 0.0f };
    float direction[3] { 0.0f, 0.0f, 0.0f };
    float diffuse[3] { 0.0f, 0.0f, 0.0f };
};

constexpr bool IsZeroPosition(const float position[3]) noexcept
{
    return position[0] == 0.0f && position[1] == 0.0f && position[2] == 0.0f;
}

constexpr float LengthSquared(const float v[3]) noexcept { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

// A unit vector within two percent: a direction at infinity has to be one, and a value that is not is a light that is off, a layout this build
// no longer has, or a light of another kind (the contact shadow pass uses the same bound for the same buffer).
constexpr bool IsUnitVector(const float v[3]) noexcept
{
    const float lengthSquared = LengthSquared(v);
    if (!(lengthSquared == lengthSquared))
        return false;
    return lengthSquared >= 0.9604f && lengthSquared <= 1.0404f; // 0.98^2 .. 1.02^2
}

// Why a light draw's own parameters were not the sun. A directional light is a unit direction at no world position; the sun's buffer reports
// the direction the light travels FROM, in view space, and the frame's directional shadow matrix carries the same axis in world space, so a
// direction whose WORLD elevation is below the horizon is the sun's own mirror - one of the two vectors two directional lights of one frame can
// hold, and the value that put the shafts behind the camera while the sun was on screen. The elevation cannot be read from the direction's own y:
// that is the view's up, which goes negative as soon as the player pitches the camera above the sun.
enum class LightVerdict : uint8_t
{
    Sun = 0,     // a directional light above the horizon: the frame's sun
    Lamp,        // a world position: a local light, whatever its direction says
    BelowHorizon, // a directional light whose direction points below the horizon: the sun's mirror, not the sun
    NoDirection, // a direction that is not a unit vector: a light that is off, or a layout this build no longer has
};

// The smallest elevation sine a directional light may have and still be the frame's sun. The light reports the direction it travels from, so a
// sun above the world's horizon has a positive world elevation, and this bound is a small negative tolerance - about one degree below the horizon
// line. A sun one to three degrees up at dawn or dusk is a sun the player can see, and it is exactly when the shafts are longest, so a bound above
// zero refused it as "below the horizon" for the minute the sun spends there. The sun's own mirror carries the whole vector negated (elevation
// about -0.8), and nothing a degree or more below the horizon is a sun, so every light the bound refuses is still the mirror and not the sun.
inline constexpr float kMinSunElevation = -0.02f;

// The sine of a view-space direction's elevation above the world's horizon: the world's up in view space is the view matrix's second row (row
// 1), so the elevation is that row dotted with the direction. `view` is the camera copy's first three rows (kViewRows floats, row-major). A
// direction the arithmetic cannot read answers NaN, which the bound below refuses rather than accepting a frame it cannot place.
constexpr float WorldElevationOf(const float view[kViewRows], const float direction[3]) noexcept
{
    if (view == nullptr || direction == nullptr)
        return std::numeric_limits<float>::quiet_NaN();
    return view[1] * direction[0] + view[5] * direction[1] + view[9] * direction[2];
}

// A view-space direction as the world sees it: the view matrix's rows are the world's axes expressed in view space, so applying its transpose
// (the inverse rotation) turns the light's own view direction back into the world vector the frame's directional shadow is built from. False when
// there is nothing to read; the caller then compares nothing rather than comparing the wrong space.
constexpr bool WorldDirectionOf(const float view[kViewRows], const float direction[3], float world[3]) noexcept
{
    if (view == nullptr || direction == nullptr || world == nullptr)
        return false;
    world[0] = view[0] * direction[0] + view[4] * direction[1] + view[8] * direction[2];
    world[1] = view[1] * direction[0] + view[5] * direction[1] + view[9] * direction[2];
    world[2] = view[2] * direction[0] + view[6] * direction[1] + view[10] * direction[2];
    return true;
}

// A world direction as one camera's view sees it: the view matrix's rows are the world's axes in view space, so each row dotted with the world
// vector is that axis' component. The inverse of WorldDirectionOf, and what keeps a sun held across a camera that turned since its read on the
// sun instead of sliding it with the view that measured it.
constexpr bool ViewDirectionOf(const float view[kViewRows], const float world[3], float direction[3]) noexcept
{
    if (view == nullptr || world == nullptr || direction == nullptr)
        return false;
    direction[0] = view[0] * world[0] + view[1] * world[1] + view[2] * world[2];
    direction[1] = view[4] * world[0] + view[5] * world[1] + view[6] * world[2];
    direction[2] = view[8] * world[0] + view[9] * world[1] + view[10] * world[2];
    return true;
}

// A light's verdict with the elevation the caller computed for it: the world's elevation sine from WorldElevationOf, or the direction's own y
// when the frame's camera copy carries no view matrix (a host test's synthetic light, and the one case where the light's own y is all there is).
constexpr LightVerdict ClassifyLight(const LightFacts& facts, float elevation) noexcept
{
    if (!IsZeroPosition(facts.position))
        return LightVerdict::Lamp;
    if (!IsUnitVector(facts.direction))
        return LightVerdict::NoDirection;
    if (!(elevation > kMinSunElevation))
        return LightVerdict::BelowHorizon;
    return LightVerdict::Sun;
}

constexpr LightVerdict ClassifyLight(const LightFacts& facts) noexcept { return ClassifyLight(facts, facts.direction[1]); }

// Whether a light draw's own parameters say it is the sun. This is the hard requirement the shape rule below cannot supply on its own - the
// light-buffer pair is written by every local light too, and a lamp's direction would put the shafts over a streetlamp - and it carries the
// sign: no world position, a unit direction, and that direction above the world's horizon.
constexpr bool IsSunLight(const LightFacts& facts) noexcept { return ClassifyLight(facts) == LightVerdict::Sun; }
constexpr bool IsSunLight(const LightFacts& facts, float elevation) noexcept { return ClassifyLight(facts, elevation) == LightVerdict::Sun; }

// ---- the frame's directional shadow: the anchor that says which light is the sun -------------------------------------------

// The engine's `g_DirectionalShadowParameter` starts with the directional light's shadow matrix (`m_ShadowProjectionMatrix`, 64 bytes), and its
// third row - the bytes at +0x20 - is the light's own axis in the same VIEW space the matrix projects from, with the shadow cascade's scale and
// the opposite sign. Negated and normalised it is the frame's sun to the buffer's precision: on a captured frame the row
// (-0.027606, -0.035109, 0.100868) normalises to (0.250255, 0.318262, -0.914375), the direction the frame's own sun draw reported in that same
// frame, while a light that was a 48.9 degree yaw of the sun (a session drew from one) fails it by that angle. The light's direction and this row
// are therefore compared as they are, both view space. So the pass copies this 128-byte buffer at the sun-shaped light draw, beside the light
// and the camera, and requires the light it read to agree with it - the shape rule and the horizon rule cannot tell the sun from another
// directional light the frame also draws, and a wrong light puts the shafts somewhere the sun is not.
inline constexpr uint32_t kShadowBytes = 0x80;    // the bound size of g_DirectionalShadowParameter: the matrix, the distance, the map
inline constexpr uint32_t kShadowRowAt = 0x20;    // m_ShadowProjectionMatrix's third row: the light's axis, negated and scaled by the cascade
inline constexpr uint32_t kShadowCandidates = 4;  // 128-byte constant buffers one light draw may bind: the shadow's and a neighbour's

// The tolerance the light's direction is compared with the shadow's axis at. The engine builds the matrix from the light, so a frame's own sun
// agrees to the buffer's precision; a few degrees leave room for a build that smooths one of the two, and a light that is another direction at
// all is far outside it (the yawed light a session drew from was 48.9 degrees away).
inline constexpr float kShadowAgreementDegrees = 3.0f;

constexpr float Dot3(const float a[3], const float b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// One light draw's 128-byte parameter buffers are not all the shadow's: a captured sun draw binds `g_DirectionalShadowParameter` beside
// `g_FakeSpecularParam`, of the same size. The shadow parameter's own numbers say which is which: its first member is a light-space 4x4 matrix,
// so the fourth row is (0, 0, 0, 1) and the first three rows are mutually orthogonal (a rotation with one scale each), where another 128-byte
// buffer's first sixteen floats are not. The shape is checked before the axis is read, so a neighbour's numbers cannot decide a light.
inline constexpr uint32_t kShadowFloats = kShadowBytes / 4u;
inline constexpr float kShadowShapeTolerance = 1.0e-3f;
constexpr bool ShadowMatrixShape(const float bytes[kShadowFloats]) noexcept
{
    if (bytes == nullptr)
        return false;
    for (uint32_t i = 0; i < kShadowFloats; ++i)
        if (!(bytes[i] == bytes[i]))
            return false;
    // The fourth row of a light-space transform is (0, 0, 0, 1): the matrix has no projective row and no translation of its own.
    if (!(bytes[12] > -kShadowShapeTolerance && bytes[12] < kShadowShapeTolerance) ||
        !(bytes[13] > -kShadowShapeTolerance && bytes[13] < kShadowShapeTolerance) ||
        !(bytes[14] > -kShadowShapeTolerance && bytes[14] < kShadowShapeTolerance) ||
        !(bytes[15] > 1.0f - kShadowShapeTolerance && bytes[15] < 1.0f + kShadowShapeTolerance))
        return false;
    // The first three rows are orthogonal: the light's basis with one scale each. The comparison is relative, so a cascade's own scale does not
    // matter, and squared lengths keep it a comparison of squares (no root in a rule the tests read).
    for (uint32_t row = 0; row < 3; ++row)
    {
        const float* a = bytes + row * 4;
        const float aSquared = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(aSquared > 1.0e-12f))
            return false;
        for (uint32_t other = row + 1; other < 3; ++other)
        {
            const float* b = bytes + other * 4;
            const float bSquared = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
            if (!(bSquared > 1.0e-12f))
                return false;
            const float dot = Dot3(a, b);
            if (!(dot * dot <= kShadowShapeTolerance * kShadowShapeTolerance * aSquared * bSquared))
                return false;
        }
    }
    return true;
}

// One candidate's third row, as the reader keeps it: whether the bytes had the shadow matrix's shape, and the row's axis (the light's direction,
// negated and scaled by the cascade's own scale).
struct ShadowCandidate
{
    bool shaped = false;
    float row[3] {};
};

// The angle between a shadow row's axis and the light's direction, in degrees, or a negative value when the row is not a vector this pass can
// read (zero, a NaN) - a buffer that is not the shadow parameter rather than a light that disagrees. `negated` reads the row the way this
// build's matrix stores it (the row is minus the light); the other sign is the one a build of another layout might store, and it is asked for
// only to keep such a build from losing the effect for a whole session.
inline float ShadowDegrees(const float row[3], const float direction[3], bool negated = true) noexcept
{
    if (row == nullptr || direction == nullptr)
        return -1.0f;
    const float rowSquared = LengthSquared(row);
    const float directionSquared = LengthSquared(direction);
    if (!(rowSquared > 1.0e-12f) || !(rowSquared == rowSquared) || !(directionSquared > 1.0e-12f) || !(directionSquared == directionSquared))
        return -1.0f;
    const float dot = negated ? -Dot3(row, direction) : Dot3(row, direction);
    const float cosine = dot / std::sqrt(rowSquared * directionSquared);
    if (!(cosine == cosine))
        return -1.0f;
    // The angle is in [0, 180], so a direction 180 degrees from the axis is 180 and not a wrapped small value: the caller compares it with the
    // tolerance, and a mirror light has to read as the far side.
    return std::acos(cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine)) * (180.0f / 3.14159265358979f);
}

// What the frame's own shadow rows said about the light the read-back found. `Missing` is a frame whose light draw bound no 128-byte parameter
// buffer with the shadow matrix's shape (or whose copy has not arrived): the light's own direction alone decides the frame, one line says so, and
// the effect is never silently refused for it. `AgreedUnnegated` is a build whose matrix stores the light's axis with the other sign: the
// direction still has to match it within the tolerance, so a light that is another direction fails both. `Refused` is a light that is not the
// frame's sun: its own count and its own line, and the frame is left as it was.
enum class ShadowResult : uint8_t
{
    Missing = 0,
    Agreed = 1,
    AgreedUnnegated = 2,
    Refused = 3,
};

struct ShadowJudgement
{
    ShadowResult result = ShadowResult::Missing;
    float degrees = -1.0f; // the closest candidate's angle to the light (negative: none readable)
};

// The judgement over the candidates one light draw bound, the way this build stores the axis first: a candidate is judged only if its bytes have
// the shadow matrix's shape, so a 128-byte neighbour cannot refuse a light. A build whose matrix stores the light's axis unnegated is accepted
// only after every candidate has failed the negated reading.
inline ShadowJudgement JudgeShadow(const ShadowCandidate* candidates, uint32_t count, const float direction[3]) noexcept
{
    ShadowJudgement judgement;
    if (candidates == nullptr || count == 0 || direction == nullptr)
        return judgement; // no such buffer at this draw: the direction alone decides (logged once)
    float best = -1.0f;
    bool agreed = false;
    float bestUnnegated = -1.0f;
    bool agreedUnnegated = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!candidates[i].shaped)
            continue;
        const float degrees = ShadowDegrees(candidates[i].row, direction);
        if (degrees >= 0.0f && (best < 0.0f || degrees < best))
            best = degrees;
        if (degrees >= 0.0f && degrees <= kShadowAgreementDegrees)
            agreed = true;
        const float other = ShadowDegrees(candidates[i].row, direction, false);
        if (other >= 0.0f && (bestUnnegated < 0.0f || other < bestUnnegated))
            bestUnnegated = other;
        if (other >= 0.0f && other <= kShadowAgreementDegrees)
            agreedUnnegated = true;
    }
    if (agreed)
    {
        judgement.result = ShadowResult::Agreed;
        judgement.degrees = best;
        return judgement;
    }
    if (agreedUnnegated)
    {
        judgement.result = ShadowResult::AgreedUnnegated;
        judgement.degrees = bestUnnegated;
        return judgement;
    }
    if (best < 0.0f)
        return judgement; // no candidate had the shape (or a readable row): Missing, not a refusal
    judgement.result = ShadowResult::Refused;
    judgement.degrees = best;
    return judgement;
}

// The sun a frame draws with when its own light read has not landed: the last light a read accepted and the frame it was accepted at. The module
// keeps one of these; a camera cut empties it, and its weight is HoldFade of its age (0 past kHoldFrames, so the frame is skipped). A refused
// light does not empty it - the frame's light being a lamp or a mirror says nothing about the sun the last read measured - and that is the one
// behaviour the type leaves to its caller.
struct SunHold
{
    bool have = false;
    LightFacts facts {};
    float world[3] { 0.0f, 0.0f, 0.0f }; // the accepted light's direction as the world sees it (WorldDirectionOf): a held sun outlives its view
    uint64_t frame = 0;

    constexpr void Accept(const LightFacts& light, const float worldDirection[3], uint64_t at) noexcept
    {
        have = true;
        facts = light;
        for (uint32_t i = 0; i < 3; ++i)
            world[i] = worldDirection != nullptr ? worldDirection[i] : 0.0f;
        frame = at;
    }

    constexpr void Drop() noexcept
    {
        have = false;
        facts = LightFacts {};
        for (float& value : world)
            value = 0.0f;
        frame = 0;
    }

    // The frame's distance from the accepted read: kHoldFrames + 1 when nothing is held, which is past the last weight and reads as 0.
    constexpr uint32_t Age(uint64_t now) const noexcept
    {
        return have && now >= frame ? uint32_t(now - frame) : uint32_t(kHoldFrames + 1u);
    }

    constexpr float Weight(uint64_t now) const noexcept { return have ? HoldFade(Age(now)) : 0.0f; }
};

// ---- the sun's own draw --------------------------------------------------------------------------------------------------

// A g_LightParam draw's shape, measured at the draw before anything is copied. The engine's directional light pass writes the frame's light
// buffers - the pair of R11G11B10 targets the deferred composite later reads - and it is the only light draw of the frame that does not blend
// into them (the local lights accumulate on top of it). Both facts are needed: the rain-exposure volumes of the wetness mask also declare
// g_LightParam and also bind the depth and the camera (a captured frame's first four light draws are those), and the local lights write the
// same target pair as the sun.
struct SunDrawFacts
{
    uint32_t targets = 0;         // the draw's render targets
    uint32_t targetFormat = 0;    // the first target's view format
    uint32_t targetWidth = 0, targetHeight = 0;
    bool sameTargets = false;     // every target is that format and that size
    bool blended = true;          // the draw blends into its targets
    uint32_t sceneWidth = 0, sceneHeight = 0; // the frame's lit scene, when the composite has already been seen
    bool depth = false;           // the draw binds a depth this pass can read
    bool camera = false;          // and a buffer large enough to hold the camera's projection
};

inline constexpr uint32_t kLightTargets = 2;       // the light-buffer pair: the diffuse and the specular light buffers
inline constexpr uint32_t kLightTargetFormat = 26;  // DXGI_FORMAT_R11G11B10_FLOAT

constexpr bool IsSunDraw(const SunDrawFacts& facts) noexcept
{
    if (facts.targets != kLightTargets || !facts.sameTargets)
        return false;
    if (facts.targetFormat != kLightTargetFormat || facts.targetWidth == 0 || facts.targetHeight == 0)
        return false;
    if (facts.blended)
        return false;
    if (!facts.depth || !facts.camera)
        return false;
    // The pair is allocated at the display size, like the scene the composite writes; a pair of another size is another pass's surface (and one
    // the tone map's window cannot be matched against). The size is only known once a composite has been seen, so the first frames of a session
    // accept any pair of one size.
    if (facts.sceneWidth != 0 && (facts.targetWidth != facts.sceneWidth || facts.targetHeight != facts.sceneHeight))
        return false;
    return true;
}

// ---- the colour key ------------------------------------------------------------------------------------------------------

// `LightShaftsColor`: `auto` is the engine's own colour (the light's diffuse, otherwise the sky's at the sun's screen position), `white` is
// a neutral shaft tinted by nothing but the intensity key. The second exists so a session can tell a colour problem from a geometry problem:
// a white shaft in the wrong place is the projection, a coloured shaft in the wrong place is the colour.
enum class Colour : uint8_t
{
    Auto = 0,
    White = 1,
};

constexpr Colour ParseColour(std::string_view text) noexcept
{
    return text == "white" || text == "White" || text == "WHITE" ? Colour::White : Colour::Auto;
}

// The luminance weights the engine's own tone map uses (its decompiled shader's Rec.709 coefficients), so this test and the engine agree on
// what "bright" means.
inline constexpr float kLumaR = 0.2126390059f;
inline constexpr float kLumaG = 0.7151686788f;
inline constexpr float kLumaB = 0.0721923154f;

// A sun's colour below this luminance is no sun at all: night, deep shade, or a sky the pass sampled at a pixel the sun is not in. The pass
// adds nothing for it and counts no refusal - it is a frame without shafts in it, not a frame it failed to measure.
inline constexpr float kMinTintLuminance = 0.02f;

constexpr float Luminance(const float rgb[3]) noexcept
{
    if (rgb == nullptr)
        return 0.0f;
    return rgb[0] * kLumaR + rgb[1] * kLumaG + rgb[2] * kLumaB;
}

constexpr bool TintBrightEnough(const float rgb[3]) noexcept
{
    if (rgb == nullptr)
        return false;
    if (!(rgb[0] == rgb[0]) || !(rgb[1] == rgb[1]) || !(rgb[2] == rgb[2]))
        return false;
    return Luminance(rgb) >= kMinTintLuminance;
}

// ---- the sun's colour ----------------------------------------------------------------------------------------------------

// Which colour the shafts take: the light's own diffuse when the engine reports one (normalised to a colour of peak 1, so its brightness is not
// the effect's), and otherwise the sky's colour at the sun's screen position (read from the scene at the sun's pixel). The live build's directional
// light reports a zero diffuse, so the second case is the one a player sees; the first keeps a build that reports it exact.
struct SunTint
{
    float rgb[3] { 1.0f, 1.0f, 1.0f };
    bool fromSky = true;
};

constexpr SunTint ResolveSunTint(const float diffuse[3], Colour colour = Colour::Auto) noexcept
{
    SunTint tint;
    if (colour == Colour::White)
    {
        tint.rgb[0] = tint.rgb[1] = tint.rgb[2] = 1.0f;
        tint.fromSky = false; // a neutral shaft: the intensity key alone decides how much light it adds
        return tint;
    }
    if (diffuse == nullptr)
        return tint;
    if (!(LengthSquared(diffuse) > 1.0e-6f) || !(LengthSquared(diffuse) == LengthSquared(diffuse)))
        return tint; // zero or a NaN: the sky at the sun's position is the colour to use
    // A colour the render target could hold: an unbounded value would turn the shafts into a clipped white smear.
    for (uint32_t i = 0; i < 3; ++i)
        tint.rgb[i] = diffuse[i] < 0.0f ? 0.0f : (diffuse[i] > 16.0f ? 16.0f : diffuse[i]);
    // A colour, not a magnitude. The light's diffuse carries its own brightness - a captured frame reports 1.96, another light in the same session
    // 0.06 - and the intensity key is what says how much light the shafts add, so the colour is divided by its brightest component:
    // {1.96, 1.79, 1.58} becomes {1.0, 0.916, 0.807}. Without this the same key meant a sixty-fourth of the frame's white in one scene and several
    // times it in another, and the light's own brightness decided the effect instead of the slider.
    float peak = tint.rgb[0];
    for (uint32_t i = 1; i < 3; ++i)
        peak = tint.rgb[i] > peak ? tint.rgb[i] : peak;
    if (peak > 0.0f)
        for (uint32_t i = 0; i < 3; ++i)
            tint.rgb[i] /= peak;
    tint.fromSky = false;
    return tint;
}

// ---- the engine's formats ------------------------------------------------------------------------------------------------

inline constexpr uint32_t kFp16Format = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT: the lit scene the tone map reads
inline constexpr uint32_t kDepthTypeless = 44; // DXGI_FORMAT_R24G8_TYPELESS: the depth buffer's own format
inline constexpr uint32_t kDepthUnorm = 46;    // DXGI_FORMAT_R24_UNORM_X8_TYPELESS: the view a captured frame binds at the light draws
inline constexpr uint32_t kDepthFloat = 41;    // DXGI_FORMAT_R32_FLOAT: the same depth buffer on a build that binds it as float

constexpr bool IsSceneFormat(uint32_t format) noexcept { return format == kFp16Format; }

// The engine's depth as the light draws bind it: the shader-resource view of an R24G8 depth buffer is R24_UNORM_X8_TYPELESS (46), which is what
// a captured frame reports at the light draws' depth slot; R24G8_TYPELESS (44) and R32_FLOAT (41) are the other bindings a build may make of the
// same buffer. The typeless resource format itself is never a view's format, so it is not in this list.
constexpr bool IsDepthFormat(uint32_t format) noexcept
{
    return format == kDepthTypeless || format == kDepthUnorm || format == kDepthFloat;
}

// Which of a light draw's depth views to read: one at the scene's own size (the frame's depth) beats one of another size (a shadow map or a
// reflection's depth bound in a lower slot), and the first view of the better size wins. The rule exists because the tone map refuses a depth
// whose size is not the scene's, and that refusal would count against the module on every frame of such a session.
constexpr bool BetterDepthView(bool haveBest, bool bestMatchesScene, bool candidateMatchesScene) noexcept
{
    if (!haveBest)
        return true;
    return candidateMatchesScene && !bestMatchesScene;
}

// A projection matrix is recognised by its own shape, not by the slot it sits in: a D3D perspective projection's last row is (0, 0, -1, 0)
// and its first two diagonal entries are the x and y scales (row-major storage, a column-vector multiply, as CameraRegisters.h lays the
// engine's matrices out). `CameraLayoutOk` in the contact shadow rules checks the whole table; the pass only reads the projection, so it checks
// what it reads. The w row is compared with a tolerance rather than for equality: the float32 the buffer holds was computed in double, and a
// live build that stores a slightly different row is still the same projection (a refusal here costs the frame, not the picture).
inline constexpr float kProjectionTolerance = 1.0e-3f;
constexpr bool ProjectionOk(const float projection[kProjectionRows]) noexcept
{
    if (projection == nullptr)
        return false;
    for (uint32_t i = 0; i < kProjectionRows; ++i)
        if (projection[i] != projection[i])
            return false;
    if (!(projection[0] > 0.0f) || !(projection[5] > 0.0f))
        return false;
    if (!(projection[14] < -1.0f + kProjectionTolerance && projection[14] > -1.0f - kProjectionTolerance))
        return false;
    if (!(projection[12] < kProjectionTolerance && projection[12] > -kProjectionTolerance) ||
        !(projection[13] < kProjectionTolerance && projection[13] > -kProjectionTolerance) ||
        !(projection[15] < kProjectionTolerance && projection[15] > -kProjectionTolerance))
        return false;
    return true;
}

// The view-projection is recognised the same way, by what it has to be: the camera's rotation seen through the projection. Its last row's
// first three entries are minus the view matrix's third row (a rotation's row), so they are about unit length - a buffer whose +0x060 holds a
// translation, an inverse or a half-written matrix is refused instead of being projected with. The entries are checked for being finite for the
// same reason ProjectionOk checks its own.
constexpr bool ViewProjectionOk(const float viewProjection[kProjectionRows]) noexcept
{
    if (viewProjection == nullptr)
        return false;
    for (uint32_t i = 0; i < kProjectionRows; ++i)
        if (viewProjection[i] != viewProjection[i])
            return false;
    const float wRow[3] { viewProjection[12], viewProjection[13], viewProjection[14] };
    const float lengthSquared = LengthSquared(wRow);
    return lengthSquared >= 0.9604f && lengthSquared <= 1.0404f; // 0.98^2 .. 1.02^2, like the light's own unit test
}

// ---- the tone-map draw ---------------------------------------------------------------------------------------------------

// What the draw measured before it decided. The tone map is the engine's own post pass that reads the lit scene once and writes the
// post-tone colour; it is recognised by that shape - one FP16 render target at the display allocation's size, a fullscreen draw, and a `t0`
// that is the same resource the frame's deferred composite wrote as the scene - never by a shader CRC or an address.
struct ToneMapFacts
{
    uint32_t renderTargets = 0;  // exactly one, the post-tone colour
    uint32_t targetWidth = 0, targetHeight = 0;
    uint32_t targetFormat = 0;
    bool targetIsScene = false;  // ... and it is not the scene itself: the tone map writes a different texture
    uint32_t sceneFormat = 0;    // the t0 binding
    uint32_t sceneWidth = 0, sceneHeight = 0;
    bool sceneSampled = false;   // t0 is a single-sample 2D view
    bool sceneIsScene = false;   // t0's resource is the scene the frame's composite wrote
    Window window;
    bool replay = false;         // the draw is another module's replay of a game draw
};

// Why the tone-map draw was left alone. Every one of these leaves the game's call exactly as it was, with the game's own scene bound.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,     // the key is off, the intensity is 0, or the module stood down for the session
    NoDepth,        // no light draw of this frame had the sun draw's shape (so there is no depth for this frame)
    NoSun,          // the light draw's bytes have not arrived yet, or they are not a sun this pass could project
    NoCamera,       // the light draw's camera buffer did not hold the projection
    NotToneMap,     // the draw's shape is another post pass
    RenderTarget,   // not exactly one render target, or it is the scene itself
    TargetFormat,   // the tone map's target is not FP16
    SceneFormat,    // the scene is not FP16
    SizeMismatch,   // the scene, the depth and the window do not belong to one frame
    Window,         // the render window is empty or does not fit the scene
    CharaReplay,    // a character-preview replay of a game draw
    PassFailed,     // the pass could not build its textures, shaders or draws
    Exception,      // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the intensity is 0";
    case Decline::NoDepth: return "no light draw of this frame had the sun draw's shape, so its depth is not this frame's";
    case Decline::NoSun: return "no light draw of the frame carried a sun to project";
    case Decline::NoCamera: return "the light draw's camera buffer did not hold the camera's projection";
    case Decline::NotToneMap: return "the draw is not the tone map";
    case Decline::RenderTarget: return "the tone map's render target is not one post-tone texture";
    case Decline::TargetFormat: return "the tone map's render target is not FP16";
    case Decline::SceneFormat: return "the scene is not FP16";
    case Decline::SizeMismatch: return "the scene, the depth and the render window do not belong to one frame";
    case Decline::Window: return "the render window is empty or does not fit the scene";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::PassFailed: return "the pass could not build its textures, shaders or draws";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

constexpr Decline ClassifyToneMap(const ToneMapFacts& facts) noexcept
{
    if (facts.replay)
        return Decline::CharaReplay;
    // "t0 is the frame's own scene" is tested first: a draw of the same shader family that writes the scene back (the engine reuses it for the
    // post-tone copy) or one that reads another texture is not the tone map, and it is passed on without counting - one of those per frame would
    // otherwise stand the module down before its own draw ever came round.
    if (!facts.sceneSampled || !facts.sceneIsScene)
        return Decline::NotToneMap;
    if (facts.targetIsScene)
        return Decline::NotToneMap; // the draw writes the scene itself: the frame's lit picture is what it reads, not the tone map
    if (facts.renderTargets != 1)
        return Decline::RenderTarget;
    if (!IsSceneFormat(facts.targetFormat))
        return Decline::TargetFormat;
    if (!IsSceneFormat(facts.sceneFormat))
        return Decline::SceneFormat;
    if (facts.sceneWidth != facts.targetWidth || facts.sceneHeight != facts.targetHeight)
        return Decline::SizeMismatch;
    if (!WindowUsable(facts.window))
        return Decline::Window;
    if (!(facts.window.x + facts.window.width <= float(facts.sceneWidth) + 0.5f) ||
        !(facts.window.y + facts.window.height <= float(facts.sceneHeight) + 0.5f))
        return Decline::Window;
    if (HalfExtent(facts.window.width) == 0 || HalfExtent(facts.window.height) == 0)
        return Decline::Window;
    return Decline::None;
}

// ---- the engine's scene HDR ----------------------------------------------------------------------------------------------

// The engine's scene HDR path replays the engine's own tone chain in its own branch and never hands the draw path this module's draws, so the
// pass cannot run while that path is requested. The key is a live config expression, not a session fact: it is sampled again wherever the module
// decides, so a session that switched HDR on or off with the shafts key untouched stops and starts the pass without anything else moving.
constexpr bool SceneHdrInactive(bool enabled, bool requested) noexcept { return enabled && requested; }

// The module's view of it, for the one log line a session gets. `Sample` answers true on the sample that first found the module inactive: a key
// left untouched through an HDR toggle therefore writes the line once, not once a frame, and again if HDR is switched on a second time.
struct SceneHdrWatch
{
    bool inactive = false;

    constexpr bool Sample(bool enabled, bool requested) noexcept
    {
        const bool next = SceneHdrInactive(enabled, requested);
        const bool entered = next && !inactive;
        inactive = next;
        return entered;
    }
};

// ---- the log and the cost -------------------------------------------------------------------------------------------------

inline constexpr unsigned kMaxLogLines = 32;

// One count per Decline, indexed by the enum's value, so a caller can carry the table without a guess at its length.
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

inline std::string FormatDecline(Decline reason)
{
    return std::format("Light shafts: the draw was left alone ({})", DeclineName(reason));
}

// A frame the module leaves as it was without counting anything against itself: no sun draw of that frame, a read-back that has not landed, a
// light that turned out not to be the sun, a sun behind the camera. It is a frame without shafts in it, not a refusal.
inline std::string FormatSkip(Decline reason)
{
    if (reason == Decline::None)
        return "Light shafts: the frame was left as it was (the line above says why: no new light and camera bytes to draw with yet)";
    return std::format("Light shafts: the frame was left as it was ({})", DeclineName(reason));
}

// The read-back line: a light whose parameters say it is at a world position is not the sun, and the module looks at the frame's next light
// draw from then on. One line per session, because a build that renames the buffer would repeat it on every frame.
inline std::string FormatNotTheSun(const LightFacts& facts)
{
    return std::format("Light shafts: the light draw at a position ({:.1f}, {:.1f}, {:.1f}) is a lamp, not the sun", double(facts.position[0]),
                       double(facts.position[1]), double(facts.position[2]));
}

// The line for a directional light whose direction points below the horizon: the mirror of a light the frame also draws, and the value that put
// the shafts behind the camera while the sun was on screen. It is named apart from a lamp because it is a directional light, which is the shape
// the sun has: a session that reads it knows the frame drew two of them and that the module took the other one.
inline std::string FormatBelowHorizon(const LightFacts& facts)
{
    return std::format("Light shafts: the directional light's direction ({:.4f}, {:.4f}, {:.4f}) points below the horizon, so it is the sun's "
                       "mirror and not the sun",
                       double(facts.direction[0]), double(facts.direction[1]), double(facts.direction[2]));
}

// The projection's own facts, once per session: the row the shape check reads, so a capture of a session says whether the buffer really holds a
// perspective projection (the w row must be (0, 0, -1, 0) up to a thousandth).
inline std::string FormatProjection(const float projection[kProjectionRows])
{
    if (projection == nullptr)
        return "Light shafts: the camera's projection was not read";
    return std::format("Light shafts: the camera's projection is x {:.4f}, y {:.4f}, w row ({:.4f}, {:.4f}, {:.4f}, {:.4f})",
                       double(projection[0]), double(projection[5]), double(projection[12]), double(projection[13]), double(projection[14]),
                       double(projection[15]));
}

// The camera's view-projection, once per session, beside the projection line: the matrix the sun's world direction is projected through, all four
// rows at four decimals. The projection line alone cannot tell a wrong direction from a wrong matrix, so a session reads here whether the buffer's
// +0x060 really holds the view-projection (the rotation in its first three columns) or a copy of another matrix.
inline std::string FormatViewProjection(const float viewProjection[kProjectionRows])
{
    if (viewProjection == nullptr)
        return "Light shafts: the camera's view-projection was not read";
    return std::format("Light shafts: the camera's view-projection is ({:.4f}, {:.4f}, {:.4f}, {:.4f}), ({:.4f}, {:.4f}, {:.4f}, {:.4f}), "
                       "({:.4f}, {:.4f}, {:.4f}, {:.4f}), ({:.4f}, {:.4f}, {:.4f}, {:.4f})",
                       double(viewProjection[0]), double(viewProjection[1]), double(viewProjection[2]), double(viewProjection[3]),
                       double(viewProjection[4]), double(viewProjection[5]), double(viewProjection[6]), double(viewProjection[7]),
                       double(viewProjection[8]), double(viewProjection[9]), double(viewProjection[10]), double(viewProjection[11]),
                       double(viewProjection[12]), double(viewProjection[13]), double(viewProjection[14]), double(viewProjection[15]));
}

// The one line a session gets when the engine's scene HDR comes on while the shafts key is on: that path replays the engine's tone chain in its
// own branch, so this module's draw path never runs and the pass cannot draw. Saying so beats a card that looks armed. The line is written once
// per transition into that state, not once per session: the key can be switched off again without the shafts key moving.
inline constexpr const char* kSceneHdrInactive =
    "Light shafts: inactive while scene HDR is on, because that path replaces the tone chain this pass draws before";

inline constexpr const char* kReadbackStale =
    "Light shafts: the frame's light and camera bytes stopped arriving, so the pass is leaving the frames alone";

// The line a session gets when the light's bytes arrive but the camera's have not landed yet: the camera map is answered "still drawing", which
// is a GPU that is behind (or a light the read-back has just dropped), not a build mismatch. The frame is skipped without a refusal.
inline constexpr const char* kCameraPending =
    "Light shafts: the camera's bytes have not landed yet, so the frame was left as it was";

// The line a frame writes when its light was refused as the sun's mirror: the frame is left as it was, and the read-back's own line (written
// once a session) already named the light it refused. It is its own line so a session does not read a light draw that did happen as one that did
// not.
inline constexpr const char* kBelowHorizonSkip =
    "Light shafts: the frame was left as it was (its directional light was the sun's mirror, not the sun)";

// The line a session gets when the sun-shaped light draw bound no 128-byte parameter buffer, or its copy has not arrived: the frame still draws,
// placed by the light's own direction alone. It is said once, because a build whose layout has no such buffer would otherwise repeat it every
// frame - and a session that reads it knows the identity rule is not the one placing its shafts.
inline constexpr const char* kShadowAnchorMissing =
    "Light shafts: this frame's light draw bound no directional-shadow parameters, so the light's own direction is what places the sun (the "
    "shadow's axis is the check, not a second source)";

// The line a session gets when the frame's shadow matrix stores the light's axis with the other sign: the direction is still required to match it,
// so a light that is another direction is refused either way, and a build of that layout does not lose the effect.
inline constexpr const char* kShadowUnnegated =
    "Light shafts: the frame's directional shadow stores the light's axis unnegated, so the direction is matched against it with the other sign";

// The line a frame writes when its light was refused by the frame's own directional shadow: the frame is left as it was, and the read-back's own
// line (written once a session) named the light and the angle. It is its own line so a session does not read a light draw that did happen as one that
// did not.
inline constexpr const char* kNotTheSunSkip =
    "Light shafts: the frame was left as it was (the light the frame drew is not the one its directional shadow is built from, so it is not the "
    "sun)";

// The line for a light the frame's own shadow refused: the direction the light reported, the shadow row that says otherwise, and the angle
// between them. It is the one measurement that tells a session its light was not the sun, and it is written once a session (the same light
// repeats on every frame).
inline std::string FormatShadowRefused(const float direction[3], float degrees)
{
    const float dx = direction != nullptr ? direction[0] : 0.0f;
    const float dy = direction != nullptr ? direction[1] : 0.0f;
    const float dz = direction != nullptr ? direction[2] : 0.0f;
    return std::format("Light shafts: the light's direction ({:.4f}, {:.4f}, {:.4f}) is {:.1f} degrees from the frame's directional shadow, so it "
                       "is not this frame's sun",
                       double(dx), double(dy), double(dz), double(degrees));
}

// The line a session gets when the view cut while a sun was held: the held sun is dropped instead of lighting the new picture from the old one's
// sky, and the frames after the cut draw again once a light lands.
inline constexpr const char* kCameraCut =
    "Light shafts: the camera cut since the last sun, so the sun it was holding was dropped";

// A session whose maps have been answered "still drawing" this many times and that has never read anything is a stalled read-back, not a frame
// without a sun: one line says so instead of the "no sun" line naming the wrong cause.
inline constexpr uint32_t kReadbackStallFrames = 64;
inline constexpr const char* kReadbackNotLanding =
    "Light shafts: the frame's light and camera bytes have never landed, so the maps are not ready and the frames are left alone";

// The one line that says the module stood itself down for the session.
inline std::string FormatSwitchedOff(uint32_t limit)
{
    return std::format("Light shafts: {} of the module's draws in a row were left alone, so it is off for this session", limit);
}

// The line of a frame that ran: the sun's screen position and the window, the direction the light reported and how far outside the window it is,
// and what the pass did with it. The direction, the offscreen value and the light buffer's own address are what a session needs to settle a light
// that is not the sun: the reported position follows from them by the projection the log already prints, and the address ties the line to the
// capture's `cbdump.csv` row for the same buffer. The held sun's fade is printed too, so a frame drawn from the last accepted sun is not mistaken
// for one that read the frame's own.
inline std::string FormatRun(const SunProjection& sun, const Window& window, const Params& params, uint32_t sceneWidth,
                             uint32_t sceneHeight, const float direction[3], float fade, const void* light, float shadowDegrees,
                             float whitePoint = kFallbackWhitePoint)
{
    const float dx = direction != nullptr ? direction[0] : 0.0f;
    const float dy = direction != nullptr ? direction[1] : 0.0f;
    const float dz = direction != nullptr ? direction[2] : 0.0f;
    const std::string shadow = shadowDegrees >= 0.0f ? std::format("{:.1f} deg", double(shadowDegrees)) : std::string("no row");
    return std::format("Light shafts: the sun at {:.1f}x{:.1f} of the {}x{} window inside a {}x{} scene, {:.2f} windows outside, direction "
                       "({:.4f}, {:.4f}, {:.4f}) of the light at {:x}, shadow {}, white {:.3f}, fade {:.2f}, {} samples, decay {:.3f}, "
                       "density {:.2f}, intensity {:.2f}",
                       double(sun.screenX), double(sun.screenY), uint32_t(window.width), uint32_t(window.height), sceneWidth, sceneHeight,
                       double(sun.offscreen), double(dx), double(dy), double(dz), uintptr_t(light), shadow, double(whitePoint), double(fade),
                       params.samples, double(params.decay), double(params.density), double(params.intensity));
}

// The line the pass writes when the frame's own tone map parameters first arrive: the white point the bound uses, and the exposure it is the
// reciprocal of, so a session reads the bound instead of guessing it from the picture.
inline std::string FormatWhitePoint(float whitePoint, float exposure)
{
    return std::format("Light shafts: the frame's white point is {:.3f} (the tone map's exposure {:.3f} is its reciprocal)", double(whitePoint),
                       double(exposure));
}

// The line a session gets while the frame's own exposure has not arrived (or has stopped): the bound is the captured frame's measurement, and the
// session reads it rather than guessing it from the picture. This line used to be built and never written.
inline constexpr const char* kWhitePointFallback =
    "Light shafts: the tone map's own exposure has not landed, so the frame's white point is the captured measurement, 0.838: the shafts are "
    "drawn as the daylight frame they are in";

// The one line a skipped frame writes when the reason changes: a sun behind the camera or out of the frame is not a refusal (nothing is
// counted against the module), so it has its own line instead of a drop-list reason.
inline std::string FormatSunSkipped(const SunProjection& sun, const Window& window)
{
    if (sun.behind)
        return "Light shafts: the sun is behind the camera this frame, so the frame was left as it was";
    return std::format("Light shafts: the sun is {:.2f} windows outside the {}x{} viewport, so the frame was left as it was",
                       double(sun.offscreen), uint32_t(window.width), uint32_t(window.height));
}

// The line a session gets the first time a frame's shafts are placed with the sky's own sun rather than the light's direction: the two disagree
// whenever the engine clamps the shadow-casting light's elevation, and a session that reads one sun position on the card needs to know which of
// them the pass used. Written once, because the UVs move every frame.
inline std::string FormatSunUv(float uvX, float uvY, const SunProjection& sun)
{
    return std::format("Light shafts: the sky draws its sun at uv ({:.4f}, {:.4f}), so the shafts are placed at {:.1f}x{:.1f} of the window instead "
                       "of where the shadow-casting light's clamped direction points",
                       double(uvX), double(uvY), double(sun.screenX), double(sun.screenY));
}

// Why a frame had no shafts in it, in the five reasons the card counts apart. One `skipped` number for "no sun among the frame's light draws",
// "the light's bytes not arrived", "the sun out of view" and "the light below the horizon" is what made a session read "3259 without the sun in
// view" for three different things and left its question unanswerable; the fifth is the light the frame's own directional shadow refused. The
// first covers a frame whose light draws none had the sun's shape and a frame whose light was a lamp, so its label names no draw's shape.
enum class SkipReason : uint8_t
{
    NoDepth = 0,      // no sun among the frame's light draws: none had the sun draw's shape, or its light was a lamp
    NoSun = 1,        // the light's bytes had not arrived, or carried no sun this pass could project
    OffScreen = 2,    // the sun projected behind the camera or outside the viewport margin
    BelowHorizon = 3, // the frame's light was refused as the sun's mirror
    NotTheSun = 4,    // the frame's light's direction is not the one its directional shadow is built from
};

// The card's counters. `skipped` is the number of frames left alone without the shafts, and the five reasons are its whole decomposition: a frame
// is counted through CountSkip once, so no reason is counted again under another. An earlier build refused a light below the horizon once in the
// read-back and again as a frame without a light draw, so the numbers printed under one `skipped` added up to more than it.
struct CardCounts
{
    uint32_t skipped = 0;
    uint32_t noDepth = 0;
    uint32_t noSun = 0;
    uint32_t offScreen = 0;
    uint32_t belowHorizon = 0;
    uint32_t notTheSun = 0;
};

// The one place a frame is counted as skipped: `skipped` and exactly one of the five reasons move together.
constexpr void CountSkip(CardCounts& counts, SkipReason reason) noexcept
{
    ++counts.skipped;
    switch (reason)
    {
    case SkipReason::NoDepth: ++counts.noDepth; return;
    case SkipReason::NoSun: ++counts.noSun; return;
    case SkipReason::OffScreen: ++counts.offScreen; return;
    case SkipReason::BelowHorizon: ++counts.belowHorizon; return;
    case SkipReason::NotTheSun: ++counts.notTheSun; return;
    }
}

// The card's one line, from the numbers a session counted: the reasons above print apart, and they are the whole of `skipped`, so a session can add
// them up and read where every skipped frame went. `sunX`/`sunY` are written on every frame the sun was projected for, so the card stops saying
// "0,0 of a 0x0 window" as soon as a frame is measured.
struct CardNumbers
{
    float intensity = kDefaultIntensity;
    uint64_t frames = 0; // the module's own count of rendered frames: the base of the share
    uint32_t runs = 0;
    uint32_t held = 0; // of those, the frames drawn with the last accepted sun (this frame's own read had not landed)
    uint32_t declined = 0;
    uint32_t skipped = 0;
    uint32_t noDepth = 0;
    uint32_t noSun = 0;
    uint32_t offScreen = 0;
    uint32_t belowHorizon = 0;
    uint32_t notTheSun = 0;
    uint32_t refused = 0; // the read-back refused a light the frame's own shadow disagreed with; a refused read never draws, and a frame drawn
                          // from a held sun is not this count
    uint32_t reads = 0;
    uint32_t notReady = 0;
    float sunX = 0.0f, sunY = 0.0f;
    uint32_t windowWidth = 0, windowHeight = 0;
};

inline std::string FormatCard(const CardNumbers& numbers)
{
    return std::format("on at {:.2f}: {} rendered frames, {} with the shafts ({} of them drawn with a held sun), {} left alone, {} with no sun to "
                       "draw (no sun among the frame's light draws: {}, the light's bytes not arrived: {}, the sun out of view: {}, the light below the "
                       "horizon: {}, the light not the frame's sun: {}); the read-back: {} reads, {} not ready, {} refused as not the sun; the last sun at {:.0f},{:.0f} of a "
                       "{}x{} window",
                       double(numbers.intensity), numbers.frames, numbers.runs, numbers.held, numbers.declined, numbers.skipped,
                       numbers.noDepth, numbers.noSun, numbers.offScreen, numbers.belowHorizon, numbers.notTheSun, numbers.reads, numbers.notReady,
                       numbers.refused,
                       double(numbers.sunX), double(numbers.sunY), numbers.windowWidth, numbers.windowHeight);
}

// How often the card's numbers are written to the log, in rendered frames: about a minute at 60 Hz. The card's counts are the only place a session
// can see what share of its frames drew, and the log used to carry none of them - which is why "the pass drew on two of 1272 frames" had to be
// guessed from a cost line. A session now reads the share straight out of the log.
inline constexpr uint64_t kCountsLogFrames = 3600;

// The on-cost line, in the numbers the design counts it with: what the pass costs on the GPU (one timestamp pair around its four draws,
// read a run or two later and never waited for) and what the frame's draws came to.
inline std::string FormatCost(bool haveGpuMilliseconds, double gpuMilliseconds, uint64_t frames, uint32_t runs, uint32_t declined)
{
    const std::string gpu = haveGpuMilliseconds ? std::format("{:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string("its GPU time not measured yet");
    return std::format("Light shafts: {} over {} frames ({} left alone); one pass a frame, {}", gpu, frames, declined, runs);
}

// ---- the anchors this module claims in the engine module's shared word ---------------------------------------------------

// The bits and the slot field are the engine module's map (EnginePassRules.h), which lists every field of the packed word and refuses at compile
// time any two that overlap: the light pass's bit, the composite's bit, the tone map's bit and the light parameter's slot field. This module keeps
// no private copy of them, so a renumber is one edit in the map. The knob is this module's own: it counts the shafts' refusals apart from the two
// shipped knobs' streaks.
static_assert(uint32_t(engine::Knob::LightShafts) == 2, "the light shafts' refusal knob moved");
} // namespace ext::ffxiv::shafts
