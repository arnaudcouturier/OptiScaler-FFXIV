#pragma once
// Diagnostics of preview activity that involves more than one preview image: pure counters with the clock and the game frame
// number passed in, so every rule is tested without a device.
//
// One burst of preview activity owns one instance: the burst bookkeeping in CharaIdentify clears it when a burst starts and
// writes it when the burst ends. What is counted, all per burst:
//   * per output entry (0..7, anything else in one bucket): accepted finishers, finisher checks that failed, evaluations, UI
//     draws that were given the processed image, failures of the entry's slot, and what the upscaler step did with the
//     finishers of that entry;
//   * two accepted finishers of one preview image in one game frame (two windows that share one output texture);
//   * the output entries seen and the peak number of preview images a UI draw sampled within 250 ms;
//   * renders per game frame (1, 2, 3, 4 or more), taken from the pass starts;
//   * the reasons the signed-motion replay refused a draw, and the first few untagged G-buffer shapes it did not recognise
//     (shader, constant buffer sizes, render target entries), which say whether a render's motion was replayed at all;
//   * how the passes were named (a processed window, a window outside the set, a camera block that was never learned, an ambiguous one)
//     and what the finishers taught (a new camera block, an ambiguity that ended), which says whether the identity works at all.
//
// The numbers are for the log only: nothing here changes what the feature does.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ext/ffxiv/chara/CharaDlssRules.h"   // FinishVerdict
#include "ext/ffxiv/chara/CharaMotionRules.h" // Skip
#include "ext/ffxiv/chara/CharaNrRules.h"     // kMaxStreams (the preview NR part of the burst line)
#include "ext/ffxiv/chara/CharaWindowSet.h"   // Name (the identity part of the burst line)

namespace ext::ffxiv::chara
{
inline constexpr size_t kWindowEntries = 8;    // the preview renderer's output entries 0..7
inline constexpr size_t kWindowOtherEntry = 8; // the bucket of a draw whose target is not an output entry
inline constexpr size_t kWindowBuckets = kWindowEntries + 1;
inline constexpr size_t kRendersPerFrame = 4;  // 1, 2, 3, 4 or more renders in one game frame
inline constexpr size_t kSampledImages = 16;   // preview images remembered for the "sampled within 250 ms" peak
inline constexpr uint64_t kSampledWindowMs = 250;
inline constexpr size_t kUnknownShapes = 4;    // distinct untagged G-buffer shapes recorded per burst
inline constexpr size_t kReplaySkips = 12;     // one counter per motion::Skip value
inline constexpr size_t kIdentityNames = 5;    // one counter per WindowSet::Name value
inline constexpr size_t kWindowVerdicts = static_cast<size_t>(dlss::FinishVerdict::WaitingForCamera) + 1;

static_assert(static_cast<size_t>(motion::Skip::DepthStateFailed) + 1 == kReplaySkips, "a motion::Skip value has no counter here");
static_assert(static_cast<size_t>(WindowSet::Name::Invalid) + 1 == kIdentityNames, "a WindowSet::Name value has no counter here");

// What happened to one output entry in this burst.
struct WindowEntryCounts
{
    uint64_t finishers = 0;      // accepted finishers into this entry
    uint64_t rejected = 0;       // finisher checks that failed for a draw into this entry
    uint64_t evaluated = 0;      // successful evaluations of this entry's preview
    uint64_t processedDraws = 0; // UI draws that sampled this entry and were given the processed image
    uint64_t failures = 0;       // failures of this entry's slot
    std::array<uint64_t, kWindowVerdicts> verdicts {}; // what the upscaler step did with the finishers of this entry
};

// The preview NR side of one burst: what happened since the previous burst and what exists now. Filled from nr::Status at the burst end and
// formatted by NrBurstText, so the line is tested without a device.
struct NrBurstCounts
{
    uint64_t runs = 0;            // successful runs in this burst
    uint64_t skipTableFull = 0;   // of the calls that kept the image: the stream table was full
    uint64_t skipSettling = 0;    // ... the wanted stream shape was still settling
    uint64_t skipNoList = 0;      // ... no command list or allocator was free
    uint32_t streamsPeak = 0;     // the most streams that existed at once, since the module started
    uint32_t streamsNow = 0;      // streams that exist now
    std::array<int, nr::kMaxStreams> entries = None();   // each stream's output entry, -1 for a stream that does not exist
    std::array<uint64_t, nr::kMaxStreams> streamRuns {}; // runs of each stream in this burst

