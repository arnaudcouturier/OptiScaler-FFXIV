#pragma once
// Contact shadows before the sun light pass: the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered
// by tests/ext/ffxiv/relight on the host.
//
// What the feature does, in one line: the engine builds a screen-space sun visibility mask (a B8G8R8A8_UNORM texture the size of the
// display allocation, 0..1, accumulated with MIN) and the sun light draw samples it as `g_SamplerShadow`. A short march along the sun
// direction over the view-position buffer finds the contact occlusion the cascade map cannot resolve (a character's shadow at its
// feet, a prop on the ground), and the mask is multiplied by it in a private copy that is bound for that one draw.
//
// Everything that can be decided without a device lives here: which draw is a sun light draw, which of its texture bindings is the mask
// (the cascade atlas shares the same reflected name, so only size and format tell them apart), whether the render window is usable, and
// when the module gives up for the session. The fail-open contract is the drop list below: every reason leaves the game's draw exactly
// as it was, with the game's own shader resource still bound.

#include <cstdint>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::relight
{
// ---- the keys ---------------------------------------------------------------------------------------------------------

// Range of the length key, in metres. The contact term is meant to sit below the cascade map's texel footprint and above the engine's
// own AO radius (half a metre in practice); a longer march costs more and starts to look like a second shadow map.
inline constexpr float kMinLength = 0.05f;
inline constexpr float kMaxLength = 2.0f;
inline constexpr float kDefaultLength = 0.5f;
// How much of the sun's visibility the term may take away at its strongest (0 = nothing, 1 = a fully black contact shadow).
inline constexpr float kMinStrength = 0.0f;
inline constexpr float kMaxStrength = 1.0f;
inline constexpr float kDefaultStrength = 0.5f;
// Step budget, clamped: fewer than four cannot see a contact, more than 24 is no longer a "short" march and the per-pixel cost of the
// half-resolution pass grows with it.
inline constexpr int32_t kMinSteps = 4;
inline constexpr int32_t kMaxSteps = 24;
inline constexpr int32_t kDefaultSteps = 12;

// The sun vector's space: whether the game reports it in world space (then the view matrix turns it into view space) or already in view
// space. The measurement this feature was built on found VIEW space: the direction the game reports changed with the camera while
// `direction * g_CameraParameter`'s view matrix stayed the same per scene, which is exactly a view-space direction. The key exists because
// the same measurement is what has to be redone after a game patch.
enum class SunSpace : uint8_t
{
    World = 0,
    View = 1,
};

// The sign: whether the game's direction points from the surface toward the sun (the ray to trace) or the way the light travels (the
// ray's opposite). The measured vector points up, which reads as "toward the sun".
enum class SunSign : uint8_t
{
    Toward = 0,
    From = 1,
};

constexpr SunSpace ParseSunSpace(std::string_view text) noexcept
{
    return text == "world" || text == "WORLD" || text == "World" ? SunSpace::World : SunSpace::View;
}

constexpr SunSign ParseSunSign(std::string_view text) noexcept
{
    return text == "from" || text == "FROM" || text == "From" ? SunSign::From : SunSign::Toward;
}

struct Params
{
    float length = kDefaultLength;
    float strength = kDefaultStrength;
    uint32_t steps = uint32_t(kDefaultSteps);
    SunSpace space = SunSpace::View; // `auto` reads as view: the measured space
    SunSign sign = SunSign::Toward;   // `auto` reads as toward the sun: the measured sign
};

constexpr float ClampLength(float v) noexcept { return v < kMinLength ? kMinLength : (v > kMaxLength ? kMaxLength : v); }
constexpr float ClampStrength(float v) noexcept { return v < kMinStrength ? kMinStrength : (v > kMaxStrength ? kMaxStrength : v); }
constexpr uint32_t ClampSteps(int32_t v) noexcept
{
    const int32_t steps = v < kMinSteps ? kMinSteps : (v > kMaxSteps ? kMaxSteps : v);
    return uint32_t(steps);
}

// The parameters a pass run uses, from the four keys. A strength of zero is no term at all, which the caller reads as "do not run".
constexpr Params BuildParams(float length, float strength, int32_t steps, SunSpace space, SunSign sign) noexcept
{
    Params params;
    params.length = ClampLength(length);
    params.strength = ClampStrength(strength);
    params.steps = ClampSteps(steps);
    params.space = space;
    params.sign = sign;
    return params;
}

// ---- the drop list ----------------------------------------------------------------------------------------------------

// Why a run was left alone. Every one of these leaves the game's draw exactly as it was, with the game's own shader resource bound,
// and each is one line of the log the first time it happens.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,          // the key is off, or the module switched itself off for the session
    StrengthZero,        // ContactShadowsStrength = 0: no term to apply
    NoMask,              // the draw declares no g_SamplerShadow
    NoDepth,             // no g_SamplerDepth
    NoViewPosition,      // no g_SamplerViewPosition (no longer a refusal: the position comes from the depth)
    NoLightParam,        // no g_LightParam constant buffer
    NoCameraParam,       // no g_CameraParameter constant buffer (the view matrix to turn the sun into view space)
    MaskNotA2D,          // the binding is not a single-sample 2D texture
    MaskMsaa,            // multisampled
    MaskArrayOr3D,       // a texture array or a volume view
    MaskFormat,          // not an 8-bit colour format: the cascade atlas (R8_UNORM) or anything else
    MaskSize,            // not the display allocation's size
    DepthSize,           // the depth binding is not the mask's size
    ViewPositionSize,    // the view-position binding is not the mask's size
    WindowDegenerate,    // the draw's render window is empty or outside the mask
    WindowNotInMask,     // the window does not intersect the mask
    SunNotUnit,          // the direction is not a unit vector (a patch changed the layout)
    SunMoving,           // the direction changed faster than time of day allows
    CameraLayout,        // the camera buffer is not the layout the shaders read (a patch changed it)
    Reentry,             // the call came from one of our own draws (the scene HDR replay, a preview)
    TextureFailed,       // our own copy could not be created
    PassFailed,          // the pass itself failed
    Budget,              // step or query budget exceeded
    Exception,           // anything thrown
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off";
    case Decline::StrengthZero: return "ContactShadowsStrength is 0";
    case Decline::NoMask: return "the draw declares no g_SamplerShadow";
    case Decline::NoDepth: return "the draw declares no g_SamplerDepth";
    case Decline::NoLightParam: return "the draw declares no g_LightParam";
    case Decline::NoCameraParam: return "the draw declares no g_CameraParameter";
    case Decline::MaskNotA2D: return "the mask binding is not a single-sample 2D texture";
    case Decline::MaskMsaa: return "the mask binding is multisampled";
    case Decline::MaskArrayOr3D: return "the mask binding is a texture array or a volume";
    case Decline::MaskFormat: return "the mask binding has an unexpected format";
    case Decline::MaskSize: return "the mask is not the size of the draw's full-resolution buffers";
    case Decline::WindowDegenerate: return "the render window is empty";
    case Decline::WindowNotInMask: return "the render window is outside the mask";
    case Decline::SunNotUnit: return "the sun direction is not a unit vector";
    case Decline::SunMoving: return "the sun direction moves faster than the sun can";
    case Decline::CameraLayout: return "the camera buffer is not the layout these shaders read";
    case Decline::TextureFailed: return "the mask copy could not be created";
    case Decline::PassFailed: return "the pass failed";
    case Decline::Reentry: return "the call came from one of our own passes";
    case Decline::Budget: return "the step or query budget was exceeded";
    case Decline::Exception: return "an exception was thrown";
    }
    return "unknown reason";
}

