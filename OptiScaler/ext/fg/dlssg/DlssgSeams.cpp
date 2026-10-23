#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/fg/Dx12Access.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/dlssg/PresetMenuText.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"

#include <framegen/IFGFeature_Dx12.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_base.h>
#include <proxies/Streamline_Proxy.h>

#include <magic_enum.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

// D2: the DLSS-G option seams of DLSSG_Common::SetOptions and the observed dynamic-MFG multiplier. These were the
// DLSSG_Common virtuals AdjustDlssgOptions / AfterDlssgSetOptions and DLSSG_Dx12's overrides (SEAMS 5: free functions
// instead of virtuals). The Dx12 state lives in Dx12Ext; DLSSG_Vk has none, so the Dx12-only parts are no-ops there.

namespace
{
// DLSS-G cannot present next to the Vulkan menu overlay (a Vulkan swapchain, or the Vulkan overlay on a D3D12 game under
// vkd3d-proton): turn it off while the menu is open, keeping its resources so it resumes without a restart. Same rule
// as the [UP] one in Streamline_Hooks.cpp, which only sees the game's own slDLSSGSetOptions calls (this is OptiFG's).
void ApplyMenuDlssgInterlock(sl::DLSSGOptions& options)
{
    auto& state = State::Instance();
    if (state.swapchainApi != API::Vulkan && !state.menuOverlayIsVulkan)
        return;

    if (MenuOverlayBase::IsVisible())
    {
        options.mode = sl::DLSSGMode::eOff;
        options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
        ReflexHooks::setDlssgFrameCount(0);
    }
}

// Separate UI path: without it DLSS-G treats tagged HUD-less / UI images as hints only and still interpolates the UI.
// nvngx_dlssg reads it at feature *creation* and a later change makes Streamline recreate the feature, so the final value must already
// be in the very first SetOptions, long before the first capture. It follows [HudLess] Mode == recomposition (and [DLSSG]
// UIRecomposition, unset = on), not "an image was delivered", and the render preset: A and C take the recomposition away inside NVIDIA's
// runtime (verified in 310.9.16 ApplyRenderPreset), so they are not asked for it. ONE pure rule (UiRecompositionRule.h, via
// CurrentUiRecompositionDecision) decides from the active request on every call, the first included; the runtime's reply to a preset
// never takes part (it only exists after the creation, and deciding from it created the feature with the flag on and withdrew it a
// moment later). The hud-less format is the last captured one, else the bridge's buffer format (S10, ext/hudless).
void AdjustUiRecomposition(sl::DLSSGOptions& options)
{
    const auto recomposition = ext::hudless::QueryRecomposition();
    const auto decision = ext::fg::preset::CurrentUiRecompositionDecision(recomposition.enabled, true);
    const bool dropped = decision.droppedByPreset;

    if (decision.requested)
    {
        options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;

        if (recomposition.format != 0)
            options.hudLessBufferFormat = recomposition.format;

        options.uiBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM); // UiImage::Build's format
    }

    static int lastLogged = -1;
    static int lastEnabled = -1;
    const int effective = decision.requested ? 1 : 0;
    if (lastLogged != effective)
    {
        // "only applies after a game restart" is true for the config / hud-less mode changing; a drop by the preset is picked up at the
        // next feature creation, which the live preset switch itself causes.
        const char* restart = lastEnabled != -1 && lastEnabled != (int) recomposition.enabled
                                  ? " -- changed after start, only applies after a game restart"
                                  : "";
        LOG_INFO("DLSSG UI recomposition: {} (HudLess mode {}, hudless format {} from {}, ui format {}){}{}{}{}",
                 effective != 0, ext::hudless::ModeName(ext::hudless::CurrentMode()), options.hudLessBufferFormat,
                 recomposition.formatSource, options.uiBufferFormat, dropped ? " -- not requested, dropped by the DLSS-G render preset (" : "",
                 dropped ? ext::fg::preset::Name(decision.preset) : "", dropped ? ")" : "", restart);
        lastLogged = effective;
    }
    // One sentence per episode (at boot, and when a live switch changes which preset drops the recomposition), never per frame.
    static int lastDroppedBy = -1;
    const int droppedBy = dropped ? decision.preset : -1;
    if (droppedBy != lastDroppedBy)
    {
        if (droppedBy >= 0)
            LOG_INFO("{}", ext::fg::preset::UiRecompositionDroppedSentence(droppedBy));
        lastDroppedBy = droppedBy;
    }
    lastEnabled = (int) recomposition.enabled;
}

IFGFeature_Dx12* AsDx12(IFGFeature* fg) { return dynamic_cast<IFGFeature_Dx12*>(fg); }

