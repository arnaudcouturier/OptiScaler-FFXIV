#include "pch.h"

#include "ext/status/StatusFile.h"

#include "ext/core/ExtConfig.h"
#include "ext/status/StatusCollect.h"

#include <State.h>

#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

// Writer lifecycle (docs/notes/status-json.md): session state, per-present cadence ticks, the worker thread and the
// atomic replace. The present thread only takes timestamps and pushes them into fixed-size rings; the worker does the
// collecting, the JSON and the file. Locking: the present path and the worker meet only to copy small structs under
// the mutex (sorting/percentiles happen after, outside it). Shutdown: the state is intentionally leaked (no static
// destructor runs on unmapped code) and the module is pinned while the worker runs (so FreeLibrary never unmaps
// code a late worker may run); detach only signals the worker and deletes our own file, it never waits: the worker
// may need the loader lock (file-version reads, logging), which the detach holds, so a wait could never succeed.
// Every entry point is noexcept: a failure is counted into the file's own error tally and skipped, the game is never
// blocked.
namespace ext::status
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr size_t kRecentFrames = 180; // exact percentiles over the last N intervals
constexpr size_t kReservoirCap = 1024; // session percentiles sampled over this many slots
constexpr double kStaleMs = 60.0 * 1000.0; // a longer gap is a hitch/pause, not a frame time
constexpr auto kFirstWriteDelay = std::chrono::seconds(2);
constexpr auto kHeartbeat = std::chrono::seconds(10);
constexpr auto kMinWriteGap = std::chrono::seconds(1);
constexpr auto kTick = std::chrono::seconds(1);
constexpr auto kLockBackoff = std::chrono::seconds(30); // read-only folder / failed lock: counted once, retried after

