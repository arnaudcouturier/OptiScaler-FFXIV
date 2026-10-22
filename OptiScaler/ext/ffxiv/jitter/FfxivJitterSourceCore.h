#pragma once
// Source-level jitter fix, pure core ([FFXIV] JitterSource). No Windows calls, no logging, no pch: the glue (FfxivJitterSourceHooks.cpp)
// supplies the game's memory, the Detours hooks and the clock, so every number and every decision below is covered by
// tests/ext/ffxiv/jitter/jitter_source_*.cpp on plain values.
//
// What the fix is, in one paragraph. FFXIV's camera applies only a fraction of the jitter its generator produces (the "native strength",
// measured 0.6) and, in DLSS mode, calls the generator with an unbounded phase count so the pattern never repeats. Calibrate-and-scale
// (FfxivJitterCore.h) only changes what NGX is TOLD. This fix changes what is DRAWN: it divides the generator output by the native strength so
// the game's own multiplication leaves the full +-0.5 pixel lattice, it replaces the unbounded phase count by NVIDIA's
// ceil(8 * output area / render area), and it tells NGX exactly the offset that was drawn (the NDC translation of the same frame, converted to
// pixels). Everything is computed from values the game itself produced, in the order the game uses them.
//
// The game's formula, as the executable computes it (camera update, RVA 0x25f690; the bytes are pinned in GameBuild.h):
//   ratio    = (u16 at renderer+0x700 / u16 at renderer+0x704) * float at settings+0x4c
//   strength = (float at settings+0x78 - 0.001f) * (ratio * ratio) + 0.001f
//   ndc      = (2 * strength * jitter) / size        (y negated)      applied pixel offset = ndc * size / 2 = strength * jitter

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace ext::ffxiv::jitter::source
{
// ---- phase count ---------------------------------------------------------------------------------------------------------------

inline constexpr std::uint32_t kNativePhases = 0xFFFFFFFFu; // the unbounded phase count the game passes in DLSS mode
inline constexpr std::uint32_t kMinPhases = 8;
inline constexpr std::uint32_t kMaxPhases = 4096;
inline constexpr std::uint32_t kMaxExtent = 16384;

// NVIDIA's rule, 8 * (output area / render area) rounded up, in exact integer arithmetic (8 at native, 18 Quality, 24 Balanced, 32 Performance,
// 72 Ultra Performance). 0 = the sizes are not usable (an extent of 0 or above kMaxExtent, or a render size larger than the output): the
// caller leaves the game's own phase count alone.
constexpr std::uint32_t PhaseCount(std::uint32_t renderW, std::uint32_t renderH, std::uint32_t outW, std::uint32_t outH)
{
    if (renderW == 0 || renderH == 0 || outW == 0 || outH == 0 || renderW > kMaxExtent || renderH > kMaxExtent || outW > kMaxExtent ||
        outH > kMaxExtent || renderW > outW || renderH > outH)
        return 0;
    const std::uint64_t numerator = 8ull * outW * outH;
    const std::uint64_t denominator = static_cast<std::uint64_t>(renderW) * renderH;
    const std::uint64_t count = (numerator + denominator - 1) / denominator;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(count, kMinPhases, kMaxPhases));
}

// ---- native strength -----------------------------------------------------------------------------------------------------------

inline constexpr float kMinStrength = 0.125f; // outside [kMinStrength, kMaxStrength] the strength is not believed and the call is left alone
inline constexpr float kMaxStrength = 4.0f;
inline constexpr float kStrengthFloor = 0.001f; // the constant at RVA 0x211dd64 that the executable subtracts and adds

// The executable's own float expression, in its own order of operations. nullopt = a value is not usable (extent 0, not finite, outside the range).
inline std::optional<float> NativeStrength(float amount, float scale, std::uint16_t extent0, std::uint16_t extent2)
{
    if (extent2 == 0 || !std::isfinite(amount) || !std::isfinite(scale))
        return std::nullopt;
    const float ratio = (static_cast<float>(extent0) / static_cast<float>(extent2)) * scale;
    const float strength = (amount - kStrengthFloor) * (ratio * ratio) + kStrengthFloor;
    if (!std::isfinite(strength) || strength < kMinStrength || strength > kMaxStrength)
        return std::nullopt;
    return strength;
}

