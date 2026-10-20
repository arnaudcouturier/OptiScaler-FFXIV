#include "pch.h"

#include "ext/nr/runtime/NrRuntime.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/PathText.h"
#include "ext/nr/NrConfig.h"
#include "ext/nr/amd/AmdBridge.h"
#include "ext/nr/runtime/NrRuntimeScan.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <map>
#include <mutex>
#include <thread>

namespace ext::nr::runtime
{
std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

namespace
{
const fs::path kModelFile = L"nvngx_dlssnr.dll";

std::wstring Key(const fs::path& p)
{
    std::wstring k = p.native();
    for (auto& c : k)
        c = detail::Lower(c);
    return k;
}

struct Glue
{
    std::mutex mutex; // guards everything below and the config value it reads and writes
    std::vector<Entry> catalog;
    bool catalogBuilt = false;
    std::map<std::wstring, std::string> hashes; // lower-case path -> SHA-256, filled by the worker
    std::unique_ptr<Selector> selector;
    std::shared_ptr<const Snapshot> startup;
    fs::path ngxFolder;
    bool hashWorkerRunning = false;
};

Glue& G()
{
    static Glue& g = *new Glue; // never destroyed: the hash worker may still run at process exit
    return g;
}

// Same exclusion Util::FindFilePath applies: nothing under <OptiScaler folder>\streamline (unless DLSS-G is the active output).
ExcludeFn StreamlineExclusion()
{
    fs::path streamline;
    if (Config::Instance()->MainDllPath.has_value())
        streamline = fs::path(Config::Instance()->MainDllPath.value()) / L"streamline";
    const bool allowAll = State::Instance().activeFgOutput == FGOutput::DLSSG;
    return [streamline, allowAll](const fs::path& p) { return !allowAll && !streamline.empty() && IsUnder(p, streamline); };
}

void FillFileFacts(Entry& e)
{
    e.fileVersion = ReadFileVersion(e.path);
    e.sizeBytes = FileSize(e.path);
}

void HashWorker(std::vector<fs::path> paths)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (const auto& path : paths)
    {
        const std::string sha = Sha256File(path);
        if (sha.empty())
        {
            LOG_WARN("DLSS-NR runtime: could not hash {}", PathText(path));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(G().mutex);
            G().hashes[Key(path)] = sha;
        }
        LOG_INFO("DLSS-NR runtime {}: SHA-256 {}", PathText(path), sha);
    }
    std::lock_guard<std::mutex> lock(G().mutex);
    G().hashWorkerRunning = false;
}

// Caller holds the mutex.
void BuildCatalogLocked()
{
    Glue& g = G();
    const fs::path dllDir = Util::DllPath().remove_filename();
    const fs::path exeDir = Util::ExePath().remove_filename();
    const ExcludeFn excluded = StreamlineExclusion();

    std::vector<Entry> entries;
    // Legacy file: beside OptiScaler first, then beside the executable, as the model search always did.
    auto legacy = FindLegacy(dllDir, kModelFile, excluded);
    if (!legacy)
        legacy = FindLegacy(exeDir, kModelFile, excluded);
    if (legacy)
    {
        Entry e;
        e.id = std::wstring(kLegacyId);
        e.legacy = true;
        std::error_code ec;
        const fs::path canon = fs::canonical(*legacy, ec);
        e.path = ec ? legacy->lexically_normal() : canon;
        entries.push_back(std::move(e));
    }

    std::vector<fs::path> roots;
    if (Config::Instance()->MainDllPath.has_value())
        roots.emplace_back(Config::Instance()->MainDllPath.value());
    roots.push_back(dllDir / L"OptiScaler");
    roots.push_back(exeDir / L"OptiScaler");
    for (auto& e : ScanFolders(roots, kModelFile))
    {
        // A folder entry that is the very file the legacy search found is the legacy file, not a second candidate.
        bool same = false;
        for (const auto& have : entries)
            same = same || Key(have.path) == Key(e.path);
        if (!same)
            entries.push_back(std::move(e));
    }

    std::vector<fs::path> toHash;
    for (auto& e : entries)
    {
        FillFileFacts(e);
        const auto it = g.hashes.find(Key(e.path));
        if (it != g.hashes.end())
            e.sha256 = it->second;
        else
            toHash.push_back(e.path);
    }
    g.catalog = std::move(entries);
    g.catalogBuilt = true;

    std::string listing;
    for (const auto& e : g.catalog)
    {
        listing += "\n  ";
        listing += Utf8(e.id) + " " + Utf8(VersionText(OrderVersion(e))) + ", " + SizeText(e.sizeBytes) + ", " + PathText(e.path);
    }
    LOG_INFO("DLSS-NR runtime catalog: {} candidate(s){}", g.catalog.size(), listing);

    if (!toHash.empty() && !g.hashWorkerRunning)
    {
        g.hashWorkerRunning = true;
        try
        {
            std::thread(HashWorker, std::move(toHash)).detach();
        }
        catch (...)
        {
            g.hashWorkerRunning = false;
        }
    }
}

void EnsureSelectorLocked()
{
    Glue& g = G();
    if (g.selector)
        return;
    if (!g.catalogBuilt)
        BuildCatalogLocked();
    g.selector = std::make_unique<Selector>(
        [](std::wstring_view requested)
        {
            Snapshot s = MakeSnapshot(G().catalog, requested);
            return s;
        });
}

bool LockedBySession(const char** why)
{
    const char* reason = nullptr;
    if (ext::Cfg().nr.precision.value_or_default() == 4)
        reason = "the hybrid FP4 model precision keeps its weights per device";
    else if (!DlssNr::AmdBridge::Status().empty())
        reason = "the AMD bridge loads its model once";
    if (why)
        *why = reason;
    return reason != nullptr;
}
} // namespace

