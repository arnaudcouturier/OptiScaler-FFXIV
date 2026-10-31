// The live output switch against refcounted COM fakes: the release hook, the providers' destroy, the bridge pin, the
// per-class hooks, the factory re-entry and the measured release model, in every combination the design calls out.
//
// What is modelled here, from the product's own code:
//   * the swapchain release hook: the pass-through, the "preserve" branch that swallows a last release, and the other
//     branch that releases the bridge to zero from inside the release (hooked/FG_Hooks.cpp, hkFGRelease);
//   * the bridge's own release order on the game's final reference: clear the pointers, the provider's deactivate and
//     destroy, then the pin (with_dx12/dx11_with_dx12_sc.cpp, Release + the destructor);
//   * a provider's ReleaseSwapchain: the window guard, the re-entry return, the library destroy, the global pointer it
//     clears unconditionally (framegen/ffx/FSRFG_Dx12.cpp, framegen/xefg/XeFG_Dx12.cpp);
//   * a library destroy in both measured models: M1 ends the object whatever the reference count says, M2 drops the
//     context's own reference;
//   * the per-class hooks: one class at a time, the early return when a class is already hooked, one raw slot;
//   * the FG factory's re-entry: it overwrites the global state while it creates the library's inner chain;
//   * the private-data witness, including a wrapper that forwards it to its inner chain (so a dead wrapper looks alive);
//   * the D3D12 queues (QueueRules.h): every library creates its real chain on a queue of its own and releases it in its
//     destroy, the factory re-entry points the D3D11 interop and the global queue at that queue, and the bridge's copy
//     queue is the interop's queue at construction, with no reference of the bridge's. The interop's evaluate thread and
//     the bridge's copy keep submitting on their queues between switches: a queue that died under them is a fault.
//
// The core code under test is the real one (ext/fg/outswitch/*.h). Each scenario also contains a negative control: the
// one-line defect that used to be in the first design (a release while the global pointer still names the chain, a
// skipped state restore, a read of the old chain after its teardown, a held barrier, a disabled unhook) must be caught
// by the same assertions that pass for the sequence.
#include "ext/fg/outswitch/HookLifecycle.h"
#include "ext/fg/outswitch/QueueRules.h"
#include "ext/fg/outswitch/SwitchOpsContract.h"
#include "ext/fg/outswitch/SwitchSequence.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("      FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr int kSlotRelease = 0;
constexpr int kSlotPresent = 1;
constexpr int kSlotSetFullscreen = 2;
constexpr int kSlotGetFullscreen = 3;
constexpr int kSlotGetFullscreenDesc = 4;
constexpr int kSlotResizeBuffers = 5;
constexpr int kSlotResizeTarget = 6;
constexpr int kSlotPresent1 = 7;
constexpr int kSlotWaitable = 8;
constexpr int kSlotResizeBuffers1 = 9;
constexpr int kSlots = 10;

constexpr uint32_t kGenericFailure = 0x80004005u;

enum class Interop : uint32_t
{
    None,
    Dx11wDx12
};

struct Witness
{
    bool ownerReleased = false; // the chain (or its inner chain) dropped its reference
};

// A D3D12 command queue: its references, and whether the last one went.
struct Queue
{
    uint32_t refs = 1;
    bool freed = false;
    const char* name = "";
};

struct Chain
{
    Class cls = Class::Plain;
    uint32_t refs = 1;
    bool libraryRef = false; // M2: the provider's context holds one reference
    bool freed = false;
    bool forwardsWitness = false;
    Witness* witness = nullptr;
    Chain* inner = nullptr;
    uint32_t testResult = kOk;
    uint32_t presentResult = kOk;
    int presents = 0;
    Queue* queue = nullptr; // a plain chain's queue: the chain holds one reference on it
};

struct Provider
{
    Class cls = Class::FsrFg;
    Chain* chain = nullptr; // stale after the destroy, like the FSR shell
    void* hwnd = nullptr;
    bool swapchainContext = false;
    bool active = true;
    bool shutdown = false;
    bool releaseRefused = false;
    int releaseCalls = 0;
    Queue* ownQueue = nullptr;  // the queue the library created its real chain on: the library's only, released in its destroy
    Queue* gameQueue = nullptr; // the queue the provider was created with: the library holds one reference on it
};

struct Bridge
{
    uint32_t refs = 1;
    bool deleted = false;
    Chain* fgChain = nullptr;
    Provider* provider = nullptr;
};

// The globals a switch must save and put back: the FG factory's own re-entry overwrites all of them.
struct Globals
{
    void* currentSwapchain = nullptr;
    void* currentWrappedSwapchain = nullptr;
    void* currentRealSwapchain = nullptr;
    void* currentFGSwapchain = nullptr;
    void* currentFG = nullptr;
    void* interopPresent = nullptr;
    void* currentD3D12Device = nullptr;
    void* currentCommandQueue = nullptr;
    Interop interopApi = Interop::Dx11wDx12;
    bool allowTearing = false;
    uint32_t lastFlags = 0;
    bool realExclusiveFullscreen = false;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    uint32_t descWidth = 0;
    uint32_t descHeight = 0;
};

struct Hooks
{
    bool attached[kSlots] {};
    bool trampoline[kSlots] {};
    Class cls = Class::Plain;
    int attaches = 0;
    int detaches = 0;
    int dispatch = 0;
    int noDispatch = 0;
    int detachedSlots = 0;
    // A thread inside a hook body right now, and the gate that keeps its trampolines alive (HookLifecycle.h).
    HookGate gate;
    int detachBlocked = 0;
    int passes = 0;   // a body that reached the pass-through path and called its slot's original
    int refused = 0;  // a body that arrived after the originals were restored and called nothing
    int clearedWhileBusy = 0;
    bool stuck = false; // a detach could not drain: the old class stays patched as dead code
    // Upstream's HookFGSwapchain reopens the gate as its last statement; this is that behaviour, and the knob is the
    // product without it (the state a switch must not end in: every body refused, presents answered S_OK, nothing on
    // screen).
    bool reopenGateOnRehook = true;
};

struct World
{
    // Configuration of one run.
    Model libraryModel = Model::M2;
    Model tableModel = Model::M2;
    bool witnessHonoured = true;
    // The order the table's row was measured with (LibraryModel.h): a row authorises its own order only.
    bool pinFirstRow = false;
    // A provider whose context is destroyed before the caller's reference goes (XeFG): the destroy drops its handle and
    // leaves the context allocated, so the other order leaks one context per switch and the pin-first order does not.
    bool contextRefusesWhileHeld = false;
    // A destroy that answers success and frees nothing (the context kept the chain): the teardown must see the object
    // still there and stop instead of trying again.
    bool destroyLeavesChain = false;
    // A build whose frame-generation context holds no reference of its own: the caller's release is the last one, so a
    // pin-first row that assumed a context reference frees the chain with that release. The post-pin witness must catch it.
    bool createWithoutLibraryRef = false;
    int contextLeaks = 0;
    int secondDestroys = 0;
    bool forwardsWitness = false;
    bool witnessRefused = false;         // SetPrivateDataInterface fails on the chain the teardown asks
    bool witnessPreflightRefused = false; // the read-only place-and-clear in ReadPreflight failed
    bool preserve = true;
    bool failDestroy = false;
    bool releaseRefused = false;
    bool providerCreateFails = false;
    int deniedTimes = 0;
    bool hooksLeavePresentNull = false;
    bool disableUnhook = false;
    bool xeFgSwitchAllowed = true; // the core exercises both paths; the product refuses any XeFG switch
    bool skipRestore = false;
    bool replayFails = false;
    bool testFails = false;
    bool adoptFails = false;
    bool drainOk = true;
    bool enterHookBodyAtDrain = false; // a body arrives after the preflight looked, so the unhook is the one that finds it
    bool nativePresentInside = false;
    bool holdBarrier = false;
    bool barrierBlocks = false;
    bool isShuttingDown = false;
    bool windowed = true;
    bool loadersOk = true;
    bool hooksConsistent = true;
    bool hooksTransactable = true; // another thread's Detours transaction is not open
    bool plainCreateFails = false;
    Target target = Target::Plain;
    Target active = Target::Plain;
    bool targetKnown = true;
    bool wanted = true;            // Ops::Wanted(): the feature key
    bool readLive = true;          // Ops::ReadLive() can name the boot chain
    bool clearOldSwapChain = true; // the switch clears the release hook's stale pointer at the detach
    bool libraryReleaseThroughHook = true; // the library's own releases run the patched Release, as a code patch makes them
    int extraHolders = 0;          // an extra COM holder of the chain (an overlay, a recorder, the HDR module)

    // Live objects (owned for the whole run; `freed` marks logical death, like a deleted COM object).
    std::vector<std::unique_ptr<Chain>> chains;
    std::vector<std::unique_ptr<Provider>> providers;
    std::vector<std::unique_ptr<Witness>> witnesses;
    std::vector<std::unique_ptr<Queue>> queueObjects;
    // The D3D11 interop's queue (the upscaler's evaluate thread and the sync objects submit there), the bridge's copy queue
    // (no reference of the bridge's) and the switch's own reference.
    Queue* interopQueue = nullptr;
    Queue* bridgeQueue = nullptr;
    Queue* pinnedQueue = nullptr;
    std::vector<std::string> queueFaults;
    Globals globals;
    Bridge bridge;
    Hooks hooks;
    Chain* windowChain = nullptr;
    Chain* oldSwapChain = nullptr;
    void* window = reinterpret_cast<void*>(0x1234);
    void* bridgePointer = nullptr; // what the module recorded, as the configure seam does
    uint64_t clock = 1000;

    // Observations.
    std::vector<std::string> useAfterFree;
    std::vector<Event> events;
    Status status;
    int h1Preserved = 0;
    int h1Nuclear = 0;
    int deadlocks = 0;
    int createRefused = 0;
    bool barrierHeld = false;
    int retired = 0;
    int pinReleases = 0;
    int publishes = 0;
    int refusesReported = 0;
    int nullTrampoline = 0;
    std::vector<std::string> order; // the calls the teardown made, to pin their order
    int oldSwapChainHits = 0;

    int capturesOfFreed = 0;
    int destroyCalls = 0;
    int creates = 0;
    int teardownWhileAttached = 0; // the provider destroy ran while the bridge still named the chain
    int destroyedBridge = 0;
    int clobbers = 0;

    // ---- objects ---------------------------------------------------------------------------------------------------
    Chain* NewChain(Class cls)
    {
        chains.push_back(std::make_unique<Chain>());
        Chain* chain = chains.back().get();
        chain->cls = cls;
        chain->forwardsWitness = forwardsWitness;
        return chain;
    }

    Provider* NewProvider(Class cls, Chain* chain)
    {
        providers.push_back(std::make_unique<Provider>());
        Provider* provider = providers.back().get();
        provider->cls = cls;
        provider->chain = chain;
        provider->hwnd = window;
        provider->swapchainContext = true;
        return provider;
    }

    Witness* NewWitness()
    {
        witnesses.push_back(std::make_unique<Witness>());
        return witnesses.back().get();
    }

    Queue* NewQueue(const char* name)
    {
        queueObjects.push_back(std::make_unique<Queue>());
        Queue* queue = queueObjects.back().get();
        queue->name = name;
        return queue;
    }

    // A use of a queue by something that keeps it: a chain or a provider created on it.
    void QueueAddRef(Queue* queue, const char* use)
    {
        if (queue == nullptr)
            return;
        if (queue->freed)
        {
            queueFaults.push_back(std::string(use) + " on the freed queue " + queue->name);
            return;
        }
        ++queue->refs;
    }

    void QueueRelease(Queue* queue)
    {
        if (queue == nullptr)
            return;
        if (queue->freed)
        {
            queueFaults.push_back(std::string("a release of the freed queue ") + queue->name);
            return;
        }
        if (--queue->refs == 0)
            queue->freed = true;
    }

    // A submission on a queue by something that holds no reference of its own.
    void QueueUse(Queue* queue, const char* use)
    {
        if (queue != nullptr && queue->freed)
            queueFaults.push_back(std::string(use) + " on the freed queue " + queue->name);
    }

    // The interop's own thread between two presents: it submits on the interop queue, and asking the interop for its
    // objects moves the global queue to the interop's (the product's UpdateStateObjects).
    void InteropTick()
    {
        QueueUse(interopQueue, "the interop's evaluate thread submits");
        globals.currentCommandQueue = interopQueue;
    }

    void Free(Chain* chain)
    {
        if (chain->freed)
        {
            useAfterFree.push_back("double free");
            return;
        }
        chain->freed = true;
        chain->refs = 0;
        if (chain->witness != nullptr)
        {
            chain->witness->ownerReleased = true;
            chain->witness = nullptr;
        }
        if (chain->queue != nullptr)
        {
            QueueRelease(chain->queue);
            chain->queue = nullptr;
        }
        if (windowChain == chain)
            windowChain = nullptr;
    }

    // The defect the sequence must not have: the provider destroy and the pin release run while the global FG chain
    // pointer still names the chain. With a preserve configuration the release is swallowed (a leak); without one the
    // bridge is released to zero from inside the release (deleted inside a present).
    void EmissiveTeardown(const LiveChain& live)
    {
        Chain* chain = static_cast<Chain*>(live.chain);
        globals.currentFGSwapchain = chain; // the pointer a switch that never cleared it would leave
        chain->refs = 1;                    // the bridge's pin and nothing else
        HookRelease(chain);
    }

    // ---- reference counting ----------------------------------------------------------------------------------------
    uint32_t AddRef(Chain* chain)
    {
        if (chain->freed)
        {
            useAfterFree.push_back("AddRef");
            return 0;
        }
        return ++chain->refs;
    }

    uint32_t Release(Chain* chain)
    {
        if (chain->freed)
        {
            useAfterFree.push_back("Release");
            return 0;
        }
        if (--chain->refs == 0)
        {
            Free(chain);
            return 0;
        }
        return chain->refs;
    }

