// The status snapshot collector: every feature reports its documented state
// and reason, runtimes report what the loader sees, folders stay labels, and
// the loader list is only read once start-up has settled. This test builds
// the real StatusCollect.cpp against stand-ins for the accessors the menu
// already reads and drives each feature through its states.
#include "ext/status/StatusCollect.h"
#include "ext/status/StatusCollect.cpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace status = ext::status;
namespace fs = std::filesystem;

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

static const status::Feature* FindFeature(const status::Snapshot& snap, const char* name)
{
    for (const auto& f : snap.features)
        if (std::string(f.name) == name)
            return &f;
    return nullptr;
}

static const status::Runtime* FindRuntime(const status::Snapshot& snap, const char* name)
{
    for (const auto& r : snap.runtimes)
        if (std::string(r.name) == name)
            return &r;
    return nullptr;
}

static void CheckFeature(const status::Snapshot& snap, const char* name, const char* state, const char* reason)
{
    const status::Feature* f = FindFeature(snap, name);
    if (f == nullptr)
    {
        std::printf("FAIL: feature %s missing\n", name);
        ++g_failed;
        return;
    }
    if (std::string(f->state) != state || std::string(f->reason) != reason)
    {
        std::printf("FAIL: feature %s is %s/%s, want %s/%s\n", name, f->state, f->reason, state, reason);
        ++g_failed;
    }
}

static fs::path TestRoot;
static fs::path GameDir;
static fs::path OptiDir;

static status::Snapshot CollectDefault()
{
    status::PerfSeries base, output;
    status::Errors errors;
    return status::Collect(42, "session-id", 1234, "2026-10-29T20:00:00Z", base, output, false, errors, false);
}

static void ResetStubs()
{
    ext::g_cfg = ext::ExtConfig();
    ext::g_activeFeatures = 0;
    ext::ffxiv::companion::g_installed = false;
    ext::hudless::g_mode = ext::hudless::Mode::Off;
    ext::hudless::g_status = ext::hudless::Status();
    ext::hudless::g_statusLine = "Active";
    ext::game::g_game = ext::game::Game();
    State::Instance().activeFgInput = FGInput::NoFG;
    State::Instance().activeFgOutput = FGOutput::NoFG;
    AmpereMfgLoader::g_status = AmpereMfgLoader::Status();
    MfgUnlock::g_status = MfgUnlock::Status();
}

// Everything off and unmatched: every feature reports why it is off, every
// runtime is unknown, the fixed fields pass through.
static void CheckAllOff()
{
    ResetStubs();
    const status::Snapshot snap = CollectDefault();
    CHECK(snap.seq == 42);
    CHECK(snap.sessionId == "session-id");
    CHECK(snap.pid == 1234);
    CHECK(snap.startUtc == "2026-10-29T20:00:00Z");
    CHECK(snap.features.size() == 7);
    CheckFeature(snap, "gameModule", "off", "no-match");
    CHECK(!snap.gameMatched);
    CheckFeature(snap, "nativeQuality", "off", "no-game-module");
    CheckFeature(snap, "companion", "off", "disabled");
    CheckFeature(snap, "nr", "off", "disabled");
    CheckFeature(snap, "frameGeneration", "off", "disabled");
    const status::Feature* fg = FindFeature(snap, "frameGeneration");
    CHECK(fg != nullptr && fg->detail == "NoFG-in/NoFG-out");
    CheckFeature(snap, "hudless", "off", "disabled");
    CheckFeature(snap, "hdr", "off", "disabled");
    CHECK(!snap.nrEnabled && !snap.nrActive);
    CHECK(snap.fgInput == "NoFG" && snap.fgOutput == "NoFG");
    CHECK(snap.runtimes.size() == 4);
    CHECK(std::string(snap.runtimes[0].name) == "dlss");
    CHECK(std::string(snap.runtimes[1].name) == "dlssg");
    CHECK(std::string(snap.runtimes[2].name) == "nr");
    CHECK(std::string(snap.runtimes[3].name) == "streamline");
    for (const auto& r : snap.runtimes)
    {
        if (r.found || r.loaded || !r.version.empty())
        {
            std::printf("FAIL: runtime %s known without any module: %d/%d/%s\n", r.name, (int) r.found,
                        (int) r.loaded, r.version.c_str());
            ++g_failed;
        }
    }
    CHECK(snap.forkDescribe == "test-fork");
    CHECK(snap.forkUpstream == "test-upstream");
    CHECK(snap.exeName == "ffxiv_dx11.exe");
    CHECK(snap.buildVer == "2026.09.1234.5678"); // first line of the game's version stamp
    CHECK(snap.peStamp != 0 && snap.peImageSize != 0); // this process's own headers
}