  private:
    static std::array<int, nr::kMaxStreams> None()
    {
        std::array<int, nr::kMaxStreams> entries {};
        entries.fill(-1);
        return entries;
    }
};

// "3 runs, skipped: full table 0, settling 1, no command list 0; streams peak 2 of 16, now 2; per stream: stream 0 (output entry 1) 3 runs"
inline std::string NrBurstText(const NrBurstCounts& c)
{
    std::string text = std::to_string(c.runs) + " runs, skipped: full table " + std::to_string(c.skipTableFull) + ", settling " +
                       std::to_string(c.skipSettling) + ", no command list " + std::to_string(c.skipNoList) + "; streams peak " +
                       std::to_string(c.streamsPeak) + " of " + std::to_string(nr::kMaxStreams) + ", now " + std::to_string(c.streamsNow);
    std::string perStream;
    for (size_t i = 0; i < c.entries.size(); ++i)
    {
        if (c.entries[i] < 0)
            continue;
        perStream += std::string(perStream.empty() ? "" : ", ") + "stream " + std::to_string(i) + " (output entry " +
                     std::to_string(c.entries[i]) + ") " + std::to_string(c.streamRuns[i]) + " runs";
    }
    return text + "; per stream: " + (perStream.empty() ? std::string("none") : perStream);
}

// Whether the NR part of the burst line says anything: a run, a call that was skipped, or a stream that exists or existed in this burst. With
// preview NR off the line would only say "0 runs" at every burst end.
inline bool NrBurstWorthLogging(const NrBurstCounts& c)
{
    if (c.runs != 0 || c.skipTableFull != 0 || c.skipSettling != 0 || c.skipNoList != 0 || c.streamsNow != 0)
        return true;
    for (size_t i = 0; i < c.entries.size(); ++i)
    {
        if (c.entries[i] >= 0 || c.streamRuns[i] != 0)
            return true;
    }
    return false;
}

// The identity side of one burst: how the passes were named and what the finishers taught. Filled from WindowStats at the burst end and
// formatted by IdentityBurstText, so the line is tested without a device.
struct IdentityCounts
{
    uint64_t named = 0;     // passes named as a processed window (followed)
    uint64_t outside = 0;   // passes named as a window outside the set (left native)
    uint64_t unknown = 0;   // passes whose camera block binding no finisher has taught yet (left native)
    uint64_t ambiguous = 0; // passes whose camera block two windows drew with (left native)
    uint64_t invalid = 0;   // passes with nothing that can hold the camera block bound at their own stage (left native)
    uint64_t learned = 0;   // camera blocks learned from a finisher in this burst
    uint64_t cleared = 0;   // ambiguities that ended in this burst
    uint64_t mismatch = 0;  // passes whose finisher drew another image than the binding named (the binding cannot tell the two apart)
    uint32_t known = 0;     // camera blocks that name a window now
};

// "3 passes named for a processed window, 1 named for one outside the set, 2 not learned yet, 0 ambiguous, 0 with no camera block bound; 2
// camera blocks learned, 1 ambiguity ended, 3 known now, 0 mis-named"
inline std::string IdentityBurstText(const IdentityCounts& c)
{
    return std::to_string(c.named) + " passes named for a processed window, " + std::to_string(c.outside) +
           " named for one outside the set, " + std::to_string(c.unknown) + " not learned yet, " + std::to_string(c.ambiguous) +
           " ambiguous, " + std::to_string(c.invalid) + " with no camera block bound; " + std::to_string(c.learned) +
           " camera blocks learned, " + std::to_string(c.cleared) + " ambiguity ended, " + std::to_string(c.known) + " known now, " +
           std::to_string(c.mismatch) + " mis-named";
}

// One untagged G-buffer draw the signed-motion replay refused. The constant buffer sizes and the render target entries are what
// tells a G-buffer draw of another shader from a draw of the preview renderer whose camera block has another size.
struct UnknownShape
{
    uint32_t crc = 0;         // pixel shader CRC (0: the shader was created before the hooks, so its CRC is not known)
    uint32_t vsCb0Bytes = 0;  // byte width of vertex-shader constant buffer 0
    uint32_t psCb2Bytes = 0;  // byte width of pixel-shader constant buffer 2
    std::array<int, 5> rtvEntry { -1, -1, -1, -1, -1 }; // table entry of render targets 0..4
};

class WindowStats
{
  public:
    // The bucket an output entry is counted in: -1 and 8 or more are not outputs of the preview renderer.
    static constexpr size_t Bucket(int entry)
    {
        return entry >= 0 && entry < static_cast<int>(kWindowEntries) ? static_cast<size_t>(entry) : kWindowOtherEntry;
    }