double Percentile(std::vector<double> sorted, double q)
{
    if (sorted.empty())
        return 0.0;
    std::sort(sorted.begin(), sorted.end());
    const double rank = q * (static_cast<double>(sorted.size()) - 1.0);
    const size_t lo = static_cast<size_t>(rank);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// One frame-time series. The present path only calls Push: fixed-size rings, no allocation, noexcept. The worker
// copies the whole struct under the lock and computes percentiles after, outside it (Recent/Session may allocate).
struct Series
{
    std::array<double, kRecentFrames> recent{};
    size_t recentCount = 0;
    size_t recentHead = 0; // next slot to overwrite
    std::array<double, kReservoirCap> reservoir{};
    size_t reservoirCount = 0;
    uint64_t reservoirSeen = 0;
    uint64_t frames = 0;
    double sumMs = 0.0;
    double maxMs = 0.0;
    uint64_t lcg = 0x9E3779B97F4A7C15ull; // deterministic replacement, no rand() state shared

    void Push(double ms) noexcept
    {
        ++frames;
        sumMs += ms;
        if (ms > maxMs)
            maxMs = ms;
        recent[recentHead] = ms;
        recentHead = (recentHead + 1) % kRecentFrames;
        if (recentCount < kRecentFrames)
            ++recentCount;
        if (reservoirCount < kReservoirCap)
            reservoir[reservoirCount++] = ms;
        else
        {
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const uint64_t j = (lcg >> 33) % (reservoirSeen + 1);
            if (j < kReservoirCap)
                reservoir[static_cast<size_t>(j)] = ms;
        }
        ++reservoirSeen;
    }

    PerfStats Recent() const
    {
        PerfStats s;
        s.frames = frames;
        if (recentCount == 0)
            return s;
        std::vector<double> sorted;
        sorted.reserve(recentCount);
        for (size_t i = 0; i < recentCount; ++i)
            sorted.push_back(recent[i]); // ring order is irrelevant to percentiles
        s.p50Ms = Percentile(sorted, 0.50);
        s.p95Ms = Percentile(sorted, 0.95);
        s.p99Ms = Percentile(sorted, 0.99);
        s.meanMs = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
        s.maxMs = *std::max_element(sorted.begin(), sorted.end());
        return s;
    }

    PerfStats Session() const
    {
        PerfStats s;
        s.frames = frames;
        if (frames == 0)
            return s;
        s.meanMs = sumMs / static_cast<double>(frames);
        s.maxMs = maxMs;
        std::vector<double> sorted;
        sorted.reserve(reservoirCount);
        for (size_t i = 0; i < reservoirCount; ++i)
            sorted.push_back(reservoir[i]);
        s.p50Ms = Percentile(sorted, 0.50);
        s.p95Ms = Percentile(sorted, 0.95);
        s.p99Ms = Percentile(sorted, 0.99);
        return s;
    }
};

PerfSeries ToSeries(const Series& in)
{
    PerfSeries out;
    out.recent = in.Recent();
    out.session = in.Session();
    return out;
}

struct State
{
    std::mutex mutex; // present-thread ticks and the worker meet only here, briefly, for small copies
    Series base;
    Series output;
    Clock::time_point lastBase = Clock::time_point::min();
    Clock::time_point lastOutput = Clock::time_point::min();
    std::chrono::system_clock::time_point startSystem = std::chrono::system_clock::time_point::min();
    unsigned long pid = 0;
    std::string sessionId;
    std::wstring dllDir; // local only: the file goes here, the directory itself is never emitted
    bool started = false; // Banner ran
    bool threadRunning = false;
    bool threadFailed = false; // _beginthreadex failed: no retry storm on every present
    HANDLE thread = nullptr; // worker handle: signaled then closed at shutdown, never here
    HANDLE stopEvent = nullptr; // manual-reset: set to wake the worker (an event wait, not sleep_for)
    std::atomic<bool> stopRequested { false };
    uint64_t seq = 0;
    uint64_t lastHash = 0; // FNV of the last written StateKey (state fields only, never percentiles/timestamps)
    Clock::time_point lastWrite = Clock::time_point::min();
    Clock::time_point fileRetryAfter = Clock::time_point::min(); // read-only folder / failed lock backoff
    bool fileErrorActive = false; // an error is counted and logged once per episode (cleared by a good write)
    Errors errors;
};

State& Global()
{
    // Intentionally leaked: the worker is stopped at DLL_PROCESS_DETACH (FreeLibrary) while this still exists, and
    // at process exit the OS tears everything down. No destructor ever runs on unmapped code.
    static State& state = *new State();
    return state;
}

bool Enabled() noexcept
{
    try
    {
        return ext::Cfg().status.fileEnabled.value_or_default();
    }
    catch (...)
    {
        return false;
    }
}

std::string UtcString(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm {};
    gmtime_s(&tm, &t);
    char buf[32] = {};
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// The folder of this module (the DLL beside which status.json is written). Found once, from the module handle of a
// function in this file: no caller passes a path in.
std::wstring ThisModuleDir()
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Global), &module) == 0 ||
        module == nullptr)
        return {};
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) == 0)
        return {};
    std::wstring dir = path;
    if (const size_t slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos)
        dir.resize(slash);
    return dir;
}

// Pinned while the worker runs (see StartWorker): FreeLibrary then never unmaps code a late worker may still run,
// so the DLL cannot be unloaded until process exit (intended). Returns false when pinning fails: the caller must
// not start the worker then (the key stays effectively off).
bool PinModule() noexcept
{
    try
    {
        HMODULE pinned = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&Global), &pinned) != 0 &&
            pinned != nullptr)
            return true;
    }
    catch (...)
    {
    }
    return false;
}

