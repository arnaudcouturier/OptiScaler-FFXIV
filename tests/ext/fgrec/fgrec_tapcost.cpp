// H2 + M2: taps stay cheap. With the key on and the worker running, every
// present tap used to rebuild the session info: a 64 KiB stack buffer, module
// table calls, a settings summary and two mutex round trips per tap. The fix
// gathers the info only when a start is actually needed (the slow path runs
// once, when the worker is down) and gates every tap on a cached key plus a
// worker-liveness load. Off taps do one relaxed load and a thread-local tick,
// with a config re-read every 16 taps per thread. This test counts config
// reads over 1000 off taps (a per-tap SyncKey fails here), counts heap blocks
// on the tap thread over 1000 hot on taps (per-tap strings fail here) and
// pins the gate-first tap shape. A rebuilding tap or a missing gate fails.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecSeams.cpp" // the core links separately (see the recipe)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

namespace ext::core
{
const char* ForkVersion() { return "fgrec-test"; }
} // namespace ext::core

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

// Heap blocks on the tap thread only: the worker drains (and allocates) on
// its own thread while the burst runs, so a process-wide count would blame
// the wrong thread.
static DWORD g_tapTid = 0;
static std::atomic<long long> g_tapNews { 0 };

void* operator new(size_t n)
{
    if (g_tapTid != 0 && GetCurrentThreadId() == g_tapTid)
        g_tapNews.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n)
{
    if (g_tapTid != 0 && GetCurrentThreadId() == g_tapTid)
        g_tapNews.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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

static void TapStructurePins(const std::string& repo)
{
    // Gate first, info only on the slow path, module dir cached, liveness by
    // one load: the shape that keeps the hot tap to loads and an enqueue.
    const std::string seams = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecSeams.cpp");
    CHECK(seams.find("bool TapEnter()") != std::string::npos);
    CHECK(seams.find("if (!TapEnter())") != std::string::npos);
    CHECK(seams.find("void BuildSessionInfo(SessionInfo& info, std::string& summaryStore)") != std::string::npos);
    CHECK(seams.find("static const std::wstring dir = ThisModuleDir();") != std::string::npos);
    CHECK(seams.find("if (ext::fgrec::Alive())") != std::string::npos);
}

static void KeyOffBarelyReadsConfig()
{
    ext::Cfg().framePacing.fgRecorder = false;
    for (int i = 0; i < 20; ++i)
        fg::FgPresentBegin(false, 0, 0); // settle the poll state with the key off
    ext::CfgReads() = 0;
    for (int i = 0; i < 1000; ++i)
        fg::FgPresentBegin(true, 1, 0); // off: gate, tick, no record
    const unsigned reads = ext::CfgReads();
    std::printf("config reads over 1000 key-off taps: %u\n", reads);
    CHECK(reads <= 100); // a rate-limited re-read, never once per tap
    CHECK(!fg::RecArmed());
}

static std::wstring RecDir()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring dir = exe;
    if (const size_t slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos)
        dir.resize(slash);
    return dir + L"\\fg-recordings";
}

static std::wstring g_recDir;

static bool FileSettled()
{
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((g_recDir + L"\\fg-rec-*.jsonl").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const std::string bytes = ft::ReadBytes(g_recDir + L"\\" + f.cFileName);
    FindClose(h);
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    if (lines.empty())
        return false;
    bool ok = false;
    ft::JsonValue v = ft::ParseLine(lines.back(), ok);
    return ok && ft::IsText(ft::Find(v, "kind"), "session.end");
}

static void KeyOnHotTapsAllocateNothing()
{
    ext::Cfg().framePacing.fgRecorder = true;
    State::Instance().gameExe = "ffxiv_dx11.exe";
    State::Instance().fgLastFrame = 7;
    State::Instance().frameCount = 8;
    State::Instance().activeFgOutput = FGOutput::DLSSG;
    ext::game::TestGame().lighting = reinterpret_cast<const void*>(1);
    for (int i = 0; i < 20; ++i)
        fg::FgPresentBegin(false, 0, 0); // prime the cached key (and start the session)
    CHECK(fg::RecArmed());
    CHECK(fg::Running());
    for (int i = 0; i < 10; ++i)
        fg::FgPresentBegin(true, 1, 0); // warm up: first-use costs land here, not in the count
    g_tapTid = GetCurrentThreadId();
    g_tapNews.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
        fg::FgPresentBegin(true, 1, 0); // on and running: loads and an enqueue, no strings
    const long long news = g_tapNews.load(std::memory_order_relaxed);
    g_tapTid = 0;
    std::printf("tap-thread heap blocks over 1000 hot taps: %lld\n", news);
    CHECK(news == 0); // no stack buffers, no module calls, no string building per tap

    ext::Cfg().framePacing.fgRecorder = false;
    for (int i = 0; i < 20; ++i)
        fg::FgPresentBegin(true, 1, 0); // disarm and stop the worker (real taps: the off key must stop it)
    g_recDir = RecDir();
    CHECK(ft::WaitUntil(FileSettled, 30000));
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((g_recDir + L"\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE)
    {
        const std::string bytes = ft::ReadBytes(g_recDir + L"\\" + f.cFileName);
        FindClose(h);
        bool ok = false;
        ft::JsonValue begin = ft::ParseLine(ft::SplitLines(bytes).front(), ok);
        CHECK(ok && ft::IsText(ft::Find(begin, "kind"), "session.begin"));
        const ft::JsonValue* summary = ft::Find(begin, "summary");
        CHECK(summary != nullptr && summary->type == ft::JsonValue::Type::Text && !summary->text.empty());
    }
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TapStructurePins(repo);
    KeyOffBarelyReadsConfig();
    KeyOnHotTapsAllocateNothing();
    if (g_failed != 0)
    {
        std::printf("fgrec_tapcost: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_tapcost: all passed\n");
    return 0;
}
