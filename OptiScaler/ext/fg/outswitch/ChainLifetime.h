#pragma once
// The lifetime of one live chain: what the switch owns, and the exact order it lets go of it.
//
// The order is the one the bridge itself uses when the game destroys it (provider release first, the chain reference
// last, with the FG chain pointer already cleared), reproduced here so the switch and the game's own teardown cannot
// disagree:
//
//   1  the bridge no longer points at the chain (the caller detached it; a forwarder sees the plain fallbacks)
//   2  clear the global FG chain pointer, and the plain-presenter registration for a plain chain
//   3  provider Deactivate()                       (frame generation off, no new work reaches the chain)
//   4  for a row measured with the caller's reference released first: THE MEASURED PIN RELEASE, and then the library's
//      own destroy, exactly once; otherwise the destroy first and the pin release after it (step 5 below)
//   5  provider Shutdown()                          (its contexts, command lists, fences)
//   6  the provider object is retired, never reused; the global provider pointer is cleared
//
// Steps 2 and 3 are in the bridge's own order (`with_dx12/dx11_with_dx12_sc.cpp`, Release: the pointer is cleared,
// then the provider is deactivated and its chain released). The switch clears the pointer of a chain that is no longer
// the live one, so the order matters only for the release hook's view of the world; keeping it identical to upstream's
// means the two teardowns cannot disagree about which pointer is stale.
//
// The reference count is touched exactly once, in the place the measured row names (LibraryModel.h):
//   destroyfirst  the library's destroy ends its own reference, and the caller's release follows it (M2), or the destroy
//                 ends the object and no release happens at all (M1);
//   pinfirst      the caller's reference is released before the destroy, and the object is expected to survive it
//                 because the provider's context holds one too. That is the order for a library whose context refuses
//                 to be destroyed while somebody else still holds the chain (XeFG): destroyed in the other order, the
//                 context is left allocated with nobody left to own it. After the release the witness must still be
//                 there, and the single destroy must end it: a destroy that leaves the object alive is a chain and a
//                 context the switch cannot free, so it stops instead of trying a second destroy (an access violation
//                 on an object the first call already freed).
//
// A plain chain has no library and no measured row: its release is the pin release and there is nothing to destroy.
// An unknown model never gets this far: preflight refuses it.
//
// Clearing the global FG chain pointer in step 2 before any release is what makes the release safe: while it still
// equalled the chain, the release hook treats a last-reference release specially (it either keeps the chain to honour
// a "preserve" configuration, or deletes the bridge from inside the present), and neither may happen during a switch.
// With the pointer cleared and, in the pin-first order, the provider's context still holding the chain, the release is
// never the last one either: the hook's two special branches stay out of the switch entirely.
//
// The witness is a diagnostic, never a cause: it can stop a release (fail closed), it can never force one.
// A row is only ever written with a witness the library accepted (LibraryModel.h: M2 needs it in every run), so a row
// that says witness=honoured is a promise that the interface can be placed on a chain of that class. If the live chain
// refuses it, the promise does not hold for this instance (a wrapper, another build beside it, an overlay) and the
// table alone would decide a release that can touch freed memory: the teardown refuses before it clears the pointer,
// deactivates or releases anything. A disagreement between the witness and the table is a model violation: the caller
// stops touching the chain and gives up for the session.
//
// The independent end-to-end proof that the window is free again is the next create on the same window: it either
// succeeds or answers E_ACCESSDENIED. That is checked by the sequence, not here.
//
// Ops contract used here (all no-ops for a plain chain where the provider is null):
//   bool SetWitness(void* chain)   void ReleaseWitness()   bool WitnessAlive()
//   void Deactivate(void* provider)
//   void ClearChainRefs(const LiveChain&)      // FG chain pointer, plain-presenter registration, bridge bookkeeping
//   bool HasSwapchainContext(void* provider)   bool ReleaseSwapchain(void* provider, void* hwnd)
//   uint32_t ReleasePin(void* chain)           // exactly one Release, returns the count the release left
//   void ShutdownProvider(void* provider)      void RetireProvider(void* provider)   void ClearCurrentProvider()
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_com_sim.cpp, fg_switch_sequence.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstdint>

namespace ext::fg::outswitch
{
// The one chain the switch owns. Handles are opaque here: the runtime casts them to its own types, the tests to fakes.
// `Empty()` means nothing is on the window; a chain is only ever put here by Adopt and only taken out by the teardown.
struct LiveChain
{
    void* chain = nullptr;
    void* provider = nullptr; // null for plain presentation
    Class cls = Class::Plain;
    void* hwnd = nullptr;

