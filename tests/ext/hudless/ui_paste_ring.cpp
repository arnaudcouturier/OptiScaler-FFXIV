// S10 package C: the paste ring rules (ext/hudless/UiPasteRing.h) as a pure state model. No GPU, no clock: fence values
// and time are fed in by the test.
#include "ext/hudless/UiPasteRing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ext::hudless::ring;

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

// One real frame of the producer as UiPaste::Produce performs it. Returns the written slot or -1 (skipped).
struct Producer
{
    Ring& ring;
    uint64_t issued = 0; // produce-fence values handed out
    int Frame(const Fences& done, double nowMs)
    {
        const int previous = ring.Tick();
        const int target = ring.PickTarget(previous, done);
        if (target < 0)
            return -1;
        ring.BeginWrite(target);
        ring.EndWrite(target, ++issued, nowMs);
        return target;
    }
};

void FreshRing()
{
    std::printf("  fresh ring\n");
    Ring ring;
    const Fences done {};
    EXPECT(ring.PickPaste(done, 0.0, false) == -1, "nothing to paste");
    EXPECT(!ring.AnyPasteable(done, 0.0), "not pasteable");
    EXPECT(ring.Newest() == -1, "no newest");
    EXPECT(ring.Tick() == -1 && ring.frame == 1, "first tick has no previous image");
    EXPECT(ring.PickTarget(-1, done) == 0, "an empty ring offers slot 0");
}

void PickNewestCompleted()
{
    std::printf("  consumer picks the newest COMPLETED image\n");
    Ring ring;
    Producer producer { ring };
    Fences done {};
    const int a = producer.Frame(done, 0.0); // value 1, frame 1
    done.produced = 1;
    const int b = producer.Frame(done, 10.0); // value 2, frame 2
    const int c = producer.Frame(done, 20.0); // value 3, frame 3
    EXPECT(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c, "three distinct slots");
    EXPECT(ring.PickPaste(done, 25.0, false) == a, "only the first write completed -> picks it, not the newer ones");
    done.produced = 2;
    EXPECT(ring.PickPaste(done, 25.0, false) == b, "second completed -> newest completed");
    done.produced = 3;
    EXPECT(ring.PickPaste(done, 25.0, false) == c, "third completed");
    EXPECT(ring.PickPaste(Fences { 0, 0 }, 25.0, false) == -1, "device removed / nothing completed -> nothing");
}

void StaleImages()
{
    std::printf("  images older than 250 ms are never pasted\n");
    Ring ring;
    Producer producer { ring };
    Fences done {};
    const int a = producer.Frame(done, 1000.0);
    done.produced = 1;
    EXPECT(ring.PickPaste(done, 1249.9, false) == a, "249.9 ms old");
    EXPECT(ring.PickPaste(done, 1250.0, false) == a, "exactly 250 ms old is still allowed");
    EXPECT(ring.PickPaste(done, 1250.1, false) == -1, "250.1 ms old is stale");
    EXPECT(!ring.AnyPasteable(done, 1300.0), "Ready() view agrees");
    EXPECT(ring.AnyPasteable(done, 1100.0), "fresh in the model's terms");
}

void NeverBackInTime()
{
    std::printf("  never older than the last pasted image\n");
    Ring ring;
    Producer producer { ring };
    Fences done {};
    const int x = producer.Frame(done, 0.0); // frame 1, value 1
    const int y = producer.Frame(done, 1.0); // frame 2, value 2
    done.produced = 2;
    EXPECT(ring.PickPaste(done, 2.0, false) == y, "newest wins");
    ring.BeginRead(y);
    ring.EndRead(y, 1);
    EXPECT(ring.lastPastedFrame == 2, "last pasted frame is %llu", (unsigned long long) ring.lastPastedFrame);
    EXPECT(ring.PickPaste(done, 3.0, false) == y, "the same image may be pasted again (generated frames)");
    EXPECT(ring.slots[x].ready && ring.PickPaste(done, 3.0, false) != x, "older image is passed over");
    ring.slots[y].ready = false; // as if the newer one were invalidated away
    EXPECT(ring.PickPaste(done, 3.0, false) == -1, "the older image is NOT shown instead (would go back in time)");
}

void SteadyTiming()
{
    std::printf("  steady timing picks the image of one real frame behind\n");
    Ring ring;
    Producer producer { ring };
    Fences done {};
    const int f1 = producer.Frame(done, 0.0);
    const int f2 = producer.Frame(done, 10.0);
    const int f3 = producer.Frame(done, 20.0);
    done.produced = 3;
    EXPECT(ring.frame == 3, "frame counter");
    EXPECT(ring.PickPaste(done, 21.0, false) == f3, "normal timing: newest");
    EXPECT(ring.PickPaste(done, 21.0, true) == f2, "steady timing: one frame behind the newest frame submitted");
    done.produced = 1;
    EXPECT(ring.PickPaste(done, 21.0, true) == f1, "steady: newest allowed image that completed");
    // only the newest frame exists: the fallback shows it instead of letting the UI blink out
    Ring only;
    Producer p2 { only };
    Fences d2 {};
    const int g = p2.Frame(d2, 0.0);
    d2.produced = 1;
    EXPECT(only.PickPaste(d2, 1.0, true) == g, "steady falls back to the newest qualifying image");
}

