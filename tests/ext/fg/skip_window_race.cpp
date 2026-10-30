// Concurrent skip windows: every worker thread opens two windows with its own owner ids, checks the name each one holds,
// and closes them while reader threads check that no window of another thread leaks into them. This is the state two
// threads shared before the fix (one global owner id, one global map, written while the load hook on any other thread read
// it), so the readers' check fails on the old code, and the map operations interleave under the checked containers.
//
// Built with the debug CRT (/MDd, _ITERATOR_DEBUG_LEVEL=2) by run_skip_window_race.ps1: tools\test.ps1 compiles with /MD,
// where the container checks are off. A deadline stops the process (exit 1) if a corrupted map hangs a thread, so the run
// always ends by itself. The assertions read the production code sliced out of OptiScaler\State.h.
#include "StateSkipWindow.h"

#include <windows.h>

#include <atomic>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int kWorkers = 6;
constexpr int kReaders = 2;
constexpr int kRounds = 20000;
constexpr int kNames = 5;
const char* const kWindowNames[kNames] = { "", "dxgi", "sl.", "sl.interposer", "sl.dlss_g" };

std::atomic<int> g_failed { 0 };
std::atomic<int> g_done { 0 };

void Fail(const char* what, int round, const char* wanted, bool skip, const std::string& name)
{
    if (g_failed.fetch_add(1) < 8) // the first few, then only the count
    {
        std::printf("CHECK FAILED %s (round %d, wanted [%s]): SkipDllChecks=%d SkipDllName=[%s]\n", what, round, wanted,
                    skip ? 1 : 0, name.c_str());
    }
}

void Worker(int seed)
{
    for (int round = 0; round < kRounds; ++round)
    {
        const char* first = kWindowNames[(seed + round) % kNames];
        const char* second = kWindowNames[(seed + round + 2) % kNames];
        const UINT outer = State::GetOwner();
        const UINT inner = State::GetOwner();

        State::DisableChecks(outer, first);
        bool skip = State::SkipDllChecks();
        std::string name = State::SkipDllName();
        if (!skip || name != first)
            Fail("the window it just opened", round, first, skip, name);

        State::DisableChecks(inner, second);
        skip = State::SkipDllChecks();
        name = State::SkipDllName();
        if (!skip || name != second)
            Fail("the inner window it just opened", round, second, skip, name);

        // the outer window ends first: the inner one is still open and still holds its own name
        State::EnableChecks(outer);
        skip = State::SkipDllChecks();
        name = State::SkipDllName();
        if (!skip || name != second)
            Fail("the outer window closed first", round, second, skip, name);

        State::EnableChecks(inner);
        skip = State::SkipDllChecks();
        name = State::SkipDllName();
        if (skip || !name.empty())
            Fail("both windows closed", round, "", skip, name);
    }

    g_done.fetch_add(1);
}

// Opens no window of its own: while the workers are running, this thread must never see a skip.
void Reader()
{
    while (g_done.load() < kWorkers)
    {
        const bool skip = State::SkipDllChecks();
        const std::string name = State::SkipDllName();
        if (skip || !name.empty())
        {
            Fail("a window of another thread leaked into a thread that opened none", -1, "", skip, name);
            return;
        }

        std::this_thread::yield();
    }
}
} // namespace

int main()
{
    // No debug-CRT message box may wait for a click: report to the console and carry on.
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);
#endif

    if (State::SkipDllChecks() || !State::SkipDllName().empty())
    {
        std::puts("CHECK FAILED: the state is not empty before any window was opened");
        return 1;
    }

    std::vector<std::thread> threads;
    threads.reserve(kWorkers + kReaders);
    for (int i = 0; i < kWorkers; ++i)
        threads.emplace_back(Worker, i * 3);
    for (int i = 0; i < kReaders; ++i)
        threads.emplace_back(Reader);

    // Deadline: the old code can leave a map operation spinning in a corrupted tree, which no join would ever end.
    const ULONGLONG deadline = GetTickCount64() + 90000;
    while (g_done.load() < kWorkers && GetTickCount64() < deadline)
        Sleep(5);

    if (g_done.load() < kWorkers)
    {
        std::printf("CHECK FAILED: the workers did not finish within 90 s (%d of %d done)\n", g_done.load(), kWorkers);
        std::fflush(stdout);
        std::_Exit(1);
    }

    for (auto& thread : threads)
        thread.join();

    if (State::SkipDllChecks() || !State::SkipDllName().empty())
    {
        std::puts("CHECK FAILED: the state is not empty after every window was closed");
        ++g_failed;
    }

    if (g_failed.load() != 0)
    {
        std::printf("skip_window_race: %d check(s) failed\n", g_failed.load());
        return 1;
    }

    std::puts("skip_window_race: concurrent windows stayed on their own threads");
    return 0;
}