// ---- the draw --------------------------------------------------------------------------------------------------------

// The format a mask must have. B8G8R8A8_UNORM is what the capture measured for the screen-space mask and for the engine's own AO
// buffer; an R8_UNORM binding of the same name is the cascade atlas and is rejected by format *and* by size.
inline constexpr bool IsMaskFormat(uint32_t format) noexcept { return format == 87u; } // DXGI_FORMAT_B8G8R8A8_UNORM

// The reflected names this feature depends on. A draw is a sun light draw when its pixel shader declares the mask, the depth and the two
// constant buffers. A view-position binding is NOT required: the sun light draw of the shipped build does not bind one (the capture shows
// g_SamplerDepth and g_SamplerShadow but no g_SamplerViewPosition), so the pass reconstructs the position from the depth and the camera's
// inverse projection instead.
inline constexpr const char* kMaskSampler = "g_SamplerShadow";inline constexpr const char* kDepthSampler = "g_SamplerDepth";
inline constexpr const char* kViewPositionSampler = "g_SamplerViewPosition"; // not required; kept named because a patch could add one
inline constexpr const char* kLightParamBuffer = "g_LightParam";
inline constexpr const char* kCameraParamBuffer = "g_CameraParameter";

// Whether a pixel shader's bytecode can be a sun light draw at all: it must name the mask. This byte scan is what lets the index be filled
// whatever the key says - the shaders of a session are created long before the menu can be reached - while the reflection parse itself
// runs only for the few shaders whose bytes contain the mask's name. The names arrive as the effect compiler writes them
// (`g_SamplerShadow.T`), so the plain name is a prefix of what the blob holds.
inline bool MentionsMaskName(const void* code, size_t size) noexcept
{
    constexpr char needle[] = "g_SamplerShadow";
    constexpr size_t length = sizeof needle - 1;
    if (code == nullptr || size < length)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i + length <= size; ++i)
    {
        size_t j = 0;
        while (j < length && bytes[i + j] == static_cast<unsigned char>(needle[j]))
            ++j;
        if (j == length)
            return true;
    }
    return false;
}

