#include "pch.h"

#include "ext/Seams.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/fg/outswitch/ProviderOps.h"
#include "ext/fg/outswitch/QueueRules.h"

#include <hooks/DxgiFactory_Hooks.h>
#include <hooks/DxgiFactory_WrappedCalls.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Hook_Utils.h>

#include <framegen/IFGFeature_Dx12.h>
#include <proxies/FfxApi_Proxy.h>
#include <proxies/XeFG_Proxy.h>

#include <wrapped/wrapped_factory.h>

#include <State.h>

#include <with_dx12/dx11_with_dx12_sc.h>
#include <with_dx12/with_dx12.h>

#include <atomic>
#include <cstdio>
#include <cstring>

// The provider, factory and process-state adapters of the live output switch. The rules they keep are in ProviderOps.h and
// SwitchOpsContract.h; this file is the Windows/D3D glue and decides nothing about the switch itself.

namespace ext::fg::outswitch::provider
{
namespace
{
// The witness the teardown places on the chain it is about to release. It carries no data: what matters is whether the
// library released it with the outer object. Our own reference is held for the session, so the count is 1 when nothing is
// registered and 2+ while the interface is on a chain.
class ReleaseWitness final : public IUnknown
{
  public:
    ULONG AddRef() override { return ++refs_; }
    ULONG Release() override { return --refs_; }
    HRESULT QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown))
        {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG Refs() const { return refs_.load(std::memory_order_acquire); }

  private:
    std::atomic<ULONG> refs_ { 1 };
};

// A GUID of this module's own: no other private-data user can collide with it, and it means "the switch's witness".
constexpr GUID kWitnessGuid { 0x3f0d2c1a, 0x8b74, 0x4f2e, { 0x9a, 0x51, 0x6c, 0x2d, 0x07, 0xb4, 0x1e, 0x88 } };

ReleaseWitness* witness_ = nullptr;

// The loader verdict per target, latched once: the probes load a DLL and can mutate the configuration when they fail, so
// they must never run on a frame path or twice for the same answer.
struct LoaderCache
{
    bool asked = false;
    bool ready = false;
};
LoaderCache loaderCache_[3];

// The measured row per class, keyed by the module that answered last time: the identity is the loaded image, so the same
// handle is the same build. The question costs two syscalls per class, which the preflight would otherwise pay on every
// present while a wish is pending.
struct RowCache
{
    bool asked = false;
    HMODULE module = nullptr;
    HMODULE companion = nullptr;
    LibraryModel model;
};
RowCache rowCache_[3];

HMODULE ModuleFor(Class cls)
{
    switch (cls)
    {
    case Class::FsrFg:
        return FfxApiProxy::Dx12Module_FG();
    case Class::XeFg:
        return XeFGProxy::Module();
    default:
        return nullptr;
    }
}

// The shell Create() keeps between its retries, and the target it belongs to.
Target shellTarget_ = Target::Plain;
void* shell_ = nullptr;

IFGFeature_Dx12* AsProvider(void* provider) { return static_cast<IFGFeature_Dx12*>(provider); }
IDXGISwapChain* AsChain(void* chain) { return static_cast<IDXGISwapChain*>(chain); }

// The switch's own reference on the bridge's queue (QueueRules.h, rule 1), and the bridge it belongs to.
void* pinnedBridge_ = nullptr;
ID3D12CommandQueue* pinnedQueue_ = nullptr;

QueueTrace trace_;
// The library queue of the provider the last create made (LibraryQueue). Borrowed, and read only for that provider: a
// provider shell is retired, never freed (OutputSwitch.cpp RetireProvider), so its address is never reused by another one.
void* libraryProvider_ = nullptr;
void* libraryQueue_ = nullptr;

// The descriptor the last forward capture read on the window, and the bridge it belongs to (RecordChainDesc).
void* recordedBridge_ = nullptr;
DXGI_SWAP_CHAIN_DESC recordedDesc_ {};
bool fallbackWarned_ = false;
} // namespace

