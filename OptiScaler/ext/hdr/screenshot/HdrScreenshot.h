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
void Reset(ID3D12CommandList* commands) noexcept;
void AfterExecute(void* context, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept;
std::string Status();
} // namespace ext::hdr::screenshot
