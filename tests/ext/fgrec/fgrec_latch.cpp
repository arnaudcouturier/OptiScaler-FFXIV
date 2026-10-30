// H3: no failure latch. A failed start (folder, disk, path, thread) used to
// retry on every tap: each present re-ran CreateDirectoryW, the sweep and
// _beginthreadex on the render thread, and overwrote s.thread without closing
// the old handle. The fix latches the failure until the key goes off and on
// again (one log line, error code sticks) and closes the old thread handle on
// every path. This test starts against an uncreatable folder (a CON component
// fails both the directory and the file), then drives 1000 more starts: one
// attempt leaks nothing measurable, a retry storm leaks ~1000 thread handles.
// A growing handle count or a missing latch pin fails here.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

static DWORD HandleCount()
{
    DWORD n = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &n) == 0)
        return 0;
    return n;
}

static std::string ReadSource(const std::string& repo, const char* rel)
{
    const std::string path = repo + rel;
    std::string text;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return text;
    }
    char buf[65536] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

static void LatchPin(const std::string& repo)
{
    // A sticky failure: once set, further starts return without retrying, and the
    // tap-side check is a single load (never a lock, which a present thread would
    // pay on every tap while the latch is set).
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    CHECK(core.find("s.failed") != std::string::npos);
    CHECK(core.find("if (s.failed.load(std::memory_order_relaxed))") != std::string::npos);
    const size_t at = core.find("bool Failed() noexcept");
    CHECK(at != std::string::npos);
    const std::string body = core.substr(at, 300);
    CHECK(body.find("failed.load(std::memory_order_relaxed)") != std::string::npos);
    CHECK(body.find("mutex") == std::string::npos); // no lock on the tap fast path
    // The key-on edge clears the latch, so a failure that lands after the key went
    // off cannot block the next session.
    CHECK(core.find("if (keyEdge)") != std::string::npos);
}

static void FailedStartRetriesNothing()
{
    const std::wstring base = ft::FreshDir("latch");
    const std::wstring dead = base + L"\\CON"; // reserved name: no directory, no file
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dead.c_str();
    fg::EnsureStarted(true, info);
    CHECK(ft::WaitUntil(WorkerGone, 30000) || !fg::Running());
    CHECK(!fg::Running());
    CHECK(fg::Error() != 0); // the failure sticks an error code for the menu

    const DWORD before = HandleCount();
    for (int i = 0; i < 1000; ++i)
    {
        fg::EnsureStarted(true, info); // latched: no retry from this thread
        // Pace the taps past the worker's failure: a tight loop finishes
        // before the first attempt fails (every later tap then sees a live
        // worker and returns), which would hide the storm. A present thread
        // taps for seconds, so each attempt must settle before the next.
        if (!ft::WaitUntil(WorkerGone, 5000))
            break;
    }
    const DWORD growth = HandleCount() - before;
    std::printf("handle growth over 1000 latched starts: %lu\n", growth);
    CHECK(growth <= 5); // a retry storm leaks a thread handle per attempt
    CHECK(!fg::Running());
    CHECK(fg::Error() != 0); // still latched after 1000 taps

    fg::EnsureStarted(false, fg::SessionInfo {}); // key off clears the latch
    const DWORD beforeRearm = HandleCount();
    fg::EnsureStarted(true, info); // key on: exactly one fresh attempt
    CHECK(ft::WaitUntil(WorkerGone, 30000) || !fg::Running());
    const DWORD rearmGrowth = HandleCount() - beforeRearm;
    std::printf("handle growth over one rearmed start: %lu\n", rearmGrowth);
    CHECK(rearmGrowth <= 3); // one attempt, not a storm
    CHECK(fg::Error() != 0); // the folder is still dead, so it fails again
    fg::EnsureStarted(false, fg::SessionInfo {});
}

// A worker failure that lands after the key went off used to latch past the next key-on: the key-off clear had
// already run (or lost its try_lock), so the first tap of the new session refused to start until the user
// toggled the key twice. The latch is set here exactly as the failure path leaves it - under the mutex, after
// the key-off clear - so the race is reproduced without depending on timing.
static void LatchSetAfterKeyOffRetriesOnce()
{
    const std::wstring base = ft::FreshDir("lateset");
    const std::wstring dead = base + L"\\CON";
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dead.c_str();
    fg::EnsureStarted(true, info); // one attempt against an uncreatable folder
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(fg::Failed());
    fg::EnsureStarted(false, fg::SessionInfo {}); // the key-off clear runs now (or loses its try_lock)
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        fg::Global().failed.store(true, std::memory_order_relaxed); // the worker's late failure
    }
    CHECK(fg::Failed());

    // The next key-on with a folder that works must record: a new session, not a
    // silent refusal from the latch.
    const std::wstring good = ft::FreshDir("lateset-good");
    fg::SessionInfo usable;
    usable.version = v.c_str();
    usable.exe = e.c_str();
    usable.build = b.c_str();
    usable.summary = sum.c_str();
    usable.moduleDir = good.c_str();
    fg::EnsureStarted(true, usable);
    CHECK(fg::Running()); // a latched start would leave nothing running here
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "after-latch");
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(fg::Error() == 0);
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((good + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE)
    {
        const std::wstring session = good + L"\\fg-recordings\\" + f.cFileName;
        FindClose(h);
        CHECK(ft::WaitForSessionEnd(session, 30000));
    }
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    LatchPin(repo);
    FailedStartRetriesNothing();
    LatchSetAfterKeyOffRetriesOnce();
    if (g_failed != 0)
    {
        std::printf("fgrec_latch: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_latch: all passed\n");
    return 0;
}
