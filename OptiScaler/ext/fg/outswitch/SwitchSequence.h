#pragma once
// The live output switch: one synchronous sequence inside one bridge present, from the configuration to the new chain.
//
// Everything happens on the present thread between two presents of the game, so the game never sees a detach of its own
// window. The order (each step is one Event, none of them can be skipped):
//
//   0  preflight, read only      SwitchPolicy.h: Go, or a refusal that touches nothing. A hook transaction that another
//                                thread holds open refuses here too (HookBusy, transient): it is the one part of the detach
//                                that a teardown cannot be done after
//   1  barrier                   a try-lock that proves no native present of the old class is in flight; released at
//                                once, never held across a teardown (holding it would deadlock the XeFG present thread)
//                                a timeout is a transient refusal: nothing has been touched yet
//   2  quiesce                   the old provider: frame generation off, the frame-generation change flag set
//   3  drain                     the game queue and the copy queue, strictly idle, capped; not idle is a refusal and
//                                is NOT remembered (a busy GPU is not a property of the target)
//   3b capture                   descriptor, factory, window and queue of the live chain, plus a snapshot of the global
//                                state the libraries overwrite while they create their inner chain. Everything the
//                                creation needs is read HERE: after step 6 the old chain may be gone
//   4  interop release           overlay render target and the hud-less slots behind their fences
//   5  detach                    the bridge stops pointing at the old chain; other threads then see the plain fallbacks.
//                                This is the first destructive step, so the session budget is spent here: a switch that
//                                rolls back has retired a provider shell and pays for it like a commit does
//   6  teardown                  ChainLifetime.h with the OLD class's measured model
//   7  unhook                    one class hooked at a time: none until the new chain arrives. A body that did not
//                                leave in time (Busy) rolls the switch back to plain without remembering the target
//   8  create                    the new chain, through the guards, with the state snapshot restored after every
//                                attempt (a failed one included); a denied window is retried a few times
//   9  adopt                     the creation reference becomes the bridge's pin: no count arithmetic
//   10 replay                    colour space, HDR renegotiation, frame latency
//   11 test present              one DXGI_PRESENT_TEST on the new chain, through the hooked vtable
//   12 rollback to plain         the SAME routine with another target, using the step-3b capture; runs when 8-11 fail
//                                and when the new chain fails its judgement later (an adopted chain is torn down by its
//                                own provider, which is what an earlier design could not do)
//   13 commit                    generation, budget, cooldown, published status, probation starts
//
// And two paths that are not the sequence:
//   DetachedPresent()      a present that arrives while the bridge has no chain: S_OK inside the window, then the
//                          error the bridge always returned; it also rebuilds a plain chain while limping
//   OnFgPresentResult()    the present result of the live FG chain: probation and the watchdog (ProbationWatch.h)
//
// Ops contract: the shape of every call is frozen by SwitchOpsContract.h, and the rules a concept cannot express (the
// gate's ownership and the Detours order, the pass-through answer per hook body, the create guard, the state snapshot,
// the threading, the identity sources) are written THERE, once, and nowhere else. Nothing in this file repeats them.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_sequence.cpp, fg_switch_com_sim.cpp).
//
// Two invariants the code below keeps and the tests assert:
//   I-REL  no release of a chain while the global FG chain pointer still names it: the teardown clears the pointer
//          first, so the release is a plain pass-through and neither of the release hook's special branches can run;
//   I-CREATE  a chain is created only while the switch owns no chain, and the descriptor always comes from the capture
//          taken before the old chain was released (never from the old chain afterwards).

#include "ext/fg/outswitch/ChainLifetime.h"
#include "ext/fg/outswitch/ProbationWatch.h"
#include "ext/fg/outswitch/SwitchPolicy.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstddef>
#include <cstdint>

