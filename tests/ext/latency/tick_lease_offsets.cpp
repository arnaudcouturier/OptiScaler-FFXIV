// The telemetry offsets are not the lease's business: a build that resolved only some of them must still install, the unresolved value must never be
// compared, and the observer must not read a counter whose address was never published. This is the other half of the install test (which installs
// with all three resolved): a separate binary, because one process installs one lease.
//
// The scenario is the reviewer's: the 2024 build with a foreign inline hook over the tick entry loses the frame-counter seam (it is the one anchor
// that starts at the entry) while the two QueryPerformanceCounter seams resolve. The table default left in the lost field contradicts them; if the
// layout check used it, a telemetry-only loss would refuse the lease.
#include "lease_harness.h"

namespace
{
void TestResolvedPairContradictionRefuses()
{
    ResetImage();
    SetBuild(ext::ffxiv::resolve::kLatencyAllOffsetsResolved, kFrameCounterOffset, kPerfValueOffset, kPerfValueOffset); // pv != pf + 8
    g_configKey = true;
    ext::latency::RefreshKeys();
    ResetAttemptCounters();
    ext::latency::OnDeviceHooked();
    CHECK(!ext::latency::Leased());
    CHECK(g_exchangeCalls == 0);
    CHECK(AnyNoteContains("layout"));
}

void TestUnresolvedOffsetIsSkipped()
{
    ResetImage();
    // Framecounter unresolved and left at the table default, which contradicts the two that did resolve. Only pv/pf are compared (they agree), so the
    // lease installs; the counter is never read and the probe gets `no counter`, which is what makes age and W read n/a instead of a guess.
    SetBuild(ext::ffxiv::resolve::kLatencyPerfValueResolved | ext::ffxiv::resolve::kLatencyPerfFreqResolved, 0x1234, kPerfValueOffset,
             kPerfFreqOffset);
    g_configKey = true;
    ext::latency::RefreshKeys();
    g_armed = true;
    g_tickCalls = 0;
    g_tickBegins.clear();
    g_tickEnds.clear();
    ResetAttemptCounters();

    ext::latency::OnDeviceHooked();
    CHECK(ext::latency::Leased());
    CHECK(g_exchangeCalls == 1);
    CHECK(AnyNoteContains("installed"));
    CHECK(AnyNoteContains("counter=n/a"));

    std::uint32_t counter = 1;
    CHECK(!ext::latency::ReadFrameCounter(counter)); // no address was published for an unresolved offset
    CHECK(counter == 1);

    auto** vtable = reinterpret_cast<void**>(g_image + kVtableAt);
    const auto observer = reinterpret_cast<bool (*)(void*)>(vtable[4]);
    CHECK(observer(reinterpret_cast<void*>(g_image + kInstanceAt)) == g_tickResult);
    CHECK(g_tickCalls == 1);
    CHECK(g_tickBegins.size() == 1 && !g_tickBegins[0].counter && g_tickBegins[0].frame == 0);
    CHECK(g_tickEnds.size() == 1 && g_tickEnds[0] == 0);
}
} // namespace

int main()
{
    TestResolvedPairContradictionRefuses();
    TestUnresolvedOffsetIsSkipped();
    return TEST_RESULT("tick_lease_offsets");
}
