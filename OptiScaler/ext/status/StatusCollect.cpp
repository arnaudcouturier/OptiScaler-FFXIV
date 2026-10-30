#include "pch.h"

#include "ext/status/StatusCollect.h"

#include "ext/core/Banner.h"
#include "ext/core/ExtConfig.h"
#include "ext/ExtFeatures.h"
#include "ext/ffxiv/companion/Companion.h"
#include "ext/fg/dlssg/AmpereMfgLoader.h"
#include "ext/fg/dlssg/MfgUnlock.h"
#include "ext/core/HooksBypass.h"
#include "ext/game/Game.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"

#include <State.h>
#include <Util.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <winver.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>

#pragma comment(lib, "version.lib")

// Snapshot collection (docs/notes/status-json.md): every field comes from an accessor the menu already reads
// (config, game module, feature states, runtime loaders). Nothing here names a path in its output: folders are
// labels only, and the exe/game names are bare file names. Every block is guarded: one unreadable source keeps its
// field's default, never the whole file.
namespace ext::status
{
namespace
{
namespace fs = std::filesystem;

constexpr size_t kDetailMax = 160; // every free-text detail is capped here

std::string Cap(std::string s)
{
    TruncateUtf8(s, kDetailMax);
    return s;
}

std::string WideToUtf8Bounded(const std::wstring& w, size_t maxBytes = 256)
{
    if (w.empty())
        return {};
    const size_t chars = std::min<size_t>(w.size(), 1024);
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(chars), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return "?";
    std::string text(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(chars), text.data(), bytes, nullptr,
                            nullptr) <= 0)
        return "?";
    TruncateUtf8(text, maxBytes);
    return text;
}

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// Lexical folder key (preferred separators, lower case): no disk access, so labeling never causes IO. Only used to
// pick a label, never emitted. A path that is lexically different but canonically equal (short names, junctions)
// falls through to the fallback label -- the safe direction, still never a path.
std::wstring LexKey(const fs::path& p)
{
    fs::path out = p;
    out.make_preferred();
    return Lower(out.wstring());
}

// Folder label, never a path: one of "game folder", "OptiScaler folder", "streamline folder", "NGX driver folder"
// or "other folder" ("" when there is no folder at all). A leaf directory name would leak the user's folder layout,
// so anything outside the known folders takes the caller's `fallback`: the NGX search order finds driver-store
// copies ("NGX driver folder"); the NR/streamline lookups only cover the game and OptiScaler folders, so anything
// else there is "other folder".
std::string FolderLabel(const fs::path& dir, const std::wstring& gameKey, const std::wstring& optiKey,
                        const char* fallback)
{
    if (dir.empty())
        return {};
    const std::wstring key = LexKey(dir);
    if (!gameKey.empty() && key == gameKey)
        return "game folder";
    if (!optiKey.empty())
    {
        if (key == optiKey)
            return "OptiScaler folder";
        if (!dir.parent_path().empty() && LexKey(dir.parent_path()) == optiKey)
            return "OptiScaler folder"; // a direct child of the OptiScaler folder (dlssg_sm86 counts as its folder)
    }
    if (Lower(dir.filename().wstring()) == L"streamline")
        return "streamline folder";
    return fallback;
}

// A loaded module by file name (any folder): the handle's own path says which copy it is. Handle, path and
// snippet only -- no file reads -- so this runs on every tick; the version text (disk reads) is cached separately.
struct LoadedProbe
{
    bool loaded = false;
    std::wstring path; // local only: only the folder label or the version derived from it is emitted
    uint32_t snippet = 0; // NVSDK_NGX_GetSnippetVersion of the mapped copy, cheap (no disk read)
};

// Loaded module by file name (any folder): the handle's own path says which copy it is.
LoadedProbe ProbeHandle(const wchar_t* fileName)
{
    LoadedProbe probe;
    HMODULE module = GetModuleHandleW(fileName); // bare name: any loaded copy
    if (module == nullptr)
        return probe;
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) == 0)
        return probe;
    probe.loaded = true;
    probe.path = path;
    if (auto fn = reinterpret_cast<uint32_t (*)()>(GetProcAddress(module, "NVSDK_NGX_GetSnippetVersion")))
        probe.snippet = fn();
    return probe;
}

