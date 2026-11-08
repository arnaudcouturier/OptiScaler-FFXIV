#pragma once
// Effect meter (METER): the pure rules. No pch, no Windows, D3D or log call, so every decision here is covered by tests/ext/ffxiv/meter on the host
// (meter_rules); the GPU pass is MeterPass.h (meter_warp), the runtime is Meter.cpp (meter_pins).
//
// Why it exists. The 2026-11-07 live sessions showed that half the effects changed nothing on the user's screen and that nothing in the log said so: the haze
// passed every lab bar and never ran live because one harmless input variant was refused. The meter answers, live and per effect, the two questions that matter:
// "did it run?" (frames reached / ran / skipped by reason) and "can it be seen?" (the share of the effect's region whose output differs from its input by more
// than 8/255, and the mean change).
//
// THE MEASURE, defined once (the shader Meter.hlsl, the CPU reference below and the acceptance scripts all use this):
//   level(x)   = saturate(x / white) * 255          for the chain's stages (the scene is stored colour: `white` is the frame's white point, the stored value that
//                                                   reaches display-white, so this is "display terms" after a tone-map proxy, clamped at white because a pixel
//                                                   above white shows the same on the display);
//              = saturate(x) * 255                  for the pre-light stages (UNORM textures: masks, the occlusion, the planar map, the G-buffer: their own levels);
//   change     = max over the stage's channels of |level(out) - level(in)|   (VIS1's convention: the largest channel)
//   visible    = change > 8   (kVisibleLevels)
//   region     = the texels where the effect acts, decided per effect by RegionKind (WHERE IT ACTS, stated in kEffects):
//                  window    - the render window (the chain stages and the puddle gloss)
//                  occluded  - the occlusion texels that are not white in the engine's or in this module's texture (level < 254.5)
//                  lit       - the sun mask's texels with any sun in the engine's or in the combined mask (level >= 7.5)
//                  shaded    - the shadow-lift mask's texels that are not fully lit in either mask (level < 247.5)
//                  reflecting- the planar map's texels with any light in the engine's or in this module's map (a channel level >= 0.5)
//   share      = visible texels in the region / texels in the region;   mean = the sum of `change` over the region / texels in the region
//   coverage   = region texels / window texels
// A NaN texel counts as no change (it is neither visible nor in the mean).
//
// HOW IT IS MEASURED. One pixel-shader draw (no vertex-shader resources, so the explicit-state guard covers everything it touches) over a small target: each
// target texel is one BLOCK of the region (block = 1, 2, 4 ... texels on a side) and reads kTapsPerSide^2 stratified, jittered taps of it (every texel when the
// block is 4 or smaller), and writes (region taps, visible taps, change sum, taps). The target is copied to a staging texture and read a frame or more later
// without waiting; the CPU adds the texels in doubles. At 1920x1080 that is 16 taps of a 16x16 block (1/16 of the texels) and at 3840x2160 16 taps of a 32x32 block
// (1/64), 131k taps either way: the cost is the texture reads, so the tap budget is what the GPU time follows (meter_cost).

