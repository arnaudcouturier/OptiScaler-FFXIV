#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/upscaler/MipBias.h"

#include <State.h>

#include <imgui/imgui.h>

#include <string>

// "Automatic Mipmap Bias" card (tab Textures, right after upstream's "Mipmap Bias" card). The decisions and the texts
// are in ext/upscaler/MipBias.h (tested); this file only draws them and stores the two settings.
namespace ext::upscaler::menu
{
namespace
{
// Same condition as upstream's Mipmap Bias card: the sampler hooks it relies on exist for DX11 and DX12 only.
bool Visible(const ext::menu::Ctx& ctx) { return ctx.state.swapchainApi != Vulkan; }

void Dim(const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void Draw(ext::menu::Ctx& ctx)
{
    ext::menu::SectionTitle("Automatic Mipmap Bias");

    auto& settings = ext::Cfg().upscaler;

    bool on = settings.mipBiasAuto.value_or_default();
    if (ImGui::Checkbox("Follow the upscaling ratio", &on))
    {
        settings.mipBiasAuto = on;
        mipbias::OnSettingChanged();
    }

    ext::menu::Tooltip("Sets the texture sharpening from the upscaling ratio,\n"
                       "log2(render width / display width). This is the number\n"
                       "the calculator above gives. It replaces the manual value\n"
                       "while it is on.\n\n"
                       "Like the manual value it only reaches textures created\n"
                       "after a change, and only those the game already biases\n"
                       "(tick \"MB Override All Textures\" above for the rest).");

    // Applied while dragging (the number is stored at once), published when the slider is let go: one log line per
    // adjustment instead of one per frame.
    float offset = settings.mipBiasAutoOffset.value_or_default();
    if (ImGui::SliderFloat("Extra offset", &offset, -3.0f, 3.0f, "%.2f"))
        settings.mipBiasAutoOffset = offset;
    if (ImGui::IsItemDeactivatedAfterEdit())
        mipbias::OnSettingChanged();

    ext::menu::Tooltip("Added to the automatic value. Negative makes textures sharper.");

    if (on)
    {
        Dim(mipbias::StatusLine(mipbias::CurrentStatus()));
        Dim(mipbias::CreatedLine(ctx.state.lastMipBias, ctx.state.lastMipBiasMax));
    }

    Dim("While this is on, \"Set\" and \"Reset\" in the card above are overwritten at the next quality change.");
}

const ext::menu::AutoRegister registration { { "mipmap_auto", "Textures", "Automatic Mipmap Bias", Draw, "mipmap",
                                                 Visible } };
} // namespace
} // namespace ext::upscaler::menu
