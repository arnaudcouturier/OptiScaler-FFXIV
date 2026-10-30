// S10 package B: the pure engine logic (ext/hudless/HudlessLogic.h): fallback reasons, the displayed-state tracker with
// hysteresis, the mask-coverage guard (trip, release, starvation, "unknown"), the slot ring with reader fences (never overwrite
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
    static_assert(kReasonCount == 16);
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
                           "PasteNotReady", "SceneHdrOwnsFrame", "MaskCoverage", "DebugTintOn", "PausedByPreset" })
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
    // Driven by PasteStatus::coverageSequence: only a NEW sequence is a sample. The first update just takes the baseline
    // (a readback that existed before the guard started is stale), so it is never a sample.
    struct Rig
    {
        CoverageGuard g;
        uint64_t seq = 0;
        Rig() { g.Configure(0.60f); }
        // a new readback completed / the same readback read again; `probed` = the previous present took a capture
        // while tripped (the engine only counts those towards starvation)
        void Fresh(float coverage, bool probed = false) { g.Update(coverage, ++seq, probed); }
        void Stale(float coverage, bool probed = false) { g.Update(coverage, seq, probed); }
    };

    // Trips after 5 consecutive fresh samples over the limit
    Rig a;
    a.Fresh(0.9f); // baseline
    for (int i = 0; i < 4; ++i)
    {
        a.Fresh(0.9f);
        assert(!a.g.Blocking());
    }
    a.Fresh(0.9f); // 5th sample over the limit
    assert(a.g.Blocking() && a.g.Trips() == 1);

    // Tripped, no readback arrives: the guard holds until kStarveFrames PROBED presents went by (the starvation release
    // is covered in TestCoverageGuardStarvation), then it lets go and the old readback stays stale
    for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
    {
        a.Stale(0.9f, true);
        assert(a.g.Blocking());
    }
    a.Stale(0.9f, true);
    assert(!a.g.Blocking());
    for (int i = 0; i < 30; ++i)
    {
        a.Stale(0.9f); // the old readback again and again: never counted, however often it is read
        assert(!a.g.Blocking());
    }
    assert(a.g.Unknown());
    for (int i = 0; i < 4; ++i)
    {
        a.Fresh(0.9f);
        assert(!a.g.Blocking());
    }
    assert(!a.g.Unknown());
    a.Fresh(0.9f);
    assert(a.g.Blocking() && a.g.Trips() == 2);

    // A stale value never trips, however high: the same sequence read 30 times is not 30 samples
    Rig s;
    for (int i = 0; i < 31; ++i)
    {
        s.Stale(0.95f);
        assert(!s.g.Blocking());
    }

    // A drop below the limit resets the streak
    Rig r;
    r.Fresh(0.9f);
    for (int i = 0; i < 4; ++i)
        r.Fresh(0.9f);
    r.Fresh(0.2f);
    for (int i = 0; i < 4; ++i)
        r.Fresh(0.9f);
    assert(!r.g.Blocking());
    r.Fresh(0.9f);
    assert(r.g.Blocking());

    // Presents without a new readback (up to kUnknownFrames - 1) neither add to nor reset the streak
    Rig gap;
    gap.Fresh(0.9f);
    for (int i = 0; i < 3; ++i)
        gap.Fresh(0.9f);
    for (int i = 0; i < CoverageGuard::kUnknownFrames - 1; ++i)
        gap.Stale(0.9f);
    assert(!gap.g.Unknown() && !gap.g.Blocking());
    gap.Fresh(0.9f);
    assert(!gap.g.Blocking());
    gap.Fresh(0.9f); // 5th sample
    assert(gap.g.Blocking());

    // kUnknownFrames presents in a row without a new readback: unknown, and the streak does not survive it
    Rig lost;
    lost.Fresh(0.9f);
    for (int i = 0; i < 4; ++i)
        lost.Fresh(0.9f);
    assert(!lost.g.Blocking()); // 4 samples so far
    for (int i = 0; i < CoverageGuard::kUnknownFrames; ++i)
        lost.Stale(0.9f);
    assert(lost.g.Unknown());
    lost.Fresh(0.9f);
    assert(!lost.g.Unknown());
    for (int i = 0; i < 3; ++i)
    {
        lost.Fresh(0.9f);
        assert(!lost.g.Blocking());
    }
    lost.Fresh(0.9f);
    assert(lost.g.Blocking());

    // Coverage < 0 (nothing completed yet) never blocks and reads as unknown after kUnknownFrames
    CoverageGuard u;
    u.Configure(0.60f);
    for (uint64_t i = 1; i <= 20; ++i)
    {
        u.Update(-1.f, 0);
        assert(!u.Blocking());
        assert(u.Unknown() == (i >= static_cast<uint64_t>(CoverageGuard::kUnknownFrames)));
    }
    u.Update(0.1f, 100); // a readback completes
    assert(!u.Unknown());

    // A producer that never stamps the sequence (0) is "unknown", whatever the coverage says: fail open
    CoverageGuard unstamped;
    unstamped.Configure(0.60f);
    for (uint64_t i = 1; i <= 30; ++i)
    {
        unstamped.Update(0.99f, 0);
        assert(!unstamped.Blocking());
    }
    assert(unstamped.Unknown());

    // A readback that was there before the guard started (mode switched on later) is not a sample
    CoverageGuard late;
    late.Configure(0.60f);
    for (uint64_t i = 1; i <= 10; ++i)
    {
        late.Update(0.9f, 500);
        assert(!late.Blocking());
    }

    // Off
    CoverageGuard off;
    off.Configure(0.f);
    for (uint64_t i = 1; i <= 50; ++i)
        off.Update(1.0f, i);
    assert(!off.Blocking() && !off.Unknown());
}

