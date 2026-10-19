#pragma once
// Read-only look at the game's own camera jitter, and the optional correction built on it ([FFXIV] JitterProbe, [FFXIV] JitterCorrection,
// both off by default). Pure logic only: no logging, no pch, no Windows calls. The glue (FfxivJitterProbe.cpp) supplies the reader of
// process memory, the NGX parameters and the clock, so every decision here is covered by tests/ext/ffxiv/jitter on fake memory images.
//
// What it is about: the game renders with an NDC jitter translation (ndcX, ndcY) it computes as s * jitter / size, but tells NGX the
// UNSCALED pixel jitter (jx, jy). They describe the same shift only when s == 2. The applied pixel jitter is
//   ax = ndcX * renderWidth / 2,  ay = -ndcY * renderHeight / 2   (the matrix uses ndcY = -s * jy / height)
// and the ratios rx = ax / jx, ry = ay / jy equal 1 exactly when s == 2 (implied s = 2 * ratio).
//
// Safety rules (the maintainer's): nothing here writes game memory; every read goes through a caller-supplied reader that must fail
// softly (ReadProcessMemory does); every pointer, float and counter is checked before use; any failed guard is reported once per
// episode and does nothing else.

#include "ext/ffxiv/GameBuild.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace ext::ffxiv::jitter
{
enum class Guard : std::uint8_t
{
    None = 0,
    BuildMismatch,       // the executable is not the supported build: nothing is read
    GlobalUnreadable,    // [exe base + cameraGlobalRva] could not be read
    GlobalInvalid,       // ... is null or not a user-mode address
    CameraUnreadable,
    CameraInvalid,
    ObjectPointerUnreadable,
    ObjectPointerInvalid,
    ObjectUnreadable,    // the 20-byte object could not be read
    ObjectUnstable,      // two consecutive reads never agreed (the game was writing it)
    JitterOutOfRange,    // jx or jy not finite or |value| > 0.5
    NdcOutOfRange,       // ndcX or ndcY not finite or |value| > 1
    CounterNotIncreasing,
    // Correction only (the object itself was fine):
    RenderSizeUnknown,   // no trusted render size (the game's Width/Height parameters are missing)
    NgxJitterUnavailable,
    ObjectNotThisEvaluate, // the object's jitter differs from what this evaluate carries: it belongs to another frame
    AppliedImplausible,
    SignMismatch,        // reported and applied jitter have opposite signs: the convention is not understood, nothing is touched
    NoPreviousFrame,     // the first evaluate after a gap: nothing to confirm it with yet
    RatioNotSteady,      // the applied/reported ratio differs from the previous evaluate's: the NDC values may belong to another frame
};

constexpr const char* GuardName(Guard g)
{
    switch (g)
    {
    case Guard::None: return "none";
    case Guard::BuildMismatch: return "executable build does not match the supported build";
    case Guard::GlobalUnreadable: return "camera global unreadable";
    case Guard::GlobalInvalid: return "camera global null or not a user address";
    case Guard::CameraUnreadable: return "camera object pointer unreadable";
    case Guard::CameraInvalid: return "camera object pointer null or not a user address";
    case Guard::ObjectPointerUnreadable: return "jitter object pointer unreadable";
    case Guard::ObjectPointerInvalid: return "jitter object pointer null or not a user address";
    case Guard::ObjectUnreadable: return "jitter object unreadable";
    case Guard::ObjectUnstable: return "jitter object changed while it was read";
    case Guard::JitterOutOfRange: return "jitter value not finite or above 0.5 pixel";
    case Guard::NdcOutOfRange: return "NDC jitter value not finite or above 1";
    case Guard::CounterNotIncreasing: return "frame counter did not increase";
    case Guard::RenderSizeUnknown: return "render size not known from the Width/Height parameters";
    case Guard::NgxJitterUnavailable: return "NGX jitter parameters not readable";
    case Guard::ObjectNotThisEvaluate: return "jitter object does not match this evaluate (another frame)";
    case Guard::AppliedImplausible: return "applied jitter not finite or above 1 pixel";
    case Guard::SignMismatch: return "reported and applied jitter have opposite signs";
    case Guard::NoPreviousFrame: return "first evaluate after a gap, no previous evaluate to confirm it";
    case Guard::RatioNotSteady: return "applied/reported ratio differs from the previous evaluate's";
    }
    return "unknown";
}

// ---- reading -------------------------------------------------------------------------------------------------------------------

inline constexpr std::uint64_t kMinUserPointer = 0x10000;
inline constexpr std::uint64_t kMaxUserPointer = 0x00007FFFFFFFFFFFull;
constexpr bool PlausiblePointer(std::uint64_t p) { return p >= kMinUserPointer && p <= kMaxUserPointer; }

struct RawObject
{
    float jx = 0.0f, jy = 0.0f;
    float ndcX = 0.0f, ndcY = 0.0f;
    std::int32_t counter = 0;
};

// `read(address, out, size)` returns false when any byte is unreadable. `build` is MatchCurrentProcess()'s answer (null = no match).
template <class Reader>
Guard ReadChain(Reader&& read, const GameBuild* build, std::uintptr_t moduleBase, RawObject& out)
{
    if (build == nullptr)
        return Guard::BuildMismatch;
    const JitterBuild& b = build->jitter;

    std::uint64_t global = 0, camera = 0, object = 0;
    if (!read(moduleBase + b.cameraGlobalRva, &global, sizeof(global)))
        return Guard::GlobalUnreadable;
    if (!PlausiblePointer(global))
        return Guard::GlobalInvalid;
    if (!read(global + b.cameraOffset, &camera, sizeof(camera)))
        return Guard::CameraUnreadable;
    if (!PlausiblePointer(camera))
        return Guard::CameraInvalid;
    if (!read(camera + b.jitterPointerOffset, &object, sizeof(object)))
        return Guard::ObjectPointerUnreadable;
    if (!PlausiblePointer(object))
        return Guard::ObjectPointerInvalid;

    // The object is read until two consecutive reads agree (at most three): a torn read must not become a verdict.
    constexpr std::size_t kMax = 64;
    if (b.objectBytes == 0 || b.objectBytes > kMax)
        return Guard::ObjectUnreadable;
    unsigned char previous[kMax] {}, current[kMax] {};
    if (!read(object, previous, b.objectBytes))
        return Guard::ObjectUnreadable;
    bool stable = false;
    for (int attempt = 0; attempt < 2 && !stable; ++attempt)
    {
        if (!read(object, current, b.objectBytes))
            return Guard::ObjectUnreadable;
        stable = std::memcmp(previous, current, b.objectBytes) == 0;
        if (!stable)
            std::memcpy(previous, current, b.objectBytes);
    }
    if (!stable)
        return Guard::ObjectUnstable;

    auto field = [&](std::uint32_t offset, auto& dest) { std::memcpy(&dest, current + offset, sizeof(dest)); };
    RawObject o;
    if (b.jitterXOffset + 4 > b.objectBytes || b.jitterYOffset + 4 > b.objectBytes || b.ndcXOffset + 4 > b.objectBytes ||
        b.ndcYOffset + 4 > b.objectBytes || b.counterOffset + 4 > b.objectBytes)
        return Guard::ObjectUnreadable;
    field(b.jitterXOffset, o.jx);
    field(b.jitterYOffset, o.jy);
    field(b.ndcXOffset, o.ndcX);
    field(b.ndcYOffset, o.ndcY);
    field(b.counterOffset, o.counter);

    if (!std::isfinite(o.jx) || !std::isfinite(o.jy) || std::fabs(o.jx) > 0.5f || std::fabs(o.jy) > 0.5f)
        return Guard::JitterOutOfRange;
    if (!std::isfinite(o.ndcX) || !std::isfinite(o.ndcY) || std::fabs(o.ndcX) > 1.0f || std::fabs(o.ndcY) > 1.0f)
        return Guard::NdcOutOfRange;
    out = o;
    return Guard::None;
}

// "The counter increases" between two samples. A failure re-bases (a re-created camera restarts at 0), so it never latches.
class CounterTracker
{
  public:
    Guard Check(std::int32_t counter)
    {
        const bool ok = !_have || counter > _last;
        _have = true;
        _last = counter;
        return ok ? Guard::None : Guard::CounterNotIncreasing;
    }

  private:
    bool _have = false;
    std::int32_t _last = 0;
};

// ---- analysis ------------------------------------------------------------------------------------------------------------------

struct RenderSize
{
    std::uint32_t width = 0, height = 0;
    bool trusted = false; // derived from the game's own Width/Height parameters (not from the subrect alone)
    const char* source = "unknown";
};

// FFXIV hands NGX a subrect that can be the whole display allocation; the game's render size is the per-axis minimum of the subrect and
// its Width/Height parameters (the rule ext::upscaler::ResolveRenderExtentValue applies). Without Width/Height the subrect is only a
// hint and the size is not trusted.
constexpr RenderSize ResolveRenderSize(std::uint32_t subrectW, std::uint32_t subrectH, std::uint32_t paramW, std::uint32_t paramH)
{
    if (paramW != 0 && paramH != 0)
    {
        const bool haveSubrect = subrectW != 0 && subrectH != 0;
        return { haveSubrect && subrectW < paramW ? subrectW : paramW, haveSubrect && subrectH < paramH ? subrectH : paramH, true,
                 haveSubrect ? "min of subrect and Width/Height" : "Width/Height" };
    }
    if (subrectW != 0 && subrectH != 0)
        return { subrectW, subrectH, false, "subrect only (may be the display size)" };
    return {};
}

inline constexpr float kMinRatioJitter = 1.0f / 64.0f; // below this the ratio of two tiny numbers says nothing
inline constexpr float kRatioTolerance = 0.02f;        // |ratio - 1| within this counts as consistent (s within 2 +- 0.04)
inline constexpr float kMaxAppliedPixels = 1.0f;       // an applied jitter above this is not believed

enum class Verdict : std::uint8_t
{
    Undetermined, // no axis has enough jitter this sample, or the render size is unknown
    Consistent,
    Differ
};

struct Analysis
{
    RawObject object;
    RenderSize size;
    bool haveApplied = false;
    float ax = 0.0f, ay = 0.0f;
    bool haveRx = false, haveRy = false;
    float rx = 0.0f, ry = 0.0f;
    bool signMismatchX = false, signMismatchY = false;
    Verdict verdict = Verdict::Undetermined;
    float ImpliedScaleX() const { return 2.0f * rx; }
    float ImpliedScaleY() const { return 2.0f * ry; }
};

inline Analysis Analyse(const RawObject& o, const RenderSize& size)
{
    Analysis a;
    a.object = o;
    a.size = size;
    if (size.width == 0 || size.height == 0)
        return a;
    a.haveApplied = true;
    a.ax = static_cast<float>(static_cast<double>(o.ndcX) * size.width / 2.0);
    a.ay = static_cast<float>(-static_cast<double>(o.ndcY) * size.height / 2.0);
    if (std::fabs(o.jx) >= kMinRatioJitter)
    {
        a.haveRx = true;
        a.rx = a.ax / o.jx;
    }
    if (std::fabs(o.jy) >= kMinRatioJitter)
    {
        a.haveRy = true;
        a.ry = a.ay / o.jy;
    }
    a.signMismatchX = a.haveRx && a.rx < 0.0f;
    a.signMismatchY = a.haveRy && a.ry < 0.0f;
    if (a.haveRx || a.haveRy)
    {
        const bool offX = a.haveRx && std::fabs(a.rx - 1.0f) > kRatioTolerance;
        const bool offY = a.haveRy && std::fabs(a.ry - 1.0f) > kRatioTolerance;
        a.verdict = (offX || offY || a.signMismatchX || a.signMismatchY) ? Verdict::Differ : Verdict::Consistent;
    }
    return a;
}

enum class NgxMatch : std::uint8_t
{
    Unavailable,
    Same,
    Different
};

// Does the jitter this evaluate carries to NGX equal the object's? The game's wrapper copies the floats unchanged, so any difference
// beyond float noise means the object has moved on (another frame) or the values were changed on the way.
inline NgxMatch CompareNgx(const RawObject& o, const std::optional<float>& ngxX, const std::optional<float>& ngxY)
{
    if (!ngxX || !ngxY || !std::isfinite(*ngxX) || !std::isfinite(*ngxY))
        return NgxMatch::Unavailable;
    constexpr float kSame = 1e-6f;
    return std::fabs(*ngxX - o.jx) <= kSame && std::fabs(*ngxY - o.jy) <= kSame ? NgxMatch::Same : NgxMatch::Different;
}

// ---- correction ----------------------------------------------------------------------------------------------------------------

struct CorrectionDecision
{
    bool replace = false;
    bool consistent = false;   // the game already hands NGX what it applies (ratios within 2% of 1): nothing to do, not a failure
    bool undetermined = false; // both axes too small this frame to tell: values left alone, not a failure
    Guard guard = Guard::None;
    float ax = 0.0f, ay = 0.0f;
};

// Replace the reported jitter by the applied one only when everything is proven for THIS evaluate; otherwise the game's value stays.
// `readGuard` is ReadChain's answer (the probe's 10-second counter check is not used here: the proof that the object belongs to this
// evaluate is that its jitter equals the NGX parameters of this evaluate, and FrameRules below adds the counter and ratio rules that
// the glue applies on top of this decision).
inline CorrectionDecision DecideCorrection(Guard readGuard, const Analysis& a, NgxMatch ngx)
{
    CorrectionDecision d;
    if (readGuard != Guard::None)
    {
        d.guard = readGuard;
        return d;
    }
    if (!a.size.trusted || !a.haveApplied)
    {
        d.guard = Guard::RenderSizeUnknown;
        return d;
    }
    if (ngx == NgxMatch::Unavailable)
    {
        d.guard = Guard::NgxJitterUnavailable;
        return d;
    }
    if (ngx == NgxMatch::Different)
    {
        d.guard = Guard::ObjectNotThisEvaluate;
        return d;
    }
    if (!std::isfinite(a.ax) || !std::isfinite(a.ay) || std::fabs(a.ax) > kMaxAppliedPixels || std::fabs(a.ay) > kMaxAppliedPixels)
    {
        d.guard = Guard::AppliedImplausible;
        return d;
    }
    if (a.signMismatchX || a.signMismatchY)
    {
        d.guard = Guard::SignMismatch;
        return d;
    }
    // One criterion for the probe and the correction: the verdict of Analyse (ratios within 2% of 1).
    if (a.verdict == Verdict::Consistent)
    {
        d.consistent = true;
        return d;
    }
    if (a.verdict == Verdict::Undetermined) // both axes too small to tell: leave the values, change no episode
    {
        d.undetermined = true;
        return d;
    }
    d.replace = true;
    d.ax = a.ax;
    d.ay = a.ay;
    return d;
}

// ---- same-frame rules ----------------------------------------------------------------------------------------------------------

// The equality proof (CompareNgx) covers jx/jy only; the replacement is computed from ndcX/ndcY, and nothing proves those belong to the
// same frame as jx/jy. Two rules narrow that, applied once per evaluate of the correction path and only ever taking a replacement away:
//   (i)  the object's counter has INCREASED since the previous evaluate (the same camera step is never corrected twice, and a camera
//        that was re-created or stalled passes the game's value through);
//   (ii) the implied ratio agrees with the previous evaluate's within the shared 2% criterion (per axis where both evaluates have one,
//        at least one axis in common). A one-frame lag between the NDC values and the jitter makes the ratio change with every step of
//        the jitter sequence, so it fails here; a real scale error is the same on every frame, so it passes.
// So a replacement needs two consecutive evaluates that PASSED the proof (the guard of DecideCorrection is None: object read, trusted
// size, NGX jitter equal, plausible, signs ok) and that agree under both rules. An evaluate the proof rejected, for any reason, FORGETS
// the reference like a failed read does (the next evaluate is a first frame again and passes the game's value through); a first
// evaluate or a rule failure re-bases (the failing evaluate becomes the reference) and passes the game's value through.
//
// What these rules do NOT exclude (stated so nobody trusts them further than they reach): a systematic error that is the same on every
// frame (the wrong render size for the NDC scale, the wrong convention constant) gives a steady ratio and passes both rules, bounded only
// by the 1 pixel cap and the sign test; a lag that happens to leave two consecutive ratios within 2% passes; an object whose counter,
// jx/jy and NDC all belong to a frame other than the one being rendered (a constant offset in time) passes; a game that evaluates
// twice per camera step is corrected on every other evaluate only (visible in the held-back counts of the status line); and a game that
// also evaluates a second, differently sized DLSS feature in the same frame makes the correction inert, because that evaluate is rejected
// and forgets the reference every frame (the safe side; it shows as "first frame after a gap" dominating the status line).
class FrameRules
{
  public:
    // `rejection` is the guard DecideCorrection answered for this evaluate (Guard::None = the proof passed; a failed read of the object
    // arrives here as its read guard).
    Guard Observe(Guard rejection, const Analysis& a)
    {
        if (rejection != Guard::None)
        {
            _have = false;
            return Guard::NoPreviousFrame;
        }
        const Reference before = _reference;
        const bool had = _have;
        _reference = { a.object.counter, a.haveRx, a.rx, a.haveRy, a.ry };
        _have = true;
        if (!had)
            return Guard::NoPreviousFrame;
        if (a.object.counter <= before.counter)
            return Guard::CounterNotIncreasing;
        return Agree(before, _reference) ? Guard::None : Guard::RatioNotSteady;
    }

  private:
    struct Reference
    {
        std::int32_t counter = 0;
        bool haveRx = false;
        float rx = 0.0f;
        bool haveRy = false;
        float ry = 0.0f;
    };

    static bool Agree(const Reference& previous, const Reference& current)
    {
        bool compared = false;
        auto axis = [&](bool havePrevious, float p, bool haveCurrent, float c)
        {
            if (!havePrevious || !haveCurrent)
                return true; // this axis cannot be compared
            compared = true;
            return std::fabs(c - p) <= kRatioTolerance * std::fabs(p);
        };
        const bool x = axis(previous.haveRx, previous.rx, current.haveRx, current.rx);
        const bool y = axis(previous.haveRy, previous.ry, current.haveRy, current.ry);
        return compared && x && y;
    }

    bool _have = false;
    Reference _reference;
};

// Applies FrameRules' answer to a decision: a replacement the rules refused becomes a pass-through with that reason. A first evaluate is
// not a failure and changes no episode (flagged like a frame too small to tell). Every other decision is returned unchanged.
inline CorrectionDecision ApplyFrameRules(CorrectionDecision d, Guard frame)
{
    if (!d.replace || frame == Guard::None)
        return d;
    d.replace = false;
    d.guard = frame;
    d.undetermined = frame == Guard::NoPreviousFrame;
    return d;
}

// ---- rate limit and episodes ---------------------------------------------------------------------------------------------------

// One sample per 10 s; a change of the feature configuration (render size, preset) is sampled early (after 2 s), at most 16 times a
// session, so a game that changes its render size every frame cannot flood the log.
class SampleLimiter
{
  public:
    static constexpr std::uint64_t kIntervalMs = 10000;
    static constexpr std::uint64_t kChangeMinMs = 2000;
    static constexpr std::uint32_t kMaxChangeSamples = 16;

    // The two cheap questions the probe-only path asks BEFORE it reads any parameter (the configuration key is the only thing that needs
    // them): is the interval over (or nothing sampled yet), and could a configuration change be taken at all (2 s since the last sample,
    // early-sample budget left). If both are no, Allow cannot say yes whatever the key is.
    bool IntervalElapsed(std::uint64_t nowMs) const { return !_have || Elapsed(nowMs) >= kIntervalMs; }
    bool ChangeWindowOpen(std::uint64_t nowMs) const { return _have && Elapsed(nowMs) >= kChangeMinMs && _changeSamples < kMaxChangeSamples; }

    bool Allow(std::uint64_t nowMs, std::uint64_t configurationKey)
    {
        bool take = false;
        if (!_have)
            take = true;
        else
        {
            const std::uint64_t elapsed = Elapsed(nowMs);
            if (elapsed >= kIntervalMs)
                take = true;
            else if (configurationKey != _key && elapsed >= kChangeMinMs && _changeSamples < kMaxChangeSamples)
            {
                take = true;
                ++_changeSamples;
            }
        }
        if (take)
        {
            _have = true;
            _last = nowMs;
            _key = configurationKey;
        }
        return take;
    }

  private:
    std::uint64_t Elapsed(std::uint64_t nowMs) const { return nowMs >= _last ? nowMs - _last : 0; } // a clock going backwards is "no time passed"

    bool _have = false;
    std::uint64_t _last = 0, _key = 0;
    std::uint32_t _changeSamples = 0;
};

// One log line per episode of failures: true only for the first failing sample after a good one.
class EpisodeLog
{
  public:
    static constexpr std::uint32_t kMaxLines = 32; // per session: a game that flickers between states cannot flood the log

    bool ShouldLog(Guard g) { return Event(g != Guard::None); }
    // `active` = this evaluate is inside the episode. True only at the start of an episode, and only while the line budget lasts.
    bool Event(bool active)
    {
        if (!active)
        {
            _open = false;
            return false;
        }
        if (_open)
            return false;
        _open = true;
        if (_lines >= kMaxLines)
        {
            ++_suppressed;
            return false;
        }
        ++_lines;
        return true;
    }
    void Close() { _open = false; }
    std::uint32_t Suppressed() const { return _suppressed; }

  private:
    bool _open = false;
    std::uint32_t _lines = 0, _suppressed = 0;
};

// Thins a periodic line instead of cutting it off: the first kFullLines lines are written whenever they are offered (every sample tick,
// about every 10 s), after that at most one per kSlowIntervalMs, however often one is offered. The lines this guards carry TOTALS since
// the start of the session, so a thinned line loses nothing and the last line of a long session still says everything.
class LineDecimator
{
  public:
    static constexpr std::uint32_t kFullLines = 60;
    static constexpr std::uint64_t kSlowIntervalMs = 60000;

    // True when a line may be written now; taking it counts it. A clock that goes backwards counts as "no time passed".
    // Would a line be allowed now? Does not count it: a caller that builds the line first (and can fail doing so) asks this, then Take.
    bool Ready(std::uint64_t nowMs) const { return _lines < kFullLines || Elapsed(nowMs) >= kSlowIntervalMs; }
    bool Take(std::uint64_t nowMs)
    {
        if (!Ready(nowMs))
            return false;
        ++_lines;
        _last = nowMs;
        return true;
    }
    std::uint32_t Lines() const { return _lines; }

  private:
    std::uint64_t Elapsed(std::uint64_t nowMs) const { return nowMs >= _last ? nowMs - _last : 0; }

    std::uint32_t _lines = 0;
    std::uint64_t _last = 0;
};

// ---- text ----------------------------------------------------------------------------------------------------------------------

struct Context
{
    std::uint32_t subrectW = 0, subrectH = 0, widthParam = 0, heightParam = 0, outW = 0, outH = 0;
    std::optional<int> perfQuality;      // the PerfQualityValue parameter of this evaluate, when present
    std::optional<int> forcedQuality;    // ext::upscaler::LastQualityAnswer().quality (the preset the game was last answered)
    std::uint32_t answeredRenderW = 0, answeredRenderH = 0, answeredDisplayW = 0, answeredDisplayH = 0;
};

// The configuration that makes a new sample worthwhile when it changes.
constexpr std::uint64_t ConfigurationKey(const Context& c)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint64_t v : { std::uint64_t(c.subrectW), std::uint64_t(c.subrectH), std::uint64_t(c.widthParam), std::uint64_t(c.heightParam),
                             std::uint64_t(c.outW), std::uint64_t(c.outH),
                             std::uint64_t(static_cast<std::uint32_t>(c.perfQuality.value_or(-1))),
                             std::uint64_t(static_cast<std::uint32_t>(c.forcedQuality.value_or(-1))) })
    {
        h ^= v;
        h *= 1099511628211ull;
    }
    return h;
}

