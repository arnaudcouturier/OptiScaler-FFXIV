// Session begin/end and shutdown: a session opens with a session.begin line
// and closes with session.end on a clean stop, and DLL detach only signals
// the worker (it never waits: the worker may need the loader lock this
// detach holds). Shutdown on a never-started recorder is a no-op, and a
// second shutdown is safe. A missing end marker or a waiting shutdown fails.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <chrono>
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


// True worker exit: Running() gates on the key, so a test that just stopped
// the recorder must watch the flag itself (see fgrec_record.cpp).
static bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

static std::wstring OnlySessionFile(const std::wstring& dir)
{
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    std::wstring file;
    if (h != INVALID_HANDLE_VALUE)
    {
        file = dir + L"\\fg-recordings\\" + f.cFileName;
        FindClose(h);
    }
    return file;
}

static void BeginEndOnCleanStop()
{
    const std::wstring dir = ft::FreshDir("lifecycle");
    fg::SessionInfo info;
    const std::string v = "9.1-test", e = "game_test.exe", b = "matched", sum = "fg=xefg automfg=off";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "reflex");
    fg::Shutdown(true); // detach (FreeLibrary): signal only, never wait
    CHECK(ft::WaitUntil(WorkerGone, 30000));

    const std::wstring file = OnlySessionFile(dir);
    CHECK(!file.empty());
    if (file.empty())
        return;
    const std::vector<std::string> lines = ft::SplitLines(ft::ReadBytes(file));
    CHECK(lines.size() == 3);
    bool ok = false;
    ft::JsonValue first = ft::ParseLine(lines.front(), ok);
    CHECK(ok && ft::IsText(ft::Find(first, "kind"), "session.begin"));
    CHECK(ft::IsText(ft::Find(first, "version"), "9.1-test"));
    ft::JsonValue last = ft::ParseLine(lines.back(), ok);
    CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end"));
    fg::Shutdown(false); // a second shutdown after the stop is safe
    CHECK(!fg::Running());
}

static void ShutdownSignalsAndNeverWaits()
{
    const std::wstring dir = ft::FreshDir("shutdown");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    for (int i = 0; i < 5000; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "queued");
    const auto start = std::chrono::steady_clock::now();
    fg::Shutdown(true); // returns once the flags are set, whatever is queued
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    CHECK(ms.count() < 5000); // signalling, not draining: seconds of queued records still return at once
    CHECK(ft::WaitUntil(WorkerGone, 60000));
    const std::wstring file = OnlySessionFile(dir);
    CHECK(!file.empty());
    if (!file.empty())
    {
        const std::vector<std::string> lines = ft::SplitLines(ft::ReadBytes(file));
        bool ok = false;
        ft::JsonValue last = ft::ParseLine(lines.back(), ok);
        CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end")); // drained first, then ended
    }
}

static void ShutdownWithoutStartIsNoOp()
{
    fg::Shutdown(true);
    fg::Shutdown(false);
    CHECK(!fg::Running());
    CHECK(fg::Error() == 0);
}

static void ShutdownNeverWaitsPin(const std::string& repo)
{
    // Signal-only detach, like the status file: set the flags and the event,
    // close the handle, and never wait on the worker under the loader lock.
    std::string text;
    FILE* f = nullptr;
    const std::string path = repo + "/OptiScaler/ext/fgrec/FgRecorder.cpp";
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return;
    }
    char buf[65536] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        text.append(buf, n);
    fclose(f);
    const size_t at = text.find("void Shutdown(");
    CHECK(at != std::string::npos);
    const std::string body = text.substr(at, 1500);
    CHECK(body.find("SetEvent(s.stopEvent)") != std::string::npos);
    CHECK(body.find("CloseHandle(s.thread)") != std::string::npos);
    CHECK(body.find("WaitForSingleObject") == std::string::npos);
    CHECK(body.find("WaitForMultipleObjects") == std::string::npos);
    CHECK(body.find("sleep_for") == std::string::npos);
    CHECK(body.find("_beginthreadex") == std::string::npos);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    BeginEndOnCleanStop();
    ShutdownSignalsAndNeverWaits();
    ShutdownWithoutStartIsNoOp();
    ShutdownNeverWaitsPin(repo);
    if (g_failed != 0)
    {
        std::printf("fgrec_lifecycle: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_lifecycle: all passed\n");
    return 0;
}
