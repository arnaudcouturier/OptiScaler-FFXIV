#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/dlssg/MfgUnlock.h"

#include <menu/menu_common.h>

// Card `fg_unlocks` (tab "Frame Gen"): MFG unlocks for DLSS-G (D1 Ada built-in unlock; D4 adds the RTX 20/30 companion
// runtime). Was inline at the top of RenderFrameGenerationSelection.

namespace
{
void DrawFgUnlocks(ext::menu::Ctx& ctx)
{
    (void) ctx;
    auto& cfg = ext::Cfg().fg;

    /// FG INPUTS
    bool adaUnlock = cfg.adaMfgUnlock.value_or_default();
    if (ImGui::Checkbox("Built-in RTX 40 MFG unlock (experimental; restart)", &adaUnlock))
        cfg.adaMfgUnlock = adaUnlock;
    ext::menu::Tooltip("Ada unlock with targeted temporal correction (dashdogy / Dreamt / mavismmg). Save Settings and restart to enable or remove it."
                       "\nRequires a supported DLSSG runtime and Streamline 2.7.1+ for multiplier overrides."
                       "\nDo not combine with another MFG unlocker. Does not add FG to an unsupported game."
                       "\nNot validated on RTX 40 hardware here; RTX 20/30/50 are left unchanged.");
    if (adaUnlock)
    {
        const auto status = MfgUnlock::LastStatus();
        ImGui::TextWrapped("DLSSG %s: capability %s, validation %s, corrected temporal kernels %u",
                           status.SnippetVersion.empty() ? "not patched" : status.SnippetVersion.c_str(),
                           status.AdvertiseMatched ? "matched" : "not matched",
                           status.ValidateMatched ? "matched" : "not matched", status.KernelsRewritten);
    }
}

ext::menu::AutoRegister reg { { "fg_unlocks", "Frame Gen", "Frame generation unlocks", DrawFgUnlocks,
                                "fg_selection" } };
} // namespace