    bool Empty() const { return chain == nullptr; }
};

struct TearDownResult
{
    bool ok = true;
    Fail reason = Fail::None;
    Model model = Model::Unknown;
    bool pinReleased = false;
    uint32_t refsAfterPin = 0;
    // False while the chain is exactly as it was: every refusal above the first destructive statement leaves it that way,
    // and the caller may put it back on the window instead of leaving an intact chain unowned.
    bool touched = false;
    bool witnessAttempted = false;
    bool witnessAlive = false;
};

// Releases one chain and, on success, leaves `live` empty (the only place a live chain is cleared).
// On failure `live` keeps describing the chain that could not be freed: the caller must not touch it again.
template <class Ops> TearDownResult TearDownLive(LiveChain& live, Ops& ops, const LibraryModel& row)
{
    TearDownResult out;
    if (live.Empty())
        return out;

    // An M2 row without a witness the library accepted is not a row (LibraryModel.h: Usable). The teardown is the last
    // place that could release on the table's word alone - the direction that touches freed memory - so it refuses
    // before the FG chain pointer, the provider or the chain itself is touched.
    if (live.cls != Class::Plain && row.model == Model::M2 && !row.witnessHonoured)
    {
        out.ok = false;
        out.reason = Fail::ModelViolation;
        return out;
    }

    const bool plain = live.cls == Class::Plain;
    if (!plain && row.model == Model::Unknown)
    {
        // No measured model: the reference count must not be touched, and neither must the chain.
        out.ok = false;
        out.reason = Fail::NoModel;
        return out;
    }

    out.model = plain ? Model::Unknown : row.model;
    const bool releasePin = plain || row.model == Model::M2;
    const bool crossCheck = !plain && row.witnessHonoured;
    // The measured order of this row: the release of the caller's reference first, then the library's destroy. The row
    // says which one this build was measured with (LibraryModel.h), and a row is only used for its own order.
    const bool pinFirst = !plain && releasePin && row.pinFirst;

    // The row was measured with a witness the library accepted; without one on THIS chain the cross-check the row leans
    // on does not exist, and nothing may be touched. Refusing here is the difference between a refused switch and a
    // release the table alone decided.
    if (crossCheck && !ops.SetWitness(live.chain))
    {
        out.ok = false;
        out.reason = Fail::WitnessUnavailable;
        return out;
    }

    // The witness goes on before anything is destroyed: the library destroys the object, and the interface is released
    // with it.
    const bool witness = crossCheck;
    struct WitnessGuard
    {
        Ops& ops;
        bool active;
        ~WitnessGuard()
        {
            if (active)
                ops.ReleaseWitness();
        }
    } guard { ops, witness };
    out.witnessAttempted = witness;

    // The order the bridge itself uses when the game destroys it: the global FG chain pointer is cleared first, then the
    // provider is deactivated and its chain released (with_dx12/dx11_with_dx12_sc.cpp, Release).
    out.touched = true;
    ops.ClearChainRefs(live);

    if (!plain)
        ops.Deactivate(live.provider);

    if (pinFirst)
    {
        // The order this row was measured with: the reference the switch holds goes first, and the library's destroy runs
        // once, afterwards. With the caller's reference gone the object is held only by the provider's own context, which
        // is exactly the state the library's destroy expects: an XeFG context refuses to be destroyed while the caller
        // still holds the chain and is then left allocated with nobody to own it, which is what this order removes.
        out.pinReleased = true;
        out.refsAfterPin = ops.ReleasePin(live.chain);
        // The release goes first, so the witness answer must be read AFTER it: a row that describes a build whose context
        // holds no reference would have been freed by this release, and the destroy below would run on freed memory. A
        // dead witness here is a model violation and nothing else may be touched.
        out.witnessAlive = witness && ops.WitnessAlive();
        if (witness && !out.witnessAlive)
        {
            out.ok = false;
            out.reason = Fail::ModelViolation;
            return out;
        }

        // Exactly one destroy, never retried: a second call on a chain the library already destroyed is an access
        // violation, so whatever this one answers is final. The library's own answer is not visible through the provider
        // wrapper (it answers success whatever it did), which is why the witness, not the boolean, decides here.
        const bool released = ops.ReleaseSwapchain(live.provider, live.hwnd);
        (void) released;

        out.witnessAlive = witness && ops.WitnessAlive();
        if (witness && out.witnessAlive)
        {
            // The destroy did not free the object: the context it refused to destroy is still there. The chain is
            // unreachable from the bridge and must not be touched again (no second destroy, no second release).
            out.ok = false;
            out.reason = Fail::OldChainLeaked;
            return out;
        }
        if (!witness && !released)
        {
            // Without a witness the provider's own answer is all there is: a refusal means nothing was destroyed and the
            // chain cannot be named any more, so it is the caller that must stop.
            out.ok = false;
            out.reason = Fail::TeardownRefused;
            return out;
        }

        ops.ShutdownProvider(live.provider);
        ops.ClearCurrentProvider();
        ops.RetireProvider(live.provider);

        ops.ReleaseWitness();
        guard.active = false;
        live = {};
        return out;
    }

    const bool hadContext = !plain && ops.HasSwapchainContext(live.provider);
    const bool released = plain ? true : ops.ReleaseSwapchain(live.provider, live.hwnd);
    if (hadContext && !released)
    {
        // The provider's own guard refused (the window it was asked about is not the one it holds). Nothing was
        // destroyed, so nothing may be released either.
        out.ok = false;
        out.reason = Fail::TeardownRefused;
        out.witnessAlive = witness && ops.WitnessAlive();
        return out;
    }

    if (releasePin)
    {
        if (witness && !ops.WitnessAlive())
        {
            // The table says the object still exists after the destroy, the witness says it does not. Releasing the
            // pin would touch freed memory, so it does not happen.
            out.ok = false;
            out.reason = Fail::ModelViolation;
            return out;
        }
        out.pinReleased = true;
        out.refsAfterPin = ops.ReleasePin(live.chain);
    }

    if (witness)
    {
        out.witnessAlive = ops.WitnessAlive();
        if (row.model == Model::M1 && out.witnessAlive)
        {
            // The destroy was measured to end the object; the witness still being registered means it did not.
            out.ok = false;
            out.reason = Fail::ModelViolation;
            return out;
        }
        if (row.model == Model::M2 && out.witnessAlive)
        {
            // The library held a reference the table says it does not: the object outlived the pin.
            out.ok = false;
            out.reason = Fail::OldChainLeaked;
            return out;
        }
    }

    ops.ShutdownProvider(live.provider);
    ops.ClearCurrentProvider();
    ops.RetireProvider(live.provider);

    ops.ReleaseWitness();
    guard.active = false;
    live = {};
    return out;
}
} // namespace ext::fg::outswitch