inline std::string ContextText(const Context& c)
{
    auto size = [](std::uint32_t w, std::uint32_t h) { return w != 0 && h != 0 ? std::format("{}x{}", w, h) : std::string("n/a"); };
    auto number = [](const std::optional<int>& v) { return v ? std::to_string(*v) : std::string("n/a"); };
    return std::format("NGX subrect {}, Width/Height {}, OutWidth/OutHeight {}, PerfQualityValue {}; last forced-quality answer: quality {}, render {}, display {}",
                       size(c.subrectW, c.subrectH), size(c.widthParam, c.heightParam), size(c.outW, c.outH), number(c.perfQuality),
                       number(c.forcedQuality), size(c.answeredRenderW, c.answeredRenderH), size(c.answeredDisplayW, c.answeredDisplayH));
}

inline std::string VerdictText(const Analysis& a)
{
    auto scale = [](bool have, float s) { return have ? std::format("{:.3f}", s) : std::string("n/a"); };
    const std::string values = std::format("implied s = {}/{}", scale(a.haveRx, a.ImpliedScaleX()), scale(a.haveRy, a.ImpliedScaleY()));
    switch (a.verdict)
    {
    case Verdict::Consistent: return values + " (consistent)";
    case Verdict::Differ: return values + " (reported and applied jitter differ)";
    default: return values + " (undetermined: " + (a.haveApplied ? "jitter too small this sample" : "render size unknown") + ")";
    }
}

