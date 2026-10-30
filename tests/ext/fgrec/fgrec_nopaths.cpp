// The no-paths rule: no record carries an absolute path, a user name or a
// machine name, and a module is named by file name only. session.begin holds
// exactly the documented fields (no folder, no directory), the exe arrives as
// a file name (Util.cpp assigns exePathFilename), and the folder part of a
// module path is stripped at the seam. A record that leaks the recordings
// folder, a drive letter or the module directory fails here.
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

static void ExeArrivesAsFileName(const std::string& repo)
{
    // The only exe text the recorder can emit comes from State::gameExe,
    // which Util.cpp sets to the file name, never a path.
    const std::string util = ReadSource(repo, "/OptiScaler/Util.cpp");
    CHECK(util.find("State::Instance().gameExe = exePathFilename;") != std::string::npos);
}

static void BeginCarriesNoFolder(const std::string& repo)
{
    // session.begin is built from short file content only (version, names,
    // settings): the recordings folder and the module directory are local
    // and never emitted.
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    const size_t worker = core.find("WorkerProc(void*)");
    CHECK(worker != std::string::npos);
    const size_t at = core.find("session.begin", worker);
    CHECK(at != std::string::npos);
    const std::string built = core.substr(at, 1200);
    for (const char* field : { "qpc_frequency", "pid", "utc", "version", "exe", "build", "summary" })
        CHECK(built.find(field) != std::string::npos);
    for (const char* banned : { "moduleDir", "fg-recordings", "GetModuleFileName", "GetTempPath", "Users" })
        CHECK(built.find(banned) == std::string::npos);
}

static void ModuleStripLoop(const std::string& repo)
{
    // OnModule keeps what follows the last separator: a module is recorded
    // by file name only, never a path.
    const std::string seams = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecSeams.cpp");
    const size_t at = seams.find("void OnModule(");
    CHECK(at != std::string::npos);
    const std::string body = seams.substr(at, 900);
    CHECK(body.find("base = p + 1;") != std::string::npos);
    CHECK(body.find("GetModuleFileName") == std::string::npos);
}

static void SessionFileHoldsNoPaths()
{
    const std::wstring dir = ft::FreshDir("nopaths");
    fg::SessionInfo info;
    const std::string v = "t", e = "ffxiv_dx11.exe", b = "matched", sum = "fg=dlssg automfg=off";
    info.version = v.c_str();
    info.exe = e.c_str();
    info.build = b.c_str();
    info.summary = sum.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    fg::Emit(fg::kSleep, 1, 10, 0, 0, "reflex");
    fg::EnsureStarted(false, fg::SessionInfo {});
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
    // the folder itself never lands in a record: no drive letter, no Users
    // segment, no recorded directory next to the DLL
    CHECK(bytes.find(":\\") == std::string::npos);
    CHECK(bytes.find("Users") == std::string::npos);
    CHECK(bytes.find("fg-recordings") == std::string::npos);
    for (const auto& line : ft::SplitLines(bytes))
    {
        bool ok = false;
        ft::ParseLine(line, ok);
        CHECK(ok);
    }
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    ExeArrivesAsFileName(repo);
    BeginCarriesNoFolder(repo);
    ModuleStripLoop(repo);
    SessionFileHoldsNoPaths();
    if (g_failed != 0)
    {
        std::printf("fgrec_nopaths: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_nopaths: all passed\n");
    return 0;
}
