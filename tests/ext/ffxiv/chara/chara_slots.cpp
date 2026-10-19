#include "check.h"
#include "ext/ffxiv/chara/CharaJitter.h"
#include "ext/ffxiv/chara/CharaSlots.h"

#include <set>
#include <vector>

using namespace ext::ffxiv::chara;

namespace
{
constexpr uintptr_t kA = 0xA000;
constexpr uintptr_t kB = 0xB000;

void TestRecentList()
{
    SlotTable t;
    CHECK(!t.RecentContains(kA, 1000));
    t.NoteRecent(kA, 1000, 1);
    CHECK(t.RecentContains(kA, 1000));
    CHECK(t.RecentContains(kA, 1250));   // 250 ms is still recent
    CHECK(!t.RecentContains(kA, 1251));  // 251 ms is not
    CHECK(!t.RecentContains(kA, 999));   // a clock that went backwards: not recent
    CHECK(!t.RecentContains(0, 1000));

    // refreshing an entry extends it
    t.NoteRecent(kA, 1200, 2);
    CHECK(t.RecentContains(kA, 1450));
    CHECK(!t.RecentContains(kA, 1451));

    // a null texture is ignored
    t.NoteRecent(0, 1000, 1);
    CHECK(!t.RecentContains(0, 1000));

    // 16 entries; a 17th replaces the oldest live one (or an expired one first)
    SlotTable full;
    for (uintptr_t i = 1; i <= kMaxRecent; ++i)
        full.NoteRecent(0x1000 + i, 1000 + i, 1); // 0x1001 is the oldest
    for (uintptr_t i = 1; i <= kMaxRecent; ++i)
        CHECK(full.RecentContains(0x1000 + i, 1100));
    full.NoteRecent(0x2000, 1100, 1);
    CHECK(full.RecentContains(0x2000, 1100));
    CHECK(!full.RecentContains(0x1001, 1100)); // the oldest was replaced
    CHECK(full.RecentContains(0x1002, 1100));
    // an expired entry is preferred over a live one
    full.NoteRecent(0x3000, 1260, 1); // entries 0x1002 (tick 1002) .. expired by 1260? 1260-1002 = 258 > 250
    CHECK(full.RecentContains(0x3000, 1260));
}

void TestConsumerAcquire()
{
    SlotTable t;
    // not recent: no slot
    CHECK(t.AcquireForConsumer(kA, 1000, 576, 960) == kNoSlot);
    CHECK(t.Find(kA) == kNoSlot);

    t.NoteRecent(kA, 1000, 1);
    const int a = t.AcquireForConsumer(kA, 1100, 576, 960);
    CHECK(a != kNoSlot);
    CHECK(t.Find(kA) == a);
    CHECK(t.Slot(a).key == kA && t.Slot(a).width == 576 && t.Slot(a).height == 960 && t.Slot(a).lastUse == 1100);
    CHECK(!t.Slot(a).everEvaluated && !t.Slot(a).failed);

    // the same texture keeps its slot even after the recent entry expired
    CHECK(t.AcquireForConsumer(kA, 5000, 576, 960) == a);

    // a recent entry that expired cannot claim a new slot
    t.NoteRecent(kB, 1000, 1);
    CHECK(t.AcquireForConsumer(kB, 1300, 576, 960) == kNoSlot);

    // 16 slots at most
    SlotTable many;
    for (uintptr_t i = 1; i <= kMaxSlots; ++i)
    {
        many.NoteRecent(0x100 * i, 1000, 1);
        CHECK(many.AcquireForConsumer(0x100 * i, 1000, 64, 64) != kNoSlot);
    }
    many.NoteRecent(0x9999, 1000, 1);
    CHECK(many.AcquireForConsumer(0x9999, 1000, 64, 64) == kNoSlot);
    CHECK(kMaxSlots == 16);
}

void TestResetRule()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 576, 960);
    CHECK(t.ShouldReset(s, 1000)); // never evaluated
    t.MarkEvaluated(s, 1100, 1);
    CHECK(!t.ShouldReset(s, 1100));
    CHECK(!t.ShouldReset(s, 1350)); // exactly 250 ms: no reset
    CHECK(t.ShouldReset(s, 1351));  // older than 250 ms
    CHECK(t.ShouldReset(s, 1099));  // clock went backwards
    t.ClearEvaluated(s);
    CHECK(t.ShouldReset(s, 1110));
}

