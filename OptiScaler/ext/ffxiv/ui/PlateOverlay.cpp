#include "pch.h"

#include "ext/core/MenuRegistry.h"
#include "ext/ffxiv/ui/PlatePackets.h"
#include "ext/hudless/PlateProbe.h"
#include "ext/hudless/PlateProbePackets.h"

#include <imgui/imgui.h>

#include <atomic>
#include <cstdio>

// Developer overlay of the draw probe ([HudLess] PlateOverlay, with PlateDiagnostics on): outlines the vertex box of every rule r0 candidate
// draw of the most recent LOGGED frame on the foreground draw list, and says on one line what the box was assumed to be. The boxes are in the
// game's own vertex units, which nobody has measured against pixels yet, so the line also carries a switch between three readings (pixels of
// the draw's viewport, 0..1 across the display, -1..1 across the display with y up); it is clickable while the menu is open. Nothing here
// touches the game: it reads the snapshot the draw probe published and draws ImGui lines.
namespace
{
namespace pf = ext::hudless::plate;
namespace probe = ext::ffxiv::ui::plate;

std::atomic<uint8_t> g_units { static_cast<uint8_t>(pf::OverlayUnits::Pixels) };
bool g_noSlot = false; // the overlay table had no room when the probe asked

pf::OverlayUnits Units()
{
    const uint8_t v = g_units.load(std::memory_order_relaxed);
    return v < pf::kOverlayUnitCount ? static_cast<pf::OverlayUnits>(v) : pf::OverlayUnits::Pixels;
}

// Drawn every menu frame; the frame is kept alive for it (also with the menu closed) only while the probe is on and has a frame to show.
bool WantsFrame() { return pf::OverlayShouldDraw(pf::OverlayWanted(), probe::LastLoggedFrame()); }

void DrawOverlay(ext::menu::Ctx&)
{
    if (!WantsFrame())
        return;
    probe::OverlayFrame frame;
    if (!probe::CopyOverlay(frame))
        return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 1.0f || display.y < 1.0f)
        return;

    const pf::OverlayUnits units = Units();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    float refW = 0.0f, refH = 0.0f; // the first box's reference size, for the line
    for (const probe::OverlayBox& b : frame.boxes)
    {
        // the raw units are read against the viewport the draw was made with (its render target when none was recorded)
        const bool hasViewport = b.vpWidth != 0 && b.vpHeight != 0;
        const float w = hasViewport ? float(b.vpWidth) : float(b.rtWidth);
        const float h = hasViewport ? float(b.vpHeight) : float(b.rtHeight);
        if (refW == 0.0f)
        {
            refW = w;
            refH = h;
        }
        const pf::ScreenRect r = pf::MapBox(units, b.box, display.x, display.y, w, h);
        if (r.ok)
            list->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(255, 70, 210, 235), 0.0f, 0, 2.0f);
    }

    const std::string line = pf::OverlayLine(units, display.x, display.y, refW, refH, frame.frame, frame.boxes.size());
    ImGui::SetNextWindowPos(ImVec2(12.0f, display.y - 12.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("PlateProbeOverlay", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove))
    {
        ImGui::TextUnformatted(line.c_str());
        int mode = static_cast<int>(units);
        ImGui::RadioButton("pixels", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("0..1", &mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("-1..1", &mode, 2);
        if (mode != static_cast<int>(units))
            g_units.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
    }
    ImGui::End();
}

// Registered during static initialisation like the other overlays (the menu reads the table without a lock afterwards). The table holds
// kMaxOverlays; when it is full the overlay is not registered and CheckOverlaySlot says so once, at run time, where the log is up.
struct Registration
{
    Registration()
    {
        size_t count = 0;
        ext::menu::detail::Overlays(count);
        if (!pf::OverlaySlotFree(count, ext::menu::kMaxOverlays))
        {
            g_noSlot = true;
            return;
        }
        ext::menu::RegisterOverlay({ DrawOverlay, WantsFrame });
    }
} registration;
} // namespace

namespace ext::ffxiv::ui::plate
{
void CheckOverlaySlot()
{
    static std::atomic<bool> done { false };
    if (done.exchange(true))
        return;
    size_t count = 0;
    const ext::menu::Overlay* overlays = ext::menu::detail::Overlays(count);
    bool registered = false;
    for (size_t i = 0; i < count; ++i)
        registered = registered || overlays[i].draw == &DrawOverlay;
    if (!registered)
        LOG_WARN("Plate probe: PlateOverlay is on but the menu has no free overlay slot ({} of {} in use{}); the overlay is not drawn", count,
                 ext::menu::kMaxOverlays, g_noSlot ? "" : ", the probe's registration was dropped");
}
} // namespace ext::ffxiv::ui::plate