namespace ext::fg::outswitch
{
// The barrier waits at most this long for a native present of the old class to leave the resize lock.
inline constexpr uint32_t kBarrierTimeoutMs = 250;
// One create attempt plus these retries, spaced by kCreateRetryDelayMs, when the window is still bound.
inline constexpr uint32_t kCreateAttempts = 4;
inline constexpr uint32_t kCreateRetryDelayMs = 20;
// After a teardown failure the module keeps answering S_OK for this long and retries a plain chain every kLimpRetryMs.
inline constexpr uint32_t kLimpGiveUpMs = 3000;
inline constexpr uint32_t kLimpRetryMs = 250;
// A hard refusal is looked at again after this long, not on every present: the preflight reads the chain on the window
// (its descriptors, a reference count through the release hook) and asks the loaders and the model table, and a refusal
// that cannot change by itself must not pay that per frame. A changed configuration is still seen within this time.
inline constexpr uint32_t kHardRecheckMs = 1000;

struct DrainResult
{
    bool game = true;
    bool fg = true;
    uint32_t ms = 0;
};

struct SwitchState
{
    Policy policy;
    Probation probation;
    LiveChain live;
    LibraryModel models[3]; // indexed by Class: the measured row per class, filled from preflight
    Target active = Target::Plain;
    Target target = Target::Plain;
    bool managed = false; // the bridge has no chain because this module took it away (a present must not be an error)
    bool limp = false;
    bool limpRebuild = false; // a plain chain may still be rebuilt while limping
    uint64_t limpUntilMs = 0;
    uint64_t limpRetryAtMs = 0;
    bool dead = false;
    Fail deadReason = Fail::None;
    Fail limpReason = Fail::None;
    bool running = false; // a sequence is inside this state: a nested present must not start a second one
    Fail rolledBackReason = Fail::None; // why the last rollback happened: the status line after it says so
    Fail refusedReason = Fail::None;    // why the last refusal happened, for the transient refusals nothing remembers
    uint64_t hardHoldUntilMs = 0;       // after a hard refusal no preflight runs before this (kHardRecheckMs)
    State published = State::Idle;
    Status lastPublished; // the last value handed to Ops::Publish; an unchanged status is not published again
    bool publishedValid = false;
    uint32_t hitchMs = 0;