void TestJitterGates()
{
    SlotTable t;
    CHECK(!t.JitterLive(1000) && !t.JitterApplies(1000));
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 576, 960);
    // a fresh slot: live, but the jitter does not apply until one evaluation succeeded
    CHECK(t.JitterLive(1000));
    CHECK(!t.JitterApplies(1000));
    t.MarkEvaluated(s, 1010, 1);
    CHECK(t.JitterApplies(1010));
    CHECK(t.JitterLive(1250) && t.JitterApplies(1250));   // used at 1000: 250 ms
    t.Touch(s, 1300);
    CHECK(t.JitterApplies(1550));
    CHECK(!t.JitterLive(1551) && !t.JitterApplies(1551)); // 251 ms since use
    // a failed slot is not live
    t.Touch(s, 2000);
    t.MarkFailed(s);
    CHECK(!t.JitterLive(2000) && !t.JitterApplies(2000));
    CHECK(t.Slot(s).failed);
}

void TestRebuild()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 576, 960);
    CHECK(!t.NeedsRebuild(s, 576, 960, false));
    CHECK(t.NeedsRebuild(s, 577, 960, false));
    CHECK(t.NeedsRebuild(s, 576, 959, false));
    CHECK(t.NeedsRebuild(s, 576, 960, true));
}

void TestCleanup()
{
    std::vector<int> released;
    std::set<int> pending;
    auto done = [&](int i) { return pending.count(i) == 0; };
    auto release = [&](int i) { released.push_back(i); };

    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    t.NoteRecent(kB, 1000, 1);
    const int a = t.AcquireForConsumer(kA, 1000, 576, 960);
    const int b = t.AcquireForConsumer(kB, 1000, 576, 960);
    t.Slot(a).hasFeature = true;
    t.Slot(b).hasFeature = true;
    t.MarkEvaluated(a, 1000, 1);
    t.MarkEvaluated(b, 1000, 1);
    t.Touch(b, 2500);
    CHECK(t.ActiveFeatures() == 2);

    // within 2000 ms of use nothing is released
    CHECK(t.Cleanup(3000, false, done, release) == 0);
    CHECK(t.Find(kA) == a && t.Find(kB) == b);
    // exactly 2000 ms is not "more than 2000"
    CHECK(t.Cleanup(3000, false, done, release) == 0);
    // a's last use is 1000: at 3001 it is idle for 2001 ms
    pending.insert(a); // its query is not done yet: kept
    CHECK(t.Cleanup(3001, false, done, release) == 0 && released.empty());
    CHECK(t.Find(kA) == a);
    pending.clear();
    CHECK(t.Cleanup(3001, false, done, release) == 1);
    CHECK(released.size() == 1 && released[0] == a);
    CHECK(t.Find(kA) == kNoSlot && t.Find(kB) == b);
    CHECK(t.ActiveFeatures() == 1);

    // disabling releases every slot (once its query is done)
    pending.insert(b);
    CHECK(t.Cleanup(3002, true, done, release) == 0);
    pending.clear();
    CHECK(t.Cleanup(3002, true, done, release) == 1);
    CHECK(t.Find(kB) == kNoSlot && t.ActiveFeatures() == 0);

    // a slot that was never evaluated has no pending query and releases at once
    SlotTable fresh;
    fresh.NoteRecent(kA, 1000, 1);
    const int f = fresh.AcquireForConsumer(kA, 1000, 64, 64);
    pending.insert(f); // would block, but there is no query to wait for
    released.clear();
    CHECK(fresh.Cleanup(4000, false, done, release) == 1);

    // a released slot starts clean when the texture comes back
    fresh.NoteRecent(kA, 4100, 2);
    const int again = fresh.AcquireForConsumer(kA, 4100, 64, 64);
    CHECK(again != kNoSlot && !fresh.Slot(again).failed && !fresh.Slot(again).everEvaluated);
}

