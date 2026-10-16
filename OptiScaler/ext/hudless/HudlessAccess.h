#pragma once
// ext::Access::Hudless: the HUD-less engine's window into the private state of Dx11wDx12SC (the D3D11 -> D3D12 bridge)
// and into the protected UI fence of IFGFeature_Dx12. A nested class of the friend `ext::Access` has Access's rights.
// Included only by ext/hudless/*.cpp; upstream files never see it.

#include "ext/Seams.h"

#include <framegen/IFGFeature_Dx12.h>
#include <with_dx12/dx11_with_dx12_sc.h>

struct ext::Access::Hudless
{
    // ---- Dx11wDx12SC ------------------------------------------------------------------------------------------------
    static ID3D11Device* Dx11Device(Dx11wDx12SC& sc) { return sc._dx11Device; }
    static ID3D11DeviceContext* Dx11Context(Dx11wDx12SC& sc) { return sc._dx11Context; }
    static ID3D12Device* Dx12Device(Dx11wDx12SC& sc) { return sc._dx12Device; }
    static IDXGISwapChain4* FgSwapChain(Dx11wDx12SC& sc) { return sc._fgSwapChain; }
    static IFGFeature_Dx12* Fg(Dx11wDx12SC& sc) { return sc._fg; }
    static DXGI_FORMAT BufferFormat(const Dx11wDx12SC& sc) { return sc._bufferFormat; }
    static bool HdrOutput(const Dx11wDx12SC& sc) { return sc._ext && sc._ext->hdrOutput; }
    static UINT FakeIndex(const Dx11wDx12SC& sc) { return sc._currentFakeIndex; }

    // The bridge's shared D3D11 copy of this frame's backbuffer (null when not created) and its D3D12 view, the FINAL image
    static ID3D11Texture2D* SharedBackBuffer(Dx11wDx12SC& sc, UINT slot)
    {
        return slot < sc._sharedDx11BackBufferCopies.size() ? sc._sharedDx11BackBufferCopies[slot] : nullptr;
    }
    static ID3D12Resource* OpenedBackBuffer(Dx11wDx12SC& sc, UINT slot)
    {
        return slot < sc._openedDx11BackBuffers.size() ? sc._openedDx11BackBuffers[slot] : nullptr;
    }

    // The interop copy queue's fence and the value its last copy list signalled (the copy list reads the base image)
    static ID3D12Fence* CopyFence(Dx11wDx12SC& sc) { return sc._copyFence; }
    static uint64_t LastCopyValue(const Dx11wDx12SC& sc) { return sc._lastInteropCopyFenceValue; }

    // ---- IFGFeature_Dx12 --------------------------------------------------------------------------------------------
    // The fence the frame generator signals after it submitted the UI command list of `index`, and the value reserved for
    // that list when it was reset (read right after GetUICommandList(index)). The tags and the UI image are recorded on
    // that list, so they complete when the fence reaches the value.
    static ID3D12Fence* UiFence(IFGFeature_Dx12& fg) { return fg._uiFence; }
    static uint64_t UiFenceValue(IFGFeature_Dx12& fg, int index)
    {
        return index >= 0 && index < BUFFER_COUNT ? fg._uiAllocatorFenceValues[index] : 0;
    }
};
