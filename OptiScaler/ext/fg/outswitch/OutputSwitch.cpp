#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/fg/outswitch/BridgeRegistry.h"
#include "ext/fg/outswitch/BridgeSwap.h"
#include "ext/fg/outswitch/HookAccess.h"
#include "ext/fg/outswitch/HookClock.h"
#include "ext/fg/outswitch/HookRebind.h"
#include "ext/fg/outswitch/OpsGates.h"
#include "ext/fg/outswitch/OutputSwitch.h"
#include "ext/fg/outswitch/Probe.h"
#include "ext/fg/outswitch/ProviderOps.h"
#include "ext/fg/outswitch/SwitchOpsContract.h"
#include "ext/fg/outswitch/SwitchPolicy.h"
#include "ext/fg/outswitch/SwitchLog.h"
#include "ext/fg/outswitch/SwitchNotify.h"
#include "ext/fg/outswitch/SwitchScript.h"

#include <ext/gpu/Gpu.h>

#include <hooks/FG_Hooks.h>

#include <with_dx12/dx11_with_dx12_sync.h>
#include <with_dx12/dx11_with_dx12_sc.h>

#include <Config.h>
#include <State.h>

#include <menu/menu_overlay_dx.h>

#include <atomic>
#include <string>

// The composition of the live output switch. Everything the pure core asks for is answered here from the bridge, the
// provider adapters (ProviderOps.h) and the process state; the rules themselves are SwitchSequence.h's, ChainLifetime.h's and
// SwitchPolicy.h's, and the shape of every call is SwitchOpsContract.h's.
//
// The module owns one bridge, one chain and one sequence: the state below is touched by the present thread only, the status
// is the one thing other threads read (StatusBox.h).

