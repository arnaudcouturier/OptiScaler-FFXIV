// A rebuild hitch at the documented 0.2 s scale inside an open watch is judged on the calm that follows,
// never as the switch's own re-creation stall: the two ignored ticks restart the watch and their own intervals
// are not recorded by it, so the verdict never carries the rebuild hitch as the switch's signature (which would
// reduce trust for 12 s and feed the counters that stop automatic mode).
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
    double target = 263.0;
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
        in.selfTest = false;
        in.maxCfg = 4;
        in.targetCfg = target;
        in.limit = 0.0;
        in.refresh = 240.0;
        in.gate = gate;
        in.stateValid = true;
        in.slStatus = 0;
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

    uint64_t SwitchUp()
    {
        target = 395.0;
        for (int i = 0; i < 2000; ++i)
        {
            const uint64_t at = t;
            if (Step(8.0).setFactor)
                return at;
        }
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

void RebuildHitchNotJudgedAsSwitchStall()
{
    Rig seam;
    seam.RunMs(5000, 8.0);
    CHECK(seam.SwitchUp() != 0 && seam.factor == 3);
    seam.c.NoteExternalStall();
    seam.Step(190.0);
    seam.Step(190.0);
    seam.RunMs(2500, 8.0);
    CHECK(seam.factor == 3); // the switch stands: the verdict does not switch back
    bool settled = false;
    for (const auto& l : seam.lines)
        if (l.second.find("settled") != std::string::npos)
            settled = true;
    CHECK(settled); // the verdict runs on the calm that follows
    CHECK(!seam.Has("cost a")); // ...without carrying the rebuild hitch as the switch's signature
}
} // namespace

int main()
{
    RebuildHitchNotJudgedAsSwitchStall();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: rebuild hitch inside an open watch judges the calm, never the switch");
    return 0;
}