// A guard (limit 0.60, release level 0.45) tripped by five fresh samples at 0.9; the baseline sample comes first
struct Tripped
{
    CoverageGuard g;
    uint64_t seq = 0;
    Tripped()
    {
        g.Configure(0.60f);
        Fresh(0.9f); // baseline
        for (int i = 0; i < CoverageGuard::kTripFrames; ++i)
            Fresh(0.9f);
        assert(g.Blocking() && g.Trips() == 1);
    }
    void Fresh(float coverage, bool probed = true) { g.Update(coverage, ++seq, probed); }
    void Stale(float coverage, bool probed = true) { g.Update(coverage, seq, probed); }
};

void TestCoverageGuardRelease()
{
    static_assert(CoverageGuard::kReleaseSamples == 30 && CoverageGuard::kTripFrames == 5);

    // 29 clean samples do not release, the 30th does
    {
        Tripped t;
        for (int i = 0; i < CoverageGuard::kReleaseSamples - 1; ++i)
        {
            t.Fresh(0.40f);
            assert(t.g.Blocking());
        }
        t.Fresh(0.40f);
        assert(!t.g.Blocking());
        assert(t.g.Trips() == 1 && t.g.Releases() == 1); // a release is counted like a trip
    }

    // The release level is 0.75 x the limit: just under it counts, just over it does not
    {
        Tripped t;
        for (int i = 0; i < 100; ++i)
            t.Fresh(0.46f);
        assert(t.g.Blocking());
        for (int i = 0; i < CoverageGuard::kReleaseSamples; ++i)
            t.Fresh(0.44f);
        assert(!t.g.Blocking());
    }

    // A fresh sample above the release level resets the count (the 30 samples must be consecutive)
    {
        Tripped t;
        for (int i = 0; i < 29; ++i)
            t.Fresh(0.10f);
        t.Fresh(0.50f); // one in the band: back to zero
        for (int i = 0; i < 29; ++i)
        {
            t.Fresh(0.10f);
            assert(t.g.Blocking());
        }
        t.Fresh(0.10f);
        assert(!t.g.Blocking());
    }

    // The band between the release level and the limit holds the state, however long it lasts; so does a value over the
    // limit
    {
        Tripped t;
        for (int i = 0; i < 500; ++i)
        {
            t.Fresh(0.50f);
            assert(t.g.Blocking());
        }
        for (int i = 0; i < 100; ++i)
            t.Fresh(i % 2 ? 0.50f : 0.95f);
        assert(t.g.Blocking() && t.g.Trips() == 1);
        // 29 clean, one in the band, repeated: never released
        for (int round = 0; round < 10; ++round)
        {
            for (int i = 0; i < 29; ++i)
                t.Fresh(0.20f);
            t.Fresh(0.55f);
        }
        assert(t.g.Blocking());
    }

    // Stale repeats of one sequence never count, probed or not (up to the starvation limit)
    {
        Tripped t;
        for (int i = 0; i < 100; ++i)
        {
            t.Stale(0.05f, i % 2 == 0);
            assert(t.g.Blocking());
        }
        // and they do not disturb a count in progress
        Tripped u;
        for (int i = 0; i < 29; ++i)
            u.Fresh(0.10f);
        for (int i = 0; i < 100; ++i)
            u.Stale(0.9f, false);
        assert(u.g.Blocking());
        u.Fresh(0.10f);
        assert(!u.g.Blocking());
    }

    // After the release the guard is a fresh guard: five consecutive fresh samples over the limit trip it again; a sample
    // under the limit (also in the band) resets that streak
    {
        Tripped t;
        for (int i = 0; i < CoverageGuard::kReleaseSamples; ++i)
            t.Fresh(0.10f);
        assert(!t.g.Blocking());
        for (int i = 0; i < 4; ++i)
            t.Fresh(0.9f);
        t.Fresh(0.55f);
        for (int i = 0; i < 4; ++i)
            t.Fresh(0.9f);
        assert(!t.g.Blocking());
        t.Fresh(0.9f);
        assert(t.g.Blocking() && t.g.Trips() == 2);
    }

    // A changed limit moves the release level with it
    {
        CoverageGuard g;
        g.Configure(0.40f); // release level 0.30
        uint64_t seq = 0;
        g.Update(0.9f, ++seq);
        for (int i = 0; i < CoverageGuard::kTripFrames; ++i)
            g.Update(0.9f, ++seq);
        assert(g.Blocking());
        for (int i = 0; i < 100; ++i)
            g.Update(0.35f, ++seq); // below the limit but above 0.75 x the limit
        assert(g.Blocking());
        for (int i = 0; i < CoverageGuard::kReleaseSamples; ++i)
            g.Update(0.25f, ++seq);
        assert(!g.Blocking());
    }
}

void TestCoverageGuardStarvation()
{
    // kStarveFrames probed presents without a fresh sample release the guard
    {
        Tripped t;
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
        {
            t.Stale(0.9f, true);
            assert(t.g.Blocking());
        }
        t.Stale(0.9f, true);
        assert(!t.g.Blocking());
        assert(t.g.Trips() == 1 && t.g.Releases() == 1); // the starvation release counts as a release
    }

    // Presents that were not probed (no capture: loading screen, missing boundary) never count
    {
        Tripped t;
        for (int i = 0; i < 100 * CoverageGuard::kStarveFrames; ++i)
            t.Stale(0.9f, false);
        assert(t.g.Blocking());
        // they do not reset a starvation count either: 119 probed, a pause, the 120th probed one releases
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
            t.Stale(0.9f, true);
        for (int i = 0; i < 1000; ++i)
            t.Stale(0.9f, false);
        assert(t.g.Blocking());
        t.Stale(0.9f, true);
        assert(!t.g.Blocking());
    }

    // A fresh sample (also one that does not release: the band, or over the limit) restarts the starvation count
    {
        Tripped t;
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
            t.Stale(0.9f, true);
        t.Fresh(0.50f);
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
        {
            t.Stale(0.9f, true);
            assert(t.g.Blocking());
        }
        t.Stale(0.9f, true);
        assert(!t.g.Blocking());
    }

    // Unknown coverage (no readback at all) counts as starvation too, and never trips
    {
        CoverageGuard g;
        g.Configure(0.60f);
        uint64_t seq = 0;
        g.Update(0.9f, ++seq);
        for (int i = 0; i < CoverageGuard::kTripFrames; ++i)
            g.Update(0.9f, ++seq);
        assert(g.Blocking());
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
            g.Update(-1.f, 0, true);
        assert(g.Blocking());
        g.Update(-1.f, 0, true);
        assert(!g.Blocking() && g.Unknown());
    }

    // After the starvation release the old sequence is still stale, only a newer one counts
    {
        Tripped t;
        for (int i = 0; i < CoverageGuard::kStarveFrames; ++i)
            t.Stale(0.9f, true);
        assert(!t.g.Blocking());
        for (int i = 0; i < 50; ++i)
            t.Stale(0.9f, true);
        assert(!t.g.Blocking());
        for (int i = 0; i < CoverageGuard::kTripFrames - 1; ++i)
            t.Fresh(0.9f);
        assert(!t.g.Blocking());
        t.Fresh(0.9f);
        assert(t.g.Blocking() && t.g.Trips() == 2);
    }
}