uint64_t HashOf(const std::string& s)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a: change detection only, not identity
    for (const unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Random 64-bit session id as 16 hex chars. Mixed from the high-resolution clock, the tick count and the pid
// (splitmix64): the pid alone would repeat every process, and BCryptGenRandom could pull bcrypt.dll under the loader
// lock (Banner runs in the attach path).
std::string MakeSessionId(unsigned long pid)
{
    LARGE_INTEGER qpc {};
    QueryPerformanceCounter(&qpc);
    uint64_t z = static_cast<uint64_t>(qpc.QuadPart) + (static_cast<uint64_t>(pid) << 32) +
                 static_cast<uint64_t>(GetTickCount64()) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; // splitmix64
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    if (z == 0)
        z = 0x9E3779B97F4A7C15ull;
    static constexpr char kHex[] = "0123456789abcdef";
    char id[17] = {};
    for (int i = 0; i < 16; ++i)
        id[i] = kHex[(z >> ((15 - i) * 4)) & 0xF];
    return id;
}

void NoteErrorLocked(State& state, const std::string& code, const std::string& detail)
{
    ++state.errors.count;
    state.errors.code = code;
    state.errors.detail = detail;
}

// A file-step failure: which step and the real error code (reported as "<step> <code>", numbers only).
struct FileError
{
    const char* step = "";
    DWORD code = 0;
};

// Ownership of the single file across two game clients: status.lock holds "pid seq unix-time". The lock is ours
// when it is missing, unreadable, older than 30 s, or its pid is gone -- or when its pid is our own pid (a previous
// cycle of this process): the writer never blocks itself on its own lock. Refreshing it on every write keeps a live
// owner from being evicted. Stale locks (a crashed client) age out on their own. A pid we cannot open for lack
// of rights (ERROR_ACCESS_DENIED) counts as alive, as in the temp sweep: it is never evicted.
bool TakeOwnership(const std::wstring& dir, unsigned long pid, uint64_t seq, FileError& err)
{
    const std::wstring lockPath = dir + L"\\status.lock";
    bool owned = false;
    {
        HANDLE h = CreateFileW(lockPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            owned = true; // missing or unreadable: take it
        else
        {
            char text[64] = {};
            DWORD read = 0;
            const BOOL ok = ReadFile(h, text, sizeof text - 1, &read, nullptr);
            CloseHandle(h);
            unsigned long ownerPid = 0;
            long long ownerTime = 0;
            if (!ok || std::sscanf(text, "%lu %*llu %lld", &ownerPid, &ownerTime) < 1)
                owned = true;
            else if (ownerPid == pid)
                owned = true; // our own lock: we own it
            else
            {
                const long long now =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                        .count();
                HANDLE owner = ownerPid != 0 ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ownerPid) : nullptr;
                const DWORD ownerErr = (owner == nullptr && ownerPid != 0) ? GetLastError() : ERROR_SUCCESS;
                if (owner != nullptr)
                    CloseHandle(owner);
                if (owner == nullptr && ownerErr == ERROR_ACCESS_DENIED)
                    owned = false; // the pid is alive, only not ours to query (as the temp sweep treats it)
                else
                    owned = owner == nullptr || (now - ownerTime) > 30;
            }
        }
    }
    if (!owned)
        return false; // another live client owns the file: stay quiet, retry next cycle (no error, no backoff)
    HANDLE h = CreateFileW(lockPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = { "open", GetLastError() };
        return false;
    }
    const long long now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char text[64] = {};
    const int n = std::snprintf(text, sizeof text, "%lu %llu %lld", pid, static_cast<unsigned long long>(seq), now);
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
    const DWORD code = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok)
    {
        err = { "write", code };
        return false;
    }
    return true;
}

// Atomic replace: the temp file is written, flushed and closed, then renamed over status.json in one call. A reader
// never sees a half-written file; a crash leaves the previous file intact. Returns the failing step and the real
// error code (create / write / flush / move).
struct WriteResult
{
    bool ok = false;
    const char* step = "";
    DWORD code = 0;
};

WriteResult WriteAtomically(const std::wstring& dir, unsigned long pid, const std::string& json)
{
    wchar_t tmp[32] = {};
    std::swprintf(tmp, std::size(tmp), L"status.tmp-%lu.json", pid);
    const std::wstring tmpPath = dir + L"\\" + tmp;
    const std::wstring finalPath = dir + L"\\status.json";
    HANDLE h = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return { false, "create", GetLastError() };
    DWORD written = 0;
    const BOOL wrote = WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    bool ok = wrote && written == json.size();
    const char* step = "";
    DWORD code = 0;
    if (!ok) // no partial file is ever renamed: the temp is deleted below
    {
        step = "write";
        code = wrote ? ERROR_WRITE_FAULT : GetLastError(); // short write: there is no error code to return
    }
    else if (FlushFileBuffers(h) == 0) // the comment says flushed: flush before the rename, and report it
    {
        ok = false;
        step = "flush";
        code = GetLastError();
    }
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(tmpPath.c_str());
        return { false, step, code };
    }
    if (MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        code = GetLastError();
        DeleteFileW(tmpPath.c_str());
        return { false, "move", code };
    }
    return { true, "", 0 };
}

