#pragma once
// Fork state of one Dx11wDx12SC (with_dx12/dx11_with_dx12_sc.h `_ext`, docs/SEAMS.md 3.2). All fork per-bridge state
// hangs off this struct; the hudless engine (S10) adds its ring and flags here.

#include <d3d12.h>

#include <cstdint>

namespace ext::dx11bridge
{
struct BridgeExt
{
    // C8: the FG swapchain carries OptiHDR's PQ output (was Dx11wDx12SC::_hdrOutput)
    bool hdrOutput = false;
    // The live output switch's per-chain record (ext/dx11bridge/SwapRecord.h): the colour space the game asked for, and
    // how many resizes this bridge has seen. Both are written by seams the bridge already calls.
    uint32_t lastColorSpace = 0;
    bool colorSpaceRecorded = false;
    uint32_t resizeGeneration = 0;
    uint64_t lastResizeMs = 0;     // the switch's monotonic clock, 0 = none this session
    uint32_t lastResizeThread = 0; // the thread that ran the resize: another thread than the present thread refuses

    // The composed live output switch for this bridge, null until the composition attaches one. A null means the two
    // present seams answer exactly what the bridge answered before they existed.
    void* switchSwap = nullptr;

    // C9: valid only between ConvertCopySource and the matching copy-list screenshot seam.
    ID3D12Resource* screenshotSdrSource = nullptr;
    D3D12_RESOURCE_STATES screenshotSdrState = D3D12_RESOURCE_STATE_COMMON;
};
} // namespace ext::dx11bridge
