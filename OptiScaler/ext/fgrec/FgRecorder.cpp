#include "pch.h"

#include "ext/fgrec/FgRecorder.h"

#include <process.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Recorder core (FgRecorder.h): the bounded queue, the writer thread and the session files. Ported from
// SirenBrink/FFXIVRender's fg_diagnostics (GPL-3.0-or-later) with smaller records (fixed small fields, no big
// arrays), a rotation of 16 MiB x 9 files (a 25-minute play session keeps its head) and a session cap (newest 5 sessions,
// own-pattern files only).
//
// Locking: emitters and the worker meet only to swap small batches under the mutex. The worker's file I/O
// runs without the mutex, so stopping it never needs the loader lock: shutdown signals and, when the writer
// is already gone, takes over the open handle synchronously. The session file has one owner: every open,
// rotation and close publishes the new handle to State::out under the mutex, so the stop path, the exit
// takeover and the fault path always act on the live file and never on a value the worker already closed.
// The writer drains on its own timer (about 100 ms) or when the queue passes a high-water mark, and asks the
// device to flush at most about once a second: a cache flush throttles the game's own disk I/O, and data
// already written survives a process crash without one.
// Failures log once and latch an error code for the menu status line; the game is never blocked. Every entry
// point is noexcept.
namespace ext::fgrec
{
namespace
{
constexpr size_t kQueueCap = 4096; // bounded: overflow is an explicit records_dropped record
// The tap wakes the writer only when the ring is half full: an idle session then costs the timer wake,
// never one wake per frame (a wake per frame would flush the device once per frame).
constexpr size_t kHighWater = kQueueCap / 2;
constexpr uint64_t kRotateBytes = 16ull * 1024 * 1024; // each file rotates at 16 MiB ...
// ... keeping base + .1 .. .8 (144 MiB) per session. Nine files hold about 40 minutes at 60 base fps with 2x frame generation (the
// 3.4 MB/min a measured 2x session wrote), and each file holds 16 MiB plus the queue batch that crossed the limit, so the bound
// depends on the rate the session writes. A 25-minute session keeps its beginning: the head of a recording is where a problem
// usually starts, and the per-present output records cannot be aggregated without losing the batch / generated-frame accounting the
// analyzer does.
constexpr int kKeepRotations = 8;
constexpr int kKeepSessions = 5; // at start, only the newest 5 sessions stay
constexpr DWORD kBatchWaitMs = 100; // the writer drains about every 100 ms
constexpr uint64_t kFlushMs = 1000; // ... and flushes at most about once a second
constexpr size_t kKindCap = 28; // record field capacities, also the escape bounds below
constexpr size_t kMsgCap = 160; // AutoMFG lines keep their reason (cut at 159 characters)
constexpr size_t kTakeoverChunk = 16384; // stack buffer of the exit takeover's writes (it never allocates)
constexpr size_t kMaxRecordLine = 1536; // worst case: every kind and msg byte escaped to six characters
constexpr char kEndLine[] = "{\"kind\":\"session.end\"}\n"; // the terminal record, written as it is
constexpr const wchar_t* kDirName = L"fg-recordings";
constexpr const wchar_t* kFilePrefix = L"fg-rec-";
constexpr const wchar_t* kFileSuffix = L".jsonl";

struct Record
{
    uint64_t seq = 0, qpc = 0, frame = 0, gen = 0;
    uint32_t tid = 0;
    unsigned prov = 0;
    int64_t a = 0, b = 0, c = 0;
    char kind[kKindCap] = {};
    char msg[kMsgCap] = {};
};

static_assert(6 * (kKindCap + kMsgCap) + 256 <= kMaxRecordLine,
              "kMaxRecordLine must hold one fully escaped record");

// Fixed ring of records, preallocated once at start: steady-state taps only
// write a slot, never allocate. Full means drop-and-count (Emit checks first),
// and the worker drains in FIFO order. The member keeps the queue name so the
// queue bound reads the same everywhere.
struct Ring
{
    std::vector<Record> slots;
    size_t head = 0, count = 0;

