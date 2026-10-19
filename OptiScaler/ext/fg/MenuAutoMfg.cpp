#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/DynamicMfgGate.h"
#include "ext/fg/FgInternal.h"

#include <State.h>
#include <misc/IdentifyGpu.h>

#include <menu/menu_common.h>

// Card `fg_auto_mfg` (tab "Frame Gen", after the DLSSG card): the automatic 2X-4X multiplier of DynamicMfgSeams.cpp.
// Hidden when the DLSS-G output is not the active one and on RTX 50 (NVIDIA's own dynamic mode is the tool there).

namespace
{
bool IsBlackwell()
{
    static const bool blackwell = []
    {
        const auto gpu = IdentifyGpu::getPrimaryGpu();
        return gpu.vendorId == VendorId::Nvidia &&
               ext::fg::IsBlackwellArchitecture(static_cast<uint32_t>(gpu.nvidiaArchInfo.architecture_id));
    }();
    return blackwell;
}

bool AutoMfgVisible(const ext::menu::Ctx& ctx)
{
    return ext::fg::AutoMfgCardVisible(ctx.state.activeFgOutput == FGOutput::DLSSG, IsBlackwell());
}

void DrawAutoMfg(ext::menu::Ctx& ctx)
{
    (void) ctx;
    auto& cfg = ext::Cfg().fg;

    bool enabled = cfg.autoMfg.value_or_default();
    if (ImGui::Checkbox("Automatic multiplier (2X-4X, experimental)", &enabled))
        cfg.autoMfg = enabled;
    ext::menu::Tooltip("Picks 2X-4X from the real frame time and changes it while frame generation keeps running,\n"
                       "without the off/on cycle of the Frame Gen slider. Never above 4X; the multiplier it picks is not\n"
                       "saved with Save Settings.\n"
                       "A change can still cost one long frame (about 0.2 s) if DLSS-G re-creates itself on this setup:\n"
                       "the mode then waits 12 s between changes and stops after a second one.\n"
                       "Needs the built-in RTX 40 MFG unlock for 3X and 4X. Untested in game.\n"
                       "Turning it off keeps the multiplier currently in use until you move the Frame Gen slider.");

    ImGui::BeginDisabled(!enabled);
    int highest = std::clamp(cfg.autoMfgMax.value_or_default(), 2, 4);
    if (ImGui::SliderInt("Highest multiplier##AutoMfg", &highest, 2, 4, "%dX"))
        cfg.autoMfgMax = highest;

    float target = cfg.autoMfgTarget.value_or_default();
    if (ImGui::SliderFloat("Target FPS##AutoMfg", &target, 0.0f, 500.0f, target > 0.0f ? "%.0f" : "Auto"))
        cfg.autoMfgTarget = target;
    ext::menu::Tooltip("Frame rate to reach. Auto is the lower of the frame rate limit and the display refresh rate.");

    const auto status = ext::fg::AutoMfgGetStatus();
    if (enabled && status.gate == ext::fg::AutoMfgGate::Open)
    {
        ImGui::TextWrapped("Current: %dX (selected), target %.0f fps (%s, %s)", status.factor, status.target,
                           status.refresh > 0.0 ? std::format("display {:.0f} Hz", status.refresh).c_str()
                                                : "display unknown",
                           status.limit > 0.0 ? std::format("limit {:.0f}", status.limit).c_str() : "no limit");
        if (status.selfTestRunning)
            ImGui::TextWrapped("Self-test running: the multiplier changes every few seconds, the result goes to the log.");
    }
    else if (enabled && status.gate != ext::fg::AutoMfgGate::KeyOff)
        ImGui::TextWrapped("Not active: %s", ext::fg::GateText(status.gate));
    else if (enabled)
        ImGui::TextWrapped("Waiting for the first frame with frame generation running.");
    ImGui::EndDisabled();
}

ext::menu::AutoRegister reg { { "fg_auto_mfg", "Frame Gen", "Automatic multi frame generation", DrawAutoMfg, "fg_dlssg",
                                AutoMfgVisible } };
} // namespace
