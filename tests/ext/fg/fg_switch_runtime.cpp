// The runtime helpers around the pure core: the status hand-off (StatusBox.h), the one-bridge registry
// (BridgeRegistry.h) and the detach gate of the hook rebind (HookLifecycle.h, HookScope.h). All three are plain atomics
// with a rule the switch depends on, so each rule gets a check here.
#include "ext/fg/outswitch/BridgeRegistry.h"
#include "ext/fg/outswitch/HookLifecycle.h"
#include "ext/fg/outswitch/HookScope.h"
#include "ext/fg/outswitch/StatusBox.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace ext::fg::outswitch;

namespace
{
uint64_t NowMsReal()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
void SleepMsReal(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
void StatusRoundTrip()
{
    std::printf("  the status hand-off\n");
    StatusBox box;
    Status empty = box.Read();
    CHECK(empty.state == State::Idle);
    CHECK(empty.reason == Fail::None);
    CHECK(box.StateNow() == State::Idle);

    Status status;
    status.state = State::RolledBack;
    status.reason = Fail::Probation;
    status.target = Target::XeFg;
    status.active = Target::Plain;
    status.generation = 3;
    status.lastHitchMs = 812;
    status.switchesLeft = 5;
    status.probationPresents = 12;
    box.Publish(status);
    const Status snapshot = box.Read();
    CHECK(snapshot.state == State::RolledBack);
    CHECK(snapshot.reason == Fail::Probation);
    CHECK(snapshot.target == Target::XeFg);
    CHECK(snapshot.active == Target::Plain);
    CHECK(snapshot.generation == 3);
    CHECK(snapshot.lastHitchMs == 812);
    CHECK(snapshot.switchesLeft == 5);
    CHECK(snapshot.probationPresents == 12);
    std::printf("    [ok] every field survives the round trip\n");

    // A writer on another thread publishes a generation with its state; a reader must never see a state that belongs to
    // a later generation with an earlier number, because the writer stores the fields first and the state last.
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 200000; ++i)
        {
            Status s;
            s.state = State::Probation;
            s.generation = i;
            s.switchesLeft = 1000;
            s.probationPresents = i;
            box.Publish(s);
        }
        stop.store(true, std::memory_order_release);
    });
    int torn = 0;
    while (!stop.load(std::memory_order_acquire))
    {
        const Status s = box.Read();
        if (s.state == State::Probation && (s.generation == 0 || s.switchesLeft != 1000))
            ++torn; // a state without its fields
    }
    writer.join();
    CHECK(torn == 0);
    std::printf("    [ok] a reader never sees the new state with the previous fields (%d torn reads)\n", torn);
}

void BridgeRegistryTest()
{
    std::printf("  the one-bridge registry\n");
    BridgeRegistry registry;
    CHECK(registry.Bridge() == nullptr);
    CHECK(!registry.SecondBridge());

    int first = 1;
    int second = 2;
    registry.NoteConfigured(&first);
    CHECK(registry.Bridge() == &first);
    CHECK(!registry.SecondBridge());

    // The same bridge configured again is not a second one.
    registry.NoteConfigured(&first);
    CHECK(!registry.SecondBridge());

    // A different bridge while one is registered: refused for the session.
    registry.NoteConfigured(&second);
    CHECK(registry.SecondBridge());
    CHECK(registry.Bridge() == &first); // the recorded bridge never changes under the switch

    // The first bridge goes away: the refusal stays latched for the session, and the newest bridge is still found, so
    // the read-only probe does not report "the bridge was not recorded" while a bridge exists.
    registry.NoteDestroyed(&first);
    CHECK(registry.Bridge() == &second);
    CHECK(registry.SecondBridge());
    registry.NoteDestroyed(&first); // a second destroy of the same pointer is harmless
    CHECK(registry.Bridge() == &second);
    registry.NoteDestroyed(&second);
    CHECK(registry.Bridge() == nullptr);
    CHECK(registry.SecondBridge()); // the refusal is for the session, even after both are gone
    std::printf("    [ok] a second bridge latches the refusal, the newest is found, and the pointer clears on destroy\n");
}