    // ---- Finishers and the collision detector ----
    // A finisher draw of the preview image `key` in game frame `frame`; `accepted` is the finisher check's answer. Returns true
    // when this is the second accepted finisher of the same image in the same game frame: two windows then share one output
    // texture, and their upscaler histories would mix. A frame number of 0 (no frame callback) never counts as a collision.
    bool NoteFinisher(int entry, bool accepted, uintptr_t key, uint64_t frame)
    {
        WindowEntryCounts& e = _entries[Bucket(entry)];
        if (!accepted)
        {
            ++e.rejected;
            return false;
        }
        ++e.finishers;
        if (entry >= 0 && entry < static_cast<int>(kWindowEntries))
            _entriesSeen[static_cast<size_t>(entry)] = true;
        const bool collision = _haveLast && frame != 0 && _lastFrame != 0 && key != 0 && key == _lastKey && frame == _lastFrame;
        if (collision)
            ++_collisions;
        _lastKey = key;
        _lastFrame = frame;
        _haveLast = true;
        _lastCollided = collision;
        return collision;
    }

    // What the upscaler step decided for the finisher noted last (true: the two-window collision above). The caller that resets
    // an image's history asks right after the dispatcher noted the finisher, before the upscaler step of that render runs.
    bool LastFinisherCollided() const { return _lastCollided; }
    uint64_t Collisions() const { return _collisions; }

    void NoteFinishVerdict(int entry, dlss::FinishVerdict verdict)
    {
        const size_t i = static_cast<size_t>(verdict);
        if (i < kWindowVerdicts)
            ++_entries[Bucket(entry)].verdicts[i];
    }

    void NoteEvaluated(int entry) { ++_entries[Bucket(entry)].evaluated; }
    void NoteProcessedDraw(int entry) { ++_entries[Bucket(entry)].processedDraws; }

    // A failure of the slot `slot` that belongs to `entry` (feeds the decision about the scope of the session-wide failure latch).
    void NoteSrFailure(int slot, int entry)
    {
        ++_entries[Bucket(entry)].failures;
        if (slot >= 0 && static_cast<size_t>(slot) < _slotFailures.size())
            ++_slotFailures[static_cast<size_t>(slot)];
    }
    uint64_t SlotFailures(int slot) const
    {
        return slot >= 0 && static_cast<size_t>(slot) < _slotFailures.size() ? _slotFailures[static_cast<size_t>(slot)] : 0;
    }

