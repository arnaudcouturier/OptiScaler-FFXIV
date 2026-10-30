// The AutoMFG expected-stall seam (ext/fg/DynamicMfgControl.h): an NR model rebuild owns the next two frame
// intervals. The next open tick restarts the selector (its settle drops exactly those two) and an open watch starts
// over on both, neither tick feeds the stall average, and a pending stall is dropped while the gate is closed. A
// rebuild hitch inside an open watch is judged on the calm that follows, never as the switch's own re-creation
// stall.
#include "ext/fg/DynamicMfgControl.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

using namespace ext::fg;

struct Rig
{
    AutoMfgController c;
    GateInputs gate;
    uint64_t t = 100000;
    int factor = 2;
    double target = 263.0; // the presented rate at 2x sits at 0.95 of this: the selector holds
    bool selfTest = false;
    uint32_t status = 0; // a Streamline status the fake plugin reports, 0 = clean
    std::vector<std::pair<uint64_t, std::string>> lines;

    Rig()
    {
        gate.keyOn = true;
        gate.dx12 = true;
        gate.dlssgOutput = true;
        gate.nvidia = true;
        gate.archId = 0x190;
        gate.maxGenerated = 5;
    }

    AutoMfgOut Step(double realMs)
    {
        AutoMfgTick in;
        in.nowMs = t;
        in.realMs = realMs;
        in.appliedFactor = factor;
        in.selfTest = selfTest;
        in.maxCfg = 4;
        in.targetCfg = target;
        in.limit = 0.0;
        in.refresh = 240.0;
        in.gate = gate;
        in.stateValid = true;
        in.slStatus = status;
        in.presented = (uint32_t) factor;
        in.detectedGenerated = factor - 1;
        in.paused = false;
        AutoMfgOut out = c.Tick(in);
        for (const auto& l : out.lines)
            lines.push_back({ t, l.text });
        if (out.setFactor || out.recover)
            factor = out.factor;
        t += (uint64_t) std::max(1.0, realMs);
        return out;
    }

    void RunMs(uint64_t ms, double realMs)
    {
        const uint64_t end = t + ms;
        while (t < end)
            Step(realMs);
    }

    // force the selector up to 3x and hold it there: the returned tick opened the watch
    uint64_t SwitchUp()
    {
        target = 395.0; // 2x falls short, 3x sits at 0.95 of this
        for (int i = 0; i < 2000; ++i)
        {
            const uint64_t at = t;
            if (Step(8.0).setFactor)
                return at;
        }
        return 0;
    }

    uint64_t SettledAt() const
    {
        for (const auto& l : lines)
            if (l.second.find("settled") != std::string::npos)
                return l.first;
        return 0;
    }

    bool Has(const char* text) const
    {
        for (const auto& l : lines)
            if (l.second.find(text) != std::string::npos)
                return true;
        return false;
    }
};

// The selector restart drops exactly the next two intervals: after the stall plus two hitches the running average
// is reset, and only the third hitch is seen. Without the stall the same hitches leave the average untouched.
void SelectorIgnoresExactlyTwo()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    CHECK(seam.factor == 2);
    seam.c.NoteExternalStall();
    CHECK(!seam.Step(190.0).setFactor && !seam.Step(190.0).setFactor);
    CHECK(seam.factor == 2); // two rebuild hitches switch nothing
    CHECK(seam.c.Selector().AverageMs() == 0.0); // restarted: both hitches dropped by the settle
    seam.Step(190.0);
    CHECK(seam.c.Selector().AverageMs() == 190.0); // the third interval is seen again

    Rig control;
    control.RunMs(5000, 8.0);
    control.Step(190.0);
    control.Step(190.0);
    CHECK(control.factor == 2);
    CHECK(control.c.Selector().AverageMs() > 5.0); // no restart: the average still holds the calm rate
}

// A pending stall is dropped while the gate is closed: after reopening, the next hitches are seen. Without the
// closed tick in between the same hitches are ignored.
void GateClosedClearsPendingStall()
{
    Rig cleared;
    cleared.RunMs(5000, 8.0);
    cleared.c.NoteExternalStall();
    cleared.gate.keyOn = false;
    cleared.Step(8.0); // the closed tick drops the pending stall
    cleared.gate.keyOn = true;
    cleared.Step(8.0);
    cleared.Step(190.0);
    cleared.Step(190.0);
    CHECK(cleared.factor == 2);
    CHECK(cleared.c.Selector().AverageMs() == 190.0); // seen: nothing was pending anymore

    Rig held;
    held.RunMs(5000, 8.0);
    held.c.NoteExternalStall();
    held.Step(190.0);
    held.Step(190.0);
    CHECK(held.c.Selector().AverageMs() == 0.0); // ignored: the stall was still pending
}

// Neither ignored tick feeds the stall average: after five hitches the later three are seen (the average holds
// them) instead of the controller stalling. Without the skip the fourth hitch would lift the clipped average
// over the stall line and the restarts would zero the selector average instead.
void StallAverageSkippedOnBothTicks()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    seam.c.NoteExternalStall();
    for (int i = 0; i < 5; ++i)
        seam.Step(190.0);
    CHECK(seam.factor == 2);
    CHECK(seam.c.Selector().AverageMs() == 190.0); // dropped two, fed three: never stalled
}