    LibraryModel ModelOf(Class cls) const { return models[static_cast<size_t>(cls)]; }
};

struct SwitchResult
{
    State state = State::Idle;
    Fail reason = Fail::None;
    Target target = Target::Plain;
    Target active = Target::Plain;
};

inline Status Snapshot(const SwitchState& state)
{
    Status status;
    status.state = state.dead ? State::Dead : state.limp ? State::Limp : state.published;
    const Target shown = state.target;
    if (status.state == State::Dead)
        status.reason = state.deadReason;
    else if (status.state == State::Limp)
        status.reason = state.limpReason;
    else if (status.state == State::RolledBack)
        status.reason = state.rolledBackReason != Fail::None ? state.rolledBackReason : state.policy.RememberedReason(shown);
    else if (status.state == State::Refused)
        status.reason = state.policy.RememberedReason(shown) != Fail::None ? state.policy.RememberedReason(shown)
                                                                          : state.refusedReason;
    status.target = state.target;
    status.active = state.active;
    status.generation = state.policy.Generation();
    status.lastHitchMs = state.hitchMs;
    status.switchesLeft = state.policy.SwitchesLeft();
    status.probationPresents = state.probation.Presents();
    return status;
}

// The menu's "try again": forget one target's failure. Nothing else is reset; the budget is not refunded.
inline void TryAgain(SwitchState& state, Target target)
{
    state.policy.ClearMemory(target);
    state.hardHoldUntilMs = 0; // the player asked: the next present looks again at once
}

namespace detail
{
inline Event MakeEvent(Step step, Target from, Target to)
{
    Event event;
    event.step = step;
    event.from = from;
    event.to = to;
    return event;
}

inline void Publish(SwitchState& state, State published) { state.published = published; }

// The menu thread and status.json see a status only when it changed: a refusal that repeats every present must not write
// a line and a status file a hundred times a second.
template <class Ops> void PublishIfChanged(SwitchState& state, Ops& ops)
{
    const Status status = Snapshot(state);
    if (state.publishedValid && SameStatus(state.lastPublished, status))
        return;
    state.lastPublished = status;
    state.publishedValid = true;
    ops.Publish(status);
}

template <class Ops> SwitchResult Refused(SwitchState& state, Ops& ops, Target target, Fail reason, Severity severity,
                                          bool log, uint32_t result, bool policyChecked = false)
{
    // One line per distinct refusal, whichever origin it has: a preflight refusal was deduplicated by the policy already
    // (policyChecked), an in-sequence one (barrier, drain, capture) is deduplicated here.
    const bool line = log && (policyChecked || state.policy.ShouldLog(target, reason));
    if (line)
    {
        Event event = MakeEvent(Step::Refused, state.active, target);
        event.fail = reason;
        event.severity = severity;
        event.b = result;
        event.flagA = log;
        ops.Report(event);
    }

    // A transient refusal raised here pauses the whole feature for a moment: nothing was touched on this path, and
    // re-running the attempt on the next present would log, publish and pay the barrier or the drain cost every frame.
    // A preflight refusal (policyChecked) already set its pause, and a preflight pause is the plain retry with no
    // doubling and no give-up: pausing again here would count one refusal twice, which gave up on the fifth instead of
    // the tenth and stored a state condition the player could have changed as a property of the target.
    if (!policyChecked && severity == Severity::Transient && reason != Fail::Cooling)
        state.policy.PauseTransient(ops.NowMs(), target, reason);
    // A hard refusal is not asked again on the next present (kHardRecheckMs).
    if (severity == Severity::Hard)
        state.hardHoldUntilMs = ops.NowMs() + kHardRecheckMs;

    // The per-target memory keeps the failure that caused it: "this target failed earlier" must never replace the
    // stored cause, and a spent budget is not a failure of the target at all.
    state.refusedReason = reason;
    const bool remember = severity == Severity::Hard && reason != Fail::Remembered && reason != Fail::Budget && log;
    if (remember)
        state.policy.Remember(target, reason);

    Publish(state, State::Refused);
    PublishIfChanged(state, ops);
    return { State::Refused, reason, target, state.active };
}

template <class Ops> SwitchResult Limped(SwitchState& state, Ops& ops, Fail reason, bool rebuild, Target target,
                                         uint32_t result)
{
    const uint64_t now = ops.NowMs();
    state.limp = true;
    state.limpRebuild = rebuild;
    state.limpReason = reason;
    state.limpUntilMs = now + kLimpGiveUpMs;
    state.limpRetryAtMs = now + kLimpRetryMs;
    state.dead = false;
    state.deadReason = reason;
    state.managed = true;

    Publish(state, State::Limp);
    PublishIfChanged(state, ops);

    Event event = MakeEvent(Step::Limp, state.active, target);
    event.fail = reason;
    event.flagA = rebuild;
    event.b = result;
    ops.Report(event);
    return { State::Limp, reason, target, state.active };
}

template <class Ops> void Died(SwitchState& state, Ops& ops, Fail reason)
{
    state.dead = true;
    state.limp = false;
    state.deadReason = reason;
    Publish(state, State::Dead);
    PublishIfChanged(state, ops);

    Event event = MakeEvent(Step::Dead, state.active, Target::Plain);
    event.fail = reason;
    ops.Report(event);
}

// Disposes of what a failed create left behind: a chain nobody adopted is torn down by its provider (its own model
// row), a provider object without a chain is shut down and retired. Never reused.
template <class Ops>
void DisposeCreated(SwitchState& state, Ops& ops, Target target, void* chain, void* provider, void* hwnd)
{
    if (chain != nullptr)
    {
        LiveChain orphan { chain, provider, ClassOf(target), hwnd };
        TearDownLive(orphan, ops, state.ModelOf(ClassOf(target)));
        return;
    }

    if (provider != nullptr)
    {
        ops.ShutdownProvider(provider);
        ops.ClearCurrentProvider();
        ops.RetireProvider(provider);
    }
}

struct BringUpResult
{
    bool ok = false;
    Fail fail = Fail::None;
    uint32_t result = 0;
    uint32_t retries = 0;
};

// Steps 8-11 for one target: create (bounded retries, state restored after every attempt), hook check, adopt, replay,
// test present. On success the chain is the bridge's and state.live names it; on failure whatever was created has
// already been disposed of, except after a successful adopt, where the caller's rollback tears it down.
template <class Ops>
BringUpResult BringUp(SwitchState& state, Ops& ops, Target target, typename Ops::Capture& capture,
                      const typename Ops::StateSnapshot& snapshot)
{
    BringUpResult out;
    const Class cls = ClassOf(target);

    typename Ops::CreateResult created {};
    for (uint32_t attempt = 0; attempt < kCreateAttempts; ++attempt)
    {
        const uint64_t attemptStart = ops.NowMs();
        created = ops.Create(target, capture);
        // The library creates its inner chain through the hooked factory, which overwrites the global state: put it
        // back before anything else runs, on a failed attempt too.
        ops.RestoreState(snapshot);

        Event event = MakeEvent(Step::Create, state.active, target);
        event.a = attempt;
        event.b = created.hr;
        event.flagA = created.chain != nullptr;
        event.chain = created.chain;
        event.ms = static_cast<int64_t>(ops.NowMs() - attemptStart);
        ops.Report(event);

        if (created.chain != nullptr || !created.denied)
            break;
        if (attempt + 1 < kCreateAttempts)
        {
            out.retries = attempt + 1;
            ops.SleepMs(kCreateRetryDelayMs);
        }
    }

    if (created.chain == nullptr)
    {
        // The window is still bound to a chain we could not free: no create can work, and the bridge stays detached.
        out.fail = created.denied ? Fail::CreateDenied : Fail::Create;
        out.result = created.hr;
        DisposeCreated(state, ops, target, nullptr, created.provider, capture.hwnd);
        return out;
    }

    if (!ops.HooksAttached(target))
    {
        out.fail = Fail::Rehook;
        DisposeCreated(state, ops, target, created.chain, created.provider, capture.hwnd);
        return out;
    }

    if (!ops.Adopt(created.chain, created.provider, target))
    {
        out.fail = Fail::Adopt;
        DisposeCreated(state, ops, target, created.chain, created.provider, capture.hwnd);
        return out;
    }

    state.live = LiveChain { created.chain, created.provider, cls, capture.hwnd };
    state.managed = false;

    if (!ops.Replay(created.chain))
    {
        out.fail = Fail::Replay;
        return out; // state.live keeps the chain: the rollback tears it down
    }

    // Reported after the replay so the one adopt line carries what the replay put back (the colour space and whether the
    // fork's own HDR output owns the chain), which is the state the chain is committed with.
    Event adopt = MakeEvent(Step::Adopt, state.active, target);
    adopt.cls = cls;
    adopt.flagA = true; // this line is only reported after the adopt ran
    ops.Report(adopt);

    const uint32_t result = ops.TestPresent(created.chain);
    Event test = MakeEvent(Step::TestPresent, state.active, target);
    test.b = result;
    ops.Report(test);
    if (!PresentAccepted(result))
    {
        out.fail = Fail::TestPresent;
        out.result = result;
        return out;
    }

    out.ok = true;
    return out;
}

// The one rollback routine: remember the target, detach, tear down whatever is live (the old chain mid-switch, or the
// adopted dud after a failed judgement), unhook, and bring plain presentation up from the capture taken before
// anything was released. A failure of the plain chain is the limp, never a second chain on the window.
template <class Ops>
SwitchResult Rollback(SwitchState& state, Ops& ops, typename Ops::Capture& capture,
                      const typename Ops::StateSnapshot& snapshot, Target failed, Fail reason, uint32_t result,
                      bool unhookAgain = true)
{
    // A transient cause (a hook body that did not leave in time) is retried after the same pause a transient refusal
    // takes; Remember() ignores transient causes, so the target is not banned for the session.
    if (SeverityOf(reason) == Severity::Transient)
        state.policy.PauseTransient(ops.NowMs(), failed, reason);

    state.policy.Remember(failed, reason);
    state.rolledBackReason = reason;

    Event failure = MakeEvent(Step::Rollback, state.active, Target::Plain);
    failure.fail = reason;
    failure.to = failed; // the target that is remembered, not the output that was on the window
    failure.b = result;
    ops.Report(failure);

    // Steps 1-3 of the forward path run here too, best effort. A chain that failed its judgement, or a step that failed
    // after the detach, is torn down while the XeFG present worker or queued GPU work may still be running: the same two
    // hazards the forward path excludes before it releases anything. Nothing here refuses - a barrier that times out or a
    // queue that does not prove idle must not keep a failing chain on the window any longer than one present, and the
    // teardown that follows already accepts that state (it is the state a step-6 failure leaves behind).
    if (!state.live.Empty())
    {
        if (state.live.cls == Class::XeFg)
        {
            const uint64_t barrierStart = ops.NowMs();
            const bool ok = ops.Barrier();
            Event event = MakeEvent(Step::Barrier, state.active, Target::Plain);
            event.flagA = ok;
            event.ms = static_cast<int64_t>(ops.NowMs() - barrierStart);
            ops.Report(event);
        }
        if (state.live.provider != nullptr)
        {
            ops.Quiesce(state.live.provider);
            ops.Report(MakeEvent(Step::Quiesce, state.active, Target::Plain));
        }
        const uint64_t drainStart = ops.NowMs();
        const DrainResult drain = ops.Drain(state.live.provider);
        Event event = MakeEvent(Step::Drain, state.active, Target::Plain);
        event.flagA = drain.game;
        event.flagB = drain.fg;
        event.ms = drain.ms != 0 ? drain.ms : static_cast<int64_t>(ops.NowMs() - drainStart);
        ops.Report(event);
    }

    ops.Detach();
    state.managed = true;

    if (!state.live.Empty())
    {
        const Class dudClass = state.live.cls;
        void* dudChain = state.live.chain;
        const uint64_t teardownStart = ops.NowMs();
        TearDownResult torn = TearDownLive(state.live, ops, state.ModelOf(dudClass));
        Event teardown = MakeEvent(Step::Teardown, state.active, Target::Plain);
        teardown.cls = dudClass;
        teardown.model = torn.model;
        teardown.flagA = torn.pinReleased;
        teardown.flagB = torn.witnessAlive;
        teardown.a = torn.refsAfterPin;
        teardown.c = torn.witnessAttempted ? 1u : 0u; // the line says witness=na when none was placed
        teardown.chain = dudChain;
        teardown.ms = static_cast<int64_t>(ops.NowMs() - teardownStart);
        ops.Report(teardown);
        if (!torn.ok)
            return Limped(state, ops, torn.reason, false, failed, result);
    }

    // A detach that just answered Busy or Failed with nothing newly attached is not retried here: the second call would
    // pay another full drain timeout for an answer the first one already gave, and the binding reopens the gate either
    // way (SwitchOpsContract.h).
    if (unhookAgain)
    {
        const UnhookResult unhooked = ops.Unhook();
        Event hooks = MakeEvent(Step::Unhook, state.active, Target::Plain);
        hooks.flagA = unhooked == UnhookResult::Ok;
        hooks.flagB = unhooked == UnhookResult::Busy;
        ops.Report(hooks);
    }

    BringUpResult plain = BringUp(state, ops, Target::Plain, capture, snapshot);
    if (plain.fail == Fail::CreateDenied)
        return Limped(state, ops, Fail::OldChainLeaked, false, failed, plain.result);
    if (!plain.ok)
        return Limped(state, ops, Fail::PlainCreate, true, failed, plain.result);

    state.active = Target::Plain;
    state.managed = false;
    state.probation.End();
    Publish(state, State::RolledBack);
    PublishIfChanged(state, ops);
    return { State::RolledBack, reason, Target::Plain, Target::Plain };
}
} // namespace detail

// The bridge this state describes. A game path that destroys and recreates its bridge (a display-mode change, a device
// reset) leaves every chain pointer this state cached pointing into the old bridge, so a state that names another bridge
// is dropped rather than re-validated: nothing about the old window describes the new one. The per-target memory stays,
// because a library that failed in this session fails in a new window too.
// False = the state already described this bridge.
inline bool OwnedBy(SwitchState& state, void*& owner, void* bridge)
{
    if (owner == bridge)
        return false;
    owner = bridge;
    const Policy policy = state.policy;
    state = SwitchState {};
    state.policy = policy;
    return true;
}

// The switch point: one call per present, before anything else in the bridge present. With the key off this is one
// relaxed load and one comparison.
template <class Ops> SwitchResult RunSwitch(SwitchState& state, Ops& ops, uint64_t presentStartMs)
{
    if (state.dead)
        return { State::Dead, state.deadReason, state.target, state.active };

    // The bridge has no chain because this module took it away (a switch running on this thread's own present, or a limp
    // whose plain chain is being rebuilt). No sequence can start from that state: there is no chain to detach and no
    // capture to take. Defence only - the step order already guarantees the switch thread's own hooked calls (the pin
    // release, the test present) run before the detach or after the adopt, so they always find the gate open.
    if (state.managed)
        return { State::Idle, Fail::None, state.target, state.active };

    // A nested present (the FG library's factory re-entry, a second present on the same thread) must not run a second
    // sequence on the same state: everything below is single-threaded, and re-entering it would tear down the chain the
    // outer call is creating.
    if (state.running)
        return { State::Idle, Fail::None, state.target, state.active };
    state.running = true;
    struct RunningGuard
    {
        bool& running;
        ~RunningGuard() { running = false; }
    } runningGuard { state.running };

    // The judgement of the chain that is on the window comes first, before the key and before the configured output:
    // after a commit the configured output IS the active one, so a binding that answers "is another output wanted"
    // answers no, and a chain that fails its presents would never be put back to plain.
    if (state.probation.RollbackDue())
    {
        const Fail why = state.probation.LastFail();
        const uint32_t result = state.probation.LastResult();
        const Target failed = state.active;
        state.probation.ClearRollback();
        state.probation.End();

        typename Ops::Capture capture {};
        if (!ops.ReadBridgeCapture(capture))
        {
            ops.Detach();
            state.managed = true;
            if (!state.live.Empty())
            {
                TearDownResult torn = TearDownLive(state.live, ops, state.ModelOf(state.live.cls));
                if (!torn.ok)
                    return detail::Limped(state, ops, torn.reason, false, failed, result);
            }
            return detail::Limped(state, ops, Fail::NoCapture, true, failed, result);
        }
        struct CaptureGuard
        {
            Ops& ops;
            typename Ops::Capture& capture;
            ~CaptureGuard() { ops.ReleaseCapture(capture); }
        } guard { ops, capture };

        const typename Ops::StateSnapshot snapshot = ops.Snapshot();
        return detail::Rollback(state, ops, capture, snapshot, failed, why, result);
    }

    // The idle cost of the whole feature: one relaxed load of the key, one compare. Nothing below runs with the key off.
    if (!ops.Wanted())
        return { State::Idle, Fail::KeyOff, state.target, state.active };

    // A hard refusal a moment ago: nothing is read and nothing is evaluated until the hold ends (or "try again").
    if (state.hardHoldUntilMs != 0 && ops.NowMs() < state.hardHoldUntilMs)
        return { State::Refused, state.refusedReason, state.target, state.active };

    const PreflightInputs inputs = ops.ReadPreflight();
    state.target = inputs.target;
    state.active = inputs.active;
    state.models[static_cast<size_t>(ClassOf(inputs.active))] = inputs.active == Target::Plain ? LibraryModel {} : inputs.activeModel;
    state.models[static_cast<size_t>(ClassOf(inputs.target))] = inputs.target == Target::Plain ? LibraryModel {} : inputs.targetModel;

    const Verdict verdict = state.policy.Evaluate(inputs, ops.NowMs());
    if (verdict.kind == VerdictKind::Idle)
        return { State::Idle, verdict.reason, inputs.target, inputs.active };
    if (verdict.kind == VerdictKind::Refuse)
        return detail::Refused(state, ops, inputs.target, verdict.reason, verdict.severity, verdict.log, 0, true);

    // The module did not create the chain that is on the window when it starts (the boot chain is the bridge's own).
    // Read it before anything can be detached: a switch that cannot name the live chain would leave the old chain bound
    // to the window and answer the game with a device removal on the first present.
    //
    // The read runs on every attempt, not once per session: this state is process-wide and the chain it names can be
    // replaced under it (the game recreating its bridge, a provider rebuilding its inner chain). A stale name would be
    // dereferenced by the capture and by the teardown on freed memory. Reading it again is idempotent after a switch,
    // because the adopt wrote the same chain into the bridge and into this state.
    if (!state.managed)
    {
        LiveChain live;
        if (!ops.ReadLive(live) || live.Empty())
            return detail::Refused(state, ops, inputs.target, Fail::NoCapture, Severity::Hard, true, 0);
        state.live = live;
    }
    // Never detach a chain the module cannot name: the same refusal, and nothing has been touched.
    if (state.live.Empty())
        return detail::Refused(state, ops, inputs.target, Fail::NoCapture, Severity::Hard, true, 0);

    const Target target = inputs.target;
    const Target from = inputs.active;
    const uint64_t startMs = presentStartMs != 0 ? presentStartMs : ops.NowMs();

    Event begin = detail::MakeEvent(Step::Begin, from, target);
    begin.generation = state.policy.Generation() + 1;
    ops.Report(begin);

    Event preflight = detail::MakeEvent(Step::Preflight, from, target);
    preflight.a = state.policy.SwitchesLeft();
    ops.Report(preflight);

    // The one part of the detach that cannot be retried for long: another thread's pending Detours transaction. Asking
    // here, before the barrier and before anything is detached, turns it into a refusal that costs nothing instead of a
    // teardown followed by a rollback, and never into a ban of the target (the detach retries an enlist it lost itself).
    if (!ops.HooksTransactable())
        return detail::Refused(state, ops, target, Fail::HookBusy, Severity::Transient, true, 0);

    // Step 1: a barrier, not a lock. Held across the teardown it would deadlock the provider's own present thread.
    const bool barrier = target == Target::XeFg || from == Target::XeFg;
    if (barrier)
    {
        const uint64_t barrierStart = ops.NowMs();
        const bool ok = ops.Barrier();
        Event event = detail::MakeEvent(Step::Barrier, from, target);
        event.flagA = ok;
        event.ms = static_cast<int64_t>(ops.NowMs() - barrierStart);
        ops.Report(event);
        if (!ok)
            return detail::Refused(state, ops, target, Fail::Barrier, Severity::Transient, true, 0);
    }

    // Step 2: quiesce the old provider.
    if (state.live.provider != nullptr)
    {
        ops.Quiesce(state.live.provider);
        ops.Report(detail::MakeEvent(Step::Quiesce, from, target));
    }

    // Step 3: drain both queues. Not idle is transient and is not remembered.
    {
        const uint64_t drainStart = ops.NowMs();
        const DrainResult drain = ops.Drain(state.live.provider);
        Event event = detail::MakeEvent(Step::Drain, from, target);
        event.flagA = drain.game;
        event.flagB = drain.fg;
        event.ms = drain.ms != 0 ? drain.ms : static_cast<int64_t>(ops.NowMs() - drainStart);
        ops.Report(event);
        if (!drain.game || !drain.fg)
        {
            if (state.live.provider != nullptr)
                ops.Resume(state.live.provider);
            return detail::Refused(state, ops, target, Fail::Drain, Severity::Transient, true, 0);
        }
    }

    // Step 3b: capture everything the creation needs, and the global state, while the old chain is still alive.
    typename Ops::Capture capture {};
    if (!ops.ReadCapture(state.live.chain, capture))
    {
        if (state.live.provider != nullptr)
            ops.Resume(state.live.provider);
        return detail::Refused(state, ops, target, Fail::NoCapture, Severity::Hard, true, 0);
    }
    struct CaptureGuard
    {
        Ops& ops;
        typename Ops::Capture& capture;
        ~CaptureGuard() { ops.ReleaseCapture(capture); }
    } guard { ops, capture };

    const typename Ops::StateSnapshot snapshot = ops.Snapshot();
    ops.Report(detail::MakeEvent(Step::Capture, from, target));

    // Step 4: the interop resources of the old chain.
    ops.ReleaseInterop(capture.hwnd);

    // Step 5: detach. From here a present on another thread sees the plain fallbacks, never a dangling pointer. This is
    // the first destructive step, so the session budget is spent here (a rollback retires a shell too), and a stale
    // release-hook pointer is cleared before any chain is created at a recycled address.
    //
    // Everything the attempt owns is written HERE and not at its start: the refusals above this line (barrier, drain,
    // capture) all return before the interop is touched, so the chain on the window is untouched by them: they must not
    // end its judgement, must not forget the previous reason and must not publish a switch that never started.
    state.probation.End();
    state.rolledBackReason = Fail::None;
    state.refusedReason = Fail::None;
    detail::Publish(state, State::Switching);
    detail::PublishIfChanged(state, ops);
    state.policy.Spend();
    ops.ClearOldSwapChain();
    ops.Detach();
    state.managed = true;
    ops.Report(detail::MakeEvent(Step::Detach, from, target));

    // Step 6: tear the old chain down, with the OLD class's measured model.
    if (!state.live.Empty())
    {
        const Class oldClass = state.live.cls;
        const LibraryModel row = state.ModelOf(oldClass);
        void* oldChain = state.live.chain;
        const uint64_t teardownStart = ops.NowMs();
        TearDownResult torn = TearDownLive(state.live, ops, row);

        Event teardown = detail::MakeEvent(Step::Teardown, from, target);
        teardown.cls = oldClass;
        teardown.model = row.model;
        teardown.flagA = torn.pinReleased;
        teardown.flagB = torn.witnessAlive;
        teardown.a = torn.refsAfterPin;
        teardown.c = torn.witnessAttempted ? 1u : 0u; // the line says witness=na when none was placed
        teardown.chain = oldChain;
        teardown.ms = static_cast<int64_t>(ops.NowMs() - teardownStart);
        ops.Report(teardown);

        if (!torn.ok)
        {
            // A teardown that refused before it touched anything (an unknown model, a witness the chain would not take)
            // has not damaged the chain: it is still the one on the window, and the game keeps presenting through it.
            // Re-attach it and resume the provider; the refusal is the whole cost of the attempt.
            if (!torn.touched && ops.Adopt(state.live.chain, state.live.provider, from))
            {
                (void) ops.Replay(state.live.chain);
                if (state.live.provider != nullptr)
                    ops.Resume(state.live.provider);
                state.managed = false;
                return detail::Refused(state, ops, target, torn.reason, SeverityOf(torn.reason), true, 0);
            }
            return detail::Limped(state, ops, torn.reason, false, target, 0);
        }
    }

    // Step 7: one class hooked at a time. A detach that cannot drain leaves the old class patched as dead code: the new
    // class cannot be hooked, so the switch goes back to plain. A body that did not leave in time is a transient cause
    // (the target is not remembered, and the next attempt may succeed); an inconsistent hook set is not.
    {
        const UnhookResult unhooked = ops.Unhook();
        Event event = detail::MakeEvent(Step::Unhook, from, target);
        event.flagA = unhooked == UnhookResult::Ok;
        event.flagB = unhooked == UnhookResult::Busy;
        ops.Report(event);
        if (unhooked == UnhookResult::Busy)
            return detail::Rollback(state, ops, capture, snapshot, target, Fail::HookBusy, 0, false);
        if (unhooked == UnhookResult::Failed)
            return detail::Rollback(state, ops, capture, snapshot, from, Fail::HookFailed, 0, false);
    }

    // Steps 8-11.
    detail::BringUpResult up = detail::BringUp(state, ops, target, capture, snapshot);
    if (up.fail == Fail::CreateDenied)
        return detail::Limped(state, ops, Fail::OldChainLeaked, false, target, up.result);
    if (!up.ok)
        return detail::Rollback(state, ops, capture, snapshot, target, up.fail, up.result);

    // Step 13: commit.
    const uint64_t now = ops.NowMs();
    state.policy.Committed(now); // the budget was spent at the detach
    state.active = target;
    state.managed = false;
    state.hitchMs = static_cast<uint32_t>(now >= startMs ? now - startMs : 0);
    state.probation.Begin(now);
    detail::Publish(state, State::Probation);
    detail::PublishIfChanged(state, ops);

    Event commit = detail::MakeEvent(Step::Commit, from, target);
    commit.generation = state.policy.Generation();
    commit.ms = static_cast<int64_t>(state.hitchMs);
    commit.a = state.policy.SwitchesLeft();
    ops.Report(commit);

    return { State::Probation, Fail::None, target, target };
}

// A present that arrives while the bridge has no chain. With nothing torn down by this module the answer is exactly
// what the bridge always answered. While limping the game keeps getting S_OK for a bounded window, during which a
// plain chain is retried every kLimpRetryMs; when the window ends the module gives up and the old error comes back.
template <class Ops> uint32_t DetachedPresent(SwitchState& state, Ops& ops, bool realChainMissing)
{
    if (!state.managed)
        return kDeviceRemoved;
    if (state.dead)
        return kDeviceRemoved;
    if (realChainMissing)
    {
        detail::Died(state, ops, state.limpReason != Fail::None ? state.limpReason : Fail::PlainCreate);
        return kDeviceRemoved;
    }

    const uint64_t now = ops.NowMs();
    if (!state.limp)
        return kOk; // the switch is inside this thread's own present: the game must not see the gap

    if (now >= state.limpUntilMs)
    {
        detail::Died(state, ops, state.limpReason);
        return kDeviceRemoved;
    }
    if (!state.limpRebuild || now < state.limpRetryAtMs)
        return kOk;
    state.limpRetryAtMs = now + kLimpRetryMs;

    typename Ops::Capture capture {};
    if (!ops.ReadBridgeCapture(capture))
        return kOk;
    struct CaptureGuard
    {
        Ops& ops;
        typename Ops::Capture& capture;
        ~CaptureGuard() { ops.ReleaseCapture(capture); }
    } guard { ops, capture };

    const typename Ops::StateSnapshot snapshot = ops.Snapshot();
    detail::BringUpResult up = detail::BringUp(state, ops, Target::Plain, capture, snapshot);
    if (!up.ok)
        return kOk;

    state.limp = false;
    state.managed = false;
    state.active = Target::Plain;
    detail::Publish(state, State::RolledBack);
    detail::PublishIfChanged(state, ops);

    Event event = detail::MakeEvent(Step::Detached, Target::Plain, Target::Plain);
    event.flagA = true;
    ops.Report(event);
    return kOk;
}

// The present result of the live FG chain, before it reaches the game. Probation and the watchdog live in
// ProbationWatch.h; a lost device is never hidden from the game and never rolled back.
template <class Ops>
uint32_t OnFgPresentResult(SwitchState& state, Ops& ops, uint32_t result, uint32_t presentMs)
{
    const uint64_t now = ops.NowMs();
    const Probation::Note note = state.probation.Present(result, presentMs, now);

    if (note.health == Health::DeviceLost)
    {
        Event event = detail::MakeEvent(Step::Probation, state.active, state.target);
        event.fail = Fail::DeviceLost;
        event.b = result;
        ops.Report(event);
        detail::Died(state, ops, Fail::DeviceLost);
        return result;
    }

    if (note.health == Health::Failed)
    {
        Event event = detail::MakeEvent(Step::Probation, state.active, state.target);
        event.fail = state.probation.LastFail();
        event.b = result;
        event.ms = presentMs;
        event.flagA = note.mask;
        ops.Report(event);
        return note.mask ? kOk : result;
    }

    if (note.health == Health::Healthy && state.probation.TakeJudged())
    {
        Event event = detail::MakeEvent(Step::Probation, state.active, state.target);
        event.a = state.probation.Healthy();
        event.ms = now >= state.probation.StartedMs() ? static_cast<int64_t>(now - state.probation.StartedMs()) : 0;
        event.flagA = true;
        ops.Report(event);
        detail::Publish(state, State::Idle);
        detail::PublishIfChanged(state, ops);
    }

    return note.mask ? kOk : result;
}
} // namespace ext::fg::outswitch
