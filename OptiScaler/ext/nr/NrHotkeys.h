#pragma once

// Hotkey step rules for the NR model resolution and the enlargement mode (the Classic/Matched residual switch).
// Pure: the press handlers in the NR menu and the Enlargement combo read these, so the two can never drift apart.

#include "ext/nr/NrModelSize.h"

#include <cmath>
#include <cstdint>

namespace DlssNr::Hotkeys
{
// One press moves the uniform model resolution by this many whole percent of the frame.
constexpr long kStepPercent = 5;

// The model resolution after one press in the given direction: exactly one 5% increment from the 5% grid,
// clamped to the range the Model resolution slider offers (25% to 200%). Anything not finite behaves as 100%.
// The value is an integer percent, so the hotkey and the slider write bit-identical settings and repeated
// presses never drift; a value off the grid joins it towards the press.
inline float StepWorkingScale(float current, bool up)
{
    const long percent = std::lround(SanitizeWorkScale(current) * 100.0f);
    // Integer division truncates towards zero, and every value here is positive, so this is the floor (up)
    // or the ceiling (down) on the 5% grid; the comparison against the rounded value moves an on-grid value
    // one step further instead of sticking to it.
    long stepped = up ? (percent / kStepPercent) * kStepPercent : ((percent + kStepPercent - 1) / kStepPercent) * kStepPercent;
    if (up ? stepped <= percent : stepped >= percent)
        stepped += up ? kStepPercent : -kStepPercent;
    if (stepped < 25)
        stepped = 25;
    if (stepped > 200)
        stepped = 200;
    return (float) stepped / 100.0f;
}

// Whether the Enlargement combo is interactive for these effective per-axis scales: the same rule the menu uses to
// grey it out. The toggle is a no-op while this is false.
inline bool TransferToggleable(float scaleX, float scaleY)
{
    const bool aboveFull = scaleX > 1.0f || scaleY > 1.0f;
    return !aboveFull && (scaleX < 0.999f || scaleY < 0.999f);
}

// The enlargement mode after a toggle press: Classic (0) and Matched residual (1) swap; anything else is shown
// as Classic by the menu, so it becomes Matched residual like Classic does. A greyed-out combo leaves the value alone.
inline uint32_t ToggleTransfer(uint32_t current, bool toggleable)
{
    if (!toggleable)
        return current;
    return current == 1 ? 0 : 1;
}
} // namespace DlssNr::Hotkeys