// Divides the generator output by the strength so that the game's multiplication leaves the generator's own value. true = the frame is drawn on
// the generator's own lattice: the output was divided, or the strength is within 1e-6 of 1 and there is nothing to divide (the output is already
// what the game will draw; the phase count was replaced all the same, so the call must count as normalised). false, output unchanged, when the
// output is not a Halton value in [-0.5, 0.5] (plus float noise) or the strength is outside its range.
inline bool NormalizeHalton(float& x, float& y, float strength)
{
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 0.501f || std::fabs(y) > 0.501f || !std::isfinite(strength) ||
        strength < kMinStrength || strength > kMaxStrength)
        return false;
    if (std::fabs(strength - 1.0f) < 1e-6f)
        return true;
    x /= strength;
    y /= strength;
    return true;
}

// ---- what the generator hook does, as a plan -----------------------------------------------------------------------------------

struct GeneratorInputs
{
    bool dlssMode = false;      // the game's settings byte at +0x54 equals 2
    float amount = 0.0f;        // settings+0x78
    float scale = 0.0f;         // settings+0x4c
    std::uint16_t extent0 = 0;  // renderer+0x700
    std::uint16_t extent2 = 0;  // renderer+0x704
    std::uint32_t phases = 0;   // the phase count the game passed
    std::uint32_t renderW = 0, renderH = 0; // renderer+0x428
    std::uint32_t outW = 0, outH = 0;       // device+0x8c
};

enum class PlanWhy : std::uint8_t
{
    Normalize,        // normalise the output (and the phase count when the game's is unbounded)
    NotDlssMode,      // another mode: the call is left alone
    StrengthInvalid   // a value is not usable: the call is left alone
};

struct GeneratorPlan
{
    PlanWhy why = PlanWhy::NotDlssMode;
    float strength = 0.0f;
    std::uint32_t phases = 0; // what to pass to the game's generator (the game's own value unless it was unbounded and the sizes are usable)
};

inline GeneratorPlan PlanGenerate(const GeneratorInputs& in)
{
    GeneratorPlan plan;
    plan.phases = in.phases;
    if (!in.dlssMode)
        return plan;
    const std::optional<float> strength = NativeStrength(in.amount, in.scale, in.extent0, in.extent2);
    if (!strength)
    {
        plan.why = PlanWhy::StrengthInvalid;
        return plan;
    }
    plan.why = PlanWhy::Normalize;
    plan.strength = *strength;
    if (in.phases == kNativePhases)
    {
        const std::uint32_t count = PhaseCount(in.renderW, in.renderH, in.outW, in.outH);
        if (count != 0)
            plan.phases = count;
    }
    return plan;
}

// ---- the offset that was drawn --------------------------------------------------------------------------------------------------

struct Pair
{
    float x = 0.0f, y = 0.0f;
};

// What the camera getter returned to the NGX packet producer: the generator output the game stored (rawX/rawY, what NGX is told today),
// the NDC translation it built from it, and the render size of that frame.
struct Observed
{
    float rawX = 0.0f, rawY = 0.0f, ndcX = 0.0f, ndcY = 0.0f;
    std::uint32_t width = 0, height = 0;
    std::uint64_t timeMs = 0;
};

// The pixel offset the frame was drawn with: x = ndcX * width / 2, y = -ndcY * height / 2. nullopt = the sample is torn or unrelated (a
// non-finite value, a raw value above 4, an offset above 2 pixels, the two axes not describing one strength, the sign of the NDC translation
// against the raw value wrong). The raw pair 0/0 is the game's jitter-disabled path, which leaves old NDC storage in place: its offset is 0/0.
inline std::optional<Pair> ProjectOffset(const Observed& s)
{
    if (s.width == 0 || s.height == 0 || s.width > kMaxExtent || s.height > kMaxExtent || !std::isfinite(s.rawX) || !std::isfinite(s.rawY) ||
        !std::isfinite(s.ndcX) || !std::isfinite(s.ndcY) || std::fabs(s.rawX) > 4.0f || std::fabs(s.rawY) > 4.0f)
        return std::nullopt;
    float x = static_cast<float>(static_cast<double>(s.ndcX) * s.width * 0.5);
    float y = static_cast<float>(-static_cast<double>(s.ndcY) * s.height * 0.5);
    if (s.rawX == 0.0f && s.rawY == 0.0f)
        x = y = 0.0f;
    // Both axes must describe the same strength; torn, stale or unrelated storage is refused rather than turned into an offset.
    if (std::fabs(x * s.rawY - y * s.rawX) > 1e-5f || x * s.rawX + y * s.rawY < -1e-6f)
        return std::nullopt;
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 2.0f || std::fabs(y) > 2.0f)
        return std::nullopt;
    if ((std::fabs(s.rawX) <= 1e-7f && std::fabs(x) >= 1e-5f) || (std::fabs(s.rawY) <= 1e-7f && std::fabs(y) >= 1e-5f))
        return std::nullopt;
    return Pair { x, y };
}