// The render window of the draw, in the mask's pixels.
struct Window
{
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

struct DrawFacts
{
    bool hasMask = false, hasDepth = false, hasViewPosition = false, hasLightParam = false, hasCameraParam = false;
    uint32_t maskFormat = 0;
    uint32_t maskWidth = 0, maskHeight = 0;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint32_t viewPositionWidth = 0, viewPositionHeight = 0;
    uint32_t displayWidth = 0, displayHeight = 0; // the draw's full-resolution allocation, taken from its depth binding
    uint32_t maskSamples = 1;
    bool mask2D = true;  // a plain Texture2D view (not an array, not a volume)
    Window window;
};

constexpr bool SameSize(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh) noexcept { return aw == bw && ah == bh; }

// The whole draw classification, in the order the drop list is written: the first reason that applies is the reason the log names.
constexpr Decline Classify(const DrawFacts& facts) noexcept
{
    if (!facts.hasMask)
        return Decline::NoMask;
    if (!facts.hasDepth)
        return Decline::NoDepth;
    if (!facts.hasLightParam)
        return Decline::NoLightParam;
    if (!facts.hasCameraParam)
        return Decline::NoCameraParam;
    if (!facts.mask2D)
        return Decline::MaskArrayOr3D;
    if (facts.maskSamples != 1)
        return Decline::MaskMsaa;
    if (!IsMaskFormat(facts.maskFormat))
        return Decline::MaskFormat;
    if (facts.maskWidth == 0 || facts.maskHeight == 0)
        return Decline::MaskNotA2D;
    // The screen-space mask is the size of the display allocation, so it is the size of the draw's full-resolution depth binding.
    // The cascade atlas (2048x10240) fails this and the format test; nothing in this list ever binds the atlas by accident.
    if (!SameSize(facts.maskWidth, facts.maskHeight, facts.displayWidth, facts.displayHeight) || !facts.depthWidth || !facts.depthHeight)
        return Decline::MaskSize;
    if (facts.window.width < 1.0f || facts.window.height < 1.0f)
        return Decline::WindowDegenerate;
    // A window whose extent leaves the mask is a resolution the design has not seen: leave the draw alone rather than clamp blindly.
    if (facts.window.x < 0.0f || facts.window.y < 0.0f || facts.window.x + facts.window.width > float(facts.maskWidth) + 1.0f ||
        facts.window.y + facts.window.height > float(facts.maskHeight) + 1.0f)
        return Decline::WindowNotInMask;
    return Decline::None;
}

// The half-resolution target of the march from the window: half in each axis, at least one pixel.
constexpr uint32_t HalfExtent(float pixels) noexcept
{
    const float half = pixels * 0.5f;
    const uint32_t rounded = uint32_t(half);
    return rounded < 1 ? 1 : rounded;
}

// ---- the sun direction -------------------------------------------------------------------------------------------------

// A unit vector within one percent (the value the capture measured is exactly 1.0; a patch that moves the member would not be).
constexpr bool IsUnitDirection(float x, float y, float z) noexcept
{
    const float lengthSquared = x * x + y * y + z * z;
    return lengthSquared > 0.9604f && lengthSquared < 1.0404f; // (1 -+ 0.02)^2, i.e. within 1 % of a unit length on both sides
}

// A direction that moved faster than the sun can between two runs: the game's time of day advances slowly, and a jump means the value
// is not the sun (a spot light, a preview camera, a renamed member). 5 degrees per second is the design's ceiling.
inline constexpr float kCosMaxStepPerSecond = 0.9962f; // cos(5 degrees)

// `seconds` = the time between two samples of the direction. More than five degrees inside a second is not the sun: either the member
// moved in a patch, or the value is something else that changed (a preview camera's light).
constexpr bool MovesTooFast(float cosAngle, double seconds) noexcept
{
    if (!(seconds > 0.0))
        return false; // no baseline yet: nothing to compare the direction with
    return seconds < 1.0 && cosAngle < kCosMaxStepPerSecond;
}

// ---- the camera buffer -------------------------------------------------------------------------------------------------

// The layout of g_CameraParameter the shaders read is in shaders/CameraRegisters.h (the reflected layout of the engine's own light shader).
// On the first light draw of a session the pass copies the first CS_CAMERA_CHECK_BYTES of it to the CPU once and checks that it really is a
// camera: a view-projection and its inverse that multiply to the identity, a projection and its inverse likewise, and an orthonormal view
// basis. A game patch that moves a matrix then fails this check on the first light draw instead of shading with nonsense.
inline constexpr uint32_t kCameraCheckFloats = 22 * 4;
inline constexpr float kCameraCheckTolerance = 2e-3f; // the matrices are float32 and the inverses were computed in double
// A camera buffer the check refuses is looked at again after this long instead of switching the feature off: the first light draws of a
// session can carry a loading camera, and the check is the thing that tells a patch from a loading screen.
inline constexpr double kCameraCheckSeconds = 5.0;

// `rows` is `count` float4 rows, m_ViewMatrix's three at 0, the inverse view's at 3, the view-projection's at 6, the inverse
// view-projection's at 10, the inverse projection's at 14 and the projection's at 18.
constexpr bool CameraLayoutOk(const float* rows, size_t count) noexcept
{
    if (rows == nullptr || count < kCameraCheckFloats)
        return false;
    // The view matrix's 3x3: three unit rows, mutually perpendicular.
    for (uint32_t r = 0; r < 3; ++r)
    {
        const float* a = rows + r * 4;
        const float lengthSquared = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(lengthSquared > 0.95f && lengthSquared < 1.05f))
            return false;
        for (uint32_t s = r + 1; s < 3; ++s)
        {
            const float* b = rows + s * 4;
            const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (!(dot > -0.05f && dot < 0.05f))
                return false;
        }
    }
    // Two products that must be the identity: the view-projection with its inverse (rows 6..9 and 10..13), and the projection with its
    // inverse (rows 18..21 and 14..17).
    const auto productIsIdentity = [rows](uint32_t left, uint32_t right) {
        for (uint32_t r = 0; r < 4; ++r)
            for (uint32_t c = 0; c < 4; ++c)
            {
                float sum = 0.0f;
                for (uint32_t k = 0; k < 4; ++k)
                    sum += rows[(left + r) * 4 + k] * rows[(right + k) * 4 + c];
                const float expected = r == c ? 1.0f : 0.0f;
                if (!(sum > expected - kCameraCheckTolerance && sum < expected + kCameraCheckTolerance))
                    return false;
            }
        return true;
    };
    return productIsIdentity(6, 10) && productIsIdentity(18, 14);
}

