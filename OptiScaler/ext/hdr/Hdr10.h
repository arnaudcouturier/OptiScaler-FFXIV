#pragma once
// OptiHDR (C8): HDR10 (PQ / BT.2020) output through the DX11 bridge presenter. Was shaders/hdr/Hdr10.h. ext-internal;
// the upstream-facing seams are in HdrSeams.h. The command-list wrappers of the reference build (Hdr10::ResetCommands /
// ExecuteCommands) are ext::gpu::ResetCommands / ExecuteCommands now; the conversion pool is one ext::gpu::Owner.

#include "ext/gpu/GpuSubmission.h"
#include "ext/hdr/SceneInput.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <functional>
#include <string>

namespace ext::hdr
{
// Policy query for the presenter: OptiHDR enabled, a game that supports it, FG output compatible and the window's
// monitor currently in HDR10. Side effect free apart from the status text.
bool Request(HWND window);
// Negotiates PQ/BT.2020 on the FG swapchain. On success OptiHDR is engaged (Active() becomes true, nightly's
// outputColorSpace/hdrOutputActive are filled) and the conversion pool is registered as a GPU submission owner.
bool Configure(IDXGISwapChain4* chain);
void Deactivate();
std::string Status();
// Converts `src` (SDR or FP16) to the PQ R10G10B10A2 presentation image on `commands`. The result belongs to the pool
// and stays valid until the list has completed; null on failure.
ID3D12Resource* Convert(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES,
                        SceneInput scene = {});
// Reuse probe of a list recorded by another HDR pass (scene input, DLSS scene input): completion plus recording retired.
std::function<bool()> TrackCommands(ID3D12GraphicsCommandList*);
// ext::gpu::Owner callbacks of the conversion pool (registered by Configure).
gpu::GpuSubmission BeginSubmission(UINT, ID3D12CommandList* const*);
void QuarantineSubmission(UINT, ID3D12CommandList* const*);
void Reset(ID3D12CommandList*);
} // namespace ext::hdr

namespace ext::hdr
{
// ext::dx11bridge::SetColorSpace, OptiHDR not engaged: records the colour space the game selected on the bridge in
// nightly's State::outputColorSpace / hdrOutputActive, as a wrapped swapchain's UpdateOutputColorSpace would (the
// reference build wrote its own write-only isHdrActive here).
void NoteGameColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
} // namespace ext::hdr