// A fixed-size ring, newest first on lookup. Not thread safe: the shared state locks around it.
template <class T, std::size_t N> class Ring
{
  public:
    void Push(const T& value)
    {
        _items[_next % N] = value;
        ++_next;
    }
    // The newest entry for which `match` is true.
    template <class F> std::optional<T> FindNewest(F&& match) const
    {
        const std::size_t count = std::min<std::size_t>(_next, N);
        for (std::size_t i = 0; i < count; ++i)
        {
            const T& item = _items[(_next - 1 - i) % N];
            if (match(item))
                return item;
        }
        return std::nullopt;
    }
    void Clear() { _next = 0; }
    std::size_t Size() const { return std::min<std::size_t>(_next, N); }

  private:
    std::array<T, N> _items {};
    std::size_t _next = 0;
};

inline constexpr std::size_t kRingSize = 32;
inline constexpr std::uint64_t kMaxSampleAgeMs = 1000; // an observed sample older than this is not paired with an evaluate
inline constexpr float kRawMatchTolerance = 1e-7f;      // the NGX packet holds a float copy of the raw pair: an exact match, with float noise as margin

struct Match
{
    Observed observed;
    Pair pixels;
};

// One entry per frame the generator produced: what it stored (outX/outY, equal to the raw pair of that frame's observed sample) and, when the
// output was normalised, the Halton value the game must now draw (expectedX/expectedY, the pre-normalisation output).
struct Generated
{
    float outX = 0.0f, outY = 0.0f;
    float expectedX = 0.0f, expectedY = 0.0f;
    float strength = 0.0f;
    std::uint32_t phases = 0;
    std::uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0; // the sizes the plan was made from
    bool normalized = false;
    std::uint64_t timeMs = 0;
};

inline bool SamePair(float ax, float ay, float bx, float by)
{
    return std::fabs(ax - bx) <= kRawMatchTolerance && std::fabs(ay - by) <= kRawMatchTolerance;
}

// The pairing rule between an evaluate's NGX packet and an observed camera sample: the raw pair is the same float pair (the game copies it into
// the packet), the render size is the same within one pixel, and the sample is younger than a second. The newest sample wins; a sample whose
// offset cannot be computed is skipped. Anything else is no match (the evaluate then passes through).
inline std::optional<Match> PairSample(const Ring<Observed, kRingSize>& ring, float ngxX, float ngxY, std::uint32_t renderW, std::uint32_t renderH,
                                       std::uint64_t nowMs)
{
    if (!std::isfinite(ngxX) || !std::isfinite(ngxY) || renderW == 0 || renderH == 0)
        return std::nullopt;
    auto close = [](std::uint32_t a, std::uint32_t b) { return a > b ? a - b <= 1 : b - a <= 1; };
    const std::optional<Observed> found = ring.FindNewest(
        [&](const Observed& o)
        {
            const std::uint64_t age = nowMs >= o.timeMs ? nowMs - o.timeMs : 0;
            return o.timeMs != 0 && age < kMaxSampleAgeMs && close(o.width, renderW) && close(o.height, renderH) &&
                   SamePair(o.rawX, o.rawY, ngxX, ngxY) && ProjectOffset(o).has_value();
        });
    if (!found)
        return std::nullopt;
    return Match { *found, *ProjectOffset(*found) };
}

// ---- verification -------------------------------------------------------------------------------------------------------------

