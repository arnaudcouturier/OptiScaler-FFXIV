// Session cleanup: at start only the newest 5 sessions stay (the sweep keeps
// the newest 4 old ones and the new file brings the total to 5), matched by
// the fork's own file-name pattern and nothing else. Foreign files in the
// folder (other tools' logs, backups, lookalikes, subfolders) are never
// touched, and rotation files (.1/.2) go with their own session. A widened
// pattern or a wrong cap fails here.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
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

static void Touch(const std::wstring& path, const char* text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
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

static bool Exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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

static void SweepSkipsNonFiles(const std::string& repo)
{
    // The sweep only names the exact pattern the recorder writes (an anchored
    // match on the full file name, rotation suffix included) and never deletes
    // directories or reparse points.
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    CHECK(core.find("FILE_ATTRIBUTE_DIRECTORY") != std::string::npos);
    CHECK(core.find("FILE_ATTRIBUTE_REPARSE_POINT") != std::string::npos);
}

static void OwnNames()
{
    // The name parser is the sweep's whole safety: it has to accept exactly the names the recorder writes (base + .1 .. .8) and
    // nothing else. A leading zero (".01", ".008") is a stray copy: accepting it made that file a session key, so it took one of
    // the kept slots and was swept with a session that never wrote it.
    std::wstring session;
    auto own = [&session](const std::wstring& name)
    {
        session.clear();
        return fg::OwnFile(name, session);
    };
    const std::wstring base = L"fg-rec-20260101-000000-000-100.jsonl";
    CHECK(own(base) && session == base);
    for (int r = 1; r <= fg::kKeepRotations; ++r)
        CHECK(own(base + L"." + std::to_wstring(r)) && session == base);
    CHECK(!own(base + L".0"));
    CHECK(!own(base + L".9"));   // past the cap: not a name this recorder writes
    CHECK(!own(base + L".10"));
    CHECK(!own(base + L".01"));  // a leading zero is a stray copy
    CHECK(!own(base + L".008"));
    CHECK(!own(base + L".1."));
    CHECK(!own(base + L".bak"));
    CHECK(!own(L"FG-REC-20260101-000000-000-100.jsonl"));
    CHECK(!own(L"fg-rec-20260101-000000-000-100 - Copy.jsonl"));
    CHECK(!own(L"fg-rec-20260101-000000-000-100.jsonl.1.bak"));
}

static void SessionName(const std::wstring& dir, int i, wchar_t* base)
{
    _snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%s\\fg-rec-2026010%d-000000-000-100.jsonl", dir.c_str(), i);
}

static void SessionCleanup()
{
    CHECK(fg::kKeepSessions == 5); // the documented cap: the newest 5 stay in total
    const std::wstring dir = ft::FreshDir("sweep");

    // seven own-pattern sessions: every second one carries rotations
    for (int i = 1; i <= 7; ++i)
    {
        wchar_t base[MAX_PATH] = {};
        SessionName(dir, i, base);
        Touch(base, "base");
        if (i % 2 == 0)
        {
            Touch(std::wstring(base) + L".1", "one");
            Touch(std::wstring(base) + L".2", "two");
            // a full rotation set: .3 .. .8 belong to the session too, or a long session would leave them behind
            for (int r = 3; r <= fg::kKeepRotations; ++r)
                Touch(std::wstring(base) + L"." + std::to_wstring(r), "more");
        }
    }
    // decoys: foreign files that share the folder. The Explorer copy and the
    // lookalike sort below the own-pattern sessions, so a widened pattern
    // deletes them; the anchored pattern never touches any of these.
    Touch(dir + L"\\notes.txt", "notes");
    Touch(dir + L"\\status.json", "status");
    Touch(dir + L"\\fg-rec-20260101-000000-000-100 - Copy.jsonl", "explorer copy of session 1");
    Touch(dir + L"\\fg-rec-1old.jsonl", "lookalike user file");
    Touch(dir + L"\\fg-rec-x.jsonl.bak", "backup of session x");
    Touch(dir + L"\\FG-REC-20200101-000000-000-100.jsonl", "upper case");
    Touch(dir + L"\\fg-rec-nosuffix", "no suffix");
    Touch(dir + L"\\random.jsonl", "no prefix");
    CHECK(Exists(dir + L"\\notes.txt"));
    // a directory wearing a session-like name: never deleted, never grouped
    CreateDirectoryW((dir + L"\\fg-rec-20200101-000000-000-100.jsonl").c_str(), nullptr);
    CHECK(Exists(dir + L"\\fg-rec-20200101-000000-000-100.jsonl"));

    fg::SweepOldSessions(dir);

    // the sweep runs before the new file exists, so it keeps the newest 4;
    // the new session brings the total to the documented 5
    for (int i = 4; i <= 7; ++i)
    {
        wchar_t base[MAX_PATH] = {};
        SessionName(dir, i, base);
        CHECK(Exists(base));
        if (i % 2 == 0)
        {
            CHECK(Exists(std::wstring(base) + L".1"));
            CHECK(Exists(std::wstring(base) + L".2"));
            CHECK(Exists(std::wstring(base) + L".8")); // the oldest kept rotation is a session file too
        }
    }
    // sessions 1..3 are gone with their rotations: only own-pattern names
    for (int i = 1; i <= 3; ++i)
    {
        wchar_t base[MAX_PATH] = {};
        SessionName(dir, i, base);
        CHECK(!Exists(base));
        CHECK(!Exists(std::wstring(base) + L".1"));
        CHECK(!Exists(std::wstring(base) + L".2"));
        CHECK(!Exists(std::wstring(base) + L".8")); // and a removed session takes every rotation with it
    }
    // every decoy is untouched, byte for byte
    CHECK(ft::ReadBytes(dir + L"\\notes.txt") == "notes");
    CHECK(ft::ReadBytes(dir + L"\\status.json") == "status");
    CHECK(ft::ReadBytes(dir + L"\\fg-rec-20260101-000000-000-100 - Copy.jsonl") == "explorer copy of session 1");
    CHECK(ft::ReadBytes(dir + L"\\fg-rec-1old.jsonl") == "lookalike user file");
    CHECK(ft::ReadBytes(dir + L"\\fg-rec-x.jsonl.bak") == "backup of session x");
    CHECK(ft::ReadBytes(dir + L"\\FG-REC-20200101-000000-000-100.jsonl") == "upper case");
    CHECK(ft::ReadBytes(dir + L"\\fg-rec-nosuffix") == "no suffix");
    CHECK(ft::ReadBytes(dir + L"\\random.jsonl") == "no prefix");
    CHECK(Exists(dir + L"\\fg-rec-20200101-000000-000-100.jsonl"));
    // a stray rotation copy with a leading zero is not a session file of this recorder: it is neither counted toward the kept
    // sessions nor swept with one
    Touch(dir + L"\\fg-rec-20251231-000000-000-100.jsonl.01", "stray rotation copy");
    CHECK(ft::ReadBytes(dir + L"\\fg-rec-20251231-000000-000-100.jsonl.01") == "stray rotation copy");
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    SweepSkipsNonFiles(repo);
    OwnNames();
    SessionCleanup();
    if (g_failed != 0)
    {
        std::printf("fgrec_sweep: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_sweep: all passed\n");
    return 0;
}