namespace ext::fg::outswitch
{
namespace
{
StatusBox g_status;
BridgeSwap g_swap;
// The one state the sequence and the two present seams judge: the seams receive the swap (BridgeSwap.h), so the state they
// read is the swap's own. A second SwitchState here would make probation, its fault and the limp window inert.
SwitchState& g_state = g_swap.state;
Script g_script;
bool g_scriptLoaded = false;
FaultGate g_fault;
// The epoch a switch bumps: read by the automatic multiplier's thread, written on the present thread, so it is atomic.
std::atomic<uint32_t> g_epoch { 0 };
uint32_t g_retiredShells = 0;
uint32_t g_captureGeneration = 0;
// A "try again" the menu asked for, applied on the present thread: the policy is read there, and the overlay draws on
// its own thread.
std::atomic<uint32_t> g_tryAgainPending { 0 }; // 0 = none, else the Target value + 1
bool g_witnessProbed = false;
bool g_witnessUsable = true;
// A switch the script asked for: the next begin line says so (why=script) instead of why=config.
bool g_scriptWhy = false;
// The recorder's sink is registered once: the switch itself does not know the recorder exists.
bool g_recorderRegistered = false;
// The bridge the switch state describes: the state is process-wide and a game path that recreates its bridge leaves every
// chain pointer in it pointing into the old one (SwitchSequence.h OwnedBy).
void* g_bridge = nullptr;

// The XeFG directions this build takes live (SwitchPolicy.h). All four: the device removals and driver faults that an
// XeFG teardown and a live FSR FG create showed were the switch's own (the next chain created on, and the D3D11 interop
// left on, a queue the old library owned, QueueRules.h; the provider created without the factory guard boot uses,
// FactoryGuards.cpp), not the libraries': the probe host presents through create -> destroy -> create of both classes
// cleanly (tools/fg-probe, --cycle and --replay).
constexpr XeFgDirections kXeFgDirections {};

constexpr const char* StepText(Step step)
{
    switch (step)
    {
    case Step::Begin:
        return "begin";
    case Step::Preflight:
        return "preflight";
    case Step::Refused:
        return "refused";
    case Step::Barrier:
        return "barrier";
    case Step::Quiesce:
        return "quiesce";
    case Step::Drain:
        return "drain";
    case Step::Capture:
        return "capture";
    case Step::Detach:
        return "detach";
    case Step::Teardown:
        return "teardown";
    case Step::Unhook:
        return "hooks";
    case Step::Create:
        return "create";
    case Step::Rehook:
        return "rehook";
    case Step::Adopt:
        return "adopt";
    case Step::Replay:
        return "replay";
    case Step::TestPresent:
        return "test-present";
    case Step::Commit:
        return "commit";
    case Step::Rollback:
        return "rollback";
    case Step::Probation:
        return "probation";
    case Step::Limp:
        return "limp";
    case Step::Dead:
        return "dead";
    case Step::Detached:
        return "detached";
    default:
        return "idle";
    }
}

Target TargetOf(FGOutput output)
{
    switch (output)
    {
    case FGOutput::FSRFG:
        return Target::FsrFg;
    case FGOutput::XeFG:
        return Target::XeFg;
    case FGOutput::DLSSG:
    case FGOutput::Reprojection:
        return Target::Plain; // known, but the switch refuses them: targetKnown stays false
    default:
        return Target::Plain;
    }
}

FGOutput OutputOf(Target target)
{
    switch (target)
    {
    case Target::FsrFg:
        return FGOutput::FSRFG;
    case Target::XeFg:
        return FGOutput::XeFG;
    default:
        return FGOutput::NoFG;
    }
}

bool TargetKnown(FGOutput output)
{
    return output == FGOutput::NoFG || output == FGOutput::FSRFG || output == FGOutput::XeFG;
}

// The recorder writes the eight milestones under the names its own header lists (ext/fgrec/FgRecorder.h). Translation
// only: the switch does not read anything a recorder decides.
const char* RecorderKind(NotifyPhase phase)
{
    switch (phase)
    {
    case NotifyPhase::Refused:
        return ext::fgrec::kSwitchRefused;
    case NotifyPhase::Teardown:
        return ext::fgrec::kSwitchTeardown;
    case NotifyPhase::Create:
        return ext::fgrec::kSwitchCreate;
    case NotifyPhase::Adopt:
        return ext::fgrec::kSwitchAdopt;
    case NotifyPhase::Commit:
        return ext::fgrec::kSwitchCommit;
    case NotifyPhase::Probation:
        return ext::fgrec::kSwitchProbation;
    case NotifyPhase::Rollback:
        return ext::fgrec::kSwitchRollback;
    default:
        return ext::fgrec::kSwitchBegin;
    }
}

unsigned RecorderProvider(Target target)
{
    switch (target)
    {
    case Target::FsrFg:
        return ext::fgrec::kProvFsr;
    case Target::XeFg:
        return ext::fgrec::kProvXeFg;
    default:
        return ext::fgrec::kProvNone;
    }
}

void RecorderSink(const SwitchEvent& event) noexcept
{
    ext::fgrec::OnSwitch(RecorderKind(event.phase), RecorderProvider(event.from), RecorderProvider(event.to),
                         event.text);
}

// The one fault the diagnostic key injects, once: the step answers what it would answer on a real failure, so the rollback
// to plain presentation is proven in a real process instead of only in the simulation. `active` is the condition the step
// exists in (OpsGates.h): a step that cannot be reached on this present leaves the fault for one where it can.
bool FaultAt(Step step, bool active = true)
{
    if (!g_fault.Take(step, active))
        return false;
    LOG_WARN("[fg-switch] injected fault at {}", StepText(step));
    return true;
}

// The real Ops: the contract's shape (SwitchOpsContract.h), answered from the bridge, the provider adapters and State.
struct ProductOps
{
    struct Capture : provider::Captured
    {
    };
    struct StateSnapshot : provider::StateSnapshot
    {
        StateSnapshot() = default;
        StateSnapshot(const provider::StateSnapshot& base) : provider::StateSnapshot(base) {}
    };
    struct CreateResult
    {
        void* chain = nullptr;
        void* provider = nullptr;
        uint32_t hr = 0;
        bool denied = false;
    };

    Dx11wDx12SC* bridge = nullptr;

    uint64_t NowMs() const { return outswitch::NowMs(); }
    void SleepMs(uint32_t ms) const { ::Sleep(ms); }

