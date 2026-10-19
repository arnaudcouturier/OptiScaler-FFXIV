// L2 on a fake mutex: LockDepthGuard / UnlockedScope (WindowLiveOps.h) are what WindowModule.cpp uses around its three calls
// out. The fake mutex counts how often the one test thread owns it and records every unlock it does not own (undefined
// behaviour on a real recursive mutex, and a lock left held for every other thread afterwards).
#include "window_test.h"
#include "ext/window/WindowLiveOps.h"

using namespace ext::window;

namespace
{
struct FakeMutex
{
    int owned = 0;
    int violations = 0;
    void lock() { ++owned; }
    void unlock()
    {
        if (owned == 0)
            ++violations;
        else
            --owned;
    }
};
} // namespace

int main()
{
    // One level held (an entry point's own lock): the call out gives it up, takes it back, depth is restored.
    {
        FakeMutex m;
        int depth = 0;
        m.lock();
        live::LockDepthGuard entry(depth);
        CHECK(m.owned == 1 && depth == 1);
        {
            live::UnlockedScope<FakeMutex> out(m, depth);
            CHECK(m.owned == 0 && depth == 0);
        }
        CHECK(m.owned == 1 && depth == 1);
        CHECK(m.violations == 0);
    }

    // Re-entrant entries on the same thread (two levels): both are given up and both come back.
    {
        FakeMutex m;
        int depth = 0;
        m.lock();
        live::LockDepthGuard outer(depth);
        m.lock();
        live::LockDepthGuard inner(depth);
        CHECK(m.owned == 2 && depth == 2);
        {
            live::UnlockedScope<FakeMutex> out(m, depth);
            CHECK(m.owned == 0 && depth == 0);
        }
        CHECK(m.owned == 2 && depth == 2);
        CHECK(m.violations == 0);
    }

    // No level held (a call out made without the lock): nothing is unlocked, nothing is locked afterwards.
    {
        FakeMutex m;
        int depth = 0;
        {
            live::UnlockedScope<FakeMutex> out(m, depth);
            CHECK(m.owned == 0 && depth == 0);
        }
        CHECK(m.owned == 0 && depth == 0 && m.violations == 0);
    }

    // The latent defect: inside the call out the game's window procedure re-enters an entry point on the same thread, and that
    // nested entry itself reaches a call out. The nested scope must give up exactly the one level it owns.
    {
        FakeMutex m;
        int depth = 0;
        m.lock();
        live::LockDepthGuard entry(depth);
        {
            live::UnlockedScope<FakeMutex> out(m, depth); // the outer call out
            CHECK(m.owned == 0 && depth == 0);

            m.lock(); // the nested entry point takes the lock again ...
            {
                live::LockDepthGuard nested(depth);
                CHECK(m.owned == 1 && depth == 1);
                {
                    live::UnlockedScope<FakeMutex> in(m, depth); // ... and reaches a call out of its own
                    CHECK(m.owned == 0 && depth == 0);
                    CHECK(m.violations == 0);
                }
                CHECK(m.owned == 1 && depth == 1);
            }
            m.unlock(); // the nested entry returns
            CHECK(m.owned == 0 && depth == 0);
        }
        // back in the outer entry: it owns its level again, and nothing was over-unlocked or left locked
        CHECK(m.owned == 1 && depth == 1);
        CHECK(m.violations == 0);
    }

    return wt::g_failed ? 1 : 0;
}