void TestFailureSticks()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 64, 64);
    t.MarkFailed(s, 1000);
    // asking again returns the same, still failed, slot (no retry) ...
    CHECK(t.AcquireForConsumer(kA, 1100, 64, 64) == s);
    CHECK(t.Slot(s).failed);

    // ... for as long as it is in use, but not for ever: a failed slot is released after kFailedReleaseMs even while the UI keeps
    // touching it (a touch keeps a healthy slot alive, so a failed one could otherwise never be retried)
    const auto done = [](int) { return true; };
    int released = 0;
    const auto release = [&](int) { ++released; };
    for (uint64_t now = 1100; now <= 1000 + kFailedReleaseMs; now += 500)
    {
        t.Touch(s, now);
        CHECK(t.Cleanup(now, false, done, release) == 0); // still inside the window
    }
    t.Touch(s, 1000 + kFailedReleaseMs + 1);
    CHECK(t.Cleanup(1000 + kFailedReleaseMs + 1, false, done, release) == 1 && released == 1);
    CHECK(t.Find(kA) == kNoSlot); // free again: the next consumer claims a fresh slot
    t.NoteRecent(kA, 12000, 2);
    const int again = t.AcquireForConsumer(kA, 12000, 64, 64);
    CHECK(again != kNoSlot && !t.Slot(again).failed);

    // a slot marked failed without a clock value (0) is never released by this rule
    t.MarkFailed(again);
    t.Touch(again, 99999);
    CHECK(t.Cleanup(99999, false, done, release) == 0);

    // the failed release still waits for the slot's query
    SlotTable u;
    u.NoteRecent(kB, 1000, 1);
    const int b = u.AcquireForConsumer(kB, 1000, 64, 64);
    u.MarkEvaluated(b, 1000, 1);
    u.MarkFailed(b, 1100);
    u.Touch(b, 20000);
    CHECK(u.Cleanup(20000, false, [](int) { return false; }, release) == 0);
    CHECK(u.Cleanup(20000, false, done, release) == 1);
}

// A slot's GPU objects may go only after the work submitted for it finished: the same check serves the idle release and the rebuild
// of a resized preview (which used to free the NGX feature while the last evaluate could still be running).
void TestReadyForRelease()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 64, 64);
    int asked = 0;
    const auto notDone = [&](int) { ++asked; return false; };
    const auto done = [&](int) { ++asked; return true; };

    CHECK(t.ReadyForRelease(s, notDone) && asked == 0); // nothing submitted: no query to wait for
    t.MarkEvaluated(s, 1010, 1);
    CHECK(t.Slot(s).queryPending);
    CHECK(!t.ReadyForRelease(s, notDone) && asked == 1); // the evaluate may still run
    CHECK(t.Slot(s).queryPending);
    CHECK(t.ReadyForRelease(s, done) && asked == 2);
    CHECK(!t.Slot(s).queryPending); // cleared once it reported done
    CHECK(t.ReadyForRelease(s, notDone) && asked == 2); // not asked again

    // an HRESULT of a failed query counts as done (a lost device never reports S_OK); only S_FALSE means "not yet"
    CHECK(QueryResultMeansDone(0));            // S_OK
    CHECK(!QueryResultMeansDone(1));           // S_FALSE
    CHECK(QueryResultMeansDone(-2005270523));  // DXGI_ERROR_DEVICE_REMOVED (0x887A0005)
    CHECK(QueryResultMeansDone(-2147024809));  // E_INVALIDARG
}