// A refusal about the draw's *shape*: the indexed shader binds something other than the screen-space mask. The cascade atlas writer declares
// the same four names as the sun light draw and binds the 2048x10240 atlas instead, so its draws reach the classification on every shadow
// pass and are refused there. Those refusals say "this is a different draw", not "this build lost a binding": they are logged once, the
// draw is skipped from then on (Inject.cpp remembers it for a few seconds, because a resolution change re-creates the mask and the new one
// must be looked at again), and they do not count towards the switch-off below. A camera buffer that is not the layout the shaders read is
// the same kind of answer: it is judged again after a few seconds rather than switching the feature off for the session.
inline constexpr bool IsShapeDecline(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::MaskNotA2D:
    case Decline::MaskMsaa:
    case Decline::MaskArrayOr3D:
    case Decline::MaskFormat:
    case Decline::MaskSize:
    case Decline::CameraLayout:
        return true;
    default:
        return false;
    }
}

// How long a remembered shape refusal keeps a shader's draws out of the classification. A resolution change re-creates the mask within a
// frame or two; the atlas writer, whose draws are refused for their size, is looking at a 2048x10240 binding that no resolution change
// touches. Three seconds is long enough that the atlas writer's weightless refusals cost one inspection every few seconds and short enough
// that a re-created mask is picked up while a cutscene is still loading.
inline constexpr double kShapeRefusalSeconds = 3.0;