// A matched build with live readings: every feature reports engaged with its
// documented reason and detail.
static int g_marker = 7;
static ext::game::FakeQuality g_quality;

static void CheckEngaged()
{
    ResetStubs();
    ext::game::g_game.geometry = &g_marker;
    ext::game::g_game.quality = &g_quality;
    ext::game::g_game.lighting = &g_marker;
    ext::game::g_game.hdr = &g_marker;
    ext::game::g_game.fgMarkers = &g_marker;
    ext::game::g_game.ngx = &g_marker;
    ext::game::g_game.nrQuirks = &g_marker;
    ext::game::g_game.hudless = &g_marker;
    g_quality.live = true;
    g_quality.dlaa = false;
    ext::ffxiv::companion::g_installed = true;
    ext::g_cfg.nr.enabled.value = true;
    ext::g_activeFeatures = static_cast<uint64_t>(ext::F::Nr) | static_cast<uint64_t>(ext::F::Hdr);
    State::Instance().activeFgInput = FGInput::Upscaler;
    State::Instance().activeFgOutput = FGOutput::DLSSG;
    ext::hudless::g_mode = ext::hudless::Mode::UiFree;
    ext::hudless::g_status.phase = ext::hudless::Phase::Active;
    ext::hudless::g_statusLine = "Active";
    ext::g_cfg.hdr.ffxivHdr.value = true;
    ext::g_cfg.hdr.mode.value = 1;

    const status::Snapshot snap = CollectDefault();
    CHECK(snap.gameMatched);
    CheckFeature(snap, "gameModule", "engaged", "matched");
    const status::Feature* module = FindFeature(snap, "gameModule");
    CHECK(module != nullptr &&
          module->detail == "geometry+quality+lighting+hdr+fgMarkers+ngx+nrQuirks+hudless");
    CheckFeature(snap, "nativeQuality", "engaged", "live");
    const status::Feature* quality = FindFeature(snap, "nativeQuality");
    CHECK(quality != nullptr && quality->detail == "preset");
    CheckFeature(snap, "companion", "engaged", "installed");
    CheckFeature(snap, "nr", "engaged", "evaluating");
    CHECK(snap.nrEnabled && snap.nrActive);
    CheckFeature(snap, "frameGeneration", "engaged", "active");
    CHECK(snap.fgInput == "Upscaler" && snap.fgOutput == "DLSSG");
    CheckFeature(snap, "hudless", "engaged", "delivering");
    CheckFeature(snap, "hdr", "engaged", "active");
    const status::Feature* hdr = FindFeature(snap, "hdr");
    CHECK(hdr != nullptr && hdr->detail == "scene");
}

