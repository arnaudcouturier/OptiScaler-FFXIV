// H1: the writer sleeps when idle. After the first record the old worker kept
// a manual-reset event signalled forever, so WaitForSingleObject returned at
// once and the loop spun: lock, swap an empty batch, FlushFileBuffers, repeat.
// The fix wakes the worker with an auto-reset event (or the 100 ms timeout)
// and skips the flush when the batch is empty. This test starts a session,
// drains it, then measures process CPU over a 500 ms idle window: a spinning
// writer burns most of a core, a sleeping one burns nothing. A hot idle loop
// or a missing auto-reset/skip-empty pin fails here.
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

static void WakeAndFlushPins(const std::string& repo)
{
    // Auto-reset wake: one tap wakes the worker once, then it sleeps again.
    // Manual-reset (TRUE) is the spin: the event stays signalled forever.
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    CHECK(core.find("CreateEventW(nullptr, FALSE, FALSE, nullptr)") != std::string::npos);
    // No flush on an empty batch: the idle loop writes and flushes nothing, and the
    // flush itself waits for its own cadence (fgrec_flush reads the counters).
    CHECK(core.find("if (!s.drained.empty() || lost != 0)") != std::string::npos);
    CHECK(core.find("now - lastFlush >= kFlushMs") != std::string::npos);
}

static uint64_t ProcessCpuMs()
{
    FILETIME creation {}, exit {}, kernel {}, user {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
        return 0;
    const uint64_t k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const uint64_t u = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return (k + u) / 10000;
}

static void IdleWriterSleeps()
{
    const std::wstring dir = ft::FreshDir("idle");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    for (int i = 0; i < 10; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "warm");
    // wait for the drain: the queue is empty before the idle window starts
    for (int i = 0; i < 300; ++i)
    {
        bool empty = false;
        {
            std::lock_guard<std::mutex> lock(fg::Global().mutex);
            empty = fg::Global().queue.empty();
        }
        if (empty)
            break;
        Sleep(10);
    }
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        CHECK(fg::Global().queue.empty());
    }

    const uint64_t before = ProcessCpuMs();
    Sleep(2000); // idle window: no records, the worker should only time out
    const uint64_t idleMs = ProcessCpuMs() - before;
    std::printf("idle writer CPU over 2000 ms: %llu ms\n", idleMs);
    CHECK(idleMs < 100); // a spinning writer burns a core here (about 500 ms)

    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(fg::Error() == 0);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    WakeAndFlushPins(repo);
    IdleWriterSleeps();
    if (g_failed != 0)
    {
        std::printf("fgrec_idle: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_idle: all passed\n");
    return 0;
}
