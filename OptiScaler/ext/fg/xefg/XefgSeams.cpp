#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fg/Dx12Access.h"
#include "ext/fg/xefg/XeFGCamera.h"
#include "ext/fg/xefg/XeFGPacing.h"
#include "ext/fg/xefg/XeFGUnlock.h"
#include "ext/fg/xefg/XefgState.h"
#include "ext/game/Game.h"

#include <framegen/IFGFeature_Dx12.h>
#include <menu/menu_common.h>
#include <proxies/XeFG_Proxy.h>

#include <algorithm>
#include <cmath>

// D3: the XeFG seams (XeFG_Dx12::Dispatch, XeFG_Proxy.h, the XeFG menu card). Provider unlock and pacing are
// XeFGUnlock.h / XeFGPacing.h; the Dispatch logic below is the reference build's, moved out of XeFG_Dx12.cpp.

namespace ext::fg
{
void OnXefgProviderLoaded(HMODULE provider)
{
    // Patch the provider in memory before anything calls into it - the MFG gate is evaluated during swapchain init,
    // so it has to happen here. A failure just leaves the provider as Intel shipped it.
    XeFGUnlock::Apply(provider);
}

int XefgSelectFactor(IFGFeature_Dx12* fg, int current, int maxInterpolation, XefgFactorInfo& info)
{
    auto& cfg = *Config::Instance();
    auto& fgCfg = ext::Cfg().fg;
    auto& dx12Ext = Access::FgDx12::Ext(*fg);

    if (!dx12Ext.xefg)
        dx12Ext.xefg = std::make_unique<XefgState>();
    auto& xefg = *dx12Ext.xefg;

    const int supported = std::max(1, maxInterpolation);
    int requested = std::clamp(cfg.FGInterpolationCount(cfg.FGXeFGInterpolationCount.value_or_default()), 1, supported);
    const bool dynamic = fgCfg.xefgDynamic.value_or_default();
    float target = fgCfg.xefgDynamicTarget.value_or_default();
    if (!std::isfinite(target))
        target = 138.0f;
    target = std::clamp(target, 30.0f, 500.0f);
    const float limit = cfg.FramerateLimit.value_or_default();
    if (std::isfinite(limit) && limit > 0)
        target = std::min(target, limit);
    if (dynamic != xefg.wasEnabled || target != xefg.lastTarget)
    {
        xefg.dynamic.Reset();
        xefg.wasEnabled = dynamic;
        xefg.lastTarget = target;
    }
    if (dynamic)
        requested = xefg.dynamic.Select(State::Instance().lastFGFrameTime, target, GetTickCount64(), current + 1,
                                        std::min(4, supported + 1)) -
                    1;

    info.dynamic = dynamic;
    info.target = target;
    return requested;
}

double XefgFrameTime(double inputFrameTime)
{
    // The FFXIV bridge DOES supply the input frame time, but it is the present interval and can include provider
    // pacing. Prefer fresh pacing-adjusted telemetry for a game module that owns its own markers (FFXIV); preserve
    // genuine input timing in other integrations.
    auto frameRenderTime = inputFrameTime;
    const auto pacedTime = XeFGPacing::RenderTimeMs();
    if (pacedTime > 0.0 && (game::Current().fgMarkers != nullptr || !(frameRenderTime > 0.0)))
        frameRenderTime = pacedTime;
    if (!(frameRenderTime > 0.0) || !std::isfinite(frameRenderTime))
        frameRenderTime = State::Instance().lastFGFrameTime;
    return frameRenderTime;
}

void XefgNoteFrameTime(float& frameRenderTime, bool& resetHistory)
{
    // Rebuild/loading pauses are discontinuities, not a frame-time target.
    // Zero is the existing provider-estimated timing mode used by this integration.
    if (!std::isfinite(frameRenderTime) || frameRenderTime < 0.0f || frameRenderTime > 250.0f)
    {
        LOG_INFO("XeFG timing discontinuity: discarding {} ms frame-time sample", frameRenderTime);
        frameRenderTime = 0.0f;
        if (!Config::Instance()->FGSkipReset.value_or_default())
            resetHistory = true;
    }

    XeFGPacing::NoteFedFrameTime(frameRenderTime);
}

bool XefgBuildCamera(float* viewMatrix, const float* position, const float* right, const float* up,
                     const float* forward, uint64_t frameCount, int guideAge)
{
    const bool havePose = XeFGCamera::BuildView(viewMatrix, position, right, up, forward);
    if (!havePose && frameCount % 240 == 0)
        LOG_INFO("XeFG camera: stationary view fallback; pose unavailable, scene age={}", guideAge);
    return havePose;
}
} // namespace ext::fg

