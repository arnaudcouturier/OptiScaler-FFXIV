#pragma once
// What the DLSS-G preset control shows in the menu (written on the FG dispatch thread by PresetSeams.cpp, read by the UI thread).

#include "ext/fg/dlssg/PresetSwitch.h"

#include <cstdint>
#include <string>

namespace ext::fg::preset
{
struct Status
{
    int configured = kAuto;        // [DLSSG] RenderPreset as the user set it
    int active = kAuto;            // what the runtime was last told
    int boot = kAuto;              // what it was told at start-up
    bool restartNeeded = false;    // the configured request could not be applied while the game runs
    SwitchFail fail = SwitchFail::None;
    Unavailable unavailable = Unavailable::None;
    std::string moduleText;        // PresetModule.h StatusText()
    uint32_t switches = 0;         // completed live switches
};

Status GetStatus();
} // namespace ext::fg::preset