    bool Wanted()
    {
        if (!ext::Cfg().fg.outputHotSwap.value_or_default())
            return false;
        const FGOutput wanted = Config::Instance()->FGOutput.value_or_default();
        return TargetKnown(wanted) && wanted != ::State::Instance().activeFgOutput;
    }

    PreflightInputs ReadPreflight()
    {
        PreflightInputs inputs;
        auto& state = ::State::Instance();
        const FGOutput wanted = Config::Instance()->FGOutput.value_or_default();
        inputs.keyOn = Wanted();
        inputs.targetKnown = TargetKnown(wanted);
        inputs.target = TargetOf(wanted);
        inputs.activeKnown = TargetKnown(state.activeFgOutput);
        inputs.active = TargetOf(state.activeFgOutput);
        inputs.bridgePresent = state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;
        inputs.shuttingDown = state.isShuttingDown;
        inputs.secondBridge = Bridges().SecondBridge();
        inputs.loadersOk = provider::LoadersReady(inputs.target);
        inputs.hooksOk = Access::FgPresentHooked() || inputs.active == Target::Plain;
        inputs.hooksIdle = HookGateFor().InFlight() == 0 && HookGateFor().Passing() == 0;
        inputs.retiredShells = g_retiredShells;
        if (inputs.active != Target::Plain)
            inputs.activeModel = provider::RowFor(ClassOf(inputs.active));
        if (inputs.target != Target::Plain)
            inputs.targetModel = provider::RowFor(ClassOf(inputs.target));

        // The live chain's own answers, read only: window state, the waitable flag, the reference count at standstill.
        if (IDXGISwapChain4* chain = Access::Bridge::FgSwapChain(*bridge))
        {
            DXGI_SWAP_CHAIN_DESC desc {};
            if (SUCCEEDED(chain->GetDesc(&desc)))
            {
                inputs.windowed = desc.Windowed != FALSE;
                inputs.canCapture = desc.OutputWindow != nullptr;
            }
            DXGI_SWAP_CHAIN_DESC1 desc1 {};
            if (SUCCEEDED(chain->GetDesc1(&desc1)))
                inputs.waitable = (desc1.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
            const ULONG before = chain->AddRef();
            inputs.activeRefs = static_cast<uint32_t>(chain->Release());
            (void)before;
            if (inputs.activeModel.model == Model::M2 || inputs.activeModel.witnessHonoured)
            {
                // Asked once per session: the interface is placed and cleared, and a chain that refused it once refuses it
                // for as long as it is live.
                if (!g_witnessProbed)
                {
                    g_witnessProbed = true;
                    g_witnessUsable = provider::WitnessPlaceable(chain);
                }
                inputs.witnessUsable = g_witnessUsable;
            }
        }
        else
        {
            inputs.windowed = true;
            inputs.canCapture = Access::Bridge::RealSwapChain(*bridge) != nullptr;
        }

        const auto& record = Access::Bridge::Ext(*bridge);
        inputs.lastResizeMs = record.lastResizeMs;
        if (record.lastResizeThread != 0 && record.lastResizeThread != GetCurrentThreadId())
            inputs.lastForeignResizeMs = record.lastResizeMs;
        if (inputs.active != Target::Plain && inputs.activeModel.standstillRefs != 0)
            inputs.expectedRefs = probe::StandstillRefs() != 0 ? probe::StandstillRefs() : inputs.activeModel.standstillRefs;
        inputs.optiHdr = Access::Bridge::HdrOutput(*bridge);
        inputs.xeFgSwitchAllowed = XeFgDirectionAllowed(kXeFgDirections, inputs.active, inputs.target);
        inputs.gameHdr = false; // refusing game-native HDR is the preflight's own rule (Fail::GameHdr) once a value is recorded
        return inputs;
    }

    bool ReadLive(LiveChain& live)
    {
        auto& state = ::State::Instance();
        auto* chain = Access::Bridge::FgSwapChain(*bridge);
        auto* fg = Access::Bridge::Fg(*bridge);
        if (chain == nullptr)
            chain = static_cast<IDXGISwapChain4*>(Access::Bridge::RealSwapChain(*bridge));
        if (chain == nullptr)
            return false;
        live.chain = chain;
        live.provider = fg;
        live.cls = ClassOf(TargetOf(state.activeFgOutput));
        live.hwnd = Access::Bridge::Window(*bridge);
        return true;
    }

    bool Barrier()
    {
        auto& mutex = Dx11wDx12Sync::PresentResizeMutex();
        const uint64_t start = NowMs();
        while (NowMs() - start < kBarrierTimeoutMs)
        {
            if (mutex.try_lock())
            {
                mutex.unlock(); // a barrier, not a lock: holding it across the teardown would deadlock the XeFG worker
                return true;
            }
            ::Sleep(1);
        }
        return false;
    }

    void Quiesce(void* provider)
    {
        ::State::Instance().fgChanged = true;
        if (auto* fg = static_cast<IFGFeature_Dx12*>(provider))
            fg->Deactivate();
    }

    void Resume(void* provider)
    {
        if (auto* fg = static_cast<IFGFeature_Dx12*>(provider))
            fg->Activate();
    }

    DrainResult Drain(void* provider)
    {
        DrainResult result;
        if (FaultAt(Step::Drain))
        {
            result.game = false;
            result.fg = false;
            return result;
        }
        const uint64_t start = NowMs();
        // Every queue the old chain's work can still be on, once each: the global queue, the old provider's own game
        // queue (its frame-generation and UI lists run there), and the bridge's copy queue.
        ID3D12CommandQueue* global = ::State::Instance().currentCommandQueue;
        auto* game = static_cast<ID3D12CommandQueue*>(provider::ProviderQueue(provider));
        ID3D12CommandQueue* copy = Access::Bridge::CopyQueue(*bridge);
        // And the library's own queue when the switch created the provider: its present and the overlay drawn on its
        // real chain run there (the boot provider's library queue is the bridge's copy queue).
        auto* library = static_cast<ID3D12CommandQueue*>(provider::LibraryQueue(provider));
        result.game = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(global));
        if (game != nullptr && game != global)
            result.game = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(game)) && result.game;
        if (copy != global && copy != game)
            result.fg = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(copy));
        if (library != nullptr && library != global && library != game && library != copy)
            result.fg = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(library)) && result.fg;
        result.ms = static_cast<uint32_t>(NowMs() - start);
        return result;
    }

    bool ReadCapture(void* handle, Capture& out)
    {
        if (!provider::CaptureLive(handle, out))
            return false;
        // The plain rollback creates its chain through the bridge's own factory, never the wrapped one (ProviderOps.h):
        // without it nothing is touched, the switch refuses (NoCapture).
        if (!provider::CaptureRealFactory(bridge, out))
        {
            provider::ReleaseCapture(out);
            return false;
        }
        // The bridge's queue is pinned here, before anything is torn down, and every chain is created on it
        // (QueueRules.h). No queue: nothing is touched, the switch refuses.
        if (!provider::ChooseCreationQueue(bridge, out))
        {
            provider::ReleaseCapture(out);
            return false;
        }
        // The whole capture succeeded: its descriptor is the one a later rollback or limp creates plain from.
        provider::RecordChainDesc(bridge, out.desc);
        g_captureGeneration = Access::Bridge::Ext(*bridge).resizeGeneration;
        return true;
    }

    bool ReadBridgeCapture(Capture& out) { return provider::CaptureBridge(bridge, out); }
    void ReleaseCapture(Capture& capture) { provider::ReleaseCapture(capture); }

    StateSnapshot Snapshot() { return provider::SnapshotState(); }
    void RestoreState(const StateSnapshot& snapshot) { provider::RestoreState(snapshot); }

    void ReleaseInterop(void* hwnd)
    {
        MenuOverlayDx::CleanupRenderTarget(true, static_cast<HWND>(hwnd));
        BridgeReleaseInterop(bridge);
    }

    void Detach()
    {
        BridgeSetChain(bridge, nullptr);
        BridgeSetProvider(bridge, nullptr);
    }

    void ClearOldSwapChain()
    {
        // The release hook's stale pointer is only written when CreateSwapChain finds a FG chain, and Create() hands the
        // library no chain at all: there is nothing to clear, and the switch never sets it.
    }

    UnhookResult Unhook()
    {
        // Counted before the detach: the line that follows says how many trampolines left and how many slots were never
        // attached (a raw vtable address or null), which is what makes a partial set visible in the benchmark log.
        HookSlot slots[kHookSlots];
        const size_t count = Access::FgHookSlots(slots, kHookSlots);
        const size_t attached = AttachedSlots(slots, count, nullptr, 0);
        unhooked = static_cast<uint32_t>(attached);
        raw = static_cast<uint32_t>(count > attached ? count - attached : 0);
        return UnhookHookedSwapchain();
    }

    bool HooksTransactable() { return outswitch::HooksTransactable(); }

    bool HooksAttached(Target target)
    {
        if (target == Target::Plain)
            return true; // the plain presenter dispatches through the bridge's own chain, not through the hook slots
        return Access::FgPresentHooked();
    }

    CreateResult Create(Target target, const Capture& capture)
    {
        CreateResult out;
        if (FaultAt(Step::Create))
        {
            out.hr = static_cast<uint32_t>(E_FAIL);
            return out;
        }
        const provider::Created created = provider::Create(target, capture);
        out.chain = created.chain;
        out.provider = created.provider;
        out.hr = created.hr;
        out.denied = created.denied;
        return out;
    }

    bool Adopt(void* chain, void* provider, Target target)
    {
        if (FaultAt(Step::Adopt))
            return false;
        BridgeAdoptWithTransfer(bridge, chain, provider, target);
        BridgeRefreshDesc(bridge);
        return true;
    }

    bool Replay(void* chain)
    {
        if (FaultAt(Step::Replay))
            return false;
        // The order the review of the design fixed: the resize the switch's own epilogue must not count (M-4) is compared
        // against the generation captured before the teardown, the game's colour space is put back next, and the HDR and
        // window epilogue runs last (under OptiHDR it is the whole replay).
        if (!BridgeReplayResize(bridge, g_captureGeneration))
            return false;

        auto& record = Access::Bridge::Ext(*bridge);
        if (!Access::Bridge::HdrOutput(*bridge) && record.colorSpaceRecorded)
        {
            auto* live = static_cast<IDXGISwapChain4*>(chain);
            if (live != nullptr)
                live->SetColorSpace1(static_cast<DXGI_COLOR_SPACE_TYPE>(record.lastColorSpace));
        }
        // The part of the resize path's FG state a switch needs: the old provider's captured resources belong to a chain
        // that is gone, and "captured only" was a property of that pair. The provider was created for this chain in this
        // same present, so the context rebuild the resize path asks for with scChanged would only repeat its creation.
        auto& state = ::State::Instance();
        state.fgResetCapturedResources = true;
        state.fgOnlyUseCapturedResources = false;
        BridgeResizeEpilogue(bridge);
        return true;
    }

    uint32_t TestPresent(void* chain)
    {
        auto* live = static_cast<IDXGISwapChain*>(chain);
        if (live == nullptr)
            return kDeviceRemoved;
        return static_cast<uint32_t>(live->Present(0, DXGI_PRESENT_TEST));
    }

    bool SetWitness(void* chain) { return provider::PlaceWitness(chain); }
    void ReleaseWitness() { provider::ClearWitness(); }
    bool WitnessAlive() { return provider::WitnessAlive(); }

    void Deactivate(void* provider) { provider::DeactivateProvider(provider); }

    void ClearChainRefs(const LiveChain& live)
    {
        ::State::Instance().currentFGSwapchain = nullptr;
        if (live.cls == Class::Plain && live.chain != nullptr)
            FGHooks::ClearDx12InteropPresentSC(static_cast<IDXGISwapChain*>(live.chain));
    }

    bool HasSwapchainContext(void* provider) { return provider::HasSwapchainContext(provider); }

    bool ReleaseSwapchain(void* provider, void* hwnd)
    {
        if (FaultAt(Step::Teardown))
            return false;
        return provider::ReleaseSwapchain(provider, hwnd);
    }

    uint32_t ReleasePin(void* chain) { return provider::ReleasePin(chain); }
    void ShutdownProvider(void* provider) { provider::ShutdownProvider(provider); }
    void RetireProvider(void* /*provider*/) { ++g_retiredShells; }
    void ClearCurrentProvider() { ::State::Instance().currentFG = nullptr; }

    void Report(const Event& event)
    {
        // One line per step, in the shape the benchmark driver parses and a reader compares with the log (SwitchLog.h).
        // Logged outside the Detours window only (the sequence never reports inside it).
        if (event.step == Step::Idle)
            return;

        SwitchLogFields f;
        f.step = event.step;
        f.fail = event.fail;
        f.severity = event.severity;
        f.from = event.from;
        f.to = event.to;
        f.cls = event.cls;
        f.model = event.model;
        f.generation = event.generation;
        f.budgetLeft = event.a; // the commit line's budget; the other steps do not print it
        f.hr = event.b;
        f.ok = event.flagA;
        f.ms = event.ms;
        f.chain = reinterpret_cast<uintptr_t>(event.chain);
        f.thread = GetCurrentThreadId();
        f.resizeGen = Access::Bridge::Ext(*bridge).resizeGeneration;
        f.pinReleased = event.flagA;
        f.witnessAlive = event.flagB;
        f.witnessPlaced = event.step != Step::Teardown || event.c != 0;
        f.why = g_scriptWhy ? "script" : "config";

        switch (event.step)
        {
        case Step::Begin:
        case Step::Refused:
            // A scripted step is answered by its begin or by its refusal: either way the next attempt is the config's.
            g_scriptWhy = false;
            break;
        case Step::Preflight:
            f.loadersFsr = provider::LoadersReady(Target::FsrFg);
            f.loadersXe = provider::LoadersReady(Target::XeFg);
            break;
        case Step::Barrier:
            f.ok = event.flagA;
            break;
        case Step::Drain:
            f.gameDrained = event.flagA;
            f.fgDrained = event.flagB;
            break;
        case Step::Teardown:
        {
            // The identity of the module the class's present slot lives in: what the model row was matched on, for a reader
            // comparing two builds. A class without a module prints lib=none.
            LibraryIdentity identity;
            if (event.cls != Class::Plain && provider::IdentityFor(event.cls, identity) && identity.file[0] != 0)
            {
                std::snprintf(f.libraryFile, sizeof(f.libraryFile), "%s", identity.file);
                f.librarySize = identity.size;
                f.libraryStamp = identity.stamp;
            }
            break;
        }
        case Step::Unhook:
            f.unhooked = unhooked;
            f.raw = raw;
            break;
        case Step::Create:
            f.retries = event.a; // the attempt number is the number of retries that preceded this try
            break;
        case Step::Adopt:
        {
            const auto& record = Access::Bridge::Ext(*bridge);
            f.colorspace = record.colorSpaceRecorded ? record.lastColorSpace : 0;
            f.hdr = ext::hdr::Active();
            break;
        }
        case Step::Probation:
            f.presents = event.a;
            f.fgActive = ::State::Instance().activeFgOutput != FGOutput::NoFG;
            break;
        case Step::Limp:
            f.rebuild = event.flagA;
            break;
        default:
            break;
        }

        char line[512];
        FormatSwitchLine(f, line);
        LOG_INFO("[fg-switch] {}", line);

        // The queues around the create: the pinned queue, the queue the chain was created on, the interop's queue before
        // the create, the one the library's factory path left in it, the interop's queue now (after the restore), and the
        // new provider's game queue. A switch that keeps every queue alive reads creation = pinned = providerQueue and
        // interopNow = interopBefore.
        if (event.step == Step::Create && event.flagA)
        {
            const provider::QueueTrace trace = provider::LastQueueTrace();
            LOG_INFO("[fg-switch] queues pinned={:X} creation={:X} interopBefore={:X} libraryLeft={:X} interopNow={:X} "
                     "providerQueue={:X}",
                     reinterpret_cast<uintptr_t>(trace.pinned), reinterpret_cast<uintptr_t>(trace.creation),
                     reinterpret_cast<uintptr_t>(trace.interopBefore), reinterpret_cast<uintptr_t>(trace.interopClobbered),
                     reinterpret_cast<uintptr_t>(provider::InteropQueueNow()),
                     reinterpret_cast<uintptr_t>(provider::ProviderQueue(::State::Instance().currentFG)));
        }

        // The new class's present slot, once the chain exists: the rehook is upstream's own HookFGSwapchain, and this is
        // the line that proves which class it hooked (the chain the create line named is alive here).
        if (event.step == Step::Create && event.flagA && event.to != Target::Plain && event.chain != nullptr)
        {
            SwitchLogFields rehook;
            rehook.step = Step::Rehook;
            auto** vtable = *reinterpret_cast<void***>(const_cast<void*>(event.chain));
            rehook.vtbl = vtable != nullptr ? reinterpret_cast<uintptr_t>(vtable[8]) : 0;
            FormatSwitchLine(rehook, line);
            LOG_INFO("[fg-switch] {}", line);
        }

        // The recorder (ext/fgrec) reads the milestones through the observer; the switch itself never depends on it.
        const PhaseOfStep phase = PhaseOf(event.step, event.fail);
        if (phase.valid)
        {
            SwitchEvent notification;
            notification.phase = phase.phase;
            notification.from = event.from;
            notification.to = event.to;
            notification.fail = event.fail;
            notification.generation = event.generation;
            notification.number = event.a;
            notification.text = line;
            Notify(notification);
        }

        if (event.step == Step::Commit || event.step == Step::Rollback || event.step == Step::Detached)
        {
            ++g_epoch;
            // A switch changed the chain, so the preflight's witness answer describes the chain that is gone: the next
            // preflight asks the new one instead of reusing an answer about another object (a replaced chain can be of
            // another class, and a wrapper may forward the interface differently).
            g_witnessProbed = false;
        }
    }

    uint32_t unhooked = 0; // set by Unhook(): trampolines that left
    uint32_t raw = 0;      // slots that were never attached (a raw vtable address or null)

    void Publish(const Status& status) { g_status.Publish(status); }
};