    // A release the library itself makes while it destroys the context: in the product the Release is a code patch, so
    // every internal release runs the hook. The sim can make it either way, and both must end in the same place once the
    // global pointer has been cleared.
    uint32_t LibraryRelease(Chain* chain)
    {
        if (libraryReleaseThroughHook)
            return HookRelease(chain);
        return Release(chain);
    }

    // hkFGRelease, transcribed: the oldSwapChain branch, the pass-through, and the two branches for a last release while
    // the global pointer still names this chain.
    uint32_t HookRelease(Chain* chain)
    {
        if (chain->freed)
        {
            useAfterFree.push_back("Release through the hook");
            return 0;
        }
        // The first branch of the real hook: a chain the game created while the module still pointed at another one.
        // Nothing is released, which is why a stale pointer of this kind makes a chain leak at a recycled address.
        if (oldSwapChain == chain)
        {
            ++oldSwapChainHits;
            return 0;
        }
        if (hooks.cls == Class::Plain || !hooks.attached[kSlotRelease] || globals.currentFGSwapchain != chain ||
            isShuttingDown)
            return Release(chain);

        AddRef(chain);
        const uint32_t refs = Release(chain);
        if (!preserve)
        {
            if (refs == 1)
            {
                // GPU wait, backbuffer release loop, the provider's destroy, then the bridge released to zero.
                ++h1Nuclear;
                if (bridge.provider != nullptr)
                    ProviderReleaseSwapchain(*bridge.provider);
                globals.currentFGSwapchain = nullptr;
                while (!bridge.deleted)
                    BridgeRelease();
                return 0;
            }
            return Release(chain);
        }
        if (refs == 1)
        {
            ++h1Preserved; // the release is swallowed and the chain stays alive: the leak of the first design
            return 0;
        }
        return Release(chain);
    }

    // The bridge's own release order when the game drops its last reference.
    uint32_t BridgeRelease()
    {
        if (bridge.deleted)
        {
            useAfterFree.push_back("bridge released twice");
            return 0;
        }
        if (--bridge.refs > 0)
            return bridge.refs;

        globals.currentSwapchain = nullptr;
        globals.currentWrappedSwapchain = nullptr;
        globals.currentRealSwapchain = nullptr;
        if (globals.currentFGSwapchain == bridge.fgChain)
            globals.currentFGSwapchain = nullptr;
        if (bridge.provider != nullptr)
            ProviderReleaseSwapchain(*bridge.provider);

        Chain* pinned = bridge.fgChain;
        bridge.fgChain = nullptr;
        bridge.provider = nullptr;
        bridge.deleted = true;
        ++destroyedBridge;
        if (pinned != nullptr)
            HookRelease(pinned);
        return 0;
    }

    // A provider's ReleaseSwapchain, transcribed (window guard, re-entry return, the library destroy, the global
    // pointer it clears unconditionally).
    bool ProviderReleaseSwapchain(Provider& provider)
    {
        // The bridge must not point at the chain while its provider destroys it: the sequence detaches first, so other
        // threads see the plain fallbacks and a present cannot reach the chain mid-destroy.
        if (bridge.fgChain != nullptr && bridge.fgChain == provider.chain)
            ++teardownWhileAttached;
        // A second destroy of one provider's context: on the real library that is an access violation after the first
        // call freed the chain, so a sequence that repeats it must fail a test instead of merely looking odd.
        if (++provider.releaseCalls > 1)
            ++secondDestroys;
        order.push_back("destroy");
        if (provider.swapchainContext && (provider.hwnd != window || provider.hwnd == nullptr))
            return false;
        if (releaseRefused || provider.releaseRefused)
            return false;

        // The barrier case: the destroy needs the provider's own present thread to stop; a held resize lock would
        // wait for it forever.
        if (barrierHeld && nativePresentInside)
        {
            ++deadlocks;
            return false;
        }

        if (provider.active)
        {
            provider.active = false; // Deactivate() inside the destroy
        }
        if (provider.swapchainContext)
        {
            ++destroyCalls;
            // A context destroyed while the caller still holds the chain (XeFG) refuses: the library answers
            // POINTER_STILL_IN_USE, the wrapper drops the handle and answers success, and the context is allocated with
            // nobody left to destroy it. The chain itself still ends with the caller's release, which is why the window
            // reads free either way in the destroy-first measurement.
            if (contextRefusesWhileHeld && provider.chain != nullptr && provider.chain->refs > 1)
                ++contextLeaks;
            if (!failDestroy && destroyLeavesChain)
            {
                // The library answered success and the object survived: only the witness says so.
            }
            else if (!failDestroy)
            {
                if (libraryModel == Model::M1)
                    Free(provider.chain);
                else if (provider.chain->libraryRef)
                {
                    provider.chain->libraryRef = false;
                    LibraryRelease(provider.chain);
                }
            }
            provider.swapchainContext = false;
            globals.currentFGSwapchain = nullptr;
            if (!failDestroy)
            {
                // The library's own queue goes with it (nothing else holds it), and so does its reference on the game
                // queue it was created with.
                QueueRelease(provider.ownQueue);
                QueueRelease(provider.gameQueue);
                provider.ownQueue = nullptr;
                provider.gameQueue = nullptr;
            }
        }
        return true;
    }

    // ---- the FG factory, its guards and its re-entry ---------------------------------------------------------------
    void FactoryReentry(Chain*& innerReal, Chain*& innerWrapped, Queue* libraryQueue)
    {
        ++clobbers;
        // prologue
        globals.allowTearing = true;
        globals.lastFlags = 0x2;
        globals.realExclusiveFullscreen = false;
        // D3D12 queue branch: the library's own queue becomes the global queue and the D3D11 interop's queue
        globals.currentD3D12Device = &globals;
        globals.currentCommandQueue = libraryQueue;
        interopQueue = libraryQueue;
        // "coming from the FG library" tail
        globals.interopApi = Interop::None;
        innerReal = NewChain(Class::Plain);
        innerWrapped = NewChain(Class::Plain);
        globals.currentRealSwapchain = static_cast<void*>(innerReal);
        globals.currentWrappedSwapchain = static_cast<void*>(innerWrapped);
        if (globals.currentFGSwapchain != nullptr)
            oldSwapChain = static_cast<Chain*>(globals.currentFGSwapchain); // the static nothing clears: the switch must never set it
    }

    void HookFor(Chain* chain)
    {
        // The binding opens the gate as soon as a class is hooked, whether this call attaches the set or finds it already
        // attached after an abandoned detach: a gate left shut would make every body take the pass-through path for the
        // rest of the session (the contract's ownership rule, not the test's job to remember). Upstream's own
        // HookFGSwapchain does it with one statement at its end, on the attach-failure path too.
        if (hooks.reopenGateOnRehook)
            hooks.gate.Reattached();
        if (hooks.attached[kSlotRelease])
            return; // one class at a time: the hook set already exists
        ++hooks.attaches;
        hooks.stuck = false;
        for (int i = 0; i < kSlots; ++i)
        {
            hooks.attached[i] = true;
            hooks.trampoline[i] = true;
        }
        hooks.trampoline[kSlotSetFullscreen] = false; // a raw address for one of the extras
        if (hooksLeavePresentNull)
            hooks.attached[kSlotPresent] = false;
        hooks.cls = chain->cls;
    }

    // The defect the gate exists for: the originals are cleared while a thread is inside a hook body. A body that is
    // already running then calls through a null pointer.
    void ClearHooksWhileBusy()
    {
        ++hooks.clearedWhileBusy;
        nullTrampoline += static_cast<int>(hooks.gate.InFlight());
        for (int i = 0; i < kSlots; ++i)
            hooks.attached[i] = false;
        hooks.cls = Class::Plain;
    }

    UnhookResult UnhookHooks()
    {
        ++hooks.detaches;
        if (!hooksConsistent)
        {
            hooks.gate.Reattached(); // nothing was detached: the gate opens again (the contract's ownership rule)
            return UnhookResult::Failed; // all or nothing
        }
        if (disableUnhook)
            return UnhookResult::Ok; // the defect: the old class stays patched, so the new class is never hooked

        // The detach drains both gate phases before it restores anything: a counted body that is still running, and a
        // pass-through that is still calling an original, would otherwise call through a freed trampoline. The wait is
        // the gate's own (HookLifecycle.h), bounded by kHookDrainTimeoutMs on the sim's clock.
        if (!hooks.gate.DetachFor(kHookDrainTimeoutMs, [this] { return clock; }, [this](uint32_t ms) { clock += ms; }))
        {
            ++hooks.detachBlocked; // the wait expired: the old class stays patched as dead code
            hooks.gate.Reattached(); // an abandoned detach reopens the gate before Unhook() returns
            hooks.stuck = true;
            return UnhookResult::Busy;
        }
        if (!hooks.gate.Complete())
        {
            // The gate did not confirm the drain: nothing is restored.
            ++hooks.clearedWhileBusy;
            nullTrampoline += static_cast<int>(hooks.gate.InFlight() + hooks.gate.Passing());
            hooks.gate.Reattached();
            hooks.stuck = true;
            return UnhookResult::Failed;
        }
        hooks.detachedSlots = 0;
        for (int i = 0; i < kSlots; ++i)
        {
            if (hooks.attached[i] && hooks.trampoline[i])
                ++hooks.detachedSlots;
            hooks.attached[i] = false;
        }
        hooks.cls = Class::Plain;
        hooks.stuck = false;
        return UnhookResult::Ok;
    }

    uint32_t PresentOn(Chain* chain)
    {
        if (chain->freed)
        {
            useAfterFree.push_back("present on a freed chain");
            return kDeviceRemoved;
        }
        ++chain->presents;
        if (hooks.cls == chain->cls && hooks.attached[kSlotPresent])
        {
            const GateEntry entry = hooks.gate.Enter();
            if (entry == GateEntry::PassThrough)
            {
                // A detach holds the gate but its originals are still valid: the body calls its slot's original (here
                // the provider's own present) and returns its result, and the detach waits for it.
                ++hooks.passes;
                hooks.gate.PassDone();
                return chain->presentResult;
            }
            if (entry == GateEntry::Refused)
            {
                // The originals are restored: the body calls nothing and returns the fixed answer the slot defines.
                ++hooks.refused;
                return kOk;
            }
            ++hooks.dispatch;
            hooks.gate.Leave();
            return chain->presentResult;
        }
        ++hooks.noDispatch;
        return chain->presentResult;
    }

    // ---- clamping and comparing the globals ------------------------------------------------------------------------
    bool SameGlobals(const Globals& a, const Globals& b, std::string& diff) const
    {
        const std::pair<const char*, bool> fields[] = {
            { "currentSwapchain", a.currentSwapchain == b.currentSwapchain },
            { "currentWrappedSwapchain", a.currentWrappedSwapchain == b.currentWrappedSwapchain },
            { "currentRealSwapchain", a.currentRealSwapchain == b.currentRealSwapchain },
            { "currentD3D12Device", a.currentD3D12Device == b.currentD3D12Device },
            { "currentCommandQueue", a.currentCommandQueue == b.currentCommandQueue },
            { "interopApi", a.interopApi == b.interopApi },
            { "allowTearing", a.allowTearing == b.allowTearing },
            { "lastFlags", a.lastFlags == b.lastFlags },
            { "realExclusiveFullscreen", a.realExclusiveFullscreen == b.realExclusiveFullscreen },
            { "screenWidth", a.screenWidth == b.screenWidth },
            { "screenHeight", a.screenHeight == b.screenHeight },
            { "descWidth", a.descWidth == b.descWidth },
            { "descHeight", a.descHeight == b.descHeight },
        };
        for (const auto& field : fields)
        {
            if (!field.second)
            {
                diff = field.first;
                return false;
            }
        }
        return true;
    }
};

// ---- the Ops binding ---------------------------------------------------------------------------------------------

struct SimOps
{
    struct Capture
    {
        void* hwnd = nullptr;
        Chain* chain = nullptr;
        bool fromBridge = false;
        Queue* queue = nullptr; // the queue the next chain is created on (QueueRules.h)
    };
    struct StateSnapshot
    {
        Globals globals;
        Queue* interop = nullptr;
    };
    struct CreateResult
    {
        void* chain = nullptr;
        void* provider = nullptr;
        uint32_t hr = 0;
        bool denied = false;
    };

    World& world;
    Target lastTarget = Target::Plain;
    Class lastClass = Class::Plain;
    Provider* lastProvider = nullptr; // Create is retry-idempotent: the same provider object comes back
    Witness* witness = nullptr;

    uint64_t NowMs() { return world.clock; }
    void SleepMs(uint32_t ms) { world.clock += ms; }

    // The binding's whole idle cost, exactly as the contract states it: the key is on and the configured output is not
    // the active one. After a commit those two are equal, which is why the judgement of the live chain runs first.
    bool Wanted() { return world.wanted && world.target != world.active; }

    // The boot chain the module never created: the bridge's chain plus its provider, with the class of the active output.
    bool ReadLive(LiveChain& out)
    {
        if (!world.readLive)
            return false;
        Chain* chain = world.bridge.fgChain;
        if (chain == nullptr)
            return false;
        out = LiveChain { chain, world.bridge.provider, ClassOf(world.active), world.window };
        return !out.Empty();
    }

