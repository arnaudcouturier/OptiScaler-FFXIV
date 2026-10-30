// Key off means off: with the key never turned on, every tap is one relaxed
// atomic load (no thread, no allocation, no file), and turning the key off
// again after a session drains the queue, writes session.end and stops the
// worker by itself. A tap that allocates, starts the worker or writes while
// the key is off fails here; so does a stop that strands records: a record
// accepted after the key went off belongs to no session at all.
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


// True worker exit: Running() gates on the key, so a test that just stopped
// the recorder must watch the flag itself (see fgrec_record.cpp).
static bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

static bool QueueEmpty()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return fg::Global().queue.empty();
}

static void KeyOffTakesNoPath()
{
    // Emit's first two reads are the key-off gate: no record is built, no
    // thread starts and no file appears while the key stays off.
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(!fg::Running());
    for (int i = 0; i < 100000; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "off");
    CHECK(!fg::Running());
    CHECK(fg::Error() == 0);
    CHECK(fg::Global().queue.empty() && fg::Global().dropped == 0);
}

static void KeyOffAfterSessionDrains()
{
    const std::wstring dir = ft::FreshDir("keyoff");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    for (int i = 0; i < 100; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "on");
    fg::EnsureStarted(false, fg::SessionInfo {}); // the worker drains, ends and exits by itself
    CHECK(ft::WaitUntil(WorkerGone, 30000));

    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE)
        return;
    const std::wstring session = dir + L"\\fg-recordings\\" + f.cFileName;
    FindClose(h);
    CHECK(ft::WaitForSessionEnd(session, 30000)); // the flag clears before the last write lands
    const std::string bytes = ft::ReadBytes(session);
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    CHECK(lines.size() == 102); // begin + 100 events + end, nothing stranded
    bool ok = false;
    ft::ParseLine(lines.front(), ok);
    CHECK(ok);
    ft::ParseLine(lines.back(), ok);
    CHECK(ok);
}

static void KeyOffGateOrder(const std::string& repo)
{
    // The key-off gate runs before any record is built: no allocation, no
    // clock read, no lock on the off path.
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
    const size_t emit = text.find("void Emit(");
    CHECK(emit != std::string::npos);
    const size_t gate = text.find("if (!s.enabled.load(std::memory_order_relaxed))", emit);
    const size_t built = text.find("Record r;", emit);
    CHECK(gate != std::string::npos && built != std::string::npos && gate < built);
}

static void StopWritesItsOwnTail(const std::string& repo)
{
    // The stop path writes the records it drained into this worker's own buffer: once the flags say the worker
    // is gone a new session may start and reuse State::drained for its loop, so the old worker must not be
    // reading that vector after the unlock.
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
    CHECK(text.find("tail.swap(s.drained);") != std::string::npos);
    CHECK(text.find("WriteBatch(mine, tail, tailLost, ignored);") != std::string::npos);
}

// A record accepted after the key went off belongs to no session: the old worker stops after its final
// drain, so a record queued once that drain has run is cleared by the next session's start without a trace,
// and one queued before it lands in a file that has already ended. Measured here in the shape that reaches
// further: the key goes off, then on again at once (the old worker is still finishing, so the start returns),
// and the tap of that key-on is the record that would be stranded. The ring and the old session's file must
// both stay clear of it.
static void KeyOffRejectsTheTapsOfTheStop()
{
    const std::wstring dir = ft::FreshDir("strand");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());
    for (int i = 0; i < 5; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "before");
    // Let the worker drain and go back to its ~100 ms wait, so the late tap below lands while it sleeps instead
    // of racing its final drain.
    CHECK(ft::WaitUntil(QueueEmpty, 30000));
    std::wstring session;
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        session = fg::Global().file;
    }
    CHECK(!session.empty());

    fg::EnsureStarted(false, fg::SessionInfo {}); // the key goes off
    CHECK(fg::Global().stopRequested.load(std::memory_order_relaxed)); // the stop is requested, the gate is shut
    fg::EnsureStarted(true, info); // the key does not stay off: the worker is still finishing, so this returns
    CHECK(fg::Alive());            // the case is about the old worker's last moment, not about a new session
    fg::Emit(fg::kSleep, 99, 99, 0, 0, "late"); // the tap of that key-on
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        queued = fg::Global().queue.size();
    }
    CHECK(queued == 0); // rejected: nothing is queued for a worker that is on its way out

    fg::EnsureStarted(false, fg::SessionInfo {}); // settle: the worker stops either way
    CHECK(ft::WaitUntil(WorkerGone, 15000));
    CHECK(ft::WaitForSessionEnd(session, 15000));
    const std::string bytes = ft::ReadBytes(session);
    CHECK(bytes.find("\"late\"") == std::string::npos); // the late record never reached the file
    CHECK(ft::SplitLines(bytes).size() == 7); // begin + 5 events + end, nothing else
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    KeyOffGateOrder(repo);
    StopWritesItsOwnTail(repo);
    KeyOffTakesNoPath();
    KeyOffAfterSessionDrains();
    KeyOffRejectsTheTapsOfTheStop();
    if (g_failed != 0)
    {
        std::printf("fgrec_keyoff: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_keyoff: all passed\n");
    return 0;
}