// The gate has to close BEFORE the count is read. A body that reaches Enter() after the detach started is told to pass
// through (its slot's original, or the fixed answer once the originals are gone) and is not counted, so a detach that
// saw zero cannot have an uncounted body behind it.
void GateEntryProtocol()
{
    std::printf("  the hook detach gate closes before it reads the count\n");
    HookGate gate;
    CHECK(gate.Enter() == GateEntry::Counted); // a normal body
    CHECK(gate.InFlight() == 1);
    CHECK(!gate.BeginDetach()); // a body in flight: the detach may not clear anything
    CHECK(gate.Draining());
    // A late entrant while the detach is waiting: it may still call its original (the pass-through phase is open), and
    // it is NOT counted with the bodies, so the wait for them is not extended by calls that arrive after the close.
    CHECK(gate.Enter() == GateEntry::PassThrough);
    CHECK(gate.InFlight() == 1);
    CHECK(gate.Passing() == 1);
    gate.PassDone();
    gate.Leave();
    CHECK(gate.Ready());
    // The pass-through phase has to be closed and drained before the originals are cleared; Complete() refuses
    // otherwise and clears nothing.
    CHECK(!gate.Complete());
    CHECK(!gate.Cleared());
    CHECK(gate.DetachFor(kHookDrainTimeoutMs, [] { return uint64_t { 0 }; }, [](uint32_t) {}));
    CHECK(gate.Complete());
    CHECK(gate.Cleared());
    // The gate stays shut until the new class is hooked: a body that arrives now must call nothing.
    CHECK(gate.Enter() == GateEntry::Refused);
    CHECK(gate.InFlight() == 0);
    gate.Reattached();
    CHECK(!gate.Draining());
    CHECK(gate.Enter() == GateEntry::Counted);
    gate.Leave();
    CHECK(gate.InFlight() == 0);
    std::printf("    [ok] a counted body blocks the detach, a late one passes through, and Reattached opens the gate again\n");
}

// The same rule with real threads: a detach that saw zero in-flight bodies must never clear the originals while a body
// that was counted is still inside, and no body may enter as counted once the gate is closed.
void GateRace()
{
    std::printf("  a detach racing real hook bodies\n");
    std::atomic<int> clearedWhileCounted { 0 };
    std::atomic<int> countedAfterClear { 0 };
    std::atomic<int> passThroughWhileCounted { 0 };
    int entered = 0;
    for (int round = 0; round < 200; ++round)
    {
        HookGate gate;
        std::atomic<bool> originalsValid { true }; // the detach clears this once it has put the real targets back
        std::atomic<bool> go { false };
        std::atomic<bool> stop { false };

        std::thread bodies[4];
        for (std::thread& body : bodies)
        {
            body = std::thread([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                while (!stop.load(std::memory_order_acquire))
                {
                    const GateEntry entry = gate.Enter();
                    if (entry == GateEntry::Refused)
                        continue; // the originals are gone: call nothing
                    if (entry == GateEntry::PassThrough)
                    {
                        // The original is still valid here, and the detach must be waiting for this call. It must not
                        // have cleared anything yet.
                        if (gate.Cleared())
                            passThroughWhileCounted.fetch_add(1);
                        gate.PassDone();
                        continue;
                    }
                    // Counted: the detach must not have cleared the originals while this body is inside, and no body
                    // may be counted once they are.
                    if (gate.Cleared())
                        clearedWhileCounted.fetch_add(1);
                    if (gate.InFlight() == 0)
                        clearedWhileCounted.fetch_add(1);
                    std::this_thread::yield();
                    // The sharper sample: after this body did its work, and after a detach that may have completed in
                    // between. Either signal while this body is still counted is the defect.
                    if (gate.Cleared())
                        clearedWhileCounted.fetch_add(1);
                    if (!originalsValid.load(std::memory_order_acquire))
                        clearedWhileCounted.fetch_add(1);
                    gate.Leave();
                }
            });
        }

        go.store(true, std::memory_order_release);
        std::this_thread::yield();
        // The detach: close the gate, wait until no counted body is inside, then drain the pass-throughs, clear, and
        // keep the gate shut. The retry loop only exists because a body can read the open gate and enter just before
        // the close; it is bounded by the timeout of each attempt.
        while (!gate.BeginDetach())
            std::this_thread::yield();
        const bool drained = gate.DetachFor(kHookDrainTimeoutMs, NowMsReal, SleepMsReal);
        if (drained)
        {
            if (!gate.Complete())
                ++countedAfterClear;
            // The originals are gone from here on. A counted body that still runs after this line is the defect the
            // second sample in the body exists for.
            originalsValid.store(false, std::memory_order_release);
        }
        else
        {
            // A pass-through that did not leave in time: the detach gives up without clearing, and the gate reopens.
            gate.Reattached();
        }
        stop.store(true, std::memory_order_release);
        for (std::thread& body : bodies)
            body.join();
        if (gate.InFlight() != 0 || gate.Passing() != 0)
            countedAfterClear.fetch_add(1);
        entered += 1;
    }
    CHECK(entered == 200);
    CHECK(clearedWhileCounted == 0);
    CHECK(countedAfterClear == 0);
    CHECK(passThroughWhileCounted == 0);
    std::printf("    [ok] 200 rounds, 4 bodies each: no clear with a body inside, no counted body after the clear\n");
}