    // ---- Several images sampled by UI draws ----
    // A UI draw sampled the preview image `key` of `entry`: remembers it for the 250 ms peak, whatever the other options say.
    void NoteSample(uintptr_t key, int entry, uint64_t now)
    {
        if (key == 0)
            return;
        if (entry >= 0 && entry < static_cast<int>(kWindowEntries))
            _entriesSeen[static_cast<size_t>(entry)] = true;
        size_t target = 0;
        bool found = false;
        for (size_t i = 0; i < _sampled.size(); ++i)
        {
            if (_sampled[i].key == key)
            {
                target = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            // The oldest entry goes: the peak below only looks at the last 250 ms anyway.
            target = 0;
            for (size_t i = 1; i < _sampled.size(); ++i)
            {
                if (_sampled[i].tick < _sampled[target].tick)
                    target = i;
            }
        }
        _sampled[target] = { key, now };
        size_t live = 0;
        for (const Sampled& s : _sampled)
        {
            if (s.key != 0 && now >= s.tick && now - s.tick <= kSampledWindowMs)
                ++live;
        }
        if (live > _peakSampled)
            _peakSampled = live;
    }

    size_t DistinctEntries() const
    {
        size_t n = 0;
        for (const bool seen : _entriesSeen)
            n += seen ? 1 : 0;
        return n;
    }
    uint32_t PeakSampled() const { return _peakSampled; }

    // ---- Renders per game frame ----
    // A pass of the preview renderer started in game frame `frame`.
    void NoteRenderStart(uint64_t frame)
    {
        if (_frameOpen && frame != _frame)
            FlushFrame();
        _frame = frame;
        _frameOpen = true;
        ++_rendersInFrame;
    }

    // Counts the renders of the frame in progress. The burst end calls it before it reads the histogram.
    void FlushFrame()
    {
        if (!_frameOpen)
            return;
        if (_rendersInFrame != 0)
        {
            const size_t bucket = _rendersInFrame >= kRendersPerFrame ? kRendersPerFrame - 1 : static_cast<size_t>(_rendersInFrame) - 1;
            ++_rendersPerFrame[bucket];
        }
        _rendersInFrame = 0;
        _frameOpen = false;
    }

    const std::array<uint64_t, kRendersPerFrame>& RendersPerFrame() const { return _rendersPerFrame; }

    // ---- Signed-motion replay ----
    void NoteReplaySkip(motion::Skip reason)
    {
        const size_t i = static_cast<size_t>(reason);
        if (i < _replaySkips.size())
            ++_replaySkips[i];
    }
    uint64_t ReplaySkips(motion::Skip reason) const
    {
        const size_t i = static_cast<size_t>(reason);
        return i < _replaySkips.size() ? _replaySkips[i] : 0;
    }

    // An untagged G-buffer draw the replay refused. True when the shape is new in this burst and was stored (the caller writes one
    // log line; at most kUnknownShapes shapes per burst).
    bool NoteUnknownShape(const UnknownShape& shape)
    {
        for (size_t i = 0; i < _unknownCount; ++i)
        {
            if (_unknownShapes[i].crc == shape.crc && _unknownShapes[i].vsCb0Bytes == shape.vsCb0Bytes &&
                _unknownShapes[i].psCb2Bytes == shape.psCb2Bytes && _unknownShapes[i].rtvEntry == shape.rtvEntry)
                return false;
        }
        if (_unknownCount >= _unknownShapes.size())
            return false;
        _unknownShapes[_unknownCount++] = shape;
        return true;
    }
    size_t UnknownShapeCount() const { return _unknownCount; }
    const UnknownShape& UnknownShapeAt(size_t i) const { return _unknownShapes[i < _unknownCount ? i : 0]; }

    // ---- Reading ----
    const std::array<WindowEntryCounts, kWindowBuckets>& Entries() const { return _entries; }

    // ---- Preview identity ----
    // How the pass was named at its first geometry draw, and what a finisher taught.
    void NoteIdentity(WindowSet::Name name)
    {
        const size_t i = static_cast<size_t>(name);
        if (i < _identity.size())
            ++_identity[i];
    }
    void NoteIdentityLearned() { ++_identityLearned; }
    void NoteIdentityCleared() { ++_identityCleared; }
    void NoteIdentityMismatch() { ++_identityMismatch; }

    // The identity counters of this burst; `known` is filled by the caller from the set (it counts the bindings that exist now).
    IdentityCounts Identity() const
    {
        IdentityCounts c;
        c.named = _identity[static_cast<size_t>(WindowSet::Name::Named)];
        c.outside = _identity[static_cast<size_t>(WindowSet::Name::Outside)];
        c.unknown = _identity[static_cast<size_t>(WindowSet::Name::Unknown)];
        c.ambiguous = _identity[static_cast<size_t>(WindowSet::Name::Ambiguous)];
        c.invalid = _identity[static_cast<size_t>(WindowSet::Name::Invalid)];
        c.learned = _identityLearned;
        c.cleared = _identityCleared;
        c.mismatch = _identityMismatch;
        return c;
    }

    // The burst is over: everything starts at zero again, including the collision detector's last finisher.
    void Reset()
    {
        _entries = {};
        _entriesSeen = {};
        _slotFailures = {};
        _sampled = {};
        _peakSampled = 0;
        _rendersPerFrame = {};
        _frame = 0;
        _rendersInFrame = 0;
        _frameOpen = false;
        _replaySkips = {};
        _unknownShapes = {};
        _unknownCount = 0;
        _collisions = 0;
        _lastKey = 0;
        _lastFrame = 0;
        _haveLast = false;
        _lastCollided = false;
        _identity = {};
        _identityLearned = 0;
        _identityCleared = 0;
        _identityMismatch = 0;
    }

  private:
    struct Sampled
    {
        uintptr_t key = 0;
        uint64_t tick = 0;
    };

    std::array<WindowEntryCounts, kWindowBuckets> _entries {};
    std::array<bool, kWindowEntries> _entriesSeen {};
    std::array<uint64_t, kMaxSlots> _slotFailures {};
    std::array<Sampled, kSampledImages> _sampled {};
    uint32_t _peakSampled = 0;
    std::array<uint64_t, kRendersPerFrame> _rendersPerFrame {};
    uint64_t _frame = 0;
    uint32_t _rendersInFrame = 0;
    bool _frameOpen = false;
    std::array<uint64_t, kReplaySkips> _replaySkips {};
    std::array<UnknownShape, kUnknownShapes> _unknownShapes {};
    size_t _unknownCount = 0;
    uint64_t _collisions = 0;
    uintptr_t _lastKey = 0;
    uint64_t _lastFrame = 0;
    bool _haveLast = false;
    bool _lastCollided = false;
    std::array<uint64_t, kIdentityNames> _identity {}; // one per WindowSet::Name value
    uint64_t _identityLearned = 0;
    uint64_t _identityCleared = 0;
    uint64_t _identityMismatch = 0;
};

// The burst's counters (draw thread only: the burst bookkeeping writes and reads them on that thread).
inline WindowStats g_windowStats;
} // namespace ext::ffxiv::chara
