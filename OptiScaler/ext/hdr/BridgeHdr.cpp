#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/hdr/Hdr10.h"

#include <State.h>

// C8: OptiHDR in the DX11 -> DX12 bridge swapchain (with_dx12/dx11_with_dx12_sc.cpp). The inline blocks of the reference
// build, moved behind seams; logic and results unchanged.
namespace ext::dx11bridge
{
void ConfigureOutput(Dx11wDx12SC& sc)
{
    auto* fgChain = Access::Bridge::FgSwapChain(sc);
    DXGI_SWAP_CHAIN_DESC1 hdrDesc {};

    if (ext::Cfg().hdr.ffxivHdr.value_or_default() && fgChain && SUCCEEDED(fgChain->GetDesc1(&hdrDesc)) &&
        hdrDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        const bool hdrOutput = hdr::Configure(fgChain);
        Access::Bridge::Ext(sc).hdrOutput = hdrOutput;

        if (!hdrOutput)
            fgChain->ResizeBuffers(0, 0, 0, Access::Bridge::BufferFormat(sc), hdrDesc.Flags);
    }
}

void OnDestroy(Dx11wDx12SC& sc)
{
    if (Access::Bridge::HdrOutput(sc))
        hdr::Deactivate();
}

DXGI_FORMAT OutputFormat(Dx11wDx12SC& sc, DXGI_FORMAT requested)
{
    return Access::Bridge::HdrOutput(sc) ? DXGI_FORMAT_R10G10B10A2_UNORM : requested;
}

HRESULT OnResizeDone(Dx11wDx12SC& sc)
{
    if (Access::Bridge::HdrOutput(sc) && !hdr::Configure(Access::Bridge::FgSwapChain(sc)))
        return DXGI_ERROR_UNSUPPORTED;

    return S_OK;
}

bool SetColorSpace(Dx11wDx12SC& sc, DXGI_COLOR_SPACE_TYPE colorSpace, HRESULT& out)
{
    if (Access::Bridge::HdrOutput(sc))
    {
        out = hdr::Configure(Access::Bridge::FgSwapChain(sc)) ? S_OK : DXGI_ERROR_UNSUPPORTED;
        return true;
    }

    // The reference build wrote the fork's write-only `isHdrActive` here; nightly's fields are the truth now, so the
    // game's own HDR colour spaces are recorded there (what UpdateOutputColorSpace does for a wrapped swapchain).
    hdr::NoteGameColorSpace(colorSpace);
    return false;
}

bool ConvertCopySource(Dx11wDx12SC& sc, UINT slot, ID3D12Resource*& copySource, D3D12_RESOURCE_STATES& before)
{
    if (!Access::Bridge::HdrOutput(sc))
        return true;

    copySource = hdr::Convert(Access::Bridge::Dx12Device(sc), Access::Bridge::CopyList(sc, slot), copySource, before);
    if (!copySource)
        return false;

    before = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return true;
}
} // namespace ext::dx11bridge