    size_t capacity() const { return slots.size(); }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    void clear()
    {
        head = 0;
        count = 0;
    }
    void allocate(size_t n)
    {
        slots.assign(n, Record {});
        head = 0;
        count = 0;
    }
    const Record& front() const { return slots[head]; }
    const Record& back() const { return slots[(head + count - 1) % slots.size()]; }
    // False when full: the caller counts the drop. Never allocates (the slots
    // are fixed), so the present thread pays no heap per event.
    bool push_back(const Record& r)
    {
        if (count >= slots.size())
            return false;
        slots[(head + count) % slots.size()] = r;
        ++count;
        return true;
    }
    // Move every record out in order; the ring is empty afterwards. The caller's
    // vector keeps the queue cap as its capacity (preallocated at start), so this
    // copies in two blocks and never allocates while it holds the mutex.
    void drain(std::vector<Record>& out)
    {
        out.clear();
        out.reserve(count);
        const size_t first = std::min(count, slots.size() - head);
        out.insert(out.end(), slots.begin() + static_cast<ptrdiff_t>(head),
                   slots.begin() + static_cast<ptrdiff_t>(head + first));
        out.insert(out.end(), slots.begin(), slots.begin() + static_cast<ptrdiff_t>(count - first));
        head = 0;
        count = 0;
    }
};

struct State
{
    std::atomic<bool> enabled { false }; // last key seen (the only thing Emit reads)
    std::atomic<bool> stopRequested { false };
    std::atomic<unsigned> prov { kProvNone };
    std::atomic<uint64_t> gen { 0 };
    std::atomic<unsigned> error { 0 };
    std::atomic<bool> alive { false }; // worker liveness hint (set under the mutex at start/exit)
    std::atomic<bool> keyWasOn { false }; // the key of the previous start call: an off-to-on edge retries a latch
    std::atomic<bool> failed { false }; // sticky start failure, written under the mutex, read without a lock
    std::atomic<uint64_t> loopCount { 0 }; // writer iterations (idle shows the sleep cadence)
    std::atomic<uint64_t> flushCount { 0 }; // writer flushes (none on empty batches, at most about one a second)
    std::mutex mutex; // emitters and the worker meet only here, briefly, for small copies
    Ring queue;
    // The worker's drain target, sized to the queue cap at start: draining never
    // allocates under the mutex, and the exit takeover reuses it without the heap.
    std::vector<Record> drained;
    uint64_t next = 0, dropped = 0;
    bool running = false; // the worker is alive
    HANDLE thread = nullptr; // signaled then closed at shutdown, never waited on there
    HANDLE stopEvent = nullptr; // auto-reset: a tap wakes the worker once
    // The session file, one value: every open, rotation and close publishes here
    // under the mutex, so a stop, a takeover or a fault never names a closed handle.
    HANDLE out = INVALID_HANDLE_VALUE;
    std::wstring dir; // recordings folder, local only and never emitted
    std::wstring file; // current session base file
    uint64_t qpf = 0;
    unsigned long pid = 0;
    std::string utc, version, exe, build, summary; // session.begin content, never a path
};

State& Global()
{
    static State state; // no heap until the first start: the queue, thread and file come later
    return state;
}

// Pinned while the worker runs (the status file pins the same way): detach then never unmaps code a late
// worker may still run. False when pinning fails: the caller must not start the worker then.
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

void UtcStamp(SYSTEMTIME& utc, char* out, size_t n, bool fileName)
{
    if (fileName)
        std::snprintf(out, n, "%04u%02u%02u-%02u%02u%02u-%03u", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                      utc.wMinute, utc.wSecond, utc.wMilliseconds);
    else
        std::snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                      utc.wMinute, utc.wSecond);
}

// Past MAX_PATH the extended prefix keeps the open working. A UNC directory needs the \\?\UNC\ form:
// a plain \\?\ before \\server\share is not a valid path.
std::wstring ExtendedPath(const std::wstring& dir)
{
    if (dir.compare(0, 4, L"\\\\?\\") == 0)
        return dir; // already extended
    if (dir.compare(0, 2, L"\\\\") == 0)
        return L"\\\\?\\UNC\\" + dir.substr(2);
    return L"\\\\?\\" + dir;
}

// A fixed text buffer for the exit takeover: formatting into it never touches the heap, so the takeover
// stays safe inside DllMain, where a terminated worker may have died holding the process heap lock.
// std::string and this struct both provide append(const char*, size_t) and push_back(char), which is all
// the serializers below use.
struct FixedText
{
    char* data = nullptr;
    size_t cap = 0, len = 0;
    void append(const char* text, size_t n)
    {
        if (n <= cap - len)
        {
            memcpy(data + len, text, n);
            len += n;
        }
    }
    void push_back(char c)
    {
        if (len < cap)
            data[len++] = c;
    }
};

// Every line stays strict JSON: '"' and '\\' are escaped, control bytes become \u00XX, valid UTF-8 passes
// through and anything else becomes U+FFFD (a bounded record may cut a UTF-8 sequence, as upstream noted).
template <class Out>
void AppendEscaped(Out& out, const char* text)
{
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
    {
        if (*p == '"' || *p == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(*p));
        }
        else if (*p < 0x20)
        {
            char encoded[7] = {};
            std::snprintf(encoded, sizeof encoded, "\\u%04x", static_cast<unsigned>(*p));
            out.append(encoded, 6);
        }
        else if (*p < 0x80)
        {
            out.push_back(static_cast<char>(*p));
        }
        else
        {
            const unsigned count = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC2 ? 2 : 0;
            unsigned code = count != 0 ? *p & ((1u << (7 - count)) - 1) : 0;
            bool valid = count != 0 && *p <= 0xF4;
            for (unsigned i = 1; valid && i < count; ++i)
            {
                valid = p[i] != 0 && (p[i] & 0xC0) == 0x80;
                if (valid)
                    code = (code << 6) | (p[i] & 0x3F);
            }
            valid = valid && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF) &&
                    code >= (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u);
            if (valid)
            {
                out.append(reinterpret_cast<const char*>(p), count);
                p += count - 1;
            }
            else
            {
                out.append("\xEF\xBF\xBD", 3);
            }
        }
    }
}

