// The slot life cycle of the DLSS path driven the way CharaDlss.cpp drives it: SlotTable with an injected clock plus the
// finish rules. No device.
#include "check.h"
#include "ext/ffxiv/chara/CharaDlssRules.h"
#include "ext/ffxiv/chara/CharaJitter.h"
#include "ext/ffxiv/chara/CharaSlots.h"

#include <set>

using namespace ext::ffxiv::chara;
using namespace ext::ffxiv::chara::dlss;

namespace
{
constexpr uintptr_t kOutput = 0x5000;

FinishFacts Ready(uint64_t now, int slot, const SlotTable& t)
{
    FinishFacts f;
    f.width = 576;
    f.height = 960;
    f.now = now;
    f.slotFound = slot != kNoSlot;
    if (f.slotFound)
    {
        f.slotFailed = t.Slot(slot).failed;
        f.lastUse = t.Slot(slot).lastUse;
        f.needsRebuild = t.NeedsRebuild(slot, 576, 960, false);
    }
    f.guides = { true, true, true, true };
    return f;
}

void TestFinishRules()
{
    SlotTable t;
    FinishFacts f = Ready(1000, kNoSlot, t);
    CHECK(DecideFinish(f) == FinishVerdict::NoSlot);

    f.width = 4097;
    f.height = 4097;
    CHECK(DecideFinish(f) == FinishVerdict::TooLarge); // 2x output would exceed 8192
    f.width = 4096;
    f.height = 4096;
    CHECK(DecideFinish(f) != FinishVerdict::TooLarge);
    f.width = 0;
    CHECK(DecideFinish(f) == FinishVerdict::TooLarge);

    t.NoteRecent(kOutput, 1000, 1);
    const int slot = t.AcquireForConsumer(kOutput, 1000, 576, 960);
    CHECK(slot != kNoSlot);
    t.Slot(slot).reversedZ = false;
    CHECK(DecideFinish(Ready(1000, slot, t)) == FinishVerdict::Proceed);
    CHECK(DecideFinish(Ready(1250, slot, t)) == FinishVerdict::Proceed);
    CHECK(DecideFinish(Ready(1251, slot, t)) == FinishVerdict::Stale); // not used by a UI draw for more than 250 ms

    // every missing guide is its own verdict and the check order is motion, depth, depth view, camera
    f = Ready(1000, slot, t);
    f.guides = { false, false, false, false };
    CHECK(DecideFinish(f) == FinishVerdict::WaitingForMotion);
    f.guides = { true, false, false, false };
    CHECK(DecideFinish(f) == FinishVerdict::WaitingForDepth);
    f.guides = { true, true, false, false };
    CHECK(DecideFinish(f) == FinishVerdict::WaitingForDepthView);
    f.guides = { true, true, true, false };
    CHECK(DecideFinish(f) == FinishVerdict::WaitingForCamera);
    CHECK(IsWaiting(FinishVerdict::WaitingForCamera) && !IsWaiting(FinishVerdict::Stale));
    CHECK(WaitingText(FinishVerdict::WaitingForMotion)[0] != '\0' && WaitingText(FinishVerdict::Proceed)[0] == '\0');

    // size or depth convention change -> rebuild
    f = Ready(1000, slot, t);
    f.needsRebuild = t.NeedsRebuild(slot, 288, 480, false);
    CHECK(DecideFinish(f) == FinishVerdict::Rebuild);
    f.needsRebuild = t.NeedsRebuild(slot, 576, 960, true);
    CHECK(DecideFinish(f) == FinishVerdict::Rebuild);

    // a failed slot stays failed
    t.MarkFailed(slot);
    CHECK(DecideFinish(Ready(1000, slot, t)) == FinishVerdict::Failed);
}

void TestDepthReadable()
{
    CHECK(DepthReadable(44, true));   // R24G8_TYPELESS bound as a shader resource
    CHECK(!DepthReadable(44, false));
    CHECK(!DepthReadable(45, true));  // D24_UNORM_S8_UINT: no typeless view possible
    CHECK(!DepthReadable(39, true));  // R32_TYPELESS: not the 24-bit path
}

// A preview from first sight to release, with the clock injected.
void TestLifecycle()
{
    SlotTable t;
    uint64_t now = 10000;
    std::set<int> released;
    bool queryDone = false;
    auto cleanup = [&](bool disabled) {
        return t.Cleanup(
            now, disabled, [&](int) { return queryDone; }, [&](int i) { released.insert(i); });
    };

    // Pass 1: the finisher runs, no slot yet, so nothing is jittered or finished.
    t.NoteRecent(kOutput, now, 1);
    CHECK(t.Find(kOutput) == kNoSlot);
    CHECK(!t.JitterLive(now) && !t.JitterApplies(now));

    // A UI draw samples the output within 250 ms: a slot is claimed; it is live but not yet evaluated, so the jitter
    // stays off for the first evaluation.
    now += 16;
    const int slot = t.AcquireForConsumer(kOutput, now, 576, 960);
    CHECK(slot != kNoSlot);
    CHECK(t.JitterLive(now) && !t.JitterApplies(now));
    CHECK(t.ShouldReset(slot, now)); // never evaluated: the history is reset

    // Pass 2: first evaluation (reset), then the jitter applies and the next evaluation keeps the history.
    now += 16;
    t.Touch(slot, now);
    CHECK(t.ShouldReset(slot, now));
    t.MarkEvaluated(slot, now, 2);
    CHECK(t.JitterApplies(now));
    now += 16;
    t.Touch(slot, now);
    CHECK(!t.ShouldReset(slot, now));
    t.MarkEvaluated(slot, now, 3);

    // A gap of more than 250 ms (window hidden): history reset again, and the jitter is off while the slot is not live.
    now += 300;
    CHECK(!t.JitterLive(now) && !t.JitterApplies(now));
    CHECK(t.ShouldReset(slot, now));

    // Not released at 2000 ms of idleness while its GPU work is pending; released once the query reports done.
    now += 2000;
    CHECK(cleanup(false) == 0);
    queryDone = true;
    CHECK(cleanup(false) == 1);
    CHECK(released.count(slot) == 1 && t.Find(kOutput) == kNoSlot);
}

// A failure latch drains: slots are released through the query-done path even while the path is "disabled".
void TestLatchDrain()
{
    SlotTable t;
    uint64_t now = 5000;
    std::set<int> released;
    bool queryDone = false;
    auto cleanup = [&](bool disabled) {
        return t.Cleanup(
            now, disabled, [&](int) { return queryDone; }, [&](int i) { released.insert(i); });
    };

    t.NoteRecent(kOutput, now, 1);
    const int a = t.AcquireForConsumer(kOutput, now, 576, 960);
    t.NoteRecent(kOutput + 0x100, now, 1);
    const int b = t.AcquireForConsumer(kOutput + 0x100, now, 576, 960);
    t.MarkEvaluated(a, now, 1); // a has work in flight (query pending), b has none
    CHECK(a != kNoSlot && b != kNoSlot && a != b);

    // disabled: b goes at once, a waits for its query
    CHECK(cleanup(true) == 1 && released.count(b) == 1 && released.count(a) == 0);
    CHECK(t.Find(kOutput) == a);
    queryDone = true;
    CHECK(cleanup(true) == 1 && released.count(a) == 1);
    CHECK(t.ActiveFeatures() == 0);
}

// The jitter sequence is the one the slots hand to the viewport shift.
void TestJitterFeed()
{
    SlotTable t;
    t.NoteRecent(kOutput, 100, 1);
    const int slot = t.AcquireForConsumer(kOutput, 100, 576, 960);
    t.MarkEvaluated(slot, 100, 1);
    Jitter seen[kJitterPhases];
    for (uint32_t pass = 1; pass <= kJitterPhases; ++pass)
        seen[pass - 1] = t.JitterApplies(100) ? PassJitter(pass) : Jitter {};
    int distinct = 0;
    for (uint32_t i = 0; i < kJitterPhases; ++i)
    {
        bool unique = true;
        for (uint32_t j = 0; j < i; ++j)
            unique = unique && !(seen[i].x == seen[j].x && seen[i].y == seen[j].y);
        distinct += unique ? 1 : 0;
        CHECK(seen[i].x > -0.5f && seen[i].x < 0.5f && seen[i].y > -0.5f && seen[i].y < 0.5f);
    }
    CHECK(distinct == static_cast<int>(kJitterPhases));
}
} // namespace

int main()
{
    TestFinishRules();
    TestDepthReadable();
    TestLifecycle();
    TestLatchDrain();
    TestJitterFeed();
    return TEST_RESULT("chara_dlss_lifecycle");
}