// A failed take or replace: counted and logged once per episode (a read-only folder would otherwise spam both),
// then backed off for 30 s. Cleared by a good write.
void NoteFileError(State& state, const char* code, const FileError& err)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.fileErrorActive)
    {
        state.fileErrorActive = true;
        char detail[48] = {};
        std::snprintf(detail, sizeof detail, "%s %lu", err.step, err.code); // operation + numeric error, never text
        NoteErrorLocked(state, code, detail);
        try
        {
            LOG_WARN("status.json: {} failed ({}), backing off 30 s", code, detail);
        }
        catch (...)
        {
        }
    }
    state.fileRetryAfter = Clock::now() + kLockBackoff;
}

// Event wait that a shutdown can interrupt (the old sleep_for could not). Falls back to a sleep + flag poll when
// the event was never created.
bool WaitStop(State& state, DWORD millis)
{
    if (state.stopEvent != nullptr)
        return WaitForSingleObject(state.stopEvent, millis) != WAIT_TIMEOUT;
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    return state.stopRequested.load(std::memory_order_relaxed);
}

// One worker cycle: snapshot, render, and write when the state changed (at most once a second) or the heartbeat is
// due. The key is re-read every cycle, so turning it off stops the writes without a restart. Writes are gated by the
// state hash (StateKey): percentiles and timestamps alone never cause a write. File and version reads happen only on
// a write-due tick (refreshIo), so idle ticks touch no disk beyond the lock check of a due write.
void WorkerCycle(State& state, const std::chrono::system_clock::time_point& nowSystem, const Clock::time_point& now)
{
    if (!Enabled())
        return;
    if (state.stopRequested.load(std::memory_order_relaxed))
        return;
    if (now < state.fileRetryAfter)
        return; // backed off after a failed take/replace (worker-only field, no lock needed)

    Series baseCopy, outputCopy;
    bool haveOutput = false;
    uint64_t seq = 0;
    std::string sessionId;
    unsigned long pid = 0;
    std::string startUtc;
    std::chrono::system_clock::time_point startSystem = std::chrono::system_clock::time_point::min();
    Errors errors;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        baseCopy = state.base; // copies under the lock; percentiles are computed after, outside it
        outputCopy = state.output;
        haveOutput = state.output.frames > 0;
        seq = state.seq;
        sessionId = state.sessionId;
        pid = state.pid;
        startSystem = state.startSystem;
        errors = state.errors;
    }
    if (startSystem == std::chrono::system_clock::time_point::min())
        return; // no Banner yet: nothing to stamp the snapshot with

    const PerfSeries base = ToSeries(baseCopy);
    const PerfSeries output = ToSeries(outputCopy);

    const uint64_t uptime =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(nowSystem - startSystem).count());

    Snapshot snap;
    try
    {
        snap = Collect(seq + 1, sessionId, pid, UtcString(startSystem), base, output, haveOutput, errors,
                       /*refreshIo=*/false); // change test: cached file/version reads, no disk
    }
    catch (...)
    {
        return; // Collect is already guarded per block; this is a backstop that skips one cycle
    }
    snap.uptimeSec = uptime;
    snap.writtenUtc = UtcString(nowSystem);

    const bool due = state.lastWrite == Clock::time_point::min() || now - state.lastWrite >= kHeartbeat;
    const bool gapOk = state.lastWrite == Clock::time_point::min() || now - state.lastWrite >= kMinWriteGap;
    const uint64_t key = HashOf(StateKey(snap));
    const bool changed = key != state.lastHash;
    if ((!changed && !due) || !gapOk)
        return;
    if (state.stopRequested.load(std::memory_order_relaxed))
        return; // detached mid-cycle: never recreate the file after stop
    FileError lockErr;
    if (!TakeOwnership(state.dllDir, pid, seq + 1, lockErr))
    {
        if (lockErr.step[0] != '\0')
            NoteFileError(state, "lock", lockErr); // counted + logged once, backs off 30 s
        return; // another client owns the file: stay quiet, retry next cycle
    }
    try
    {
        snap = Collect(seq + 1, sessionId, pid, UtcString(startSystem), base, output, haveOutput, errors,
                       /*refreshIo=*/true); // a write is due: refresh the file/version reads first
    }
    catch (...)
    {
        return;
    }
    snap.uptimeSec = uptime;
    snap.writtenUtc = UtcString(nowSystem);
    const uint64_t freshKey = HashOf(StateKey(snap));
    if (freshKey != key && !due && freshKey == state.lastHash)
        return; // refreshed reads match the last write and no heartbeat is due
    const std::string json = ToJson(snap);
    if (state.stopRequested.load(std::memory_order_relaxed))
        return; // detached while rendering: the file is already gone, do not recreate it
    const WriteResult wr = WriteAtomically(state.dllDir, pid, json);
    if (!wr.ok)
    {
        NoteFileError(state, "write", { wr.step, wr.code }); // counted + logged once, backs off 30 s
        return;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.seq;
    state.lastHash = freshKey;
    state.lastWrite = now;
    state.fileErrorActive = false; // the episode is over: the next failure counts and logs again
    state.fileRetryAfter = Clock::time_point::min();
}

