#pragma once
// Atmosphere: the timed counts line's pure rules (INT2). No pch, no Windows, D3D or log call, so every decision here is covered by tests/ext/ffxiv/engine on
// the host (atmo_counts_rules); the wiring in AtmoDraw.cpp is pinned as text (atmo_counts_pins).
//
// Why the stage needs one. A session check says "by day the atmosphere's run count keeps rising, frame after frame", and until now nothing in the log could
// say so: the run line is a state-change line (one per scene size), and the card's counts line (FormatCard, about once a minute) was written only from the owner
// role and the stage's own seam entry - never from the chained entry, which is the entry the stage is reached through whenever the light bounce, the march or
// the night grade has the chain (the night grade is on by default, and runs every night). This line is written about every ten seconds from every entry plus
// the once-an-output-frame heartbeat, so a stage that is armed but never reached writes a line that says "reached 0 times".
//
// What it counts. Per window (since the last line) and in total (since the stage started):
//   reached - the stage was asked at a tone map and had its one answer for the frame available (the one-answer gate let it in), per ENTRY;
//   ran     - the pass answered, per entry;
//   declined - the frame was left alone and counted against the stage's stand-down streak, per REASON;
//   skipped  - the frame was left alone without counting against the stage (no depth yet, no camera, no fog state), per reason.
// An entry is the hook the stage was reached by, never the caller: the stage cannot know whether the light bounce, the night grade or the march handed it the
// chain (and must not: the chain's order is written only in ChainHooks.h). Own seam = the engine pass dispatch reached the stage's own seam entry; chained
// hook = the texture hook (chainAtmoScene), the entry behind every stage in front of it that answers; owner hook = the owner hook (toneMapAtmoOwner), the
// entry behind the stage in front of it when that one does not draw.

#include "ext/ffxiv/engine/AtmoRules.h"

#include <cstdint>
#include <format>
#include <string>

namespace ext::ffxiv::engine::atmo
{
enum class Entry : uint8_t
{
    Seam = 0, // the engine pass dispatch -> atmo::OnSeamDraw
    Chain,    // chainAtmoScene (ChainScene)
    Owner,    // toneMapAtmoOwner (ToneMapOwner)
};

inline constexpr uint32_t kEntryCount = 3;

constexpr const char* EntryName(Entry entry) noexcept
{
    switch (entry)
    {
    case Entry::Seam: return "own seam";
    case Entry::Chain: return "chained hook";
    case Entry::Owner: return "owner hook";
    }
    return "unknown";
}

// About ten seconds of wall clock between two lines (the session check reads them one after the other).
inline constexpr uint64_t kCountsLogMs = 10000;

inline constexpr uint32_t kDeclineReasonCount = uint32_t(Decline::Exception) + 1;
inline constexpr uint32_t kSkipReasonCount = 3; // SkipReason::NoDepth, NoCamera, NoFog

// The short name a decline carries in the counts line (DeclineName is a sentence for the per-change line; this is a key in a list).
constexpr const char* DeclineKey(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "key off";
    case Decline::NotToneMap: return "not the tone map";
    case Decline::RenderTarget: return "render target";
    case Decline::TargetFormat: return "target format";
    case Decline::SceneFormat: return "scene format";
    case Decline::SizeMismatch: return "size mismatch";
    case Decline::Window: return "window";
    case Decline::CharaReplay: return "character preview";
    case Decline::SceneHdr: return "scene HDR";
    case Decline::NoDepth: return "no depth";
    case Decline::NoCamera: return "no camera";
    case Decline::NoFog: return "no fog state";
    case Decline::PassFailed: return "pass failed";
    case Decline::Exception: return "exception";
    }
    return "unknown";
}

constexpr const char* SkipKey(SkipReason reason) noexcept
{
    switch (reason)
    {
    case SkipReason::None: return "none";
    case SkipReason::NoDepth: return "depth";
    case SkipReason::NoCamera: return "camera";
    case SkipReason::NoFog: return "fog";
    }
    return "unknown";
}

// One window's (or the whole session's) tally.
struct Tally
{
    uint64_t reached[kEntryCount] {};
    uint64_t ran[kEntryCount] {};
    uint64_t declined[kDeclineReasonCount] {};
    uint64_t skipped[kSkipReasonCount] {};

    constexpr uint64_t Reached() const noexcept
    {
        uint64_t sum = 0;
        for (uint64_t value : reached)
            sum += value;
        return sum;
    }
    constexpr uint64_t Ran() const noexcept
    {
        uint64_t sum = 0;
        for (uint64_t value : ran)
            sum += value;
        return sum;
    }
    constexpr uint64_t Declined() const noexcept
    {
        uint64_t sum = 0;
        for (uint64_t value : declined)
            sum += value;
        return sum;
    }
    constexpr uint64_t Skipped() const noexcept
    {
        uint64_t sum = 0;
        for (uint64_t value : skipped)
            sum += value;
        return sum;
    }
    constexpr void Add(const Tally& other) noexcept
    {
        for (uint32_t i = 0; i < kEntryCount; ++i)
        {
            reached[i] += other.reached[i];
            ran[i] += other.ran[i];
        }
        for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
            declined[i] += other.declined[i];
        for (uint32_t i = 0; i < kSkipReasonCount; ++i)
            skipped[i] += other.skipped[i];
    }
};