// Whether a remembered shape refusal still keeps this shader's draws out of the classification. A refusal is looked at again once it is
// older than kShapeRefusalSeconds, so a re-created mask (a resolution change) is picked up within a few frames while the atlas writer, whose
// 2048x10240 binding no resolution change touches, still costs one inspection every few seconds instead of one per shadow pass.
constexpr bool ShapeRefusalActive(int64_t refusedQpc, int64_t nowQpc, int64_t frequency) noexcept
{
    if (refusedQpc <= 0 || frequency <= 0 || nowQpc < refusedQpc)
        return false;
    return double(nowQpc - refusedQpc) / double(frequency) < kShapeRefusalSeconds;
}

// ---- the seam's draw, once -------------------------------------------------------------------------------------------

// The draw hook's body runs the game's draw exactly once per call. The pass belongs to the draw the frame makes, so the first call runs
// it through `bound` (which runs the draw itself with the mask copy bound) and every replay of the same draw - the scene HDR path re-runs
// a draw to repeat the tone chain, the lighting scan runs it twice - runs the draw alone through `plain`. A replay must neither run the
// pass again (twice the cost, a second write of the mask copy) nor be dropped (the draw would go missing from the frame's picture), so
// both calls are made here and a test counts them.
template <class Bound, class Plain> inline void RunDrawOnce(bool& first, bool replayed, Bound&& bound, Plain&& plain)
{
    if (first && !replayed)
    {
        first = false;
        bound();
        return;
    }
    plain();
}

// ---- the cost meter ----------------------------------------------------------------------------------------------------

// The on-cost of the draw hook, measured on the game's own thread: after kCostEveryDraws draws, the next kCostSampleDraws have their
// lookup bracketed with the clock, and one line reports the average together with the seam's own cost. The clock is never read on the draws
// between samples: those pay one counter and the sample's own two tests. A session runs about nine thousand draws a frame.
inline constexpr uint32_t kCostEveryDraws = 262144;
inline constexpr uint32_t kCostSampleDraws = 64;
inline constexpr double kCostBudgetMilliseconds = 0.02;

struct CostMeter
{
    uint32_t skipped = 0;  // draws seen since the last sample
    uint32_t sampled = 0;  // draws bracketed in this sample
    int64_t totalQpc = 0;  // the bracketed time of the sample
    bool logged = false;

