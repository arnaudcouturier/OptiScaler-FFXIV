#pragma once
// Read-only look at the game's own camera jitter, and the optional calibrate-and-scale correction built on it ([FFXIV] JitterProbe, [FFXIV] JitterCorrection,
// both off by default). Pure logic only: no logging, no pch, no Windows calls. The glue (FfxivJitterProbe.cpp) supplies the reader of
// process memory, the NGX parameters and the clock, so every decision here is covered by tests/ext/ffxiv/jitter on fake memory images.
// The calibration (learning the ratio, scaling the NGX pair) is in FfxivJitterCalibration.h; this header is the reading and analysis layer.
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
    // Correction only (the object itself was fine; the calibration also reports why it skipped a snapshot or did not scale a pair):
    RenderSizeUnknown,   // no trusted render size (the game's Width/Height parameters are missing)
    NgxJitterUnavailable,
    JitterTooSmall,        // an axis has too little reported jitter to give a ratio (the calibration skips the snapshot)
    ReportedOutOfRange,    // the jitter this evaluate carries to NGX is above the plausible reported size (not scaled)
    ScaleRatioInvalid,     // the ratio handed to the scaling is not finite or not in (0, 1]: nothing is written
    AppliedImplausible,
    SignMismatch,        // reported and applied jitter have opposite signs: the convention is not understood, nothing is touched
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
    case Guard::JitterTooSmall: return "reported jitter too small on an axis to give a ratio";
    case Guard::ReportedOutOfRange: return "NGX jitter value above 0.55 pixel";
    case Guard::ScaleRatioInvalid: return "scale ratio not finite or not in the range above 0 up to 1";
    case Guard::AppliedImplausible: return "applied jitter not finite or above 1 pixel";
    case Guard::SignMismatch: return "reported and applied jitter have opposite signs";
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
// `maxJitter` is the largest |jx|, |jy| the object may hold: 0.5 (the game's own range) unless the source-level jitter fix divides the generator output by
// the native strength, which lets the object reach 0.5 / strength (SourceNote).
template <class Reader>
Guard ReadChain(Reader&& read, const GameBuild* build, std::uintptr_t moduleBase, RawObject& out, float maxJitter = 0.5f)
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

    if (!std::isfinite(o.jx) || !std::isfinite(o.jy) || std::fabs(o.jx) > maxJitter || std::fabs(o.jy) > maxJitter)
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
    // One event, not a run (a configuration change, a ratio adopted): true while the line budget lasts, and the event closes at once.
    bool Occurrence()
    {
        const bool logged = Event(true);
        _open = false;
        return logged;
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

// The source-level jitter fix ([FFXIV] JitterSource, FfxivJitterSourceController.h) next to this engine: while it is active the generator output
// is divided by the native strength, so the camera object holds up to 0.5 / strength instead of 0.5. The wrapper passes `active` and the larger bound;
// the probe then reads the object and says so (reported jx/jy is the generator value before the game's multiplier, rx/ry is that multiplier).
struct SourceNote
{
    bool active = false;
    float maxJitter = 0.5f;
    bool standDown = false; // the fix owns this evaluate's pair (active, or finishing frames drawn on the full lattice): the engine was told correct = false for that reason only,
                            // so it must not forget what it learned (it forgets only when the correction key itself is off)
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

inline std::string VerdictText(const Analysis& a, bool sourceActive = false)
{
    auto scale = [](bool have, float s) { return have ? std::format("{:.3f}", s) : std::string("n/a"); };
    const std::string values = std::format("implied s = {}/{}", scale(a.haveRx, a.ImpliedScaleX()), scale(a.haveRy, a.ImpliedScaleY()));
    switch (a.verdict)
    {
    case Verdict::Consistent: return values + " (consistent)";
    case Verdict::Differ:
        return values + (sourceActive ? " (applied = native strength * reported, as the fix intends)" : " (reported and applied jitter differ)");
    default: return values + " (undetermined: " + (a.haveApplied ? "jitter too small this sample" : "render size unknown") + ")";
    }
}

inline std::string FormatSample(const Analysis& a, NgxMatch ngx, const std::optional<float>& ngxX, const std::optional<float>& ngxY,
                                const Context& ctx, bool sourceActive = false)
{
    std::string ngxText;
    switch (sourceActive ? NgxMatch::Unavailable : ngx)
    {
    case NgxMatch::Same: ngxText = "NGX saw the same values"; break;
    case NgxMatch::Different:
        ngxText = std::format("NGX saw different values ({:.4f}/{:.4f}: the object belongs to another frame or the values changed on the way)",
                              ngxX.value_or(0.0f), ngxY.value_or(0.0f));
        break;
    default:
        ngxText = sourceActive ? std::format("jitter source fix active: reported jx/jy is the generator value before the game's multiplier, rx/ry is that "
                                             "multiplier, NGX is told the drawn offset ({:.4f}/{:.4f} now)",
                                             ngxX.value_or(0.0f), ngxY.value_or(0.0f))
                               : std::string("NGX jitter not available at this seam");
        break;
    }
    auto ratio = [](bool have, float r) { return have ? std::format("{:.3f}", r) : std::string("n/a"); };
    return std::format("Game jitter: counter {}, reported jx/jy {:.4f}/{:.4f}, ndc {:.6f}/{:.6f}, render {}x{} ({}), applied ax/ay {:.4f}/{:.4f}, "
                       "ratio rx/ry {}/{}, signs {}, {}, {}; {}",
                       a.object.counter, a.object.jx, a.object.jy, a.object.ndcX, a.object.ndcY, a.size.width, a.size.height, a.size.source,
                       a.ax, a.ay, ratio(a.haveRx, a.rx), ratio(a.haveRy, a.ry),
                       a.signMismatchX || a.signMismatchY ? std::string("MISMATCH") : std::string("ok"), VerdictText(a, sourceActive), ngxText,
                       ContextText(ctx));
}

inline std::string FormatProbeFailure(Guard g) { return std::format("Game jitter: not read, guard failed: {}", GuardName(g)); }

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

// `passThroughs` = jitter pairs that were not usable, `replacements` = first scaled pairs of a calibration, `calibrationEvents` = learning started,
// calibrated, consistent, refused, ratio changed, configuration changed, verification stale and fresh again. The first three fields and their
// order are the probe-only report of the earlier versions.
inline std::string FormatSuppressed(std::uint64_t probeFailures, std::uint64_t passThroughs, std::uint64_t replacements,
                                    std::uint64_t abandonedSteps, std::uint64_t calibrationEvents)
{
    return std::format("Game jitter: log line budget used up, episodes not logged so far: {} probe failures, {} pass-throughs, {} replacements, "
                       "{} abandoned steps, {} calibration events",
                       probeFailures, passThroughs, replacements, abandonedSteps, calibrationEvents);
}
} // namespace ext::ffxiv::jitter
