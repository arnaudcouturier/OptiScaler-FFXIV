#include "pch.h"

#include "ext/hudless/HudlessGpu.h"

// S10 package B: stand-in definitions of the GPU contract (ext/hudless/HudlessGpu.h) so the engine builds and links before
// package C lands. The orchestrator DELETES THIS FILE when C's UiPaste.cpp / UiImage.cpp are stacked. Ready() is false, so
// ui_free never leaves its "PasteNotReady" fallback and nothing is ever swapped, produced or pasted.

namespace ext::hudless
{
namespace UiPaste
{
void Produce(ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource*, DXGI_FORMAT, ID3D12Resource*, DXGI_FORMAT,
             ID3D12Resource*, const MaskParams&)
{
}

void Invalidate(const char*) {}

bool Ready(ID3D12Device*, uint32_t, uint32_t) { return false; }

void Release() {}

void Paste(IDXGISwapChain*, ID3D12CommandQueue*) {}

void SetDebugTint(bool) {}

void SetSteadyTiming(bool) {}

PasteStatus Snapshot() { return {}; }
} // namespace UiPaste

namespace UiImage
{
ID3D12Resource* Build(ID3D12Device*, ID3D12GraphicsCommandList*, int, ID3D12Resource*, DXGI_FORMAT, ID3D12Resource*,
                      DXGI_FORMAT, const MaskParams&)
{
    return nullptr;
}

void Release() {}
} // namespace UiImage
} // namespace ext::hudless