inline constexpr float kVerifyTolerancePx = 0.005f;    // projected offset against the generator's own Halton value, per axis
inline constexpr unsigned kOutsideToleranceLimit = 90; // consecutive checked frames outside the tolerance that make the fix fall back
inline constexpr unsigned kUnpairedLimit = 90;         // consecutive evaluates with no matching camera sample that make the fix fall back (a miss is never harmless)

enum class Fallback : std::uint8_t
{
    None,
    OutsideTolerance, // what was drawn is not what the generator produced: the strength or the game's formula is not the one we know
    Unpaired,         // the evaluates stopped matching any camera sample
    Untellable        // a generator-divided value no record explains and the engine cannot scale: nothing sensible can be told (at once)
};

// Counts what the boundary saw. `Checked` = a normalised frame whose drawn offset was compared with the generator's value.
class Watchdog
{
  public:
    struct Totals
    {
        std::uint64_t checked = 0, outside = 0, unverified = 0, unpaired = 0;
        std::uint64_t toldFromRecord = 0; // of the unpaired evaluates, those told the generator record's own value
        std::uint64_t untellable = 0;
        float maxDeviation = 0.0f;
    };

    // A frame was paired and its generator record found and normalised: compare.
    void Checked(float deviation)
    {
        ++_totals.checked;
        _totals.maxDeviation = std::max(_totals.maxDeviation, deviation);
        _unpairedRun = 0;
        if (deviation > kVerifyTolerancePx)
        {
            ++_totals.outside;
            ++_outsideRun;
        }
        else
            _outsideRun = 0;
    }
    // Paired, but no normalised generator record for it (the generator left that call alone, or the record is gone).
    void Unverified()
    {
        ++_totals.unverified;
        _unpairedRun = 0;
    }
    void Unpaired()
    {
        ++_totals.unpaired;
        ++_unpairedRun;
    }
    // An unpaired evaluate that was told the generator record's value (counted in `unpaired` as well: a pairing that is broken for good must
    // still fall back, however well the record path hides it).
    void ToldFromRecord() { ++_totals.toldFromRecord; }
    // An unpaired evaluate whose value cannot be told or scaled: the verdict is a fall-back at once.
    void Untellable()
    {
        ++_totals.untellable;
        _untellable = true;
    }
    Fallback Verdict() const
    {
        if (_untellable)
            return Fallback::Untellable;
        if (_outsideRun >= kOutsideToleranceLimit)
            return Fallback::OutsideTolerance;
        if (_unpairedRun >= kUnpairedLimit)
            return Fallback::Unpaired;
        return Fallback::None;
    }
    const Totals& totals() const { return _totals; }
    void Reset() { *this = Watchdog {}; }

  private:
    Totals _totals;
    unsigned _outsideRun = 0, _unpairedRun = 0;
    bool _untellable = false;
};

// The pairs NGX was told, over a window: their extremes and how many distinct pairs there were. At Performance that is at most 32 pairs with
// |x|, |y| up to about 0.488 (NVIDIA's lattice), whatever the length of the window; an unbounded pattern (the game's own phase count) fills the
// table and sets the overflow marker. This checks amplitude and period without trusting the strength decode.
inline constexpr std::size_t kMaxLatticePairs = 128;

class ToldLattice
{
  public:
    struct Result
    {
        std::uint64_t frames = 0;
        std::size_t distinct = 0;  // at most kMaxLatticePairs
        bool overflow = false;     // more distinct pairs than the table holds
        float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    };

    void Add(float x, float y)
    {
        if (_frames == 0)
            _r.minX = _r.maxX = x, _r.minY = _r.maxY = y;
        else
        {
            _r.minX = std::min(_r.minX, x);
            _r.maxX = std::max(_r.maxX, x);
            _r.minY = std::min(_r.minY, y);
            _r.maxY = std::max(_r.maxY, y);
        }
        ++_frames;
        for (std::size_t i = 0; i < _count; ++i)
            if (_pairs[i].x == x && _pairs[i].y == y) // value equality: 0 and -0 are one pair
                return;
        if (_count < kMaxLatticePairs)
            _pairs[_count++] = { x, y };
        else
            _overflow = true;
    }
    Result Summary() const
    {
        Result r = _r;
        r.frames = _frames;
        r.distinct = _count;
        r.overflow = _overflow;
        return r;
    }
    void Reset() { *this = ToldLattice {}; }

