#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"
#include "ext/reshade/ReShadeDepth.h"

#include <imgui/imgui.h>

#include <string>

// "ReShade depth compatibility" card (tab FFXIV, between the window mode and the FFXIV HDR10 cards; position: ext/core/MenuLayout.h):
// the [FFXIV] AutomaticReShadeDepth key as a live switch and the status line of the feature. The card is ALWAYS there on the FFXIV
// build match (it is not hidden while ReShade is absent or the option is off: a ReShade installed as dxgi.dll is only looked for while the
// option is on, so a card that waited for it would never show). The status line says "ReShade not detected" for that case. This file
// includes no ReShade header: it reads the facade (ext/reshade/ReShadeDepth.h) only.
namespace ext::reshade::depth::menu
{
namespace
{
// Same condition as the other FFXIV cards: the FFXIV build entry exists for this executable.
bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("ReShade depth compatibility");

    auto& cfg = ext::Cfg().ffxiv;

    bool on = cfg.automaticReShadeDepth.value_or_default();
    if (ImGui::Checkbox("Automatic scene depth", &on))
    {
        cfg.automaticReShadeDepth = on;
        ext::reshade::depth::SetEnabled(on);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ext::menu::Tooltip("Keeps ReShade effects (ambient occlusion etc.) lined up with the picture when the game renders below the output size: "
                       "realigns the depth and the game's normals/motion/material automatically; needs ReShade 6.8+; presets need no edits unless you scaled or offset the depth by hand (the status line says when it stands down)");

    const std::string status = ext::reshade::depth::Status();
    ImGui::TextWrapped("%s", status.c_str());
}

const ext::menu::AutoRegister registration { { "reshade_depth", "FFXIV", "ReShade depth compatibility", Draw, "fg_dlssg", Visible } };
} // namespace
} // namespace ext::reshade::depth::menu