std::string SnippetVersion(uint32_t snippet)
{
    if (snippet == 0)
        return {};
    char buf[32] = {};
    std::snprintf(buf, sizeof buf, "%u.%u.%u", (snippet >> 16) & 0xFFFFu, (snippet >> 8) & 0xFFu, snippet & 0xFFu);
    return buf;
}

std::string FileVersionText(const fs::path& file)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0 || size > (1u << 20))
        return {};
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || info == nullptr ||
        length < sizeof(VS_FIXEDFILEINFO))
        return {};
    char buf[32] = {};
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

// First folder (of `dirs`) holding `fileName`, empty when none does. Only the caller-chosen label is emitted.
fs::path FirstHolding(const std::vector<fs::path>& dirs, const wchar_t* fileName)
{
    for (const fs::path& dir : dirs)
    {
        if (dir.empty())
            continue;
        std::error_code ec;
        const fs::path file = dir / fileName;
        if (fs::is_regular_file(file, ec))
            return dir;
    }
    return {};
}

Runtime FinishRuntime(const char* name, const LoadedProbe& probe, std::string version, const fs::path& foundDir,
                      const std::wstring& gameKey, const std::wstring& optiKey, const char* fallbackLabel)
{
    Runtime r;
    r.name = name;
    r.loaded = probe.loaded;
    r.found = probe.loaded || !foundDir.empty();
    r.version = std::move(version);
    if (probe.loaded)
        r.folder = FolderLabel(fs::path(probe.path).parent_path(), gameKey, optiKey, fallbackLabel);
    else if (!foundDir.empty())
        r.folder = FolderLabel(foundDir, gameKey, optiKey, fallbackLabel);
    return r;
}

// File and version reads (runtime probing, ffxivgame.ver) happen at most every 30 s, and only when a write may be
// due: ticks that only test for a change reuse the caches without touching the disk. The loaded-module bit
// (GetModuleHandle, no disk read) is fresh on every tick, so a runtime that appears mid-session is noticed at
// once; its version text fills in on the write-due refresh.
constexpr auto kIoTtl = std::chrono::seconds(30);

struct FileInfo
{
    std::string fileVersion; // FileVersionText of the mapped copy ("" when unknown or not loaded at refresh)
    fs::path foundDir; // FirstHolding result ("" when not found, or loaded)
};

struct ProbeSlot
{
    FileInfo info;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::time_point::min();
    bool has = false;
};

// True when the slot must be (re)filled this tick: missing, or stale and a write may be due.
bool RefreshDue(const std::chrono::steady_clock::time_point& at, bool has, bool refreshIo)
{
    if (!has)
        return true; // the first tick fills the caches (one IO round at startup)
    return refreshIo && std::chrono::steady_clock::now() - at >= kIoTtl;
}

bool ProbeDue(ProbeSlot& slot, bool refreshIo)
{
    return RefreshDue(slot.at, slot.has, refreshIo);
}

// The NGX search order the DLSS DLL rows use (State::NVNGX_FeatureInfo_Paths), then the OptiScaler folder, then the
// game folder. Rule: the loader list is read at most on write-due ticks (refreshIo), never on every tick, and only
// after NGX init has finished -- the NGX module is loaded and the list was non-empty on two consecutive write-due
// ticks -- copying it once and freezing it for the session. The list is mutated on another thread during NGX init
// (clear + push_backs), so reading it earlier could race the writer; the module-loaded plus twice-non-empty gate
// means the writer is done before the single copy.
const std::vector<fs::path>& NgxSearchDirs(const fs::path& optiDir, const fs::path& gameDir, bool refreshIo)
{
    static std::vector<fs::path> dirs;
    static int readyHits = 0; // consecutive write-due ticks with the list non-empty: freeze on the second
    static bool frozen = false; // the list was copied once: never read again
    if (frozen)
        return dirs;
    std::vector<fs::path> baseline;
    if (!optiDir.empty())
        baseline.push_back(optiDir);
    if (!gameDir.empty())
        baseline.push_back(gameDir);
    if (!refreshIo)
    {
        if (dirs.empty())
            dirs = baseline; // fallback folders only: no loader-list read, no disk
        return dirs;
    }
    if (GetModuleHandleW(L"nvngx_dlss.dll") == nullptr)
    {
        readyHits = 0; // NGX init has not run: no list to copy yet
        dirs = baseline;
        return dirs;
    }
    std::vector<std::wstring> copy;
    try
    {
        copy = State::Instance().NVNGX_FeatureInfo_Paths; // the single read of the loader list this tick
    }
    catch (...)
    {
        readyHits = 0;
        dirs = baseline;
        return dirs;
    }
    bool nonEmpty = false;
    for (const auto& p : copy)
    {
        if (!p.empty())
        {
            nonEmpty = true;
            break;
        }
    }
    if (!nonEmpty)
    {
        readyHits = 0;
        dirs = baseline;
        return dirs;
    }
    if (++readyHits < 2)
    {
        dirs = baseline; // seen once: wait for a second consecutive sighting before the single copy
        return dirs;
    }
    std::vector<fs::path> fresh;
    for (const auto& p : copy) // build from the copy in hand: the loader list is not read a second time
        if (!p.empty())
            fresh.emplace_back(p);
    fresh.insert(fresh.end(), baseline.begin(), baseline.end());
    dirs = std::move(fresh);
    frozen = true;
    return dirs;
}