    bool Sampling() const noexcept { return sampled < kCostSampleDraws && skipped >= kCostEveryDraws; }
    bool Done() const noexcept { return sampled >= kCostSampleDraws; }

    void Skip()
    {
        if (!Done())
            ++skipped;
    }

    void Sample(int64_t ticks)
    {
        ++sampled;
        if (ticks > 0)
            totalQpc += ticks;
    }

    // Milliseconds per draw, and per frame at the draw count the caller reports.
    double MillisecondsPerDraw(int64_t qpcFrequency) const noexcept
    {
        return sampled == 0 || qpcFrequency <= 0 ? 0.0 : double(totalQpc) * 1000.0 / double(qpcFrequency) / double(sampled);
    }
};

// The seam's meter: the pixel-shader tracker in the context's PSSetShader hook, which is where the classification's work moved. Sampling
// starts later than the draw meter, because the game sets a pixel shader far less often than it draws.
inline constexpr uint32_t kBindCostEvery = 16384;

struct BindCostMeter
{
    uint32_t skipped = 0;
    uint32_t sampled = 0;
    int64_t totalQpc = 0;
    bool logged = false;

    bool Sampling() const noexcept { return sampled < kCostSampleDraws && skipped >= kBindCostEvery; }
    void Skip()
    {
        if (sampled < kCostSampleDraws)
            ++skipped;
    }
    void Sample(int64_t ticks)
    {
        ++sampled;
        if (ticks > 0)
            totalQpc += ticks;
    }
    double MillisecondsPerBind(int64_t qpcFrequency) const noexcept
    {
        return sampled == 0 || qpcFrequency <= 0 ? 0.0 : double(totalQpc) * 1000.0 / double(qpcFrequency) / double(sampled);
    }
};

// The cost line: both halves of the classification, in the numbers the brief counts them with - the draw-side lookup (what used to be a
// PSGetShader per draw) at the game's ~9,250 draws a frame, and the seam that feeds it at the rate the session actually set pixel shaders.
// The per-draw figure is an upper bound: the sample's own two clock reads are inside the bracket. A frame's figure at 9,250 draws is the
// budget the design names (0.02 ms) and is printed beside it, so the number and the budget are read together.
inline std::string FormatCost(const CostMeter& draws, uint64_t drawsSeen, const BindCostMeter& binds, uint64_t bindsSeen,
                              int64_t qpcFrequency)
{
    const double perDraw = draws.MillisecondsPerDraw(qpcFrequency);
    const double perBind = binds.MillisecondsPerBind(qpcFrequency);
    const double bindsPerFrame = drawsSeen > 0 ? double(bindsSeen) / double(drawsSeen) * 9250.0 : 0.0;
    return std::format("Contact shadows: on cost: {} draws seen, {} sampled, the draw's lookup {:.5f} ms per draw ({:.4f} ms at 9250 draws "
                       "a frame, the meter's two clock reads included; the design budget is {:.2f} ms); {} pixel-shader binds seen, {} "
                       "sampled, {:.5f} ms per bind ({:.4f} ms a frame at that rate)",
                       drawsSeen, draws.sampled, perDraw, perDraw * 9250.0, kCostBudgetMilliseconds, bindsSeen, binds.sampled, perBind,
                       perBind * bindsPerFrame);
}

// ---- the one-frame dump (the developer check) ------------------------------------------------------------------------

// [FFXIV] ContactShadowsDump is an integer, ini only, off by default: the seconds after the game starts at which the first sun light draw
// that runs is written out, with its mask before and after the pass, the term, the depth and the camera and sun bytes. One frame, once per
// session. The files land in OptiScaler\contact-dump\<date-time>\ beside the DLL.
inline constexpr int32_t kMaxDumpDelaySeconds = 3600;
inline constexpr const char* kDumpFolder = "contact-dump";
inline constexpr const char* kDumpMaskBeforeFile = "mask-before.bgra8";
inline constexpr const char* kDumpTermFile = "term.r8";
inline constexpr const char* kDumpMaskAfterFile = "mask-after.bgra8";
inline constexpr const char* kDumpDepthFile = "depth.r32";
inline constexpr const char* kDumpCameraFile = "camera.bin";
inline constexpr const char* kDumpSunFile = "sun.bin";
inline constexpr const char* kDumpFactsFile = "facts.txt";