unsigned __stdcall WorkerProc(void*)
{
    try
    {
        State& state = Global();
        {
            // One-off sweep on the worker (never in DllMain): delete status.tmp-<pid>.json of dead processes.
            std::wstring dir;
            unsigned long pid = 0;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                dir = state.dllDir;
                pid = state.pid;
            }
            SweepStaleTemps(dir, pid);
        }
        if (WaitStop(state, static_cast<DWORD>(kFirstWriteDelay.count() * 1000)))
            return 0; // shutdown during the first-write delay: exit at once
        for (;;)
        {
            try
            {
                WorkerCycle(state, std::chrono::system_clock::now(), Clock::now());
            }
            catch (...)
            {
            }
            if (WaitStop(state, static_cast<DWORD>(kTick.count() * 1000)))
                break;
        }
    }
    catch (...)
    {
    }
    return 0;
}

void StartWorker(State& state)
{
    if (state.threadRunning || state.threadFailed)
        return;
    if (!PinModule()) // pinning failed: never start the worker (the key stays effectively off): one log line
    {
        state.threadFailed = true; // no retry storm on every present: one log line, then silence
        try
        {
            LOG_WARN("status.json: module pin failed, no file will be written");
        }
        catch (...)
        {
        }
        return;
    }
    // Pinned: FreeLibrary from here on never unloads the module, so the DLL cannot be unloaded until process exit
    // (intended) and the FreeLibrary detach branch below is unreachable once the worker runs (see OnShutdown).
    if (state.stopEvent == nullptr)
        state.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset, initially clear
    uintptr_t handle = _beginthreadex(nullptr, 0, &WorkerProc, nullptr, 0, nullptr);
    if (handle == 0)
    {
        state.threadFailed = true; // no retry storm on every present: one log line, then silence
        try
        {
            LOG_WARN("status.json: worker thread failed to start, no file will be written");
        }
        catch (...)
        {
        }
        return;
    }
    SetThreadPriority(reinterpret_cast<HANDLE>(handle), THREAD_PRIORITY_BELOW_NORMAL);
    state.thread = reinterpret_cast<HANDLE>(handle); // signaled then closed at shutdown, never here
    state.threadRunning = true;
    try
    {
        LOG_INFO("status.json: enabled, writing beside the DLL (first write in about 2 s)");
    }
    catch (...)
    {
    }
}

} // namespace

void OnStartup() noexcept
{
    try
    {
        State& state = Global();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started)
            return;
        state.started = true;
        state.startSystem = std::chrono::system_clock::now();
        state.pid = GetCurrentProcessId();
        state.sessionId = MakeSessionId(state.pid);
        state.dllDir = ThisModuleDir();
    }
    catch (...)
    {
    }
}

