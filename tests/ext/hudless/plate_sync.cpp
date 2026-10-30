// The plate engine's two locks (ext/hudless/PlateSync.h) in the test build: a same-thread re-lock is reported instead
// of deadlocking, the leaf lock count of the current thread is what AssertNoLocksHeld looks at, a non-leaf lock (the
// pass mutex) may be held across stand-in calls, two threads exclude each other, and the RAII scope always releases.
#include <windows.h>

#include "ext/hudless/PlateSync.h"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace ext::hudless::plates::sync;

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

void Clear()
{
    ClearViolations();
}
} // namespace

int main()
{
    // 1. One leaf scope: no violation, and the leaf count is back to zero afterwards
    {
        Clear();
        Mutex leaf(true);
        {
            Lock lock(leaf);
            EXPECT(Violations() == 0, "a plain leaf lock is no violation: %s", LastViolation().c_str());
            AssertNoLocksHeld("stand-in under the leaf");
            EXPECT(Violations() == 1, "a stand-in call under the leaf is a violation");
            EXPECT(LastViolation() == "stand-in under the leaf", "the violation carries the stand-in name: %s",
                   LastViolation().c_str());
        }
        Clear();
        AssertNoLocksHeld("stand-in outside the leaf");
        EXPECT(Violations() == 0, "a stand-in call outside the leaf is fine");
    }

    // 2. The pass mutex is not a leaf: the pass may hold it across calls that are not allowed under the leaf
    {
        Clear();
        Mutex pass(false);
        {
            Lock lock(pass);
            AssertNoLocksHeld("stand-in under the pass mutex");
            EXPECT(Violations() == 0, "the pass mutex does not count as a held leaf");
        }
    }

    // 3. A same-thread re-lock is reported and does not deadlock; the inner scope's unlock leaves the outer one intact
    {
        Clear();
        Mutex leaf(true);
        {
            Lock outer(leaf);
            {
                Lock inner(leaf); // reported, and the section still runs
                AssertNoLocksHeld("inside the re-locked section");
            }
            EXPECT(Violations() == 2, "the re-lock and the call under it are both reported: %s",
                   LastViolation().c_str());
            EXPECT(LastViolation() == "inside the re-locked section", "the re-lock did not clear the leaf count: %s",
                   LastViolation().c_str());
        }
        Clear();
        AssertNoLocksHeld("after the re-locked sections");
        EXPECT(Violations() == 0, "the leaf is released once the outer section ends");
    }

    // 4. Two threads exclude each other, and the leaf counter is per thread
    {
        Clear();
        Mutex leaf(true);
        std::atomic<int> inside { 0 };
        std::atomic<int> overlapped { 0 };
        auto body = [&]()
        {
            for (int i = 0; i < 2000; ++i)
            {
                Lock lock(leaf);
                if (inside.fetch_add(1) != 0)
                    ++overlapped;
                std::this_thread::yield();
                inside.fetch_sub(1);
            }
        };
        std::thread a(body);
        std::thread b(body);
        a.join();
        b.join();
        EXPECT(overlapped.load() == 0, "two threads never hold the leaf together");
        EXPECT(Violations() == 0, "no violation from another thread: %s", LastViolation().c_str());
    }

    // 5. The leaf count is per thread: holding it on one thread does not make a stand-in on another thread fail
    {
        Clear();
        Mutex leaf(true);
        std::atomic<bool> otherSawViolation { true };
        {
            Lock lock(leaf);
            std::thread other([&]()
                              {
                                  AssertNoLocksHeld("other thread");
                                  otherSawViolation = Violations() != 0;
                              });
            other.join();
        }
        EXPECT(!otherSawViolation.load(), "a stand-in on another thread sees no leaf lock");
        Clear();
    }

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: plate locks: leaf re-lock reported, leaf count per thread, pass mutex not a leaf, two threads "
              "exclude each other, scopes always release");
    return 0;
}
