// The script and fault keys of the live output switch (ext/fg/outswitch/SwitchScript.h): the parser the benchmark drives, the
// step that is due and the catch-up rule, and the fault names. Pure: the header has no Windows or upstream include.
#include "ext/fg/outswitch/SwitchScript.h"

#include <cstdio>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                      \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

static void ScriptShape()
{
    std::printf("  the script parses seconds:target pairs in order\n");
    Script script;
    ParseScript("45:xefg,90:none,135:fsrfg", script);
    CHECK(script.count == 3);
    CHECK(script.steps[0].atMs == 45000 && script.steps[0].target == Target::XeFg);
    CHECK(script.steps[1].atMs == 90000 && script.steps[1].target == Target::Plain);
    CHECK(script.steps[2].atMs == 135000 && script.steps[2].target == Target::FsrFg);

    // Spaces are allowed around the parts; an empty entry and an unknown target are dropped, the rest survives.
    Script spaced;
    ParseScript(" 10 : xefg , ,20:bogus, 30:fsrfg ", spaced);
    CHECK(spaced.count == 2);
    CHECK(spaced.steps[0].atMs == 10000 && spaced.steps[0].target == Target::XeFg);
    CHECK(spaced.steps[1].atMs == 30000 && spaced.steps[1].target == Target::FsrFg);

    // A missing time, a fractional one and a missing target are not steps.
    Script bad;
    ParseScript("xefg,45:,45:1.5,45", bad);
    CHECK(bad.count == 0);
    Script emptyText;
    ParseScript("", emptyText);
    CHECK(emptyText.Empty());
    ParseScript(nullptr, emptyText);
    CHECK(emptyText.Empty());
    std::printf("    [ok] three steps, two of four kept, nothing for junk\n");
}

static void DueAndCatchUp()
{
    std::printf("  a step fires at its time, and a slow frame catches up instead of repeating\n");
    Script script;
    ParseScript("2:xefg,4:none", script);
    CHECK(!script.started);
    Target target = Target::FsrFg;
    CHECK(!script.Due(1000, target)); // the clock has not started
    script.Start(1000);
    CHECK(!script.Due(2500, target)); // 1.5 s after the start: nothing due
    CHECK(script.next == 0);
    CHECK(script.Due(3000, target)); // 2 s
    CHECK(target == Target::XeFg);
    CHECK(script.next == 1);
    CHECK(!script.Due(3500, target)); // the same step does not fire again
    // One call 5 s in fires the step that is due and skips over the one that passed: the last due target wins.
    CHECK(script.Due(9000, target));
    CHECK(target == Target::Plain);
    CHECK(script.next == 2);
    CHECK(!script.Due(9000, target));
    std::printf("    [ok] fired at 2 s, once, and the catch-up ends on the last due step\n");
}

static void FaultNames()
{
    std::printf("  the fault key names one step, and anything else is no fault\n");
    CHECK(ParseFaultStep("drain") == Step::Drain);
    CHECK(ParseFaultStep("teardown") == Step::Teardown);
    CHECK(ParseFaultStep("create") == Step::Create);
    CHECK(ParseFaultStep("adopt") == Step::Adopt);
    CHECK(ParseFaultStep("replay") == Step::Replay);
    CHECK(ParseFaultStep("probation") == Step::Probation);
    CHECK(ParseFaultStep(" drain") == Step::Drain); // the ini reader keeps the value, not the spaces
    CHECK(ParseFaultStep("") == Step::Idle);
    CHECK(ParseFaultStep(nullptr) == Step::Idle);
    CHECK(ParseFaultStep("drain2") == Step::Idle); // a longer name is not the step
    CHECK(ParseFaultStep("preflight") == Step::Idle); // a preflight refusal is tested without a fault injection
    std::printf("    [ok] six steps names, everything else no fault\n");
}

static void FullList()
{
    std::printf("  the list keeps as many steps as it can hold, in order\n");
    Script script;
    ParseScript("1:xefg,2:xefg,3:xefg,4:xefg,5:xefg,6:xefg,7:xefg,8:xefg,9:xefg,10:xefg,11:xefg,12:xefg,13:xefg,14:xefg,"
                "15:xefg,16:xefg,17:xefg,18:xefg",
                script);
    CHECK(script.count == kMaxScriptSteps);
    CHECK(script.steps[0].atMs == 1000);
    CHECK(script.steps[kMaxScriptSteps - 1].atMs == static_cast<uint32_t>(kMaxScriptSteps) * 1000);
    std::printf("    [ok] %zu steps kept of 18 written\n", script.count);
}

int main()
{
    ScriptShape();
    DueAndCatchUp();
    FaultNames();
    FullList();
    if (g_failed == 0)
        std::printf("ALL SCRIPT SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
