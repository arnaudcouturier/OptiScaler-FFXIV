#pragma once
// The state sentences of the DLSS-G render preset selector (MenuFgPreset.cpp). Pure text, so tests/ext/fg/fg_preset_menu_text.cpp
// builds it standalone. The selector stays visible; when the choice and what the runtime was told differ, the menu says what is chosen,
// what is in use and why they differ. "Chosen" is the value of the selector: it is the ini value once Save Settings was pressed
// (the menu cannot see what is on disk). A preset is "in use" as a fact only when the runtime confirmed it (PresetLog.h).

#include "ext/fg/dlssg/PresetLog.h"
#include "ext/fg/dlssg/PresetSwitch.h"

#include <cctype>
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
        // The tail says what stays, with the same label as the head: a preset the runtime did not confirm is never "in use" here, and
        // "Chosen" is the selector (unsaved until Save Settings), so what applies at the next start is the value in the ini.
        std::string stays = InUseLabel(active, effect);
        if (!stays.empty())
            stays[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stays[0]))); // starts the sentence
        out += stays + " stays until the next start; then the value in the ini applies (Save Settings stores the chosen one).";
    }
    else
        out += "The change is made when frame generation runs next.";
    return out;
}

// The log shows NVIDIA's own copy of the runtime (NGX init line "Built with APP_NAME = nvapp_override"). Says what the log shows and what may
// be the reason; the reason itself is inferred (no log has a real user profile value). The one measured case: the run in which this tool
// answered the DLSS override id on (RTX 4090, max generated frames 1, about 47 fps against about 74).
inline std::string NvidiaCopySentence()
{
    return "The log shows NVIDIA's own copy of the DLSS-G runtime loaded instead of this one (APP_NAME = nvapp_override). An NVIDIA profile or "
           "the NVIDIA app's DLSS override may be the reason. In the run where this was measured (one RTX 4090 setup) it gave no 3x/4x unlock "
           "and lower fps.";
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