const char* FgInputName(FGInput input)
{
    switch (input)
    {
    case FGInput::Upscaler:
        return "Upscaler";
    case FGInput::DLSSG:
        return "DLSSG";
    case FGInput::NvngxFG:
        return "NvngxFG";
    case FGInput::FSRFG:
        return "FSRFG";
    case FGInput::FSRFG30:
        return "FSRFG30";
    case FGInput::XeFG:
        return "XeFG";
    case FGInput::ForceXeLL:
        return "ForceXeLL";
    default:
        return "NoFG";
    }
}

const char* FgOutputName(FGOutput output)
{
    switch (output)
    {
    case FGOutput::FSRFG:
        return "FSRFG";
    case FGOutput::DLSSG:
        return "DLSSG";
    case FGOutput::XeFG:
        return "XeFG";
    case FGOutput::Reprojection:
        return "Reprojection";
    default:
        return "NoFG";
    }
}

// The running exe's PE headers (TimeDateStamp, SizeOfImage) read from the current process's own headers, without
// the game-build table.
void ReadOwnPeHeaders(uint32_t& stamp, uint32_t& imageSize)
{
    stamp = 0;
    imageSize = 0;
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    stamp = nt->FileHeader.TimeDateStamp;
    imageSize = nt->OptionalHeader.SizeOfImage;
}

// First line of <gameDir>\ffxivgame.ver (the game's own version stamp), "" when unreadable. Content only: a version
// string, never a path.
std::string ReadBuildVer(const fs::path& gameDir)
{
    if (gameDir.empty())
        return {};
    std::error_code ec;
    const fs::path file = gameDir / L"ffxivgame.ver";
    const uint64_t size = fs::file_size(file, ec);
    if (ec || size == 0 || size > 1024)
        return {};
    std::vector<char> data(static_cast<size_t>(size));
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok)
        return {};
    std::string line(data.data(), read);
    if (const size_t end = line.find_first_of("\r\n"); end != std::string::npos)
        line.resize(end);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    line = CleanUtf8(line); // raw file bytes: validate before the file ever sees them
    return Cap(line);
}

// First non-software DXGI adapter: name and dedicated VRAM, enumerated once per session and copied into every
// snapshot (earlier code filled only the first caller's fields, leaving later snapshots empty). A display name and
// a size, never a path. The factory call runs under the hook bypass scope: CreateDXGIFactory1 is hooked (it would
// otherwise wrap the factory), and the hook already supports BypassScope for exactly this kind of diagnostic query.
// There is no existing state to read the adapter from (other call sites query their own one-off factory), so the
// bypassed enumeration is the call. A failed enumeration retries on later write-due ticks, bounded: the flag is set
// only after an enumeration ran, never before the first attempt.
void ReadGpu(std::string& name, uint64_t& vramMB, bool refreshIo)
{
    static std::string cachedName;
    static uint64_t cachedVramMB = 0;
    static bool done = false; // the worker is the only caller
    static int attempts = 0;
    static constexpr int kMaxAttempts = 60;
    if (!done && attempts < kMaxAttempts && (attempts == 0 || refreshIo))
    {
        ++attempts;
        const ext::core::BypassScope bypass;
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) &&
            factory != nullptr)
        {
            for (UINT i = 0;; ++i)
            {
                IDXGIAdapter1* adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_ADAPTER_DESC1 desc {};
                const HRESULT hr = adapter != nullptr ? adapter->GetDesc1(&desc) : E_FAIL;
                if (adapter != nullptr)
                    adapter->Release();
                if (FAILED(hr) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                    continue;
                cachedName = WideToUtf8Bounded(desc.Description, 128);
                cachedVramMB = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024u * 1024u);
                break;
            }
            factory->Release();
            done = true; // the enumeration ran; retrying a working factory cannot find more
        }
    }
    name = cachedName;
    vramMB = cachedVramMB;
}

