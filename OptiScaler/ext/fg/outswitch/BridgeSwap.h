#pragma once
// The bridge side of the live output switch: what the switch reads and writes on the swapchain object it owns, and the
// two present seams upstream now calls into.
//
// Who does what:
//   * the accessors (BridgeSetChain ... BridgeAdoptWithTransfer) name the upstream private state (with_dx12/
//     dx11_with_dx12_sc.h, `friend struct ext::Access`), so they live in ext and the bridge changes only where a seam
//     must be (U4, U5). They do not decide anything: the sequence does, through its Ops.
//   * the record: the colour space the game asked for and the resize generation (ext/dx11bridge/SwapRecord.h), copied
//     into the swap when a switch captures the chain, so the replay after the swap knows what to put back.
//   * the two seams answer exactly what the bridge answered before they existed while no swap is attached, which is the
//     whole of today's behaviour with the feature keys off. With a swap attached they run the state machine the
//     composition installed (WP5's Ops): the switch point itself is not decided here.
//
// Pure templates inside, Windows only in BridgeSwap.cpp: tests/ext/fg/fg_switch_bridge.cpp drives BridgeSwap with a fake
// bridge through the same two function pointers.

#include "ext/fg/outswitch/SwitchSequence.h"

#include <cstdint>

namespace ext::fg::outswitch
{
// One bridge's switch. The composition owns it and keeps it alive for the bridge's lifetime.
struct BridgeSwap
{
    SwitchState state;

    // The switch point the composition installs (WP5): a present that arrives while the bridge has no chain (U4), and
    // the present result of the live FG chain (U5). `presentStartMs` is this present's start on the switch's clock, and
    // the present-thread rule of the contract means these run on that thread only.
    // The present start is 64 bits on both sides: the switch's clock is the 64-bit steady one, and a start truncated to
    // 32 bits mismatches it after 49.7 days of machine uptime (the stall then measures about 2^32 ms, or negative).
    using DetachedFn = uint32_t (*)(BridgeSwap& swap, bool realChainMissing, uint64_t presentStartMs);
    using FgResultFn = uint32_t (*)(BridgeSwap& swap, uint32_t result, uint64_t presentStartMs);
    DetachedFn detached = nullptr;
    FgResultFn fgResult = nullptr;
    uint64_t presentStartMs = 0;
    // The removed reason of a lost device is logged once per bridge: a game that keeps presenting after a removal would
    // otherwise get one warning and one COM call per frame.
    bool deviceLostLogged = false;

    // The chain's record, copied by the capture: the game's colour space and the resize generation at that moment.
    uint32_t colorSpace = 0;
    bool colorSpaceRecorded = false;
    uint32_t resizeGeneration = 0;
};

// The seam's answer with the swap handed in (the product wrapper below looks it up on the bridge). Both are pure so the
// tests can drive them with a fake engine: a null swap, or one without the switch point installed, answers `fallback`,
// which is exactly the line's own value.
inline uint32_t DetachedAnswer(BridgeSwap* swap, bool realChainMissing, uint32_t fallback)
{
    if (swap == nullptr || swap->detached == nullptr)
        return fallback;
    return swap->detached(*swap, realChainMissing, swap->presentStartMs);
}

inline uint32_t FgResultAnswer(BridgeSwap* swap, uint32_t result)
{
    if (swap == nullptr || swap->fgResult == nullptr)
        return result;
    return swap->fgResult(*swap, result, swap->presentStartMs);
}

// The swap attached to a bridge (a Dx11wDx12SC*), or null when the composition has not attached one.
BridgeSwap* SwapFor(void* bridge);
void AttachSwap(void* bridge, BridgeSwap* swap);

// The bridge accessors the switch's Ops are built from. `bridge` is a Dx11wDx12SC*.
void BridgeSetChain(void* bridge, void* chain);           // the bridge's FG chain member, and the global pointer
void BridgeSetProvider(void* bridge, void* provider);     // the bridge's IFGFeature member, and State::currentFG
void BridgeRefreshDesc(void* bridge);                     // the cached descriptor the capture hands the new chain
void BridgeReleaseInterop(void* bridge);                  // the interop render targets and the hud-less slots behind them
void BridgeResizeEpilogue(void* bridge);                  // the HDR/window epilogue the bridge's own resize runs
void BridgeAdoptWithTransfer(void* bridge, void* chain, void* provider, Target target); // no AddRef: the pin moves
// Re-issues a resize on the chain now on the window when a resize ran across the switch (SwapRecord.h). False when the
// chain refused it, which fails the replay step rather than leaving a chain sized for the old window.
bool BridgeReplayResize(void* bridge, uint32_t generationAtCapture);
} // namespace ext::fg::outswitch
