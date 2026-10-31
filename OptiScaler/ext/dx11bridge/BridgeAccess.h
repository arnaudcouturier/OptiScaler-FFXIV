#pragma once
// ext::Access::Bridge: the fork's window into the private state of Dx11wDx12SC (with_dx12/dx11_with_dx12_sc.h, friend
// `ext::Access`). A nested class of the friend has its rights. Included only by fork code under ext/ (dx11bridge, hdr, window);
// upstream files never see it.

#include "ext/Seams.h"

#include <upscalers/IFeature_Dx11wDx12.h>
#include <with_dx12/dx11_with_dx12_sc.h>

struct ext::Access::Bridge
{
    static ext::dx11bridge::BridgeExt& Ext(Dx11wDx12SC& sc)
    {
        if (!sc._ext)
            sc._ext = std::make_unique<ext::dx11bridge::BridgeExt>();
        return *sc._ext;
    }
    static bool HdrOutput(const Dx11wDx12SC& sc) { return sc._ext && sc._ext->hdrOutput; }

    // C4: lazily creates the per-feature forced-quality colour scaler state (upscalers/IFeature_Dx11wDx12.h `_ext`).
    static ext::dx11bridge::BridgeState& Ext(IFeature_Dx11wDx12& feature)
    {
        if (!feature._ext)
            feature._ext = std::make_unique<ext::dx11bridge::BridgeState>();
        return *feature._ext;
    }

    // C4 (Fix 2): ext::dx11bridge::PrepareUpscalerColor needs this frame's actual render size, not the (one-frame-
    // stale) IFeature_Dx11wDx12::RenderWidth()/RenderHeight() public accessor -- it resolves it itself through
    // ext::upscaler::ResolveRenderGeometry, which needs the same feature identity CorrectRenderSubrect passes
    // moments later (dx12Feature.get(), not `&feature`) so UpscalerSeams.cpp's per-feature geometry-log dedup state
    // isn't bounced between two different pointers for what is really one logical feature.
    static IFeature* InnerFeature(IFeature_Dx11wDx12& feature) { return feature.dx12Feature.get(); }

    static IDXGISwapChain4* FgSwapChain(Dx11wDx12SC& sc) { return sc._fgSwapChain; }
    // The hidden real chain the bridge presents through: what the live output switch's capture reads when there is no FG
    // chain on the window any more (a rollback or a limp retry).
    static IDXGISwapChain* RealSwapChain(Dx11wDx12SC& sc) { return sc._real; }
    static DXGI_FORMAT BufferFormat(const Dx11wDx12SC& sc) { return sc._bufferFormat; }
    static ID3D12Device* Dx12Device(Dx11wDx12SC& sc) { return sc._dx12Device; }
    static ID3D11DeviceContext* Dx11Context(Dx11wDx12SC& sc) { return sc._dx11Context; }
    static ID3D12CommandQueue* CopyQueue(Dx11wDx12SC& sc) { return sc._dx12CommandQueue; }

    // Frame-pacing GPU timers (ext/present/GpuBridgeTimers.cpp): the fence the interop copy signals after each
    // execution, and the value it signaled last for one copy list. Reads only (GetCompletedValue on the timer side,
    // never a wait): a slot reports once the fence has passed the value that covers its resolve. The list value is
    // 0 for an out-of-range list, which never reports (a covering value must exceed the recorded one).
    static ID3D12Fence* CopyFence(Dx11wDx12SC& sc) { return sc._copyFence; }
    static UINT64 CopyAllocatorFenceValue(const Dx11wDx12SC& sc, UINT slot)
    {
        return slot < sc._copyAllocatorFenceValues.size() ? sc._copyAllocatorFenceValues[slot] : 0;
    }
    static ID3D12GraphicsCommandList* CopyList(Dx11wDx12SC& sc, UINT slot) { return sc._copyCommandLists[slot]; }
    static HWND Window(Dx11wDx12SC& sc) { return sc._handle; }
    static IFGFeature_Dx12* Fg(Dx11wDx12SC& sc) { return sc._fg; }

    // Window mode (M2): the bridge's back buffer index and buffer count. Dx11wDx12SC::Present advances the index
    // (_AdvanceFakeBackBufferIndex) only after `_fgSwapChain->Present` returned success, with frame generation on or off;
    // every early `return DXGI_ERROR_DEVICE_REMOVED` before that call leaves it alone. Read-only.
    // The live output switch's accessors (ext/fg/outswitch/BridgeSwap.h). They write only what a switch owns: the FG chain
    // and provider pointers, the cached descriptor the capture hands the new chain, and the interop resources a resize
    // would retire anyway. Nothing here decides anything.
    static void SetFgSwapChain(Dx11wDx12SC& sc, IDXGISwapChain4* chain) { sc._fgSwapChain = chain; }
    static void SetFg(Dx11wDx12SC& sc, IFGFeature_Dx12* fg) { sc._fg = fg; }
    static void RefreshDesc(Dx11wDx12SC& sc) { sc._RefreshCachedSwapchainDesc(); }
    static void ReleaseInteropBackBuffers(Dx11wDx12SC& sc) { sc._ReleaseInteropBackBuffers(); }

    static UINT FakeBackBufferIndex(const Dx11wDx12SC& sc) { return sc._currentFakeIndex; }
    static UINT BufferCount(const Dx11wDx12SC& sc) { return sc._bufferCount; }
};