// An open watch starts over on both ignored ticks and the stall average is skipped: a small hitch delays the
// verdict past the hitch and judges the calm, with no switch back.
void WatchRestartsAndStallSkipped()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    const uint64_t swAt = seam.SwitchUp();
    CHECK(swAt != 0 && seam.factor == 3);
    seam.c.NoteExternalStall();
    const uint64_t h2 = seam.t + 120; // two 60 ms hitches
    seam.Step(60.0);
    seam.Step(60.0);
    seam.RunMs(2500, 8.0);
    CHECK(seam.factor == 3); // the hitch is not the switch's: no switch back
    CHECK(!seam.Has("cost a")); // no re-creation signature on a hitch under the costly line
    const uint64_t settled = seam.SettledAt();
    CHECK(settled != 0); // judged, on the calm that follows
    if (settled != 0)
        CHECK(settled >= h2 + 900 && settled <= h2 + 1300); // the verdict runs a full watch past the hitch

    Rig control;
    control.RunMs(5000, 8.0);
    const uint64_t cswAt = control.SwitchUp();
    CHECK(cswAt != 0 && control.factor == 3);
    control.Step(60.0);
    control.Step(60.0);
    control.RunMs(2500, 8.0);
    CHECK(control.factor == 3);
    const uint64_t csettled = control.SettledAt();
    CHECK(csettled != 0);
    if (settled != 0 && csettled != 0)
        CHECK(settled > csettled); // the restarted watch judges later than the untouched one
}

// Positive control: a real status fault that persists past a rebuild is still caught. The two ignored ticks
// restart the watch and record nothing, but recording resumes on the next tick, so a fault that is still there
// builds its run and is convicted at the next verdict.
void PersistentStatusFaultAfterRebuildIsStillCaught()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    const uint64_t swAt = seam.SwitchUp();
    CHECK(swAt != 0 && seam.factor == 3);
    seam.c.NoteExternalStall();
    seam.Step(190.0);
    seam.Step(190.0); // the rebuild's own intervals: ignored, the watch starts over twice
    seam.status = 1; // a real Streamline fault, still present on the next tick
    seam.RunMs(3000, 8.0);
    CHECK(seam.factor == 2); // convicted: back to the last good count
    CHECK(seam.Has("automatic mode stopped")); // ...and automatic mode stopped
    (void) swAt;
}

// A rebuild on every tick keeps the watch unjudged: after kStallGiveUpMs it ends as unknown, with no strike
// and the count staying where the switch left it.
void RepeatedRebuildsReachGiveUp()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    const uint64_t swAt = seam.SwitchUp();
    CHECK(swAt != 0 && seam.factor == 3);
    int guard = 0;
    while (seam.t - swAt < 5200 && guard++ < 2000)
    {
        seam.c.NoteExternalStall();
        seam.Step(8.0);
        seam.Step(8.0); // every interval is ignored: the watch restarts faster than the verdict
    }
    CHECK(seam.Has("skipped, the game was stalled")); // unknown, not a signature
    CHECK(seam.factor == 3); // the count stays
    CHECK(!seam.Has("cost a") && !seam.Has("automatic mode stopped")); // no strike, no stop
    CHECK(seam.c.CostlyCount() == 0);
}

// A rebuild during the self-test changes neither the verdict nor the refresh-bound readout: the ignored
// intervals feed neither the watch nor the at-4x mean.
void RebuildDuringSelfTest()
{
    Rig seam;
    seam.selfTest = true;
    seam.RunMs(21000, 8.0); // 20 s of calm: the test starts
    CHECK(seam.Has("AutoMFG self-test: started"));
    int guard = 0;
    while (seam.factor != 4 && guard++ < 20000)
        seam.Step(8.0); // the cycle climbs to 4x on its own
    CHECK(seam.factor == 4);
    const uint64_t at4x = seam.t;
    while (seam.t < at4x + 3500 && guard++ < 20000)
        seam.Step(8.0); // into the measured second half of the 4x step
    seam.c.NoteExternalStall();
    seam.Step(190.0);
    seam.Step(190.0); // the rebuild's own intervals: ignored by the watch and the readout
    seam.RunMs(60000, 8.0);
    CHECK(seam.Has("AutoMFG self-test: PASS")); // pass/fail unchanged
    CHECK(seam.Has("at 4x real interval 8.0 ms")); // the readout never took the hitch
}

} // namespace

int main()
{
    SelectorIgnoresExactlyTwo();
    GateClosedClearsPendingStall();
    StallAverageSkippedOnBothTicks();
    WatchRestartsAndStallSkipped();
    PersistentStatusFaultAfterRebuildIsStillCaught();
    RepeatedRebuildsReachGiveUp();
    RebuildDuringSelfTest();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: AutoMFG expected-stall seam (two ignored intervals, gate-closed clearing, watch verdict)");
    return 0;
}
