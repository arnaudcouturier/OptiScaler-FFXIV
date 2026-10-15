#pragma once
// Seams of the HDR group (SEAMS-catalog group `hdr` and the HDR rows of `fg`, `dx11bridge`, `present`, `menu`),
// included by ext/Seams.h. State: ext/hdr/*. Everything is inert (one feature-bit test) until OptiHDR engaged.

#include "ext/ExtFeatures.h"

namespace ext::hdr
{
// Out-of-line half of Active(): nightly's `hdrOutputActive` (State), filled for the DX11 bridge by
// ext::dx11bridge::SetColorSpace / Hdr10 Configure. `isHdrActive` of the reference build does not exist any more.
bool OutputIsHdr();

// True while OptiHDR is engaged: the bridge negotiated PQ/BT.2020 on the FG swapchain (feature bit F::Hdr) and
// nightly's `hdrOutputActive` is true. Was Hdr10::Active().
inline bool Active() { return ext::Active(ext::F::Hdr) && OutputIsHdr(); }
} // namespace ext::hdr

// ---------------------------------------------------------------------------------------------------------------
// Presenter plan (C8; ext/hdr/PresentHdr.cpp). Hosts: hooks/DxgiFactory_Hooks.cpp and hooks/DxgiFactory_WrappedCalls.cpp,
// CreateSwapChain and CreateSwapChainForHwnd of each (4 sites): when OptiHDR is requested the DX11 game gets a DX12
// presenter swapchain (R10G10B10A2) even with frame generation off.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_2.h>
#include <windows.h>

namespace ext::present
{
struct PresenterPlan
{
    bool hdr = false;      // OptiHDR wants the DX12 presenter (Hdr10::Request): upstream ORs it into its own condition
    bool createFg = true;  // an FG provider is selected: false = plain DX12 presenter only (FG provider None)
};
// Policy query only (config + the window's monitor HDR state), side effect free.
PresenterPlan PlanDx12Presenter(HWND window, bool skipFgSwapchainCreation);
// The FG/presenter chain's descriptor: R10G10B10A2 when plan.hdr.
void PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC& desc, const PresenterPlan& plan);
void PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC1& desc, const PresenterPlan& plan);
// The FG swapchain could not be created and a plain DX12 swapchain is created instead: an info line when that is by
// design (no FG provider), the original warning otherwise.
void LogPlainPresenter(const PresenterPlan& plan, unsigned int fgResult);
} // namespace ext::present

// ---------------------------------------------------------------------------------------------------------------
// OptiHDR in the D3D11 bridge (C8; ext/hdr/BridgeHdr.cpp). Host: with_dx12/dx11_with_dx12_sc.cpp. State:
// ext::dx11bridge::BridgeExt (`Dx11wDx12SC::_ext`).
class Dx11wDx12SC;
struct ID3D12Resource;

namespace ext::dx11bridge
{
// ctor, right after the cached swapchain desc was refreshed: when HDR.FfxivHDR is on and the FG chain is R10G10B10A2,
// negotiate PQ/BT.2020; on failure put the chain back to the game's format.
void ConfigureOutput(Dx11wDx12SC& sc);
// dtor, first statement: leave OptiHDR. Never blocks, never releases GPU objects (loader lock safe).
void OnDestroy(Dx11wDx12SC& sc);
// ResizeBuffers / ResizeBuffers1, the FG chain's format argument: R10G10B10A2 while the bridge outputs HDR.
DXGI_FORMAT OutputFormat(Dx11wDx12SC& sc, DXGI_FORMAT requested);
// ResizeBuffers / ResizeBuffers1, after a successful resize: renegotiates the HDR output. A failure is returned
// (DXGI_ERROR_UNSUPPORTED) and fails the resize.
HRESULT OnResizeDone(Dx11wDx12SC& sc);
// SetColorSpace1, first statement. True = handled, `out` is the result (OptiHDR owns the output and swallows the game's
// call). Otherwise the game's colour space is recorded in nightly's outputColorSpace/hdrOutputActive (SEAMS 5).
bool SetColorSpace(Dx11wDx12SC& sc, DXGI_COLOR_SPACE_TYPE colorSpace, HRESULT& out);
// _CopyDx11SharedToDx12FGBackBuffer, after the copy source was chosen: converts it to the PQ presentation image on the
// copy list while the bridge outputs HDR. `copySource`/`before` are updated; false = the conversion failed (the caller
// closes the list and returns false).
bool ConvertCopySource(Dx11wDx12SC& sc, UINT slot, ID3D12Resource*& copySource, D3D12_RESOURCE_STATES& before);
} // namespace ext::dx11bridge