inline std::string FormatSample(const Analysis& a, NgxMatch ngx, const std::optional<float>& ngxX, const std::optional<float>& ngxY,
                                const Context& ctx)
{
    std::string ngxText;
    switch (ngx)
    {
    case NgxMatch::Same: ngxText = "NGX saw the same values"; break;
    case NgxMatch::Different:
        ngxText = std::format("NGX saw different values ({:.4f}/{:.4f}: the object belongs to another frame or the values changed on the way)",
                              ngxX.value_or(0.0f), ngxY.value_or(0.0f));
        break;
    default: ngxText = "NGX jitter not available at this seam"; break;
    }
    auto ratio = [](bool have, float r) { return have ? std::format("{:.3f}", r) : std::string("n/a"); };
    return std::format("Game jitter: counter {}, reported jx/jy {:.4f}/{:.4f}, ndc {:.6f}/{:.6f}, render {}x{} ({}), applied ax/ay {:.4f}/{:.4f}, "
                       "ratio rx/ry {}/{}, signs {}, {}, {}; {}",
                       a.object.counter, a.object.jx, a.object.jy, a.object.ndcX, a.object.ndcY, a.size.width, a.size.height, a.size.source,
                       a.ax, a.ay, ratio(a.haveRx, a.rx), ratio(a.haveRy, a.ry),
                       a.signMismatchX || a.signMismatchY ? std::string("MISMATCH") : std::string("ok"), VerdictText(a), ngxText,
                       ContextText(ctx));
}

