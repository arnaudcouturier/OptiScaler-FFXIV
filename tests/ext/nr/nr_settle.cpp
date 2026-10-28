// The settle decision of the NR pass (NrSettle.h): which layout runs while a size change is pending, what a change costs
// (nothing, a history reset, a partial park, a rebuild) and the 500 ms boundary.
#include "ext/nr/NrSettle.h"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using namespace DlssNr::Settle;

Identity Id(uint32_t fw, uint32_t fh, uint32_t ww, uint32_t wh, uint64_t key)
{
    Identity i;
    i.frameW = fw;
    i.frameH = fh;
    i.workW = ww;
    i.workH = wh;
    i.layoutKey = key;
    return i;
}

const Identity kApplied = Id(3840, 2160, 3328, 1952, 11);

void IdentityEquality()
{
    CHECK(kApplied == Id(3840, 2160, 3328, 1952, 11));
    CHECK(!(kApplied == Id(3841, 2160, 3328, 1952, 11)));
    CHECK(!(kApplied == Id(3840, 2161, 3328, 1952, 11)));
    CHECK(!(kApplied == Id(3840, 2160, 3329, 1952, 11)));
    CHECK(!(kApplied == Id(3840, 2160, 3328, 1953, 11)));
    CHECK(!(kApplied == Id(3840, 2160, 3328, 1952, 12)));
}

void NothingChanged()
{
    State s;
    CHECK(Decide(s, kApplied, kApplied, 1000) == Action::Keep && !s.pending);
    // a pending request that goes back to the applied one is dropped
    const Identity request = Id(3840, 2160, 3200, 1900, 12);
    CHECK(Decide(s, kApplied, request, 1000) == Action::Wait && s.pending);
    CHECK(Decide(s, kApplied, kApplied, 1200) == Action::Keep && !s.pending);
    // and the next request starts its own timer, not the dropped one's
    CHECK(Decide(s, kApplied, request, 1400) == Action::Wait && s.since == 1400);
    CHECK(Decide(s, kApplied, request, 1899) == Action::Wait);
    CHECK(Decide(s, kApplied, request, 1900) == Action::Rebuild);
}

void FrameSizeChangeIsImmediate()
{
    State s;
    const Identity smaller = Id(2560, 1440, 3328, 1952, 11);
    CHECK(Decide(s, kApplied, smaller, 0) == Action::RebuildNow && !s.pending);
    // both sizes changed: still immediate, and a pending working-size request is dropped
    const Identity pendingRequest = Id(3840, 2160, 3200, 1900, 12);
    CHECK(Decide(s, kApplied, pendingRequest, 100) == Action::Wait && s.pending);
    CHECK(Decide(s, kApplied, Id(1920, 1080, 1664, 976, 13), 150) == Action::RebuildNow && !s.pending);
    CHECK(Decide(s, kApplied, Id(3840, 2161, 3328, 1952, 11), 160) == Action::RebuildNow);
    CHECK(Decide(s, kApplied, Id(3841, 2160, 3328, 1952, 11), 160) == Action::RebuildNow);
    // nothing applied yet (the first frame): the frame size differs from zero
    CHECK(Decide(s, Identity {}, kApplied, 170) == Action::RebuildNow);
}

void WorkingSizeChangeWaitsTheSettleTime()
{
    State s;
    const Identity request = Id(3840, 2160, 3200, 1900, 12);
    CHECK(kSettleMs == 500);
    CHECK(Decide(s, kApplied, request, 10000) == Action::Wait && s.pending && s.since == 10000);
    CHECK(Decide(s, kApplied, request, 10001) == Action::Wait);
    CHECK(Decide(s, kApplied, request, 10250) == Action::Wait);
    CHECK(Decide(s, kApplied, request, 10499) == Action::Wait);
    CHECK(Decide(s, kApplied, request, 10500) == Action::Rebuild && !s.pending); // held for exactly 500 ms
    // after the rebuild the request is the applied one
    CHECK(Decide(s, request, request, 10501) == Action::Keep);
    // later than the boundary
    State t;
    CHECK(Decide(t, kApplied, request, 0) == Action::Wait);
    CHECK(Decide(t, kApplied, request, 501) == Action::Rebuild);
    State u;
    CHECK(Decide(u, kApplied, request, 0) == Action::Wait);
    CHECK(Decide(u, kApplied, request, 5000) == Action::Rebuild);
    // either axis of the working size counts
    State w;
    CHECK(Decide(w, kApplied, Id(3840, 2160, 3328, 1900, 12), 0) == Action::Wait);
    CHECK(Decide(w, kApplied, Id(3840, 2160, 3328, 1900, 12), 600) == Action::Rebuild);
    State v;
    CHECK(Decide(v, kApplied, Id(3840, 2160, 3200, 1952, 12), 0) == Action::Wait);
    CHECK(Decide(v, kApplied, Id(3840, 2160, 3200, 1952, 12), 600) == Action::Rebuild);
    // a custom settle time
    State c;
    CHECK(Decide(c, kApplied, request, 0, false, 100) == Action::Wait);
    CHECK(Decide(c, kApplied, request, 99, false, 100) == Action::Wait);
    CHECK(Decide(c, kApplied, request, 100, false, 100) == Action::Rebuild);
}

