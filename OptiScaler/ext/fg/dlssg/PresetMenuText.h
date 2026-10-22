#pragma once
// The state sentences of the DLSS-G render preset selector (MenuFgPreset.cpp). Pure text, so tests/ext/fg/fg_preset_menu_text.cpp
// builds it standalone. The selector stays visible; when the choice and what the runtime was told differ, the menu says what is chosen,
// what is in use and why they differ. "Chosen" is the value of the selector: it is the ini value once Save Settings was pressed
// (the menu cannot see what is on disk). A preset is "in use" as a fact only when the runtime confirmed it (PresetLog.h).

#include "ext/fg/dlssg/PresetLog.h"
#include "ext/fg/dlssg/PresetSwitch.h"

#include <string>

namespace ext::fg::preset
{
// What is in use: Auto, a confirmed preset, or a preset the runtime has not confirmed (or did not apply) said as such.
inline std::string InUseLabel(int active, Effect effect)
{
    if (!ValidRequest(active) || active == kAuto)
        return "Auto";
    switch (effect)
    {
    case Effect::Applied:
        return Name(active);
    case Effect::NotApplied:
        return std::string("the runtime's own default (") + Name(active) + " was not applied)";
    default:
        return std::string(Name(active)) + " (not confirmed by the runtime yet)";
    }
}

// Empty when the choice is what the runtime was told. Otherwise: why not yet (a live change waits for frame generation to run) or
// why not at all while the game runs (restart needed, with the reason of the failed step).
inline std::string DifferenceSentence(int chosen, int active, Effect effect, bool restartNeeded, SwitchFail fail)
{
    if (chosen == active)
        return {};
    std::string out = std::string("Chosen: ") + Name(chosen) + ", in use: " + InUseLabel(active, effect) + ". ";
    if (restartNeeded)
    {
        if (fail != SwitchFail::None)
            out += std::string("Restart needed: ") + FailText(fail) + ". ";
        else
            out += "Restart needed. ";
        out += std::string(Name(active)) + " stays in use until the next start, then the saved choice applies.";
    }
    else
        out += "The change is made when frame generation runs next.";
    return out;
}

// The control is off (companion runtime, replacement runtime, other output): a chosen preset is not in use, and the reason follows.
inline std::string UnavailableSentence(int chosen, Unavailable why)
{
    if (why == Unavailable::None)
        return {};
    if (!ValidRequest(chosen) || chosen == kAuto)
        return UnavailableText(why);
    return std::string("Chosen: ") + Name(chosen) + ", not in use: " + UnavailableText(why);
}
} // namespace ext::fg::preset
