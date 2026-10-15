#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/FgInternal.h"

#include <menu/menu_common.h>

// In-card extras of the Frame Gen cards (SEAMS 3.6): they sit between upstream controls, so they cannot be cards.

namespace ext::menu
{
void ObservedMultiplier(IFGFeature* dlssg)
{
    const auto observedMultiplier = ext::fg::ObservedMultiplier(dlssg);
    if (observedMultiplier)
        ImGui::Text("Current DMFG: %uX (observed)", observedMultiplier);
    else
        ImGui::TextDisabled("Current DMFG: waiting for presentation data");
    Tooltip("Latest observed output frames per real frame, including the real frame. "
            "May differ from the driver's next selection during transitions. "
            "This reads cached telemetry and does not change factor selection.");
}
} // namespace ext::menu
