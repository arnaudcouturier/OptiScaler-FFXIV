// Dynamic MFG kill-switch bounds (ext/fg/DynamicMfgControl.h): how long a bad Streamline status or a wrong applied count has to
// persist at the end of a watch before it is a failure, and how many of the first intervals after a switch can carry the
// re-creation stall. The controller is driven tick by tick at exact times (AutoMfgTick directly), so every constant is pinned
// from both sides: samples (kBadStatusSamples, kWrongSamples), span (kBadRunMs), and the signature window (kSignatureSamples).
//
// Why exact times: the filters count ticks, and a tick is a game frame. At 224 real fps three ticks last about 13 ms, so a
// count alone says nothing about time; the controller also asks the bad run to span kBadRunMs.
#include "dynamic_mfg_harness.h"

#include "ext/fg/DynamicMfgControl.h"

#include <initializer_list>
#include <string>
#include <vector>

using namespace ext::fg;

namespace
{
enum class Fault
{
    Status, // Streamline status 0x2 (it was 0 when the switch was made)
    Zero,   // the applied count reads 0 ("unknown"), which is not the requested count either
};

// A controller driven by hand: one tick per call, at the time given.
struct Drv
{
    AutoMfgController c;
    Plant plant;     // heavy scene: the controller steps up to 3x at once
    int factor = 2;  // the count in force (the world applies what the controller says)
    GateInputs gate;
    uint64_t t = 1000;
    uint64_t sw = 0; // time of the first switch (the watch opens there)
    int recoveries = 0;
    std::vector<AutoMfgLine> lines;

    Drv()
    {
        plant.base2x = 18.2;
        gate.keyOn = true;
        gate.dx12 = true;
        gate.dlssgOutput = true;
        gate.nvidia = true;
        gate.archId = 0x190;
        gate.maxGenerated = 5;
    }

