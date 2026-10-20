// The REAL picker glue (ext/nr/runtime/NrRuntimeCore.h) on temporary folders with a fake environment (clock, config value, log):
// catalog build, selection polled per frame, rescan, the render thread never waiting for a disk scan (review M3), the hash worker that
// never logs and does not re-read what it could not hash (M4), fail-open scan (L2), the session lock and its pending note (L1).
#include "ext/nr/runtime/NrRuntimeCore.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

using namespace ext::nr::runtime;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static const fs::path kFile = L"nvngx_dlssnr.dll";

static void Put(const fs::path& file, const std::string& content)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

struct World
{
    fs::path root, game, optiscaler;
    // the fake environment
    std::string requested;
    std::atomic<uint64_t> now { 1000 };
    std::atomic<bool> hybrid { false };
    std::atomic<bool> amd { false };
    std::mutex logMutex;
    std::vector<std::string> logs;
    std::vector<std::thread::id> logThreads;
    std::function<void()> scanHook;

    World()
    {
        std::random_device rd;
        root = fs::temp_directory_path() / (L"nr_runtime_core_" + std::to_wstring(rd()) + L"_" + std::to_wstring(GetCurrentProcessId()));
        game = root / L"game";
        optiscaler = game / L"OptiScaler";
        fs::create_directories(optiscaler);
    }
    ~World()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Env MakeEnv()
    {
        Env e;
        e.dllDir = [this] { return optiscaler; };
        e.exeDir = [this] { return game; };
        e.mainDllPath = [this]() -> std::optional<fs::path> { return optiscaler; };
        e.allowStreamlineFolder = [] { return false; };
        e.requested = [this] { return requested; };
        e.setRequested = [this](const std::string& t) { requested = t; };
        e.hybridPrecisionNow = [this] { return hybrid.load(); };
        e.amdBridgeActive = [this] { return amd.load(); };
        e.nowMs = [this] { return now.load(); };
        e.log = [this](bool, const std::string& text)
        {
            std::lock_guard<std::mutex> lock(logMutex);
            logs.push_back(text);
            logThreads.push_back(std::this_thread::get_id());
        };
        e.onScan = [this] { if (scanHook) scanHook(); };
        return e;
    }
    bool Logged(const std::string& needle)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos)
                return true;
        return false;
    }
    int CountLogged(const std::string& needle)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        int n = 0;
        for (const auto& l : logs)
            n += l.find(needle) != std::string::npos;
        return n;
    }
};

static std::string Id(const std::shared_ptr<const Snapshot>& s) { return Runtime::Utf8(s->id); }

static void CatalogAndAutomatic()
{
    {
        World w;
        Put(w.optiscaler / kFile, "legacy model");
        Runtime rt(w.MakeEnv());
        const auto s = rt.Current();
        CHECK(s->revision == 1 && s->Available() && s->legacy);
        CHECK(w.Logged("DLSS-NR runtime catalog: 1 candidate(s)"));
        CHECK(rt.Startup() == s);
        rt.Shutdown();
    }
    {
        World w;
        Put(w.optiscaler / kFile, "legacy model");
        Put(w.optiscaler / L"NR.310.8" / kFile, "model 8");
        Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
        Put(w.optiscaler / L"NR.old" / kFile, "ignored: not a valid name");
        Runtime rt(w.MakeEnv());
        const auto s = rt.Current();
        CHECK(Id(s) == "NR.310.9"); // the newest; the legacy file has no version resource, NR.old is not a candidate
        const auto catalog = rt.Catalog();
        CHECK(catalog.size() == 3u);
        rt.Shutdown();
    }
    {
        // the folder search never picks a versioned folder as the legacy file
        World w;
        Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
        Put(w.game / L"NR.310.8" / kFile, "ignored: the game folder is not a root of NR folders...");
        Runtime rt(w.MakeEnv());
        const auto s = rt.Current();
        CHECK(Id(s) == "NR.310.9" && !s->legacy);
        rt.Shutdown();
    }
}

