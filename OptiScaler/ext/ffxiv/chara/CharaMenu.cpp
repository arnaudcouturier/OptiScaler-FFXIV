#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/chara/CharaDiag.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// Menu card of the character preview feature (CharaView). The texts are the ones of the build this feature was
// reconstructed from, so users of both builds see the same labels. Hosted on a matched FFXIV build only (the hooks that
// feed it live in the lighting capture).
namespace
{
using namespace ext::ffxiv::chara;

bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void DrawCharaCard(ext::menu::Ctx&)
{
    auto& cfg = ext::Cfg().ffxiv.chara;
    ext::menu::SectionTitle("Character preview (experimental)");

    bool dlss = cfg.previewDlss.value_or_default();
    if (ImGui::Checkbox("Preview DLSS (2x output, experimental)", &dlss))
        cfg.previewDlss = dlss;

    const dlss::Counters counters = dlss::GetCounters();
    ImGui::Text("DLSS previews: %u active; %llu evaluations; %llu fallbacks", counters.activeSlots,
                (unsigned long long) counters.evaluations, (unsigned long long) counters.fallbacks);
    if (const char* note = dlss::StatusNote(); note != nullptr && note[0] != '\0')
        ImGui::TextDisabled("%s", note);
    if (cfg.previewGuideCapture.value_or_default())
        ImGui::Text("Preview diagnostics: %u files, %llu bytes", ext::ffxiv::chara::diag::FilesWritten(),
                    (unsigned long long) ext::ffxiv::chara::diag::BytesWritten());

    ImGui::BeginDisabled(dlss);
    bool aa = cfg.previewAA.value_or_default();
    if (ImGui::Checkbox("Preview spatial AA", &aa))
        cfg.previewAA = aa;
    bool upscale = cfg.previewUpscale.value_or_default();
    if (ImGui::Checkbox("2x preview upscaling", &upscale))
        cfg.previewUpscale = upscale;
    ImGui::EndDisabled();

    ImGui::TextWrapped("DLSS uses each preview's native resolution with 2x output. Separate histories; up to 16 previews. "
                       "Unsupported layouts retain native rendering. Preview NR and FG are not active.");

    bool tint = cfg.previewDebugTint.value_or_default();
    if (ImGui::Checkbox("Show preview processing (debug)", &tint))
        cfg.previewDebugTint = tint;
    ext::menu::Tooltip("Debug aid, leave off in normal use. Mixes magenta into the preview image that the spatial AA or the DLSS "
                       "pass produced, so you can see in the game whether the processed image is the one on screen.");

    // Why the whole feature is off, if it is (wrong game build, accessor not verified). Never written to the config.
    if (const char* reason = table::InertReason(); reason != nullptr && reason[0] != '\0')
    {
        ImGui::TextWrapped("Character preview is off for this session: %s.", reason);
        return;
    }

    if (aa && !dlss)
    {
        const unsigned long long processed = g_stats.aaProcessed.load(std::memory_order_relaxed);
        ImGui::Text("Processed preview draws: %llu", processed);
        if (processed == 0)
        {
            if (g_stats.deferredSeen.load(std::memory_order_relaxed) != 0)
                ImGui::TextWrapped("Native preview retained: deferred rendering needs a separate integration path.");
            else if (g_stats.equipShaderSeen.load(std::memory_order_relaxed) != 0)
                ImGui::TextWrapped("Waiting for the native equipment preview texture.");
            else
                ImGui::TextWrapped("Waiting for the equipment preview display shader.");
        }
    }
}

const ext::menu::AutoRegister registration { { "ffxiv_character_preview", "Upscaling", "Character preview", DrawCharaCard,
                                               "dlss_nr", Visible } };
} // namespace
