#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"
#include "ext/nr/DlssNr.h"
#include "ext/nr/NrHooks.h"
#include "ext/nr/amd/AmdBridge.h"
#include "ext/nr/exposure/DlssNr_ExposureScan.h"
#include "ext/nr/native/DlssNrNative.h"
#include "ext/nr/presets/NrUserPresets.h"
#include "ext/nr/vk/DlssNrFeature_Vk.h"

#include <Config.h>
#include <State.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DlssNr
{
#include "ext/nr/presets/NrUserPresetsMenu.inl"

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f", bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(label, "Intensity") == 0)
        HelpMarker("Overall enhancement strength for this pass. 1 = default; results depend on the profile.\nValues above 1 are experimental; the runtime may clamp or ignore them.");
    else if (std::strcmp(label, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast requested from the model (high-frequency structure).\n1 = default; values above 1 are experimental.");
    else if (std::strcmp(label, "Local tone") == 0)
        HelpMarker("Broad brightness and lighting changes requested from the model (low-frequency tone).\nLater passes default to 0. Values above 1 are experimental.");
    else if (std::strcmp(label, "Skin structure") == 0)
        HelpMarker("Fine detail for pixels the model identifies as skin. -1 follows Local structure; 0 reduces skin detail.\nSkin colour is controlled separately. Values above 1 are experimental.");
    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt,
                                  const char* const* names, int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

// Drawn as the body of the ext::menu card `dlss_nr` (ext/nr/menu/NrMenu.cpp), i.e. inside upstream's ScopedCard: the
// card title replaces the reference build's own collapsing header (docs/SEAMS.md 3.6).
void RenderMenu(float menuResScale)
{
    auto& nr = ext::Cfg().nr;

    // DLSS Neural Rendering -----------------------------
    ext::menu::SectionTitle("DLSS Neural Rendering");

    // The per-resource scan hooks (ext::Active(F::Nr)) are latched by the ini and by the first enabled evaluate; the
    // menu (checkbox, saved presets) may enable NR at runtime, so latch it here too and the scan starts this frame.
    if (nr.enabled.value_or_default())
        ext::SetActive(ext::F::Nr, true);

    {
        ImGui::Spacing();

        RenderUserPresets(nr);

        const auto amdStatus = DlssNr::AmdBridge::Status();
        const auto amdPrerequisite = DlssNr::AmdBridge::PrerequisiteError();
        if (!amdStatus.empty())
        {
            ImGui::TextWrapped("%s", amdPrerequisite.empty() ? amdStatus.c_str() : amdPrerequisite.c_str());
            ImGui::TextWrapped("AMD experimental: enable Run before upscaling. NVIDIA model/style controls do not apply.");
        }
        bool enabled = nr.enabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            nr.enabled = enabled;

        HelpMarker("Enhance lighting and material appearance with the NR model. Placement selects before or after upscaling.\nRequires nvngx_dlssnr.dll plus the included nvngx.dll_dlssnr.dll helper.");

        bool beforeSr = nr.runBeforeSr.value_or_default();
        const bool deferredActive = nr.deferredDlss.value_or_default();
        if (deferredActive)
            ImGui::BeginDisabled();
        if (ImGui::Checkbox("Apply before Super Resolution", &beforeSr))
            nr.runBeforeSr = beforeSr;
        if (deferredActive)
            ImGui::EndDisabled();

        HelpMarker("On: apply NR before SR or combined RR+SR. Off: apply it afterward.\nBefore RR is experimental. Unsupported input layouts fall back after upscaling.");

        bool spatial = nr.spatialCompression.value_or_default();
        if (ImGui::Checkbox("Peripheral spatial compression (experimental)", &spatial))
            nr.spatialCompression = spatial;
        HelpMarker("Preserves the central image and spends fewer NR pixels at the edges.\nColor, depth and motion are transformed together. Uses matched residual composition.\nThis test supports DX12 model resolutions up to 100%; larger scales use ordinary NR.");
        if (spatial)
        {
            if (DeferredSlider("Central region", &nr.spatialCenterX, 10.0f, 95.0f, 80.0f, "%.0f%%"))
                nr.spatialCenterY = nr.spatialCenterX.value_or_default();
            if (DeferredSlider("Peripheral work budget", &nr.spatialWorkX, 25.0f, 100.0f, 90.0f, "%.0f%%"))
                nr.spatialWorkY = nr.spatialWorkX.value_or_default();
            ImGui::TextWrapped("%s", SpatialStatus().c_str());
            ImGui::TextDisabled("Work budget must exceed the central-region percentage.");
        }

        bool deferredDlss = nr.deferredDlss.value_or_default();
        int precisionChoice = nr.precision.value_or_default() == 4 ? 1 : 0;
        const char* precisions[] = { "NVIDIA (FP8)", "Experimental (FP8+NVFP4 hybrid)" };
        if (ImGui::Combo("Model precision", &precisionChoice, precisions, IM_ARRAYSIZE(precisions)))
            nr.precision = precisionChoice == 1 ? 4u : 0u;
        HelpMarker("NVIDIA: original FP8 model (default), with some sensitive operations kept at higher precision.\nExperimental: this fork's FP8+NVFP4 hybrid for RTX 50 GPUs; output may differ slightly.");
        ImGui::BeginDisabled(precisionChoice > 0 || !amdStatus.empty());
        bool vitReuse = nr.vitEvery.value_or_default() > 1;
        if (ImGui::Checkbox("Reuse NR bottleneck every second evaluation (experimental)", &vitReuse))
            nr.vitEvery = vitReuse ? 2u : 1u;
        HelpMarker("Requires a verified NVIDIA NR kernel module unless the experimental override below is enabled. Alternate/patched runtimes may remain unavailable; the status explains why. May cause stale detail during motion. Off by default; hybrid mode always computes fully.");
        ImGui::BeginDisabled(!vitReuse);
        bool firstPassOnly = nr.vitFirstPassOnly.value_or_default();
        if (ImGui::Checkbox("First pass only (later passes compute fully)", &firstPassOnly))
            nr.vitFirstPassOnly = firstPassOnly;
        HelpMarker("On: limits reuse to pass 1 to reduce multipass flicker. Off: restores the original reuse on every pass, which may save more work but can flicker on VFX or transparent effects. Applies live. Save Settings to keep your choice.");
        bool allowUnverified = nr.vitAllowUnverified.value_or_default();
        if (ImGui::Checkbox("Allow unverified NR kernels (experimental)", &allowUnverified))
            nr.vitAllowUnverified = allowUnverified;
        HelpMarker("Bypasses the exact kernel-module fingerprint check for bottleneck reuse. Kernel names and launch-order checks remain required. May cause corruption, GPU hangs or crashes on incompatible runtimes. Off by default. Applies live; changing this forces a fresh computation. Save Settings to retain it.");
        ImGui::EndDisabled();
        if (vitReuse && allowUnverified)
            ImGui::TextWrapped("Experimental: NR kernel compatibility is not verified.");
        if (vitReuse) ImGui::TextWrapped("%s", DlssNrNative::VitStatus().c_str());
        if (vitReuse && nr.passes.value_or_default() > 1)
            ImGui::TextWrapped("%s", firstPassOnly ? "Multipass: reuse on pass 1 only." : "Multipass: reuse on every pass (original behaviour).");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(nr.passes.value_or_default() < 2 || !amdStatus.empty());
        float feedback = nr.passFeedback.value_or_default();
        if (ImGui::SliderFloat("Multipass feedback", &feedback, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) nr.passFeedback = feedback;
        HelpMarker("How much of each pass's edit reaches the next pass. 1 preserves the existing behavior. Single-pass NR is unchanged.");
        ImGui::EndDisabled();
        const auto hybridStatus = DlssNrNative::Status();
        if (hybridStatus.rfind("Restart required:", 0) == 0 ||
            (precisionChoice > 0 && hybridStatus.find("fallback") != std::string::npos))
            ImGui::TextWrapped("%s", hybridStatus.c_str());
        if (ImGui::Checkbox("Generate before SR, apply after SR (DLSS)", &deferredDlss))
            nr.deferredDlss = deferredDlss;
        HelpMarker("Compute NR at input resolution, upscale its changes with DLSS, then apply them after SR.\nExperimental: may flicker and adds GPU cost. Requires DLSS on DX12 or its bridges; does not support RR.\nOverrides Apply before Super Resolution. Disable Hold frame, Compare and Debug view.");
        if (deferredDlss)
            ImGui::TextWrapped("Residual DLSS: %s", DlssNr::DeferredDlssStatus().c_str());
        ImGui::BeginDisabled(!deferredDlss);
        bool trailGuard = nr.preSrTrailGuard.value_or_default();
        if (ImGui::Checkbox("Reduce PreSR trailing (experimental)", &trailGuard))
            nr.preSrTrailGuard = trailGuard;
        HelpMarker("Limits reconstructed NR edits to the current frame's local range before residual FG.\nAlso reduces interpolated edits where the clean scene changes strongly.\nMay reduce trails, but can reduce fine detail or expose flicker. Applies live.\nAdds a GPU pass and one output-sized texture. Test with NR every second frame off first.");
        bool residualFg = nr.residualFg.value_or_default();
        if (ImGui::Checkbox("NR every second frame (NVIDIA FG, experimental)", &residualFg))
            nr.residualFg = residualFg;
        HelpMarker("Run NR every other rendered frame and use NVIDIA FG to interpolate its changes.\nRequires the option above. Adds one rendered frame of latency and may misalign effects or UI.\nIf motion vectors are unavailable, each NR result is reused for two frames.");
        bool approxCamera = nr.residualFgApproxCamera.value_or_default();
        if (ImGui::Checkbox("Allow approximate FG camera guides (experimental)", &approxCamera))
            nr.residualFgApproxCamera = approxCamera;
        HelpMarker("Allows the every-second-frame mode to run its separate NVIDIA FG pass using estimated camera data.\nThis adds FG and image-history work; it is not just a cheap guide toggle. May cause artifacts during camera movement.\nThe GPU breakdown reports cost per call. Normally NR and residual FG run together on NR anchors; the split-work option separates them.");
        ImGui::BeginDisabled(!residualFg || !approxCamera || !ext::nr::BridgedNativeTitle());
        bool splitWork = nr.splitFrameWork.value_or_default();
        if (ImGui::Checkbox("Spread alternating NR across two frames (experimental)", &splitWork))
            nr.splitFrameWork = splitWork;
        HelpMarker("FFXIV: run NR on one frame and its private DLSS/FG on the next.\nRetains matching guides and scene images. Adds one extra rendered frame of buffering (two total).\nMay improve pacing but increases latency and memory use. Applies live and restarts private history.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("Set the NR toggle shortcut under Keybinds.");

        bool applyModel = nr.applyModel.value_or_default();
        if (ImGui::Checkbox("Apply the model", &applyModel))
            nr.applyModel = applyModel;

        HelpMarker("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural Rendering to stop its GPU cost.");

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled("NR off.");
        }
        else if (!amdStatus.empty())
        {
            // AMD status and prerequisites are displayed above; NVIDIA NGX state does not apply.
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const auto feature = State::Instance().currentFeature;
            const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
            const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (nativeVk)
                    ImGui::TextUnformatted("Restart the game to retry native Vulkan NR.");
                else if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
            {
                ImGui::TextWrapped("NR needs the D3D12 bridge on D3D11. Choose an upscaler marked w/Dx12 and restart.");
            }
            else if (nativeVk && nr.deferredDlss.value_or_default())
            {
                ImGui::TextWrapped("Disable Generate before SR, apply after SR (DLSS) to use native Vulkan NR.");
            }
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame. So the cost is real, and saying so
            // stops the reading looking like a bug. Enable Neural Rendering off is what zeroes it.
            const char* runSuffix =
                !nr.applyModel.value_or_default() ? "  (model running, edit hidden)" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms per frame%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Total NR GPU time, including model processing, copies and composition.\nCompare with overall frame time to judge its cost.");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("Performance");

        bool unlockPasses = nr.unlockPasses.value_or_default();
        if (ImGui::Checkbox("Lift model pass limit (up to 30; expensive)", &unlockPasses))
            nr.unlockPasses = unlockPasses;
        HelpMarker("Allow up to 30 passes instead of 3. More passes use more GPU time and VRAM; high values may crash the game.");
        const unsigned int passLimit = unlockPasses ? MaxPassCount : DefaultMaxPassCount;

        {
            int passes = (int) std::clamp(nr.passes.value_or_default(), 1u,
                                          passLimit);
            const ImVec4 colour = passes <= 1   ? ImVec4(0.35f, 0.88f, 0.38f, 1.0f)
                                  : passes == 2 ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                                : ImVec4(0.92f, 0.30f, 0.25f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, colour);

            if (ImGui::SliderInt("Model passes", &passes, 1, (int) passLimit,
                                 passes == 1 ? "%d (normal)" : "%dx model cost"))
                nr.passes = (uint32_t) std::clamp(passes, 1, (int) passLimit);

            ImGui::PopStyleColor(2);

            HelpMarker("Process the image repeatedly. More passes strengthen the effect and increase GPU cost.\nEach pass has its own settings and history. Start with 1.");
        }

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.
        static int pendingScale = -1;

        int scalePercent = pendingScale >= 0
                               ? pendingScale
                               : (int) lroundf(nr.workingScale.value_or_default() * 100.0f);

        unsigned modelWidth = 0, modelHeight = 0;
        CurrentModelSize(modelWidth, modelHeight);
        if (modelWidth && modelHeight)
            ImGui::TextDisabled("Model input: %u x %u; network: %u x %u", modelWidth, modelHeight,
                                (modelWidth + 1) / 2, (modelHeight + 1) / 2);
        if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
        {
            nr.workingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
        }

        HelpMarker("NR resolution relative to the image it processes. 50% halves width and height; 100% uses the full size.\nLower values reduce cost and fine detail. Above 100% increases cost. Game output resolution is unchanged.");

        if (scalePercent > 100)
            ImGui::TextDisabled("NR scale: %.2fx. Higher resolution increases GPU cost.",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", "Lanczos2",
                                             "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
            int ds = (int) nr.scalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                nr.scalingDownscaler = (Scaler) ds;

            HelpMarker("Filter used to reduce NR output when Model resolution exceeds 100%.\nSharper filters may introduce ringing around edges.");
        }


        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = nr.workingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "Classic", "Matched residual" };
            int enlarge = nr.transfer.value_or_default() == 1 ? 1 : 0;

            if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                nr.transfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("Below 100% model resolution: Classic enlarges the model output; Matched residual enlarges only its changes.\nMatched residual can reduce blur and colour shifts. No effect at 100% or above.");
        }

        ImGui::SeparatorText("Effect strength");

        float transfer = nr.transferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
            nr.transferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##detail"))
            nr.transferStrength = 1.0f;

        HelpMarker("Overall NR detail strength: 0 = no effect, 1 = normal, above 1 = exaggerated.");

        float colour = nr.colourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            nr.colourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##colour"))
            nr.colourStrength = 1.0f;

        HelpMarker("NR colour strength: 0 = preserve game colours, 1 = model colours, above 1 = stronger saturation.");

        // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
        // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
        static const char* reversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed",
                                                 "Neutwo proxy + replace", "Hybrid proxy + composed",
                                                 "Hybrid proxy + replace" };
        int reversible = (int) nr.reversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("HDR mapping (experimental)", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            nr.reversibleMode = (uint32_t) reversible;

        HelpMarker("Choose how HDR brightness is mapped for NR.\nSoft knee compresses highlights. Neutwo uses a reversible curve. Hybrid preserves midtones and compresses highlights.\nComposed uses the strength and highlight controls. Replace bypasses them and may flicker.");

        ImGui::SeparatorText("Model passes");
        ImGui::TextWrapped("Settings apply when you release a slider.");
        static const char* styles[] = { "Standard", "Natural", "Cinematic" };
        static const char* inheritedStyles[] = { "Auto (inherit pass 1)", "Standard", "Natural", "Cinematic" };

        if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int style = (int) std::min(nr.style.value_or_default(), 2u);
            if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
                nr.style = (uint32_t) style;
            HelpMarker("Select the appearance profile: Standard, Natural or Cinematic. Intensity controls its strength.");
            DeferredSlider("Intensity", &nr.intensity, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local structure", &nr.localStructure, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local tone", &nr.localTone, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Skin structure", &nr.skinStructure, -1.0f, 2.0f, -1.0f);
            bool mask = nr.autoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                nr.autoMask = mask;
            HelpMarker("Use the model's learned skin selection to apply Skin structure without an authored mask.\nAccuracy varies. This is separate from the colour-based mask below.");
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 2", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
            InheritedProfileCombo("Style", &nr.pass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &nr.pass2Intensity, 0.0f, 2.0f, nr.intensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &nr.pass2LocalStructure, 0.0f, 2.0f, nr.localStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &nr.pass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &nr.pass2SkinStructure, -1.0f, 2.0f, nr.skinStructure.value_or_default(), "%.2f", true);
            bool mask = nr.pass2AutoMask.has_value() ? nr.pass2AutoMask.value() : nr.autoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                nr.pass2AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                nr.pass2AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 3", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
            InheritedProfileCombo("Style", &nr.pass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &nr.pass3Intensity, 0.0f, 2.0f, nr.intensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &nr.pass3LocalStructure, 0.0f, 2.0f, nr.localStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &nr.pass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &nr.pass3SkinStructure, -1.0f, 2.0f, nr.skinStructure.value_or_default(), "%.2f", true);
            bool mask = nr.pass3AutoMask.has_value() ? nr.pass3AutoMask.value() : nr.autoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                nr.pass3AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                nr.pass3AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        const unsigned int visiblePasses = std::clamp(nr.passes.value_or_default(), 1u, passLimit);
        for (unsigned int pass = 3; pass < visiblePasses; ++pass)
        {
            auto& settings = nr.extraPasses[pass - 3];
            if (!ImGui::TreeNode(std::format("Pass {}", pass + 1).c_str()))
                continue;
            ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0.");
            InheritedProfileCombo("Style", &settings.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &settings.intensity, 0.0f, 2.0f, nr.intensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &settings.structure, 0.0f, 2.0f, nr.localStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &settings.tone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &settings.skin, -1.0f, 2.0f, nr.skinStructure.value_or_default(), "%.2f", true);
            bool mask = settings.autoMask.value_or(nr.autoMask.value_or_default());
            if (ImGui::Checkbox("Auto skin mask", &mask))
                settings.autoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                settings.autoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Advanced preset hints (effect unverified)"))
        {
            ImGui::TextWrapped("Experimental model hints; visual effect unverified. Use Style to select a profile.");
            static const char* presets[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
            static const char* inheritedPresets[] = { "Auto (inherit pass 1)", "Default", "Preset 1", "Preset 2", "Preset 3" };
            int preset = (int) std::min(nr.preset.value_or_default(), 3u);
            if (ImGui::Combo("Pass 1 preset hint", &preset, presets, IM_ARRAYSIZE(presets)))
                nr.preset = (uint32_t) preset;
            InheritedProfileCombo("Pass 2 preset hint", &nr.pass2Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            InheritedProfileCombo("Pass 3 preset hint", &nr.pass3Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Pass settings apply to SR and RR on DX12 and native Vulkan. The driver-proxy backend supports one pass.");

        ImGui::SeparatorText("Colour");

        if (ImGui::TreeNode("Skin and environment (final edit)"))
        {
            ImGui::TextWrapped("Select skin by colour and adjust the final NR effect separately for skin and scenery. Selection can be inaccurate; check Preview.");
            bool filter = nr.skinProtection.value_or_default();
            if (ImGui::Checkbox("Separate skin / environment controls", &filter))
                nr.skinProtection = filter;
            ImGui::BeginDisabled(!filter);
            bool tone = nr.skinToneEnabled.value_or_default();
            if (ImGui::Checkbox("Allow skin tone / colour changes", &tone))
                nr.skinToneEnabled = tone;
            HelpMarker("Allow NR colour changes in the selected skin region. Turn off to preserve its colour; detail can still change.");
            const auto slider = [](const char* label, auto& option) {
                float v = option.value_or_default();
                if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                    option = v;
                HelpMarker("NR strength in this region: 0 = no change, 1 = full effect.");
            };
            slider("Skin detail / lighting", nr.skinDetail);
            ImGui::BeginDisabled(!tone);
            slider("Skin colour", nr.skinColour);
            ImGui::EndDisabled();
            slider("Environment detail / lighting", nr.environmentDetail);
            slider("Environment colour", nr.environmentColour);
            bool preview = nr.showSkinMask.value_or_default();
            if (ImGui::Checkbox("Preview colour-based mask", &preview))
                nr.showSkinMask = preview;
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        // Native DX11 lighting is independent of the FG presentation bridge.
        auto* const lighting = ext::nr::NativeLighting(); // FFXIV native DX11 lighting (B6), null for every other game
        const bool nativeLighting = lighting != nullptr;
        if (nativeLighting)
            ImGui::TextWrapped("FFXIV applies exposure before NR. Native scanned lighting can detect abrupt lighting changes for history rejection.");
        else
            ImGui::TextDisabled("HDR input settings. Adjust the brightness range presented to NR.");




        if (nativeLighting)
        {
            static const char* sources[] { "Manual paper white", "Game exposure", "Scanned exposure (native DX11)" };
            int source = (int) std::min(nr.whitePointSource.value_or_default(), 2u);
            if (ImGui::Combo("Exposure source", &source, sources, IM_ARRAYSIZE(sources)))
                nr.whitePointSource = (uint32_t) source;
            HelpMarker("Scanned exposure follows FFXIV's verified native DX11 lighting shaders.\nThe game has already applied exposure: no additional brightness correction or calibration is used.");
            if (source == 2)
            {
                bool history = nr.lightingHistory.value_or_default();
                if (ImGui::Checkbox("Reject abrupt lighting history (experimental)", &history))
                    nr.lightingHistory = history;
                HelpMarker("Restart NR and its private PreSR reconstruction history when native tone response changes abruptly.\nGradual adaptation and stale readings do not trigger resets. Turn this off to compare while keeping the scan active.");
                const auto light = lighting->Status();
                if (light.failed)
                    ImGui::TextWrapped("Native lighting scan stopped after a capture or readback error. Lighting rejection is inactive until restart.");
                else if (!light.hooksInstalled)
                    ImGui::TextWrapped("Native lighting hooks unavailable. Lighting rejection is inactive.");
                else if (light.fresh)
                {
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                        "Native DX11 lighting: active | adapted gain %.3fx", light.gain);
                    ImGui::TextDisabled("%llu readings | %llu lighting cuts detected | %llu ms old",
                        light.samples, light.events, light.ageMs);
                    if (!nr.enabled.value_or_default() || !history || nr.holdFrame.value_or_default())
                        ImGui::TextDisabled("Monitoring only; lighting history rejection is inactive.");
                }
                else
                    ImGui::TextWrapped("Lighting rejection is inactive: %s", light.valid ? "last reading is stale." : light.reason);
            }
            else if (source == 1)
                ImGui::TextWrapped("FFXIV supplies no separate game exposure texture. Select Scanned exposure for native lighting tracking.");
            else
                ImGui::TextWrapped("Paper-white correction is bypassed for FFXIV's already tone-mapped input. Native lighting tracking is off.");

            float maxRatio = nr.maxRatio.value_or_default();
            if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, unlockPasses ? (float) MaxPassCount : 8.0f, "%.1fx"))
                nr.maxRatio = maxRatio;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##guard")) nr.maxRatio = 2.0f;
            HelpMarker("Limit how much NR can brighten or darken a pixel. Independent of native exposure tracking.");
        }
        else
        {
        // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
        // a frame the game already tone mapped wants roughly 1, the high end because there is no
        // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
        // given game needs to go is a property of that game's exposure, not of anything we can bound.
        // One tester was still improving at 100. A linear slider over that span would spend nine
        // tenths of its travel on values nobody needs and never reach the ones they do.
        // One dropdown, because there is one answer.
        //
        // This was two checkboxes that could both be on, and every attempt to stop that was a patch
        // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
        // once both were set the only way out was a button the notice never mentioned. Clearing
        // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
        // state being REACHED; a single choice cannot reach it, because there is only one value to
        // be in.
        //
        // Each option also says whether it can actually do anything in THIS game, in colour, so the
        // choice is made on what is available rather than on what sounds best.
        {
            const auto ex = DlssNr::GameExposureStatus();
            const bool vk = DlssNr::IsRunningVk();
            const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

            const float anchorNow = DlssNr::ExposureScan::BestValue();
            const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

            static const char* sourceNames[] = { "Manual paper white", "Game exposure",
                                                 "Scanned exposure (experimental)" };

            int source = (int) nr.whitePointSource.value_or_default();

            if (source < 0 || source > 2)
                source = 0;

            if (ImGui::Combo("White point source", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                nr.whitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("Manual: use Paper white. Game exposure: use exposure supplied by the game.\nScanned exposure: estimate it from game buffers; requires calibration and may select the wrong buffer.");

            // Availability, in colour, for the option currently chosen.
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("Waiting for a frame...");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "No game exposure available. Using manual paper white.");
                else if (vk)
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Using game exposure.");
                else if (ex.exposure > 1e-6f)
                {
                    const float trim =
                        std::clamp(nr.whitePointTrim.value_or_default(), 0.25f, 4.0f);
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Game exposure %.4f  ->  white point %.2f%s", ex.exposure,
                                       ex.preExposure / ex.exposure * trim,
                                       ex.offeredNow ? "" : "  (held: absent this frame)");
                }
                else
                    ImGui::TextDisabled("Reading exposure...");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "No exposure candidates found.");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "%u candidates; move between bright and dark areas to test them.",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "Exposure candidate found. Adjust Paper white, then select Anchor here.");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "Game exposure is available.");
            }
        }






        // A measured suggestion for paper white used to sit here and has been withdrawn.
        //
        // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
        // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
        // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
        // sits wherever most tiles are. The guard meant to catch that compared each tile against the
        // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
        // relative-threshold mistake the white point meter was removed for, made a second time.
        //
        // A wrong number offered confidently is worse than no number, so nothing is offered. What
        // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
        // the exposure texture where a game supplies one, and otherwise the ratio between the
        // scene-referred buffer and the finished frame, which is that exposure by definition.

        // Two controls, not one control with two meanings.
        //
        // These are different quantities. The manual path wants an absolute divisor on an open-ended
        // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
        // number the game already supplied, where 1 is correct and anything far from it says the read
        // is wrong rather than that somebody prefers it.
        //
        // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
        // a ruinous value unreachable but left two worse problems: moving the slider in one mode
        // silently destroyed the number found in the other, and there was no way back to "just take
        // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
        // Switching modes is now non-destructive in both directions.
        // The trim belongs to both automatic sources, since both end in "the game's number times a
        // little". Only the manual source gets the absolute slider.
        // One slider per source, each remembering its own number.
        //
        // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
        // things, and a value found against one means nothing against the other. Sharing them meant
        // changing source silently carried a number across, so a picture that had been tuned came
        // back wrong for a reason nothing on screen explained.
        //
        // The scan before it is anchored is the exception, and it has to be: anchoring captures an
        // absolute white point, so there must be an absolute slider to set. Showing a trim there
        // asked people to "set paper white below" next to a control that was not paper white.
        const int wpSource = (int) nr.whitePointSource.value_or_default();

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
        // local and not persisted; the anchor block below sets it when a row is clicked. Declared
        // here because both the slider (this block) and the table (below) read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();
        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            // The single scan -> white point readout, above the sliders it explains.
            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, nr.scanInverted.value_or_default(),
                        nr.scanTrim.value_or_default());

                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Paper white shows only when there is a point to set: before the first anchor, or when a
            // row is selected to edit. Once points exist and none is selected, the white point is fixed
            // by the anchors and only the trim adjusts the live picture -- so the trim takes the
            // slider's place, the same shape as the game-exposure source.
            const bool showPaperWhite = anchors.empty() || editingRow;

            if (showPaperWhite)
            {
                float pw = editingRow ? anchors[selectedAnchor].white
                                      : nr.whitePointScale.value_or_default();

                char lbl[48];
                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "Paper white");

                if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        nr.scanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        nr.whitePointScale = pw;
                }

                HelpMarker("Adjust the selected calibration point, or set the value for the next point.\nUse Anchor here to save the current lighting condition.");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = nr.scanTrim.value_or_default();

                if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    nr.scanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Reset##scantrim"))
                    nr.scanTrim = 1.0f;

                HelpMarker("Multiply the calibrated white point. Anchor here saves the adjusted value and resets this multiplier to 1.");
            }
        }
        else if (wpSource == 1)
        {
            const bool ofScan = false;

            float trim = ofScan ? nr.scanTrim.value_or_default()
                                : nr.whitePointTrim.value_or_default();

            if (ImGui::SliderFloat(ofScan ? "Trim (x the scan)" : "Trim (x the game's exposure)", &trim,
                                   0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (ofScan)
                    nr.scanTrim = std::clamp(trim, 0.25f, 4.0f);
                else
                    nr.whitePointTrim = std::clamp(trim, 0.25f, 4.0f);
            }

            ImGui::SameLine();

            // Deliberately always present rather than greyed at 1. The point of it is that the safe
            // value is one click away without having to know what the safe value is.
            if (ImGui::SmallButton("Reset##wptrim"))
            {
                if (ofScan)
                    nr.scanTrim = 1.0f;
                else
                    nr.whitePointTrim = 1.0f;
            }

            HelpMarker("Multiply the white point derived from game exposure. 1 = no adjustment.");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = nr.whitePointScale.value_or_default();

            if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                nr.whitePointScale = wpScale;

        HelpMarker("Brightness reference used to prepare HDR colour for NR. Higher values darken the model input; lower values brighten it.\nAdjust if NR loses detail or produces colour shifts.");
        }

        // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
        // belongs with the exposure controls it works alongside.
        float maxRatio = nr.maxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, unlockPasses ? (float) MaxPassCount : 8.0f, "%.1fx"))
            nr.maxRatio = maxRatio;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##guard"))
            nr.maxRatio = 2.0f;

        HelpMarker("Limit how much NR can brighten or darken a pixel. Lower values restrict highlight changes; higher values allow more.");

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. It used to sit under Inspect, a whole section away from the slider it
        // reads, which left "Anchor here" looking like a control for something else entirely.
        {
            // No checkbox here any more.
            //
            // The dropdown above says whether the scan is the white point's source, and that is
            // the only reason anybody using this would want it running. A second control could
            // only agree with the dropdown or contradict it, and both were on offer: it began as
            // a redundant question and became a way to switch off the thing the chosen source
            // depended on.
            //
            // The ini key survives as a developer override for the one case a user has no reason
            // to want -- running the scan in a game that supplies a REAL exposure, so the log can
            // compare the two. That is validation, and validation does not need a widget.
            //
            // Worth keeping written down, since the panel no longer says it: the scan matches
            // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
            // real exposure, so the right answer sat visible beside it -- the best candidate was
            // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
            // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
            // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = nr.scanMeter.value_or_default();

                if (nr.whitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox("Show exposure meter", &meter))
                    nr.scanMeter = meter;

                HelpMarker("Show the scanned exposure value and a colour indicator. Display only; does not change the image.");

            // Shown when the scan is actually running, whichever way it got switched on.
            if (DlssNr::ExposureScan::Scanning())
            {
                // Anchoring: one press, then it never needs touching again.
                //
                // The absolute white point cannot come out of a buffer whose units are unknown.
                // Every value AFTER the first can: only the ratio against the anchor is used, so
                // whatever the number means, it cancels. That is why this is a button and not a
                // measurement -- the one thing a person can supply that no amount of cleverness
                // can is "this looks right to me".
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                const bool isSource = nr.whitePointSource.value_or_default() == 2;

                // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                // replace. One row is the old single-anchor ratio law; add a second in different
                // light and the white point is interpolated between the points, so it holds across
                // the whole range instead of only near one anchor. Greyed unless the scan is the
                // chosen source and it currently has a value to capture.
                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button("Anchor here"))
                {
                    // What to capture. Before the first point, the paper white above (an absolute value
                    // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                    // picture is showing right now -- the interpolated value times the Trim the user just
                    // dialed in -- so a second point in different light captures the trimmed look, not a
                    // frozen paper white (which would make two equal whites and a flat, non-tracking
                    // curve). The trim is reset afterwards: the new point, which the picture now passes
                    // through exactly, must not be multiplied by it a second time.
                    const float captureWhite =
                        anchors.empty()
                            ? std::max(0.01f, nr.whitePointScale.value_or_default())
                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                  live, nr.scanInverted.value_or_default(),
                                                  nr.scanTrim.value_or_default()));

                    if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                    {
                        nr.scanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        nr.scanTrim = 1.0f;
                        selectedAnchor = -1;
                    }
                }

                ImGui::EndDisabled();

                HelpMarker("Save the current exposure and white point as a calibration point.\nAdjust Paper white for the first point, then Trim for additional lighting conditions. Up to 8 points.");

                if (!isSource)
                    ImGui::TextDisabled("Scanned exposure is not the selected white point source.");

                if (!anchors.empty())
                {
                    // The row nearest the live scan value (in log space) is the one driving the
                    // picture right now; mark it so the user can see which calibration is in effect.
                    int active = 0;
                    float bestDist = 1e30f;
                    const float liveLog = std::log(std::max(live, 1e-6f));

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        const float d =
                            std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            active = (int) i;
                        }
                    }

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        ImGui::PushID((int) i);

                        // Delete first, so its click is never swallowed by the row-wide Selectable.
                        if (ImGui::SmallButton("x"))
                        {
                            DlssNr::ExposureScan::AnchorRemove((int) i);
                            nr.scanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            if (selectedAnchor == (int) i)
                                selectedAnchor = -1;
                            else if (selectedAnchor > (int) i)
                                --selectedAnchor;
                            ImGui::PopID();
                            continue;
                        }

                        ImGui::SameLine();

                        const bool sel = (int) i == selectedAnchor;
                        char row[96];
                        snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                                 anchors[i].white, sel ? "   [editing]" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("Select a row to edit it; select it again"
                                        " to deselect. > marks the active point.");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = nr.scanInverted.value_or_default();
                    if (ImGui::Checkbox("Invert exposure tracking", &inverted))
                        nr.scanInverted = inverted;

                    HelpMarker("Reverse how scanned exposure changes the white point. Only needed with one calibration point.");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("Advanced"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                      ? why
                                                      : "No exposure candidates found.");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                       : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1,
                                               c.shape.c_str(), c.latest, c.lowest, c.highest,
                                               c.moves ? "MOVES" : "flat so far");
                        }

                        ImGui::TextDisabled("Move between bright and dark areas to check exposure tracking.");
                        ImGui::TextDisabled("A value that only increases may be a counter.");
                    }

                    ImGui::TreePop();
                }
            }
        }


        }

        ImGui::SeparatorText("Compare");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = nr.holdFrame.value_or_default();
        if (ImGui::Checkbox("Hold frame", &held))
            nr.holdFrame = held;

        HelpMarker("Freeze NR's input to compare its settings. The game's HUD and later effects may keep updating.\nDoes not re-run SR/RR or show changes to their settings. Turn off to resume.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) nr.compare.value_or_default();
        if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            nr.compare = (uint32_t) compare;

        HelpMarker("Compare the original and NR result. Side by side fits both images; Wipe divides one full-size image.");

        if (compare != 0)
        {
            bool swap = nr.compareSwap.value_or_default();
            if (ImGui::Checkbox("Swap sides", &swap))
                nr.compareSwap = swap;
            HelpMarker("Swap the original and NR sides.");

            bool tags = nr.compareTags.value_or_default();
            if (ImGui::Checkbox("Label the sides", &tags))
                nr.compareTags = tags;

            HelpMarker("Display labels identifying the original and NR sides.");

            if (tags)
            {
                float tagScale = nr.tagScale.value_or_default();
                if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    nr.tagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

        }

        if (compare == 1)
        {
            float zoom = nr.compareZoom.value_or_default();
            if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
                nr.compareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("Side-by-side zoom: 1 = fit the whole image, 2 = fill each half by cropping the sides.");
        }

        if (compare == 2)
        {
            float split = nr.compareSplit.value_or_default();
            if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
                nr.compareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Position of the comparison boundary. Swap sides reverses which image appears on each side.");
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) nr.debugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            nr.debugView = (uint32_t) debugView;

        HelpMarker("Show the model input, raw output, or a 20x amplified difference. Grey in Difference means no change.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr
