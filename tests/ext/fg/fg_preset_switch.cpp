// DLSS-G live preset switch (ext/fg/dlssg/PresetSwitch.h): the state machine that runs on the FG dispatch thread with
// injected steps and an injected clock, on top of the request model. The order is: check the runtime read-only (evaluate), FG off,
// WAIT (later ticks), release the DLSS-G resources, install the hooks, commit. Every failure keeps the old request.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetRequest.h"
#include "ext/fg/dlssg/PresetSwitch.h"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ext::fg::preset;

namespace
{
struct Fake
{
    std::vector<std::string> calls;
    bool evaluateOk = true, offOk = true, releaseOk = true, installOk = true;
    bool throwAtRelease = false;
    int activated = 0;

    SwitchSteps Steps()
    {
        SwitchSteps s;
        s.evaluate = [this] { calls.push_back("evaluate"); return evaluateOk; };
        s.off = [this] { calls.push_back("off"); return offOk; };
        s.release = [this]
        {
            calls.push_back("release");
            if (throwAtRelease)
                throw std::runtime_error("boom");
            return releaseOk;
        };
        s.install = [this] { calls.push_back("install"); return installOk; };
        s.activated = [this] { calls.push_back("activated"); ++activated; };
        return s;
    }
    std::string Order() const
    {
        std::string out;
        for (const auto& c : calls)
            out += (out.empty() ? "" : ",") + c;
        return out;
    }
};

struct Boot
{
    RequestState s;
    explicit Boot(int request) { s.InitOnce(request); }
};

// One frame per call: 16 ms later.
struct Clock
{
    uint64_t now = 1000;
    uint64_t Tick() { return now += 16; }
};

// Drives Apply once per frame until it stops being InProgress; returns the number of Apply calls and the final result.
struct Run
{
    SwitchResult result = SwitchResult::Unchanged;
    int calls = 0;
};
Run Drive(Switcher& sw, int requested, const SwitchSteps& steps, Clock& clock, int limit = 64)
{
    Run r;
    do
    {
        r.result = sw.Apply(requested, steps, clock.Tick());
        ++r.calls;
    } while (r.result == SwitchResult::InProgress && r.calls < limit);
    return r;
}

void SameRequestDoesNothing()
{
    Boot bs(kPresetA);
    Switcher sw(bs.s);
    Fake f;
    Clock c;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetA, steps, c.Tick()) == SwitchResult::Unchanged);
    CHECK(f.calls.empty());
    CHECK(bs.s.Active() == kPresetA && sw.Generation() == 0 && !sw.Pending());
}

void SuccessRunsTheStepsInOrderOverSeveralTicks()
{
    Boot bs(kAuto);
    bs.s.NoteDriverQuery(true, true, 2);
    Switcher sw(bs.s);
    Fake f;
    Clock c;
    auto steps = f.Steps();

    // tick 1: the runtime is checked and FG goes off; nothing is released yet and the request is unchanged
    CHECK(sw.Apply(kPresetB, steps, c.Tick()) == SwitchResult::InProgress);
    CHECK(f.Order() == "evaluate,off");
    CHECK(sw.Pending() && bs.s.Active() == kAuto);
    // the next ticks only wait
    CHECK(sw.Apply(kPresetB, steps, c.Tick()) == SwitchResult::InProgress);
    CHECK(f.Order() == "evaluate,off");
    // the release happens on a LATER tick, then the hooks, then the commit
    SwitchResult last = SwitchResult::InProgress;
    int ticks = 2;
    while (last == SwitchResult::InProgress && ticks < 40)
    {
        last = sw.Apply(kPresetB, steps, c.Tick());
        ++ticks;
    }
    CHECK(last == SwitchResult::Switched);
    CHECK(f.Order() == "evaluate,off,release,install,activated");
    CHECK(bs.s.Active() == kPresetB && sw.Generation() == 1 && !sw.Pending());
    CHECK(sw.LastFail() == SwitchFail::None && sw.FailedRequest() == -1);
}

