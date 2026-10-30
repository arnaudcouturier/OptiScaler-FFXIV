// The recorder's queue cap and dropped-records record: the queue never grows
// past its bound (overflow is counted, never silent), the writer reports the
// count in exactly one records_dropped line per drain, and the public burst
// path accounts every event (kept lines plus dropped counts equal the emitted
// total). The first case drives the real Emit accounting with the worker held
// off and then drains through the real WorkerProc; the second fires a burst
// at a live session. A raised cap, a lost counter or a silent drop fails here.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
#include <mutex>
#include <process.h>
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

// The worker entry point, launched here on a helper thread exactly the way
// EnsureStarted does (auto-reset event, below-normal priority): the drain,
// the dropped line and session.end are the product's own code paths.
static void LaunchWorkerForTest()
{
    fg::State& s = fg::Global();
    s.stopEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LARGE_INTEGER freq {};
    QueryPerformanceFrequency(&freq);
    s.qpf = static_cast<uint64_t>(freq.QuadPart);
    s.dir = L"."; // the worker sweeps and creates this before opening the file
    const uintptr_t handle = _beginthreadex(nullptr, 0, &fg::WorkerProc, nullptr, 0, nullptr);
    CHECK(handle != 0);
    if (handle != 0)
    {
        SetThreadPriority(reinterpret_cast<HANDLE>(handle), THREAD_PRIORITY_BELOW_NORMAL);
        s.thread = reinterpret_cast<HANDLE>(handle);
        s.running = true;
    }
}

static void QueueCapAndDroppedLine()
{
    CHECK(fg::kQueueCap == 4096); // the documented bound: overflow is explicit, never silent
    const std::wstring dir = ft::FreshDir("queue");
    fg::State& s = fg::Global();

    s.enabled.store(true, std::memory_order_relaxed); // armed, but no worker: nothing drains
    s.queue.allocate(fg::kQueueCap); // the fixed ring: what EnsureStarted preallocates at start
    CHECK(!s.running);
    const uint64_t total = fg::kQueueCap + 500;
    for (uint64_t i = 0; i < total; ++i)
        fg::Emit(fg::kSleep, i, static_cast<int64_t>(i), 0, 0, "burst");
    CHECK(s.queue.size() == fg::kQueueCap); // bounded: the oldest 500 wait nowhere, they are counted
    CHECK(s.dropped == 500);
    CHECK(s.queue.front().frame == 0 && s.queue.back().frame == fg::kQueueCap - 1);
    CHECK(s.queue.front().seq == 1 && s.queue.back().seq == fg::kQueueCap);

    s.file = dir + L"\\queued.jsonl"; // FreshDir already created dir
    // the worker writes that file; the test reads it back below
    s.utc = "2026-01-01T00:00:00Z";
    s.version = "t";
    s.exe = "t.exe";
    s.build = "t";
    s.summary = "t";
    s.pid = GetCurrentProcessId();
    LaunchWorkerForTest();
    s.enabled.store(false, std::memory_order_relaxed);
    s.stopRequested.store(true, std::memory_order_relaxed);
    SetEvent(s.stopEvent);
    CHECK(WaitForSingleObject(s.thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(s.thread);
    CloseHandle(s.stopEvent);
    s.thread = nullptr;
    s.stopEvent = nullptr;

    const std::string bytes = ft::ReadBytes(s.file);
    CHECK(!bytes.empty());
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    long long kept = 0, dropped = 0;
    long long first = -1, last = -1;
    for (const auto& line : lines)
    {
        bool ok = false;
        ft::JsonValue v = ft::ParseLine(line, ok);
        CHECK(ok);
        const ft::JsonValue* kind = ft::Find(v, "kind");
        if (kind != nullptr && kind->type == ft::JsonValue::Type::Text && kind->text == "records_dropped")
        {
            ++dropped;
            const ft::JsonValue* count = ft::Find(v, "count");
            CHECK(count != nullptr && count->type == ft::JsonValue::Type::Number && count->text == "500");
        }
        const ft::JsonValue* seq = ft::Find(v, "seq");
        if (seq != nullptr)
        {
            ++kept;
            const long long n = std::stoll(seq->text);
            if (first < 0)
                first = n;
            last = n;
        }
    }
    CHECK(kept == static_cast<long long>(fg::kQueueCap));
    CHECK(dropped == 1); // one explicit record for the whole overflow
    CHECK(first == 1 && last == static_cast<long long>(fg::kQueueCap));
}

static void BurstAccountsEveryEvent()
{
    const std::wstring dir = ft::FreshDir("burst");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    const int total = 20000;
    for (int i = 0; i < total; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "live");
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));

    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    long long kept = 0, dropped = 0;
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring path = dir + L"\\fg-recordings\\" + f.cFileName;
            CHECK(ft::WaitForSessionEnd(path, 30000)); // the flag clears before the last write lands
            const std::string bytes = ft::ReadBytes(path);
            for (const auto& line : ft::SplitLines(bytes))
            {
                bool ok = false;
                ft::JsonValue v = ft::ParseLine(line, ok);
                CHECK(ok);
                const ft::JsonValue* kind = ft::Find(v, "kind");
                if (kind != nullptr && kind->type == ft::JsonValue::Type::Text &&
                    kind->text == "records_dropped")
                {
                    const ft::JsonValue* count = ft::Find(v, "count");
                    if (count != nullptr)
                        dropped += std::stoll(count->text);
                }
                else if (ft::Find(v, "seq") != nullptr)
                    ++kept;
            }
        } while (FindNextFileW(h, &f) != 0);
        FindClose(h);
    }
    CHECK(kept > 0);
    CHECK(kept + dropped == total); // every event is either written or counted
}

int main()
{
    QueueCapAndDroppedLine();
    BurstAccountsEveryEvent();
    if (g_failed != 0)
    {
        std::printf("fgrec_queue: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_queue: all passed\n");
    return 0;
}
