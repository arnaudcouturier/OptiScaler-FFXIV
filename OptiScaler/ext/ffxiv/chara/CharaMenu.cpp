#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/ffxiv/chara/CharaDiag.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaProbe.h"
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

// Preview Neural Rendering (new work, not part of the build the other preview features were reconstructed from).
void DrawNrSection(CharaConfig& cfg)
{
    bool nrOn = cfg.previewNr.value_or_default();
    if (ImGui::Checkbox("Preview Neural Rendering (NR, experimental)", &nrOn))
        cfg.previewNr = nrOn;
    ext::menu::Tooltip("Runs the DLSS NR model on each open character preview window (new work). Needs an NVIDIA card, the DirectX 11 on 12 "
                       "bridge, and nvngx_dlssnr.dll with nvngx.dll_dlssnr.dll. It works alone or together with Preview DLSS (the DLSS "
                       "result is shown when both produced one). The spatial AA still takes the equipment preview while NR has no result "
                       "for a draw. Every open window costs "
                       "video memory and GPU time; the numbers below show both. Any failure switches it off for the session and the "
                       "native preview stays.");

    const nr::Status st = nr::GetStatus();
    if (!nrOn && st.streams == 0)
        return;

    if (!nrOn)
        ImGui::TextDisabled("Preview NR: off, releasing %u model(s)", st.streams);
    else if (st.availability != nr::Availability::Available)
        ImGui::TextWrapped("Preview NR: unavailable: %s.", st.reason[0] != 0 ? st.reason : "not ready");
    else if (st.runs == 0 && st.streams == 0)
        ImGui::TextDisabled("Preview NR: ready, waiting for a character preview window");
    else if (st.runs == 0)
        ImGui::TextDisabled("Preview NR: preparing the model (the first result comes a few frames later)");
    else
        ImGui::Text("Preview NR: running");

    ImGui::TextDisabled("NR windows: %u active, %u model(s); %llu NR results, %llu renders kept the native image", st.liveStreams,
                        st.streams, (unsigned long long) st.runs, (unsigned long long) st.fallbacks);
    ImGui::TextDisabled("UI draws given an NR result: %llu", (unsigned long long) g_stats.nrSwaps.load(std::memory_order_relaxed));
    if (st.lastGpuMs > 0.0 || st.lastCpuMs > 0.0)
        ImGui::TextDisabled("Last NR run: GPU %.2f ms, CPU %.2f ms (recording and submitting)", st.lastGpuMs, st.lastCpuMs);
    if (st.videoMemoryMb != 0)
        ImGui::TextDisabled("Video memory added by the NR models: about %llu MB", (unsigned long long) st.videoMemoryMb);
    else if (st.streams != 0)
        ImGui::TextDisabled("Video memory added by the NR models: not measured");
    if (st.late)
        ImGui::TextDisabled("Showing the previous frame's NR result (PreviewNRLate)");
}

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

    // Not greyed out: the AA is used whenever neither Preview DLSS nor Preview NR produces the equipment image (see the tooltip).
    bool aa = cfg.previewAA.value_or_default();
    if (ImGui::Checkbox("Preview spatial AA", &aa))
        cfg.previewAA = aa;
    ext::menu::Tooltip("Spatial anti-aliasing of the equipment preview image. It is used when neither Preview DLSS nor Preview NR "
                       "produces that image: with both off, while the DLSS path cannot run, and with Preview NR on whenever NR has no "
                       "result for a draw yet (the first frames after opening a window, renders it skipped, a preview it cannot process). "
                       "When Preview DLSS runs, its result is shown instead.");
    bool upscale = cfg.previewUpscale.value_or_default();
    if (ImGui::Checkbox("2x preview upscaling", &upscale))
        cfg.previewUpscale = upscale;

    ImGui::TextWrapped("DLSS uses each preview's native resolution with 2x output. Separate histories; up to 16 previews. "
                       "Unsupported layouts retain native rendering. Frame generation does not apply to the preview.");

    DrawNrSection(cfg);

    bool tint = cfg.previewDebugTint.value_or_default();
    if (ImGui::Checkbox("Show preview processing (debug)", &tint))
        cfg.previewDebugTint = tint;
    ext::menu::Tooltip("Debug aid, leave off in normal use. Mixes magenta into the preview image that the spatial AA or the DLSS "
                       "pass produced, so you can see in the game whether the processed image is the one on screen.");
    ImGui::TextDisabled("Preview debug tint: %s, applied %llu times", tint ? "on" : "off",
                        (unsigned long long) g_stats.tintApplied.load(std::memory_order_relaxed));

    bool flood = cfg.previewDebugFlood.value_or_default();
    if (ImGui::Checkbox("Flood the preview with solid magenta (debug)", &flood))
        cfg.previewDebugFlood = flood;
    ext::menu::Tooltip("Debug aid, leave off in normal use. Every draw that samples the preview image, or a copy of it that was followed, "
                       "gets a solid magenta image instead. If nothing on screen turns magenta while this is on, the draws that were "
                       "changed do not put the preview on screen. It works without the spatial AA, the DLSS path and the probe. A draw "
                       "that reads the image with texel loads instead of sampling it shows black with a magenta corner. It checks every "
                       "draw while it is on.");
    ImGui::TextDisabled("Preview flood: %s, %llu draws given the magenta image", flood ? "on" : "off",
                        (unsigned long long) g_stats.floodSwaps.load(std::memory_order_relaxed));

    bool probeOn = cfg.previewProbe.value_or_default();
    if (ImGui::Checkbox("Preview probe (debug)", &probeOn))
        cfg.previewProbe = probeOn;
    ext::menu::Tooltip("Debug aid, leave off in normal use. Follows the preview image through copies and the draws that sample it, "
                       "and writes what it finds to the log (lines that start with \"CharaView probe:\"). It adds a small cost to every "
                       "draw while the preview is open.");
    if (probeOn)
    {
        ImGui::TextDisabled("Probe: %u textures derived from the preview, %u copy cases, %u sampler-draw cases, %u swap traces",
                            g_stats.probeDerivedLive.load(std::memory_order_relaxed), g_stats.probeCopyLines.load(std::memory_order_relaxed),
                            g_stats.probeSamplerLines.load(std::memory_order_relaxed), g_stats.probeSwapLines.load(std::memory_order_relaxed));
        ImGui::TextDisabled("Probe: %llu copies of the preview followed, %llu draws sampled the preview or a copy",
                            (unsigned long long) g_stats.probeCopies.load(std::memory_order_relaxed),
                            (unsigned long long) g_stats.probeSamplerDraws.load(std::memory_order_relaxed));
        if (probe::ResolveTrackingIncomplete())
            ImGui::TextWrapped("Probe: copy tracking incomplete: ResolveSubresource could not be hooked, so copies made by resolves are not followed "
                               "(the other hooks are not affected).");

        const probe::DumpStatus dumpStatus = probe::GetDumpStatus();
        ImGui::BeginDisabled(dumpStatus.busy);
        if (ImGui::Button("Save preview images"))
            probe::RequestDump();
        ImGui::EndDisabled();
        ext::menu::Tooltip("Saves the native preview image, the spatial AA result, the DLSS 2x result and the preview NR result (whichever exist) as PNG files, "
                           "each with its alpha channel as a separate grayscale PNG, plus state.txt, into a new time-stamped folder under "
                           "OptiScaler\\PreviewDebug next to the game's dxgi.dll. It never waits for the GPU. At most 40 files and 300 MiB "
                           "per session.");
        if (dumpStatus.busy)
            ImGui::TextDisabled("Preview images: saving...");
        ImGui::TextDisabled("Preview images: %u files written in %u dumps, %u refused by the budget%s%s", dumpStatus.files, dumpStatus.dumps,
                            dumpStatus.refusals, dumpStatus.lastFolder.empty() ? "" : ", last folder ", dumpStatus.lastFolder.c_str());
        if (!dumpStatus.lastMessage.empty())
            ImGui::TextDisabled("Last request: %s", dumpStatus.lastMessage.c_str());
    }

    // Why the whole feature is off, if it is (wrong game build, accessor not verified). Never written to the config.
    if (const char* reason = table::InertReason(); reason != nullptr && reason[0] != '\0')
    {
        ImGui::TextWrapped("Character preview is off for this session: %s.", reason);
        return;
    }

    // The spatial AA runs when PreviewDLSS is off, and also while the DLSS path cannot run (its status note says why).
    const char* dlssNote = dlss::StatusNote();
    if (aa && (!dlss || (dlssNote != nullptr && dlssNote[0] != '\0')))
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
