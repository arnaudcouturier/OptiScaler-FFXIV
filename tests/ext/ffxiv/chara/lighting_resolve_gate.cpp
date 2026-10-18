// The transaction gate of the lighting layer: the pairing of the run-time attach/detach of the ResolveSubresource detour and the rule that
// two transactions never run at the same time. The Detours calls themselves are the callables passed in (counted here).
#include "check.h"
#include "ext/ffxiv/lighting/FfxivResolveGate.h"

#include <atomic>
#include <chrono>
#include <thread>

using ext::ffxiv::lighting::TransactionGate;

namespace
{
void TestPairing()
{
    TransactionGate gate;
    int attaches = 0, detaches = 0;
    bool installed = true;
    bool attachCommits = true, detachCommits = true;
    const auto ready = [&] { return installed; };
    const auto attach = [&]
    {
        ++attaches;
        return attachCommits;
    };
    const auto detach = [&]
    {
        ++detaches;
        return detachCommits;
    };

    CHECK(!gate.ResolveAttached());
    gate.DetachResolve(detach); // nothing attached: no transaction
    CHECK(detaches == 0);

    installed = false; // the main hooks are not installed: no attach
    CHECK(!gate.AttachResolve(ready, attach) && attaches == 0 && !gate.ResolveAttached());
    installed = true;

    attachCommits = false; // a transaction that fails leaves it detached, and the next request tries again
    CHECK(!gate.AttachResolve(ready, attach) && attaches == 1 && !gate.ResolveAttached());
    attachCommits = true;
    CHECK(gate.AttachResolve(ready, attach) && attaches == 2 && gate.ResolveAttached());
    CHECK(gate.AttachResolve(ready, attach) && attaches == 2); // already attached: not again

    detachCommits = false; // a failing detach leaves it attached
    gate.DetachResolve(detach);
    CHECK(detaches == 1 && gate.ResolveAttached());
    detachCommits = true;
    gate.DetachResolve(detach);
    CHECK(detaches == 2 && !gate.ResolveAttached());
    gate.DetachResolve(detach); // once only
    CHECK(detaches == 2);

    // The main detach took the detour with it (it saw it attached inside its own transaction and committed): the probe's later detach finds
    // nothing to do. Before, it detached a detour that was already gone and aborted its transaction.
    CHECK(gate.AttachResolve(ready, attach) && attaches == 3);
    const bool sawAttached = gate.Run([&] { const bool seen = gate.ResolveAttached(); gate.ResolveGone(); return seen; });
    CHECK(sawAttached && !gate.ResolveAttached());
    gate.DetachResolve(detach);
    CHECK(detaches == 2);
    // and the other way round: the probe detached first, the main detach sees nothing attached and leaves the detour out
    CHECK(gate.AttachResolve(ready, attach));
    gate.DetachResolve(detach);
    CHECK(!gate.Run([&] { return gate.ResolveAttached(); }));
}

// The readiness is evaluated under the lock: an attach that waited for a main detach finds the hooks gone.
void TestReadyUnderLock()
{
    TransactionGate gate;
    std::atomic<bool> installed { true };
    std::atomic<bool> inMain { false };
    std::atomic<bool> release { false };
    int attaches = 0;

    std::thread mainDetach(
        [&]
        {
            gate.Run(
                [&]
                {
                    inMain = true;
                    while (!release)
                        std::this_thread::yield();
                    installed = false; // the hooks are gone when the transaction ends
                    return 0;
                });
        });
    while (!inMain)
        std::this_thread::yield();
    bool attached = true;
    std::thread attacher([&] { attached = gate.AttachResolve([&] { return installed.load(); }, [&] { ++attaches; return true; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // the attacher is waiting for the lock
    release = true;
    mainDetach.join();
    attacher.join();
    CHECK(!attached && attaches == 0 && !gate.ResolveAttached());
}

// Two transactions of different threads never overlap: while one runs, the other cannot start. The first transaction starts the second
// thread, gives it time to run, and checks that it has not entered its own transaction.
template <class First, class Second> void CheckExclusive(TransactionGate& gate, First&& first, Second&& second, bool& overlapped)
{
    std::atomic<bool> secondEntered { false };
    std::thread other;
    first(
        [&]
        {
            other = std::thread([&] { second([&] { secondEntered = true; }); });
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            overlapped = overlapped || secondEntered.load(); // it must still be waiting for the lock
        });
    other.join();
    CHECK(secondEntered.load()); // and it ran once the first one ended
}

void TestNoOverlap()
{
    TransactionGate gate;
    bool overlapped = false;
    const auto ready = [] { return true; };

    // the main transaction (install / detach) against a run-time attach
    CheckExclusive(
        gate, [&](auto&& body) { gate.Run([&] { body(); return 0; }); },
        [&](auto&& body) { gate.AttachResolve(ready, [&] { body(); return true; }); }, overlapped);
    CHECK(!overlapped);

    // a run-time attach against a main transaction
    CHECK(gate.ResolveAttached());
    gate.DetachResolve([] { return true; });
    CheckExclusive(
        gate, [&](auto&& body) { gate.AttachResolve(ready, [&] { body(); return true; }); },
        [&](auto&& body) { gate.Run([&] { body(); return 0; }); }, overlapped);
    CHECK(!overlapped);

    // a run-time detach against a main transaction
    CheckExclusive(
        gate, [&](auto&& body) { gate.DetachResolve([&] { body(); return true; }); },
        [&](auto&& body) { gate.Run([&] { body(); return 0; }); }, overlapped);
    CHECK(!overlapped);
}
} // namespace

int main()
{
    TestPairing();
    TestReadyUnderLock();
    TestNoOverlap();
    return TEST_RESULT("lighting_resolve_gate");
}
