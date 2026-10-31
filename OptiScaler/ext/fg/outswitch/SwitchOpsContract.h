#pragma once
// The Ops contract of the live output switch, frozen. The one place every rule of the binding is written down.
//
// SwitchSequence.h and ChainLifetime.h are templates over a binding: the product binds the bridge, the tests bind the
// refcounted COM fakes. Nothing in the templates stops a later package from changing one call's shape and discovering it
// only at link time, so the contract is written once here as a concept and every binding asserts it:
//
//     static_assert(ext::fg::outswitch::SwitchOps<SimOps>);
//
// The rules the signatures carry, which a concept cannot express:
//   * Create() is retry-idempotent for one target and window: a second call hands back the same provider object, not a
//     fresh one, so the retry loop in BringUp neither leaks a shell nor returns a provider the sequence already shut
//     down. A failed call may still return a provider (and always returns the last one). Create() refuses while
//     currentFG, currentFGSwapchain or the release hook's oldSwapChain still names something: the switch owns no chain
//     at that point, and a library that finds one answers with the existing chain instead of a new one.
//   * ReadPreflight() fills activeRefs with the live chain's reference count read now and expectedRefs with the count
//     the row measured at standstill (ModelRow::standstillRefs, ExpectedRefs() in LibraryModel.h); the live in-game
//     probe's count, when it ran, answers the same question and wins. Both are read-only; a difference refuses the
//     switch transiently (Fail::RefMismatch) instead of letting an unaccounted holder surface in the teardown.
//   * ReadPreflight() also fills witnessUsable: true when the row in play does not need the witness, or when a
//     read-only place-and-clear of the private-data interface on the live chain succeeded. An honoured row whose
//     witness cannot be placed refuses before anything is released, in the preflight and again in the teardown.
//   * ReadLive() reads the chain the module did not create (the boot chain) and the one that is on the window now. It is
//     called on every attempt that is not detached: the state may name a chain the game has replaced since the last
//     present (a recreated bridge, a provider that rebuilt its inner chain), and capturing or tearing down a cached name
//     would touch freed memory. It never touches the chain: no release, no GetDesc on a chain the module does not own yet.
//     Empty() = "I cannot name it", which refuses the switch.
//   * Wanted() is the whole idle cost of the feature: one relaxed load of the key and one compare, answering exactly "the
//     key is on and the configured output is not the active one". It never stands for the judgement of the chain that is
//     already on the window: RunSwitch() handles a due probation/watchdog rollback BEFORE it asks Wanted(), because after
//     a commit the configured output equals the active one and a chain that fails its presents must still be replaced.
//   * Snapshot()/RestoreState() cover every global a library's factory writes back while it creates its inner chain,
//     including the shared D3D12 device, command queue and queue type the bridge hands to the D3D12 side of the
//     interop (the state the whole D3D12 path reads, not just the module's own copy), and currentSwapchain, which the
//     library's own create points at its inner chain and every reader of that global expects to name the bridge.
//     RestoreState() is idempotent: it
//     writes the same values back and logs, so restoring twice, or restoring when nothing changed, is harmless. The
//     three chain globals it must never write are currentFGSwapchain, currentFG and activeFgOutput: those are the
//     switch's own decision (Create/Adopt/Commit).
//   * The queues of a capture follow QueueRules.h, the one place they are decided: the bridge's copy queue is pinned (and
//     re-pinned when it changes), every chain is created on it, and RestoreState() gives the interop its queue back.
//   * ReadBridgeCapture() builds the capture from the bridge's own hidden real chain and its cached descriptor, never
//     from a frame-generation chain the switch has already detached.
//   * Publish() is called only when the status changed (SwitchSequence.h) and must be safe from the present thread for
//     a reader on the menu thread (StatusBox.h).
//   * NowMs() is monotonic (QPC) and the same clock as the present thread's presentStartMs.
//   * RunSwitch(), DetachedPresent() and OnFgPresentResult() are called on the bridge's present thread only; the state
//     has no synchronisation. Report() and Publish() must not allocate or block, because they run inside a present.
//
// The hook rebind: gate ownership, the Detours order, the pass-through answer per body
// -------------------------------------------------------------------------------------
// One gate per hook set, not one per class or per slot: the hook installs one class at a time over the same ten vtable
// slots and detaches them in one transaction, and the next class's slots are written only after that transaction
// committed. A per-slot gate would multiply the wait without making anything safer, and a per-class gate would be the
// same object reached through another name. Every body uses HookGateFor() (HookScope.h); nothing else builds a gate.
//
// The switch thread's own hooked calls find the gate open. The pin release and the dud teardown go through the release
// hook and the test present through the present hook, and both run before step 7 (unhook) or after the new class was
// hooked and the gate reopened. This is an invariant of the step order, not a promise of the hook bodies: moving the
// unhook in front of the teardown would close the gate under the switch's own release, and that release would take the
// pass-through path instead of its own. A test pins it.
//
// Unhook() does exactly this, in this order:
//   0  HooksTransactable(): one open-and-abort of an empty transaction, before anything is detached or torn down. Detours
//      has one process-wide transaction, so another thread's pending one (a LoadLibrary-time attach, a library create that
//      reaches the hook) makes Begin answer ERROR_INVALID_OPERATION. Asking here turns that into a transient refusal that
//      costs nothing; the detach itself retries a few times before it answers Busy, and never remembers the target.
//   1  gate.DetachFor(kHookDrainTimeoutMs, NowMs, SleepMs): close the counted gate, wait for the bodies inside, close
//      the pass-through phase, wait for the pass-throughs already calling an original. False = Busy.
//   2  On Busy: gate.Reattached() before returning, so an abandoned detach does not leave the gate shut for the
//      session. No slot was touched.
//   3  DetourTransactionBegin(); DetourUpdateThread() for every thread of the process; a DetourDetach() call for every
//      attached slot; then step 4 decides whether the transaction commits. Nothing is detached before step 1 answered
//      true: Detours suspends a thread whose instruction pointer is inside a patched prologue, and a counted body
//      suspended there could never leave, so the wait has to finish first. A Busy result never begins a transaction.
//      The thread list is taken FRESH for each of the three attempts, so a thread that exited before it was suspended (the
//      enlist fails) is not listed again. A C++ allocation failure inside the window is caught by the structured exception
//      guard and aborts the transaction, which resumes every thread.
//      "Every attached slot" is literal: upstream attaches Present1 and ResizeBuffers1 only when the vtable has them, and
//      the two Get bodies and the waitable hook only when the active output is XeFG at hook time, so FSR FG runs seven of
//      the ten. The rebind detaches exactly what its trampoline filter finds (a MEM_PRIVATE original), and the rehook is
//      upstream's own HookFGSwapchain, which reads the NEW class's activeFgOutput, early-returns while any original is
//      still non-null (the rebind nulls every original after the detach, which is what makes it start over), and reopens
//      the gate as its last statement (the attach-failure path included: the gate must not stay shut for a class that was
//      not hooked either).
//   4  gate.Complete(): false means a pass-through arrived after the phase closed, so the gate was not drained; then
//      DetourTransactionAbort() instead of a commit, Reattached(), and Failed. A trampoline is never cleared on a gate
//      that did not answer true. Our own code in the window allocates nothing: the logging, the slot array, the thread
//      list and the stall watch are all built before DetourTransactionBegin. Detours itself allocates and frees there
//      (`new`/`delete` in DetourUpdateThread, DetourDetach and the commit), which is why the window runs under
//      DetourStallWatch: it resumes every enlisted thread after kDetourStallMs, so a thread suspended inside the heap lock
//      costs that bound instead of hanging the present thread.
//   5  Commit, then null every original (a committed detach restores the variable to the real target, it does not null
//      it), and the gate stays closed (cleared) until the new class is hooked. The rehook that reopens it is the last
//      statement of upstream's HookFGSwapchain, so it runs for a class the attach reached and for one it did not. A
//      `Release` that arrives in that window is the refused answer below, not the original.
// A plain target has no hooks to detach and reopens the gate at the commit. Unhook() is never called from a thread that
// holds a HookScope: a body waiting for its own detach would always time out.
//
// Every body starts with the same two statements (HookScope.h), and a slot whose refused answer is out-parameters passes
// a callable for them (the two Get bodies): 20 lines in the ten bodies, plus the two header lines that make them compile
// (the include that declares the scope type and the friend declaration that lets the rebind name the private trampolines),
// plus the one line HookFGSwapchain ends with to reopen the gate: 23 added upstream lines, and 25 with the two present
// lines of the bridge that were replaced. Its pass-through answer is the one below.
// "Original" is the `o_*` trampoline the hook was attached over, still valid while DetachFor is waiting; "refused" is
// the answer once Complete() restored the originals, used only by a call that arrived after the detach had drained (the
// chain it names is already gone).
//
//   hkFGPresent            original o_FGSCPresent(This, SyncInterval, Flags);                    refused: S_OK
//   hkFGPresent1           original o_FGSCPresent1(This, SyncInterval, Flags, p);               refused: S_OK
//   hkFGRelease            original o_FGRelease(This); its ULONG is the answer;                refused: 0
//                          never the AddRef / preserve / bridge-delete branches: the switch cleared the global pointer
//                          before the release, and a swallowed release would leak a reference the model counts on
//   hkSetFullscreenState   original o_FGSCSetFullscreenState(This, Fullscreen, pTarget);        refused: S_OK
//   hkGetFullscreenState   original o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);
//                          its HRESULT; refused: *pFullscreen = FALSE, *ppTarget = nullptr when not null, S_OK
//   hkResizeBuffers        original o_FGSCResizeBuffers(This, Count, Width, Height, Format, Flags);
//                          its HRESULT;                                                         refused: S_OK
//   hkResizeBuffers1       original o_FGSCResizeBuffers1(This, ..., pCreationNodeMask, ppPresentQueue);
//                          its HRESULT;                                                         refused: S_OK
//   hkResizeTarget         original o_FGSCResizeTarget(This, pNewTargetParameters);             its HRESULT;
//                          refused: S_OK
//   hkGetFullscreenDesc    original o_FGSCGetFullscreenDesc(This, pDesc); its HRESULT;
//                          refused: *pDesc = DXGI_SWAP_CHAIN_FULLSCREEN_DESC {} with Windowed = TRUE, S_OK
//   hkGetFrameLatencyWaitableObject
//                          original o_FGSCGetFrameLatencyWaitableObject(This); its HANDLE;      refused: nullptr
//
// A refused answer never creates, destroys or releases anything, and neither pass-through path runs any of the body's
// own logic: a deactivate, a resize side effect or a mode rewrite after the chain was torn down would act on a chain
// the switch no longer owns. The pass-through protects the trampoline, not the chain: a call that takes it while the
// detach drains calls an original on a chain the switch has already decided to destroy. That is why the barrier, the
// quiesce and the drain run before the detach (and best effort again in a rollback) - the pass-through makes a call
// safe to make, never safe to depend on.
//
// The rollback's steps 1-3, and the transient backoff
// ------------------------------------------------
// A rollback (a failed judgement after a commit, or a failed step) runs Barrier() for a chain of the XeFG class,
// Quiesce() and Drain() best effort before it detaches and tears down. They cannot refuse the rollback: a barrier that
// times out or a queue that does not prove idle delays it by a bounded number of presents, because a chain that already
// failed its presents must not stay on the window. They exist because the rollback reaches a teardown the forward path
// only reaches after those three steps, and a teardown while the XeFG present worker or queued GPU work still runs is
// exactly what they exclude.
//
// A transient refusal raised after the switch started touching things (a barrier, a drain, a busy detach) pauses the
// whole feature for 1, 2, 4, 8, 16 and then 30 s; the pause is per refusal in a row and starts over at the next commit,
// at the menu's "try again", or when a pause longer than the longest one passed before the next Go. The tenth refusal in
// a row stores its cause as a hard one, so the menu offers a restart instead of the module stalling for 30 s at a time for
// the rest of the session. A busy detach is not remembered before that: a call in flight is not a property of the target.
// A preflight refusal (a mismatched reference count included) is free to look at again: it pauses 1 s, never doubles and
// never counts toward the tenth, because nothing was touched and the next present may find the condition gone.
//
// The identity sources
// --------------------
// A row is keyed on the module that holds the chain's present slot: file name, size and PE TimeDateStamp, read from the
// mapped image. For XeFG the row also carries the companion DLL the measured configuration needs (libxell.dll, the XeLL
// context the product always attaches): another companion is another build of the pair the row measured, and the
// lookup refuses it. The target's identity before the first chain of its class exists comes from the same loaded module
// (the provider adapter has loaded the library to know it can create), never from the live chain. The in-game probe logs
// the live chain's `file= size= stamp=` once per class at the first present, so a row can be checked against the DLLs a
// player actually runs.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_sequence.cpp, fg_switch_com_sim.cpp).