#include "ext/ffxiv/diag/StageDumpFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::meter
{
using ext::ffxiv::diag::StageId;

inline constexpr uint32_t kEffectCount = uint32_t(StageId::Count);

// ---- the measure -----------------------------------------------------------------------------------------------------------------------------------

inline constexpr float kVisibleLevels = 8.0f; // 8/255: what stands out without a comparison (VIS1's glance tier)
inline constexpr float kLevelRange = 255.0f;
inline constexpr float kFallbackWhite = 0.838f; // the captured daylight measurement (shafts::kVerifiedWhitePoint), when no stage of the frame knew the white

// Whether a change is visible: strictly above the threshold (8.0 exactly is not).
constexpr bool IsVisible(float change) noexcept { return change > kVisibleLevels; }

enum class Unit : uint8_t
{
    Display = 0, // stored colour of the tone-map chain: levels of (value / white), clamped at white
    Unorm = 1,   // a UNORM texture's own levels
};

enum class RegionKind : uint8_t
{
    Window = 0,
    Occluded = 1,
    Lit = 2,
    Shaded = 3,
    Reflecting = 4,
};

inline constexpr uint32_t kChannelR = 1, kChannelG = 2, kChannelB = 4, kChannelA = 8;
inline constexpr uint32_t kChannelsRgb = kChannelR | kChannelG | kChannelB;

struct EffectInfo
{
    StageId id;
    const char* name;       // the log's and the card's name
    const char* regionText; // where it acts, as the log says it
    Unit unit;
    RegionKind region;
    uint32_t channels;
    bool metered; // false: the effect makes no picture (the lens flare patches the engine's constants)
};

// One row per StageId, in the registry's order (StageDumpFormat.h). The regions are fixed before reading any capture and stated here once.
inline constexpr EffectInfo kEffects[kEffectCount] {
    { StageId::ContactShadows, "Contact shadows", "the sun mask's lit texels", Unit::Unorm, RegionKind::Lit, kChannelR, true },
    { StageId::ShadowLift, "Shadow lift", "the shadow mask's texels that are not fully lit", Unit::Unorm, RegionKind::Shaded, kChannelR, true },
    { StageId::Ao, "Ambient occlusion", "the occlusion texels that are not white", Unit::Unorm, RegionKind::Occluded, kChannelR, true },
    { StageId::Gloss, "Puddle gloss", "the render window (the gloss channel)", Unit::Unorm, RegionKind::Window, kChannelA, true },
    { StageId::Ssr, "Water reflections", "the planar map's texels with any reflection light", Unit::Unorm, RegionKind::Reflecting, kChannelsRgb, true },
    { StageId::Shafts, "Light shafts", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::PuddleTone, "Puddle tone", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::Godrays, "Volumetric fog", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::Gi, "Light bounce", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::Dof, "Depth of field", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::Flare, "Lens flare", "(no picture)", Unit::Display, RegionKind::Window, kChannelsRgb, false },
    { StageId::Atmosphere, "Atmosphere", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
    { StageId::NightGrade, "Exposure hold", "the render window", Unit::Display, RegionKind::Window, kChannelsRgb, true },
};

constexpr const EffectInfo& InfoOf(StageId id) noexcept { return kEffects[size_t(id)]; }

constexpr bool RowsInRegistryOrder() noexcept
{
    for (uint32_t i = 0; i < kEffectCount; ++i)
        if (uint32_t(kEffects[i].id) != i)
            return false;
    return true;
}
static_assert(RowsInRegistryOrder(), "kEffects must have one row per StageId, in the registry's order");

constexpr float Saturate(float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// One channel's level (0..255) in the effect's unit. A NaN is 0 (the comparison form below never lets it through).
constexpr float LevelOf(float value, Unit unit, float white) noexcept
{
    const float scaled = unit == Unit::Display ? value / white : value;
    return Saturate(scaled) * kLevelRange;
}

constexpr bool WhiteUsable(float white) noexcept { return white > 0.05f && white < 64.0f; }

// The change of one texel: the largest channel's |level(out) - level(in)|. -1 when the texel is not a number (it counts as nothing).
constexpr float ChangeLevels(const float in[4], const float out[4], uint32_t channels, Unit unit, float white) noexcept
{
    float change = 0.0f;
    bool any = false;
    for (uint32_t c = 0; c < 4; ++c)
    {
        if ((channels & (1u << c)) == 0)
            continue;
        if (!(in[c] == in[c]) || !(out[c] == out[c]))
            return -1.0f;
        any = true;
        const float d = LevelOf(out[c], unit, white) - LevelOf(in[c], unit, white);
        const float a = d < 0.0f ? -d : d;
        if (a > change)
            change = a;
    }
    return any ? change : -1.0f;
}

// Whether the texel is in the effect's region (WHERE IT ACTS, the file's header): decided on the channels the effect writes, in the effect's unit.
constexpr bool InRegion(RegionKind kind, const float in[4], const float out[4], uint32_t channels, Unit unit, float white) noexcept
{
    if (kind == RegionKind::Window)
        return true;
    float minLevel = 1.0e9f, maxLevel = -1.0e9f;
    for (uint32_t c = 0; c < 4; ++c)
    {
        if ((channels & (1u << c)) == 0)
            continue;
        const float a = LevelOf(in[c], unit, white), b = LevelOf(out[c], unit, white);
        minLevel = std::min(minLevel, std::min(a, b));
        maxLevel = std::max(maxLevel, std::max(a, b));
    }
    switch (kind)
    {
    case RegionKind::Window: return true;
    case RegionKind::Occluded: return minLevel < 254.5f;
    case RegionKind::Lit: return maxLevel >= 7.5f;
    case RegionKind::Shaded: return minLevel < 247.5f;
    case RegionKind::Reflecting: return maxLevel >= 0.5f;
    }
    return true;
}

// ---- the sampling geometry -------------------------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kMaxBinsSide = 256;     // the target is at most 256 x 256 texels (one RGBA32F megabyte)
inline constexpr uint32_t kMaxBins = 8192;        // and at most this many blocks in all (131k taps: memory-bound, measured 0.03 ms at 4K FP16)
inline constexpr uint32_t kMaxTapsPerSide = 4;
inline constexpr uint32_t kMaxBlock = 128;

struct Geometry
{
    uint32_t block = 1;        // texels on a side of one block
    uint32_t binsX = 0, binsY = 0;
    uint32_t tapsPerSide = 1;  // min(kMaxTapsPerSide, block)
    uint32_t cell = 1;         // texels on a side of one stratum: block / tapsPerSide
    constexpr uint32_t Taps() const noexcept { return tapsPerSide * tapsPerSide; }
    constexpr bool Exact() const noexcept { return block <= kMaxTapsPerSide; } // every texel of the box is a tap
    constexpr bool Valid() const noexcept { return binsX != 0 && binsY != 0; }
};

// The smallest power-of-two block that fits the box in the target: at most kMaxBinsSide blocks a side and kMaxBins in all.
constexpr Geometry GeometryFor(uint32_t width, uint32_t height) noexcept
{
    Geometry g;
    if (width == 0 || height == 0)
        return g;
    for (uint32_t block = 1; block <= kMaxBlock; block <<= 1)
    {
        const uint32_t bx = (width + block - 1) / block, by = (height + block - 1) / block;
        if (bx <= kMaxBinsSide && by <= kMaxBinsSide && uint64_t(bx) * by <= kMaxBins)
        {
            g.block = block;
            g.binsX = bx;
            g.binsY = by;
            g.tapsPerSide = block < kMaxTapsPerSide ? block : kMaxTapsPerSide;
            g.cell = block / g.tapsPerSide;
            return g;
        }
    }
    return g; // wider than 64 * 256 texels: not measured
}

// The jitter of one tap: an integer hash of the block, the tap and the sample's seed. The shader repeats it bit for bit (unsigned 32-bit arithmetic).
constexpr uint32_t TapHash(uint32_t binX, uint32_t binY, uint32_t tap, uint32_t seed) noexcept
{
    uint32_t h = (binX * 73856093u) ^ (binY * 19349663u) ^ (tap * 83492791u) ^ (seed * 2654435761u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

struct TapOffset
{
    uint32_t x = 0, y = 0;
};

// The tap's offset inside its block: its stratum (tap % T, tap / T) plus the jitter inside the stratum.
constexpr TapOffset TapOf(const Geometry& g, uint32_t binX, uint32_t binY, uint32_t tap, uint32_t seed) noexcept
{
    const uint32_t cx = tap % g.tapsPerSide, cy = tap / g.tapsPerSide;
    const uint32_t h = TapHash(binX, binY, tap, seed);
    TapOffset o;
    o.x = cx * g.cell + (g.cell > 1 ? (h & 0xFFFFu) % g.cell : 0u);
    o.y = cy * g.cell + (g.cell > 1 ? (h >> 16) % g.cell : 0u);
    return o;
}

// ---- one measurement -------------------------------------------------------------------------------------------------------------------------------

struct Params
{
    Unit unit = Unit::Display;
    RegionKind region = RegionKind::Window;
    uint32_t channels = kChannelsRgb;
    float white = kFallbackWhite;
};

constexpr Params ParamsOf(StageId id, float white) noexcept
{
    const EffectInfo& info = InfoOf(id);
    Params p;
    p.unit = info.unit;
    p.region = info.region;
    p.channels = info.channels;
    p.white = info.unit == Unit::Display ? (WhiteUsable(white) ? white : kFallbackWhite) : 1.0f;
    return p;
}

// What a pass (or the CPU reference) found: sums over the taps.
struct Totals
{
    double taps = 0.0;    // taps inside the box
    double region = 0.0;  // taps in the effect's region
    double visible = 0.0; // region taps whose change is above kVisibleLevels
    double sum = 0.0;     // the sum of the change over the region taps
    constexpr bool HaveRegion() const noexcept { return region > 0.0; }
    constexpr double Share() const noexcept { return region > 0.0 ? visible / region : 0.0; }
    constexpr double Mean() const noexcept { return region > 0.0 ? sum / region : 0.0; }
    constexpr double Coverage() const noexcept { return taps > 0.0 ? region / taps : 0.0; }
    constexpr void Add(const Totals& o) noexcept
    {
        taps += o.taps;
        region += o.region;
        visible += o.visible;
        sum += o.sum;
    }
};

// One texel into the totals (the shader's loop body).
constexpr void Accumulate(Totals& t, const float in[4], const float out[4], const Params& p) noexcept
{
    t.taps += 1.0;
    const float change = ChangeLevels(in, out, p.channels, p.unit, p.white);
    if (!(change >= 0.0f))
        return;
    if (!InRegion(p.region, in, out, p.channels, p.unit, p.white))
        return;
    t.region += 1.0;
    t.sum += double(change);
    if (IsVisible(change))
        t.visible += 1.0;
}

// The CPU references (tests and acceptance scripts; the product never runs them). `fetch(x, y, in, out)` fills the texel at box-relative (x, y).
// ReferenceTotals visits EVERY texel of the box; TapTotals visits exactly the taps the shader does (same geometry, same hash, same seed), so a GPU result
// equals it up to float rounding.
template <class Fetch> Totals ReferenceTotals(uint32_t width, uint32_t height, const Params& p, Fetch&& fetch)
{
    Totals t;
    float in[4], out[4];
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            fetch(x, y, in, out);
            Accumulate(t, in, out, p);
        }
    return t;
}

template <class Fetch> Totals TapTotals(uint32_t width, uint32_t height, const Geometry& g, uint32_t seed, const Params& p, Fetch&& fetch)
{
    Totals t;
    float in[4], out[4];
    for (uint32_t by = 0; by < g.binsY; ++by)
        for (uint32_t bx = 0; bx < g.binsX; ++bx)
            for (uint32_t k = 0; k < g.Taps(); ++k)
            {
                const TapOffset o = TapOf(g, bx, by, k, seed);
                const uint32_t x = bx * g.block + o.x, y = by * g.block + o.y;
                if (x >= width || y >= height)
                    continue;
                fetch(x, y, in, out);
                Accumulate(t, in, out, p);
            }
    return t;
}

// The read-back: the target's float4 texels (region, visible, sum, taps) added in doubles.
inline Totals Reduce(const float* texels, uint32_t binsX, uint32_t binsY, uint32_t rowFloats)
{
    Totals t;
    for (uint32_t y = 0; y < binsY; ++y)
    {
        const float* row = texels + size_t(y) * rowFloats;
        for (uint32_t x = 0; x < binsX; ++x)
        {
            t.region += double(row[x * 4 + 0]);
            t.visible += double(row[x * 4 + 1]);
            t.sum += double(row[x * 4 + 2]);
            t.taps += double(row[x * 4 + 3]);
        }
    }
    return t;
}

// ---- the sampling schedule -------------------------------------------------------------------------------------------------------------------------

inline constexpr uint64_t kSampleMs = 3000;       // one sample of each effect about every three seconds
inline constexpr uint64_t kStaggerMs = 250;       // the effects' first samples are spread so a sampled frame carries one effect
inline constexpr uint64_t kEffectLogMs = 10000;   // one line per effect about every ten seconds
inline constexpr uint64_t kSummaryFirstMs = 60000;  // the summary: sixty seconds after the first rendered frame ...
inline constexpr uint64_t kSummaryRepeatMs = 300000; // ... and then every five minutes
inline constexpr uint64_t kMaxPendingMs = 2000;   // a sample whose result has not landed in this long is dropped (the next one may start)

class Schedule
{
  public:
    // Whether the effect should be sampled now, on this rendered frame. `token` is the frame; at most one sample starts in a frame, and none while a sample is
    // in flight. The first call for an effect fixes its first due time (the start plus its place in the stagger), so the calls of one frame never all say yes.
    bool Due(StageId id, uint64_t nowMs, uint64_t token) noexcept
    {
        const uint32_t i = uint32_t(id);
        if (i >= kEffectCount)
            return false;
        if (!_started)
        {
            _started = true;
            _startMs = nowMs;
        }
        if (_haveFrame && token == _lastSampleToken)
            return false;
        if (_inFlight)
        {
            if (nowMs - _flightMs < kMaxPendingMs)
                return false;
            _inFlight = false; // the result never landed: give up on it
        }
        if (!_have[i])
        {
            _have[i] = true;
            _dueMs[i] = _startMs + uint64_t(i) * kStaggerMs;
        }
        return nowMs >= _dueMs[i];
    }

    // The sample was started: the next of this effect is a period later, and nothing else starts in this frame or until the result is collected.
    void Started(StageId id, uint64_t nowMs, uint64_t token) noexcept
    {
        const uint32_t i = uint32_t(id);
        if (i >= kEffectCount)
            return;
        _have[i] = true;
        _dueMs[i] = nowMs + kSampleMs;
        _inFlight = true;
        _flightMs = nowMs;
        _haveFrame = true;
        _lastSampleToken = token;
    }

    // The sample could not start (a view the pass cannot read, a window outside the texture): the effect is asked again a period later, not on the next frame.
    void Declined(StageId id, uint64_t nowMs) noexcept
    {
        const uint32_t i = uint32_t(id);
        if (i < kEffectCount)
        {
            _have[i] = true;
            _dueMs[i] = nowMs + kSampleMs;
        }
    }

    void Collected() noexcept { _inFlight = false; }
    bool InFlight() const noexcept { return _inFlight; }
    void Reset() noexcept { *this = Schedule {}; }

  private:
    bool _started = false;
    uint64_t _startMs = 0;
    bool _have[kEffectCount] {};
    uint64_t _dueMs[kEffectCount] {};
    bool _inFlight = false;
    uint64_t _flightMs = 0;
    bool _haveFrame = false;
    uint64_t _lastSampleToken = 0;
};

// ---- the per-frame fold ----------------------------------------------------------------------------------------------------------------------------

// A skip reason as the meter keys it: the stage's own wording with every run of digits (and a decimal point inside it) folded into '#', lower-cased and cut
// to kReasonChars, so "weight 0 (night curve 0.081, moon)" and "weight 0 (night curve 0.185, sun)" are one reason and a std::format'd text is never a new key.
inline constexpr uint32_t kReasonChars = 56;
inline constexpr uint32_t kReasonSlots = 10; // distinct reasons kept per effect and window; the rest count as "other"

inline std::string ReasonKey(std::string_view text)
{
    std::string key;
    key.reserve(std::min<size_t>(text.size(), kReasonChars));
    bool inNumber = false;
    for (char ch : text)
    {
        if (key.size() >= kReasonChars)
            break;
        const bool digit = ch >= '0' && ch <= '9';
        if (digit)
        {
            if (!inNumber)
                key += '#';
            inNumber = true;
            continue;
        }
        if (inNumber && ch == '.')
            continue; // the point inside a number
        inNumber = false;
        key += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch;
    }
    return key;
}

struct ReasonCount
{
    char key[kReasonChars + 1] {};
    uint64_t count = 0;
};

// One effect over one window: frames it was reached in, frames it ran in, frames it was skipped in (by reason).
struct Tally
{
    uint64_t reached = 0, ran = 0, skipped = 0;
    uint64_t other = 0; // skipped frames whose reason did not fit a slot
    ReasonCount reasons[kReasonSlots] {};

    void AddReason(std::string_view key, uint64_t count)
    {
        const size_t n = std::min<size_t>(key.size(), kReasonChars);
        for (ReasonCount& slot : reasons)
            if (slot.count != 0 && std::string_view(slot.key) == key.substr(0, n))
            {
                slot.count += count;
                return;
            }
        for (ReasonCount& slot : reasons)
            if (slot.count == 0)
            {
                std::memcpy(slot.key, key.data(), n);
                slot.key[n] = '\0';
                slot.count = count;
                return;
            }
        other += count;
    }

    void NoteSkippedFrame(std::string_view reason)
    {
        ++skipped;
        AddReason(ReasonKey(reason), 1);
    }

    void Add(const Tally& o)
    {
        reached += o.reached;
        ran += o.ran;
        skipped += o.skipped;
        other += o.other;
        for (const ReasonCount& slot : o.reasons)
            if (slot.count != 0)
                AddReason(slot.key, slot.count);
    }
};

// What one effect did in the frame being counted: the strongest event wins (ran beats skipped), the first skip reason is the frame's reason.
struct FrameState
{
    bool reached = false, ran = false, skipped = false;
    char reason[kReasonChars + 1] {};
};

// The meter's frame book. A "rendered frame" is a present interval (a token) in which some effect was reached; events carry the present counter's token. The
// frame being counted folds into the window's tallies when an event of a later token arrives (or when a line is due), so the counts are per FRAME, whatever
// number of draws of an effect a frame holds (the occlusion's readers, the water draws).
class Book
{
  public:
    void NoteRan(StageId id, uint64_t token)
    {
        if (!Advance(token) || uint32_t(id) >= kEffectCount)
            return;
        FrameState& s = _now[uint32_t(id)];
        s.reached = true;
        s.ran = true;
    }

    void NoteSkip(StageId id, uint64_t token, std::string_view reason)
    {
        if (!Advance(token) || uint32_t(id) >= kEffectCount)
            return;
        FrameState& s = _now[uint32_t(id)];
        s.reached = true;
        if (!s.skipped)
        {
            s.skipped = true;
            const size_t n = std::min<size_t>(reason.size(), kReasonChars);
            std::memcpy(s.reason, reason.data(), n);
            s.reason[n] = '\0';
        }
    }

    // Folds the frame being counted (a line is due, or the summary): the next event of the same token starts it again.
    void FoldNow()
    {
        if (!_haveToken)
            return;
        Fold();
        _haveToken = false;
        _token = 0;
    }

    uint64_t WindowFrames() const noexcept { return _windowFrames; }
    uint64_t TotalFrames() const noexcept { return _totalFrames + _windowFrames; }
    const Tally& Window(StageId id) const noexcept { return _window[uint32_t(id)]; }
    // The whole session so far: the closed windows and the one being counted.
    Tally Total(StageId id) const
    {
        Tally t = _total[uint32_t(id)];
        t.Add(_window[uint32_t(id)]);
        return t;
    }
    bool Started() const noexcept { return _totalFrames + _windowFrames != 0; }

    // The window ends: its tallies join the totals and a new window starts.
    void CloseWindow()
    {
        for (uint32_t i = 0; i < kEffectCount; ++i)
        {
            _total[i].Add(_window[i]);
            _window[i] = Tally {};
        }
        _totalFrames += _windowFrames;
        _windowFrames = 0;
    }

    void Reset() noexcept { *this = Book {}; }

  private:
    // True when the event belongs to a frame that is still being counted; moving to a later token folds the one before it.
    bool Advance(uint64_t token)
    {
        if (_haveToken && token == _token)
            return true;
        if (_haveToken && token < _token)
            return false; // an older token: a stale event of an earlier frame, not counted
        if (_haveToken)
            Fold();
        _haveToken = true;
        _token = token;
        return true;
    }

    void Fold()
    {
        bool any = false;
        for (uint32_t i = 0; i < kEffectCount; ++i)
        {
            FrameState& s = _now[i];
            if (!s.reached)
                continue;
            any = true;
            Tally& t = _window[i];
            ++t.reached;
            if (s.ran)
                ++t.ran;
            else if (s.skipped)
                t.NoteSkippedFrame(s.reason);
            s = FrameState {};
        }
        if (any)
            ++_windowFrames;
    }

    bool _haveToken = false;
    uint64_t _token = 0;
    FrameState _now[kEffectCount] {};
    Tally _window[kEffectCount] {};
    Tally _total[kEffectCount] {};
    uint64_t _windowFrames = 0;
    uint64_t _totalFrames = 0;
};

// ---- the log's clock -------------------------------------------------------------------------------------------------------------------------------

// The effects' lines are written together about every ten seconds; the summary block 60 s after the first rendered frame and then every five minutes. The
// clock is a number of milliseconds from any monotonic source, handed in by the caller (GetTickCount64 in the game).
class Cadence
{
  public:
    // The first rendered frame starts both clocks; a clock that went backwards starts them again.
    void Tick(uint64_t nowMs) noexcept
    {
        if (!_started || nowMs < _windowStartMs)
        {
            _started = true;
            _firstMs = nowMs;
            _windowStartMs = nowMs;
            _summaryDueMs = nowMs + kSummaryFirstMs;
        }
    }
    bool Started() const noexcept { return _started; }
    bool WindowDue(uint64_t nowMs) const noexcept { return _started && nowMs - _windowStartMs >= kEffectLogMs; }
    uint64_t WindowMs(uint64_t nowMs) const noexcept { return nowMs >= _windowStartMs ? nowMs - _windowStartMs : 0; }
    void TookWindow(uint64_t nowMs) noexcept { _windowStartMs = nowMs; }
    bool SummaryDue(uint64_t nowMs) const noexcept { return _started && nowMs >= _summaryDueMs; }
    void TookSummary(uint64_t nowMs) noexcept
    {
        ++_summaries;
        _summaryDueMs = nowMs + kSummaryRepeatMs;
    }
    uint32_t Summaries() const noexcept { return _summaries; }
    void Reset() noexcept { *this = Cadence {}; }

  private:
    bool _started = false;
    uint64_t _firstMs = 0;
    uint64_t _windowStartMs = 0;
    uint64_t _summaryDueMs = 0;
    uint32_t _summaries = 0;
};

// ---- wording ---------------------------------------------------------------------------------------------------------------------------------------

constexpr double Percent(double part, double whole) noexcept { return whole > 0.0 ? 100.0 * part / whole : 0.0; }

// What the latest finished sample of an effect said.
struct Sample
{
    bool have = false;
    bool haveRegion = false;
    double share = 0.0;    // 0..1
    double mean = 0.0;     // levels of 255
    double coverage = 0.0; // 0..1 of the box
    uint32_t count = 0;    // samples taken so far
};

inline std::string FormatReasons(const Tally& t)
{
    std::string list;
    // the three biggest reasons, in order
    const ReasonCount* chosen[3] {};
    for (const ReasonCount& slot : t.reasons)
    {
        if (slot.count == 0)
            continue;
        for (uint32_t k = 0; k < 3; ++k)
        {
            if (chosen[k] == nullptr || slot.count > chosen[k]->count)
            {
                for (uint32_t m = 2; m > k; --m)
                    chosen[m] = chosen[m - 1];
                chosen[k] = &slot;
                break;
            }
        }
    }
    for (const ReasonCount* c : chosen)
    {
        if (c == nullptr)
            continue;
        if (!list.empty())
            list += "; ";
        list += std::format("{} {}", c->count, c->key);
    }
    if (t.other != 0)
        list += std::format("{}{} other", list.empty() ? "" : "; ", t.other);
    return list.empty() ? std::string() : " (" + list + ")";
}

// The per-effect line, about every ten seconds: frames reached / ran / skipped by reason, the visible share and the mean of the latest sample.
inline std::string FormatEffectLine(StageId id, uint64_t windowMs, uint64_t frames, const Tally& t, const Sample& s)
{
    const EffectInfo& info = InfoOf(id);
    std::string seen;
    if (!s.have)
        seen = "no visibility sample yet";
    else if (!s.haveRegion)
        seen = std::format("last sample: nothing in its region ({}), {} samples", info.regionText, s.count);
    else
        seen = std::format("last sample: {:.1f} % of its region ({}, {:.1f} % of the window) changed by > {:.0f}/255, mean {:.2f}/255, {} samples",
                           s.share * 100.0, info.regionText, s.coverage * 100.0, double(kVisibleLevels), s.mean, s.count);
    return std::format("Effect meter: {}: in the last {:.1f} s ({} rendered frames) reached {}, ran {}, skipped {}{}; {}", info.name, double(windowMs) / 1000.0, frames,
                       t.reached, t.ran, t.skipped, FormatReasons(t), seen);
}

// The summary block's line, one per effect (the wording the brief fixes).
inline std::string FormatSummaryLine(StageId id, uint64_t frames, const Tally& t, const Sample& s)
{
    const EffectInfo& info = InfoOf(id);
    const double ran = Percent(double(t.ran), double(frames));
    if (!s.have)
        return std::format("Effect visibility: {} ran {:.0f} % of frames, no visibility sample yet", info.name, ran);
    if (!s.haveRegion)
        return std::format("Effect visibility: {} ran {:.0f} % of frames, nothing in its region at the last sample", info.name, ran);
    return std::format("Effect visibility: {} ran {:.0f} % of frames, changed {:.0f} % of its region by > {:.0f}/255 (mean {:.1f}/255)", info.name, ran,
                       s.share * 100.0, double(kVisibleLevels), s.mean);
}

// The menu card's line: "ran X %, visible Y %", live.
inline std::string FormatCard(bool armed, uint64_t frames, const Tally& t, const Sample& s)
{
    if (!armed)
        return "Meter: off";
    if (frames == 0 || t.reached == 0)
        return "Meter: not reached yet";
    const double ran = Percent(double(t.ran), double(frames));
    if (!s.have)
        return std::format("Meter: ran {:.0f} %, visible not measured yet", ran);
    if (!s.haveRegion)
        return std::format("Meter: ran {:.0f} %, visible n/a (nothing in its region)", ran);
    return std::format("Meter: ran {:.0f} %, visible {:.0f} % (mean {:.1f}/255)", ran, s.share * 100.0, s.mean);
}

} // namespace ext::ffxiv::meter
