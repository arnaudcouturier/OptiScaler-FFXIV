// The status file writer: single-owner locking, atomic replaces, the stale
// temp sweep, own-file deletion, the once-per-episode error tally, the
// frame-time series, and the throttled worker cycle. This test builds the
// real StatusFile.cpp (plus the collector and renderer it calls) and drives
// the rules against real temporary folders, real processes and a real clock.
#include "ext/status/StatusFile.h"
#include "ext/status/StatusCollect.cpp" // first: unqualified State below means the upstream state
#include "ext/status/StatusJson.cpp"
#include "ext/status/StatusFile.cpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>

#include <psapi.h>

namespace status = ext::status;
namespace fs = std::filesystem;

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

// The writer state and its helpers live in an anonymous namespace: reachable
// through expressions, not names.
using FileState = std::decay_t<decltype(status::Global())>;
using SeriesT = std::decay_t<decltype(status::Global().base)>;
template <typename F> struct FourthArg;
template <typename R, typename A, typename B, typename C, typename D>
struct FourthArg<R (*)(A, B, C, D&)>
{
    using Type = D;
};
using FileErrorT = typename FourthArg<decltype(&status::TakeOwnership)>::Type;

static fs::path TestRoot;
static HANDLE g_child = nullptr;
static DWORD g_childPid = 0;

static long long UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static void WriteText(const fs::path& file, const std::string& text)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        std::printf("FAIL: cannot write %ls\n", file.c_str());
        ++g_failed;
        return;
    }
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
}

static std::string ReadText(const fs::path& file)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    char buf[65536] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof buf - 1, &read, nullptr);
    CloseHandle(h);
    return std::string(buf, read);
}

// A live foreign process: holds a pid that is neither ours nor dead.
static void SpawnChild()
{
    wchar_t cmd[] = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
    STARTUPINFOW si {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        g_child = pi.hProcess;
        g_childPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
    }
    else
    {
        std::printf("FAIL: cannot spawn helper process\n");
        ++g_failed;
    }
}

static DWORD FindDeadPid(DWORD ownPid)
{
    for (DWORD p = ownPid + 10000; p < ownPid + 60000; ++p)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p);
        if (h != nullptr)
        {
            CloseHandle(h);
            continue;
        }
        if (GetLastError() != ERROR_ACCESS_DENIED)
            return p; // no such process
    }
    return 0;
}

static DWORD FindDeniedPid(DWORD ownPid)
{
    DWORD ids[4096] = {};
    DWORD bytes = 0;
    if (!EnumProcesses(ids, sizeof ids, &bytes))
        return 0;
    for (DWORD i = 0; i < bytes / sizeof(DWORD); ++i)
    {
        if (ids[i] == 0 || ids[i] == ownPid)
            continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
        if (h != nullptr)
        {
            CloseHandle(h);
            continue;
        }
        if (GetLastError() == ERROR_ACCESS_DENIED)
            return ids[i]; // alive, but not ours to query
    }
    return 0;
}

// ---- frame-time series: exact recent percentiles, sampled session ones ----

