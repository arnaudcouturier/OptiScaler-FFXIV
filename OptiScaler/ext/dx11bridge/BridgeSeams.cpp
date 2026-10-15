#include "pch.h"

#include "ext/Seams.h"

#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/dx11bridge/BridgeState.h"
#include "ext/dx11bridge/ColorScaler_Dx12.h"
#include "ext/game/Game.h"
#include "ext/game/GameInterfaces.h"

#include <State.h>
#include <upscalers/IFeature_Dx11wDx12.h>

// C4: ext::dx11bridge::PrepareUpscalerColor (docs/SEAMS-catalog.md "dx11bridge"), the oracle's
// IFeature_Dx11wDx12::PrepareForcedQualityColor ported through the fork's seams.

namespace
{
// IFeature_Dx11wDx12::ResourceBarrier's body, copied here rather than widening ext::Access: it is a protected
// member (not static), names no private state, and is a pure D3D12 API wrapper -- nothing worth reaching through
// the friend grant for.
void Barrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES beforeState,
            D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}
} // namespace

namespace ext::dx11bridge
{
ID3D12Resource* PrepareUpscalerColor(IFeature_Dx11wDx12& feature, ID3D12GraphicsCommandList* cmd,
                                     NVSDK_NGX_Parameter* params, ID3D12Resource* color, unsigned int& guideSrcWidth,
                                     unsigned int& guideSrcHeight)
{
    guideSrcWidth = 0;
    guideSrcHeight = 0;

    auto* quality = game::Current().quality;
    auto* geometry = game::Current().geometry;

    // Oracle gate: FfxivNativeQuality::Available() || cmd/params/color null || dx12Feature null || quirk off ->
    // no-op, original colour unchanged. `dx12Feature == nullptr` has no separate check here: RenderWidth()/
    // RenderHeight() below already answer 0 through IFeature_Dx11wDx12's own CallFeature when there is no inner
    // feature, and ColorIsDisplayAllocation's renderWidth==0 check already treats that as a no-op -- same outcome,
    // reached through the public accessor instead of naming the private dx12Feature member.
    if ((quality != nullptr && quality->Live()) || cmd == nullptr || params == nullptr || color == nullptr ||
        geometry == nullptr || !(State::Instance().gameQuirks & GameQuirk::ScaleDisplayColorForForcedQuality))
        return color;

    const auto desc = color->GetDesc();
    const auto renderWidth = feature.RenderWidth();
    const auto renderHeight = feature.RenderHeight();

    unsigned int subrectWidth = 0, subrectHeight = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &subrectHeight);

    // Folds the oracle's two remaining checks (render already equals the allocation -> nothing to scale; the
    // game's subrect doesn't describe the full allocation -> don't second-guess it) into one interface call --
    // see ext/upscaler/RenderGeometry.h's ColorIsDisplayAllocationValue for the combined math and the
    // RenderSizeFromWidthHeight gate coupling.
    if (!geometry->ColorIsDisplayAllocation(renderWidth, renderHeight, desc.Width, desc.Height, subrectWidth,
                                            subrectHeight))
        return color;

    auto& state = Access::Bridge::Ext(feature);
    if (state.colorScaler == nullptr)
        state.colorScaler = std::make_unique<ColorScaler_Dx12>("Forced quality color input", Access::Dx11On12Device(),
                                                                false, Scaler::Lanczos3);

    if (!state.colorScaler->CreateBufferResource(Access::Dx11On12Device(), color, renderWidth, renderHeight,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        LOG_ERROR("Forced quality color input: could not create the {}x{} staging texture", renderWidth,
                  renderHeight);
        return nullptr;
    }

    state.colorScaler->SetBufferState(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool scaled = state.colorScaler->Dispatch(cmd, color, state.colorScaler->Buffer());
    Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    if (!scaled)
    {
        LOG_ERROR("Forced quality color input: scaling {}x{} to {}x{} failed", desc.Width, desc.Height, renderWidth,
                  renderHeight);
        return nullptr;
    }

    state.colorScaler->SetBufferState(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (!state.reportedForcedQualityColorScale)
    {
        state.reportedForcedQualityColorScale = true;
        LOG_INFO("Forced quality color input: scaling the complete {}x{} frame to {}x{} before the upscaler; the "
                 "game's render subrect describes the allocation, not a crop",
                 desc.Width, desc.Height, renderWidth, renderHeight);
    }

    // Preserve the ORIGINAL (unscaled) extent for NR before the host overwrites the subrect with the render size.
    guideSrcWidth = (unsigned int) desc.Width;
    guideSrcHeight = desc.Height;

    return state.colorScaler->Buffer();
}
} // namespace ext::dx11bridge