// The contract, checked where the real binding is compiled: a call whose shape drifts fails here instead of at link time.
static_assert(ext::fg::outswitch::SwitchOps<ProductOps>, "the live output switch's product binding does not match SwitchOpsContract.h");

ProductOps MakeOps(void* bridge) { return ProductOps { static_cast<Dx11wDx12SC*>(bridge) }; }

// The two hooks the composition installs into the bridge's swap (BridgeSwap.h): the present that arrives while the bridge
// has no chain, and the result of the live chain's present.
uint32_t DetachedFromSwap(BridgeSwap& swap, bool realChainMissing, uint64_t presentStartMs)
{
    (void)presentStartMs;
    ProductOps ops = MakeOps(Bridges().Bridge());
    return DetachedPresent(swap.state, ops, realChainMissing);
}

uint32_t FgResultFromSwap(BridgeSwap& swap, uint32_t result, uint64_t presentStartMs)
{
    ProductOps ops = MakeOps(Bridges().Bridge());
    // The judgement is told how long this present took, not when it started: a start that is a count since boot is above
    // the stall limit on every machine that has been up for more than 2.5 s.
    const uint32_t presentMs = ElapsedMs(presentStartMs, NowMs());
    // A lost device is forwarded to the game; the reason the D3D12 device reports is what the log keeps, because the
    // game only sees the HRESULT on its own present.
    if (DeviceLost(result) && !swap.deviceLostLogged)
    {
        swap.deviceLostLogged = true;
        if (auto* device = ::State::Instance().currentD3D12Device)
            LOG_WARN("[fg-switch] the D3D12 device was removed: reason=0x{:08X}",
                     static_cast<uint32_t>(device->GetDeviceRemovedReason()));
        else
            LOG_WARN("[fg-switch] the device was lost and there is no D3D12 device to ask for its reason");
    }
    // The probation fault only exists inside a probation: outside one there is nothing to judge, so injecting it would
    // hand the game an error HRESULT for a failure that has no meaning.
    if (FaultAt(Step::Probation, swap.state.probation.InProbation()))
        return OnFgPresentResult(swap.state, ops, 0x80004005u, presentMs);
    return OnFgPresentResult(swap.state, ops, result, presentMs);
}

