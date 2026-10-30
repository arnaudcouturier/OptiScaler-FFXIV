// The product side of the lease (ext/latency/TickLease.cpp) with stand-ins for the four Win32 calls it makes: the production order of the install on a
// stand-in image (a page the test owns, a vtable in it, a tick trampoline in it), the checks in front of every dereference (a pointer to unmapped
// memory is refused and never read), the layout rule over the resolved telemetry offsets only, the pin before the write, and the observer.
//
// The order test is the one that would have caught the HIGH finding this round fixes: the compare-exchange stand-in calls the observer from inside the
// exchange, exactly where a game thread can take the slot. The observer must run the original tick there - before the fix the original was published
// after the exchange and the observer returned the game's own teardown value without running a tick.
//
// The last section pins what cannot be observed from a run: with the key off, the present path reads nothing but the mirrored key before it returns;
// the retry plan's state is atomic because the present thread advances it while an attempt records.
#include "lease_harness.h"

namespace
{
void TestRefusals()
{
    ResetImage();
    SetBuild(ext::ffxiv::resolve::kLatencyAllOffsetsResolved);

    // the key off: no attempt, nothing read or written (and no failure line: this is not a refusal)
    g_configKey = g_keyOn = false;
    ResetAttemptCounters();
    const std::size_t notes = g_notes.size();
    ext::latency::OnDeviceHooked();
    CHECK(g_notes.size() == notes);
    CHECK(g_pinCalls == 0 && g_protectCalls == 0 && g_exchangeCalls == 0);
    for (int i = 0; i < 500; ++i) // the present path with the key off: nothing but the mirrored key
        ext::latency::OnBridgePresent();
    CHECK(g_pinCalls == 0 && g_protectCalls == 0 && g_exchangeCalls == 0);

    // the key on from here: every scenario below is a refusal
    g_configKey = true;
    ext::latency::RefreshKeys();
    CHECK(ext::latency::KeyOn());

    // 1. the image global points at unmapped memory: refused before it is dereferenced
    ExpectRefusal("readable", [] { SetInstanceGlobal(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xdead0000))); });

    // 2. the Framework object itself is unmapped: refused before the vtable pointer is read
    SetInstanceGlobal(g_image + kInstanceAt);
    ExpectRefusal("readable", [] { SetInstance(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xdead1000))); });

    // 3. the vtable pointer is unmapped: refused before the slot is read
    SetInstance(g_image + kVtableAt);
    ExpectRefusal("readable", [] { SetInstance(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xdead2000))); });

    // 4. the vtable's slot 4 crosses into an inaccessible page: the span check refuses it, the identity check never runs on a half-read slot
    ExpectRefusal("readable", [] { MakeVtableAtEnd(); });

    // 5. all three telemetry offsets resolved, but the two QueryPerformanceCounter gaps contradict each other
    ResetImage();
    SetBuild(ext::ffxiv::resolve::kLatencyAllOffsetsResolved, kFrameCounterOffset, kPerfValueOffset, kPerfValueOffset);
    ExpectRefusal("layout");

    // 6. the pin fails: refused, and nothing is made writable or exchanged
    SetBuild(ext::ffxiv::resolve::kLatencyAllOffsetsResolved);
    ExpectRefusal("pinned", [] { g_failPin = true; });
    g_failPin = false;

    // 7. the slot page cannot be made writable: refused, nothing exchanged
    ExpectRefusal("writable", [] { g_failProtect = true; });
    g_failProtect = false;
}

void TestInstallOrderAndObserver()
{
    ResetImage();
    SetBuild(ext::ffxiv::resolve::kLatencyAllOffsetsResolved);
    g_configKey = true;
    g_keyOn = true;
    g_armed = true;
    g_tickCalls = 0;
    g_tickBegins.clear();
    g_tickEnds.clear();
    ResetAttemptCounters();

    // the compare-exchange stand-in calls the observer from inside the exchange: the slot holds the observer the instant the exchange lands, and the
    // observer must find and run the original. Before this round's fix the original was stored after the exchange and this call returned false
    // without running the game's tick.
    g_onExchange = &CallObserverFromSlot;
    ext::latency::OnDeviceHooked();
    g_onExchange = nullptr;

    CHECK(ext::latency::Leased());
    CHECK(ext::latency::StateText() == std::string("installed"));
    CHECK(g_pinCalls >= 1); // pinned before the write
    CHECK(g_protectCalls >= 1 && g_exchangeCalls == 1);
    CHECK(g_tickCalls == 1); // the observer that ran during the exchange called the original
    CHECK(g_raceObserverResult == g_tickResult);
    CHECK(g_lastFramework == g_image + kInstanceAt);
    CHECK(AnyNoteContains("installed"));
    CHECK(AnyNoteContains("order=slot-first")); // who was first in the slot is on the line
    CHECK(AnyNoteContains("counter=on"));
    // the observer recorded on the game thread's behalf, with the counter it read on each side of the original
    CHECK(g_tickBegins.size() == 1 && g_tickBegins[0].frame == kFrameCounterStart && g_tickBegins[0].counter);
    CHECK(g_tickEnds.size() == 1 && g_tickEnds[0] == kFrameCounterStart + 1);

    // the slot now holds the observer, and every later call still runs the original: armed, unarmed and shutting down
    auto** vtable = reinterpret_cast<void**>(g_image + kVtableAt);
    CHECK(vtable[4] != reinterpret_cast<void*>(g_base + kTickRva));
    const auto observer = reinterpret_cast<bool (*)(void*)>(vtable[4]);
    const int before = g_tickCalls;
    CHECK(observer(reinterpret_cast<void*>(g_image + kInstanceAt)) == g_tickResult);
    CHECK(g_tickCalls == before + 1);
    CHECK(g_tickBegins.size() == 2 && g_tickBegins[1].frame == kFrameCounterStart + 1);

    g_armed = false; // the probe between the install and its first present: a pass-through, still the original
    CHECK(observer(reinterpret_cast<void*>(g_image + kInstanceAt)) == g_tickResult);
    CHECK(g_tickCalls == before + 2);
    CHECK(g_tickBegins.size() == 2); // nothing recorded
    g_armed = true;

    State::Instance().isShuttingDown = true; // the documented shutdown pass-through also calls the original
    CHECK(observer(reinterpret_cast<void*>(g_image + kInstanceAt)) == g_tickResult);
    CHECK(g_tickCalls == before + 3);
    CHECK(g_tickBegins.size() == 2);
    State::Instance().isShuttingDown = false;

    // the unhook path puts the canonical tick back and never clears the original: a direct call still runs the game's tick
    ext::latency::OnUnload();
    CHECK(ext::latency::StateText() == std::string("restored"));
    CHECK(vtable[4] == reinterpret_cast<void*>(g_base + kTickRva));
    std::uint32_t counter = 0;
    CHECK(ext::latency::ReadFrameCounter(counter)); // the counter address survives the restore
    CHECK(counter == static_cast<std::uint32_t>(kFrameCounterStart + g_tickCalls));
    CHECK(observer(reinterpret_cast<void*>(g_image + kInstanceAt)) == g_tickResult);
    CHECK(g_tickCalls == before + 4);
}
} // namespace