inline std::string FormatProbeFailure(Guard g) { return std::format("Game jitter: not read, guard failed: {}", GuardName(g)); }

// `readX/readY` are what the parameter block returned when it was asked again after the write (nullopt = the read failed): the line
// shows the intended values and the stored ones side by side, so a block that did not take the write is visible.
inline std::string FormatCorrection(const Analysis& a, const CorrectionDecision& d, const std::optional<float>& readX,
                                    const std::optional<float>& readY)
{
    const std::string stored = readX && readY ? std::format("({:.4f},{:.4f})", *readX, *readY) : std::string("unavailable");
    return std::format("Jitter correction: reported ({:.4f},{:.4f}) replaced by applied ({:.4f},{:.4f}), ratio {:.3f}/{:.3f}; parameter block read back: {}",
                       a.object.jx, a.object.jy, d.ax, d.ay, a.haveRx ? a.rx : 0.0f, a.haveRy ? a.ry : 0.0f, stored);
}
inline std::string FormatPassThrough(Guard g)
{
    return std::format("Jitter correction: game values passed through unchanged, guard failed: {}", GuardName(g));
}

// What happened to the evaluates since the correction started (totals for the whole session, never reset: the status lines are thinned,
// so each one has to carry everything). `held*` count the evaluates in which the values differed and every guard
// of DecideCorrection passed, but a same-frame rule refused the replacement: a session in which these dominate is one in which the
// correction is inert (the game does not give the proof the rules ask for).
struct StatusCounts
{
    std::uint64_t evaluated = 0, replaced = 0, passed = 0;
    std::uint64_t heldCounter = 0, heldRatio = 0, heldFirst = 0;
};

