// DLSS-G live preset switch (ext/fg/dlssg/PresetSwitch.h): the state machine that runs on the FG dispatch thread with
// injected steps (prepare, off, free), on top of the request model. Every failure keeps the old request.
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
    bool prepareOk = true, offOk = true, freeOk = true;
    bool throwAt = false; // the free step throws
    int activated = 0;

    SwitchSteps Steps()
    {
        SwitchSteps s;
        s.prepare = [this] { calls.push_back("prepare"); return prepareOk; };
        s.off = [this] { calls.push_back("off"); return offOk; };
        s.release = [this]
        {
            calls.push_back("free");
            if (throwAt)
                throw std::runtime_error("boom");
            return freeOk;
        };
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

void SameRequestDoesNothing()
{
    Boot stateBoot(kPresetA);
    auto& state = stateBoot.s;
    Switcher sw(state);
    Fake f;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetA, steps) == SwitchResult::Unchanged);
    CHECK(f.calls.empty());
    CHECK(state.Active() == kPresetA);
    CHECK(sw.Generation() == 0);
}

void SuccessRunsTheStepsInOrder()
{
    Boot stateBoot(kAuto);
    auto& state = stateBoot.s;
    state.NoteDriverQuery(true, true, 2);
    Switcher sw(state);
    Fake f;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetB, steps) == SwitchResult::Switched);
    CHECK(f.Order() == "prepare,off,free,activated");
    CHECK(state.Active() == kPresetB);
    CHECK(sw.Generation() == 1);
    CHECK(sw.LastFail() == SwitchFail::None && sw.FailedRequest() == -1);
}

void ActivatedSeesTheNewRequest()
{
    // the activation callback runs after the commit (the readout and the history reset read the new request)
    Boot stateBoot(kAuto);
    auto& state = stateBoot.s;
    state.NoteDriverQuery(true, true, 0);
    Switcher sw(state);
    int seen = -1;
    SwitchSteps steps;
    steps.prepare = [] { return true; };
    steps.off = [] { return true; };
    steps.release = [] { return true; };
    steps.activated = [&] { seen = state.Active(); };
    CHECK(sw.Apply(kPresetC, steps) == SwitchResult::Switched);
    CHECK(seen == kPresetC);
}

void EachFailureKeepsTheOldRequest()
{
    struct Case
    {
        bool prepare, off, release;
        const char* order;
        SwitchFail fail;
    };
    const Case cases[] = {
        { false, true, true, "prepare", SwitchFail::Prepare },
        { true, false, true, "prepare,off", SwitchFail::Off },
        { true, true, false, "prepare,off,free", SwitchFail::Release },
    };
    for (const auto& c : cases)
    {
        Boot stateBoot(kPresetA);
        auto& state = stateBoot.s;
        Switcher sw(state);
        Fake f;
        f.prepareOk = c.prepare;
        f.offOk = c.off;
        f.freeOk = c.release;
        auto steps = f.Steps();
        CHECK_MSG(sw.Apply(kPresetC, steps) == SwitchResult::RestartNeeded, "%s", c.order);
        CHECK_MSG(f.Order() == c.order, "%s -> %s", c.order, f.Order().c_str());
        CHECK(state.Active() == kPresetA); // the old request is kept
        CHECK(f.activated == 0);
        CHECK(sw.LastFail() == c.fail && sw.FailedRequest() == kPresetC);
        CHECK(sw.Generation() == 0);

        // no retry of the same target every frame
        f.calls.clear();
        CHECK(sw.Apply(kPresetC, steps) == SwitchResult::RestartNeeded);
        CHECK(f.calls.empty());
        CHECK(sw.RestartNeeded(kPresetC));
        CHECK(!sw.RestartNeeded(kPresetB));

        // another target is tried again, and succeeds once the steps work
        f.prepareOk = f.offOk = f.freeOk = true;
        CHECK(sw.Apply(kPresetB, steps) == SwitchResult::Switched);
        CHECK(state.Active() == kPresetB && sw.FailedRequest() == -1 && sw.LastFail() == SwitchFail::None);
    }
}