void ANewRequestRestartsTheTimer()
{
    State s;
    const Identity a = Id(3840, 2160, 3200, 1900, 12);
    const Identity b = Id(3840, 2160, 3100, 1800, 13);
    CHECK(Decide(s, kApplied, a, 0) == Action::Wait);
    CHECK(Decide(s, kApplied, a, 400) == Action::Wait);
    CHECK(Decide(s, kApplied, b, 450) == Action::Wait && s.since == 450); // a different request: the clock restarts
    CHECK(Decide(s, kApplied, b, 800) == Action::Wait);
    CHECK(Decide(s, kApplied, b, 949) == Action::Wait);
    CHECK(Decide(s, kApplied, b, 950) == Action::Rebuild);
    // the same working size under another layout key is a different request too
    State t;
    const Identity c = Id(3840, 2160, 3200, 1900, 14);
    CHECK(Decide(t, kApplied, a, 0) == Action::Wait);
    CHECK(Decide(t, kApplied, c, 300) == Action::Wait && t.since == 300);
    CHECK(Decide(t, kApplied, c, 700) == Action::Wait);
    CHECK(Decide(t, kApplied, c, 800) == Action::Rebuild);
}

void WarpOnlyChangeResetsHistory()
{
    State s;
    const Identity warp = Id(3840, 2160, 3328, 1952, 99);
    CHECK(Decide(s, kApplied, warp, 5) == Action::HistoryReset && !s.pending); // at once: no timer for it
    CHECK(Decide(s, kApplied, warp, 5000000) == Action::HistoryReset);
    // a pending working-size request that turns into a warp-only one
    const Identity request = Id(3840, 2160, 3200, 1900, 12);
    CHECK(Decide(s, kApplied, request, 0) == Action::Wait && s.pending);
    CHECK(Decide(s, kApplied, warp, 100) == Action::HistoryReset && !s.pending);
}

void ImmediateOverridesTheWait()
{
    State s;
    const Identity request = Id(3840, 2160, 3200, 1900, 12);
    CHECK(Decide(s, kApplied, request, 0, true) == Action::RebuildNow && !s.pending);
    CHECK(Decide(s, kApplied, request, 1, false) == Action::Wait);
    CHECK(Decide(s, kApplied, request, 2, true) == Action::RebuildNow && !s.pending);
    CHECK(Decide(s, kApplied, kApplied, 3, true) == Action::Keep);
    CHECK(Decide(s, kApplied, Id(3840, 2160, 3328, 1952, 99), 4, true) == Action::HistoryReset);
}

void OnlyAdoptingActionsTakeTheRequestedLayout()
{
    CHECK(!AdoptsRequested(Action::Keep) && !AdoptsRequested(Action::Wait));
    CHECK(AdoptsRequested(Action::HistoryReset) && AdoptsRequested(Action::Rebuild) && AdoptsRequested(Action::RebuildNow));
    // every consumer reads through Pick: the applied thing while waiting or unchanged, the requested otherwise
    const std::string applied = "applied", requested = "requested";
    CHECK(&Pick(Action::Wait, applied, requested) == &applied);
    CHECK(&Pick(Action::Keep, applied, requested) == &applied);
    CHECK(&Pick(Action::HistoryReset, applied, requested) == &requested);
    CHECK(&Pick(Action::Rebuild, applied, requested) == &requested);
    CHECK(&Pick(Action::RebuildNow, applied, requested) == &requested);
    // a layout-like value: while waiting no consumer sees the new one
    struct Layout { uint32_t workW, workH; uint64_t key; };
    const Layout was { 3328, 1952, 11 }, want { 3200, 1900, 12 };
    State s;
    const Action act = Decide(s, Id(3840, 2160, was.workW, was.workH, was.key), Id(3840, 2160, want.workW, want.workH, want.key), 0);
    CHECK(act == Action::Wait);
    const Layout& used = Pick(act, was, want);
    CHECK(used.workW == 3328 && used.workH == 1952 && used.key == 11);
}