// ---- the density policy of the guard ([HudLess] GuardMode = density) -----------------------------------------------------------------
// Trips at 3 bad of the last 12 fresh samples, releases only after 60 consecutive clean fresh samples AND a present
// that is not anchored after an off-screen UI draw (the release is decided after the boundary's same-frame answer: ResolveRelease).
// The starvation release is the old one. "Clean" is as before: at or below 0.75 x the limit.
struct DensityRig
{
    CoverageGuard g;
    uint64_t seq = 0;
    explicit DensityRig(GuardPolicy policy = GuardPolicy::Density, int releaseHolds = 0, bool tripOnAnchorLater = false)
    {
        g.Configure(0.60f, policy, releaseHolds, tripOnAnchorLater);
        Fresh(0.9f); // the baseline: a readback from before the guard started is never a sample
    }
    // one present with a new readback, then the boundary's answer for the same present
    void Fresh(float coverage, bool anchorLater = false, bool probed = true)
    {
        g.Update(coverage, ++seq, probed);
        g.ResolveRelease(anchorLater);
    }
    void Stale(float coverage, bool anchorLater = false, bool probed = true)
    {
        g.Update(coverage, seq, probed);
        g.ResolveRelease(anchorLater);
    }
    // the samples (0.9 = bad, 0.1 = clean) until it trips; 0 = never within `n`
    int TripsAt(int n, int period)
    {
        for (int i = 1; i <= n; ++i)
        {
            Fresh(i % period == 0 ? 0.9f : 0.1f);
            if (g.Blocking())
                return i;
        }
        return 0;
    }
};

void TestDensityGuard()
{
    static_assert(CoverageGuard::kDensityWindow == 12 && CoverageGuard::kDensityTripBad == 3 &&
                  CoverageGuard::kDensityReleaseSamples == 60);

    // 3 of the last 12 trip: every fourth sample bad trips at the 12th; the legacy guard never trips on that stream
    assert(DensityRig().TripsAt(600, 4) == 12);
    assert(DensityRig(GuardPolicy::Legacy).TripsAt(600, 4) == 0);
    // 2 of any 12 never trip
    assert(DensityRig().TripsAt(600, 6) == 0);
    // three bad in a row trip at the third (legacy needs five): a run is covered by the 3-of-12 rule, there is no separate one
    {
        DensityRig d;
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(d.g.Blocking() && d.g.Trips() == 1);
    }
    // old samples fall out of the window: B B C*10 B is 2 of 12, then B is 2 of 12 again, the next one is 3
    {
        DensityRig d;
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        for (int i = 0; i < 10; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(d.g.Blocking());
    }
    // between the release level and the limit is not bad for the trip
    {
        DensityRig d;
        for (int i = 0; i < 100; ++i)
            d.Fresh(0.55f);
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
    }

    // release: 59 clean samples do not, the 60th does (30 are the legacy number)
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        assert(d.g.Blocking());
        for (int i = 0; i < CoverageGuard::kDensityReleaseSamples - 1; ++i)
        {
            d.Fresh(0.1f);
            assert(d.g.Blocking());
        }
        d.Fresh(0.1f);
        assert(!d.g.Blocking() && d.g.Trips() == 1 && d.g.Releases() == 1);
    }
    // the clean samples must be consecutive; a sample in the band between the release level and the limit restarts the count
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.50f);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        assert(d.g.Blocking());
        d.Fresh(0.1f);
        assert(!d.g.Blocking());
    }
    // after a release the window starts empty: three new bad samples trip it again, two do not
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < 60; ++i)
            d.Fresh(0.1f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(d.g.Blocking() && d.g.Trips() == 2);
    }
    // kUnknownFrames presents without a new readback forget the window, like the old streak
    {
        DensityRig d;
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        for (int i = 0; i < CoverageGuard::kUnknownFrames; ++i)
            d.Stale(0.9f, false, false);
        assert(d.g.Unknown());
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(d.g.Blocking());
    }

    // the same-frame fact holds a tripped guard: the present that would release is anchor-later, so it does not, and the count of clean
    // samples starts again
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.1f, true); // the 60th clean sample, on an anchor-later present
        assert(d.g.Blocking() && d.g.Releases() == 0 && d.g.Holds() == 1);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        assert(d.g.Blocking());
        d.Fresh(0.1f);
        assert(!d.g.Blocking() && d.g.Releases() == 1);
    }
    // ... also when the clean count was lower (a run of anchor-later presents keeps the fallback, whatever the readbacks say)
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < 40; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.1f, true);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        assert(d.g.Blocking());
        d.Fresh(0.1f);
        assert(!d.g.Blocking());
        // anchor-later presents with no new readback at all: nothing progresses
        DensityRig e;
        for (int i = 0; i < 3; ++i)
            e.Fresh(0.9f);
        for (int i = 0; i < 100; ++i)
            e.Stale(0.1f, true);
        assert(e.g.Blocking());
    }
    // a present that never reached the boundary (nothing to ask) resolves its release without the fact, so a due release cannot stick
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < 59; ++i)
            d.Fresh(0.1f);
        d.g.Update(0.1f, ++d.seq, true); // the 60th clean sample, the present ended before the boundary answered
        assert(d.g.Blocking());
        d.g.ResolveRelease(false);
        assert(!d.g.Blocking());
    }
    // an anchor-later present that is not tripped changes nothing
    {
        DensityRig d;
        d.Fresh(0.1f, true);
        d.Fresh(0.9f, true);
        assert(!d.g.Blocking() && d.g.Holds() == 0);
    }

    // the starvation release is the old one: 120 probed presents without a fresh sample, whatever the boundary says
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        assert(d.g.Blocking());
        for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
        {
            d.Stale(0.9f, true, true);
            assert(d.g.Blocking());
        }
        d.Stale(0.9f, true, true);
        assert(!d.g.Blocking() && d.g.Releases() == 1);
    }

    // the legacy policy ignores the fact and keeps its numbers: 5 in a row trip, 30 clean release, at once
    {
        DensityRig d(GuardPolicy::Legacy);
        for (int i = 0; i < 4; ++i)
            d.Fresh(0.9f, true);
        assert(!d.g.Blocking());
        d.Fresh(0.9f, true);
        assert(d.g.Blocking());
        for (int i = 0; i < CoverageGuard::kReleaseSamples - 1; ++i)
            d.Fresh(0.1f, true);
        assert(d.g.Blocking());
        d.Fresh(0.1f, true);
        assert(!d.g.Blocking() && d.g.Holds() == 0);
    }
    // a changed policy starts the guard over
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        assert(d.g.Blocking());
        d.g.Configure(0.60f, GuardPolicy::Legacy);
        assert(!d.g.Blocking());
    }
    // the policy text: only "density" (any case) is density, everything else is legacy
    assert(ParseGuardPolicy("density") == GuardPolicy::Density && ParseGuardPolicy(" Density ") == GuardPolicy::Density);
    for (const char* text : { "legacy", "auto", "", "dens", "sticky", "1" })
        assert(ParseGuardPolicy(text) == GuardPolicy::Legacy);
}