void ReleaseNeverInTheSameTickAsOff()
{
    // whatever the clock does, release comes at least kMinWaitTicks ticks after off, and at least kMinWaitMs later
    for (uint64_t step : { 0u, 1u, 16u, 5000u })
    {
        Boot bs(kAuto);
        bs.s.NoteDriverQuery(true, true, 0);
        Switcher sw(bs.s);
        Fake f;
        auto steps = f.Steps();
        uint64_t now = 1000;
        int tick = 0, offTick = -1, releaseTick = -1;
        uint64_t offAt = 0, releaseAt = 0;
        for (; tick < 200 && releaseTick < 0; ++tick)
        {
            const size_t before = f.calls.size();
            sw.Apply(kPresetA, steps, now);
            for (size_t i = before; i < f.calls.size(); ++i)
            {
                if (f.calls[i] == "off")
                    offTick = tick, offAt = now;
                if (f.calls[i] == "release")
                    releaseTick = tick, releaseAt = now;
            }
            now += step;
        }
        if (step == 0)
        {   // time never advances: the wait cannot finish (the tick count alone is not enough)
            CHECK(offTick == 0 && releaseTick < 0);
            continue;
        }
        CHECK_MSG(offTick == 0 && releaseTick >= offTick + kMinWaitTicks, "step %llu: off %d release %d", (unsigned long long) step, offTick, releaseTick);
        CHECK_MSG(releaseAt - offAt >= kMinWaitMs, "step %llu: %llu ms", (unsigned long long) step, (unsigned long long) (releaseAt - offAt));
    }
}

void EachFailureKeepsTheOldRequest()
{
    struct Case
    {
        bool evaluate, off, release, install;
        const char* order;
        SwitchFail fail;
    };
    const Case cases[] = {
        { false, true, true, true, "evaluate", SwitchFail::Prepare },
        { true, false, true, true, "evaluate,off", SwitchFail::Off },
        { true, true, false, true, "evaluate,off,release", SwitchFail::Release },
        { true, true, true, false, "evaluate,off,release,install", SwitchFail::Install },
    };
    for (const auto& c : cases)
    {
        Boot bs(kPresetA);
        Switcher sw(bs.s);
        Fake f;
        Clock clock;
        f.evaluateOk = c.evaluate;
        f.offOk = c.off;
        f.releaseOk = c.release;
        f.installOk = c.install;
        auto steps = f.Steps();
        const auto r = Drive(sw, kPresetC, steps, clock);
        CHECK_MSG(r.result == SwitchResult::RestartNeeded, "%s", c.order);
        CHECK_MSG(f.Order() == c.order, "%s -> %s", c.order, f.Order().c_str());
        CHECK(bs.s.Active() == kPresetA); // the old request is kept
        CHECK(f.activated == 0 && !sw.Pending());
        CHECK(sw.LastFail() == c.fail && sw.FailedRequest() == kPresetC && sw.Generation() == 0);

        // no retry of the same target every frame
        f.calls.clear();
        CHECK(sw.Apply(kPresetC, steps, clock.Tick()) == SwitchResult::RestartNeeded);
        CHECK(f.calls.empty() && sw.RestartNeeded(kPresetC) && !sw.RestartNeeded(kPresetB));

        // another target is tried again, and succeeds once the steps work
        f.evaluateOk = f.offOk = f.releaseOk = f.installOk = true;
        const auto again = Drive(sw, kPresetB, steps, clock);
        CHECK(again.result == SwitchResult::Switched);
        CHECK(bs.s.Active() == kPresetB && sw.FailedRequest() == -1 && sw.LastFail() == SwitchFail::None);
    }
}

void NothingIsInstalledBeforeFgIsQuiet()
{
    // the hooks are installed only after off AND release succeeded; a failure of either never reaches install
    for (int failAt = 0; failAt < 2; ++failAt)
    {
        Boot bs(kPresetA);
        Switcher sw(bs.s);
        Fake f;
        Clock clock;
        (failAt == 0 ? f.offOk : f.releaseOk) = false;
        auto steps = f.Steps();
        Drive(sw, kPresetB, steps, clock);
        CHECK(f.Order().find("install") == std::string::npos);
    }
    // and the evaluate step (read-only) is the only one that runs while FG is still running
    Boot bs(kPresetA);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    sw.Apply(kPresetB, steps, clock.Tick());
    CHECK(f.Order() == "evaluate,off");
}

void GoingBackCancelsAPendingSwitch()
{
    Boot bs(kPresetA);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetB, steps, clock.Tick()) == SwitchResult::InProgress);
    CHECK(sw.Pending());
    f.calls.clear();
    CHECK(sw.Apply(kPresetA, steps, clock.Tick()) == SwitchResult::Unchanged); // the user went back before the release
    CHECK(!sw.Pending() && f.calls.empty() && bs.s.Active() == kPresetA);
    // a later request starts a fresh sequence
    CHECK(sw.Apply(kPresetB, steps, clock.Tick()) == SwitchResult::InProgress);
    CHECK(f.Order() == "evaluate,off");
}

void ChangingTheTargetRestartsTheSequence()
{
    Boot bs(kAuto);
    bs.s.NoteDriverQuery(true, true, 0);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    sw.Apply(kPresetA, steps, clock.Tick());
    CHECK(sw.Pending());
    f.calls.clear();
    CHECK(sw.Apply(kPresetC, steps, clock.Tick()) == SwitchResult::InProgress); // a new target: evaluate and off again
    CHECK(f.Order() == "evaluate,off");
    const auto r = Drive(sw, kPresetC, steps, clock);
    CHECK(r.result == SwitchResult::Switched && bs.s.Active() == kPresetC);
}

