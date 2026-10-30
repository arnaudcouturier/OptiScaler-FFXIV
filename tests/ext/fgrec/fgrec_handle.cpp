// The session file has one handle. It used to be published once at start, so after the first 16 MiB rotation
// the stop path, the exit takeover and the fault path all acted on a closed handle value: the real file never
// got its tail or session.end, and whatever the system had already given that value to was written into,
// truncated or closed. Windows reuses a freed handle value for the next open, which is why a test on real files
// can pass on the buggy code: this test replaces the file calls with a layer that gives every open a fresh id
// and counts any write, flush, seek or close of a closed one, so the stale handle cannot hide.
//
// Case 1 rotates a real session past 16 MiB through the public path, then checks that the published handle is
// the live one, that the final file ends with session.end, and that no closed handle was ever touched.
// Case 2 kills the worker inside a rotation (between the rename steps, where a process exit can land) and
// shuts down: with the published handle emptied for the length of the rotation, the takeover must skip the
// unfinished file instead of writing into the handle the rotation closed.
// Case 3 hands the recorder a dead-writer state the way process exit leaves it (a partial tail line, records
// still queued) and checks that the takeover finishes the live file and appends session.end. It runs with the
// heap counted: the takeover happens inside DllMain, where a terminated worker may have died holding the heap
// lock, so the first allocation there could hang the game for good.
// The layer itself is shared with the test thread, so it takes one lock in every entry point: case 4 proves a
// read waits for a holder, and pins the lock in each entry point.
#include "fgrec_json.h"

#define FGREC_FAKE_FILE_LAYER
#include "fgrec_fakefile.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace fg = ext::fgrec;
namespace ff = fgrecfake;
namespace ft = fgrec_test;

static std::atomic<long long> g_news { 0 };
static std::atomic<bool> g_countHeap { false };

void* operator new(size_t n)
{
    if (g_countHeap.load(std::memory_order_relaxed))
        g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n)
{
    if (g_countHeap.load(std::memory_order_relaxed))
        g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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

static HANDLE WorkerHandle()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().thread;
}

// Forget everything a killed session left behind, so the next case starts from an idle recorder.
static void ResetState()
{
    fg::State& s = fg::Global();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.out = INVALID_HANDLE_VALUE;
        s.running = false;
        s.thread = nullptr;
        s.queue.clear();
        s.drained.clear();
        s.next = 0;
        s.dropped = 0;
    }
    s.enabled.store(false, std::memory_order_relaxed);
    s.stopRequested.store(false, std::memory_order_relaxed);
    s.alive.store(false, std::memory_order_relaxed);
    s.failed.store(false, std::memory_order_relaxed);
    s.error.store(0, std::memory_order_relaxed);
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

static bool QueueEmpty()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().queue.empty();
}

static HANDLE PublishedHandle()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().out;
}

static void StartSession(const std::wstring& dir)
{
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
}

static std::wstring SessionBase()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().file; // the name EnsureStarted fixed for this session
}

// The rotated base: after a rotation the newest base file opens with the continuation line.
static bool Rotated(const std::wstring& base)
{
    return ff::Contents(base).rfind("{\"kind\":\"continuation\"", 0) == 0;
}

static void RotationRepublishesTheHandle()
{
    const std::wstring dir = ft::FreshDir("handle");
    StartSession(dir);
    const std::wstring base = SessionBase();
    CHECK(!base.empty());
    if (base.empty())
    {
        fg::EnsureStarted(false, fg::SessionInfo {});
        ft::WaitUntil(WorkerGone, 30000);
        return;
    }

    // Past the 16 MiB limit through the public path, paced so nothing drops: the
    // rotation itself is the recorder's own.
    const std::string big(95, 'x');
    uint64_t emitted = 0;
    for (int chunk = 0; chunk < 40 && !Rotated(base); ++chunk)
    {
        for (int i = 0; i < 4000; ++i)
            fg::Emit(fg::kSleep, ++emitted, static_cast<int64_t>(emitted), 0, 0, big.c_str());
        for (int wait = 0; wait < 5000 && !QueueEmpty(); ++wait)
            Sleep(1);
    }
    CHECK(Rotated(base)); // the limit was crossed and the rotation succeeded
    CHECK(ff::Exists(base + L".1"));
    for (int wait = 0; wait < 5000 && !QueueEmpty(); ++wait)
        Sleep(1);

    // The published handle is the live one: not the value the rotation closed.
    CHECK(ff::LiveHandle(base) != nullptr);
    CHECK(PublishedHandle() == ff::LiveHandle(base));
    CHECK(ff::OpenHandles(base) == 1);

    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 60000));

    const std::string bytes = ff::Contents(base);
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    CHECK(!lines.empty());
    bool allStrict = true;
    for (const auto& line : lines)
    {
        bool ok = false;
        ft::ParseLine(line, ok);
        allStrict = allStrict && ok;
    }
    CHECK(allStrict);
    if (!lines.empty())
    {
        bool ok = false;
        const ft::JsonValue last = ft::ParseLine(lines.back(), ok);
        CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end")); // the tail landed on the live file
    }
    // The pre-rotation file kept its whole content.
    const std::vector<std::string> old = ft::SplitLines(ff::Contents(base + L".1"));
    CHECK(old.size() > 1000);
    if (!old.empty())
    {
        bool ok = false;
        const ft::JsonValue first = ft::ParseLine(old.front(), ok);
        CHECK(ok && ft::IsText(ft::Find(first, "kind"), "session.begin"));
    }
    std::printf("writes through the layer: %llu, violations: %d\n", static_cast<unsigned long long>(ff::Writes()),
                ff::Violations());
    CHECK(ff::Violations() == 0); // nothing ever wrote, flushed, seeked or closed a closed handle
    CHECK(PublishedHandle() == INVALID_HANDLE_VALUE); // a finished session owns no handle
}