void ApplyScript()
{
    if (!g_scriptLoaded)
    {
        g_scriptLoaded = true;
        g_fault.step = ParseFaultStep(ext::Cfg().fg.outputSwitchFault.value_or_default().c_str());
        ParseScript(ext::Cfg().fg.outputSwitchScript.value_or_default().c_str(), g_script);
    }
    if (g_script.Empty())
        return;

    const uint64_t now = NowMs();
    g_script.Start(now);
    Target due = Target::Plain;
    if (g_script.Due(now, due))
    {
        Config::Instance()->FGOutput.set_volatile_value(OutputOf(due));
        g_scriptWhy = true;
        LOG_INFO("[fg-switch] script sets the configured output to {} at {} ms", TargetText(due), now - g_script.startMs);
    }
}
} // namespace

void OnPresent()
{
    // The present's start on the switch clock is stamped on EVERY present, before the key check: the two present seams
    // measure a present's duration from it, and a value left over from the last key-on present grows by one frame per
    // present while the key is off.
    const uint64_t start = NowMs();
    g_swap.presentStartMs = start;

    // The whole idle cost after the stamp above: one config read and one compare. A due judgement of the chain
    // that is on the window still runs with the key off: the rollback of a chain that failed its presents must not be
    // held back because the player unticked the box after the switch.
    const bool keyOn = ext::Cfg().fg.outputHotSwap.value_or_default();
    if (!keyOn && !g_state.probation.RollbackDue())
        return;

    void* bridge = Bridges().Bridge();
    if (bridge == nullptr)
        return;

    // The recorder subscribes once; with the recorder's key off the tap is one relaxed load.
    if (!g_recorderRegistered)
        g_recorderRegistered = AddSwitchObserver(&RecorderSink);

    // The switch state belongs to one bridge: a state that names another one describes chains that no longer exist, so
    // it is dropped and the status goes back to idle (the per-target memory stays). A new bridge can be allocated at the
    // address of the freed one, which the address compare cannot see, so the attach point below resets as well.
    bool reset = OwnedBy(g_state, g_bridge, bridge);
    if (reset)
    {
        g_witnessProbed = false;
        g_witnessUsable = true;
        // The queue pinned for the old bridge is given back; the next capture pins this bridge's own.
        provider::UnpinBridgeQueue();
    }

    // The composition installs the switch into the bridge's swap once: the two seams answer the line's own values until then.
    if (SwapFor(bridge) != &g_swap)
    {
        // A bridge this swap has not served yet: whatever the state describes belongs to an object that is gone, even when
        // its address was reused. The per-target memory and the session budget stay, as they do on a bridge change.
        reset = true;
        const Policy policy = g_state.policy;
        g_state = SwitchState {};
        g_state.policy = policy;
        g_witnessProbed = false;
        g_witnessUsable = true;
        g_swap.detached = &DetachedFromSwap;
        g_swap.fgResult = &FgResultFromSwap;
        g_swap.deviceLostLogged = false;
        AttachSwap(bridge, &g_swap);
    }

    if (reset)
        g_status.Publish(Snapshot(g_state));

    // The menu's "try again", applied here, on the thread that owns the policy.
    const uint32_t posted = g_tryAgainPending.exchange(0, std::memory_order_acq_rel);
    if (posted != 0)
        TryAgain(g_state, static_cast<Target>(posted - 1));

    if (keyOn)
        ApplyScript(); // the benchmark keys are read only while the feature is on

    ProductOps ops = MakeOps(bridge);
    (void)RunSwitch(g_state, ops, start);
}

uint32_t SwitchEpoch() { return g_epoch.load(std::memory_order_acquire); }

Status MenuStatus() { return g_status.Read(); }

void TryAgainTarget(Target target)
{
    // The menu thread only posts it: the policy memory is read and written by the present thread (SwitchSequence.h), and
    // clearing it here would be a data race with the sequence that is judging a chain at this moment.
    g_tryAgainPending.store(static_cast<uint32_t>(target) + 1, std::memory_order_release);
}

bool RestartNeeded() { return g_state.dead; }
} // namespace ext::fg::outswitch