// The wait itself. A body that leaves after 20 ms lets the detach through; a body that stays makes it answer Busy no
// earlier than the timeout. The clock is the caller's, so the second case does not sleep for real.
void GateWait()
{
    std::printf("  the detach waits for a body that leaves, and gives up after the timeout\n");
    {
        HookGate gate;
        std::atomic<bool> inside { false };
        std::thread body([&] {
            CHECK(gate.Enter() == GateEntry::Counted);
            inside.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate.Leave();
        });
        while (!inside.load(std::memory_order_acquire))
            std::this_thread::yield();
        uint64_t clock = 0;
        const bool drained = gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) {
            clock += ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        });
        body.join();
        CHECK(drained);
        CHECK(clock >= 2);        // the wait found the body still inside at least once
        CHECK(clock < 100);       // and the body leaving ended it well before the timeout
        CHECK(gate.Complete());
        std::printf("    [ok] a body that leaves after 20 ms is drained in place (waited %llu ms)\n",
                    static_cast<unsigned long long>(clock));
    }
    {
        HookGate gate;
        CHECK(gate.Enter() == GateEntry::Counted); // this body never leaves
        uint64_t clock = 0;
        const bool drained = gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) {
            clock += ms;
        });
        CHECK(!drained);
        CHECK(clock >= kHookDrainTimeoutMs); // Busy no earlier than the timeout
        CHECK(!gate.Complete());            // and nothing is cleared
        CHECK(!gate.Cleared());
        gate.Reattached();
        CHECK(gate.Enter() == GateEntry::Counted); // the abandoned detach reopened the gate
        gate.Leave();
        std::printf("    [ok] a body that does not leave keeps the originals (Busy at %llu ms)\n",
                    static_cast<unsigned long long>(clock));
    }
    {
        // A pass-through still inside its original also has to be waited for, or the detach would clear it mid-call.
        HookGate gate;
        CHECK(gate.BeginDetach());
        CHECK(gate.Enter() == GateEntry::PassThrough);
        uint64_t clock = 0;
        const bool drained = gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) {
            clock += ms;
        });
        CHECK(!drained);         // the pass-through is still calling an original
        CHECK(!gate.Complete());
        gate.PassDone();
        clock = 0;
        CHECK(gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) { clock += ms; }));
        CHECK(gate.Complete());
        std::printf("    [ok] a pass-through in flight is waited for too, and the originals stay valid meanwhile\n");
    }
}

// The late entrant made deterministic. The seam runs between the first gate read and the count increment, so the test
// can start a detach exactly in the window a scheduler would otherwise have to hit by chance.
void GateLateEntrantSeam()
{
    std::printf("  a late entrant at the exact window between the gate read and the count\n");
    HookGate gate;
    bool detachedInSeam = false;
    std::pair<HookGate*, bool*> seamState { &gate, &detachedInSeam };
    gate.SetEnterSeam(
        [](void* context) {
            auto* state = static_cast<std::pair<HookGate*, bool*>*>(context);
            *state->second = state->first->BeginDetach();
        },
        &seamState);

    // The seam closes the gate after the entrant read it open: Enter() must notice on the second read and pass through
    // instead of counting itself, and it must not call Leave() (the count is zero).
    const GateEntry entry = gate.Enter();
    CHECK(detachedInSeam);
    CHECK(entry == GateEntry::PassThrough);
    CHECK(gate.InFlight() == 0);
    CHECK(gate.Passing() == 1);
    gate.PassDone();
    CHECK(gate.Passing() == 0);
    std::printf("    [ok] an entrant that read the open gate before the close passes through and is not counted\n");
}