void OnScopeBegin(ext::present::Phase phase) noexcept
{
    try
    {
        if (phase != ext::present::Phase::NativePresent && phase != ext::present::Phase::OutputPresent)
            return; // ignored phases return at once: before the key read, the clock and the lock
        if (!Enabled())
            return; // one config read, no timestamp, no lock
        State& state = Global();
        const Clock::time_point now = Clock::now();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.started || state.dllDir.empty())
            return; // no Banner yet, or no folder to write to
        if (!state.threadRunning)
            StartWorker(state); // lazy: nothing is created in DllMain (a failed start never retries)
        Series* series = nullptr;
        Clock::time_point* last = nullptr;
        if (phase == ext::present::Phase::NativePresent)
        {
            series = &state.base;
            last = &state.lastBase;
        }
        else
        {
            series = &state.output;
            last = &state.lastOutput;
        }
        if (*last != Clock::time_point::min())
        {
            const double ms =
                std::chrono::duration<double, std::milli>(now - *last).count();
            if (ms >= 0.0 && ms < kStaleMs)
                series->Push(ms); // a longer gap is a hitch or a pause, not a frame time
        }
        *last = now;
    }
    catch (...)
    {
    }
}

// Best effort, only when we wrote at least once: delete our own status.json + status.lock when the lock still names
// our pid (another client may own the file now). Deletion -- not a "state":"exited" marker -- so no schema change
// is needed: a clean exit leaves no file, while a crash leaves a file with a stale lock (older than 30 s or a dead
// pid), which is how a dead process's file stays recognisable.
void DeleteOwnFile(State& state) noexcept
{
    try
    {
        std::wstring dir;
        unsigned long pid = 0;
        {
            std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
            if (!lock.owns_lock())
                return;
            if (state.seq == 0)
                return; // we never wrote: nothing of ours to delete
            dir = state.dllDir;
            pid = state.pid;
        }
        if (dir.empty() || pid == 0)
            return;
        const std::wstring lockPath = dir + L"\\status.lock";
        HANDLE h = CreateFileW(lockPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return; // no lock (or unreadable): delete nothing we cannot prove is ours
        char text[64] = {};
        DWORD read = 0;
        const BOOL ok = ReadFile(h, text, sizeof text - 1, &read, nullptr);
        CloseHandle(h);
        unsigned long ownerPid = 0;
        if (!ok || std::sscanf(text, "%lu", &ownerPid) != 1 || ownerPid != pid)
            return; // someone else's file now (or unreadable): leave it alone
        DeleteFileW((dir + L"\\status.json").c_str());
        DeleteFileW(lockPath.c_str());
    }
    catch (...)
    {
    }
}

void OnShutdown(bool freeLibrary) noexcept
{
    try
    {
        State& state = Global();
        if (!freeLibrary)
        {
            // Normal game exit (ExitProcess): the other threads are already gone, so delete our own file here too
            // (DeleteOwnFile takes the mutex with try_lock and deletes only when the lock still names our pid;
            // another client's file is left alone). No signaling or waiting: there is nothing left to stop.
            // Chose deletion over a final "exited" document: no schema change is needed, a clean exit leaves no
            // file, and a crash still leaves a file with a stale lock behind.
            DeleteOwnFile(state);
            return;
        }
        // FreeLibrary: only signal the worker and delete our own file, never wait. The worker may need the loader
        // lock (file-version reads pull in version.dll, logging formats), which this detach holds, so waiting on
        // the thread could never succeed and would burn the whole timeout. The pin makes this FreeLibrary branch
        // unreachable after the worker starts (see StartWorker: pinning comes first, and no worker starts when
        // pinning fails), so code a late worker may run stays mapped; the worker sees the stop flag before every
        // write (see WorkerCycle) and exits at its next wait, so it cannot recreate the file after the delete below.
        state.stopRequested.store(true, std::memory_order_relaxed);
        if (state.stopEvent != nullptr)
            SetEvent(state.stopEvent); // wake the worker wherever it waits
        if (state.thread != nullptr)
        {
            CloseHandle(state.thread); // closing the handle does not stop the thread; the stop flag does that
            state.thread = nullptr;
            state.threadRunning = false;
        }
        DeleteOwnFile(state);
    }
    catch (...)
    {
    }
}

} // namespace ext::status
