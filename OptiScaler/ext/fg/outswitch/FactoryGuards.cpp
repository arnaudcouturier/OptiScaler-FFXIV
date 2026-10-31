#include "pch.h"

#include "ext/Seams.h"

#include <hooks/DxgiFactory_Hooks.h>
#include <hooks/DxgiFactory_WrappedCalls.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Hook_Utils.h>

#include <wrapped/wrapped_factory.h>

// The factory entries the live output switch's plain chain goes through. Both upstream classes keep the FG swapchain skip
// and the real create entry private (their headers carry the friend line), and this file is the only caller.
//
// The guard set is the one boot's own fallback uses when the library create failed: the DXGI load checks and the parent
// wrapping are skipped, so the chain that comes back is the real one and it is registered as the plain presenter by the
// caller (FGHooks::SetDx12InteropPresentSC). Nothing here decides when a chain is created: the switch's Ops does.
long ext::Access::CreatePlainSwapChain(void* factory, void* wrappedFactory, void* device, void* desc, void** chain)
{
    auto* realFactory = static_cast<IDXGIFactory*>(factory);
    auto* swapchainDesc = static_cast<DXGI_SWAP_CHAIN_DESC*>(desc);
    auto* out = reinterpret_cast<IDXGISwapChain**>(chain);
    if (realFactory == nullptr || device == nullptr || swapchainDesc == nullptr || out == nullptr)
        return E_INVALIDARG;
    *out = nullptr;

    ScopedSkipDxgiLoadChecks skipLoadChecks {};
    ScopedSkipParentWrapping skipParentWrapping {};
    if (wrappedFactory != nullptr)
    {
        DxgiFactoryWrappedCalls::ScopedSkipFGSCCreation skipFgSwapChainCreation {};
        return DxgiFactoryWrappedCalls::CreateSwapChain(realFactory,
                                                       static_cast<WrappedIDXGIFactory7*>(wrappedFactory),
                                                       static_cast<IUnknown*>(device), swapchainDesc, out);
    }

    DxgiFactoryHooks::ScopedSkipFGSCCreation skipFgSwapChainCreation {};
    return DxgiFactoryHooks::o_CreateSwapChain(realFactory, static_cast<IUnknown*>(device), swapchainDesc, out);
}

// A provider chain: boot's own call (DxgiFactory_Hooks.cpp, the Dx11wDx12 branch), upstream's CreateSwapChain with the
// factory's FG swapchain creation skipped. The library creates its real chain through the factory it is handed, and the
// hooked factory would otherwise take that inner create for a game's new swapchain and run the provider's create a second
// time from inside the first: FSR FG then nests a second frame-interpolation swapchain under the first, and XeFG fails a
// second initialisation of the context it is initialising. Both factory classes are skipped, since the library may reach
// either.
long ext::Access::CreateProviderSwapChain(void* factory, void* device, void* desc, void** chain)
{
    auto* realFactory = static_cast<IDXGIFactory*>(factory);
    auto* swapchainDesc = static_cast<DXGI_SWAP_CHAIN_DESC*>(desc);
    auto* out = reinterpret_cast<IDXGISwapChain**>(chain);
    if (realFactory == nullptr || device == nullptr || swapchainDesc == nullptr || out == nullptr)
        return E_INVALIDARG;
    *out = nullptr;

    DxgiFactoryHooks::ScopedSkipFGSCCreation skipHookedFactory {};
    DxgiFactoryWrappedCalls::ScopedSkipFGSCCreation skipWrappedFactory {};
    return FGHooks::CreateSwapChain(realFactory, static_cast<IUnknown*>(device), swapchainDesc, out);
}
