#include "pch.h"

#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/dx11bridge/SwapRecord.h"
#include "ext/fg/FgSeams.h"
#include "ext/fg/outswitch/BridgeRegistry.h"
#include "ext/fg/outswitch/SwitchTypes.h"
#include "ext/fg/outswitch/BridgeSwap.h"

#include <State.h>

#include <with_dx12/dx11_with_dx12_sc.h>

// The bridge side of the switch: the accessors (they name upstream's private state through ext::Access::Bridge) and the
// two present seams U4/U5 call. Nothing here decides a switch; see BridgeSwap.h.

namespace ext::fg::outswitch
{
namespace
{
Dx11wDx12SC* AsBridge(void* bridge) { return static_cast<Dx11wDx12SC*>(bridge); }
} // namespace

BridgeSwap* SwapFor(void* bridge)
{
    if (bridge == nullptr)
        return nullptr;
    return static_cast<BridgeSwap*>(Access::Bridge::Ext(*AsBridge(bridge)).switchSwap);
}

void AttachSwap(void* bridge, BridgeSwap* swap)
{
    if (bridge == nullptr)
        return;
    Access::Bridge::Ext(*AsBridge(bridge)).switchSwap = swap;
}

void BridgeSetChain(void* bridge, void* chain)
{
    auto* sc = AsBridge(bridge);
    Access::Bridge::SetFgSwapChain(*sc, static_cast<IDXGISwapChain4*>(chain));
    ::State::Instance().currentFGSwapchain = static_cast<IDXGISwapChain*>(chain);
}

void BridgeSetProvider(void* bridge, void* provider)
{
    auto* sc = AsBridge(bridge);
    Access::Bridge::SetFg(*sc, static_cast<IFGFeature_Dx12*>(provider));
    ::State::Instance().currentFG = static_cast<IFGFeature_Dx12*>(provider);
}

void BridgeRefreshDesc(void* bridge) { Access::Bridge::RefreshDesc(*AsBridge(bridge)); }

void BridgeReleaseInterop(void* bridge) { Access::Bridge::ReleaseInteropBackBuffers(*AsBridge(bridge)); }

void BridgeResizeEpilogue(void* bridge)
{
    // The same HDR and window epilogue the bridge's own resize runs, with one difference: this is not a resize of the
    // window, so it does not move the resize record the switch captured and replays against (a recorded resize here would
    // make the replay resize a correctly sized chain and the next switch refuse as a busy resize). A failure is logged by
    // the caller's step, not swallowed here.
    (void) ext::dx11bridge::OnResizeDone(*AsBridge(bridge), false);
}

void BridgeAdoptWithTransfer(void* bridge, void* chain, void* provider, Target target)
{
    // Ownership transfer, not a second reference: the creation reference becomes the bridge's pin. The three chain
    // globals are the switch's own decision (Create/Adopt/Commit), which is why they are written here and nowhere else.
    auto* sc = AsBridge(bridge);
    Access::Bridge::SetFgSwapChain(*sc, static_cast<IDXGISwapChain4*>(chain));
    Access::Bridge::SetFg(*sc, static_cast<IFGFeature_Dx12*>(provider));
    ::State::Instance().currentFGSwapchain = static_cast<IDXGISwapChain*>(chain);
    ::State::Instance().currentFG = static_cast<IFGFeature_Dx12*>(provider);
    // The name of the chain on the window is the bridge again, which is what boot leaves behind (the library's own create
    // had pointed it at its inner chain): every reader of that global gets the object the game presents through.
    ::State::Instance().currentSwapchain = static_cast<IDXGISwapChain*>(bridge);
    ::State::Instance().currentWrappedSwapchain = static_cast<IDXGISwapChain*>(bridge);
    ::State::Instance().activeFgOutput = target == Target::XeFg    ? FGOutput::XeFG
                                         : target == Target::FsrFg ? FGOutput::FSRFG
                                                                   : FGOutput::NoFG;
}

bool BridgeReplayResize(void* bridge, uint32_t generationAtCapture)
{
    auto* sc = AsBridge(bridge);
    auto& record = Access::Bridge::Ext(*sc);
    if (!ext::dx11bridge::ResizeCrossed(generationAtCapture, record.resizeGeneration))
        return true; // no resize ran across the switch: the new chain already has the window's size

    IDXGISwapChain4* chain = Access::Bridge::FgSwapChain(*sc);
    if (chain == nullptr)
        return false;
    // A zero resize carries the window's current size and keeps the format and flags the chain was created with.
    DXGI_SWAP_CHAIN_DESC1 desc {};
    if (FAILED(chain->GetDesc1(&desc)))
        return false;
    return SUCCEEDED(chain->ResizeBuffers(0, 0, 0, desc.Format, desc.Flags));
}
} // namespace ext::fg::outswitch

namespace ext::fg
{
uint32_t DetachedPresent(Dx11wDx12SC& sc, bool realChainMissing)
{
    return outswitch::DetachedAnswer(outswitch::SwapFor(&sc), realChainMissing, outswitch::kDeviceRemoved);
}

HRESULT OnFgPresentResult(Dx11wDx12SC& sc, HRESULT result)
{
    // The bridge that presented is the one whose swap answers: the registry's pointer is only a fallback for a bridge
    // that has not been configured yet, and with two windows it would be the wrong one.
    return static_cast<HRESULT>(outswitch::FgResultAnswer(outswitch::SwapFor(&sc), static_cast<uint32_t>(result)));
}
} // namespace ext::fg