void AThrowingStepIsAFailure()
{
    Boot bs(kAuto);
    bs.s.NoteDriverQuery(true, true, 1);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    f.throwAtRelease = true;
    auto steps = f.Steps();
    CHECK(Drive(sw, kPresetA, steps, clock).result == SwitchResult::RestartNeeded);
    CHECK(bs.s.Active() == kAuto && sw.LastFail() == SwitchFail::Release);
}

void AMissingStepFailsClosed()
{
    Boot bs(kPresetA);
    Switcher sw(bs.s);
    SwitchSteps steps; // nothing wired: no step may count as done
    Clock clock;
    CHECK(sw.Apply(kPresetB, steps, clock.Tick()) == SwitchResult::RestartNeeded);
    CHECK(bs.s.Active() == kPresetA && sw.LastFail() == SwitchFail::Prepare);

    // a missing install step is a failure too (the other steps wired)
    Boot b2(kPresetA);
    Switcher s2(b2.s);
    Fake f;
    auto wired = f.Steps();
    wired.install = nullptr;
    CHECK(Drive(s2, kPresetB, wired, clock).result == SwitchResult::RestartNeeded);
    CHECK(s2.LastFail() == SwitchFail::Install && b2.s.Active() == kPresetA);
}

void AutoNeedsAKnownOriginal()
{
    // booted with a request, baseline never seen: Auto cannot be restored live; nothing is touched
    Boot bs(kPresetB);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    CHECK(sw.Apply(kAuto, steps, clock.Tick()) == SwitchResult::RestartNeeded);
    CHECK(f.calls.empty());
    CHECK(sw.LastFail() == SwitchFail::CannotRestoreAuto && sw.FailedRequest() == kAuto && bs.s.Active() == kPresetB);

    // once the driver reply was seen, it works
    Boot kb(kPresetB);
    kb.s.NoteDriverQuery(false, false, 0);
    Switcher ok(kb.s);
    Fake g;
    auto stepsOk = g.Steps();
    CHECK(Drive(ok, kAuto, stepsOk, clock).result == SwitchResult::Switched);
    CHECK(kb.s.Active() == kAuto);
    CHECK(kb.s.ReadCached(kRenderPresetSettingId).handled && !kb.s.ReadCached(kRenderPresetSettingId).found);
}

