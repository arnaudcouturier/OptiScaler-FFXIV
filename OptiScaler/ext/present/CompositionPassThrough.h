#pragma once
// E5: the pass-through decision of the CreateSwapChainForComposition hook (ext/present/CompositionSwapchain.cpp).
// Pure: plain bools and integers, no upstream types, so tests/ext/present/composition_passthrough_test.cpp builds it
// standalone (same split as ext/ffxiv/markers/FfxivMarkersEnabled.h).
//
// "Pass-through" means: call the trampoline, hand its result back, and do NOT wrap the swapchain, touch State or
// log it as ours. The hook always calls the trampoline first -- in every case, pass-through or not -- so a true
// result here yields "trampoline only, no wrapping" by the hook's structure (it returns right after the trampoline
// when this is true), not by anything this function does.
//
// Inputs, in the order of the oracle's passThrough expression (hooks/DxgiFactory_Hooks.cpp at the pinned
// ref-nightly-merge SHA, `FfxivNameplateLiveScope::active || ...`):
//   hooksBypassed      ext::core::HooksBypassed(): the calling thread is inside an ext::core::BypassScope. This is
//                      the Companion case: the nameplate layer (ext/ffxiv/companion/replay/FfxivNameplateLive.h Run())
//                      creates its own composition swapchain under that scope, and it must never get wrapped, have
//                      the overlay attached, or become State::currentSwapchain.
//   vulkanCreatingSC   State::vulkanCreatingSC: Vulkan's DXGI interop is building the chain.
//   skipFgSwapChain    DxgiFactoryHooks::_skipFGSwapChainCreation: the upstream recursion guard of the FG swapchain.
//   hasDevice          pDevice != nullptr
//   hasDesc            pDesc != nullptr (width/height are ignored when false)
//   hasOutParam        ppSwapChain != nullptr
//   width, height      pDesc->Width / pDesc->Height. Below 100 is an overlay/helper chain (Discord, Steam, browsers).

namespace ext::present
{
inline constexpr unsigned int kCompositionMinExtent = 100;

inline bool CompositionPassThrough(bool hooksBypassed, bool vulkanCreatingSC, bool skipFgSwapChain, bool hasDevice,
                                   bool hasDesc, bool hasOutParam, unsigned int width, unsigned int height)
{
    return hooksBypassed || vulkanCreatingSC || skipFgSwapChain || !hasDevice || !hasDesc || !hasOutParam ||
           width < kCompositionMinExtent || height < kCompositionMinExtent;
}
} // namespace ext::present