// ---- edges of the guard that the first round left open (found by the W2 mutation checks) -----------------------------------------------
// The far edge of the window, the limit and the release level exactly, the window surviving a Reset, the call protocol of ResolveRelease,
// and the older constants as literals (the other tests use their symbols).
void TestGuardEdges()
{
    assert(CoverageGuard::kStarveFrames == 120 && CoverageGuard::kUnknownFrames == 8);

    // the window is 12 fresh samples: bad samples 12 apart (the 1st and the 12th) trip together, 13 apart do not
    {
        DensityRig d;
        d.Fresh(0.9f);
        for (int i = 0; i < 5; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.9f);
        for (int i = 0; i < 4; ++i)
            d.Fresh(0.1f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f); // the 12th sample: the first one is still inside the window
        assert(d.g.Blocking());
    }
    {
        DensityRig d;
        d.Fresh(0.9f);
        for (int i = 0; i < 5; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.9f);
        for (int i = 0; i < 5; ++i)
            d.Fresh(0.1f);
        d.Fresh(0.9f); // the 13th: the first one has left the window
        assert(!d.g.Blocking());
    }

    // a sample exactly at the limit is not bad, one exactly at 0.75 x the limit is clean (both policies)
    const float limit = 0.60f;
    const float level = limit * CoverageGuard::kReleaseFraction;
    for (const GuardPolicy policy : { GuardPolicy::Density, GuardPolicy::Legacy })
    {
        DensityRig d(policy);
        for (int i = 0; i < 200; ++i)
            d.Fresh(limit);
        assert(!d.g.Blocking() && d.g.Trips() == 0);

        DensityRig t(policy);
        for (int i = 0; i < 6; ++i)
            t.Fresh(0.9f);
        assert(t.g.Blocking());
        const int need = policy == GuardPolicy::Density ? CoverageGuard::kDensityReleaseSamples : CoverageGuard::kReleaseSamples;
        for (int i = 0; i < need - 1; ++i)
            t.Fresh(level);
        assert(t.g.Blocking());
        t.Fresh(level);
        assert(!t.g.Blocking() && t.g.Releases() == 1);
    }

    // Reset forgets the density window: two bad samples before it, one after it, no trip
    {
        DensityRig d;
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        d.g.Reset();
        d.Fresh(0.1f); // the first sample after a Reset is a baseline
        d.Fresh(0.9f);
        assert(!d.g.Blocking() && d.g.Trips() == 0);
    }

    // ResolveRelease is asked after every Update; a release that is due (the 60th clean sample) is taken back by a band sample that
    // comes before the answer
    {
        DensityRig d;
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        assert(d.g.Blocking());
        for (int i = 0; i < CoverageGuard::kDensityReleaseSamples - 1; ++i)
            d.Fresh(0.1f);
        d.g.Update(0.1f, ++d.seq, true); // the 60th clean sample: the release is due, the answer has not come yet
        d.g.Update(0.5f, ++d.seq, true); // a band sample (above 0.75 x the limit, not over it): the clean run starts over
        d.g.ResolveRelease(false);
        assert(d.g.Blocking() && d.g.Releases() == 0);
    }
}

