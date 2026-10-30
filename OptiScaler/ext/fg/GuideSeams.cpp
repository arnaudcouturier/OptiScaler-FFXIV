#include "pch.h"

#include "ext/Seams.h"
#include "ext/fg/Dx12Access.h"
#include "ext/fg/GuideState.h"
#include "ext/game/Game.h"
#include "ext/hdr/HdrFg.h"

#include <State.h>

#include <framegen/dlssg/DLSSG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <proxies/Streamline_Proxy.h>

// B5/C5 (FG side): the seams around the provider-neutral guide matcher (GuideMatcher.cpp) and the scene-paired history
// continuity of DLSS-G and XeFG. Everything here is inert unless a game module supplies ext::game::IFgMarkers: that
// pointer replaces the `gameExe == "ffxiv_dx11.exe"` string compares of the reference build.

namespace
{
using ext::Access;

IFGFeature_Dx12* AsDx12(IFGFeature* fg) { return dynamic_cast<IFGFeature_Dx12*>(fg); }

bool SceneBridge() { return ext::game::Current().fgMarkers != nullptr; }
} // namespace

namespace ext::fg
{
void OnUpscaleStart(IFGFeature* fg)
{
    auto* markers = game::Current().fgMarkers;
    if (markers == nullptr)
        return;

    if (auto* dx12 = AsDx12(fg))
        Access::FgDx12::SetPresentationGuideDelay(*dx12, markers->PresentationGuideDelay());

    markers->OnUpscale(fg->FrameCount());
}

bool SetResourcePre(IFGFeature_Dx12* fg, int fIndex, Dx12Resource*& input, Dx12Resource& matched, Provider provider)
{
    // C8: OptiHDR's raw scene capture / UI tag drop / PQ HUD-less capture (HudlessColor and UIColor inputs only, so
    // it never competes with the guide matcher for `matched`)
    if (!hdr::SetResourceHdr(fg, fIndex, input, matched, provider))
        return false;

    if (Access::FgDx12::GuideAge(*fg, fIndex) < 0 ||
        (input->type != FG_ResourceType::Depth && input->type != FG_ResourceType::Velocity))
        return true;

    matched = *input;
    if (!Access::FgDx12::MatchPresentationGuide(*fg, matched, fIndex))
        return false;

    input = &matched;
    return true;
}

void SetResourcePost(const Dx12Resource& input, Dx12Resource& frameResource)
{
    // C8: a PQ-encoded HUD-less input is the converted image itself; there is nothing to copy at Present
    frameResource.hdrEncoded = input.hdrEncoded;
    if (input.hdrEncoded)
        frameResource.copy = nullptr;

    // The matched snapshot owns its data: never keep a per-frame copy of the guides.
    if (input.type == FG_ResourceType::Depth || input.type == FG_ResourceType::Velocity)
        frameResource.copy = nullptr;
}

GuideInfo GuideAge(IFGFeature_Dx12* fg, int fIndex, uint64_t willDispatchFrame)
{
    const int age = Access::FgDx12::GuideAge(*fg, fIndex);
    return { age, willDispatchFrame - (age < 0 ? 0 : age) };
}

bool DispatchResetHistory(IFGFeature_Dx12* fg, const GuideInfo& guide, uint64_t frame, Provider provider)
{
    if (!SceneBridge())
        return false;

    auto& guides = Access::FgDx12::Guides(*fg);

    if (provider == Provider::XeFg)
        return guides.sceneHistory.NeedsReset(guide.sceneFrame);

    // The dispatch selector advances even when input readiness later prevents FG.
    // FFXIV must not reuse pre-pause/pre-gap history on the next accepted frame.
    auto& dx12Ext = Access::FgDx12::Ext(*fg);
    const bool discontinuity = dx12Ext.dlssgFrames.NeedsReset(frame) || guides.sceneHistory.NeedsReset(guide.sceneFrame);
    dx12Ext.forceConstantsReset = discontinuity;
    return discontinuity;
}

void PostDispatch(IFGFeature_Dx12* fg, const GuideInfo& guide, uint64_t frame, bool discontinuity, Provider provider)
{
    ext::present::FgDispatchEnd(); // PT: ends the span FgDispatchBegin opened in PresetSwitchTick
    if (!SceneBridge())
        return;

    auto& guides = Access::FgDx12::Guides(*fg);

    if (provider == Provider::XeFg)
    {
        guides.sceneHistory.Commit(guide.sceneFrame);
        return;
    }

    auto& dx12Ext = Access::FgDx12::Ext(*fg);
    if (dx12Ext.setOptionsOk && dx12Ext.optionsMode != static_cast<uint32_t>(sl::DLSSGMode::eOff))
    {
        if (discontinuity && !Config::Instance()->FGSkipReset.value_or_default())
            LOG_INFO("DLSSG history restart: accepted frame {} after {} (hadHistory={})", frame,
                     dx12Ext.dlssgFrames.last, dx12Ext.dlssgFrames.valid);
        dx12Ext.dlssgFrames.Commit(frame);
        guides.sceneHistory.Commit(guide.sceneFrame);
    }
    else
        dx12Ext.dlssgFrames.Invalidate();
}

void ReleaseGuides(IFGFeature_Dx12* fg) { Access::FgDx12::ReleaseGuides(*fg); }

void OnDeactivating(IFGFeature* fg)
{
    if (fg != nullptr && fg->IsActive())
    {
        // Once per real teardown, labeled by the provider being torn down (the
        // activation cast): this tap runs before the _isActive check in every
        // Deactivate body and on the preset-switch path, so no-op calls are
        // filtered here instead of recorded.
        if (dynamic_cast<XeFG_Dx12*>(fg) != nullptr)
            ext::fgrec::OnProviderOff("xefg"); // recorder: a teardown ends the generation
        else if (dynamic_cast<DLSSG_Dx12*>(fg) != nullptr)
            ext::fgrec::OnProviderOff("dlssg"); // recorder: a teardown ends the generation
    }
    auto* dx12 = AsDx12(fg);
    if (dx12 == nullptr)
        return;

    auto& dx12Ext = Access::FgDx12::Ext(*dx12);
    dx12Ext.dlssgFrames.Invalidate();
    Access::FgDx12::InvalidateGuides(*dx12);
    dx12Ext.observedMultiplier.store(0, std::memory_order_relaxed);
    dx12Ext.sampleMode = -1;
}

bool ForceConstantsReset(IFGFeature* fg)
{
    if (!SceneBridge())
        return false;

    auto* dx12 = AsDx12(fg);
    return dx12 != nullptr && Access::FgDx12::Ext(*dx12).forceConstantsReset;
}
} // namespace ext::fg