void* PinBridgeQueue(void* bridge)
{
    if (!queues::kPinBridgeQueue || bridge == nullptr)
        return nullptr;
    ID3D12CommandQueue* queue = Access::Bridge::CopyQueue(*static_cast<Dx11wDx12SC*>(bridge));
    if (queue == nullptr)
        return nullptr;
    // The pin follows the bridge's queue, not the bridge's address (QueueRules.h RepinNeeded).
    if (!queues::RepinNeeded(pinnedQueue_, queue))
        return pinnedQueue_;

    // Said, never refused: the two differ legitimately when a game-side create moved the interop, and a reader comparing
    // the queue lines wants to know which one the pin is.
    if (queue != WithDx12::GetD3D12CommandQueue())
        LOG_INFO("[fg-switch] the bridge's queue {:X} is not the interop's {:X} at the pin", reinterpret_cast<uintptr_t>(queue),
                 reinterpret_cast<uintptr_t>(WithDx12::GetD3D12CommandQueue()));
    // Taken before anything is torn down: at a frame-generation boot this queue is the boot library's own, and that
    // library's destroy drops the only other reference. The new reference is taken before the old one is given back.
    queue->AddRef();
    UnpinBridgeQueue();
    pinnedBridge_ = bridge;
    pinnedQueue_ = queue;
    LOG_INFO("[fg-switch] queue pin bridge={:X} queue={:X}", reinterpret_cast<uintptr_t>(bridge),
             reinterpret_cast<uintptr_t>(queue));
    return queue;
}

void UnpinBridgeQueue()
{
    if (pinnedQueue_ != nullptr)
        pinnedQueue_->Release();
    pinnedQueue_ = nullptr;
    pinnedBridge_ = nullptr;
}

bool ChooseCreationQueue(void* bridge, Captured& out)
{
    void* queue = queues::CreationQueue(PinBridgeQueue(bridge), ::State::Instance().currentCommandQueue);
    out.queue = static_cast<IUnknown*>(queue);
    return queue != nullptr;
}

QueueTrace LastQueueTrace() { return trace_; }

void RecordChainDesc(void* bridge, const DXGI_SWAP_CHAIN_DESC& desc)
{
    recordedBridge_ = bridge;
    recordedDesc_ = desc;
}

void* InteropQueueNow() { return WithDx12::GetD3D12CommandQueue(); }

void* ProviderQueue(void* provider)
{
    auto* fg = AsProvider(provider);
    return fg != nullptr ? fg->GetCommandQueue() : nullptr;
}

void* LibraryQueue(void* provider)
{
    return provider != nullptr && provider == libraryProvider_ ? libraryQueue_ : nullptr;
}

bool CaptureLive(void* chain, Captured& out)
{
    auto* live = AsChain(chain);
    if (live == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(live->GetDesc(&desc)))
        return false;

    IDXGIFactory* factory = nullptr;
    if (FAILED(live->GetParent(IID_PPV_ARGS(&factory))) || factory == nullptr)
        return false;

    out = Captured {};
    out.chain = chain;
    out.hwnd = desc.OutputWindow;
    out.desc = desc;
    out.factory = factory;
    WrappedIDXGIFactory7* wrappedFactory = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&wrappedFactory))))
        out.wrappedFactory = wrappedFactory;
    // The creation queue is chosen by the caller (ChooseCreationQueue): it needs the bridge, this capture does not.
    out.valid = true;
    return true;
}

bool CaptureRealFactory(void* bridge, Captured& out)
{
    if (bridge == nullptr)
        return false;
    auto& sc = *static_cast<Dx11wDx12SC*>(bridge);
    IDXGISwapChain* real = Access::Bridge::RealSwapChain(sc);
    if (real == nullptr)
        return false;
    IDXGIFactory* factory = nullptr;
    if (FAILED(real->GetParent(IID_PPV_ARGS(&factory))) || factory == nullptr)
        return false;
    if (out.realFactory != nullptr)
        out.realFactory->Release();
    out.realFactory = factory;
    return true;
}

bool CaptureBridge(void* bridge, Captured& out)
{
    if (bridge == nullptr)
        return false;
    auto& sc = *static_cast<Dx11wDx12SC*>(bridge);
    IDXGISwapChain* real = Access::Bridge::RealSwapChain(sc);
    if (real == nullptr)
        return false;

    // The descriptor of the chain the last forward capture read on the window: every rollback and limp follows one. The
    // global one is the fallback only, and it says so once: a library's inner chain rewrites it on every present.
    DXGI_SWAP_CHAIN_DESC desc {};
    if (recordedBridge_ == bridge && recordedDesc_.OutputWindow != nullptr)
    {
        desc = recordedDesc_;
    }
    else
    {
        if (!fallbackWarned_)
        {
            fallbackWarned_ = true;
            LOG_WARN("[fg-switch] the bridge capture has no recorded chain descriptor; using the global one");
        }
        desc = ::State::Instance().currentSwapchainDesc;
        if (desc.OutputWindow == nullptr || desc.BufferDesc.Width == 0)
        {
            if (FAILED(real->GetDesc(&desc)))
                return false;
        }
    }

    IDXGIFactory* factory = nullptr;
    if (FAILED(real->GetParent(IID_PPV_ARGS(&factory))) || factory == nullptr)
        return false;

    out = Captured {};
    out.chain = real;
    out.hwnd = desc.OutputWindow;
    out.desc = desc;
    out.factory = factory;
    WrappedIDXGIFactory7* wrappedFactory = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&wrappedFactory))))
        out.wrappedFactory = wrappedFactory;
    out.valid = true;
    if (!ChooseCreationQueue(bridge, out))
    {
        ReleaseCapture(out);
        return false;
    }
    return true;
}

