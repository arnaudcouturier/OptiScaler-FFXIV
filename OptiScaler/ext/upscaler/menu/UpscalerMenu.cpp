#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"
#include "ext/upscaler/UpscalerSeams.h"
#include "upscalers/IFeature.h"

#include <imgui/imgui.h>

// "Forced quality level" card (id forced_quality, tab Upscaling, right after the Upscale Ratio Override card; position:
// ext/core/MenuLayout.h; docs/SEAMS-catalog.md "menu").
// Mirrors the oracle's menu/menu_common.cpp RenderUpscaleRatioSettings "Quality Preset" section (menu_common.cpp
// ~6701-6795 there), promoted to its own MenuRegistry card per the catalog's design rather than inlined into the
// Upscale Ratio Override card.
namespace ext::upscaler::menu
{
namespace
{
// Values are NVSDK_NGX_PerfQuality_Value, written out rather than including the NGX header for six integers,
// ordered best-first for the reader rather than in enum order (same table as the oracle's).
constexpr const char* kNames[] = { "Game's choice", "Native / DLAA", "Ultra Quality", "Quality", "Balanced",
                                     "Performance" };
constexpr int kValues[] = { -1, 5, 4, 2, 1, 0 };
constexpr const char* kRatios[] = { "", "1.0", "1.3", "1.5", "1.7", "2.0" };

int IndexOf(int value)
{
    for (int i = 0; i < IM_ARRAYSIZE(kValues); ++i)
        if (kValues[i] == value)
            return i;
    return 0;
}

// Same gate the oracle's "upscale_ratio"/"sharpness" cards use (menu_common.cpp IsUpscalerActive), reimplemented
// from Ctx since that upstream helper is not exposed.
bool Visible(const ext::menu::Ctx& ctx) { return ctx.currentFeature != nullptr && !ctx.currentFeature->IsFrozen(); }

void Draw(ext::menu::Ctx& ctx)
{
    ext::menu::SectionTitle("Forced quality level");

    const int currentForced = ext::Cfg().upscaler.forcePerfQuality.value_or_default();
    int index = IndexOf(currentForced);

    auto* quality = ext::game::Current().quality;
    const bool live = quality != nullptr && quality->Live();

    if (ImGui::Combo("Preset", &index, kNames, IM_ARRAYSIZE(kNames)))
    {
        ext::Cfg().upscaler.forcePerfQuality = kValues[index];
        if (live)
            quality->Request(kValues[index]);
    }

    if (live)
        ext::menu::Tooltip("Changes FFXIV's scene resolution through its native update path.\n"
                            "Presets apply automatically while native DLSS is active.\n"
                            "Game's choice restores the game's dynamic resolution range.");
    else
        ext::menu::Tooltip("Overrides the quality returned when the game asks for optimal settings.\n"
                            "Some games require a resolution change or restart to apply it.");

    if (index != 0)
    {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("(%sx)", kRatios[index]);
    }

    // What is actually in force, as opposed to what is selected -- see ext/upscaler/UpscalerSeams.cpp
    // ApplyForcedQuality for why a game that already asked and allocated cannot be moved after the fact.
    const auto answered = ext::upscaler::LastQualityAnswer();

    if (answered.queries == 0)
    {
        ImGui::TextDisabled("The game has not asked for a resolution yet.");
    }
    else
    {
        const int answeredIndex = IndexOf(answered.quality);
        ImGui::TextDisabled("Last sizing response: %s, %ux%u -> %ux%u (%llu quer%s)", kNames[answeredIndex],
                             answered.renderWidth, answered.renderHeight, answered.displayWidth,
                             answered.displayHeight, answered.queries, answered.queries == 1 ? "y" : "ies");
    }

    const bool selectionAhead = answered.queries > 0 && currentForced >= 0 && currentForced != answered.quality;

    if (ImGui::Button("Apply preset"))
    {
        if (live)
            quality->Request(ext::Cfg().upscaler.forcePerfQuality.value_or_default());
        else
            for (auto& entry : ctx.state.changeBackend)
                entry.second = true; // mirrors menu_common.cpp's MARK_ALL_BACKENDS_CHANGED()
    }

    if (live)
        ImGui::TextDisabled("Native scene update enabled. Presets apply automatically.");
    else if (selectionAhead)
        ImGui::TextDisabled("Preset pending a new resolution query; apply or restart the game.");
}

const ext::menu::AutoRegister registration { { "forced_quality", "Upscaling", "Forced quality level", Draw, "sharpness",
                                                 Visible } };
} // namespace
} // namespace ext::upscaler::menu
