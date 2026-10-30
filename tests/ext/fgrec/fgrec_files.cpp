// File rules: long directories, the 5-session cap, loud rotation failures and
// unique session names. L1 used to check dir.size() + 16 against MAX_PATH,
// which ignores the real file path (dir + fg-recordings + the stamped name +
// rotation suffix): DLL dirs of 209-243 characters failed the file open with
// no useful code, and 244+ returned silently with no file at all. L2 kept 6
// sessions (the sweep ran before the new file existed). L7 truncated the base
// when a rotation move failed. L10 truncated the previous session when two
// sessions shared one UTC second. A missing file, a sixth session, a silent
// truncation or a colliding name fails here.
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

static bool IsDigits(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// The stamped name the recorder writes: fg-rec-<8>-<6>-<3>-<pid>.jsonl
// (date, UTC time, milliseconds, pid). The millisecond group is what makes a
// key off/on within one second a new file instead of a truncation.
static bool IsSessionName(const std::string& name)
{
    if (name.size() < 30 || name.compare(0, 7, "fg-rec-") != 0)
        return false;
    if (name.compare(name.size() - 6, 6, ".jsonl") != 0)
        return false;
    const std::string stem = name.substr(7, name.size() - 7 - 6);
    const size_t d1 = stem.find('-'), d2 = stem.find('-', d1 + 1), d3 = stem.find('-', d2 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos || d3 == std::string::npos)
        return false;
    if (stem.find('-', d3 + 1) != std::string::npos)
        return false;
    return stem.size() == d3 + 1 + (stem.size() - d3 - 1) && d1 == 8 && d2 - d1 - 1 == 6 && d3 - d2 - 1 == 3 &&
           IsDigits(stem.substr(0, 8)) && IsDigits(stem.substr(9, 6)) && IsDigits(stem.substr(16, 3)) &&
           IsDigits(stem.substr(20));
}

static std::vector<std::wstring> FindSessions(const std::wstring& dir)
{
    std::vector<std::wstring> bases;
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return bases;
    do
    {
        std::wstring name = f.cFileName;
        for (int r = 1; r <= 2; ++r)
        {
            const std::wstring suf = r == 1 ? L".1" : L".2";
            if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0)
                name.resize(name.size() - suf.size());
        }
        if (std::find(bases.begin(), bases.end(), name) == bases.end())
            bases.push_back(name);
    } while (FindNextFileW(h, &f) != 0);
    FindClose(h);
    return bases;
}

static void Touch(const std::wstring& path, const char* text)
{
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        std::printf("FAIL: cannot write %ls\n", path.c_str());
        ++g_failed;
        return;
    }
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(h);
}

static void LongDirectoryGetsAFile()
{
    // A DLL dir whose real file path passes MAX_PATH still records: the
    // worker uses the \\?\ prefix instead of failing the open or, worse,
    // returning silently with no file.
    std::wstring deep = ft::FreshDir("long");
    for (int i = 0; i < 24; ++i)
        deep += L"\\0123456789";
    for (size_t i = 0; i < deep.size(); ++i)
        if (deep[i] == L'\\')
            CreateDirectoryW((L"\\\\?\\" + deep.substr(0, i)).c_str(), nullptr);
    CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr);
    CHECK(GetFileAttributesW((L"\\\\?\\" + deep).c_str()) != INVALID_FILE_ATTRIBUTES);

    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = deep.c_str();
    fg::EnsureStarted(true, info);
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "deep");
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 60000));
    CHECK(fg::Error() == 0);
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((L"\\\\?\\" + deep + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE); // a long dir still yields a session file
    if (h != INVALID_HANDLE_VALUE)
        FindClose(h);
}

static void SweepKeepsFiveTotal()
{
    // Seven own-pattern sessions on disk, then a real start: the sweep (now in
    // the worker, before the new file exists) keeps the newest 4, and the new
    // file brings the total to exactly 5.
    const std::wstring dir = ft::FreshDir("five");
    for (int i = 1; i <= 7; ++i)
    {
        wchar_t base[MAX_PATH] = {};
        _snwprintf_s(base, _TRUNCATE, L"%s\\fg-recordings\\fg-rec-2026010%d-000000-000-100.jsonl", dir.c_str(), i);
        CreateDirectoryW((dir + L"\\fg-recordings").c_str(), nullptr);
        Touch(base, "base");
    }
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "five");
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 60000));
    const std::vector<std::wstring> sessions = FindSessions(dir);
    std::printf("sessions after sweep+start: %zu\n", sessions.size());
    CHECK(sessions.size() == 5); // four old plus the new one, never six
}

