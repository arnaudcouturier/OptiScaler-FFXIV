#include "pch.h"

#include "ext/fg/Dx12Access.h"
#include "ext/fg/xefg/XeFGHdr.h"
#include "ext/core/ExtConfig.h"
#include "ext/hdr/HdrFg.h"
#include "ext/hdr/Hdr10.h"

#include <State.h>
#include <framegen/IFGFeature.h>
#include <proxies/Streamline_Proxy.h>

// C8/C10: OptiHDR in the DLSS-G / XeFG input path. The blocks are the reference build's inline SetResource code of
// DLSSG_Dx12.cpp and XeFG_Dx12.cpp, moved behind seams (SEAMS-catalog `fg`); log messages unchanged.
namespace ext::hdr
{
namespace
{
// Mode 1 (RenoDX scene HDR): the raw HUD-less colour is held and converted after the DX11 fence
bool SceneRawInput(const Dx12Resource& input)
{
    return Active() && ext::Cfg().hdr.mode.value_or_default() == 1 && input.type == FG_ResourceType::HudlessColor &&
           !input.hdrEncoded;
}
} // namespace

bool SetResourceHdr(IFGFeature_Dx12* fg, int fIndex, Dx12Resource*& input, Dx12Resource& storage, fg::Provider provider)
{
    if (!Active())
        return true;

    const bool sceneRaw = SceneRawInput(*input);

    if (provider == fg::Provider::DlssG)
    {
        if (sceneRaw && !input->cmdList)
            return false;

        // This first HDR path uses final/HUD-less differencing, not a separate SDR UI tag.
        if (input->type == FG_ResourceType::UIColor)
        {
            Access::FgDx12::NoUi(*fg, fIndex) = true;
            return false;
        }

        // Convert while the capture command list is still recording, not when the
        // deferred tag is revisited at Present (that recording may already be closed).
        if (!input->hdrEncoded && input->type == FG_ResourceType::HudlessColor &&
            input->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            if (!input->cmdList)
                return false;

            storage = *input;
            storage.validity = sceneRaw ? FG_ResourceValidity::ValidButMakeCopy : FG_ResourceValidity::ValidNow;
            input = &storage;
        }

        return true;
    }

    // XeFG: Use XeFG's backbuffer + HUD-less composition for OptiHDR. An SDR UI-only
    // texture cannot be alpha-blended in PQ space (and RGB10A2 loses alpha
    // precision). The native HUD is already in the converted backbuffer.
    if (sceneRaw)
    {
        if (!input->cmdList)
            return false;

        storage = *input;
        storage.validity = FG_ResourceValidity::ValidButMakeCopy;
        input = &storage;
    }

    return input->type != FG_ResourceType::UIColor;
}
} // namespace ext::hdr

namespace ext::fg
{
DXGI_FORMAT NormalizeFormat(DXGI_FORMAT format)
{
    return hdr::Active() ? DXGI_FORMAT_R10G10B10A2_UNORM : format;
}

bool SceneRawHudless(const Dx12Resource& input)
{
    return hdr::Active() && ext::Cfg().hdr.mode.value_or_default() == 1 &&
           input.type == FG_ResourceType::HudlessColor && !input.hdrEncoded;
}

bool DlssgTagHudless(IFGFeature_Dx12* fg, Dx12Resource& fResource, sl::Resource& resource, sl::ResourceTag& tag)
{
    if (!hdr::Active() || fResource.hdrEncoded || fResource.type != FG_ResourceType::HudlessColor)
        return true;

    auto* converted = hdr::Convert(Access::FgDx12::Device(*fg), fResource.cmdList, fResource.GetResource(), fResource.state);
    if (!converted)
    {
        LOG_ERROR("HDR10 hudless conversion failed; refusing mismatched FG input");
        fg->Deactivate();
        return false;
    }

    resource.native = converted;
    resource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Streamline copies this tag on the same recording before our packet can be reused.
    tag.lifecycle = sl::ResourceLifecycle::eOnlyValidNow;
    return true;
}

bool XefgPrepareHudless(IFGFeature_Dx12* fg, Dx12Resource& fResource, int fIndex,
                        _xefg_swapchain_d3d12_resource_data_t& resourceParam)
{
    if (!hdr::Active() || fResource.hdrEncoded || fResource.type != FG_ResourceType::HudlessColor)
        return true;

    if (!fResource.cmdList)
        fResource.cmdList = fg->GetUICommandList(fIndex);

    if (!XeFGHdr::PrepareHudless(Access::FgDx12::Device(*fg), fResource.cmdList, resourceParam))
    {
        LOG_ERROR("XeFG OptiHDR HUD-less conversion failed; disabling FG rather than tagging SDR with HDR10 output");
        fg->Deactivate();
        return false;
    }

    // The SDK owns a same-list copy; do not re-tag the SDR source at Present.
    fResource.validity = FG_ResourceValidity::ValidNow;
    static bool reported = false;
    if (!reported)
    {
        LOG_INFO("XeFG OptiHDR: RGB10A2 PQ HUD-less input, immediate SDK copy, HUD extracted from HDR backbuffer");
        reported = true;
    }

    return true;
}

void AdjustConstants(FG_Constants& constants)
{
    // Fork: OptiHDR output also needs the FG HDR flag
    if (hdr::Active())
        constants.flags |= FG_Flags::Hdr;
}
} // namespace ext::fg