// What a session still logs once the budgets run out (EpisodeLog::kMaxLines per kind of episode, Engine::kMaxSampleLines sample lines): one
// line for the sample budget, and the counts of the episodes that were not logged. The status lines have no budget, they are thinned
// (LineDecimator).
inline std::string FormatSampleBudgetUsedUp(std::uint32_t lines)
{
    return std::format("Game jitter: sample line budget of {} lines used up for this session, further samples are not logged", lines);
}

inline std::string FormatStepException(const char* what)
{
    return std::format("Game jitter: step abandoned after an exception: {} (the rest of that evaluate is skipped; one line per episode)", what);
}

inline std::string FormatSuppressed(std::uint64_t probeFailures, std::uint64_t passThroughs, std::uint64_t replacements,
                                    std::uint64_t abandonedSteps)
{
    return std::format("Game jitter: log line budget used up, episodes not logged so far: {} probe failures, {} pass-throughs, {} replacements, "
                       "{} abandoned steps",
                       probeFailures, passThroughs, replacements, abandonedSteps);
}

// `frameGeneration`: whether the fork's frame generation is running (nullopt = could not be told). The replaced pair feeds it too, but
// only through the Upscaler input (the others get their jitter elsewhere), so `input` (State::activeFgInput as text, empty = not known)
// is printed next to it.
inline std::string FormatStatus(const StatusCounts& c, const std::optional<bool>& frameGeneration, const std::string& input)
{
    const std::string inputText = input.empty() ? std::string() : std::format(" (input {})", input);
    return std::format("Jitter correction: on; totals since the start of the session: {} evaluates seen, {} replaced by the applied jitter, {} passed through "
                       "unchanged (held back by the same-frame rules although the values differed: {} frame counter did not increase, {} ratio "
                       "not steady, {} first frame after a gap or a rejected evaluate); frame generation active {}{}",
                       c.evaluated, c.replaced, c.passed, c.heldCounter, c.heldRatio, c.heldFirst,
                       frameGeneration ? (*frameGeneration ? "yes" : "no") : "unknown", inputText);
}
} // namespace ext::ffxiv::jitter