// The consumer draws the SR result of a slot evaluated in this render; the NR bring-up uses this to skip a result nobody would see.
void TestEvaluatedInPass()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    const int s = t.AcquireForConsumer(kA, 1000, 64, 64);
    CHECK(!t.EvaluatedInPass(s, 3)); // never evaluated
    t.MarkEvaluated(s, 1010, 3);
    CHECK(t.EvaluatedInPass(s, 3));
    CHECK(!t.EvaluatedInPass(s, 4)); // evaluated in an earlier render: not this one
    CHECK(!t.EvaluatedInPass(s, 0)); // no pass
    t.ClearEvaluated(s);             // the render after it was rejected, stale, or waiting for guides
    CHECK(!t.EvaluatedInPass(s, 3));
    t.MarkEvaluated(s, 1020, 5);
    t.MarkFailed(s, 1030);
    CHECK(!t.EvaluatedInPass(s, 5)); // a failed slot shows nothing
}

// The jitter phase is a per-preview render count. The jitter of a pass is chosen before the finisher says which preview the render is for,
// so the slot is predicted from the order of the finishers seen so far.
struct Signs
{
    bool negative = false, positive = false;
    void Add(float jx)
    {
        negative = negative || jx < 0.0f;
        positive = positive || jx > 0.0f;
    }
    bool Both() const { return negative && positive; }
};

// Renders `count` renders; `slotOf(i)` is the slot (0 or 1) that render i is for. Returns the signs of jx handed to each slot's renders,
// from the table (JitterPhase) and, as a negative control, from the shared counter that was used before.
struct Streams
{
    Signs table[2], shared[2];
    int wrongPredictions = 0;
};

template <class SlotOf> Streams Simulate(SlotTable& t, const int slot[2], int count, SlotOf&& slotOf, int skip = 3)
{
    Streams out;
    uint32_t sharedCounter = 0;
    for (int i = 0; i < count; ++i)
    {
        ++sharedCounter;
        const int which = slotOf(i);
        const uint32_t phase = t.JitterPhase(sharedCounter);
        if (t.PredictPassSlot() != slot[which])
            ++out.wrongPredictions;
        if (i >= skip) // the first renders teach the order
        {
            out.table[which].Add(PassJitter(phase).x);
            out.shared[which].Add(PassJitter(sharedCounter).x);
        }
        t.NoteFinishedRender(slot[which]);
    }
    return out;
}

