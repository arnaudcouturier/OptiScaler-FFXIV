#include "check.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaNrRules.h"

#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ext::ffxiv::chara::nr;

namespace
{
GateFacts Good()
{
    GateFacts f;
    f.configOn = true;
    f.bridgeUp = true;
    f.nvidia = Probe::Present;
    f.forwarder = Probe::Present;
    f.model = Probe::Present;
    return f;
}

void TestAvailability()
{
    CHECK(Evaluate(Good()) == Availability::Available);
    GateFacts f = Good();
    f.nvidia = f.forwarder = f.model = Probe::Unknown; // not looked at yet: the module tries
    CHECK(Evaluate(f) == Availability::Available);

    f = Good(); f.configOn = false;
    CHECK(Evaluate(f) == Availability::ConfigOff);
    f = Good(); f.latched = true;
    CHECK(Evaluate(f) == Availability::Latched);
    f = Good(); f.bridgeUp = false;
    CHECK(Evaluate(f) == Availability::BridgeNotUp);
    f = Good(); f.bridgeSuspended = true;
    CHECK(Evaluate(f) == Availability::BridgeShuttingDown);
    f = Good(); f.releasePending = true;
    CHECK(Evaluate(f) == Availability::ReleasePending);
    f = Good(); f.nvidia = Probe::Missing;
    CHECK(Evaluate(f) == Availability::NotNvidia);
    f = Good(); f.forwarder = Probe::Missing;
    CHECK(Evaluate(f) == Availability::ForwarderMissing);
    f = Good(); f.model = Probe::Missing;
    CHECK(Evaluate(f) == Availability::ModelMissing);

    // precedence: config off beats everything, a latch beats the environment, the bridge beats the card, the card the files
    f = Good(); f.configOn = false; f.latched = true; f.bridgeUp = false;
    CHECK(Evaluate(f) == Availability::ConfigOff);
    f = Good(); f.latched = true; f.bridgeUp = false;
    CHECK(Evaluate(f) == Availability::Latched);
    f = Good(); f.bridgeUp = false; f.nvidia = Probe::Missing;
    CHECK(Evaluate(f) == Availability::BridgeNotUp);
    f = Good(); f.releasePending = true; f.nvidia = Probe::Missing; // old objects still held: that comes before the card and the files
    CHECK(Evaluate(f) == Availability::ReleasePending);
    f = Good(); f.bridgeSuspended = true; f.releasePending = true;
    CHECK(Evaluate(f) == Availability::BridgeShuttingDown);
    f = Good(); f.nvidia = Probe::Missing; f.forwarder = Probe::Missing; f.model = Probe::Missing;
    CHECK(Evaluate(f) == Availability::NotNvidia);
    f = Good(); f.forwarder = Probe::Missing; f.model = Probe::Missing;
    CHECK(Evaluate(f) == Availability::ForwarderMissing);

    // every unavailable reason has text; available and config-off have none
    for (Availability a : { Availability::Latched, Availability::BridgeNotUp, Availability::BridgeShuttingDown, Availability::ReleasePending, Availability::NotNvidia,
                            Availability::ForwarderMissing, Availability::ModelMissing })
        CHECK(std::string(ReasonText(a)).size() > 10 && WorthLogging(a));
    CHECK(std::string(ReasonText(Availability::Available)).empty() && std::string(ReasonText(Availability::ConfigOff)).empty());
    CHECK(!WorthLogging(Availability::Available) && !WorthLogging(Availability::ConfigOff));
}

void TestStateLog()
{
    StateLog log;
    CHECK(!log.ShouldLog(Availability::ConfigOff));    // unchanged
    CHECK(!log.ShouldLog(Availability::Available));    // nothing was wrong before
    CHECK(log.ShouldLog(Availability::BridgeNotUp));   // a reason: one line
    CHECK(!log.ShouldLog(Availability::BridgeNotUp));  // the same reason again: none
    CHECK(log.ShouldLog(Availability::NotNvidia));     // another reason
    CHECK(log.ShouldLog(Availability::Available));     // recovery after a reason
    CHECK(!log.ShouldLog(Availability::ConfigOff));    // switching the feature off is not a fault
    // a flapping state stops logging after the limit
    StateLog flap;
    uint32_t lines = 0;
    for (int i = 0; i < 100; ++i)
        lines += flap.ShouldLog(i % 2 == 0 ? Availability::BridgeNotUp : Availability::Available) ? 1 : 0;
    CHECK(lines == kMaxStateLines);
}

void TestLatches()
{
    // Each latch is independent of the other and reports only the call that set it (the production decisions read them through
    // PlumbingFacts, CharaNrPlumbing.h; chara_nr_plumbing feeds this object into them).
    Latches a;
    CHECK(!a.SrLatched() && !a.NrLatched());
    CHECK(a.LatchSr());
    CHECK(!a.LatchSr()); // only the first call reports
    CHECK(a.SrLatched() && !a.NrLatched());
    Latches b;
    CHECK(b.LatchNr());
    CHECK(!b.LatchNr());
    CHECK(b.NrLatched() && !b.SrLatched());
    CHECK(b.LatchSr()); // the second latch still reports its own first call
    CHECK(b.SrLatched() && b.NrLatched());
}

void TestStreams()
{
    StreamTable t;
    CHECK(t.Bind(0, 576, 960, 0) == kNoStream);
    const int a = t.Bind(0x1000, 576, 960, 100);
    const int b = t.Bind(0x2000, 576, 960, 100);
    CHECK(a == 0 && b == 1 && a != b);
    CHECK(t.Bind(0x1000, 576, 960, 200) == a); // the same image keeps its stream
    CHECK(t.Find(0x2000) == b && t.Find(0x3000) == kNoStream);
    CHECK(t.At(a).width == 576 && t.At(a).height == 960); // the size the stream was bound for
    CHECK(t.UsedCount() == 2);
    CHECK(t.LiveCount(300) == 2); // both within 250 ms of their last live time (100)
    t.At(a).lastLive = 400;
    CHECK(t.LiveCount(500) == 1);
    t.Free(b);
    CHECK(t.Find(0x2000) == kNoStream && t.UsedCount() == 1);
    CHECK(t.Bind(0x4000, 8, 8, 0) == b); // the freed stream is reused
    // a full table
    StreamTable full;
    for (size_t i = 0; i < kMaxStreams; ++i)
        CHECK(full.Bind(0x100 + i, 1, 1, 0) == static_cast<int>(i));
    CHECK(full.Bind(0x9999, 1, 1, 0) == kNoStream);
}

void TestLinger()
{
    CHECK(LingerMs(5) == 5000 && LingerMs(0) == 0 && LingerMs(100000) == kMaxLingerSeconds * 1000ull);
    StreamFacts f;
    f.lastLive = 1000;
    f.lingerMs = LingerMs(5);
    // inside the linger time: kept, also with no GPU work
    f.now = 1000;
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 6000; // exactly 5 s: not longer than the linger time
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 6001;
    CHECK(DecideStream(f) == StreamAction::Release);
    // never released while the GPU may still be using it
    f.hasGpuWork = true;
    f.gpuDone = false;
    CHECK(DecideStream(f) == StreamAction::WaitGpu);
    f.gpuDone = true;
    CHECK(DecideStream(f) == StreamAction::Release);
    // before the linger time has passed the GPU state does not matter
    f.now = 3000;
    f.gpuDone = false;
    CHECK(DecideStream(f) == StreamAction::Keep);
    // disabled: no linger, still waits for the GPU
    f.disabled = true;
    CHECK(DecideStream(f) == StreamAction::WaitGpu);
    f.gpuDone = true;
    CHECK(DecideStream(f) == StreamAction::Release);
    // a clock that went backwards keeps the stream
    f = StreamFacts {};
    f.lastLive = 5000;
    f.now = 1000;
    CHECK(DecideStream(f) == StreamAction::Keep);
    // a linger time of 0 releases a window that was closed, not one that is open: an open window is drawn every few milliseconds, and
    // a stream released between two draws would be rebuilt for the next one
    f.lingerMs = LingerMs(0);
    f.lastLive = 1000;
    f.now = 1001;
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 1000 + 16; // drawn 16 ms ago, linger 0: still open
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 1000 + kLiveMs; // exactly the live window
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 1000 + kLiveMs + 1; // not drawn for longer than that: closed
    CHECK(DecideStream(f) == StreamAction::Release);
    // any linger below the live window behaves the same; a longer one is kept as configured
    f.lingerMs = 100;
    f.now = 1000 + 200;
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.lingerMs = LingerMs(1);
    f.now = 1000 + 1000;
    CHECK(DecideStream(f) == StreamAction::Keep);
    f.now = 1000 + 1001;
    CHECK(DecideStream(f) == StreamAction::Release);
    // disabled releases at once whatever the grace
    f.disabled = true;
    f.now = 1001;
    CHECK(DecideStream(f) == StreamAction::Release);
}

void TestReset()
{
    CHECK(DecideReset(false, 0, 1000) == ResetReason::FirstRun); // a new or rebuilt stream
    CHECK(DecideReset(true, 1000, 1100) == ResetReason::None);
    CHECK(DecideReset(true, 1000, 1250) == ResetReason::None);   // exactly the gap
    CHECK(DecideReset(true, 1000, 1251) == ResetReason::Gap);    // window closed and reopened
    CHECK(DecideReset(true, 5000, 1000) == ResetReason::Gap);    // clock went backwards
    // the super-resolution evaluate of the same render reset its history (a skipped, rejected or stale render before it): the NR history restarts
    CHECK(DecideReset(true, 1000, 1100, true) == ResetReason::SuperResolution);
    CHECK(DecideReset(true, 1000, 1100, false) == ResetReason::None);
    CHECK(DecideReset(false, 0, 1000, true) == ResetReason::FirstRun); // the older reasons keep their meaning and order
    CHECK(DecideReset(true, 1000, 1251, true) == ResetReason::Gap);
    CHECK(std::string(ResetReasonText(ResetReason::SuperResolution)) == "after a super-resolution reset");
}

struct FakeOps
{
    std::vector<std::string> calls;
    HandoffStep failAt = HandoffStep::Wait11;
    bool fail = false;
    FenceCounter done; // the done fence's own counter (the module's completion counter)
    bool Step(HandoffStep s, const std::string& name, uint64_t v)
    {
        calls.push_back(name + (v != 0 ? std::to_string(v) : ""));
        return !(fail && failAt == s);
    }
    bool WaitColourFree(uint64_t v) { return Step(HandoffStep::ReuseWait, "WCF:", v); }
    void CopyColour() { calls.push_back("COPY"); }
    bool Signal11(uint64_t v) { return Step(HandoffStep::Signal11, "S11:", v); }
    bool Wait12(uint64_t v) { return Step(HandoffStep::Wait12, "W12:", v); }
    bool Record(uint64_t& d)
    {
        calls.push_back("REC");
        if (fail && failAt == HandoffStep::Record)
            return false; // recording failed: no done value was taken
        d = done.Next();
        calls.push_back("S12:" + std::to_string(d));
        return true;
    }
    bool Wait11(uint64_t v) { return Step(HandoffStep::Wait11, "W11:", v); }
    bool Has(const char* prefix) const
    {
        for (const auto& c : calls)
        {
            if (c.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }
};

void TestHandoff()
{
    // each side has its own counter: the D3D11-only ready fence and the D3D12-only done fence both start at 1
    FenceCounter ready;
    FakeOps ok;
    const HandoffOutcome a = RunHandoff(ok, ready);
    CHECK(a.ok && a.ready == 1 && a.done == 1);
    CHECK((ok.calls == std::vector<std::string> { "S11:1", "W12:1", "REC", "S12:1", "W11:1" }));

    // a failure at any step before the D3D11 wait: the D3D11 wait is never issued, and nothing later runs
    for (HandoffStep step : { HandoffStep::Signal11, HandoffStep::Wait12, HandoffStep::Record })
    {
        FenceCounter c;
        FakeOps f;
        f.fail = true;
        f.failAt = step;
        const HandoffOutcome o = RunHandoff(f, c);
        CHECK(!o.ok && o.failedAt == step);
        CHECK(!f.Has("W11"));
        if (step == HandoffStep::Signal11)
            CHECK(!f.Has("W12") && !f.Has("REC") && !f.Has("S12"));
        if (step == HandoffStep::Wait12)
            CHECK(!f.Has("REC") && !f.Has("S12"));
        if (step == HandoffStep::Record)
            CHECK(!f.Has("S12"));
        CHECK(o.done == 0); // the D3D12 signal was not queued, so there is no value to wait for
    }
    // a failed D3D11 wait is reported (the caller latches); the signal before it was queued
    {
        FenceCounter c;
        FakeOps f;
        f.fail = true;
        f.failAt = HandoffStep::Wait11;
        const HandoffOutcome o = RunHandoff(f, c);
        CHECK(!o.ok && o.failedAt == HandoffStep::Wait11 && o.done == 1);
    }
    // ready values only grow, also across failures: a failed run never lets a later run reuse a value
    FenceCounter c;
    FakeOps bad;
    bad.fail = true;
    bad.failAt = HandoffStep::Record;
    RunHandoff(bad, c);
    FakeOps good;
    const HandoffOutcome next = RunHandoff(good, c);
    CHECK(next.ok && next.ready == 2 && next.done == 1);
    // the late variant does not wait inline: the consumer one frame later waits on `done`
    FenceCounter lateCounter;
    FakeOps late;
    const HandoffOutcome l = RunHandoff(late, lateCounter, false);
    CHECK(l.ok && l.done == 1 && !late.Has("W11"));
    CHECK((late.calls == std::vector<std::string> { "S11:1", "W12:1", "REC", "S12:1" }));
}

// RunFrame: the colour copy and, in the late variant, the wait that frees the colour image it overwrites.
void TestFrame()
{
    // the default variant has one colour image: the inline wait of the previous run already freed it
    {
        FenceCounter ready;
        FakeOps f;
        const HandoffOutcome o = RunFrame(f, ready, false, 7);
        CHECK(o.ok);
        CHECK((f.calls == std::vector<std::string> { "COPY", "S11:1", "W12:1", "REC", "S12:1", "W11:1" })); // no wait before the copy
    }
    // the late variant waits for the run that last read this colour image (two runs ago) before it overwrites it
    {
        FenceCounter ready;
        FakeOps f;
        const HandoffOutcome o = RunFrame(f, ready, true, 7);
        CHECK(o.ok);
        CHECK((f.calls == std::vector<std::string> { "WCF:7", "COPY", "S11:1", "W12:1", "REC", "S12:1" }));
        // the first two runs have nothing to wait for
        FakeOps first;
        FenceCounter r2;
        RunFrame(first, r2, true, 0);
        CHECK(!first.Has("WCF"));
    }
    // a failing wait stops everything: no copy, no signal
    {
        FenceCounter ready;
        FakeOps f;
        f.fail = true;
        f.failAt = HandoffStep::ReuseWait;
        const HandoffOutcome o = RunFrame(f, ready, true, 7);
        CHECK(!o.ok && o.failedAt == HandoffStep::ReuseWait && o.done == 0 && o.ready == 0);
        CHECK(!f.Has("COPY") && !f.Has("S11") && !f.Has("REC"));
        CHECK(ready.last == 0); // nothing was signalled: no ready value was taken
    }
    CHECK(ColourReuseWait(false, 9) == 0 && ColourReuseWait(true, 9) == 9 && ColourReuseWait(true, 0) == 0);
}

struct ConsumerOps
{
    std::vector<uint64_t> waits;
    bool ok = true;
    bool WaitResult(uint64_t v)
    {
        waits.push_back(v);
        return ok;
    }
};

void TestConsumerBuffer()
{
    // chain: runs since the history was last reset; for a stream that never reset it equals runs
    CHECK(ConsumerBuffer(false, 0, 0) == -1 && ConsumerBuffer(true, 0, 0) == -1);
    CHECK(ConsumerBuffer(false, 1, 1) == 0); // the first run wrote buffer 0
    CHECK(ConsumerBuffer(false, 2, 2) == 1 && ConsumerBuffer(false, 3, 3) == 0);
    CHECK(ConsumerBuffer(true, 1, 1) == -1); // late: the first result is shown one frame later
    CHECK(ConsumerBuffer(true, 2, 2) == 0);  // late: the result of run 0 while run 1 is in flight
    CHECK(ConsumerBuffer(true, 3, 3) == 1);
    // the late consumer never reads the buffer the newest run writes
    for (uint64_t runs = 2; runs < 20; ++runs)
        CHECK(ConsumerBuffer(true, runs, runs) != ResultBufferForRun(runs - 1));
    // the newest run writes the buffer the previous consumer did not use
    for (uint64_t runs = 1; runs < 20; ++runs)
        CHECK(ConsumerBuffer(false, runs, runs) == ResultBufferForRun(runs - 1));
    // the late variant after a gap or a rebuild: the run before the newest belongs to an earlier chain and is not shown
    CHECK(ConsumerBuffer(true, 9, 1) == -1);
    CHECK(ConsumerBuffer(true, 9, 2) == ResultBufferForRun(7));
    CHECK(ConsumerBuffer(false, 9, 1) == ResultBufferForRun(8)); // the default variant shows the newest run whatever the chain

    // what a consumer waits for
    CHECK(ConsumerWaitValue(false, 5) == 0 && ConsumerWaitValue(true, 5) == 5 && ConsumerWaitValue(true, 0) == 0);
    ConsumerOps c;
    CHECK(ConsumerWait(c, false, 5) && c.waits.empty()); // the default variant waited inline
    CHECK(ConsumerWait(c, true, 5) && c.waits == std::vector<uint64_t> { 5 });
    c.ok = false;
    CHECK(!ConsumerWait(c, true, 6));
}

// A bring-up call that does not produce a result makes the older result stale (it belongs to an earlier render).
void TestResultCurrent()
{
    StreamState st;
    CHECK(!st.ResultIsCurrent());
    ++st.attempts; // the first call: preparing, no result yet
    CHECK(!st.ResultIsCurrent());
    ++st.attempts; // a run
    st.everRan = true;
    st.ranAttempt = st.attempts;
    CHECK(st.ResultIsCurrent());
    ++st.attempts; // the next render was skipped (no free allocator) or failed: the result in the stream is older
    CHECK(!st.ResultIsCurrent());
    ++st.attempts; // and the render after it ran again
    st.ranAttempt = st.attempts;
    CHECK(st.ResultIsCurrent());
    StreamState never;
    never.everRan = true; // ran flag without an attempt record is not current
    CHECK(!never.ResultIsCurrent());
}

// A render of the image for which the module produced nothing (the dispatcher never reached the bring-up: finisher rejected,
// nobody shows the window, the SR result won) withdraws the older result; the late variant does not reach back over it either.
void TestRenderWithoutResult()
{
    StreamTable table;
    const uintptr_t key = 0x1000;
    const int index = table.Bind(key, 576, 960, 100);
    StreamState& st = table.At(index);

    // two runs of the same chain (the late variant needs both)
    for (int render = 0; render < 2; ++render)
    {
        ++st.attempts;
        st.everRan = true;
        st.ranAttempt = st.attempts;
        ++st.runs;
        st.chain = st.chain + 1;
    }
    CHECK(st.ResultIsCurrent() && ConsumerBuffer(false, st.runs, st.chain) >= 0 && ConsumerBuffer(true, st.runs, st.chain) >= 0);

    // the next render's finisher is rejected: the dispatcher marks the image, no bring-up call was made
    CHECK(InvalidateResult(table, key));
    CHECK(!st.ResultIsCurrent());                // ResultFor / ResultTexture answer null for this render
    CHECK(ConsumerBuffer(true, st.runs, st.chain) == -1); // and the late variant does not show the run before it either
    CHECK(ConsumerBuffer(false, st.runs, st.chain) >= 0); // (the chain only matters to the late variant; ResultIsCurrent guards the rest)

    // the next render runs again: current again, but the late variant needs a second run of the new chain
    ++st.attempts;
    st.ranAttempt = st.attempts;
    ++st.runs;
    st.chain = st.chain + 1;
    CHECK(st.ResultIsCurrent());
    CHECK(ConsumerBuffer(true, st.runs, st.chain) == -1); // would be the run from before the rejected render
    ++st.attempts;
    st.ranAttempt = st.attempts;
    ++st.runs;
    st.chain = st.chain + 1;
    CHECK(ConsumerBuffer(true, st.runs, st.chain) == ResultBufferForRun(st.runs - 2)); // two runs after the gap: shown again

    // an image without a stream is not marked; marking never creates one
    CHECK(!InvalidateResult(table, 0x2000));
    CHECK(table.UsedCount() == 1);

    // a bring-up call that ended without a run breaks the chain (but is not counted again: it counted itself when it started)
    const uint64_t attemptsBefore = st.attempts;
    st.chain = 5;
    NoteBringUpEnded(table, key, true);
    CHECK(st.chain == 5);
    NoteBringUpEnded(table, key, false);
    CHECK(st.chain == 0 && st.attempts == attemptsBefore);
    NoteBringUpEnded(table, 0x2000, false); // unknown image: nothing happens
    CHECK(table.UsedCount() == 1);
}

// A size, preset, style or variant change must hold still before the stream is rebuilt.
void TestRebuild()
{
    StreamShape a;
    a.width = 576;
    a.height = 960;
    a.preset = 2;
    a.style = 1;
    StreamState st;
    CHECK(DecideRebuild(st, a, 1000) == RebuildAction::Keep); // nothing built yet
    st.built = a;
    st.hasBuilt = true;
    CHECK(DecideRebuild(st, a, 1000) == RebuildAction::Keep);

    // a size that keeps changing never gets built; the one that holds for 500 ms does
    StreamShape b = a;
    for (uint32_t w = 600; w < 700; w += 4)
    {
        b.width = w;
        CHECK(DecideRebuild(st, b, 2000 + w) == RebuildAction::Settle);
    }
    b.width = 700;
    CHECK(DecideRebuild(st, b, 3000) == RebuildAction::Settle); // first sight of 700
    CHECK(DecideRebuild(st, b, 3499) == RebuildAction::Settle);
    CHECK(DecideRebuild(st, b, 3500) == RebuildAction::Rebuild);
    // going back to the built shape cancels the pending change
    CHECK(DecideRebuild(st, a, 3600) == RebuildAction::Keep && !st.hasPending);
    CHECK(DecideRebuild(st, b, 3700) == RebuildAction::Settle); // the clock starts again

    // each part of the shape counts
    StreamShape other = a;
    other.preset = 3;
    CHECK(!(other == a));
    other = a; other.style = 2;
    CHECK(!(other == a));
    other = a; other.late = true;
    CHECK(!(other == a));
    other = a; other.height = 961;
    CHECK(!(other == a));
    other = a;
    CHECK(other == a);
    StreamState lateSwitch;
    lateSwitch.built = a;
    lateSwitch.hasBuilt = true;
    StreamShape wantLate = a;
    wantLate.late = true; // PreviewNRLate switched on in the menu
    CHECK(DecideRebuild(lateSwitch, wantLate, 5000) == RebuildAction::Settle);
    CHECK(DecideRebuild(lateSwitch, wantLate, 5500) == RebuildAction::Rebuild);
    // a clock that went backwards does not rebuild
    StreamState back;
    back.built = a;
    back.hasBuilt = true;
    CHECK(DecideRebuild(back, wantLate, 9000) == RebuildAction::Settle);
    CHECK(DecideRebuild(back, wantLate, 100) == RebuildAction::Settle);
}

// The stream's own state is the single source of the "GPU work" rule.
void TestFactsOf()
{
    StreamState st;
    st.lastLive = 1000;
    CHECK(!FactsOf(st, false, 1500, 0, true).hasGpuWork);
    st.lastSubmitted = 4;
    const StreamFacts f = FactsOf(st, true, 1500, 7000, false);
    CHECK(f.hasGpuWork && !f.gpuDone && f.disabled && f.now == 1500 && f.lastLive == 1000 && f.lingerMs == 7000);
    CHECK(DecideStream(f) == StreamAction::WaitGpu); // disabled, but the GPU may still use it
    CHECK(DecideStream(FactsOf(st, true, 1500, 7000, true)) == StreamAction::Release);
}

void TestCost()
{
    CostAccumulator c;
    CHECK(!c.Due(0)); // nothing collected
    c.Add(2.0);
    CHECK(c.Due(1000)); // the first line comes at once
    CostAccumulator::Summary s = c.Take(1000);
    CHECK(s.count == 1 && s.average == 2.0 && s.max == 2.0);
    c.Add(1.0);
    c.Add(3.0);
    CHECK(!c.Due(2000)); // inside the interval
    CHECK(c.Due(1000 + kCostLogIntervalMs));
    s = c.Take(7000);
    CHECK(s.count == 2 && s.average == 2.0 && s.max == 3.0);
    CHECK(!c.Due(8000)); // reset after Take
}

// ---- The boundary towards the game -------------------------------------------------------------------------------------------------
struct Boom
{
};

void TestGuarded()
{
    int latchLines = 0;
    Latches latches;
    // The shape of the module's entry points: the body throws, the handler latches (one line only) and gives the fallback value.
    const auto onThrow = [&]
    {
        if (latches.LatchNr())
            ++latchLines;
        return -1;
    };

    CHECK(Guarded([] { return 7; }, onThrow) == 7); // no exception: the body's value, the handler is not called
    CHECK(latchLines == 0 && !latches.NrLatched());

    const int fromThrowingBody = Guarded([]() -> int { throw Boom {}; }, onThrow);
    CHECK(fromThrowingBody == -1);
    CHECK(latchLines == 1 && latches.NrLatched() && !latches.SrLatched());

    // Every kind of exception is stopped (library exceptions and non-standard ones); the second throw does not log again.
    CHECK(Guarded([]() -> int { throw std::runtime_error("x"); }, onThrow) == -1);
    CHECK(Guarded([]() -> int { throw 42; }, onThrow) == -1);
    CHECK(Guarded([]() -> int { throw std::bad_alloc(); }, onThrow) == -1);
    CHECK(latchLines == 1);

    // A void entry point (Maintain, Detach, ...).
    int ran = 0;
    Guarded([&] { ++ran; }, [&] { ran += 100; });
    CHECK(ran == 1);
    Guarded([&]() { throw Boom {}; }, [&] { ran += 100; });
    CHECK(ran == 101);

    // A pointer result falls back to null; a handler that throws itself still does not let anything out (a value-initialised answer).
    int target = 5;
    CHECK(Guarded([&]() -> int* { return &target; }, []() -> int* { return nullptr; }) == &target);
    CHECK(Guarded([&]() -> int* { throw Boom {}; }, []() -> int* { return nullptr; }) == nullptr);
    CHECK(Guarded([]() -> int* { throw Boom {}; }, []() -> int* { throw Boom {}; }) == nullptr);
    CHECK(Guarded([]() -> int { throw Boom {}; }, []() -> int { throw Boom {}; }) == 0);
    Guarded([]() { throw Boom {}; }, []() { throw Boom {}; });

    // The answer of a throwing run entry is the failure outcome, not an exception: the dispatcher keeps the native image.
    int fallbacks = 0;
    const int outcome = Guarded([]() -> int { throw Boom {}; },
                                [&]
                                {
                                    ++fallbacks;
                                    return static_cast<int>(3);
                                });
    CHECK(outcome == 3 && fallbacks == 1);

    // The real outcome enum: when the body throws and even the failure handler throws, the answer is a value-initialised outcome. That must
    // not be "Ran" (a result image exists): the dispatcher would count a run and skip withdrawing the older result.
    CHECK(Guarded([]() -> RunOutcome { throw Boom {}; }, []() -> RunOutcome { throw Boom {}; }) != RunOutcome::Ran);
    CHECK(Guarded([]() -> RunOutcome { throw Boom {}; }, []() -> RunOutcome { throw Boom {}; }) == RunOutcome::Failed);
    CHECK(static_cast<int>(RunOutcome::Failed) == 0); // the value-initialised outcome
    CHECK(Guarded([]() -> RunOutcome { return RunOutcome::Ran; }, []() -> RunOutcome { return RunOutcome::Failed; }) == RunOutcome::Ran);

    // A body that throws while it holds the module's lock leaves the lock free (the lock guard unwinds before the handler runs): a second
    // thread can take it right after.
    std::mutex moduleLock;
    Guarded(
        [&]
        {
            std::lock_guard<std::mutex> hold(moduleLock);
            throw Boom {};
        },
        [&] {});
    bool lockFree = false;
    std::thread other(
        [&]
        {
            lockFree = moduleLock.try_lock();
            if (lockFree)
                moduleLock.unlock();
        });
    other.join();
    CHECK(lockFree);
}

// The done fence after a signal that fails behind a list that ran. A fake queue: the signals it accepts complete in order, a rejected one
// never does.
struct FakeDoneQueue
{
    uint64_t completed = 0; // GetCompletedValue of the done fence
    bool rejectNext = false;
    bool Signal(uint64_t value)
    {
        if (rejectNext)
        {
            rejectNext = false;
            return false;
        }
        completed = value; // queued behind everything before it: the fence catches up to this value
        return true;
    }
};

void TestDoneRepair()
{
    FakeDoneQueue q;
    FenceCounter done;
    bool broken = false;
    StreamState st;
    st.key = 0x1000;
    st.lastLive = 100;
    const auto signal = [&](uint64_t v) { return q.Signal(v); };
    const auto action = [&](bool gpuDone)
    {
        // a stream whose window closed long ago, so only the GPU decides
        return DecideStream(FactsOf(st, false, 100000, LingerMs(5), gpuDone));
    };

    // run 1: signalled, completes
    uint64_t v1 = 0;
    CHECK(SignalDoneStep(done, broken, v1, signal) && v1 == 1 && !broken && q.completed == 1);
    st.lastSubmitted = v1;
    CHECK(action(q.completed >= st.lastSubmitted) == StreamAction::Release);

    // run 2: the list ran, its done signal is rejected. The value was taken, the fence is broken, the stream waits for the GPU.
    q.rejectNext = true;
    uint64_t v2 = 0;
    CHECK(!SignalDoneStep(done, broken, v2, signal));
    CHECK(v2 == 2 && broken && q.completed == 1);                         // the value is kept for the caller, the fence stays below it
    st.lastSubmitted = v2;                                                // the caller records the list as submitted whether or not its signal was accepted
    CHECK(st.HasGpuWork());
    CHECK(action(q.completed >= st.lastSubmitted) == StreamAction::WaitGpu); // forever, without a repair

    // a repair through a queue that may be gone signals nothing and takes no value
    CHECK(!RepairDoneStep(done, broken, false, signal) && broken && done.last == 2);
    // a repair whose signal is rejected too keeps the fence broken and still only grows the values
    q.rejectNext = true;
    CHECK(!RepairDoneStep(done, broken, true, signal) && broken && done.last == 3 && q.completed == 1);
    // the repair signals a newer value behind everything queued: the fence catches up and the stream can go
    CHECK(RepairDoneStep(done, broken, true, signal) && !broken && done.last == 4 && q.completed == 4);
    CHECK(q.completed > v2);
    CHECK(action(q.completed >= st.lastSubmitted) == StreamAction::Release);

    // an intact fence is left alone: no value is taken, nothing is signalled
    CHECK(!RepairDoneStep(done, broken, true, signal) && done.last == 4 && q.completed == 4);
}


// A teardown entry that throws marks the release pending first (before the latch line, which can throw itself), so the retry applies.
void TestGuardedTeardown()
{
    std::vector<std::string> order;
    bool pending = false;
    const auto mark = [&]
    {
        pending = true;
        order.push_back("pending");
    };
    const auto latch = [&] { order.push_back("latch"); };

    GuardedTeardown([&] { order.push_back("body"); }, mark, latch); // a clean pass touches nothing
    CHECK(!pending && order == std::vector<std::string> { "body" });

    order.clear();
    GuardedTeardown([]() { throw Boom {}; }, mark, latch); // the body throws: pending first, then the latch
    CHECK((pending && order == std::vector<std::string> { "pending", "latch" }));

    pending = false;
    order.clear();
    GuardedTeardown([]() { throw std::bad_alloc(); }, mark, [&] { order.push_back("latch"); throw Boom {}; }); // the latch line throws too
    CHECK((pending && order == std::vector<std::string> { "pending", "latch" })); // pending was already set, and nothing leaves the call

    // the same handler sequence for a body that throws every time (the retry path keeps working: the call never throws out)
    for (int i = 0; i < 3; ++i)
        GuardedTeardown([]() { throw Boom {}; }, mark, latch);
    CHECK(pending);
}


// A render that kept the image is counted once per call, whatever the layers below did with the outcome: the call is counted where it
// ends (CountedCall), the production entry is a subset of the total.
void TestFallbackTally()
{
    enum class Out { Failed, Ran, Preparing };
    const auto ran = [](Out o) { return o == Out::Ran; };
    FallbackTally t;
    CHECK(CountedCall(t, false, [] { return Out::Ran; }, ran) == Out::Ran);
    CHECK(t.all == 0 && t.production == 0); // a run is not a fallback
    CHECK(CountedCall(t, false, [] { return Out::Preparing; }, ran) == Out::Preparing);
    CHECK(t.all == 1 && t.production == 0);
    CHECK(CountedCall(t, true, [] { return Out::Failed; }, ran) == Out::Failed);
    CHECK(t.all == 2 && t.production == 1);
    CHECK(CountedCall(t, true, [] { return Out::Ran; }, ran) == Out::Ran);
    CHECK(t.all == 2 && t.production == 1);
    t.Note(true); // a render the dispatcher did not offer to production
    CHECK(t.all == 3 && t.production == 2);
    t.Note(false);
    CHECK(t.all == 4 && t.production == 2 && t.production <= t.all);
}


// The video memory figure goes down when a stream's objects go, whichever way they go.
void TestVideoMemoryAccount()
{
    CHECK(AddedMegabytes(100u << 20, 164u << 20) == 64);
    CHECK(AddedMegabytes(164u << 20, 100u << 20) == 0);          // other users freed memory meanwhile: never negative
    CHECK(AddedMegabytes(0, (3u << 20) - 1) == 2);                 // whole megabytes
    CHECK(AddedMegabytes(5ull << 30, (5ull << 30) + (200ull << 20)) == 200);

    VideoMemoryAccount account;
    uint64_t a = 0, b = 0; // two streams' parts
    account.Added(a, 52);  // a bring-up stream (images and model)
    account.Added(b, 110); // another window
    CHECK(account.Total() == 162 && a == 52 && b == 110);

    // the bring-up -> production switch: the old stream is released (its figure goes), the new one adds its own
    account.Released(a);
    CHECK(account.Total() == 110 && a == 0);
    account.Added(a, 200);
    CHECK(account.Total() == 310 && a == 200);

    // a linger release and a second release of the same (already empty) part change nothing more
    account.Released(b);
    CHECK(account.Total() == 200 && b == 0);
    account.Released(b);
    CHECK(account.Total() == 200);

    // window open / close cycles do not accumulate
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        uint64_t part = 0;
        account.Added(part, 150);
        account.Released(part);
    }
    CHECK(account.Total() == 200);

    // the full release resets the total; a part released afterwards cannot take it below zero
    account.Reset();
    CHECK(account.Total() == 0);
    account.Released(a); // a still holds 200 from before the reset
    CHECK(account.Total() == 0 && a == 0);
}


// The timestamp ring of GpuTime_Dx12 (OptiScaler/gpu_time/GpuTime_Dx12.cpp, shared with the main pass), as that source behaves: Start moves to
// the next slot and uses it, End marks it readable, ReadGpuTime maps the slot AFTER the current one (the run two back) and returns what the
// readback buffer holds, whether or not the GPU ran that list. `holds[slot]` is the id of the run whose timestamps the buffer holds there
// (0: nothing was ever written, zeros).
struct FakeTimestampRing
{
    static constexpr size_t kSlots = 3;
    size_t current = 0;
    bool trigger[kSlots] = {};
    uint32_t holds[kSlots] = {};
    uint32_t written[kSlots] = {}; // the run whose list ran for this slot (set by the fake GPU when the list completes)
    void Start(uint32_t runId)
    {
        current = (current + 1) % kSlots;
        pending = runId;
    }
    void End() { trigger[current] = true; }
    uint32_t pending = 0;
    // The GPU executed the list of `runId`: its timestamps reach the readback buffer.
    void GpuRan(uint32_t runId)
    {
        for (size_t i = 0; i < kSlots; ++i)
            if (slotOfRun[i] == runId)
                holds[i] = runId;
    }
    uint32_t slotOfRun[kSlots] = {};
    // ReadGpuTime: returns the run id whose timestamps are in the slot (0: zeros / nothing)
    std::optional<uint32_t> Read()
    {
        const size_t prev = (current + 1) % kSlots;
        if (!trigger[prev])
            return std::nullopt;
        trigger[prev] = false;
        return holds[prev];
    }
};

void TestTimerRing()
{
    // Three previews in one frame: runs 1, 2, 3 are submitted (done values 1, 2, 3) while the GPU is still behind (completed 0). The read for
    // run 3 would map the slot of run 1, which has not executed: the old code read it anyway and took zeros into the average.
    FakeTimestampRing gpu;
    TimerRing ring;
    uint64_t completed = 0;
    const auto gpuDone = [&](uint64_t v) { return completed >= v; };
    std::vector<std::pair<uint32_t, bool>> reads; // (run whose time was read, was it a production run)
    uint32_t zeroReadsOfTheOldCode = 0;

    const auto run = [&](uint32_t id, bool production)
    {
        ring.NoteStart();
        gpu.Start(id);
        gpu.slotOfRun[gpu.current] = id;
        gpu.End();
        ring.NoteSubmitted(id, production); // done value == run id in this model
        // what the old code did: read at once
        FakeTimestampRing oldCopy = gpu;
        if (const auto v = oldCopy.Read(); v.has_value() && *v == 0)
            ++zeroReadsOfTheOldCode;
        // the fix: read only when the list in that slot is complete
        if (ring.CanRead(gpuDone))
        {
            const bool isProduction = ring.ReadIsProduction();
            if (const auto v = gpu.Read(); v.has_value())
                reads.emplace_back(*v, isProduction);
        }
    };

    run(1, false);
    run(2, true);
    run(3, false);
    CHECK(reads.empty());                  // nothing completed yet: nothing is read
    CHECK(zeroReadsOfTheOldCode >= 1);     // (the old code read slots the GPU had not written)

    // the GPU catches up with runs 1 and 2; the next run reads the run two back, which is complete, and attributes it to ITS mode
    completed = 2;
    gpu.GpuRan(1);
    gpu.GpuRan(2);
    run(4, false);
    CHECK(reads.size() == 1 && reads[0].first == 2 && reads[0].second == true); // run 2 (production), not the current run's mode
    gpu.GpuRan(3);
    gpu.GpuRan(4);
    completed = 4;
    run(5, true);
    CHECK(reads.size() == 2 && reads[1].first == 3 && reads[1].second == false); // run 3 (bring-up), though the current run is production

    // a run whose list never ran (it failed after the timer started): the slot it took is not read, the next one is
    ring.NoteStart(); // run 6 started and was abandoned: the ring moved on, nothing was submitted
    gpu.Start(6);
    gpu.slotOfRun[gpu.current] = 6;
    gpu.trigger[gpu.current] = true; // the slot still holds a stale measurement of an older run (3) that was never read out
    gpu.holds[gpu.current] = 3;
    completed = 6;
    for (uint32_t id = 7; id <= 9; ++id)
    {
        gpu.GpuRan(id - 1);
        run(id, false);
    }
    // the abandoned slot (run 6) was never submitted: the ring does not let its stale content be read as a measurement
    bool readAbandoned = false;
    for (const auto& r : reads)
        readAbandoned = readAbandoned || r.first == 6;
    CHECK(!readAbandoned);
    uint32_t staleReads = 0;
    for (const auto& r : reads)
        staleReads += r.first == 3 ? 1 : 0;
    CHECK(staleReads == 1); // run 3 was read once, before the abandoned start; the stale copy is not read again
}

} // namespace

int main()
{
    TestAvailability();
    TestStateLog();
    TestLatches();
    TestStreams();
    TestLinger();
    TestReset();
    TestHandoff();
    TestFrame();
    TestConsumerBuffer();
    TestResultCurrent();
    TestRenderWithoutResult();
    TestRebuild();
    TestFactsOf();
    TestCost();
    TestGuarded();
    TestDoneRepair();
    TestGuardedTeardown();
    TestFallbackTally();
    TestVideoMemoryAccount();
    TestTimerRing();
    return TEST_RESULT("chara_nr_rules");
}