// The middle states: on but not delivering yet, armed and falling back.
static void CheckPending()
{
    ResetStubs();
    ext::game::g_game.quality = &g_quality;
    g_quality.live = false; // interface present, no reading yet
    CheckFeature(CollectDefault(), "nativeQuality", "pending", "not-live");
    g_quality.live = true;
    g_quality.dlaa = true;
    const status::Snapshot dlaa = CollectDefault();
    CheckFeature(dlaa, "nativeQuality", "engaged", "live");
    const status::Feature* quality = FindFeature(dlaa, "nativeQuality");
    CHECK(quality != nullptr && quality->detail == "dlaa-required");

    ResetStubs();
    ext::g_cfg.ffxiv.companionHudReplacement.value = true; // asked for, not installed
    CheckFeature(CollectDefault(), "companion", "pending", "configured-not-installed");
    const status::Snapshot both = CollectDefault();
    const status::Feature* companion = FindFeature(both, "companion");
    CHECK(companion != nullptr && companion->detail == "replacement");
    ext::g_cfg.ffxiv.companionHudInterpolation.value = true;
    const status::Snapshot bothSnap = CollectDefault(); // named: the pointers below stay valid
    const status::Feature* bothOn = FindFeature(bothSnap, "companion");
    CHECK(bothOn != nullptr && bothOn->detail == "replacement+interpolation");

    ResetStubs();
    ext::g_cfg.nr.enabled.value = true; // armed, no model ran
    CheckFeature(CollectDefault(), "nr", "pending", "armed-no-evaluate");

    ResetStubs();
    State::Instance().activeFgInput = FGInput::DLSSG; // input without output
    CheckFeature(CollectDefault(), "frameGeneration", "pending", "no-output");

    ResetStubs();
    ext::hudless::g_mode = ext::hudless::Mode::Recomposition;
    ext::hudless::g_status.phase = ext::hudless::Phase::Searching;
    CheckFeature(CollectDefault(), "hudless", "pending", "searching");
    ext::hudless::g_status.phase = ext::hudless::Phase::Fallback;
    ext::hudless::g_statusLine = "Fallback(MaskCoverage)";
    const status::Snapshot fallback = CollectDefault();
    CheckFeature(fallback, "hudless", "pending", "fallback");
    const status::Feature* hudless = FindFeature(fallback, "hudless");
    CHECK(hudless != nullptr && hudless->detail == "Fallback(MaskCoverage)");
    ext::hudless::g_status.phase = ext::hudless::Phase::Off; // mode on, engine silent
    CheckFeature(CollectDefault(), "hudless", "unknown", "no-phase");

    ResetStubs();
    ext::g_cfg.hdr.mode.value = 0;
    ext::g_cfg.hdr.ffxivHdr.value = false;
    CheckFeature(CollectDefault(), "hdr", "off", "disabled");
    ext::g_cfg.hdr.ffxivHdr.value = true; // on, feature not created
    const status::Snapshot sdr = CollectDefault();
    CheckFeature(sdr, "hdr", "pending", "not-created");
    const status::Feature* hdr = FindFeature(sdr, "hdr");
    CHECK(hdr != nullptr && hdr->detail == "sdr-expansion");
}

// The unlock's provisional sentence never enters a version field; a real
// version does. The loader module is absent throughout, so the file version
// underneath stays unknown.
static void CheckVersionGate()
{
    ResetStubs();
    CHECK(GetModuleHandleW(L"nvngx_dlss.dll") == nullptr); // premise: no loader module here
    MfgUnlock::g_status.SnippetVersion = "metadata pending";
    const status::Snapshot pendingSnap = CollectDefault(); // named: the pointers below stay valid
    const status::Runtime* pending = FindRuntime(pendingSnap, "dlssg");
    CHECK(pending != nullptr && pending->version.empty());
    MfgUnlock::g_status.SnippetVersion = "310.5.3";
    const status::Snapshot realSnap = CollectDefault();
    const status::Runtime* real = FindRuntime(realSnap, "dlssg");
    CHECK(real != nullptr && real->version == "310.5.3");
    MfgUnlock::g_status.SnippetVersion = "not a version!";
    const status::Snapshot rejectedSnap = CollectDefault();
    const status::Runtime* rejected = FindRuntime(rejectedSnap, "dlssg");
    CHECK(rejected != nullptr && rejected->version.empty());
}