static void SwitchingAndRescan()
{
    World w;
    Put(w.optiscaler / kFile, "legacy model");
    Put(w.optiscaler / L"NR.310.8" / kFile, "model 8");
    Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
    Runtime rt(w.MakeEnv());
    CHECK(Id(rt.Current()) == "NR.310.9");
    rt.RequestRuntime("NR.310.8");
    CHECK(w.requested == "NR.310.8");
    CHECK(rt.Requested() == "NR.310.8");
    w.now += 100;
    CHECK(Id(rt.Current()) == "NR.310.9" && rt.Pending()); // the quiet period has not passed
    w.now += 250;
    const auto s = rt.Current();
    CHECK(Id(s) == "NR.310.8" && s->revision == 2 && !rt.Pending());
    CHECK(rt.Startup()->revision == 1); // the startup selection stays what it was
    // an explicit choice that is not installed: unavailable, nothing else loaded; a rescan after installing recovers
    rt.RequestRuntime("NR.310.7");
    rt.Current(); // the next frame sees the request
    w.now += 300;
    const auto missing = rt.Current();
    CHECK(!missing->Available() && missing->status == Status::ExplicitMissing && missing->revision == 3);
    Put(w.optiscaler / L"NR.310.7" / kFile, "model 7");
    CHECK(!rt.Current()->Available()); // nothing moves by itself
    rt.Rescan();
    const auto recovered = rt.Current();
    CHECK(recovered->Available() && Id(recovered) == "NR.310.7" && recovered->revision == 4);
    rt.Shutdown();
}

static void RenderThreadNeverWaitsForAScan()
{
    World w;
    Put(w.optiscaler / kFile, "legacy model");
    Runtime rt(w.MakeEnv());
    rt.Current(); // the first scan, before the hook is armed
    std::atomic<bool> inScan { false }, release { false };
    w.scanHook = [&]
    {
        inScan = true;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
    std::thread rescanner([&] { rt.Rescan(); });
    while (!inScan)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // the "menu thread" is inside a long scan: the "render thread" keeps polling and is never held up by it
    Clock::duration worst {};
    for (int i = 0; i < 50; ++i)
    {
        const auto start = Clock::now();
        const auto s = rt.Current();
        rt.Pending();
        worst = std::max(worst, Clock::now() - start);
        CHECK(s->legacy); // still the old catalog
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(worst < std::chrono::milliseconds(100));
    release = true;
    rescanner.join();
    w.scanHook = nullptr;
    CHECK(Id(rt.Current()) == "NR.310.9"); // the finished scan was published as one step and the selection re-resolved
    rt.Shutdown();
}

static void HashWorker()
{
    World w;
    Put(w.optiscaler / kFile, "abc");
    Put(w.optiscaler / L"NR.310.9" / kFile, std::string(3u * 1024 * 1024 + 5, 'x'));
    Runtime rt(w.MakeEnv());
    rt.Current();
    CHECK(rt.HashAttempts() == 0); // nothing is read for a hash until the card asks
    auto catalog = rt.Catalog();
    rt.WaitForHashing();
    catalog = rt.Catalog();
    bool sawLegacy = false;
    for (const auto& e : catalog)
        if (e.legacy)
        {
            sawLegacy = true;
            CHECK(e.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        }
        else
            CHECK(e.sha256.size() == 64u);
    CHECK(sawLegacy);
    CHECK(rt.HashAttempts() == 2);
    rt.Catalog();
    rt.Catalog();
    CHECK(rt.HashAttempts() == 2); // hashed files are not read again
    // the worker never logs; the hash line comes from the thread that asked for the catalog, once per file
    CHECK(w.CountLogged("SHA-256 ba7816bf8f01") == 1);
    {
        std::lock_guard<std::mutex> lock(w.logMutex);
        for (const auto& id : w.logThreads)
            CHECK(id == std::this_thread::get_id());
    }
    // a file replaced in place is hashed again after a rescan
    Put(w.optiscaler / kFile, "abcd");
    rt.Rescan();
    catalog = rt.Catalog();
    rt.WaitForHashing();
    catalog = rt.Catalog();
    for (const auto& e : catalog)
        if (e.legacy)
            CHECK(e.sha256 == Sha256Hex("abcd", 4));
    CHECK(rt.HashAttempts() == 3);
    rt.Shutdown();
    rt.Shutdown(); // twice is fine
    Put(w.optiscaler / kFile, "changed after shutdown");
    rt.Rescan();
    rt.Catalog();
    CHECK(rt.HashAttempts() == 3); // no worker after Shutdown
}

static void UnhashableFileIsNotReadAgain()
{
    World w;
    const fs::path file = w.optiscaler / kFile;
    Put(file, "locked model");
    Runtime rt(w.MakeEnv());
    rt.Current();
    // another process holds the file open without sharing: the hash fails
    HANDLE lockHandle = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lockHandle != INVALID_HANDLE_VALUE);
    rt.Catalog();
    rt.WaitForHashing();
    CHECK(rt.HashAttempts() == 1);
    for (int i = 0; i < 5; ++i) // the card draws every frame
    {
        rt.Catalog();
        rt.WaitForHashing();
    }
    CHECK(rt.HashAttempts() == 1); // not read again, not logged again
    CHECK(w.CountLogged("SHA-256") == 0);
    CloseHandle(lockHandle);
    rt.Rescan(); // a rescan forgets the failure
    rt.Catalog();
    rt.WaitForHashing();
    CHECK(rt.HashAttempts() == 2);
    const auto catalog = rt.Catalog();
    CHECK(catalog.size() == 1u && catalog[0].sha256 == Sha256Hex("locked model", 12));
    rt.Shutdown();
}

static void ScanFailsOpen()
{
    World w;
    Put(w.optiscaler / kFile, "legacy model");
    bool throwNow = true;
    w.scanHook = [&] { if (throwNow) throw std::runtime_error("disk exploded"); };
    Runtime rt(w.MakeEnv());
    const auto s = rt.Current(); // must not throw
    CHECK(!s->Available() && s->status == Status::NothingInstalled);
    CHECK(w.Logged("the catalog scan failed (disk exploded)"));
    throwNow = false;
    rt.Rescan();
    CHECK(rt.Current()->Available());
    rt.Shutdown();
}

static void SessionLockAndPending()
{
    World w;
    Put(w.optiscaler / L"NR.310.8" / kFile, "model 8");
    Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
    Runtime rt(w.MakeEnv());
    CHECK(Id(rt.Current()) == "NR.310.9");
    CHECK(rt.SwitchLockReason() == nullptr);
    // a request is waiting for its quiet period when the hybrid precision comes on: nothing may switch, and nothing may claim to wait
    rt.RequestRuntime("NR.310.8");
    w.now += 50;
    CHECK(rt.Current() != nullptr && rt.Pending());
    w.hybrid = true;
    CHECK(rt.SwitchLockReason() != nullptr);
    CHECK(!rt.Pending());
    w.now += 1000;
    CHECK(Id(rt.Current()) == "NR.310.9"); // frozen
    w.hybrid = false;
    CHECK(rt.SwitchLockReason() != nullptr); // sticky for the session
    w.now += 1000;
    CHECK(Id(rt.Current()) == "NR.310.9");
    rt.Shutdown();
    // the AMD bridge: locked while it is active, free again after
    World a;
    Put(a.optiscaler / L"NR.310.8" / kFile, "model 8");
    Put(a.optiscaler / L"NR.310.9" / kFile, "model 9");
    Runtime art(a.MakeEnv());
    art.Current();
    a.amd = true;
    CHECK(art.SwitchLockReason() != nullptr);
    a.amd = false;
    CHECK(art.SwitchLockReason() == nullptr);
    art.Shutdown();
}

static void NgxSearchFolderRules()
{
    World w;
    Put(w.optiscaler / L"NR.310.8" / kFile, "model 8");
    Put(w.optiscaler / L"NR.310.9" / kFile, "model 9");
    Runtime rt(w.MakeEnv());
    const auto s = rt.Current();
    CHECK(rt.FollowsNgxSearchPath(*s)); // nothing recorded: nothing to disagree with
    rt.NoteNgxSearchFolder(s->path.parent_path());
    rt.NoteNgxSearchFolder(w.game); // the first report counts
    CHECK(rt.NgxSearchFolder() == s->path.parent_path());
    CHECK(rt.FollowsNgxSearchPath(*s));
    rt.RequestRuntime("NR.310.8");
    rt.Current();
    w.now += 300;
    CHECK(!rt.FollowsNgxSearchPath(*rt.Current())); // another folder than the one the NGX core was told about
    Snapshot none;
    CHECK(rt.FollowsNgxSearchPath(none)); // no model: no note
    rt.Shutdown();
}

int main()
{
    CatalogAndAutomatic();
    SwitchingAndRescan();
    RenderThreadNeverWaitsForAScan();
    HashWorker();
    UnhashableFileIsNotReadAgain();
    ScanFailsOpen();
    SessionLockAndPending();
    NgxSearchFolderRules();
    if (failures == 0)
        std::puts("PASS: NR runtime core");
    return failures == 0 ? 0 : 1;
}