    PreflightInputs ReadPreflight()
    {
        PreflightInputs inputs;
        inputs.keyOn = true;
        inputs.target = world.target;
        inputs.targetKnown = world.targetKnown;
        inputs.active = world.active;
        inputs.activeKnown = true;
        inputs.bridgePresent = true;
        inputs.shuttingDown = world.isShuttingDown;
        inputs.windowed = world.windowed;
        inputs.waitable = false;
        inputs.gameHdr = false;
        inputs.optiHdr = false;
        inputs.loadersOk = world.loadersOk;
        inputs.hooksOk = world.hooksConsistent;
        // The read-only peek the switch does before it touches anything: the same two counters the unhook waits for.
        inputs.hooksIdle = world.hooks.gate.InFlight() == 0 && world.hooks.gate.Passing() == 0;
        inputs.canCapture = world.windowChain != nullptr;
        inputs.witnessUsable = !world.witnessPreflightRefused;
        inputs.secondBridge = false;
        inputs.retiredShells = static_cast<uint32_t>(world.retired);
        inputs.xeFgSwitchAllowed = world.xeFgSwitchAllowed;
        if (world.active != Target::Plain)
            inputs.activeModel = LibraryModel { world.tableModel, world.witnessHonoured, world.pinFirstRow };
        if (world.target != Target::Plain)
            inputs.targetModel = LibraryModel { world.tableModel, world.witnessHonoured, world.pinFirstRow };
        return inputs;
    }

    bool Barrier()
    {
        if (world.barrierBlocks)
            return false;
        if (world.holdBarrier)
            world.barrierHeld = true; // the defect under test: the lock would stay held across the teardown
        return true;
    }
    void Quiesce(void* provider)
    {
        if (provider != nullptr)
            static_cast<Provider*>(provider)->active = false;
    }
    void Resume(void* provider)
    {
        if (provider != nullptr)
            static_cast<Provider*>(provider)->active = true;
    }
    DrainResult Drain(void* provider)
    {
        DrainResult result;
        result.game = world.drainOk;
        result.fg = world.drainOk;
        if (world.enterHookBodyAtDrain)
        {
            // A hook body that started after the read-only preflight peek (a present that arrived during the barrier):
            // only the unhook's own wait can see it.
            world.enterHookBodyAtDrain = false;
            CHECK(world.hooks.gate.Enter() == GateEntry::Counted);
        }
        return result;
    }

    bool ReadCapture(void* handle, Capture& out)
    {
        Chain* chain = static_cast<Chain*>(handle);
        if (chain == nullptr || chain->freed)
        {
            world.useAfterFree.push_back("GetDesc/GetParent after the teardown");
            ++world.capturesOfFreed;
            if (chain != nullptr)
                ++chain->presents; // poison: any call on a freed chain is a use after free
            return false;
        }
        out.hwnd = world.window;
        out.chain = chain;
        out.fromBridge = false;
        return ChooseQueue(out);
    }
    bool ReadBridgeCapture(Capture& out)
    {
        if (world.globals.currentRealSwapchain == nullptr)
            return false;
        out.hwnd = world.window;
        out.chain = static_cast<Chain*>(world.globals.currentRealSwapchain);
        out.fromBridge = true;
        return ChooseQueue(out);
    }
    // The product's capture (ProviderOps.cpp): the pin is taken at the first capture, before anything is torn down, and
    // the creation queue follows the shared rule. No queue: the capture fails and nothing is touched.
    bool ChooseQueue(Capture& out)
    {
        if (queues::kPinBridgeQueue && queues::RepinNeeded(world.pinnedQueue, world.bridgeQueue))
        {
            // The new reference first, the old one after (the product's order): the two can be the same queue.
            world.QueueAddRef(world.bridgeQueue, "the switch's pin");
            world.QueueRelease(world.pinnedQueue);
            world.pinnedQueue = world.bridgeQueue;
        }
        out.queue = static_cast<Queue*>(queues::CreationQueue(world.pinnedQueue, world.globals.currentCommandQueue));
        return out.queue != nullptr;
    }
    void ReleaseCapture(Capture& out) {}

    StateSnapshot Snapshot()
    {
        StateSnapshot snapshot;
        snapshot.globals = world.globals;
        snapshot.interop = world.interopQueue;
        return snapshot;
    }
    void RestoreState(const StateSnapshot& snapshot)
    {
        if (world.skipRestore)
            return;
        // The interop first, the globals after: the product's interop setter writes the global queue too.
        world.interopQueue = static_cast<Queue*>(queues::InteropAfterCreate(snapshot.interop, world.interopQueue));
        world.globals.currentSwapchain = snapshot.globals.currentSwapchain;
        world.globals.currentWrappedSwapchain = snapshot.globals.currentWrappedSwapchain;
        world.globals.currentRealSwapchain = snapshot.globals.currentRealSwapchain;
        world.globals.interopPresent = snapshot.globals.interopPresent;
        world.globals.interopApi = snapshot.globals.interopApi;
        world.globals.allowTearing = snapshot.globals.allowTearing;
        world.globals.lastFlags = snapshot.globals.lastFlags;
        world.globals.realExclusiveFullscreen = snapshot.globals.realExclusiveFullscreen;
        world.globals.currentD3D12Device = snapshot.globals.currentD3D12Device;
        world.globals.currentCommandQueue = snapshot.globals.currentCommandQueue;
        world.globals.screenWidth = snapshot.globals.screenWidth;
        world.globals.screenHeight = snapshot.globals.screenHeight;
        world.globals.descWidth = snapshot.globals.descWidth;
        world.globals.descHeight = snapshot.globals.descHeight;
    }

    void ReleaseInterop(void* hwnd) { world.globals.interopPresent = nullptr; }
    void Detach()
    {
        world.bridge.fgChain = nullptr;
        world.bridge.provider = nullptr;
    }
    void ClearOldSwapChain()
    {
        if (world.clearOldSwapChain && world.oldSwapChain != nullptr)
        {
            world.oldSwapChain = nullptr;
            world.order.push_back("clear-old-swapchain");
        }
    }

    UnhookResult Unhook() { return world.UnhookHooks(); }
    bool HooksTransactable() { return world.hooksTransactable; }
    bool HooksAttached(Target target)
    {
        if (target == Target::Plain)
            return true; // a plain presenter is not hooked, and nothing dispatches through the FG hook slots
        return world.hooks.cls == ClassOf(target) && world.hooks.attached[kSlotPresent];
    }

    CreateResult Create(Target target, const Capture& capture)
    {
        ++world.creates;
        lastTarget = target;
        lastClass = ClassOf(target);
        CreateResult result;

        // The switch owns no chain at this point: the bridge pointer, the provider global and the release hook's stale
        // pointer must all be clear. A library that finds one answers with the existing chain instead of a new one, so a
        // create that runs anyway would hand back a chain the sequence does not own.
        if (world.globals.currentFG != nullptr || world.globals.currentFGSwapchain != nullptr ||
            world.oldSwapChain != nullptr)
        {
            ++world.createRefused;
            result.hr = kAccessDenied;
            result.denied = true;
            result.provider = lastProvider;
            return result;
        }

        // Retry-idempotent, as the contract asks: a denied attempt hands back the provider object the previous attempt
        // created instead of a fresh one, so the retry loop cannot leak a shell.
        if (world.deniedTimes > 0)
        {
            --world.deniedTimes;
            result.hr = kAccessDenied;
            result.denied = true;
            result.provider = lastProvider;
            return result;
        }
        if (world.windowChain != nullptr)
        {
            result.hr = kAccessDenied;
            result.denied = true;
            result.provider = lastProvider;
            return result;
        }
        if (world.providerCreateFails && target != Target::Plain)
        {
            result.hr = kGenericFailure;
            return result;
        }
        if (world.plainCreateFails && target == Target::Plain)
        {
            result.hr = kGenericFailure;
            return result;
        }

        if (target != Target::Plain)
        {
            Chain* innerReal = nullptr;
            Chain* innerWrapped = nullptr;
            Queue* own = world.NewQueue(lastClass == Class::XeFg ? "xefg's own" : "fsrfg's own");
            world.QueueAddRef(capture.queue, "a provider created");
            world.FactoryReentry(innerReal, innerWrapped, own);

            Chain* chain = world.NewChain(lastClass);
            chain->refs = world.createWithoutLibraryRef ? 1 : 2; // the creation reference plus the context's own
            chain->refs += world.extraHolders;
            chain->libraryRef = !world.createWithoutLibraryRef;
            chain->inner = innerWrapped;
            Provider* provider = world.NewProvider(lastClass, chain);
            provider->ownQueue = own;
            provider->gameQueue = capture.queue;
            lastProvider = provider;
            world.windowChain = chain;
            world.globals.currentFGSwapchain = chain;
            // FGHooks::SetFGSwapchain writes the global that names the chain on the window too: the library's own create
            // leaves its inner chain there, and the switch's restore is what puts the bridge back.
            world.globals.currentSwapchain = chain;
            world.globals.currentFG = provider;
            world.HookFor(chain);
            result.chain = chain;
            result.provider = provider;
            result.hr = kOk;
            return result;
        }

        Chain* chain = world.NewChain(Class::Plain);
        chain->refs = 1;
        world.QueueAddRef(capture.queue, "a plain chain created");
        chain->queue = capture.queue;
        world.windowChain = chain;
        world.globals.interopPresent = chain;
        result.chain = chain;
        result.hr = kOk;
        return result;
    }

    bool Adopt(void* chainHandle, void* providerHandle, Target target)
    {
        if (world.adoptFails && target != Target::Plain)
            return false;
        Chain* chain = static_cast<Chain*>(chainHandle);
        if (chain->freed)
        {
            world.useAfterFree.push_back("adopt");
            return false;
        }
        world.bridge.fgChain = chain;
        world.bridge.provider = static_cast<Provider*>(providerHandle);
        world.globals.currentSwapchain = &world.bridge;
        world.globals.currentWrappedSwapchain = &world.bridge;
        if (target == Target::Plain)
            world.globals.interopPresent = chain;
        return true;
    }

    bool Replay(void* chainHandle)
    {
        return static_cast<Chain*>(chainHandle)->cls == Class::Plain || !world.replayFails;
    }
    uint32_t TestPresent(void* chainHandle)
    {
        Chain* chain = static_cast<Chain*>(chainHandle);
        if (chain->freed)
        {
            world.useAfterFree.push_back("test present on a freed chain");
            return kDeviceRemoved;
        }
        ++chain->presents;
        if (world.hooks.cls == chain->cls && world.hooks.attached[kSlotPresent])
            ++world.hooks.dispatch;
        else
            ++world.hooks.noDispatch;
        if (world.testFails && chain->cls != Class::Plain)
            return kGenericFailure;
        return chain->testResult;
    }

    // ChainLifetime.h
    void Deactivate(void* provider)
    {
        world.order.push_back("deactivate");
        if (provider != nullptr)
            static_cast<Provider*>(provider)->active = false;
    }
    void ClearChainRefs(const LiveChain& live)
    {
        world.order.push_back("clear-refs");
        Chain* chain = static_cast<Chain*>(live.chain);
        if (world.globals.currentFGSwapchain == chain)
            world.globals.currentFGSwapchain = nullptr;
        if (world.globals.interopPresent == chain)
            world.globals.interopPresent = nullptr;
        if (world.globals.currentWrappedSwapchain != &world.bridge)
            world.globals.currentWrappedSwapchain = &world.bridge;
        world.globals.currentSwapchain = &world.bridge;
    }
    bool HasSwapchainContext(void* provider)
    {
        return provider != nullptr && static_cast<Provider*>(provider)->swapchainContext;
    }
    bool ReleaseSwapchain(void* provider, void* hwnd)
    {
        if (provider == nullptr)
            return true;
        return world.ProviderReleaseSwapchain(*static_cast<Provider*>(provider));
    }
    uint32_t ReleasePin(void* chainHandle)
    {
        world.order.push_back("pin-release");
        ++world.pinReleases;
        return world.HookRelease(static_cast<Chain*>(chainHandle));
    }
    bool SetWitness(void* chainHandle)
    {
        Chain* chain = static_cast<Chain*>(chainHandle);
        if (chain->freed)
        {
            world.useAfterFree.push_back("SetPrivateDataInterface after the teardown");
            return false;
        }
        if (world.witnessRefused)
        {
            // The library (or a wrapper in front of it) refuses the interface on this chain. The row was measured with a
            // witness the library accepted, so the cross-check the row leans on does not exist here.
            return false;
        }
        witness = world.NewWitness();
        // A wrapper that forwards the interface to its inner chain: the witness then dies with the inner chain, so the
        // outer object looks alive after its own death.
        if (chain->forwardsWitness && chain->inner != nullptr)
            chain->inner->witness = witness;
        else
            chain->witness = witness;
        return true;
    }
    void ReleaseWitness() { witness = nullptr; }
    bool WitnessAlive() { return witness != nullptr && !witness->ownerReleased; }
    void ShutdownProvider(void* provider)
    {
        if (provider != nullptr)
            static_cast<Provider*>(provider)->shutdown = true;
    }
    void RetireProvider(void* provider)
    {
        if (provider != nullptr)
            ++world.retired;
    }
    void ClearCurrentProvider() { world.globals.currentFG = nullptr; }

    void Report(const Event& event)
    {
        if (event.step == Step::Refused)
            ++world.refusesReported;
        world.events.push_back(event);
    }
    void Publish(const Status& snapshot)
    {
        ++world.publishes;
        world.status = snapshot;
    }
};

// The contract is frozen: the fakes must keep satisfying every call the core makes, or this file stops compiling.
static_assert(ext::fg::outswitch::SwitchOps<SimOps>, "SimOps no longer matches the switch's Ops contract");

// ---- a harness: the boot chain, the switch calls and the invariants after every step ----------------------------

struct Harness
{
    World world;
    SwitchState state;
    SimOps ops { world };
    Globals boot;
    Chain* bootChain = nullptr;
    Provider* bootProvider = nullptr;
    Class bootClass = Class::Plain;
    uint32_t bootRefs = 0;

    explicit Harness(Class start, bool withoutLibraryRef = false)
    {
        world.createWithoutLibraryRef = withoutLibraryRef;
        Boot(start);
    }

