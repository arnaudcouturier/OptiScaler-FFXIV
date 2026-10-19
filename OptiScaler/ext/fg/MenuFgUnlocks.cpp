#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/dlssg/AmpereMfgLoader.h"
#include "ext/fg/dlssg/MfgUnlock.h"

#include <menu/menu_common.h>

// Card `fg_unlocks` (tab "Frame Gen", after the runtime cards, then fg_auto_mfg; position: ext/core/MenuLayout.h): MFG unlocks
// for DLSS-G, D4 (RTX 20/30 frame-generation runtime) and D1 (built-in Ada
// unlock). Was inline at the top of RenderFrameGenerationSelection.

namespace
{
void DrawAmpereRuntime(ext::FgConfig& cfg)
{
    if (!ImGui::TreeNode("RTX 20/30 frame-generation runtime (experimental)"))
        return;

    bool enabled = cfg.ampereMfgUnlock.value_or_default();
    if (ImGui::Checkbox("Load RTX 20/30 frame-generation runtime (restart)", &enabled))
        cfg.ampereMfgUnlock = enabled;
    ext::menu::Tooltip("Save settings and restart. Supply the runtime DLL in OptiScaler/dlssg_sm86.\nOptiFG remains the frame producer. Do not enable the built-in Ada unlock with this runtime.");
    auto runtime = AmpereMfgLoader::LastStatus();
    if (ImGui::Button("Detect installed runtime"))
        runtime = AmpereMfgLoader::ProbeCandidate(true);
    ImGui::TextWrapped("Runtime: %s; %s", runtime.ModName.empty() ? "not detected" : runtime.ModName.c_str(),
                       runtime.DllLoaded ? "loaded" : "not loaded");
    if (!runtime.ErrorMessage.empty())
        ImGui::TextWrapped("%s", runtime.ErrorMessage.c_str());
    if (enabled && cfg.adaMfgUnlock.value_or_default())
        ImGui::TextWrapped("Disable the built-in Ada unlock before enabling this runtime.");
    int maxFrames = cfg.ampereMfgMaxFrames.value_or_default();
    if (ImGui::SliderInt("RTX 20/30 max generated frames (restart)", &maxFrames, 1, 5))
        cfg.ampereMfgMaxFrames = maxFrames;
    ImGui::BeginDisabled(!runtime.DllLoaded || !runtime.LiveControlSupported);
    static int mode = 0, factor = 2, target = 144;
    const char* modes[] = { "Follow OptiFG", "Dynamic", "Fixed factor" };
    ImGui::Combo("Runtime control", &mode, modes, 3);
    ImGui::SliderInt("Runtime factor", &factor, 2, runtime.Is3101Runtime ? 4 : 6);
    ImGui::SliderInt("Runtime target FPS", &target, 30, 360);
    static std::string result;
    if (ImGui::Button("Apply runtime controls"))
        result = AmpereMfgLoader::ApplyLiveControl(mode, target, factor)
                     ? "Runtime accepted the request"
                     : "Runtime rejected or does not support this request";
    if (!result.empty())
        ImGui::TextWrapped("%s", result.c_str());
    ImGui::EndDisabled();
    ImGui::TextWrapped("Live controls require the SilyNoMeta exports. Detection alone does not confirm FG support.");
    ImGui::TreePop();
}

void DrawFgUnlocks(ext::menu::Ctx& ctx)
{
    (void) ctx;
    auto& cfg = ext::Cfg().fg;

    ext::menu::SectionTitle("Frame generation unlocks");

    DrawAmpereRuntime(cfg);

    /// FG INPUTS
    bool adaUnlock = cfg.adaMfgUnlock.value_or_default();
    if (ImGui::Checkbox("Built-in RTX 40 MFG unlock (experimental; restart)", &adaUnlock))
        cfg.adaMfgUnlock = adaUnlock;
    ext::menu::Tooltip("Ada unlock with targeted temporal correction (dashdogy / Dreamt / mavismmg). Save Settings and restart to enable or remove it."
                       "\nRequires a supported DLSSG runtime and Streamline 2.7.1+ for multiplier overrides."
                       "\nDo not combine with another MFG unlocker. Does not add FG to an unsupported game."
                       "\nVerified on an RTX 4090 at 2x to 4x. 5x and 6x are experimental; one GPU hang was seen at 6x."
                       "\nRTX 20/30/50 are left unchanged.");
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
