// The live output switch's observer (ext/fg/outswitch/SwitchNotify.h): a step is announced once with its phase and the
// line, a duplicate registration is not doubled, the two slots are enough for the recorder and a test, and every step the
// recorder knows maps to its kind (a probation that failed is a rollback, because a rollback is what follows it).
#include "ext/fg/outswitch/SwitchNotify.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
std::vector<std::string> g_seen;

void First(const SwitchEvent& event) noexcept { g_seen.push_back(std::string("1:") + event.text); }
void Second(const SwitchEvent& event) noexcept { g_seen.push_back(std::string("2:") + event.text); }

void RegistrationAndFanOut()
{
    g_seen.clear();
    // A clean slot list: the test binary owns it, nothing else registered.
    RemoveSwitchObserver(&First);
    RemoveSwitchObserver(&Second);

    CHECK(AddSwitchObserver(&First));
    CHECK(AddSwitchObserver(&First)); // idempotent, not a second slot
    SwitchEvent event;
    event.phase = NotifyPhase::Begin;
    event.from = Target::FsrFg;
    event.to = Target::XeFg;
    event.generation = 4;
    event.text = "begin gen=4 from=fsrfg to=xefg why=script";
    Notify(event);
    CHECK(g_seen.size() == 1 && g_seen[0] == "1:begin gen=4 from=fsrfg to=xefg why=script");

    CHECK(AddSwitchObserver(&Second));
    Notify(event);
    CHECK(g_seen.size() == 3 && g_seen[1] == "1:begin gen=4 from=fsrfg to=xefg why=script" &&
          g_seen[2] == "2:begin gen=4 from=fsrfg to=xefg why=script");

    // A third observer has no slot: the registration is refused, and the two that are in still run.
    SwitchObserver refused = [](const SwitchEvent&) noexcept {};
    CHECK(!AddSwitchObserver(refused));

    RemoveSwitchObserver(&First);
    g_seen.clear();
    Notify(event);
    CHECK(g_seen.size() == 1 && g_seen[0].rfind("2:", 0) == 0);
    RemoveSwitchObserver(&Second);
    g_seen.clear();
    Notify(event);
    CHECK(g_seen.empty());
}

void StepsMapToPhases()
{
    const struct
    {
        Step step;
        Fail fail;
        bool valid;
        NotifyPhase phase;
    } cases[] = {
        { Step::Begin, Fail::None, true, NotifyPhase::Begin },
        { Step::Preflight, Fail::None, false, NotifyPhase::Begin },
        { Step::Refused, Fail::NoBridge, true, NotifyPhase::Refused },
        { Step::Barrier, Fail::None, false, NotifyPhase::Begin },
        { Step::Drain, Fail::None, false, NotifyPhase::Begin },
        { Step::Capture, Fail::None, false, NotifyPhase::Begin },
        { Step::Detach, Fail::None, false, NotifyPhase::Begin },
        { Step::Teardown, Fail::None, true, NotifyPhase::Teardown },
        { Step::Unhook, Fail::None, false, NotifyPhase::Begin },
        { Step::Create, Fail::None, true, NotifyPhase::Create },
        { Step::Rehook, Fail::None, false, NotifyPhase::Begin },
        { Step::Adopt, Fail::None, true, NotifyPhase::Adopt },
        { Step::Replay, Fail::None, false, NotifyPhase::Begin },
        { Step::TestPresent, Fail::None, false, NotifyPhase::Begin },
        { Step::Commit, Fail::None, true, NotifyPhase::Commit },
        { Step::Probation, Fail::None, true, NotifyPhase::Probation },
        // A failed probation is a probation record: the rollback that may follow is its own record, once.
        { Step::Probation, Fail::Probation, true, NotifyPhase::Probation },
        { Step::Probation, Fail::Stall, true, NotifyPhase::Probation },
        { Step::Probation, Fail::DeviceLost, true, NotifyPhase::Probation },
        { Step::Rollback, Fail::Create, true, NotifyPhase::Rollback },
        { Step::Limp, Fail::PlainCreate, false, NotifyPhase::Begin },
        { Step::Dead, Fail::OldChainLeaked, false, NotifyPhase::Begin },
        { Step::Detached, Fail::None, false, NotifyPhase::Begin },
        { Step::Idle, Fail::None, false, NotifyPhase::Begin },
    };
    for (const auto& c : cases)
    {
        const PhaseOfStep p = PhaseOf(c.step, c.fail);
        CHECK(p.valid == c.valid);
        if (c.valid && p.valid)
            CHECK(p.phase == c.phase);
    }
}
} // namespace

int main()
{
    RegistrationAndFanOut();
    StepsMapToPhases();
    if (g_failed != 0)
    {
        std::printf("fg_switch_notify: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fg_switch_notify: all passed\n");
    return 0;
}
