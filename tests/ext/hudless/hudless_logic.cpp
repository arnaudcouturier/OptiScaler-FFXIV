// S10 package B: the pure engine logic (ext/hudless/HudlessLogic.h): fallback reasons, the displayed-state tracker with
// hysteresis, the mask-coverage guard (trip, retry timing, "unknown"), the slot ring with reader fences (never overwrite
// a busy slot, never block) and the exact log line formats of docs/HUDLESS.md section 5.
#include "ext/hudless/HudlessLogic.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace ext::hudless;

namespace
{
struct FakeFences final : FenceReader
{
    uint64_t completed[4] = { 0, 0, 0, 0 };
    int queries = 0;
    uint64_t Completed(const void* fence) override
    {
        ++queries;
        return completed[reinterpret_cast<uintptr_t>(fence) - 1];
    }
    // fence ids are 1-based "pointers"
    static const void* Id(int n) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(n)); }
};

void Feed(StateTracker& t, int n, Reason r, bool armed = true)
{
    for (int i = 0; i < n; ++i)
        t.Observe(armed, r);
}

void TestReasons()
{
    static_assert(kReasonCount == 15);
    std::set<std::string> names;
    for (size_t i = 0; i < kReasonCount; ++i)
    {
        const auto r = static_cast<Reason>(i);
        assert(std::strlen(ReasonName(r)) > 1 && std::string(ReasonName(r)) != "?");
        assert(names.insert(ReasonName(r)).second); // unique
        if (r != Reason::Count)
            assert(std::strlen(ReasonText(r)) > 5);
    }
    // The documented list (docs/HUDLESS.md section 5), by name
    for (const char* n : { "ModeOff", "NoFrameGeneration", "ProviderUnsupported", "NoSource", "NoBoundaryThisFrame",
                           "CaptureRejected", "SizeMismatch", "FormatUnsupported", "MsaaOrArray", "SlotBusy",
                           "PasteNotReady", "SceneHdrOwnsFrame", "MaskCoverage", "DebugTintOn" })
        assert(names.count(n) == 1);
    assert(std::string(ReasonName(Reason::Count)) == "?");
}

void TestStateTracker()
{
    StateTracker t;
    assert(t.GetPhase() == Phase::Off);

    // Searching until enough captures, then Active
    t.Observe(true, Reason::None);
    assert(t.GetPhase() == Phase::Searching);
    Feed(t, 8, Reason::None);
    assert(t.GetPhase() == Phase::Searching); // 9 samples < kMinSamples
    t.Observe(true, Reason::None);
    assert(t.GetPhase() == Phase::Active);

    // Hysteresis: 10 % failures (the enter threshold itself) keep Active; the leave threshold is lower than the enter one
    StateTracker h;
    Feed(h, 60, Reason::None);
    assert(h.GetPhase() == Phase::Active);
    for (int i = 0; i < 6; ++i)
        h.Observe(true, Reason::SlotBusy);
    assert(h.Samples() == 60 && h.GetPhase() == Phase::Active); // 54/60 = 90 %
    for (int i = 0; i < 9; ++i)
        h.Observe(true, Reason::SlotBusy);
    assert(h.GetPhase() == Phase::Active); // 45/60 = 75 % is not below kLeave
    h.Observe(true, Reason::SlotBusy);
    assert(h.GetPhase() == Phase::Fallback && h.Dominant() == Reason::SlotBusy); // 44/60
    // Back above 75 % but below 90 % stays Fallback (no flapping)
    Feed(h, 8, Reason::None);
    assert(h.GetPhase() == Phase::Fallback); // 44/60 = 73 %
    Feed(h, 30, Reason::None);
    assert(h.GetPhase() == Phase::Fallback); // the failures are still in the window
    Feed(h, 30, Reason::None);
    assert(h.GetPhase() == Phase::Active);   // at most 6 failures left: 90 % again

    // Searching never finds anything: Fallback after kSearchFrames armed presents, naming the dominant reason
    StateTracker s;
    Feed(s, 59, Reason::NoBoundaryThisFrame);
    assert(s.GetPhase() == Phase::Searching);
    s.Observe(true, Reason::NoBoundaryThisFrame);
    assert(s.GetPhase() == Phase::Fallback && s.Dominant() == Reason::NoBoundaryThisFrame);

    // The displayed reason is sticky: two alternating reasons do not flip it
    StateTracker k;
    Feed(k, 12, Reason::SlotBusy);
    Feed(k, 20, Reason::None);
    const Reason first = k.Dominant();
    assert(first == Reason::SlotBusy);
    for (int i = 0; i < 10; ++i)
    {
        k.Observe(true, i % 2 ? Reason::PasteNotReady : Reason::SlotBusy);
        assert(k.Dominant() == Reason::SlotBusy);
    }
    Feed(k, 18, Reason::PasteNotReady); // now ahead by the switch margin (23 vs 17)
    assert(k.Dominant() == Reason::PasteNotReady);

    // Off: after kOffAfter consecutive unarmed presents (frame generation paused briefly keeps the state), or at once
    StateTracker o;
    Feed(o, 60, Reason::None);
    Feed(o, 9, Reason::NoFrameGeneration, false);
    assert(o.GetPhase() == Phase::Active);
    o.Observe(true, Reason::None); // resets the unarmed run
    Feed(o, 9, Reason::NoFrameGeneration, false);
    assert(o.GetPhase() == Phase::Active);
    o.Observe(false, Reason::NoFrameGeneration);
    assert(o.GetPhase() == Phase::Off);
    o.Observe(true, Reason::None);
    assert(o.GetPhase() == Phase::Searching);
    o.SetOff();
    assert(o.GetPhase() == Phase::Off && o.Samples() == 0);
}

