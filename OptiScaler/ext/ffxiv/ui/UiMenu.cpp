#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/ui/UiConfig.h"
#include "ext/ffxiv/ui/UiDiag.h"
#include "ext/ffxiv/ui/UiSnapshot.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// S10 package A: menu card "HUD-less diagnostics" of the D1 build (docs/HUDLESS.md section 12). Visible only while
// [HudLess] Diagnostics is true on a matched FFXIV build.
namespace
{
using namespace ext::ffxiv::ui;

void DrawDiagCard(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("HUD-less diagnostics (D1, observation only)");
    ImGui::TextWrapped("Nothing here changes a draw, a copy or a Present. Send the run folder back after the session.");

    const diag::Status st = diag::GetStatus();
    if (!st.running)
    {
        ImGui::TextWrapped("Waiting for the first presented frame...");
        return;
    }

    ImGui::TextWrapped("Run folder: %s", st.runDir.c_str());
    ImGui::Separator();
    ImGui::Text("Frames observed: %llu", (unsigned long long) st.frames);
    ImGui::Text("Draws per frame: %.0f   hook cost: %.1f us/frame (%.0f ns/draw)", st.drawsPerFrame, st.hookUsPerFrame,
                st.hookNsPerDraw);
    ImGui::Text("Pixel shaders: %llu created after the hook, %u only seen at draws (created before the hook)",
                (unsigned long long) st.shadersAfterHook, st.preHookShaders);
    ImGui::Text("Boundary: R1 in %llu frames, R2 in %llu, same draw in %llu; scene frames with no boundary %llu of %llu",
                (unsigned long long) st.framesR1, (unsigned long long) st.framesR2, (unsigned long long) st.framesSame,
                (unsigned long long) st.framesNone, (unsigned long long) st.framesWithScene);
    if (st.droppedJobs)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Writer queue dropped %llu jobs", (unsigned long long) st.droppedJobs);

    ImGui::Separator();
    if (ImGui::Button("Save UI diagnostics snapshot"))
        snapshot::Request();
    ImGui::SameLine();
    ImGui::Text("%s (%u of %u presses, %u images)", st.snapshotState.c_str(), st.snapshotPresses, snapshot::kMaxPresses,
                st.snapshotImages);
    ImGui::TextWrapped("Press once in a scene with the UI shown; toggle the game's hide-UI key; press again in the same spot. "
                       "Each press saves the boundary capture(s) and the backbuffer of the same frame.");
}

bool DiagVisible(const ext::menu::Ctx&) { return ext::game::Current().hudless != nullptr && DiagnosticsOn(); }

const ext::menu::AutoRegister registration { { "hudless_diag", "Frame Gen", "HUD-less diagnostics", DrawDiagCard, nullptr,
                                               DiagVisible } };
} // namespace
