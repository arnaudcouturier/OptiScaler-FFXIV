// The lease's rules (ext/latency/TickLeaseCore.h) on ordinary memory: what CheckLease refuses and why (including the readable-span verdicts in front of
// every dereference), the layout rule over the resolved telemetry offsets only, the compare-exchange install of a slot that a real vtable stands in for
// (a local array of pointers, the observer a known pointer), the restore that must not take a slot back from a second owner, and the retry plan.
//
// The product side (TickLease.cpp) adds exactly the memory queries this test cannot make portably: VirtualQuery for the readable spans, VirtualProtect
// for the write, the pin, and the order the values are published in. Everything between them is here.
#include "../ffxiv/chara/check.h"

#include "ext/latency/TickLeaseCore.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace ext::latency;

namespace
{
// A vtable stand-in: eight slots, slot 4 the tick (`canonical`), the rest other functions.
struct FakeVtable
{
    void* slots[8] = {};
    static void* Load(void* slot) { return *static_cast<void**>(slot); }
    static bool Exchange(void* slot, void* expected, void* value) { return PlainExchange(slot, expected, value); }
    SlotOps Ops() { return SlotOps { &slots[4], &Load, &Exchange }; }
};

std::uint8_t g_tick[16] = {};
std::uint8_t g_observer[16] = {};
std::uint8_t g_foreign[16] = {};

void FillCanonical(FakeVtable& v)
{
    for (int i = 0; i < 8; ++i)
        v.slots[i] = g_tick + i;
    v.slots[4] = g_tick;
}

void TestCheckLease()
{
    FakeVtable v;
    FillCanonical(v);
    LeaseCheck ok;
    ok.instance = &v;
    ok.vtable = v.slots;
    ok.canonicalTick = g_tick;
    ok.vtableInReadOnlyImage = true;
    ok.layoutConsistent = true;
    ok.resolvedOffsets = ext::ffxiv::resolve::kLatencyAllOffsetsResolved;
    ok.frameCounterOffset = 0x16d0;
    ok.perfCounterValueOffset = 0x16b0;
    ok.perfCounterFreqOffset = 0x16a8;
    CHECK(CheckLease(ok) == LeaseVerdict::Ready);

    // nothing published yet: not a refusal, a retry
    LeaseCheck notReady = ok;
    notReady.instance = nullptr;
    CHECK(CheckLease(notReady) == LeaseVerdict::NotReady);
    notReady = ok;
    notReady.vtable = nullptr;
    CHECK(CheckLease(notReady) == LeaseVerdict::NotReady);
    notReady = ok;
    notReady.canonicalTick = nullptr;
    CHECK(CheckLease(notReady) == LeaseVerdict::NotReady);

    // a vtable pointer that is not read-only image data (a heap copy, a rotated table)
    LeaseCheck heap = ok;
    heap.vtableInReadOnlyImage = false;
    CHECK(CheckLease(heap) == LeaseVerdict::NotReadOnly);

    // the readable-span verdicts run before anything is dereferenced: a pointer to unmapped memory is refused, never read, and never reported as
    // "not published yet" or as a slot mismatch
    LeaseCheck unreadableSlot = ok;
    unreadableSlot.instanceSlotReadable = false;
    CHECK(CheckLease(unreadableSlot) == LeaseVerdict::NotReadable);
    LeaseCheck unreadableInstance = ok;
    unreadableInstance.instanceReadable = false;
    CHECK(CheckLease(unreadableInstance) == LeaseVerdict::NotReadable);
    LeaseCheck unreadableVtable = ok;
    unreadableVtable.vtableReadable = false;
    CHECK(CheckLease(unreadableVtable) == LeaseVerdict::NotReadable);
    // even when another owner holds the slot: the unreadable pointer wins, because the slot bytes cannot be trusted either
    LeaseCheck unreadableForeign = unreadableInstance;
    unreadableForeign.vtable = nullptr;
    CHECK(CheckLease(unreadableForeign) == LeaseVerdict::NotReadable);

    // another owner holds the slot: a tool that hooked the same slot, or a build whose Tick is a different function
    FakeVtable foreign;
    FillCanonical(foreign);
    foreign.slots[4] = g_foreign;
    LeaseCheck taken = ok;
    taken.vtable = foreign.slots;
    CHECK(CheckLease(taken) == LeaseVerdict::SlotMismatch);

    // the third offset does not belong with the other two
    LeaseCheck layout = ok;
    layout.layoutConsistent = false;
    CHECK(CheckLease(layout) == LeaseVerdict::LayoutMismatch);

    // the verdict names are what the log line says (a wrong reason is the one thing a player cannot check)
    CHECK(std::string(VerdictName(LeaseVerdict::SlotMismatch)).find("does not hold") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::NotReadOnly)).find("read-only") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::LayoutMismatch)).find("layout") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::NotWritable)).find("writable") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::NotReadable)).find("readable") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::NotPinned)).find("pinned") != std::string::npos);
    CHECK(std::string(VerdictName(LeaseVerdict::Ready)).find("ready") != std::string::npos);
}

