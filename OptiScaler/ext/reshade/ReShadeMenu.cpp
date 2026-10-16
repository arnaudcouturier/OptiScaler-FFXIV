#include "pch.h"

#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeEvents.h"

#include <imgui/imgui.h>

#include <atomic>
#include <format>

// Menu card of the ReShade built-in add-on probe (S10 package D0). Plain status structs only: no ReShade types here
// (ReShade's headers and ImGui must not share a translation unit).
namespace
{
using ext::reshade::probe::Session;

const char* StateText(Session::ProbeState s)
{
    switch (s)
    {
    case Session::ProbeState::Idle:
        return "idle";
    case Session::ProbeState::Armed:
        return "ARMED: waiting for the HUD boundary call";
    case Session::ProbeState::Running:
        return "running";
    case Session::ProbeState::AwaitFrameEnd:
        return "applied, waiting for the end of the frame";
    }
    return "?";
}

void DrawReShadeCard(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("ReShade built-in add-on (diagnostics)");

    const auto addon = ext::reshade::Status();
    if (addon.registered)
        ImGui::Text("Registration: registered with ReShade (add-on API %u)", addon.apiVersion);
    else
        ImGui::TextWrapped("Registration: inert (%s)", ext::reshade::Describe(addon.reason));

    ext::reshade::probe::Session* session = ext::reshade::ProbeSession();
    if (session == nullptr)
    {
        ImGui::TextWrapped("Probe: not subscribed (needs [HudLess] ReShadeProbe = true and a successful registration).");
        return;
    }

    ext::reshade::EnsureReportStarted();

    ext::reshade::probe::FileReport* report = ext::reshade::ProbeReport();
    const std::string folder = report != nullptr ? report->Folder().string() : std::string();
    const auto status = session->Status(folder, report != nullptr && report->Failed());

    ImGui::Text("Effect runtimes alive: %u    ReShade frames seen: %llu", status.aliveRuntimes,
                static_cast<unsigned long long>(status.framesSeen));
    ImGui::TextWrapped("Events: %s", ext::reshade::probe::FormatEventCounts(status.counts).c_str());
    if (addon.registered && status.counts.Total() == 0)
    {
        // Once: the report must say so even though no ReShade callback ever started it (registered, but nothing arrives).
        static std::atomic<bool> warned { false };
        if (!warned.exchange(true))
        {
            session->Note("[F1] WARNING: registered with ReShade but NO event has been delivered (seen from the menu card). "
                          "Either the registration made in DllMain was too early for this ReShade (it searches for add-ons later, "
                          "at the first device creation) or this build restricts the events. This report existing with no "
                          "event lines IS the answer for F1-F5: nothing can be probed. Next registration point to try: the "
                          "existing ext::game::OnD3D11DeviceHooked seam.");
            LOG_WARN("ReShade built-in add-on: registered but ReShade has delivered no event");
        }
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                           "Registered, but ReShade has delivered no event yet. If this stays so while the game renders, the "
                           "registration was too early or this ReShade build restricts events (see reshade-probe.txt).");
    }
    if (status.reportFailed)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "The report file could not be created.");
    else if (!status.reportFolder.empty())
        ImGui::TextWrapped("Report: %s\\reshade-probe.txt", status.reportFolder.c_str());
    else
        ImGui::TextWrapped("Report: created at the first event, below <game>\\OptiScaler\\hudless-diag\\.");

    const auto probe = session->ProbeStatusCopy();
    ImGui::Separator();
    ImGui::Text("Probe: %s", StateText(probe.state));
    if (!probe.lastResult.empty())
        ImGui::TextWrapped("Last result: %s", probe.lastResult.c_str());
    if (probe.inert)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Latched inert after an exception: restart the game to try again.");

    static bool alsoDisable = false;
    ImGui::Checkbox("Also disable ReShade effects for the rest of that frame", &alsoDisable);
    ext::menu::Tooltip("Skip method 2. Leave it OFF for the first probe: render_effects documents that it already prevents "
                       "ReShade's usual end-of-frame pass. Turn it ON only if the report says the end-of-frame pass still ran. "
                       "The effects state is restored right after the frame and the restore is verified in the report.");

    ImGui::BeginDisabled(!addon.registered || probe.state != Session::ProbeState::Idle || probe.inert);
    if (ImGui::Button("ReShade probe: apply effects now"))
    {
        std::string why;
        if (!session->Arm(alsoDisable ? ext::reshade::probe::SkipMethod::AlsoDisableEffects
                                      : ext::reshade::probe::SkipMethod::RenderEffectsOnly,
                          &why))
            session->Note("[F2] arm refused: " + why);
    }
    ImGui::EndDisabled();
    ext::menu::Tooltip("Arms ONE frame. At the next HUD boundary OptiScaler asks ReShade to apply the enabled effects in place on "
                       "the scene render target, with all D3D11 state saved and restored, then logs what happened. Never "
                       "automatic. Expect the picture to be briefly processed twice or not at all; report anything odd. "
                       "Needs the HUD boundary detector to call ProbeAtBoundary (otherwise the arm times out after 240 frames).");
    if (probe.state == Session::ProbeState::Armed)
    {
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            session->Disarm();
    }
}

bool ReShadeCardVisible(const ext::menu::Ctx&) { return ext::reshade::ProbeEnabled(); }

const ext::menu::AutoRegister reshadeCard {
    { "reshade_addon_probe", "Compatibility", "ReShade built-in add-on (diagnostics)", DrawReShadeCard, nullptr, ReShadeCardVisible }
};
} // namespace