void ProducerNeverReusesBusySlots()
{
    std::printf("  producer never reuses a busy slot\n");
    {
        // write not completed
        Ring ring;
        Producer producer { ring };
        Fences done {};
        int written = 0;
        for (int i = 0; i < 8; ++i)
            written += producer.Frame(done, double(i)) >= 0; // the GPU completes nothing
        EXPECT(written == 4, "with nothing completed each slot is written once and never again: %d", written);
        EXPECT(ring.PickTarget(-1, done) == -1, "every slot is in flight -> skip");
    }
    {
        // paste not completed
        Ring ring;
        Producer producer { ring };
        Fences done {};
        for (int i = 0; i < 4; ++i)
        {
            producer.Frame(done, double(i));
            done.produced = producer.issued;
        }
        for (int s = 0; s < kSlots; ++s)
        {
            ring.BeginRead(s);
            ring.EndRead(s, 10 + s);
        }
        done.pasted = 9;
        EXPECT(ring.PickTarget(-1, done) == -1, "all slots still read by pending pastes -> skip");
        done.pasted = 10; // slot 0's paste completed
        const int t = ring.PickTarget(-1, done);
        EXPECT(t >= 0 && ring.slots[t].lastReadValue <= done.pasted, "only a slot whose paste completed (%d)", t);
        done.pasted = 100;
        EXPECT(ring.PickTarget(-1, done) >= 0, "everything completed -> a slot is available");
    }
    {
        // a paste between pick and submit blocks reuse, an abandoned pick releases it
        Ring ring;
        Producer producer { ring };
        Fences done {};
        producer.Frame(done, 0.0);
        done.produced = 1;
        const int s = ring.PickPaste(done, 1.0, false);
        ring.BeginRead(s);
        for (int i = 0; i < 6; ++i)
        {
            const int t = ring.PickTarget(-1, done);
            EXPECT(t != s, "slot being read is never the target");
            if (t >= 0)
            {
                ring.BeginWrite(t);
                ring.EndWrite(t, ++producer.issued, 0.0);
                done.produced = producer.issued;
            }
        }
        ring.AbortRead(s);
        EXPECT(ring.slots[s].readers == 0, "abort releases the reader");
    }
    {
        // poisoned slots (signal failed) are gone for good
        Ring ring;
        ring.Tick();
        ring.PoisonWrite(0);
        EXPECT(ring.PickTarget(-1, Fences { UINT64_MAX - 1, UINT64_MAX - 1 }) != 0, "poisoned slot is never a target");
        EXPECT(ring.PickPaste(Fences { UINT64_MAX - 1, 0 }, 0.0, false) == -1, "nor pasted");
    }
    {
        // previous and newest are excluded
        Ring ring;
        Producer producer { ring };
        Fences done {};
        const int a = producer.Frame(done, 0.0);
        done.produced = producer.issued;
        const int previous = ring.Tick();
        EXPECT(previous == a, "the slot written on the preceding tick is the cleanup input");
        EXPECT(ring.PickTarget(previous, done) != a, "previous slot is never the target");
        ring.Tick();
        EXPECT(ring.Tick() == -1, "after a tick without a write there is no previous image");
    }
}

void InvalidateAndReset()
{
    std::printf("  invalidate / reset\n");
    Ring ring;
    Producer producer { ring };
    Fences done {};
    producer.Frame(done, 0.0);
    done.produced = 1;
    EXPECT(ring.AnyPasteable(done, 1.0), "pasteable");
    ring.Tick();
    ring.InvalidateReady();
    EXPECT(!ring.AnyPasteable(done, 1.0) && ring.PickPaste(done, 1.0, false) == -1, "invalidate stops pasting");
    EXPECT(ring.Tick() == -1, "and breaks the cleanup chain");

    // in-flight bookkeeping survives an invalidate: an uncompleted write is still not a target
    Ring busy;
    Producer p2 { busy };
    const Fences none {};
    const int written = p2.Frame(none, 0.0);
    busy.InvalidateReady();
    EXPECT(busy.PickTarget(-1, none) != written, "uncompleted write stays protected after invalidate");

    const uint32_t epoch = busy.epoch;
    busy.Reset();
    EXPECT(busy.epoch == epoch + 1 && !busy.slots[0].allocated && busy.lastWritten == -1,
           "reset forgets the slots and bumps the epoch");
    EXPECT(busy.frame == 1, "frame keeps counting");
}