void GoingBackToTheActiveRequestClearsTheFailure()
{
    Boot stateBoot(kPresetA);
    auto& state = stateBoot.s;
    Switcher sw(state);
    Fake f;
    f.freeOk = false;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetB, steps) == SwitchResult::RestartNeeded);
    CHECK(sw.RestartNeeded(kPresetB));
    f.calls.clear();
    CHECK(sw.Apply(kPresetA, steps) == SwitchResult::Unchanged); // the user went back
    CHECK(f.calls.empty());
    CHECK(!sw.RestartNeeded(kPresetB) && sw.FailedRequest() == -1);
    // asking for B again tries again (the failure was forgotten)
    f.freeOk = true;
    f.calls.clear();
    CHECK(sw.Apply(kPresetB, steps) == SwitchResult::Switched);
}

void AThrowingStepIsAFailure()
{
    Boot stateBoot(kAuto);
    auto& state = stateBoot.s;
    state.NoteDriverQuery(true, true, 1);
    Switcher sw(state);
    Fake f;
    f.throwAt = true;
    auto steps = f.Steps();
    CHECK(sw.Apply(kPresetA, steps) == SwitchResult::RestartNeeded);
    CHECK(state.Active() == kAuto);
    CHECK(sw.LastFail() == SwitchFail::Release);
}

void AutoNeedsAKnownOriginal()
{
    // booted with a request, baseline never seen: Auto cannot be restored live; nothing is touched
    Boot stateBoot(kPresetB);
    auto& state = stateBoot.s;
    Switcher sw(state);
    Fake f;
    auto steps = f.Steps();
    CHECK(sw.Apply(kAuto, steps) == SwitchResult::RestartNeeded);
    CHECK(f.calls.empty());
    CHECK(sw.LastFail() == SwitchFail::CannotRestoreAuto && sw.FailedRequest() == kAuto);
    CHECK(state.Active() == kPresetB);

    // once the driver reply was seen, it works
    Boot knownBoot(kPresetB);
    auto& known = knownBoot.s;
    known.NoteDriverQuery(false, false, 0);
    Switcher ok(known);
    Fake g;
    auto stepsOk = g.Steps();
    CHECK(ok.Apply(kAuto, stepsOk) == SwitchResult::Switched);
    CHECK(known.Active() == kAuto);
    CHECK(!known.ReadCached(kRenderPresetSettingId).found && known.ReadCached(kRenderPresetSettingId).handled);
}

void InvalidRequestIsRejected()
{
    Boot stateBoot(kPresetA);
    auto& state = stateBoot.s;
    Switcher sw(state);
    Fake f;
    auto steps = f.Steps();
    for (int bad : { -1, 4, 99 })
    {
        CHECK(sw.Apply(bad, steps) == SwitchResult::Unchanged);
        CHECK(state.Active() == kPresetA);
    }
    CHECK(f.calls.empty());
    CHECK(sw.FailedRequest() == -1);
}

void ASequenceOfSwitches()
{
    Boot stateBoot(kAuto);
    auto& state = stateBoot.s;
    state.NoteDriverQuery(true, true, 2);
    Switcher sw(state);
    Fake f;
    auto steps = f.Steps();
    for (int r : { kPresetA, kPresetB, kPresetC, kAuto, kPresetB })
        CHECK(sw.Apply(r, steps) == SwitchResult::Switched);
    CHECK(sw.Generation() == 5 && f.activated == 5);
    CHECK(state.Active() == kPresetB);
}
} // namespace

int main()
{
    SameRequestDoesNothing();
    SuccessRunsTheStepsInOrder();
    ActivatedSeesTheNewRequest();
    EachFailureKeepsTheOldRequest();
    GoingBackToTheActiveRequestClearsTheFailure();
    AThrowingStepIsAFailure();
    AutoNeedsAKnownOriginal();
    InvalidRequestIsRejected();
    ASequenceOfSwitches();
    if (g_failures)
    {
        std::printf("fg_preset_switch: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_switch: order, failure at every step keeps the old request, no retry loop, Auto needs a known original passed");
    return 0;
}