// "own seam 0, chained hook 12, owner hook 0".
inline std::string FormatByEntry(const uint64_t (&values)[kEntryCount])
{
    return std::format("{} {}, {} {}, {} {}", EntryName(Entry::Seam), values[0], EntryName(Entry::Chain), values[1], EntryName(Entry::Owner), values[2]);
}

// " (scene HDR 3, no camera 1)" for the reasons that happened, "" for none.
inline std::string FormatDeclines(const Tally& tally)
{
    std::string list;
    for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        if (tally.declined[i] != 0)
        {
            if (!list.empty())
                list += ", ";
            list += std::format("{} {}", DeclineKey(Decline(i)), tally.declined[i]);
        }
    return list.empty() ? std::string() : " (" + list + ")";
}

inline std::string FormatSkips(const Tally& tally)
{
    std::string list;
    for (uint32_t i = 0; i < kSkipReasonCount; ++i)
        if (tally.skipped[i] != 0)
        {
            if (!list.empty())
                list += ", ";
            list += std::format("{} {}", SkipKey(SkipReason(i + 1)), tally.skipped[i]);
        }
    return list.empty() ? std::string() : " (" + list + ")";
}

// The window's tally and the session's total on one line: the first half is what the last ten seconds did, the second half is the running total (so "keeps
// rising" is readable from one line to the next, and a stage that stopped shows a window of zeros under a total that does not move). The settings at the end
// are the card's, so a slider tweak shows up in the log.
inline std::string FormatTimedCounts(const Tally& window, const Tally& total, uint64_t windowMs, uint64_t windowFrames, uint64_t frames, const Params& params)
{
    const double seconds = double(windowMs) / 1000.0;
    return std::format("Atmosphere: in the last {:.1f} s ({} rendered frames) reached {} times ({}), ran {} times ({}), declined {}{}, skipped {}{}; "
                       "since the start: {} frames, reached {}, ran {}, declined {}, skipped {}; density {:.2f}/km, falloff {:.4f}/m, start {:.0f} m, g {:.2f}, sun {:.2f}, sky {:.2f}, night {:.2f}, neutral {:.2f}",
                       seconds, windowFrames, window.Reached(), FormatByEntry(window.reached), window.Ran(), FormatByEntry(window.ran), window.Declined(),
                       FormatDeclines(window), window.Skipped(), FormatSkips(window), frames, total.Reached(), total.Ran(), total.Declined(), total.Skipped(),
                       double(params.density), double(params.falloff), double(params.start), double(params.phaseG), double(params.sunStrength),
                       double(params.skyStrength), double(params.nightDensity), double(params.neutral));
}

// The timed counts: the window being counted, the total of the windows already written, and the clock the next line is due on. The clock is a number of
// milliseconds from any monotonic source (GetTickCount64 in the game), handed in by the caller so the rules need no clock of their own.
class TimedCounts
{
  public:
    void NoteReached(Entry entry) noexcept { ++_window.reached[uint32_t(entry)]; }
    void NoteRan(Entry entry) noexcept { ++_window.ran[uint32_t(entry)]; }
    void NoteDeclined(Decline reason) noexcept
    {
        if (uint32_t(reason) < kDeclineReasonCount)
            ++_window.declined[uint32_t(reason)];
    }
    void NoteSkipped(SkipReason reason) noexcept
    {
        const uint32_t index = uint32_t(reason);
        if (index >= 1 && index <= kSkipReasonCount)
            ++_window.skipped[index - 1];
    }

    // True when a line is due at `nowMs`. The first call only starts the window (no line at the start of a session, and the window never includes time before
    // the stage was first looked at); a clock that went backwards starts the window again instead of writing a line.
    bool Due(uint64_t nowMs, uint64_t frames) noexcept
    {
        if (!_started || nowMs < _windowStartMs)
        {
            _started = true;
            _windowStartMs = nowMs;
            _windowStartFrames = frames;
            return false;
        }
        return nowMs - _windowStartMs >= kCountsLogMs;
    }

    // The line for the window that ends at `nowMs`, and the next window starts there. Call only when Due() said so.
    std::string TakeLine(uint64_t nowMs, uint64_t frames, const Params& params)
    {
        _total.Add(_window);
        const uint64_t windowMs = nowMs >= _windowStartMs ? nowMs - _windowStartMs : 0;
        const uint64_t windowFrames = frames >= _windowStartFrames ? frames - _windowStartFrames : 0;
        std::string line = FormatTimedCounts(_window, _total, windowMs, windowFrames, frames, params);
        _window = Tally {};
        _windowStartMs = nowMs;
        _windowStartFrames = frames;
        return line;
    }

    void Reset() noexcept { *this = TimedCounts {}; }

    const Tally& Window() const noexcept { return _window; }
    const Tally& Total() const noexcept { return _total; }

  private:
    Tally _window;
    Tally _total; // the windows already written; the line adds the window being written
    bool _started = false;
    uint64_t _windowStartMs = 0;
    uint64_t _windowStartFrames = 0;
};
} // namespace ext::ffxiv::engine::atmo
