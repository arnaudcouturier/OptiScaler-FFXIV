#include "check.h"

#include "ext/ffxiv/chara/CharaLifecycle.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace ext::ffxiv::chara;

namespace
{
void TestDecision()
{
    CHECK(DecideDetach(true) == DetachAction::Now);
    CHECK(DecideDetach(false) == DetachAction::Defer);

    // the dispatcher gate: inert wins, then a pending teardown (the draw must reach the dispatcher), then a finished teardown
    CHECK(GateOverrideFor(true, false, false) == GateOverride::Off);
    CHECK(GateOverrideFor(true, true, false) == GateOverride::Off);
    CHECK(GateOverrideFor(false, true, false) == GateOverride::On);
    CHECK(GateOverrideFor(false, true, true) == GateOverride::On); // a new request after an earlier teardown
    CHECK(GateOverrideFor(false, false, true) == GateOverride::Off);
    CHECK(GateOverrideFor(false, false, false) == GateOverride::None);
}

void TestGate()
{
    DetachGate gate;
    CHECK(!gate.Pending() && !gate.Detached() && !gate.Take());
    gate.Request();
    gate.Request(); // two requests are one teardown
    CHECK(gate.Pending());
    CHECK(gate.Take());
    CHECK(!gate.Pending() && !gate.Take());
    CHECK(!gate.Detached());
    gate.Completed();
    CHECK(gate.Detached());
    gate.Request();
    gate.Reset(); // the hooks were installed again
    CHECK(!gate.Pending() && !gate.Detached());
}

// The unhook thread asks, the draw thread performs the teardown exactly once, and the unhook thread never does.
void TestHandOverBetweenThreads()
{
    DetachGate gate;
    std::atomic<bool> drawThreadRunning { false };
    std::atomic<bool> stop { false };
    std::atomic<int> teardowns { 0 };
    std::thread::id teardownThread;
    const std::thread::id unhookThread = std::this_thread::get_id();

    std::thread draw(
        [&]
        {
            drawThreadRunning.store(true);
            // the draw loop: every entry asks the gate first
            while (!stop.load())
            {
                if (gate.Take())
                {
                    teardownThread = std::this_thread::get_id();
                    teardowns.fetch_add(1);
                    gate.Completed();
                }
                std::this_thread::yield();
            }
        });
    while (!drawThreadRunning.load())
        std::this_thread::yield();

    // the unhook thread: not the draw thread, so the decision is to defer
    const std::thread::id drawId = draw.get_id();
    CHECK(DecideDetach(unhookThread == drawId) == DetachAction::Defer);
    gate.Request();

    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gate.Detached() && std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
    stop.store(true);
    draw.join();

    CHECK(gate.Detached());
    CHECK(teardowns.load() == 1);
    CHECK(teardownThread == drawId && teardownThread != unhookThread);
}
} // namespace

int main()
{
    TestDecision();
    TestGate();
    TestHandOverBetweenThreads();
    return TEST_RESULT("chara_lifecycle");
}
