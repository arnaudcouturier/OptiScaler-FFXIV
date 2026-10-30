// M7: process exit finishes the file. At ExitProcess the writer thread is
// already dead, so the old signal-only Shutdown left the session file without
// its queued records and without session.end: the normal quit looked
// unfinished. The fix takes over when the writer is provably gone: it writes
// what is queued plus the terminal record synchronously through the open
// handle (bounded by the queue cap, waiting on nobody), cutting a partial
// tail line first so the file stays strict JSON. This test kills the worker
// the way the OS does at process exit (TerminateThread), then shuts down:
// the file must hold every record plus session.end, written by the calling
// thread with no waiting. A missing end marker or a lost record fails here.
//
// The kill must never catch the worker holding the queue mutex (killing a
// thread blocked on it breaks the owner's later unlock) nor the heap lock.
// The worker is killed fresh off a completed write: the queue is empty, the
// size just moved, and no new emits exist, so no second batch can start and
// the next timeout wake is ~100 ms away. A try_lock after the wait turns a
// mistimed kill into a clean failure instead of a hang.
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

static std::wstring SessionFile(const std::wstring& dir)
{
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    std::wstring file = dir + L"\\fg-recordings\\" + f.cFileName;
    FindClose(h);
    return file;
}

static uint64_t FileSize(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER size {};
    const bool ok = GetFileSizeEx(h, &size) != 0;
    CloseHandle(h);
    return ok && size.QuadPart > 0 ? static_cast<uint64_t>(size.QuadPart) : 0;
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

static void TakeoverPins(const std::string& repo)
{
    // Shutdown never blocks (try_lock, no waiting), and when the writer is
    // already gone the calling thread finishes the file itself: partial tail
    // cut, queue written, terminal record appended.
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    const size_t at = core.find("void Shutdown(");
    CHECK(at != std::string::npos);
    const std::string body = core.substr(at, 2600);
    CHECK(body.find("try_to_lock") != std::string::npos);
    CHECK(body.find("GetExitCodeThread") != std::string::npos);
    CHECK(body.find("TruncatePartialLine") != std::string::npos);
    CHECK(body.find("WriteBatch") != std::string::npos);
    CHECK(body.find("session.end") != std::string::npos);
    CHECK(body.find("WaitForSingleObject") == std::string::npos);
}

static bool QueueEmpty()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().queue.empty();
}

// Waits for the worker to be fresh off a completed write (the queue is empty and the
// file size just moved), then kills it the way the OS does at process exit. Killed
// there, it holds neither the queue mutex nor the heap lock, so the caller can take
// the file over. A mistimed kill turns into a clean failure through the try_lock.
static bool KillFreshWorker(const std::wstring& file)
{
    uint64_t prev = FileSize(file);
    uint64_t lastChange = GetTickCount64();
    bool fresh = false;
    for (int i = 0; i < 500 && !fresh; ++i)
    {
        Sleep(2);
        const uint64_t cur = FileSize(file);
        const uint64_t now = GetTickCount64();
        if (cur != prev)
        {
            prev = cur;
            lastChange = now;
            continue; // still writing: restart the freshness clock
        }
        if (prev != 0 && now - lastChange >= 3 && now - lastChange <= 30 && QueueEmpty())
            fresh = true;
    }
    CHECK(fresh);
    if (!fresh)
        return false;
    HANDLE worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        worker = fg::Global().thread;
    }
    CHECK(worker != nullptr);
    if (worker == nullptr)
        return false;
    if (TerminateThread(worker, 0) == 0) // the OS kills threads first at process exit
    {
        CHECK(false);
        return false;
    }
    // TerminateThread is asynchronous: wait until the thread is really dead, the
    // way it is at process exit, or Shutdown would see it still alive and only signal.
    CHECK(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
    {
        // A mistimed kill abandons the queue mutex: never block on it, fail loud instead.
        std::unique_lock<std::mutex> lock(fg::Global().mutex, std::try_to_lock);
        CHECK(lock.owns_lock());
        if (!lock.owns_lock())
            return false;
    }
    return true;
}