static void CheckSeries()
{
    SeriesT empty;
    CHECK(empty.Recent().frames == 0 && empty.Recent().p50Ms == 0.0);
    CHECK(empty.Session().frames == 0 && empty.Session().meanMs == 0.0);

    SeriesT one;
    one.Push(5.0);
    CHECK(one.Recent().frames == 1 && one.Recent().p50Ms == 5.0 && one.Recent().maxMs == 5.0);
    CHECK(one.Session().frames == 1 && one.Session().meanMs == 5.0 && one.Session().p99Ms == 5.0);

    SeriesT hundred; // known set: interpolation between close ranks is exact
    for (double v = 1.0; v <= 100.0; v += 1.0)
        hundred.Push(v);
    CHECK(hundred.Recent().frames == 100);
    CHECK(hundred.Recent().p50Ms == 50.5);
    CHECK(hundred.Recent().meanMs == 50.5 && hundred.Recent().maxMs == 100.0);
    CHECK(hundred.Session().meanMs == 50.5 && hundred.Session().maxMs == 100.0);

    SeriesT ring; // the recent window holds the last 180 intervals only
    for (double v = 1.0; v <= 200.0; v += 1.0)
        ring.Push(v);
    CHECK(ring.frames == 200 && ring.recentCount == 180);
    CHECK(ring.Recent().frames == 200); // the counter is session-wide, the window is not
    CHECK(ring.Recent().meanMs == 110.5); // mean of 21..200
    CHECK(ring.Recent().p50Ms == 110.5);
    CHECK(ring.Recent().maxMs == 200.0);
    CHECK(ring.Session().frames == 200 && ring.Session().meanMs == 100.5);

    SeriesT session; // the reservoir holds 1024 samples of the whole session
    for (double v = 1.0; v <= 2000.0; v += 1.0)
        session.Push(v);
    CHECK(session.frames == 2000 && session.reservoirCount == 1024);
    CHECK(session.Session().meanMs == 1000.5 && session.Session().maxMs == 2000.0);
    const double p50 = session.Session().p50Ms; // sampled: near the middle, never exact
    CHECK(p50 > 850.0 && p50 < 1150.0);
}

// The session id is 16 lowercase hex characters: random per process, short
// enough to stay out of every cap.
static void CheckSessionId()
{
    const std::string id = status::MakeSessionId(1234);
    CHECK(id.size() == 16);
    for (const char c : id)
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    CHECK(id != "0000000000000000");
}

// The change hash is deterministic: same state, same hash.
static void CheckHash()
{
    CHECK(status::HashOf("abc") == status::HashOf("abc"));
    CHECK(status::HashOf("abc") != status::HashOf("abd"));
}

// ---- ownership: one file, one live owner, the dead and stale give way ----

static void CheckTakeOwnership()
{
    const fs::path dir = TestRoot / L"locks";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const DWORD own = GetCurrentProcessId();
    const std::wstring wdir = dir.wstring();

    FileErrorT err{}; // missing lock: ours
    CHECK(status::TakeOwnership(wdir, own, 1, err));
    const std::string lock = ReadText(dir / L"status.lock");
    CHECK(lock.rfind(std::to_string(own) + " 1 ", 0) == 0); // "pid seq time"

    err = FileErrorT{}; // our own lock: still ours, never self-blocking
    CHECK(status::TakeOwnership(wdir, own, 2, err));
    CHECK(err.step[0] == '\0');

    CHECK(g_childPid != 0); // a live foreign owner blocks, quietly
    WriteText(dir / L"status.lock", std::to_string(g_childPid) + " 9 " + std::to_string(UnixNow()));
    err = FileErrorT{};
    CHECK(!status::TakeOwnership(wdir, own, 3, err));
    CHECK(err.step[0] == '\0'); // no error, no backoff: retried next cycle

    WriteText(dir / L"status.lock", "not a lock file"); // an unreadable lock is taken over
    err = FileErrorT{};
    CHECK(status::TakeOwnership(wdir, own, 4, err));
    CHECK(err.step[0] == '\0');

    const DWORD dead = FindDeadPid(own); // a dead pid is taken over, fresh stamp or not
    CHECK(dead != 0);
    WriteText(dir / L"status.lock", std::to_string(dead) + " 9 " + std::to_string(UnixNow()));
    err = FileErrorT{};
    CHECK(status::TakeOwnership(wdir, own, 5, err));
    WriteText(dir / L"status.lock", std::to_string(dead) + " 9 " + std::to_string(UnixNow() - 60));
    err = FileErrorT{}; // a stale lock (older than 30 s) is taken over too
    CHECK(status::TakeOwnership(wdir, own, 6, err));

    const DWORD denied = FindDeniedPid(own); // alive but not ours to query: never evicted
    if (denied != 0)
    {
        WriteText(dir / L"status.lock", std::to_string(denied) + " 9 " + std::to_string(UnixNow()));
        err = FileErrorT{};
        CHECK(!status::TakeOwnership(wdir, own, 7, err));
        CHECK(err.step[0] == '\0');
    }
    else
        std::puts("note: no access-denied pid on this machine; the branch is pinned by text");

    err = FileErrorT{}; // an unwritable folder reports the step and the code
    CHECK(!status::TakeOwnership(L"Z:\\no-such-folder-w4\\sub", own, 8, err));
    CHECK(std::string(err.step) == "open" && err.code != 0);
}