// ---- the two opt-in extensions of the density guard: a release limit (GuardReleaseHolds) and a per-frame trip
// (GuardTripOnAnchorLater) ---------------------------------------------------------------------------------------------------
void TestDensityReleaseLimit()
{
    static_assert(CoverageGuard::kMaxReleaseHolds == 600);

    // A tripped guard and one anchor-later present per 40: the clean run is reset every time, so the plain density rule never releases and the
    // release limit needs N holds. The first hold counts as a window too (no sample over the release level since the trip).
    auto run = [](int holds, int anchorPresents)
    {
        DensityRig d(GuardPolicy::Density, holds);
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f); // tripped
        for (int i = 1; i <= anchorPresents * 40; ++i)
            d.Fresh(0.1f, i % 40 == 0);
        return d.g;
    };
    // default 0 = off: never released by holds, whatever comes
    assert(run(0, 6).Blocking() && run(0, 6).Releases() == 0 && run(0, 6).HoldReleases() == 0);
    // 3 holds: not released at 2, released at the 3rd
    {
        const CoverageGuard at2 = run(3, 2);
        assert(at2.Blocking() && at2.Holds() == 2 && at2.Releases() == 0 && at2.HoldReleases() == 0);
        const CoverageGuard at3 = run(3, 3);
        assert(!at3.Blocking() && at3.Holds() == 3 && at3.Releases() == 1 && at3.HoldReleases() == 1);
    }
    // 1 releases at the first hold
    {
        const CoverageGuard one = run(1, 1);
        assert(!one.Blocking() && one.Releases() == 1 && one.HoldReleases() == 1);
    }

    // A hold window with a sample over the release level does not count and restarts the run: band samples count as not clean, as everywhere else
    {
        DensityRig d(GuardPolicy::Density, 2);
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        assert(d.g.Blocking() && d.g.Holds() == 0);
        d.Fresh(0.1f, true); // 1st clean hold
        assert(d.g.Blocking() && d.g.Holds() == 1);
        d.Fresh(0.50f); // band sample: the next hold window is not clean
        d.Fresh(0.1f, true); // a hold, but the run starts over
        assert(d.g.Blocking() && d.g.Holds() == 2 && d.g.Releases() == 0);
        d.Fresh(0.1f, true); // a clean window: run 1 of 2
        assert(d.g.Blocking() && d.g.Releases() == 0);
        d.Fresh(0.1f, true); // run 2 of 2
        assert(!d.g.Blocking() && d.g.Releases() == 1 && d.g.HoldReleases() == 1);
    }
    // ... and a sample over the limit (a bad frame) does the same
    {
        DensityRig d(GuardPolicy::Density, 2);
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f); // tripped
        d.Fresh(0.9f); // over the limit, while tripped: the window since the trip is not clean
        d.Fresh(0.1f, true); // hold 1, run 0
        assert(d.g.Blocking() && d.g.Holds() == 1 && d.g.Releases() == 0);
        d.Fresh(0.1f, true); // hold 2, run 1
        assert(d.g.Blocking() && d.g.Releases() == 0);
        d.Fresh(0.1f, true); // hold 3, run 2: the limit
        assert(!d.g.Blocking() && d.g.Releases() == 1 && d.g.HoldReleases() == 1);
    }

    // After the release the guard is off the trip and trips again by the normal rule (3 bad of the last 12), not by the limit
    {
        DensityRig d(GuardPolicy::Density, 1);
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f); // the trip it started with
        d.Fresh(0.1f, true); // a clean window ends it at once
        assert(!d.g.Blocking() && d.g.Trips() == 1 && d.g.Releases() == 1 && d.g.HoldReleases() == 1);
        d.Fresh(0.9f);
        d.Fresh(0.9f);
        assert(!d.g.Blocking());
        d.Fresh(0.9f);
        assert(d.g.Blocking() && d.g.Trips() == 2 && d.g.Releases() == 1);
        // a new limit starts the guard over, and the new trip starts its hold run from zero: two clean holds end it, one does not
        d.g.Configure(0.60f, GuardPolicy::Density, 2, false);
        assert(!d.g.Blocking());
        for (int i = 0; i < 4; ++i)
            d.Fresh(0.9f); // the first sample after a reset is a baseline
        assert(d.g.Blocking() && d.g.Trips() == 3);
        d.Fresh(0.1f, true);
        assert(d.g.Blocking() && d.g.HoldReleases() == 1);
        d.Fresh(0.1f, true);
        assert(!d.g.Blocking() && d.g.HoldReleases() == 2);
    }

    // The clean-run release is untouched: 60 clean samples release while no anchor-later present comes
    {
        DensityRig d(GuardPolicy::Density, 5);
        for (int i = 0; i < 3; ++i)
            d.Fresh(0.9f);
        for (int i = 0; i < CoverageGuard::kDensityReleaseSamples; ++i)
            d.Fresh(0.1f);
        assert(!d.g.Blocking() && d.g.Releases() == 1 && d.g.HoldReleases() == 0);
    }

    // The limit is clamped by the guard: a negative value is off, a huge one is one summary window
    {
        DensityRig off(GuardPolicy::Density, -5);
        for (int i = 0; i < 3; ++i)
            off.Fresh(0.9f);
        assert(off.g.Blocking());
        for (int i = 0; i < 200; ++i)
            off.Fresh(0.1f, true); // holds alone never release
        assert(off.g.Blocking() && off.g.Releases() == 0 && off.g.HoldReleases() == 0);

        DensityRig big(GuardPolicy::Density, 100000);
        for (int i = 0; i < 3; ++i)
            big.Fresh(0.9f);
        assert(big.g.Blocking());
        for (int i = 0; i < CoverageGuard::kMaxReleaseHolds - 1; ++i)
            big.Fresh(0.1f, true);
        assert(big.g.Blocking() && big.g.HoldReleases() == 0);
        big.Fresh(0.1f, true);
        assert(!big.g.Blocking() && big.g.HoldReleases() == 1);
    }
}