// gameModule: any game interface published means a matched FFXIV build (game/Game.h). The detail names the
// interfaces, a fixed vocabulary.
Feature CollectGameModule(const ext::game::Game& game)
{
    Feature f;
    f.name = "gameModule";
    std::string detail;
    const auto add = [&](bool on, const char* name)
    {
        if (on)
        {
            if (!detail.empty())
                detail += '+';
            detail += name;
        }
    };
    add(game.geometry != nullptr, "geometry");
    add(game.quality != nullptr, "quality");
    add(game.lighting != nullptr, "lighting");
    add(game.hdr != nullptr, "hdr");
    add(game.fgMarkers != nullptr, "fgMarkers");
    add(game.ngx != nullptr, "ngx");
    add(game.nrQuirks != nullptr, "nrQuirks");
    add(game.hudless != nullptr, "hudless");
    if (detail.empty())
    {
        f.state = kOff;
        f.reason = "no-match";
    }
    else
    {
        f.state = kEngaged;
        f.reason = "matched";
        f.detail = Cap(detail);
    }
    return f;
}

Feature CollectNativeQuality(const ext::game::Game& game)
{
    Feature f;
    f.name = "nativeQuality";
    if (game.quality == nullptr)
    {
        f.state = kOff;
        f.reason = "no-game-module";
        return f;
    }
    const bool live = game.quality->Live();
    if (!live)
    {
        f.state = kPending;
        f.reason = "not-live";
        return f;
    }
    f.state = kEngaged;
    f.reason = "live";
    f.detail = game.quality->DlaaRequired() ? "dlaa-required" : "preset";
    return f;
}

Feature CollectCompanion()
{
    Feature f;
    f.name = "companion";
    if (ext::ffxiv::companion::IsInstalled())
    {
        f.state = kEngaged;
        f.reason = "installed";
        return f;
    }
    const auto& cfg = ext::Cfg().ffxiv;
    const bool replacement = cfg.companionHudReplacement.value_or_default();
    const bool interpolation = cfg.companionHudInterpolation.value_or_default();
    if (replacement || interpolation)
    {
        f.state = kPending;
        f.reason = "configured-not-installed";
        f.detail = replacement && interpolation ? "replacement+interpolation"
                   : replacement                ? "replacement"
                                                : "interpolation";
        return f;
    }
    f.state = kOff;
    f.reason = "disabled";
    return f;
}

Feature CollectNr()
{
    Feature f;
    f.name = "nr";
    if (!ext::Cfg().nr.enabled.value_or_default())
    {
        f.state = kOff;
        f.reason = "disabled";
        return f;
    }
    // The F::Nr bit latches on the key and on an evaluate (ext/nr/NrSeams.cpp): set means a model ran.
    if (ext::Active(ext::F::Nr))
    {
        f.state = kEngaged;
        f.reason = "evaluating";
    }
    else
    {
        f.state = kPending;
        f.reason = "armed-no-evaluate";
    }
    return f;
}

Feature CollectFrameGeneration(FGInput input, FGOutput output)
{
    Feature f;
    f.name = "frameGeneration";
    f.detail = Cap(std::string(FgInputName(input)) + "-in/" + FgOutputName(output) + "-out");
    if (output == FGOutput::NoFG && input == FGInput::NoFG)
    {
        f.state = kOff;
        f.reason = "disabled";
    }
    else if (output != FGOutput::NoFG)
    {
        f.state = kEngaged;
        f.reason = "active";
    }
    else
    {
        f.state = kPending;
        f.reason = "no-output";
    }
    return f;
}