// The RAII scope and the pass-through answer. A counted call runs the body's logic; a call that arrives while a detach
// drains runs the original and returns its result; a call that arrives after the originals were restored returns the
// fixed answer the slot defines.
void HookScopeAnswers()
{
    std::printf("  the hook scope answers every body the same way\n");
    HookGate gate;
    HookGate& global = HookGateFor(Class::XeFg);
    CHECK(&HookGateFor(Class::FsrFg) == &global); // one gate for the process, whichever class asks
    CHECK(&HookGateFor() == &global);             // a body that names no class reaches the same object
    (void)global;

    int bodyRuns = 0;
    int originalCalls = 0;
    auto original = [&] {
        ++originalCalls;
        return static_cast<uint32_t>(0x1234);
    };

    {
        const HookScope scope(gate);
        if (scope)
            ++bodyRuns;
        CHECK(scope.Counted());
    }
    CHECK(bodyRuns == 1);
    CHECK(gate.InFlight() == 0); // the destructor left the count

    CHECK(gate.BeginDetach());
    {
        const HookScope scope(gate);
        CHECK(!scope.Counted());
        const uint32_t answer = scope.Pass(original, static_cast<uint32_t>(0x0BAD));
        CHECK(answer == 0x1234); // the original is still valid while the detach drains
    }
    CHECK(originalCalls == 1);
    CHECK(gate.Passing() == 0); // the destructor ended the pass-through
    uint64_t clock = 0;
    CHECK(gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) { clock += ms; }));
    CHECK(gate.Complete());     // both phases are drained: the originals may be cleared

    {
        const HookScope scope(gate); // the originals are cleared: the original must not run
        const uint32_t answer = scope.Pass(original, static_cast<uint32_t>(0x0BAD));
        CHECK(answer == 0x0BAD);
    }
    CHECK(originalCalls == 1);
    CHECK(gate.InFlight() == 0 && gate.Passing() == 0);
    std::printf("    [ok] counted runs the body, pass-through returns the original, a cleared gate returns the fixed "
                "answer\n");
}

void HookScopeWritesOutParameters()
{
    std::printf("  a refused slot that answers with out-parameters writes them, and only then\n");
    // The two Get bodies of the contract answer with a BOOL and a pointer, not with a value inside HRESULT: the refused
    // path must write both, because the caller reads them after S_OK. The pass-through path must NOT run the refusing
    // callable: the original owns the out-parameters while it is still valid, and it overwrites them afterwards.
    HookGate gate;
    uint32_t fullscreen = 0xFFFFFFFFu; // stand-in for BOOL*
    void* target = reinterpret_cast<void*>(0x1234);
    int originals = 0;
    int refusals = 0;

    auto body = [&] { // hkGetFullscreenState's shape: the counted path runs the body, every other path passes through
        const HookScope scope(gate);
        if (!scope)
            return scope.Pass(
                [&] {
                    ++originals;
                    fullscreen = 1; // the original's answer
                    target = reinterpret_cast<void*>(0x2222);
                    return kOk;
                },
                [&] {
                    ++refusals;
                    fullscreen = 0; // FALSE
                    target = nullptr;
                    return kOk;
                });
        return static_cast<uint32_t>(kOk);
    };

    // Counted: the body runs and neither pass-through branch is reached.
    CHECK(body() == kOk);
    CHECK(originals == 0 && refusals == 0);
    CHECK(gate.InFlight() == 0);

    // Pass-through while the detach drains: the original writes the out-parameters, the refusing callable does not run.
    fullscreen = 0xFFFFFFFFu;
    target = reinterpret_cast<void*>(0x1234);
    CHECK(gate.BeginDetach());
    {
        const HookScope scope(gate);
        const uint32_t answer = scope.Pass(
            [&] {
                ++originals;
                fullscreen = 1;
                target = reinterpret_cast<void*>(0x2222);
                return kOk;
            },
            [&] {
                ++refusals;
                fullscreen = 0;
                target = nullptr;
                return kOk;
            });
        CHECK(answer == kOk);
        CHECK(fullscreen == 1 && target == reinterpret_cast<void*>(0x2222));
        CHECK(refusals == 0);
    }
    CHECK(gate.Passing() == 0);

    // Detach finished and the originals are restored: the refusing callable writes both out-parameters.
    uint64_t clock = 0;
    CHECK(gate.DetachFor(kHookDrainTimeoutMs, [&] { return clock; }, [&](uint32_t ms) { clock += ms; }));
    CHECK(gate.Complete());
    fullscreen = 0xFFFFFFFFu;
    target = reinterpret_cast<void*>(0x1234);
    CHECK(body() == kOk);
    CHECK(fullscreen == 0); // FALSE, never left unwritten
    CHECK(target == nullptr);
    CHECK(refusals == 1 && originals == 1);
    CHECK(gate.InFlight() == 0 && gate.Passing() == 0);
    std::printf("    [ok] the refused answer writes every out-parameter, and the pass-through never runs it\n");
}
} // namespace

int main()
{
    StatusRoundTrip();
    BridgeRegistryTest();
    GateEntryProtocol();
    GateRace();
    GateWait();
    GateLateEntrantSeam();
    HookScopeAnswers();
    HookScopeWritesOutParameters();

    if (g_failed == 0)
        std::printf("ALL RUNTIME SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