    void Boot(Class start)
    {
        const Target target = start == Class::FsrFg ? Target::FsrFg : start == Class::XeFg ? Target::XeFg : Target::Plain;
        Chain* chain = nullptr;
        Provider* provider = nullptr;

        // The interop's own queue, held by the interop for the process; a boot library is created with it.
        Queue* interopOwn = world.NewQueue("the interop's own");
        world.interopQueue = interopOwn;
        world.globals.currentCommandQueue = interopOwn;
        Queue* bootQueue = nullptr;

        if (start != Class::Plain)
        {
            Chain* innerReal = nullptr;
            Chain* innerWrapped = nullptr;
            bootQueue = world.NewQueue(start == Class::XeFg ? "the boot xefg's own" : "the boot fsrfg's own");
            world.QueueAddRef(interopOwn, "the boot provider");
            world.FactoryReentry(innerReal, innerWrapped, bootQueue);
            chain = world.NewChain(start);
            chain->refs = world.createWithoutLibraryRef ? 1 : 2;
            chain->refs += world.extraHolders;
            chain->libraryRef = !world.createWithoutLibraryRef;
            chain->inner = innerWrapped;
            provider = world.NewProvider(start, chain);
            provider->ownQueue = bootQueue;
            provider->gameQueue = interopOwn;
            world.globals.currentFGSwapchain = chain;
            world.globals.currentFG = provider;
            world.HookFor(chain);
        }
        else
        {
            chain = world.NewChain(Class::Plain);
            chain->refs = 1;
            world.QueueAddRef(interopOwn, "the boot plain chain");
            chain->queue = interopOwn;
            world.globals.interopPresent = chain;
        }
        // The bridge is constructed after the boot create and takes the interop's queue as its copy queue.
        world.bridgeQueue = world.interopQueue;

        world.windowChain = chain;
        world.bridge.fgChain = chain;
        world.bridge.provider = provider;
        world.bridge.refs = 1;
        world.bridgePointer = &world.bridge;
        world.globals.currentSwapchain = &world.bridge;
        world.globals.currentWrappedSwapchain = &world.bridge;
        if (start == Class::Plain)
            world.globals.currentRealSwapchain = chain;
        world.globals.interopApi = Interop::Dx11wDx12;
        world.globals.screenWidth = 2560;
        world.globals.screenHeight = 1440;
        world.globals.descWidth = 2560;
        world.globals.descHeight = 1440;
        world.globals.allowTearing = false;
        world.globals.lastFlags = 0;
        world.globals.realExclusiveFullscreen = false;

        state.active = target;
        state.target = target;
        // Deliberately NOT seeded: the module did not create the boot chain, so the first switch has to read it from the
        // bridge (Ops::ReadLive). Seeding it here is exactly the defect this harness used to hide.
        state.live = {};
        state.models[static_cast<size_t>(Class::FsrFg)] =
            LibraryModel { world.tableModel, world.witnessHonoured, world.pinFirstRow };
        state.models[static_cast<size_t>(Class::XeFg)] =
            LibraryModel { world.tableModel, world.witnessHonoured, world.pinFirstRow };
        boot = world.globals;
        bootChain = chain;
        bootProvider = provider;
        bootClass = start;
        bootRefs = chain->refs;
        world.target = target;
        world.active = target;
        world.events.clear();
    }

    // The witness forwarding is a property of the wrapper, so it applies to the chains that already exist too.
    void ForwardWitness(bool forwards)
    {
        world.forwardsWitness = forwards;
        for (auto& chain : world.chains)
            chain->forwardsWitness = forwards;
    }

    SwitchResult Switch(Target to)
    {
        world.target = to;
        world.active = state.active;
        world.InteropTick();
        return RunSwitch(state, ops, world.clock);
    }

    // A real present on the live chain, then its result goes through the probation hook as the bridge does.
    uint32_t Present(uint32_t result = kOk, uint32_t presentMs = 8)
    {
        Chain* chain = static_cast<Chain*>(state.live.chain);
        if (chain != nullptr)
            chain->presentResult = result;
        // The bridge copies the frame on its own queue, the chain presents on its queue (a provider's on its game queue),
        // and the interop's thread runs in between.
        world.QueueUse(world.bridgeQueue, "the bridge's interop copy");
        if (chain != nullptr && chain->queue != nullptr)
            world.QueueUse(chain->queue, "a plain present");
        if (auto* provider = static_cast<Provider*>(state.live.provider); provider != nullptr)
            world.QueueUse(provider->gameQueue, "the provider's game queue");
        world.InteropTick();
        const uint32_t seen = chain != nullptr ? world.PresentOn(chain) : kDeviceRemoved;
        return OnFgPresentResult(state, ops, seen, presentMs);
    }

    // Everything the design lists as an invariant, checked against the live world.
    void Invariants(const char* where)
    {
        if (!world.useAfterFree.empty())
        {
            std::printf("      use after free in %s: %s\n", where, world.useAfterFree.front().c_str());
            CHECK(false);
        }
        // Every queue the switch and the interop go on using is alive: the bridge's copy queue, the interop's queue, the
        // queue of the chain on the window (a provider's game queue).
        if (!world.queueFaults.empty())
            std::printf("      queue fault in %s: %s\n", where, world.queueFaults.front().c_str());
        CHECK(world.queueFaults.empty());
        CHECK(world.bridgeQueue != nullptr && !world.bridgeQueue->freed);
        CHECK(world.interopQueue != nullptr && !world.interopQueue->freed);
        CHECK(!world.bridge.deleted);
        CHECK(world.oldSwapChain == nullptr); // the switch never sets the static nothing clears
        // One chain per window, and the module's live chain is the one the window is bound to.
        CHECK(state.live.Empty() || state.live.chain == world.windowChain);
        // The bridge presents the chain the module adopted: after a switch the old chain is gone and the bridge must
        // not still name it (a forwarder would present freed memory).
        CHECK(world.bridge.fgChain == (state.managed ? nullptr : state.live.chain));
        // No provider destroy ran while the bridge still named the chain.
        CHECK(world.teardownWhileAttached == 0);
        if (state.live.Empty())
            CHECK(world.windowChain == nullptr);
        // The global FG chain pointer never names a chain at the moment it is released: with `preserve` on, a release
        // through the hook would swallow it (a leak), with it off it would delete the bridge.
        CHECK(world.h1Preserved == 0);
        CHECK(world.h1Nuclear == 0);
        // The bridge is still found by the pointer the module recorded, and the global pointer that the library's
        // re-entry overwrote no longer names it.
        CHECK(world.bridgePointer == &world.bridge);
        // Hooks describe the live class, or none for plain. A detach that could not drain leaves the old class patched
        // as dead code, which is the state the sequence rolls back from without remembering the target.
        if (!world.hooks.stuck)
            CHECK(world.hooks.cls == (state.live.Empty() ? Class::Plain : state.live.cls));
        if (state.live.cls != Class::Plain && !state.live.Empty())
            CHECK(world.hooks.attached[kSlotPresent]);
        // A provider shell is never reused and never grows past the switch budget.
        CHECK(world.retired <= static_cast<int>(kMaxSwitches));
        // The global provider pointer names the live provider, and nothing for a plain chain: a teardown that leaves the
        // old provider behind would let the next create hand back a shell the module already shut down.
        CHECK(world.globals.currentFG == state.live.provider);
        // Nothing reads the old chain after its teardown.
        CHECK(world.capturesOfFreed == 0);
        CHECK(world.deadlocks == 0);
        // A hook body never saw a cleared trampoline, and no detach cleared one while a body was in flight.
        CHECK(world.nullTrampoline == 0);
        CHECK(world.hooks.clearedWhileBusy == 0);
        // The global state the library's factory re-entry overwrites is back where it was.
        std::string diff;
        if (!world.SameGlobals(boot, world.globals, diff))
            std::printf("      globals differ in %s\n", diff.c_str());
        CHECK(world.SameGlobals(boot, world.globals, diff));
    }

    // The chain the switch let go of is gone exactly once, in both models (the table decides how).
    void FreedOnce(Chain* chain)
    {
        CHECK(chain->freed);
        CHECK(chain->refs == 0);
    }
};
} // namespace

// The Ops contract of this file needs two extra fields on World that the scenario code sets.
// (Kept out of the class above so the fakes stay one block.)
namespace
{
void Report(const char* text) { std::printf("    [ok] %s\n", text); }
} // namespace