void TestLayout()
{
    using namespace ext::ffxiv::resolve;
    // the two real builds: 0x16d0 / 0x16b0 / 0x16a8 and 0x16c8 / 0x16a8 / 0x16a0
    CHECK(LayoutConsistent(0x16d0, 0x16b0, 0x16a8));
    CHECK(LayoutConsistent(0x16c8, 0x16a8, 0x16a0));
    // a mixed pair: the frame counter 8 above where the clock block says the value is
    CHECK(!LayoutConsistent(0x16d0, 0x16a8, 0x16a0));
    // the two QueryPerformanceCounter fields in the wrong order
    CHECK(!LayoutConsistent(0x16c8, 0x16a0, 0x16a8));
    // the frame counter alone, off by one field size
    CHECK(!LayoutConsistent(0x16d8, 0x16b0, 0x16a8));

    // only the resolved offsets are compared, so an offset the resolver lost can neither refuse the lease nor hide a contradiction between the two
    // that did resolve. The stale value below is what an unresolved Framecounter keeps (the table default): it is not compared at all.
    const std::uint8_t both = kLatencyPerfValueResolved | kLatencyPerfFreqResolved;
    CHECK(LayoutConsistent(both, 0x16d0, 0x16b0, 0x16a8)); // fc skipped, pv/pf agree
    CHECK(!LayoutConsistent(both, 0x16d0, 0x16b0, 0x16a0)); // pv/pf contradict each other
    const std::uint8_t fcOnly = kLatencyFrameCounterResolved | kLatencyPerfValueResolved;
    CHECK(LayoutConsistent(fcOnly, 0x16d0, 0x16b0, 0x16a0)); // pf skipped: the pair that resolved agrees
    CHECK(!LayoutConsistent(fcOnly, 0x16d8, 0x16b0, 0x16a8)); // the pair that resolved contradicts itself
    CHECK(LayoutConsistent(kLatencyFrameCounterResolved, 0x16d8, 0x1234, 0x5678)); // one offset alone: nothing to compare
    CHECK(LayoutConsistent(0, 0x16d8, 0x1234, 0x5678)); // nothing resolved: the install must still go ahead
    // a missing middle offset leaves the outer two uncomparible rather than guessing a direct gap
    CHECK(LayoutConsistent(kLatencyFrameCounterResolved | kLatencyPerfFreqResolved, 0x16d8, 0x1234, 0x16a8));
}