  private:
    Result _r;
    std::uint64_t _frames = 0;
    std::array<Pair, kMaxLatticePairs> _pairs {};
    std::size_t _count = 0;
    bool _overflow = false;
};

inline float Deviation(const Pair& drawn, const Generated& g)
{
    return std::max(std::fabs(drawn.x - g.expectedX), std::fabs(drawn.y - g.expectedY));
}

// ---- decision ------------------------------------------------------------------------------------------------------------------

enum class Reason : std::uint8_t
{
    None, // active
    KeyOff,
    ExecutableNotMatched,
    GuardFailed,
    HooksNotInstalled,
    FellBack,
    GeneratorIdle
};

struct DecisionInput
{
    bool keyOn = false;
    bool buildMatched = false;
    bool guardsOk = false;
    bool hooksInstalled = false;
    bool fellBack = false;
    bool generatorRecent = false; // the generator normalised a call within the last second
};

struct Decision
{
    bool active = false;
    Reason reason = Reason::KeyOff;
};

// The first failing condition names the reason; the fix is active only when none fails. Everything but "active" is today's behaviour:
// calibrate-and-scale (when its key is on) takes over.
constexpr Decision Decide(const DecisionInput& in)
{
    if (!in.keyOn)
        return { false, Reason::KeyOff };
    if (!in.buildMatched)
        return { false, Reason::ExecutableNotMatched };
    if (!in.guardsOk)
        return { false, Reason::GuardFailed };
    if (!in.hooksInstalled)
        return { false, Reason::HooksNotInstalled };
    if (in.fellBack)
        return { false, Reason::FellBack };
    if (!in.generatorRecent)
        return { false, Reason::GeneratorIdle };
    return { true, Reason::None };
}

constexpr const char* ReasonText(Reason r)
{
    switch (r)
    {
    case Reason::None: return "active";
    case Reason::KeyOff: return "key off";
    case Reason::ExecutableNotMatched: return "executable not matched";
    case Reason::GuardFailed: return "instruction guard failed";
    case Reason::HooksNotInstalled: return "hooks not installed";
    case Reason::FellBack: return "fell back after the verification failed";
    case Reason::GeneratorIdle: return "the game's generator is not being normalised (not in DLSS mode, native strength not valid, the world camera not known yet, or not running)";
    }
    return "unknown";
}

constexpr const char* FallbackText(Fallback f)
{
    switch (f)
    {
    case Fallback::OutsideTolerance: return "the drawn offset differs from the generator's Halton value for the same frame";
    case Fallback::Unpaired: return "no camera sample matched the NGX jitter of the evaluate";
    case Fallback::Untellable: return "a jitter value divided by the generator hook cannot be told or scaled (no generator record, above the 0.55 px the scaling accepts)";
    case Fallback::None: break;
    }
    return "none";
}

// ---- shared state between the detours (game threads) and the boundary (evaluate thread) ---------------------------------------------

inline constexpr std::uint64_t kRecentMs = 1000;      // "the generator is normalising" = it normalised a call this recently
inline constexpr std::uint64_t kDrainMs = 500;        // frames already drawn on the full lattice when the fix is switched off are still told what was drawn

// What the generator hook last did. The detours write it, the boundary reads it.
struct GeneratorStatus
{
    bool recent = false;           // normalised a call within kRecentMs of `now`
    std::uint64_t lastNormalizedMs = 0;
    PlanWhy lastWhy = PlanWhy::NotDlssMode; // what the most recent call did (Normalize, or why it left the call alone)
    float strength = 0.0f;
    std::uint32_t phases = 0, renderW = 0, renderH = 0, outW = 0, outH = 0; // of the last normalised call
};

// The distinct jitter objects (output pointers) the generator's camera call site has been called with, and how often. A second one means a second
// jittered camera reaches the call site; the fix only ever touches the world camera's (the one the NGX packet producer reads).
inline constexpr std::size_t kMaxSeenObjects = 8;

struct SeenObject
{
    std::uintptr_t pointer = 0;
    std::uint64_t calls = 0;
};

struct SeenObjects
{
    std::array<SeenObject, kMaxSeenObjects> items {};
    std::size_t count = 0;   // distinct pointers stored (at most kMaxSeenObjects)
    bool overflow = false;   // more distinct pointers than the table holds
};

