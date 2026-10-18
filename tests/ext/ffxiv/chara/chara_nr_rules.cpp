#include "check.h"
#include "ext/ffxiv/chara/CharaNrRules.h"

#include <string>
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
    f = Good(); f.nvidia = Probe::Missing; f.forwarder = Probe::Missing; f.model = Probe::Missing;
    CHECK(Evaluate(f) == Availability::NotNvidia);
    f = Good(); f.forwarder = Probe::Missing; f.model = Probe::Missing;
    CHECK(Evaluate(f) == Availability::ForwarderMissing);

    // every unavailable reason has text; available and config-off have none
    for (Availability a : { Availability::Latched, Availability::BridgeNotUp, Availability::BridgeShuttingDown, Availability::NotNvidia,
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
    // an SR latch does not stop NR
    Latches a;
    CHECK(a.LatchSr());
    CHECK(!a.LatchSr()); // only the first call reports
    CHECK(a.SrLatched() && !a.NrLatched());
    CHECK(!a.SrMayRun(true));
    CHECK(a.NrMayRun(true));
    CHECK(a.PlumbingAlive(true, true));            // NR keeps the preview plumbing alive
    CHECK(a.PlumbingAlive(true, false) == false);  // SR only, and SR is latched: nothing left
    // an NR latch does not stop SR
    Latches b;
    CHECK(b.LatchNr());
    CHECK(b.NrLatched() && !b.SrLatched());
    CHECK(b.SrMayRun(true));
    CHECK(!b.NrMayRun(true));
    CHECK(b.PlumbingAlive(true, true));
    CHECK(!b.PlumbingAlive(false, true));
    // both latched: nothing runs
    CHECK(b.LatchSr());
    CHECK(!b.PlumbingAlive(true, true));
    // not wanted means not running, latched or not
    Latches c;
    CHECK(!c.SrMayRun(false) && !c.NrMayRun(false) && !c.PlumbingAlive(false, false));
    CHECK(c.PlumbingAlive(false, true) && c.PlumbingAlive(true, false));
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
    CHECK(!t.NeedsRebuild(a, 576, 960) && t.NeedsRebuild(a, 288, 480) && t.NeedsRebuild(a, 576, 961));
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
    // a short linger from the configuration
    f.lingerMs = LingerMs(0);
    f.lastLive = 1000;
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
}

struct FakeOps
{
    std::vector<std::string> calls;
    HandoffStep failAt = HandoffStep::Wait11;
    bool fail = false;
    bool Step(HandoffStep s, const char* name, uint64_t v)
    {
        calls.push_back(std::string(name) + (v != 0 ? std::to_string(v) : ""));
        return !(fail && failAt == s);
    }
    bool Signal11(uint64_t v) { return Step(HandoffStep::Signal11, "S11:", v); }
    bool Wait12(uint64_t v) { return Step(HandoffStep::Wait12, "W12:", v); }
    bool Record() { return Step(HandoffStep::Record, "REC", 0); }
    bool Signal12(uint64_t v) { return Step(HandoffStep::Signal12, "S12:", v); }
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
    FenceCounter counter;
    FakeOps ok;
    const HandoffOutcome a = RunHandoff(ok, counter);
    CHECK(a.ok && a.ready == 1 && a.done == 2);
    CHECK((ok.calls == std::vector<std::string> { "S11:1", "W12:1", "REC", "S12:2", "W11:2" }));

    // a failure at any step before the D3D11 wait: the D3D11 wait is never issued, and nothing later runs
    for (HandoffStep step : { HandoffStep::Signal11, HandoffStep::Wait12, HandoffStep::Record, HandoffStep::Signal12 })
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
        CHECK(!o.ok && o.failedAt == HandoffStep::Wait11 && o.done == 2);
    }
    // values only grow, also across failures: a failed run never lets a later run reuse a value
    FenceCounter c;
    FakeOps bad;
    bad.fail = true;
    bad.failAt = HandoffStep::Record;
    RunHandoff(bad, c);
    FakeOps good;
    const HandoffOutcome next = RunHandoff(good, c);
    CHECK(next.ok && next.ready == 2 && next.done == 3);
    // the late variant does not wait inline: the consumer one frame later waits on `done`
    FenceCounter lateCounter;
    FakeOps late;
    const HandoffOutcome l = RunHandoff(late, lateCounter, false);
    CHECK(l.ok && l.done == 2 && !late.Has("W11"));
    CHECK((late.calls == std::vector<std::string> { "S11:1", "W12:1", "REC", "S12:2" }));
}

void TestConsumerBuffer()
{
    CHECK(ConsumerBuffer(false, 0) == -1 && ConsumerBuffer(true, 0) == -1);
    CHECK(ConsumerBuffer(false, 1) == 0); // the first run wrote buffer 0
    CHECK(ConsumerBuffer(false, 2) == 1 && ConsumerBuffer(false, 3) == 0);
    CHECK(ConsumerBuffer(true, 1) == -1); // late: the first result is shown one frame later
    CHECK(ConsumerBuffer(true, 2) == 0);  // late: the result of run 0 while run 1 is in flight
    CHECK(ConsumerBuffer(true, 3) == 1);
    // the late consumer never reads the buffer the newest run writes
    for (uint64_t runs = 2; runs < 20; ++runs)
        CHECK(ConsumerBuffer(true, runs) != ResultBufferForRun(runs - 1));
    // the newest run writes the buffer the previous consumer did not use
    for (uint64_t runs = 1; runs < 20; ++runs)
        CHECK(ConsumerBuffer(false, runs) == ResultBufferForRun(runs - 1));
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
    TestConsumerBuffer();
    TestCost();
    return TEST_RESULT("chara_nr_rules");
}