static void DeadWriterIsTakenOver()
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
    CHECK(fg::Running());
    for (uint64_t i = 1; i <= 100; ++i)
        fg::Emit(fg::kSleep, i, 10, 0, 0, "burst");
    // Wait for the drain: the session file exists with content and the queue
    // is empty. Brief observations only, never held across a sleep or a kill.
    std::wstring file;
    for (int i = 0; i < 500 && file.empty(); ++i)
    {
        Sleep(10);
        file = SessionFile(dir);
        if (!file.empty() && (FileSize(file) == 0 || !QueueEmpty()))
            file.clear();
    }
    CHECK(!file.empty());
    if (file.empty() || !KillFreshWorker(file))
        return;
    fg::Shutdown(false); // must finish the file synchronously: no waiting on the dead writer

    const std::string bytes = ft::ReadBytes(file);
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    CHECK(!lines.empty());
    int64_t data = 0;
    bool sawBegin = false;
    bool allStrict = true;
    for (const auto& line : lines)
    {
        bool ok = false;
        ft::JsonValue v = ft::ParseLine(line, ok);
        if (!ok)
        {
            allStrict = false;
            break;
        }
        if (ft::IsText(ft::Find(v, "kind"), "session.begin"))
            sawBegin = true;
        if (ft::Find(v, "seq") != nullptr)
            ++data;
    }
    CHECK(allStrict); // a terminated mid-line write is cut, never left partial
    CHECK(sawBegin);
    std::printf("records after a dead writer: %lld of 100\n", data);
    CHECK(data == 100); // everything queued reaches the file
    if (!lines.empty())
    {
        bool ok = false;
        ft::JsonValue last = ft::ParseLine(lines.back(), ok);
        CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end")); // finished synchronously
    }
}

static void RecordsQueuedAfterTheKillReachTheFile()
{
    // The other half of the takeover: records that were queued when the writer died.
    // They are emitted after the kill, so no writer will ever drain them, and the
    // shutdown must write them in order plus session.end - losing them is exactly
    // the gap the old takeover had.
    const std::wstring dir = ft::FreshDir("queued");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    for (uint64_t i = 1; i <= 20; ++i)
        fg::Emit(fg::kSleep, i, 10, 0, 0, "before");
    std::wstring file;
    for (int i = 0; i < 500 && file.empty(); ++i)
    {
        Sleep(10);
        file = SessionFile(dir);
        if (!file.empty() && (FileSize(file) == 0 || !QueueEmpty()))
            file.clear();
    }
    CHECK(!file.empty());
    if (file.empty() || !KillFreshWorker(file))
        return;
    const int queued = 50;
    for (int i = 0; i < queued; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(100 + i), 20 + i, 0, 0, "after");
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        CHECK(fg::Global().queue.size() == static_cast<size_t>(queued)); // nothing drains them
    }
    fg::Shutdown(false);

    const std::vector<std::string> lines = ft::SplitLines(ft::ReadBytes(file));
    int before = 0, after = 0;
    bool ordered = true;
    long long lastFrame = 99; // the queued records start at frame 100
    bool allStrict = true;
    for (const auto& line : lines)
    {
        bool ok = false;
        const ft::JsonValue v = ft::ParseLine(line, ok);
        allStrict = allStrict && ok;
        const ft::JsonValue* msg = ft::Find(v, "msg");
        const ft::JsonValue* frame = ft::Find(v, "frame");
        if (msg == nullptr || msg->type != ft::JsonValue::Type::Text || frame == nullptr)
            continue;
        const long long n = std::stoll(frame->text);
        if (msg->text == "before")
            ++before;
        else if (msg->text == "after")
        {
            ++after;
            ordered = ordered && n == lastFrame + 1;
            lastFrame = n;
        }
    }
    CHECK(allStrict);
    CHECK(before == 20 && after == queued); // every record, old and queued, once
    CHECK(ordered); // and in emission order
    if (!lines.empty())
    {
        bool ok = false;
        const ft::JsonValue last = ft::ParseLine(lines.back(), ok);
        CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end"));
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: a crash still shows the last phase
    const std::string repo = argc > 1 ? argv[1] : ".";
    TakeoverPins(repo);
    DeadWriterIsTakenOver();
    RecordsQueuedAfterTheKillReachTheFile();
    if (g_failed != 0)
    {
        std::printf("fgrec_shutdown: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_shutdown: all passed\n");
    return 0;
}
