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
    ext::menu::Tooltip("Needs ReShade 6.8 or newer (add-on API 20) loaded in the game.\n\n"
                       "FFXIV keeps its scene buffers at the output size and draws into a window of them when it renders below that size (DLSS\n"
                       "Quality and lower), so the depth ReShade finds does not line up with the picture and effects that read it (MXAO and the\n"
                       "like) misfit. With this on, the depth the upscaler is given is copied to the size of ReShade's back buffer and handed to\n"
                       "ReShade's depth input while it renders its effects; the original depth input is put back afterwards.\n\n"
                       "It stands down only for NON-NEUTRAL values of RESHADE_DEPTH_INPUT_X_SCALE, _Y_SCALE, _X_OFFSET, _Y_OFFSET,\n"
                       "_X_PIXEL_OFFSET and _Y_PIXEL_OFFSET (neutral is 1 for the scales and 0 for the offsets): defined with the neutral value it\n"
                       "does not stand down.\n\n"
                       "The game's depth is reverse-Z: if ambient occlusion looks inverted with this on, set RESHADE_DEPTH_INPUT_IS_REVERSED=1\n"
                       "in your ReShade preprocessor definitions.\n\n"
                       "It applies while the game runs; switching it on takes effect within a few frames.");

    const std::string status = ext::reshade::depth::Status();
    ImGui::TextWrapped("%s", status.c_str());
}

const ext::menu::AutoRegister registration { { "reshade_depth", "FFXIV", "ReShade depth compatibility", Draw, "fg_dlssg", Visible } };
} // namespace
} // namespace ext::reshade::depth::menu
