#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"
#include "ext/window/WindowMode.h"
#include "ext/window/WindowSeams.h"

#include <imgui/imgui.h>

#include <string>

// "Window mode (FFXIV)" card (tab Frame Gen, right after the DLSSG settings): the [Window] Mode key as a drop-down. The mode
// is published once, when the settings are loaded (the ReShade event is registered then), so a change here is stored for the
// NEXT start (saved in the ini with the other settings) and the card says which mode is active in this session.
namespace ext::window::menu
{
namespace
{
// Same condition as the other FFXIV cards: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Window mode (FFXIV)");

    auto& cfg = ext::Cfg().window;

    static constexpr const char* kNames[] = { "off", "probe", "on" };
    const Mode stored = cfg.GetMode();
    int index = 0;
    for (int i = 0; i < 3; ++i)
        if (ParseMode(kNames[i]) == stored)
            index = i;

    if (ImGui::Combo("Mode (applies after a restart)", &index, "off\0probe\0on\0"))
        cfg.mode = std::string(kNames[index]);

    ext::menu::Tooltip("off: nothing runs.\n"
                       "probe: only logs the window and resize calls (lines starting with \"Window:\").\n"
                       "on: probe, plus the border fix after leaving the borderless \"fullscreen\" and the hold-back of a\n"
                       "window-mode call while frame generation runs (needs ReShade). Only for the borderless\n"
                       "\"fullscreen\" that [XeFG] ForceBorderless gives.\n\n"
                       "Read once at the start of the game: a change is stored for the next start.");

    ImGui::TextDisabled("Active in this session: %s", ModeName(CurrentMode()));
    if (CurrentMode() != stored)
        ImGui::TextWrapped("The selection above takes effect after the game is restarted.");
}

const ext::menu::AutoRegister registration { { "window_mode", "FFXIV", "Window mode (FFXIV)", Draw, "fg_dlssg",
                                                 Visible } };
} // namespace
} // namespace ext::window::menu