// PRESET-BOOT phase 1, item 3. The boot answers the preset id ONLY (the DLSS override id is never answered). The live switch needs
// the driver's own answer as the baseline for going back to Auto after a boot on A/B/C; that baseline comes from the preset id's
// reply alone, so nothing of the live switch relied on the old two-id answer. The sequence is driven through the real hook entry
// (HandleDriverReply for both ids) and the state machine, with the in-game order: boot on C, C -> Auto -> A -> Auto.
void BootOnAPresetThenLiveSwitches()
{
    struct Reply
    {
        bool ok, isNumber;
        uint32_t value;
        bool found; // the reader answers "found" with `value`
    };
    // what the driver says to the preset query: no such setting (the normal case on the maintainer's machine), a profile value, a 0
    for (const Reply& driver : { Reply { false, false, 0, false }, Reply { true, true, 2, true }, Reply { true, true, 0, true } })
        for (int bootPreset : { kPresetA, kPresetB, kPresetC })
        {
            Boot bs(bootPreset);
            // boot: the runtime asks for the override id and for the preset id (either order); only the preset id is answered
            CHECK(!HandleDriverReply(bs.s, kOverrideSettingId, false, false, 0).replace);
            const auto edit = HandleDriverReply(bs.s, kRenderPresetSettingId, driver.ok, driver.isNumber, driver.value);
            CHECK(edit.replace && edit.value == static_cast<uint32_t>(bootPreset));
            CHECK(!HandleDriverReply(bs.s, kOverrideSettingId, true, true, 1).replace);
            CHECK(bs.s.Boot() == bootPreset && bs.s.Active() == bootPreset);
            CHECK(bs.s.Base().kind == (driver.found ? Baseline::Kind::Value : Baseline::Kind::NotFound));
            CHECK(bs.s.CanReturnToAuto());

            Switcher sw(bs.s);
            Fake f;
            Clock clock;
            auto steps = f.Steps();
            auto expectAuto = [&]
            {
                const auto a = bs.s.ReadCached(kRenderPresetSettingId);
                CHECK(a.handled && a.found == driver.found && (!driver.found || a.value == driver.value));
                CHECK(!bs.s.ReadCached(kOverrideSettingId).handled);
            };
            // C -> Auto: no refusal, the reader gets the driver's original back (a value, or "not found")
            CHECK(Drive(sw, kAuto, steps, clock).result == SwitchResult::Switched);
            CHECK(sw.LastFail() == SwitchFail::None && !sw.RestartNeeded(kAuto) && bs.s.Active() == kAuto);
            expectAuto();
            // Auto -> A (or B when the boot was A): the request is answered again
            const int other = bootPreset == kPresetA ? kPresetB : kPresetA;
            CHECK(Drive(sw, other, steps, clock).result == SwitchResult::Switched);
            const auto live = bs.s.ReadCached(kRenderPresetSettingId);
            CHECK(live.handled && live.found && live.value == static_cast<uint32_t>(other));
            CHECK(!bs.s.ReadCached(kOverrideSettingId).handled);
            // and Auto again: still the original, not the stale request
            CHECK(Drive(sw, kAuto, steps, clock).result == SwitchResult::Switched);
            CHECK(sw.LastFail() == SwitchFail::None && bs.s.Active() == kAuto);
            expectAuto();
            CHECK(sw.Generation() == 3 && f.activated == 3);
        }

    // the driver was never asked (an old runtime, or the query came before the hook): Auto cannot be restored live, as before
    Boot never(kPresetC);
    Switcher sw(never.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    CHECK(sw.Apply(kAuto, steps, clock.Tick()) == SwitchResult::RestartNeeded);
    CHECK(sw.LastFail() == SwitchFail::CannotRestoreAuto && f.calls.empty() && never.s.Active() == kPresetC);
    // the override id alone never counts as "the driver was asked"
    HandleDriverReply(never.s, kOverrideSettingId, true, true, 1);
    CHECK(!never.s.CanReturnToAuto() && !never.s.QuerySeen());
}

void InvalidRequestIsRejected()
{
    Boot bs(kPresetA);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    for (int bad : { -1, 4, 99 })
    {
        CHECK(sw.Apply(bad, steps, clock.Tick()) == SwitchResult::Unchanged);
        CHECK(bs.s.Active() == kPresetA);
    }
    CHECK(f.calls.empty() && sw.FailedRequest() == -1);
}

void ASequenceOfSwitches()
{
    Boot bs(kAuto);
    bs.s.NoteDriverQuery(true, true, 2);
    Switcher sw(bs.s);
    Fake f;
    Clock clock;
    auto steps = f.Steps();
    for (int r : { kPresetA, kPresetB, kPresetC, kAuto, kPresetB })
        CHECK(Drive(sw, r, steps, clock).result == SwitchResult::Switched);
    CHECK(sw.Generation() == 5 && f.activated == 5 && bs.s.Active() == kPresetB);
}

void AvailabilityReasons()
{
    // the live control is off, with a stated reason, for the RTX 20/30 companion runtime and for everything that is not the
    // built-in DLSS-G runtime; the companion reason wins when several apply
    CHECK(CheckAvailable(false, false, true) == Unavailable::None);
    CHECK(CheckAvailable(true, false, true) == Unavailable::AmpereRuntime);
    CHECK(CheckAvailable(true, true, false) == Unavailable::AmpereRuntime);
    CHECK(CheckAvailable(false, true, true) == Unavailable::ReplacementRuntime);
    CHECK(CheckAvailable(false, true, false) == Unavailable::ReplacementRuntime);
    CHECK(CheckAvailable(false, false, false) == Unavailable::OtherOutput);
    for (auto u : { Unavailable::AmpereRuntime, Unavailable::ReplacementRuntime, Unavailable::OtherOutput })
        CHECK(std::string(UnavailableText(u)).size() > 20);
    CHECK(std::string(UnavailableText(Unavailable::None)).empty());
    CHECK(std::string(UnavailableText(Unavailable::AmpereRuntime)).find("RTX 20/30") != std::string::npos);
    CHECK(std::string(UnavailableText(Unavailable::OtherOutput)).find("not DLSS-G") != std::string::npos);
}
} // namespace

int main()
{
    SameRequestDoesNothing();
    SuccessRunsTheStepsInOrderOverSeveralTicks();
    ReleaseNeverInTheSameTickAsOff();
    EachFailureKeepsTheOldRequest();
    NothingIsInstalledBeforeFgIsQuiet();
    GoingBackCancelsAPendingSwitch();
    ChangingTheTargetRestartsTheSequence();
    AThrowingStepIsAFailure();
    AMissingStepFailsClosed();
    AutoNeedsAKnownOriginal();
    BootOnAPresetThenLiveSwitches();
    InvalidRequestIsRejected();
    ASequenceOfSwitches();
    AvailabilityReasons();
    if (g_failures)
    {
        std::printf("fg_preset_switch: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_switch: evaluate+off, wait, release, install, commit order; every failure keeps the old request; no retry loop passed");
    return 0;
}
