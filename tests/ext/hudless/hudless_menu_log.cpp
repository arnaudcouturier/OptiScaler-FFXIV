// The HUD-less card's slider logging (ext/hudless/HudlessMenuLog.h), driven as the card drives it: a slider reports a change on
// every mouse move, so a change only arms a timer and exactly one line is written once the value has settled, with the value the
// slider came to rest on. A second move restarts the timer; nothing is written before kSettleMs. The lines name the ini key, so a
// session log can be matched with a saved ini.
#include "ext/hudless/HudlessMenuLog.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ext::hudless;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

namespace
{
void TestSettle()
{
    CHECK(SliderSettleLog::kSettleMs == 500);
    SliderSettleLog s;
    CHECK(!s.Armed() && !s.Due(0)); // nothing changed: nothing to log
    s.Observe(0.012f, 1000);
    CHECK(s.Armed() && s.Pending() == 0.012f);
    CHECK(!s.Due(1499)); // one tick short of the settle time
    CHECK(s.Due(1500));  // exactly at the settle time
    CHECK(s.Take() == 0.012f);
    CHECK(!s.Armed() && !s.Due(99999)); // one line per change, not per frame

    // A drag: every move restarts the timer, and only the value it came to rest on is written
    s.Observe(0.02f, 2000);
    CHECK(!s.Due(2300));
    s.Observe(0.03f, 2400);
    CHECK(!s.Due(2800)); // the first change's 500 ms are past, the second one's are not
    CHECK(s.Due(2900));
    CHECK(s.Take() == 0.03f);

    // The same value again is not a move
    s.Observe(0.04f, 3000);
    s.Observe(0.04f, 3400);
    CHECK(s.Due(3500)); // still measured from 3000: one gesture, one line
    CHECK(s.Take() == 0.04f);

    // A clock that jumps backwards does not log the old value early
    s.Observe(0.05f, 5000);
    CHECK(!s.Due(4000));
    CHECK(s.Due(5500));
    CHECK(s.Take() == 0.05f);
}

void TestSliderLines()
{
    HudlessSliderLogs logs;
    // Nothing changed yet
    CHECK(logs.Due(10000).empty());

    // All three sliders moved inside the settle window: nothing is written until each of them settled
    const uint64_t now = 20000;
    logs.maskThreshold.Observe(0.012f, now);
    logs.maskDilation.Observe(3.f, now);
    logs.coverageGuard.Observe(0.05f, now);
    CHECK(logs.Due(now + SliderSettleLog::kSettleMs - 1).empty());
    const std::vector<std::string> lines = logs.Due(now + SliderSettleLog::kSettleMs);
    CHECK(lines.size() == 3);
    CHECK(lines[0] == "HudLess: MaskThreshold set to 0.012 from the menu");
    CHECK(lines[1] == "HudLess: MaskDilation set to 3 px from the menu");
    CHECK(lines[2] == "HudLess: CoverageGuard set to 0.05 (5% of the screen) from the menu");
    CHECK(logs.Due(now + 2 * SliderSettleLog::kSettleMs).empty()); // written once

    // The final value of a drag, not the intermediate ones
    logs.coverageGuard.Observe(0.30f, 30000);
    logs.coverageGuard.Observe(0.60f, 30100);
    CHECK(logs.Due(30500).empty());
    CHECK(logs.Due(30600).size() == 1);
    CHECK(logs.coverageGuard.Pending() == 0.60f);

    // The guard off has its own words (0 is the ini value for "off")
    logs.coverageGuard.Observe(0.f, 40000);
    const std::vector<std::string> off = logs.Due(40500);
    CHECK(off.size() == 1 && off[0] == "HudLess: CoverageGuard set to off from the menu");
    CHECK(HudlessSliderLogs::GuardLine(0.f) == "HudLess: CoverageGuard set to off from the menu");
    CHECK(HudlessSliderLogs::GuardLine(0.6f) == "HudLess: CoverageGuard set to 0.60 (60% of the screen) from the menu");

    // A slider that was never touched writes nothing, even after the settle time
    HudlessSliderLogs untouched;
    CHECK(untouched.Due(999999).empty());
}

void TestReset()
{
    // "Reset advanced" writes one line with the values it restored, and the drags that had not settled are dropped with them (the
    // log must not show a value the Reset already replaced)
    HudlessSliderLogs logs;
    logs.maskThreshold.Observe(0.012f, 1000);
    logs.coverageGuard.Observe(0.05f, 1100);
    const std::vector<std::string> reset = logs.Reset(0.008f, 1, 0.60f);
    CHECK(reset.size() == 1);
    CHECK(reset[0] == "HudLess: advanced settings reset from the menu: MaskThreshold 0.008, MaskDilation 1 px, CoverageGuard 0.60");
    CHECK(!logs.maskThreshold.Armed() && !logs.coverageGuard.Armed());
    CHECK(logs.Due(99999).empty());

    // The guard off reads as "off", like the slider's own line
    const std::vector<std::string> off = logs.Reset(0.008f, 1, 0.f);
    CHECK(off.size() == 1 && off[0].find("CoverageGuard off") != std::string::npos);
}
} // namespace

int main()
{
    TestSettle();
    TestSliderLines();
    TestReset();
    if (g_failed != 0)
    {
        std::printf("hudless_menu_log: %d failure(s)\n", g_failed);
        return 2;
    }
    std::puts("hudless_menu_log: PASS");
    return 0;
}
