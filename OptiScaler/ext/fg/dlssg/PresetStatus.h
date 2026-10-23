#pragma once
// What the DLSS-G preset control shows in the menu (written on the FG dispatch thread by PresetSeams.cpp, read by the UI thread).

#include "ext/fg/dlssg/PresetSwitch.h"
#include "ext/fg/dlssg/UiRecompositionRule.h"

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
    int failedTarget = -1;         // the request whose switch failed (restartNeeded and fail describe that one), -1 = none
    Unavailable unavailable = Unavailable::None;
    std::string moduleText;        // PresetModule.h StatusText()
    uint32_t switches = 0;         // completed live switches
};

// The restart state is published by the dispatch tick for the target that failed, while `configured` is read live from the selector.
// When the user has picked another preset since (frame generation not dispatching, so nothing republished), the restart reason belongs to
// the old target and must not be shown against the new choice.
inline void DropStaleRestart(Status& status)
{
    if (status.restartNeeded && status.failedTarget != status.configured)
    {
        status.restartNeeded = false;
        status.fail = SwitchFail::None;
    }
}

Status GetStatus();

// Why the control is off right now (None = on): the companion runtime is configured, a replacement runtime is active or the output is
// not DLSS-G. Used by the dispatch tick, the menu and the driver-settings seam. Safe from any thread.
Unavailable CurrentAvailability();

// The recomposition rule (UiRecompositionRule.h) for the active request of the process. `atCreation` is passed by the DLSS-G option seam
// (every SetOptions): the first such call remembers the availability of the control, so a later change of it (the companion-runtime
// checkbox is written at once although it says restart) cannot flip the flag of a running feature. Other callers (the HUD-less engine, the
// menu) read the remembered value, or the live one while nothing was remembered. Safe from any thread.
UiRecompositionDecision CurrentUiRecompositionDecision(bool modeWantsIt, bool atCreation = false);

// The sentence the menu shows next to the HUD-less mode and next to the preset selector (PresetMenuText.h UiRecompositionNote), empty when
// there is nothing to say; and the readout line from the runtime's own reply (readout only).
std::string UiRecompositionMenuNote();
std::string UiRecompositionReadoutText();
} // namespace ext::fg::preset