class SharedState
{
  public:
    // The switch both detours check first. false = both are pass-through (the generator is not touched, the camera getter is not sampled).
    // Switching on forgets the world camera's object: it is learned again from the producer (the camera may have been rebuilt while the hooks were
    // passive), and until then the generator is left alone.
    void Enable(bool on)
    {
        const bool was = _enabled.exchange(on, std::memory_order_acq_rel);
        if (on && !was)
            _world.store(0, std::memory_order_release);
    }
    bool Enabled() const { return _enabled.load(std::memory_order_acquire); }

    // The jitter object of the world camera = the pointer the camera getter returns to the world-scene NGX packet producer ([camera + 0x278], the
    // same pointer the camera update passes to the generator as its output). 0 = not known yet; a null pointer is never learned.
    void LearnWorldObject(std::uintptr_t object)
    {
        if (object != 0)
            _world.store(object, std::memory_order_release);
    }
    std::uintptr_t WorldObject() const { return _world.load(std::memory_order_acquire); }

    // The generator's camera call site was reached with this output pointer (before any decision about it).
    void NoteCallSiteObject(std::uintptr_t object)
    {
        std::lock_guard lock(_mutex);
        for (std::size_t i = 0; i < _seen.count; ++i)
            if (_seen.items[i].pointer == object)
            {
                ++_seen.items[i].calls;
                return;
            }
        if (_seen.count < kMaxSeenObjects)
            _seen.items[_seen.count++] = { object, 1 };
        else
            _seen.overflow = true;
    }
    SeenObjects Seen() const
    {
        std::lock_guard lock(_mutex);
        return _seen;
    }

    void RecordObserved(const Observed& o)
    {
        std::lock_guard lock(_mutex);
        _observed.Push(o);
    }
    void RecordGenerated(const Generated& g, PlanWhy why)
    {
        std::lock_guard lock(_mutex);
        _generated.Push(g);
        _status.lastWhy = why;
        if (g.normalized)
        {
            _status.lastNormalizedMs = g.timeMs;
            _status.strength = g.strength;
            _status.phases = g.phases;
            _status.renderW = g.renderW;
            _status.renderH = g.renderH;
            _status.outW = g.outW;
            _status.outH = g.outH;
        }
    }
    // The call was left alone (another mode, a value not usable); kept as the reason shown when the generator is idle.
    void NoteLeftAlone(PlanWhy why)
    {
        std::lock_guard lock(_mutex);
        _status.lastWhy = why;
    }

    std::optional<Match> Pair(float ngxX, float ngxY, std::uint32_t renderW, std::uint32_t renderH, std::uint64_t nowMs) const
    {
        std::lock_guard lock(_mutex);
        return PairSample(_observed, ngxX, ngxY, renderW, renderH, nowMs);
    }
    // The generator's record of the frame whose stored output is this raw pair (newest first).
    std::optional<Generated> FindGenerated(float rawX, float rawY) const
    {
        std::lock_guard lock(_mutex);
        return _generated.FindNewest([&](const Generated& g) { return g.timeMs != 0 && SamePair(g.outX, g.outY, rawX, rawY); });
    }
    GeneratorStatus Status(std::uint64_t nowMs) const
    {
        std::lock_guard lock(_mutex);
        GeneratorStatus s = _status;
        const std::uint64_t age = nowMs >= s.lastNormalizedMs ? nowMs - s.lastNormalizedMs : 0;
        s.recent = s.lastNormalizedMs != 0 && age < kRecentMs;
        return s;
    }
    void Clear()
    {
        std::lock_guard lock(_mutex);
        _observed.Clear();
        _generated.Clear();
        _status = GeneratorStatus {};
        _seen = SeenObjects {};
        _world.store(0, std::memory_order_release);
    }

  private:
    mutable std::mutex _mutex;
    std::atomic<bool> _enabled { false };
    std::atomic<std::uintptr_t> _world { 0 };
    SeenObjects _seen;
    Ring<Observed, kRingSize> _observed;
    Ring<Generated, kRingSize> _generated;
    GeneratorStatus _status;
};

// ---- log lines (exact text; the in-game checklist quotes them) ---------------------------------------------------------------------

