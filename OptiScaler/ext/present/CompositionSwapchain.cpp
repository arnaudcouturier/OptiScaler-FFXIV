#include "pch.h"

#include "ext/Seams.h"
#include "ext/present/CompositionPassThrough.h"
#include "ext/present/CompositionWindow.h"

#include <State.h>

#include <hooks/DxgiFactory_Hooks.h>
#include <hooks/Hook_Utils.h>
#include <wrapped/wrapped_swapchain.h>

#include <detours/detours.h>

// E5: the composition-swapchain half of the oracle's swapchain size-to-window feature (the 0x0 resolve half is not
// carried, SEAMS.md section 5). Two pieces:
//   - HookFactoryComposition: vtable slot 24 of IDXGIFactory2 (CreateSwapChainForComposition), attached in this
//     file's own Detours transaction from DxgiFactoryHooks::HookToFactory. A game or overlay that composes through
//     DirectComposition (browsers, Discord/Steam overlays, UWP shells) gets its chain wrapped, so the menu overlay
//     can attach and the chain is Present-tracked; the chain is never replaced by a frame-generation swapchain.
//   - ResolveCompositionHandle: the wrapper-side seam (Present / Present1 of a composition chain), see above.
//
// Companion (FFXIV nameplate layer): ext/ffxiv/companion/replay/FfxivNameplateLive.h Run() creates its own composition
// swapchain with an ordinary CreateSwapChainForComposition call on a factory from CreateDXGIFactory1, all under an
// ext::core::BypassScope on that thread. Detours rewrites the function body in dxgi.dll that every factory's slot 24
// points to, so once the GAME's factory is hooked the Companion's call enters this hook too -- the bypass in
// Dxgi_Hooks.cpp (hkCreateDXGIFactory*) only keeps the Companion's factory from being wrapped or hooked; it does not
// keep its calls out of an already-installed detour. What keeps the Companion's chain unwrapped is the
// HooksBypassed() input of CompositionPassThrough below (the 8th and last HooksBypassed site, SEAMS.md 3.9).

namespace
{
using PFN_CreateSwapChainForComposition = rewrite_signature<decltype(&IDXGIFactory2::CreateSwapChainForComposition)>::type;

// IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11, IDXGIFactory1 12-13, IDXGIFactory2 14-24.
constexpr size_t kCreateSwapChainForCompositionSlot = 24;

PFN_CreateSwapChainForComposition o_CreateSwapChainForComposition = nullptr;

HRESULT WINAPI hkCreateSwapChainForComposition(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                               const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
                                               IDXGISwapChain1** ppSwapChain);

static_assert(std::is_same_v<decltype(&hkCreateSwapChainForComposition), PFN_CreateSwapChainForComposition>,
              "Signature mismatch: hkCreateSwapChainForComposition does not match PFN_CreateSwapChainForComposition");

HRESULT WINAPI hkCreateSwapChainForComposition(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                               const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
                                               IDXGISwapChain1** ppSwapChain)
{
    // Always call the trampoline, including pass-through/error cases. Calling the detoured virtual method here
    // re-enters this hook. Keep the composition descriptor intact: notably, a desktop VSync override must not turn
    // its FLIP_SEQUENTIAL swap effect into FLIP_DISCARD.
    const bool passThrough = ext::present::CompositionPassThrough(
        ext::core::HooksBypassed(), State::Instance().vulkanCreatingSC, DxgiFactoryHooks::SkipFgSwapChainCreation(),
        pDevice != nullptr, pDesc != nullptr, ppSwapChain != nullptr, pDesc != nullptr ? pDesc->Width : 0,
        pDesc != nullptr ? pDesc->Height : 0);

    if (pDesc != nullptr && (pDesc->Width < ext::present::kCompositionMinExtent ||
                             pDesc->Height < ext::present::kCompositionMinExtent))
        LOG_WARN("Composition overlay/helper call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};
        result = o_CreateSwapChainForComposition(realFactory, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (passThrough || FAILED(result) || *ppSwapChain == nullptr)
        return result;

    WrappedIDXGISwapChain4* existing = nullptr;
    if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&existing)) == S_OK)
    {
        existing->Release();
        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 resolved {};
    if (FAILED((*ppSwapChain)->GetDesc1(&resolved)))
        return result; // Do not install a wrapper with unknown dimensions/flags.

    // A composition chain has no native HWND. Use only an eligible window in this process, and defer overlay
    // initialization to Present if no unambiguous window exists yet. Do not borrow another application's foreground
    // window. HWND association remains a best-effort fallback.
    const HWND window = ext::present::FindCompositionWindow();
    IDXGISwapChain1* const created = *ppSwapChain;
    auto* wrapped = new WrappedIDXGISwapChain4(created, pDevice, window, resolved.Flags, false, true);
    *ppSwapChain = wrapped;

    // Preserve the returned COM/proxy chain: the wrapper owns its original reference. Unwrapping it here can leak an
    // outer proxy and bypass another provider's presentation path.
    State::Instance().currentRealSwapchain = created;
    State::Instance().currentSwapchain = wrapped;
    State::Instance().currentWrappedSwapchain = wrapped;
    State::Instance().swapchainInteropApi = SwapchainInteropApi::None;
    State::Instance().SCAllowTearing = (resolved.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    State::Instance().SCLastFlags = resolved.Flags;
    State::Instance().realExclusiveFullscreen = false;
    State::Instance().screenWidth = (float) resolved.Width;
    State::Instance().screenHeight = (float) resolved.Height;

    DXGI_SWAP_CHAIN_DESC legacy {};
    if (SUCCEEDED(created->GetDesc(&legacy)))
        State::Instance().currentSwapchainDesc = legacy;

    LOG_INFO("Wrapped composition swapchain {}x{}, window {:X}; plain presentation, no FG swapchain replacement",
             resolved.Width, resolved.Height, (SIZE_T) window);
    return result;
}
} // namespace

namespace ext::present
{
void HookFactoryComposition(IDXGIFactory* factory)
{
    if (factory == nullptr || o_CreateSwapChainForComposition != nullptr)
        return;

    // Same QueryInterface-then-Release pattern as HookToFactory's own slot reads: the caller keeps the factory alive
    // for the duration of this call, and only the vtable pointer is taken.
    IDXGIFactory2* factory2 = nullptr;
    if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK || factory2 == nullptr)
        return;

    void** const vtable = *reinterpret_cast<void***>(factory2);
    factory2->Release();

    auto* target = (PFN_CreateSwapChainForComposition) vtable[kCreateSwapChainForCompositionSlot];
    if (target == nullptr)
        return;

    o_CreateSwapChainForComposition = target;

    // Own transaction: the one HookToFactory opened has already been committed when this is called.
    const LONG began = DetourTransactionBegin();
    if (began != NO_ERROR)
    {
        LOG_ERROR("Failed to begin composition hook transaction: {:X}", (unsigned int) began);
        o_CreateSwapChainForComposition = nullptr;
        return;
    }

    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CreateSwapChainForComposition, hkCreateSwapChainForComposition);

    const LONG committed = DetourTransactionCommit();
    if (committed != NO_ERROR)
    {
        LOG_ERROR("Failed to hook IDXGIFactory2::CreateSwapChainForComposition: {:X}", (unsigned int) committed);
        o_CreateSwapChainForComposition = nullptr;
        return;
    }

    LOG_INFO("Hooked IDXGIFactory2::CreateSwapChainForComposition");
}

bool ResolveCompositionHandle(HWND& handle)
{
    if (!IsCompositionWindow(handle))
        handle = FindCompositionWindow();

    return handle != nullptr;
}
} // namespace ext::present
