#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/hdr/Hdr10.h"
#include "ext/hdr/FfxivHdrPreview.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"
#include "ext/game/Game.h"

#include <framegen/IFGFeature_Dx12.h>

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
    auto& bridge = Access::Bridge::Ext(sc);
    bridge.screenshotSdrSource = nullptr;
    if (!Access::Bridge::HdrOutput(sc))
        return true;

    auto* device = Access::Bridge::Dx12Device(sc);
    auto* list = Access::Bridge::CopyList(sc, slot);
    bridge.screenshotSdrSource = copySource;
    bridge.screenshotSdrState = before;

    hdr::SceneInput scene;
    const auto sourceDesc = copySource->GetDesc();
    if (auto* provider = game::Current().hdr)
        scene = provider->Open(device, (UINT) sourceDesc.Width, sourceDesc.Height, true);

    copySource = hdr::Convert(device, list, copySource, before, scene);

    auto* fg = Access::Bridge::Fg(sc);
    if (copySource && ext::Cfg().hdr.mode.value_or_default() == 1 && fg && fg->IsActive())
    {
        // The original HUD-less copy was recorded on the upscaler list.
        // This queue now waits on DX11 (and its upscaler interop completion),
        // so both colour conversions can use the same immutable scene pair.
        Microsoft::WRL::ComPtr<ID3D12Resource> nativeHudless;
        Dx12Resource hudless {};
        const auto frame = fg->GetIndex();
        {
            auto input = fg->GetResource(FG_ResourceType::HudlessColor, frame);
            if (input && !input->hdrEncoded)
            {
                hudless = *input.resource;
                nativeHudless = input->GetResource();
            }
        }

        if (nativeHudless)
        {
            auto* hdrHudless = hdr::Convert(device, list, nativeHudless.Get(), hudless.state, scene);
            if (hdrHudless)
            {
                hudless.resource = hdrHudless;
                hudless.copy = nullptr;
                hudless.hdrEncoded = true;
                hudless.cmdList = list;
                hudless.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                hudless.validity = FG_ResourceValidity::ValidNow;
                hudless.frameIndex = frame;
                fg->SetResource(&hudless);
            }
        }
    }

    if (!copySource)
        return false;

    before = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return true;
}

void PrepareScreenshot(Dx11wDx12SC& sc, UINT slot, ID3D12Resource* hdrSource)
{
    auto& bridge = Access::Bridge::Ext(sc);
    auto* sdrSource = bridge.screenshotSdrSource;
    bridge.screenshotSdrSource = nullptr;
    if (!sdrSource || !ext::hdr::screenshot::Pending())
        return;
    // F4: with the menu open an HDR PNG is taken after the overlay drew the menu (OverlayScope), so this request is left alone.
    if (ext::hdr::screenshot::MenuCaptureWanted())
        return;

    auto* device = Access::Bridge::Dx12Device(sc);
    auto* list = Access::Bridge::CopyList(sc, slot);
    ext::hdr::screenshot::Prepare(Access::Bridge::Window(sc), device, list, hdrSource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  sdrSource, bridge.screenshotSdrState);
}

PresentScope::~PresentScope()
{
    if (auto* scene = game::Current().hdr)
        scene->EndFrame();
}

void BeforeSharedCopy(Dx11wDx12SC& sc)
{
    if (Access::Bridge::HdrOutput(sc) && game::Current().hdr)
        hdr::FfxivHdrPreview::SealFrame();
}
} // namespace ext::dx11bridge