void ReadAccounting()
{
    std::printf("  read accounting\n");
    Ring ring;
    ring.slots[1].frame = 4;
    ring.BeginRead(1);
    ring.BeginRead(1);
    EXPECT(ring.slots[1].readers == 2, "two readers");
    ring.EndRead(1, 7);
    ring.EndRead(1, 5);
    EXPECT(ring.slots[1].readers == 0 && ring.slots[1].lastReadValue == 7, "lastReadValue is the maximum (%llu)",
           (unsigned long long) ring.slots[1].lastReadValue);
    ring.BeginRead(1);
    ring.EndRead(1, kNever);
    ring.BeginRead(1);
    ring.EndRead(1, 9);
    EXPECT(ring.slots[1].lastReadValue == kNever, "a paste without a completion token keeps the slot out for good");
    EXPECT(ring.lastPastedFrame == 4, "last pasted frame follows the read slot");
}

// Randomised run: GPU completion, pastes and producer frames interleave arbitrarily; the rules must hold throughout.
void RandomisedInvariants(bool steady)
{
    std::printf("  randomised run (%s timing)\n", steady ? "steady" : "normal");
    Ring ring;
    Producer producer { ring };
    uint64_t seed = steady ? 0x9E3779B97F4A7C15ull : 0xD1B54A32D192ED03ull;
    auto rnd = [&](uint32_t n)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t((seed >> 33) % n);
    };

    Fences done {};
    uint64_t pasteIssued = 0;
    double now = 0.0;
    std::vector<int> pendingReads;
    uint64_t lastShownFrame = 0;
    int pastes = 0, writes = 0, skips = 0;

    for (int step = 0; step < 20000; ++step)
    {
        now += rnd(20) / 2.0;
        switch (rnd(7))
        {
        case 0: // the GPU makes progress on the produce queue
            done.produced = std::min<uint64_t>(producer.issued, done.produced + rnd(3));
            break;
        case 1: // ... and on the present queue
            done.pasted = std::min<uint64_t>(pasteIssued, done.pasted + rnd(3));
            break;
        case 2:
        case 3: // real frame
        {
            const int previous = ring.Tick();
            const int target = ring.PickTarget(previous, done);
            if (target < 0)
            {
                ++skips;
                break;
            }
            const Slot& s = ring.slots[target];
            EXPECT(s.readers == 0, "step %d: target has readers", step);
            EXPECT(s.producedValue <= done.produced, "step %d: target write incomplete", step);
            EXPECT(!(s.allocated && s.lastReadValue > done.pasted), "step %d: target still read by a paste", step);
            EXPECT(target != previous && target != ring.Newest(), "step %d: target is previous/newest", step);
            ring.BeginWrite(target);
            ring.EndWrite(target, ++producer.issued, now);
            ++writes;
            break;
        }
        case 4: // a paste starts
        {
            const int pick = ring.PickPaste(done, now, steady);
            if (pick < 0)
                break;
            const Slot& s = ring.slots[pick];
            EXPECT(s.producedValue <= done.produced, "step %d: pasted an incomplete image", step);
            EXPECT(now - s.producedAtMs <= kStaleImageMs, "step %d: pasted a stale image", step);
            EXPECT(s.frame >= lastShownFrame, "step %d: UI went back in time (%llu < %llu)", step,
                   (unsigned long long) s.frame, (unsigned long long) lastShownFrame);
            lastShownFrame = s.frame;
            ring.BeginRead(pick);
            pendingReads.push_back(pick);
            ++pastes;
            break;
        }
        case 5: // a started paste is submitted
            if (!pendingReads.empty())
            {
                const size_t i = rnd(uint32_t(pendingReads.size()));
                ring.EndRead(pendingReads[i], ++pasteIssued);
                pendingReads.erase(pendingReads.begin() + long(i));
            }
            break;
        default: // the game did not produce this frame
            if (rnd(4) == 0)
            {
                ring.Tick();
                ring.InvalidateReady();
            }
            break;
        }
    }
    std::printf("    %d writes, %d skipped frames, %d pastes\n", writes, skips, pastes);
    EXPECT(writes > 1000 && pastes > 500, "the run exercised the rules (%d writes, %d pastes)", writes, pastes);
}
} // namespace

int main()
{
    FreshRing();
    PickNewestCompleted();
    StaleImages();
    NeverBackInTime();
    SteadyTiming();
    ProducerNeverReusesBusySlots();
    InvalidateAndReset();
    ReadAccounting();
    RandomisedInvariants(false);
    RandomisedInvariants(true);
    std::printf("[ui_paste_ring] failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