// E3: presentation telemetry since the last activation (diagnostics only; the reference build's file-local static too,
// SetOptions and Activate run on the same serialized thread in practice).
struct PresentStats
{
    ULONGLONG lastLog = 0;
    uint64_t frames = 0, presented = 0;
};
PresentStats presentStats;
} // namespace

namespace ext::fg
{
void AdjustDlssgOptions(IFGFeature* fg, sl::DLSSGOptions& options)
{
    // dynamic MFG on RTX 40 (may change this frame's count), then the ForceDMFG / Ada-ceiling guard on the result
    AutoMfgAdjustOptions(fg, options);

    if (AsDx12(fg) != nullptr)
        AdjustUiRecomposition(options);

    ApplyMenuDlssgInterlock(options);
}

void AfterDlssgSetOptions(IFGFeature* fg, const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options,
                          sl::Result dlssgSetOptionsResult)
{
    auto* dx12 = AsDx12(fg);
    if (dx12 == nullptr)
        return;

    auto& dx12Ext = Access::FgDx12::Ext(*dx12);
    dx12Ext.setOptionsOk = dlssgSetOptionsResult == sl::Result::eOk;
    dx12Ext.optionsMode = static_cast<uint32_t>(options.mode);

    // Query once per real dispatch, on the existing serialized SL thread. Do not query
    // from ImGui: GetState is not thread safe and consumes presentation telemetry.
    // This is an observed output multiplier, not a prediction of the driver's next factor.
    sl::DLSSGState runtimeState {};
    const auto queryResult = StreamlineProxy::DLSSGGetState()(viewport, runtimeState, nullptr);
    const auto sampleTime = GetTickCount64();
    const auto mode = static_cast<int>(options.mode);
    const auto count = runtimeState.numFramesActuallyPresented;
    AutoMfgObserve(queryResult == sl::Result::eOk, runtimeState);
    if (dx12Ext.sampleMode == mode && options.mode == sl::DLSSGMode::eDynamic &&
        dlssgSetOptionsResult == sl::Result::eOk && queryResult == sl::Result::eOk &&
        static_cast<unsigned>(runtimeState.status) == 0 && count >= 1 &&
        count <= static_cast<unsigned>(fg->GetMaxInterpolationCount() + 1))
        dx12Ext.observedMultiplier.store((sampleTime << 8) | count, std::memory_order_relaxed);
    else
        dx12Ext.observedMultiplier.store(0, std::memory_order_relaxed);
    dx12Ext.sampleMode = queryResult == sl::Result::eOk ? mode : -1;

    // E3: first dispatch after each activation, then at most every 5 s. The first sample covers the time since the
    // previous GetState call, so it may be stale; the running average is presented frames per game frame (dispatch).
    if (queryResult == sl::Result::eOk)
    {
        ++presentStats.frames;
        presentStats.presented += count;
    }
    if (presentStats.lastLog == 0 || sampleTime - presentStats.lastLog >= 5000)
    {
        presentStats.lastLog = sampleTime;
        LOG_INFO("DLSSG presentation: query={} status={:#x} numFramesActuallyPresented={} requested mode={} "
                 "numFramesToGenerate={} (max {}), avg presented/game frame {:.2f} over {} frames since activation",
                 magic_enum::enum_name(queryResult), static_cast<unsigned>(runtimeState.status), count, mode,
                 options.numFramesToGenerate, runtimeState.numFramesToGenerateMax,
                 presentStats.frames ? double(presentStats.presented) / presentStats.frames : 0.0,
                 presentStats.frames);
    }
}

void AdjustReflexOptions(const sl::DLSSGOptions& options, sl::ReflexOptions& reflexConst)
{
    // Let Streamline see the same output cap as the user, so dynamic MFG can account for it.
    if (options.mode == sl::DLSSGMode::eDynamic)
    {
        const float limit = Config::Instance()->FramerateLimit.value_or_default();
        if (limit > 0.0f && std::isfinite(limit))
            reflexConst.frameLimitUs = static_cast<uint32_t>(std::clamp(1000000.0 / limit, 1.0, 4294967295.0));
    }
}

void ResetDlssgPresentStats() { presentStats = {}; }

unsigned ObservedMultiplier(IFGFeature* fg)
{
    auto* dx12 = AsDx12(fg);
    if (dx12 == nullptr)
        return 0;

    const auto sample = Access::FgDx12::Ext(*dx12).observedMultiplier.load(std::memory_order_relaxed);
    return sample && GetTickCount64() - (sample >> 8) < 3000 ? unsigned(sample & 0xff) : 0;
}
} // namespace ext::fg