static void TakeoverFinishesTheLiveFile()
{
    // The state process exit leaves: the writer is gone, the file holds a partial
    // line, and records are still queued. Hand it over and the takeover must cut
    // the partial line, write the queue and append session.end, from stack buffers.
    const std::wstring dir = ft::FreshDir("takeover");
    const std::wstring file = dir + L"\\fg-rec-handover.jsonl";
    ff::Reserve(file, 1u << 20); // the layer's own growth must not pollute the heap count
    HANDLE mine = ff::Open(file);
    CHECK(mine != INVALID_HANDLE_VALUE);
    ff::Write(mine, "{\"schema\":1,\"kind\":\"session.begin\",\"qpc_frequency\":1000}\n{\"kind\":\"sleep\",\"tr");
    fg::State& s = fg::Global();
    s.queue.allocate(fg::kQueueCap); // what EnsureStarted preallocates at start
    s.drained.reserve(fg::kQueueCap);
    s.enabled.store(true, std::memory_order_relaxed);
    s.stopRequested.store(false, std::memory_order_relaxed);
    const int queued = 40;
    for (int i = 1; i <= queued; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "queued");
    CHECK(s.queue.size() == static_cast<size_t>(queued));
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.out = mine; // the worker's live handle
        s.running = false; // and the worker is gone
    }
    g_news.store(0, std::memory_order_relaxed);
    g_countHeap.store(true, std::memory_order_relaxed);
    fg::Shutdown(false); // must not allocate: this runs inside DllMain
    g_countHeap.store(false, std::memory_order_relaxed);
    const long long news = g_news.load(std::memory_order_relaxed);
    std::printf("heap allocations inside the takeover: %lld\n", news);
    CHECK(news == 0);

    const std::string bytes = ff::Contents(file);
    CHECK(bytes.find("\"tr") == std::string::npos); // the partial tail line is gone
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    int records = 0;
    bool allStrict = true;
    for (const auto& line : lines)
    {
        bool ok = false;
        const ft::JsonValue v = ft::ParseLine(line, ok);
        allStrict = allStrict && ok;
        if (ft::Find(v, "seq") != nullptr)
            ++records;
    }
    CHECK(allStrict);
    CHECK(records == queued); // every queued record reached the file, once
    if (!lines.empty())
    {
        bool ok = false;
        const ft::JsonValue last = ft::ParseLine(lines.back(), ok);
        CHECK(ok && ft::IsText(ft::Find(last, "kind"), "session.end"));
    }
    CHECK(PublishedHandle() == INVALID_HANDLE_VALUE);
    CHECK(ff::OpenHandles(file) == 0); // the handle was closed with the file finished
    CHECK(ff::Violations() == 0);
    s.enabled.store(false, std::memory_order_relaxed);
    s.queue.clear();
    s.next = 0;
    s.dropped = 0;
    s.drained.clear();
}

static void TakeoverShapePin(const std::string& repo)
{
    // The takeover path formats out of fixed buffers and reads its tail from the
    // buffer preallocated at start: no std::string, no std::vector and no reserve
    // in the block that finishes the file.
    const std::string text = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    const size_t at = text.rfind("s.queue.drain(s.drained);"); // the takeover's: the loop drains earlier
    CHECK(at != std::string::npos);
    const std::string body = text.substr(at, 900);
    CHECK(body.find("TruncatePartialLine") != std::string::npos);
    CHECK(body.find("WriteBatchFixed") != std::string::npos);
    CHECK(body.find("kEndLine") != std::string::npos);
    CHECK(body.find("std::string") == std::string::npos);
    CHECK(body.find("std::vector") == std::string::npos);
    CHECK(body.find("reserve") == std::string::npos);
    CHECK(body.find("new ") == std::string::npos);
    // The buffers it relies on are fixed or preallocated.
    CHECK(text.find("char buf[4096] = {};") != std::string::npos); // the partial-line cut
    CHECK(text.find("char store[kTakeoverChunk] = {};") != std::string::npos); // the batch write
    CHECK(text.find("s.drained.reserve(kQueueCap);") != std::string::npos); // allocated at start, not at exit
}

