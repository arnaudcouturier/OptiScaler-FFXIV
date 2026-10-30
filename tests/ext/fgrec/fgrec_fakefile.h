#pragma once
// A stand-in for the file calls the recorder core makes, so a test can tell which handle each write went to.
// Windows hands a freed handle value straight back to the next open, so on real files a stale handle and a
// live one look identical; here every open gets a fresh id, and closing an id twice or writing, flushing,
// seeking or truncating a closed one is counted as a violation with its id and operation.
//
// One lock guards the whole layer: a test reads the contents and the handle table from its own thread while
// the worker writes, renames and rotates from the writer thread, so every entry point below takes it, and the
// helpers that run inside an entry point (Find, Live, Violation) never do.
//
// Usage: include this header, then FGrecFakeFileLayer() (it defines the core's file calls to the fakes), then
// the core source, then FGrecFakeFileLayerEnd(). Paths are ASCII in these tests; the file contents live in
// memory and the real temp folder is never touched by the core.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace fgrecfake
{
struct Handle
{
    std::string path;
    uint64_t pos = 0;
    bool open = true;
};

struct Layer
{
    std::mutex mutex; // one lock for the maps, the strings and the counters below
    std::map<std::string, std::string> files; // path -> content
    std::map<uintptr_t, Handle> handles; // id -> handle state (closed ones stay, so a late use is visible)
    uintptr_t nextId = 0x1000; // never reused: a stale id can never alias a live one
    int violations = 0;
    std::string violationsText;
    uint64_t writes = 0;
    uint64_t flushes = 0;
    // Hook for a test that needs the worker to die mid-operation: the next rename terminates that thread there
    // (a rotation sits between two renames and holds no recorder lock at that point). The hook runs before the
    // lock is taken, so the layer stays usable for the takeover that follows.
    std::atomic<bool> killOnMove { false };
    std::atomic<bool> killed { false };
    HANDLE killThread = nullptr;
};

inline Layer& State()
{
    static Layer layer;
    return layer;
}

// The layer's lock, exposed so a test can prove that a read waits for a writer.
inline std::mutex& LayerLock() { return State().mutex; }

inline std::string Narrow(const wchar_t* path)
{
    std::string text;
    for (const wchar_t* p = path; p != nullptr && *p != 0; ++p)
        text.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    return text;
}

inline std::string Narrow(const std::wstring& path) { return Narrow(path.c_str()); }

// Below: callers hold the layer's lock.
inline void Violation(const char* what, HANDLE h)
{
    Layer& layer = State();
    ++layer.violations;
    char text[128] = {};
    std::snprintf(text, sizeof text, "%s on handle %p", what, static_cast<void*>(h));
    if (!layer.violationsText.empty())
        layer.violationsText += "; ";
    layer.violationsText += text;
}

inline Handle* Find(HANDLE h)
{
    const auto it = State().handles.find(reinterpret_cast<uintptr_t>(h));
    return it == State().handles.end() ? nullptr : &it->second;
}

// A live (open) handle, or nullptr after a violation.
inline Handle* Live(HANDLE h, const char* what)
{
    Handle* handle = Find(h);
    if (handle == nullptr || !handle->open)
    {
        Violation(what, h);
        return nullptr;
    }
    return handle;
}

// Test-side helpers (the fakes themselves are reached through the macros below).
inline void Reset()
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Layer& layer = State();
    layer.files.clear();
    layer.handles.clear();
    layer.violations = 0;
    layer.violationsText.clear();
    layer.writes = 0;
    layer.flushes = 0;
}

inline int Violations()
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().violations;
}

inline std::string ViolationsText()
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().violationsText;
}

inline uint64_t Writes()
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().writes;
}

// Arms the mid-operation kill: the next rename terminates that thread where it stands.
inline void ArmThreadKill(HANDLE thread)
{
    Layer& layer = State();
    layer.killThread = thread;
    layer.killOnMove.store(true, std::memory_order_release);
}

inline bool Killed() { return State().killed.load(std::memory_order_acquire); }

// Reads never create an entry: a lookup that inserted an empty file would make the layer report the path as
// existing, and the recorder would then bump its session name instead of writing the file it named.
inline std::string Contents(const std::wstring& path)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    const auto it = State().files.find(Narrow(path));
    return it == State().files.end() ? std::string() : it->second;
}

inline bool Exists(const std::wstring& path)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().files.count(Narrow(path)) != 0;
}

// Room for a file the test is about to hand over (the layer's own growth then never touches the heap count).
inline void Reserve(const std::wstring& path, size_t bytes)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    State().files[Narrow(path)].reserve(bytes);
}

// The id of the newest handle opened for that path (0 when none was ever opened).
inline HANDLE LiveHandle(const std::wstring& path)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    const std::string key = Narrow(path);
    HANDLE found = nullptr;
    for (const auto& entry : State().handles)
        if (entry.second.path == key && entry.second.open)
            found = reinterpret_cast<HANDLE>(entry.first);
    return found;
}

inline int OpenHandles(const std::wstring& path)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    const std::string key = Narrow(path);
    int open = 0;
    for (const auto& entry : State().handles)
        if (entry.second.path == key && entry.second.open)
            ++open;
    return open;
}

inline HANDLE FakeCreateFileW(LPCWSTR rawPath, DWORD, DWORD, void*, DWORD disposition, DWORD, HANDLE)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Layer& layer = State();
    const std::string path = Narrow(rawPath);
    const bool exists = layer.files.count(path) != 0;
    if (disposition == CREATE_NEW && exists)
    {
        SetLastError(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition == OPEN_EXISTING && !exists)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition == CREATE_ALWAYS || disposition == CREATE_NEW)
        layer.files[path].clear();
    const uintptr_t id = layer.nextId++;
    layer.handles[id] = Handle { path, 0, true };
    return reinterpret_cast<HANDLE>(id);
}

