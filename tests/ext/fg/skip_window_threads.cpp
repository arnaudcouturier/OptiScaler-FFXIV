// The LoadLibrary skip window (`State::DisableChecks` / `EnableChecks` / `SkipDllName`) belongs to the thread that opened
// it. Every caller wraps a call that loads modules on its own thread, so a window must never disable the load checks for a
// module that another thread loads, a window ends with the owner id that opened it, and a thread that opened none never
// sees a skip. The state is a map keyed by owner id; shared between threads (the code before this test) a write in
// DisableChecks raced the load hook's read on every other thread.
//
// The assertions read the production code, sliced out of OptiScaler\State.h by prepare_skip_window_state.ps1.
#include "StateSkipWindow.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<int> g_failed { 0 };

void Report(bool skip, const std::string& name, const char* where)
{
    std::printf("  [%s] SkipDllChecks=%d SkipDllName=[%s]\n", where, skip ? 1 : 0, name.c_str());
}

// The state the calling thread must be in: a window is open (and holds this name) or none is.
bool ExpectState(bool wantSkip, const char* wantName, const char* where)
{
    const bool skip = State::SkipDllChecks();
    const std::string name = State::SkipDllName();
    const bool ok = skip == wantSkip && name == wantName;

    if (!ok)
    {
        std::printf("CHECK FAILED %s: wanted SkipDllChecks=%d SkipDllName=[%s]\n", where, wantSkip ? 1 : 0, wantName);
        Report(skip, name, where);
        ++g_failed;
    }

    return ok;
}

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)
} // namespace

int main()
{
    // No window: nothing is skipped, here and on a thread that was just created.
    ExpectState(false, "", "start");
    {
        std::atomic<bool> fresh { false };
        std::thread other([&fresh] {
            fresh = !State::SkipDllChecks() && State::SkipDllName().empty();
        });
        other.join();
        CHECK(fresh.load());
    }

    // A window opened here is invisible to another thread, and what that thread does with its own window is invisible here.
    {
        const UINT owner = State::GetOwner();
        State::DisableChecks(owner, "dxgi");
        ExpectState(true, "dxgi", "own window");

        std::atomic<bool> sawSkip { true };
        std::atomic<bool> sawName { true };
        std::thread other([&sawSkip, &sawName] {
            sawSkip = State::SkipDllChecks();
            sawName = !State::SkipDllName().empty();
        });
        other.join();
        CHECK(!sawSkip.load());
        CHECK(!sawName.load());

        // a window another thread opens and closes leaves this one exactly as it was
        std::thread other2([] {
            const UINT own = State::GetOwner();
            State::DisableChecks(own); // an empty name skips every load
            State::SkipDllChecks();
            State::SkipDllName();
            State::EnableChecks(own);
        });
        other2.join();
        ExpectState(true, "dxgi", "after another thread's window");

        State::EnableChecks(owner);
        ExpectState(false, "", "window closed");
    }

    // A window that a thread opened and never closed ends with that thread.
    {
        std::thread leaker([] {
            State::DisableChecks(State::GetOwner());
            State::SkipDllChecks();
        });
        leaker.join();
        ExpectState(false, "", "a thread that left its window open exited");
    }

    // Nesting: the last window opened decides the name, and closing it brings the outer one back.
    {
        const UINT outer = State::GetOwner();
        const UINT inner = State::GetOwner();
        State::DisableChecks(outer, "sl.");
        ExpectState(true, "sl.", "outer window");
        State::DisableChecks(inner, "sl.interposer");
        ExpectState(true, "sl.interposer", "inner window");
        State::EnableChecks(inner);
        ExpectState(true, "sl.", "inner window closed");
        State::EnableChecks(outer);
        ExpectState(false, "", "both closed");
    }

    // A window ends when the owner id it was opened with ends, so an outer window that closes first leaves the inner one
    // open (the caller that is still inside its load keeps its own skip).
    {
        const UINT outer = State::GetOwner();
        const UINT inner = State::GetOwner();
        State::DisableChecks(outer, "sl.");
        State::DisableChecks(inner, "sl.interposer");
        State::EnableChecks(outer);
        ExpectState(true, "sl.interposer", "outer window closed first");
        State::EnableChecks(inner);
        ExpectState(false, "", "both closed out of order");
    }

    // With three windows open the newest one decides the name, also once the middle one has ended.
    {
        const UINT oldest = State::GetOwner();
        const UINT middle = State::GetOwner();
        const UINT newest = State::GetOwner();
        State::DisableChecks(oldest, "sl.");
        State::DisableChecks(middle, "dxgi");
        State::DisableChecks(newest, "sl.interposer");
        ExpectState(true, "sl.interposer", "three windows");
        State::EnableChecks(middle);
        ExpectState(true, "sl.interposer", "the middle window closed");
        State::EnableChecks(newest);
        ExpectState(true, "sl.", "the newest closed, the oldest remains");
        State::EnableChecks(oldest);
        ExpectState(false, "", "all three closed");
    }

    // Closing an owner id that opened nothing changes nothing.
    {
        const UINT owner = State::GetOwner();
        State::DisableChecks(owner, "dxgi");
        State::EnableChecks(owner + 1000);
        ExpectState(true, "dxgi", "unknown owner id ignored");
        State::EnableChecks(owner);
        ExpectState(false, "", "closed");
    }

    // The owner id keys the windows, so the ids handed out on different threads at the same time must all differ.
    {
        const int threads = 8;
        const int ids = 50000;
        std::vector<std::vector<UINT>> perThread(threads);
        std::vector<std::thread> workers;

        for (int i = 0; i < threads; ++i)
        {
            workers.emplace_back([&perThread, i, ids] {
                perThread[i].reserve(ids);
                for (int n = 0; n < ids; ++n)
                    perThread[i].push_back(State::GetOwner());
            });
        }

        for (auto& worker : workers)
            worker.join();

        std::set<UINT> all;
        for (const auto& owned : perThread)
            all.insert(owned.begin(), owned.end());

        CHECK(all.size() == (size_t) threads * ids);
        CHECK(!all.empty() && *all.begin() > 0);
    }

    ExpectState(false, "", "end");

    if (g_failed.load() != 0)
    {
        std::printf("skip_window_threads: %d check(s) failed\n", g_failed.load());
        return 1;
    }

    std::puts("skip_window_threads: the window is per thread, unwinds by owner id, and leaves nothing behind");
    return 0;
}