void TestCoverageGuard()
{
    // Trips after 5 consecutive frames over the limit; produced advances by one per frame
    CoverageGuard g;
    g.Configure(0.60f);
    uint64_t frame = 0;
    auto step = [&](float coverage) { ++frame; g.Update(frame, coverage, frame); };
    for (int i = 0; i < 3; ++i)
        step(0.9f); // the first kReadbackLag frames are stale data
    assert(!g.Blocking());
    for (int i = 0; i < 4; ++i)
    {
        step(0.9f);
        assert(!g.Blocking());
    }
    step(0.9f); // 5th over-limit sample
    assert(g.Blocking() && g.Trips() == 1);
    const uint64_t trippedAt = frame;

    // A drop below the limit in between resets the count
    CoverageGuard r;
    r.Configure(0.60f);
    uint64_t f2 = 0;
    for (int i = 0; i < 3; ++i)
        r.Update(++f2, 0.9f, f2);
    for (int i = 0; i < 4; ++i)
        r.Update(++f2, 0.9f, f2);
    r.Update(++f2, 0.2f, f2);
    for (int i = 0; i < 4; ++i)
        r.Update(++f2, 0.9f, f2);
    assert(!r.Blocking());

    // Blocks for kRetryFrames, then retries; the old readbacks are stale until 3 new images were produced
    for (int i = 1; i < CoverageGuard::kRetryFrames; ++i)
    {
        step(0.9f);
        assert(g.Blocking());
    }
    step(0.9f); // frame == trippedAt + 120
    assert(frame == trippedAt + CoverageGuard::kRetryFrames);
    assert(!g.Blocking());
    for (int i = 0; i < 2 + 4; ++i)
    {
        step(0.9f); // 2 stale frames (the retry frame itself is the 3rd), then 4 samples over the limit
        assert(!g.Blocking());
    }
    step(0.9f);
    assert(g.Blocking() && g.Trips() == 2);

    // Unknown never blocks and reads as unknown after kUnknownFrames
    CoverageGuard u;
    u.Configure(0.60f);
    for (uint64_t i = 1; i <= 20; ++i)
    {
        u.Update(i, -1.f, 0);
        assert(!u.Blocking());
        assert(u.Unknown() == (i >= static_cast<uint64_t>(CoverageGuard::kUnknownFrames)));
    }
    u.Update(21, 0.1f, 100); // a readback completes
    assert(!u.Unknown());

    // Off
    CoverageGuard off;
    off.Configure(0.f);
    for (uint64_t i = 1; i <= 50; ++i)
        off.Update(i, 1.0f, i);
    assert(!off.Blocking() && !off.Unknown());
}