void TestSlotInstallAndRestore()
{
    FakeVtable v;
    FillCanonical(v);
    const SlotOps ops = v.Ops();
    CHECK(ops.slot != nullptr);

    // install: the slot held the canonical tick, it holds the observer afterwards, and `original` is the old function
    void* original = nullptr;
    CHECK(LeaseSlot(ops, g_tick, g_observer, original) == InstallOutcome::Installed);
    CHECK(original == g_tick);
    CHECK(v.slots[4] == g_observer);

    // a second install over a slot that no longer holds the canonical tick is refused and writes nothing
    void* second = nullptr;
    CHECK(LeaseSlot(ops, g_tick, g_foreign, second) == InstallOutcome::NotCanonical);
    CHECK(second == nullptr && v.slots[4] == g_observer);

    // restore while the slot is ours: the canonical value comes back
    CHECK(RestoreSlot(ops, g_observer, g_tick) == RestoreOutcome::Restored);
    CHECK(v.slots[4] == g_tick);

    // restore while another owner chained over us: refused, and the other owner's pointer is left exactly where it is
    void* third = nullptr;
    CHECK(LeaseSlot(ops, g_tick, g_observer, third) == InstallOutcome::Installed);
    v.slots[4] = g_foreign; // a second owner replaced our observer with its own detour
    CHECK(RestoreSlot(ops, g_observer, g_tick) == RestoreOutcome::ForeignOwner);
    CHECK(v.slots[4] == g_foreign);

    // a slot that became empty (a tool that removed its own hook without putting ours back) is not ours either
    v.slots[4] = nullptr;
    CHECK(RestoreSlot(ops, g_observer, g_tick) == RestoreOutcome::ForeignOwner);
    CHECK(v.slots[4] == nullptr);

    // a compare-exchange that cannot happen (the slot changed under the check) reports ExchangeFailed and leaves the slot alone
    struct Racing
    {
        static void* Load(void* slot) { return *static_cast<void**>(slot); }
        static bool Exchange(void*, void*, void*) { return false; }
    };
    void* none = nullptr;
    v.slots[4] = g_tick;
    const SlotOps racing { &v.slots[4], &Racing::Load, &Racing::Exchange };
    CHECK(LeaseSlot(racing, g_tick, g_observer, none) == InstallOutcome::ExchangeFailed);
    CHECK(none == nullptr && v.slots[4] == g_tick);

    // a missing operation is an exchange failure, never an install
    const SlotOps empty {};
    void* missing = nullptr;
    CHECK(LeaseSlot(empty, g_tick, g_observer, missing) == InstallOutcome::ExchangeFailed);
    CHECK(missing == nullptr);
}

void TestPassThrough()
{
    // the observer calls the old function and records nothing while the process shuts down or the lease is parked
    CHECK(!PassThrough(false, false));
    CHECK(PassThrough(true, false));
    CHECK(PassThrough(false, true));
    CHECK(PassThrough(true, true));
}

// The retry budget is the design's numbers, not whatever the constants happen to be: every other check below derives its expectations from the same
// constants, so this is the one that fails when the cadence itself moves.
void TestRetryBudget()
{
    CHECK(kRetryPresents == 120);
    CHECK(kRefusalCap == 8);
    CHECK(kRetryWindowUs == 600'000'000);
}

void TestRetryPlan()
{
    Attempts a;
    CHECK(a.Due(0)); // the first device hook attempts at once
    a.Record(false, false, 0); // the instance was not published yet: not a refusal
    CHECK(!a.Due(1));
    for (std::uint32_t i = 1; i < kRetryPresents; ++i)
        a.Present();
    CHECK(!a.Due(1000));
    a.Present();
    CHECK(a.Due(2000)); // 120 presents later
    a.Record(true, false, 2000); // installed
    CHECK(!a.Due(3'000'000) && a.Settled());

    // refusals stop the plan after the cap, and a refusal counts from the first one
    Attempts r;
    CHECK(r.Due(0));
    for (std::uint32_t i = 0; i < kRefusalCap; ++i)
    {
        r.Record(false, true, 0);
        CHECK(r.Refusals() == i + 1);
    }
    CHECK(r.Settled() && !r.Due(1));

    // an attempt is not due twice within one present window
    Attempts w;
    CHECK(w.Due(0));
    w.Record(false, false, 0);
    for (std::uint32_t i = 0; i < kRetryPresents - 1; ++i)
        w.Present();
    CHECK(!w.Due(1));
    w.Present();
    CHECK(w.Due(1));

    // out of time: ten minutes after the first attempt nothing more is due, even with no refusal at all
    Attempts t;
    CHECK(t.Due(0));
    t.Record(false, false, 0);
    for (std::uint32_t i = 0; i < kRetryPresents; ++i)
        t.Present();
    CHECK(!t.Due(kRetryWindowUs));
    CHECK(!t.Due(kRetryWindowUs + 1));
    CHECK(!t.Settled()); // out of time is not "settled by a refusal": the state stays as it is
    t.Record(false, false, kRetryWindowUs + 1);
    CHECK(t.Settled());
}
} // namespace

int main()
{
    TestCheckLease();
    TestLayout();
    TestSlotInstallAndRestore();
    TestPassThrough();
    TestRetryBudget();
    TestRetryPlan();
    return TEST_RESULT("tick_lease");
}