static void LockedRotationFailsLoud()
{
    // A .2 the reader holds open (no share) used to be deleted/moved around
    // silently and the base truncated: 16 MiB lost with success reported.
    // Now the rotation fails instead of truncating.
    const std::wstring dir = ft::FreshDir("locked");
    const std::wstring base = dir + L"\\fg-rec-20260101-000000-000-7.jsonl";
    Touch(base, "base-content\n");
    Touch(base + L".1", "one-content\n");
    Touch(base + L".2", "two-content\n");
    HANDLE locked = CreateFileW((base + L".2").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    HANDLE out = CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(out != INVALID_HANDLE_VALUE);
    uint64_t bytes = fg::kRotateBytes + 10;
    CHECK(!fg::Rotate(out, base, 1234, bytes)); // loud failure, not a silent truncation
    CHECK(ft::ReadBytes(base) == "base-content\n"); // the base is untouched
    CloseHandle(out);
    CloseHandle(locked);
}

static void SessionNameIsUnique()
{
    const std::wstring dir = ft::FreshDir("unique");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    for (int i = 0; i < 3; ++i)
    {
        fg::EnsureStarted(true, info);
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "u");
        fg::EnsureStarted(false, fg::SessionInfo {});
        CHECK(ft::WaitUntil(WorkerGone, 60000));
    }
    const std::vector<std::wstring> sessions = FindSessions(dir);
    CHECK(sessions.size() == 3); // rapid restarts never truncate each other
    for (const auto& name : sessions)
    {
        char narrow[MAX_PATH] = {};
        std::snprintf(narrow, sizeof narrow, "%ls", name.c_str());
        CHECK(IsSessionName(narrow)); // millisecond group present
    }
}

// L8: a start without a module folder cannot name a file. It used to allocate the 1 MiB ring and then stay
// disarmed, so every later tap re-ran the whole slow path for nothing. Now the folder is checked first and the
// failure is latched (error 7) like every other start failure, so the menu says so and the tap stops retrying.
static void UnknownModuleFolderAllocatesNothing()
{
    fg::State& s = fg::Global();
    CHECK(s.queue.capacity() == 0); // first case in this binary: nothing is preallocated yet
    fg::SessionInfo info; // moduleDir stays nullptr
    fg::EnsureStarted(true, info);
    CHECK(!fg::Running());
    CHECK(s.queue.capacity() == 0); // no queue was built for a session that cannot name its file
    CHECK(fg::Failed());
    CHECK(fg::Error() == 7);
    fg::EnsureStarted(false, fg::SessionInfo {}); // the key off clears the latch, the code stays for the menu
    CHECK(!fg::Failed());
    CHECK(fg::Error() == 7);
}

// L9: a module folder on a network share past MAX_PATH needs the \\?\UNC\ form; a plain \\?\ prefix in front of
// \\server\share is not a valid path, so the file open would fail where a local folder works.
static void LongUncFolderUsesTheExtendedForm()
{
    std::wstring unc = L"\\\\srv\\share";
    for (int i = 0; i < 30; ++i)
        unc += L"\\0123456789"; // well past MAX_PATH
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = unc.c_str();
    fg::EnsureStarted(true, info);
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        dir = fg::Global().dir;
    }
    CHECK(dir.compare(0, 8, L"\\\\?\\UNC\\") == 0); // the extended UNC form
    CHECK(dir.find(L"\\\\?\\\\\\") == std::wstring::npos); // never \\?\ in front of \\server
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000)); // the share does not exist: the worker latches its failure
}

int main()
{
    UnknownModuleFolderAllocatesNothing();
    LongDirectoryGetsAFile();
    SweepKeepsFiveTotal();
    LockedRotationFailsLoud();
    SessionNameIsUnique();
    LongUncFolderUsesTheExtendedForm();
    if (g_failed != 0)
    {
        std::printf("fgrec_files: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_files: all passed\n");
    return 0;
}