void TestDensityAnchorTrip()
{
    // A cleaner stream than the density rule needs: one anchor-later present per 40, every image clean (0.10). The samples never trip it.
    auto clean = [](bool tripOnAnchorLater, GuardPolicy policy = GuardPolicy::Density)
    {
        DensityRig d(policy, 0, tripOnAnchorLater);
        for (int i = 1; i <= 200; ++i)
            d.Fresh(0.10f, i % 40 == 0);
        return d.g;
    };
    // the key off: today's behaviour, nothing trips, nothing holds
    assert(!clean(false).Blocking() && clean(false).Trips() == 0 && clean(false).Holds() == 0);
    // the key on: the first anchor-later present trips the guard itself (it is the present whose own base would have been swapped)
    {
        const CoverageGuard on = clean(true);
        assert(on.Trips() == 1);
        assert(on.Holds() >= 1);
        // a trip lasts until the normal release rules end it: this stream keeps it (every 40 presents a hold restarts the 60-sample run)
        assert(on.Blocking());
    }
    // the frame itself: the trip happens on the anchor-later present, not two presents later when its readback arrives
    {
        DensityRig d(GuardPolicy::Density, 0, true);
        for (int i = 1; i < 40; ++i)
            d.Fresh(0.10f);
        assert(!d.g.Blocking() && d.g.Trips() == 0);
        d.Fresh(0.10f, true); // anchor-later: tripped at once, before its own readback
        assert(d.g.Blocking() && d.g.Trips() == 1);
    }
    // an anchor-later present that is not tripped and has the key off changes nothing (the old rule)
    {
        DensityRig d;
        d.Fresh(0.10f, true);
        assert(!d.g.Blocking() && d.g.Trips() == 0);
    }
    // guard off (limit 0): the per-frame trip cannot start a guard that is off
    {
        CoverageGuard g;
        g.Configure(0.f, GuardPolicy::Density, 0, true);
        g.Update(-1.f, 0, false);
        g.ResolveRelease(true);
        assert(!g.Blocking() && g.Trips() == 0);
    }
    // legacy ignores the key (its decisions are the recorded ones)
    assert(!clean(true, GuardPolicy::Legacy).Blocking() && clean(true, GuardPolicy::Legacy).Trips() == 0);
    // the trip ends by the normal clean run: the anchor-later present that tripped it is itself a clean sample
    {
        DensityRig d(GuardPolicy::Density, 0, true);
        d.Fresh(0.10f, true);
        assert(d.g.Blocking() && d.g.Trips() == 1);
        for (int i = 0; i < CoverageGuard::kDensityReleaseSamples - 1; ++i)
            d.Fresh(0.10f);
        assert(d.g.Blocking()); // 59 clean samples after the trip
        d.Fresh(0.10f);
        assert(!d.g.Blocking() && d.g.Trips() == 1 && d.g.Releases() == 1);
    }
    // a changed key starts the guard over, like a changed policy
    {
        DensityRig d(GuardPolicy::Density, 0, true);
        d.Fresh(0.10f, true);
        assert(d.g.Blocking());
        d.g.Configure(0.60f, GuardPolicy::Density, 0, false);
        assert(!d.g.Blocking());
    }
    // the line written when density becomes active names the options that are on
    const std::string plain = DensityGuardLine(0, false);
    assert(plain.find("trips on " + std::to_string(CoverageGuard::kDensityTripBad) + " bad of the last " +
                      std::to_string(CoverageGuard::kDensityWindow) + " samples") != std::string::npos);
    assert(plain.find("releases after " + std::to_string(CoverageGuard::kDensityReleaseSamples) + " clean ones in a row") != std::string::npos);
    assert(plain.find("clean holds") == std::string::npos && plain.find("trips it at once") == std::string::npos);
    const std::string limited = DensityGuardLine(4, true);
    assert(limited.find("or after 4 clean holds in a row") != std::string::npos);
    assert(limited.find("such a frame trips it at once") != std::string::npos);
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
    w.amplitudeLowSum = 0.39;
    w.amplitudeMidSum = 0.21;
    w.amplitudeHighSum = 0.04;
    w.amplitudeLowMax = 0.092f;
    w.amplitudeMidMax = 0.050f;
    w.amplitudeHighMax = 0.011f;
    const std::string line = SummaryLine(Mode::UiFree, Phase::Active, Reason::None, w, 598, 1190, 6, "ok");
    assert(line == "HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, "
                   "NoBoundaryThisFrame 1); mask coverage avg 4.1% max 9.7%; amplitude 0.008/0.05/0.2 avg "
                   "3.9/2.1/0.4% max 9.2/5.0/1.1%; paste built 598 pasted 1190 skipped 6; "
                   "slot waits 0; boundary: ok; guard trips 0 releases 0 (0 by release limit); swapped bad frames 0; "
                   "anchor-later 0 of 0 frames (0.0%), longest run 0; tag changes 0");

    // The log counters come last, after every field that was there before, in this order
    w.guardTrips = 2;
    w.guardReleases = 1;
    w.guardHoldReleases = 1;
    w.swappedBad = 3;
    w.anchorFrames = 74;
    w.anchorTakes = 598;
    w.anchorLongest = 31;
    w.tagChanges = 5;
    const std::string counted = SummaryLine(Mode::UiFree, Phase::Active, Reason::None, w, 598, 1190, 6, "ok");
    assert(counted.ends_with("slot waits 0; boundary: ok; guard trips 2 releases 1 (1 by release limit); swapped bad frames 3; "
                             "anchor-later 74 of 598 frames (12.4%), longest run 31; tag changes 5"));
    assert(counted.rfind("HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, "
                         "NoBoundaryThisFrame 1); mask coverage avg 4.1% max 9.7%; amplitude 0.008/0.05/0.2 avg "
                         "3.9/2.1/0.4% max 9.2/5.0/1.1%; paste built 598 pasted 1190 skipped 6; "
                         "slot waits 0; boundary: ok; ",
                         0) == 0);

    // The amplitude fields follow the samples: no readback, no amplitude text either
    WindowStats unsampled = w;
    unsampled.coverageSamples = 0;
    unsampled.coverageSum = 0.0;
    assert(SummaryLine(Mode::UiFree, Phase::Active, Reason::None, unsampled, 0, 0, 0, "ok").find("amplitude") ==
           std::string::npos);
    assert(SummaryLine(Mode::UiFree, Phase::Active, Reason::None, unsampled, 0, 0, 0, "ok")
               .find("; mask coverage n/a; paste built 0 pasted 0 skipped 0; ") != std::string::npos);

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
           "built 0 pasted 0 skipped 0; slot waits 0; boundary: ok; guard trips 0 releases 0 (0 by release limit); swapped bad "
           "frames 0; anchor-later 0 of 0 frames (0.0%), longest run 0; tag changes 0");
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
void TestGuardAdvisory()
{
    // The warning compares the RELEASE level (0.75 x the limit), not the limit: a guard that only just beats the HUD area still
    // never lets go, which is the trap a play session fell into (0.05 over a 4.4 % HUD releases at or below 3.75 %).
    assert(GuardAdvisory::kAssumedHudCoverage == 0.15f);
    assert(GuardAdvisory::kMinSamples == 30);
    assert(GuardAdvisory::kSettleMs == 500);
    GuardAdvisory a = EvaluateGuardAdvisory(0.05f, 0.044f, 100);
    assert(a.warn && a.release < a.reference); // 0.05 > 0.044, but 0.0375 < 0.044
    a = EvaluateGuardAdvisory(0.10f, 0.09f, 100);
    assert(a.warn); // 0.10 > 0.09, but 0.075 < 0.09
    assert(!EvaluateGuardAdvisory(0.12f, 0.09f, 100).warn); // release 0.09 = the HUD area: it releases
    assert(EvaluateGuardAdvisory(0.11f, 0.09f, 100).warn);
    // The tolerance is a hundredth of a slider step: a release level that lands one float ulp under the area is not a trap
    assert(!EvaluateGuardAdvisory(0.12f, 0.12f * CoverageGuard::kReleaseFraction, 100).warn);
    // A release level exactly at the tolerance above the area warns too (the comparison is strict: reaching it releases)
    const float boundary = 0.12f * CoverageGuard::kReleaseFraction + kGuardLevelTolerance;
    assert(!EvaluateGuardAdvisory(0.12f, boundary, 100).warn);
    assert(EvaluateGuardAdvisory(0.12f, boundary + 1e-3f, 100).warn);

    // Nothing measured yet: the assumed HUD level (15%) is compared, so a low limit is still caught before the first readbacks
    a = EvaluateGuardAdvisory(0.05f, -1.f, 0);
    assert(a.warn && a.measured < 0.f && a.reference == 0.15f && a.samples == 0);
    a = EvaluateGuardAdvisory(0.20f, -1.f, 0);
    assert(!a.warn && a.reference == 0.15f); // 0.20 > 0.15 and its release level 0.15 reaches it
    a = EvaluateGuardAdvisory(0.19f, -1.f, 0);
    assert(a.warn && a.release < a.reference); // 0.19 > 0.15 is not enough: its release level is 0.1425
    assert(!EvaluateGuardAdvisory(0.20f, -1.f, 0).warn); // 0.20's release level is 0.15, the assumed area

    // Too few samples are not a measurement: the assumed level is still used
    a = EvaluateGuardAdvisory(0.05f, 0.09f, GuardAdvisory::kMinSamples - 1);
    assert(a.warn && a.measured < 0.f && a.samples == GuardAdvisory::kMinSamples - 1);
    // ... and from the first trusted sample on, the measurement decides
    a = EvaluateGuardAdvisory(0.05f, 0.09f, GuardAdvisory::kMinSamples);
    assert(a.warn && a.measured == 0.09f && a.reference == 0.09f && a.samples == GuardAdvisory::kMinSamples);
    assert(!EvaluateGuardAdvisory(0.12f, 0.09f, 100).warn);
    assert(EvaluateGuardAdvisory(0.119f, 0.09f, 100).warn);
    // 0 = guard off: nothing can trip, so nothing warns (and a measured value does not matter)
    assert(!EvaluateGuardAdvisory(0.f, 0.30f, 100).warn && !EvaluateGuardAdvisory(0.f, -1.f, 0).warn);

    // The advice names a value that actually releases: the smallest two-decimal limit whose release level reaches the HUD area
    assert(ReleaseLimitFor(0.09f) == 0.12f);
    assert(ReleaseLimitFor(0.044f) == 0.06f);
    assert(ReleaseLimitFor(0.15f) == 0.20f);
    assert(ReleaseLimitFor(0.01f) == 0.02f);
    assert(ReleaseLimitFor(0.09f) * CoverageGuard::kReleaseFraction + kGuardLevelTolerance >= 0.09f);
    assert((ReleaseLimitFor(0.09f) - 0.01f) * CoverageGuard::kReleaseFraction + kGuardLevelTolerance < 0.09f); // the step below does not
    assert(EvaluateGuardAdvisory(0.05f, 0.09f, 100).advice == ReleaseLimitFor(0.09f));
    // The advice really releases: no warning at the value it names, one at the step below
    assert(!EvaluateGuardAdvisory(ReleaseLimitFor(0.09f), 0.09f, 100).warn);
    assert(EvaluateGuardAdvisory(ReleaseLimitFor(0.09f) - 0.01f, 0.09f, 100).warn);

    // The share texts the brief's sentence is built from
    assert(ShareText(0.05f) == "5%");
    assert(ShareText(0.6f) == "60%");
    assert(ShareText(0.05f * CoverageGuard::kReleaseFraction) == "3.8%");
    assert(ShareText(0.09f) == "9%");

    // The status line: the limit, the release level and why a present falls back. The density policy releases after its clean run,
    // so its sentence names that instead of "release below".
    assert(GuardStatusLine(0.05f, true) == "over the guard limit 5%, falling back; release below 3.8%");
    assert(GuardStatusLine(0.60f, false) == "guard limit 60%, release below 45%");
    assert(GuardStatusLine(0.05f, false) == "guard limit 5%, release below 3.8%");
    assert(GuardStatusLine(0.f, false) == "Coverage guard: off, no limit");
    assert(GuardStatusLine(0.05f, false, GuardPolicy::Density) ==
           "guard limit 5%, releases after 60 clean readbacks at or below 3.8%");
    assert(GuardStatusLine(0.05f, true, GuardPolicy::Density) ==
           "over the guard limit 5%, falling back; releases after 60 clean readbacks at or below 3.8%");

    // The menu warning names the limit, its release level, where the number came from, the value to set and that the user's value is
    // left alone
    const std::string menu = GuardAdvisoryMenuLine(EvaluateGuardAdvisory(0.05f, 0.09f, 120));
    assert(menu.find("coverage guard limit (5%) releases only at or below 3.8%") != std::string::npos);
    assert(menu.find("the HUD's flagged area is 9% (the low fifth of the last 120 readbacks while UI-free ran)") != std::string::npos);
    assert(menu.find("raise the limit to " + ShareText(ReleaseLimitFor(0.09f)) + " or more") != std::string::npos);
    assert(menu.find("Your value is not changed for you.") != std::string::npos);
    assert(menu.find("Reset advanced") != std::string::npos && menu.find("60%") != std::string::npos);
    assert(GuardAdvisoryMenuLine(EvaluateGuardAdvisory(0.60f, 0.09f, 120)).empty());
    assert(GuardAdvisoryMenuLine(EvaluateGuardAdvisory(0.05f, -1.f, 0)).find("no readback has been measured yet") !=
           std::string::npos);
    // Above three quarters of the screen no limit can release, and the advice says so instead of naming one
    const std::string hopeless = GuardAdvisoryMenuLine(EvaluateGuardAdvisory(0.05f, 0.80f, 120));
    assert(hopeless.find("no limit releases this mask") != std::string::npos);

    // The log line: one per settled change. It says the same numbers and that the value was not changed.
    const std::string log = GuardAdvisoryLogLine(EvaluateGuardAdvisory(0.05f, 0.09f, 120));
    assert(log.rfind("HudLess: coverage guard 5% releases only at or below 3.8%, below the HUD it must let through", 0) == 0);
    assert(log.find("measured 9% as the low fifth of 120 readbacks") != std::string::npos);
    assert(log.find("raise the limit to " + ShareText(ReleaseLimitFor(0.09f)) + " or more") != std::string::npos);
    assert(log.find("the value is not changed") != std::string::npos);
    assert(GuardAdvisoryLogLine(EvaluateGuardAdvisory(0.05f, -1.f, 0)).find("assuming 15%") != std::string::npos);
    assert(GuardAdvisoryLogLine(EvaluateGuardAdvisory(0.60f, 0.09f, 120)) ==
           "HudLess: coverage guard 60% can release over the HUD's flagged area again (9%): the guard warning clears");
    assert(GuardAdvisoryLogLine(EvaluateGuardAdvisory(ReleaseLimitFor(0.09f), 0.09f, 120))
               .find("can release over the HUD's flagged area again") != std::string::npos);
    assert(GuardAdvisoryLogLine(EvaluateGuardAdvisory(0.f, 0.09f, 120)) ==
           "HudLess: the coverage guard is off: the guard-limit warning clears");

    // The U paste gate: with the tint off it IS PasteVisible (byte for byte today); the tint only opens it where it has an image to
    // draw (a staged, producing present), never on one that invalidates the ring
    assert(PasteGateOpen(false, false) == false);
    assert(PasteGateOpen(true, false) == true);
    assert(PasteGateOpen(false, true) == true); // the guard is tripped: only the tint is drawn
    assert(PasteGateOpen(true, true) == true);
}