void PartialPark()
{
    // a warp-only change parks nothing; a working-size change parks the working-size scratch only; a frame-size change parks all
    CHECK(ParkScopeFor(kApplied, Id(3840, 2160, 3328, 1952, 99)) == ParkScope::None);
    CHECK(ParkScopeFor(kApplied, kApplied) == ParkScope::None);
    CHECK(ParkScopeFor(kApplied, Id(3840, 2160, 3200, 1952, 11)) == ParkScope::WorkSize);
    CHECK(ParkScopeFor(kApplied, Id(3840, 2160, 3328, 1900, 11)) == ParkScope::WorkSize);
    CHECK(ParkScopeFor(kApplied, Id(2560, 2160, 3328, 1952, 11)) == ParkScope::All);
    CHECK(ParkScopeFor(kApplied, Id(3840, 1440, 3328, 1952, 11)) == ParkScope::All);
    CHECK(ParkScopeFor(kApplied, Id(2560, 1440, 2208, 1312, 11)) == ParkScope::All);
    CHECK(!ParksFrameScratch(ParkScope::None) && !ParksWorkScratch(ParkScope::None));
    CHECK(!ParksFrameScratch(ParkScope::WorkSize) && ParksWorkScratch(ParkScope::WorkSize));
    CHECK(ParksFrameScratch(ParkScope::All) && ParksWorkScratch(ParkScope::All));
}

// The ordinary working grid sizes the unpack scratch; with the model size unchanged, a change of it alone is still a work-size
// change (wait, then rebuild, park the work-size scratch), not a warp-only history reset.
void OrdinarySizeOnlyChangeIsAWorkSizeChange()
{
    Identity a = Id(3840, 2160, 3328, 1952, 11);
    a.ordW = 1728;
    a.ordH = 972;
    Identity b = a;
    b.ordW = 1744;
    b.ordH = 981;
    CHECK(!(a == b) && WorkSizeDiffers(a, b));
    State s;
    CHECK(Decide(s, a, b, 100) == Action::Wait && s.pending);
    CHECK(Decide(s, a, b, 599) == Action::Wait);
    CHECK(Decide(s, a, b, 600) == Action::Rebuild && !s.pending);
    CHECK(ParkScopeFor(a, b) == ParkScope::WorkSize);
    // the same ordinary size with only the warp key changed stays a history reset
    Identity warp = a;
    warp.layoutKey = 12;
    State t;
    CHECK(!WorkSizeDiffers(a, warp) && Decide(t, a, warp, 5) == Action::HistoryReset);
    CHECK(ParkScopeFor(a, warp) == ParkScope::None);
    // each of the four sizes counts on its own
    Identity w = a;
    w.workW += 2;
    CHECK(WorkSizeDiffers(a, w));
    w = a;
    w.workH += 2;
    CHECK(WorkSizeDiffers(a, w));
    w = a;
    w.ordW += 2;
    CHECK(WorkSizeDiffers(a, w));
    w = a;
    w.ordH += 2;
    CHECK(WorkSizeDiffers(a, w));
    CHECK(!WorkSizeDiffers(a, a));
    // the pass's staleness rule for the running model's work-size scratch: the model's size or the ordinary grid
    CHECK(!WorkScratchStale(a, 3328, 1952, 1728, 972));
    CHECK(WorkScratchStale(a, 3326, 1952, 1728, 972));
    CHECK(WorkScratchStale(a, 3328, 1950, 1728, 972));
    CHECK(WorkScratchStale(a, 3328, 1952, 1744, 972));
    CHECK(WorkScratchStale(a, 3328, 1952, 1728, 981));
}
} // namespace

int main()
{
    OrdinarySizeOnlyChangeIsAWorkSizeChange();
    IdentityEquality();
    NothingChanged();
    FrameSizeChangeIsImmediate();
    WorkingSizeChangeWaitsTheSettleTime();
    ANewRequestRestartsTheTimer();
    WarpOnlyChangeResetsHistory();
    ImmediateOverridesTheWait();
    OnlyAdoptingActionsTakeTheRequestedLayout();
    PartialPark();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR settle decisions (keep, wait, history reset, rebuild, immediate rebuild, 500 ms boundary, partial park)");
    return 0;
}
