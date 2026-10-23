// UI recomposition rule (ext/fg/dlssg/UiRecompositionRule.h): the one pure decision that says whether OptiScaler asks DLSS-G for UI
// recomposition. Requested iff [HudLess] Mode is recomposition AND the active preset is Auto or B (A and C drop it, per the
// nvngx_dlssg.dll 310.9.16 preset table). The decision has no input from the runtime's confirmation, so the very first SetOptions,
// long before any confirmation exists, already carries the final value and Streamline never sees a create-time flag change.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetRequest.h"
#include "ext/fg/dlssg/PresetSwitch.h"
#include "ext/fg/dlssg/UiRecompositionRule.h"

#include <string>
#include <vector>

using namespace ext::fg::preset;

namespace
{
constexpr Unavailable kAll[] = { Unavailable::None, Unavailable::AmpereRuntime, Unavailable::ReplacementRuntime, Unavailable::OtherOutput };

void RuleTable()
{
    // mode wanted x preset, control available: A and C drop it, Auto and B keep it
    struct Row
    {
        int preset;
        bool requested;
        bool dropped;
    };
    const Row rows[] = { { kAuto, true, false }, { kPresetA, false, true }, { kPresetB, true, false }, { kPresetC, false, true } };
    for (const auto& row : rows)
    {
        const auto on = DecideUiRecomposition(true, Unavailable::None, row.preset);
        CHECK_MSG(on.requested == row.requested && on.droppedByPreset == row.dropped && on.preset == row.preset,
                  "preset %d wanted: requested %d dropped %d", row.preset, on.requested, on.droppedByPreset);
        // not wanted (mode off / ui_free / no source / UIRecomposition off): never requested, and nothing is "dropped by the preset"
        const auto off = DecideUiRecomposition(false, Unavailable::None, row.preset);
        CHECK_MSG(!off.requested && !off.droppedByPreset, "preset %d not wanted: requested %d dropped %d", row.preset, off.requested,
                  off.droppedByPreset);
    }
}

void NobodyAskedForRecompositionNeverPauses()
{
    // The HUD-less engine reports "paused by the preset" from droppedByPreset (HudlessEngine.cpp PausedByPreset). Recomposition is wanted
    // only with the mode on, a hud-less source for the game build and [DLSSG] UIRecomposition not false (QueryRecomposition().enabled);
    // when it is not wanted, Preset A or C take nothing away, so the engine must not stop or report a pause, on every availability.
    for (auto why : kAll)
        for (int preset : { kAuto, kPresetA, kPresetB, kPresetC })
        {
            const auto d = DecideUiRecomposition(false, why, preset);
            CHECK_MSG(!d.droppedByPreset && !d.requested, "not wanted, preset %d availability %d: dropped %d requested %d", preset,
                      static_cast<int>(why), d.droppedByPreset, d.requested);
        }
    // the same preset C pauses it as soon as it is wanted
    CHECK(DecideUiRecomposition(true, Unavailable::None, kPresetC).droppedByPreset);
}

void FirstCallCarriesTheFinalValue()
{
    // The decision takes no runtime confirmation: the first SetOptions of a process booted on C asks for nothing, the first one on Auto
    // or B asks for recomposition. This is the case that used to create with the flag on and withdraw it 0.09 s later.
    RequestState boot;
    boot.InitOnce(kPresetC);
    CHECK(!DecideUiRecomposition(true, Unavailable::None, boot.Active()).requested);
    CHECK(DecideUiRecomposition(true, Unavailable::None, boot.Active()).droppedByPreset);
    RequestState bootAuto;
    bootAuto.InitOnce(kAuto);
    CHECK(DecideUiRecomposition(true, Unavailable::None, bootAuto.Active()).requested);
    RequestState bootA;
    bootA.InitOnce(kPresetA);
    CHECK(!DecideUiRecomposition(true, Unavailable::None, bootA.Active()).requested);
    RequestState bootB;
    bootB.InitOnce(kPresetB);
    CHECK(DecideUiRecomposition(true, Unavailable::None, bootB.Active()).requested);
    // an out-of-range configured value means Auto (RequestState), so it keeps the recomposition
    RequestState bootBad;
    bootBad.InitOnce(7);
    CHECK(DecideUiRecomposition(true, Unavailable::None, bootBad.Active()).requested);
    CHECK(DecideUiRecomposition(true, Unavailable::None, 7).requested);
    CHECK(DecideUiRecomposition(true, Unavailable::None, -1).requested);
}

void VersionIndependent()
{
    // 310.9.1.0 ignores Preset C (the runtime reports applied=0 and does not touch the recomposition), 310.9.16 applies it with the
    // recomposition off. The rule has no input that tells them apart: C + recomposition loses the recomposition on both, like the
    // reference build's `preset != 1 && preset != 3`. A signature that carried the runtime's answer would be a different rule.
    CHECK(!DecideUiRecomposition(true, Unavailable::None, kPresetC).requested);
    static_assert(DecideUiRecomposition(true, Unavailable::None, kPresetC).droppedByPreset, "constant-evaluable: no runtime state in it");
    static_assert(!DecideUiRecomposition(true, Unavailable::None, kPresetC).requested, "Preset C never requests the recomposition");
}

void UnavailableControlRunsAsAuto()
{
    // The driver-settings seam only answers while the control is available (PresetDrs.cpp): with the companion runtime, a replacement
    // runtime or another output the preset is never told to the runtime, so the preset cannot take the recomposition away.
    for (auto why : kAll)
    {
        if (why == Unavailable::None)
            continue;
        for (int preset : { kAuto, kPresetA, kPresetB, kPresetC })
        {
            const auto d = DecideUiRecomposition(true, why, preset);
            CHECK_MSG(d.requested && !d.droppedByPreset && d.preset == kAuto, "unavailable %d preset %d", static_cast<int>(why), preset);
        }
    }
    CHECK(EffectiveRequest(Unavailable::None, kPresetC) == kPresetC);
    CHECK(EffectiveRequest(Unavailable::OtherOutput, kPresetC) == kAuto);
    CHECK(EffectiveRequest(Unavailable::None, 9) == kAuto);
}

void LatchRemembersTheFirstLook()
{
    // nothing remembered: a reader that does not commit sees the live value every time and leaves the latch empty
    AvailabilityLatch menuFirst;
    CHECK(menuFirst.Resolve(Unavailable::AmpereRuntime, false) == Unavailable::AmpereRuntime);
    CHECK(menuFirst.Resolve(Unavailable::None, false) == Unavailable::None);
    // the first committing caller fixes it, whoever it is; every later look gets that value, committing or not
    CHECK(menuFirst.Resolve(Unavailable::OtherOutput, true) == Unavailable::OtherOutput);
    CHECK(menuFirst.Resolve(Unavailable::None, false) == Unavailable::OtherOutput);
    CHECK(menuFirst.Resolve(Unavailable::None, true) == Unavailable::OtherOutput);
    CHECK(menuFirst.Resolve(Unavailable::AmpereRuntime, true) == Unavailable::OtherOutput);
    // the case of the review: the driver query at boot (available) comes first, the user ticks the companion-runtime box, the first
    // SetOptions sees the live AmpereRuntime but the rule keeps the value the runtime was told
    AvailabilityLatch boot;
    CHECK(boot.Resolve(Unavailable::None, true) == Unavailable::None); // driver query
    const auto atSetOptions = boot.Resolve(Unavailable::AmpereRuntime, true);
    CHECK(atSetOptions == Unavailable::None);
    CHECK(!DecideUiRecomposition(true, atSetOptions, kPresetC).requested); // C still dropped: the runtime applied C
    CHECK(DecideUiRecomposition(true, Unavailable::AmpereRuntime, kPresetC).requested); // what a second clock would have said
    // and the same when the first SetOptions comes first (no driver query at all)
    AvailabilityLatch late;
    CHECK(late.Resolve(Unavailable::None, true) == Unavailable::None);
    CHECK(late.Resolve(Unavailable::None, true) == Unavailable::None);
    // every value round-trips
    for (auto why : kAll)
    {
        AvailabilityLatch each;
        CHECK(each.Resolve(why, true) == why);
        CHECK(each.Resolve(Unavailable::None == why ? Unavailable::OtherOutput : Unavailable::None, false) == why);
    }
}

void PresetsThatDrop()
{
    CHECK(PresetDropsUiRecomposition(kPresetA));
    CHECK(PresetDropsUiRecomposition(kPresetC));
    CHECK(!PresetDropsUiRecomposition(kAuto));
    CHECK(!PresetDropsUiRecomposition(kPresetB));
    CHECK(!PresetDropsUiRecomposition(4));
    CHECK(!PresetDropsUiRecomposition(-1));
}

// A live switch drives the real Switcher with fake steps. The decision is taken from Requests().Active(), which changes only when the
// switch commits (after the resources were released), so: the old value through the off tick and the waiting ticks (no SetOptions is
// made then anyway), and the NEW value on the first SetOptions after the "Switched" tick, which is the creation of the next feature.
// No SetOptions between the tick and the commit sees a mixed value.
struct Trace
{
    std::vector<bool> beforeCommit; // decision at every tick before Switched
    bool atCreation = false;        // decision at the first SetOptions after Switched
    bool oldValue = false;
    SwitchResult last = SwitchResult::Unchanged;
};

Trace Transition(int from, int to, bool wanted = true)
{
    RequestState state;
    state.InitOnce(from);
    state.NoteDriverQuery(false, false, 0); // the runtime's first driver query was seen (no such setting): Auto can be restored live
    Switcher sw(state);
    SwitchSteps steps;
    steps.evaluate = [] { return true; };
    steps.off = [] { return true; };
    steps.release = [] { return true; };
    steps.install = [] { return true; };
    Trace t;
    t.oldValue = DecideUiRecomposition(wanted, Unavailable::None, state.Active()).requested;
    uint64_t now = 1000;
    for (int i = 0; i < 64; ++i)
    {
        t.last = sw.Apply(to, steps, now += 16);
        if (t.last == SwitchResult::Switched)
            break;
        t.beforeCommit.push_back(DecideUiRecomposition(wanted, Unavailable::None, state.Active()).requested);
    }
    t.atCreation = DecideUiRecomposition(wanted, Unavailable::None, state.Active()).requested;
    return t;
}

void LiveTransitions()
{
    struct Case
    {
        int from, to;
        bool before, after;
    };
    const Case cases[] = {
        { kAuto, kPresetC, true, false },     // Auto -> C: the next feature is created without the flag, no flip after the switch's creation
        { kPresetC, kAuto, false, true },     // C -> Auto: the next feature is created with the flag
        { kAuto, kPresetA, true, false },
        { kPresetA, kAuto, false, true },
        { kAuto, kPresetB, true, true },      // B keeps it: the same value before and after
        { kPresetB, kPresetC, true, false },
        { kPresetC, kPresetB, false, true },
        { kPresetA, kPresetC, false, false }, // both drop it: never on
        { kPresetC, kPresetA, false, false },
    };
    for (const auto& c : cases)
    {
        const auto t = Transition(c.from, c.to);
        CHECK_MSG(t.last == SwitchResult::Switched, "%d -> %d did not switch", c.from, c.to);
        CHECK_MSG(t.oldValue == c.before, "%d -> %d old value", c.from, c.to);
        CHECK_MSG(!t.beforeCommit.empty(), "%d -> %d no tick before the commit", c.from, c.to);
        for (bool seen : t.beforeCommit)
            CHECK_MSG(seen == c.before, "%d -> %d: a tick before the commit saw %d, expected %d", c.from, c.to, seen, c.before);
        CHECK_MSG(t.atCreation == c.after, "%d -> %d: the creation after the switch saw %d, expected %d", c.from, c.to, t.atCreation,
                  c.after);
    }
    // not wanted (HUD-less off or ui_free): never requested at any point of any switch
    for (int from : { kAuto, kPresetA, kPresetB, kPresetC })
        for (int to : { kAuto, kPresetA, kPresetB, kPresetC })
        {
            if (from == to)
                continue;
            const auto t = Transition(from, to, false);
            CHECK(!t.oldValue && !t.atCreation);
            for (bool seen : t.beforeCommit)
                CHECK(!seen);
        }
}
} // namespace

int main()
{
    RuleTable();
    NobodyAskedForRecompositionNeverPauses();
    FirstCallCarriesTheFinalValue();
    VersionIndependent();
    UnavailableControlRunsAsAuto();
    PresetsThatDrop();
    LatchRemembersTheFirstLook();
    LiveTransitions();
    if (g_failures)
    {
        std::printf("fg_ui_recomposition_rule: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_ui_recomposition_rule: rule table, first call, version independence, unavailable control, live transitions passed");
    return 0;
}