Feature CollectHudless()
{
    Feature f;
    f.name = "hudless";
    const ext::hudless::Mode mode = ext::hudless::CurrentMode();
    if (mode == ext::hudless::Mode::Off)
    {
        f.state = kOff;
        f.reason = "disabled";
        return f;
    }
    const ext::hudless::Status status = ext::hudless::GetStatus();
    f.detail = Cap(ext::hudless::StatusLine(status));
    using Phase = ext::hudless::Phase;
    if (status.phase == Phase::Active)
    {
        f.state = kEngaged;
        f.reason = "delivering";
    }
    else if (status.phase == Phase::Searching)
    {
        f.state = kPending;
        f.reason = "searching";
    }
    else if (status.phase == Phase::Fallback)
    {
        // The dominant reason rides in the detail (StatusLine says "Fallback(<Reason>)").
        f.state = kPending;
        f.reason = "fallback";
    }
    else
    {
        f.state = kUnknown;
        f.reason = "no-phase";
    }
    return f;
}

Feature CollectHdr()
{
    Feature f;
    f.name = "hdr";
    const auto& hdr = ext::Cfg().hdr;
    if (!hdr.ffxivHdr.value_or_default() && hdr.mode.value_or_default() == 0)
    {
        f.state = kOff;
        f.reason = "disabled";
        return f;
    }
    f.detail = hdr.mode.value_or_default() == 0 ? "sdr-expansion" : "scene";
    if (ext::Active(ext::F::Hdr))
    {
        f.state = kEngaged;
        f.reason = "active";
    }
    else
    {
        f.state = kPending;
        f.reason = "not-created";
    }
    return f;
}

} // namespace

// Deletes status.tmp-<pid>.json files whose pid is gone (a crashed writer's leftovers). Our own pid is kept; a pid
// we cannot open for lack of rights (ERROR_ACCESS_DENIED) counts as alive. Best effort, once per process.
void SweepStaleTemps(const std::wstring& dir, unsigned long ownPid) noexcept
{
    try
    {
        if (dir.empty() || ownPid == 0)
            return;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec)
                return;
            const std::wstring name = entry.path().filename().wstring();
            if (name.size() < 16 || name.compare(0, 11, L"status.tmp-") != 0 ||
                name.compare(name.size() - 5, 5, L".json") != 0)
                continue;
            unsigned long pid = 0;
            try
            {
                pid = static_cast<unsigned long>(std::stoul(name.substr(11)));
            }
            catch (...)
            {
                continue;
            }
            if (pid == 0 || pid == ownPid)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h != nullptr)
            {
                CloseHandle(h);
                continue; // alive
            }
            if (GetLastError() == ERROR_ACCESS_DENIED)
                continue; // alive, but not ours to query
            std::error_code del;
            fs::remove(entry.path(), del);
        }
    }
    catch (...)
    {
    }
}