// Every input/output code names itself; a code the writer does not know
// falls back to the closed "NoFG" word.
static void CheckFgNames()
{
    CHECK(std::string(status::FgInputName(FGInput::NoFG)) == "NoFG");
    CHECK(std::string(status::FgInputName(FGInput::Upscaler)) == "Upscaler");
    CHECK(std::string(status::FgInputName(FGInput::DLSSG)) == "DLSSG");
    CHECK(std::string(status::FgInputName(FGInput::NvngxFG)) == "NvngxFG");
    CHECK(std::string(status::FgInputName(FGInput::FSRFG)) == "FSRFG");
    CHECK(std::string(status::FgInputName(FGInput::FSRFG30)) == "FSRFG30");
    CHECK(std::string(status::FgInputName(FGInput::XeFG)) == "XeFG");
    CHECK(std::string(status::FgInputName(FGInput::ForceXeLL)) == "ForceXeLL");
    CHECK(std::string(status::FgInputName(static_cast<FGInput>(99))) == "NoFG");
    CHECK(std::string(status::FgOutputName(FGOutput::NoFG)) == "NoFG");
    CHECK(std::string(status::FgOutputName(FGOutput::FSRFG)) == "FSRFG");
    CHECK(std::string(status::FgOutputName(FGOutput::DLSSG)) == "DLSSG");
    CHECK(std::string(status::FgOutputName(FGOutput::XeFG)) == "XeFG");
    CHECK(std::string(status::FgOutputName(FGOutput::Reprojection)) == "Reprojection");
    CHECK(std::string(status::FgOutputName(static_cast<FGOutput>(99))) == "NoFG");
}

// Folder labels name the kind of folder, never the folder: the game folder,
// the writer's own folder (a direct child counts as its folder), a folder
// named streamline, or the caller's fallback. A label never holds a path.
static void CheckFolderLabels()
{
    const fs::path game = fs::path(L"C:\\Games\\FINAL FANTASY XIV Online");
    const fs::path opti = game / L"OptiScaler";
    const std::wstring gameKey = status::LexKey(game);
    const std::wstring optiKey = status::LexKey(opti);
    CHECK(status::FolderLabel(game, gameKey, optiKey, "other folder") == "game folder");
    CHECK(status::FolderLabel(fs::path(L"C:\\GAMES\\final fantasy xiv online"), gameKey, optiKey,
                              "other folder") == "game folder"); // matching ignores case
    CHECK(status::FolderLabel(opti, gameKey, optiKey, "other folder") == "OptiScaler folder");
    CHECK(status::FolderLabel(opti / L"dlssg_sm86", gameKey, optiKey, "other folder") ==
          "OptiScaler folder");
    CHECK(status::FolderLabel(fs::path(L"D:\\tools\\streamline"), gameKey, optiKey, "other folder") ==
          "streamline folder");
    CHECK(status::FolderLabel(fs::path(L"E:\\driver\\oem"), gameKey, optiKey, "NGX driver folder") ==
          "NGX driver folder"); // the NGX search order finds driver-store copies
    CHECK(status::FolderLabel(fs::path(L"E:\\driver\\oem"), gameKey, optiKey, "other folder") == "other folder");
    CHECK(status::FolderLabel(fs::path(), gameKey, optiKey, "other folder").empty());
    for (const fs::path dir :
         { game, opti, fs::path(L"D:\\tools\\streamline"), fs::path(L"E:\\driver\\oem") })
    {
        const std::string label = status::FolderLabel(dir, gameKey, optiKey, "other folder");
        if (label.find('\\') != std::string::npos || label.find(':') != std::string::npos ||
            label.find("Users") != std::string::npos)
        {
            std::printf("FAIL: label carries path text: %s\n", label.c_str());
            ++g_failed;
        }
    }
}

// The adapter name is copied into every snapshot: a later snapshot never
// comes back empty after the first one filled it.
static void CheckGpuCached()
{
    ResetStubs();
    const std::string first = CollectDefault().gpuName;
    const uint64_t firstVram = CollectDefault().gpuVramMB;
    const status::Snapshot second = CollectDefault();
    CHECK(!first.empty()); // this machine has an adapter to report
    CHECK(second.gpuName == first);
    CHECK(second.gpuVramMB == firstVram);
    std::string name;
    uint64_t vram = 0;
    status::ReadGpu(name, vram, true);
    CHECK(name == first && vram == firstVram);
}