inline std::string FormatActive(float strength, std::uint32_t phases, std::uint32_t renderW, std::uint32_t renderH, std::uint32_t outW,
                                std::uint32_t outH)
{
    return std::format("Jitter source: active (strength {:.3f}, phases {} for render {}x{} output {}x{})", strength, phases, renderW, renderH, outW,
                       outH);
}

inline std::string FormatInactive(Reason r) { return std::format("Jitter source: inactive ({})", ReasonText(r)); }

inline std::string FormatPhasesChanged(std::uint32_t before, std::uint32_t after, float strength, std::uint32_t renderW, std::uint32_t renderH,
                                       std::uint32_t outW, std::uint32_t outH)
{
    return std::format("Jitter source: phases {} -> {} (strength {:.3f}, render {}x{} output {}x{})", before, after, strength, renderW, renderH, outW,
                       outH);
}

inline std::string FormatStandDown()
{
    return "Jitter source: jitter correction stands down while the source fix is active (NGX is told the drawn offset, no scaling)";
}

// The told offsets over the window since the previous verification line.
inline std::string FormatLattice(const ToldLattice::Result& l, std::uint32_t phases)
{
    if (l.frames == 0)
        return "no told frames";
    return std::format("told offsets over {} frames: x {:.3f}..{:.3f} px, y {:.3f}..{:.3f} px, {} distinct pairs (phase count {})", l.frames, l.minX,
                       l.maxX, l.minY, l.maxY, l.overflow ? std::format("more than {}", l.distinct) : std::format("{}", l.distinct), phases);
}

inline std::string FormatVerified(const Watchdog::Totals& t, const ToldLattice::Result& lattice, std::uint32_t phases)
{
    // "equals" only when nothing was outside the tolerance
    const std::string drawn = t.outside == 0 ? std::string("drawn offset equals the generator's value")
                                             : std::format("drawn offset differs from the generator's value in {} frames", t.outside);
    return std::format("Jitter source: verified {} frames, {} (max deviation {:.5f} px, outside {:.3f} px: {}), "
                       "{} frames without a generator record, {} evaluates without a camera sample ({} told the generator's recorded value, {} passed through); {}",
                       t.checked, drawn, t.maxDeviation, kVerifyTolerancePx, t.outside, t.unverified, t.unpaired, t.toldFromRecord,
                       t.unpaired >= t.toldFromRecord ? t.unpaired - t.toldFromRecord : 0, FormatLattice(lattice, phases));
}

inline std::string FormatFellBack(Fallback f, const Watchdog::Totals& t)
{
    return std::format("Jitter source: fell back to the old behaviour, generator hook and told-offset write disabled together ({}; checked {}, "
                       "outside tolerance {}, unpaired {})",
                       FallbackText(f), t.checked, t.outside, t.unpaired);
}

// The one diagnostic of the world-camera tie: the distinct objects the generator's call site was reached with. More than one = a second jittered
// object exists; the fix leaves it alone (it is not the world camera the NGX packet producer reads).
inline std::string FormatSeenObjects(const SeenObjects& seen, std::uintptr_t world)
{
    std::string list;
    for (std::size_t i = 0; i < seen.count; ++i)
    {
        if (i != 0)
            list += ", ";
        list += std::format("{:#x} ({} calls{})", seen.items[i].pointer, seen.items[i].calls, seen.items[i].pointer == world ? ", world camera" : "");
    }
    if (seen.overflow)
        list += ", more";
    std::string text = std::format("Jitter source: the generator's camera call site was reached with {} distinct jitter object{}: {}", seen.count,
                                   seen.count == 1 ? "" : "s", list);
    if (seen.count > 1 || seen.overflow)
        text += "; a second jittered object exists, it is left alone (only the world camera's is normalised)";
    return text;
}

inline std::string FormatSizeDisagreement(std::uint32_t gameW, std::uint32_t gameH, std::uint32_t ngxW, std::uint32_t ngxH)
{
    return std::format("Jitter source: the output size the game reports ({}x{}) differs from NGX OutWidth/OutHeight ({}x{}); the phase count follows the "
                       "game's size",
                       gameW, gameH, ngxW, ngxH);
}
} // namespace ext::ffxiv::jitter::source