inline BOOL FakeWriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, void*)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Handle* handle = Live(h, "write");
    if (handle == nullptr)
        return FALSE;
    std::string& data = State().files[handle->path];
    if (data.size() < handle->pos + length)
        data.resize(static_cast<size_t>(handle->pos) + length);
    std::memcpy(data.data() + handle->pos, buffer, length);
    handle->pos += length;
    ++State().writes;
    if (written != nullptr)
        *written = length;
    return TRUE;
}

inline BOOL FakeReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, void*)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Handle* handle = Live(h, "read");
    if (handle == nullptr)
        return FALSE;
    const std::string& data = State().files[handle->path];
    const uint64_t left = data.size() > handle->pos ? data.size() - handle->pos : 0;
    const DWORD n = static_cast<DWORD>(left < length ? left : length);
    if (n != 0)
        std::memcpy(buffer, data.data() + handle->pos, n);
    handle->pos += n;
    if (read != nullptr)
        *read = n;
    return TRUE;
}

inline BOOL FakeFlushFileBuffers(HANDLE h)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    if (Live(h, "flush") == nullptr)
        return FALSE;
    ++State().flushes;
    return TRUE;
}

inline BOOL FakeCloseHandle(HANDLE h)
{
    {
        std::lock_guard<std::mutex> guard(LayerLock());
        Handle* handle = Find(h);
        if (handle == nullptr)
        {
            // a handle the layer never handed out (the worker thread, the event): real close, outside the lock
        }
        else if (!handle->open)
        {
            Violation("close", h);
            return FALSE;
        }
        else
        {
            handle->open = false;
            return TRUE;
        }
    }
    return ::CloseHandle(h);
}

inline BOOL FakeGetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Handle* handle = Live(h, "size");
    if (handle == nullptr)
        return FALSE;
    size->QuadPart = static_cast<LONGLONG>(State().files[handle->path].size());
    return TRUE;
}

inline BOOL FakeSetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER newPos, DWORD method)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Handle* handle = Live(h, "seek");
    if (handle == nullptr)
        return FALSE;
    const int64_t base = method == FILE_BEGIN ? 0
                         : method == FILE_CURRENT ? static_cast<int64_t>(handle->pos)
                                                  : static_cast<int64_t>(State().files[handle->path].size());
    const int64_t next = base + distance.QuadPart;
    handle->pos = next > 0 ? static_cast<uint64_t>(next) : 0;
    if (newPos != nullptr)
        newPos->QuadPart = static_cast<LONGLONG>(handle->pos);
    return TRUE;
}

inline BOOL FakeSetEndOfFile(HANDLE h)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    Handle* handle = Live(h, "truncate");
    if (handle == nullptr)
        return FALSE;
    State().files[handle->path].resize(static_cast<size_t>(handle->pos));
    return TRUE;
}

inline DWORD FakeGetFileAttributesW(LPCWSTR rawPath)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().files.count(Narrow(rawPath)) != 0 ? FILE_ATTRIBUTE_NORMAL : INVALID_FILE_ATTRIBUTES;
}

inline BOOL FakeDeleteFileW(LPCWSTR rawPath)
{
    std::lock_guard<std::mutex> guard(LayerLock());
    return State().files.erase(Narrow(rawPath)) != 0 ? TRUE : FALSE;
}

inline BOOL FakeMoveFileW(LPCWSTR from, LPCWSTR to)
{
    // The kill hook runs before the lock: the thread that dies here holds nothing, so the takeover that follows
    // can still read the layer.
    if (State().killOnMove.exchange(false))
    {
        State().killed.store(true, std::memory_order_release);
        TerminateThread(State().killThread, 1); // never returns
    }
    std::lock_guard<std::mutex> guard(LayerLock());
    Layer& layer = State();
    const std::string source = Narrow(from), target = Narrow(to);
    const auto it = layer.files.find(source);
    if (it == layer.files.end())
        return FALSE;
    layer.files[target] = it->second;
    layer.files.erase(it);
    return TRUE;
}

// Opens (or creates) a file and returns its live id: the same shape as the recorder's own open, for tests
// that prepare a file and hand it to the core.
inline HANDLE Open(const std::wstring& path, DWORD disposition = CREATE_ALWAYS)
{
    return FakeCreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline void Write(HANDLE h, const char* text)
{
    DWORD written = 0;
    FakeWriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}
} // namespace fgrecfake

// The core's file calls, redirected to the layer above. Define FGREC_FAKE_FILE_LAYER before including this
// header, then include the core source: only the recorder's own file calls are replaced, so the test keeps the
// real directory, clock, thread and parse helpers. The defines stay for the rest of the translation unit, so a
// test that wants a real file must use the standard C library (fopen) or open it through the layer.
#ifdef FGREC_FAKE_FILE_LAYER
#define CreateFileW fgrecfake::FakeCreateFileW
#define WriteFile fgrecfake::FakeWriteFile
#define ReadFile fgrecfake::FakeReadFile
#define FlushFileBuffers fgrecfake::FakeFlushFileBuffers
#define CloseHandle fgrecfake::FakeCloseHandle
#define GetFileSizeEx fgrecfake::FakeGetFileSizeEx
#define SetFilePointerEx fgrecfake::FakeSetFilePointerEx
#define SetEndOfFile fgrecfake::FakeSetEndOfFile
#define GetFileAttributesW fgrecfake::FakeGetFileAttributesW
#define DeleteFileW fgrecfake::FakeDeleteFileW
#define MoveFileW fgrecfake::FakeMoveFileW
#endif
