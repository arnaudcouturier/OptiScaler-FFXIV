// Rotation at the size limit and the 3-file session cap: each session file
// rotates at 16 MiB (base to .1, .1 to .2, the oldest dropped), so a session
// never holds more than base + .1 + .2. The first case drives the real Rotate
// on fixture files (contents shift, the oldest is dropped, the fresh base
// opens with the continuation line); the second writes past the real 16 MiB
// limit through the public path and proves the rotation happened. A wrong
// threshold, a lost rotation or a fourth file fails here.
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

static void WriteText(const std::wstring& path, const char* text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(h);
}

static void RotateShiftsThreeFiles()
{
    CHECK(fg::kRotateBytes == 16ull * 1024 * 1024); // the documented limit
    CHECK(fg::kKeepRotations == 2); // base + .1 + .2, never a fourth file
    const std::wstring dir = ft::FreshDir("rotate");
    const std::wstring base = dir + L"\\fg-rec-20260101-000000-7.jsonl";
    WriteText(base, "base-content\n");
    WriteText(base + L".1", "one-content\n");
    WriteText(base + L".2", "two-content\n");

    HANDLE out = CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(out != INVALID_HANDLE_VALUE);
    uint64_t bytes = fg::kRotateBytes + 10;
    CHECK(fg::Rotate(out, base, 1234, bytes));
    CloseHandle(out);
    CHECK(bytes < 1024); // the fresh base only holds the continuation line

    CHECK(ft::ReadBytes(base + L".2") == "one-content\n"); // .1 became .2, the oldest is gone
    CHECK(ft::ReadBytes(base + L".1") == "base-content\n"); // the base became .1
    const std::string fresh = ft::ReadBytes(base);
    bool ok = false;
    ft::JsonValue v = ft::ParseLine(ft::SplitLines(fresh).front(), ok);
    CHECK(ok && ft::IsText(ft::Find(v, "kind"), "continuation"));
    const ft::JsonValue* freq = ft::Find(v, "qpc_frequency");
    CHECK(freq != nullptr && freq->type == ft::JsonValue::Type::Number && freq->text == "1234");

    // a second rotation keeps exactly three files with shifted contents, and the helper the worker uses
    // publishes the handle it opened (the stop path then never names the closed one)
    out = CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    bytes = fg::kRotateBytes;
    CHECK(fg::RotateAndPublish(fg::Global(), out, base, 7, bytes));
    CHECK(out != INVALID_HANDLE_VALUE);
    CHECK(fg::Global().out == out); // the rotation published the live handle
    CloseHandle(out);
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        fg::Global().out = INVALID_HANDLE_VALUE;
    }
    CHECK(ft::ReadBytes(base + L".2") == "base-content\n");
    CHECK(ft::ReadBytes(base + L".1").find("continuation") != std::string::npos);
    WIN32_FIND_DATAW f {};
    int count = 0;
    HANDLE h = FindFirstFileW((dir + L"\\fg-rec-*.jsonl*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            ++count;
        } while (FindNextFileW(h, &f) != 0);
        FindClose(h);
    }
    CHECK(count == 3);
}

static void RotationAtTheLimit()
{
    const std::wstring dir = ft::FreshDir("limit");
    fg::SessionInfo info;
    const std::string v = "t", e = "t.exe", b = "t", sum = "t";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    // Paced past the limit: a tight burst would outrun the writer and drop
    // most events, so each chunk waits for the drain (chunks stay under the
    // cap, hence nothing drops). Full-size messages (~200 bytes a line) keep
    // the run short: 90k lines clear the 16 MiB limit once.
    const std::string big(95, 'x');
    for (int base = 0; base < 90000; base += 4000)
    {
        for (int j = base; j < base + 4000 && j < 90000; ++j)
            fg::Emit(fg::kSleep, static_cast<uint64_t>(j), j, 0, 0, big.c_str());
        for (;;) // wait for the drain: chunks stay under the cap, so nothing drops
        {
            bool empty = false;
            {
                std::lock_guard<std::mutex> lock(fg::Global().mutex);
                empty = fg::Global().queue.empty();
            }
            if (empty)
                break;
            Sleep(10);
        }
    }
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 240000));

    const std::wstring sub = dir + L"\\fg-recordings\\";
    WIN32_FIND_DATAW f {};
    bool haveBase = false, haveOne = false, haveTwo = false;
    uint64_t oneSize = 0;
    HANDLE h = FindFirstFileW((sub + L"fg-rec-*.jsonl*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = f.cFileName;
            const std::wstring path = sub + name;
            LARGE_INTEGER size {};
            HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (r != INVALID_HANDLE_VALUE)
            {
                GetFileSizeEx(r, &size);
                CloseHandle(r);
            }
            if (name.size() >= 6 && name.compare(name.size() - 2, 2, L".1") == 0)
            {
                haveOne = true;
                oneSize = static_cast<uint64_t>(size.QuadPart);
            }
            else if (name.size() >= 6 && name.compare(name.size() - 2, 2, L".2") == 0)
                haveTwo = true;
            else
                haveBase = true;
        } while (FindNextFileW(h, &f) != 0);
        FindClose(h);
    }
    CHECK(haveBase && haveOne && !haveTwo); // exactly one rotation: base + .1
    CHECK(oneSize >= 16ull * 1024 * 1024); // the rotation happened at the limit
}

static void LockedRotationKeepsTheBaseAndNoHandle()
{
    // L7 coverage with the handle rule on top: the rotation target .2 is held open by a
    // reader, so the rotation must fail instead of deleting it and truncating the base
    // (16 MiB lost with success reported), and the failure must leave no handle open and
    // none published (the stop path would otherwise write into a closed handle).
    const std::wstring dir = ft::FreshDir("locked");
    const std::wstring base = dir + L"\\fg-rec-20260101-000000-000-7.jsonl";
    WriteText(base, "base-content\n");
    WriteText(base + L".1", "one-content\n");
    WriteText(base + L".2", "two-content\n");
    HANDLE locked = CreateFileW((base + L".2").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    HANDLE out = CreateFileW(base.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(out != INVALID_HANDLE_VALUE);
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        fg::Global().out = out; // what a running worker publishes
    }
    uint64_t bytes = fg::kRotateBytes + 10;
    CHECK(!fg::RotateAndPublish(fg::Global(), out, base, 1234, bytes));
    CHECK(out == INVALID_HANDLE_VALUE); // nothing this call opened stays open
    {
        std::lock_guard<std::mutex> lock(fg::Global().mutex);
        CHECK(fg::Global().out == INVALID_HANDLE_VALUE); // and nothing stays published
    }
    CHECK(ft::ReadBytes(base) == "base-content\n"); // the base was never truncated
    CHECK(ft::ReadBytes(base + L".1") == "one-content\n");
    CHECK(ft::ReadBytes(base + L".2") == "two-content\n"); // the held file was never deleted
    CloseHandle(locked);
}

int main()
{
    RotateShiftsThreeFiles();
    RotationAtTheLimit();
    LockedRotationKeepsTheBaseAndNoHandle();
    if (g_failed != 0)
    {
        std::printf("fgrec_rotate: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_rotate: all passed\n");
    return 0;
}
