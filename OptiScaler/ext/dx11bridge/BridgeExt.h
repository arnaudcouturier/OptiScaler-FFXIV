#pragma once
// Fork state of one Dx11wDx12SC (with_dx12/dx11_with_dx12_sc.h `_ext`, docs/SEAMS.md 3.2). All fork per-bridge state
// hangs off this struct; the hudless engine (S10) adds its ring and flags here.

namespace ext::dx11bridge
{
struct BridgeExt
{
    // C8: the FG swapchain carries OptiHDR's PQ output (was Dx11wDx12SC::_hdrOutput)
    bool hdrOutput = false;
};
} // namespace ext::dx11bridge
