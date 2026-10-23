#pragma once

#include <d3d12.h>
#include <string>
#include <windows.h>

namespace ext::hdr::screenshot
{
void Request();
bool Pending();
bool PendingHDR();
void Prepare(HWND window, ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* hdrSource,
             D3D12_RESOURCE_STATES hdrState, ID3D12Resource* sdrSource, D3D12_RESOURCE_STATES sdrState);
// F4 (docs/RECONSTRUCTION-REFERENCE.md): "Include OptiScaler menu". True while an HDR PNG should be taken after the menu
// was drawn onto the presented image (OptiHDR + HDR PNG + ScreenshotMenu + overlay menu enabled + menu visible). The
// bridge then leaves a pending request alone and PrepareMenu takes it.
bool MenuCaptureWanted();
// OverlayScope, right after the menu layer was composited into the PQ output (`output` is still a render target):
// records the readback of the finished image into `commands` when MenuCaptureWanted() and a screenshot is pending.
// Submitted by AfterExecute when `commands` is executed, like the bridge's capture. Never throws.
void PrepareMenu(ID3D12GraphicsCommandList* commands, ID3D12Resource* output) noexcept;
void Reset(ID3D12CommandList* commands) noexcept;
void AfterExecute(void* context, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept;
std::string Status();
} // namespace ext::hdr::screenshot