template <class Out>
void AppendRecord(Out& out, const Record& r)
{
    char head[256] = {};
    const int n = std::snprintf(head, sizeof head,
                                "{\"seq\":%llu,\"qpc\":%llu,\"tid\":%u,\"frame\":%llu,\"gen\":%llu,\"prov\":%u,\"kind\":\"",
                                r.seq, r.qpc, r.tid, r.frame, r.gen, r.prov);
    out.append(head, n > 0 ? static_cast<size_t>(n) : 0);
    AppendEscaped(out, r.kind);
    char nums[128] = {};
    const int m = std::snprintf(nums, sizeof nums, "\",\"a\":%lld,\"b\":%lld,\"c\":%lld,\"msg\":\"", r.a, r.b, r.c);
    out.append(nums, m > 0 ? static_cast<size_t>(m) : 0);
    AppendEscaped(out, r.msg);
    out.append("\"}\n", 3);
}

// Session key of an own-pattern file: the base name without a trailing .1 .. .8 rotation suffix. Only the
// exact pattern the recorder writes is grouped: fg-rec-<8 digits>-<6 digits>-<3 digits>-<pid>.jsonl plus
// an optional .1 .. .<kKeepRotations>. Backups, Explorer copies, lookalikes and other tools' logs never
// match, so they are never grouped and never deleted.
bool OwnFile(const std::wstring& name, std::wstring& session)
{
    const std::wstring pre(kFilePrefix), suf(kFileSuffix);
    size_t at = 0;
    if (name.compare(0, pre.size(), pre) != 0)
        return false;
    at = pre.size();
    auto digits = [&](size_t n) {
        if (name.size() < at + n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (name[at + i] < L'0' || name[at + i] > L'9')
                return false;
        at += n;
        return true;
    };
    if (!digits(8) || at >= name.size() || name[at++] != L'-')
        return false;
    if (!digits(6) || at >= name.size() || name[at++] != L'-')
        return false;
    if (!digits(3) || at >= name.size() || name[at++] != L'-')
        return false;
    const size_t pidStart = at;
    while (at < name.size() && name[at] >= L'0' && name[at] <= L'9')
        ++at;
    if (at == pidStart || name.compare(at, suf.size(), suf) != 0)
        return false;
    at += suf.size();
    size_t end = at;
    if (at + 1 < name.size() && name[at] == L'.')
    {
        size_t n = at + 1;
        unsigned number = 0;
        while (n < name.size() && name[n] >= L'0' && name[n] <= L'9' && number <= 1000)
            number = number * 10 + static_cast<unsigned>(name[n++] - L'0');
        // A rotation suffix the recorder writes has no leading zero: "….jsonl.01" or "….jsonl.008" is a stray copy, not a session file,
        // and must not become a session key that takes one of the kept slots (and is then swept with it).
        if (n > at + 1 && number >= 1 && number <= static_cast<unsigned>(kKeepRotations) && name[at + 1] != L'0')
            end = n;
    }
    if (end != name.size())
        return false;
    session = name.substr(0, at);
    return true;
}

// Numeric stamp of an own-pattern session key (YYYYMMDDHHMMSSmmm concatenated): sessions sort by time,
// not by string order, so a hand-named copy can never outrank a real session.
uint64_t SessionStamp(const std::wstring& key)
{
    uint64_t stamp = 0;
    for (size_t i = 7; i < key.size() && stamp < 1000000000000000000ull; ++i)
        if (key[i] >= L'0' && key[i] <= L'9')
            stamp = stamp * 10 + static_cast<uint64_t>(key[i] - L'0');
    return stamp;
}

// Deletes recordings of sessions older than the newest few, matched by the fork's own file-name pattern and
// nothing else: foreign files, directories and reparse points in the same folder are never touched. Runs in
// the worker before the new file exists, so it keeps kKeepSessions - 1 old sessions and the new file brings
// the total to the documented 5.
void SweepOldSessions(const std::wstring& dir) noexcept
{
    try
    {
        struct Group
        {
            std::wstring key;
            uint64_t stamp = 0;
        };
        std::vector<Group> sessions;
        WIN32_FIND_DATAW found {};
        HANDLE h = FindFirstFileW((dir + L"\\" + kFilePrefix + L"*" + kFileSuffix + L"*").c_str(), &found);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
        {
            if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
                continue; // a directory wearing a session-like name stays, as does any reparse point
            std::wstring session;
            if (!OwnFile(found.cFileName, session))
                continue;
            if (std::find_if(sessions.begin(), sessions.end(), [&](const Group& g) { return g.key == session; }) ==
                sessions.end())
                sessions.push_back(Group { session, SessionStamp(session) });
        } while (FindNextFileW(h, &found) != 0);
        FindClose(h);
        std::sort(sessions.begin(), sessions.end(),
                  [](const Group& a, const Group& b) { return a.stamp != b.stamp ? a.stamp > b.stamp : a.key > b.key; });
        for (size_t i = static_cast<size_t>(kKeepSessions - 1); i < sessions.size(); ++i)
            for (int r = 0; r <= kKeepRotations; ++r)
            {
                const std::wstring path =
                    dir + L"\\" + sessions[i].key + (r == 0 ? L"" : L"." + std::to_wstring(r));
                const DWORD attrs = GetFileAttributesW(path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES ||
                    (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
                    continue; // only our own plain files go, never anything else
                DeleteFileW(path.c_str()); // best effort: only own-pattern names
            }
    }
    catch (...)
    {
    }
}

bool WriteText(HANDLE out, const char* text, size_t length, uint64_t& bytes) noexcept
{
    if (length == 0)
        return true;
    DWORD written = 0;
    if (WriteFile(out, text, static_cast<DWORD>(length), &written, nullptr) == 0 || written != length)
        return false;
    bytes += written;
    return true;
}

bool WriteAll(HANDLE out, const std::string& text, uint64_t& bytes) noexcept
{
    return WriteText(out, text.data(), text.size(), bytes);
}

// One drained batch to the session file: an explicit records_dropped line for
// the overflow the ring counted, then the records in order. No flush here: the
// caller flushes on its own cadence. False when a write fails.
bool WriteBatch(HANDLE out, const std::vector<Record>& batch, uint64_t lost, uint64_t& bytes) noexcept
{
    if (lost != 0)
    {
        const std::string line = "{\"kind\":\"records_dropped\",\"count\":" + std::to_string(lost) + "}\n";
        if (!WriteAll(out, line, bytes))
            return false;
    }
    if (!batch.empty())
    {
        std::string text;
        text.reserve(batch.size() * 160);
        for (const auto& r : batch)
            AppendRecord(text, r);
        if (!WriteAll(out, text, bytes))
            return false;
    }
    return true;
}

// The same batch out of a fixed stack buffer, for the exit takeover: it runs
// inside DllMain, where a terminated worker may have died holding the heap lock,
// so it must not allocate. The chunk is flushed whenever the next record could
// not fit whole, so no line is ever cut.
bool WriteBatchFixed(HANDLE out, const std::vector<Record>& batch, uint64_t lost, uint64_t& bytes) noexcept
{
    char store[kTakeoverChunk] = {};
    FixedText text { store, sizeof store, 0 };
    auto flush = [&]() -> bool {
        if (text.len == 0)
            return true;
        DWORD written = 0;
        if (WriteFile(out, store, static_cast<DWORD>(text.len), &written, nullptr) == 0 || written != text.len)
            return false;
        bytes += written;
        text.len = 0;
        return true;
    };
    if (lost != 0)
    {
        char line[96] = {};
        const int n = std::snprintf(line, sizeof line, "{\"kind\":\"records_dropped\",\"count\":%llu}\n",
                                    static_cast<unsigned long long>(lost));
        text.append(line, n > 0 ? static_cast<size_t>(n) : 0);
    }
    for (const auto& r : batch)
    {
        if (sizeof store - text.len < kMaxRecordLine && !flush())
            return false;
        AppendRecord(text, r);
    }
    return flush();
}

// Rotate only this session's own files (144 MiB maximum): the oldest rotation .<kKeepRotations> is dropped, then every .n goes
// to .n + 1 (highest first) and the base goes to .1. Every step is checked: a rotation a reader blocks fails loudly (the session
// stops with an error) instead of truncating the base and losing 16 MiB silently. On every false return out is
// INVALID_HANDLE_VALUE and nothing this call opened stays open, so no caller can keep a handle that another path then closes again.
bool Rotate(HANDLE& out, const std::wstring& base, uint64_t qpf, uint64_t& bytes) noexcept
{
    try
    {
        CloseHandle(out);
        out = INVALID_HANDLE_VALUE;
        const std::wstring oldest = base + L"." + std::to_wstring(kKeepRotations);
        if (GetFileAttributesW(oldest.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(oldest.c_str()) == 0)
            return false; // the oldest rotation is held open: keep the base intact and stop
        for (int n = kKeepRotations - 1; n >= 1; --n)
        {
            const std::wstring from = base + L"." + std::to_wstring(n);
            const std::wstring to = base + L"." + std::to_wstring(n + 1);
            if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES && MoveFileW(from.c_str(), to.c_str()) == 0)
                return false;
        }
        if (MoveFileW(base.c_str(), (base + L".1").c_str()) == 0)
            return false;
        out = CreateFileW(base.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
            return false;
        char freq[64] = {};
        std::snprintf(freq, sizeof freq, "{\"kind\":\"continuation\",\"schema\":1,\"qpc_frequency\":%llu}\n", qpf);
        std::string line(freq);
        bytes = 0;
        if (!WriteAll(out, line, bytes) || FlushFileBuffers(out) == 0)
        {
            CloseHandle(out);
            out = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }
    catch (...)
    {
        if (out != INVALID_HANDLE_VALUE)
            CloseHandle(out);
        out = INVALID_HANDLE_VALUE;
        return false;
    }
}

// Rotation changes the session file, so it also republishes the handle: State::out is the value the stop
// path, the exit takeover and the fault path act on. It is emptied under the mutex before Rotate closes the
// old handle, so no other path ever names a closed value - not even during the rotation itself, when the
// system may already have handed the freed value to someone else.
bool RotateAndPublish(State& s, HANDLE& out, const std::wstring& base, uint64_t qpf, uint64_t& bytes) noexcept
{
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        // The session file is unfinished for the length of the rotation: a takeover sees no handle and skips
        // it (the analyzer reports the missing session.end) instead of acting on a value Rotate released.
        s.out = INVALID_HANDLE_VALUE;
    }
    const bool rotated = Rotate(out, base, qpf, bytes);
    if (!rotated)
        out = INVALID_HANDLE_VALUE;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.out = out;
    return rotated;
}

// Same-millisecond restarts (or a stale file under a reused pid) collide on the
// stamped name: bump the millisecond group until the name is new, so a new
// session never truncates an old one. False when the name is not ours.
bool BumpName(std::wstring& base, unsigned bump)
{
    const size_t jsonl = base.rfind(L".jsonl");
    if (jsonl == std::wstring::npos)
        return false;
    const size_t pidDash = base.rfind(L'-', jsonl);
    if (pidDash == std::wstring::npos || pidDash == 0)
        return false;
    const size_t msDash = base.rfind(L'-', pidDash - 1);
    if (msDash == std::wstring::npos || pidDash - msDash != 4)
        return false;
    const unsigned ms = static_cast<unsigned>(std::wcstoul(base.substr(msDash + 1, 3).c_str(), nullptr, 10));
    wchar_t rep[4] = {};
    std::swprintf(rep, std::size(rep), L"%03u", (ms + bump) % 1000);
    base.replace(msDash + 1, 3, rep);
    return true;
}

// The dead worker may have been terminated mid-line: cut back to the last
// newline so the file stays strict JSON, then the caller appends whole lines.
// Leaves the file pointer at the end either way. Runs inside DllMain, so the
// tail is read into a fixed stack buffer and nothing is allocated.
void TruncatePartialLine(HANDLE out) noexcept
{
    try
    {
        LARGE_INTEGER size {};
        if (GetFileSizeEx(out, &size) == 0)
            return;
        LARGE_INTEGER end {};
        end.QuadPart = size.QuadPart;
        if (size.QuadPart != 0)
        {
            char buf[4096] = {};
            const uint64_t tail =
                static_cast<uint64_t>(size.QuadPart) > sizeof buf ? sizeof buf : static_cast<uint64_t>(size.QuadPart);
            LARGE_INTEGER pos {};
            pos.QuadPart = size.QuadPart - static_cast<LONGLONG>(tail);
            DWORD read = 0;
            if (SetFilePointerEx(out, pos, nullptr, FILE_BEGIN) != 0 &&
                ReadFile(out, buf, static_cast<DWORD>(tail), &read, nullptr) != 0 && read != 0)
            {
                for (DWORD i = read; i != 0; --i)
                    if (buf[i - 1] == '\n')
                    {
                        end.QuadPart = pos.QuadPart + static_cast<LONGLONG>(i);
                        break;
                    }
            }
        }
        SetFilePointerEx(out, end, nullptr, FILE_BEGIN);
        SetEndOfFile(out);
    }
    catch (...)
    {
    }
}

// Take the session file under the mutex and mark the worker gone: the caller
// owns the returned handle (or INVALID when already taken over) and writes or
// closes it without the mutex. Sets the sticky failure, the error code and one
// log line on the first failure; a quiet take passes code 0 with no text.
HANDLE FailStop(State& s, unsigned code, const char* text) noexcept
{
    // The caller holds s.mutex.
    (void) text; // only the logging build reads it (the test stub drops the log)
    if (!s.failed.load(std::memory_order_relaxed) && code != 0)
    {
        s.failed.store(true, std::memory_order_relaxed);
        s.error.store(code, std::memory_order_relaxed);
        try
        {
            LOG_WARN("{}", text);
        }
        catch (...)
        {
        }
    }
    HANDLE mine = s.out;
    s.out = INVALID_HANDLE_VALUE;
    s.running = false;
    s.alive.store(false, std::memory_order_relaxed);
    return mine;
}

unsigned __stdcall WorkerProc(void*) noexcept
{
    try
    {
        State& s = Global();
        std::string begin;
        std::wstring base, dir;
        uint64_t qpf = 0;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            begin = "{\"schema\":1,\"kind\":\"session.begin\",\"qpc_frequency\":" + std::to_string(s.qpf) +
                    ",\"pid\":" + std::to_string(s.pid) + ",\"utc\":\"" + s.utc + "\",\"version\":\"";
            AppendEscaped(begin, s.version.c_str());
            begin += "\",\"exe\":\"";
            AppendEscaped(begin, s.exe.c_str());
            begin += "\",\"build\":\"";
            AppendEscaped(begin, s.build.c_str());
            begin += "\",\"summary\":\"";
            AppendEscaped(begin, s.summary.c_str());
            begin += "\"}\n";
            base = s.file; // the session file never moves under a running worker (a new session waits for it)
            dir = s.dir;
            qpf = s.qpf;
        }
        CreateDirectoryW(dir.c_str(), nullptr); // may exist: the open below is what counts
        SweepOldSessions(dir); // newest 4 old sessions stay; the new file below makes 5
        for (unsigned bump = 0; bump <= 16; ++bump)
        {
            if (bump != 0 && !BumpName(base, 1))
                break;
            if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES)
                break;
            if (bump == 16)
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                FailStop(s, 1, "fg-recorder: no fresh session name is free, no file will be written");
                return 0;
            }
        }
        HANDLE out = CreateFileW(base.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            FailStop(s, 1, "fg-recorder: the session file could not be opened, no file will be written");
            return 0;
        }
        uint64_t bytes = 0;
        if (!WriteAll(out, begin, bytes) || FlushFileBuffers(out) == 0)
        {
            CloseHandle(out);
            out = INVALID_HANDLE_VALUE;
            std::lock_guard<std::mutex> lock(s.mutex);
            FailStop(s, 2, "fg-recorder: the session file write failed, no file will be written");
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.out = out; // published for the process-exit takeover (the only handle this session has)
        }
        uint64_t lastFlush = GetTickCount64();
        for (;;)
        {
            WaitForSingleObject(s.stopEvent, kBatchWaitMs); // auto-reset: sleeps when idle
            s.loopCount.fetch_add(1, std::memory_order_relaxed);
            const bool stop = s.stopRequested.load(std::memory_order_relaxed) ||
                              !s.enabled.load(std::memory_order_relaxed);
            uint64_t lost = 0;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.queue.drain(s.drained);
                lost = s.dropped;
                s.dropped = 0;
            }
            if (!s.drained.empty() || lost != 0)
            {
                // The write goes out at once; the device flush waits for its own
                // cadence, because a flush per batch throttles the game's disk I/O.
                const uint64_t now = GetTickCount64();
                const bool flush = now - lastFlush >= kFlushMs;
                if (!WriteBatch(out, s.drained, lost, bytes) || (flush && FlushFileBuffers(out) == 0))
                {
                    HANDLE mine = INVALID_HANDLE_VALUE;
                    {
                        std::lock_guard<std::mutex> lock(s.mutex);
                        mine = FailStop(s, 2, "fg-recorder: the session file write failed");
                    }
                    if (mine != INVALID_HANDLE_VALUE)
                        CloseHandle(mine);
                    return 0;
                }
                if (flush)
                {
                    lastFlush = now;
                    s.flushCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (bytes >= kRotateBytes && !RotateAndPublish(s, out, base, qpf, bytes))
            {
                // RotateAndPublish already published INVALID: there is no live handle left to close.
                std::lock_guard<std::mutex> lock(s.mutex);
                FailStop(s, 2, "fg-recorder: rotation failed, the session stops instead of losing data silently");
                return 0;
            }
            if (stop)
            {
                // Drain-then-end without holding the mutex across file I/O. The tail is swapped into this
                // worker's own buffer, so a new session that starts as soon as the flags below say the worker
                // is gone can reuse State::drained for its own loop without refilling the batch being written.
                uint64_t tailLost = 0;
                HANDLE mine = INVALID_HANDLE_VALUE;
                std::vector<Record> tail;
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    if (s.out == INVALID_HANDLE_VALUE)
                        return 0; // taken over by the detach path: nothing left to write or close
                    s.queue.drain(s.drained);
                    tailLost = s.dropped;
                    s.dropped = 0;
                    mine = FailStop(s, 0, "");
                    tail.swap(s.drained); // the tail belongs to this worker from here on
                }
                uint64_t ignored = 0;
                WriteBatch(mine, tail, tailLost, ignored);
                WriteText(mine, kEndLine, sizeof kEndLine - 1, ignored);
                FlushFileBuffers(mine);
                CloseHandle(mine);
                return 0;
            }
        }
    }
    catch (...)
    {
        try
        {
            State& s = Global();
            std::lock_guard<std::mutex> lock(s.mutex);
            HANDLE mine = FailStop(s, 6, "fg-recorder: the writer faulted");
            if (mine != INVALID_HANDLE_VALUE)
                CloseHandle(mine);
        }
        catch (...)
        {
        }
        return 0;
    }
}

void CopyCapped(std::string& to, const char* from, size_t cap)
{
    to.clear();
    if (from == nullptr)
        return;
    to.assign(from, strnlen(from, cap));
}

} // namespace

