#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/dlssg/AmpereMfgLoader.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/dlssg/MfgUnlock.h"

#include <menu/menu_common.h>

// Card `fg_unlocks` (tab "Frame Gen", after the runtime cards, then fg_auto_mfg; position: ext/core/MenuLayout.h): MFG unlocks
// for DLSS-G, D4 (RTX 20/30 frame-generation runtime) and D1 (built-in Ada
// unlock). Was inline at the top of RenderFrameGenerationSelection.

namespace
{
// Position of the stored text in the list of choices, case-insensitive (only the Preset combo uses it: the settings writer takes A/B
// in either case); anything unknown reads as the first entry (Auto), the same way the runtime's settings writer treats it.
// The Kernel image and Kernel family combos compare exactly like the writer: AmpereMfgLoader::KernelImageChoice / RouterChoice.
int IndexOfValue(const std::string& value, const char* const* values, int count)
{
    for (int i = 0; i < count; ++i)
        if (_stricmp(value.c_str(), values[i]) == 0)
            return i;
    return 0;
}

// The settings writer accepts exactly "1"/"true" (on) and "0"/"false" (off) and leaves the key out for anything else.
int SpoofArchIndex(const std::string& value)
{
    if (value == "1" || value == "true")
        return 1;
    if (value == "0" || value == "false")
        return 2;
    return 0;
}

// The tuning keys of the runtime. They are written to the runtime's own settings file (dlssg_sm86.ini next to its DLL, see
// AmpereMfgLoader.cpp GenerateIniContent) every time the runtime is loaded, so each change applies after a restart.
void DrawAmpereAdvanced(ext::FgConfig& cfg)
{
    if (!ImGui::TreeNode("Advanced (RTX 20/30 runtime)##ampere_advanced"))
        return;

    ImGui::TextWrapped("Written to the runtime's own settings file (dlssg_sm86.ini next to its DLL) each time it is loaded: every "
                       "change here applies after a restart. Leave everything on Auto unless you are tuning the runtime.");

    static const char* const kKernelImages[] = { "Auto", "PTX", "Cubin" };
    int kernelImage = AmpereMfgLoader::KernelImageChoice(cfg.ampereMfgKernelImage.value_or_default());
    if (ImGui::Combo("Kernel image (restart)", &kernelImage, "Auto\0PTX\0Cubin\0"))
        cfg.ampereMfgKernelImage = std::string(kKernelImages[kernelImage]);
    ext::menu::Tooltip("Interpolation kernel image of the runtime. Auto picks one for your GPU (and for Linux/Proton); PTX or Cubin force it.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    static const char* const kRouters[] = { "Auto", "SM75", "SM86" };
    int router = AmpereMfgLoader::RouterChoice(cfg.ampereMfgRouter.value_or_default());
    if (ImGui::Combo("Kernel family (restart)", &router, "Auto\0SM75\0SM86\0"))
        cfg.ampereMfgRouter = std::string(kRouters[router]);
    ext::menu::Tooltip("Kernel family the runtime routes to. Auto picks SM75 on RTX 20 (when the runtime has it) and SM86 on RTX 30. "
                       "SM75 falls back to Auto when the loaded runtime has no SM75 kernels.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    bool hardwareBilinear = cfg.ampereMfgHardwareBilinear.value_or_default();
    if (ImGui::Checkbox("Hardware bilinear sampling (restart)", &hardwareBilinear))
        cfg.ampereMfgHardwareBilinear = hardwareBilinear;
    ext::menu::Tooltip("Use hardware bilinear sampling in the runtime's kernels. Off by default.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    int optimized = cfg.ampereMfgOptimized.value_or_default();
    if (ImGui::SliderInt("Optimization level (restart)", &optimized, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp))
        cfg.ampereMfgOptimized = optimized;
    ext::menu::Tooltip("Optimization level the runtime is configured with, 0 to 3 (default 1).\n"
                       "Written to the runtime's settings file: applies after a restart.");

    static const char* const kPresets[] = { "Auto", "A", "B" };
    int preset = IndexOfValue(cfg.ampereMfgPreset.value_or_default(), kPresets, 3);
    if (ImGui::Combo("Preset (restart)", &preset, "Auto\0A\0B\0"))
        cfg.ampereMfgPreset = std::string(kPresets[preset]);
    ext::menu::Tooltip("Compatibility preset of the runtime: Auto, A or B.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    static const char* const kSpoofArch[] = { "Auto", "true", "false" };
    int spoofArch = SpoofArchIndex(cfg.ampereMfgSpoofArchToGame.value_or_default());
    if (ImGui::Combo("Report GPU architecture to the game (restart)", &spoofArch, "Auto\0On\0Off\0"))
        cfg.ampereMfgSpoofArchToGame = std::string(kSpoofArch[spoofArch]);
    ext::menu::Tooltip("Whether the runtime tells the game it runs on the GPU architecture the runtime was built for. Auto writes nothing "
                       "(the runtime decides); On and Off write the choice.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    int logLevel = cfg.ampereMfgLogLevel.value_or_default();
    if (ImGui::SliderInt("Runtime log level (restart)", &logLevel, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp))
        cfg.ampereMfgLogLevel = logLevel;
    ext::menu::Tooltip("Log level of the runtime, 0 to 3 (default 1); a diagnostic setting, its logs go to the dlssg_sm86 logs folder.\n"
                       "Written to the runtime's settings file: applies after a restart.");

    ImGui::TreePop();
}

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
    if (enabled)
        DrawAmpereAdvanced(cfg);
    ImGui::TreePop();
}

void DrawFgUnlocks(ext::menu::Ctx& ctx)
{
    (void) ctx;
    auto& cfg = ext::Cfg().fg;

    ext::menu::SectionTitle("Frame generation unlocks");

    DrawAmpereRuntime(cfg);

    ext::fg::DrawRenderPresetSection(); // WS2

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