    AutoMfgOut Tick(uint64_t at, double realMs, uint32_t status, int detected, uint32_t presented)
    {
        AutoMfgTick in;
        in.nowMs = at;
        in.realMs = realMs;
        in.appliedFactor = factor;
        in.maxCfg = 4;
        in.targetCfg = 224.0;
        in.limit = plant.limit;
        in.refresh = plant.refresh;
        in.gate = gate;
        in.stateValid = true;
        in.slStatus = status;
        in.presented = presented;
        in.detectedGenerated = detected;
        const AutoMfgOut out = c.Tick(in);
        lines.insert(lines.end(), out.lines.begin(), out.lines.end());
        if (out.setFactor)
            factor = out.factor;
        if (out.recover)
        {
            ++recoveries;
            factor = out.factor;
            c.OnActivated();
        }
        return out;
    }
    // A healthy tick: the count and the presented frames of the factor in force.
    AutoMfgOut Good(uint64_t at, double realMs = -1.0)
    {
        return Tick(at, realMs < 0.0 ? plant.Ms(factor) : realMs, 0, factor - 1, (uint32_t) factor);
    }
    // Healthy ticks every 20 ms until the first switch (2x -> 3x); `sw` is its time.
    void ToFirstSwitch()
    {
        for (int i = 0; i < 2000 && sw == 0; ++i)
        {
            t += 20;
            if (Good(t).setFactor)
                sw = t;
        }
    }
    bool Broken() const { return recoveries == 1 && c.Stopped(); }
    bool Clean() const { return recoveries == 0 && !c.Stopped(); }
    bool Has(const std::string& needle) const
    {
        for (const auto& l : lines)
            if (l.text.find(needle) != std::string::npos)
                return true;
        return false;
    }
};

// One watch after the first switch, ending with its verdict at 1000 ms. Healthy ticks every 20 ms until the first faulty tick,
// then only the faulty ticks (a healthy one in between would end the run); the times are ms after the switch, the last is 1000.
Drv Watch(Fault fault, std::initializer_list<uint64_t> bad)
{
    Drv d;
    d.ToFirstSwitch();
    CHECK_MSG(d.sw != 0 && d.factor == 3, "no switch to watch");
    for (uint64_t r = 20; r < *bad.begin(); r += 20)
        d.Good(d.sw + r);
    for (const uint64_t r : bad)
        d.Tick(d.sw + r, d.plant.Ms(d.factor), fault == Fault::Status ? 0x2u : 0u, fault == Fault::Zero ? 0 : d.factor - 1,
               (uint32_t) d.factor);
    return d;
}

// The faulty ticks 20 ms apart from `from` to 1000 (both included).
std::vector<uint64_t> Every20(uint64_t from)
{
    std::vector<uint64_t> v;
    for (uint64_t r = from; r <= 1000; r += 20)
        v.push_back(r);
    return v;
}

Drv WatchEvery20(Fault fault, uint64_t from)
{
    Drv d;
    d.ToFirstSwitch();
    for (uint64_t r = 20; r < from; r += 20)
        d.Good(d.sw + r);
    for (const uint64_t r : Every20(from))
        d.Tick(d.sw + r, d.plant.Ms(d.factor), fault == Fault::Status ? 0x2u : 0u, fault == Fault::Zero ? 0 : d.factor - 1,
               (uint32_t) d.factor);
    return d;
}

void RunBounds(Fault fault, const char* name)
{
    // The sample count, lower side: two bad ticks that span 200 ms are still not enough.
    {
        const Drv d = Watch(fault, { 800, 1000 });
        CHECK_MSG(d.Clean(), "%s: two bad samples over 200 ms were a failure", name);
    }
    // The sample count, upper side: three bad ticks over 200 ms are.
    {
        const Drv d = Watch(fault, { 800, 900, 1000 });
        CHECK_MSG(d.Broken(), "%s: three bad samples over 200 ms were not a failure", name);
        CHECK(d.Has(fault == Fault::Status ? "AutoMFG: Streamline status 0x2, switch 2x -> 3x; going back to 2x" :
                                             "AutoMFG: applied count 0 != requested 2 (generated frames), switch 2x -> 3x"));
    }
    // The span, lower side: nine bad ticks in a row, but only 140 ms from the first to the verdict (a game running at 60 fps
    // for a moment, or three frames at 224 fps, are not a failed change).
    {
        const Drv d = WatchEvery20(fault, 860);
        CHECK_MSG(d.Clean(), "%s: a bad run of 140 ms was a failure", name);
    }
    // The span, upper side: 160 ms is.
    {
        const Drv d = WatchEvery20(fault, 840);
        CHECK_MSG(d.Broken(), "%s: a bad run of 160 ms was not a failure", name);
    }
    // And the boundary itself: exactly 150 ms counts (4 ticks).
    {
        const Drv d = Watch(fault, { 850, 900, 950, 1000 });
        CHECK_MSG(d.Broken(), "%s: a bad run of exactly 150 ms was not a failure", name);
    }
    // Three bad ticks within 10 ms (the case the span rule is for).
    {
        const Drv d = Watch(fault, { 990, 995, 1000 });
        CHECK_MSG(d.Clean(), "%s: three bad ticks within 10 ms were a failure", name);
    }
    // A healthy tick ends the run: the same ticks with one healthy one in the middle are two short runs.
    {
        Drv d;
        d.ToFirstSwitch();
        for (uint64_t r = 20; r < 800; r += 20)
            d.Good(d.sw + r);
        for (const uint64_t r : { 800ull, 850ull, 900ull })
            d.Tick(d.sw + r, d.plant.Ms(d.factor), fault == Fault::Status ? 0x2u : 0u, fault == Fault::Zero ? 0 : d.factor - 1,
                   (uint32_t) d.factor);
        d.Good(d.sw + 950);
        for (const uint64_t r : { 980ull, 1000ull })
            d.Tick(d.sw + r, d.plant.Ms(d.factor), fault == Fault::Status ? 0x2u : 0u, fault == Fault::Zero ? 0 : d.factor - 1,
                   (uint32_t) d.factor);
        CHECK_MSG(d.Clean(), "%s: a run interrupted by a healthy tick was added up", name);
    }
}

void StatusBounds()
{
    RunBounds(Fault::Status, "status");
}

void ZeroCountBounds()
{
    RunBounds(Fault::Zero, "zero count");
    // A silent stretch (no tick for 600 ms, under the 1 s gap rule) and two zero ticks after it: what the DLL can really show
    // after a short pause. Two samples are not a failure whatever the time between them.
    Drv d;
    d.ToFirstSwitch();
    for (uint64_t r = 20; r <= 300; r += 20)
        d.Good(d.sw + r);
    d.Tick(d.sw + 900, d.plant.Ms(d.factor), 0, 0, (uint32_t) d.factor);
    d.Tick(d.sw + 1000, d.plant.Ms(d.factor), 0, 0, (uint32_t) d.factor);
    CHECK(d.Clean());
}

void ANonZeroWrongCountIsNotUnknown()
{
    // A count that is not zero and not the requested one is a real mismatch, not "unknown": one sample at the verdict is
    // enough (the plugin applied another count).
    Drv d;
    d.ToFirstSwitch();
    for (uint64_t r = 20; r < 1000; r += 20)
        d.Good(d.sw + r);
    d.Tick(d.sw + 1000, d.plant.Ms(d.factor), 0, 5, (uint32_t) d.factor);
    CHECK(d.Broken());
}

// A 190 ms interval at the n-th tick after a switch; everything else is healthy. Returns the strikes the controller counted.
int StrikesWhenTheStallIsTick(int n)
{
    Drv d;
    d.ToFirstSwitch();
    for (int k = 1; k <= 50; ++k) // the verdict is the 50th tick
        d.Good(d.sw + 20ull * k, k == n ? 190.0 : -1.0);
    CHECK(d.Clean());
    return d.c.CostlyCount();
}

void TheSignatureIsOneOfTheFirstTwoIntervals()
{
    static_assert(AutoMfgController::kSignatureSamples == 2);
    CHECK_MSG(StrikesWhenTheStallIsTick(1) == 1, "a stall in the first interval after the switch is the signature");
    CHECK_MSG(StrikesWhenTheStallIsTick(2) == 1, "a stall in the second interval after the switch is the signature");
    CHECK_MSG(StrikesWhenTheStallIsTick(3) == 0, "a stall in the third interval is the game's hitch");
    CHECK_MSG(StrikesWhenTheStallIsTick(25) == 0, "a stall in the middle of the watch is the game's hitch");
    // 99 ms is below the threshold whatever the tick, 100 ms is the threshold
    Drv a;
    a.ToFirstSwitch();
    for (int k = 1; k <= 50; ++k)
        a.Good(a.sw + 20ull * k, k == 1 ? 99.0 : -1.0);
    CHECK(a.c.CostlyCount() == 0);
    Drv b;
    b.ToFirstSwitch();
    for (int k = 1; k <= 50; ++k)
        b.Good(b.sw + 20ull * k, k == 1 ? 100.0 : -1.0);
    CHECK(b.c.CostlyCount() == 1);
}
} // namespace

int main()
{
    StatusBounds();
    ZeroCountBounds();
    ANonZeroWrongCountIsNotUnknown();
    TheSignatureIsOneOfTheFirstTwoIntervals();
    if (g_failures)
    {
        std::printf("dynamic_mfg_killswitch_bounds: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_killswitch_bounds: all passed\n");
    return 0;
}