int main()
{
    // ---- SIM-H5: both measured models, both preserve settings ---------------------------------------------------
    {
        std::printf("  SIM-H5-m1: the destroy ends the object, the pin is never released\n");
        for (bool preserve : { true, false })
        {
            Harness h(Class::FsrFg);
            h.world.libraryModel = Model::M1;
            h.world.tableModel = Model::M1;
            h.world.preserve = preserve;
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Probation);
            CHECK(h.world.pinReleases == 0); // M1: no release after the destroy
            h.FreedOnce(h.bootChain);
            h.Invariants("SIM-H5-m1");
            CHECK(h.world.bridge.fgChain != nullptr);
            Report("M1 with preserve on and off frees the old chain once, no release on freed memory");
        }

        std::printf("  SIM-H5-m2: the context held a reference, the pin release frees it\n");
        for (bool preserve : { true, false })
        {
            Harness h(Class::FsrFg);
            h.world.libraryModel = Model::M2;
            h.world.tableModel = Model::M2;
            h.world.preserve = preserve;
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Probation);
            CHECK(h.world.pinReleases == 1);
            h.FreedOnce(h.bootChain);
            h.Invariants("SIM-H5-m2");
            Report("M2 releases the pin exactly once and the chain is freed");
        }
    }

    // ---- the wrong table entry is caught, not survived ----------------------------------------------------------
    {
        std::printf("  SIM-H5-wrong-m2-table-on-m1: one use after free caused by the wrong row, then Dead\n");
        Harness h(Class::FsrFg);
        h.world.libraryModel = Model::M1;
        h.world.tableModel = Model::M2; // the wrong entry
        const SwitchResult result = h.Switch(Target::XeFg);
        CHECK(result.state == State::Limp || result.state == State::Dead);
        // With the witness the wrong row is caught before the release, so not even the wrong entry's own damage
        // happens; the case below shows what that release would have done.
        CHECK(h.world.useAfterFree.empty());
        CHECK(h.world.pinReleases == 0);
        Report("a wrong M2 row on an M1 library is caught by the witness before any release");

        Report("SIM-H5-wrong-m2-table-on-m1 without a witness: the row is not admitted, nothing is released");
        Harness u(Class::FsrFg);
        u.world.libraryModel = Model::M1;
        u.world.tableModel = Model::M2;
        u.world.witnessHonoured = false; // a row no measure can produce, and the one that would release on the table's word
        const SwitchResult refused = u.Switch(Target::XeFg);
        // The refusal came before anything was touched: the chain is taken back instead of being left unowned.
        CHECK(refused.state == State::Refused);
        CHECK(refused.reason == Fail::ModelViolation);
        CHECK(u.world.pinReleases == 0);                 // the release never happened, so no freed memory was touched
        CHECK(u.world.destroyCalls == 0);                // nor did the destroy: the refusal comes before every call
        CHECK(u.world.useAfterFree.empty());
        CHECK(!u.bootChain->freed);
        CHECK(!u.state.managed && u.world.bridge.fgChain == u.bootChain);
        u.Invariants("SIM-H5-m2-no-witness");
        Report("an M2 row without a witness is not a row: the teardown refuses before the destroy and the release");

        std::printf("  SIM-H5-wrong-m1-table-on-m2: no release, the witness says alive, Dead\n");
        Harness m(Class::FsrFg);
        m.world.libraryModel = Model::M2;
        m.world.tableModel = Model::M1;
        const SwitchResult dead = m.Switch(Target::XeFg);
        CHECK(dead.state == State::Limp || dead.state == State::Dead);
        CHECK(m.world.useAfterFree.empty());
        CHECK(!m.bootChain->freed); // the leak the witness caught, not a crash
        Report("a wrong M1 row on an M2 library leaks the chain, the cross-check stops the module");

        std::printf("  SIM-H5-wrong-m1-table-on-m2 without a witness: the next create is denied\n");
        Harness n(Class::FsrFg);
        n.world.libraryModel = Model::M2;
        n.world.tableModel = Model::M1;
        n.world.witnessHonoured = false;
        const SwitchResult denied = n.Switch(Target::XeFg);
        CHECK(denied.state == State::Limp);
        CHECK(denied.reason == Fail::OldChainLeaked);
        CHECK(n.world.useAfterFree.empty());
        Report("without a witness the leaked window is caught by the next create");
    }

    // ---- SIM-H5-unknown-build -----------------------------------------------------------------------------------
    {
        std::printf("  SIM-H5-unknown-build: a library with no measured row is refused, nothing is touched\n");
        Harness h(Class::FsrFg);
        h.world.tableModel = Model::Unknown;
        const SwitchResult result = h.Switch(Target::XeFg);
        CHECK(result.state == State::Refused);
        CHECK(result.reason == Fail::NoModel);
        CHECK(h.world.creates == 0);
        CHECK(h.world.destroyCalls == 0);
        CHECK(h.world.events.size() == 1);
        CHECK(h.bootChain->refs == h.bootRefs);
        Report("NoModel refuses before the barrier, the drain or the teardown");
    }

    // ---- SIM-H5-witness-forwards --------------------------------------------------------------------------------
    {
        std::printf("  SIM-H5-witness-forwards: a dead wrapper that looks alive never causes a release\n");
        Harness h(Class::FsrFg);
        h.world.libraryModel = Model::M1;
        h.world.tableModel = Model::M1;
        h.ForwardWitness(true); // the interface went to the inner chain
        const SwitchResult result = h.Switch(Target::XeFg);
        CHECK(h.world.pinReleases == 0); // the decisive check: no release because of the witness
        CHECK(h.world.useAfterFree.empty());
        CHECK(result.state == State::Limp || result.state == State::Dead);
        CHECK(h.world.h1Preserved == 0 && h.world.h1Nuclear == 0);
        Report("the witness reports alive on a dead wrapper; the sequence releases nothing and stops");
    }

    // ---- SIM-H1: preserve on and off, the old failure modes -----------------------------------------------------
    {
        std::printf("  SIM-H1-offbyone + SIM-H1-preserve-leak: the hooks' special branches are unreachable\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        // The unhook left the gate closed for the new class; the rehook is what reopens it, and it hooked XeFG.
        CHECK(h.world.hooks.cls == Class::XeFg);
        CHECK(!h.world.hooks.gate.Draining());
        CHECK(h.world.h1Preserved == 0);
        CHECK(h.world.h1Nuclear == 0);
        CHECK(!h.world.bridge.deleted);
        Report("a switch with the bridge pin releases nothing while the global pointer names the chain");

        // The negative control: release while the pointer still names it. Preserve on swallows the release (the leak),
        // preserve off deletes the bridge from inside the release.
        Harness leak(Class::FsrFg);
        leak.world.preserve = true;
        // the defect: the teardown does not clear the global pointer first
        leak.world.EmissiveTeardown(LiveChain { leak.bootChain, leak.bootProvider, leak.bootClass, leak.world.window });
        CHECK(leak.world.h1Preserved == 1);
        CHECK(!leak.bootChain->freed); // the leak: the chain is still alive and still bound
        Report("a release with the pointer still set leaks under preserve=true");

        Harness nuke(Class::FsrFg);
        nuke.world.preserve = false;
        nuke.world.EmissiveTeardown(LiveChain { nuke.bootChain, nuke.bootProvider, nuke.bootClass, nuke.world.window });
        CHECK(nuke.world.h1Nuclear >= 1); // the nested library release takes the branch again
        CHECK(nuke.world.bridge.deleted);
        Report("a release with the pointer still set deletes the bridge under preserve=false");
    }

    // ---- SIM-H2: an adopted dud is torn down by its own provider --------------------------------------------------
    {
        std::printf("  SIM-H2-adopted-dud: probation fails, plain comes back, one chain per window\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        Chain* dud = static_cast<Chain*>(h.state.live.chain);
        CHECK(h.Present(kGenericFailure) == kOk); // masked during probation
        const SwitchResult rolled = h.Switch(Target::Plain);
        CHECK(rolled.state == State::RolledBack);
        CHECK(dud->freed);
        CHECK(h.state.live.cls == Class::Plain);
        CHECK(h.world.windowChain == h.state.live.chain);
        CHECK(h.world.windowChain != dud);
        CHECK(h.state.policy.Remembered(Target::XeFg));
        h.Invariants("SIM-H2");
        Report("the dud's provider tore it down before plain was created");
    }

    // ---- SIM-H3: one class hooked at a time ---------------------------------------------------------------------
    {
        std::printf("  SIM-H3-class: the new class is hooked after the switch\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        CHECK(h.world.hooks.cls == Class::XeFg);
        CHECK(h.world.hooks.attaches == 2); // boot, then the rebind
        CHECK(h.world.hooks.detaches == 1);
        CHECK(h.world.hooks.detachedSlots > 0);
        const int before = h.world.hooks.dispatch;
        h.Present();
        CHECK(h.world.hooks.dispatch == before + 1);
        h.Invariants("SIM-H3");
        Report("present dispatch increments on the XeFG class after the switch");

        Harness broken(Class::FsrFg);
        broken.world.disableUnhook = true; // the defect: the old class stays patched
        const SwitchResult result = broken.Switch(Target::XeFg);
        CHECK(result.state == State::RolledBack);
        CHECK(result.reason == Fail::Rehook);
        CHECK(broken.world.hooks.dispatch == 0);
        Report("without the unhook the new class is never hooked: Rehook and a rollback");
    }

    // ---- SIM-STATE: the library's factory re-entry is undone ----------------------------------------------------
    {
        std::printf("  SIM-STATE: every field the library's factory re-entry overwrites is restored\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        CHECK(h.world.clobbers == 2); // boot and the switch
        CHECK(h.world.globals.interopApi == Interop::Dx11wDx12);
        std::string diff;
        CHECK(h.world.SameGlobals(h.boot, h.world.globals, diff));
        h.Invariants("SIM-STATE");
        Report("interop, tearing, exclusive flag, device, queue and sizes are all back");

        Harness skipped(Class::FsrFg);
        skipped.world.skipRestore = true;
        CHECK(skipped.Switch(Target::XeFg).state == State::Probation);
        CHECK(skipped.world.globals.interopApi == Interop::None); // the defect the restore exists for
        Report("without the restore the interop api stays None, which the next preflight refuses");
    }

    // ---- SIM-BRIDGE-PTR: the global pointer names the inner chain after a switch --------------------------------
    {
        Report("SIM-BRIDGE-PTR: the bridge is found by the recorded pointer, not by the global");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        // Boot parity is restored, and the real chain is still the one the library's re-entry recorded at boot, so
        // neither global can be used to find the bridge.
        CHECK(h.world.globals.currentWrappedSwapchain == &h.world.bridge);
        CHECK(h.world.globals.currentRealSwapchain != h.state.live.chain);
        CHECK(h.world.bridgePointer == &h.world.bridge);
        // A second switch still works: the module never reads the global to find the bridge.
        h.world.clock += kCooldownMs;
        CHECK(h.Switch(Target::FsrFg).state == State::Probation);
        h.Invariants("SIM-BRIDGE-PTR");
        Report("the recorded pointer names the bridge while the globals name the library's own chains");

        Harness skipped(Class::FsrFg);
        skipped.world.skipRestore = true;
        CHECK(skipped.Switch(Target::XeFg).state == State::Probation);
        // Without the restore the globals keep what the library's re-entry wrote: it names the new inner chain, so a
        // lookup through it would name a chain the module does not own.
        CHECK(skipped.world.globals.currentRealSwapchain != skipped.boot.currentRealSwapchain);
        CHECK(skipped.world.globals.interopApi == Interop::None);
        Report("without the restore the globals keep the inner chain and interop api the library wrote");
    }

    // ---- SIM-CAPTURE: the old chain is read before it is released ----------------------------------------------
    {
        std::printf("  SIM-CAPTURE: the descriptor is read before the teardown, never after\n");
        Harness h(Class::FsrFg);
        h.world.libraryModel = Model::M1;
        h.world.tableModel = Model::M1;
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        CHECK(h.world.capturesOfFreed == 0);
        const int captures = h.world.capturesOfFreed;
        SimOps::Capture late {};
        h.ops.ReadCapture(h.bootChain, late); // the negative control: the old chain is gone
        CHECK(h.world.capturesOfFreed == captures + 1);
        CHECK(!h.world.useAfterFree.empty());
        Report("a read after the teardown is recorded as a use after free; the sequence never does it");
    }

    // ---- SIM-XE-deadlock: the barrier is not held across the destroy -------------------------------------------
    {
        std::printf("  SIM-XE-deadlock: a held resize lock would deadlock against the native present thread\n");
        Harness held(Class::XeFg);
        held.world.nativePresentInside = true;
        held.world.holdBarrier = true; // the defect under test
        held.Switch(Target::FsrFg);
        CHECK(held.world.deadlocks > 0);
        Report("a barrier held across the destroy reproduces the hang inside the simulation");

        Harness fine(Class::XeFg);
        fine.world.nativePresentInside = true;
        CHECK(fine.Switch(Target::FsrFg).state == State::Probation);
        CHECK(fine.world.deadlocks == 0);
        CHECK(!fine.world.barrierHeld);
        fine.Invariants("SIM-XE-deadlock");
        Report("the real sequence takes and releases the barrier before the destroy: no deadlock");

        Harness blocked(Class::XeFg);
        blocked.world.barrierBlocks = true;
        const SwitchResult refused = blocked.Switch(Target::FsrFg);
        CHECK(refused.state == State::Refused);
        CHECK(refused.reason == Fail::Barrier);
        CHECK(blocked.world.creates == 0);
        CHECK(blocked.world.retired == 0);
        Report("a barrier that times out refuses transiently, nothing is touched");
    }

    // ---- SIM-livelock: a paused provider never triggers a rollback ---------------------------------------------
    {
        std::printf("  SIM-livelock: ten simulated minutes with frame generation paused\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        for (int i = 0; i < 600; ++i)
        {
            h.world.clock += 1000;
            // Frame generation paused: the provider reports no activity and no present reaches the hook.
        }
        CHECK(!h.state.probation.RollbackDue());
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        CHECK(h.world.status.state == State::Probation);
        Report("no activity is not a failure: the module never retries or rolls back");
    }

    // ---- SIM-H4: the game never sees an error of ours ----------------------------------------------------------
    {
        std::printf("  SIM-H4-detached: S_OK while detached, the old error only after giving up\n");
        Harness h(Class::FsrFg);
        h.world.providerCreateFails = true;
        h.world.plainCreateFails = true;
        const SwitchResult limp = h.Switch(Target::XeFg);
        CHECK(limp.state == State::Limp);
        CHECK(h.world.status.state == State::Limp);
        CHECK(DetachedPresent(h.state, h.ops, false) == kOk);
        h.world.clock += kLimpGiveUpMs;
        CHECK(DetachedPresent(h.state, h.ops, false) == kDeviceRemoved);
        CHECK(h.world.status.state == State::Dead);
        Report("S_OK for the whole window, then DXGI_ERROR_DEVICE_REMOVED");

        Harness unmanaged(Class::FsrFg);
        CHECK(DetachedPresent(unmanaged.state, unmanaged.ops, false) == kDeviceRemoved);
        Report("without a detach of ours the answer is exactly today's");
    }

    // ---- the teardown keeps the bridge's own order ---------------------------------------------------------------
    {
        std::printf("  the teardown clears the global pointer before it deactivates the provider\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        const auto clearAt = std::find(h.world.order.begin(), h.world.order.end(), std::string("clear-refs"));
        const auto deactivateAt = std::find(h.world.order.begin(), h.world.order.end(), std::string("deactivate"));
        CHECK(clearAt != h.world.order.end() && deactivateAt != h.world.order.end());
        CHECK(clearAt < deactivateAt);
        Report("the order matches the bridge's own release: pointer, then provider, then the pin");
    }

    // ---- the boot chain is read from the bridge, never assumed (C-1) ---------------------------------------------
    {
        std::printf("  SIM-C1: the first switch seeds LiveChain from the bridge\n");
        Harness h(Class::FsrFg);
        CHECK(h.state.live.Empty()); // the harness does not seed it: that is the defect this scenario exists for
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.FreedOnce(h.bootChain);
        h.Invariants("SIM-C1");
        Report("the first switch reads the boot chain and tears it down");

        Harness blind(Class::FsrFg);
        blind.world.readLive = false;
        const SwitchResult refused = blind.Switch(Target::XeFg);
        CHECK(refused.state == State::Refused);
        CHECK(refused.reason == Fail::NoCapture);
        CHECK(blind.world.creates == 0 && blind.world.destroyCalls == 0 && blind.world.pinReleases == 0);
        CHECK(!blind.bootChain->freed);
        CHECK(blind.bootChain->refs == blind.bootRefs);
        Report("a live chain the bridge cannot name is refused hard, nothing touched");

        Harness off(Class::FsrFg);
        off.world.wanted = false;
        CHECK(off.Switch(Target::XeFg).state == State::Idle);
        CHECK(off.world.events.empty() && off.world.publishes == 0);
        Report("with the key off the switch is one comparison and no report");
    }

    // ---- a refusal is one line, one event and one status per change (C-2) ---------------------------------------
    {
        std::printf("  SIM-C2: a repeated refusal writes nothing and keeps the stored cause\n");
        Harness h(Class::FsrFg);
        h.world.testFails = true;
        CHECK(h.Switch(Target::XeFg).state == State::RolledBack);
        CHECK(h.state.policy.RememberedReason(Target::XeFg) == Fail::TestPresent);
        CHECK(h.Switch(Target::XeFg).state == State::Refused);
        const int refuseEvents = h.world.refusesReported;
        const int published = h.world.publishes;
        for (int i = 0; i < 60; ++i)
        {
            h.world.clock += 16;
            CHECK(h.Switch(Target::XeFg).state == State::Refused);
        }
        CHECK(h.world.refusesReported == refuseEvents);
        CHECK(h.world.publishes == published);
        CHECK(h.state.policy.RememberedReason(Target::XeFg) == Fail::TestPresent); // Remembered never overwrites
        CHECK(h.world.status.reason == Fail::TestPresent);
        Report("sixty presents of the same refusal: no event, no status write, the cause unchanged");
    }

    // ---- rollbacks spend the budget, and the retired shells are capped independently (C-3) ----------------------
    {
        std::printf("  SIM-C3: a target that always fails cannot grow the retired set for free\n");
        Harness h(Class::FsrFg);
        h.world.testFails = true;
        for (int i = 0; i < 12; ++i)
        {
            if (i > 0)
            {
                h.state.policy.ClearMemory(Target::XeFg); // the menu's "try again", every time
                h.world.clock += kCooldownMs + 1;
            }
            const uint32_t left = h.state.policy.SwitchesLeft();
            const int retired = h.world.retired;
            const SwitchResult result = h.Switch(Target::XeFg);
            if (result.reason == Fail::Budget)
                break;
            // A destructive attempt spent a unit and retired at least the old shell (and the dud it created).
            CHECK(h.state.policy.SwitchesLeft() == left - 1);
            CHECK(h.world.retired > retired);
            CHECK(h.world.retired <= static_cast<int>(kMaxSwitches));
        }
        CHECK(h.world.retired <= static_cast<int>(kMaxSwitches));
        // Past the hard-refusal hold, so the preflight really runs again: a spent budget is refused again, and it is
        // never stored as a failure of the target.
        h.world.clock += kHardRecheckMs;
        const SwitchResult spent = h.Switch(Target::XeFg);
        CHECK(spent.state == State::Refused);
        CHECK(spent.reason == Fail::Budget);
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        CHECK(h.world.useAfterFree.empty());
        Report("eight retired shells at most, every rollback paid for, then Budget even after try again");
    }

    // ---- every failure injection, each with refcounts (C-5) ------------------------------------------------------
    {
        std::printf("  SIM-C5: the failure injections, each ending without a use after free\n");
        // failDestroy: XeFG's destroy can fail; the context is lost, the chain is not freed, nothing may be released.
        for (Model model : { Model::M1, Model::M2 })
        {
            Harness h(Class::FsrFg);
            h.world.libraryModel = model;
            h.world.failDestroy = true;
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Limp || result.state == State::Dead);
            CHECK(h.world.useAfterFree.empty());
            CHECK(!h.bootChain->freed); // the failed destroy freed nothing
            CHECK(h.state.dead || h.state.limp);
        }
        Report("a failed destroy limps without releasing a chain that still exists");

        // releaseRefused: the provider's own guard refuses; TeardownRefused, the pin is not released.
        {
            Harness h(Class::FsrFg);
            h.world.releaseRefused = true;
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Limp);
            CHECK(result.reason == Fail::TeardownRefused);
            CHECK(h.world.pinReleases == 0);
            CHECK(!h.bootChain->freed);
            CHECK(h.world.useAfterFree.empty());
        }
        Report("a provider that refuses to release its chain stops the sequence, pin untouched");

        // deniedTimes 1..3: retried, one provider object, one retirement.
        for (int denied = 1; denied <= 3; ++denied)
        {
            Harness h(Class::FsrFg);
            h.world.deniedTimes = denied;
            CHECK(h.Switch(Target::XeFg).state == State::Probation);
            CHECK(h.world.creates == denied + 1); // one attempt per denial plus the successful one
            CHECK(h.world.retired == 1);          // one retired old shell: no shell was created before the success
            h.FreedOnce(h.bootChain);
            h.Invariants("SIM-C5-denied");
        }
        Report("1..3 denied attempts are retried, with no extra shell");

        // deniedTimes 4: every attempt denied -> Limp (the window stayed bound).
        {
            Harness h(Class::FsrFg);
            h.world.deniedTimes = static_cast<int>(kCreateAttempts);
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Limp);
            CHECK(result.reason == Fail::OldChainLeaked);
            CHECK(h.world.useAfterFree.empty());
        }
        Report("every attempt denied is OldChainLeaked, not a second chain");

        // adoptFails / replayFails / testFails / hooksLeavePresentNull: the created chain is torn down by its provider.
        const struct
        {
            const char* name;
            void (*set)(World&);
        } failures[] = {
            { "adopt", [](World& w) { w.adoptFails = true; } },
            { "replay", [](World& w) { w.replayFails = true; } },
            { "test", [](World& w) { w.testFails = true; } },
            { "hook", [](World& w) { w.hooksLeavePresentNull = true; } },
        };
        for (const auto& failure : failures)
        {
            for (Model model : { Model::M1, Model::M2 })
            {
                Harness h(Class::FsrFg);
                h.world.libraryModel = model;
                h.world.tableModel = model;
                failure.set(h.world);
                const SwitchResult result = h.Switch(Target::XeFg);
                CHECK(result.state == State::RolledBack);
                h.FreedOnce(h.bootChain);
                CHECK(h.world.useAfterFree.empty());
                CHECK(h.state.live.cls == Class::Plain);
                if (failure.name[0] == 'h')
                    CHECK(h.world.hooks.dispatch == 0);
                // The created dud was torn down by its own provider as well: exactly two retirements.
                CHECK(h.world.retired == 2);
            }
        }
        Report("adopt, replay, test and hook failures roll back with every chain freed once");

        // isShuttingDown: a transient refusal, nothing touched.
        {
            Harness h(Class::FsrFg);
            h.world.isShuttingDown = true;
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Refused);
            CHECK(result.reason == Fail::ShuttingDown);
            CHECK(h.world.creates == 0 && h.world.destroyCalls == 0);
        }
        Report("shutting down refuses transiently before anything is touched");

        // An extra COM holder: the pin release is not the last reference, and the module stops instead of building the
        // next chain on a window that is still bound.
        {
            Harness h(Class::FsrFg);
            ++h.bootChain->refs; // an extra COM holder (an overlay, a recorder, the HDR module)
            const SwitchResult result = h.Switch(Target::XeFg);
            CHECK(result.state == State::Limp || result.state == State::Dead);
            CHECK(h.world.pinReleases == 1);
            CHECK(!h.bootChain->freed); // the extra holder kept it alive: the honest answer
            CHECK(h.world.useAfterFree.empty());
            // The module stops at the cross-check: it does not try to create a second chain on a window that is still
            // bound (the create would only answer E_ACCESSDENIED four times).
            CHECK(h.world.creates == 0);
            CHECK(h.world.retired == 0);
        }
        Report("an extra COM holder is caught: the release leaves the chain alive and the module stops");

        // oldSwapChain: cleared at the detach, so the release hook cannot swallow the pin release.
        for (bool clear : { true, false })
        {
            Harness h(Class::FsrFg);
            h.world.clearOldSwapChain = clear;
            h.world.oldSwapChain = h.bootChain;
            const SwitchResult result = h.Switch(Target::XeFg);
            if (clear)
            {
                CHECK(result.state == State::Probation);
                h.FreedOnce(h.bootChain);
                CHECK(h.world.oldSwapChainHits == 0);
            }
            else
            {
                CHECK(result.state == State::Limp || result.state == State::Dead);
                CHECK(!h.bootChain->freed); // the release was swallowed at the stale address
                CHECK(h.world.oldSwapChainHits > 0);
            }
            CHECK(h.world.useAfterFree.empty());
        }
        Report("a stale oldSwapChain pointer is cleared at the detach; without it the pin release is swallowed");

        // The library's own release of its context reference, through the patched Release and around it.
        for (bool throughHook : { true, false })
        {
            Harness h(Class::FsrFg);
            h.world.libraryReleaseThroughHook = throughHook;
            CHECK(h.Switch(Target::XeFg).state == State::Probation);
            h.FreedOnce(h.bootChain);
            CHECK(h.world.pinReleases == 1);
            h.Invariants("SIM-C5-library-release");
        }
        Report("the library's internal release frees the chain once, hooked or not");
    }

    // ---- the read-only peek refuses a busy hook set before anything is touched (C-9) ----------------------------
    {
        std::printf("  SIM-C9: a hook body inside at the preflight costs nothing\n");
        Harness h(Class::FsrFg);
        CHECK(h.world.hooks.gate.Enter() == GateEntry::Counted); // the game's thread is inside the present hook
        const SwitchResult busy = h.Switch(Target::XeFg);
        CHECK(busy.state == State::Refused);
        CHECK(busy.reason == Fail::HookBusy);          // a call in flight, not a failure of the target
        CHECK(h.world.hooks.detaches == 0);                  // nothing was detached
        CHECK(h.world.destroyCalls == 0);              // the old chain was not torn down
        CHECK(h.world.hooks.detachBlocked == 0);       // no drain timeout was paid
        CHECK(h.world.retired == 0);                   // no shell was retired
        CHECK(h.state.policy.SwitchesLeft() == kMaxSwitches); // and no budget unit was spent
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        CHECK(!h.bootChain->freed);                    // the old chain is still the live one
        h.world.hooks.gate.Leave();
        h.world.clock += kTransientRetryMs; // the state condition is over
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.Invariants("SIM-C9");
        Report("a body inside at the preflight refuses before the detach, the teardown and the budget");

        Harness defect(Class::FsrFg);
        CHECK(defect.world.hooks.gate.Enter() == GateEntry::Counted);
        defect.world.ClearHooksWhileBusy(); // the defect under test: clear while a body runs
        CHECK(defect.world.nullTrampoline > 0);
        defect.world.hooks.gate.Leave();
        Report("clearing the originals while a body runs is caught as a null trampoline");
    }

    // ---- a detach never clears a trampoline another thread is inside (C-8, the step-7 fallback) ------------------
    {
        std::printf("  SIM-C8: a body that arrives after the peek keeps the originals valid\n");
        Harness h(Class::FsrFg);
        h.world.enterHookBodyAtDrain = true; // the peek was clean; this one starts during the barrier
        const SwitchResult blocked = h.Switch(Target::XeFg);
        CHECK(h.world.hooks.detachBlocked == 1); // one wait: the rollback does not repeat a detach that just answered Busy
        CHECK(h.world.nullTrampoline == 0);           // the originals were not cleared
        CHECK(h.world.hooks.attached[kSlotPresent]);  // the old class stays patched as dead code
        CHECK(blocked.state == State::RolledBack);
        CHECK(blocked.reason == Fail::HookBusy);      // a call in flight, not a failure of the target
        CHECK(h.state.live.cls == Class::Plain);      // plain presentation is up
        // The busy detach is transient: the target is NOT banned for the session and no "try again" is needed.
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        h.world.hooks.gate.Leave();
        // No manual Reattached(): Unhook() reopened the gate when it answered Busy, and HookFor() reopens it too.
        h.world.clock += kTransientRetryMs; // the transient pause is over
        const SwitchResult again = h.Switch(Target::XeFg);
        CHECK(again.state == State::Probation);
        CHECK(h.world.hooks.detachBlocked == 1); // no further blocked detach once the body left
        CHECK(h.world.hooks.cls == Class::XeFg); // the new class is the one hooked after the rebind
        CHECK(!h.world.hooks.gate.Draining()); // and the rehook reopened the gate
        h.Invariants("SIM-C8");
        Report("a detach that cannot drain rolls back without banning the target; after the body leaves it succeeds");
    }

    // ---- the rehook is what reopens the gate; a class hooked with it still shut shows nothing --------------------
    {
        std::printf("  SIM-C10: a switch whose rehook leaves the gate shut presents nothing and looks healthy\n");
        Harness h(Class::FsrFg);
        h.world.hooks.reopenGateOnRehook = false; // the product without the line HookFGSwapchain now ends with
        const SwitchResult switched = h.Switch(Target::XeFg);
        CHECK(switched.state == State::Probation); // the sequence cannot see the gate: the switch "worked"
        CHECK(h.world.hooks.cls == Class::XeFg);   // and the new class is the one patched
        Chain* live = static_cast<Chain*>(h.state.live.chain);
        const int dispatchBefore = h.world.hooks.dispatch;
        const int refusedBefore = h.world.hooks.refused;
        const int noDispatchBefore = h.world.hooks.noDispatch;
        const uint32_t shown = h.world.PresentOn(live);
        CHECK(shown == kOk);                          // the game is told the present worked
        CHECK(h.world.hooks.refused == refusedBefore + 1); // the body refused: no present reached the chain at all
        CHECK(h.world.hooks.dispatch == dispatchBefore); // nothing showed
        CHECK(h.world.hooks.noDispatch == noDispatchBefore); // the body ran, it just refused
        // The product's rehook runs again for the live class: it finds the set attached and reopens the gate (the line
        // upstream's HookFGSwapchain ends with).
        h.world.hooks.reopenGateOnRehook = true;
        h.world.HookFor(live);
        CHECK(h.world.PresentOn(live) == kOk);
        CHECK(h.world.hooks.dispatch == dispatchBefore + 1); // with the reopen the same call runs the body
        CHECK(h.world.hooks.refused == refusedBefore + 1);
        h.Invariants("SIM-C10");
        Report("a gate left shut by the rehook answers S_OK and presents nothing, so reopening it is not optional");
    }

    // ---- a foreign Detours transaction refuses before anything is torn down (C-11) ------------------------------
    {
        std::printf("  SIM-C11: a pending hook transaction refuses the switch before anything is torn down\n");
        Harness h(Class::FsrFg);
        h.world.hooksTransactable = false; // another thread holds a Detours transaction open
        const SwitchResult refused = h.Switch(Target::XeFg);
        CHECK(refused.state == State::Refused);
        CHECK(refused.reason == Fail::HookBusy); // a moment, not a property of the target
        CHECK(h.world.hooks.detaches == 0);      // nothing was detached
        CHECK(h.world.destroyCalls == 0);        // and the old chain is still on the window
        CHECK(h.world.retired == 0);
        CHECK(h.state.policy.SwitchesLeft() == kMaxSwitches); // no budget unit was spent
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        CHECK(!h.bootChain->freed);
        h.world.hooksTransactable = true; // the other transaction closed
        h.world.clock += kTransientRetryMs;
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.Invariants("SIM-C11");
        Report("a foreign transaction costs one refusal, never the old chain and never a banned target");
    }

    // ---- the teardown refuses an unknown model itself (defence in depth, reached directly) -----------------------
    {
        std::printf("  the teardown refuses a chain whose model is unknown, before touching a reference\n");
        Harness h(Class::FsrFg);
        LiveChain live { h.bootChain, h.bootProvider, Class::FsrFg, h.world.window };
        const TearDownResult torn = TearDownLive(live, h.ops, LibraryModel {});
        CHECK(!torn.ok);
        CHECK(torn.reason == Fail::NoModel);
        CHECK(h.world.pinReleases == 0);
        CHECK(h.world.destroyCalls == 0);
        CHECK(!h.bootChain->freed);
        Report("an unknown model is refused by the teardown itself: no destroy, no release, the chain stays");
    }

    // ---- a transient refusal inside the sequence pauses the feature (H-1) ---------------------------------------
    {
        std::printf("  SIM-H1-pause: a drain refusal pauses the feature and touches nothing\n");
        Harness h(Class::FsrFg);
        h.world.drainOk = false;
        const SwitchResult first = h.Switch(Target::XeFg);
        CHECK(first.state == State::Refused);
        CHECK(first.reason == Fail::Drain);
        const size_t events = h.world.events.size();
        const int publishes = h.world.publishes;
        const int creates = h.world.creates;
        for (int i = 0; i < 30; ++i)
        {
            h.world.clock += 16; // thirty presents inside the one-second pause
            CHECK(h.Switch(Target::XeFg).state == State::Idle);
        }
        CHECK(h.world.events.size() == events); // not one more line
        CHECK(h.world.publishes == publishes);  // not one more status write
        CHECK(h.world.creates == creates);      // nothing was created
        CHECK(h.world.destroyCalls == 0);       // nothing was torn down
        CHECK(!h.state.policy.Remembered(Target::XeFg));
        CHECK(h.state.probation.Watching() == false); // no chain was committed, nothing is judged
        h.world.drainOk = true;
        h.world.clock += kTransientRetryMs;
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.Invariants("SIM-H1-pause");
        Report("thirty presents inside the pause: no event, no publish, no create, no teardown; then the switch runs");

        // A refusal while a committed chain is being judged must not end that judgement.
        Harness p(Class::FsrFg);
        CHECK(p.Switch(Target::XeFg).state == State::Probation);
        CHECK(p.state.probation.InProbation());
        p.world.drainOk = false;
        p.world.clock += kCooldownMs + 1;
        const SwitchResult refused = p.Switch(Target::FsrFg);
        CHECK(refused.state == State::Refused);
        CHECK(refused.reason == Fail::Drain);
        CHECK(p.state.probation.Watching());
        CHECK(p.state.probation.InProbation());
        p.Present(kGenericFailure);
        CHECK(p.state.probation.RollbackDue());
        Report("a drain refusal during probation leaves the judgement running");
    }

    // ---- the rollback is not behind the "is another output wanted" answer (H-2) ---------------------------------
    {
        std::printf("  SIM-H2-wanted: the probation rollback runs while no other output is wanted\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.Present(kGenericFailure); // masked, the rollback is due on the next present
        CHECK(h.state.probation.RollbackDue());
        // After a commit the configured output IS the active one: a binding that only compares the two answers "not
        // wanted", and the failing chain must still go back to plain.
        const SwitchResult rolled = h.Switch(Target::XeFg);
        CHECK(rolled.state == State::RolledBack);
        CHECK(rolled.reason == Fail::Probation);
        CHECK(h.state.live.cls == Class::Plain);
        h.Invariants("SIM-H2-wanted");
        Report("a chain that fails its presents is put back to plain with nothing else wanted");
    }

    // ---- a row that promises a witness, on a chain that refuses it --------------------------------------------
    {
        std::printf("  SIM-WITNESS: an honoured row whose witness cannot be placed releases nothing\n");
        Harness h(Class::FsrFg);
        h.world.witnessRefused = true; // SetPrivateDataInterface fails on the live chain
        const SwitchResult torn = h.Switch(Target::XeFg);
        // The refusal came before anything was touched, so the chain that is still on the window is taken back instead
        // of being left unowned behind a Dead state: a clean refusal, the provider resumed, the game keeps presenting.
        CHECK(torn.state == State::Refused);
        CHECK(torn.reason == Fail::WitnessUnavailable);
        CHECK(h.world.pinReleases == 0);   // the table alone never decided a release
        CHECK(!h.bootChain->freed);
        CHECK(h.world.windowChain == h.bootChain);
        CHECK(h.world.creates == 0);       // nothing was created on a window that is still bound
        CHECK(h.world.hooks.detaches == 0);
        CHECK(h.world.useAfterFree.empty());
        CHECK(!h.state.managed);           // the present that follows is not a gap the game has to survive
        CHECK(h.world.bridge.fgChain == h.bootChain);
        CHECK(h.world.bridge.provider == h.bootProvider);
        CHECK(h.bootProvider->active);     // Resume ran: frame generation is on again
        h.Invariants("SIM-WITNESS-REATTACH");
        Report("a chain that refuses the witness is not released on the table's word alone, and is taken back intact");

        Harness p(Class::FsrFg);
        p.world.witnessPreflightRefused = true; // the read-only place-and-clear failed before anything ran
        const SwitchResult early = p.Switch(Target::XeFg);
        CHECK(early.state == State::Refused);
        CHECK(early.reason == Fail::WitnessUnavailable);
        CHECK(p.world.events.size() == 1); // the refusal line only: no Begin, no barrier, no drain
        CHECK(p.world.creates == 0);
        CHECK(p.state.live.Empty());
        Report("the same refusal is available in the preflight, before the barrier or the drain");
    }

    // ---- Create refuses while the three globals still name a chain --------------------------------------------
    {
        std::printf("  SIM-CREATEGUARD: Create refuses while the switch owns no chain but a global names one\n");
        Harness h(Class::FsrFg);
        SimOps::Capture capture {};
        capture.hwnd = h.world.window;
        h.world.windowChain = nullptr; // the window is free; only the three globals still name the old chain
        CHECK(h.world.globals.currentFG != nullptr);
        const SimOps::CreateResult refused = h.ops.Create(Target::XeFg, capture);
        CHECK(refused.chain == nullptr);
        CHECK(h.world.createRefused == 1);

        h.world.globals.currentFG = nullptr;
        h.world.globals.currentFGSwapchain = nullptr;
        h.world.oldSwapChain = nullptr;
        const SimOps::CreateResult made = h.ops.Create(Target::XeFg, capture);
        CHECK(made.chain != nullptr);
        CHECK(h.world.createRefused == 1);
        Report("a create while the globals still name a chain is refused; with them clear the same call creates");
    }

    // ---- the teardown order the row was measured with (the XeFG context leak) ----------------------------------
    {
        std::printf("  SIM-D1: the row's own order, and the context the other order leaks\n");
        // A provider whose context refuses to be destroyed while the caller still holds the chain (what XeFG answers),
        // and a table row measured with the reference released first.
        auto indexOf = [](const Harness& h, const char* what) {
            const auto at = std::find(h.world.order.begin(), h.world.order.end(), std::string(what));
            return at == h.world.order.end() ? -1 : static_cast<int>(at - h.world.order.begin());
        };

        Harness h(Class::FsrFg);
        h.world.pinFirstRow = true;
        h.world.contextRefusesWhileHeld = true;
        const SwitchResult switched = h.Switch(Target::XeFg);
        CHECK(switched.state == State::Probation);
        CHECK(h.world.contextLeaks == 0);   // the order the row was measured with frees the context too
        CHECK(h.world.secondDestroys == 0); // one destroy, never a second call on a freed context
        CHECK(h.world.pinReleases == 1);
        h.FreedOnce(h.bootChain);
        // The release is the pin's, before the destroy, with the global names already cleared.
        const int pin = indexOf(h, "pin-release");
        const int destroy = indexOf(h, "destroy");
        const int clearRefs = indexOf(h, "clear-refs");
        CHECK(clearRefs >= 0 && pin > clearRefs && destroy > pin);
        h.Invariants("SIM-D1-pinfirst");
        Report("a pin-first row releases the reference, then destroys once, and leaks no context");

        Harness d(Class::FsrFg);
        d.world.pinFirstRow = false; // the same library, torn down in the order the old rows used
        d.world.contextRefusesWhileHeld = true;
        const SwitchResult other = d.Switch(Target::XeFg);
        CHECK(other.state == State::Probation);
        CHECK(d.world.contextLeaks == 1); // the context the destroy refused is allocated with nobody to destroy it
        CHECK(d.world.secondDestroys == 0);
        const int destroyFirst = (std::find(d.world.order.begin(), d.world.order.end(), std::string("destroy")) -
                                  d.world.order.begin());
        const int pinAfter = (std::find(d.world.order.begin(), d.world.order.end(), std::string("pin-release")) -
                              d.world.order.begin());
        CHECK(destroyFirst < pinAfter);
        d.FreedOnce(d.bootChain);
        d.Invariants("SIM-D1-destroyfirst");
        Report("the same library in the other order leaks one context per switch: that is what pin-first removes");

        // A destroy that leaves the object alive after the pin release: the chain is leaked and the module stops, without
        // a second destroy on an object the first call may already have freed.
        Harness leak(Class::FsrFg);
        leak.world.pinFirstRow = true;
        leak.world.destroyLeavesChain = true;
        const SwitchResult leaked = leak.Switch(Target::XeFg);
        CHECK(leaked.state == State::Limp);
        CHECK(leaked.reason == Fail::OldChainLeaked);
        CHECK(leak.world.secondDestroys == 0);
        CHECK(leak.world.pinReleases == 1);
        CHECK(!leak.bootChain->freed); // the leak the witness caught, not a crash
        CHECK(leak.world.useAfterFree.empty());
        leak.Invariants("SIM-D1-LEAK");
        Report("a destroy that frees nothing after the pin release is a leak the module stops on, never a second destroy");

        // The pin release itself ends the object: the row says the context holds a reference and this build's does not.
        // The witness must be read after the release, so no destroy runs on the freed object and nothing else is touched.
        Harness p(Class::FsrFg, true);
        p.world.pinFirstRow = true;
        const SwitchResult freed = p.Switch(Target::XeFg);
        CHECK(freed.state == State::Limp);
        CHECK(freed.reason == Fail::ModelViolation);
        CHECK(p.world.pinReleases == 1);
        CHECK(p.world.destroyCalls == 0); // no destroy on the freed object
        CHECK(p.world.useAfterFree.empty());
        CHECK(p.bootChain->freed); // the release was the last reference, which is the leak the guard caught
        Report("a pin release that frees the chain stops the teardown before the destroy, with no use after free");

        // Any switch that would create or take down an XeFG chain is refused before anything is read or touched: its
        // teardown has not survived a real process, so an XeFG chain of the game's keeps the window.
        Harness xe(Class::XeFg);
        xe.world.xeFgSwitchAllowed = false;
        const SwitchResult fromXe = xe.Switch(Target::Plain);
        CHECK(fromXe.state == State::Refused);
        CHECK(fromXe.reason == Fail::XeFgSwitch);
        CHECK(xe.world.pinReleases == 0 && xe.world.destroyCalls == 0);
        CHECK(xe.world.creates == 0);
        Harness toXe(Class::FsrFg);
        toXe.world.xeFgSwitchAllowed = false;
        const SwitchResult toXeResult = toXe.Switch(Target::XeFg);
        CHECK(toXeResult.state == State::Refused);
        CHECK(toXeResult.reason == Fail::XeFgSwitch);
        CHECK(toXe.world.creates == 0 && toXe.world.pinReleases == 0);
        Report("an XeFG target or source is refused with nothing touched, and the chain on the window stays");

        // A pin-first row on a library the table measured as destroy-first stays in the destroy-first order.
        Harness k(Class::FsrFg);
        k.world.pinFirstRow = false;
        k.world.contextRefusesWhileHeld = false;
        CHECK(k.Switch(Target::XeFg).state == State::Probation);
        CHECK(k.world.contextLeaks == 0);
        Report("a row without the pin-first mark keeps the destroy-first order");
    }

    // ---- the live chain is read again on every attempt (a chain the game replaced) -----------------------------
    {
        std::printf("  SIM-M3-LIVE: a chain the game replaced is read again before the next switch\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        h.Present();
        // The game replaced the bridge's chain (a recreated bridge, a provider that rebuilt its inner chain): the state
        // still names the old, freed one. A cached read would capture it and tear it down again.
        Chain* replaced = h.world.NewChain(Class::FsrFg);
        replaced->refs = 2;
        replaced->libraryRef = true;
        Provider* provider = h.world.NewProvider(Class::FsrFg, replaced);
        h.world.bridge.fgChain = replaced;
        h.world.bridge.provider = provider;
        h.world.windowChain = replaced;
        h.world.globals.currentFGSwapchain = replaced;
        h.world.globals.currentFG = provider;
        h.world.oldSwapChain = nullptr;
        h.state.active = Target::FsrFg;
        h.world.active = Target::FsrFg;
        h.world.clock += kCooldownMs + 1;
        const SwitchResult next = h.Switch(Target::XeFg);
        CHECK(next.state == State::Probation);
        CHECK(h.world.useAfterFree.empty());
        CHECK(h.world.capturesOfFreed == 0);
        CHECK(h.state.live.chain == h.world.windowChain); // the new chain, not the one the state had cached
        h.Invariants("SIM-M3-LIVE");
        Report("every attempt re-reads the live chain, so a replaced one is never captured or torn down");
    }

    // ---- the present that ran a switch that ended detached is not an error for the game ------------------------
    {
        std::printf("  SIM-M2-SWITCHING-PRESENT: the present that ran the switch answers S_OK\n");
        Harness h(Class::FsrFg);
        h.world.providerCreateFails = true;
        h.world.plainCreateFails = true;
        CHECK(h.Switch(Target::XeFg).state == State::Limp);
        CHECK(h.state.managed);
        // The bridge's present runs the switch in its first statement and then finds no chain: that present, and not only
        // the ones after it, answers what DetachedPresent answers.
        CHECK(DetachedPresent(h.state, h.ops, false) == kOk);
        h.world.clock += kLimpGiveUpMs;
        CHECK(DetachedPresent(h.state, h.ops, false) == kDeviceRemoved);
        Report("the switching present is inside the S_OK window, and the error comes only after the window");
    }

    // ---- a rollback runs the barrier, quiesce and drain the forward path runs -----------------------------------
    {
        std::printf("  SIM-ROLLBACK-STEPS: a rolled-back XeFG chain still gets its barrier, quiesce and drain\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation); // the chain judged from now on is XeFG's
        h.Present(kGenericFailure);                              // masked, the rollback is due on the next present
        CHECK(h.state.probation.RollbackDue());
        h.world.barrierBlocks = true; // best effort: a false barrier must not stop the rollback
        h.world.drainOk = false;      // and neither must a queue that is not idle
        const size_t first = h.world.events.size();
        const size_t orderFirst = h.world.order.size();
        const SwitchResult rolled = h.Switch(Target::XeFg); // nothing else is wanted; the judgement runs first
        CHECK(rolled.state == State::RolledBack);
        CHECK(rolled.reason == Fail::Probation);
        CHECK(h.state.live.cls == Class::Plain);

        // The three steps, in order, before the teardown that follows the detach.
        int barrier = -1;
        int quiesce = -1;
        int drain = -1;
        int teardown = -1;
        for (size_t i = first; i < h.world.events.size(); ++i)
        {
            const Step step = h.world.events[i].step;
            if (step == Step::Barrier && barrier < 0)
                barrier = static_cast<int>(i);
            else if (step == Step::Quiesce && quiesce < 0)
                quiesce = static_cast<int>(i);
            else if (step == Step::Drain && drain < 0)
                drain = static_cast<int>(i);
            else if (step == Step::Teardown && teardown < 0)
                teardown = static_cast<int>(i);
        }
        // The teardown starts with ClearChainRefs, which follows the rollback's Detach: its order entry is the marker.
        int clearRefs = -1;
        for (size_t i = orderFirst; i < h.world.order.size(); ++i)
            if (h.world.order[i] == "clear-refs" && clearRefs < 0)
                clearRefs = static_cast<int>(i);
        CHECK(barrier >= 0 && quiesce > barrier && drain > quiesce);
        CHECK(clearRefs >= 0); // the teardown ran after the three steps
        CHECK(teardown > drain);
        CHECK(h.world.useAfterFree.empty());
        h.Invariants("SIM-ROLLBACK-STEPS");
        if (!(barrier >= 0 && quiesce > barrier && drain > quiesce && clearRefs >= 0 && teardown > drain))
            std::printf("      order: barrier=%d quiesce=%d drain=%d teardown=%d clear-refs=%d (live class %s)\n", barrier,
                        quiesce, drain, teardown, clearRefs, ClassText(h.state.live.cls));
        Report("a rollback still proves the barrier, quiesces the provider and drains before it destroys");

        // The forward path keeps its own barrier before the detach.
        Harness f(Class::XeFg);
        const size_t before = f.world.events.size();
        CHECK(f.Switch(Target::Plain).state == State::Probation);
        int fbarrier = -1;
        int fteardown = -1;
        for (size_t i = before; i < f.world.events.size(); ++i)
        {
            if (f.world.events[i].step == Step::Barrier && fbarrier < 0)
                fbarrier = static_cast<int>(i);
            if (f.world.events[i].step == Step::Teardown && fteardown < 0)
                fteardown = static_cast<int>(i);
        }
        CHECK(fbarrier >= 0 && fteardown > fbarrier);
        Report("the forward path keeps its own barrier before the destroy");
    }

    // ---- every direction, both models, both preserve settings --------------------------------------------------
    {
        std::printf("  SIM-MATRIX: every direction, both models, both preserve settings, the witness on and off\n");
        struct Sequence
        {
            Class start;
            Target first;
            Target second;
            Target third;
        };
        const Sequence sequences[] = {
            { Class::FsrFg, Target::XeFg, Target::Plain, Target::FsrFg },
            { Class::XeFg, Target::FsrFg, Target::Plain, Target::XeFg },
            { Class::FsrFg, Target::Plain, Target::XeFg, Target::FsrFg },
            { Class::XeFg, Target::Plain, Target::FsrFg, Target::XeFg },
            { Class::Plain, Target::FsrFg, Target::XeFg, Target::Plain },
            { Class::Plain, Target::XeFg, Target::FsrFg, Target::Plain },
        };
        int runs = 0;
        for (Model model : { Model::M1, Model::M2 })
            for (bool preserve : { true, false })
                for (bool forwards : { false, true })
                    {
                        // A wrapper whose witness outlives the outer chain is not an M2 measurement: the classifier
                        // refuses that row ("the witness outlived the outer chain"), so the matrix cannot invent one.
                        if (model == Model::M2 && forwards)
                            continue;
                    for (const Sequence& sequence : sequences)
                    {
                        Harness h(sequence.start);
                        h.world.libraryModel = model;
                        h.world.tableModel = model;
                        h.world.preserve = preserve;
                        h.world.forwardsWitness = forwards;
                        // The row follows the probe: a wrapper whose witness outlives its outer chain is not a measurement,
                        // so the row carries witness=no, which only an M1 row may do (M2 needs the accepted witness).
                        h.world.witnessHonoured = !forwards;
                        Chain* previous = h.bootChain;

                        const Target targets[3] = { sequence.first, sequence.second, sequence.third };
                        for (Target target : targets)
                        {
                            h.world.clock += kCooldownMs + 1;
                            const SwitchResult result = h.Switch(target);
                            CHECK(result.state == State::Probation);
                            CHECK(result.active == target);
                            if (result.state != State::Probation)
                                std::printf("      model=%d preserve=%d forwards=%d | %s -> %s stopped: %s\n",
                                            (int) model, (int) preserve, (int) forwards, ClassText(sequence.start),
                                            TargetText(target), FailText(result.reason));
                            if (previous != h.state.live.chain)
                                h.FreedOnce(previous); // the chain that left is gone exactly once
                            previous = static_cast<Chain*>(h.state.live.chain);
                            h.Invariants("SIM-MATRIX");
                            // A real present on the new chain dispatches frame generation for a provider class.
                            const int dispatch = h.world.hooks.dispatch;
                            h.Present();
                            if (target != Target::Plain)
                                CHECK(h.world.hooks.dispatch > dispatch);
                            ++runs;
                        }
                        CHECK(h.world.retired <= 3);
                    }
                }
        std::printf("    %d switches in the matrix\n", runs);
        Report("every direction frees each old chain once, keeps one chain on the window and restores the state");
    }

    // ---- SIM-QUEUE: every chain on the bridge's queue, the interop never left on a library's own queue ------------
    {
        std::printf("  SIM-QUEUE-six-directions: boot FSR FG, then FSR->XeFG->plain->FSR->plain->XeFG->FSR\n");
        Harness h(Class::FsrFg);
        Queue* bridge = h.world.bridgeQueue;
        CHECK(bridge == h.bootProvider->ownQueue); // at a frame-generation boot the bridge copies on the library's queue
        const Target order[] = { Target::XeFg, Target::Plain, Target::FsrFg, Target::Plain, Target::XeFg, Target::FsrFg };
        for (Target target : order)
        {
            h.world.clock += kCooldownMs + 1;
            const SwitchResult result = h.Switch(target);
            CHECK(result.state == State::Probation);
            if (result.state != State::Probation)
                std::printf("      -> %s stopped: %s\n", TargetText(target), FailText(result.reason));
            // The chain on the window was created on the bridge's queue, whatever the old library owned.
            auto* live = static_cast<Chain*>(h.state.live.chain);
            auto* provider = static_cast<Provider*>(h.state.live.provider);
            if (target == Target::Plain)
                CHECK(live != nullptr && live->queue == bridge);
            else
                CHECK(provider != nullptr && provider->gameQueue == bridge && provider->ownQueue != bridge);
            // The interop is back on the bridge's queue, never on the new library's own one.
            CHECK(h.world.interopQueue == bridge);
            h.Present();
            h.Invariants("SIM-QUEUE-six-directions");
        }
        // The boot library is long gone; its queue lives on because the switch pinned it once.
        CHECK(h.world.pinnedQueue == bridge);
        CHECK(!bridge->freed);
        Report("the six directions put every chain on the bridge's pinned queue and leave the interop on it");
    }
    {
        std::printf("  SIM-QUEUE-xefg-plain: an XeFG boot's own queue outlives XeFG for the plain chain\n");
        Harness h(Class::XeFg);
        Queue* xefgQueue = h.bootProvider->ownQueue;
        CHECK(h.world.globals.currentCommandQueue == xefgQueue); // the state the switch starts from
        CHECK(h.Switch(Target::Plain).state == State::Probation);
        auto* plain = static_cast<Chain*>(h.state.live.chain);
        CHECK(plain != nullptr && plain->queue == h.world.bridgeQueue);
        CHECK(!xefgQueue->freed); // only the switch's pin holds it now, and that is enough
        h.Present();
        h.Invariants("SIM-QUEUE-xefg-plain");
        Report("XeFG -> plain creates the plain chain on a queue XeFG's destroy cannot free");
    }
    {
        std::printf("  SIM-QUEUE-interop: the library's create does not leave the interop on its own queue\n");
        Harness h(Class::FsrFg);
        Queue* before = h.world.interopQueue;
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        auto* provider = static_cast<Provider*>(h.state.live.provider);
        CHECK(provider != nullptr && h.world.interopQueue == before && h.world.interopQueue != provider->ownQueue);
        CHECK(h.world.clobbers == 2); // the re-entry did move it, at boot and at the switch
        h.world.clock += kCooldownMs + 1;
        CHECK(h.Switch(Target::FsrFg).state == State::Probation); // XeFG's own queue dies here
        h.Present();
        h.Invariants("SIM-QUEUE-interop");
        Report("the interop is given back its queue after every create, so XeFG's teardown frees nothing it uses");
    }
    {
        std::printf("  SIM-QUEUE-rollback: a probation rollback from XeFG puts plain on the pinned queue\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        CHECK(h.Present(kGenericFailure) == kOk);
        const SwitchResult rolled = h.Switch(Target::XeFg);
        CHECK(rolled.state == State::RolledBack);
        auto* plain = static_cast<Chain*>(h.state.live.chain);
        CHECK(plain != nullptr && plain->cls == Class::Plain && plain->queue == h.world.bridgeQueue);
        h.Present();
        h.Invariants("SIM-QUEUE-rollback");
        Report("the rollback path captures the same pinned queue as the forward path");
    }
    {
        std::printf("  SIM-QUEUE-global-elsewhere: the global queue names another queue at the capture\n");
        // A game's own D3D12 create through the hooked factory points the global queue somewhere else; the next chain is
        // still created on the bridge's pinned queue.
        Harness h(Class::FsrFg);
        Queue* elsewhere = h.world.NewQueue("a queue the switch does not own");
        h.world.globals.currentCommandQueue = elsewhere;
        h.world.target = Target::XeFg;
        h.world.active = h.state.active;
        CHECK(RunSwitch(h.state, h.ops, h.world.clock).state == State::Probation);
        auto* provider = static_cast<Provider*>(h.state.live.provider);
        CHECK(provider != nullptr && provider->gameQueue == h.world.bridgeQueue && provider->gameQueue != elsewhere);
        h.Present();
        h.Invariants("SIM-QUEUE-global-elsewhere");
        Report("the creation queue is the pinned bridge queue whatever the global queue names");
    }
    {
        std::printf("  SIM-QUEUE-bridge-recreated: a bridge recreated at the old one's address pins its own queue\n");
        Harness h(Class::FsrFg);
        CHECK(h.Switch(Target::Plain).state == State::Probation); // pins the boot library's queue
        Queue* firstPin = h.world.pinnedQueue;
        CHECK(firstPin != nullptr);
        // The game recreates its bridge and the new bridge lands at the freed one's address: a new boot library, whose own
        // queue is the new bridge's copy queue and the interop's. The module drops the old state, as the attach point does.
        const Policy policy = h.state.policy;
        h.state = SwitchState {};
        h.state.policy = policy;
        h.Boot(Class::FsrFg);
        Queue* recreated = h.world.bridgeQueue;
        CHECK(recreated != firstPin);
        h.world.clock += kCooldownMs + 1;
        // The first switch away from the new boot library frees its queue unless the switch pinned it.
        CHECK(h.Switch(Target::XeFg).state == State::Probation);
        CHECK(h.world.pinnedQueue == recreated);
        CHECK(!recreated->freed);
        auto* provider = static_cast<Provider*>(h.state.live.provider);
        CHECK(provider != nullptr && provider->gameQueue == recreated);
        h.Present();
        h.Invariants("SIM-QUEUE-bridge-recreated");
        Report("the pin follows the bridge's queue, not the bridge's address: a recreated bridge keeps its own queue alive");
    }
    {
        std::printf("  SIM-QUEUE-pin-once: one reference per bridge, however many switches\n");
        Harness h(Class::FsrFg);
        const uint32_t before = h.world.bridgeQueue->refs;
        const Target order[] = { Target::Plain, Target::FsrFg, Target::Plain };
        for (Target target : order)
        {
            h.world.clock += kCooldownMs + 1;
            CHECK(h.Switch(target).state == State::Probation);
        }
        // The boot library's own reference went with it; the pin is the one left, plus the live chain's.
        auto* plain = static_cast<Chain*>(h.state.live.chain);
        CHECK(plain != nullptr && plain->queue == h.world.bridgeQueue);
        CHECK(h.world.bridgeQueue->refs == before - 1 + 1 + 1);
        h.Invariants("SIM-QUEUE-pin-once");
        Report("the pin is taken once and the queue's references balance across three switches");
    }

    // ---- the session budget in the simulation ------------------------------------------------------------------
    {
        std::printf("  SIM-BUDGET: nine switches, the ninth refused, never a ninth shell\n");
        Harness h(Class::FsrFg);
        Target next[9] = { Target::XeFg, Target::FsrFg, Target::XeFg, Target::FsrFg, Target::XeFg,
                           Target::FsrFg, Target::XeFg, Target::FsrFg, Target::XeFg };
        int commits = 0;
        for (int i = 0; i < 9; ++i)
        {
            h.world.clock += kCooldownMs + 1;
            const SwitchResult result = h.Switch(next[i]);
            if (result.state == State::Probation)
                ++commits;
            else
            {
                CHECK(result.reason == Fail::Budget);
                CHECK(i == 8);
            }
        }
        CHECK(commits == static_cast<int>(kMaxSwitches));
        CHECK(h.world.retired == static_cast<int>(kMaxSwitches));
        h.Invariants("SIM-BUDGET");
        Report("eight commits, the ninth refused, eight retired shells");
    }

    if (g_failed == 0)
        std::printf("ALL COM SIMULATION SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