Snapshot Collect(uint64_t seq, const std::string& sessionId, unsigned long pid, const std::string& startUtc,
                 const PerfSeries& base, const PerfSeries& output, bool haveOutput, const Errors& errors,
                 bool refreshIo)
{
    Snapshot snap;
    snap.seq = seq;
    snap.sessionId = sessionId;
    snap.pid = pid;
    snap.startUtc = startUtc;
    snap.errors = errors;
    snap.base = base;
    snap.output = output;
    snap.haveOutput = haveOutput;

    try
    {
        snap.forkDescribe = Cap(ext::core::ForkVersion());
        snap.forkUpstream = Cap(ext::core::UpstreamBase());
    }
    catch (...)
    {
    }

    // Folders, resolved once per process (they never move): the game folder (the exe's parent, where ffxivgame.ver
    // sits) and the OptiScaler folder (the DLL's folder). The keys are lexical (LexKey: no disk access), so labeling
    // below never causes IO; only the label is ever emitted.
    static fs::path cachedGameDir, cachedOptiDir;
    static std::wstring cachedGameKey, cachedOptiKey;
    static bool dirsDone = false;
    if (!dirsDone)
    {
        try
        {
            cachedGameDir = Util::ExePath().parent_path();
            if (Config::Instance()->MainDllPath.has_value())
                cachedOptiDir = Config::Instance()->MainDllPath.value();
        }
        catch (...)
        {
        }
        cachedGameKey = cachedGameDir.empty() ? std::wstring() : LexKey(cachedGameDir);
        cachedOptiKey = cachedOptiDir.empty() ? std::wstring() : LexKey(cachedOptiDir);
        dirsDone = true;
    }
    const fs::path& gameDir = cachedGameDir;
    const fs::path& optiDir = cachedOptiDir;
    const std::wstring& gameKey = cachedGameKey;
    const std::wstring& optiKey = cachedOptiKey;

    try
    {
        snap.exeName = Cap(WideToUtf8Bounded(fs::path(State::Instance().gameExe).filename().wstring(), 64));
    }
    catch (...)
    {
    }
    try
    {
        snap.fgInput = FgInputName(State::Instance().activeFgInput);
        snap.fgOutput = FgOutputName(State::Instance().activeFgOutput);
    }
    catch (...)
    {
    }

    const ext::game::Game* game = nullptr;
    try
    {
        game = &ext::game::Current();
    }
    catch (...)
    {
    }

    // Each feature block is isolated: one throwing accessor keeps its field's default, never the whole file.
    try
    {
        if (game == nullptr)
            snap.features.push_back(Feature { "gameModule", kOff, "no-game" });
        else
        {
            const Feature module = CollectGameModule(*game);
            snap.gameMatched = module.state == kEngaged;
            snap.features.push_back(module);
        }
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(game != nullptr ? CollectNativeQuality(*game)
                                                : Feature { "nativeQuality", kOff, "no-game-module" });
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(CollectCompanion());
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(CollectNr());
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(CollectFrameGeneration(State::Instance().activeFgInput,
                                                       State::Instance().activeFgOutput));
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(CollectHudless());
    }
    catch (...)
    {
    }
    try
    {
        snap.features.push_back(CollectHdr());
    }
    catch (...)
    {
    }
    try
    {
        snap.nrEnabled = ext::Cfg().nr.enabled.value_or_default();
        snap.nrActive = ext::Active(ext::F::Nr);
    }
    catch (...)
    {
    }

    try
    {
        ReadOwnPeHeaders(snap.peStamp, snap.peImageSize);
        static std::string cachedBuildVer;
        static std::chrono::steady_clock::time_point buildVerAt =
            std::chrono::steady_clock::time_point::min();
        static bool buildVerHas = false;
        if (RefreshDue(buildVerAt, buildVerHas, refreshIo))
        {
            cachedBuildVer = ReadBuildVer(gameDir);
            buildVerAt = std::chrono::steady_clock::now();
            buildVerHas = true;
        }
        snap.buildVer = cachedBuildVer;
    }
    catch (...)
    {
    }
    try
    {
        ReadGpu(snap.gpuName, snap.gpuVramMB, refreshIo);
    }
    catch (...)
    {
    }

    // Runtimes: DLSS in NGX search order, DLSS-G (built-in unlock + companion runtime), the NR model, Streamline.
    // The loaded bit below is fresh every tick (GetModuleHandle reads no files); the version text and found-folder
    // probes are cached (ProbeDue) and refreshed only when a write may be due.
    try
    {
        const std::vector<fs::path>& dirs = NgxSearchDirs(optiDir, gameDir, refreshIo);
        const LoadedProbe dlssProbe = ProbeHandle(L"nvngx_dlss.dll");
        static ProbeSlot dlssSlot;
        if (ProbeDue(dlssSlot, refreshIo))
        {
            FileInfo info;
            if (dlssProbe.loaded)
                info.fileVersion = FileVersionText(dlssProbe.path);
            else
                info.foundDir = FirstHolding(dirs, L"nvngx_dlss.dll");
            dlssSlot.info = std::move(info);
            dlssSlot.at = std::chrono::steady_clock::now();
            dlssSlot.has = true;
        }
        std::string dlssVersion = SnippetVersion(dlssProbe.snippet);
        if (dlssVersion.empty())
            dlssVersion = dlssSlot.info.fileVersion;
        snap.runtimes.push_back(FinishRuntime("dlss", dlssProbe, dlssVersion,
                                              dlssProbe.loaded ? fs::path() : dlssSlot.info.foundDir, gameKey, optiKey,
                                              "NGX driver folder"));
    }
    catch (...)
    {
    }
    try
    {
        const LoadedProbe dlssgProbe = ProbeHandle(L"nvngx_dlssg.dll");
        MfgUnlock::Status unlock {};
        try
        {
            unlock = MfgUnlock::LastStatus();
        }
        catch (...)
        {
        }
        AmpereMfgLoader::Status ampere {};
        try
        {
            ampere = AmpereMfgLoader::LastStatus();
        }
        catch (...)
        {
        }
        static ProbeSlot dlssgSlot;
        if (ProbeDue(dlssgSlot, refreshIo))
        {
            FileInfo info;
            if (dlssgProbe.loaded)
                info.fileVersion = FileVersionText(dlssgProbe.path);
            dlssgSlot.info = std::move(info);
            dlssgSlot.at = std::chrono::steady_clock::now();
            dlssgSlot.has = true;
        }
        std::string dlssgVersion = SnippetVersion(dlssgProbe.snippet);
        if (dlssgVersion.empty())
        {
            const std::string unlockVer = Cap(CleanUtf8(unlock.SnippetVersion)); // capped + cleaned
            if (IsVersionText(unlockVer))
                dlssgVersion = unlockVer; // the unlock read the file version of the mapped copy
        }
        if (dlssgVersion.empty())
            dlssgVersion = dlssgSlot.info.fileVersion;
        const fs::path dlssgFound = !ampere.DllFound ? fs::path() : optiDir / L"dlssg_sm86";
        snap.runtimes.push_back(FinishRuntime("dlssg", dlssgProbe, dlssgVersion,
                                              dlssgProbe.loaded ? fs::path() : dlssgFound, gameKey, optiKey,
                                              "NGX driver folder"));
    }
    catch (...)
    {
    }
    try
    {
        const LoadedProbe nrProbe = ProbeHandle(L"nvngx_dlssnr.dll");
        std::vector<fs::path> dirs;
        if (!optiDir.empty())
            dirs.push_back(optiDir);
        if (!gameDir.empty())
            dirs.push_back(gameDir);
        static ProbeSlot nrSlot;
        if (ProbeDue(nrSlot, refreshIo))
        {
            FileInfo info;
            if (nrProbe.loaded)
                info.fileVersion = FileVersionText(nrProbe.path);
            else
                info.foundDir = FirstHolding(dirs, L"nvngx_dlssnr.dll");
            nrSlot.info = std::move(info);
            nrSlot.at = std::chrono::steady_clock::now();
            nrSlot.has = true;
        }
        std::string nrVersion = SnippetVersion(nrProbe.snippet);
        if (nrVersion.empty())
            nrVersion = nrSlot.info.fileVersion;
        snap.runtimes.push_back(FinishRuntime("nr", nrProbe, nrVersion,
                                              nrProbe.loaded ? fs::path() : nrSlot.info.foundDir, gameKey, optiKey,
                                              "other folder"));
    }
    catch (...)
    {
    }
    try
    {
        LoadedProbe slProbe = ProbeHandle(L"sl.common.dll");
        if (!slProbe.loaded)
            slProbe = ProbeHandle(L"sl.dlss_g.dll");
        std::vector<fs::path> dirs;
        if (!optiDir.empty())
        {
            dirs.push_back(optiDir / L"streamline");
            dirs.push_back(optiDir);
        }
        static ProbeSlot slSlot;
        if (ProbeDue(slSlot, refreshIo))
        {
            FileInfo info;
            if (slProbe.loaded)
                info.fileVersion = FileVersionText(slProbe.path);
            else
            {
                info.foundDir = FirstHolding(dirs, L"sl.common.dll");
                if (info.foundDir.empty())
                    info.foundDir = FirstHolding(dirs, L"sl.dlss_g.dll");
            }
            slSlot.info = std::move(info);
            slSlot.at = std::chrono::steady_clock::now();
            slSlot.has = true;
        }
        std::string slVersion = SnippetVersion(slProbe.snippet);
        if (slVersion.empty())
            slVersion = slSlot.info.fileVersion;
        snap.runtimes.push_back(FinishRuntime("streamline", slProbe, slVersion,
                                              slProbe.loaded ? fs::path() : slSlot.info.foundDir, gameKey, optiKey,
                                              "other folder"));
    }
    catch (...)
    {
    }

    return snap;
}

} // namespace ext::status
