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

void Red(const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
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
                       "(tick \"MB Override All Textures\" above for the rest).\n\n"
                       "A tick of \"MB Scale Override\" in the card above is noticed while this card is on screen,\n"
                       "otherwise at the next quality change.");

    // Applied while dragging (the number is stored at once), published when the slider is let go: one log line per
    // adjustment instead of one per frame.
    float offset = settings.mipBiasAutoOffset.value_or_default();
    // AlwaysClamp: a value typed with Ctrl+click stays inside the range too; ClampOffset is the same limit the policy uses.
    if (ImGui::SliderFloat("Extra offset", &offset, -3.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
        settings.mipBiasAutoOffset = static_cast<float>(mipbias::ClampOffset(offset));
    if (ImGui::IsItemDeactivatedAfterEdit())
        mipbias::OnSettingChanged();

    ext::menu::Tooltip("Added to the automatic value. Negative makes textures sharper.");

    // "MB Scale Override" can be ticked in the card above at any time: give the override back (or take it again) now. This is the ONLY
    // place that looks between two creates (nothing else of the module runs every frame or every draw), so with this card not drawn
    // (only upstream's Mipmap Bias card pinned to the Custom Tab, another tab open) the tick waits for the next feature create.
    mipbias::OnScaleOverrideMaybeChanged();

    if (on)
    {
        const auto status = mipbias::CurrentStatus();
        if (status.blocked)
            Red(mipbias::BlockedLine(status.blocked));
        else
        {
            Dim(mipbias::StatusLine(status));
            Dim(mipbias::CreatedLine(ctx.state.lastMipBias, ctx.state.lastMipBiasMax));
        }
    }

    Dim(mipbias::CardNote());
}

const ext::menu::AutoRegister registration { { "mipmap_auto", "Textures", "Automatic Mipmap Bias", Draw, "mipmap",
                                                 Visible } };
} // namespace
} // namespace ext::upscaler::menu