// The dump is due once: the key holds a delay of at least one second (0 = off), the session has not written a dump yet, and the delay has
// passed since the config load. A delay of zero is "off" and never writes, which keeps the key's default free of any work.
constexpr bool DumpDue(int32_t delaySeconds, bool alreadyWritten, int64_t startQpc, int64_t nowQpc, int64_t frequency) noexcept
{
    if (alreadyWritten || delaySeconds <= 0 || delaySeconds > kMaxDumpDelaySeconds || startQpc <= 0 || frequency <= 0)
        return false;
    return double(nowQpc - startQpc) / double(frequency) >= double(delaySeconds);
}

// ---- the session state -------------------------------------------------------------------------------------------------

// After this many declined draws in a row the module switches itself off for the session with one log line: a build mismatch then costs
// one line and no per-draw work instead of a query on every draw. A shape refusal (IsShapeDecline) never counts: the atlas writer would
// reach the limit on its own within a frame and stop the feature for the session.
inline constexpr uint32_t kDeclineLimit = 16;

struct Session
{
    uint32_t consecutiveDeclines = 0;
    bool off = false; // switched itself off for the session

    // A declined draw. Returns true when this decline is the one that switched the module off.
    bool NoteDecline()
    {
        if (off)
            return false;
        if (++consecutiveDeclines >= kDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    void NoteRun() { consecutiveDeclines = 0; }

    bool Armed() const noexcept { return !off; }
};

// The log budget: one line per change of state and at most 32 a session (the fork's habit for a developer-visible gate).
inline constexpr unsigned kMaxLogLines = 32;

struct LogGate
{
    uint32_t last = 0; // the Decline of the last line, packed with the run flag by the caller
    bool have = false;
    unsigned lines = 0;

    // `tag` identifies the state: the module packs the outcome and, for a run, the mask size class into one 32-bit value.
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

// The one line a change of state writes. `maskWidth`/`maskHeight` and `windowWidth`/`windowHeight` are the sizes the state speaks
// about; `depthWidth`/`depthHeight` are the depth binding's, which is what a size decline is usually about.
inline std::string FormatDecline(Decline reason)
{
    return std::format("Contact shadows: the draw was left alone ({})", DeclineName(reason));
}

inline std::string FormatRun(const Params& params, uint32_t maskWidth, uint32_t maskHeight, float windowWidth, float windowHeight,
                             bool haveGpuMilliseconds, double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds
                                ? std::format(", its two draws {:.4f} ms on the GPU", gpuMilliseconds)
                                : std::string(", its two draws' GPU time not measured yet");
    return std::format("Contact shadows: multiplying the sun mask ({}x{}) by a {:.2f} m march over the {}x{} render window, "
                       "strength {:.2f}, {} step(s), sun in {} space toward the {}{}",
                       maskWidth, maskHeight, double(params.length), uint32_t(windowWidth), uint32_t(windowHeight),
                       double(params.strength), params.steps, params.space == SunSpace::View ? "view" : "world",
                       params.sign == SunSign::Toward ? "sun" : "scene", gpu);
}

inline std::string FormatSwitchedOff()
{
    return std::format("Contact shadows: {} draws in a row were left alone, so the pass is off for this session", kDeclineLimit);
}

// The classification line the design asks for: the mask's format and size, the sun vector and the space it was read in.
inline std::string FormatClassification(uint32_t maskFormat, uint32_t maskWidth, uint32_t maskHeight, float dirX, float dirY,
                                        float dirZ, SunSpace space)
{
    return std::format("Contact shadows: sun light draw found, mask {}x{} format {}, sun ({:.3f}, {:.3f}, {:.3f}) read as {} space",
                       maskWidth, maskHeight, maskFormat, double(dirX), double(dirY), double(dirZ),
                       space == SunSpace::View ? "view" : "world");
}
} // namespace ext::ffxiv::relight