// The loader list is read at most on write-due ticks, never on every tick,
// and only after start-up has settled: the module loaded plus two
// consecutive non-empty sightings. Then the copy freezes for the session.
static void CheckNgxGating()
{
    const fs::path opti = OptiDir;
    const fs::path game = GameDir;
    g_loaderListReads = 0;

    const auto baseline = status::NgxSearchDirs(opti, game, false); // a change-test tick
    CHECK(baseline.size() == 2 && baseline[0] == opti && baseline[1] == game);
    CHECK(g_loaderListReads == 0); // no loader-list read, no disk
    const auto absent = status::NgxSearchDirs(opti, game, true); // write-due, module still absent
    CHECK(absent.size() == 2 && absent[0] == opti && absent[1] == game);
    CHECK(g_loaderListReads == 0); // nothing to copy yet

    // Settle the start-up: map a stand-in for the loader module and publish
    // the list, as the loader does once initialisation has finished.
    const fs::path probe = TestRoot / L"nvngx_dlss.dll";
    std::error_code ec;
    fs::copy_file(fs::path(L"C:\\Windows\\System32\\version.dll"), probe, ec);
    if (ec)
    {
        std::printf("FAIL: cannot stage loader stand-in: %s\n", ec.message().c_str());
        ++g_failed;
        return;
    }
    HMODULE loader = LoadLibraryW(probe.c_str());
    if (loader == nullptr)
    {
        std::printf("FAIL: cannot map loader stand-in\n");
        ++g_failed;
        return;
    }
    State::Instance().NVNGX_FeatureInfo_Paths.paths = { L"C:\\DriverStore\\ngx", L"D:\\tools" };

    const auto once = status::NgxSearchDirs(opti, game, true); // first sighting: still baseline
    CHECK(once.size() == 2 && once[0] == opti && once[1] == game);
    CHECK(g_loaderListReads == 1);
    const auto frozen = status::NgxSearchDirs(opti, game, true); // second sighting: the single copy
    CHECK(frozen.size() == 4);
    CHECK(frozen[0] == fs::path(L"C:\\DriverStore\\ngx") && frozen[1] == fs::path(L"D:\\tools"));
    CHECK(frozen[2] == opti && frozen[3] == game);
    const unsigned reads = g_loaderListReads;
    State::Instance().NVNGX_FeatureInfo_Paths.paths = { L"C:\\Changed" }; // late change: ignored
    const auto kept = status::NgxSearchDirs(opti, game, true);
    CHECK(kept.size() == 4 && kept[0] == fs::path(L"C:\\DriverStore\\ngx"));
    CHECK(g_loaderListReads == reads); // frozen: never read again
    const auto tick = status::NgxSearchDirs(opti, game, false);
    CHECK(tick.size() == 4 && g_loaderListReads == reads);
    FreeLibrary(loader);
    fs::remove(probe, ec);
}

int main()
{
    std::error_code ec;
    TestRoot = fs::temp_directory_path(ec) / L"status-w4-collect";
    fs::remove_all(TestRoot, ec);
    GameDir = TestRoot / L"game";
    OptiDir = GameDir / L"OptiScaler";
    fs::create_directories(OptiDir, ec);
    if (ec)
    {
        std::printf("FAIL: cannot create test folders\n");
        return 1;
    }
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (GameDir / L"ffxivgame.ver").c_str(), L"wb");
        if (f == nullptr)
        {
            std::printf("FAIL: cannot write version stamp\n");
            return 1;
        }
        std::fputs("2026.09.1234.5678\r\nsecond line\r\n", f);
        std::fclose(f);
    }
    Util::g_exePath = GameDir / L"ffxiv_dx11.exe"; // the game folder derives from the exe
    Config::Instance()->MainDllPath = OptiDir.wstring();

    CheckAllOff();
    CheckEngaged();
    CheckPending();
    CheckVersionGate();
    CheckFgNames();
    CheckFolderLabels();
    CheckGpuCached();
    CheckNgxGating(); // last: freezes the loader list and maps a module

    fs::remove_all(TestRoot, ec);
    if (g_failed == 0)
        std::puts("PASS: status collector reports every feature state, labels folders, gates versions, "
                  "caches the adapter and reads the loader list once settled");
    return g_failed == 0 ? 0 : 1;
}