void EnsureStarted(bool keyOn, const SessionInfo& info) noexcept
{
    try
    {
        State& s = Global();
        if (!keyOn)
        {
            s.enabled.store(false, std::memory_order_relaxed);
            s.keyWasOn.store(false, std::memory_order_relaxed);
            // The worker stops after its drain (session.end first). Requesting that stop here also closes the
            // tap gate at once: a tap that passes the key check just before this call, or a key-on that lands
            // while the old worker is still finishing, cannot leave a record in a ring the stopping worker has
            // already drained, where the next session would clear it without a trace.
            s.stopRequested.store(true, std::memory_order_relaxed);
            // Clear the failure latch with try_lock only, never block for it here.
            std::unique_lock<std::mutex> unlock(s.mutex, std::try_to_lock);
            if (unlock.owns_lock())
                s.failed.store(false, std::memory_order_relaxed); // key off clears the latch: the next key-on retries
            return;
        }
        s.enabled.store(true, std::memory_order_relaxed);
        const bool keyEdge = !s.keyWasOn.exchange(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (keyEdge)
            s.failed.store(false, std::memory_order_relaxed); // a new key session retries once, whatever latched last
        if (s.running)
            return;
        if (s.failed.load(std::memory_order_relaxed))
            return; // latched: a failed start never retries from this thread
        if (info.moduleDir == nullptr || info.moduleDir[0] == 0)
        {
            // No folder, so no file can be named: latch instead of allocating a
            // queue and sizing the path on every later tap.
            FailStop(s, 7, "fg-recorder: the module folder is unknown, no file will be written");
            return;
        }
        s.stopRequested.store(false, std::memory_order_relaxed);
        s.queue.allocate(kQueueCap); // the fixed ring: no allocation per event from here on
        s.drained.reserve(kQueueCap); // the worker's drain buffer: draining never allocates, not even at exit
        s.next = s.dropped = 0;
        CopyCapped(s.version, info.version, 64);
        CopyCapped(s.exe, info.exe, 32);
        CopyCapped(s.build, info.build, 32);
        CopyCapped(s.summary, info.summary, 128);
        s.pid = GetCurrentProcessId();
        LARGE_INTEGER freq {};
        QueryPerformanceFrequency(&freq);
        s.qpf = static_cast<uint64_t>(freq.QuadPart);
        // The real file path, not the directory alone: dir + folder + the
        // stamped name + the rotation suffix. Past MAX_PATH the extended
        // prefix keeps the open working; past the extended limit it latches.
        SYSTEMTIME utc {};
        GetSystemTime(&utc);
        char stamp[32] = {};
        UtcStamp(utc, stamp, sizeof stamp, true);
        wchar_t name[MAX_PATH] = {};
        std::swprintf(name, std::size(name), L"%s%S-%lu.jsonl", kFilePrefix, stamp, s.pid);
        std::wstring dirName = std::wstring(info.moduleDir) + L"\\" + kDirName;
        std::wstring file = dirName + L"\\" + name;
        if (file.size() + 4 >= MAX_PATH)
        {
            dirName = ExtendedPath(dirName);
            file = dirName + L"\\" + name;
        }
        if (file.size() + 4 >= 32767)
        {
            FailStop(s, 5, "fg-recorder: the recordings path is too long, no file will be written");
            return;
        }
        s.dir = dirName;
        s.file = file;
        UtcStamp(utc, stamp, sizeof stamp, false);
        s.utc = stamp;
        if (!PinModule())
        {
            FailStop(s, 3, "fg-recorder: the module could not be pinned, no file will be written");
            return;
        }
        if (s.stopEvent == nullptr)
            s.stopEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset, initially clear
        if (s.stopEvent == nullptr)
        {
            FailStop(s, 4, "fg-recorder: the wake event could not be created, no file will be written");
            return;
        }
        if (s.thread != nullptr)
        {
            CloseHandle(s.thread); // the stale handle of an exited worker: never leak across restarts
            s.thread = nullptr;
        }
        const uintptr_t handle = _beginthreadex(nullptr, 0, &WorkerProc, nullptr, 0, nullptr);
        if (handle == 0)
        {
            FailStop(s, 4, "fg-recorder: the worker thread failed to start, no file will be written");
            return;
        }
        SetThreadPriority(reinterpret_cast<HANDLE>(handle), THREAD_PRIORITY_BELOW_NORMAL);
        s.thread = reinterpret_cast<HANDLE>(handle); // signaled then closed at shutdown, never waited on there
        s.running = true;
        s.alive.store(true, std::memory_order_relaxed);
        s.error.store(0, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void RearmKey() noexcept
{
    try
    {
        State& s = Global();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.failed.store(false, std::memory_order_relaxed); // once per key-on: the next tap may retry a latched start
    }
    catch (...)
    {
    }
}

namespace
{
// The mark counter, like the key state: it survives a key-off/key-on of the recorder (the numbers the user saw in the log stay the
// numbers in the file) and needs no thread, no allocation and no lock.
std::atomic<unsigned> g_marks { 0 };
} // namespace

unsigned Mark() noexcept
{
    try
    {
        const unsigned number = g_marks.fetch_add(1, std::memory_order_relaxed) + 1;
        Emit(kMark, 0, static_cast<int64_t>(number), 0, 0, nullptr);
        return number;
    }
    catch (...)
    {
        return 0;
    }
}

void NoteProvider(unsigned provider) noexcept
{
    try
    {
        if (Global().prov.load(std::memory_order_relaxed) != provider)
            Global().prov.store(provider, std::memory_order_relaxed); // store only on change
    }
    catch (...)
    {
    }
}

void NoteGeneration() noexcept
{
    try
    {
        Global().gen.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void Emit(const char* kind, uint64_t frame, int64_t a, int64_t b, int64_t c, const char* msg) noexcept
{
    try
    {
        State& s = Global();
        if (!s.enabled.load(std::memory_order_relaxed))
            return; // key off: one relaxed load, no thread, no allocation, no file
        if (s.stopRequested.load(std::memory_order_relaxed))
            return;
        Record r;
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        r.qpc = static_cast<uint64_t>(now.QuadPart);
        r.tid = GetCurrentThreadId();
        r.frame = frame;
        r.gen = s.gen.load(std::memory_order_relaxed);
        r.prov = s.prov.load(std::memory_order_relaxed);
        r.a = a;
        r.b = b;
        r.c = c;
        if (kind != nullptr)
            strncpy_s(r.kind, kind, _TRUNCATE);
        if (msg != nullptr)
            strncpy_s(r.msg, msg, _TRUNCATE);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopRequested.load(std::memory_order_relaxed))
            return;
        if (s.queue.size() >= kQueueCap)
        {
            ++s.dropped; // bounded: the writer reports the count, nothing is lost silently
            return;
        }
        const bool wake = s.queue.size() >= kHighWater;
        r.seq = ++s.next;
        s.queue.push_back(r);
        if (wake && s.stopEvent != nullptr)
            SetEvent(s.stopEvent); // auto-reset: the writer runs early when the ring is half full
    }
    catch (...)
    {
    }
}

bool Running() noexcept
{
    try
    {
        State& s = Global();
        if (!s.enabled.load(std::memory_order_relaxed))
            return false;
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.running;
    }
    catch (...)
    {
        return false;
    }
}

unsigned Error() noexcept
{
    try
    {
        return Global().error.load(std::memory_order_relaxed);
    }
    catch (...)
    {
        return 0;
    }
}

bool Alive() noexcept
{
    try
    {
        return Global().alive.load(std::memory_order_relaxed);
    }
    catch (...)
    {
        return false;
    }
}

bool Failed() noexcept
{
    try
    {
        return Global().failed.load(std::memory_order_relaxed); // one relaxed load: the tap never locks
    }
    catch (...)
    {
        return false;
    }
}

void Shutdown(bool freeLibrary) noexcept
{
    (void) freeLibrary; // both detach paths behave the same: a live worker drains on its own, a dead one is taken over
    try
    {
        // Signal, never wait: the worker may need the loader lock (file I/O pulls in drivers), which this
        // detach holds, so waiting on the thread could never succeed. try_lock for the same reason: a thread
        // killed inside the critical section at process exit would deadlock a blocking lock. The pin from the
        // start keeps code a late worker runs mapped.
        State& s = Global();
        std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return; // the worker owns the mutex: it drains, writes session.end and exits on its own
        if (!s.running && s.out == INVALID_HANDLE_VALUE)
            return; // never started, or already stopped: nothing to signal or take
        s.enabled.store(false, std::memory_order_relaxed);
        s.stopRequested.store(true, std::memory_order_relaxed);
        if (s.stopEvent != nullptr)
            SetEvent(s.stopEvent); // wake the worker wherever it waits
        bool dead = !s.running;
        if (s.thread != nullptr)
        {
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeThread(s.thread, &code) != 0 && code != STILL_ACTIVE)
                dead = true;
            CloseHandle(s.thread); // closing the handle does not stop the thread; the flags do that
            s.thread = nullptr;
        }
        if (!dead || s.out == INVALID_HANDLE_VALUE)
            return; // a live worker drains, ends and exits on its own; nothing to take over
        // The writer is already gone (process exit terminates threads first): write what is queued plus the
        // terminal record synchronously through the open handle, bounded by the queue cap, without waiting
        // on anyone. The file pointer is repositioned past any partial tail first. This path runs inside
        // DllMain, so it uses no heap: the tail comes from the buffer preallocated at start, the partial-line
        // cut and WriteBatchFixed work out of fixed stack buffers, and session.end is a fixed line.
        s.queue.drain(s.drained);
        const uint64_t lost = s.dropped;
        s.dropped = 0;
        HANDLE mine = s.out;
        s.out = INVALID_HANDLE_VALUE;
        s.running = false;
        s.alive.store(false, std::memory_order_relaxed);
        lock.unlock(); // file I/O without the mutex
        TruncatePartialLine(mine);
        uint64_t ignored = 0;
        WriteBatchFixed(mine, s.drained, lost, ignored);
        WriteText(mine, kEndLine, sizeof kEndLine - 1, ignored);
        FlushFileBuffers(mine);
        CloseHandle(mine);
    }
    catch (...)
    {
    }
}

} // namespace ext::fgrec
