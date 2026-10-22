// DLSS-G render preset: the state sentences of the menu (ext/fg/dlssg/PresetMenuText.h), pure text. PRESET-BOOT phase 1, item 2.
// The selector stays visible; when the choice and what the runtime was told differ, the menu says "Chosen: X, in use: Y" with the
// reason. "In use" for a preset is only ever said for a preset the runtime confirmed. "Chosen" is the value of the selector (the ini
// value once the user pressed Save Settings): the menu cannot read what is on disk without an upstream line.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetMenuText.h"

#include <string>

using namespace ext::fg::preset;

namespace
{
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void InUseLabels()
{
    CHECK(InUseLabel(kAuto, Effect::Unknown) == "Auto");
    CHECK(InUseLabel(kAuto, Effect::NotApplied) == "Auto");
    CHECK(InUseLabel(kPresetC, Effect::Applied) == "Preset C");
    CHECK(InUseLabel(kPresetA, Effect::Applied) == "Preset A");
    CHECK(InUseLabel(kPresetB, Effect::Unknown) == "Preset B (not confirmed by the runtime yet)");
    CHECK(InUseLabel(kPresetC, Effect::NotApplied) == "the runtime's own default (Preset C was not applied)");
    CHECK(InUseLabel(9, Effect::Applied) == "Auto"); // anything invalid reads as Auto
}

void NothingToSayWhenTheyAgree()
{
    for (int p : { kAuto, kPresetA, kPresetB, kPresetC })
        for (auto e : { Effect::Unknown, Effect::Applied, Effect::NotApplied })
            CHECK(DifferenceSentence(p, p, e, false, SwitchFail::None).empty());
}

void PendingChange()
{
    // chosen C, Auto in use, the switch has not run yet: said with both names and what happens next
    const auto s = DifferenceSentence(kPresetC, kAuto, Effect::Applied, false, SwitchFail::None);
    CHECK(s == "Chosen: Preset C, in use: Auto. The change is made when frame generation runs next.");
    // a preset in use that nobody confirmed is not called "in use" as a fact
    const auto u = DifferenceSentence(kAuto, kPresetB, Effect::Unknown, false, SwitchFail::None);
    CHECK(Has(u, "Chosen: Auto, in use: Preset B (not confirmed by the runtime yet).") && Has(u, "when frame generation runs next"));
    const auto n = DifferenceSentence(kAuto, kPresetC, Effect::NotApplied, false, SwitchFail::None);
    CHECK(Has(n, "in use: the runtime's own default (Preset C was not applied)."));
}

void BlockedChange()
{
    // a live change the runtime or the guards refused: the reason, what stays in use, what happens at the next start
    for (auto fail : { SwitchFail::CannotRestoreAuto, SwitchFail::Prepare, SwitchFail::Off, SwitchFail::Release, SwitchFail::Install })
    {
        const auto s = DifferenceSentence(kPresetA, kPresetC, Effect::Applied, true, fail);
        CHECK_MSG(Has(s, "Chosen: Preset A, in use: Preset C.") && Has(s, FailText(fail)), "fail %d: %s", static_cast<int>(fail), s.c_str());
        CHECK(Has(s, "Restart needed: ") && Has(s, "Preset C stays until the next start; then the value in the ini applies (Save Settings stores the chosen one)."));
    }
    // the wording the maintainer will meet: booted on C, back to Auto when the driver's own answer was never seen
    const auto s = DifferenceSentence(kAuto, kPresetC, Effect::Applied, true, SwitchFail::CannotRestoreAuto);
    CHECK(s ==
          "Chosen: Auto, in use: Preset C. Restart needed: the driver's own answer was not seen at start-up, so Auto cannot be restored while the "
          "game runs. Preset C stays until the next start; then the value in the ini applies (Save Settings stores the chosen one).");
    // M1: the tail follows what the runtime confirmed, never "<Preset> stays in use" for a preset that is unconfirmed or was not applied
    // (whole sentences for the three effects)
    CHECK(DifferenceSentence(kAuto, kPresetC, Effect::Unknown, true, SwitchFail::CannotRestoreAuto) ==
          "Chosen: Auto, in use: Preset C (not confirmed by the runtime yet). Restart needed: the driver's own answer was not seen at start-up, "
          "so Auto cannot be restored while the game runs. Preset C (not confirmed by the runtime yet) stays until the next start; then the "
          "value in the ini applies (Save Settings stores the chosen one).");
    CHECK(DifferenceSentence(kAuto, kPresetC, Effect::NotApplied, true, SwitchFail::CannotRestoreAuto) ==
          "Chosen: Auto, in use: the runtime's own default (Preset C was not applied). Restart needed: the driver's own answer was not seen at "
          "start-up, so Auto cannot be restored while the game runs. The runtime's own default (Preset C was not applied) stays until the next "
          "start; then the value in the ini applies (Save Settings stores the chosen one).");
    // no reason recorded, a preset chosen over Auto in use: the tail names Auto
    CHECK(DifferenceSentence(kPresetB, kAuto, Effect::Applied, true, SwitchFail::None) ==
          "Chosen: Preset B, in use: Auto. Restart needed. Auto stays until the next start; then the value in the ini applies (Save Settings stores the chosen one).");
    // restartNeeded without a recorded reason still says it
    CHECK(Has(DifferenceSentence(kPresetB, kAuto, Effect::Applied, true, SwitchFail::None), "Restart needed"));
}

void UnavailableControl()
{
    // Auto chosen: just the reason (nothing was asked for)
    CHECK(UnavailableSentence(kAuto, Unavailable::ReplacementRuntime) == UnavailableText(Unavailable::ReplacementRuntime));
    // a preset chosen but the control is off: it is NOT in use, and the reason follows
    for (auto why : { Unavailable::AmpereRuntime, Unavailable::ReplacementRuntime, Unavailable::OtherOutput })
        for (int p : { kPresetA, kPresetB, kPresetC })
        {
            const auto s = UnavailableSentence(p, why);
            CHECK(s.find(std::string("Chosen: ") + Name(p) + ", not in use: ") == 0 && Has(s, UnavailableText(why)));
            CHECK(!Has(s, "In use") && !Has(s, "in use: Preset"));
        }
    CHECK(UnavailableSentence(kPresetC, Unavailable::None).empty());
}

void InUseOnlyForConfirmedOrAuto()
{
    // across every combination: the words "in use: Preset" never appear for a preset the runtime did not confirm
    for (int chosen : { kAuto, kPresetA, kPresetB, kPresetC })
        for (int active : { kAuto, kPresetA, kPresetB, kPresetC })
            for (auto e : { Effect::Unknown, Effect::NotApplied })
                for (bool restart : { false, true })
                {
                    const auto s = DifferenceSentence(chosen, active, e, restart, SwitchFail::Prepare);
                    if (active == kAuto)
                        continue;
                    CHECK_MSG(!Has(s, "in use: Preset A.") && !Has(s, "in use: Preset B.") && !Has(s, "in use: Preset C."), "%s", s.c_str());
                    // M1: and the restart tail never calls it "in use" either
                    CHECK_MSG(!Has(s, "stays in use"), "%s", s.c_str());
                }
}
// M2: the warning when the log shows NVIDIA's own copy of the runtime. It says what the log shows and what may be the reason (hedged), not a fact
void NvidiaCopyWarning()
{
    CHECK(NvidiaCopySentence() ==
          "The log shows NVIDIA's own copy of the DLSS-G runtime loaded instead of this one (APP_NAME = nvapp_override). An NVIDIA profile or the "
          "NVIDIA app's DLSS override may be the reason. In the run where this was measured (one RTX 4090 setup) it gave no 3x/4x unlock and lower fps.");
    const auto s = NvidiaCopySentence();
    // hedged: "may", not "is"; it never claims a preset is in use and never speaks of a missing report
    CHECK(Has(s, "may be the reason") && !Has(s, "In use") && !Has(s, "in use") && !Has(s, "missing") && !Has(s, "not reported"));
}
} // namespace

int main()
{
    InUseLabels();
    NothingToSayWhenTheyAgree();
    PendingChange();
    BlockedChange();
    UnavailableControl();
    InUseOnlyForConfirmedOrAuto();
    NvidiaCopyWarning();
    if (g_failures)
    {
        std::printf("fg_preset_menu_text: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_menu_text: chosen vs in use sentences, reasons, in use only for confirmed presets passed");
    return 0;
}