void TestHudCoverageWindow()
{
    // A recent window, not a lifetime mean: the oldest readback leaves, so a changed scene and a changed mask are followed
    HudCoverageWindow window;
    assert(window.Samples() == 0 && window.Estimate() < 0.f);
    window.Add(0.09f);
    assert(window.Samples() == 1 && window.Estimate() == 0.09f);
    for (size_t i = 1; i < HudCoverageWindow::kSamples; ++i)
        window.Add(0.09f);
    assert(window.Samples() == HudCoverageWindow::kSamples);
    assert(std::fabs(window.Estimate() - 0.09f) < 1e-6f);

    // A fade peak is a minority of the window and does not lift the estimate
    for (size_t i = 0; i < HudCoverageWindow::kSamples / 10; ++i)
        window.Add(0.95f);
    assert(std::fabs(window.Estimate() - 0.09f) < 1e-6f);
    // ... a persistent change does, once it is more than a fifth of the window
    for (size_t i = 0; i < HudCoverageWindow::kSamples; ++i)
        window.Add(0.30f);
    assert(std::fabs(window.Estimate() - 0.30f) < 1e-6f);

    // The low fifth of a small window is its lowest value, and of an ordered one the value a fifth of the way in
    HudCoverageWindow small;
    small.Add(0.05f);
    small.Add(0.02f);
    small.Add(0.09f);
    assert(small.Estimate() == 0.02f);
    HudCoverageWindow ordered;
    for (int i = 1; i <= 10; ++i)
        ordered.Add(static_cast<float>(i) / 100.f);
    assert(ordered.Estimate() == 0.02f); // kPercentile 0.20 of 10 samples is index 1

    // Ties (the same value twice, a value that equals the one leaving) never lose an entry: the window keeps one per sample
    HudCoverageWindow ties;
    for (size_t i = 0; i < HudCoverageWindow::kSamples + 3; ++i)
        ties.Add(0.07f);
    assert(ties.Samples() == HudCoverageWindow::kSamples && ties.Estimate() == 0.07f);
    // Reset empties it (a mode change, a mask change)
    ties.Reset();
    assert(ties.Samples() == 0 && ties.Estimate() < 0.f);
}

} // namespace

int main()
{
    TestReasons();
    TestStateTracker();
    TestCoverageGuard();
    TestCoverageGuardRelease();
    TestCoverageGuardStarvation();
    TestDensityGuard();
    TestDensityReleaseLimit();
    TestDensityAnchorTrip();
    TestGuardEdges();
    TestSlotRing();
    TestFormats();
    TestGuardAdvisory();
    TestHudCoverageWindow();
    std::puts("PASS: reasons, state tracker hysteresis, coverage guard (trip, release, starvation, unknown, release limit, per-frame trip), "
              "guard-limit advisory and paste gate, slot ring fences, log formats");
    return 0;
}