namespace ext::menu
{
void XeFgExtras(IFGFeature* fgOutput)
{
    auto& fgCfg = ext::Cfg().fg;

    if (fgOutput->GetMaxInterpolationCount() > 1)
    {
        // Fork: Dynamic MFG. The slider above still picks Off/Default; while this is on the 2X-4X multiplier is
        // chosen automatically from the real frame timing.
        ImGui::SameLine(0.0f, 16.0f);
        const int currentCount = (int) fgOutput->GetInterpolatedFrameCount();
        bool dynamicXe = fgCfg.xefgDynamic.value_or_default();
        ImGui::BeginDisabled(XeFGProxy::SetNumInterpolatedFrames() == nullptr);
        if (ImGui::Checkbox("Dynamic XeMFG (max 4X)", &dynamicXe))
            fgCfg.xefgDynamic = dynamicXe;
        ImGui::EndDisabled();
        float targetXe = fgCfg.xefgDynamicTarget.value_or_default();
        ImGui::BeginDisabled(!dynamicXe);
        if (ImGui::SliderFloat("Target FPS (XeFG dynamic)##XeDynamic", &targetXe, 30.0f, 240.0f, "%.0f"))
            fgCfg.xefgDynamicTarget = targetXe;
        if (currentCount >= 1)
            ImGui::Text("Current XeMFG: %dX (selected)", currentCount + 1);
        Tooltip("Automatically selects 2X-4X using real-frame timing.\n"
                "Respects the configured FPS limit. Changes are spaced out to avoid oscillation.\n"
                "At a frame cap, limited timing can prevent detecting headroom to lower the factor.");
        ImGui::EndDisabled();
    }

    // Takes effect once, at XeFG init: the pacing hook rewrites the present
    // thunk, so this cannot be toggled while the game is running.
    ImGui::SameLine(0.0f, 16.0f);
    bool fgExtraPacing = fgCfg.xefgExtraPacing.value_or_default();
    if (ImGui::Checkbox("Extra Pacing", &fgExtraPacing))
        fgCfg.xefgExtraPacing = fgExtraPacing;

    Tooltip("Pace every generated frame above 2X\n\n"
            "Without it the provider hands the whole burst of generated\n"
            "frames over at once and only spaces out the last one, which\n"
            "reads as judder at 4X and above\n\n"
            "NEEDS GAME RESTART TO BE ACTIVE!");

    // Fork: the two provider-unlock keys of the [XeFG] section. Both are read once, when the provider is loaded and the
    // swapchain is created (XeFGUnlock::Apply), so a change is stored for the next start.
    bool unlockMfg = fgCfg.xefgUnlock.value_or_default();
    if (ImGui::Checkbox("Unlock multi frame generation (restart)", &unlockMfg))
        fgCfg.xefgUnlock = unlockMfg;
    Tooltip("Unlocks 3X and higher multi frame generation in Intel's XeFG provider: OptiScaler patches the loaded provider\n"
            "image in memory (the file on disk is never changed). Without it the provider reports a single generated\n"
            "frame (2X) on other makers' GPUs. Turn it off if the provider misbehaves or its build is not recognised.\n\n"
            "The patches are applied when the provider is loaded: applies after a restart.");

    int maxMultiplier = std::clamp(fgCfg.xefgMaxInterpolatedFrames.value_or_default(), 1, ext::kXeFgMaxInterpolations) + 1;
    ImGui::BeginDisabled(!unlockMfg);
    if (ImGui::SliderInt("Max XeFG multiplier (restart)", &maxMultiplier, 2, ext::kXeFgMaxInterpolations + 1, "%dX"))
        fgCfg.xefgMaxInterpolatedFrames = maxMultiplier - 1;
    Tooltip("The highest multiplier the XeFG menu offers, 2X to 8X (default 8X). If a high multiplier turns out to be\n"
            "unstable, try 4X first.\n\n"
            "The same number is handed to the provider when the swapchain is created: applies after a restart.");
    ImGui::EndDisabled();
}
} // namespace ext::menu
