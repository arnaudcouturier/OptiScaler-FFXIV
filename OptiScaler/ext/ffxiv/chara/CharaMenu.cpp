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
#include "ext/ffxiv/chara/CharaWindowSet.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// Menu card of the character preview feature (CharaView). The texts are the ones of the build this feature was
// reconstructed from, so users of both builds see the same labels. Hosted on a matched FFXIV build only (the hooks that
// feed it live in the lighting capture).
//
// Layout (tab FFXIV, position in ext/core/MenuLayout.h): the user toggles on top (Preview DLSS, Preview spatial AA with its
// 2x upscaling option, Preview NR with "Run NR before DLSS"), then the one-line states that explain what is running or why
// nothing is, then ONE fold "Advanced (debug)", closed by default, with every counter and debug aid. The fold is drawn by
// DrawCharaCard itself (TreeNode/TreePop in one place, no return in between).
namespace
{
using namespace ext::ffxiv::chara;

bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

// ---- main view -----------------------------------------------------------------------------------------------------

// Preview Neural Rendering (new work, not part of the build the other preview features were reconstructed from):
// the two switches.
void DrawNrToggles(CharaConfig& cfg)
{
    bool nrOn = cfg.previewNr.value_or_default();
    if (ImGui::Checkbox("Preview Neural Rendering (NR, experimental)", &nrOn))
        cfg.previewNr = nrOn;
    ext::menu::Tooltip("Runs the DLSS NR model on each open character preview window (new work). Needs an NVIDIA card, the DirectX 11 on 12 "
                       "bridge, and nvngx_dlssnr.dll with nvngx.dll_dlssnr.dll. Alone it runs on the native preview image. Together with "
                       "Preview DLSS it runs on the DLSS result (twice the size, with the preview's depth and motion) and the preview's own "
                       "alpha is put back afterwards; when NR cannot run for a render the plain DLSS image is shown. The spatial AA still "
                       "takes the equipment preview while NR has no result for a draw, unless the Preview DLSS pass runs. Every open window costs "
                       "video memory and GPU time; the numbers in the Advanced (debug) section show both. Any failure switches it off for the session and the "
                       "native preview stays.");

    bool before = cfg.previewNrBeforeSr.value_or_default();
    if (ImGui::Checkbox("Run NR before DLSS (experimental)", &before))
        cfg.previewNrBeforeSr = before;
    ext::menu::Tooltip("Runs the NR model on the picture before DLSS instead of on the DLSS result. Needs Preview DLSS and Preview NR. The model then works at "
                       "the render size, so it is expected to use less GPU time and video memory per open window (not measured yet), but it sees a jittered picture that has not "
                       "been anti-aliased yet: look for shimmer or trails on hair and cloth and switch it off if you see any. If DLSS refuses the NR "
                       "picture, this render uses the plain picture, this option switches itself off for the session (one log line) and NR runs after "
                       "DLSS as usual. The previous-frame option does not apply to it. Switching it rebuilds the window's model after half a second.");
}

// The one-line state of Preview NR (off and releasing, unavailable and why, ready, preparing, running)
void DrawNrStatusLine(const CharaConfig& cfg)
{
    const bool nrOn = cfg.previewNr.value_or_default();
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
}

// The processed set: how many preview windows get the upscaler and the NR work. Drawn with the user switches, before the fold.
void DrawWindowBudget(CharaConfig& cfg)
{
    bool budget = cfg.previewBudget.value_or_default();
    if (ImGui::Checkbox("Limit the number of processed preview windows (experimental)", &budget))
        cfg.previewBudget = budget;
    ext::menu::Tooltip("With this on, only the set number of preview windows gets the DLSS pass and preview NR; the others keep the game's own "
                       "picture and are not jittered. A window is recognised by the camera constant buffer it draws with (the buffer, its first "
                       "constant and the render size), without reading anything back from the GPU, so a render whose window cannot be recognised yet "
                       "runs untouched. If the game's windows cannot be told apart, none of them is processed and each keeps the game's own picture; "
                       "the log line `preview identity` shows it. A window that draws and is on screen takes the place of one that has not been drawn "
                       "for a quarter of a second, at most one exchange per quarter of a second, so a hitch does not rebuild the whole set; two windows "
                       "that alternate keep their places. Closing a processed window hands its place to a waiting one. Every processed window costs GPU "
                       "time and video memory, so a view with several characters is smoother with two than with all of them. Off by default: every preview "
                       "window is processed as before.");

    int maxWindows = static_cast<int>(cfg.previewMaxWindows.value_or_default());
    ImGui::BeginDisabled(!budget);
    if (ImGui::SliderInt("Processed preview windows", &maxWindows, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp))
        cfg.previewMaxWindows = static_cast<uint32_t>(maxWindows);
    ImGui::EndDisabled();
    ext::menu::Tooltip("How many preview windows are processed while the limit above is on (1 to 8; the preview renderer has eight output entries). "
                       "Two covers the character window plus one more (examine, try-on, dye). Not read while the limit is off.");

    if (budget)
        ImGui::TextDisabled("Processed preview windows: %u of %u (the others keep the game's picture)",
                            g_stats.windowsProcessed.load(std::memory_order_relaxed), g_stats.windowsMax.load(std::memory_order_relaxed));
}

// ---- Advanced (debug) ------------------------------------------------------------------------------------------------

// The counters of Preview NR (everything below the one-line state)
void DrawNrCounters(const CharaConfig& cfg)
{
    const bool nrOn = cfg.previewNr.value_or_default();
    const bool before = cfg.previewNrBeforeSr.value_or_default();
    const nr::Status st = nr::GetStatus();
    if (!nrOn && st.streams == 0)
        return;

    ImGui::TextDisabled("NR windows: %u active, %u model(s); %llu NR results, %llu renders kept the native or plain DLSS image (focus pauses not counted)", st.liveStreams,
                        st.streams, (unsigned long long) st.runs, (unsigned long long) st.fallbacks);
    ImGui::TextDisabled("UI draws given an NR result: %llu", (unsigned long long) g_stats.nrSwaps.load(std::memory_order_relaxed));
    if (st.lastGpuMs > 0.0 || st.lastCpuMs > 0.0)
        ImGui::TextDisabled("Last NR run: GPU %.2f ms, CPU %.2f ms (recording and submitting)", st.lastGpuMs, st.lastCpuMs);
    // The production path (NR on the DLSS result): its own numbers, shown while both options are on or after it ran.
    const bool dlssOn = cfg.previewDlss.value_or_default();
    const bool beforeActive = before && !st.preSrLatched; // NR runs before DLSS now: the line below is about NR on the DLSS result
    if ((nrOn && dlssOn && !beforeActive) || st.productionRuns != 0 || st.productionFallbacks != 0)
    {
        if (st.productionRuns == 0 && st.productionFallbacks == 0)
            ImGui::TextDisabled("NR on the DLSS result: waiting for a preview that DLSS evaluated");
        else
        {
            ImGui::TextDisabled("NR on the DLSS result: %llu runs, %llu renders kept the plain DLSS image (focus pauses not counted); %llu DLSS images used the NR output",
                                (unsigned long long) st.productionRuns, (unsigned long long) st.productionFallbacks,
                                (unsigned long long) g_stats.nrProductionRuns.load(std::memory_order_relaxed));
            ImGui::TextDisabled("UI draws given a DLSS image made from NR: %llu",
                                (unsigned long long) g_stats.nrProductionSwaps.load(std::memory_order_relaxed));
            if (st.productionGpuMs > 0.0 || st.productionCpuMs > 0.0)
                ImGui::TextDisabled("Last NR run on the DLSS result: GPU %.2f ms, CPU %.2f ms", st.productionGpuMs, st.productionCpuMs);
        }
    }
    // NR before DLSS: its own numbers (runs, renders that used the plain picture, the last cost), or why it is off.
    if (before || st.preSrRuns != 0 || st.preSrFallbacks != 0 || st.preSrLatched)
    {
        if (st.preSrLatched)
            ImGui::TextWrapped("NR before DLSS: switched off for this session (DLSS refused the NR picture); NR runs after DLSS.");
        else if (st.preSrRuns == 0 && st.preSrFallbacks == 0)
            ImGui::TextDisabled("NR before DLSS: waiting for a preview that DLSS evaluates");
        else
        {
            ImGui::TextDisabled("NR before DLSS: %llu runs, %llu renders used the plain picture (focus pauses not counted); DLSS history reset %llu time(s) for a picture change",
                                (unsigned long long) st.preSrRuns, (unsigned long long) st.preSrFallbacks,
                                (unsigned long long) g_stats.srSourceResets.load(std::memory_order_relaxed));
            if (st.preSrGpuMs > 0.0 || st.preSrCpuMs > 0.0)
                ImGui::TextDisabled("Last NR run before DLSS: GPU %.2f ms, CPU %.2f ms", st.preSrGpuMs, st.preSrCpuMs);
        }
    }
    if (st.videoMemoryMb != 0)
        ImGui::TextDisabled("Video memory added by the NR streams (images and models): about %llu MB", (unsigned long long) st.videoMemoryMb);
    else if (st.streams != 0)
        ImGui::TextDisabled("Video memory added by the NR streams (images and models): not measured");
    if (st.late)
        ImGui::TextDisabled("Showing the previous frame's NR result for NR on the native image (PreviewNRLate, bring-up only)");
}

// Everything diagnostic: counters, status of the passes, the debug aids and the probe. `inert` = the whole feature is off
// for this session (the reason is shown in the main view).
void DrawDebugSection(CharaConfig& cfg, bool inert)
{
    // The tuning and diagnostic keys of the preview. All four are read live, so a change applies at once.
    int lingerSeconds = static_cast<int>(cfg.previewNrLingerSeconds.value_or_default());
    if (ImGui::SliderInt("Keep preview NR models after closing (seconds)", &lingerSeconds, 0, 60, "%d", ImGuiSliderFlags_AlwaysClamp))
        cfg.previewNrLingerSeconds = static_cast<uint32_t>(lingerSeconds);
    ext::menu::Tooltip("How long a preview's NR model is kept after its window closes, so reopening the window is instant. "
                       "0 releases it as soon as the window is no longer drawn (a quarter of a second). Default 5. "
                       "Read every frame: applies at once.");

    bool late = cfg.previewNrLate.value_or_default();
    if (ImGui::Checkbox("Preview NR: show the previous frame's result (bring-up)", &late))
        cfg.previewNrLate = late;
    ext::menu::Tooltip("Show the NR result of the previous frame instead of waiting for this frame's: no GPU wait on the DirectX 11 "
                       "side, one frame of lag. Try it only if the wait is visible in the frame time. It applies to NR on the native "
                       "preview image only. A change applies at once and rebuilds the preview's model after half a second.");

    bool guideCapture = cfg.previewGuideCapture.value_or_default();
    if (ImGui::Checkbox("Write preview DLSS guide dumps (diagnostics)", &guideCapture))
        cfg.previewGuideCapture = guideCapture;
    ext::menu::Tooltip("Diagnostics for the DLSS path: writes up to 12 files (100 MiB) of guide and image dumps into "
                       "OptiScaler\\PreviewDLSSDiagnostics next to the DLL, twice per session, and logs the camera and motion "
                       "ranges. Leave off unless you are investigating. Read live: applies at once.");

    bool trace = cfg.previewNrTrace.value_or_default();
    if (ImGui::Checkbox("Trace preview NR in the log (diagnostics)", &trace))
        cfg.previewNrTrace = trace;
    ext::menu::Tooltip("Rate-limited log lines for the preview NR path (finisher verdicts, evaluations, UI swaps). Only logs, "
                       "changes nothing. Read live: applies at once.");

    bool wideShape = cfg.previewReplayWideShape.value_or_default();
    if (ImGui::Checkbox("Signed-motion replay: any camera block size (diagnostics)", &wideShape))
        cfg.previewReplayWideShape = wideShape;
    ext::menu::Tooltip("Diagnostics, leave off in normal use. The replay that produces the signed motion vectors normally only takes a "
                       "G-buffer draw whose vertex and pixel constant buffers are exactly the 59-float4 camera block (944 bytes). With this "
                       "on, any constant buffer size that can hold the camera block is accepted (the draw must still write the preview "
                       "renderer's own G-buffer). A shader that reads the camera block as a view into a larger buffer is then replayed "
                       "too, so more pixels get motion vectors and the picture can change: look for shimmer on hair and cloth and switch "
                       "it off if you see any. Read live: applies at once.");
    ImGui::Separator();

    const dlss::Counters counters = dlss::GetCounters();
    ImGui::Text("DLSS previews: %u active; %llu evaluations; %llu fallbacks", counters.activeSlots,
                (unsigned long long) counters.evaluations, (unsigned long long) counters.fallbacks);
    if (cfg.previewGuideCapture.value_or_default())
        ImGui::Text("Preview diagnostics: %u files, %llu bytes", ext::ffxiv::chara::diag::FilesWritten(),
                    (unsigned long long) ext::ffxiv::chara::diag::BytesWritten());

    DrawNrCounters(cfg);

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

    // The spatial AA runs when PreviewDLSS is off, and also while the DLSS path cannot run (its status note says why).
    const bool aa = cfg.previewAA.value_or_default();
    const bool dlss = cfg.previewDlss.value_or_default();
    const char* dlssNote = dlss::StatusNote();
    if (!inert && aa && (!dlss || (dlssNote != nullptr && dlssNote[0] != '\0')))
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

void DrawCharaCard(ext::menu::Ctx&)
{
    auto& cfg = ext::Cfg().ffxiv.chara;
    ext::menu::SectionTitle("Character preview");
    ImGui::TextDisabled("(experimental) Changes how the character preview windows are drawn. Off by default; the normal preview "
                        "stays whenever a pass cannot run.");

    // the user switches
    bool dlss = cfg.previewDlss.value_or_default();
    if (ImGui::Checkbox("Preview DLSS (2x output, experimental)", &dlss))
        cfg.previewDlss = dlss;
    if (const char* note = dlss::StatusNote(); note != nullptr && note[0] != '\0')
        ImGui::TextDisabled("%s", note);

    // Not greyed out: the AA is used whenever neither Preview DLSS nor Preview NR produces the equipment image (see the tooltip).
    bool aa = cfg.previewAA.value_or_default();
    if (ImGui::Checkbox("Preview spatial AA", &aa))
        cfg.previewAA = aa;
    ext::menu::Tooltip("Spatial anti-aliasing of the equipment preview image. It is used while Preview DLSS is off, and while the DLSS "
                       "pass cannot run. With Preview NR on, a result NR has for a draw is shown instead, so the AA only takes the draws "
                       "NR has no result for yet (the first frames after opening a window, renders it skipped, a preview it cannot "
                       "process). While the DLSS pass runs the AA is not used, also for a draw NR has no result for.");
    bool upscale = cfg.previewUpscale.value_or_default();
    if (ImGui::Checkbox("2x preview upscaling", &upscale))
        cfg.previewUpscale = upscale;

    ImGui::TextWrapped("DLSS uses each preview's native resolution with 2x output. Separate histories; up to 16 previews. "
                       "Unsupported layouts retain native rendering. Frame generation does not apply to the preview.");

    DrawNrToggles(cfg);
    DrawNrStatusLine(cfg);
    DrawWindowBudget(cfg);

    // Why the whole feature is off, if it is (wrong game build, accessor not verified). Never written to the config.
    const char* reason = table::InertReason();
    const bool inert = reason != nullptr && reason[0] != '\0';
    if (inert)
        ImGui::TextWrapped("Character preview is off for this session: %s.", reason);

    // every counter and debug aid, closed by default
    if (ImGui::TreeNode("Advanced (debug)##chara_preview"))
    {
        DrawDebugSection(cfg, inert);
        ImGui::TreePop();
    }
}

const ext::menu::AutoRegister registration { { "ffxiv_character_preview", "FFXIV", "Character preview", DrawCharaCard,
                                               "dlss_nr", Visible } };
} // namespace