// ---- pins for what a run cannot observe ----
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos)
        return {};
    const std::size_t end = text.find(to, at + from.size());
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void TestSourcePins(const std::string& repo)
{
    const std::string lease = ReadFile(repo + "/OptiScaler/ext/latency/TickLease.cpp");
    CHECK(!lease.empty());
    const std::string present = Between(lease, "void OnBridgePresent()", "bool ReadFrameCounter");
    CHECK(!present.empty());
    // with the key off the present path returns on the mirrored key before it touches the retry plan, the gate or the clock
    const std::size_t keyGate = present.find("if (!KeyOn())");
    const std::size_t planAdvance = present.find("g_attempts.Present()");
    CHECK(keyGate != std::string::npos && planAdvance != std::string::npos && keyGate < planAdvance);
    CHECK(present.find("Cfg()") == std::string::npos); // no ini read per present
    CHECK(present.find("NowUs()") == std::string::npos || present.find("NowUs()") > keyGate);

    // every dereference of a pointer the anchors supplied is behind a readable-span check
    const std::string attempt = Between(lease, "LeaseVerdict Attempt()", "void TryInstall()");
    CHECK(!attempt.empty());
    const std::size_t checkInstance = attempt.find("*reinterpret_cast<const void* const*>(instanceAt)");
    const std::size_t readableGlobal = attempt.find("ReadableSpan(instanceAt");
    const std::size_t checkVtable = attempt.find("*static_cast<const void* const*>(instance)");
    const std::size_t readableInstance = attempt.find("ReadableSpan(instance");
    CHECK(readableGlobal != std::string::npos && checkInstance != std::string::npos && readableGlobal < checkInstance);
    CHECK(readableInstance != std::string::npos && checkVtable != std::string::npos && readableInstance < checkVtable);
    // the original is published before the exchange and never cleared by the restore
    const std::size_t publish = attempt.find("g_original.store");
    const std::size_t exchange = attempt.find("LeaseSlot(");
    CHECK(publish != std::string::npos && exchange != std::string::npos && publish < exchange);
    const std::string unload = Between(lease, "void OnUnload()", "bool Leased()");
    CHECK(unload.find("g_original.store(nullptr") == std::string::npos);

    // the retry plan is atomic: the present thread advances it while an attempt records
    const std::string core = ReadFile(repo + "/OptiScaler/ext/latency/TickLeaseCore.h");
    CHECK(core.find("std::atomic<std::uint64_t> _presents") != std::string::npos);
    CHECK(core.find("std::atomic<bool> _settled") != std::string::npos);
    CHECK(core.find("std::atomic<std::uint32_t> _refusals") != std::string::npos);
}

void StressAttempts()
{
    // Not a proof of absence of a race (C++ cannot give one), but the plan must survive the interleavings the two threads really produce: no torn
    // state, no lost present, and a successful record settles it.
    ext::latency::Attempts plan;
    std::atomic<bool> stop { false };
    std::atomic<std::uint64_t> presents { 0 };
    std::thread presentThread(
        [&]
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                plan.Present();
                presents.store(presents.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                (void) plan.Due(0);
            }
        });
    while (presents.load(std::memory_order_relaxed) == 0)
        std::this_thread::yield(); // the present thread really ran before the plan is resolved
    for (int i = 0; i < 2000; ++i)
    {
        plan.Record(false, false, 0);
        (void) plan.Due(0);
    }
    plan.Record(true, false, 0);
    stop.store(true, std::memory_order_relaxed);
    presentThread.join();
    CHECK(plan.Settled());
    CHECK(presents.load() > 0);
}

int main(int argc, char** argv)
{
    TestRefusals();
    TestInstallOrderAndObserver();
    if (argc > 1)
        TestSourcePins(argv[1]);
    else
        std::printf("SKIP: no repo argument, source pins not checked\n");
    StressAttempts();
    return TEST_RESULT("tick_lease_install");
}