// ---- atomic replace: temp plus rename, the old file survives a failure ----

static void CheckWriteAtomically()
{
    const fs::path dir = TestRoot / L"atom";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const DWORD own = GetCurrentProcessId();
    const std::wstring wdir = dir.wstring();

    const auto good = status::WriteAtomically(wdir, own, "{\"a\":1}");
    CHECK(good.ok);
    CHECK(ReadText(dir / L"status.json") == "{\"a\":1}");
    CHECK(!fs::exists(dir / (L"status.tmp-" + std::to_wstring(own) + L".json"), ec)); // no temp left

    const auto badDir = status::WriteAtomically(L"Z:\\no-such-folder-w4\\sub", own, "{}");
    CHECK(!badDir.ok && std::string(badDir.step) == "create" && badDir.code != 0);

    WriteText(dir / L"status.json", "previous"); // a locked destination fails the move
    HANDLE held = CreateFileW((dir / L"status.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const auto locked = status::WriteAtomically(wdir, own, "{\"a\":2}");
    CHECK(!locked.ok && std::string(locked.step) == "move" && locked.code != 0);
    CloseHandle(held);
    CHECK(ReadText(dir / L"status.json") == "previous"); // the previous file is intact
    CHECK(!fs::exists(dir / (L"status.tmp-" + std::to_wstring(own) + L".json"), ec)); // temp cleaned up
}

// ---- sweep: only dead crash leftovers go, ours and the living stay ----

static void CheckSweep()
{
    const fs::path dir = TestRoot / L"sweep";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const DWORD own = GetCurrentProcessId();
    const DWORD dead = FindDeadPid(own);
    CHECK(dead != 0);
    const fs::path gone = dir / (L"status.tmp-" + std::to_wstring(dead) + L".json");
    const fs::path kept = dir / (L"status.tmp-" + std::to_wstring(own) + L".json");
    const fs::path live =
        g_childPid != 0 ? dir / (L"status.tmp-" + std::to_wstring(g_childPid) + L".json") : fs::path();
    const fs::path odd = dir / L"status.tmp-abc.json";
    const fs::path other = dir / L"status.json";
    const DWORD denied = FindDeniedPid(own);
    const fs::path untouchable =
        denied != 0 ? dir / (L"status.tmp-" + std::to_wstring(denied) + L".json") : fs::path();
    WriteText(gone, "x");
    WriteText(kept, "x");
    if (!live.empty())
        WriteText(live, "x");
    if (!untouchable.empty())
        WriteText(untouchable, "x");
    WriteText(odd, "x");
    WriteText(other, "x");
    status::SweepStaleTemps(dir.wstring(), own);
    CHECK(!fs::exists(gone, ec)); // the dead pid's leftover is gone
    CHECK(fs::exists(kept, ec));  // our own pid is kept
    if (!live.empty())
        CHECK(fs::exists(live, ec)); // a live pid is kept
    if (!untouchable.empty())
        CHECK(fs::exists(untouchable, ec)); // an unqueryable pid counts as alive
    CHECK(fs::exists(odd, ec) && fs::exists(other, ec)); // anything else is untouched
    status::SweepStaleTemps(L"", 0); // empty inputs never crash
    status::SweepStaleTemps(dir.wstring(), 0);
}

// ---- own-file deletion: only ours, only when the lock still names us ----

static void CheckDeleteOwnFile()
{
    const fs::path dir = TestRoot / L"owned";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const DWORD own = GetCurrentProcessId();
    const std::wstring wdir = dir.wstring();

    FileState fresh; // never wrote: nothing of ours to delete
    fresh.dllDir = wdir;
    fresh.pid = own;
    WriteText(dir / L"status.json", "f");
    WriteText(dir / L"status.lock", std::to_string(own) + " 1 " + std::to_string(UnixNow()));
    status::DeleteOwnFile(fresh);
    CHECK(fs::exists(dir / L"status.json", ec));

    fresh.seq = 1; // our lock: both go
    status::DeleteOwnFile(fresh);
    CHECK(!fs::exists(dir / L"status.json", ec) && !fs::exists(dir / L"status.lock", ec));

    WriteText(dir / L"status.json", "f"); // someone else's file now: hands off
    WriteText(dir / L"status.lock", std::to_string(g_childPid != 0 ? g_childPid : own + 1) + " 1 " +
                                        std::to_string(UnixNow()));
    status::DeleteOwnFile(fresh);
    CHECK(fs::exists(dir / L"status.json", ec) && fs::exists(dir / L"status.lock", ec));

    fs::remove(dir / L"status.lock", ec); // no lock to prove ownership: delete nothing
    status::DeleteOwnFile(fresh);
    CHECK(fs::exists(dir / L"status.json", ec));
}

// ---- error tally: counted and logged once per episode, cleared by success ----

static void CheckNoteFileError()
{
    FileState fresh;
    status::NoteFileError(fresh, "write", FileErrorT { "move", 5 });
    {
        std::lock_guard<std::mutex> lock(fresh.mutex);
        CHECK(fresh.errors.count == 1);
        CHECK(fresh.errors.code == "write");
        CHECK(fresh.errors.detail == "move 5"); // operation plus number, never message text
        CHECK(fresh.fileErrorActive);
    }
    status::NoteFileError(fresh, "write", FileErrorT { "move", 5 }); // same episode: not counted again
    {
        std::lock_guard<std::mutex> lock(fresh.mutex);
        CHECK(fresh.errors.count == 1);
    }
}

// ---- worker start: a failed start never retries, a failed pin never starts ----

static void CheckStartWorker()
{
    CHECK(status::PinModule()); // this process's own module pins
    FileState failed;
    failed.threadFailed = true; // an earlier failure: no retry storm on every present
    status::StartWorker(failed);
    CHECK(!failed.threadRunning && failed.thread == nullptr && failed.stopEvent == nullptr);
}

// ---- scope hooks: ignored phases cost nothing, a disabled key starts nothing ----

static void CheckScopeBegin()
{
    using Phase = ext::present::Phase;
    const unsigned before = ext::g_cfgReads;
    status::OnScopeBegin(Phase::HiddenPresent); // ignored: before the key read, the clock and the lock
    CHECK(ext::g_cfgReads == before);

    ext::g_cfg.status.fileEnabled.value = false;
    const unsigned keyed = ext::g_cfgReads;
    status::OnScopeBegin(Phase::NativePresent); // one key read, no timestamp, no lock, no thread
    CHECK(ext::g_cfgReads == keyed + 1);
    auto& global = status::Global();
    CHECK(!global.threadRunning);
    CHECK(global.lastBase == std::chrono::steady_clock::time_point::min());
}

// ---- worker cycle: first write, then at most one write a second, none when off ----

static void CheckWorkerCycle()
{
    const fs::path dir = TestRoot / L"cycle";
    std::error_code ec;
    fs::create_directories(dir, ec);
    FileState st;
    st.startSystem = std::chrono::system_clock::now();
    st.pid = GetCurrentProcessId();
    st.sessionId = "testsession";
    st.dllDir = dir.wstring();
    ext::g_cfg.status.fileEnabled.value = true;

    const auto nowSystem = std::chrono::system_clock::now();
    const auto now = std::chrono::steady_clock::now();
    status::WorkerCycle(st, nowSystem, now); // first cycle: the state is new, so it writes
    CHECK(st.seq == 1);
    const std::string first = ReadText(dir / L"status.json");
    CHECK(first.find("\"format\":\"optiscaler-ffxiv-status\"") != std::string::npos);
    CHECK(first.find("\"seq\":1") != std::string::npos);
    CHECK(fs::exists(dir / L"status.lock", ec));

    status::WorkerCycle(st, nowSystem, now); // same tick: throttled, no second write
    CHECK(st.seq == 1);
    CHECK(ReadText(dir / L"status.json") == first);

    ext::g_cfg.nr.enabled.value = true; // the state changed, but the second has not passed
    status::WorkerCycle(st, nowSystem, now);
    CHECK(st.seq == 1);
    ext::g_cfg.nr.enabled.value = false;

    ext::g_cfg.status.fileEnabled.value = false; // the key is off: writes stop without a restart
    status::WorkerCycle(st, nowSystem, now);
    CHECK(st.seq == 1);
    CHECK(ReadText(dir / L"status.json") == first);
}

// ---- shutdown: signal, never wait; delete only our own file ----

static void CheckShutdown()
{
    auto& global = status::Global(); // last: leaves the stop flag set
    CHECK(!global.stopRequested.load(std::memory_order_relaxed));
    status::OnShutdown(false); // process exit: no signal, nothing else
    CHECK(!global.stopRequested.load(std::memory_order_relaxed));

    const fs::path dir = TestRoot / L"owned";
    const DWORD own = GetCurrentProcessId();
    global.dllDir = dir.wstring();
    global.pid = own;
    global.seq = 1;
    WriteText(dir / L"status.json", "f");
    WriteText(dir / L"status.lock", std::to_string(own) + " 1 " + std::to_string(UnixNow()));
    const auto before = std::chrono::steady_clock::now();
    status::OnShutdown(true); // FreeLibrary: signal and delete ours, never wait
    const auto took = std::chrono::steady_clock::now() - before;
    CHECK(global.stopRequested.load(std::memory_order_relaxed));
    CHECK(!global.threadRunning);
    CHECK(took < std::chrono::seconds(1)); // returns at once even with the worker mid-cycle
    std::error_code ec;
    CHECK(!fs::exists(dir / L"status.json", ec) && !fs::exists(dir / L"status.lock", ec));

    WriteText(dir / L"status.json", "f"); // a game exit deletes our own file too
    WriteText(dir / L"status.lock", std::to_string(own) + " 1 " + std::to_string(UnixNow()));
    status::OnShutdown(false);
    CHECK(!fs::exists(dir / L"status.json", ec) && !fs::exists(dir / L"status.lock", ec));
}

int main()
{
    std::error_code ec;
    TestRoot = fs::temp_directory_path(ec) / L"status-w4-file";
    fs::remove_all(TestRoot, ec);
    fs::create_directories(TestRoot / L"game", ec);
    Util::g_exePath = TestRoot / L"game" / L"ffxiv_dx11.exe"; // the collector freezes these on first use
    Config::Instance()->MainDllPath = (TestRoot / L"game" / L"OptiScaler").wstring();
    SpawnChild();
    if (g_failed != 0)
        return 1;

    CheckSeries();
    CheckSessionId();
    CheckHash();
    CheckTakeOwnership();
    CheckWriteAtomically();
    CheckSweep();
    CheckDeleteOwnFile();
    CheckNoteFileError();
    CheckStartWorker();
    CheckScopeBegin();
    CheckWorkerCycle();
    CheckShutdown(); // last: sets the shared stop flag

    if (g_child != nullptr)
    {
        TerminateProcess(g_child, 0);
        CloseHandle(g_child);
    }
    fs::remove_all(TestRoot, ec);
    if (g_failed == 0)
        std::puts("PASS: status writer owns the file singly, replaces atomically, sweeps, throttles and "
                  "shuts down cleanly");
    return g_failed == 0 ? 0 : 1;
}

