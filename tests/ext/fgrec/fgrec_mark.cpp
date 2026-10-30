// The recorder's one-key mark (ext/fgrec::Mark): every press writes exactly one `mark` record into the session file, the record's
// `a` is the number the user saw in the log line ("user mark N"), the numbers keep counting across a key-off/key-on of the
// recorder (the user's count, not the file's), and with the recorder off a press writes nothing but still returns the next number.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
#include <cstdlib>
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

namespace
{
bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

std::wstring SessionFile(const std::wstring& dir)
{
    std::wstring found;
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return found;
    found = dir + L"\\fg-recordings\\" + f.cFileName;
    FindClose(h);
    return found;
}

// The `a` values of the mark records of one file, in file order
std::vector<long long> Marks(const std::wstring& file)
{
    std::vector<long long> marks;
    const std::string bytes = ft::ReadBytes(file);
    for (const std::string& line : ft::SplitLines(bytes))
    {
        bool ok = false;
        ft::JsonValue v = ft::ParseLine(line, ok);
        if (!ok || !ft::IsText(ft::Find(v, "kind"), "mark"))
            continue;
        const ft::JsonValue* a = ft::Find(v, "a");
        if (a != nullptr && a->type == ft::JsonValue::Type::Number)
            marks.push_back(std::strtoll(a->text.c_str(), nullptr, 10));
    }
    return marks;
}

void TestMarks()
{
    CHECK(std::string(fg::kMark) == "mark");

    // With the recorder off a press still counts, and writes nothing (there is no file to write to)
    CHECK(fg::Mark() == 1);
    CHECK(fg::Mark() == 2);

    const std::wstring dir = ft::FreshDir("mark");
    fg::SessionInfo info;
    const std::string v = "7.0-test", e = "ffxiv_dx11.exe", b = "matched", sum = "recorder on";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());

    // The numbers continue from the presses made while the recorder was off: they are the user's marks, not the file's
    CHECK(fg::Mark() == 3);
    CHECK(fg::Mark() == 4);

    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    const std::wstring file = SessionFile(dir);
    CHECK(!file.empty());
    if (!file.empty())
    {
        CHECK(ft::WaitForSessionEnd(file, 30000));
        const std::vector<long long> marks = Marks(file);
        CHECK(marks.size() == 2);
        if (marks.size() == 2)
            CHECK(marks[0] == 3 && marks[1] == 4);

        // ... and a press after the session stopped writes nothing into the closed file
        CHECK(fg::Mark() == 5);
        CHECK(Marks(file).size() == 2);
    }

    // A second session starts at 6: the counter belongs to the module, so two recordings of one run can be put in order
    const std::wstring second = ft::FreshDir("mark2");
    info.moduleDir = second.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Mark() == 6);
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    const std::wstring secondFile = SessionFile(second);
    CHECK(!secondFile.empty());
    if (!secondFile.empty())
    {
        CHECK(ft::WaitForSessionEnd(secondFile, 30000));
        const std::vector<long long> marks = Marks(secondFile);
        CHECK(marks.size() == 1 && marks[0] == 6);
    }
}
} // namespace

int main()
{
    TestMarks();
    if (g_failed != 0)
    {
        std::printf("fgrec_mark: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_mark: all passed\n");
    return 0;
}