void ReleaseCapture(Captured& capture)
{
    if (capture.factory != nullptr)
        capture.factory->Release();
    if (capture.wrappedFactory != nullptr)
        static_cast<WrappedIDXGIFactory7*>(capture.wrappedFactory)->Release();
    if (capture.realFactory != nullptr)
        capture.realFactory->Release();
    capture = Captured {};
}

StateSnapshot SnapshotState()
{
    auto& state = ::State::Instance();
    StateSnapshot snapshot;
    snapshot.desc = state.currentSwapchainDesc;
    snapshot.real = state.currentRealSwapchain;
    snapshot.wrapped = state.currentWrappedSwapchain;
    snapshot.current = state.currentSwapchain;
    snapshot.d3d12Device = state.currentD3D12Device;
    snapshot.d3d11Device = state.currentD3D11Device;
    snapshot.commandQueue = state.currentCommandQueue;
    snapshot.interopApi = static_cast<uint32_t>(state.swapchainInteropApi);
    snapshot.allowTearing = state.SCAllowTearing;
    snapshot.lastFlags = state.SCLastFlags;
    snapshot.exclusiveFullscreen = state.realExclusiveFullscreen;
    snapshot.screenWidth = state.screenWidth;
    snapshot.screenHeight = state.screenHeight;
    snapshot.interopDevice = WithDx12::GetD3D12Device();
    snapshot.interopQueue = WithDx12::GetD3D12CommandQueue();
    snapshot.interopListType = static_cast<uint32_t>(WithDx12::GetD3D12CommandListType());
    return snapshot;
}

void RestoreState(const StateSnapshot& snapshot)
{
    // The interop first (QueueRules.h, rule 3): its setter writes the global device and queue as well, and the block below
    // then puts every global back exactly as the snapshot had it.
    auto* queue = static_cast<ID3D12CommandQueue*>(
        queues::InteropAfterCreate(snapshot.interopQueue, WithDx12::GetD3D12CommandQueue()));
    auto* device = snapshot.interopDevice != nullptr ? static_cast<ID3D12Device*>(snapshot.interopDevice)
                                                     : WithDx12::GetD3D12Device();
    if (device != nullptr && queue != nullptr &&
        (queue != WithDx12::GetD3D12CommandQueue() || device != WithDx12::GetD3D12Device()))
    {
        WithDx12::SetD3D12Objects(device, queue, static_cast<D3D12_COMMAND_LIST_TYPE>(snapshot.interopListType));
    }

    auto& state = ::State::Instance();
    state.currentSwapchainDesc = snapshot.desc;
    state.currentRealSwapchain = snapshot.real;
    state.currentWrappedSwapchain = snapshot.wrapped;
    state.currentSwapchain = snapshot.current;
    state.currentD3D12Device = snapshot.d3d12Device;
    state.currentD3D11Device = snapshot.d3d11Device;
    state.currentCommandQueue = snapshot.commandQueue;
    state.swapchainInteropApi = static_cast<SwapchainInteropApi>(snapshot.interopApi);
    state.SCAllowTearing = snapshot.allowTearing;
    state.SCLastFlags = snapshot.lastFlags;
    state.realExclusiveFullscreen = snapshot.exclusiveFullscreen;
    state.screenWidth = snapshot.screenWidth;
    state.screenHeight = snapshot.screenHeight;
}

bool LoadersReady(Target target)
{
    const size_t index = static_cast<size_t>(target);
    LoaderCache& cached = loaderCache_[index];
    if (cached.asked)
        return cached.ready;

    cached.asked = true;
    switch (target)
    {
    case Target::FsrFg:
        FfxApiProxy::InitFfxDx12();
        cached.ready = FfxApiProxy::IsFGReady(false);
        break;
    case Target::XeFg:
        cached.ready = XeFGProxy::Module() != nullptr || XeFGProxy::InitXeFG();
        break;
    case Target::Plain:
        cached.ready = true; // no library: the plain chain is the bridge's own
        break;
    }
    return cached.ready;
}