#include "ext/fg/outswitch/ChainLifetime.h"
#include "ext/fg/outswitch/SwitchPolicy.h"
#include "ext/fg/outswitch/SwitchSequence.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <concepts>
#include <cstdint>
#include <utility>

namespace ext::fg::outswitch
{
template <class Capture>
concept CaptureLike = requires(Capture capture) { capture.hwnd; };

template <class Result>
concept CreateResultLike = requires(Result result) {
    result.chain;
    result.provider;
    result.hr;
    result.denied;
};

template <class Ops>
concept SwitchOps =
    requires {
        typename Ops::Capture;
        typename Ops::StateSnapshot;
        typename Ops::CreateResult;
    } && CaptureLike<typename Ops::Capture> && CreateResultLike<typename Ops::CreateResult> &&
    requires(Ops ops, Target target, void* handle, uint32_t delay, Event event, Status status, LiveChain live) {
        // Clock.
        { ops.NowMs() } -> std::convertible_to<uint64_t>;
        { ops.SleepMs(delay) } -> std::same_as<void>;
        // Preflight, read only.
        { ops.Wanted() } -> std::convertible_to<bool>;
        { ops.ReadPreflight() } -> std::same_as<PreflightInputs>;
        { ops.ReadLive(live) } -> std::convertible_to<bool>;
        // Barrier, quiesce, drain.
        { ops.Barrier() } -> std::convertible_to<bool>;
        { ops.Quiesce(handle) } -> std::same_as<void>;
        { ops.Resume(handle) } -> std::same_as<void>;
        { ops.Drain(handle) } -> std::same_as<DrainResult>;
        // Capture and the state the libraries overwrite while they create their inner chain.
        { ops.ReadCapture(handle, std::declval<typename Ops::Capture&>()) } -> std::convertible_to<bool>;
        { ops.ReadBridgeCapture(std::declval<typename Ops::Capture&>()) } -> std::convertible_to<bool>;
        { ops.ReleaseCapture(std::declval<typename Ops::Capture&>()) } -> std::same_as<void>;
        { ops.Snapshot() } -> std::same_as<typename Ops::StateSnapshot>;
        { ops.RestoreState(std::declval<const typename Ops::StateSnapshot&>()) } -> std::same_as<void>;
        // Interop, detach, hooks.
        { ops.ReleaseInterop(handle) } -> std::same_as<void>;
        { ops.Detach() } -> std::same_as<void>;
        { ops.ClearOldSwapChain() } -> std::same_as<void>;
        { ops.Unhook() } -> std::same_as<UnhookResult>;
        { ops.HooksTransactable() } -> std::convertible_to<bool>;
        { ops.HooksAttached(target) } -> std::convertible_to<bool>;
        // Create, adopt, replay, test present.
        { ops.Create(target, std::declval<const typename Ops::Capture&>()) } -> std::same_as<typename Ops::CreateResult>;
        { ops.Adopt(handle, handle, target) } -> std::convertible_to<bool>;
        { ops.Replay(handle) } -> std::convertible_to<bool>;
        { ops.TestPresent(handle) } -> std::same_as<uint32_t>;
        // Teardown (ChainLifetime.h).
        { ops.SetWitness(handle) } -> std::convertible_to<bool>;
        { ops.ReleaseWitness() } -> std::same_as<void>;
        { ops.WitnessAlive() } -> std::convertible_to<bool>;
        { ops.Deactivate(handle) } -> std::same_as<void>;
        { ops.ClearChainRefs(live) } -> std::same_as<void>;
        { ops.HasSwapchainContext(handle) } -> std::convertible_to<bool>;
        { ops.ReleaseSwapchain(handle, handle) } -> std::convertible_to<bool>;
        { ops.ReleasePin(handle) } -> std::same_as<uint32_t>;
        { ops.ShutdownProvider(handle) } -> std::same_as<void>;
        { ops.RetireProvider(handle) } -> std::same_as<void>;
        { ops.ClearCurrentProvider() } -> std::same_as<void>;
        // Report.
        { ops.Report(event) } -> std::same_as<void>;
        { ops.Publish(status) } -> std::same_as<void>;
    };
} // namespace ext::fg::outswitch