std::shared_ptr<const Snapshot> Current()
{
    Glue& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureSelectorLocked();
    if (g.selector->Current() && LockedBySession(nullptr))
        return g.selector->Current();
    const auto snapshot =
        g.selector->Poll(Wide(ext::Cfg().nr.runtime.value_or_default()), GetTickCount64());
    if (!g.startup)
    {
        g.startup = snapshot;
        if (snapshot->Available())
            g.ngxFolder = snapshot->path.parent_path();
    }
    return snapshot;
}

std::shared_ptr<const Snapshot> Startup()
{
    {
        std::lock_guard<std::mutex> lock(G().mutex);
        if (G().startup)
            return G().startup;
    }
    Current();
    std::lock_guard<std::mutex> lock(G().mutex);
    return G().startup;
}

void RequestRuntime(const std::string& text)
{
    std::lock_guard<std::mutex> lock(G().mutex);
    ext::Cfg().nr.runtime = text;
}

void Rescan()
{
    Glue& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    BuildCatalogLocked();
    if (g.selector)
        g.selector->Rescan();
}

bool Pending()
{
    Glue& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.selector && g.selector->Pending();
}

std::vector<Entry> Catalog()
{
    Glue& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.catalogBuilt)
        BuildCatalogLocked();
    std::vector<Entry> copy = g.catalog;
    for (auto& e : copy)
    {
        const auto it = g.hashes.find(Key(e.path));
        if (it != g.hashes.end())
            e.sha256 = it->second;
    }
    return copy;
}

const char* SwitchLockReason()
{
    std::lock_guard<std::mutex> lock(G().mutex);
    const char* why = nullptr;
    LockedBySession(&why);
    return why;
}

fs::path NgxSearchFolder()
{
    std::lock_guard<std::mutex> lock(G().mutex);
    return G().ngxFolder;
}

bool FollowsNgxSearchPath(const Snapshot& s)
{
    std::lock_guard<std::mutex> lock(G().mutex);
    return s.Available() && !G().ngxFolder.empty() && Key(s.path.parent_path()) == Key(G().ngxFolder);
}
} // namespace ext::nr::runtime