bool IdentityFor(Class cls, LibraryIdentity& out)
{
    out = LibraryIdentity {};
    HMODULE module = nullptr;
    const char* file = "";
    switch (cls)
    {
    case Class::FsrFg:
        module = FfxApiProxy::Dx12Module_FG();
        file = "amd_fidelityfx_framegeneration_dx12.dll";
        break;
    case Class::XeFg:
        module = XeFGProxy::Module();
        file = "libxess_fg.dll";
        break;
    default:
        return false;
    }
    if (module == nullptr)
        return false;

    std::snprintf(out.file, sizeof(out.file), "%s", file);
    wchar_t path[MAX_PATH] {};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
        return false;
    out.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;

    // The PE TimeDateStamp is in the mapped image headers: no file read, no version.dll.
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    out.stamp = nt->FileHeader.TimeDateStamp;
    return true;
}

LibraryModel RowFor(Class cls)
{
    if (cls == Class::Plain)
        return LibraryModel {};

    // The same module objects are the same loaded builds: the answer is cached per handle pair, and a library that was
    // reloaded (another handle) is asked again.
    const HMODULE module = ModuleFor(cls);
    const HMODULE companionModule = cls == Class::XeFg ? GetModuleHandleW(L"libxell.dll") : nullptr;
    RowCache& cached = rowCache_[static_cast<size_t>(cls)];
    if (cached.asked && module != nullptr && cached.module == module && cached.companion == companionModule)
        return cached.model;

    LibraryIdentity identity;
    if (!IdentityFor(cls, identity))
        return LibraryModel {};

    LibraryIdentity companion;
    if (cls == Class::XeFg)
    {
        // The XeLL context the product always attaches is part of the pair a XeFG row was measured with.
        const HMODULE xell = GetModuleHandleW(L"libxell.dll");
        if (xell == nullptr)
            return LibraryModel {};
        std::snprintf(companion.file, sizeof(companion.file), "%s", "libxell.dll");
        wchar_t path[MAX_PATH] {};
        WIN32_FILE_ATTRIBUTE_DATA attributes {};
        if (GetModuleFileNameW(xell, path, MAX_PATH) == 0 ||
            !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return LibraryModel {};
        companion.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        const auto* base = reinterpret_cast<const unsigned char*>(xell);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return LibraryModel {};
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return LibraryModel {};
        companion.stamp = nt->FileHeader.TimeDateStamp;
    }

    LibraryModel model;
    Seeded().Find(cls, identity, companion, model);
    cached.asked = true;
    cached.module = module;
    cached.companion = companionModule;
    cached.model = model;
    return model;
}

Created Create(Target target, const Captured& capture)
{
    Created out;
    if (capture.factory == nullptr || capture.queue == nullptr || !capture.valid)
    {
        out.hr = static_cast<uint32_t>(E_INVALIDARG);
        return out;
    }

    auto& state = ::State::Instance();
    DXGI_SWAP_CHAIN_DESC desc = capture.desc;
    IDXGISwapChain* chain = nullptr;

    if (target == Target::Plain)
    {
        // The plain presenter is the bridge's own D3D12 flip chain, created through the one factory entry that keeps the FG
        // swapchain hook and the parent wrapping off (FactoryGuards.cpp), then registered as the plain presenter the bridge
        // presents through.
        // The unwrapped factory when the capture has one (boot's own fallback path); the wrapped factory's create
        // would return a wrapped chain, which is not the chain the bridge presents.
        void* plainFactory = capture.realFactory != nullptr ? static_cast<void*>(capture.realFactory) : capture.factory;
        void* plainWrapped = capture.realFactory != nullptr ? nullptr : capture.wrappedFactory;
        trace_ = QueueTrace { pinnedQueue_, capture.queue, WithDx12::GetD3D12CommandQueue(), nullptr };
        out.hr = static_cast<uint32_t>(Access::CreatePlainSwapChain(plainFactory, plainWrapped, capture.queue, &desc,
                                                                   reinterpret_cast<void**>(&chain)));
        trace_.interopClobbered = WithDx12::GetD3D12CommandQueue();
        if (chain != nullptr)
            NotePlainPresented(chain, capture.hwnd);
        out.chain = chain;
        out.denied = out.hr == static_cast<uint32_t>(E_ACCESSDENIED);
        return out;
    }

    // A provider chain. The configured output is what the library create reads, and the old chain pointers must be clear
    // so the library makes a fresh chain instead of handing back the one it remembers. The shell is created once per
    // target and reused by the retries (a failed attempt may already have made it).
    if (shellTarget_ != target || state.currentFG == nullptr)
    {
        shellTarget_ = target;
        state.currentFG = nullptr;
    }
    state.currentFGSwapchain = nullptr;
    state.activeFgOutput = target == Target::XeFg ? FGOutput::XeFG : FGOutput::FSRFG;

    // Boot's own call: the real factory, upstream's CreateSwapChain, and the factory's FG swapchain creation skipped for
    // the library's inner create (FactoryGuards.cpp).
    IDXGIFactory* factory = capture.realFactory != nullptr ? capture.realFactory : capture.factory;
    trace_ = QueueTrace { pinnedQueue_, capture.queue, WithDx12::GetD3D12CommandQueue(), nullptr };
    const HRESULT hr = static_cast<HRESULT>(
        Access::CreateProviderSwapChain(factory, capture.queue, &desc, reinterpret_cast<void**>(&chain)));
    trace_.interopClobbered = WithDx12::GetD3D12CommandQueue();
    out.hr = static_cast<uint32_t>(hr);
    out.chain = chain;
    out.provider = state.currentFG;
    // The queue the library put its real chain on: the next switch drains it before this provider is torn down.
    if (chain != nullptr && trace_.interopClobbered != trace_.interopBefore)
    {
        libraryProvider_ = out.provider;
        libraryQueue_ = trace_.interopClobbered;
    }
    shell_ = state.currentFG;
    out.denied = out.chain == nullptr && out.hr == static_cast<uint32_t>(E_ACCESSDENIED);
    return out;
}

void NotePlainPresented(void* chain, HWND hwnd)
{
    if (chain == nullptr)
        return;
    FGHooks::SetDx12InteropPresentSC(AsChain(chain), hwnd);
}

void DeactivateProvider(void* provider)
{
    if (auto* fg = AsProvider(provider))
        fg->Deactivate();
}

bool HasSwapchainContext(void* provider)
{
    auto* fg = AsProvider(provider);
    return fg != nullptr && fg->SwapchainContext() != nullptr;
}

bool ReleaseSwapchain(void* provider, void* hwnd)
{
    auto* fg = AsProvider(provider);
    return fg != nullptr && fg->ReleaseSwapchain(static_cast<HWND>(hwnd));
}

void ShutdownProvider(void* provider)
{
    if (auto* fg = AsProvider(provider))
        fg->Shutdown();
}

uint32_t ReleasePin(void* chain)
{
    auto* live = AsChain(chain);
    return live == nullptr ? 0u : static_cast<uint32_t>(live->Release());
}

bool SetBridgeChain(void* bridge, void* chain, void* provider)
{
    if (bridge == nullptr)
        return false;
    auto& sc = *static_cast<Dx11wDx12SC*>(bridge);
    Access::Bridge::SetFgSwapChain(sc, static_cast<IDXGISwapChain4*>(chain));
    Access::Bridge::SetFg(sc, static_cast<IFGFeature_Dx12*>(provider));
    return true;
}

bool PlaceWitness(void* chain)
{
    auto* live = AsChain(chain);
    if (live == nullptr)
        return false;
    if (witness_ == nullptr)
        witness_ = new ReleaseWitness(); // one per session: the switch owns one chain at a time
    // SetPrivateDataInterface takes its own reference on success, which is what makes the count answer afterwards.
    return SUCCEEDED(live->SetPrivateDataInterface(kWitnessGuid, witness_));
}

void ClearWitness()
{
    if (witness_ != nullptr)
        witness_->Release(); // our own reference; a chain that still holds the interface keeps the object alive
    // The object itself is never freed on purpose: a library that keeps the interface after the clear would otherwise
    // hold a pointer this module has freed, and the leak of one small object per session is the cheaper answer.
    witness_ = nullptr;
}

bool WitnessAlive()
{
    return witness_ != nullptr && witness_->Refs() > 1;
}

bool WitnessPlaceable(void* chain)
{
    // The read-only form the preflight uses: place the interface and clear it again. Nothing is released, no reference count
    // changes, and what is left behind is exactly what was there before.
    auto* live = AsChain(chain);
    if (live == nullptr)
        return false;
    if (!PlaceWitness(live))
        return false;
    live->SetPrivateDataInterface(kWitnessGuid, nullptr);
    ClearWitness();
    return true;
}
} // namespace ext::fg::outswitch::provider