void TestSlotRing()
{
    FakeFences fences;
    SlotRing<4> ring;
    const auto copyFence = FakeFences::Id(1), uiFence = FakeFences::Id(2), produceFence = FakeFences::Id(3);

    // Free slots are handed out round robin
    assert(ring.Acquire(0, 1, fences) == 0);
    assert(ring.Acquire(2, 1, fences) == 2);

    // Every kind of reader pins the slot until its fence value completed
    assert(ring.AddReader(0, copyFence, 5, 1));
    assert(ring.AddReader(0, uiFence, 7, 1));
    assert(ring.AddReader(0, produceFence, 3, 1));
    fences.completed[0] = 5;
    fences.completed[1] = 6;
    fences.completed[2] = 3;
    assert(ring.Busy(0, 2, fences)); // the UI fence is at 6 < 7
    assert(ring.Readers(0) == 1);    // completed readers are dropped
    fences.completed[1] = 7;
    assert(!ring.Busy(0, 3, fences));
    assert(ring.Readers(0) == 0);

    // Acquire skips busy slots and never returns one that is being read
    assert(ring.AddReader(1, copyFence, 100, 3));
    assert(ring.AddReader(2, uiFence, 100, 3));
    assert(ring.AddReader(3, produceFence, 100, 3));
    assert(ring.Acquire(1, 4, fences) == 0); // 1, 2, 3 busy, 0 free
    assert(ring.AddReader(0, copyFence, 100, 4));
    assert(ring.Acquire(0, 5, fences) == -1); // all busy: the frame is a SlotBusy fallback, nothing waits
    fences.completed[0] = 100;
    assert(ring.Acquire(1, 6, fences) == 1); // the copy fence completed: slots 0 and 1 are free, round robin from 1

    // Same fence twice keeps the larger value
    SlotRing<2> two;
    assert(two.AddReader(0, copyFence, 10, 1));
    assert(two.AddReader(0, copyFence, 4, 1));
    fences.completed[0] = 9;
    assert(two.Busy(0, 2, fences));
    fences.completed[0] = 10;
    assert(!two.Busy(0, 3, fences));

    // Device removed (UINT64_MAX) is never "completed" ...
    fences.completed[0] = UINT64_MAX;
    assert(two.AddReader(1, copyFence, 1, 10));
    assert(two.Busy(1, 11, fences));
    // ... but a reader that never signals cannot pin a slot forever: stale entries are dropped and counted
    assert(two.Busy(1, 10 + 239, fences));
    assert(!two.Busy(1, 10 + 240, fences));
    assert(two.StaleDrops() == 1);

    // Capacity: a slot tracking too many distinct fences refuses another one instead of forgetting a reader
    SlotRing<1, 2> small;
    assert(small.AddReader(0, copyFence, 1, 1));
    assert(small.AddReader(0, uiFence, 1, 1));
    assert(!small.AddReader(0, produceFence, 1, 1));
    assert(small.Readers(0) == 2);
    small.Clear(0);
    assert(small.Readers(0) == 0);
    assert(!small.AddReader(5, copyFence, 1, 1)); // out of range
    assert(small.Busy(5, 1, fences));             // out of range counts as busy

    // The check is a pure read: querying only calls Completed, never anything that could wait
    FakeFences counting;
    SlotRing<4> q;
    for (size_t s = 0; s < 4; ++s)
        q.AddReader(s, copyFence, 50, 1);
    counting.queries = 0;
    assert(q.Acquire(0, 2, counting) == -1);
    assert(counting.queries == 4);
}

void TestFormats()
{
    // The documented example line (docs/HUDLESS.md section 5), reproduced exactly
    WindowStats w;
    w.presents = 600;
    w.captured = 598;
    NoteReason(w, Reason::SlotBusy);
    NoteReason(w, Reason::NoBoundaryThisFrame);
    w.coverageSum = 0.41;
    w.coverageSamples = 10;
    w.coverageMax = 0.097f;
    const std::string line = SummaryLine(Mode::UiFree, Phase::Active, Reason::None, w, 598, 1190, 6, "ok");
    assert(line == "HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, "
                   "NoBoundaryThisFrame 1); mask coverage avg 4.1% max 9.7%; paste built 598 pasted 1190 skipped 6; "
                   "slot waits 0; boundary: ok");

    // Ties break by first occurrence, counts first
    WindowStats b;
    b.presents = 10;
    b.captured = 3;
    NoteReason(b, Reason::PasteNotReady);
    NoteReason(b, Reason::SlotBusy);
    NoteReason(b, Reason::SlotBusy);
    NoteReason(b, Reason::NoBoundaryThisFrame);
    assert(FallbackBreakdown(b) == "SlotBusy 2, PasteNotReady 1, NoBoundaryThisFrame 1");

    // No fallbacks and no coverage samples (recomposition): no parentheses, "n/a"
    WindowStats clean;
    clean.presents = 600;
    clean.captured = 600;
    assert(SummaryLine(Mode::Recomposition, Phase::Active, Reason::None, clean, 0, 0, 0, "ok") ==
           "HudLess: mode recomposition, state Active; 600 presents: 600 captured, 0 fallbacks; mask coverage n/a; paste "
           "built 0 pasted 0 skipped 0; slot waits 0; boundary: ok");
    assert(SummaryLine(Mode::UiFree, Phase::Fallback, Reason::MaskCoverage, clean, 1, 2, 3, "not detected")
               .find("state Fallback(MaskCoverage);") != std::string::npos);

    assert(StateChangeLine(Phase::Searching, Reason::None, Phase::Active, Reason::None) ==
           "HudLess: state Searching -> Active");
    assert(StateChangeLine(Phase::Active, Reason::None, Phase::Fallback, Reason::MaskCoverage) ==
           "HudLess: state Active -> Fallback(MaskCoverage)");
    assert(ModeLine(Mode::Off) == "HudLess: mode off");
    assert(ModeChangeLine(Mode::Off, Mode::UiFree) == "HudLess: mode off -> ui_free");
    assert(StateText(Phase::Off, Reason::SlotBusy) == "Off"); // the reason only shows with Fallback
}
} // namespace

int main()
{
    TestReasons();
    TestStateTracker();
    TestCoverageGuard();
    TestSlotRing();
    TestFormats();
    std::puts("PASS: reasons, state tracker hysteresis, coverage guard (trip, retry, unknown), slot ring fences, log formats");
    return 0;
}
