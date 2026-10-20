#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/FgInternal.h"

#include <menu/menu_common.h>

// The DLSS-G render preset section of the card `fg_unlocks` (tab "Frame Gen"): the choice of [DLSSG] RenderPreset. Drawn by
// DrawFgUnlocks (MenuFgUnlocks.cpp), so no card of its own and no entry in the layout table.

namespace ext::fg
{
void DrawRenderPresetSection()
{
    auto& cfg = ext::Cfg().fg;

    int preset = cfg.renderPreset.value_or_default();
    if (preset < 0 || preset > 3)
        preset = 0;
    if (ImGui::Combo("DLSS-G render preset", &preset, "Auto\0Preset A\0Preset B\0Preset C\0"))
        cfg.renderPreset = preset;
    ext::menu::Tooltip("Render preset of NVIDIA's frame generation. Auto leaves the choice to the driver and the runtime.\n"
                       "A, B and C are answered inside this process only; no NVIDIA profile is written.");
}
} // namespace ext::fg