static void RotationKilledMidWayLeavesNoHandle()
{
    // A rotation closes the session file, renames it, opens a new one and flushes that, so for a few
    // milliseconds the session has no open file. If the process exits inside that window (the OS terminates the
    // writer first), a takeover that still names the closed handle writes JSON into, truncates or closes whatever
    // the system has already handed that value to. The file is left unfinished instead: the analyzer reports it.
    const std::wstring dir = ft::FreshDir("rotkill");
    StartSession(dir);
    const std::wstring base = SessionBase();
    CHECK(!base.empty());
    if (base.empty())
    {
        fg::EnsureStarted(false, fg::SessionInfo {});
        ft::WaitUntil(WorkerGone, 30000);
        return;
    }
    const HANDLE worker = WorkerHandle();
    CHECK(worker != nullptr);
    ff::ArmThreadKill(worker); // the next rename kills the writer where it stands, inside the rotation

    const std::string big(95, 'x');
    uint64_t emitted = 0;
    for (int chunk = 0; chunk < 80 && !ff::Killed(); ++chunk)
    {
        for (int i = 0; i < 4000; ++i)
            fg::Emit(fg::kSleep, ++emitted, static_cast<int64_t>(emitted), 0, 0, big.c_str());
        for (int wait = 0; wait < 200 && !ff::Killed(); ++wait)
            Sleep(1);
    }
    CHECK(ff::Killed()); // the writer died between the renames, so the rotation never reopened the base
    std::printf("records emitted before the kill: %llu\n", static_cast<unsigned long long>(emitted));

    fg::Shutdown(false); // what the detach path runs at process exit
    std::printf("violations after a kill inside a rotation: %d (%s)\n", ff::Violations(),
                ff::ViolationsText().c_str());
    CHECK(ff::Violations() == 0); // nothing wrote, flushed, sought, truncated or closed a released handle
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        CHECK(fg::Global().out == INVALID_HANDLE_VALUE); // the session owns no handle while it rotates
    }
    ResetState();
}

// The layer is shared: the test thread reads contents and handles while the worker writes, so the layer's
// state must be serialized. A read from another thread waits while the layer is held.
static void LayerSerializesReads()
{
    ff::Reset();
    const std::wstring path = L"C:\\layer\\probe.jsonl";
    const HANDLE mine = ff::Open(path);
    CHECK(mine != INVALID_HANDLE_VALUE);
    ff::Write(mine, "one");
    ff::FakeCloseHandle(mine);

    std::atomic<bool> done { false };
    std::string seen;
    std::unique_lock<std::mutex> held(ff::LayerLock());
    std::thread reader([&]() {
        seen = ff::Contents(path);
        done.store(true, std::memory_order_release);
    });
    Sleep(100);
    CHECK(!done.load(std::memory_order_acquire)); // the read cannot copy a map node or a string mid-write
    held.unlock();
    for (int wait = 0; wait < 5000 && !done.load(std::memory_order_acquire); ++wait)
        Sleep(1);
    CHECK(done.load(std::memory_order_acquire));
    reader.join();
    CHECK(seen == "one");
}

// Every entry point of the stand-in takes the layer's lock, not just the two a test happens to drive.
static void LayerLockPin(const std::string& repo)
{
    const std::string header = ReadSource(repo, "/tests/ext/fgrec/fgrec_fakefile.h");
    CHECK(header.find("std::mutex mutex;") != std::string::npos);
    int locks = 0;
    for (size_t at = 0;
         (at = header.find("lock_guard<std::mutex> guard(LayerLock());", at)) != std::string::npos; ++at)
        ++locks;
    std::printf("layer entry points that hold the lock: %d\n", locks);
    CHECK(locks == 20); // Reset, the getters and every Fake*, Contents and the handle queries
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string repo = argc > 1 ? argv[1] : ".";
    TakeoverShapePin(repo);
    LayerLockPin(repo);
    LayerSerializesReads();
    RotationRepublishesTheHandle();
    RotationKilledMidWayLeavesNoHandle();
    TakeoverFinishesTheLiveFile();
    if (g_failed != 0)
    {
        std::printf("fgrec_handle: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_handle: all passed\n");
    return 0;
}