void TestJitterPhasePerSlot()
{
    SlotTable t;
    t.NoteRecent(kA, 100, 1);
    t.NoteRecent(kB, 100, 1);
    const int slot[2] = { t.AcquireForConsumer(kA, 100, 576, 960), t.AcquireForConsumer(kB, 100, 576, 960) };
    CHECK(slot[0] != kNoSlot && slot[1] != kNoSlot && slot[0] != slot[1]);

    // Nothing finished yet: no prediction, the shared counter is used (the jitter does not apply before the first evaluation anyway)
    CHECK(t.PredictPassSlot() == kNoSlot && t.JitterPhase(7) == 7);

    // Two previews that render in a fixed order every frame: each gets both signs of jx. With the shared counter (the old rule) one of them
    // only ever got the right half of the pixel and the other the left half.
    const Streams alternating = Simulate(t, slot, 64, [](int i) { return i & 1; });
    CHECK(alternating.table[0].Both() && alternating.table[1].Both());
    CHECK(alternating.wrongPredictions <= 3); // only while the order is learned (the first three renders)
    CHECK(!alternating.shared[0].Both() && !alternating.shared[1].Both());     // the old rule: one-sided
    CHECK(alternating.shared[0].negative != alternating.shared[1].negative);   // ... and opposite sides

    // The phase a render gets is the render count of its own preview: 1, 2, 3 ... for each
    {
        SlotTable u;
        u.NoteRecent(kA, 100, 1);
        u.NoteRecent(kB, 100, 1);
        const int s[2] = { u.AcquireForConsumer(kA, 100, 576, 960), u.AcquireForConsumer(kB, 100, 576, 960) };
        for (int i = 0; i < 20; ++i)
        {
            const int which = i & 1;
            if (i >= 2)
                CHECK(u.JitterPhase(999) == u.Slot(s[which]).renders); // the prediction names this render's preview
            u.NoteFinishedRender(s[which]);
        }
        CHECK(u.Slot(s[0]).renders == 10 && u.Slot(s[1]).renders == 10);
    }

    // One preview: its own sequence 1, 2, 3 ... (kJitterPhases = 32 phases, both signs)
    {
        SlotTable one;
        one.NoteRecent(kA, 100, 1);
        const int s[2] = { one.AcquireForConsumer(kA, 100, 576, 960), kNoSlot };
        const Streams single = Simulate(one, s, 40, [](int) { return 0; });
        CHECK(single.table[0].Both());
        CHECK(single.wrongPredictions <= 1);
        // the first jittered render of a slot: the slot has finished once (it is evaluated before the jitter applies): phase 1
        SlotTable first;
        first.NoteRecent(kA, 100, 1);
        const int f = first.AcquireForConsumer(kA, 100, 576, 960);
        first.NoteFinishedRender(f);
        const Jitter j = PassJitter(first.JitterPhase(55));
        CHECK(j.x == PassJitter(1).x && j.y == PassJitter(1).y);
    }

    // Irregular order (one preview renders three times for every render of the other): still both signs for the busy one
    {
        SlotTable u;
        u.NoteRecent(kA, 100, 1);
        u.NoteRecent(kB, 100, 1);
        const int s[2] = { u.AcquireForConsumer(kA, 100, 576, 960), u.AcquireForConsumer(kB, 100, 576, 960) };
        const Streams irregular = Simulate(u, s, 200, [](int i) { return (i % 4) == 3 ? 1 : 0; }, 8);
        CHECK(irregular.table[0].Both() && irregular.table[1].Both());
    }

    // A released slot is not predicted any more (the shared counter is used), and a slot that is acquired again starts counting from zero
    {
        SlotTable u;
        u.NoteRecent(kA, 100, 1);
        const int a = u.AcquireForConsumer(kA, 100, 576, 960);
        u.NoteFinishedRender(a);
        u.NoteFinishedRender(a);
        CHECK(u.PredictPassSlot() == a && u.JitterPhase(9) == 2);
        u.Cleanup(100 + kIdleReleaseMs + 1, false, [](int) { return true; }, [](int) {});
        CHECK(u.PredictPassSlot() == kNoSlot && u.JitterPhase(9) == 9);
        u.NoteRecent(kB, 5000, 1);
        const int b = u.AcquireForConsumer(kB, 5000, 576, 960);
        CHECK(b == a && u.Slot(b).renders == 0);
    }

    // A failed slot is not predicted
    {
        SlotTable u;
        u.NoteRecent(kA, 100, 1);
        const int a = u.AcquireForConsumer(kA, 100, 576, 960);
        u.NoteFinishedRender(a);
        u.MarkFailed(a, 100);
        CHECK(u.PredictPassSlot() == kNoSlot && u.JitterPhase(9) == 9);
        u.Clear();
        CHECK(u.PredictPassSlot() == kNoSlot);
    }
}

void TestRecentExpiryInCleanup()
{
    SlotTable t;
    t.NoteRecent(kA, 1000, 1);
    t.Cleanup(1251, false, [](int) { return true; }, [](int) {});
    CHECK(!t.RecentContains(kA, 1000)); // expired entries are dropped
    t.NoteRecent(kB, 1000, 1);
    t.Cleanup(1250, false, [](int) { return true; }, [](int) {});
    CHECK(t.RecentContains(kB, 1250));
}
} // namespace

int main()
{
    TestRecentList();
    TestConsumerAcquire();
    TestResetRule();
    TestJitterGates();
    TestRebuild();
    TestCleanup();
    TestFailureSticks();
    TestReadyForRelease();
    TestEvaluatedInPass();
    TestRecentExpiryInCleanup();
    TestJitterPhasePerSlot();
    return TEST_RESULT("chara_slots");
}
